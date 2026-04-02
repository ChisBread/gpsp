# JC4880P443C_I_W IO 引脚分配表

目标硬件：GUITION JC4880P443C_I_W（ESP32-P4，4.3" 480×800 LCD）

## MIPI-DSI 显示（ST7701）

| 功能 | GPIO / 资源 | 说明 |
|------|-------------|------|
| DSI D0P/D0N | 专用引脚 | MIPI DSI 数据通道 0 |
| DSI D1P/D1N | 专用引脚 | MIPI DSI 数据通道 1 |
| DSI CLKP/CLKN | 专用引脚 | MIPI DSI 时钟 |
| DSI PHY 供电 | LDO ch3, 2500mV | 片内 LDO |
| LCD 背光 | GPIO23 | LEDC PWM, 20kHz, 10-bit |
| LCD 复位 | GPIO5 | 低电平复位 |

- DPI 像素时钟：34 MHz
- 分辨率：480×800
- 时序：HSYNC BP=42 PW=12 FP=42，VSYNC BP=8 PW=2 FP=166

## I2C 总线（I2C_NUM_0）

| 功能 | GPIO | 说明 |
|------|------|------|
| SDA | GPIO7 | 共享总线：ES8311 + GT911 |
| SCL | GPIO8 | 400kHz |

### I2C 设备

| 设备 | 地址 | 用途 |
|------|------|------|
| ES8311 | 0x18 | 音频编解码器 |
| GT911 | 0x5D | 电容触摸屏 |

## I2S 音频

| 功能 | GPIO | 说明 |
|------|------|------|
| MCLK | GPIO13 | 主时钟输出 |
| BCLK | GPIO12 | 位时钟 |
| WS/LRCK | GPIO10 | 字选择/左右声道时钟 |
| DOUT | GPIO9 | 数据输出（到扬声器/ES8311 DAC） |
| DIN | GPIO48 | 数据输入（麦克风/ES7210） |
| PA 使能 | GPIO11 | NS4150 功放使能，高电平有效 |

## SD 卡（SDMMC Slot 0, IO MUX）

| 功能 | GPIO | 说明 |
|------|------|------|
| CLK | GPIO43 | IO MUX 固定引脚 |
| CMD | GPIO44 | IO MUX 固定引脚 |
| D0 | GPIO39 | IO MUX 固定引脚 |
| D1 | GPIO40 | IO MUX 固定引脚 |
| D2 | GPIO41 | IO MUX 固定引脚 |
| D3 | GPIO42 | IO MUX 固定引脚 |
| 供电 | LDO ch4 | 片内 LDO, 3.3V |

- 4-bit 总线宽度，高速模式

## 板载 ESP32-C6 通信（示例工程配置）

下面这组引脚来自板厂附带的 `xiaozhi-esp32` ESP-IDF 示例工程配置，
用于 ESP32-P4 作为 host、ESP32-C6 作为 slave 的 `esp_hosted` over SDIO。

| 功能 | GPIO | 说明 |
|------|------|------|
| SDIO CLK | GPIO18 | P4 -> C6 时钟 |
| SDIO CMD | GPIO19 | P4 <-> C6 命令线 |
| SDIO D0 | GPIO14 | 数据线 0 |
| SDIO D1 | GPIO15 | 数据线 1 |
| SDIO D2 | GPIO16 | 数据线 2 |
| SDIO D3 | GPIO17 | 数据线 3 |
| C6 Reset | GPIO54 | 高电平有效复位配置 |

- Host 接口：SDIO Slot 1
- 总线宽度：4-bit
- 时钟：40 MHz
- Reset 极性：active high
- 来源：`JC4880P443C_I_W/1-Demo/idf_examples/ESP-IDF/xiaozhi-esp32/sdkconfig`

## USB

| 功能 | GPIO | 说明 |
|------|------|------|
| USB D+ | GPIO24 | USB-OTG |
| USB D- | GPIO25 | USB-OTG |

## 板载按键

| 按键 | GPIO | 说明 |
|------|------|------|
| S1 (BOOT) | GPIO0 | 启动模式选择 |
| S2 (RESET) | EN | 硬件复位 |
| S3 | — | 待确认 |
| S4 | — | 待确认 |

## JP1 扩展连接器（可用于 GBA 按键映射）

| 引脚 | GPIO | 建议映射 |
|------|------|----------|
| 1 | GPIO28 | A |
| 2 | GPIO29 | B |
| 3 | GPIO30 | Select |
| 4 | GPIO31 | Start |
| 5 | GPIO32 | Right |
| 6 | GPIO33 | Left |
| 7 | GPIO34 | Up |
| 8 | GPIO35 | Down |
| 9 | GPIO49 | R |
| 10 | GPIO50 | L |
| 11 | GPIO51 | 备用 |
| 12 | GPIO52 | 备用 |

> JP1 映射为建议方案，需根据实际接线在 menuconfig 中配置对应 GPIO 编号。

## 片内 LDO 使用

| 通道 | 电压 | 用途 |
|------|------|------|
| ch3 | 2500mV | MIPI DSI PHY 供电 |
| ch4 | 3300mV | SD 卡供电 |

## GPIO 占用汇总

| GPIO | 功能 | 方向 |
|------|------|------|
| 0 | BOOT 按键 | 输入 |
| 5 | LCD 复位 | 输出 |
| 7 | I2C SDA | 双向 |
| 8 | I2C SCL | 输出 |
| 9 | I2S DOUT | 输出 |
| 10 | I2S WS | 输出 |
| 11 | PA 使能 | 输出 |
| 12 | I2S BCLK | 输出 |
| 13 | I2S MCLK | 输出 |
| 14-19 | 板载 C6 SDIO | 双向/输出 |
| 23 | LCD 背光 PWM | 输出 |
| 24 | USB D+ | 双向 |
| 25 | USB D- | 双向 |
| 28-35 | JP1 扩展 (可用) | 输入 |
| 39-42 | SD D0-D3 | 双向 |
| 43 | SD CLK | 输出 |
| 44 | SD CMD | 双向 |
| 48 | I2S DIN | 输入 |
| 49-52 | JP1 扩展 (可用) | 输入 |
| 54 | 板载 C6 Reset | 输出 |
