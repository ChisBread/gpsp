# GBA Test Harness

全功能全架构 GBA 调试/测试工具。

## 构建

```bash
make [ARCH=x86|arm|riscv] [DUAL=0|1] [TRACE=0|1] [DYNAREC=0|1]
```

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `ARCH` | `x86` | 目标架构。`arm`/`riscv` 交叉编译，需通过 QEMU 运行 |
| `DUAL` | `0` | 双核 PPU 模拟（pthread 仿真 ESP32-P4 流水线） |
| `TRACE` | `0` | 指令追踪（-DTRACE_INSTRUCTIONS） |
| `DYNAREC` | `1` | JIT 动态重编译。`0` = 纯解释器 |

每组参数生成独立的 `build/<config>/` 目录和二进制，互不冲突。

### 示例

```bash
# 默认: x86 JIT 单核
make

# 双核 PPU 模拟
make DUAL=1

# RISC-V JIT (需 riscv32 交叉工具链，qemu-riscv32 运行)
make ARCH=riscv

# ARM32 JIT (需 arm 交叉工具链，qemu-arm 运行)
make ARCH=arm

# 纯解释器
make DYNAREC=0

# 全部组合
make DUAL=1 TRACE=1

# 查看当前配置
make info

# 清理
make clean
```

## 运行

```bash
./test_gba_<config> [flags] [bios.bin] <rom.gba> [frames] [output.bin]
```

### 命令行标志

| 标志 | 说明 |
|------|------|
| `--interp` | 运行时切换到解释器（覆盖构建时 DYNAREC=1） |
| `--regs` | 每 100 帧打印寄存器状态 |
| `--dump-from N` | 从第 N 帧开始导出单帧 `.raw` 文件 |

### 位置参数

```
[bios.bin]   可选，GBA BIOS 文件。省略则使用内置 open BIOS
<rom.gba>    必需，GBA ROM 文件
[frames]     帧数，默认 18000（~5 分钟）
[output.bin] 帧输出文件名，默认 frames_single.bin / frames_dual.bin
```

参数数量决定解析方式：
- 1 个参数: rom
- 2 个参数: bios rom
- 3 个参数: bios rom frames
- 4 个参数: bios rom frames output

### 示例

```bash
# x86 单核，3000 帧
./test_gba_x86 ../../bios/gba_bios.bin ../../bios/game.gba 3000

# x86 双核
./test_gba_x86_dual ../../bios/gba_bios.bin ../../bios/game.gba 3000

# RISC-V (通过 QEMU)
qemu-riscv32 ./test_gba_riscv ../../bios/gba_bios.bin ../../bios/game.gba 500

# ARM32 (通过 QEMU)
qemu-arm ./test_gba_arm ../../bios/gba_bios.bin ../../bios/game.gba 500
```

## 输出文件

| 文件 | 格式 | 说明 |
|------|------|------|
| `frames_*.bin` | 240×160 u16 RGB565LE，每帧 76800 字节 | 所有帧像素数据 |
| `audio_*.pcm` | s16le stereo @ 65536 Hz | 音频 PCM 数据 |
| `crc_*.bin` | 每帧 16 字节: frame_nr, oam_crc, io0_crc, pixel_crc | 仅 DUAL=1 |
| `frames_*/frame_NNNNN.raw` | 单帧 76800 字节 | 仅 `--dump-from` |

## 生成视频

```bash
./make_video.sh frames_dual.bin audio_dual.pcm output.mp4
```

ffmpeg 将原始帧流 + PCM 合成 MP4（H.264 + MP3，3× 最近邻放大到 720×480）。

## 帧对比

```bash
# 构建单核和双核两个版本
make DUAL=0
make DUAL=1

# 分别运行
./test_gba_x86 ../../bios/gba_bios.bin ../../bios/game.gba 1000
./test_gba_x86_dual ../../bios/gba_bios.bin ../../bios/game.gba 1000

# 逐帧对比
python3 compare_frames.py frames_single.bin frames_dual.bin
```

### CRC 对比 (x86 vs ESP32 设备)

```bash
# ESP32 设备端启用 dump 后拉取 crc 文件
python3 compare_crc.py crc_dual.bin crc_device.bin
```

## 架构配置细节

| | x86 | ARM | RISC-V |
|---|---|---|---|
| 工具链 | 系统 gcc | `arm-linux-musleabihf-` | `riscv32-linux-musl-` |
| JIT 后端 | `x86/x86_stub.S` | `arm/arm_stub.S` | `riscv/riscv_stub.S` |
| 链接方式 | 动态 | 静态 (musl) | 静态 (musl) |
| 运行方式 | 直接 | `qemu-arm` | `qemu-riscv32` |
| 定义 | `MMAP_JIT_CACHE` | `ARM_ARCH` | `RISCV_ARCH` |
| 交叉编译器路径 | — | `CROSS=/opt/arm-.../` | `CROSS=/opt/riscv32-.../` |

所有架构均定义 `QEMU_HARNESS`。`DUAL=1` 附加 `-DDUAL_CORE_PPU -lpthread`，`TRACE=1` 附加 `-DTRACE_INSTRUCTIONS`。
