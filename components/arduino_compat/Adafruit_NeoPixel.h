#pragma once

#include <stdint.h>
#include "driver/rmt_tx.h"

class Adafruit_NeoPixel {
public:
    Adafruit_NeoPixel(uint16_t n, uint16_t pin);
    ~Adafruit_NeoPixel();

    void begin();
    void clear();
    void show();
    void setPixelColor(uint16_t idx, uint8_t r, uint8_t g, uint8_t b);
    void setPixelColor(uint16_t idx, uint32_t color);
    uint32_t Color(uint8_t r, uint8_t g, uint8_t b) {
        return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    void setBrightness(uint8_t b) { brightness_ = b + 1; }

private:
    void encodePixel(uint8_t r, uint8_t g, uint8_t b, rmt_symbol_word_t* sym);
    uint16_t num_;
    uint16_t pin_;
    uint8_t brightness_;
    uint8_t* pixels_;
    rmt_channel_handle_t tx_chan_;
    rmt_encoder_handle_t copy_enc_;
    rmt_symbol_word_t* symbols_;
    size_t num_symbols_;
};
