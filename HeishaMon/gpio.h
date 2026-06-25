#if defined(ESP8266)
#define NUMGPIO 3
#include <ESP8266WiFi.h>
#include <ESP8266WiFiGratuitous.h>
#elif defined(ESP32)
#define NUMGPIO 7
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Update.h>
#define relayOnePin 21
#define relayTwoPin 47
#endif

extern const char* mqtt_topic_gpio;

struct gpioSettingsStruct {
#if defined(ESP8266)
  unsigned int gpioPin[NUMGPIO] = {1, 3, 16};
  unsigned int gpioMode[NUMGPIO] = {INPUT_PULLUP, INPUT_PULLUP, INPUT_PULLUP};
#elif defined(ESP32)
  #ifdef CONFIG_IDF_TARGET_ESP32S3
    // ESP32-S3: GPIO 34-37 don't exist, use alternative pins
    unsigned int gpioPin[NUMGPIO] = {11, 12, 13, 14, 15, 21, 47};
  #else
    // Regular ESP32: Original pin configuration
    unsigned int gpioPin[NUMGPIO] = {33, 34, 35, 36, 37, 21, 47};
  #endif
  unsigned int gpioMode[NUMGPIO] = {INPUT_PULLUP, INPUT_PULLUP, INPUT_PULLUP, INPUT_PULLUP, INPUT_PULLUP, OUTPUT, OUTPUT};
#endif
};

void setupGPIO(gpioSettingsStruct gpioSettings);
void mqttGPIOCallback(char* topic, char* value);


