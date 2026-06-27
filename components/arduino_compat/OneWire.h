#pragma once

#include <stdint.h>

class OneWire {
public:
    OneWire(uint8_t pin);
    bool reset();
    void select(const uint8_t addr[8]);
    void skip();
    void write(uint8_t v, bool power = 0);
    uint8_t read();
    void reset_search();
    bool search(uint8_t* newAddr);
    void depower();

    static uint8_t crc8(const uint8_t* data, uint8_t len);

private:
    void writeBit(bool v);
    bool readBit();

    uint8_t _pin;
    bool _searchExhausted;
    uint8_t _searchAddress[8];
    int _searchLastDiscrepancy;
    bool _searchDone;
};
