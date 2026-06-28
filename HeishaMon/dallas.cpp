#include <OneWire.h>
#include <DallasTemperature.h>
#include <PubSubClient.h>
#include <memory>
#include "commands.h"
#include "dallas.h"
#include "rules.h"
#include "src/common/progmem.h"
#include "mqtt_queue.h"
#include <ArduinoJson.h>
#include <LittleFS.h>

#define MQTT_RETAIN_VALUES 1 // do we retain 1wire values?

#define MAXTEMPDIFFPERSEC 0.5 // what is the allowed temp difference per second which is allowed (to filter bad values)

#define DALLASASYNC 1 //async dallas yes or no (default yes)

// OneWire bus instance for the DS18B20 temperature sensors, driven by the GPIO pin defined as ONE_WIRE_BUS
OneWire oneWire(ONE_WIRE_BUS);
// DallasTemperature library instance wrapping the OneWire bus; provides the high-level API for DS18B20 sensors
DallasTemperature DS18B20(&oneWire);

// Dynamically allocated array of per-sensor data (address, temperature, alias, last-good timestamp)
dallasDataStruct* actDallasData = 0;
// Number of DS18B20 devices detected on the bus, capped at MAX_DALLAS_SENSORS
int dallasDevicecount = 0;


// Timestamp (ms) of the last full poll cycle; used in readNewDallasTemp() to throttle forced MQTT re-publishes
unsigned long lastalldatatime_dallas = 0;

// Primary interval timer for dallasLoop(); reset when a new temperature conversion is requested
unsigned long dallasTimer = 0;
// Secondary timer for async mode: records when requestTemperatures() was issued so we wait ~750 ms before reading
unsigned long dallasTimer1 = 0;
// Minimum interval (seconds) between full MQTT re-publishes of every sensor, even if the temperature is unchanged
unsigned int updateAllDallasTime = 30000; // will be set using heishmonSettings
// Polling interval (seconds) between successive temperature conversion requests
unsigned int dallasTimerWait = 30000; // will be set using heishmonSettings
void loadDallasAlias();

/*
 * initDallasSensors() — Initialises the OneWire / DallasTemperature bus.
 *
 * How it works:
 *  1. Stores the user-supplied timing parameters into the module-level
 *     globals (updateAllDallasTime, dallasTimerWait).
 *  2. Calls DS18B20.begin() to enumerate all devices on the bus.
 *  3. Caps the device count at MAX_DALLAS_SENSORS and allocates the
 *     actDallasData array.
 *  4. Reads the 64-bit ROM address of each sensor, sets its resolution,
 *     and formats the address as a hex string for display / MQTT use.
 *  5. Issues the initial requestTemperatures() call.
 *  6. If DALLASASYNC is true, switches the library to non-blocking
 *     conversion mode (setWaitForConversion(false)).
 *  7. Calls loadDallasAlias() to restore user-defined aliases from
 *     LittleFS.
 *
 * Thread-safety:
 *   Intended to be called once at startup from setup().  Not re-entrant.
 *   If called concurrently with dallasLoop() or readNewDallasTemp() the
 *   behaviour is undefined — the shared actDallasData array and the
 *   OneWire bus are not protected by a mutex.
 */
void initDallasSensors(void (*log_message)(char*), unsigned int updateAllDallasTimeSettings, unsigned int dallasTimerWaitSettings, unsigned int dallasResolution) {
  char log_msg[256];
  updateAllDallasTime = updateAllDallasTimeSettings;
  dallasTimerWait = dallasTimerWaitSettings;
  DS18B20.begin();
  dallasDevicecount  = DS18B20.getDeviceCount();
  sprintf_P(log_msg, PSTR("Number of 1wire sensors on bus: %d"), dallasDevicecount); log_message(log_msg);
  if ( dallasDevicecount > MAX_DALLAS_SENSORS) {
    dallasDevicecount = MAX_DALLAS_SENSORS;
    sprintf_P(log_msg, PSTR("Reached max 1wire sensor count. Only %d sensors will provide data."), dallasDevicecount);
    log_message(log_msg);
  }

  //init array
  delete actDallasData;
  actDallasData = new dallasDataStruct [dallasDevicecount];
  for (int j = 0 ; j < dallasDevicecount; j++) {
    DS18B20.getAddress(actDallasData[j].sensor, j);
    DS18B20.setResolution(actDallasData[j].sensor, dallasResolution);
  }

  DS18B20.requestTemperatures();
  for (int i = 0 ; i < dallasDevicecount; i++) {
    actDallasData[i].address[16] = '\0';
    for (int x = 0; x < 8; x++)  {
      // zero pad the address if necessary
      sprintf(&actDallasData[i].address[x * 2], "%02x", actDallasData[i].sensor[x]);
    }
    sprintf_P(log_msg, PSTR("Found 1wire sensor: %s"), actDallasData[i].address ); log_message(log_msg);
  }
  if (DALLASASYNC) DS18B20.setWaitForConversion(false); //async 1wire during next loops
  loadDallasAlias();
}

void resetlastalldatatime_dallas() {
  lastalldatatime_dallas = 0;
}

/*
 * readNewDallasTemp() — Reads temperatures from all discovered DS18B20
 *                       sensors and publishes changes to MQTT / WebSocket.
 *
 * How it works:
 *  1. Checks whether the per-cycle update timer has elapsed; if so it sets
 *     updatenow = true so every sensor is re-published even when unchanged.
 *  2. In synchronous mode (DALLASASYNC == 0) it fires
 *     requestTemperatures() before reading.  In async mode the conversion
 *     was already started by dallasLoop().
 *  3. Loops over dallasDevicecount sensors, calling getTempC() for each.
 *  4. Filters out clearly invalid readings (temp < -120 °C → sensor
 *     offline).
 *  5. Applies a rate-of-change guard (MAXTEMPDIFFPERSEC) to reject spikes
 *     caused by electrical noise or conversion glitches.
 *  6. If the value is accepted and updatenow is true or the value differs,
 *     it updates the cached temperature, publishes to MQTT (temperature +
 *     alias), pushes a JSON update to WebSocket clients, and fires the
 *     rules engine.
 *
 * Thread-safety:
 *   Called from dallasLoop() which runs in a FreeRTOS task.  Must not be
 *   called concurrently with itself or with initDallasSensors() — the
 *   OneWire bus and actDallasData array are shared unprotected.  Assumes
 *   dallasDevicecount and actDallasData are stable while it runs.
 */
void readNewDallasTemp(void (*log_message)(char*), char* mqtt_topic_base) {
  char log_msg[256];
  char mqtt_topic[256];
  char valueStr[80];
  bool updatenow = false;

  if ((lastalldatatime_dallas == 0) || ((unsigned long)(millis() - lastalldatatime_dallas) >  (1000 * updateAllDallasTime))) {
    updatenow = true;
    lastalldatatime_dallas = millis();
  }
  if (!(DALLASASYNC)) DS18B20.requestTemperatures();
  for (int i = 0; i < dallasDevicecount; i++) {
    float temp = DS18B20.getTempC(actDallasData[i].sensor);
    if (temp < -120.0) {
      sprintf_P(log_msg, PSTR("Error 1wire sensor offline: %s"), actDallasData[i].address); log_message(log_msg);
    } else {
      float allowedtempdiff = (((millis() - actDallasData[i].lastgoodtime)) / 1000.0) * MAXTEMPDIFFPERSEC;
      if ((actDallasData[i].temperature != -127.0) and ((temp > (actDallasData[i].temperature + allowedtempdiff)) or (temp < (actDallasData[i].temperature - allowedtempdiff)))) {
        sprintf_P(log_msg, PSTR("Filtering 1wire sensor temperature (%s). Delta to high. Current: %.2f Last: %.2f"), actDallasData[i].address, temp, actDallasData[i].temperature);
        log_message(log_msg);
      } else {
        actDallasData[i].lastgoodtime = millis();
        if ((updatenow) || (actDallasData[i].temperature != temp )) {  //only update mqtt topic if temp changed or after each update timer
          actDallasData[i].temperature = temp;
          sprintf(log_msg, PSTR("Received 1wire sensor temperature (%s): %.2f"), actDallasData[i].address, actDallasData[i].temperature);
          log_message(log_msg);
          if (true) {
            sprintf_P(valueStr, PSTR("%.2f"), actDallasData[i].temperature);
            sprintf_P(mqtt_topic, PSTR("%s/%s/%s"), mqtt_topic_base, mqtt_topic_1wire, actDallasData[i].address); mqttPublishQueued(mqtt_topic, valueStr, MQTT_RETAIN_VALUES);
            sprintf_P(valueStr, PSTR("%s"), actDallasData[i].alias);
            sprintf_P(mqtt_topic, PSTR("%s/%s/%s/alias"), mqtt_topic_base, mqtt_topic_1wire, actDallasData[i].address); mqttPublishQueued(mqtt_topic, valueStr, MQTT_RETAIN_VALUES);
          } else {
            sprintf_P(valueStr, PSTR("{\"Temperature\":%.2f,\"Alias\":\"%s\"}"), actDallasData[i].temperature, actDallasData[i].alias);
            sprintf_P(mqtt_topic, PSTR("%s/%s/%s"), mqtt_topic_base, mqtt_topic_1wire, actDallasData[i].address); mqttPublishQueued(mqtt_topic, valueStr, MQTT_RETAIN_VALUES);
          }
          sprintf_P(log_msg, PSTR("{\"data\": {\"dallasvalues\": {\"sensorID\": \"%s\", \"value\": %.2f}}}"), actDallasData[i].address, actDallasData[i].temperature);
          websocket_write_all(log_msg, strlen(log_msg));          
          rules_event_cb(_F("ds18b20#"), actDallasData[i].address);
        }
      }
    }
  }
}

/*
 * dallasLoop() — Periodic poller for DS18B20 sensors, intended to be called
 *                 from a dedicated FreeRTOS task.
 *
 * How it works:
 *  1. Every dallasTimerWait seconds it requests a new temperature conversion
 *     from all sensors on the bus.
 *  2. Asynchronous mode (DALLASASYNC == 1):
 *      - Calls requestTemperatures() (non-blocking) and records the time
 *        in dallasTimer1.
 *      - On subsequent calls, once 750 ms have elapsed since the request,
 *        it invokes readNewDallasTemp() to collect the results.
 *  3. Synchronous mode (DALLASASYNC == 0):
 *      - Calls readNewDallasTemp() directly; that function issues
 *        requestTemperatures() itself (blocking the task for ~750 ms).
 *
 * Thread-safety:
 *   Must be the sole function accessing the OneWire bus / actDallasData.
 *   No mutex is held, so initDallasSensors() must not run concurrently.
 *   The 750 ms async wait is a heuristic; a congested bus or
 *   parasitic-powered sensors may require a longer delay.
 */
void dallasLoop(void (*log_message)(char*), char* mqtt_topic_base) {
  if ((unsigned long)(millis() - dallasTimer) > (1000 * dallasTimerWait)) {
    log_message((char*)"Requesting new 1wire temperatures");
    dallasTimer = millis();
    if (DALLASASYNC){
      DS18B20.requestTemperatures();
      dallasTimer1=millis();
    }else{
      readNewDallasTemp(log_message, mqtt_topic_base);
    }
  }
  if ((dallasTimer1!=0) && ((millis() - dallasTimer1)>750)){
    dallasTimer1=0;
    readNewDallasTemp(log_message, mqtt_topic_base);
  }   
}

void dallasJsonOutput(struct webserver_t *client) {
  webserver_send_content_P(client, PSTR("["), 1);

  for (int i = 0; i < dallasDevicecount; i++) {
    webserver_send_content_P(client, PSTR("{\"Sensor\":\""), 11);
    webserver_send_content(client, actDallasData[i].address, strlen(actDallasData[i].address));
    webserver_send_content_P(client, PSTR("\",\"Temperature\":"), 16);
    char str[64];
    dtostrf(actDallasData[i].temperature, 0, 2, str);
    webserver_send_content(client, str, strlen(str));
    webserver_send_content_P(client, PSTR(",\"Alias\":\""), 10);
    webserver_send_content(client, actDallasData[i].alias, strlen(actDallasData[i].alias));
    if (i < dallasDevicecount - 1) {
      webserver_send_content_P(client, PSTR("\"},"), 3);
    } else {
      webserver_send_content_P(client, PSTR("\"}"), 2);
    }
  }
  webserver_send_content_P(client, PSTR("]"), 1);
}

void changeDallasAlias(char* address, char* alias) {
  JsonDocument jsonDoc;
  for (int i = 0 ; i < dallasDevicecount; i++) {
    if (strcmp(address, actDallasData[i].address) == 0) {
      strlcpy(actDallasData[i].alias, alias, sizeof(actDallasData[i].alias));
    }
    jsonDoc[actDallasData[i].address] = actDallasData[i].alias;
  }
  if (LittleFS.begin()) {
    File configFile = LittleFS.open("/dallas.json", "w");
    if (configFile) {
      serializeJson(jsonDoc, configFile);
      configFile.close();
    }
  }
}

void loadDallasAlias() {
  if (LittleFS.begin()) {
    if (LittleFS.exists("/dallas.json")) {
      File configFile = LittleFS.open("/dallas.json", "r");
      if (configFile) {
        size_t size = configFile.size();
        std::unique_ptr<char[]> buf(new char[size]);
        configFile.readBytes(buf.get(), size);
        JsonDocument jsonDoc;
        DeserializationError error = deserializeJson(jsonDoc, buf.get());
        if (!error) {
          for (int i = 0 ; i < dallasDevicecount; i++) {
            if ( jsonDoc[actDallasData[i].address] ) strncpy(actDallasData[i].alias, jsonDoc[actDallasData[i].address], sizeof(actDallasData[i].alias));
          }
        }
      }
    }
  }
}
