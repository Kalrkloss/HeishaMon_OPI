#pragma once

#include "Arduino.h"
#include "IPAddress.h"
#include "Stream.h"

class WiFiUDP : public Stream {
public:
    uint8_t begin(uint16_t port) { return 1; }
    void stop() {}
    int beginMulticast(IPAddress ip, uint16_t port) { return 1; }
    int beginPacket(const char* host, uint16_t port) { return 1; }
    int beginPacket(IPAddress ip, uint16_t port) { return 1; }
    int endPacket() { return 1; }
    size_t write(uint8_t c) override { return 1; }
    size_t write(const uint8_t* buf, size_t sz) override { return sz; }
    int parsePacket() { return 0; }
    int available() override { return 0; }
    int read() override { return -1; }
    int read(unsigned char* buf, size_t sz) { return 0; }
    int read(char* buf, size_t sz) { return 0; }
    int peek() override { return -1; }
    void flush() override {}
    IPAddress remoteIP() { return IPAddress(); }
    uint16_t remotePort() { return 0; }
};
