#pragma once

#include <stdint.h>
#include <stddef.h>
#include "Arduino.h"

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
    bool begin(size_t size) { return true; }
    bool write(const uint8_t* data, size_t len) { return true; }
    bool end(bool evenIfRemaining = false) { return true; }
    bool isRunning() { return false; }
    bool hasError() { return false; }
    bool setMD5(const char* md5) { return true; }
    size_t getFreeSketchSpace() { return 0x200000; }
    void printError(Stream& s) {}
    void runAsync(bool a) {}
    void abort() {}
    size_t writeStream(Stream& s) { return 0; }
    int progress() { return 0; }
    size_t size() { return 0; }
    uint8_t getError() { return UPDATE_ERROR_OK; }
};

extern UpdateClass Update;
