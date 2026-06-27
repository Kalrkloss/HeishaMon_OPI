#pragma once

#include "Server.h"
#include "WiFiClient.h"

class WiFiServer : public Server {
public:
    WiFiServer(uint16_t port);
    virtual ~WiFiServer();

    WiFiClient available() override;
    void begin() override;
    void begin(uint16_t port) override;
    void close() override;
    void stop() override;

    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t sz) override;

    void setNoDelay(bool v);
    void setTimeout(uint16_t timeout);
    bool hasClient();
    WiFiClient accept();

    using Server::available;

private:
    uint16_t port_;
    int sockfd_;
    bool listening_;
};
