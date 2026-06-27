#pragma once

#include "Client.h"
#include "IPAddress.h"

class WiFiClient : public Client {
public:
    WiFiClient();
    explicit WiFiClient(int fd);
    virtual ~WiFiClient();

    int connect(IPAddress ip, uint16_t port) override;
    int connect(const char* host, uint16_t port) override;
    void stop() override;
    uint8_t connected() override;
    int available() override;
    int read() override;
    int read(uint8_t* buf, int len);
    int peek() override;
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t sz) override;
    void flush() override;
    int setSocketTimeout(uint16_t timeout) override;

    int setTimeout(uint16_t timeout);
    int availableForWrite();
    size_t write_P(const uint8_t* buf, size_t sz) { return write(buf, sz); }
    size_t write_P(const char* buf) { return write((const uint8_t*)buf, strlen(buf)); }
    IPAddress remoteIP();
    uint16_t remotePort();
    void setNoDelay(bool v);

protected:
    bool connected_;
    int sockfd_;
    bool _has_peek;
    int _peek_byte;

    friend class WiFiServer;
};
