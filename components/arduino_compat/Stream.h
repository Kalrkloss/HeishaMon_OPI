#pragma once

#include "Print.h"

class Stream : public Print {
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    virtual void flush() {}
    size_t readBytes(char* buf, size_t len) { size_t i = 0; for (; i < len; i++) { int c = read(); if (c < 0) break; buf[i] = (char)c; } return i; }
    size_t readBytes(uint8_t* buf, size_t len) { return readBytes((char*)buf, len); }

    virtual ~Stream() {}
};
