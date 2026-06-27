#pragma once

#include "Arduino.h"

class MDNSClass {
public:
    bool begin(const char* hostname) { return true; }
    void addService(const char* service, const char* proto, uint16_t port) {}
    void update() {}
    void notifyAPChange() {}
    void announce() {}
};

extern MDNSClass MDNS;
