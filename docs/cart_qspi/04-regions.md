# 04 · Region 属性系统

Region 是 host 向 FPGA 声明"这段地址空间允许什么程度的优化"。
**默认严格透明（STRICT），任何优化必须显式 opt-in**。

## 4.1 数据结构

最多 16 个 region 槽，每槽 16 字节（`REGION_SET`）：

```c
struct region_desc {                   // 16 bytes
    u32 base;                          // /CS: 24-bit 字节地址；/CS2: 16-bit 字节地址
    u32 size;                          // 字节数；0 = 槽位禁用
    u8  channel;                       // 0=/CS (16-bit bus), 1=/CS2 (8-bit bus)
    u8  timing_idx;                    // 引用 [03-timing.md] profile 0..7
    u8  attr;                          // 见下文位定义
    u8  reserved0;
    u32 reserved1;
};
```

**通道决定访问宽度**（见 [01-overview.md §1.2](01-overview.md)），region 不再独立携带 width 字段：

| channel | 总线宽度 | 地址空间 | base 对齐要求 | size 对齐要求 |
|---|---|---|---|---|
| 0 (/CS)  | 16-bit | 24-bit (16 MiB) | 偶数 | 偶数 |
| 1 (/CS2) | 8-bit  | 16-bit (64 KiB) | 任意 | 任意 |

未对齐 → FPGA 拒绝写入，返回 `BAD_ARG`。


## 4.2 Attr 位定义

```
bit  含义
 0   READ_BURST_OK        允许 RD_BURST 命令访问本 region（用顺序 S 时序）
 1   READ_STREAM_OK       允许 RD_STREAM 命令（FPGA 可主动 prefetch）
 2   READ_CACHE_OK        FPGA 行缓存可保留并复用本 region 的读结果
 3   WRITE_BURST_OK       允许 WR_BURST 命令
 4   WRITE_MERGE_OK       连续 WR_STRICT 在内部允许合并到同一 strobe burst
 5   READ_HAS_SIDE_EFFECT 读本身改变设备状态（强制单次 strobe，禁所有 read 优化）
 6   WRITE_HAS_SIDE_EFFECT 写本身是命令（禁 merge，BARRIER 默认开）
 7   BANK_CONTROL          写本 region 后 FPGA 必须 INVALIDATE_ALL 自动执行
```

**约束**：

- bit 5 与 bit 0/1/2 互斥（FPGA 拒绝同时设置）
- bit 6 与 bit 3/4 互斥
- bit 7 蕴含 bit 6
- BURST/STREAM/CACHE 仅在 bit 5 = 0 时生效

## 4.3 默认 region

槽位 0 是**默认 region**，匹配所有未被其它槽覆盖的地址。上电时默认槽位 0 配置：

```
base=0, size=0x01000000, channel=0, timing_idx=0(安全档),
attr = 0  (即 STRICT, 无任何优化)

注：默认仅覆盖 /CS 通道。/CS2 通道的默认 region 在槽位 0 之外通过 host 显式 REGION_SET 建立；
未配置 /CS2 region 时访问 /CS2 命令返回 REGION_VIOLATION。
```

可被 host 覆写。

## 4.4 匹配规则

地址 `A`、通道 `ch` 的 region 选择：

1. 遍历槽位 1..15，找首个满足 `base ≤ A < base+size && channel == ch && size > 0` 的槽
2. 没找到 → 用槽位 0（默认）
3. 跨 region 的 burst/stream → 在边界处停止，置 ERR

## 4.5 命令 vs Attr 矩阵

| 命令 | 要求的 attr | 不满足时 |
|---|---|---|
| RD\*_STRICT, WR\*_STRICT | 无（永远允许） | – |
| RD_BURST | bit 0 | REGION_VIOLATION |
| RD_STREAM | bit 1 | REGION_VIOLATION |
| WR_BURST | bit 3 | REGION_VIOLATION |
| WRITE_LIST 单条 flags=0 | bit 4 才可与相邻条目合并 | 单独 strobe |
| BUS_CYCLES | 无（绕过 region 系统） | – |

## 4.6 烧录卡 / Bank-switch 场景

典型 SuperCard 工作流：

```
0. host 配置：
   region 1: base=0x09FFFFFE, size=2, attr=BANK_CONTROL|WRITE_HAS_SIDE_EFFECT
             (魔法寄存器位置，写后切 SDRAM/SD/Flash)
   region 2: base=0x08000000, size=0x02000000, attr=READ_BURST_OK|READ_CACHE_OK
             (当前 bank 暴露的 ROM-like 区)
   ... 其它 SD 控制寄存器同样标 SIDE_EFFECT

1. 切 bank：
   host 发 WR_CS_STRICT(0x09FFFFFE, magic_value)
   FPGA 因 BANK_CONTROL 自动执行 INVALIDATE_ALL，丢弃 region 2 的所有缓存
   随后用 BARRIER 等待 cart 静默

2. 之后访问 0x08000000 段，FPGA 重新预取/缓存，但拿到的是新 bank 的内容
```

如果不知道烧录卡的"魔法地址"在哪，最保守做法：

```
region 0 默认: attr=0 (全 STRICT)
仅当确定某段是普通 mask ROM 时，再加 region 标 BURST/CACHE_OK
```

## 4.7 Side-effect 区域

某些卡带读取本身有副作用（如 EZ-Flash 的 SD 数据 FIFO 读取，每次读自动 pop）。

```
region: attr=READ_HAS_SIDE_EFFECT|WRITE_HAS_SIDE_EFFECT
```

效果：

- 任何 RD_BURST/RD_STREAM 命中 → REGION_VIOLATION
- RD_STRICT 系列正常工作（host 必须用它们）
- FPGA 不缓存任何字节

host 想"批量读 FIFO"时，用 `READ_LIST` 命令 — 它由多条独立 strict 读组成（每条单独 strobe），但通过一个 QSPI 事务下发以摊销链路开销。

## 4.8 Hot reconfiguration

`REGION_SET` 任何时候都可调用。FPGA 行为：

1. 若该槽 attr 中 CACHE_OK 即将关闭 → 隐式 INVALIDATE 该 region
2. 若 base/size 变化 → 同上
3. 若仅 timing_idx 变化 → 仅切时序，不动缓存

`REGION_RESET` 清空所有槽 + 全部 INVALIDATE_ALL，槽 0 回到默认。

## 4.9 调试辅助

`INFO` sub=3（capability 公布 `HAS_REGION_STATS` 时）返回每槽统计：

```c
struct region_stats {     // per slot
    u32 hits;             // 总访问次数
    u32 cache_hits;       // 行缓存命中
    u32 prefetch_bytes;
    u32 violations;
};
```

便于 host 调优 region 划分。

## 4.10 不变量总结

- **未配置即 STRICT**：任何优化都必须显式 opt-in
- **STRICT 命令永远可用**：覆盖任何 region 设置
- **属性冲突拒绝写入**：FPGA 校验 attr 位组合，非法返回 BAD_ARG
- **Bank 写自动失效缓存**：BANK_CONTROL 是单向"写穿透 + 全失效"操作
- **Region 边界硬停**：burst/stream 不会越过 region 边界，避免误命中相邻 SIDE_EFFECT 区
