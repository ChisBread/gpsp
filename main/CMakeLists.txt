idf_component_register(
    SRCS
        "app_main.c"
        "av_pipeline.c"
        "c6_remote.c"
        "gba_session.c"
        "netpacket_bridge.c"
        "ppu_pipeline.c"
        "runtime_config.c"
    INCLUDE_DIRS "."
    REQUIRES
        gpsp_core
        esp32p4_hal
        esp_timer
        esp_psram
        lwip
)

if(CONFIG_GPSP_CPU_PROFILE_STATS)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC CPU_PROFILE_STATS)
endif()

target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-u netpacket_poll_receive"
    "-u netpacket_send"
    "-u netplay_client_id"
    "-u netplay_num_clients"
)

if(CONFIG_GPSP_ENABLE_C6_REMOTE)
    target_link_libraries(${COMPONENT_LIB} PUBLIC idf::esp_event idf::esp_netif idf::esp_wifi idf::nvs_flash idf::espressif__esp_hosted idf::espressif__esp_wifi_remote)
endif()
