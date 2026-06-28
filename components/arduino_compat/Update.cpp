#include "Update.h"
#include "esp_partition.h"
#include <string.h>

UpdateClass Update;

static uint8_t hex_to_byte(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

UpdateClass::UpdateClass()
    : running_(false), md5_set_(false), ota_handle_(0),
      ota_partition_(nullptr), size_(0), written_(0), error_(UPDATE_ERROR_OK) {
    memset(expected_md5_, 0, sizeof(expected_md5_));
    mbedtls_md5_init(&md5_ctx_);
}

UpdateClass::~UpdateClass() {
    if (running_) abort();
    mbedtls_md5_free(&md5_ctx_);
}

bool UpdateClass::begin(size_t size) {
    if (running_) return false;

    ota_partition_ = esp_ota_get_next_update_partition(NULL);
    if (!ota_partition_) {
        error_ = UPDATE_ERROR_NO_PARTITION;
        return false;
    }

    esp_err_t err = esp_ota_begin(ota_partition_, size, &ota_handle_);
    if (err != ESP_OK) {
        error_ = UPDATE_ERROR_SPACE;
        return false;
    }

    size_ = size;
    written_ = 0;
    error_ = UPDATE_ERROR_OK;
    running_ = true;
    md5_set_ = false;
    memset(expected_md5_, 0, sizeof(expected_md5_));
    mbedtls_md5_starts(&md5_ctx_);

    return true;
}

bool UpdateClass::write(const uint8_t* data, size_t len) {
    if (!running_ || error_ != UPDATE_ERROR_OK) return false;

    if (written_ + len > size_) {
        error_ = UPDATE_ERROR_SIZE;
        return false;
    }

    esp_err_t err = esp_ota_write(ota_handle_, data, len);
    if (err != ESP_OK) {
        error_ = UPDATE_ERROR_WRITE;
        return false;
    }

    written_ += len;
    mbedtls_md5_update(&md5_ctx_, data, len);
    return true;
}

bool UpdateClass::end(bool evenIfRemaining) {
    if (!running_) return false;

    if (written_ < size_ && !evenIfRemaining) {
        esp_ota_abort(ota_handle_);
        error_ = UPDATE_ERROR_SIZE;
        running_ = false;
        return false;
    }

    uint8_t calc_md5[16];
    mbedtls_md5_finish(&md5_ctx_, calc_md5);

    if (md5_set_ && memcmp(calc_md5, expected_md5_, 16) != 0) {
        esp_ota_abort(ota_handle_);
        error_ = UPDATE_ERROR_MD5;
        running_ = false;
        return false;
    }

    esp_err_t err = esp_ota_end(ota_handle_);
    if (err != ESP_OK) {
        error_ = UPDATE_ERROR_ERASE;
        running_ = false;
        return false;
    }

    err = esp_ota_set_boot_partition(ota_partition_);
    if (err != ESP_OK) {
        error_ = UPDATE_ERROR_ACTIVATE;
        running_ = false;
        return false;
    }

    running_ = false;
    error_ = UPDATE_ERROR_OK;
    return true;
}

bool UpdateClass::isRunning() { return running_; }
bool UpdateClass::hasError() { return error_ != UPDATE_ERROR_OK; }

bool UpdateClass::setMD5(const char* md5) {
    if (!md5) return false;
    size_t len = strlen(md5);
    if (len < 32) return false;

    for (int i = 0; i < 16; i++) {
        expected_md5_[i] = (hex_to_byte(md5[i * 2]) << 4) | hex_to_byte(md5[i * 2 + 1]);
    }
    md5_set_ = true;
    return true;
}

size_t UpdateClass::getFreeSketchSpace() {
    const esp_partition_t* part = esp_ota_get_next_update_partition(NULL);
    if (!part) return 0;
    return part->size;
}

void UpdateClass::printError(Stream& s) {
    switch (error_) {
        case UPDATE_ERROR_OK: s.print("No Error"); break;
        case UPDATE_ERROR_WRITE: s.print("Flash Write Failed"); break;
        case UPDATE_ERROR_ERASE: s.print("Flash Erase Failed"); break;
        case UPDATE_ERROR_READ: s.print("Flash Read Failed"); break;
        case UPDATE_ERROR_SPACE: s.print("Not Enough Space"); break;
        case UPDATE_ERROR_SIZE: s.print("Wrong Size"); break;
        case UPDATE_ERROR_STREAM: s.print("Stream Read Error"); break;
        case UPDATE_ERROR_MD5: s.print("MD5 Check Failed"); break;
        case UPDATE_ERROR_MAGIC_BYTE: s.print("Wrong Magic Byte"); break;
        case UPDATE_ERROR_ACTIVATE: s.print("Activation Error"); break;
        case UPDATE_ERROR_NO_PARTITION: s.print("No OTA Partition"); break;
        case UPDATE_ERROR_BAD_ARGUMENT: s.print("Bad Argument"); break;
        case UPDATE_ERROR_ABORT: s.print("Aborted"); break;
        default: s.print("Unknown Error"); break;
    }
}

void UpdateClass::runAsync(bool a) { (void)a; }
void UpdateClass::abort() {
    if (running_) {
        esp_ota_abort(ota_handle_);
        running_ = false;
        error_ = UPDATE_ERROR_ABORT;
    }
}

size_t UpdateClass::writeStream(Stream& s) {
    if (!running_) return 0;
    size_t total = 0;
    while (s.available()) {
        uint8_t buf[256];
        int r = s.readBytes((char*)buf, sizeof(buf));
        if (r <= 0) break;
        if (!write(buf, r)) return total;
        total += r;
    }
    return total;
}

int UpdateClass::progress() {
    if (!running_ || size_ == 0) return 0;
    return (int)((written_ * 100) / size_);
}

size_t UpdateClass::size() { return size_; }
uint8_t UpdateClass::getError() { return error_; }
