# gpsp → ESP32-P4 移植可行性评估报告

> **日期**: 2026-04-02  
> **项目**: gpsp (Game Boy Advance 模拟器)  
> **目标平台**: ESP32-P4 (双核 RISC-V @ 400 MHz)  
> **评估范围**: Dynamic Recompilation + PPA 2D 硬件加速

---

## 1. 概述

本报告评估将 gpsp GBA 模拟器移植到 ESP32-P4 平台的技术可行性，重点关注两个核心目标：

1. **Dynamic Recompilation (Dynarec)** — 在 RISC-V 上实现 ARM7TDMI 动态重编译
2. **2D 硬件加速** — 利用 ESP32-P4 的 PPA (Pixel Processing Accelerator) 加速 GBA PPU 渲染

---

## 2. 硬件平台对比

| 项目 | GBA (被模拟目标) | ESP32-P4 (宿主平台) |
|---|---|---|
| CPU | ARM7TDMI @ 16.78 MHz | 双核 RISC-V @ 400 MHz |
| 指令集 | ARMv4T (ARM + Thumb) | RV32IMAFCZc + AI 扩展 |
| 片上 SRAM | 32 KB IWRAM + 256 KB EWRAM + 96 KB VRAM | 768 KB SRAM + 8 KB TCM |
| 外部内存 | 32 MB GamePak ROM (卡带) | PSRAM 8-32 MB (OPI 接口) |
| 2D 图形 | 专用 PPU 硬件 (Tile/Sprite/Affine/Blend) | PPA (缩放/旋转/镜像/混合/填充) |
| 显示接口 | 内置 240×160 LCD | MIPI-DSI (最高 1080p) |
| 音频 | 4ch GBC + 2ch DirectSound | I2S + DMA |

---

## 3. Dynamic Recompilation 可行性

### 3.1 现有 Dynarec 架构分析

gpsp 的 Dynarec 引擎位于 `cpu_threaded.c`，通过架构特定的 `*_emit.h` 头文件实现指令生成。现有后端：

| 目标架构 | Emit 文件 | Stub 文件 |
|---|---|---|
| ARM32 | `arm/arm_emit.h` (~3000+ 行) | `arm/arm_stub.S` |
| ARM64 | `arm/arm64_emit.h` | `arm/arm64_stub.S` |
| MIPS | `mips/mips_emit.h` | `mips/mips_stub.S` |
| x86 | `x86/x86_emit.h` | `x86/x86_stub.S` |
| **RISC-V** | **❌ 不存在** | **❌ 不存在** |

**核心结论：需要从零编写 RISC-V Dynarec 后端。**

### 3.2 需要新增的文件

```
riscv/
├── riscv_codegen.h    # RISC-V 指令编码宏 (R/I/S/B/U/J 型指令)
├── riscv_emit.h       # GBA ARM → RISC-V 翻译宏 (预计 3000-5000 行)
└── riscv_stub.S       # JIT 执行入口/退出桩代码 (预计 500-1000 行)
```

同时需修改：
- `cpu_threaded.c` — 添加 `RISCV_ARCH` 条件编译分支
- `Makefile.common` — 添加 RISC-V 源文件和编译选项
- `gpsp_config.h` — 添加 ESP32-P4 特定配置 (缓存大小等)

### 3.3 代码缓存配置

gpsp 提供两种缓存大小配置：

| 配置 | ROM 翻译缓存 | RAM 翻译缓存 | 适用场景 |
|---|---|---|---|
| 标准模式 | 10 MB | 512 KB | 性能优先 |
| 小缓存模式 (`SMALL_TRANSLATION_CACHE`) | 2 MB | 384 KB | **ESP32-P4 适用** |

Hash 表大小固定 65536 项 (`ROM_BRANCH_HASH_SIZE`)，每项 8 字节，约 512 KB。

### 3.4 寄存器映射方案

GBA ARM7TDMI 有 16 个通用寄存器 (R0-R15) + CPSR。RISC-V RV32 有 31 个通用寄存器 (x1-x31)，映射方案建议：

```
RISC-V 寄存器        →  用途
─────────────────────────────────────
s0 (x8)             →  GBA R0
s1 (x9)             →  GBA R1
s2-s7 (x18-x23)     →  GBA R6, R9, R12, R14 + 高频寄存器
s8 (x24)            →  GBA R15 (PC)
s9 (x25)            →  CPSR N flag
s10 (x26)           →  CPSR Z flag
s11 (x27)           →  CPSR C flag
t3 (x28)            →  CPSR V flag
gp (x3)             →  reg_base 指针 (GBA 寄存器文件基址)
tp (x4)             →  内存映射表指针
t0-t2, t4-t6, a0-a7 →  临时寄存器 / 函数调用
ra (x1)             →  返回地址
sp (x2)             →  栈指针 (保留)
```

RISC-V 寄存器数量充裕，可缓存 10-12 个 GBA 寄存器，与 ARM64 后端相当。

### 3.5 ARM → RISC-V 指令映射难点

| GBA ARM 操作 | RISC-V 映射难度 | 原因与方案 |
|---|---|---|
| 带进位加法 ADC | 🔴 高 | RISC-V 无 carry flag；需 `add` + `sltu` (检测进位) + 条件加法，约 4-6 条指令 |
| 带借位减法 SBC | 🔴 高 | 同上，需额外处理 borrow |
| 桶形移位器 (DP指令内嵌移位) | 🔴 高 | ARM 每条 DP 指令可附带移位操作，RISC-V 需拆分为 `sll`/`srl`/`sra` + DP 指令 |
| 条件执行前缀 | 🟡 中 | ARM 条件执行 → RISC-V `beq`/`bne` + 跳过指令块 |
| MUL / MLA | 🟢 低 | RISC-V M 扩展直接支持 `mul` |
| LDM/STM (多寄存器) | 🟡 中 | 展开为多条 `lw`/`sw`；注意：这是 gpsp 中代码膨胀最大的指令 (单条最多 2KB) |
| Thumb BL (长跳转) | 🟢 低 | 直接映射 `jal` |
| SWP (原子交换) | 🟢 低 | GBA 单核无需真原子，普通 load+store 即可 |
| MRS/MSR (状态寄存器) | 🟡 中 | 需要从拆分的 flag 寄存器重构/解构 CPSR |

**代码膨胀预估：** RISC-V 翻译后的代码体积约为 ARM32 后端的 1.3-1.8 倍（主要因为缺少条件执行和内嵌移位器）。

### 3.6 Cache 一致性

ESP32-P4 执行 JIT 生成的代码需要刷新指令缓存：

```c
// 写入 JIT 代码后，刷新 I-cache
asm volatile ("fence.i" ::: "memory");

// ESP-IDF 方式 (如有 API)
// esp_cache_msync(addr, size, ESP_CACHE_MSYNC_FLAG_INVALIDATE | ESP_CACHE_MSYNC_FLAG_ICACHE);
```

需要在 `cpu_threaded.c` 的 `translate_icache_sync()` 函数中添加 RISC-V 实现。

### 3.7 Dynarec 风险评估

| 风险项 | 级别 | 说明 |
|---|---|---|
| PSRAM 可执行性 | 🔴 严重 | ESP-IDF 默认不允许从 PSRAM 执行代码；需验证/修改 MMU 配置将 PSRAM 区域设为可执行 |
| PSRAM 取指延迟 | 🔴 严重 | PSRAM 延迟 ~10-20x 高于 SRAM；JIT cache miss 会严重拖慢性能 |
| Dynarec 开发量 | 🔴 高 | 预计 3000-5000 行新代码，涉及深入的 ARM 指令语义理解 |
| 代码膨胀 | 🟡 中 | RISC-V 翻译代码比 ARM32 大 30-80%，加速翻译缓存耗尽 |
| 调试难度 | 🔴 高 | Dynarec bug 表现为游戏随机崩溃，需要逐指令对比验证工具 |

---

## 4. 内存可行性分析

### 4.1 gpsp 内存需求总览

| 组件 | 大小 | 可否放 PSRAM | 备注 |
|---|---|---|---|
| ROM 翻译缓存 | 2-10 MB | ✅ 必须 | 小缓存模式 2 MB |
| RAM 翻译缓存 | 384-512 KB | ✅ | — |
| GamePak ROM 缓冲 | 1-32 MB (LRU 分页) | ✅ 必须 | 可配为 2-4 MB LRU 窗口 |
| GBA EWRAM | 256 KB | ✅ | — |
| GBA IWRAM | 32 KB | ⚠️ 建议SRAM | 高频访问 |
| GBA VRAM | 96 KB | ✅ | — |
| GBA Palette | 1 KB | ⚠️ 建议SRAM | 每像素查表 |
| GBA OAM | 1 KB | ⚠️ 建议SRAM | 精灵属性 |
| GBA IO 寄存器 | 1 KB | ⚠️ 建议SRAM | 高频读写 |
| Hash 表 + 元数据 | ~512 KB | ✅ | — |
| 音频缓冲 | 64 KB | ✅ | DMA 输出 |
| 帧缓冲 | ~77 KB | ✅ | 240×160×2 (RGB565) |

### 4.2 ESP32-P4 内存分配策略

```
┌─────────────────────────────────────────────────┐
│                 片上 SRAM (768 KB)               │
│  ┌───────────────────────────────────────────┐   │
│  │ ESP-IDF 系统保留         ~250 KB          │   │
│  ├───────────────────────────────────────────┤   │
│  │ GBA IWRAM + Palette + OAM + IO  ~36 KB   │   │
│  ├───────────────────────────────────────────┤   │
│  │ 音频环形缓冲              ~64 KB          │   │
│  ├───────────────────────────────────────────┤   │
│  │ 热路径数据结构/栈         ~150 KB         │   │
│  ├───────────────────────────────────────────┤   │
│  │ L1/L2 Cache 用途          ~268 KB         │   │
│  └───────────────────────────────────────────┘   │
├─────────────────────────────────────────────────┤
│                TCM (8 KB)                        │
│  │ 关键中断处理 / 音频回调                    │   │
├─────────────────────────────────────────────────┤
│              外部 PSRAM (建议 ≥16 MB)            │
│  ┌───────────────────────────────────────────┐   │
│  │ ROM 翻译缓存              2 MB            │   │
│  ├───────────────────────────────────────────┤   │
│  │ RAM 翻译缓存              384 KB          │   │
│  ├───────────────────────────────────────────┤   │
│  │ GamePak ROM 缓冲 (LRU)    4-8 MB          │   │
│  ├───────────────────────────────────────────┤   │
│  │ GBA EWRAM + VRAM          352 KB          │   │
│  ├───────────────────────────────────────────┤   │
│  │ Hash 表                   512 KB          │   │
│  ├───────────────────────────────────────────┤   │
│  │ 帧缓冲 (双缓冲)          ~154 KB         │   │
│  ├───────────────────────────────────────────┤   │
│  │ 可用余量                  ~8-13 MB        │   │
│  └───────────────────────────────────────────┘   │
└─────────────────────────────────────────────────┘
```

### 4.3 最低可行内存配置

| PSRAM 容量 | 可行性 | 说明 |
|---|---|---|
| 8 MB | ⚠️ 紧张 | 小缓存 + 2MB ROM 窗口，仅适合小 ROM 游戏 |
| 16 MB | ✅ 推荐 | 充裕运行大多数游戏 |
| 32 MB | ✅ 理想 | 可使用标准缓存模式 |

### 4.4 关键瓶颈：PSRAM 带宽竞争

```
PSRAM 带宽消耗者 (同时运行):
├── JIT 代码取指执行      ~高 (每秒百万次指令 fetch)
├── ROM 数据读取          ~中 (LRU miss 时 burst 读取)
├── 帧缓冲写入           ~低 (每帧 77 KB × 60fps = 4.6 MB/s)
├── PPA DMA 操作         ~中 (如启用硬件加速)
└── 音频 DMA             ~极低
```

ESP32-P4 PSRAM 通过 OPI (Octal SPI) 接口，典型带宽约 **200-400 MB/s**。上述负载总和约 50-100 MB/s，带宽本身不是瓶颈，**但随机访问延迟是瓶颈**（每次 cache miss ~100-200ns）。

---

## 5. PPA 2D 硬件加速可行性

### 5.1 ESP32-P4 PPA 能力

PPA 提供三类硬件加速操作：

| 操作 | API | 能力 |
|---|---|---|
| **SRM** (缩放/旋转/镜像) | `ppa_do_scale_rotate_mirror()` | 任意缩放 (1/16 步进)、0°/90°/180°/270° 旋转、XY 镜像 |
| **Blend** (混合) | `ppa_do_blend()` | 前景+背景 Alpha 混合、Color Key |
| **Fill** (填充) | `ppa_do_fill()` | 矩形区域纯色填充 |

支持的颜色格式：ARGB8888、RGB888、RGB565、YUV420/422/444、GRAY8、A8/A4

### 5.2 GBA PPU 操作 → PPA 映射分析

#### ✅ 可加速的操作

| GBA 操作 | PPA 映射 | 收益评估 |
|---|---|---|
| Backdrop 填充 | `ppa_do_fill()` (RGB565) | 🟢 低收益 — 仅占渲染极少比例 |
| 帧缓冲缩放 (240×160 → 显示分辨率) | `ppa_do_scale_rotate_mirror()` | 🟢 **高收益** — 最佳用例 |
| 画面旋转 (横竖屏切换) | `ppa_do_scale_rotate_mirror()` | 🟢 高收益 |
| 像素格式转换 (RGB565 → ARGB8888) | SRM 颜色模式转换 | 🟢 中等收益 |

#### ⚠️ 部分可加速 (需要适配)

| GBA 操作 | 限制 | 适配方案 |
|---|---|---|
| 仿射背景变换 (BG2/BG3 Mode 1/2) | PPA 仅支持 0°/90°/180°/270°，GBA 支持任意角度连续旋转 | ❌ **不匹配** — PPA 的旋转是离散的，无法替代 GBA 的 PA/PB/PC/PD 矩阵变换 |
| Alpha 混合 (半透明特效) | GBA: `Out = (A×EVA + B×EVB) >> 4`；PPA: 标准 Alpha 公式 | ⚠️ 需要将 EVA/EVB 系数转换为等效 Alpha 值，精度可能有差异 |
| 亮度调整 (Bright/Dark) | GBA: `Out = A + (W-A)×EVY/16` (变白) 或 `Out = A - A×EVY/16` (变暗) | ⚠️ 可用 blend 与纯白/黑图层模拟，但增加额外 buffer 和 DMA 开销 |

#### ❌ 不可加速的操作

| GBA 操作 | 原因 |
|---|---|
| **Tile 解码** (4bpp/8bpp 索引色 → RGB) | PPA 不支持调色板查找，这是纯查表操作 |
| **Sprite 渲染** (128 个 OAM 对象) | 每个精灵有独立的位置/大小/变换/优先级/模式，PPA 无法批量处理 |
| **窗口** (WIN0/WIN1/OBJWIN) | 逐像素遮罩判断，不是块操作 |
| **马赛克** | 像素重复采样，PPA 不支持 |
| **精灵优先级排序** | 128 个精灵的 Z-order 依赖 OAM 属性，纯逻辑操作 |
| **半透明精灵 (OBJ Alpha)** | 需要与背景逐像素混合，精灵形状不规则 |

### 5.3 渲染管线加速比例估算

```
GBA 渲染管线 CPU 时间分布 (典型游戏):

┌─────────────────────────────────────────────┐
│ Tile 解码 + 绘制背景层          ~40%       │ ← PPA 不可加速
│ Sprite 解码 + OAM 处理          ~25%       │ ← PPA 不可加速
│ 窗口 / 优先级 / 特效合成        ~15%       │ ← PPA 不可加速
│ Alpha 混合 / 亮度调整           ~10%       │ ← PPA 部分可加速
│ 仿射变换 (Mode 1/2 游戏)       ~8%        │ ← PPA 不匹配
│ Backdrop / 其他                 ~2%        │ ← PPA 可加速
└─────────────────────────────────────────────┘

PPA 可实际加速的核心渲染比例: ~5-12%
PPA 用于后处理(缩放/旋转)的收益: 独立于渲染管线，额外收益
```

### 5.4 推荐的 PPA 使用策略

```
不推荐: 用 PPA 替代 GBA PPU 核心渲染
    原因: GBA 渲染是逐扫描线 (每行 240 像素), PPA 是块操作,
          调用开销 > 软件渲染开销 (对于这么小的数据量)

推荐: 用 PPA 做帧后处理
    ┌─────────────────┐     ┌──────────────────┐     ┌─────────────────┐
    │ CPU 软件渲染     │────→│ 240×160 RGB565   │────→│ PPA 缩放/旋转    │
    │ (GBA PPU 模拟)  │     │ 帧缓冲 (PSRAM)   │     │ → 显示分辨率     │
    └─────────────────┘     └──────────────────┘     └─────────────────┘
                                                            │
                                                            ▼
                                                     ┌─────────────────┐
                                                     │ MIPI-DSI 输出    │
                                                     │ (DMA + LCD 控制) │
                                                     └─────────────────┘
```

---

## 6. 性能预测

### 6.1 CPU 性能估算

| 模式 | 预计性能 | 计算依据 |
|---|---|---|
| **纯解释器** | ~60-80% 全速 | GBA ~16.78 MIPS；每条 ARM 指令需 ~15-25 条 RISC-V；400 MHz / 20 ≈ 20 MIPS；考虑 PSRAM 延迟后 ~12-15 MIPS |
| **Dynarec (乐观)** | ~100%+ 全速 | Dynarec 加速比 3-8x，PSRAM cache 命中率 > 90% |
| **Dynarec (悲观)** | ~60-80% 全速 | PSRAM 取指延迟严重，cache 命中率 < 80% |

### 6.2 关键性能变量

1. **PSRAM 取指 cache 命中率** — 决定 Dynarec 是否有效的第一要素
2. **JIT 代码膨胀率** — RISC-V 翻译代码越大，cache 压力越大
3. **游戏复杂度** — 简单 2D 游戏 vs. 使用大量仿射/混合特效的游戏差异巨大
4. **双核利用率** — 音频/渲染分离到 Core1 可释放 ~15-25% CPU 时间

### 6.3 与其他平台的对比参考

| 平台 | CPU | 频率 | gpsp Dynarec | 性能 |
|---|---|---|---|---|
| PSP (MIPS) | Allegrex | 333 MHz | MIPS 后端 ✅ | ~100% 全速 |
| 3DS (ARM) | ARM11 | 268 MHz | ARM 后端 ✅ | ~100% 全速 |
| ESP32-P4 (RISC-V) | RISC-V | 400 MHz | ❌ 需新建 | **待验证** |

ESP32-P4 频率高于 PSP/3DS，但 RISC-V 翻译效率 < native ARM 翻译，且 PSRAM 延迟远高于 PSP/3DS 的片上 SRAM。

---

## 7. 实施路线图

### Phase 0: 平台可行性验证 (建议最先完成)

- [ ] **验证 PSRAM 代码执行** — 在 ESP-IDF 上测试从 PSRAM 区域执行动态生成代码的可行性
- [ ] **验证 `fence.i` 行为** — 确认 I-cache 刷新对 PSRAM 区域有效
- [ ] **测量 PSRAM 取指延迟** — 与片上 SRAM 取指对比，建立性能基线
- [ ] **测量 PSRAM 带宽饱和点** — 同时开 PPA DMA + CPU 取指时的带宽竞争

> ⚠️ **如果 Phase 0 任何一项验证失败，整个 Dynarec 方案不可行，应回退到纯解释器方案。**

### Phase 1: 解释器移植 (基础可玩)

- [ ] 移植 gpsp 解释器模式 (`HAVE_DYNAREC=0`) 到 ESP-IDF
- [ ] 实现 libretro 前端适配层或独立前端
- [ ] 实现 MIPI-DSI 显示输出 (240×160 → 屏幕分辨率，使用 PPA 缩放)
- [ ] 实现 I2S 音频输出
- [ ] 实现输入 (GPIO/I2C 按键)
- [ ] 实现 ROM 加载 (SPI Flash / SD 卡)
- [ ] **基准测试** — 测量解释器模式下的帧率

### Phase 2: RISC-V Dynarec 后端 (性能提升)

- [ ] 编写 `riscv/riscv_codegen.h` — RISC-V 指令编码宏
- [ ] 编写 `riscv/riscv_emit.h` — ARM/Thumb → RISC-V 翻译层
- [ ] 编写 `riscv/riscv_stub.S` — JIT 执行入口/退出
- [ ] 修改 `cpu_threaded.c` — 添加 `RISCV_ARCH` 条件编译
- [ ] 编写逐指令验证测试 (对比解释器输出 vs Dynarec 输出)
- [ ] 性能调优 — 寄存器分配优化、热路径内联

### Phase 3: PPA 集成与优化 (画面增强)

- [ ] 实现 PPA 帧缓冲缩放 (240×160 → 显示分辨率)
- [ ] 实现 PPA 画面旋转 (横竖屏切换)
- [ ] 实现 PPA RGB565 → ARGB8888 格式转换 (如显示器需要)
- [ ] 实现双缓冲 + PPA 异步缩放管线
- [ ] 评估 PPA Alpha 混合用于 GBA 半透明特效的可行性

### Phase 4: 双核优化 (性能最大化)

```
Core 0 (HP):                    Core 1 (HP):
┌──────────────────────┐       ┌──────────────────────┐
│ CPU 模拟 (Dynarec)   │       │ 音频渲染              │
│ 内存读写模拟         │       │ PPA 后处理缩放        │
│ PPU 渲染 (软件)      │  ──→  │ 帧缓冲 DMA 到 LCD    │
│ 定时器/中断模拟      │ 同步  │ 输入轮询              │
└──────────────────────┘       └──────────────────────┘
```

---

## 8. 风险矩阵

| ID | 风险 | 影响 | 可能性 | 缓解措施 |
|---|---|---|---|---|
| R1 | PSRAM 不可执行代码 | 🔴 致命 | 🟡 中 | Phase 0 最先验证；备选方案：仅用 SRAM 做微型 JIT 缓存 (~200 KB) |
| R2 | PSRAM 取指太慢导致 Dynarec 无收益 | 🔴 严重 | 🟡 中 | 优化 JIT 代码紧凑度、提高 cache 命中率；备选：纯解释器 + overclock |
| R3 | RISC-V Dynarec 开发周期过长 | 🟡 高 | 🔴 高 | 参考现有 RISC-V JIT 实现 (rv32emu, QEMU TCG)；考虑 threaded interpreter 作为中间方案 |
| R4 | 内存不足 (8MB PSRAM) | 🟡 中 | 🟡 中 | 使用 `SMALL_TRANSLATION_CACHE` + 激进 LRU；建议 ≥16 MB PSRAM |
| R5 | PPA + CPU 同时访问 PSRAM 导致带宽饱和 | 🟡 中 | 🟢 低 | PPA 仅用于帧间后处理，避免与 CPU 渲染同时 DMA |
| R6 | Dynarec bug 导致游戏不兼容 | 🟡 中 | 🔴 高 | 逐指令对比测试框架；保留解释器模式回退 |

---

## 9. 替代方案

### 方案 A: Threaded Interpreter (推荐作为中间步骤)

不做传统 Dynarec，而是将每条 ARM 指令编译为函数指针表（间接线程化解释器）。在 RISC-V 上这种方案的性能约为纯解释器的 1.5-2x，且开发量远小于完整 Dynarec。

### 方案 B: 仅解释器 + 极致优化

利用 RISC-V 400 MHz 的原始频率优势，通过以下手段追求解释器全速：
- SIMD 扩展 (如 ESP32-P4 的 AI 指令) 加速像素处理
- 热循环用手写汇编优化
- 精确的内存时序省略（HLE 模式）
- 双核并行（CPU 模拟 + PPU 渲染分离）

### 方案 C: 微型 SRAM JIT 缓存

仅使用片上 SRAM 的一小部分 (~128-256 KB) 作为 JIT 缓存，避免 PSRAM 取指问题。缓存较小会导致频繁刷新，但从 SRAM 取指零延迟。适合热代码集中的游戏。

---

## 10. 结论

| 评估维度 | 结论 |
|---|---|
| **整体可行性** | ⚠️ **有条件可行** — 技术上无根本性障碍，但开发量大、性能存在不确定性 |
| **Dynarec** | 技术可行但为该项目的**最大工程挑战**；PSRAM 可执行性是前提条件 |
| **PPA 2D 加速** | 对核心渲染**收益有限 (~5-12%)**；**最佳用途是帧后处理缩放** |
| **内存** | 需要 ≥16 MB PSRAM；小缓存模式可降至 8 MB |
| **建议起步** | 先完成 Phase 0 (PSRAM 执行验证) + Phase 1 (解释器移植)，再决定 Dynarec 投入 |
| **最大风险** | PSRAM 代码执行的性能表现决定了 Dynarec 方案的成败 |

---

## 附录 A: gpsp 源码结构速览

```
gpsp/
├── cpu.cc / cpu.h            # CPU 模拟核心 (解释器)
├── cpu_threaded.c            # Dynarec 框架 (翻译 + 执行)
├── gba_memory.c / .h         # 内存子系统 (读写分派, ROM LRU)
├── video.cc / .h             # PPU 渲染 (纯软件, 模板化)
├── sound.c / .h              # 音频混合 (GBC 4ch + DirectSound 2ch)
├── input.c / .h              # 输入处理
├── savestate.c / .h          # 存档
├── common.h                  # 通用定义 / 平台抽象
├── gpsp_config.h             # 编译配置 (缓存大小等)
├── arm/                      # ARM32/64 Dynarec 后端
├── mips/                     # MIPS Dynarec 后端
├── x86/                      # x86 Dynarec 后端
├── libretro/                 # libretro 前端接口
└── bios/                     # 开源 BIOS 实现
```

## 附录 B: 关键数据结构

```c
// 翻译缓存 (gpsp_config.h)
#define ROM_TRANSLATION_CACHE_SIZE  (1024 * 1024 * 2)   // 小缓存: 2 MB
#define RAM_TRANSLATION_CACHE_SIZE  (1024 * 384)         // 384 KB
#define ROM_BRANCH_HASH_SIZE        (1024 * 64)          // 65536 项

// 块查找哈希 (cpu_threaded.c)
typedef struct {
    u32 pc_value;     // GBA 程序计数器
    u32 next_entry;   // 链表下一项偏移
} hashhdr_type;

// ROM 分页 LRU (gba_memory.c)
u8 *gamepak_buffers[32];           // 最多 32 个 1MB 页
struct {
    u16 next_lru;
    s16 phy_rom;                   // -1 = 未映射
} gamepak_blk_queue[1024];

// 屏幕缓冲 (common.h)
#define GBA_SCREEN_WIDTH   240
#define GBA_SCREEN_HEIGHT  160
#define GBA_SCREEN_BUFFER_SIZE  (240 * 161 * sizeof(uint16_t))
```

## 附录 C: ESP32-P4 PPA API 速查

```c
#include "driver/ppa.h"

// 1. 注册客户端
ppa_client_config_t srm_config = { .oper_type = PPA_OPERATION_SRM };
ppa_client_handle_t srm_client;
ppa_register_client(&srm_config, &srm_client);

// 2. 缩放操作 (240×160 → 480×320, 2x放大)
ppa_srm_oper_config_t srm_op = {
    .in = {
        .buffer = gba_framebuffer,
        .pic_w = 240, .pic_h = 160,
        .block_w = 240, .block_h = 160,
        .block_offset_x = 0, .block_offset_y = 0,
        .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
    },
    .out = {
        .buffer = display_framebuffer,
        .buffer_size = 480 * 320 * 2,
        .pic_w = 480, .pic_h = 320,
        .block_offset_x = 0, .block_offset_y = 0,
        .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
    },
    .scale_x = 2.0f, .scale_y = 2.0f,
    .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
    .mode = PPA_TRANS_MODE_BLOCKING,
};
ppa_do_scale_rotate_mirror(srm_client, &srm_op);

// 3. 混合操作 (前景 + 背景)
ppa_client_config_t blend_config = { .oper_type = PPA_OPERATION_BLEND };
ppa_client_handle_t blend_client;
ppa_register_client(&blend_config, &blend_client);

ppa_blend_oper_config_t blend_op = {
    .in_bg = { /* 背景图层配置 */ },
    .in_fg = { /* 前景图层配置 */ },
    .out = { /* 输出缓冲配置 */ },
    .fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
    .fg_alpha_fix_val = 128,  // 50% 透明度
    .mode = PPA_TRANS_MODE_BLOCKING,
};
ppa_do_blend(blend_client, &blend_op);
```
