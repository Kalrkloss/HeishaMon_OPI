#pragma once

#include "Arduino.h"
#include "IPAddress.h"

enum DNSReplyCode { NoError = 0 };

class DNSServer {
public:
    void setErrorReplyCode(DNSReplyCode code) {}
    void start(uint16_t port, const char* domain, IPAddress ip) {}
    void processNextRequest() {}
};
