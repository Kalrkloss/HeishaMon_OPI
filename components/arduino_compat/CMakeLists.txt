idf_component_register(
    SRCS "compat.cpp" "OneWire.cpp" "DallasTemperature.cpp" "WiFi.cpp" "WiFiClient.cpp" "WiFiServer.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        esp_timer
        esp_system
        esp_driver_gpio
        esp_driver_uart
        esp_wifi
        esp_netif
        esp_event
        lwip
)
