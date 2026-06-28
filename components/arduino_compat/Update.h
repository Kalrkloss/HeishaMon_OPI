#pragma once

#include <stdint.h>
#include <stddef.h>
#include "Arduino.h"
#include "esp_ota_ops.h"
#include "mbedtls/md5.h"

#define UPDATE_ERROR_OK          0
#define UPDATE_ERROR_WRITE       1
#define UPDATE_ERROR_ERASE       2
#define UPDATE_ERROR_READ        3
#define UPDATE_ERROR_SPACE       4
#define UPDATE_ERROR_SIZE        5
#define UPDATE_ERROR_STREAM      6
#define UPDATE_ERROR_MD5         7
#define UPDATE_ERROR_MAGIC_BYTE  8
#define UPDATE_ERROR_ACTIVATE    9
#define UPDATE_ERROR_NO_PARTITION 10
#define UPDATE_ERROR_BAD_ARGUMENT 11
#define UPDATE_ERROR_ABORT       12

class UpdateClass {
public:
    UpdateClass();
    ~UpdateClass();

    bool begin(size_t size);
    bool write(const uint8_t* data, size_t len);
    bool end(bool evenIfRemaining = false);
    bool isRunning();
    bool hasError();
    bool setMD5(const char* md5);
    size_t getFreeSketchSpace();
    void printError(Stream& s);
    void runAsync(bool a);
    void abort();
    size_t writeStream(Stream& s);
    int progress();
    size_t size();
    uint8_t getError();

private:
    bool running_;
    bool md5_set_;
    uint8_t expected_md5_[16];
    esp_ota_handle_t ota_handle_;
    const esp_partition_t* ota_partition_;
    size_t size_;
    size_t written_;
    uint8_t error_;
    mbedtls_md5_context md5_ctx_;
};

extern UpdateClass Update;
