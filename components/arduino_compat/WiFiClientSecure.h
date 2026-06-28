#pragma once

#include "WiFiClient.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"

class WiFiClientSecure : public WiFiClient {
public:
    WiFiClientSecure();
    virtual ~WiFiClientSecure();

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

    void setCACert(const char* cert);
    void setCertificate(const char* cert);
    void setPrivateKey(const char* key);

private:
    bool _ssl_initialized;
    mbedtls_ssl_context _ssl;
    mbedtls_ssl_config _config;
    mbedtls_x509_crt _cacert;
    mbedtls_x509_crt *_client_cert;
    mbedtls_pk_context *_client_pk;

    const char* _ca_cert_pem;
    const char* _client_cert_pem;
    const char* _client_key_pem;

    uint8_t _read_buf[512];
    size_t _read_buf_avail;
    size_t _read_buf_idx;
    bool _has_peek_buf;
    int _peek_buf_byte;

    bool _setup_ssl(const char* host);
    void _teardown_ssl();
    int _fill_read_buf();

    static int _bio_recv(void* ctx, unsigned char* buf, size_t len);
    static int _bio_send(void* ctx, const unsigned char* buf, size_t len);
};
