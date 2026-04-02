cmake_minimum_required(VERSION 3.16)

set(hosted_slave_dir "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__esp_hosted/slave")

add_compile_options(-Wl,--wrap=esp_wifi_init)

file(READ "${hosted_slave_dir}/main/esp_hosted_coprocessor_fw_ver.h" HEADER_CONTENTS)
set(PROJECT_VERSION_REGEX "#define[ \t]+PROJECT_VERSION_MAJOR_1[ \t]+([0-9]+)[ \t\n]+#define[ \t]+PROJECT_VERSION_MINOR_1[ \t]+([0-9]+)[ \t\n]+#define[ \t]+PROJECT_VERSION_PATCH_1[ \t]+([0-9]+)")
string(REGEX MATCH "${PROJECT_VERSION_REGEX}" PROJECT_VERSION_MATCH "${HEADER_CONTENTS}")

if(PROJECT_VERSION_MATCH)
	set(PROJECT_VER "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
endif()

set(EXTRA_COMPONENT_DIRS "${hosted_slave_dir}/main")
set(EXCLUDE_COMPONENTS lwip esp_netif)
set(COMPONENTS esp_timer esptool_py bootloader main nvs_flash esp_rom esp_wifi protocomm driver mqtt wpa_supplicant wifi_provisioning esp_driver_sdio esp_pm)
set(SDKCONFIG_DEFAULTS
	"${hosted_slave_dir}/sdkconfig.defaults"
	"${hosted_slave_dir}/sdkconfig.defaults.esp32c6"
	"${CMAKE_CURRENT_SOURCE_DIR}/sdkconfig.defaults")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32c6_bridge)

idf_build_set_property(COMPILE_OPTIONS "-fdiagnostics-color=always" APPEND)