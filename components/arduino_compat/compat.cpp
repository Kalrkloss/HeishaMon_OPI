#include "Arduino.h"
#include "WiFi.h"
#include "ESPmDNS.h"
#include "ArduinoOTA.h"
#include "LittleFS.h"
#include "Update.h"
#include "SPI.h"
#include "ETH.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "IPAddress.h"
#include "esp_spiffs.h"
#include <time.h>
#include <mdns.h>
#include <sys/stat.h>

// ---- GPIO implementation ----
void pinMode(uint8_t pin, uint8_t mode) {
    gpio_config_t conf = {};
    conf.pin_bit_mask = (1ULL << pin);
    switch (mode) {
        case OUTPUT:
            conf.mode = GPIO_MODE_OUTPUT;
            break;
        case INPUT_PULLUP:
            conf.mode = GPIO_MODE_INPUT;
            conf.pull_up_en = GPIO_PULLUP_ENABLE;
            break;
        default:
            conf.mode = GPIO_MODE_INPUT;
            break;
    }
    gpio_config(&conf);
}
void digitalWrite(uint8_t pin, uint8_t val) {
    gpio_set_level((gpio_num_t)pin, val);
}
int digitalRead(uint8_t pin) {
    return gpio_get_level((gpio_num_t)pin);
}

// ---- Interrupt shim ----
static bool gpio_isr_installed = false;

static gpio_int_type_t arduino_mode_to_idf(int mode) {
    switch (mode) {
        case 1: return GPIO_INTR_POSEDGE;
        case 2: return GPIO_INTR_ANYEDGE;
        case 3: return GPIO_INTR_NEGEDGE;
        case 4: return GPIO_INTR_LOW_LEVEL;
        case 5: return GPIO_INTR_HIGH_LEVEL;
        default: return GPIO_INTR_DISABLE;
    }
}

void attachInterrupt(uint8_t pin, voidFuncPtr callback, int mode) {
    if (!gpio_isr_installed) {
        gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
        gpio_isr_installed = true;
    }
    gpio_config_t conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = arduino_mode_to_idf(mode),
    };
    gpio_config(&conf);
    gpio_isr_handler_add((gpio_num_t)pin, (gpio_isr_t)callback, NULL);
}

void detachInterrupt(uint8_t pin) {
    gpio_isr_handler_remove((gpio_num_t)pin);
}

// ---- Time config ----
bool configTzTime(const char* tz, const char* server1, const char* server2, const char* server3) {
    return true;
}

// ---- Serial_ implementation ----

static uart_port_t uart_nr_to_port(int8_t nr) {
    switch (nr) {
        case 0: return UART_NUM_0;
        case 1: return UART_NUM_1;
        case 2: return UART_NUM_2;
        default: return UART_NUM_MAX;
    }
}

static void arduino_config_to_uart(uint32_t cfg, uart_config_t* uart_cfg) {
    uart_cfg->data_bits = (uart_word_length_t)((cfg >> 2) & 0x3);
    uart_cfg->parity = (uart_parity_t)(cfg & 0x3);
    uart_cfg->stop_bits = (uart_stop_bits_t)((cfg >> 4) & 0x3);
    uart_cfg->flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_cfg->source_clk = UART_SCLK_DEFAULT;
    uart_cfg->flags.allow_pd = 0;
}

void Serial_::begin(unsigned long baud, uint32_t config, int8_t rxPin, int8_t txPin) {
    end();

    if (_uart_nr < 0) {
        _init = true;
        return;
    }

    uart_port_t uart_id = uart_nr_to_port(_uart_nr);
    uart_config_t uart_cfg;
    arduino_config_to_uart(config, &uart_cfg);
    uart_cfg.baud_rate = (int)baud;

    if (uart_param_config(uart_id, &uart_cfg) != ESP_OK) {
        return;
    }
    if (rxPin >= 0 && txPin >= 0) {
        uart_set_pin(uart_id, txPin, rxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    _init = (uart_driver_install(uart_id, 256, 256, 0, NULL, 0) == ESP_OK);
}

void Serial_::end() {
    if (_uart_nr >= 0 && _init) {
        uart_driver_delete(uart_nr_to_port(_uart_nr));
    }
    _init = false;
    _has_peek = false;
    _peek_byte = -1;
}

void Serial_::flush() {
    if (_uart_nr < 0) {
        fflush(stdout);
        return;
    }
    if (_init) {
        uart_flush(uart_nr_to_port(_uart_nr));
    }
}

int Serial_::available() {
    if (_uart_nr < 0 || !_init) return 0;
    size_t len = 0;
    uart_get_buffered_data_len(uart_nr_to_port(_uart_nr), &len);
    return (int)len + (_has_peek ? 1 : 0);
}

int Serial_::read() {
    if (_uart_nr < 0 || !_init) return -1;
    if (_has_peek) {
        _has_peek = false;
        int c = _peek_byte;
        _peek_byte = -1;
        return c;
    }
    uint8_t c;
    int len = uart_read_bytes(uart_nr_to_port(_uart_nr), &c, 1, 0);
    return (len == 1) ? (int)c : -1;
}

int Serial_::peek() {
    if (_uart_nr < 0 || !_init) return -1;
    if (_has_peek) return _peek_byte;
    uint8_t c;
    int len = uart_read_bytes(uart_nr_to_port(_uart_nr), &c, 1, 0);
    if (len == 1) {
        _has_peek = true;
        _peek_byte = c;
        return c;
    }
    return -1;
}

size_t Serial_::write(uint8_t c) {
    if (_uart_nr < 0) {
        putchar(c);
        return 1;
    }
    if (!_init) return 0;
    return uart_write_bytes(uart_nr_to_port(_uart_nr), (const char*)&c, 1);
}

size_t Serial_::write(const uint8_t* buf, size_t sz) {
    if (_uart_nr < 0) {
        return fwrite(buf, 1, sz, stdout);
    }
    if (!_init) return 0;
    return uart_write_bytes(uart_nr_to_port(_uart_nr), (const char*)buf, sz);
}

size_t Serial_::print(const IPAddress& ip) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return write((const uint8_t*)buf, strlen(buf));
}

int Serial_::printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret;
    if (_uart_nr < 0) {
        ret = vprintf(fmt, args);
    } else if (_init) {
        char buf[256];
        ret = vsnprintf(buf, sizeof(buf), fmt, args);
        if (ret > 0) {
            uart_write_bytes(uart_nr_to_port(_uart_nr), buf, ret);
        }
    } else {
        ret = 0;
    }
    va_end(args);
    return ret;
}

// ---- MDNSClass implementation ----
bool MDNSClass::begin(const char* hostname) {
    if (mdns_init() != ESP_OK) return false;
    if (mdns_hostname_set(hostname) != ESP_OK) return false;
    return true;
}
void MDNSClass::addService(const char* service, const char* proto, uint16_t port) {
    char srv[64];
    snprintf(srv, sizeof(srv), "_%s", service);
    mdns_service_add(NULL, srv, proto, port, NULL, 0);
}

// ---- LittleFSClass (SPIFFS) implementation ----
void LittleFSClass::make_path(char* buf, size_t bufsz, const char* path) {
    snprintf(buf, bufsz, "%s%s", LITTLEFS_MOUNT_POINT, path);
}

LittleFSClass::LittleFSClass() : mounted_(false) {}
LittleFSClass::~LittleFSClass() { end(); }

bool LittleFSClass::begin(bool formatOnFail) {
    if (mounted_) return true;
    esp_vfs_spiffs_conf_t conf = {
        .base_path = LITTLEFS_MOUNT_POINT,
        .partition_label = NULL,
        .max_files = 8,
        .format_if_mount_failed = formatOnFail,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_OK) {
        mounted_ = true;
        return true;
    }
    return false;
}

void LittleFSClass::end() {
    if (mounted_) {
        esp_vfs_spiffs_unregister(NULL);
        mounted_ = false;
    }
}

bool LittleFSClass::format() {
    esp_err_t ret = esp_spiffs_format(NULL);
    return ret == ESP_OK;
}

bool LittleFSClass::exists(const char* path) {
    char full[128];
    make_path(full, sizeof(full), path);
    struct stat st;
    return stat(full, &st) == 0;
}

File LittleFSClass::open(const char* path, const char* mode) {
    char full[128];
    make_path(full, sizeof(full), path);
    FILE* fp = fopen(full, mode);
    if (!fp) return File();
    return File(fp, String(path));
}

bool LittleFSClass::remove(const char* path) {
    char full[128];
    make_path(full, sizeof(full), path);
    return unlink(full) == 0;
}

bool LittleFSClass::rename(const char* from, const char* to) {
    char full_from[128], full_to[128];
    make_path(full_from, sizeof(full_from), from);
    make_path(full_to, sizeof(full_to), to);
    return ::rename(full_from, full_to) == 0;
}

bool LittleFSClass::mkdir(const char* path) {
    (void)path;
    return true;
}

// ---- Global instances ----
WiFiClass WiFi;
MDNSClass MDNS;
ArduinoOTAClass ArduinoOTA;
LittleFSClass LittleFS;
SPIClass SPI;
ETHClass ETH;
ESPClass ESP;
Serial_ Serial(-1);
Serial_ Serial1(1);
Serial_ Serial2(2);
