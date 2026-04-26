# 02 · 协议层 · QSPI 物理 + 帧格式 + 命令集

## 2.1 物理层

| 项 | 值 |
|---|---|
| SPI 模式 | Mode 0 (CPOL=0, CPHA=0) |
| 频率 | 协商范围 1–80 MHz；上电默认 10 MHz |
| 数据线 | IO0..IO3（Quad）；CMD/ADDR 阶段单线，DATA 阶段四线 |
| CS 极性 | 低有效 |
| 字节序 | CMD/ADDR 大端；DATA 按 host 视角的设备字节序（GBA = LE） |

## 2.2 通用帧格式

仿 NOR Flash 风格，但 ADDR 位宽与 DUMMY 周期数随命令变：

```
 ┌────────┬─────────────┬──────────┬─────────────┬─────────┐
 │ CMD 8b │ ADDR ≤ 32b  │ HDR var  │ DUMMY N cyc │ DATA …  │
 └────────┴─────────────┴──────────┴─────────────┴─────────┘
   单线        单线          单线         –             四线
```

- `CMD` 高 1 bit 是 `R/W` 方向位（1 = host READ from FPGA, 0 = host WRITE to FPGA）。剩下 7 bit 是 opcode
- `ADDR` 长度由 opcode 决定，对齐到字节边界
- `HDR` 是命令特定的额外参数（length、count 等），变长
- `DUMMY` 让 FPGA 准备数据（仅 READ 类）。固定为 8 个 sclk 周期（= 1 字节时间，简化解码）
- `DATA` 长度由 HDR 中的 length 字段决定，或开放至 CS 抬起为止（流式）

每个事务由 CS 下降沿启动、上升沿结束。CS 抬起即提交（write）/ 终止（stream）。

## 2.3 完整性

每个 host → FPGA 帧（不含 stream READ）的 DATA 段末尾追加一字节 **XOR8**（对从 CMD 到 DATA-1 的所有字节按字节异或）。FPGA → host 帧同样附 XOR8。

校验失败时：

- FPGA 收到坏帧：丢弃 + 置 `ERR` flag；不应用任何副作用
- host 收到坏帧：发 `RESET_LINK` 重同步

## 2.4 命令分组

| 范围 | 类别 |
|---|---|
| 0x00–0x0F | 链路 / 控制 |
| 0x10–0x1F | 状态 / 信息 |
| 0x20–0x3F | Region 配置 |
| 0x40–0x5F | Timing 配置 |
| 0x60–0x7F | 严格透传读写（STRICT） |
| 0x80–0x9F | 优化读写（BURST / STREAM） |
| 0xA0–0xBF | 复合 / 脚本（BUS_CYCLES, WRITE_LIST） |
| 0xC0–0xDF | IRQ / 维护 |
| 0xE0–0xEF | 调试 |
| 0xF0–0xFF | 保留 |

下方 R/W 列：`H←F` = host 从 FPGA 读, `H→F` = host 向 FPGA 写。

### 2.4.1 链路 / 控制

| OP | Name | ADDR | HDR | R/W | DATA | 说明 |
|---|---|---|---|---|---|---|
| 0x00 | NOP | – | – | – | – | 心跳 |
| 0x01 | RESET_LINK | – | – | – | – | 清 RX/TX FIFO，复位命令解码 |
| 0x02 | RESET_BUS | – | – | – | – | 复位卡带侧（拉 cart power 或 strobe high） |
| 0x03 | SYNC | – | – | H←F 4B | "GBAB" magic | 链路 alive 检测 |

### 2.4.2 状态 / 信息

| OP | Name | ADDR | HDR | R/W | DATA | 说明 |
|---|---|---|---|---|---|---|
| 0x10 | STATUS | – | – | H←F 8B | `cart_status` | 见下文 |
| 0x11 | INFO | sub:8b | – | H←F var | gateware 版本 / 资源信息 | sub=0 ver, sub=1 caps |

```c
struct cart_status {       // 8 bytes
    u8  link_ok;
    u8  irq_flags;         // HOTPLUG|CART_IRQ|FIFO_HALF|FIFO_UNDERRUN|ERR|OP_DONE
    u16 fifo_level;        // bytes available in stream FIFO
    u16 last_err;           // 错误码（见 02.10）
    u8  cart_present;
    u8  reserved;
};
```

### 2.4.3 Region 配置（详见 [04-regions.md](04-regions.md)）

| OP | Name | ADDR | HDR | R/W | DATA | 说明 |
|---|---|---|---|---|---|---|
| 0x20 | REGION_RESET | – | – | – | – | 清空所有 region，恢复默认 STRICT |
| 0x21 | REGION_SET | idx:8b | – | H→F 16B | `region_desc` | 写入第 idx 项 |
| 0x22 | REGION_GET | idx:8b | – | H←F 16B | `region_desc` | 读回 |
| 0x23 | REGION_INVALIDATE | idx:8b | – | – | – | 强制丢弃该 region 在 FPGA 内的任何缓存/预取数据 |

### 2.4.4 Timing 配置（详见 [03-timing.md](03-timing.md)）

| OP | Name | ADDR | HDR | R/W | DATA | 说明 |
|---|---|---|---|---|---|---|
| 0x40 | TIMING_SET | idx:8b | – | H→F 16B | `timing_profile` | 写入 profile（最多 8 组） |
| 0x41 | TIMING_GET | idx:8b | – | H←F 16B | `timing_profile` | 读回 |
| 0x42 | LINK_SPEED | – | – | H→F 1B | freq_code | 协商 QSPI sclk |

### 2.4.5 严格透传读写（STRICT）

**每条命令保证 1:1 对应到一组确定的总线周期，FPGA 禁用一切优化**。

通道决定访问宽度，命令名按通道命名（避免歧义）：

| OP | Name | ADDR | HDR | R/W | DATA | 通道 | 总线行为 |
|---|---|---|---|---|---|---|---|
| 0x60 | RD_CS_STRICT  | 24b | – | H←F 2B | – | /CS  (16-bit) | 单半字读，N 周期 |
| 0x61 | RD_CS2_STRICT | 16b | – | H←F 1B | – | /CS2 (8-bit)  | 单字节读，N 周期 |
| 0x68 | WR_CS_STRICT  | 24b | – | H→F 2B | – | /CS  (16-bit) | 单半字写 |
| 0x69 | WR_CS2_STRICT | 16b | – | H→F 1B | – | /CS2 (8-bit)  | 单字节写 |

ADDR 长度按通道有效位宽：/CS 用 24-bit，/CS2 用 16-bit（FPGA 内部对齐到 16b 边界，下位忽略对应通道之外的高位）。

注意**没有 32-bit STRICT 命令**：32-bit 访问由 host 用两条 16-bit 命令组合，因为：

1. /CS 通道一次 strobe 只能传 16b 数据
2. 32-bit 跨边界涉及顺序判定，已属于 burst 范畴 → 用 RD_BURST(len=4)
3. 保持 STRICT 命令"1 命令 = 1 strobe"的不变量

**STRICT 命令对 region 属性免疫** —— 即使 region 标了 BURST_OK，STRICT 也走严格路径。
host 用这些命令访问烧录卡控制寄存器、有副作用的位置。

### 2.4.6 优化读写（BURST / STREAM）

**仅当目标 region 标了对应属性时才允许；否则 FPGA 返回 ERR**。

通道由 region 表确定（命令本身不带 ch 字段，避免与 region attr 冲突）：

| OP | Name | ADDR | HDR | R/W | DATA | 总线行为 |
|---|---|---|---|---|---|---|
| 0x80 | RD_BURST  | 24b | len:16b | H←F len B | – | 顺序读，首字 N，后续 S |
| 0x81 | RD_STREAM | 24b | – | H←F 至 CS↑ | – | 持续 prefetch，FPGA 自递增 |
| 0x88 | WR_BURST  | 24b | len:16b | H→F len B | – | 顺序写，首字 N，后续 S |

`len` 是**字节数**，且必须是通道宽度的整数倍（/CS 要求偶数；/CS2 任意 ≥1）。
地址递增量 = 通道宽度（/CS 每周期 +2，/CS2 每周期 +1）。

`len` 0 视为非法。`RD_STREAM` 越过 region 边界时 FPGA 自动停止并置 ERR 等待 host CS↑。

### 2.4.7 复合 / 脚本

| OP | Name | ADDR | HDR | R/W | DATA | 说明 |
|---|---|---|---|---|---|---|
| 0xA0 | WRITE_LIST | – | count:16b | H→F | 见下 | 散列写 |
| 0xA1 | READ_LIST | – | count:16b | H↔F | 见下 | 散列读 |
| 0xA8 | BUS_CYCLES | – | count:16b | H↔F | 字节流，见 02.5 | 原始总线周期 |

`WRITE_LIST` 每条目 8 字节：`(addr:24b, data:16b, ch:8b, flags:8b, pad:8b)`。
`data` 字段在 ch=/CS2 时只用低 8b，高 8b 必须为 0。
`addr` 字段在 ch=/CS2 时只用低 16b，bits[23:16] 必须为 0。

`READ_LIST` 请求条目 4 字节：`(addr:24b, ch:8b)`。响应数据按通道宽度紧密排列：
ch=/CS 每条目 2B，ch=/CS2 每条目 1B；不同条目可混合通道。

`flags`（WRITE_LIST 用）：

- bit0: STRICT（即使 region 允许 burst，本条按 strict 走）
- bit1: BARRIER（执行完此条后 FPGA 等到 cart 真正空闲再继续；用于切 bank 后下一指令必须看到新 bank）

### 2.4.8 BUS_CYCLES 详解（关键）

让 host 直接编排任意总线周期。每个周期一字节"操作码" + 可选参数：

| 子 OP | 名称 | 参数 | 说明 |
|---|---|---|---|
| 0x00 | END | – | 序列结束 |
| 0x01 | DRIVE_AD | 2B | 把 16-bit 写到 AD[15:0]（输出方向）。/CS2 通道仅低 8b 有意义 |
| 0x02 | DRIVE_A_HI | 1B | 写 A[23:16]（仅 /CS 通道有效；/CS2 时被忽略） |
| 0x03 | SET_STROBE | 1B | bitmask: bit0=/CS, bit1=/CS2, bit2=/RD, bit3=/WR（0=低有效已 assert） |
| 0x04 | DELAY_NS | 2B | 延迟 N 纳秒 |
| 0x05 | TRISTATE_AD | – | AD bus 转输入 |
| 0x06 | SAMPLE_AD | 1B | 采样 AD 当前值；参数 0=采 16b (AD[15:0])，1=采 8b (AD[7:0])，追加到响应缓冲 |
| 0x07 | LOAD_TIMING | 1B | 临时切到 timing profile #N |
| 0x08 | WAIT_LOW | 1B+2B | 等某 strobe 拉低，超时 N µs |

执行返回：所有 SAMPLE_AD 采样值 + 1B 终止状态码。

**用途**：

- 烧录卡未公开的 bank 切换序列：用一串 DRIVE_AD + SET_STROBE 精确回放
- EEPROM bit-bang：80 条左右指令一次性下发
- Flash 解锁序列（0x5555/0x2AAA 写 + 探测 toggle bit）：完全由 host 编排
- 调试时复现示波器抓到的波形

FPGA 内部把 BUS_CYCLES 作为最高优先级原子事务，禁止与其它操作交错。

### 2.4.9 IRQ / 维护

| OP | Name | ADDR | HDR | R/W | DATA | 说明 |
|---|---|---|---|---|---|---|
| 0xC0 | IRQ_ACK | mask:8b | – | – | – | 清指定 flag |
| 0xC1 | IRQ_MASK | – | – | H→F 1B | enable mask | 哪些 flag 允许置 IRQ pin |
| 0xC8 | INVALIDATE_ALL | – | – | – | – | 丢弃 FPGA 内所有缓存/预取（必要时 host 切 bank 后用） |

### 2.4.10 调试

| OP | Name | 说明 |
|---|---|---|
| 0xE0 | SCOPE_CAPTURE | 启动一次内部逻辑分析仪采集（实现可选） |
| 0xE1 | SCOPE_READ | 读回 |

## 2.5 错误码

`status.last_err`：

| Code | 含义 |
|---|---|
| 0x00 | OK |
| 0x01 | BAD_CRC |
| 0x02 | BAD_OPCODE |
| 0x03 | BAD_ARG |
| 0x04 | REGION_VIOLATION（命令属性与 region 不匹配） |
| 0x05 | TIMEOUT_BUS（卡带未响应） |
| 0x06 | FIFO_OVERRUN |
| 0x07 | FIFO_UNDERRUN |
| 0x08 | LINK_DESYNC |
| 0x09 | NOT_PRESENT（无卡带） |

## 2.6 流控

- host → FPGA：FPGA RX FIFO 满则在 SCLK 上 stretch（如平台不支持，则 FPGA 拉 IO0 = 0 表示 BUSY，host 必须在每字节起始前检查；具体握手由 LINK_SPEED 协商时锁定）
- FPGA → host：READ 类如果 FIFO 空，FPGA 在该字节位置输出 0x00 同时置 `FIFO_UNDERRUN`，host 读到 underrun 后丢弃数据并重试

## 2.7 版本

`INFO` sub=0 返回：

```c
struct info_ver {
    u8  proto_major;       // 当前 1
    u8  proto_minor;       // 当前 0
    u8  gateware_major;
    u8  gateware_minor;
    u32 build_id;
};
```

`INFO` sub=1 返回 capability bitmap（哪些可选功能已实现，例如 BUS_CYCLES、SCOPE_CAPTURE、自动 hotplug）。

## 2.8 上电默认

- 频率 = 10 MHz
- Region 表：仅槽位 0 预配置为 /CS 全空间 STRICT（无优化）；其它槽位禁用。/CS2 需 host 显式 REGION_SET
- 所有 timing profile = "安全档"（见 [03-timing.md](03-timing.md)）
- IRQ mask = 0（全屏蔽）
- Cart power = 关
