idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_http_server nvs_flash esp_netif spiffs esp_driver_uart
)