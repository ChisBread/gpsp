# 03 · Timing（Waitstate）模型

## 3.1 概念

桥接器有两种"waitstate"：

| 层 | 谁负责 | 单位 | 用途 |
|---|---|---|---|
| **仿真层 waitstate** | host (gpsp) | GBA cycle (59.6ns) | 影响 GBA CPU 周期计数（WAITCNT 寄存器仿真） |
| **物理层 timing** | FPGA | 纳秒 | 决定真卡带 strobe 实际波形 |

本协议**只规定物理层 timing**。仿真层 waitstate 是 host 的事，与 QSPI 链路无关。

注意两者数值上经常相似（毕竟物理时序定义了 GBA CPU 看到的真实最小读取时间），
但概念上必须解耦：host 可以让"仿真层报 4 cycle"但"物理层只用 90ns"，反之亦然。

## 3.2 GBA WAITCNT 对照（参考）

| WAITCNT 字段 | 含义 | 默认 cycle | ns @16.78MHz |
|---|---|---|---|
| WS0_N (bits 2-3) | ROM 0x08000000 首次/非顺序 | 4 | 238 |
| WS0_S (bit 4)    | ROM 0x08000000 顺序 | 2 | 119 |
| WS1_N (bits 5-6) | ROM 0x0A000000 首次 | 4 | 238 |
| WS1_S (bit 7)    | ROM 0x0A000000 顺序 | 4 | 238 |
| WS2_N (bits 8-9) | ROM 0x0C000000 首次 | 4 | 238 |
| WS2_S (bit 10)   | ROM 0x0C000000 顺序 | 8 | 477 |
| SRAM (bits 0-1)  | SRAM 0x0E000000 | 4 | 238 |

cycle 计数公式：N + S = `t_addr_setup + t_strobe_low + t_recover` ≈ 周期总长。

GBA WAITCNT N/S 字段值（n=0..3 对应 4/3/2/8 cycle，详见 GBATEK），物理 ns 值是
host 配置 timing profile 时按目标卡带容忍度选择的，**不是** WAITCNT 的直接换算。

## 3.3 Timing Profile 数据结构

最多 8 组 profile，每组 16 字节：

```c
struct timing_profile {            // 16 bytes, idx 0..7
    u16 t_addr_setup_ns;           // AD/A 有效 → strobe assert
    u16 t_strobe_low_first_ns;     // strobe 低脉宽 - 首次访问 (~ N cycle)
    u16 t_strobe_low_seq_ns;       // strobe 低脉宽 - 顺序后续 (~ S cycle)
    u16 t_strobe_recover_ns;       // strobe 抬高 → 下次拉低最短间隔
    u16 t_data_hold_ns;            // strobe 抬高前的数据采样窗
    u16 t_cs_to_strobe_ns;         // /CS assert → /RD or /WR assert
    u16 t_strobe_to_cs_ns;         // strobe deassert → /CS deassert
    u8  flags;                     // bit0: 启用，其余保留
    u8  reserved;
};
```

**FPGA 用 100MHz 系统时钟（10ns/tick）四舍五入存储**。host 设的 ns 不一定能整除，
FPGA 取 `ceil(ns/10)`，保证不少于请求时长。

## 3.4 Profile 与 Region 的绑定

每个 region（[04-regions.md](04-regions.md)）携带一个 `timing_idx`，访问该 region 的总线周期使用对应 profile。

例外：

- `BUS_CYCLES` 命令默认无 profile，但内部 `LOAD_TIMING` 子操作可临时切
- `STRICT` 命令仍使用所属 region 的 profile（STRICT 不改变时序，只禁用优化）

## 3.5 N vs S 周期判定（FPGA 内部）

FPGA 在 burst 中判定下一周期是否能用 S（顺序）：

1. 命令是 `RD_BURST`/`WR_BURST`/`RD_STREAM` 之一
2. 上一周期同 region、同 channel
3. 地址 = 上一地址 + 通道宽度（/CS: +2, /CS2: +1）
4. 未越过 region 边界
5. 未越过 GBA 物理边界（A[23:16] 不变 — GBA 卡带在 64KB 边界 A16 翻转时必须 N）

任一条件不满足 → 下一周期回到 N（用 `t_strobe_low_first_ns`）。

`STRICT` 系列命令永远使用 N（即使语义上"连续"也算独立事务）。

## 3.6 安全 / 标准 / 激进档预设

启动时 FPGA 默认 8 个 profile 全部填"安全档"，host 应用前应至少配置一个。

| 档 | t_addr_setup | t_strobe_low_first | t_strobe_low_seq | t_recover | t_data_hold |
|---|---|---|---|---|---|
| 安全 | 30 ns | 240 ns | 180 ns | 30 ns | 20 ns |
| 标准 | 20 ns | 180 ns | 120 ns | 20 ns | 15 ns |
| 激进 | 15 ns | 120 ns | 90 ns | 15 ns | 10 ns |
| GBA WS0=3,S=1 | 20 ns | 178 ns | 119 ns | 20 ns | 15 ns |

**这些只是 host 端建议预设，协议本身不规定**。FPGA 上电后所有 profile 字段填"安全档"。

## 3.7 时序错误处理

如果 host 设 `t_strobe_low_first_ns < 50ns`（FPGA 视为不合理）→ FPGA 拒绝写入，
返回 `BAD_ARG`。下限值由 `INFO` capability 字段公开。

如果卡带在配置时序下超时（采样时 AD bus 仍漂浮 / 弱拉），置 `TIMEOUT_BUS`，
host 应升档到更保守 profile 重试。

## 3.8 链路速度协商（QSPI sclk）

`LINK_SPEED` 命令的 freq_code：

| code | freq |
|---|---|
| 0 | 10 MHz |
| 1 | 20 MHz |
| 2 | 40 MHz |
| 3 | 60 MHz |
| 4 | 80 MHz |

host 应在改频前发 `RESET_LINK`、改频后立即发 `SYNC` 验证；若 SYNC 失败则降档。

## 3.9 测量端口（可选）

`INFO` capability 若公布 `HAS_TIMING_MONITOR`：FPGA 可在每次事务后更新一组只读寄存器，
报告"实际观察到的 strobe 时长"，方便 host 确认 profile 真的生效（用于自动调优）。
具体地址通过 `INFO` sub=2 返回，本文档不强制实现。
