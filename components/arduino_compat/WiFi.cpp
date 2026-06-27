#include "WiFi.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include <string.h>
#include <algorithm>

static const char* TAG = "WiFi";
static bool s_wifi_init = false;
static bool s_scan_done = false;
static int s_scan_count = 0;
static uint16_t s_scan_status = WIFI_SCAN_FAILED;
static SemaphoreHandle_t s_scan_sem = NULL;

extern "C" {
    static void wifi_event_cb(void* arg, esp_event_base_t base, int32_t id, void* data) {
        if (base == WIFI_EVENT) {
            switch (id) {
                case WIFI_EVENT_STA_START:
                    break;
                case WIFI_EVENT_STA_CONNECTED:
                    break;
                case WIFI_EVENT_STA_DISCONNECTED:
                    break;
                case WIFI_EVENT_SCAN_DONE: {
                    wifi_event_sta_scan_done_t* scan = (wifi_event_sta_scan_done_t*)data;
                    s_scan_count = scan->number;
                    s_scan_status = scan->status;
                    s_scan_done = true;
                    if (s_scan_sem) xSemaphoreGive(s_scan_sem);
                    break;
                }
                default:
                    break;
            }
        } else if (base == IP_EVENT) {
            if (id == IP_EVENT_STA_GOT_IP) {
                ip_event_got_ip_t* event = (ip_event_got_ip_t*)data;
                ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
            }
        }
    }
}

static void wifi_lazy_init() {
    if (s_wifi_init) return;
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_cb, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_cb, NULL);

    s_scan_sem = xSemaphoreCreateBinary();
    s_wifi_init = true;
}

wl_status_t WiFiClass::status() {
    wifi_ap_record_t ap;
    esp_err_t ret = esp_wifi_sta_get_ap_info(&ap);
    if (ret == ESP_OK) return WL_CONNECTED;
    return WL_DISCONNECTED;
}

IPAddress WiFiClass::localIP() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return IPAddress();
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        return IPAddress(ip.ip.addr);
    }
    return IPAddress();
}

IPAddress WiFiClass::subnetMask() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return IPAddress(255,255,255,0);
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        return IPAddress(ip.netmask.addr);
    }
    return IPAddress(255,255,255,0);
}

IPAddress WiFiClass::gatewayIP() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return IPAddress();
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        return IPAddress(ip.gw.addr);
    }
    return IPAddress();
}

int WiFiClass::begin(const char* ssid, const char* pwd) {
    wifi_lazy_init();

    wifi_config_t cfg = {};
    strncpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (pwd) {
        strncpy((char*)cfg.sta.password, pwd, sizeof(cfg.sta.password) - 1);
    }

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_start();

    return WL_CONNECTED;
}

void WiFiClass::disconnect(bool erase) {
    esp_wifi_disconnect();
}

void WiFiClass::mode(int m) {
    wifi_lazy_init();
    wifi_mode_t mode = WIFI_MODE_STA;
    switch (m) {
        case 1: mode = WIFI_MODE_AP; break;
        case 2: mode = WIFI_MODE_STA; break;
        case 3: mode = WIFI_MODE_APSTA; break;
    }
    esp_wifi_set_mode(mode);
    esp_wifi_start();
}

void WiFiClass::persistent(bool p) {
}

void WiFiClass::softAPConfig(IPAddress local, IPAddress gateway, IPAddress subnet) {
    wifi_lazy_init();
    esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_netif) return;

    esp_netif_ip_info_t ip;
    ip.ip.addr = local;
    ip.gw.addr = gateway;
    ip.netmask.addr = subnet;
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip);
    esp_netif_dhcps_start(ap_netif);
}

void WiFiClass::softAP(const char* ssid) {
    wifi_lazy_init();
    wifi_config_t cfg = {};
    strncpy((char*)cfg.ap.ssid, ssid, sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = strlen(ssid);
    cfg.ap.max_connection = 4;
    cfg.ap.authmode = WIFI_AUTH_OPEN;

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &cfg);
    esp_wifi_start();
}

int WiFiClass::softAPgetStationNum() {
    wifi_sta_list_t stations;
    esp_wifi_ap_get_sta_list(&stations);
    return stations.num;
}

void WiFiClass::softAPdisconnect(bool wait) {
    esp_wifi_disconnect();
}

String WiFiClass::softAPSSID() {
    wifi_config_t cfg;
    esp_wifi_get_config(WIFI_IF_AP, &cfg);
    return String((const char*)cfg.ap.ssid);
}

int16_t WiFiClass::scanNetworks() {
    return scanNetworks(false);
}

int16_t WiFiClass::scanNetworks(bool async) {
    wifi_lazy_init();

    wifi_scan_config_t scan_cfg = {};
    s_scan_done = false;
    s_scan_count = 0;
    s_scan_status = WIFI_SCAN_FAILED;

    esp_wifi_scan_start(&scan_cfg, !async);

    if (!async) {
        return s_scan_count;
    }
    return WIFI_SCAN_FAILED;
}

void WiFiClass::scanNetworksAsync(void (*cb)(int)) {
    scanNetworks(true);
}

int16_t WiFiClass::scanComplete() {
    if (s_scan_done) {
        return s_scan_count;
    }
    return WIFI_SCAN_FAILED;
}

void WiFiClass::scanDelete() {
    s_scan_done = false;
    s_scan_count = 0;
    s_scan_status = WIFI_SCAN_FAILED;
}

void WiFiClass::setSleep(bool on) {
    esp_wifi_set_ps(on ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
}

String WiFiClass::SSID() {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return String((const char*)ap.ssid);
    }
    return String("");
}

String WiFiClass::SSID(uint8_t net) {
    return String("");
}

String WiFiClass::psk() {
    wifi_config_t cfg;
    esp_wifi_get_config(WIFI_IF_STA, &cfg);
    return String((const char*)cfg.sta.password);
}

int8_t WiFiClass::RSSI() {
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return -100;
}

int8_t WiFiClass::RSSI(uint8_t net) {
    return -100;
}

bool WiFiClass::isConnected() {
    return status() == WL_CONNECTED;
}

int WiFiClass::getMode() {
    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    switch (mode) {
        case WIFI_MODE_AP: return 1;
        case WIFI_MODE_STA: return 2;
        case WIFI_MODE_APSTA: return 3;
        default: return 0;
    }
}

void WiFiClass::setScanMethod(int method) {
}

void WiFiClass::printDiag(Stream& s) {
    s.print("WiFi Mode: ");
    s.print(getMode());
    s.print("\n");
}

String WiFiClass::macAddress() {
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char buf[18];
    snprintf(buf, sizeof(buf), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

void WiFiClass::setHostname(const char* hn) {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_set_hostname(netif, hn);
    }
}

String WiFiClass::getHostname() {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        const char* hn;
        if (esp_netif_get_hostname(netif, &hn) == ESP_OK) {
            return String(hn);
        }
    }
    return String("");
}
