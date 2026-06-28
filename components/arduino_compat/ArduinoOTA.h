#pragma once

#include "Arduino.h"
#include <stdint.h>

typedef void (*ota_cb_t)();

enum ota_error_t {
    OTA_AUTH_ERROR,
    OTA_BEGIN_ERROR,
    OTA_CONNECT_ERROR,
    OTA_RECEIVE_ERROR,
    OTA_END_ERROR
};

typedef void (*ota_error_cb_t)(ota_error_t);

class ArduinoOTAClass {
public:
    ArduinoOTAClass();
    ~ArduinoOTAClass();

    void setPort(uint16_t port);
    void setHostname(const char* hostname);
    void setPassword(const char* password);
    void onStart(ota_cb_t cb);
    void onEnd(ota_cb_t cb);
    void onProgress(void (*cb)(unsigned int, unsigned int));
    void onError(ota_error_cb_t cb);
    void begin();
    void handle();

private:
    uint16_t _port;
    char _hostname[64];
    char _password[64];
    int _sock;
    ota_cb_t _start_cb;
    ota_cb_t _end_cb;
    void (*_progress_cb)(unsigned int, unsigned int);
    ota_error_cb_t _error_cb;
    bool _running;
};

extern ArduinoOTAClass ArduinoOTA;
