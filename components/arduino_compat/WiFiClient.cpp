#include "WiFiClient.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <lwip/netdb.h>
#include <unistd.h>
#include <string.h>
#include <sys/select.h>
#include <sys/ioctl.h>

WiFiClient::WiFiClient() : connected_(false), sockfd_(-1), _has_peek(false), _peek_byte(-1) {}

WiFiClient::WiFiClient(int fd) : connected_(false), sockfd_(fd), _has_peek(false), _peek_byte(-1) {
    if (fd >= 0) connected_ = true;
}

WiFiClient::~WiFiClient() { stop(); }

int WiFiClient::connect(IPAddress ip, uint16_t port) {
    if (sockfd_ >= 0) stop();

    sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) return 0;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip;

    int err = ::connect(sockfd_, (struct sockaddr*)&addr, sizeof(addr));
    if (err < 0) {
        ::close(sockfd_);
        sockfd_ = -1;
        return 0;
    }
    connected_ = true;
    return 1;
}

int WiFiClient::connect(const char* host, uint16_t port) {
    if (sockfd_ >= 0) stop();

    struct addrinfo hints = {}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    int err = getaddrinfo(host, port_str, &hints, &res);
    if (err != 0 || res == NULL) return 0;

    sockfd_ = socket(res->ai_family, res->ai_socktype, 0);
    if (sockfd_ < 0) {
        freeaddrinfo(res);
        return 0;
    }

    err = ::connect(sockfd_, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (err < 0) {
        ::close(sockfd_);
        sockfd_ = -1;
        return 0;
    }
    connected_ = true;
    return 1;
}

void WiFiClient::stop() {
    if (sockfd_ >= 0) {
        ::close(sockfd_);
        sockfd_ = -1;
    }
    connected_ = false;
    _has_peek = false;
    _peek_byte = -1;
}

uint8_t WiFiClient::connected() {
    if (sockfd_ < 0) return false;
    struct timeval tv = {0, 0};
    fd_set writefds;
    FD_ZERO(&writefds);
    FD_SET(sockfd_, &writefds);
    int ret = select(sockfd_ + 1, NULL, &writefds, NULL, &tv);
    return ret > 0 && FD_ISSET(sockfd_, &writefds);
}

int WiFiClient::available() {
    if (sockfd_ < 0) return 0;
    if (_has_peek) return 1;
    struct timeval tv = {0, 0};
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sockfd_, &readfds);
    int ret = select(sockfd_ + 1, &readfds, NULL, NULL, &tv);
    if (ret > 0 && FD_ISSET(sockfd_, &readfds)) {
        int count = 0;
        if (ioctl(sockfd_, FIONREAD, &count) == 0) {
            return count;
        }
        return 1;
    }
    return 0;
}

int WiFiClient::read() {
    if (sockfd_ < 0) return -1;
    if (_has_peek) {
        _has_peek = false;
        int c = _peek_byte;
        _peek_byte = -1;
        return c;
    }
    uint8_t c;
    int ret = ::read(sockfd_, &c, 1);
    return (ret == 1) ? c : -1;
}

int WiFiClient::read(uint8_t* buf, int len) {
    return ::read(sockfd_, buf, len);
}

int WiFiClient::peek() {
    if (sockfd_ < 0) return -1;
    if (_has_peek) return _peek_byte;
    uint8_t c;
    int ret = ::read(sockfd_, &c, 1);
    if (ret == 1) {
        _has_peek = true;
        _peek_byte = c;
        return c;
    }
    return -1;
}

size_t WiFiClient::write(uint8_t c) {
    if (sockfd_ < 0) return 0;
    return ::write(sockfd_, &c, 1);
}

size_t WiFiClient::write(const uint8_t* buf, size_t sz) {
    if (sockfd_ < 0) return 0;
    return ::write(sockfd_, buf, sz);
}

void WiFiClient::flush() {}

int WiFiClient::setSocketTimeout(uint16_t timeout) {
    if (sockfd_ < 0) return 0;
    struct timeval tv;
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;
    return setsockopt(sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

int WiFiClient::setTimeout(uint16_t timeout) {
    return setSocketTimeout(timeout);
}

IPAddress WiFiClient::remoteIP() {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (sockfd_ >= 0 && getpeername(sockfd_, (struct sockaddr*)&addr, &len) == 0) {
        return IPAddress(addr.sin_addr.s_addr);
    }
    return IPAddress();
}

uint16_t WiFiClient::remotePort() {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getpeername(sockfd_, (struct sockaddr*)&addr, &len) == 0) {
        return ntohs(addr.sin_port);
    }
    return 0;
}

void WiFiClient::setNoDelay(bool v) {
    int flag = v ? 1 : 0;
    setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
}

int WiFiClient::availableForWrite() {
    return 2048;
}
