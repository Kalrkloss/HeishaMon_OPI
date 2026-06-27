#pragma once

#include <stdint.h>

class Adafruit_NeoPixel {
public:
    Adafruit_NeoPixel(uint16_t n, uint16_t pin) : num_(n), pin_(pin) {}
    void begin() {}
    void clear() {}
    void show() {}
    void setPixelColor(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {}
    void setPixelColor(uint16_t idx, uint32_t color) {}
    uint32_t Color(uint8_t r, uint8_t g, uint8_t b) { return (uint32_t)r << 16 | (uint32_t)g << 8 | b; }
private:
    uint16_t num_;
    uint16_t pin_;
};
