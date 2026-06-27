#pragma once

#include "Print.h"

class WiFiClient;

class Server : public Print {
public:
    virtual WiFiClient available() = 0;
    virtual void begin() = 0;
    virtual void begin(uint16_t port) = 0;
    virtual void close() {}
    virtual void stop() {}
    virtual ~Server() {}
};
