#pragma once

#include "Arduino.h"
#include "FS.h"

#define LITTLEFS_MOUNT_POINT "/spiffs"

class LittleFSClass {
public:
    LittleFSClass();
    ~LittleFSClass();

    bool begin(bool formatOnFail = false);
    void end();
    bool format();
    bool exists(const char* path);
    File open(const char* path, const char* mode = "r");
    bool remove(const char* path);
    bool rename(const char* from, const char* to);
    bool mkdir(const char* path);

private:
    bool mounted_;
    void make_path(char* buf, size_t bufsz, const char* path);
};

extern LittleFSClass LittleFS;
