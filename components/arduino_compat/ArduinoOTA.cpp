#include "ArduinoOTA.h"
#include <string.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_log.h"
#include "mbedtls/md5.h"

static const char* TAG = "ArduinoOTA";

ArduinoOTAClass::ArduinoOTAClass()
    : _port(3232), _sock(-1), _start_cb(nullptr), _end_cb(nullptr),
      _progress_cb(nullptr), _error_cb(nullptr), _running(false) {
    _hostname[0] = 0;
    _password[0] = 0;
}

ArduinoOTAClass::~ArduinoOTAClass() {
    if (_sock >= 0) close(_sock);
    _running = false;
}

void ArduinoOTAClass::setPort(uint16_t port) { _port = port; }
void ArduinoOTAClass::setHostname(const char* hostname) {
    strncpy(_hostname, hostname, sizeof(_hostname) - 1);
}
void ArduinoOTAClass::setPassword(const char* password) {
    strncpy(_password, password, sizeof(_password) - 1);
}
void ArduinoOTAClass::onStart(ota_cb_t cb) { _start_cb = cb; }
void ArduinoOTAClass::onEnd(ota_cb_t cb) { _end_cb = cb; }
void ArduinoOTAClass::onProgress(void (*cb)(unsigned int, unsigned int)) { _progress_cb = cb; }
void ArduinoOTAClass::onError(ota_error_cb_t cb) { _error_cb = cb; }

void ArduinoOTAClass::begin() {
    _sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        return;
    }
    int opt = 1;
    setsockopt(_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind on port %u", _port);
        close(_sock);
        _sock = -1;
        return;
    }
    _running = true;
    ESP_LOGI(TAG, "OTA started on port %u", _port);
}

static void md5_string(const char* input, size_t len, unsigned char output[16]) {
    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);
    mbedtls_md5_update(&ctx, (const unsigned char*)input, len);
    mbedtls_md5_finish(&ctx, output);
    mbedtls_md5_free(&ctx);
}

void ArduinoOTAClass::handle() {
    if (!_running || _sock < 0) return;

    static esp_ota_handle_t ota_handle = 0;
    static const esp_partition_t* ota_partition = nullptr;
    static bool auth_ok = false;
    static bool update_running = false;

    uint8_t buf[1472];
    struct sockaddr_in remote;
    socklen_t socklen = sizeof(remote);

    int len = recvfrom(_sock, buf, sizeof(buf), MSG_DONTWAIT,
                       (struct sockaddr*)&remote, &socklen);
    if (len <= 0) return;

    if (len < 3) return;
    uint8_t type = buf[0];
    uint16_t data_len = (buf[1] << 8) | buf[2];
    uint8_t* data = buf + 3;

    // Validate declared data_len matches actual remaining
    if ((size_t)(3 + data_len) > (size_t)len) return;

    switch (type) {
    case 0: { // AUTH
        if (data_len != 16) return;
        unsigned char expected[16];
        size_t pwlen = strlen(_password);
        char* input = (char*)malloc(pwlen + 10);
        if (!input) return;
        memcpy(input, _password, pwlen);
        memcpy(input + pwlen, "ota_secret", 10);
        md5_string(input, pwlen + 10, expected);
        free(input);

        auth_ok = (memcmp(data, expected, 16) == 0);
        uint8_t resp[3] = { (uint8_t)(auth_ok ? 1 : 2), 0, 0 };
        sendto(_sock, resp, 3, 0, (struct sockaddr*)&remote, socklen);
        ESP_LOGI(TAG, "OTA auth %s", auth_ok ? "OK" : "FAILED");
        break;
    }
    case 4: { // BEGIN
        if (!auth_ok) return;
        if (data_len < 4) return;
        uint32_t firmware_size = (data[0] << 24) | (data[1] << 16) |
                                  (data[2] << 8) | data[3];
        ESP_LOGI(TAG, "OTA update start, size: %lu", (unsigned long)firmware_size);

        ota_partition = esp_ota_get_next_update_partition(NULL);
        if (!ota_partition) {
            ESP_LOGE(TAG, "No OTA partition found");
            if (_error_cb) _error_cb(OTA_BEGIN_ERROR);
            break;
        }
        esp_err_t err = esp_ota_begin(ota_partition, firmware_size, &ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
            if (_error_cb) _error_cb(OTA_BEGIN_ERROR);
            break;
        }
        update_running = true;
        if (_start_cb) _start_cb();
        uint8_t resp[3] = { 1, 0, 0 };
        sendto(_sock, resp, 3, 0, (struct sockaddr*)&remote, socklen);
        break;
    }
    case 5: { // DATA
        if (!update_running) return;
        esp_err_t err = esp_ota_write(ota_handle, data, data_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            update_running = false;
            if (_error_cb) _error_cb(OTA_RECEIVE_ERROR);
            break;
        }
        static uint32_t total_written = 0;
        total_written += data_len;
        if (_progress_cb) _progress_cb(total_written, 0);
        break;
    }
    case 6: { // END
        if (!update_running) return;
        esp_err_t err = esp_ota_end(ota_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA end failed: %s", esp_err_to_name(err));
            if (_error_cb) _error_cb(OTA_END_ERROR);
            break;
        }
        err = esp_ota_set_boot_partition(ota_partition);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OTA set boot failed: %s", esp_err_to_name(err));
            if (_error_cb) _error_cb(OTA_END_ERROR);
            break;
        }
        update_running = false;
        auth_ok = false;
        if (_end_cb) _end_cb();
        ESP_LOGI(TAG, "OTA update complete, restarting...");
        uint8_t resp[3] = { 1, 0, 0 };
        sendto(_sock, resp, 3, 0, (struct sockaddr*)&remote, socklen);
        esp_restart();
        break;
    }
    case 7: { // INFO
        char info[256];
        int info_len = snprintf(info, sizeof(info),
            "{\"hostname\":\"%s\",\"port\":%u}",
            _hostname[0] ? _hostname : "esp32", _port);
        if (info_len > 255) info_len = 255;
        uint8_t resp[259];
        resp[0] = 7;
        resp[1] = (info_len >> 8) & 0xff;
        resp[2] = info_len & 0xff;
        memcpy(resp + 3, info, info_len);
        sendto(_sock, resp, 3 + info_len, 0, (struct sockaddr*)&remote, socklen);
        break;
    }
    default:
        break;
    }
}
