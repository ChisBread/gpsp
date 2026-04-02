# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/components"
    "${CMAKE_CURRENT_SOURCE_DIR}/JC4880P443C_I_W/1-Demo/idf_examples/ESP-IDF/common_components/espressif__esp_lcd_st7701"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gpsp-esp32p4)
