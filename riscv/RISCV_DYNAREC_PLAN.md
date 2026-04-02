# gpsp RISC-V 动态重编译器实现计划

## 当前状态总览

| 文件 | 参考 (ARM64) | 当前状态 | 完成度 |
|------|-------------|---------|-------|
| `riscv_codegen.h` | ~260 行 | 指令编码基本完成 | **95%** |
| `riscv_emit.h` | ~1900 行 | **接口完全错误，需要重写** | **0%** |
| `riscv_stub.S` | ~350 行 | 入口/出口框架有，核心逻辑缺失 | **30%** |
| C 辅助函数 | ~200 行 | 未开始 | **0%** |

预计总工作量：**~2500+ 行新代码**

---

## 关键架构差异（RISC-V vs ARM）

### 1. 无硬件标志寄存器（最大挑战）
ARM 的 `adds/subs` 自动设置 NZCV 标志。RISC-V 必须显式计算每个标志：
```
N = (result >> 31) & 1       → rv_srai(reg_n, rd, 31)
Z = (result == 0) ? 1 : 0   → rv_seqz(reg_z, rd)
C = carry_out (因操作而异)   → rv_sltu / 自定义逻辑
V = overflow  (因操作而异)   → 需要多条指令
```
每次 flag-setting 操作增加 ~4-6 条指令。

### 2. 无 ROR 指令
ARM 的移位器有原生 ROR。RISC-V 需要合成：
```c
// ROR(x, n): (x >> n) | (x << (32-n))
rv_srl(tmp, rs, shamt);
rv_sll(tmp2, rs, 32-shamt);
rv_or(rd, tmp, tmp2);
```
按 ESP32-P4 公开 ISA `RV32IMAFCZc` 判断，**应默认不支持** Zbb；
工具链接受 `-march=rv32imac_zbb` 只说明编译器/汇编器认识这些指令，
**不代表硬件可执行**。因此实现上应默认使用软件合成的 rotate/bit-manip 序列。

### 3. 无条件选择指令 (CSEL)
ARM64 用 `csel` 做无分支条件赋值。RISC-V 必须用分支序列替代。

### 4. 12-bit 立即数限制
RISC-V 立即数只有 12-bit 带符号（-2048~2047）。超出需要 LUI+ADDI 两指令。

### 5. `gp`/`tp` 冲突
当前设计把 `gp`（全局指针）和 `tp`（线程指针）挪用给 JIT。但 ESP-IDF 的 C 代码依赖 `gp` 做全局数据寻址。每次 JIT→C 调用必须保存/恢复。

### 6. 寄存器压力
当前只缓存 7 个 GBA 寄存器（R0,R1,R6,R9,R10,R12,R14），其余 8 个（R2-R5,R7,R8,R11,R13/SP）在内存中，会产生大量 load/store 溢出。

---

## 核心发现：riscv_emit.h 接口完全错误

当前 `riscv_emit.h` 定义了 `emit_mov`、`emit_add` 等宏——**这不是 `cpu_threaded.c` 期望的接口**。

`cpu_threaded.c` 使用分层宏系统：
```
cpu_threaded.c 调用 → arm_data_proc(name, type, flags_op)
    → 展开为 → arm_generate_op_reg(name, load_op) / arm_generate_op_imm(...)
        → 调用 → generate_op_add_reg/imm, generate_load_reg, generate_store_reg 等
            → 最终调用 → rv_add, rv_lw, rv_sw 等 codegen 宏
```

**`riscv_emit.h` 需要从零重写**，参照 `arm64_emit.h` 的 ~1900 行实现。

---

## 分阶段实现计划

### Phase 0: 前置准备
- [x] 工具链接受 `-march=rv32imac_zbb`（仅说明 assembler/compiler 支持 Zbb 指令编码）
- [x] 根据公开 ISA `RV32IMAFCZc`，当前实现默认 **不依赖 Zbb**
- [ ] 确认 JIT translation cache 的内存放置策略（PSRAM 可执行？SRAM？）
- [x] 修复 `rv_load_imm32` 的符号扩展 bug
- [x] 添加基于 RV32IMAC 的 `rv_rori` / `rv_ror` 合成宏

### Phase 1: 基础框架（让 dynarec 能跑通最简单的代码块）

#### 1.1 寄存器映射与加载/存储
需要实现的宏（~100 行）：
```c
arm_to_rv_reg[]              // GBA reg index → host reg 映射表
generate_load_reg(ireg, idx) // 从 reg[] 加载 GBA 寄存器
generate_store_reg(ireg, idx)// 存回 reg[]
generate_load_reg_pc(ireg, idx, pc_off) // 加载 reg 或 PC+offset
generate_load_imm(ireg, imm)  // 加载 32-bit 立即数
generate_load_pc(ireg, pc)    // 加载 PC 值
generate_mov(dst, src)        // host reg 间移动
emit_save_regs() / emit_restore_regs() // 全量保存/恢复
```

**依赖**：`riscv_codegen.h`（已完成）

当前进展：
- [x] `cpu_threaded.c` 已增加 `RISCV_ARCH` 接入分支
- [x] `riscv_emit.h` 已改为与其它架构一致的 `generate_*` 接口风格
- [x] `arm_to_rv_reg[]`、`generate_load_reg()`、`generate_store_reg()`
- [x] `generate_load_imm()`、`generate_load_pc()`、`generate_load_reg_pc()`
- [x] `emit_save_regs()` / `emit_restore_regs()`
- [x] `execute_arm_translate()` C wrapper + `execute_arm_translate_internal()` 汇编入口
- [x] `generate_cycle_update()` / `generate_branch_*()` / `generate_translation_gate()` 基础骨架
- [x] 最小无-flags shifter operand（LSL/LSR/ASR/ROR）
- [x] 最小无-flags ALU 子集：`mov/add/sub/and/orr/eor` 的 reg/imm lowering
- [x] 条件执行 backpatch 骨架：`generate_condition_*()` + `arm_conditional_block_header()`
- [x] flags 链路基础版：`ands/orrs/eors/adds/subs/cmp/cmn/tst/teq`
- [ ] 仍缺 ADC/SBC/RSC、部分 flags shift 边界语义校准
- [x] 已实现 ADC/SBC/RSC 与对应 flags 版本的基础 lowering
- [x] 已实现 ARM 单数据访存 lowering 宏（byte/halfword/word load/store，含 pre/post-index）
- [x] 已实现 ARM/Thumb block transfer 基础 lowering 宏（LDM/STM、PUSH/POP 骨架）
- [x] 已实现 Thumb 通用 ALU / 分支 / 访存 / SWI / cheat / HLE div 基础宏簇
- [x] `riscv/riscv_emit.h` 已能支撑 `cpu_threaded.c` 在 `-DRISCV_ARCH` 下单文件编译通过
- [x] 已补充 `tests/riscvgen.*` 字节级对拍测试，以及 `tests` 内 RISC-V backend smoke test
- [x] 已补充 `tests/riscv_codegen_check.c`，覆盖 `rv_load_imm32` 与 rotate helper 的 host 侧语义回归测试
- [x] 已补充 `tests/riscv_emit_check.c`，覆盖 `rv_jal_offset` / `rv_patch_jal` / `rv_patch_branch` 的 host 侧回填测试
- [x] 已补充 `tests` 内 `riscv-stub-check`，验证 `riscv_stub.S` 关键 helper 符号完整导出

#### 1.2 块管理与分支
需要实现的宏（~200 行）：
```c
generate_block_prologue()     // 块入口（cycle 检查等）
generate_block_extra_vars()   // 局部变量声明
block_prologue_size           // 常量：prologue 的字节数
generate_cycle_update()       // 更新 cycle 计数
generate_branch_cycle_update(wb, new_pc)
generate_branch_no_cycle_update(wb, new_pc)
emit_branch_filler(wb_loc)   // 占位用的 NOP / 预留跳转
generate_branch_patch_conditional(dest, label)
generate_branch_patch_unconditional(dest, target)
generate_function_call(func)  // 调用 C 函数 (save gp/tp!)
generate_translation_gate(type)
```

**依赖**：1.1

#### 1.3 `riscv_stub.S` 补全
需要实现（~150 行）：
```asm
; CPSR → flag regs (入口)
; 从 reg[REG_CPSR] 提取 N/Z/C/V → s8-s11

; flag regs → CPSR (出口)
; 合并 s8-s11 回 reg[REG_CPSR]

; 块查找与跳转
; 根据 PC 查找 translation cache，跳转到翻译块

; C 函数调用跳板 (jit_call_c_trampoline)
; 保存 JIT 状态 → 恢复 gp/tp → 调用 C → 恢复 JIT 状态

; rv_update_gba — cycle 耗尽回调
; rv_indirect_branch_arm/thumb/dual — 间接跳转
```

**依赖**：1.1（寄存器映射必须一致）

当前进展：
- [x] `riscv_stub.S` 已对齐新的完整寄存器映射（R0-R14 + PC + NZCV）
- [x] 已实现 `load_registers()` / `store_registers()`
- [x] 已实现 CPSR → NZCV 提取 / NZCV → CPSR 合并
- [x] 已实现 `execute_arm_translate_internal()` 最小入口/退出骨架
- [x] 已实现 `jit_call_c_trampoline()` 的基础保存/恢复框架
- [x] 已加入 `rv_update_gba` / `rv_indirect_branch_*` 的最小 helper 骨架
- [x] `rv_update_gba` 已具备基础语义：frame 结束返回主线程、PC 改变时重新 lookup block、PC 不变时返回调用点
- [x] `execute_arm_translate_internal()` 已具备入口 sleep loop + block lookup/dispatch 主循环
- [x] 已实现 `execute_store_cpsr` / `execute_store_spsr` / `execute_spsr_restore` / `execute_swi` 基础 helper

### Phase 2: ALU 操作（纯计算指令）

#### 2.1 无标志位 ALU（~150 行）
```c
generate_op_add_reg/imm     generate_op_sub_reg/imm
generate_op_rsb_reg/imm     generate_op_and_reg/imm
generate_op_orr_reg/imm     generate_op_eor_reg/imm
generate_op_bic_reg/imm     generate_op_mov_reg/imm
generate_op_mvn_reg/imm     generate_op_neg_reg
```

#### 2.2 带标志位 ALU — 最困难部分（~300 行）
```c
// 逻辑运算标志 (N, Z; C 不变)
generate_op_logic_flags(reg)
generate_op_ands_reg/imm    generate_op_orrs_reg/imm
generate_op_eors_reg/imm    generate_op_bics_reg/imm
generate_op_movs_reg/imm    generate_op_mvns_reg/imm

// 算术运算标志 (N, Z, C, V 全部更新)
generate_op_arith_flags()
generate_op_adds_reg/imm    generate_op_subs_reg/imm
generate_op_rsbs_reg/imm    generate_op_adcs_reg/imm
generate_op_sbcs_reg/imm    generate_op_rscs_reg/imm
generate_op_cmp_reg/imm     generate_op_cmn_reg/imm
generate_op_tst_reg/imm     generate_op_teq_reg/imm
```

**RISC-V 特有挑战**：
- ADD 的 C 标志：`C = (result < operand1)` → `rv_sltu(reg_c, rd, rs1)`
- SUB 的 C 标志：ARM 中 `C = !borrow` → `rv_sltu(tmp, rs1, rs2); rv_xori(reg_c, tmp, 1)`
- V 标志（有符号溢出）：`V = ((a ^ result) & (b ^ result)) >> 31`
- ADC/SBC: 两次加法的进位需要合并，最复杂

#### 2.3 移位操作（~200 行）
```c
generate_shift_imm_lsl_no_flags/flags
generate_shift_imm_lsr_no_flags/flags
generate_shift_imm_asr_no_flags/flags
generate_shift_imm_ror_no_flags/flags    // 需要合成 ROR
generate_shift_reg_lsl_no_flags/flags
generate_shift_reg_lsr_no_flags/flags
generate_shift_reg_asr_no_flags/flags
generate_shift_reg_ror_no_flags/flags
generate_shift_imm(arm_reg, name, flags_op)
generate_shift_reg(arm_reg, name, flags_op)
generate_load_rm_sh(flags_op)
generate_load_offset_sh()
```

**移位 flags 特殊**：LSL/LSR/ASR/ROR 影响 C 标志（最后移出的 bit）。

#### 2.4 乘法（~80 行）
```c
generate_op_muls_reg
generate_multiply_u64 / s64 / u64_add / s64_add
arm_multiply(add_op, flags)
arm_multiply_long(name, add_op, flags)
```

RISC-V M 扩展已有 `mul/mulh/mulhu`，对应良好。

### Phase 3: 数据处理分发宏

顶层分发宏，连接 cpu_threaded.c 与 Phase 2 的操作（~200 行）：
```c
arm_generate_op_reg(name, load_op)
arm_generate_op_reg_flags(name, load_op)
arm_generate_op_imm(name, load_op)
arm_generate_op_imm_flags(name, load_op)
arm_data_proc(name, type, flags_op)
arm_data_proc_test(name, type)
arm_data_proc_unary(name, type, flags_op)
generate_alu_imm(imm_type, reg_type, ...)
generate_logical_imm(optype, ...)
```

### Phase 4: 条件码

#### 4.1 条件分支生成（~100 行）
```c
generate_condition_eq()   // bnez reg_z
generate_condition_ne()   // beqz reg_z
generate_condition_cs()   // bnez reg_c
generate_condition_cc()   // beqz reg_c
generate_condition_mi()   // bnez reg_n
generate_condition_pl()   // beqz reg_n
generate_condition_vs()   // bnez reg_v
generate_condition_vc()   // beqz reg_v
generate_condition_hi()   // (c && !z)
generate_condition_ls()   // (!c || z)
generate_condition_ge()   // (n == v)
generate_condition_lt()   // (n != v)
generate_condition_gt()   // (!z && n == v)
generate_condition_le()   // (z || n != v)
generate_condition()      // 分发
arm_conditional_block_header()
```

复合条件（hi/ls/ge/lt/gt/le）需要多条指令，因为 RISC-V 没有 CSEL。

### Phase 5: 内存访问

#### 5.1 单字/半字/字节加载与存储（~250 行）
```c
arm_access_memory(access_type, direction, adjust_op, mem_type, offset_type)
arm_access_memory_load/store(mem_type)
arm_access_memory_*_pre/post_up/down()
arm_data_trans_reg/imm(adjust_op, adjust_dir)
arm_data_trans_half_reg/imm(adjust_op, adjust_dir)
```

这些宏生成地址计算 + 调用 C 辅助函数（`execute_load_u32` 等）。
每次内存访问需要 JIT→C 跳转（save/restore gp/tp），开销较大。

#### 5.2 块传输 LDM/STM（~200 行）
```c
arm_block_memory(access_type, offset_type, writeback_type, s_bit)
arm_block_memory_load/store()
arm_block_memory_final_load/store(writeback)
arm_block_memory_offset_down_a/down_b/no/up()
arm_block_memory_writeback_down/up/no()
```

LDM/STM 是 GBA 中频繁使用的指令，实现复杂。

### Phase 6: PSR、SWI、交换指令
```c
arm_psr(op_type, transfer_type, psr_reg)    // MRS/MSR
arm_swi() / thumb_swi()                     // 软件中断
arm_hle_div(cpu_mode)                       // HLE BIOS 除法
arm_swap(type)                              // SWP/SWPB
```

### Phase 7: Thumb 指令集

Thumb 16-bit 指令集大约是 ARM 指令的子集，但有独立的分发宏（~300 行）：
```c
thumb_data_proc(type, name, rn_type, ...)
thumb_shift(decode_type, op_type, value_type)
thumb_access_memory(...)
thumb_block_memory(...)
thumb_b/bl/blh/bx()
thumb_conditional_branch(condition)
thumb_load_pc/sp/pc_pool_const()
```

Thumb 的寄存器访问只能用 R0-R7（低寄存器），高寄存器操作有专用指令。

### Phase 8: C 辅助函数

在 `riscv_stub.S` 或独立 C 文件中实现（~200 行）：
```c
rv_update_gba(u32 pc)                        // Cycle 耗尽 → 回到调度器
rv_indirect_branch_arm(u32 addr)             // 间接跳转查找
rv_indirect_branch_thumb(u32 addr)
rv_indirect_branch_dual(u32 addr)
rv_cheat_hook()                              // 金手指处理
// execute_load/store_* 已在 gba_memory.c 中存在
```

---

## 推荐实施顺序

```
Phase 0 → Phase 1.1 → Phase 1.2 → Phase 1.3
                                      ↓
                                  Phase 2.1
                                      ↓
                                  Phase 3 (部分)
                                      ↓
                              *** 里程碑：最简 ARM 块可执行 ***
                                      ↓
                                  Phase 2.2
                                      ↓
                                  Phase 4
                                      ↓
                                  Phase 2.3
                                      ↓
                              *** 里程碑：大部分 ARM 指令可翻译 ***
                                      ↓
                                  Phase 5.1
                                      ↓
                                  Phase 2.4
                                      ↓
                                  Phase 6
                                      ↓
                              *** 里程碑：ARM 指令集完整 ***
                                      ↓
                                  Phase 7
                                      ↓
                                  Phase 5.2
                                      ↓
                                  Phase 8
                                      ↓
                              *** 里程碑：完全可用的 dynarec ***
```

---

## 验证策略

1. **Phase 0**：编写 codegen 单元测试（编码 → 反汇编 → 比对）
2. **Phase 1 完成后**：空块能正常进入/退出，cycle 计数正确
3. **Phase 2-3 完成后**：测试 data-processing-only 的 ARM 代码块
4. **Phase 4 完成后**：条件执行正确（用 CMP + 条件分支序列测试）
5. **Phase 5+ 完成后**：能跑通 GBA BIOS 初始化
6. **Phase 7 完成后**：能启动 Thumb 模式的商业游戏
7. **全部完成**：性能对比解释器模式，目标 3-5x 加速

---

## 已完成清单

### riscv_codegen.h ✅ (95%)
- [x] 寄存器常量定义 (rv_zero ~ rv_t6)
- [x] 操作码常量 (RV_OP_*)
- [x] funct3/funct7 常量
- [x] R/I/S/B/U/J 型指令编码宏
- [x] 完整算术指令 (add/sub/addi + M 扩展 mul/div)
- [x] 完整逻辑指令 (and/or/xor + 立即数变体)
- [x] 完整移位指令 (sll/srl/sra + 立即数变体)
- [x] 完整比较指令 (slt/sltu/slti/sltiu)
- [x] 完整加载/存储 (lw/lh/lhu/lb/lbu/sw/sh/sb)
- [x] 完整分支指令 (beq/bne/blt/bge/bltu/bgeu)
- [x] 跳转指令 (jal/jalr/j/call/ret)
- [x] 上位立即数 (lui/auipc)
- [x] 伪指令 (nop/mv/not/neg/li/seqz/snez/beqz/bnez)
- [x] 系统指令 (ecall/ebreak/fence_i/fence)
- [x] 32-bit 立即数加载 (rv_load_imm32)
- [x] **修复 `rv_load_imm32` 符号扩展 bug**
- [x] **合成 ROR 宏**

### riscv_stub.S 🟡 (30%)
- [x] 栈帧布局 (64 字节, 16 字节对齐)
- [x] `execute_arm_translate` 入口：保存所有被调用者保存寄存器
- [x] GBA 寄存器加载 (reg[] → s0-s7)
- [x] GBA 寄存器回写 (s0-s7 → reg[])
- [x] `return_from_jit` 退出路径
- [x] `jit_call_c_trampoline` 签名（空实现）
- [ ] CPSR → flag 寄存器提取 (N/Z/C/V → s8-s11)
- [ ] flag 寄存器 → CPSR 回写
- [ ] 翻译块查找与跳转
- [ ] `jit_call_c_trampoline` 完整实现
- [ ] `rv_update_gba` / `rv_indirect_branch_*` 跳板

### riscv_emit.h ❌ (0%)
- [x] 寄存器映射定义（部分，接口名称错误）
- [x] `reg_map[]` 数组（需重命名为 `arm_to_rv_reg[]`）
- [x] `translate_icache_sync()` / `emit_icache_sync()`
- [ ] **以下全部缺失——需要 ~1800 行新代码：**
  - [ ] 寄存器加载/存储宏
  - [ ] 块管理宏
  - [ ] ALU 操作（无标志）
  - [ ] ALU 操作（带标志）
  - [ ] 移位操作
  - [ ] 乘法
  - [ ] 数据处理分发宏
  - [ ] 条件码
  - [ ] 内存访问
  - [ ] 块传输 (LDM/STM)
  - [ ] PSR 操作
  - [ ] SWI
  - [ ] Thumb 指令集
  - [ ] C 函数调用

---

## 下一步：开始 Phase 0

```bash
# 1. 检查 ESP32-P4 C906 是否支持 Zbb 扩展
riscv32-esp-elf-gcc -march=rv32imac_zbb -c -x c /dev/null -o /dev/null 2>&1

# 2. 在板上确认 Zbb 是否真的可执行
# 3. 确认 JIT cache 必须放 SRAM 还是可放可执行映射区
```
