#pragma once

#include <stdio.h>
#include <stdint.h>
#include "Arduino.h"

namespace fs {

enum SeekMode { SeekSet = 0, SeekCur = 1, SeekEnd = 2 };

class File {
public:
    File() : fp_(nullptr) {}
    File(FILE* fp) : fp_(fp) {}
    ~File() { if (fp_) fclose(fp_); }
    
    bool operator!() const { return !fp_; }
    operator bool() const { return fp_ != nullptr; }
    
    size_t write(const uint8_t* buf, size_t sz) { if (!fp_) return 0; return fwrite(buf, 1, sz, fp_); }
    size_t write(uint8_t c) { return write(&c, 1); }
    int read() { if (!fp_) return -1; return fgetc(fp_); }
    size_t readBytes(char* buf, size_t len) { if (!fp_) return 0; return fread(buf, 1, len, fp_); }
    int read(void* buf, size_t len) { if (!fp_) return -1; return fread(buf, 1, len, fp_); }
    int available() { return 0; }
    void close() { if (fp_) { fclose(fp_); fp_ = nullptr; } }
    size_t size() { return 0; }
    bool seek(uint32_t pos) { if (!fp_) return false; return fseek(fp_, pos, SEEK_SET) == 0; }
    bool seek(uint32_t pos, SeekMode mode) {
        if (!fp_) return false;
        int whence = SEEK_SET;
        if (mode == SeekCur) whence = SEEK_CUR;
        else if (mode == SeekEnd) whence = SEEK_END;
        return fseek(fp_, pos, whence) == 0;
    }
    String name() { return String(""); }
    int peek() { return -1; }
    void flush() { if (fp_) fflush(fp_); }

private:
    FILE* fp_;
};

} // namespace fs

using fs::File;
using fs::SeekMode;
using fs::SeekSet;
using fs::SeekCur;
using fs::SeekEnd;
