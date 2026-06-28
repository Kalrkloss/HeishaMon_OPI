#include <Arduino.h>
#include <memory>
#include <WiFiClientSecure.h>

// Forward declarations for all functions defined in HeishaMon.ino
void setupETH();
void check_wifi();
void mqtt_reconnect();
void blinkNeoPixel(bool status);
void log_message(char* string);
void logHex(char *hex, byte hex_len);
void mqttPublish(char* topic, char* subtopic, char* value);
void mqttPublish(char* topic, char* subtopic, char* value, bool retain);
bool isValidReceiveChecksum(char* check_data, byte check_length);
void readProxy();
bool readSerial();
void popCommandBuffer();
void pushCommandBuffer(byte* command, int length);
void serialTXTask(void *pvParameters);
bool send_command(byte* command, int length);
void mqtt_callback(char* topic, byte* payload, unsigned int length);
void setupOTA();
void setupHttp();
void factoryReset();
void doubleResetDetect();
void setupSerial();
void switchSerial();
void setupMqtt();
void setupConditionals();
void readHeatpump();
void checkBootButton();

#include "../HeishaMon/HeishaMon.ino"
