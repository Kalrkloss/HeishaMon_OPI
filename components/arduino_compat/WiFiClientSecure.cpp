#include "WiFiClientSecure.h"
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <lwip/netdb.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

WiFiClientSecure::WiFiClientSecure()
    : WiFiClient(), _ssl_initialized(false), _client_cert(nullptr), _client_pk(nullptr),
      _ca_cert_pem(nullptr), _client_cert_pem(nullptr), _client_key_pem(nullptr),
      _read_buf_avail(0), _read_buf_idx(0), _has_peek_buf(false), _peek_buf_byte(-1) {}

WiFiClientSecure::~WiFiClientSecure() {
    stop();
}

void WiFiClientSecure::setCACert(const char* cert) { _ca_cert_pem = cert; }
void WiFiClientSecure::setCertificate(const char* cert) { _client_cert_pem = cert; }
void WiFiClientSecure::setPrivateKey(const char* key) { _client_key_pem = key; }

int WiFiClientSecure::_bio_recv(void* ctx, unsigned char* buf, size_t len) {
    WiFiClientSecure* self = (WiFiClientSecure*)ctx;
    if (self->sockfd_ < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    int ret = ::read(self->sockfd_, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    if (ret == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
    return ret;
}

int WiFiClientSecure::_bio_send(void* ctx, const unsigned char* buf, size_t len) {
    WiFiClientSecure* self = (WiFiClientSecure*)ctx;
    if (self->sockfd_ < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    int ret = ::write(self->sockfd_, buf, len);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    return ret;
}

bool WiFiClientSecure::_setup_ssl(const char* host) {
    mbedtls_ssl_init(&_ssl);
    mbedtls_ssl_config_init(&_config);
    mbedtls_x509_crt_init(&_cacert);

    mbedtls_ssl_config_defaults(&_config, MBEDTLS_SSL_IS_CLIENT,
                                MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);

    mbedtls_ssl_conf_authmode(&_config, MBEDTLS_SSL_VERIFY_REQUIRED);

    if (_ca_cert_pem) {
        int ret = mbedtls_x509_crt_parse(&_cacert,
                    (const unsigned char*)_ca_cert_pem, strlen(_ca_cert_pem) + 1);
        if (ret != 0) {
            _teardown_ssl();
            return false;
        }
        mbedtls_ssl_conf_ca_chain(&_config, &_cacert, NULL);
    } else {
        mbedtls_ssl_conf_authmode(&_config, MBEDTLS_SSL_VERIFY_NONE);
    }

    if (_client_cert_pem && _client_key_pem) {
        _client_cert = new mbedtls_x509_crt;
        _client_pk = new mbedtls_pk_context;
        mbedtls_x509_crt_init(_client_cert);
        mbedtls_pk_init(_client_pk);

        int ret = mbedtls_x509_crt_parse(_client_cert,
                    (const unsigned char*)_client_cert_pem, strlen(_client_cert_pem) + 1);
        if (ret != 0) {
            _teardown_ssl();
            return false;
        }

        ret = mbedtls_pk_parse_key(_client_pk,
                (const unsigned char*)_client_key_pem, strlen(_client_key_pem) + 1, NULL, 0,
                NULL, NULL);
        if (ret != 0) {
            _teardown_ssl();
            return false;
        }

        mbedtls_ssl_conf_own_cert(&_config, _client_cert, _client_pk);
    }

    int ret = mbedtls_ssl_setup(&_ssl, &_config);
    if (ret != 0) {
        _teardown_ssl();
        return false;
    }

    if (host) {
        mbedtls_ssl_set_hostname(&_ssl, host);
    }

    mbedtls_ssl_set_bio(&_ssl, this, _bio_send, _bio_recv, NULL);

    _read_buf_avail = 0;
    _read_buf_idx = 0;
    _has_peek_buf = false;
    _peek_buf_byte = -1;
    _ssl_initialized = true;
    return true;
}

void WiFiClientSecure::_teardown_ssl() {
    if (_ssl_initialized) {
        mbedtls_ssl_free(&_ssl);
        mbedtls_ssl_config_free(&_config);
        mbedtls_x509_crt_free(&_cacert);
        _ssl_initialized = false;
    }
    if (_client_cert) {
        mbedtls_x509_crt_free(_client_cert);
        delete _client_cert;
        _client_cert = nullptr;
    }
    if (_client_pk) {
        mbedtls_pk_free(_client_pk);
        delete _client_pk;
        _client_pk = nullptr;
    }
    _read_buf_avail = 0;
    _read_buf_idx = 0;
}

int WiFiClientSecure::connect(IPAddress ip, uint16_t port) {
    char host[16];
    snprintf(host, sizeof(host), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return connect(host, port);
}

int WiFiClientSecure::connect(const char* host, uint16_t port) {
    stop();

    int ret = WiFiClient::connect(host, port);
    if (ret != 1) return 0;

    if (!_setup_ssl(host)) {
        WiFiClient::stop();
        return 0;
    }

    ret = mbedtls_ssl_handshake(&_ssl);

    if (ret == 0) {
        connected_ = true;
        return 1;
    }

    _teardown_ssl();
    WiFiClient::stop();
    return 0;
}

void WiFiClientSecure::stop() {
    if (_ssl_initialized) {
        mbedtls_ssl_close_notify(&_ssl);
    }
    _teardown_ssl();
    WiFiClient::stop();
}

uint8_t WiFiClientSecure::connected() {
    if (!_ssl_initialized || sockfd_ < 0) return false;
    if (_read_buf_avail > 0) return true;
    int ret = mbedtls_ssl_read(&_ssl, NULL, 0);
    return ret != MBEDTLS_ERR_SSL_CONN_EOF;
}

int WiFiClientSecure::_fill_read_buf() {
    if (_read_buf_avail > 0) return _read_buf_avail;
    if (!_ssl_initialized) return -1;

    int ret = mbedtls_ssl_read(&_ssl, _read_buf, sizeof(_read_buf));
    if (ret > 0) {
        _read_buf_avail = ret;
        _read_buf_idx = 0;
        return ret;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ) return 0;
    return ret;
}

int WiFiClientSecure::available() {
    if (_has_peek_buf) return 1 + (_read_buf_avail - _read_buf_idx);
    return _read_buf_avail - _read_buf_idx + (_fill_read_buf() > 0 ? 1 : 0);
}

int WiFiClientSecure::read() {
    if (_has_peek_buf) {
        _has_peek_buf = false;
        int c = _peek_buf_byte;
        _peek_buf_byte = -1;
        return c;
    }
    if (_read_buf_avail == 0) {
        if (_fill_read_buf() <= 0) return -1;
    }
    int c = _read_buf[_read_buf_idx++];
    _read_buf_avail--;
    return c;
}

int WiFiClientSecure::read(uint8_t* buf, int len) {
    if (_read_buf_avail > 0) {
        int to_copy = len < (int)_read_buf_avail ? len : (int)_read_buf_avail;
        memcpy(buf, _read_buf + _read_buf_idx, to_copy);
        _read_buf_idx += to_copy;
        _read_buf_avail -= to_copy;
        return to_copy;
    }
    if (!_ssl_initialized) return -1;
    int ret = mbedtls_ssl_read(&_ssl, buf, len);
    if (ret >= 0) return ret;
    if (ret == MBEDTLS_ERR_SSL_WANT_READ) return 0;
    return -1;
}

int WiFiClientSecure::peek() {
    if (_has_peek_buf) return _peek_buf_byte;
    if (_read_buf_avail == 0) {
        if (_fill_read_buf() <= 0) return -1;
    }
    _has_peek_buf = true;
    _peek_buf_byte = _read_buf[_read_buf_idx];
    return _peek_buf_byte;
}

size_t WiFiClientSecure::write(uint8_t c) {
    return write(&c, 1);
}

size_t WiFiClientSecure::write(const uint8_t* buf, size_t sz) {
    if (!_ssl_initialized) return 0;
    size_t written = 0;
    while (written < sz) {
        int ret = mbedtls_ssl_write(&_ssl, buf + written, sz - written);
        if (ret > 0) {
            written += ret;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            return written;
        }
    }
    return written;
}

void WiFiClientSecure::flush() {}

int WiFiClientSecure::setSocketTimeout(uint16_t timeout) {
    if (sockfd_ < 0) return 0;
    struct timeval tv;
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;
    return setsockopt(sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}
