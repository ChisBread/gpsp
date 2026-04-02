# ESP32-C6 Bridge

This directory is a standalone ESP-IDF subproject for the board's bundled ESP32-C6.

Current status:
- It now wraps the local `managed_components/espressif__esp_hosted/slave` firmware instead of building a placeholder app.
- The transport is aligned with the vendor `xiaozhi-esp32` example: `esp_hosted` over SDIO.
- The slave build uses the hosted project's built-in `app_main()` and ESP32-C6 defaults.
- Board auto-mapping is enabled with `CONFIG_ESP_HOST_DEV_BOARD_P4_C6_CORE=y`.

Build:

```sh
. /opt/esp-idf/export.sh
cd esp32c6_bridge
idf.py set-target esp32c6
idf.py build
```

Notes:
- The wrapper overrides the partition-table path so it can reuse `managed_components/espressif__esp_hosted/slave/partitions.esp32c6.csv` directly.
- Host-side SDIO wiring on the ESP32-P4 side remains GPIO19 CMD, GPIO18 CLK, GPIO14-17 D0-D3, with GPIO54 as the C6 reset line.