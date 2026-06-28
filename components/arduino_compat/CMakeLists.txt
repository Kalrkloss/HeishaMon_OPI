idf_component_register(
    SRCS "compat.cpp" "Adafruit_NeoPixel.cpp" "ArduinoOTA.cpp" "OneWire.cpp" "DallasTemperature.cpp" "WiFi.cpp" "WiFiClient.cpp" "WiFiClientSecure.cpp" "WiFiServer.cpp" "DNSServer.cpp" "Update.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        spiffs
        spi_flash
        esp_timer
        esp_system
        esp_driver_gpio
        esp_driver_uart
        esp_driver_rmt
        esp_wifi
        esp_netif
        esp_event
        lwip
        mbedtls
        app_update
        esp_psram
        espressif__mdns
)
