# 01 · 总体架构

## 1.1 角色

```
 ┌──────────────┐ QSPI (CLK,CS,IO0..3,IRQ) ┌─────────┐  GBA bus (16b AD + 8b A_hi + strobes)  ┌──────────┐
 │ Host (P4)    │ ◄─────────────────────► │  FPGA   │ ◄─────────────────────────────────────► │ Cartridge│
 └──────────────┘                          └─────────┘                                          └──────────┘
   语义层                                    透明桥                                               被桥接设备
   - Region 配置                             - 命令解码                                          - 普通卡带
   - 缓存/预取                               - 时序生成                                          - 烧录卡
   - 业务协议                                - FIFO                                              - 任意 16b 总线设备
                                             - IRQ 聚合
```

**关键边界**：FPGA 不存储任何关于"地址 X 是什么类型"的语义。Region 表是 host 通过命令推下来的策略提示，FPGA 据此决定是否允许优化（prefetch/merge），但**默认行为是严格透明**。

## 1.2 GBA 卡带总线信号（FPGA ↔ Cart 侧）

| 信号 | 位宽 | 方向 | 说明 |
|---|---|---|---|
| AD[15:0] | 16 | 双向 | 复用：低 16b 地址 + 16b 数据（/CS 通道）；/CS2 通道仅 AD[7:0] 作为 8b 数据 |
| A[23:16] | 8 | 输出 | 地址高 8b（仅 /CS 通道使用） |
| /CS  | 1 | 输出 | ROM 段 strobe（GBA 0x08000000-0x09FFFFFF + WS1/WS2 镜像）。**总线宽度固定 16-bit** |
| /CS2 | 1 | 输出 | SRAM 段 strobe（0x0E000000-0x0E00FFFF）。**总线宽度固定 8-bit** |
| /RD  | 1 | 输出 | 读 strobe |
| /WR  | 1 | 输出 | 写 strobe |
| IRQ  | 1 | 输入 | 卡带中断（GamePak IRQ） |

**两个通道有不同的硬件特性，协议层强制区分**：

| 特性 | /CS 通道 | /CS2 通道 |
|---|---|---|
| 地址宽度 | 24-bit (AD[15:0] + A[23:16]) | 16-bit (AD[15:0] 仅地址相，无 A_hi) |
| 数据宽度 | 16-bit (AD[15:0]) | 8-bit (AD[7:0])；AD[15:8] don't-care |
| 地址相位 | 单次 strobe 内复用 AD | 单次 strobe 内复用 AD[15:0]（仅低半字） |
| 突发递增粒度 | +2 bytes/周期 | +1 byte/周期 |

每个 host 命令必须显式指定通道（直接通过 `ch` 字段，或间接通过 region 表）。**FPGA 不会自动跨通道**。

## 1.3 抽象总线模型

FPGA 暴露给 host 的抽象是 **(channel, addr, data)** 三元组，但 channel 决定了 addr 与 data 的有效位宽：

```
struct bus_xfer {
    u8  channel;   // 0 = /CS (16-bit, 24-bit addr),  1 = /CS2 (8-bit, 16-bit addr),  2 = raw
    u32 addr;      // /CS: 24-bit;  /CS2: 16-bit (高位忽略)
    u16 data;      // /CS: 16-bit;  /CS2: 低 8-bit
};
```

注意：通道决定了访问宽度，**没有"在 /CS2 上做 16-bit"或"在 /CS 上做 8-bit"的合法组合**。
若 host 需要 8-bit ROM 访问（罕见），必须用 16-bit 读取后丢弃一半，或用 `BUS_CYCLES` 自定义。

`channel=2` (raw) 用于 BUS_CYCLES 命令，host 直接控制每根 strobe 的电平/时长。

## 1.4 协议层次

```
┌─────────────────────────────────────────────────────────┐
│  Application / Cartridge driver (host side)             │  ← 由 host 自由组织
│   - ROM 缓存 / EEPROM 状态机 / Flashcart bank 管理      │
└─────────────────────────────────────────────────────────┘
                           │
┌─────────────────────────────────────────────────────────┐
│  Bridge protocol (本文档定义)                            │
│   - Command framing                                      │
│   - Region attribute negotiation                         │
│   - Timing profile selection                             │
└─────────────────────────────────────────────────────────┘
                           │
┌─────────────────────────────────────────────────────────┐
│  QSPI physical layer                                     │
│   - SPI Mode 0, Quad I/O                                 │
└─────────────────────────────────────────────────────────┘
```

## 1.5 透明性原则

按严格度排列：

1. **STRICT**（默认）：每个 host 请求 ↔ 一组确定的总线周期。FPGA 不做任何重排序、合并、预取、缓存
2. **BURST_OK**：host 显式标记某 region 顺序读安全（无副作用），FPGA 可使用顺序时序优化
3. **PREFETCH_OK**：host 进一步允许 FPGA 在 STREAM 命令下提前发起读，但仍需保持地址递增 + 不越过 region 边界
4. **CACHE_OK**：host 提示该 region 内容稳定，FPGA 内部行缓存可重用最近读

写访问无 PREFETCH/CACHE 概念，但有：

- **WRITE_MERGE_OK**：连续写允许合并到同一 burst（仅当目标设备语义允许）
- **WRITE_SIDE_EFFECT**：写本身是命令（如烧录卡 bank 寄存器），必须按精确顺序、单独 strobe 周期

默认所有 region 均为最严格 STRICT；host 必须显式 opt-in 才启用优化。**这条原则保证未知卡带永远不会因桥接器优化而行为异常**。

## 1.6 IRQ

FPGA → host 单线 IRQ，原因汇总到状态寄存器（详见 [02-protocol.md](02-protocol.md) `STATUS`）：

| flag | 含义 |
|---|---|
| HOTPLUG | 卡带插拔检测变化 |
| CART_IRQ | 卡带 IRQ 引脚电平变化 |
| FIFO_HALF | STREAM 模式 FIFO 过半（host 可读） |
| FIFO_UNDERRUN | STREAM 模式数据耗尽（异常） |
| ERR | 协议错误（CRC 失败、非法命令、超时） |
| OP_DONE | 异步操作完成（如长 BUS_CYCLES 序列） |

host 用 `IRQ_ACK` 命令清位。
