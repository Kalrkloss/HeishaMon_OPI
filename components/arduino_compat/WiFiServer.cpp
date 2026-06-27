#include "WiFiServer.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <sys/select.h>

WiFiServer::WiFiServer(uint16_t port) : port_(port), sockfd_(-1), listening_(false) {}

WiFiServer::~WiFiServer() { stop(); }

void WiFiServer::begin() {
    if (sockfd_ >= 0) stop();

    sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) return;

    int opt = 1;
    setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(sockfd_);
        sockfd_ = -1;
        return;
    }

    if (listen(sockfd_, 8) < 0) {
        ::close(sockfd_);
        sockfd_ = -1;
        return;
    }
    listening_ = true;
}

void WiFiServer::begin(uint16_t port) {
    port_ = port;
    begin();
}

WiFiClient WiFiServer::available() {
    return accept();
}

WiFiClient WiFiServer::accept() {
    if (sockfd_ < 0) return WiFiClient();

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = ::accept(sockfd_, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) return WiFiClient();

    return WiFiClient(client_fd);
}

bool WiFiServer::hasClient() {
    if (sockfd_ < 0) return false;
    struct timeval tv = {0, 0};
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sockfd_, &readfds);
    int ret = select(sockfd_ + 1, &readfds, NULL, NULL, &tv);
    return (ret > 0 && FD_ISSET(sockfd_, &readfds));
}

void WiFiServer::stop() {
    if (sockfd_ >= 0) {
        ::close(sockfd_);
        sockfd_ = -1;
    }
    listening_ = false;
}

void WiFiServer::close() { stop(); }

size_t WiFiServer::write(uint8_t c) {
    return 0;
}

size_t WiFiServer::write(const uint8_t* buf, size_t sz) {
    return 0;
}

void WiFiServer::setNoDelay(bool v) {}

void WiFiServer::setTimeout(uint16_t timeout) {}
