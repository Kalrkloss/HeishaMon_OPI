#pragma once

#include <stdint.h>
#include "IPAddress.h"
#include "WiFiClient.h"
#include "esp_wifi.h"

typedef enum {
    WL_NO_SHIELD       = 255,
    WL_IDLE_STATUS     = 0,
    WL_NO_SSID_AVAIL   = 1,
    WL_SCAN_COMPLETED  = 2,
    WL_CONNECTED       = 3,
    WL_CONNECT_FAILED  = 4,
    WL_CONNECTION_LOST = 5,
    WL_DISCONNECTED    = 6
} wl_status_t;

// Arduino WiFi mode constants (map to ESP-IDF enum values)
#define WIFI_STA     WIFI_MODE_STA
#define WIFI_AP      WIFI_MODE_AP
#define WIFI_AP_STA  WIFI_MODE_APSTA
#define WIFI_SCAN_FAILED        -1

class WiFiClass {
public:
    wl_status_t status();
    IPAddress localIP();
    IPAddress subnetMask();
    IPAddress gatewayIP();
    
    int begin(const char* ssid, const char* pwd = nullptr);
    void disconnect(bool erase = false);
    void mode(int m);
    void persistent(bool p);

    void softAPConfig(IPAddress local, IPAddress gateway, IPAddress subnet);
    void softAP(const char* ssid);
    String softAPSSID();
    int softAPgetStationNum();
    void softAPdisconnect(bool wait = false);

    int16_t scanComplete();
    int16_t scanNetworks();
    int16_t scanNetworks(bool async);
    void scanNetworksAsync(void (*cb)(int));
    void setSleep(bool on);

    String SSID();
    String SSID(uint8_t net);
    String psk();
    int8_t RSSI();
    int8_t RSSI(uint8_t net);
    bool isConnected();
    void scanDelete();

    int getMode();
    void setScanMethod(int method);
    void printDiag(Stream& s);
    String macAddress();

    void setHostname(const char* hn);
    String getHostname();
};

extern WiFiClass WiFi;
