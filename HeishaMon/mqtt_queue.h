#ifndef MQTT_QUEUE_H
#define MQTT_QUEUE_H

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#define MQTT_PUBLISH_QUEUE_LEN 64

/*
 * Struct holding a single MQTT publish message queued from one RTOS task
 * to the MQTT publisher task.
 *
 *   topic   - MQTT topic string (null‑terminated, max 255 chars)
 *   payload - MQTT payload string (null‑terminated, max 255 chars)
 *   retain  - MQTT retain flag; if true the broker stores the message
 */
typedef struct {
  char topic[256];
  char payload[256];
  bool retain;
} mqttPublishMsg_t;

/*
 * FreeRTOS queue used as thread‑safe IPC.  Any task can push an
 * mqttPublishMsg_t onto this queue; a dedicated MQTT publisher task
 * pops items and forwards them to the broker.
 */
extern QueueHandle_t mqttPublishQueue;

/*
 * Helper that copies the string arguments into an mqttPublishMsg_t and sends
 * it to mqttPublishQueue.  Intended to be called from any RTOS task that
 * needs to publish an MQTT message without blocking on the MQTT stack.
 */
void mqttPublishQueued(const char* topic, const char* payload, bool retain);

#endif
