#include "OneWire.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

// Dallas 1-Wire timing constants (microseconds)
#define OW_RESET_LOW    480
#define OW_RESET_RELEASE 480
#define OW_PRESENCE_WAIT 60
#define OW_PRESENCE_LOW  240
#define OW_WRITE1_LOW    6
#define OW_WRITE1_HIGH   64
#define OW_WRITE0_LOW    60
#define OW_WRITE0_HIGH   10
#define OW_READ_LOW      6
#define OW_READ_SAMPLE   9
#define OW_READ_HIGH     55
#define OW_RECOVERY      5

static void ow_delay_us(unsigned int us) {
    uint64_t start = esp_timer_get_time();
    while (esp_timer_get_time() - start < us) {}
}

static void ow_set_output(uint8_t pin) {
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT_OD);
}

static void ow_set_input(uint8_t pin) {
    gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
}

static void ow_write_low(uint8_t pin) {
    gpio_set_level((gpio_num_t)pin, 0);
}

static void ow_write_high(uint8_t pin) {
    gpio_set_level((gpio_num_t)pin, 1);
}

static int ow_read_pin(uint8_t pin) {
    return gpio_get_level((gpio_num_t)pin);
}

OneWire::OneWire(uint8_t pin) : _pin(pin) {
    ow_set_output(_pin);
    ow_write_high(_pin);
    reset_search();
}

bool OneWire::reset() {
    ow_set_output(_pin);
    ow_write_low(_pin);
    ow_delay_us(OW_RESET_LOW);

    ow_set_input(_pin);
    ow_delay_us(OW_PRESENCE_WAIT);

    int presence = !ow_read_pin(_pin);
    ow_delay_us(OW_RESET_RELEASE - OW_PRESENCE_WAIT);

    ow_set_output(_pin);
    ow_write_high(_pin);
    ow_delay_us(OW_RECOVERY);

    return presence;
}

void OneWire::writeBit(bool v) {
    ow_set_output(_pin);
    if (v) {
        ow_write_low(_pin);
        ow_delay_us(OW_WRITE1_LOW);
        ow_write_high(_pin);
        ow_delay_us(OW_WRITE1_HIGH);
    } else {
        ow_write_low(_pin);
        ow_delay_us(OW_WRITE0_LOW);
        ow_write_high(_pin);
        ow_delay_us(OW_WRITE0_HIGH);
    }
}

bool OneWire::readBit() {
    ow_set_output(_pin);
    ow_write_low(_pin);
    ow_delay_us(OW_READ_LOW);
    ow_set_input(_pin);
    ow_delay_us(OW_READ_SAMPLE);
    int v = ow_read_pin(_pin);
    ow_delay_us(OW_READ_HIGH);
    ow_set_output(_pin);
    ow_write_high(_pin);
    ow_delay_us(OW_RECOVERY);
    return v;
}

void OneWire::write(uint8_t v, bool power) {
    for (uint8_t mask = 1; mask; mask <<= 1) {
        writeBit(v & mask);
    }
    if (!power) {
        ow_set_output(_pin);
        ow_write_high(_pin);
        ow_delay_us(OW_RECOVERY);
    }
}

uint8_t OneWire::read() {
    uint8_t v = 0;
    for (uint8_t mask = 1; mask; mask <<= 1) {
        if (readBit()) {
            v |= mask;
        }
    }
    return v;
}

void OneWire::select(const uint8_t addr[8]) {
    write(0x55);
    for (int i = 0; i < 8; i++) {
        write(addr[i]);
    }
}

void OneWire::skip() {
    write(0xCC);
}

void OneWire::depower() {
    ow_set_output(_pin);
    ow_write_low(_pin);
    ow_delay_us(OW_RECOVERY);
    ow_set_input(_pin);
}

void OneWire::reset_search() {
    _searchExhausted = false;
    _searchLastDiscrepancy = 0;
    _searchDone = false;
    for (int i = 0; i < 8; i++) {
        _searchAddress[i] = 0;
    }
}

bool OneWire::search(uint8_t* newAddr) {
    if (_searchExhausted) return false;
    if (!reset()) return false;

    write(0xF0);
    int lastZero = -1;
    bool done = true;

    for (int i = 0; i < 64; i++) {
        int byteIdx = i / 8;
        int bitIdx = i % 8;

        bool b1 = readBit();
        bool b2 = readBit();

        if (b1 && b2) {
            _searchExhausted = true;
            return false;
        }

        bool dir;
        if (b1 != b2) {
            dir = b1;
        } else {
            if (i < _searchLastDiscrepancy) {
                dir = (_searchAddress[byteIdx] >> bitIdx) & 1;
            } else {
                dir = (i == _searchLastDiscrepancy);
            }
            if (!dir) {
                lastZero = i;
            }
        }

        if (dir) {
            _searchAddress[byteIdx] |= (1 << bitIdx);
        } else {
            _searchAddress[byteIdx] &= ~(1 << bitIdx);
        }

        writeBit(dir);
        done = false;
    }

    if (!done) {
        if (lastZero == -1) {
            _searchExhausted = true;
        } else {
            _searchLastDiscrepancy = lastZero;
        }
    }

    for (int i = 0; i < 8; i++) {
        newAddr[i] = _searchAddress[i];
    }
    return true;
}

uint8_t OneWire::crc8(const uint8_t* data, uint8_t len) {
    uint8_t crc = 0;
    for (uint8_t i = 0; i < len; i++) {
        uint8_t byte = data[i];
        for (uint8_t b = 0; b < 8; b++) {
            uint8_t fb = (crc ^ byte) & 1;
            crc >>= 1;
            if (fb) crc ^= 0x8C;
            byte >>= 1;
        }
    }
    return crc;
}
