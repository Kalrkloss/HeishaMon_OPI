#include "DNSServer.h"
#include <sys/socket.h>
#include <lwip/netdb.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

DNSServer::DNSServer() : sock_(-1), port_(53), errorCode_(NoError) {}

DNSServer::~DNSServer() {
    if (sock_ >= 0) close(sock_);
}

void DNSServer::setErrorReplyCode(DNSReplyCode code) {
    errorCode_ = code;
}

void DNSServer::start(uint16_t port, const char* domain, IPAddress ip) {
    port_ = port;
    domain_ = (domain && strcmp(domain, "*") != 0) ? domain : "";
    ip_ = ip;

    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }

    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ < 0) return;

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock_);
        sock_ = -1;
    }
}

void DNSServer::processNextRequest() {
    if (sock_ < 0) return;

    struct sockaddr_in client;
    socklen_t client_len = sizeof(client);
    uint8_t buf[512];

    int len = recvfrom(sock_, buf, sizeof(buf), MSG_DONTWAIT,
                       (struct sockaddr*)&client, &client_len);
    if (len <= 12) return;
    if (buf[2] & 0x80) return;

    uint16_t qdcount = (buf[4] << 8) | buf[5];
    if (qdcount == 0) return;

    int pos = 12;
    bool valid = false;
    while (pos < len) {
        uint8_t label_len = buf[pos];
        if (label_len == 0) { pos++; valid = true; break; }
        if (label_len & 0xC0) { pos += 2; break; }
        pos += label_len + 1;
    }
    if (!valid || pos + 4 > len) return;

    uint16_t qtype = (buf[pos] << 8) | buf[pos + 1];
    uint16_t qclass = (buf[pos + 2] << 8) | buf[pos + 3];
    if (qtype != 1 || qclass != 1) return;

    buf[2] |= 0x80;
    buf[3] = 0x80;
    buf[6] = 0; buf[7] = 1;
    buf[8] = 0; buf[9] = 0;
    buf[10] = 0; buf[11] = 0;

    int ans = pos + 4;
    if (ans + 16 > (int)sizeof(buf)) return;

    buf[ans]     = 0xC0;
    buf[ans + 1] = 0x0C;
    buf[ans + 2] = 0;
    buf[ans + 3] = 1;
    buf[ans + 4] = 0;
    buf[ans + 5] = 1;
    buf[ans + 6] = 0;
    buf[ans + 7] = 0;
    buf[ans + 8] = 0;
    buf[ans + 9] = 60;
    buf[ans + 10] = 0;
    buf[ans + 11] = 4;
    buf[ans + 12] = ip_[0];
    buf[ans + 13] = ip_[1];
    buf[ans + 14] = ip_[2];
    buf[ans + 15] = ip_[3];

    sendto(sock_, buf, ans + 16, 0, (struct sockaddr*)&client, client_len);
}
