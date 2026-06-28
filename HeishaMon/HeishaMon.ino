
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Adafruit_NeoPixel.h>
  #define heatpumpSerial Serial1
  #define loggingSerial Serial //usb serial CDC
  #define uartSerial Serial0 //not used, 10x header pin
  #define proxySerial Serial2
  #define HEATPUMPRX 18
  #define HEATPUMPTX 17
  #define PROXYRX 9
  #define PROXYTX 8
  #define ENABLEPIN 5
  #define ENABLEOTPIN 4
  #ifndef HEISHAMON_LED_PIN
    #define HEISHAMON_LED_PIN 48
  #endif
  #define LEDPIN HEISHAMON_LED_PIN
  #define BOOTPIN 0


#include <DNSServer.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ArduinoJson.h>

#include "lwip/apps/sntp.h"
#include "src/common/timerqueue.h"
#include "src/common/stricmp.h"
#include "src/common/log.h"
#include "src/common/progmem.h"
#include "src/rules/rules.h"

#include "webfunctions.h"
#include "decode.h"
#include "commands.h"
#include "rules.h"
#include "version.h"
#include "mqtt_queue.h"

DNSServer dnsServer; // DNS server for captive portal during AP mode

const byte DNS_PORT = 53; // DNS server port

#define SERIALTIMEOUT 2000 // wait until all 203 bytes are read, must not be too long to avoid blocking the code

settingsStruct heishamonSettings; // Main settings struct, loaded from config.json on boot

uint32_t neoPixelState = 0; //running neoPixelState
bool inSetup; //bool to check if still booting
volatile bool sending = false; // mutex for sending data
bool mqttcallbackinprogress = false; // mutex for processing mqtt callback

bool extraDataBlockAvailable = false; // this will be set to true if, during boot, heishamon detects this heatpump has extra data block (like K and L series do)

#define MQTTRECONNECTTIMER 30000 //it takes 30 secs for each mqtt server reconnect attempt
unsigned long lastMqttReconnectAttempt = 0; // Timestamp of last MQTT reconnect attempt

unsigned long bootButtonNotPressed = 0; // Timestamp when boot button was last seen released (used for long-press detection)

#define WIFIRETRYTIMER 15000 // switch between hotspot and configured SSID each 10 secs if SSID is lost
unsigned long lastWifiRetryTimer = 0; // Timestamp of last WiFi connection retry
bool doInitialWifiScan = true; //we want an initial wifi scan to fill in the dropbox on the wifi settings page

unsigned long lastRunTime = 0; // Timestamp of last periodic stats/log output in loop()

volatile unsigned long sendCommandReadTime = 0; //set to millis value during send, allow to wait millis for answer

unsigned long goodreads = 0;   // Count of serial reads with valid header + checksum
unsigned long totalreads = 0;  // Total serial read attempts
unsigned long badcrcread = 0;  // Count of CRC checksum failures
unsigned long badheaderread = 0; // Count of invalid header (wrong sync bytes)
unsigned long tooshortread = 0; // Count of incomplete (truncated) reads
unsigned long toolongread = 0;  // Count of reads exceeding expected length
unsigned long timeoutread = 0;  // Count of serial read timeouts
float readpercentage = 0;      // Percentage of good reads (goodreads/totalreads*100)
static int uploadpercentage = 0; // Firmware OTA upload progress (0-20 * 5%)

// instead of passing array pointers between functions we just define this in the global scope
#define MAXDATASIZE 255        // Maximum size of serial data / proxy data buffers
char data[MAXDATASIZE] = { '\0' }; // Serial receive buffer for heatpump data
byte data_length = 0;            // Number of valid bytes in data[]

#ifdef ESP32
//for received proxied data
char proxydata[MAXDATASIZE] = { '\0' }; // Serial receive buffer for proxy (CZ-TAW1 passthrough) data
byte proxydata_length = 0;               // Number of valid bytes in proxydata[]
//for the neopixel
Adafruit_NeoPixel pixels(1, LEDPIN);
//for the vTask
QueueHandle_t pcbQueue = NULL;        // Queue (depth 1) holding the latest optional PCB query data
QueueHandle_t cmdQueue = NULL;        // Queue for user commands to send to heatpump (consumed by serialTXTask)
QueueHandle_t logQueue = NULL;        // Queue for log messages from FreeRTOS tasks (consumed in loop())
QueueHandle_t mqttPublishQueue = NULL; // Queue for MQTT publish messages (consumed by mqttTask)
#endif

// store decoded heatpump data for use by other modules (web, proxy, opentherm, mqtt publish)
char actData[DATASIZE] = { '\0' };      // Decoded heatpump main data block (header 0x10, 203 bytes)
char actDataExtra[DATASIZE] = { '\0' }; // Decoded heatpump extra data block (header 0x21, 203 bytes)
char actOptData[OPTDATASIZE]  = { '\0' }; // Decoded optional PCB data (header 0xF1)

// log message to sprintf to (reusable buffer, content is volatile)
#define LOG_MSG_SIZE 256
char log_msg[LOG_MSG_SIZE]; // Reusable buffer for sprintf log messages

// mqtt topic to sprintf and then publish to
char mqtt_topic[256]; // Reusable buffer for sprintf MQTT topic strings

static int mqttReconnects = 0; // Number of successful MQTT connections since boot

// can't have too much in buffer due to memory shortage
#define MAXCOMMANDSINBUFFER 10

// buffer for commands to send
struct cmdbuffer_t {
  uint8_t length;
  byte data[128];
} cmdbuffer[MAXCOMMANDSINBUFFER];

static uint8_t cmdstart = 0;
static uint8_t cmdend = 0;
static uint8_t cmdnrel = 0;



// mqtt
#ifdef TLS_SUPPORT
#include <WiFiClientSecure.h>
WiFiClientSecure *mqtt_tls_client = nullptr;
WiFiClient mqtt_wifi_client;
bool loadTlsCaFromFS(WiFiClientSecure *client);
static bool last_tls_enabled = false;
static bool new_ca_stored = false;
static std::unique_ptr<char[]> persistent_ca_pem;
PubSubClient mqtt_client;
#else
WiFiClient mqtt_wifi_client;
PubSubClient mqtt_client(mqtt_wifi_client);
#endif


bool firstConnectSinceBoot = true; //if this is true there is no first connection made yet

struct timerqueue_t **timerqueue = NULL;
int timerqueue_size = 0;

#ifdef ESP32
#define ETH_TYPE        ETH_PHY_W5500
#define ETH_ADDR         1
#define ETH_CS          10
#define ETH_IRQ          15
#define ETH_RST          14

#ifndef HEISHAMON_ENABLE_W5500
#define HEISHAMON_ENABLE_W5500 0
#endif

// SPI pins
#define ETH_SPI_SCK     12
#define ETH_SPI_MISO    13
#define ETH_SPI_MOSI    11

void setupETH() {
#if HEISHAMON_ENABLE_W5500
  SPI.begin(ETH_SPI_SCK, ETH_SPI_MISO, ETH_SPI_MOSI);
  if (ETH.begin(ETH_TYPE, ETH_ADDR, ETH_CS, ETH_IRQ, ETH_RST, SPI)) {
    //sethostname on ESP32 after eth.begin (!! for wifi is most be before...!!)
    ETH.setHostname(heishamonSettings.wifi_hostname);
  } else {
    loggingSerial.println("Could not start ethernet. No ethernet module installed?");
  }
#else
  loggingSerial.println("Ethernet init skipped (HEISHAMON_ENABLE_W5500=0)");
#endif
}
#endif



/*
    check_wifi will process wifi reconnecting managing
*/
void check_wifi() {
  wl_status_t wifistatus = WiFi.status();
  bool ethUp = ETH.hasIP();
  bool wifiUp = (wifistatus == WL_CONNECTED);

  /* ---------- Fast path: network is up ---------- */
  if (wifiUp || ethUp) {

    neoPixelState = pixels.Color(0, 0, 0); // normal operation
    lastWifiRetryTimer = millis();

    // Shut down hotspot if no longer needed
    if ((WiFi.getMode() & WIFI_MODE_AP) &&
        ((heishamonSettings.wifi_ssid[0] != '\0') || !heishamonSettings.hotspot)) {

      log_message(_F("WiFi or ETH connected, shutting down hotspot"));
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      if (wifistatus != WL_CONNECTED) { //it must be that ETH reconnected, so keep trying WiFi in the background
        WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
        if (heishamonSettings.wifi_password[0] == '\0') {
          WiFi.begin(heishamonSettings.wifi_ssid);
        } else {
          WiFi.begin(heishamonSettings.wifi_ssid, heishamonSettings.wifi_password);
        }
      }
    }

    if (firstConnectSinceBoot) {
      firstConnectSinceBoot = false;

      lastMqttReconnectAttempt = 0;
      setupOTA();

      MDNS.begin(heishamonSettings.wifi_hostname);
      MDNS.addService("http", "tcp", 80);

      if (heishamonSettings.wifi_ssid[0] == '\0') {
        log_message(_F("Storing WiFi credentials from persistent memory"));
        WiFi.SSID().toCharArray(heishamonSettings.wifi_ssid, 40);
        WiFi.psk().toCharArray(heishamonSettings.wifi_password, 40);
        JsonDocument jsonDoc;
        settingsToJson(jsonDoc, &heishamonSettings);
        saveJsonToFile(jsonDoc, "config.json");
      }

      ntpReload(&heishamonSettings);
      logprintln_P(F("NTP sync scheduled"));
      timerqueue_insert(300, 0, -6);
    }

    return;
  }

  /* ---------- Network is DOWN ---------- */

  neoPixelState = pixels.Color(16, 16, 0); // yellow: degraded

  if (heishamonSettings.hotspot) {
    dnsServer.processNextRequest();
  }

  // If AP client is connected, STA must back off
  if (WiFi.softAPgetStationNum() > 0) {
    if (WiFi.getMode() != WIFI_AP) {
      log_message(_F("SoftAP client active, suspending STA reconnect"));
	    WiFi.disconnect(true);
	    WiFi.mode(WIFI_AP);
    }
	  return; //always return if hotspot is used
  }

  // Periodic retry gate
  if ((unsigned long)(millis() - lastWifiRetryTimer) < WIFIRETRYTIMER) {
    return;
  }
  lastWifiRetryTimer = millis();

  // Ensure AP is running if allowed
  if (heishamonSettings.hotspot && !(WiFi.getMode() & WIFI_MODE_AP)) {
    log_message(_F("Starting setup hotspot"));
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP(_F("HeishaMon-Setup"));
  }

  // Disable STA so next retry is clean and we wait WIFIRETRYTIMER so hotspot can do its thing
  if (WiFi.getMode() != WIFI_AP) {
    log_message(_F("Disabling WiFi STA for a while..."));	
	  WiFi.disconnect(true);
    WiFi.mode(WIFI_AP); 
    return;
  }

  // Retry STA connection
  if (heishamonSettings.wifi_ssid[0] != '\0') {
    // Repair STA if it is stopped
    if (!(WiFi.getMode() & WIFI_MODE_STA)) {
      log_message(_F("STA stopped, re-enabling STA"));
      WiFi.mode(WIFI_AP_STA);
      delay(50);
    }    
    log_message(_F("Retrying configured WiFi"));
    WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
    if (heishamonSettings.wifi_password[0] == '\0') {
      WiFi.begin(heishamonSettings.wifi_ssid);
    } else {
      WiFi.begin(heishamonSettings.wifi_ssid, heishamonSettings.wifi_password);
    }
  }
}

#ifdef TLS_SUPPORT
bool loadTlsCaFromFS(WiFiClientSecure *client) {
  if (!LittleFS.exists("/ca.pem")) {
    log_message(_F("[TLS] /ca.pem not found"));
    return false;
  }
  File certFile = LittleFS.open("/ca.pem", "r");
  if (!certFile) {
    log_message(_F("[TLS] open(/ca.pem) failed"));
    return false;
  }
  size_t certSize = certFile.size();
  if (certSize == 0) {
    log_message(_F("[TLS] /ca.pem is empty"));
    certFile.close();
    return false;
  }
  persistent_ca_pem.reset(new char[certSize + 1]);
  size_t n = certFile.readBytes(persistent_ca_pem.get(), certSize);
  persistent_ca_pem[n] = '\0';
  certFile.close();
  client->setCACert(persistent_ca_pem.get());
  log_message(_F("[TLS] CA loaded into client"));
  return true;
}
#endif


/*
 * Attempts to connect/reconnect to the MQTT broker.
 * Mechanism: Throttled to one attempt per MQTTRECONNECTTIMER (30s). On connect, subscribes to
 * commands/opentherm/gpio/raw topics, publishes LWT "Online" and IP address. On first connect
 * (mqttReconnects==1), triggers a resend of all heatpump and 1-wire values.
 * Thread-safety: Called only from mqttTask on core 1. No concurrent access to mqtt_client or
 * heishamonSettings needs locking because only this task touches them.
 * Data flow: Reads heishamonSettings (mqtt_server, mqtt_topic_base, etc.). Writes mqttReconnects,
 * publishes via mqttPublishQueued. Subscribes via mqtt_client.subscribe.
 */
void mqtt_reconnect()
{
  unsigned long now = millis();
  if ((lastMqttReconnectAttempt == 0) || ((unsigned long)(now - lastMqttReconnectAttempt) > MQTTRECONNECTTIMER)) { //only try reconnect each MQTTRECONNECTTIMER seconds or on boot when lastMqttReconnectAttempt is still 0
    lastMqttReconnectAttempt = now;
    if (mqttReconnects == 0) {
      log_message(_F("Connecting to mqtt server ..."));
    } else {
      log_message(_F("Reconnecting to mqtt server ..."));
    }
    char topic[256];
    sprintf(topic, "%s/%s", heishamonSettings.mqtt_topic_base, mqtt_willtopic);
#ifdef TLS_SUPPORT
    if (heishamonSettings.mqtt_tls_enabled != last_tls_enabled) {
      mqtt_client.disconnect();
      if (last_tls_enabled) {
        mqtt_tls_client->stop();
      } else {
        mqtt_wifi_client.stop();
        if (!loadTlsCaFromFS(mqtt_tls_client)) {
          log_message(_F("[TLS] Proceeding without valid CA (expect failure)"));
        }
      }
      last_tls_enabled = heishamonSettings.mqtt_tls_enabled;
    }

    if (new_ca_stored) {
      log_message(_F("[TLS] Trying to load new CA ertificate"));
      if (!loadTlsCaFromFS(mqtt_tls_client)) {
        log_message(_F("[TLS] Proceeding without valid CA (expect failure)"));
      }
      new_ca_stored = false;
    }
    if (heishamonSettings.mqtt_tls_enabled) {
      mqtt_client.setClient(*mqtt_tls_client);
    } else {
      mqtt_client.setClient(mqtt_wifi_client);
    }
      mqtt_client.setSocketTimeout(10);
      mqtt_client.setKeepAlive(30);
      mqtt_client.setServer(heishamonSettings.mqtt_server, atoi(heishamonSettings.mqtt_port));
#endif
    if (mqtt_client.connect(heishamonSettings.wifi_hostname, heishamonSettings.mqtt_username, heishamonSettings.mqtt_password, topic, 1, true, "Offline"))
    {
      mqttReconnects++;
      if (heishamonSettings.opentherm) {
        sprintf(topic, "%s/%s/#", heishamonSettings.mqtt_topic_base, mqtt_topic_opentherm_read);
        mqtt_client.subscribe(topic);
      }
      sprintf(topic, "%s/%s/#", heishamonSettings.mqtt_topic_base, mqtt_topic_commands);
      mqtt_client.subscribe(topic);
      sprintf(topic, "%s/%s/#", heishamonSettings.mqtt_topic_base, mqtt_topic_gpio);
      mqtt_client.subscribe(topic);      
      sprintf(topic, "%s/%s", heishamonSettings.mqtt_topic_base, mqtt_send_raw_value_topic);
      mqtt_client.subscribe(topic);
      sprintf(topic, "%s/%s", heishamonSettings.mqtt_topic_base, mqtt_willtopic);
      mqttPublishQueued(topic, "Online", true);
      sprintf(topic, "%s/%s", heishamonSettings.mqtt_topic_base, mqtt_iptopic);
      if (ETH.hasIP()) {
        mqttPublishQueued(topic, ETH.localIP().toString().c_str(), true);
      } else {
        mqttPublishQueued(topic, WiFi.localIP().toString().c_str(), true);
      }

      if (heishamonSettings.use_s0) { // connect to s0 topic to retrieve older watttotal from mqtt
        sprintf_P(mqtt_topic, PSTR("%s/%s/WatthourTotal/1"), heishamonSettings.mqtt_topic_base, mqtt_topic_s0);
        mqtt_client.subscribe(mqtt_topic);
        sprintf_P(mqtt_topic, PSTR("%s/%s/WatthourTotal/2"), heishamonSettings.mqtt_topic_base, mqtt_topic_s0);
        mqtt_client.subscribe(mqtt_topic);
      }
      if (mqttReconnects == 1) { //only resend all data on first connect to mqtt so a data bomb like and bad mqtt server will not cause a reconnect bomb everytime
        if (heishamonSettings.use_1wire) resetlastalldatatime_dallas(); //resend all 1wire values to mqtt
        resetlastalldatatime(); //resend all heatpump values to mqtt
      }
      //use this to receive valid heishamon raw data from other heishamon to debug this OT code
//#define RAWDEBUG
#ifdef RAWDEBUG
      if ( heishamonSettings.listenonly) {
        mqtt_client.subscribe((char*)"panasonic_heat_pump/raw/data"); //subscribe to raw heatpump data over MQTT
      }
#endif
    }
//#ifdef TLS_SUPPORT // error state is useful in any case
    else {
      int8_t err = mqtt_client.state();
      log_message(_F("MQTT connect failed, state:"));
      switch (err) {
        case -1: log_message(_F(" -1 → TLS handshake or network error")); break;
        case -2: log_message(_F(" -2 → Connection timeout – cannot reach broker or CA/time error")); break;
        case -3: log_message(_F(" -3 → Server not found or rejected")); break;
        case -4: log_message(_F(" -4 → Connection lost")); break;
        case -5: log_message(_F(" -5 → Check username/password")); break;
        default: log_message(_F("    → Unknown error")); break;
      }
    }
//#endif
  }
}

#ifdef ESP32
/*
 * Briefly flashes the NeoPixel blue during an operation, then restores the previous state.
 * Mechanism: status=true sets pixel to dim blue; status=false restores the running neoPixelState.
 * Calls pixels.show() to commit.
 * Thread-safety: Called from log_message() which can be invoked from any task. neoPixelState is
 * read-only here. No locking; concurrent pixel writes are benign on single-LED strip.
 * Data flow: Reads neoPixelState (running color). Writes to NeoPixel via pixels API.
 */
void blinkNeoPixel(bool status) {
  if (status) {
    pixels.setPixelColor(0, 0, 0, 16); //blue
  } else {
    pixels.setPixelColor(0, neoPixelState);
  }
  pixels.show(); 
}
#endif  


/*
 * Logs a timestamped message to serial, MQTT log topic, and websocket.
 * Mechanism: Builds a string "[timestamp] (millis): message", writes to loggingSerial if
 * heishamonSettings.logSerial1 is set, queues an MQTT publish if logMqtt is set, and sends a JSON
 * websocket frame. Dynamically allocates/frees the formatted line.
 * Thread-safety: Can be called from any task (loop, serialTXTask, mqttTask, etc.). Uses mqttPublish-
 * Queued (thread-safe queue). Websocket writes are not locked — may interleave on concurrent calls.
 * inSetup flag prevents blinkNeoPixel during boot to avoid watchdog issues.
 * Data flow: Reads heishamonSettings (logSerial1, logMqtt). Writes to loggingSerial, mqttPublishQueue,
 * websocket_write_all. Calls blinkNeoPixel (side-effect on NeoPixel).
 */
void log_message(char* string)
{
#ifdef ESP32
  if (!inSetup) blinkNeoPixel(true);
#endif
  time_t rawtime;
  rawtime = time(NULL);
  struct tm *timeinfo = localtime(&rawtime);
  char timestring[32];
  strftime(timestring, 32, "%c", timeinfo);
  size_t len = strlen(string) + strlen(timestring) + 32; //+32 long enough to contain millis() and the json part later for websocket mesg
  char* log_line = (char *) malloc(len);
  snprintf(log_line, len, "%s (%lu): %s", timestring, millis(), string);

  if (heishamonSettings.logSerial1) {
    loggingSerial.println(log_line);
  }
  if (heishamonSettings.logMqtt)
  {
    char log_topic[256];
    sprintf(log_topic, "%s/%s", heishamonSettings.mqtt_topic_base, mqtt_logtopic);
    mqttPublishQueued(log_topic, log_line, false);
  }
  //send log message to websocket
  snprintf(log_line, len+12, "{\"logMsg\":\"%s (%lu): %s\"}", timestring, millis(), string);
  websocket_write_all(log_line, strlen(log_line));
  free(log_line);
#ifdef ESP32
  if (!inSetup) blinkNeoPixel(false);
#endif  
}

/*
 * Logs a hex dump of a byte array, 32 bytes per line, via log_message.
 * Mechanism: Iterates through hex[] in LOGHEXBYTESPERLINE chunks, formats each as
 * "XX XX XX ..." into a local buffer, then calls log_message().
 * Thread-safety: Pure processing plus log_message call — same thread-safety as log_message.
 * Data flow: Reads hex[] array. Writes via log_message.
 */
void logHex(char *hex, byte hex_len) {
#define LOGHEXBYTESPERLINE 32  // please be aware of max mqtt message size
  for (int i = 0; i < hex_len; i += LOGHEXBYTESPERLINE) {
    char buffer [(LOGHEXBYTESPERLINE * 3) + 1];
    buffer[LOGHEXBYTESPERLINE * 3] = '\0';
    for (int j = 0; ((j < LOGHEXBYTESPERLINE) && ((i + j) < hex_len)); j++) {
      sprintf(&buffer[3 * j], "%02X ", hex[i + j]);
    }
    sprintf_P(log_msg, PSTR("data: %s"), buffer ); log_message(log_msg);
  }
}

/*
 * Convenience wrapper to publish a subtopic under a topic with the default retain setting.
 * Mechanism: Delegates to the 4-argument mqttPublish with MQTT_RETAIN_VALUES.
 * Thread-safety: Same as 4-argument mqttPublish (thread-safe via queue).
 * Data flow: Forwards to mqttPublish(topic, subtopic, value, retain).
 */
void mqttPublish(char* topic, char* subtopic, char* value) {
  mqttPublish(topic, subtopic, value, MQTT_RETAIN_VALUES);
}

/*
 * Queues an MQTT publish message for asynchronous transmission by mqttTask.
 * Mechanism: Copies topic, payload, and retain flag into a mqttPublishMsg_t struct and sends it
 * to the mqttPublishQueue FreeRTOS queue. Non-blocking (0 ticks wait).
 * Thread-safety: Fully thread-safe via FreeRTOS queue. Can be called from any task.
 * Data flow: Writes to mqttPublishQueue (consumed by mqttTask).
 */
void mqttPublishQueued(const char* topic, const char* payload, bool retain) {
  if (mqttPublishQueue) {
    mqttPublishMsg_t msg;
    strlcpy(msg.topic, topic, sizeof(msg.topic));
    strlcpy(msg.payload, payload, sizeof(msg.payload));
    msg.retain = retain;
    xQueueSend(mqttPublishQueue, &msg, 0);
  }
}

/*
 * Formats "topic_base/topic/subtopic" and queues the value for MQTT publish.
 * Mechanism: Builds the full topic string from heishamonSettings.mqtt_topic_base and the provided
 * topic/subtopic, then calls mqttPublishQueued with the given retain flag.
 * Thread-safety: Thread-safe via mqttPublishQueued (FreeRTOS queue).
 * Data flow: Reads heishamonSettings.mqtt_topic_base. Writes to mqttPublishQueue.
 */
void mqttPublish(char* topic, char* subtopic, char* value, bool retain) {
  char mqtt_topic[256];
  sprintf_P(mqtt_topic, PSTR("%s/%s/%s"), heishamonSettings.mqtt_topic_base, topic, subtopic);
  mqttPublishQueued(mqtt_topic, value, retain);
}



/*
 * Calculates the Panasonic heatpump checksum for a command.
 * Mechanism: Sums all bytes, XORs the sum with 0xFF, then adds 1. The result is appended to
 * each outbound frame. The receiver validates by summing all bytes including the checksum — the
 * result must be 0.
 * Thread-safety: Pure function with no shared state. Fully reentrant, safe from any task.
 * Data flow: Reads command[] array. Returns computed byte.
 */
byte calcChecksum(byte* command, int length) {
  byte chk = 0;
  for ( int i = 0; i < length; i++)  {
    chk += command[i];
  }
  chk = (chk ^ 0xFF) + 01;
  return chk;
}

/*
 * Validates the checksum of a received heatpump frame.
 * Mechanism: Sums all bytes in the frame (payload + checksum byte). Returns true if the sum is 0,
 * which is the Panasonic protocol's validity condition.
 * Thread-safety: Pure function, no shared state. Safe from any task.
 * Data flow: Reads check_data[] array. Returns bool.
 */
bool isValidReceiveChecksum(char* check_data, byte check_length) {
  byte chk = 0;
  for ( int i = 0; i < check_length; i++)  {
    chk += check_data[i];
  }
  return (chk == 0); //all received bytes + checksum should result in 0
}

#ifdef ESP32
/*
 * Reads and processes data from the CZ-TAW1 proxy serial port.
 * Mechanism: Accumulates bytes from proxySerial into proxydata[]. Validates header byte (must be
 * 0x71/0x31/0xF1), length field, and checksum. On a complete valid frame: if it is a query from
 * CZ-TAW1, replies with cached actData/actDataExtra or forwards to heatpump via send_command;
 * startup messages (0x31) and unknown messages are forwarded.
 * Thread-safety: Called from loop() on core 0. Accesses proxydata/proxydata_length, actData,
 * actDataExtra (read). send_command is thread-safe via cmdQueue. No mutex for the data buffers,
 * but serialTXTask on core 1 writes actData — potential read-vs-write race on actData. In practice
 * the race window is very small and data is refreshed every waitTime seconds.
 * Data flow: Reads proxySerial. Writes proxydata[], proxydata_length. Reads actData, actDataExtra.
 * Calls send_command (writes cmdQueue). Writes to proxySerial for replies.
 */
void readProxy()
{
  int proxylen = 0;
  while ((proxySerial.available()) && ((proxydata_length + proxylen) < MAXDATASIZE)) {
    proxydata[proxydata_length + proxylen] = proxySerial.read(); //read available data and place it after the last received data
    proxylen++;
    if ((proxydata[0] != 0x71) and  (proxydata[0] != 0x31) and  (proxydata[0] != 0xF1)) { //wrong header received!
      log_message(_F("PROXY Received bad header. Ignoring this data!"));
      if (heishamonSettings.logHexdump) logHex(proxydata, proxylen);
      proxydata_length = 0;
      return; //return so this while loop does not loop forever if there happens to be a continous invalid data stream
    }
  }
  //if ((proxylen > 0) && (proxydata_length == 0 )) proxy_totalreads++; //this is the start of a new read
  proxydata_length +=  proxylen;
  if (proxydata_length > 1 ) { //should have received length part of header now
    if ((proxydata_length > ( proxydata[1] + 3)) || (proxydata_length >= MAXDATASIZE)) {
      sprintf_P(log_msg, PSTR("PROXY Received %i bytes proxy %i\n"), proxydata_length, proxydata[1]);
      log_message(log_msg);
      log_message(_F("PROXY Received more data than header suggests! Ignoring this as this is bad data."));
      proxydata_length = 0;
      if (heishamonSettings.logHexdump) logHex(proxydata, proxydata_length);
      return;
    }
    if (proxydata_length == (proxydata[1] + 3)) { //we received all data (serial2_data[1] is header length field)
      sprintf_P(log_msg, PSTR("PROXY Received %i bytes"), proxydata_length); log_message(log_msg);
      if (heishamonSettings.logHexdump) logHex(proxydata, proxydata_length);
      if (! isValidReceiveChecksum(proxydata,proxydata_length) ) {
        log_message(_F("PROXY Checksum received false!"));
        proxydata_length = 0; //for next attempt
        return;
      }      
      log_message(_F("PROXY Checksum and header received ok!"));
      if ((proxydata[0]==0x71 or proxydata[0]==0xF1) and proxydata_length == (PANASONICQUERYSIZE+1)) { //this is a query from cztaw on proxy port
        if (proxydata[0]==0xf1) {  //this is a write query, just pass this message forward as new command
          log_message(_F("PROXY received write query, copy message forward to heatpump"));
          send_command((byte*)proxydata,proxydata_length-1); //strip CRC, will be calculated again in send_command
          //then just reply with the current settings, for read and write it is the same as the write is only acknowledged in the next read
          //so we just run to the next if statement
        }
        if (proxydata[3] == 0x10) {
          log_message(_F("PROXY requests basic data"));
          if ((actData[0] == 0x71) && (actData[1] == 0xc8) && (actData[2] == 0x01)) { //don't answer if we don't have data
            proxySerial.write(actData,DATASIZE); //should contain valid checksum also
          }
        } else if (proxydata[3] == 0x21 ) {
          log_message(_F("PROXY requests extra data"));
          if ((actDataExtra[0] == 0x71) && (actDataExtra[1] == 0xc8) && (actDataExtra[2] == 0x01)) { //don't answer if we don't have data
            proxySerial.write(actDataExtra,DATASIZE); //should contain valid checksum also
          }
        } else {
          log_message(_F("PROXY has sent unknown query! Forwarding to heatpump!"));
          send_command((byte *)proxydata, proxydata_length-1); //strip CRC from end as send_command wil recalculate it
        }
        proxydata_length = 0;
        return;
      } else if (proxydata[0]==0x31) {
        log_message(_F("PROXY received startup message, forwarding to heatpump!"));
        send_command((byte *)proxydata, proxydata_length-1); //strip CRC from end as send_command wil recalculate it
        proxydata_length = 0;
        return;
      } else {
        log_message(_F("PROXY received unknown message, forwarding it to heatpump anyway!"));
        send_command((byte *)proxydata, proxydata_length-1); //strip CRC from end as send_command wil recalculate it
        proxydata_length = 0;
        return;
      }
    }
  }
}
#endif

/*
 * Reads and processes a complete frame from the heatpump serial port.
 * Mechanism: Accumulates bytes from heatpumpSerial into data[]. After receiving at least 4 bytes,
 * validates the header (sync bytes 0x71/0x31, byte 2 must be 0x01), length field, and checksum.
 * On a complete valid frame of DATASIZE (203) bytes: if byte 3 is 0x10 it decodes the main data
 * block via decode_heatpump_data; if 0x21 it decodes the extra block via decode_heatpump_data_extra.
 * Frames of other sizes are forwarded to proxySerial or decoded as optional PCB data.
 * Thread-safety: Called from loop()/readHeatpump() on core 0. Accesses data/data_length (global),
 * sending (volatile), and various stat counters. sending is set by serialTXTask (core 1) and read
 * here — the volatile qualifier ensures visibility. No mutex; relies on the fact that serialTXTask
 * does not write data[] and readSerial does not modify sending except to clear it.
 * Data flow: Reads heatpumpSerial. Writes data[], data_length, sending, stat counters (goodreads,
 * badcrcread, etc.). Writes to actData/actDataExtra/actOptData via decode functions. Forwards
 * unrecognized frames to proxySerial.
 */
bool readSerial()
{
  int len = 0;
  while ((heatpumpSerial.available()) && ((data_length + len) < MAXDATASIZE)) {
    data[data_length + len] = heatpumpSerial.read(); //read available data and place it after the last received data
    len++;
  }

  if ((len > 0) && (data_length == 0 )) totalreads++; //this is the start of a new read
  data_length += len;

  if (data_length > 3) { //should have received length part of header now

    if (((data[0] != 0x71) && (data[0] != 0x31)) || (data[2] != 0x01))  { //wrong header received!
      if (heishamonSettings.logHexdump) {
        log_message(_F("Received bad header. Ignoring this data!"));
        logHex(data, len);
      }
      badheaderread++;
      data_length = 0;
      return false;
    }

    if ((data_length > (data[1] + 3)) || (data_length >= MAXDATASIZE) ) {
      log_message(_F("Received more data than header suggests! Ignoring this as this is bad data."));
      if (heishamonSettings.logHexdump) logHex(data, data_length);
      data_length = 0;
      toolongread++;
      return false;
    }

    if (data_length == (data[1] + 3)) { //we received all data (data[1] is header length field)
      sprintf_P(log_msg, PSTR("Received %d bytes data"), data_length); log_message(log_msg);
      sending = false; //we received an answer after our last command so from now on we can start a new send request again
      if (heishamonSettings.logHexdump) logHex(data, data_length);
      if (! isValidReceiveChecksum(data, data_length) ) {
        log_message(_F("Checksum received false!"));
        data_length = 0; //for next attempt
        badcrcread++;
        return false;
      }
      log_message(_F("Checksum and header received ok!"));
      goodreads++;

      if (data_length == DATASIZE)  {  //receive a full data block
        if  (data[3] == 0x10) { //decode the normal data block
          decode_heatpump_data(data, actData, log_message, heishamonSettings.mqtt_topic_base, heishamonSettings.updateAllTime);
          if ( (!extraDataBlockAvailable) && ((actData[0] == 0x71) && (actData[0xc7] >= 3)) ) { //do we have valid header and byte 0xc7 is more or equal 3 then assume K&L and more series
            log_message(_F("Extra data available on this heatpump"));
            extraDataBlockAvailable = true; //request for extra data next run
          }
          #ifdef RAWDEBUG
          {
            char mqtt_topic[256];
            sprintf(mqtt_topic, "%s/raw/data", heishamonSettings.mqtt_topic_base);
            mqtt_client.publish(mqtt_topic, (const uint8_t *)actData, DATASIZE, false); //do not retain this raw data
          }
          #endif
          data_length = 0;
          return true;
        } else if (data[3] == 0x21) { //decode the new model extra data block
          extraDataBlockAvailable = true; //set the flag to true so we know we can request this data always
          decode_heatpump_data_extra(data, actDataExtra, log_message, heishamonSettings.mqtt_topic_base, heishamonSettings.updateAllTime);
          #ifdef RAWDEBUG
          {
            char mqtt_topic[256];
            sprintf(mqtt_topic, "%s/raw/dataextra", heishamonSettings.mqtt_topic_base);
            mqtt_client.publish(mqtt_topic, (const uint8_t *)actDataExtra, DATASIZE, false); //do not retain this raw data
          }
          #endif
          data_length = 0;
          return true;
        } else {
          log_message(_F("Received a full size datagram but not for me. Forwarding to proxy port."));
          proxySerial.write(data,data_length);
          data_length = 0;
          return false;
        }
      }
      else if (data_length == OPTDATASIZE ) { //optional pcb acknowledge answer
        log_message(_F("Received optional PCB ack answer. Decoding this in OPT topics."));
        decode_optional_heatpump_data(data, actOptData, log_message, heishamonSettings.mqtt_topic_base, heishamonSettings.updateAllTime);
        data_length = 0;
        return true;
      }
      else {
        log_message(_F("Received a shorter datagram but not for me. Forwarding to proxy port."));
        proxySerial.write(data,data_length);
        data_length = 0;
        return false;
      }
    }
  }
  return false;
}

/*
 * Pops and sends the next buffered command when the serial line is idle (non-ESP32 path).
 * Mechanism: If sending is false and cmdnrel > 0, calls send_command with the oldest entry in the
 * circular buffer, then advances cmdstart and decrements cmdnrel.
 * Thread-safety: Called from loop() on non-ESP32 builds. sending is volatile, cmdnrel/cmdstart/
 * cmdbuffer are static. No locking — single-threaded access on non-ESP32.
 * Data flow: Reads sending, cmdnrel, cmdbuffer[cmdstart]. Calls send_command (writes to serial).
 * Writes cmdstart, cmdnrel.
 */
void popCommandBuffer() {
  // to make sure we can pop a command from the buffer
  if ((!sending) && cmdnrel > 0) {
    send_command(cmdbuffer[cmdstart].data, cmdbuffer[cmdstart].length);
    cmdstart = (cmdstart + 1) % (MAXCOMMANDSINBUFFER);
    cmdnrel--;
  }
}

/*
 * Pushes a command into the circular command buffer for deferred transmission (non-ESP32 path).
 * Mechanism: Checks buffer space, then copies the command bytes into cmdbuffer[cmdend] and advances
 * cmdend / increments cmdnrel. Overwrites oldest entry if full.
 * Thread-safety: Called from send_command on non-ESP32 when sending is busy. Single-threaded
 * context — no locking needed.
 * Data flow: Reads/writes cmdbuffer[], cmdend, cmdnrel. Reads command[].
 */
void pushCommandBuffer(byte* command, int length) {
  if (cmdnrel + 1 > MAXCOMMANDSINBUFFER) {
    log_message(_F("Too much commands already in buffer. Ignoring this commands.\n"));
    return;
  }
  cmdbuffer[cmdend].length = length;
  memcpy(&cmdbuffer[cmdend].data, command, length);
  cmdend = (cmdend + 1) % (MAXCOMMANDSINBUFFER);
  cmdnrel++;
}

#ifdef ESP32
/*
 * FreeRTOS task that manages all serial transmission to the heatpump.
 * Mechanism: Runs an infinite loop with priority-ordered transmission:
 *   1. (Highest) Optional PCB query every OPTIONALPCBQUERYTIME ms — sends the PCB frame, periodically
 *      saves to flash.
 *   2. Heatpump basic data query every waitTime seconds — sends panasonicQuery with 0x10 byte.
 *   3. Heatpump extra data query every waitTime seconds (offset) — sends panasonicQuery with 0x21 byte.
 *   4. (Lowest) User commands from cmdQueue.
 * Each send sets sending=true and records sendCommandReadTime. If sending stays true longer than
 * SERIALTIMEOUT+OPTIONALPCBQUERYTIME, it is force-cleared to prevent deadlock. Log messages are
 * sent to logQueue instead of calling log_message directly (which could block).
 * Thread-safety: Runs on core 1 (pinned). Accesses sending (volatile, shared with loop/readSerial),
 * pcbQueue/cmdQueue (FreeRTOS queues, thread-safe), logQueue, heishamonSettings (read-only after
 * setup). sending is the only shared-memory variable; all other communication is via queues.
 * Data flow: Reads pcbQueue (PCB data), cmdQueue (user commands), heishamonSettings (timing).
 * Writes heatpumpSerial, logQueue. Updates sending, sendCommandReadTime.
 */
void serialTXTask(void *pvParameters) {
  unsigned long lastPCBSendTime = 0;
  unsigned long lastHPSendTime = 0;
  unsigned long lastHPExtraSendTime = 0;
  unsigned long lastPCBSaveTime = 0;
  char local_log_msg[LOG_MSG_SIZE];

  byte localPCBQuery[OPTIONALPCBQUERYSIZE] = {0xF1, 0x11, 0x01, 0x50, 0x00, 0x00, 0x40, 0xFF, 0xFF, 0xE5, 0xFF, 0xFF, 0x00, 0xFF, 0xEB, 0xFF, 0xFF, 0x00, 0x00};
  
  for (;;) {
    unsigned long now = millis();

    if (sending && ((unsigned long)(millis() - sendCommandReadTime) > (SERIALTIMEOUT + OPTIONALPCBQUERYTIME) )) {
      //clear sending flag if taking too long so the optional pcb can still send regulary
      //normally the flag would already be cleared by the readserial timeout but if that process hangs (wifi, mqtt issue) this check will free it anyways
      sending = false;
    }

    // highest priority: optional PCB query every second
    if ((!sending) && ((unsigned long)(now - lastPCBSendTime) >= OPTIONALPCBQUERYTIME)) {
      lastPCBSendTime = now;
      if (heishamonSettings.optionalPCB && !heishamonSettings.listenonly) {
        sending = true;
        sendCommandReadTime = now;
        xQueuePeek(pcbQueue, localPCBQuery, 0);
        byte chk = calcChecksum(localPCBQuery, OPTIONALPCBQUERYSIZE);
        heatpumpSerial.write(localPCBQuery, OPTIONALPCBQUERYSIZE);
        heatpumpSerial.write(chk);
        sprintf_P(local_log_msg, PSTR("optional PCB datagram sent bytes: %d"), OPTIONALPCBQUERYSIZE + 1);
        xQueueSend(logQueue,local_log_msg,0);
      }
      // save to flash periodically
      if ((unsigned long)(now - lastPCBSaveTime) >= (1000 * OPTIONALPCBSAVETIME)) {
        lastPCBSaveTime = now;
        saveOptionalPCB(localPCBQuery, OPTIONALPCBQUERYSIZE);
      }
    }

    // second priority: static heatpump query every waitTime seconds
    if ((!sending) && (!heishamonSettings.listenonly)) {
      if ((unsigned long)(now - lastHPSendTime) >= (1000 * heishamonSettings.waitTime)) {
        sending = true;
        sendCommandReadTime = now;
        lastHPSendTime = now;
        byte chk = calcChecksum(panasonicQuery, PANASONICQUERYSIZE);
        heatpumpSerial.write(panasonicQuery, PANASONICQUERYSIZE);
        heatpumpSerial.write(chk);
        sprintf_P(local_log_msg, PSTR("heatpump request query sent bytes: %d"), PANASONICQUERYSIZE + 1);
        xQueueSend(logQueue,local_log_msg,0);    
      }
    }

    // third priority: extra data block query every waitTime seconds (offset from basic query)
    if ((!sending) && (!heishamonSettings.listenonly) && extraDataBlockAvailable) {
      if ((unsigned long)(now - lastHPExtraSendTime) >= (1000 * heishamonSettings.waitTime)) {
        lastHPExtraSendTime = now;
        sending = true;
        sendCommandReadTime = now;
        panasonicQuery[3] = 0x21;
        byte chk = calcChecksum(panasonicQuery, PANASONICQUERYSIZE);
        heatpumpSerial.write(panasonicQuery, PANASONICQUERYSIZE);
        heatpumpSerial.write(chk);
        panasonicQuery[3] = 0x10;
        xQueueSend(logQueue, (void*)"heatpump extra query sent", 0);
      }
    }    

    // lowest priority: user commands from queue
    if ((!sending) && (!heishamonSettings.listenonly)) {
      struct cmdbuffer_t cmd;
      if (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
        sending = true;
        sendCommandReadTime = now;
        byte chk = calcChecksum(cmd.data, cmd.length);
        heatpumpSerial.write(cmd.data, cmd.length);
        heatpumpSerial.write(chk);
        sprintf_P(local_log_msg, PSTR("Command datagram sent bytes: %d"), cmd.length + 1);
        xQueueSend(logQueue,local_log_msg,0);      
      }
    }

    vTaskDelay(1 / portTICK_PERIOD_MS);
  }
}

/*
 * FreeRTOS task that handles the MQTT client loop and drains the publish queue.
 * Mechanism: Runs an infinite loop calling mqtt_client.loop() to keep the TCP/TLS stack alive and
 * process incoming subscriptions. If WiFi/Ethernet is up but MQTT is disconnected, calls
 * mqtt_reconnect(). Drains all messages from mqttPublishQueue and publishes them.
 * Thread-safety: Runs on core 1 (pinned). mqtt_client is exclusively accessed by this task.
 * mqttPublishQueue is a FreeRTOS queue (multi-producer, single-consumer). No mutex needed.
 * Data flow: Reads mqttPublishQueue, WiFi/ETH status. Writes to mqtt_client for publishes.
 */
void mqttTask(void *pvParameters) {
  mqttPublishMsg_t msg;
  for (;;) {
    mqtt_client.loop();

    if ((WiFi.isConnected() || ETH.connected()) && !mqtt_client.connected()) {
      if (mqttReconnects > 0) log_message((char*)"Lost MQTT connection!");
      if (strlen(heishamonSettings.mqtt_server) > 0) mqtt_reconnect();
    }

    while (xQueueReceive(mqttPublishQueue, &msg, 0) == pdTRUE) {
      if (mqtt_client.connected()) {
        mqtt_client.publish(msg.topic, msg.payload, msg.retain);
      }
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

/*
 * FreeRTOS task that periodically processes Dallas 1-Wire temperature sensors.
 * Mechanism: Runs every 100ms; if use_1wire is enabled, calls dallasLoop() which reads sensors and
 * publishes values via mqttPublish (→ mqttPublishQueue).
 * Thread-safety: Runs on core 1. heishamonSettings.use_1wire is read-only after setup. dallasLoop
 * uses mqttPublish (thread-safe queue) and log_message (queue).
 * Data flow: Reads heishamonSettings. Writes to mqttPublishQueue via dallasLoop.
 */
void dallasTask(void *pvParameters) {
  for (;;) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    if (heishamonSettings.use_1wire) {
      dallasLoop(log_message, heishamonSettings.mqtt_topic_base);
    }
  }
}

/*
 * FreeRTOS task that periodically reads S0 pulse counter inputs.
 * Mechanism: Runs every 100ms; if use_s0 is enabled, calls s0Loop() which reads pulse counts and
 * publishes energy values via mqttPublish.
 * Thread-safety: Runs on core 1. heishamonSettings (use_s0, s0Settings) is read-only after setup.
 * s0Loop uses mqttPublish (thread-safe queue) and log_message.
 * Data flow: Reads heishamonSettings.s0Settings. Writes to mqttPublishQueue via s0Loop.
 */
void s0Task(void *pvParameters) {
  for (;;) {
    vTaskDelay(100 / portTICK_PERIOD_MS);
    if (heishamonSettings.use_s0) {
      s0Loop(log_message, heishamonSettings.mqtt_topic_base, heishamonSettings.s0Settings);
    }
  }
}

/*
 * FreeRTOS task that runs the OpenTherm protocol state machine.
 * Mechanism: Runs every 10ms; if opentherm is enabled, calls HeishaOTLoop() which manages the
 * OpenTherm master/slave communication, reads/writes actData, and publishes OT values via
 * mqtt_client.
 * Thread-safety: Runs on core 1. actData is shared with loop()/readSerial() on core 0 — potential
 * read-vs-write race, but OT data is updated every waitTime seconds and the window is small.
 * mqtt_client is also shared with mqttTask on core 1 — mqtt_client is NOT thread-safe. This is a
 * known design limitation: otTask and mqttTask both call mqtt_client.publish concurrently without
 * locking. In practice the ESP32 Arduino core's PubSubClient may tolerate this, but it can cause
 * corruption.
 * Data flow: Reads actData (heatpump decoded data), heishamonSettings. Writes to mqtt_client.
 */
void otTask(void *pvParameters) {
  for (;;) {
    vTaskDelay(10 / portTICK_PERIOD_MS);
    if (heishamonSettings.opentherm) {
      HeishaOTLoop(actData, mqtt_client, heishamonSettings.mqtt_topic_base);
    }
  }
}

/*
 * Queues a command to be sent to the heatpump (ESP32 path).
 * Mechanism: Copies the command into a cmdbuffer_t and sends it to cmdQueue for consumption by
 * serialTXTask. If listenonly is true, the command is silently dropped.
 * Thread-safety: Thread-safe via FreeRTOS queue. Can be called from any task (loop, mqtt_callback,
 * web server, rules engine).
 * Data flow: Reads heishamonSettings.listenonly. Writes to cmdQueue.
 */
bool send_command(byte* command, int length) {
  if ( heishamonSettings.listenonly ) {
    log_message(_F("Not sending this command. Heishamon in listen only mode!"));
    return false;
  }
  struct cmdbuffer_t cmd;
  cmd.length = length;
  memcpy(&cmd.data, command, length);
  xQueueSend(cmdQueue, &cmd, 0);
  return true;
}

#else

/*
 * Sends a command directly to the heatpump serial port (non-ESP32 path).
 * Mechanism: If sending is already true (another command in flight), pushes the command into the
 * circular buffer via pushCommandBuffer. Otherwise sets sending=true, calculates checksum, writes
 * command + checksum to heatpumpSerial, and records sendCommandReadTime for timeout detection.
 * Thread-safety: Called from loop() context only on non-ESP32 (single-core). No concurrent access.
 * Data flow: Reads heishamonSettings.listenonly, sending. Writes heatpumpSerial, sending,
 * sendCommandReadTime. Calls pushCommandBuffer on busy.
 */
bool send_command(byte* command, int length) {
  if ( heishamonSettings.listenonly ) {
    log_message(_F("Not sending this command. Heishamon in listen only mode!"));
    return false;
  }
  if ( sending ) {
    log_message(_F("Already sending data. Buffering this send request"));
    pushCommandBuffer(command, length);
    return false;
  }
  sending = true; //simple semaphore to only allow one send command at a time, semaphore ends when answered data is received

  byte chk = calcChecksum(command, length);
  int bytesSent = heatpumpSerial.write(command, length); //first send command
  bytesSent += heatpumpSerial.write(chk); //then calculcated checksum byte afterwards
  sprintf_P(log_msg, PSTR("sent bytes: %d including checksum value: %d "), bytesSent, int(chk));
  log_message(log_msg);

  if (heishamonSettings.logHexdump) logHex((char*)command, length);
  sendCommandReadTime = millis(); //set sendCommandReadTime when to timeout the answer of this command
  return true;
}
#endif

/*
 * MQTT subscription callback — processes incoming messages on subscribed topics.
 * Mechanism: Parses the topic (strips base), dispatches to:
 *   - mqtt_send_raw_value_topic: sends raw hex bytes via send_command
 *   - mqtt_topic_s0: restores watthour total value, then unsubscribes
 *   - mqtt_topic_commands: calls send_heatpump_command
 *   - mqtt_topic_opentherm_read: calls mqttOTCallback
 *   - mqtt_topic_gpio: calls mqttGPIOCallback
 * Uses mqttcallbackinprogress flag to prevent re-entrant execution.
 * Thread-safety: Called from mqtt_client.loop() in mqttTask on core 1. The mqttcallbackinprogress
 * flag prevents re-entry from the same task (PubSubClient calls the callback synchronously during
 * loop()). Not safe if multiple tasks called loop() — but only mqttTask does.
 * Data flow: Reads topic, payload, heishamonSettings. Writes via send_command (→cmdQueue),
 * restore_s0_Watthour, mqttOTCallback, mqttGPIOCallback.
 */
// Callback function that is called when a message has been pushed to one of your topics.
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
  if (mqttcallbackinprogress) {
    log_message(_F("Already processing another mqtt callback. Ignoring this one"));
  }
  else {
    mqttcallbackinprogress = true; //simple semaphore to make sure we don't have two callbacks at the same time
    char msg[length + 1];
    for (unsigned int i = 0; i < length; i++) {
      msg[i] = (char)payload[i];
    }
    msg[length] = '\0';
    char cb_log_msg[64];
    char* topic_command = topic + strlen(heishamonSettings.mqtt_topic_base) + 1; //strip base plus seperator from topic
    if (strcmp(topic_command, mqtt_send_raw_value_topic) == 0)
    { // send a raw hex string
      byte *rawcommand;
      rawcommand = (byte *) malloc(length);
      memcpy(rawcommand, msg, length);

      sprintf_P(cb_log_msg, PSTR("sending raw value"));
      log_message(cb_log_msg);
      send_command(rawcommand, length);
      free(rawcommand);
    } else if (strncmp(topic_command, mqtt_topic_s0, strlen(mqtt_topic_s0)) == 0)  // this is a s0 topic, check for watthour topic and restore it
    {
      char* topic_s0_watthour_port = topic_command + strlen(mqtt_topic_s0) + 15; //strip the first 17 "s0/WatthourTotal/" from the topic to get the s0 port
      int s0Port = String(topic_s0_watthour_port).toInt();
      float watthour = String(msg).toFloat();
      restore_s0_Watthour(s0Port, watthour);
      //unsubscribe after restoring the watthour values
      char mqtt_topic[256];
      sprintf(mqtt_topic, "%s", topic);
      if (mqtt_client.unsubscribe(mqtt_topic)) {
        log_message(_F("Unsubscribed from S0 watthour restore topic"));
      }
    } else if (strncmp(topic_command, mqtt_topic_commands, strlen(mqtt_topic_commands)) == 0)  // check for commands to heishamon
    {
      char* topic_sendcommand = topic_command + strlen(mqtt_topic_commands) + 1; //strip the first 9 "commands/" from the topic to get what we need
      send_heatpump_command(topic_sendcommand, msg, send_command, log_message, heishamonSettings.optionalPCB);
    //use this to receive valid heishamon raw data from other heishamon to debug this OT code
#ifdef RAWDEBUG
    } else if (strcmp((char*)"panasonic_heat_pump/raw/data", topic) == 0) {  // check for raw heatpump input
      sprintf_P(cb_log_msg, PSTR("Received raw heatpump data from MQTT"));
      log_message(cb_log_msg);
      decode_heatpump_data(msg, actData, log_message, heishamonSettings.mqtt_topic_base, heishamonSettings.updateAllTime);
      memcpy(actData, msg, DATASIZE);
#endif
    } else if (strncmp(topic_command, mqtt_topic_opentherm_read, strlen(mqtt_topic_opentherm_read)) == 0)  {
      char* topic_otcommand = topic_command + strlen(mqtt_topic_opentherm_read) + 1; //strip the opentherm subtopic from the topic
      mqttOTCallback(topic_otcommand, msg);
    } else if (strncmp(topic_command, mqtt_topic_gpio, strlen(mqtt_topic_gpio)) == 0)  {
      char* topic_gpiocommand = topic_command + strlen(mqtt_topic_gpio) + 1; //strip the gpio subtopic from the topic
      mqttGPIOCallback(topic_gpiocommand, msg);
    }    
    mqttcallbackinprogress = false;
  }
}

/*
 * Configures and starts the Arduino OTA (Over-The-Air update) service.
 * Mechanism: Sets OTA port (8266), hostname from settings, password from settings, registers
 * empty onStart/onEnd/onProgress/onError callbacks, then calls ArduinoOTA.begin().
 * Thread-safety: Called once during setup on core 0. No concurrency.
 * Data flow: Reads heishamonSettings.wifi_hostname, heishamonSettings.ota_password.
 */
void setupOTA() {
  // Port defaults to 8266
  ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  ArduinoOTA.setHostname(heishamonSettings.wifi_hostname);

  // Set authentication
  ArduinoOTA.setPassword(heishamonSettings.ota_password);

  ArduinoOTA.onStart([]() {
  });
  ArduinoOTA.onEnd([]() {
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {

  });
  ArduinoOTA.onError([](ota_error_t error) {

  });
  ArduinoOTA.begin();
}



/*
 * Web server callback — handles all HTTP request routing, argument processing, and response
 * generation for the embedded web server.
 * Mechanism: State machine driven by client->step:
 *   REQUEST_METHOD  — detect POST for settings-saving routes
 *   REQUEST_URI     — route to handler IDs (1=root, 20=json, 30=reboot, 40=debug, etc.)
 *   ARGS            — process POST/GET arguments (settings save, commands, firmware upload, rules)
 *   HEADER          — process headers (currently no-op)
 *   WRITE           — generate HTTP response for each route (HTML, JSON, redirect, firmware)
 *   CREATE_HEADER   — set response headers (Location, CORS)
 *   CLOSE           — free per-request resources
 * Thread-safety: Called from webserver_loop() in loop() on core 0. Accesses heishamonSettings,
 * actData, actDataExtra, actOptData (read), LittleFS (filesystem writes). No locking; single-
 * threaded by design (all web handling on core 0).
 * Data flow: Reads heishamonSettings, actData, actDataExtra, actOptData, extraDataBlockAvailable.
 * Writes heishamonSettings via saveSettings, LittleFS via firmware/rules/cert upload. Writes HTTP
 * responses via webserver_send/webserver_send_content_P.
 */
int8_t webserver_cb(struct webserver_t *client, void *dat) {
  

  switch (client->step) {
    case WEBSERVER_CLIENT_REQUEST_METHOD: {
        if (strcmp_P((char *)dat, PSTR("POST")) == 0) {
          client->route = 110;
        }
        return 0;
      } break;
    case WEBSERVER_CLIENT_REQUEST_URI: {
        if (strcmp_P((char *)dat, PSTR("/")) == 0) {
          client->route = 1;
        } else if (strcmp_P((char *)dat, PSTR("/json")) == 0) {
          client->route = 20;
        } else if (strcmp_P((char *)dat, PSTR("/reboot")) == 0) {
          client->route = 30;
        } else if (strcmp_P((char *)dat, PSTR("/debug")) == 0) {
          client->route = 40;
          log_message(_F("Debug URL requested"));
        } else if (strcmp_P((char *)dat, PSTR("/wifiscan")) == 0) {
          client->route = 50;
        } else if (strcmp((char *)dat, "/dallasalias") == 0) {
          client->route = 60;
        } else if (strcmp((char *)dat, "/togglelog") == 0) {
          client->route = 1;
          log_message(_F("Toggled mqtt log flag"));
          heishamonSettings.logMqtt ^= true;
        } else if (strcmp_P((char *)dat, PSTR("/togglehexdump")) == 0) {
          client->route = 1;
          log_message(_F("Toggled hexdump log flag"));
          heishamonSettings.logHexdump ^= true;
        } else if (strcmp_P((char *)dat, PSTR("/connecttest.txt")) == 0 ||
                   strcmp_P((char *)dat, PSTR("/ncsi.txt")) == 0 ||
                   strcmp_P((char *)dat, PSTR("/redirect")) == 0 ||
                   strcmp_P((char *)dat, PSTR("/fwlink")) == 0 ||
                   strcmp_P((char *)dat, PSTR("/generate_204")) == 0 ||
                   strcmp_P((char *)dat, PSTR("/gen_204")) == 0 ||
                   strcmp_P((char *)dat, PSTR("/popup")) == 0) {
          client->route = 80; //for Android/Windows devices
        } else if (strcmp_P((char *)dat, PSTR("/hotspot-detect.html")) == 0 ) {
          client->route = 81;  //for Apple devices
        } else if (strcmp_P((char *)dat, PSTR("/factoryreset")) == 0) {
          client->route = 90;
        } else if (strcmp_P((char *)dat, PSTR("/command")) == 0) {
          if ((client->userdata = malloc(1)) == NULL) {
            loggingSerial.printf(PSTR("Out of memory %s:#%d\n"), __FUNCTION__, __LINE__);
            ESP.restart();
            exit(-1);
          }
          ((char *)client->userdata)[0] = 0;
          client->route = 100;
        } else if (client->route == 110) {
          // Only accept settings POST requests
          if (strcmp_P((char *)dat, PSTR("/savesettings")) == 0) {
            client->route = 110;
          } else if (strcmp_P((char *)dat, PSTR("/saverules")) == 0) {
            client->route = 170;
            if (LittleFS.begin()) {
              LittleFS.remove("/rules.new");
              client->userdata = new File(LittleFS.open("/rules.new", "a+"));
            }
#ifdef TLS_SUPPORT
        } else if (strcmp_P((char *)dat, PSTR("/cacert")) == 0) {
          client->route = 165; 
          if (LittleFS.begin()) {
            LittleFS.remove("/ca.tmp");
            File cf = LittleFS.open("/ca.tmp", "w");
            if (cf) {
              client->userdata = new File(std::move(cf));
            }
            new_ca_stored = true;
          }
#endif
          } else if (strcmp_P((char *)dat, PSTR("/firmware")) == 0) {
            if (!Update.isRunning()) {
              if (!Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000)) {
                Update.printError(loggingSerial);
                return -1;
              } else {
                client->route = 150;
              }
            } else {
              loggingSerial.println(PSTR("New firmware update client, while previous isn't finished yet! Assume broken connection, abort!"));
              Update.end();
              return -1;
            }
          } else {
            return -1;
          }
        } else if (strcmp_P((char *)dat, PSTR("/settings")) == 0) {
          client->route = 120;
        } else if (strcmp_P((char *)dat, PSTR("/getsettings")) == 0) {
          client->route = 130;
        } else if (strcmp_P((char *)dat, PSTR("/firmware")) == 0) {
          client->route = 140;
        } else if (strcmp_P((char *)dat, PSTR("/rules")) == 0) {
          client->route = 160;
#ifdef TLS_SUPPORT
        } else if (strcmp_P((char *)dat, PSTR("/cacert")) == 0) {
          client->route = 166; 
#endif
        } else if (strcmp_P((char *)dat, PSTR("/scandallas")) == 0) {
          client->route = 180;          
        } else {
          client->route = 0;
        }

        return 0;
      } break;
    case WEBSERVER_CLIENT_ARGS: {
        struct arguments_t *args = (struct arguments_t *)dat;
        switch (client->route) {
          case 60: {
              sprintf_P(log_msg, PSTR("Dallas alias changed address %s to alias %s"), args->name, args->value);
              log_message(log_msg);
              changeDallasAlias((char *)args->name, (char *)args->value);
              return 0;
            } break;
          case 100: {
              unsigned char cmd[256] = { 0 };
              char cpy[args->len + 1];
              char log_msg[256] = { 0 };
              unsigned int len = 0;

              memset(&cpy, 0, args->len + 1);
              snprintf((char *)&cpy, args->len + 1, "%.*s", args->len, args->value);

              for (uint8_t x = 0; x < sizeof(commands) / sizeof(commands[0]); x++) {
                cmdStruct tmp;
                memcpy_P(&tmp, &commands[x], sizeof(tmp));
                if (strcmp((char *)args->name, tmp.name) == 0) {
                  len = tmp.func(cpy, cmd, log_msg);
                  if ((client->userdata = realloc(client->userdata, strlen((char *)client->userdata) + strlen(log_msg) + 2)) == NULL) {
                    loggingSerial.printf(PSTR("Out of memory %s:#%d\n"), __FUNCTION__, __LINE__);
                    ESP.restart();
                    exit(-1);
                  }
                  strcat((char *)client->userdata, log_msg);
                  strcat((char *)client->userdata, "\n");
                  log_message(log_msg);
                  send_command(cmd, len);
                }
              }

              memset(&cmd, 0, 256);
              memset(&log_msg, 0, 256);

              if (heishamonSettings.optionalPCB) {
                //optional commands
                for (uint8_t x = 0; x < sizeof(optionalCommands) / sizeof(optionalCommands[0]); x++) {
                  optCmdStruct tmp;
                  memcpy_P(&tmp, &optionalCommands[x], sizeof(tmp));
                  if (strcmp((char *)args->name, tmp.name) == 0) {
                    len = tmp.func(cpy, log_msg);
                    if ((client->userdata = realloc(client->userdata, strlen((char *)client->userdata) + strlen(log_msg) + 2)) == NULL) {
                      loggingSerial.printf(PSTR("Out of memory %s:#%d\n"), __FUNCTION__, __LINE__);
                      ESP.restart();
                      exit(-1);
                    }
                    strcat((char *)client->userdata, log_msg);
                    strcat((char *)client->userdata, "\n");
                    log_message(log_msg);
#ifdef ESP32
                    xQueueOverwrite(pcbQueue, optionalPCBQuery);
#endif
                  }
                }
              }
            } break;
          case 110: {
              return cacheSettings(client, args);
            } break;
          case 150: {
              if (Update.isRunning() && (!Update.hasError())) {
                if ((strcmp((char *)args->name, "md5") == 0) && (args->len > 0)) {
                  char md5[args->len + 1];
                  memset(&md5, 0, args->len + 1);
                  snprintf((char *)&md5, args->len + 1, "%.*s", args->len, args->value);
                  sprintf_P(log_msg, PSTR("Firmware MD5 expected: %s"), md5);
                  log_message(log_msg);
                  if (!Update.setMD5(md5)) {
                    log_message(_F("Failed to set expected update file MD5!"));
                    Update.end(false);
                  }
                } else if (strcmp((char *)args->name, "firmware") == 0) {
                  if (Update.write((uint8_t *)args->value, args->len) != args->len) {
                    Update.printError(loggingSerial);
                    Update.end(false);
                  } else {
                    if (uploadpercentage != (unsigned int)(((float)client->readlen / (float)client->totallen) * 20)) {
                      uploadpercentage = (unsigned int)(((float)client->readlen / (float)client->totallen) * 20);
                      sprintf_P(log_msg, PSTR("Uploading new firmware: %d%%"), uploadpercentage * 5);
                      log_message(log_msg);
                    }
                  }
                }
              } else {
                log_message((char*)"New firmware POST data but update not running anymore!");
              }
            } break;
          case 170: {
              File *f = (File *)client->userdata;
              if (!f || !*f) {
                client->route = 160;
              } else {
                f->write(args->value, args->len);
              }
            } break;
#ifdef TLS_SUPPORT            
          case 165: {
              File *f = (File *)client->userdata;
              if (f && *f && args->len > 0) {
                  f->write((const uint8_t*)args->value, (size_t)args->len);
              }
              return 0;
            } break;
#endif
        }
      } break;
    case WEBSERVER_CLIENT_HEADER: {
        struct arguments_t *args = (struct arguments_t *)dat;
        return 0;
      } break;
    case WEBSERVER_CLIENT_WRITE: {
        switch (client->route) {
          case 0: {
              if (client->content == 0) {
                webserver_send(client, 404, (char *)"text/plain", 13);
                webserver_send_content_P(client, PSTR("404 Not found"), 13);
              }
              return 0;
            } break;
          case 1: {
              return handleRoot(client, readpercentage, mqttReconnects, &heishamonSettings);
            } break;
          case 20: {
              return handleJsonOutput(client, actData, actDataExtra, actOptData, &heishamonSettings, extraDataBlockAvailable);
            } break;
          case 30: {
              return handleReboot(client);
            } break;
          case 40: {
              if (client->content == 0) {
                webserver_send(client, 200, (char *)"text/plain", 0);
              } else if (client->content == 1) {
                webserver_send_content_P(client, PSTR("-- heatpump data --\n"), 20);
                handleDebug(client, (char *)actData, 203);
              } else if ((client->content == 2) && extraDataBlockAvailable) {
                webserver_send_content_P(client, PSTR("-- extra data --\n"), 17);
                handleDebug(client, (char *)actDataExtra, 203);
              }
              return 0;
            } break;
          case 50: {
              return handleWifiScan(client);
            } break;
          case 60: {
              return 0;
            } break;
          case 80: {
              if (client->content == 0) {
                webserver_send(client, 302, (char *)"text/html", 0);
              }
              return 0;
            } break;
          case 81: {
              if (client->content == 0) {
                static const char body[] PROGMEM =
                  "<HTML><HEAD><TITLE>HeishaMon Setup</TITLE>"
                  "<META name='viewport' content='width=device-width,initial-scale=1'>"
                  "</HEAD><BODY>"
                  "<h2>HeishaMon Setup</h2>"
                  "<p><a href='http://192.168.4.1/settings'>Open Settings</a></p>"
                  "</BODY></HTML>";
                webserver_send(client, 200, (char *)"text/html", strlen(body));
                webserver_send_content_P(client, body, strlen(body));
              }
              return 0;
            } break;            
          case 90: {
              return handleFactoryReset(client);
            } break;
          case 100: {
              if (client->content == 0) {
                webserver_send(client, 200, (char *)"text/plain", 0);
                char *RESTmsg = (char *)client->userdata;
                webserver_send_content(client, (char *)RESTmsg, strlen(RESTmsg));
                free(RESTmsg);
                client->userdata = NULL;
              }
              return 0;
            } break;
          case 110: {
              int ret = saveSettings(client, &heishamonSettings);
              if (heishamonSettings.listenonly) {
                digitalWrite(ENABLEPIN, LOW);
              } else {
                digitalWrite(ENABLEPIN, HIGH);
              }
              if (!heishamonSettings.opentherm) {
                digitalWrite(ENABLEOTPIN, LOW);
              } else {
                digitalWrite(ENABLEOTPIN, HIGH);
              }
              switch (client->route) {
                case 111: {
                    return settingsNewPassword(client, &heishamonSettings);
                  } break;
                case 112: {
                    return settingsReconnectWifi(client, &heishamonSettings);
                  } break;
                case 113: {
                    webserver_send(client, 301, (char *)"text/plain", 0);
                  } break;
              }
              return 0;
            } break;
          case 111: {
              return settingsNewPassword(client, &heishamonSettings);
            } break;
          case 112: {
              return settingsReconnectWifi(client, &heishamonSettings);
            } break;
          case 120: {
              return handleSettings(client);
            } break;
          case 130: {
              return getSettings(client, &heishamonSettings);
            } break;
          case 140: {
              return showFirmware(client);
            } break;
          case 150: {
              log_message((char*)"In /firmware client write part");
              if (Update.isRunning()) {
                if (Update.end(true)) {
                  log_message((char*)"Firmware update success");
                  timerqueue_insert(2, 0, -2); // Start reboot sequence
                  return showFirmwareSuccess(client);
                } else {
                  Update.printError(loggingSerial);
                  return showFirmwareFail(client);
                }
              }
              return 0;
            } break;
          case 160: {
              return showRules(client);
            } break;
#ifdef TLS_SUPPORT
        case 165: {
          if (client->userdata) {
            File *pf = (File *)client->userdata;
            pf->close();
            delete pf;
            client->userdata = NULL;
          }
          return handleCACert(client);
        } break;
        case 166: {
          return showCACert(client);
        } break; 
#endif  
          case 170: {
              File *f = (File *)client->userdata;
              if (f) {
                if (*f) {
                  f->close();
                }
                delete f;
              }
              client->userdata = NULL;
              timerqueue_insert(0, 1, -4);
              webserver_send(client, 301, (char *)"text/plain", 0);

            } break;
          case 180: {
              if (heishamonSettings.use_1wire) initDallasSensors(log_message, heishamonSettings.updataAllDallasTime, heishamonSettings.waitDallasTime, heishamonSettings.dallasResolution);
              webserver_send(client, 200, (char *)"text/plain", 0);
            } break;            
          default: {
              webserver_send(client, 301, (char *)"text/plain", 0);
            } break;
        }
        return -1;
      } break;
    case WEBSERVER_CLIENT_CREATE_HEADER: {
        struct header_t *header = (struct header_t *)dat;
        switch (client->route) {
          case 113: {
              header->ptr += sprintf_P((char *)header->buffer, PSTR("Location: /settings"));
              return -1;
            } break;
          case 60:
          case 70: {
              header->ptr += sprintf_P((char *)header->buffer, PSTR("Location: /"));
              return -1;
            } break;
          case 80: {
              header->ptr += sprintf_P((char *)header->buffer, 
              PSTR("Location: http://192.168.4.1/settings"));
              return -1;
            } break;            
          case 170: {
              header->ptr += sprintf_P((char *)header->buffer, PSTR("Location: /rules"));
              return -1;
            } break;
          default: {
              if (client->route != 0) {
                header->ptr += sprintf_P((char *)header->buffer, PSTR("Access-Control-Allow-Origin: *"));
              }
            } break;
        }
        return 0;
      } break;
    case WEBSERVER_CLIENT_CLOSE: {
        switch (client->route) {
          case 100: {
              if (client->userdata != NULL) {
                free(client->userdata);
              }
            } break;
          case 110: {
              struct websettings_t *tmp = NULL;
              while (client->userdata) {
                tmp = (struct websettings_t *)client->userdata;
                client->userdata = ((struct websettings_t *)(client->userdata))->next;
                free(tmp);
              }
            } break;
          case 160:
#ifdef TLS_SUPPORT
          case 165:
#endif
          case 170: {
              if (client->userdata != NULL) {
                File *f = (File *)client->userdata;
                if (f) {
                  if (*f) {
                    f->close();
                  }
                  delete f;
                }
              }
            } break;
        }
        client->userdata = NULL;
      } break;
      default: {
        return 0;
      } break;
  }

  return 0;
}

/*
 * Starts the embedded HTTP web server on port 80.
 * Mechanism: Calls webserver_start() with port 80, the webserver_cb handler, and no TLS.
 * Thread-safety: Called once during setup. Single-threaded.
 * Data flow: Starts the web server (side-effect on network stack).
 */
void setupHttp() {
  webserver_start(80, &webserver_cb, 0);
}

/*
 * Performs a factory reset: formats LittleFS, clears WiFi credentials, and enters an infinite
 * LED blink loop.
 * Mechanism: Formats the LittleFS filesystem (erases all settings/rules/certs), creates a fresh
 * /heishamon boot marker, disconnects WiFi with persistent=false, then blinks the NeoPixel
 * red↔blue forever. The device must be power-cycled afterwards.
 * Thread-safety: Called from loop() on core 0. Not reentrant; never returns.
 * Data flow: Writes LittleFS (format, create file). Writes WiFi (disconnect). Writes pixels.
 */
void factoryReset() {
    loggingSerial.println("Factory reset request detected, clearing config."); 
    LittleFS.format();
    //create first boot file
    File startupFile = LittleFS.open("/heishamon", "w");
    startupFile.close();
    WiFi.persistent(true);
    WiFi.disconnect();
    WiFi.persistent(false);
    loggingSerial.println("Config cleared. Please reset to configure this device...");
    //initiate debug led indication for factory reset
    while (true) {
     delay(100);
     pixels.setPixelColor(0, 128, 0, 0);
     pixels.show();
     delay(100);
     pixels.setPixelColor(0, 0, 0, 128);
     pixels.show();
    }
}
/*
 * Detects a double-reset condition: if /doublereset exists on boot, triggers factoryReset().
 * Mechanism: On boot, checks for the file /doublereset. If present, a previous boot did not
 * complete normally (or the user reset twice quickly), so factoryReset is called. Otherwise,
 * creates /doublereset as a marker that will be detected on the next boot.
 * Thread-safety: Called once early in setup. Single-threaded.
 * Data flow: Reads/writes LittleFS.
 */
void doubleResetDetect() {
  if (LittleFS.exists("/doublereset")) {
    factoryReset();
  }
  File doubleresetFile = LittleFS.open("/doublereset", "w");
  doubleresetFile.close();
}

/*
 * Initializes the logging serial port (USB CDC at 115200 baud) and NeoPixel LED.
 * Mechanism: Starts loggingSerial at 115200, prints the version banner, initializes the NeoPixel
 * on LEDPIN, sets initial red color and calls show().
 * Thread-safety: Called once during setup. Single-threaded.
 * Data flow: Writes to loggingSerial, pixels.
 */
void setupSerial() {
  if (heishamonSettings.logSerial1) { //settings are not loaded yet, this is the startup default
    loggingSerial.begin(115200);
#ifdef ESP32
    delay(100); //to let USB CDC to be opened if necessary
#endif    
    loggingSerial.print(F("HeishaMon version: "));
    loggingSerial.println(heishamon_version);
  }
  loggingSerial.print(F("  NeoPixel..."));
  pixels.begin();
  pixels.clear();
  pixels.setPixelColor(0, 16, 0, 0);
  pixels.show();
  loggingSerial.println(F("OK"));
}

/*
 * Configures the heatpump and proxy UARTs (9600 8E1) and sets up GPIO pins.
 * Mechanism: Flushes and reconfigures heatpumpSerial (RX=18, TX=17) and proxySerial (RX=9, TX=8)
 * to 9600 baud, 8 data bits, even parity, 1 stop bit. Calls setupGPIO, sets ENABLEPIN high (TX
 * enabled) unless CZ-TAW1 is detected on the heatpump bus (then forces listen-only). Also
 * configures ENABLEOTPIN for OpenTherm.
 * Thread-safety: Called once during setup. Single-threaded.
 * Data flow: Writes heatpumpSerial, proxySerial, GPIO pins (ENABLEPIN, ENABLEOTPIN). Reads
 * heishamonSettings.listenonly. May set heishamonSettings.listenonly = true if CZ-TAW1 detected.
 */
void switchSerial() {
  loggingSerial.print(F("  heatpumpSerial(9600 8E1 RX="));
  loggingSerial.print(HEATPUMPRX);
  loggingSerial.print(F(" TX="));
  loggingSerial.print(HEATPUMPTX);
  loggingSerial.print(F(")..."));
  heatpumpSerial.flush();
  heatpumpSerial.end();
  heatpumpSerial.begin(9600, SERIAL_8E1,HEATPUMPRX,HEATPUMPTX);
  heatpumpSerial.flush();
  loggingSerial.println(F("OK"));

  loggingSerial.print(F("  proxySerial(9600 8E1 RX="));
  loggingSerial.print(PROXYRX);
  loggingSerial.print(F(" TX="));
  loggingSerial.print(PROXYTX);
  loggingSerial.print(F(")..."));
  proxySerial.flush();
  proxySerial.end();
  proxySerial.begin(9600, SERIAL_8E1,PROXYRX,PROXYTX);
  proxySerial.flush();
  loggingSerial.println(F("OK"));

  loggingSerial.print(F("  GPIO setup..."));
  setupGPIO(heishamonSettings.gpioSettings);
  pinMode(ENABLEPIN, OUTPUT);
  #if defined (ESP32)
  pinMode(ENABLEOTPIN, OUTPUT);
  digitalWrite(ENABLEOTPIN, LOW);
  #endif
  loggingSerial.println(F("OK"));

  if (!heishamonSettings.listenonly) {
    if (heatpumpSerial.available() > 0) {
      loggingSerial.println(F("  CZ-TAW1 detected, listen-only mode"));
      heishamonSettings.listenonly = true;
    }
    else {
      digitalWrite(ENABLEPIN, HIGH);
      loggingSerial.println(F("  TX enabled"));
    }
  } else {
    loggingSerial.println(F("  listen-only mode (config)"));
  }
}

/*
 * Configures the MQTT client: buffer, server, TLS (if enabled), and callback registration.
 * Mechanism: Sets the PubSubClient buffer to 1024 bytes. On ESP32 with TLS_SUPPORT, loads the CA
 * certificate from LittleFS /ca.pem if mqtt_tls_enabled is set, and attaches a WiFiClientSecure.
 * Sets server address/port and registers mqtt_callback as the subscription handler.
 * Thread-safety: Called once during setup. Single-threaded.
 * Data flow: Reads heishamonSettings (mqtt_server, mqtt_port, mqtt_tls_enabled, etc.). Writes
 * mqtt_client config. Reads LittleFS /ca.pem for TLS.
 */
void setupMqtt() {
  mqtt_client.setBufferSize(1024);
#ifdef TLS_SUPPORT
  mqtt_client.setSocketTimeout(8); mqtt_client.setKeepAlive(30);
  if (heishamonSettings.mqtt_tls_enabled) {
    if (mqtt_tls_client == nullptr) {
        mqtt_tls_client = new WiFiClientSecure();
    }
    if (!loadTlsCaFromFS(mqtt_tls_client)) {
      log_message(_F("[TLS] Proceeding without valid CA (expect failure)"));
    }
    mqtt_client.setClient(*mqtt_tls_client );
  } else {
    mqtt_client.setClient(mqtt_wifi_client);
  }
  last_tls_enabled = heishamonSettings.mqtt_tls_enabled;
  loggingSerial.print(F("TLS="));
  loggingSerial.print(heishamonSettings.mqtt_tls_enabled);
#else
  mqtt_client.setSocketTimeout(10); mqtt_client.setKeepAlive(5);
#endif
  mqtt_client.setServer(heishamonSettings.mqtt_server, atoi(heishamonSettings.mqtt_port));
  mqtt_client.setCallback(mqtt_callback);
  loggingSerial.printf(" server=%s port=%s", heishamonSettings.mqtt_server, heishamonSettings.mqtt_port);
}

/*
 * Creates FreeRTOS queues and tasks, and initializes optional hardware modules.
 * Mechanism: 
 *   - Creates pcbQueue (depth 1), cmdQueue (MAXCOMMANDSINBUFFER), logQueue (depth 4), 
 *     mqttPublishQueue (MQTT_PUBLISH_QUEUE_LEN).
 *   - Spawns pinned tasks on core 1: serialTXTask (8KB stack), mqttTask (6KB).
 *   - Optionally spawns dallasTask (4KB), s0Task (4KB), otTask (4KB) based on settings.
 *   - If optionalPCB is enabled, tries to load saved PCB data from flash into pcbQueue.
 *   - If use_1wire, calls initDallasSensors.
 *   - If use_s0, calls initS0Sensors.
 * Thread-safety: Called once during setup on core 0. All queue and task creation is single-threaded.
 * Data flow: Reads heishamonSettings (use_1wire, use_s0, opentherm, optionalPCB). Writes to
 * FreeRTOS queues and creates tasks. Reads LittleFS for optional PCB data.
 */
void setupConditionals() {

#ifdef ESP32
  loggingSerial.print(F("  Queues..."));
  pcbQueue = xQueueCreate(1, OPTIONALPCBQUERYSIZE);
  cmdQueue = xQueueCreate(MAXCOMMANDSINBUFFER, sizeof(cmdbuffer_t));
  logQueue = xQueueCreate(4, LOG_MSG_SIZE);
  mqttPublishQueue = xQueueCreate(MQTT_PUBLISH_QUEUE_LEN, sizeof(mqttPublishMsg_t));
  loggingSerial.println(F("OK"));

  loggingSerial.print(F("  Tasks..."));
  xTaskCreatePinnedToCore(
    serialTXTask,
    "serialTXTask",
    8192,
    NULL,
    1,
    NULL,
    1
  ); // serialTXTask: manages periodic heatpump queries, optional PCB queries, and user commands via serial
  loggingSerial.print(F("serialTXTask "));
  xTaskCreatePinnedToCore(
    mqttTask,
    "mqttTask",
    6144,
    NULL,
    1,
    NULL,
    1
  ); // mqttTask: keeps MQTT connection alive, drains the publish queue
  loggingSerial.print(F("mqttTask "));
  if (heishamonSettings.use_1wire) {
    xTaskCreatePinnedToCore(
      dallasTask,
      "dallasTask",
     4096,
      NULL,
      1,
      NULL,
      1
    ); // dallasTask: periodically reads Dallas 1-Wire temperature sensors and publishes values
    loggingSerial.print(F("dallasTask "));
  }
  if (heishamonSettings.use_s0) {
    xTaskCreatePinnedToCore(
      s0Task,
      "s0Task",
      4096,
      NULL,
      1,
      NULL,
      1
    ); // s0Task: periodically reads S0 pulse counter inputs and publishes energy values
    loggingSerial.print(F("s0Task "));
  }
  if (heishamonSettings.opentherm) {
    xTaskCreatePinnedToCore(
      otTask,
      "otTask",
      4096,
      NULL,
      1,
      NULL,
      1
    ); // otTask: runs the OpenTherm protocol state machine and publishes OT values
    loggingSerial.print(F("otTask "));
  }
  loggingSerial.println(F("OK"));
#endif

  //send_initial_query(); //maybe necessary but for now disable. CZ-TAW1 sends this query on boot

  if (heishamonSettings.optionalPCB) {
    loggingSerial.print(F("  Optional PCB..."));
    if (loadOptionalPCB(optionalPCBQuery, OPTIONALPCBQUERYSIZE)) {
      log_message(_F("Loaded optional PCB data from flash"));
    }
    else {
      log_message(_F("Failed to load optional PCB data from flash!"));
    }
#ifdef ESP32
    xQueueOverwrite(pcbQueue, optionalPCBQuery);
#endif
    loggingSerial.println(F("OK"));
  }

  if (heishamonSettings.use_1wire) {
    loggingSerial.print(F("  Dallas 1-wire..."));
    initDallasSensors(log_message, heishamonSettings.updataAllDallasTime, heishamonSettings.waitDallasTime, heishamonSettings.dallasResolution);
    loggingSerial.println(F("OK"));
  }
  if (heishamonSettings.use_s0) {
    loggingSerial.print(F("  S0 counters..."));
    initS0Sensors(heishamonSettings.s0Settings);
    loggingSerial.println(F("OK"));
  }


}

/*
 * Timer callback for scheduled/deferred operations (called from timerqueue_update in loop()).
 * Mechanism: Positive timer IDs route to rules_timer_cb (user-defined rules engine). Negative
 * timer IDs handle internal operations:
 *   -1  → format LittleFS, disconnect WiFi, schedule reboot
 *   -2  → ESP.restart()
 *   -3  → (re)configure WiFi
 *   -4  → parse /rules.new, apply or revert rules
 *   -5  → resync NTP, reschedule in 24h
 *   -6  → retry NTP sync every 5 min until success
 * Thread-safety: Called from timerqueue_update in loop() on core 0. Single-threaded.
 * Data flow: Reads LittleFS for rules/config. Writes LittleFS (format, rename). Calls ESP.restart,
 * setupWifi, ntpReload, rules_parse, rules_boot.
 */
void timer_cb(int nr) {
  if (nr > 0) {
    rules_timer_cb(nr);
  } else {
    switch (nr) {
      case -1: {
          LittleFS.begin();
          LittleFS.format();
          //create first boot file
          File startupFile = LittleFS.open("/heishamon", "w");
          startupFile.close(); 
          WiFi.disconnect(true);
          timerqueue_insert(1, 0, -2);
        } break;
      case -2: {
          ESP.restart();
        } break;
      case -3: {
          setupWifi(&heishamonSettings);
        } break;
      case -4: {
          int ret = rules_parse((char*)"/rules.new");
          if (ret == -2) {
            //we received an empty rules.new file which means delete all rules
            LittleFS.remove("/rules.txt");
            LittleFS.remove("/rules.new");
            rules_deinitialize();
          } else if (ret == -1) {
            log_message(_F("Failed to load new rules, reverting back to older rules!"));
            rules_parse((char*)"/rules.txt");
          } else {
            if (LittleFS.begin()) {
              LittleFS.rename("/rules.new", "/rules.txt");
            }
          }
          rules_boot();
        } break;
      case -5: {
          ntpReload(&heishamonSettings);
          logprintln_P(F("Resynced with NTP servers. Next sync after 24 hours."));
          timerqueue_insert(86400, 0, -5);
        } break;
      case -6: {
          time_t now = time(NULL);
          struct tm *tm_struct = localtime(&now);
          if(tm_struct->tm_year == 70) {
            /*
             * No valid time yet since reboot. Retry every 5 min
             */
            ntpReload(&heishamonSettings);
            logprintln_P(F("Still trying to sync with ntp servers. Checking again in 5 minutes"));
            timerqueue_insert(300, 0, -6);
          } else {
            /*
             * Wait 300 sec less than a full day
             */
            logprintln_P(F("Successfully synced with ntp servers. Next sync after 24 hours."));
            timerqueue_insert(86100, 0, -5);
          }
        } break;
    }
  }

}


/*
 * Arduino setup() — initializes all hardware and software subsystems, then enters the main loop.
 * Mechanism: Sequential initialization in this order:
 *   1. Memory diagnostics, uptime tracking
 *   2. Serial (logging + NeoPixel)
 *   3. LittleFS filesystem — detect first boot / normal boot / migration
 *   4. Load settings from config.json
 *   5. WiFi station/AP mode
 *   6. Ethernet (W5500, optional)
 *   7. HTTP web server
 *   8. MQTT client configuration
 *   9. Switch serial ports to 9600 8E1 (heatpump + proxy)
 *   10. Conditional modules: queues, tasks, optional PCB, 1-wire, S0
 *   11. DNS captive portal
 *   12. OpenTherm (if enabled)
 *   13. Rules engine
 *   14. Final: turn off NeoPixel, clear inSetup flag
 * Thread-safety: Called once by the Arduino framework on core 0. Fully single-threaded.
 * Data flow: Writes to all subsystems. Reads LittleFS for config and boot markers.
 */
void setup() {
  //first get total memory before we do anything
  getFreeMemory();
  //set boottime
  char *up = getUptime();
  free(up);

  inSetup = true;

  loggingSerial.println();
  loggingSerial.println(F("--- HEISHAMON ---"));
  loggingSerial.println(F("starting..."));

  loggingSerial.print(F("Starting serial..."));
  setupSerial();
  loggingSerial.println(F("OK"));

#if defined(ESP32)
  loggingSerial.printf("ESP32 PSRAM: %s, size: %u bytes, free: %u bytes\n",
                       psramFound() ? "yes" : "no",
                       ESP.getPsramSize(),
                       ESP.getFreePsram());
#endif

  loggingSerial.print(F("Starting LittleFS..."));
  if (LittleFS.begin(true)) {
    if (LittleFS.exists("/heishamon")) {
      loggingSerial.println(F("OK (normal boot)"));
    } else if (LittleFS.exists("/config.json")) {
      File startupFile = LittleFS.open("/heishamon", "w");
      startupFile.close();
      loggingSerial.println(F("OK (migrated config)"));
    } else {
      loggingSerial.println(F("FIRST BOOT - creating boot file"));
      File startupFile = LittleFS.open("/heishamon", "w");
      startupFile.close();
      while (true) {
        delay(50);
        pixels.setPixelColor(0, 128, 0, 0);
        pixels.show();
        delay(50);
        pixels.setPixelColor(0, 0, 0, 128);
        pixels.show();
      }
    }
  } else {
    loggingSerial.println(F("FAIL"));
  }

  loggingSerial.print(F("Starting boot pin..."));
  pinMode(BOOTPIN,INPUT_PULLUP);
  loggingSerial.println(F("OK"));

  loggingSerial.print(F("Starting WiFi diag..."));
  WiFi.printDiag(loggingSerial);
  loggingSerial.println(F("OK"));

  loggingSerial.print(F("Starting config load..."));
  loadSettings(&heishamonSettings);
  loggingSerial.println(F("OK"));

  loggingSerial.print(F("Starting WiFi..."));
  setupWifi(&heishamonSettings);
  lastWifiRetryTimer = millis();
  loggingSerial.println(F("OK"));

#if defined(ESP32)
  loggingSerial.print(F("Starting Ethernet..."));
  setupETH();
  loggingSerial.println(F("OK"));
#endif

  loggingSerial.print(F("Starting HTTP..."));
  setupHttp();
  loggingSerial.println(F("OK"));

  loggingSerial.print(F("Starting MQTT..."));
  setupMqtt();
  loggingSerial.println(F("OK"));

  loggingSerial.print(F("Starting serial switch..."));
  switchSerial();
  loggingSerial.println(F("OK"));

  loggingSerial.printf("WiFi mode: %d, AP stations: %d\n", WiFi.getMode(), WiFi.softAPgetStationNum());

  loggingSerial.print(F("Starting conditionals..."));
  setupConditionals();
  loggingSerial.println(F("OK"));

  loggingSerial.print(F("Starting DNS..."));
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", apIP);
  loggingSerial.println(F("OK"));

  if (heishamonSettings.opentherm) {
    loggingSerial.print(F("Starting OpenTherm..."));
    digitalWrite(ENABLEOTPIN, HIGH);
    HeishaOTSetup();
    loggingSerial.println(F("OK"));
  }

  loggingSerial.print(F("Starting rules..."));
  if (heishamonSettings.force_rules == false) {
      esp_reset_reason_t reset_reason = esp_reset_reason();
      loggingSerial.printf("Reset reason: %d\n", reset_reason);
    if (reset_reason > 3 && reset_reason < 12) {
        loggingSerial.println("Skipping rules due to crash reboot");
    } else {
      rules_parse((char *)"/rules.txt");
      rules_boot();
      loggingSerial.println(F("OK"));
    }
  } else {
    rules_parse((char *)"/rules.txt");
    rules_boot();
    loggingSerial.println(F("OK"));
  }

  delay(200);
  #ifdef ESP32
  neoPixelState = pixels.Color(0,0,0);
  pixels.setPixelColor(0, neoPixelState);
  pixels.show();
  #endif

  inSetup = false;
  loggingSerial.println(F("--- SETUP COMPLETE ---"));
}

/*
 * Sends the initial startup query to the heatpump to request data.
 * Mechanism: Calls send_command() with initialQuery (defined in commands.h). Currently disabled
 * in setupConditionals because the CZ-TAW1 already sends this query on boot.
 * Thread-safety: Thread-safe via send_command (cmdQueue).
 * Data flow: Reads initialQuery (PROGMEM). Writes to cmdQueue.
 */
void send_initial_query() {
  log_message(_F("Requesting initial start query"));
  send_command(initialQuery, INITIALQUERYSIZE);

}

/*
 * Sends the periodic data query to the heatpump, including the extra data block if available.
 * Mechanism: Calls send_command with panasonicQuery (main 0x10 data block). If
 * extraDataBlockAvailable is true, also sends a modified query with byte 3 = 0x21 to request the
 * extended data block (K/L series and newer), then restores byte 3 to 0x10.
 * This function is kept for manual use but the periodic query is now handled by serialTXTask.
 * Thread-safety: Thread-safe via send_command (cmdQueue). extraDataBlockAvailable is volatile and
 * written by readSerial on core 0 — read here potentially from a different task context.
 * Data flow: Reads panasonicQuery, extraDataBlockAvailable. Writes to cmdQueue.
 */
void send_panasonic_query() {
  log_message(_F("Requesting new panasonic data"));
  send_command(panasonicQuery, PANASONICQUERYSIZE);
  // rest is for the new data block on new models
  if (extraDataBlockAvailable) {
    log_message(_F("Requesting new panasonic extra data"));
    panasonicQuery[3] = 0x21; //setting 4th byte to 0x21 is a request for extra block
    send_command(panasonicQuery, PANASONICQUERYSIZE);
    panasonicQuery[3] = 0x10; //setting 4th back to 0x10 for normal data request next time
  }
}

/*
 * Checks for serial read timeout and reads available heatpump serial data.
 * Mechanism: If sending has been true for longer than SERIALTIMEOUT (2s) without receiving a
 * complete frame, declares a timeout: increments timeout counter, clears data_length and sending
 * flag so the next query can proceed. Then, if in listen-only mode OR sending is active and serial
 * data is available, calls readSerial() to process incoming bytes.
 * Thread-safety: Called from loop() on core 0. Accesses sending (volatile, shared with
 * serialTXTask on core 1), sendCommandReadTime (volatile), data_length (shared with readSerial),
 * heatpumpSerial. No mutex — relies on volatile for visibility of sending/sendCommandReadTime.
 * Data flow: Reads sending, sendCommandReadTime, heatpumpSerial. Writes data_length, sending, stat
 * counters (timeoutread, totalreads, tooshortread). Calls readSerial.
 */
void readHeatpump() {
  if (sending && ((unsigned long)(millis() - sendCommandReadTime) > SERIALTIMEOUT)) {
    log_message(_F("Previous read data attempt failed due to timeout!"));
    sprintf_P(log_msg, PSTR("Received %d bytes data"), data_length);
    log_message(log_msg);
    if (heishamonSettings.logHexdump) logHex(data, data_length);
    if (data_length == 0) {
      timeoutread++;
      totalreads++; //at at timeout we didn't receive anything but did expect it so need to increase this for the stats
    } else {
      tooshortread++;
    }
    data_length = 0; //clear any data in array
    sending = false; //receiving the answer from the send command timed out, so we are allowed to send a new command
  }
  if ( (heishamonSettings.listenonly || sending) && (heatpumpSerial.available() > 0)) readSerial();
}

/*
 * Monitors the boot button (BOOTPIN, GPIO 0) for a long press (>10s) to trigger factory reset.
 * Mechanism: If the button is NOT pressed (pin high, pulled up), continuously updates
 * bootButtonNotPressed to the current millis(). If the button IS pressed (pin low) for more than
 * 10 seconds, calls factoryReset().
 * Thread-safety: Called from loop() on core 0. Single-threaded. bootButtonNotPressed is only
 * accessed here.
 * Data flow: Reads BOOTPIN GPIO. Writes bootButtonNotPressed. Calls factoryReset on long press.
 */
void checkBootButton() {
  if (digitalRead(BOOTPIN)) { //true = 1, not pressed
    bootButtonNotPressed = millis();
  } else {
      if ((unsigned long)(millis() - bootButtonNotPressed) > 10000) {
        //initiate factory reset
        factoryReset();
      }
  }
}

/*
 * Arduino loop() — main supervisory loop running on core 0.
 * Mechanism: Each iteration performs:
 *   1. Check boot button for long-press factory reset
 *   2. Web server loop (process HTTP requests)
 *   3. WiFi/Ethernet connectivity management (check_wifi)
 *   4. OTA handler
 *   5. Read heatpump serial data (readHeatpump → readSerial)
 *   6. Read proxy serial data (readProxy)
 *   7. Drain logQueue (messages from FreeRTOS tasks)
 *   8. Every waitTime seconds: log stats, publish to MQTT, update websocket, refresh LWT
 *   9. Timer queue processing (timerqueue_update → timer_cb)
 * Thread-safety: Runs on core 0 (Arduino loop task). Shares data with tasks on core 1 via
 * volatile variables (sending, sendCommandReadTime) and FreeRTOS queues (logQueue). Uses delay(1)
 * to feed the watchdog.
 * Data flow: Coordinates all high-level subsystems — reads heatpump/proxy serial, publishes stats,
 * maintains MQTT will message, processes timers. Reads/writes heishamonSettings, actData,
 * proxydata, stat counters, log messages.
 */
void loop() {
  //check boot button state
  checkBootButton();

  //webserver function
  webserver_loop();

  // check wifi
  check_wifi();
  // Handle OTA first.s
  ArduinoOTA.handle();

  readHeatpump();

#ifdef ESP32
  if (heishamonSettings.proxy) readProxy();

  if (logQueue != NULL) {
    if (xQueueReceive(logQueue, log_msg, 0) == pdTRUE) {
      log_message(log_msg);
    }
  }
#endif

  // run the data query only each WAITTIME
  if ((unsigned long)(millis() - lastRunTime) > (1000 * heishamonSettings.waitTime)) {
    lastRunTime = millis();


    //log stats
    if (totalreads > 0 ) readpercentage = (((float)goodreads / (float)totalreads) * 100);
    String message;
    message += F("Heishamon stats: Uptime: ");
    char *up = getUptime();
    message += up;
    free(up);
    message += F(" ## Free memory: ");
    message += getFreeMemory();
    message += F("% ## Free PSRAM: ");
    message += ESP.getFreePsram();
    message += F(" bytes ## Free heap: ");
    message += ESP.getFreeHeap();
    message += F(" bytes ## Wifi: ");
    message += getWifiQuality();
    message += F("% (RSSI: ");
    message += WiFi.RSSI();
#ifdef ESP32
    message += F(") ## Ethernet: ");
    if (ETH.phyAddr() != 0) {        
      if (ETH.connected()) {
        if (ETH.hasIP()) {
          message += F("connected (");
          message += ETH.localIP().toString();
          message += F(")");
        } else {
          message += F("connected (no IP)");
        }
      } 
      else {
        message += F("not connected");
      }
    } else {
      message += F("not installed");
    }
    message += F(" ## Mqtt reconnects: ");
#else
    message += F(") ## Mqtt reconnects: ");
#endif
    message += mqttReconnects;
    message += F(" ## Correct data: ");
    message += readpercentage;
    message += F("% Rules active: ");
    message += nrrules;
    log_message((char*)message.c_str());

    String stats;
    stats += F("{\"uptime\":");
    stats += String(millis());
    stats += F(",\"voltage\":");
    stats += "3.3";
    stats += F(",\"free memory\":");
    stats += getFreeMemory();
    stats += F(",\"free heap\":");
    stats += ESP.getFreeHeap();
    stats += F(",\"wifi\":");
    stats += getWifiQuality();
    stats += F(",\"mqtt reconnects\":");
    stats += mqttReconnects;
    stats += F(",\"total reads\":");
    stats += totalreads;
    stats += F(",\"good reads\":");
    stats += goodreads;
    stats += F(",\"bad crc reads\":");
    stats += badcrcread;
    stats += F(",\"bad header reads\":");
    stats += badheaderread;
    stats += F(",\"too short reads\":");
    stats += tooshortread;
    stats += F(",\"too long reads\":");
    stats += toolongread;
    stats += F(",\"timeout reads\":");
    stats += timeoutread;
    stats += F(",\"version\":\"");
    stats += heishamon_version;
    stats += F("\",\"board\":\"");
    stats += F("ESP32");
    stats += F("\",\"rules active\":");
    stats += nrrules;
    stats += F("}");
    sprintf_P(mqtt_topic, PSTR("%s/stats"), heishamonSettings.mqtt_topic_base);
    mqttPublishQueued(mqtt_topic, stats.c_str(), MQTT_RETAIN_VALUES);

    //websocket stats
#ifdef ESP32
    String ethernetStat;
    if (ETH.phyAddr() != 0) {        
      if (ETH.connected()) {
        if (ETH.hasIP()) {
          ethernetStat = F("connected - IP: ");
          ethernetStat += ETH.localIP().toString();
        } else {
          ethernetStat = F("connected - no IP");
        }
      } 
      else {
        ethernetStat = F("not connected");
      }
    } else {
      ethernetStat = F("not installed");
    }
    char *getuptime = getUptime();
    sprintf_P(log_msg, PSTR("{\"data\": {\"stats\": {\"wifi\": %d, \"ethernet\": \"%s\", \"memory\": %d, \"correct\": %.0f,\"mqtt\": %d,\"rules\": %d,\"uptime\": \"%s\"}}}"), getWifiQuality(), ethernetStat.c_str(), getFreeMemory(), readpercentage, mqttReconnects, nrrules, getuptime);
    free(getuptime);    
#else
    char *getuptime = getUptime();
    sprintf_P(log_msg, PSTR("{\"data\": {\"stats\": {\"wifi\": %d, \"memory\": %d, \"correct\": %.0f,\"mqtt\": %d,\"rules\": %d,\"uptime\": \"%s\"}}}"), getWifiQuality(), getFreeMemory(), readpercentage, mqttReconnects, nrrules, getuptime);    
    free(getuptime);    
#endif
    
    websocket_write_all(log_msg, strlen(log_msg));        

    //Make sure the LWT is set to Online, even if the broker have marked it dead.
    sprintf_P(mqtt_topic, PSTR("%s/%s"), heishamonSettings.mqtt_topic_base, mqtt_willtopic);
    mqttPublishQueued(mqtt_topic, "Online", true);

  }

  timerqueue_update();
  #ifdef ESP32
  delay(1); // to keep watchdog happy
  #endif
}
