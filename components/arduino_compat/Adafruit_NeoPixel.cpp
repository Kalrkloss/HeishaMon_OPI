#include "Adafruit_NeoPixel.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include <stdlib.h>
#include <string.h>

#define RMT_RES_HZ 20'000'000

#define T0H 7
#define T0L 16
#define T1H 14
#define T1L 12
#define RESET_SYMBOLS 250

static const rmt_symbol_word_t bit0 = {
    .duration0 = T0H, .level0 = 1,
    .duration1 = T0L, .level1 = 0
};
static const rmt_symbol_word_t bit1 = {
    .duration0 = T1H, .level0 = 1,
    .duration1 = T1L, .level1 = 0
};
static const rmt_symbol_word_t reset_sym = {
    .duration0 = RESET_SYMBOLS, .level0 = 0,
    .duration1 = 0, .level1 = 0
};

Adafruit_NeoPixel::Adafruit_NeoPixel(uint16_t n, uint16_t pin)
    : num_(n), pin_(pin), brightness_(256), pixels_(nullptr),
      tx_chan_(nullptr), copy_enc_(nullptr), symbols_(nullptr), num_symbols_(0)
{
    pixels_ = (uint8_t*)calloc(n, 3);
    num_symbols_ = n * 24 * 2 + 1;
    symbols_ = (rmt_symbol_word_t*)malloc(num_symbols_ * sizeof(rmt_symbol_word_t));
}

Adafruit_NeoPixel::~Adafruit_NeoPixel() {
    if (copy_enc_) rmt_del_encoder(copy_enc_);
    if (tx_chan_) rmt_del_channel(tx_chan_);
    free(symbols_);
    free(pixels_);
}

void Adafruit_NeoPixel::begin() {
    rmt_tx_channel_config_t cfg = {
        .gpio_num = (gpio_num_t)pin_,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 1,
    };
    if (rmt_new_tx_channel(&cfg, &tx_chan_) != ESP_OK) return;

    rmt_copy_encoder_config_t copy_cfg = {};
    if (rmt_new_copy_encoder(&copy_cfg, &copy_enc_) != ESP_OK) return;
}

void Adafruit_NeoPixel::clear() {
    memset(pixels_, 0, num_ * 3);
}

void Adafruit_NeoPixel::setPixelColor(uint16_t idx, uint8_t r, uint8_t g, uint8_t b) {
    if (idx >= num_) return;
    uint8_t* p = pixels_ + idx * 3;
    p[0] = g;
    p[1] = r;
    p[2] = b;
}

void Adafruit_NeoPixel::setPixelColor(uint16_t idx, uint32_t color) {
    setPixelColor(idx, (color >> 16) & 0xff, (color >> 8) & 0xff, color & 0xff);
}

void Adafruit_NeoPixel::encodePixel(uint8_t r, uint8_t g, uint8_t b, rmt_symbol_word_t* sym) {
    uint8_t grb[3] = {g, r, b};
    int si = 0;
    for (int byte = 0; byte < 3; byte++) {
        for (int bit = 7; bit >= 0; bit--) {
            sym[si++] = (grb[byte] >> bit) & 1 ? bit1 : bit0;
        }
    }
}

void Adafruit_NeoPixel::show() {
    if (!tx_chan_ || !copy_enc_ || !pixels_) return;

    int sym_idx = 0;
    for (uint16_t i = 0; i < num_; i++) {
        uint8_t* p = pixels_ + i * 3;
        uint8_t r = p[1] * brightness_ / 256;
        uint8_t g = p[0] * brightness_ / 256;
        uint8_t b = p[2] * brightness_ / 256;
        uint8_t grb[3] = {g, r, b};
        for (int byte = 0; byte < 3; byte++) {
            for (int bit = 7; bit >= 0; bit--) {
                symbols_[sym_idx++] = (grb[byte] >> bit) & 1 ? bit1 : bit0;
            }
        }
    }
    symbols_[sym_idx++] = reset_sym;

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
        .flags = { .eot_level = 0 },
    };
    rmt_transmit(tx_chan_, copy_enc_, symbols_, sym_idx * sizeof(rmt_symbol_word_t), &tx_cfg);
    rmt_tx_wait_all_done(tx_chan_, -1);
}
