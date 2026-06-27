#pragma once

#include "Arduino.h"
#include "IPAddress.h"

class SPIClass;

class ETHClass {
public:
    bool begin(uint8_t type, uint8_t addr, int cs, int irq, int rst, SPIClass& spi) { return false; }
    bool connected() { return false; }
    bool hasIP() { return false; }
    IPAddress localIP() { return IPAddress(); }
    void setHostname(const char* hn) {}
    int phyAddr() { return 0; }
};

extern ETHClass ETH;
