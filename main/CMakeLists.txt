idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        gpsp_core
        esp32p4_hal
        esp_timer
        esp_psram
        esp_event
        esp_netif
        esp_wifi
        nvs_flash
        esp_hosted
        esp_wifi_remote
)
