#pragma once

#include "Arduino.h"
#include "IPAddress.h"

enum DNSReplyCode { NoError = 0, FormError = 1, ServerFailure = 2, NXDomain = 3 };

class DNSServer {
public:
    DNSServer();
    ~DNSServer();
    void setErrorReplyCode(DNSReplyCode code);
    void start(uint16_t port, const char* domain, IPAddress ip);
    void processNextRequest();

private:
    int sock_;
    uint16_t port_;
    IPAddress ip_;
    String domain_;
    DNSReplyCode errorCode_;
};
