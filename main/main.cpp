#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_psram.h"
#include "nvs_flash.h"

static const char *TAG = "HeishaMon";

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "--- HEISHAMON --- starting...");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGI(TAG, "Erasing NVS...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP32-S3 PSRAM available: %s, size: %u bytes",
             esp_psram_is_initialized() ? "yes" : "no",
             esp_psram_get_size());

    ESP_LOGI(TAG, "Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    ESP_LOGI(TAG, "Free PSRAM: %lu bytes", (unsigned long)esp_get_free_internal_heap_size());

    // TODO Phase 2+: port initialization from Arduino setup()
    // setupSerial, loadSettings, setupWifi, setupHttp, setupMqtt, etc.

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
