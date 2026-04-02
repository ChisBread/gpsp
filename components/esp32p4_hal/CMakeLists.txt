set(HAL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../esp32p4")

set(ESP32P4_SRCS
    "${HAL_DIR}/video_driver.c"
    "${HAL_DIR}/audio_driver.c"
    "${HAL_DIR}/input_driver.c"
    "${HAL_DIR}/storage.c"
)

idf_component_register(
    SRCS ${ESP32P4_SRCS}
    INCLUDE_DIRS "${HAL_DIR}"
    REQUIRES
        driver
        esp_lcd
        esp_driver_ppa
        esp_driver_i2s
        esp_driver_i2c
        esp_driver_gpio
        esp_driver_sdmmc
        esp_driver_ledc
        esp_hw_support
        fatfs
        sdmmc
        vfs
        freertos
        espressif__esp_lcd_st7701
        gpsp_core
)

# ST7701 LCD driver component (from JC4880 BSP)
set(ST7701_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../JC4880P443C_I_W/1-Demo/idf_examples/ESP-IDF/common_components/espressif__esp_lcd_st7701")
if(EXISTS "${ST7701_DIR}/CMakeLists.txt")
    # Use the ST7701 component from the hardware reference
    message(STATUS "Using ST7701 LCD driver from JC4880 BSP")
else()
    message(WARNING "ST7701 LCD driver not found at ${ST7701_DIR}")
endif()
