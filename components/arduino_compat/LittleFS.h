#pragma once

#include "Arduino.h"
#include "FS.h"

class LittleFSClass {
public:
    LittleFSClass() : mounted_(false) {}
    
    bool begin(bool formatOnFail = false) {
        // TODO Phase 7: implement with SPIFFS
        mounted_ = true;
        return true;
    }
    
    void end() { mounted_ = false; }
    bool format() { return true; }
    
    bool exists(const char* path) {
        // TODO Phase 7
        return false;
    }
    
    File open(const char* path, const char* mode = "r") {
        // TODO Phase 7
        return File();
    }
    
    bool remove(const char* path) { return true; }
    bool rename(const char* from, const char* to) { return true; }
    bool mkdir(const char* path) { return true; }
    
private:
    bool mounted_;
};

extern LittleFSClass LittleFS;
