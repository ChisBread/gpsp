# gpsp RISC-V 动态重编译器实现计划

## 当前状态总览

| 模块 | 当前状态 | 完成度 |
|------|---------|-------|
| `riscv_codegen.h` | 指令编码、立即数加载、回填辅助、ROR 合成已可用 | **95%** |
| `riscv_emit.h` | 已接入 `cpu_threaded.c` 期望接口，承担主要 emit/runtime helper 逻辑 | **75%** |
| `riscv_stub.S` | 已具备入口/出口、寄存器装载回写、主循环、基础 helper | **70%** |
| 构建集成 | 已接入 ESP32-P4 / ESP-IDF dynarec 构建链，完整固件可编译链接 | **90%** |
| 硬件验证 | 尚未上板，无真实运行证据 | **0%** |

当前结论：
- RISC-V dynarec 已经不是“从零开始”的状态。
- 当前代码能在 ESP32-P4 配置下完整通过 `idf.py build`，并生成 `build/gpsp-esp32p4.bin`。
- translation cache 目前静态放在 `.ext_ram.bss`，即 ESP32-P4 的 PSRAM 路径。
- 结构上保持与其它后端一致：当前不再引入独立 `riscv_runtime.c`，runtime helper 继续保留在 `riscv_emit.h`。

---

## 关键结论

### 1. 早期“`riscv_emit.h` 接口完全错误”的结论已过时
当前 `riscv_emit.h` 已切换到与其它后端一致的 `generate_*` / `arm_*` / `thumb_*` 宏接口风格，能够支撑 `cpu_threaded.c` 的 RISC-V 分支编译。

### 2. 当前最大风险不再是“编不过”，而是“语义是否正确”
构建、链接、入口、cache 符号、helper 暴露这些基础问题已经基本打通；剩余风险集中在：
- CPSR/SPSR 与中断时序
- 条件执行与 flags 边界行为
- ARM/Thumb 切换与异常返回
- block linking 与间接跳转一致性
- 访存、未对齐、cache 失效路径

### 3. 不默认依赖 Zbb 仍然是正确策略
ESP32-P4 公共 ISA 信息不足以证明硬件支持 Zbb，因此 rotate / bit-manip 继续用 RV32IMAC 序列合成，避免“能汇编但上板非法指令”的风险。

### 4. `gp` / `tp` 挪用依旧是架构风险点
JIT 与 C helper 交互路径必须保证保存/恢复完整，否则上板后容易出现偶发错误，尤其是内存访问 helper、更新回调、异常路径。

---

## 已完成里程碑

### 里程碑 A：RISC-V 后端进入主构建链
- [x] `cpu_threaded.c` 已存在 `RISCV_ARCH` 接入路径
- [x] `components/gpsp_core/CMakeLists.txt` 已在 `CONFIG_GPSP_DYNAREC` + `CONFIG_IDF_TARGET_ESP32P4` 下编入 `cpu_threaded.c` 与 `riscv/riscv_stub.S`
- [x] `sdkconfig` / `sdkconfig.defaults` 已启用 dynarec
- [x] 完整 ESP-IDF 固件构建通过

### 里程碑 B：基础 codegen 与 emit 框架可用
- [x] `rv_load_imm32` 符号扩展 bug 已修复
- [x] 已提供不依赖 Zbb 的 ROR / RORI 合成
- [x] `generate_load_reg()` / `generate_store_reg()` / `generate_load_reg_pc()` / `generate_load_imm()` / `generate_load_pc()` 已接通
- [x] `generate_cycle_update()` / `generate_branch_*()` / `generate_translation_gate()` 已接通
- [x] ARM/Thumb 基础 ALU、分支、访存、SWI、HLE div 宏簇已具备基本实现

### 里程碑 C：stub 与 dynarec 运行骨架可用
- [x] `execute_arm_translate_internal()` 已存在并可链接
- [x] GBA 寄存器装载 / 回写已接通
- [x] CPSR 与 NZCV 的基础装载/回写已接通
- [x] `rv_update_gba` / `rv_indirect_branch_*` / `execute_store_cpsr` / `execute_spsr_restore` / `execute_swi` 等 helper 已存在
- [x] dynarec 需要的全局状态对象与 translation cache 符号已补齐

### 里程碑 D：内存放置与平台整合已确认
- [x] `rom_translation_cache` / `ram_translation_cache` 当前位于 `.ext_ram.bss`
- [x] 对 ESP32-P4 来说，这意味着 translation cache 当前走 PSRAM 分配路径
- [x] 当前方案至少满足“可构建、可链接、镜像可生成”

---

## 剩余高风险项

### P0：真实硬件运行正确性
- [ ] 尚未验证是否能稳定进入 dynarec 主循环
- [ ] 尚未验证 translation cache 在目标板上的实际执行行为
- [ ] 尚未验证 `fence.i` 与 ESP32-P4 指令缓存行为是否完全匹配预期

### P1：CPU 语义正确性
- [ ] flags 边界语义仍需系统回归，尤其是 ADC/SBC/RSC 与 shift carry
- [ ] 条件执行复合条件仍需 ROM 级验证
- [ ] CPSR/SPSR、异常返回、中断注入时序仍需硬件验证
- [ ] ARM/Thumb 切换和 `BX` / `SWI` / `LDM^` 类路径需重点验证

### P2：块管理与跳转稳定性
- [ ] block lookup / indirect branch / re-lookup 逻辑需要运行时验证
- [ ] block chaining 是否会出现错误回填或错误重入尚未知
- [ ] self-modifying code / translation cache invalidation 仍缺乏压力验证

### P3：性能与平台副作用
- [ ] 频繁 JIT→C helper 调用的开销还未评估
- [ ] PSRAM 上 translation cache 的延迟与吞吐尚未评估
- [ ] 尚未确认 dynarec 相对解释器模式是否稳定获得收益

---

## 分阶段收尾计划

### Phase 1：上板前静态收敛
- [x] 后端进入完整构建链
- [x] 关键 helper、cache、符号与链接问题已打通
- [x] 不再额外拆出 `riscv_runtime.c`，保持与其它后端一致
- [ ] 再补一轮基于 host 的 flags / branch / memory helper 回归

### Phase 2：最小可运行硬件验证
- [ ] 上板后先验证是否能进入 dynarec 而非立即崩溃
- [ ] 验证 BIOS 初始化、logo、主菜单等最短路径 ROM
- [ ] 为 `execute_arm_translate_internal()`、`rv_update_gba`、间接跳转失败路径加最小日志或计数器

### Phase 3：语义回归
- [ ] 使用最小 ROM 或指令测试覆盖 data-processing、branch、load/store、PSR
- [ ] 用 Thumb-heavy 游戏或测试 ROM 覆盖 Thumb 路径
- [ ] 对比解释器结果，定位第一批错误指令簇

### Phase 4：稳定性与性能
- [ ] 压测长时间运行、savestate、切换 ROM、菜单返回等路径
- [ ] 评估 translation cache 大小、PSRAM 放置、helper 调用频率
- [ ] 再决定是否需要更激进的结构优化或 helper 下沉

---

## 验证策略

1. Host 侧继续保留 codegen / emit / stub 的最小回归，确保补丁不会把链接层重新打坏。
2. 上板后第一优先级不是跑商业游戏，而是确认 dynarec 是否能进入、执行、返回，不发生非法指令或立即死机。
3. 第二优先级是 BIOS 和短路径 ROM，先验证 PC 流、异常、访存基本语义。
4. 第三优先级再看商业 ROM 的 ARM/Thumb 混合路径、长时间运行与稳定性。
5. 性能评估放在正确性之后，当前阶段不应为了性能先改复杂结构。

---

## 当前可用性评估

从代码和构建状态看，当前更适合定义为：

- “已完成集成与可构建性验证”
- “尚未完成硬件可用性验证”
- “大概率还需要 1 到 3 轮上板修正”

在开发板未到之前，不应把它视为“可用 dynarec”，更准确的定位是“具备上板验证前提的候选实现”。

---

## 下一步

### 板子未到前
- [ ] 补最值钱的 host 侧回归：flags、条件执行、CPSR/SPSR、memory helper
- [ ] 在关键入口预留轻量调试点，便于上板后快速判断卡死位置
- [ ] 保持当前结构稳定，不再做大的文件拆分或风格调整

### 板子到后
- [ ] 先验证是否能进入 dynarec 主循环
- [ ] 再验证 BIOS / logo / 简单 ROM
- [ ] 根据首个失败点决定优先修正 emit、stub、还是 memory / PSR 路径
