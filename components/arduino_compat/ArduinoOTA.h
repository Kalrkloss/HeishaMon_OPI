#pragma once

#include "Arduino.h"

typedef void (*ota_cb_t)();
typedef void (*ota_error_cb_t)(int);

class ArduinoOTAClass {
public:
    void setPort(uint16_t port) {}
    void setHostname(const char* hostname) {}
    void setPassword(const char* password) {}
    void onStart(ota_cb_t cb) {}
    void onEnd(ota_cb_t cb) {}
    void onProgress(void (*cb)(unsigned int, unsigned int)) {}
    void onError(ota_error_cb_t cb) {}
    void begin() {}
    void handle() {}
};

extern ArduinoOTAClass ArduinoOTA;
