#pragma once

#include <stdio.h>
#include <stdint.h>
#include "Arduino.h"

namespace fs {

enum SeekMode { SeekSet = 0, SeekCur = 1, SeekEnd = 2 };

class File {
public:
    File() : fp_(nullptr), owned_(false) {}
    File(FILE* fp) : fp_(fp), owned_(fp != nullptr) {}
    File(FILE* fp, const String& name) : fp_(fp), owned_(fp != nullptr), name_(name) {}
    ~File() { close(); }

    File(File&& other) : fp_(other.fp_), owned_(other.owned_), name_(other.name_) {
        other.fp_ = nullptr;
        other.owned_ = false;
        other.name_ = String();
    }
    File& operator=(File&& other) {
        if (this != &other) {
            close();
            fp_ = other.fp_;
            owned_ = other.owned_;
            name_ = other.name_;
            other.fp_ = nullptr;
            other.owned_ = false;
            other.name_ = String();
        }
        return *this;
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    bool operator!() const { return !fp_; }
    explicit operator bool() const { return fp_ != nullptr; }

    size_t write(const uint8_t* buf, size_t sz) { if (!fp_) return 0; return fwrite(buf, 1, sz, fp_); }
    size_t write(uint8_t c) { return write(&c, 1); }
    int read() { if (!fp_) return -1; return fgetc(fp_); }
    size_t readBytes(char* buf, size_t len) { if (!fp_) return 0; return fread(buf, 1, len, fp_); }
    int read(void* buf, size_t len) { if (!fp_) return -1; return fread(buf, 1, len, fp_); }
    int available() {
        if (!fp_) return 0;
        long cur = ftell(fp_);
        if (cur < 0) return 0;
        fseek(fp_, 0, SEEK_END);
        long end = ftell(fp_);
        fseek(fp_, cur, SEEK_SET);
        long remaining = end - cur;
        return (remaining > INT_MAX) ? INT_MAX : (int)remaining;
    }
    void close() { if (fp_ && owned_) { fclose(fp_); } fp_ = nullptr; owned_ = false; }
    size_t size() {
        if (!fp_) return 0;
        long cur = ftell(fp_);
        if (cur < 0) return 0;
        fseek(fp_, 0, SEEK_END);
        long sz = ftell(fp_);
        fseek(fp_, cur, SEEK_SET);
        return (sz < 0) ? 0 : (size_t)sz;
    }
    bool seek(uint32_t pos) { if (!fp_) return false; return fseek(fp_, pos, SEEK_SET) == 0; }
    bool seek(uint32_t pos, SeekMode mode) {
        if (!fp_) return false;
        int whence = SEEK_SET;
        if (mode == SeekCur) whence = SEEK_CUR;
        else if (mode == SeekEnd) whence = SEEK_END;
        return fseek(fp_, pos, whence) == 0;
    }
    String name() { return name_; }
    int peek() {
        if (!fp_) return -1;
        int c = fgetc(fp_);
        if (c != -1) ungetc(c, fp_);
        return c;
    }
    void flush() { if (fp_) fflush(fp_); }

private:
    FILE* fp_;
    bool owned_;
    String name_;
};

} // namespace fs

using fs::File;
using fs::SeekMode;
using fs::SeekSet;
using fs::SeekCur;
using fs::SeekEnd;
