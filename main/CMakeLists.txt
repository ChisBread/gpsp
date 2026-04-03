idf_component_register(
    SRCS
        "app_main.c"
        "av_pipeline.c"
        "c6_remote.c"
        "gba_session.c"
    INCLUDE_DIRS "."
    REQUIRES
        gpsp_core
        esp32p4_hal
        esp_timer
        esp_psram
)

if(CONFIG_GPSP_ENABLE_C6_REMOTE)
    idf_component_get_property(component_lib ${COMPONENT_LIB} COMPONENT_LIB)
    target_link_libraries(${component_lib} PUBLIC idf::esp_event idf::esp_netif idf::esp_wifi idf::nvs_flash idf::espressif__esp_hosted idf::espressif__esp_wifi_remote)
endif()
