#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

class String;

static inline size_t _print_int(char* buf, size_t bufsz, uint64_t val, int base, bool sign) {
    const char* digits = "0123456789abcdef";
    char tmp[65];
    size_t pos = sizeof(tmp);
    tmp[--pos] = 0;
    uint64_t v = sign ? (val >> 63 ? (uint64_t)(-(int64_t)val) : val) : val;
    do {
        tmp[--pos] = digits[v % base];
        v /= base;
    } while (v);
    if (sign && (int64_t)val < 0) tmp[--pos] = '-';
    size_t len = sizeof(tmp) - pos - 1;
    if (len >= bufsz) len = bufsz - 1;
    memcpy(buf, tmp + pos, len);
    buf[len] = 0;
    return len;
}

class Print {
public:
    Print() {}
    virtual ~Print() {}

    virtual size_t write(const uint8_t* buf, size_t sz) { return 0; }
    virtual size_t write(uint8_t c) { return 0; }
    size_t write(const char* s) { return write((const uint8_t*)s, strlen(s)); }

    size_t print(const char* s) { return write(s); }
    size_t print(char c) { return write((uint8_t)c); }
    size_t print(int val, int base = 10) { char buf[32]; _print_int(buf, sizeof(buf), (uint64_t)(int64_t)val, base, true); return write(buf); }
    size_t print(unsigned int val, int base = 10) { char buf[32]; _print_int(buf, sizeof(buf), val, base, false); return write(buf); }
    size_t print(long val, int base = 10) { char buf[32]; _print_int(buf, sizeof(buf), (uint64_t)(int64_t)val, base, true); return write(buf); }
    size_t print(unsigned long val, int base = 10) { char buf[32]; _print_int(buf, sizeof(buf), val, base, false); return write(buf); }
    size_t print(double val, int digits = 2) { char buf[64]; snprintf(buf, sizeof(buf), "%.*f", digits, val); return write(buf); }
    size_t print(const String& s);

    size_t println(const char* s) { size_t n = print(s); return n + print("\r\n"); }
    size_t println(char c) { size_t n = print(c); return n + print("\r\n"); }
    size_t println(int val, int base = 10) { size_t n = print(val, base); return n + print("\r\n"); }
    size_t println(unsigned int val, int base = 10) { size_t n = print(val, base); return n + print("\r\n"); }
    size_t println(long val, int base = 10) { size_t n = print(val, base); return n + print("\r\n"); }
    size_t println(unsigned long val, int base = 10) { size_t n = print(val, base); return n + print("\r\n"); }
    size_t println(double val, int digits = 2) { size_t n = print(val, digits); return n + print("\r\n"); }
    size_t println(const String& s);
    size_t println() { return print("\r\n"); }

    int printf(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        int ret = vprintf(fmt, args);
        va_end(args);
        return ret;
    }
};
