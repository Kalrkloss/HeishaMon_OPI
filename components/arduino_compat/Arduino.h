#pragma once

// Platform identification
#define ESP32 1
#define ARDUINO 106

// Types needed by ArduinoJson
typedef struct { } __FlashStringHelper;

// Include Print/Stream before any ArduinoJson includes
#include "Print.h"
#include "Stream.h"

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <string>
#include <algorithm>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_system.h"

// ---- Arduino type aliases ----
typedef bool boolean;
typedef uint8_t byte;
// ---- bit/byte utilities ----
#define lowByte(w) ((uint8_t)((w) & 0xff))
#define highByte(w) ((uint8_t)(((uint16_t)(w)) >> 8))
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define bitWrite(value, bit, bitvalue) ((bitvalue) ? bitSet(value, bit) : bitClear(value, bit))
#define bit(b) (1UL << (b))
typedef unsigned int word;
#define word(high, low) ((uint16_t)(((uint16_t)(high) << 8) | (uint16_t)(low)))

// ---- AVR pgmspace compatibility ----
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#define pgm_read_byte_near(addr) (*(const uint8_t*)(addr))
#define pgm_read_word_near(addr) (*(const uint16_t*)(addr))
#define pgm_read_dword_near(addr) (*(const uint32_t*)(addr))
#define pgm_read_float_near(addr) (*(const float*)(addr))
#define pgm_read_ptr(addr) (*(void* const*)(addr))

// ---- Printable interface (used by ArduinoJson) ----
class Printable {
public:
    virtual size_t printTo(Print& p) const = 0;
    virtual ~Printable() {}
};

// ---- GPIO constants ----
#define HIGH 0x1
#define LOW  0x0
#define INPUT 0x01
#define OUTPUT 0x02
#define INPUT_PULLUP 0x05
#define FUNCTION_0 0
#define FUNCTION_3 3

// ---- Serial config constants (Arduino-ESP32 compatible) ----
#define SERIAL_8N1 0x800001c
#define SERIAL_8E1 0x800001e

// ---- Serial ports ----
class Stream;

class IPAddress;

class Serial_ : public Stream {
private:
    int8_t _uart_nr;
    bool _init;
    int _peek_byte;
    bool _has_peek;
public:
    Serial_(int8_t uart_nr = -1) : _uart_nr(uart_nr), _init(false), _peek_byte(-1), _has_peek(false) {}

    void begin(unsigned long baud, uint32_t config = SERIAL_8N1, int8_t rxPin = -1, int8_t txPin = -1);
    void end();
    void flush() override;

    int available() override;
    int read() override;
    int peek() override;
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t sz) override;

    using Print::print;
    size_t print(const IPAddress& ip);
    using Print::println;

    int printf(const char* fmt, ...);

    operator bool() { return _init || _uart_nr == -1; }
};
extern Serial_ Serial;
extern Serial_ Serial1;
extern Serial_ Serial2;

// ---- Time config (ESP32 Arduino compatibility) ----
bool configTzTime(const char* tz, const char* server1 = nullptr, const char* server2 = nullptr, const char* server3 = nullptr);

// ---- Time ----
inline unsigned long millis() { return (unsigned long)(esp_timer_get_time() / 1000); }
inline unsigned long micros() { return (unsigned long)esp_timer_get_time(); }
inline void delay(unsigned long ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }
inline void delayMicroseconds(unsigned int us) { 
    if (us) { 
        unsigned long start = esp_timer_get_time();
        while (esp_timer_get_time() - start < us) { }
    }
}
inline void yield() { vTaskDelay(1); }

// ---- ESP class ----
#include "ESP.h"

// ---- GPIO shims (implemented later via IDF driver) ----
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t val);
int digitalRead(uint8_t pin);

// ---- Interrupts ----
#define digitalPinToInterrupt(p) (p)
#define CHANGE 2
#define RISING 1
#define FALLING 3
#define ONLOW 4
#define ONHIGH 5
typedef void (*voidFuncPtr)(void);
void attachInterrupt(uint8_t pin, voidFuncPtr callback, int mode);
void detachInterrupt(uint8_t pin);
inline void noInterrupts() {}
inline void interrupts() {}

// ---- System ----
inline void esp_restart_arduino() { esp_restart(); }
#define ESP_RESET esp_restart()

// ---- PROGMEM / PSTR / F() ----
#define PROGMEM
#define PSTR(x) x
#define _F(x) x
#define F(x) x
#define FPSTR(x) x
#define PGM_P const char *

inline size_t strlen_P(const char* s) { return strlen(s); }
inline int sprintf_P(char* buf, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vsprintf(buf, fmt, args);
    va_end(args);
    return ret;
}
inline int snprintf_P(char* buf, size_t size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return ret;
}
inline void* memcpy_P(void* dst, const void* src, size_t n) { return memcpy(dst, src, n); }
inline int memcmp_P(const void* a, const void* b, size_t n) { return memcmp(a, b, n); }
inline char* strstr_P(const char* haystack, const char* needle) { return strstr(haystack, needle); }
inline int strcmp_P(const char* a, const char* b) { return strcmp(a, b); }
inline int strncmp_P(const char* a, const char* b, size_t n) { return strncmp(a, b, n); }
inline char* strcpy_P(char* dst, const char* src) { return strcpy(dst, src); }
inline char* strncpy_P(char* dst, const char* src, size_t n) { return strncpy(dst, src, n); }
inline char* dtostrf(double val, int width, int precision, char* buf) {
    snprintf(buf, 64, "%*.*f", width, precision, val);
    return buf;
}

// ---- Minimal String class (substitute for Arduino String) ----
class String {
public:
    String() {}
    String(const char* s) : str_(s ? s : "") {}
    String(const std::string& s) : str_(s) {}
    String(int val) { char buf[32]; snprintf(buf, sizeof(buf), "%d", val); str_ = buf; }
    String(unsigned int val) { char buf[32]; snprintf(buf, sizeof(buf), "%u", val); str_ = buf; }
    String(long val) { char buf[32]; snprintf(buf, sizeof(buf), "%ld", val); str_ = buf; }
    String(unsigned long val) { char buf[32]; snprintf(buf, sizeof(buf), "%lu", val); str_ = buf; }
    String(float val, int decimals = 2) { char buf[64]; snprintf(buf, sizeof(buf), "%.*f", decimals, val); str_ = buf; }
    String(double val, int decimals = 2) { char buf[64]; snprintf(buf, sizeof(buf), "%.*f", decimals, val); str_ = buf; }

    const char* c_str() const { return str_.c_str(); }
    operator const char*() const { return str_.c_str(); }
    char operator[](int idx) const { return str_[idx]; }
    char& operator[](int idx) { return str_[idx]; }

    unsigned int length() const { return (unsigned int)str_.length(); }
    size_t size() const { return str_.size(); }
    bool isEmpty() const { return str_.empty(); }
    bool equals(const String& other) const { return str_ == other.str_; }
    int compareTo(const String& other) const { return str_.compare(other.str_); }
    bool operator==(const String& other) const { return str_ == other.str_; }
    bool operator!=(const String& other) const { return str_ != other.str_; }

    int toInt() const { return atoi(str_.c_str()); }
    float toFloat() const { return atof(str_.c_str()); }
    double toDouble() const { return atof(str_.c_str()); }

    void reserve(size_t sz) { str_.reserve(sz); }

    String& operator+=(const String& other) { str_ += other.str_; return *this; }
    String& operator+=(const char* s) { str_ += s; return *this; }
    String& operator+=(char c) { str_ += c; return *this; }
    String& operator+=(int val) { *this += String(val); return *this; }
    String& operator+=(unsigned int val) { *this += String(val); return *this; }
    String& operator+=(long val) { *this += String(val); return *this; }
    String& operator+=(unsigned long val) { *this += String(val); return *this; }
    String& operator+=(float val) { *this += String(val); return *this; }
    String operator+(const String& other) const { String r = *this; r += other; return r; }
    friend String operator+(const char* lhs, const String& rhs) { String r(lhs); r += rhs; return r; }

    String substring(int begin, int end) const { return str_.substr(begin, end - begin).c_str(); }
    void toCharArray(char* buf, int len) const { snprintf(buf, len, "%s", str_.c_str()); }
    char charAt(int idx) const { return str_[idx]; }
    int indexOf(char c) const { auto p = str_.find(c); return p == std::string::npos ? -1 : (int)p; }
    int indexOf(const char* s) const { auto p = str_.find(s); return p == std::string::npos ? -1 : (int)p; }
    int lastIndexOf(char c) const { auto p = str_.rfind(c); return p == std::string::npos ? -1 : (int)p; }

    void trim() {
        const char* ws = " \t\n\r\f\v";
        auto start = str_.find_first_not_of(ws);
        if (start == std::string::npos) { str_.clear(); return; }
        auto end = str_.find_last_not_of(ws);
        str_ = str_.substr(start, end - start + 1);
    }

    void replace(char oldChar, char newChar) { std::replace(str_.begin(), str_.end(), oldChar, newChar); }

    int lastIndexOf(char c, int from) const { 
        if (from >= (int)str_.length()) from = str_.length() - 1;
        auto p = str_.rfind(c, from); 
        return p == std::string::npos ? -1 : (int)p; 
    }

    int indexOf(const String& s) const { return indexOf(s.c_str()); }

    bool concat(const char* s) { str_ += s; return true; }
    bool concat(int val) { str_ += String(val).c_str(); return true; }
    bool concat(const String& s) { str_ += s.str_; return true; }

    bool startsWith(const String& s) const { return str_.substr(0, s.str_.length()) == s.str_; }

private:
    std::string str_;
};

// ---- Print::print/println for String (defined after full String class) ----
inline size_t Print::print(const String& s) { return print(s.c_str()); }
inline size_t Print::println(const String& s) { return println(s.c_str()); }
