#pragma once

#include <stdint.h>
#include <cstring>
#include "Arduino.h"
#include "lwip/ip4_addr.h"

class IPAddress {
public:
    IPAddress() : addr_(0) {}
    IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : addr_((uint32_t)a << 24 | (uint32_t)b << 16 | (uint32_t)c << 8 | d) {}
    IPAddress(uint32_t raw) : addr_(raw) {}
    IPAddress(const ip4_addr_t& ip) : addr_(ip.addr) {}

    operator uint32_t() const { return addr_; }
    uint32_t operator*() const { return addr_; }

    IPAddress& operator=(uint32_t raw) { addr_ = raw; return *this; }

    bool operator==(const IPAddress& other) const { return addr_ == other.addr_; }

    operator bool() const { return addr_ != 0; }

    String toString() const {
        char buf[20];
        snprintf(buf, sizeof(buf), "%d.%d.%d.%d",
                 (uint8_t)(addr_ >> 24), (uint8_t)(addr_ >> 16),
                 (uint8_t)(addr_ >> 8), (uint8_t)addr_);
        return String(buf);
    }

    uint8_t operator[](int idx) const {
        switch (idx) {
            case 0: return (uint8_t)(addr_ >> 24);
            case 1: return (uint8_t)(addr_ >> 16);
            case 2: return (uint8_t)(addr_ >> 8);
            case 3: return (uint8_t)addr_;
            default: return 0;
        }
    }

private:
    uint32_t addr_;
};

inline IPAddress ip4_addr_to_IPAddress(const ip4_addr_t& ip) { return IPAddress(ip); }
