#pragma once

#include <stdint.h>

class SPIClass {
public:
    void begin(int sck, int miso, int mosi) {}
    void end() {}
};

extern SPIClass SPI;
