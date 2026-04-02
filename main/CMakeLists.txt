idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        gpsp_core
        esp32p4_hal
        esp_timer
        esp_psram
)
