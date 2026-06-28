#include "webfunctions.h"
#include "decode.h"
#include "version.h"
#include "htmlcode.h"
#include "commands.h"
#include "src/common/progmem.h"
#include "src/common/webserver.h"
#include "src/common/timerqueue.h"

#include "lwip/apps/sntp.h"
#include "lwip/dns.h"

#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#include <time.h>
#include <memory>

#define UPTIME_OVERFLOW 4294967295 // Uptime overflow value

void log_message(char* string);
void log_message(const char *msg);

/*
 * dBmToQuality -- Convert WiFi RSSI (dBm) to a 0-100% quality percentage.
 *   dBm == 31   -> -1 (invalid / not connected)
 *   dBm <= -100 ->   0 (worst)
 *   dBm >= -50  -> 100 (best)
 *   Otherwise   -> linear mapping: 2 * (dBm + 100)
 * Thread-safety: Reentrant (pure function, no shared state).
 */
int dBmToQuality(int dBm) {
  if (dBm == 31)
    return -1;
  if (dBm <= -100)
    return 0;
  if (dBm >= -50)
    return 100;
  return 2 * (dBm + 100);
}


/*
 * getWifiScanResults -- Sort discovered WiFi networks by RSSI (descending),
 *   remove duplicate SSIDs, serialize the deduplicated list as JSON to
 *   "/wifiscan.json" on LittleFS, then call WiFi.scanDelete().
 *   Called after a background scan completes.
 * Thread-safety: Must not be called concurrently with other LittleFS writes.
 *   Accesses WiFi scan results which are valid only until scanDelete().
 */
void getWifiScanResults(int numSsid) {
  if (numSsid > 0) { //found wifi networks
    int indexes[numSsid];
    for (int i = 0; i < numSsid; i++) { //fill the sorted list with normal indexes first
      indexes[i] = i;
    }
    for (int i = 0; i < numSsid; i++) { //then sort
      for (int j = i + 1; j < numSsid; j++) {
        if (WiFi.RSSI(indexes[j]) > WiFi.RSSI(indexes[i])) {
          int temp = indexes[j];
          indexes[j] = indexes[i];
          indexes[i] = temp;
        }
      }
    }
    String ssid;
    for (int i = 0; i < numSsid; i++) { //then remove duplicates
      if (indexes[i] == -1) continue;
      ssid = WiFi.SSID(indexes[i]);
      for (int j = i + 1; j < numSsid; j++) {
        if (ssid == WiFi.SSID(indexes[j])) {
          indexes[j] = -1;
        }
      }
    }
    JsonDocument wifiJsonDoc;
    JsonArray wifiJsonArray = wifiJsonDoc.to<JsonArray>();
    for (int i = 0; i < numSsid; i++) { //then output json
      if (indexes[i] == -1) {
        continue;
      }
      JsonObject wifiJsonObject = wifiJsonArray.add<JsonObject>();
      wifiJsonObject[F("ssid")] = WiFi.SSID(indexes[i]);
      String quality = String(dBmToQuality(WiFi.RSSI(indexes[i]))) + "%";
      wifiJsonObject[F("rssi")] = quality;
    }
    saveJsonToFile(wifiJsonDoc,"/wifiscan.json");
    WiFi.scanDelete();
  }
}

/*
 * getWifiQuality -- Return the current WiFi connection quality (0-100%),
 *   or -1 if not connected to any AP.  Delegates to dBmToQuality().
 * Thread-safety: Reentrant (only reads WiFi.status() / WiFi.RSSI()).
 */
int getWifiQuality() {
  if (WiFi.status() != WL_CONNECTED)
    return -1;
  return dBmToQuality(WiFi.RSSI());
}

/*
 * getFreeMemory -- Return free heap memory as a percentage of total heap.
 *   Total heap is captured once on the first call (static cache).
 * Thread-safety: The static total_memory is written once and read-only
 *   afterwards; ESP.getFreeHeap() is safe to call from any context.
 */
int getFreeMemory() {
  static uint32_t total_memory = 0;  // Cached total heap size, set on first call  
  if ( 0 == total_memory ) total_memory = ESP.getHeapSize();

  uint32_t free_memory   = ESP.getFreeHeap();
  return (100 * free_memory / total_memory ) ; // as a %
}

/*
 * getUptime -- Return a human-readable uptime string ("X day(s) Y hour(s) ...").
 *   Handles millis() rollover (~49.7 days) by counting overflows via a static
 *   counter.  Allocates a heap buffer that the caller must free.
 * Thread-safety: NOT reentrant.  The static variables (last_uptime,
 *   uptime_overflows) are shared state.  If called from multiple tasks
 *   the uptime may be momentarily inconsistent (benign for display).
 */
char *getUptime(void) {
  static uint32_t last_uptime      = 0;  // Previous millis() value, used to detect rollover
  static uint8_t  uptime_overflows = 0;  // Number of times millis() wrapped around

  if (millis() < last_uptime) {
    ++uptime_overflows;
  }
  last_uptime             = millis();
  uint32_t t = uptime_overflows * (UPTIME_OVERFLOW / 1000) + (last_uptime / 1000);

  uint8_t  d   = t / 86400L;
  uint8_t  h   = ((t % 86400L) / 3600L) % 60;
  uint32_t rem = t % 3600L;
  uint8_t  m   = rem / 60;
  uint8_t  sec = rem % 60;

  unsigned int len = snprintf_P(NULL, 0, PSTR("%d day%s %d hour%s %d minute%s %d second%s"), d, (d == 1) ? "" : "s", h, (h == 1) ? "" : "s", m, (m == 1) ? "" : "s", sec, (sec == 1) ? "" : "s");

  char *str = (char *)malloc(len + 2);
  if (str == NULL) {
    Serial1.printf("Out of memory %s:#%d\n", __FUNCTION__, __LINE__);
    ESP.restart();
    exit(-1);
  }

  memset(str, 0, len + 2);
  snprintf_P(str, len + 1, PSTR("%d day%s %d hour%s %d minute%s %d second%s"), d, (d == 1) ? "" : "s", h, (h == 1) ? "" : "s", m, (m == 1) ? "" : "s", sec, (sec == 1) ? "" : "s");
  return str;
}



/*
 * ntpReload -- Configure timezone and NTP servers from the current settings.
 *   Parses the comma-separated NTP server list (up to 3 servers) and calls
 *   configTzTime().  Falls back to "pool.ntp.org" if the list is empty.
 * Thread-safety: Modifies global timezone state (setenv/tzset/configTzTime).
 *   Should only be called from the main setup / settings-save path, not
 *   concurrently.
 */
void ntpReload(settingsStruct *heishamonSettings) {
  tzStruct tzCfg;
  memcpy_P(&tzCfg, &tzdata[heishamonSettings->timezone], sizeof(tzCfg));
  setenv("TZ", tzCfg.value, 1);
  tzset();

  // Parse comma-separated NTP server list and pass up to 3 servers to configTzTime.
  char ntpList[sizeof(heishamonSettings->ntp_servers)];
  strlcpy(ntpList, heishamonSettings->ntp_servers, sizeof(ntpList));

  char *servers[3] = { NULL, NULL, NULL };
  uint8_t count = 0;
  char *token = strtok(ntpList, ",");
  while (token != NULL && count < 3) {
    while (*token == ' ') {
      token++;
    }
    if (*token != '\0') {
      servers[count++] = token;
    }
    token = strtok(NULL, ",");
  }

  if (count == 0) {
    configTzTime(tzCfg.value, "pool.ntp.org");
  } else if (count == 1) {
    configTzTime(tzCfg.value, servers[0]);
  } else if (count == 2) {
    configTzTime(tzCfg.value, servers[0], servers[1]);
  } else {
    configTzTime(tzCfg.value, servers[0], servers[1], servers[2]);
  }
}

/*
 * loadSettings -- Read and parse "/config.json" from LittleFS into the
 *   provided settingsStruct.  Each field is validated (bounds checks on
 *   waitTime, dallasResolution, etc.).  If the file is missing or corrupt
 *   the WiFi persistent credentials are cleared (forcing config reset).
 *   On success ntpReload() is called to apply timezone / NTP changes.
 * Thread-safety: NOT reentrant.  Accesses LittleFS and modifies the
 *   settingsStruct in place.  Must not be called concurrently with any
 *   other LittleFS operation.
 */
void loadSettings(settingsStruct *heishamonSettings) {
  log_message(_F("mounting FS..."));

  if (LittleFS.begin()) {
    log_message(_F("mounted file system"));
    if (LittleFS.exists("/config.json")) {
      //file exists, reading and loading
      log_message(_F("reading config file"));
      File configFile = LittleFS.open("/config.json", "r");
      if (configFile) {
        log_message(_F("opened config file"));
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        JsonDocument jsonDoc;
        DeserializationError error = deserializeJson(jsonDoc, buf.get());
        char log_msg[1024];
        serializeJson(jsonDoc, log_msg);
        log_message(log_msg);
        if (!error) {
          log_message(_F("parsed json"));
          //read updated parameters, make sure no overflow
          if ( jsonDoc[F("wifi_ssid")] ) strlcpy(heishamonSettings->wifi_ssid, jsonDoc[F("wifi_ssid")], sizeof(heishamonSettings->wifi_ssid));
          if ( jsonDoc[F("wifi_password")] ) strlcpy(heishamonSettings->wifi_password, jsonDoc[F("wifi_password")], sizeof(heishamonSettings->wifi_password));
          if ( jsonDoc[F("wifi_hostname")] ) strlcpy(heishamonSettings->wifi_hostname, jsonDoc[F("wifi_hostname")], sizeof(heishamonSettings->wifi_hostname));
          if ( jsonDoc[F("ota_password")] ) strlcpy(heishamonSettings->ota_password, jsonDoc[F("ota_password")], sizeof(heishamonSettings->ota_password));
          if ( jsonDoc[F("mqtt_topic_base")] ) strlcpy(heishamonSettings->mqtt_topic_base, jsonDoc[F("mqtt_topic_base")], sizeof(heishamonSettings->mqtt_topic_base));
          if ( jsonDoc[F("mqtt_server")] ) strlcpy(heishamonSettings->mqtt_server, jsonDoc[F("mqtt_server")], sizeof(heishamonSettings->mqtt_server));
          if ( jsonDoc[F("mqtt_port")] ) strlcpy(heishamonSettings->mqtt_port, jsonDoc[F("mqtt_port")], sizeof(heishamonSettings->mqtt_port));
          if ( jsonDoc[F("mqtt_username")] ) strlcpy(heishamonSettings->mqtt_username, jsonDoc[F("mqtt_username")], sizeof(heishamonSettings->mqtt_username));
          if ( jsonDoc[F("mqtt_password")] ) strlcpy(heishamonSettings->mqtt_password, jsonDoc[F("mqtt_password")], sizeof(heishamonSettings->mqtt_password));
          if ( jsonDoc[F("ntp_servers")] ) strlcpy(heishamonSettings->ntp_servers, jsonDoc[F("ntp_servers")], sizeof(heishamonSettings->ntp_servers));
          if ( jsonDoc[F("timezone")]) heishamonSettings->timezone = jsonDoc[F("timezone")];
#ifdef TLS_SUPPORT
          heishamonSettings->mqtt_tls_enabled = ( jsonDoc[F("mqtt_tls_enabled")] == "enabled" ) ? true : false; 
#endif
          heishamonSettings->force_rules = ( jsonDoc[F("force_rules")] == "enabled" ) ? true : false;
          heishamonSettings->use_1wire = ( jsonDoc[F("use_1wire")] == "enabled" ) ? true : false;
          heishamonSettings->use_s0 = ( jsonDoc[F("use_s0")] == "enabled" ) ? true : false;
          heishamonSettings->hotspot = ( jsonDoc[F("hotspot")] == "disabled" ) ? false : true; //default to true if not found in settings
          heishamonSettings->listenonly = ( jsonDoc[F("listenonly")] == "enabled" ) ? true : false;
          heishamonSettings->logMqtt = ( jsonDoc[F("logMqtt")] == "enabled" ) ? true : false;
          heishamonSettings->logHexdump = ( jsonDoc[F("logHexdump")] == "enabled" ) ? true : false;
          heishamonSettings->logSerial1 = ( jsonDoc[F("logSerial1")] == "enabled" ) ? true : false;
          heishamonSettings->optionalPCB = ( jsonDoc[F("optionalPCB")] == "enabled" ) ? true : false;
          heishamonSettings->opentherm = ( jsonDoc[F("opentherm")] == "enabled" ) ? true : false;
#ifdef ESP32          
          heishamonSettings->proxy = ( jsonDoc[F("proxy")] == "enabled" ) ? true : false;
#endif          
          if ( jsonDoc[F("waitTime")]) heishamonSettings->waitTime = jsonDoc[F("waitTime")];
          if (heishamonSettings->waitTime < 5) heishamonSettings->waitTime = 5;
          if ( jsonDoc[F("waitDallasTime")]) heishamonSettings->waitDallasTime = jsonDoc[F("waitDallasTime")];
          if (heishamonSettings->waitDallasTime < 5) heishamonSettings->waitDallasTime = 5;
          if ( jsonDoc[F("dallasResolution")]) heishamonSettings->dallasResolution = jsonDoc[F("dallasResolution")];
          if ((heishamonSettings->dallasResolution < 9) || (heishamonSettings->dallasResolution > 12) ) heishamonSettings->dallasResolution = 12;
          if ( jsonDoc[F("updateAllTime")]) heishamonSettings->updateAllTime = jsonDoc[F("updateAllTime")];
          if (heishamonSettings->updateAllTime < heishamonSettings->waitTime) heishamonSettings->updateAllTime = heishamonSettings->waitTime;
          if ( jsonDoc[F("updataAllDallasTime")]) heishamonSettings->updataAllDallasTime = jsonDoc[F("updataAllDallasTime")];
          if (heishamonSettings->updataAllDallasTime < heishamonSettings->waitDallasTime) heishamonSettings->updataAllDallasTime = heishamonSettings->waitDallasTime;
          //if (jsonDoc[F("s0_1_gpio")]) heishamonSettings->s0Settings[0].gpiopin = jsonDoc[F("s0_1_gpio")];
          if (jsonDoc[F("s0_1_ppkwh")]) heishamonSettings->s0Settings[0].ppkwh = jsonDoc[F("s0_1_ppkwh")];
          if (jsonDoc[F("s0_1_interval")]) heishamonSettings->s0Settings[0].lowerPowerInterval = jsonDoc[F("s0_1_interval")];
          if (jsonDoc[F("s0_1_minpulsewidth")]) heishamonSettings->s0Settings[0].minimalPulseWidth = jsonDoc[F("s0_1_minpulsewidth")];
          if (jsonDoc[F("s0_1_maxpulsewidth")]) heishamonSettings->s0Settings[0].maximalPulseWidth = jsonDoc[F("s0_1_maxpulsewidth")];
          //if (jsonDoc[F("s0_2_gpio")]) heishamonSettings->s0Settings[1].gpiopin = jsonDoc[F("s0_2_gpio")];
          if (jsonDoc[F("s0_2_ppkwh")]) heishamonSettings->s0Settings[1].ppkwh = jsonDoc[F("s0_2_ppkwh")];
          if (jsonDoc[F("s0_2_interval")] ) heishamonSettings->s0Settings[1].lowerPowerInterval = jsonDoc[F("s0_2_interval")];
          if (jsonDoc[F("s0_2_minpulsewidth")]) heishamonSettings->s0Settings[1].minimalPulseWidth = jsonDoc[F("s0_2_minpulsewidth")];
          if (jsonDoc[F("s0_2_maxpulsewidth")]) heishamonSettings->s0Settings[1].maximalPulseWidth = jsonDoc[F("s0_2_maxpulsewidth")];
          ntpReload(heishamonSettings);
        } else {
          log_message(_F("Failed to load json config, forcing config reset."));
          WiFi.persistent(true);
          WiFi.disconnect();
          WiFi.persistent(false);
        }
        configFile.close();
      }
    }
    else {
      log_message(_F("No config.json exists! Forcing a config reset."));
      WiFi.persistent(true);
      WiFi.disconnect();
      WiFi.persistent(false);
    }
  } else {
    log_message(_F("failed to mount FS"));
  }
  //end read

}

/*
 * setupWifi -- Configure and connect WiFi in AP+STA mode.
 *   - Disables modem sleep, sets hostname (from settings or "HeishaMon").
 *   - Uses WIFI_ALL_CHANNEL_SCAN to pick the best AP for the configured SSID.
 *   - If SSID is empty and hotspot mode is enabled, starts a soft-AP
 *     "HeishaMon-Setup" at address 192.168.4.1.
 *   - If SSID is set but password is empty, connects with open (no password).
 * Thread-safety: Must be called from the main loop / setup context only.
 *   Modifies global WiFi state.  Not safe from multiple tasks.
 */
void setupWifi(settingsStruct *heishamonSettings) {
  log_message(_F("Wifi reconnecting with new configuration..."));
  //WiFi.setTxPower(WIFI_POWER_8_5dBm); //fix for bad chips
  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPdisconnect(true); 
  delay(100);  // must delay to avoid error 
  WiFi.disconnect(true); 
  //sethostname on ESP32 before wifi.begin
  if (heishamonSettings->wifi_hostname[0] == '\0') {
    //Set hostname on wifi rather than ESP_xxxxx
    WiFi.setHostname("HeishaMon");
  } else {
    WiFi.setHostname(heishamonSettings->wifi_hostname);
  }
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN); //select best AP with same SSID
  if (heishamonSettings->wifi_ssid[0] != '\0') {
     log_message(_F("Wifi client mode..."));
    if (heishamonSettings->wifi_password[0] == '\0') {
        WiFi.begin(heishamonSettings->wifi_ssid);
      } else {
        WiFi.begin(heishamonSettings->wifi_ssid, heishamonSettings->wifi_password);
      }
  }
  else {
    if (heishamonSettings->hotspot) {
      log_message(_F("Wifi hotspot mode..."));
      WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0)); 
      WiFi.softAP("HeishaMon-Setup");
    }
  }
}

/*
 * handleFactoryReset -- Web handler for the factory-reset page.
 *   content 0: send HTTP headers + CSS + meta refresh.
 *   content 1: send the confirmation body text.
 *   content 2: schedule a reboot via timerqueue_insert(1, 0, -1).
 * Thread-safety: Called from the webserver's cooperative multi-part callback.
 *   Not safe for concurrent invocation on the same client.
 */
int handleFactoryReset(struct webserver_t *client) {
  switch (client->content) {
    case 0: {
        webserver_send(client, 200, (char *)"text/html", 0);
        webserver_send_content_P(client, webHeader, strlen_P(webHeader));
        webserver_send_content_P(client, webCSS, strlen_P(webCSS));
        webserver_send_content_P(client, refreshMeta, strlen_P(refreshMeta));
      } break;
    case 1: {
        webserver_send_content_P(client, webBodyStart, strlen_P(webBodyStart));
        webserver_send_content_P(client, webBodyRebootWarning, strlen_P(webBodyRebootWarning));
        webserver_send_content_P(client, menuJS, strlen_P(menuJS));
        webserver_send_content_P(client, webFooter, strlen_P(webFooter));
      } break;
    case 2: {
        timerqueue_insert(1, 0, -1); // Start reboot sequence
      } break;
  }

  return 0;
}

/*
 * handleReboot -- Web handler for the reboot page.
 *   content 0: send HTTP headers + CSS + meta refresh.
 *   content 1: send the confirmation body.
 *   content 2: schedule reboot after 5 s via timerqueue_insert(5, 0, -2).
 * Thread-safety: Same as handleFactoryReset -- single-client callback.
 */
int handleReboot(struct webserver_t *client) {
  switch (client->content) {
    case 0: {
        webserver_send(client, 200, (char *)"text/html", 0);
        webserver_send_content_P(client, webHeader, strlen_P(webHeader));
        webserver_send_content_P(client, webCSS, strlen_P(webCSS));
        webserver_send_content_P(client, refreshMeta, strlen_P(refreshMeta));
      } break;
    case 1: {
        webserver_send_content_P(client, webBodyStart, strlen_P(webBodyStart));
        webserver_send_content_P(client, webBodyRebootWarning, strlen_P(webBodyRebootWarning));
        webserver_send_content_P(client, menuJS, strlen_P(menuJS));
        webserver_send_content_P(client, webFooter, strlen_P(webFooter));
      } break;
    case 2: {
        timerqueue_insert(5, 0, -2); // Start reboot sequence
      } break;
  }

  return 0;
}

/*
 * settingsToJson -- Serialise all fields from settingsStruct into the given
 *   ArduinoJson JsonDocument.  Boolean fields are written as "enabled"/"disabled"
 *   strings.  TLS, optionalPCB, proxy, etc. are gated by their respective
 *   compile-time defines.
 * Thread-safety: Reentrant -- only reads from the settings struct and writes
 *   to a caller-owned JsonDocument (stack / local).
 */
void settingsToJson(JsonDocument &jsonDoc, settingsStruct *heishamonSettings) {
  jsonDoc[F("wifi_hostname")] = heishamonSettings->wifi_hostname;
  jsonDoc[F("wifi_password")] = heishamonSettings->wifi_password;
  jsonDoc[F("wifi_ssid")] = heishamonSettings->wifi_ssid;
  jsonDoc[F("ota_password")] = heishamonSettings->ota_password;
  jsonDoc[F("mqtt_topic_base")] = heishamonSettings->mqtt_topic_base;
  jsonDoc[F("mqtt_server")] = heishamonSettings->mqtt_server;
  jsonDoc[F("mqtt_port")] = heishamonSettings->mqtt_port;
  jsonDoc[F("mqtt_username")] = heishamonSettings->mqtt_username;
  jsonDoc[F("mqtt_password")] = heishamonSettings->mqtt_password;
  jsonDoc[F("ntp_servers")] = heishamonSettings->ntp_servers;
  jsonDoc[F("timezone")] = heishamonSettings->timezone;
#ifdef TLS_SUPPORT
  if (heishamonSettings->mqtt_tls_enabled) {
    jsonDoc[F("mqtt_tls_enabled")] = "enabled";
  } else {
    jsonDoc[F("mqtt_tls_enabled")] = "disabled";
  }
#endif
  if (heishamonSettings->use_1wire) {
    jsonDoc[F("use_1wire")] = "enabled";
  } else {
    jsonDoc[F("use_1wire")] = "disabled";
  }
  if (heishamonSettings->use_s0) {
    jsonDoc[F("use_s0")] = "enabled";
  } else {
    jsonDoc[F("use_s0")] = "disabled";
  }
  if (heishamonSettings->hotspot) {
    jsonDoc[F("hotspot")] = "enabled";
  } else {
    jsonDoc[F("hotspot")] = "disabled";
  }
  if (heishamonSettings->listenonly) {
    jsonDoc[F("listenonly")] = "enabled";
  } else {
    jsonDoc[F("listenonly")] = "disabled";
  }  if (heishamonSettings->force_rules) {
    jsonDoc[F("force_rules")] = "enabled";
  } else {
    jsonDoc[F("force_rules")] = "disabled";
  }
  if (heishamonSettings->logMqtt) {
    jsonDoc[F("logMqtt")] = "enabled";
  } else {
    jsonDoc[F("logMqtt")] = "disabled";
  }
  if (heishamonSettings->logHexdump) {
    jsonDoc[F("logHexdump")] = "enabled";
  } else {
    jsonDoc[F("logHexdump")] = "disabled";
  }
  if (heishamonSettings->logSerial1) {
    jsonDoc[F("logSerial1")] = "enabled";
  } else {
    jsonDoc[F("logSerial1")] = "disabled";
  }
  if (heishamonSettings->optionalPCB) {
    jsonDoc[F("optionalPCB")] = "enabled";
  } else {
    jsonDoc[F("optionalPCB")] = "disabled";
  }
  if (heishamonSettings->opentherm) {
    jsonDoc[F("opentherm")] = "enabled";
  } else {
    jsonDoc[F("opentherm")] = "disabled";
  }
#ifdef ESP32  
  if (heishamonSettings->proxy) {
    jsonDoc[F("proxy")] = "enabled";
  } else {
    jsonDoc[F("proxy")] = "disabled";
  }
#endif 
  jsonDoc[F("waitTime")] = heishamonSettings->waitTime;
  jsonDoc[F("waitDallasTime")] = heishamonSettings->waitDallasTime;
  jsonDoc[F("dallasResolution")] = heishamonSettings->dallasResolution;
  jsonDoc[F("updateAllTime")] = heishamonSettings->updateAllTime;
  jsonDoc[F("updataAllDallasTime")] = heishamonSettings->updataAllDallasTime;
  jsonDoc[F("s0_1_ppkwh")] = heishamonSettings->s0Settings[0].ppkwh;
  jsonDoc[F("s0_1_interval")] = heishamonSettings->s0Settings[0].lowerPowerInterval;
  jsonDoc[F("s0_1_minpulsewidth")] = heishamonSettings->s0Settings[0].minimalPulseWidth;
  jsonDoc[F("s0_1_maxpulsewidth")] = heishamonSettings->s0Settings[0].maximalPulseWidth;
  jsonDoc[F("s0_2_ppkwh")] = heishamonSettings->s0Settings[1].ppkwh;
  jsonDoc[F("s0_2_interval")] = heishamonSettings->s0Settings[1].lowerPowerInterval;
  jsonDoc[F("s0_2_minpulsewidth")] = heishamonSettings->s0Settings[1].minimalPulseWidth;
  jsonDoc[F("s0_2_maxpulsewidth")] = heishamonSettings->s0Settings[1].maximalPulseWidth;  
}

/*
 * saveJsonToFile -- Serialise the given JsonDocument to a named file on
 *   LittleFS.  Opens the file for writing (truncates), serialises, then
 *   closes.  Errors are silently ignored.
 * Thread-safety: NOT reentrant.  LittleFS must not be accessed concurrently.
 *   Typically called from the settings-save path, serialised with other FS ops.
 */
void saveJsonToFile(JsonDocument &jsonDoc, const char* filename) {
  if (LittleFS.begin()) {
    File configFile = LittleFS.open(filename, "w");
    if (configFile) {
      serializeJson(jsonDoc, configFile);
      configFile.close();
    }
  }
}

#ifdef TLS_SUPPORT
/*
 * processCAtmp_to_CAPEM -- Validate and install an uploaded CA certificate
 *   (TLS_SUPPORT only).  Reads "/ca.tmp", checks file size (<=6 kB), parses
 *   the PEM BEGIN/END tags, validates Base64 characters, and writes the
 *   cleaned certificate to "/ca.pem.new" before atomically renaming to
 *   "/ca.pem".  The temporary file is removed on success.
 * Thread-safety: NOT reentrant (LittleFS access).  Called only from
 *   handleCACert in the webserver callback context.
 */
static bool processCAtmp_to_CAPEM(String &outMsg) {
  outMsg = "";
  File rf = LittleFS.open("/ca.tmp", "r");
  if (!rf) {
    outMsg = "Could not open /ca.tmp for reading";
    return false;
  }

  size_t sz = rf.size();
  if (sz == 0) {
    outMsg = "Upload is empty";
    rf.close();
    return false;
  }

  if (sz > 6144) {
    outMsg = "Uploaded file too large (>6kB)";
    rf.close();
    return false;
  }

  std::string s;
  s.resize(sz);
  size_t n = rf.readBytes(&s[0], sz);
  rf.close();
  s.resize(n);

  // BEGIN/END check
  const char *BEGIN_TAG = "-----BEGIN CERTIFICATE-----";
  const char *END_TAG   = "-----END CERTIFICATE-----";
  size_t b = s.find(BEGIN_TAG);
  size_t e = s.find(END_TAG);

  if (b == std::string::npos || e == std::string::npos) {
    outMsg = "Uploaded certificate missing BEGIN/END";
    return false;
  }
  e += strlen(END_TAG);


  std::string cert = s.substr(b, e - b);
  if (!cert.empty() && cert.back() != '\n') cert.push_back('\n');

  if (cert.size() > 4096) {
    outMsg = "Certificate too large (>4kB)";
    log_message(_F("[WARN] Certificate >4kB – discarding."));
    return false;
  }

  // Character check (PEM/Base64 + CR/LF/Space)
  for (char c : cert) {
    unsigned char uc = (unsigned char)c;
    if (!(isalnum(uc) || c=='+'||c=='/'||c=='='||c=='\n'||c=='\r'||c==' '||c=='-')) {
      outMsg = "Certificate contains invalid characters";
      log_message(_F("[ERROR] Invalid character in certificate"));
      return false;
    }
  }

  File wf = LittleFS.open("/ca.pem.new", "w");
  if (!wf) {
    outMsg = "Could not open /ca.pem.new for writing";
    return false;
  }
  size_t w = wf.write((const uint8_t*)cert.data(), cert.size());
  wf.close();
  if (w != cert.size()) {
    outMsg = "Write to /ca.pem.new incomplete";
    log_message(_F("[ERROR] Failed to write complete certificate to /ca.pem.new."));
    LittleFS.remove("/ca.pem.new");
    return false;
  }

  LittleFS.remove("/ca.pem");
  LittleFS.rename("/ca.pem.new", "/ca.pem");
  LittleFS.remove("/ca.tmp");

  log_message(_F("[INFO] CA certificate stored as /ca.pem"));
  outMsg = "CA certificate stored in filesystem";
  return true;
}
#endif

/*
 * passwordWasChanged -- Check whether the user entered a new password
 *   (as opposed to the placeholder asterisk-only string sent by the web UI).
 *   Returns true if any non-asterisk character is found.
 * Thread-safety: Reentrant (pure function).
 */
bool passwordWasChanged(const char *returned) {
    for (size_t i = 0; i < strlen(returned); i++) {
        if (returned[i] != '*') return true;
    }
    return false;
}

/*
 * saveSettings -- Process the settings form POST data previously cached by
 *   cacheSettings().  Merges per-field values from the linked list into a
 *   JsonDocument (starting from current settings), handles OTA password
 *   verification, WiFi credential change detection, then writes the result
 *   to "/config.json" via saveJsonToFile and reloads with loadSettings().
 *   Returns an alternate route code:
 *     111  -> wrong OTA password
 *     112  -> WiFi credentials changed (triggers reconnect page)
 *     113  -> normal save success
 * Thread-safety: NOT reentrant.  Accesses LittleFS, modifies settingsStruct,
 *   and frees the web-form linked list.  Must be called from a single
 *   webserver callback context.
 */
int saveSettings(struct webserver_t *client, settingsStruct *heishamonSettings) {
  const char *wifi_ssid = NULL;
  const char *wifi_password = NULL;
  const char *mqtt_password = NULL;
  const char *new_ota_password = NULL;
  const char *current_ota_password = NULL;

  bool reconnectWiFi = false;
  bool wrongPassword = false;
  JsonDocument jsonDoc;

  settingsToJson(jsonDoc, heishamonSettings); //stores current settings in a json document

  jsonDoc["force_rules"] = "disabled";
  jsonDoc["hotspot"] = "disabled";
  jsonDoc["listenonly"] = "disabled";
  jsonDoc["logMqtt"] = "disabled";
  jsonDoc["logHexdump"] = "disabled";
  jsonDoc["logSerial1"] = "disabled";
  jsonDoc["optionalPCB"] = "disabled";
  jsonDoc["opentherm"] = "disabled";

#ifdef ESP32  
  jsonDoc["proxy"] = "disabled";
#endif  
#ifdef TLS_SUPPORT
  jsonDoc["mqtt_tls_enabled"] = "disabled";
#endif

  jsonDoc["use_1wire"] = "disabled";
  jsonDoc["use_s0"] = "disabled";

  struct websettings_t *tmp = (struct websettings_t *)client->userdata;
  while (tmp) {
    if (strcmp(tmp->name.c_str(), "wifi_hostname") == 0) {
      jsonDoc[F("wifi_hostname")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "mqtt_topic_base") == 0) {
      jsonDoc[F("mqtt_topic_base")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "mqtt_server") == 0) {
      jsonDoc[F("mqtt_server")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "mqtt_port") == 0) {
      jsonDoc[F("mqtt_port")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "mqtt_username") == 0) {
      jsonDoc[F("mqtt_username")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "mqtt_password") == 0) {
      mqtt_password = tmp->value.c_str();
    } else if (strcmp(tmp->name.c_str(), "use_1wire") == 0) {
      jsonDoc[F("use_1wire")] = tmp->value;
#ifdef TLS_SUPPORT
    } else if (strcmp(tmp->name.c_str(), "mqtt_tls_enabled") == 0) {
    jsonDoc[F("mqtt_tls_enabled")] = tmp->value;
#endif
    } else if (strcmp(tmp->name.c_str(), "use_s0") == 0) {
      jsonDoc[F("use_s0")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "hotspot") == 0) {
      jsonDoc[F("hotspot")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "listenonly") == 0) {
      jsonDoc[F("listenonly")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "force_rules") == 0) {
      jsonDoc[F("force_rules")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "logMqtt") == 0) {
      jsonDoc[F("logMqtt")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "logHexdump") == 0) {
      jsonDoc[F("logHexdump")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "logSerial1") == 0) {
      jsonDoc[F("logSerial1")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "optionalPCB") == 0) {
      jsonDoc[F("optionalPCB")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "opentherm") == 0) {
      jsonDoc[F("opentherm")] = tmp->value;
#ifdef ESP32      
    } else if (strcmp(tmp->name.c_str(), "proxy") == 0) {
      jsonDoc[F("proxy")] = tmp->value;
#endif      
    } else if (strcmp(tmp->name.c_str(), "ntp_servers") == 0) {
      jsonDoc[F("ntp_servers")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "timezone") == 0) {
      jsonDoc[F("timezone")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "waitTime") == 0) {
      jsonDoc[F("waitTime")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "waitDallasTime") == 0) {
      jsonDoc[F("waitDallasTime")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "updateAllTime") == 0) {
      jsonDoc[F("updateAllTime")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "dallasResolution") == 0) {
      jsonDoc[F("dallasResolution")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "updataAllDallasTime") == 0) {
      jsonDoc[F("updataAllDallasTime")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "wifi_ssid") == 0) {
      wifi_ssid = tmp->value.c_str();
    } else if (strcmp(tmp->name.c_str(), "wifi_password") == 0) {
      wifi_password = tmp->value.c_str();
    } else if (strcmp(tmp->name.c_str(), "new_ota_password") == 0) {
      new_ota_password = tmp->value.c_str();
    } else if (strcmp(tmp->name.c_str(), "current_ota_password") == 0) {
      current_ota_password = tmp->value.c_str();
    } else if (strcmp(tmp->name.c_str(), "s0_1_ppkwh") == 0) {
      jsonDoc[F("s0_1_ppkwh")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "s0_1_interval") == 0) {
      jsonDoc[F("s0_1_interval")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "s0_1_minpulsewidth") == 0) {
      jsonDoc[F("s0_1_minpulsewidth")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "s0_1_maxpulsewidth") == 0) {
      jsonDoc[F("s0_1_maxpulsewidth")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "s0_2_ppkwh") == 0) {
      jsonDoc[F("s0_2_ppkwh")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "s0_2_ppkwh") == 0) {
      jsonDoc[F("s0_2_ppkwh")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "s0_2_interval") == 0) {
      jsonDoc[F("s0_2_interval")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "s0_2_minpulsewidth") == 0) {
      jsonDoc[F("s0_2_minpulsewidth")] = tmp->value;
    } else if (strcmp(tmp->name.c_str(), "s0_2_maxpulsewidth") == 0) {
      jsonDoc[F("s0_2_maxpulsewidth")] = tmp->value;
    }
    tmp = tmp->next;
  }

  if (new_ota_password != NULL && strlen(new_ota_password) > 0 && current_ota_password != NULL && strlen(current_ota_password) > 0) {
    if (strcmp(heishamonSettings->ota_password, current_ota_password) == 0) {
      jsonDoc[F("ota_password")] = new_ota_password;
    } else {
      wrongPassword = true;
    }
  }

  if (wifi_ssid != NULL && strlen(wifi_ssid) > 0) {
    if (strcmp(jsonDoc[F("wifi_ssid")], wifi_ssid) != 0) reconnectWiFi = true;
    jsonDoc[F("wifi_ssid")] = wifi_ssid; 
  }

  if (wifi_password != NULL && passwordWasChanged(wifi_password)) {
    reconnectWiFi = true;
    jsonDoc[F("wifi_password")] = wifi_password;
  }

  if (mqtt_password != NULL && passwordWasChanged(mqtt_password) ) {
    jsonDoc[F("mqtt_password")] = mqtt_password;
  }

  saveJsonToFile(jsonDoc, "/config.json"); //save to config file
  loadSettings(heishamonSettings); //load config file to current settings

  while (client->userdata) {
    tmp = (struct websettings_t *)client->userdata;
    client->userdata = ((struct websettings_t *)(client->userdata))->next;
    delete tmp;
  }

  if (wrongPassword) {
    client->route = 111;
    return 0;
  }

  if (reconnectWiFi) {
    client->route = 112;
    return 0;
  }

  client->route = 113;
  return 0;
}

/*
 * cacheSettings -- Called once per form field during POST data processing.
 *   Builds a singly-linked list of websettings_t nodes anchored at
 *   client->userdata.  Each node holds one form field name + value.
 *   This list is later consumed by saveSettings().
 * Thread-safety: NOT reentrant per client.  Only safe within a single
 *   request's callback sequence.  Allocates heap (new / malloc).
 */
int cacheSettings(struct webserver_t *client, struct arguments_t * args) {
  struct websettings_t *tmp = (struct websettings_t *)client->userdata;
  while (tmp) {
    tmp = tmp->next;
  }
  if (tmp == NULL) {
    websettings_t *node = new websettings_t;
    if (node == NULL) {
      Serial1.printf("Out of memory %s:#%d\n", __FUNCTION__, __LINE__);
      ESP.restart();
      exit(-1);
    }
    node->next = NULL;
    node->name += (char *)args->name;
    if (args->value != NULL) {
      char *cpy = (char *)malloc(args->len + 1);
      if (cpy == NULL) {
        Serial1.printf("Out of memory %s:#%d\n", __FUNCTION__, __LINE__);
        ESP.restart();
        exit(-1);
      }
      memset(cpy, 0, args->len + 1);
      strncpy(cpy, (char *)args->value, args->len);
      node->value += cpy;
      free(cpy);
    }

    node->next = (struct websettings_t *)client->userdata;
    client->userdata = node;
  }

  return 0;
}

/*
 * settingsNewPassword -- Web handler for the OTA-password change page.
 *   content 0: HTTP headers + CSS + body start.
 *   content 1: settings form + password reset warning block.
 *   content 2: meta refresh + footer.
 *   content 3: call setupConditionals() (initialises form JS behaviour).
 * Thread-safety: Single-client webserver callback; reentrant only for
 *   different client instances.
 */
int settingsNewPassword(struct webserver_t *client, settingsStruct *heishamonSettings) {
  switch (client->content) {
    case 0: {
        webserver_send(client, 200, (char *)"text/html", 0);
        webserver_send_content_P(client, webHeader, strlen_P(webHeader));
        webserver_send_content_P(client, webCSS, strlen_P(webCSS));
        webserver_send_content_P(client, webBodyStart, strlen_P(webBodyStart));
      } break;
    case 1: {
        webserver_send_content_P(client, webBodySettings1, strlen_P(webBodySettings1));
        webserver_send_content_P(client, webBodySettingsResetPasswordWarning, strlen_P(webBodySettingsResetPasswordWarning));
      } break;
    case 2: {
        webserver_send_content_P(client, refreshMeta, strlen_P(refreshMeta));
        webserver_send_content_P(client, webFooter, strlen_P(webFooter));
      } break;
    case 3: {
        setupConditionals();
      } break;
  }

  return 0;
}

/*
 * settingsReconnectWifi -- Web handler for the "WiFi reconnecting" page,
 *   displayed after the user changes WiFi credentials.  Sends the HTML body
 *   with a warning and schedules a reconnect via timerqueue_insert(5, 0, -3).
 * Thread-safety: Same as settingsNewPassword.
 */
int settingsReconnectWifi(struct webserver_t *client, settingsStruct *heishamonSettings) {
  if (client->content == 0) {
    webserver_send(client, 200, (char *)"text/html", 0);
    webserver_send_content_P(client, webHeader, strlen_P(webHeader));
    webserver_send_content_P(client, webCSS, strlen_P(webCSS));
    webserver_send_content_P(client, webBodyStart, strlen_P(webBodyStart));
    webserver_send_content_P(client, webBodySettings1, strlen_P(webBodySettings1));
  } else if (client->content == 2) {
    webserver_send_content_P(client, menuJS, strlen_P(menuJS));
    webserver_send_content_P(client, webBodySettingsNewWifiWarning, strlen_P(webBodySettingsNewWifiWarning));
    webserver_send_content_P(client, refreshMeta, strlen_P(refreshMeta));
    webserver_send_content_P(client, webFooter, strlen_P(webFooter));
    timerqueue_insert(5, 0, -3); //handle wifi reconnect after 5 sec to make sure all above data is sent to client so no memory leak is introduced
  }

  return 0;
}

/*
 * maskPassword -- Fill the output buffer with asterisks of the same length
 *   as the input string.  Used to avoid sending plaintext passwords to the
 *   web UI.  Assumes output has at least strlen(input)+1 bytes.
 * Thread-safety: Reentrant (pure function, no shared state).
 */
void maskPassword(const char *input, char *output) {
    size_t len = strlen(input);
    memset(output, '*', len);
    output[len] = '\0';
}

/*
 * getSettings -- Return all current settings as a JSON response.
 *   Builds a JsonDocument via settingsToJson(), masks wifi_password and
 *   mqtt_password with asterisks, and (with TLS_SUPPORT) reports the
 *   presence of a CA certificate.  Sends the JSON as application/json.
 * Thread-safety: NOT reentrant on the same client.  Reads settingsStruct
 *   (read-only during this call) and accesses LittleFS (for CA cert check).
 */
int getSettings(struct webserver_t *client, settingsStruct *heishamonSettings) {
    switch (client->content) {
    case 0: {
        JsonDocument jsonDoc;
        settingsToJson(jsonDoc, heishamonSettings); //stores current settings in a json document  
        //overwrite passwords with placeholder string
        char masked_wifi_password[PASSWORD_LENGTH];
        maskPassword(jsonDoc[F("wifi_password")], masked_wifi_password);
        jsonDoc[F("wifi_password")] = masked_wifi_password;
        char masked_mqtt_password[PASSWORD_LENGTH];
        maskPassword(jsonDoc[F("mqtt_password")], masked_mqtt_password);
        jsonDoc[F("mqtt_password")] = masked_mqtt_password;
#ifdef TLS_SUPPORT
        jsonDoc[F("mqtt_ca_cert")] = LittleFS.exists("/ca.pem") ? F("CA certificate stored in filesystem") : F("No CA certificate found");
#endif   
        int size = measureJson(jsonDoc);
        char* buffer = (char*)malloc(size+1);
        if (buffer == nullptr) {
            //alloc failed
            log_message(_F("Failed to initialize buffer for getsettings serialize json"));
            return -1;
        }
        serializeJson(jsonDoc, buffer, size);
        webserver_send(client, 200, (char *)"application/json", 0);
        webserver_send_content(client, buffer, size);
        free(buffer);
      }  break;
    }
  return 0;
}

/*
 * handleSettings -- Web handler for the Settings configuration page.
 *   content 0: HTTP headers + CSS + body start.
 *   content 1: settings form part 1 + timezone dropdown.
 *   content 2: settings form part 2 + JS for settings, CA upload (if TLS),
 *              and populate-get-settings.
 *   content 3: WiFi scan populator JS + SSID change JS + footer.
 * Thread-safety: Single-client callback; reentrant across clients.
 */
int handleSettings(struct webserver_t *client) {
  if (client->content == 0) {
    webserver_send(client, 200, (char *)"text/html", 0);
    webserver_send_content_P(client, webHeader, strlen_P(webHeader));
    webserver_send_content_P(client, webCSS, strlen_P(webCSS));
    webserver_send_content_P(client, webBodyStart, strlen_P(webBodyStart));
  } else if (client->content == 1) {
    webserver_send_content_P(client, webBodySettings1, strlen_P(webBodySettings1));
    webserver_send_content_P(client, settingsForm1, strlen_P(settingsForm1));
    webserver_send_content_P(client, tzDataOptions, strlen_P(tzDataOptions));
  } else if (client->content == 2) {
    webserver_send_content_P(client, settingsForm2, strlen_P(settingsForm2));
    webserver_send_content_P(client, menuJS, strlen_P(menuJS));
    webserver_send_content_P(client, settingsJS, strlen_P(settingsJS));
#ifdef TLS_SUPPORT
    webserver_send_content_P(client, caUploadJS, strlen_P(caUploadJS));
#endif
    webserver_send_content_P(client, populategetsettingsJS, strlen_P(populategetsettingsJS));
  } else if (client->content == 3) {
    webserver_send_content_P(client, populatescanwifiJS, strlen_P(populatescanwifiJS));
    webserver_send_content_P(client, changewifissidJS, strlen_P(changewifissidJS));
    webserver_send_content_P(client, webFooter, strlen_P(webFooter));
  }

  return 0;
}

#ifdef TLS_SUPPORT
/*
 * handleCACert -- Process CA certificate file upload (TLS_SUPPORT only).
 *   content 0: Call processCAtmp_to_CAPEM() on the uploaded temp file,
 *              store the result message in a static String, start response.
 *   content 1: Send the buffered response text, then clear it.
 *   Uses a static String s_resp to persist the message across multi-part
 *   webserver callbacks.
 * Thread-safety: The static s_resp is shared across calls for the same
 *   request but may be corrupted if multiple upload requests interleave.
 *   In practice the webserver processes one client at a time.
 */
int handleCACert(struct webserver_t *client) {
  static String s_resp;  // Persists response message across multi-part callbacks
  if (client->content == 0) {
    String msg;
    bool ok = processCAtmp_to_CAPEM(msg); 
    
    if (ok) {
      s_resp = "CA upload success: " + msg;
    } else {
      s_resp = "CA upload failed: " + msg;
    }

    webserver_send(client, 200, (char*)"text/plain", s_resp.length());
    return 0;
  }

  if (client->content == 1) {
    if (s_resp.length() > 0) {
      webserver_send_content(client, (char*)s_resp.c_str(), s_resp.length());
    }
    s_resp = "";
    return 0;
  }

  return 0;
}
#endif

/*
 * handleWifiScan -- Return cached WiFi scan results as application/json.
 *   If a previous async scan completed (scanComplete() > 0) the results are
 *   sorted / deduplicated / saved to "/wifiscan.json" via getWifiScanResults().
 *   The JSON file is then streamed back to the client.  An async scan is
 *   initiated for the next poll cycle via WiFi.scanNetworks(true).
 * Thread-safety: NOT reentrant (LittleFS access).  WiFi scan API must not be
 *   called concurrently from multiple tasks.
 */
int handleWifiScan(struct webserver_t *client) {
#if defined(ESP32) 
  //first get result from previous scan
  int numSSID = WiFi.scanComplete();
  if (numSSID > 0) { 
    getWifiScanResults(numSSID);
  }
#endif
  if (client->content == 0) {
    webserver_send(client, 200, (char *)"application/json", 0);
    if (LittleFS.begin()) {
      File scanfile = LittleFS.open("/wifiscan.json", "r");
      if (scanfile) {
        size_t size = scanfile.size();
        if (size > 0 ) {
          // Allocate a buffer to store contents of the file.
          std::unique_ptr<char[]> buf(new char[size]);
          scanfile.readBytes(buf.get(), size);
          webserver_send_content(client, buf.get(), size);
          scanfile.close();
        } else {
          webserver_send_content_P(client, PSTR("[]"), 2);
        }
      } else {
        webserver_send_content_P(client, PSTR("[]"), 2);
      }
    } else {
      webserver_send_content_P(client, PSTR("[]"), 2);
    }

  }
  //initatie a new async scan for next try
  WiFi.scanNetworks(true);
  return 0;
}

/*
 * handleDebug -- Stream a hex dump of a raw data buffer to the web client.
 *   Formats LOGHEXBYTESPERLINE (32) bytes per line with offset markers.
 *   Used for diagnostic / log viewing.
 * Thread-safety: Single-client callback.  The hex buffer must remain valid
 *   across the multi-part callback sequence.
 */
int handleDebug(struct webserver_t *client, char *hex, byte hex_len) {
  {
#define LOGHEXBYTESPERLINE 32
    char log_msg[254];
    for (int i = 0; i < hex_len; i += LOGHEXBYTESPERLINE) {
      char buffer [(LOGHEXBYTESPERLINE * 3) + 1];
      buffer[LOGHEXBYTESPERLINE * 3] = '\0';
      for (int j = 0; ((j < LOGHEXBYTESPERLINE) && ((i + j) < hex_len)); j++) {
        sprintf(&buffer[3 * j], PSTR("%02X "), hex[i + j]);
      }
      uint8_t len = sprintf_P(log_msg, PSTR("[%03d]: %s\n"), i, buffer);
      webserver_send_content(client, log_msg, len);
    }
  }
  return 0;
}


/*
 * handleRoot -- Main dashboard page handler.
 *   content 0: HTTP headers + CSS + body start + root page header.
 *   content 1: Tab navigation (heatpump / 1-wire / S0 / Opentherm
 *              depending on settings) + listen-only status bar.
 *   content 2: Data value blocks for heatpump, 1-wire, S0, opentherm.
 *   content 3: Console log section.
 *   content 4: Menu JS.
 *   content 5: Auto-refresh JS + console toggle JS.
 *   content 6: Select JS + websocket JS + footer.
 * Thread-safety: Single-client callback.  Reads from heishamonSettings
 *   (read-only during this call).
 */
int handleRoot(struct webserver_t *client, float readpercentage, int mqttReconnects, settingsStruct *heishamonSettings) {
  switch (client->content) {
    case 0: {
        webserver_send(client, 200, (char *)"text/html", 0);
        webserver_send_content_P(client, webHeader, strlen_P(webHeader));
        webserver_send_content_P(client, webCSS, strlen_P(webCSS));
        webserver_send_content_P(client, webBodyStart, strlen_P(webBodyStart));
        webserver_send_content_P(client, webBodyRoot1, strlen_P(webBodyRoot1));
      } break;
    case 1: {
        webserver_send_content_P(client, webTabnavOpen, strlen_P(webTabnavOpen));
        if (heishamonSettings->use_1wire) {
          webserver_send_content_P(client, webBodyRootDallasTab, strlen_P(webBodyRootDallasTab));
        }
        if (heishamonSettings->use_s0) {
          webserver_send_content_P(client, webBodyRootS0Tab, strlen_P(webBodyRootS0Tab));
        }
        if (heishamonSettings->opentherm) {
          webserver_send_content_P(client, webBodyRootOpenthermTab, strlen_P(webBodyRootOpenthermTab));
        }
        webserver_send_content_P(client, webTabnavClose, strlen_P(webTabnavClose));
        if (heishamonSettings->listenonly) {
          webserver_send_content_P(client, webBodyRootStatusListenOnly, strlen_P(webBodyRootStatusListenOnly));
        }
        webserver_send_content_P(client, webBodyEndDiv, strlen_P(webBodyEndDiv));
      } break;
    case 2: {
        webserver_send_content_P(client, webBodyRootHeatpumpValues, strlen_P(webBodyRootHeatpumpValues));
        if (heishamonSettings->use_1wire) {
          webserver_send_content_P(client, webBodyRootDallasValues, strlen_P(webBodyRootDallasValues));
        }
        if (heishamonSettings->opentherm) {
          webserver_send_content_P(client, webBodyRootOpenthermValues, strlen_P(webBodyRootOpenthermValues));
        }        
        if (heishamonSettings->use_s0) {
          webserver_send_content_P(client, webBodyRootS0Values, strlen_P(webBodyRootS0Values));
        }
      } break;
    case 3: {
        webserver_send_content_P(client, webBodyRootConsole, strlen_P(webBodyRootConsole));
      } break;
    case 4: {
        webserver_send_content_P(client, menuJS, strlen_P(menuJS));
      } break;
    case 5: {
        webserver_send_content_P(client, refreshJS, strlen_P(refreshJS));
        webserver_send_content_P(client, consoleTogglesJS, strlen_P(consoleTogglesJS));

      } break;
    case 6: {
        webserver_send_content_P(client, selectJS, strlen_P(selectJS));
        webserver_send_content_P(client, websocketJS, strlen_P(websocketJS));
        webserver_send_content_P(client, webFooter, strlen_P(webFooter));
      } break;
  }
  return 0;
}

/*
 * handleJsonOutput -- Stream all heatpump telemetry data as a single JSON
 *   object using a multi-part webserver callback.  Data is split across
 *   three arrays: "heatpump" (main topics), "heatpump extra" (extra block,
 *   if available), and "heatpump optional" (optional PCB, if enabled).
 *   Each array entry contains Topic, Name, Value, and Description fields.
 *   After the arrays, 1-wire / S0 / opentherm sub-objects are appended
 *   if the respective features are enabled.
 *   content is advanced incrementally to avoid blocking the server.
 * Thread-safety: NOT reentrant per client.  The data buffers (actData,
 *   actDataExtra, actOptData) must be stable across the multi-part call
 *   sequence.  heishamonSettings is read-only during the call.
 */
int handleJsonOutput(struct webserver_t *client, char* actData, char* actDataExtra, char* actOptData, settingsStruct *heishamonSettings, bool extraDataBlockAvailable) {
  int extraTopics = extraDataBlockAvailable ? NUMBER_OF_TOPICS_EXTRA : 0; //set to 0 if there is no datablock so we don't run json data for it
  int numOptTopics = heishamonSettings->optionalPCB ? NUMBER_OF_OPT_TOPICS : 0; //set to 0 if there is no optionalPCB emulation so we don't run json data for it
  if (client->content == 0) {
    webserver_send(client, 200, (char *)"application/json", 0);
    webserver_send_content_P(client, PSTR("{\"heatpump\":["), 13);
  } else if ((client->content - 1) < NUMBER_OF_TOPICS) {
    uint8_t maxTopics =  client->content + 4; //limit the amount of topic sent per webloop
    for (uint8_t topic = client->content - 1; topic < NUMBER_OF_TOPICS && topic < maxTopics ; topic++) {  

      webserver_send_content_P(client, PSTR("{\"Topic\":\"TOP"), 13);

      {
        char str[12];
        itoa(topic, str, 10);
        webserver_send_content(client, str, strlen(str));
      }

      webserver_send_content_P(client, PSTR("\",\"Name\":\""), 10);

      webserver_send_content_P(client, topics[topic], strlen_P(topics[topic]));

      if ((topic != 44) && (topic != 92)) { //ERROR topic #44 and #92 are the only one to be a string value
        webserver_send_content_P(client, PSTR("\",\"Value\":"), 10);
      }
      else {
        webserver_send_content_P(client, PSTR("\",\"Value\":\""), 11);
      }

      {
        String dataValue = getDataValue(actData, topic);
        char* str = (char *)dataValue.c_str();
        webserver_send_content(client, str, strlen(str));
      }

      if ((topic != 44) && (topic != 92)) { //ERROR topic #44 and #92 are the only one to be a string value
        webserver_send_content_P(client, PSTR(",\"Description\":\""), 16);
      } else {
        webserver_send_content_P(client, PSTR("\",\"Description\":\""), 17);
      }

      int maxvalue = atoi(topicDescription[topic][0]);
      int value = actData[0] == '\0' ? 0 : getDataValue(actData, topic).toInt();
      if (maxvalue == 0) { //this takes the special case where the description is a real value description instead of a mode, so value should take first index (= 0 + 1)
        value = 0;
      }
      if ((value < 0) || (value > maxvalue)) {
        webserver_send_content_P(client, _unknown, strlen_P(_unknown));
      }
      else {
        webserver_send_content_P(client, topicDescription[topic][value + 1], strlen_P(topicDescription[topic][value + 1]));
      }

      webserver_send_content_P(client, PSTR("\"}"), 2);

      if (topic < NUMBER_OF_TOPICS - 1) {
        webserver_send_content_P(client, PSTR(","), 1);
      }
      client->content++;
    }
    client->content--; // The webserver also increases by 1
  } else if ((client->content - NUMBER_OF_TOPICS - 1) < extraTopics) {
    if (client->content == NUMBER_OF_TOPICS + 1) {
      webserver_send_content_P(client, PSTR("],\"heatpump extra\":["), 20);
    }
    uint8_t maxTopics =  client->content - NUMBER_OF_TOPICS + 4; //limit the amount of topic sent per webloop
    for (uint8_t topic = (client->content - NUMBER_OF_TOPICS - 1); topic < extraTopics && topic < maxTopics ; topic++) {

      webserver_send_content_P(client, PSTR("{\"Topic\":\"XTOP"), 14);

      {
        char str[12];
        itoa(topic, str, 10);
        webserver_send_content(client, str, strlen(str));
      }

      webserver_send_content_P(client, PSTR("\",\"Name\":\""), 10);

      webserver_send_content_P(client, xtopics[topic], strlen_P(xtopics[topic]));

      webserver_send_content_P(client, PSTR("\",\"Value\":\""), 11);

      {
        String dataValue = getDataValueExtra(actDataExtra, topic);
        char* str = (char *)dataValue.c_str();
        webserver_send_content(client, str, strlen(str));
      }

      webserver_send_content_P(client, PSTR("\",\"Description\":\""), 17);

      int maxvalue = atoi(xtopicDescription[topic][0]);
      int value = actDataExtra[0] == '\0' ? 0 : getDataValueExtra(actDataExtra, topic).toInt();
      if (maxvalue == 0) { //this takes the special case where the description is a real value description instead of a mode, so value should take first index (= 0 + 1)
        value = 0;
      }
      if ((value < 0) || (value > maxvalue)) {
        webserver_send_content_P(client, _unknown, strlen_P(_unknown));
      }
      else {
        webserver_send_content_P(client, xtopicDescription[topic][value + 1], strlen_P(xtopicDescription[topic][value + 1]));
      }

      webserver_send_content_P(client, PSTR("\"}"), 2);

      if (topic < (extraTopics - 1)) {
        webserver_send_content_P(client, PSTR(","), 1);
      }
      client->content++;
    }
    client->content--; // The webserver also increases by 1
  } else if ((client->content - NUMBER_OF_TOPICS - extraTopics - 1) < numOptTopics) {
    if (client->content == NUMBER_OF_TOPICS + extraTopics + 1) {
      webserver_send_content_P(client, PSTR("],\"heatpump optional\":["), 23);
    }
    uint8_t maxTopics =  client->content - NUMBER_OF_TOPICS + extraTopics + 4; //limit the amount of topic sent per webloop
    for (uint8_t topic = (client->content - NUMBER_OF_TOPICS - extraTopics - 1); topic < numOptTopics && topic < maxTopics ; topic++) {

      webserver_send_content_P(client, PSTR("{\"Topic\":\"OPT"), 13);

      {
        char str[12];
        itoa(topic, str, 10);
        webserver_send_content(client, str, strlen(str));
      }

      webserver_send_content_P(client, PSTR("\",\"Name\":\""), 10);
      webserver_send_content_P(client, optTopics[topic], strlen_P(optTopics[topic]));

      webserver_send_content_P(client, PSTR("\",\"Value\":\""), 11);

      {
        String dataValue = getOptDataValue(actOptData, topic);
        char* str = (char *)dataValue.c_str();
        webserver_send_content(client, str, strlen(str));
      }

      webserver_send_content_P(client, PSTR("\",\"Description\":\""), 17);

      int maxvalue = atoi(opttopicDescription[topic][0]);
      int value = actOptData[0] == '\0' ? 0 : getOptDataValue(actOptData, topic).toInt();
      if (maxvalue == 0) { //this takes the special case where the description is a real value description instead of a mode, so value should take first index (= 0 + 1)
        value = 0;
      }
      if ((value < 0) || (value > maxvalue)) {
        webserver_send_content_P(client, _unknown, strlen_P(_unknown));
      }
      else {
        webserver_send_content_P(client, opttopicDescription[topic][value + 1], strlen_P(opttopicDescription[topic][value + 1]));
      }

      webserver_send_content_P(client, PSTR("\"}"), 2);

      if (topic < (numOptTopics - 1)) {
        webserver_send_content_P(client, PSTR(","), 1);
      }
      client->content++;
    }
    client->content--; // The webserver also increases by 1
  } else if (client->content == (NUMBER_OF_TOPICS + extraTopics + numOptTopics + 1)) {
    webserver_send_content_P(client, PSTR("]"), 1);
    if (heishamonSettings->use_1wire) {
      webserver_send_content_P(client, PSTR(",\"1wire\":"), 9);
      dallasJsonOutput(client);
    }
    if (heishamonSettings->use_s0 ) {
      webserver_send_content_P(client, PSTR(",\"s0\":"), 6);
      s0JsonOutput(client);
    }
    if (heishamonSettings->opentherm) {
      webserver_send_content_P(client, PSTR(",\"opentherm\":"), 13);
      openthermJsonOutput(client);
    }
    webserver_send_content_P(client, PSTR("}"), 1);
  }
  return 0;
}


/*
 * showRules -- Serve the contents of "/rules.txt" (conditional rule file) as
 *   an HTML page.  Uses the client->userdata field to hold an open File
 *   pointer.  Data is served in 128-byte chunks across multiple content
 *   phases for cooperative multi-tasking.  At EOF the footer and JS are sent.
 * Thread-safety: NOT reentrant per client.  The File handle in userdata must
 *   not be shared.  LittleFS must not be accessed concurrently.
 */
int showRules(struct webserver_t *client) {
  uint16_t len = 0, len1 = 0;

  if (client->content == 0) {
    webserver_send(client, 200, (char *)"text/html", 0);
    webserver_send_content_P(client, webHeader, strlen_P(webHeader));
    webserver_send_content_P(client, webCSS, strlen_P(webCSS));
    webserver_send_content_P(client, webBodyStart, strlen_P(webBodyStart));
    webserver_send_content_P(client, showRulesPage1, strlen_P(showRulesPage1));
    if (LittleFS.begin()) {
      client->userdata = new fs::File(LittleFS.open("/rules.txt", "r"));
    }
  } else if (client->userdata != NULL) {
#define BUFFER_SIZE 128
    File *f = (File *)client->userdata;
    char content[BUFFER_SIZE];
    memset(content, 0, BUFFER_SIZE);
    if (f && *f) {
      len = f->size();
    }

    if (len > 0) {
      f->seek((client->content - 1)*BUFFER_SIZE, SeekSet);
      if (client->content * BUFFER_SIZE <= len) {
        f->readBytes(content, BUFFER_SIZE);
        len1 = BUFFER_SIZE;
      } else if ((client->content * BUFFER_SIZE) >= len && (client->content * BUFFER_SIZE) <= len + BUFFER_SIZE) {
        f->readBytes(content, len - ((client->content - 1)*BUFFER_SIZE));
        len1 = len - ((client->content - 1) * BUFFER_SIZE);
      } else {
        len1 = 0;
      }

      if (len1 > 0) {
        webserver_send_content(client, content, len1);
        if (len1 < BUFFER_SIZE || client->content * BUFFER_SIZE == len) {
          if (f) {
            if (*f) {
              f->close();
            } 
            delete f;
          }
          client->userdata = NULL;
          webserver_send_content_P(client, showRulesPage2, strlen_P(showRulesPage2));
          webserver_send_content_P(client, rulesJS, strlen_P(rulesJS));
          webserver_send_content_P(client, menuJS, strlen_P(menuJS));
          webserver_send_content_P(client, webFooter, strlen_P(webFooter));
        }
      } else if (client->content == 1) {
        if (f) {
          if (*f) {
            f->close();
          }
          delete f;
        }
        client->userdata = NULL;
        webserver_send_content_P(client, showRulesPage2, strlen_P(showRulesPage2));
        webserver_send_content_P(client, rulesJS, strlen_P(rulesJS));
        webserver_send_content_P(client, menuJS, strlen_P(menuJS));
        webserver_send_content_P(client, webFooter, strlen_P(webFooter));
      }
    } else if (client->content == 1) {
      if (f) {
        if (*f) {
          f->close();
        }
        delete f;
      }
      client->userdata = NULL;
      webserver_send_content_P(client, showRulesPage2, strlen_P(showRulesPage2));
      webserver_send_content_P(client, rulesJS, strlen_P(rulesJS));
      webserver_send_content_P(client, menuJS, strlen_P(menuJS));
      webserver_send_content_P(client, webFooter, strlen_P(webFooter));
    }
  } else if (client->content == 1) {
    webserver_send_content_P(client, showRulesPage2, strlen_P(showRulesPage2));
    webserver_send_content_P(client, rulesJS, strlen_P(rulesJS));
    webserver_send_content_P(client, menuJS, strlen_P(menuJS));
    webserver_send_content_P(client, webFooter, strlen_P(webFooter));
  }

  return 0;
}

/*
 * showFirmware -- Serve the firmware update page.
 *   content 0: HTTP headers + CSS + body start.
 *   content 1: Firmware upload form + menu JS + footer.
 * Thread-safety: Single-client callback; reentrant across clients.
 */
int showFirmware(struct webserver_t *client) {
  if (client->content == 0) {
    webserver_send(client, 200, (char *)"text/html", 0);
    webserver_send_content_P(client, webHeader, strlen_P(webHeader));
    webserver_send_content_P(client, webCSS, strlen_P(webCSS));
    webserver_send_content_P(client, webBodyStart, strlen_P(webBodyStart));
  } else  if (client->content == 1) {
    webserver_send_content_P(client, showFirmwarePage, strlen_P(showFirmwarePage));
    webserver_send_content_P(client, menuJS, strlen_P(menuJS));
    webserver_send_content_P(client, webFooter, strlen_P(webFooter));
  }

  return 0;
}

#ifdef TLS_SUPPORT
/*
 * showCACert -- Serve the uploaded CA certificate file content as plain text
 *   (TLS_SUPPORT only).  Reads "/ca.pem" from LittleFS in 256-byte chunks.
 *   Returns an error message if the file does not exist or cannot be opened.
 * Thread-safety: NOT reentrant (LittleFS access).
 */
int showCACert(struct webserver_t *client) {
  if (client->content == 0) {
    webserver_send(client, 200, (char*)"text/plain", 0);
    if (!LittleFS.exists("/ca.pem")) {
      webserver_send_content_P(client, PSTR("No CA certificate uploaded yet\n"), 31);
      return 0;
    }
    File f = LittleFS.open("/ca.pem", "r");
    if (!f) {
      webserver_send_content_P(client, PSTR("Failed to open /ca.pem\n"), 24);
      return 0;
    }
    char buf[256];
    while (f.available()) {
      size_t n = f.readBytes(buf, sizeof(buf));
      if (n > 0) {
        webserver_send_content(client, buf, n);
      }
    }
    f.close();
  }
  return 0;
}
#endif

/*
 * showFirmwareSuccess -- Serve a simple HTML "firmware update succeeded"
 *   response page.  Sends the firmwareSuccessResponse string verbatim.
 * Thread-safety: Single-client callback; reentrant across clients.
 */
int showFirmwareSuccess(struct webserver_t *client) {
  if (client->content == 0) {
    webserver_send(client, 200, (char *)"text/html", strlen_P(firmwareSuccessResponse));
    webserver_send_content_P(client, firmwareSuccessResponse, strlen_P(firmwareSuccessResponse));
  }
  return 0;
}

/*
 * printUpdateError -- Translate the OTA Update.error() code into a
 *   human-readable string written into the caller-provided buffer.
 *   Handles: OK, WRITE, ERASE, READ, SPACE, SIZE, STREAM, NO_DATA,
 *   MAGIC_BYTE, ACTIVATE, NO_PARTITION, BAD_ARGUMENT, ABORT, and UNKNOWN.
 * Thread-safety: Reentrant (only uses local state and the Update singleton).
 */
static void printUpdateError(char **out, uint8_t size) {
  uint8_t len = 0;
  len = snprintf_P(*out, size, PSTR("ERROR[%u]: "), Update.getError());
  if (Update.getError() == UPDATE_ERROR_OK) {
    snprintf_P(&(*out)[len], size - len, PSTR("No Error"));
  } else if (Update.getError() == UPDATE_ERROR_WRITE) {
    snprintf_P(&(*out)[len], size - len, PSTR("Flash Write Failed"));
  } else if (Update.getError() == UPDATE_ERROR_ERASE) {
    snprintf_P(&(*out)[len], size - len, PSTR("Flash Erase Failed"));
  } else if (Update.getError() == UPDATE_ERROR_READ) {
    snprintf_P(&(*out)[len], size - len, PSTR("Flash Read Failed"));
  } else if (Update.getError() == UPDATE_ERROR_SPACE) {
    snprintf_P(&(*out)[len], size - len, PSTR("Not Enough Space"));
  } else if (Update.getError() == UPDATE_ERROR_SIZE) {
    snprintf_P(&(*out)[len], size - len, PSTR("Bad Size Given"));
  } else if (Update.getError() == UPDATE_ERROR_STREAM) {
    snprintf_P(&(*out)[len], size - len, PSTR("Stream Read Timeout"));
#ifdef UPDATE_ERROR_NO_DATA
  } else if (Update.getError() == UPDATE_ERROR_NO_DATA) {
    snprintf_P(&(*out)[len], size - len, PSTR("No data supplied"));
#endif
  } else if (Update.getError() == UPDATE_ERROR_MAGIC_BYTE) {
    snprintf_P(&(*out)[len], size - len, PSTR("Wrong Magic Byte, not 0xE9"));
  } else if (Update.getError() == UPDATE_ERROR_ACTIVATE) {
    snprintf_P(&(*out)[len], size - len, PSTR("Could Not Activate The Firmwaren"));
  } else if (Update.getError() == UPDATE_ERROR_NO_PARTITION) {
    snprintf_P(&(*out)[len], size - len, PSTR("Partition Could Not be Found"));
  } else if (Update.getError() == UPDATE_ERROR_BAD_ARGUMENT) {
    snprintf_P(&(*out)[len], size - len, PSTR("Bad Argument"));
  } else if (Update.getError() == UPDATE_ERROR_ABORT) {
    snprintf_P(&(*out)[len], size - len, PSTR("Aborted , Invalid bootstrapping state, reset ESP32 before updating"));
  } else {
    snprintf_P(&(*out)[len], size - len, PSTR("UNKNOWN"));
  }
}


/*
 * showFirmwareFail -- Serve a "firmware update failed" HTML response page
 *   that includes the error description produced by printUpdateError().
 * Thread-safety: Single-client callback; reentrant across clients.
 */
int showFirmwareFail(struct webserver_t *client) {
  if (client->content == 0) {
    char str[255] = { '\0' }, *p = str;
    printUpdateError(&p, sizeof(str));

    webserver_send(client, 200, (char *)"text/html", strlen_P(firmwareFailResponse) + strlen(str));
    webserver_send_content_P(client, firmwareFailResponse, strlen_P(firmwareFailResponse));
    webserver_send_content(client, str, strlen(str));
  }
  return 0;
}
