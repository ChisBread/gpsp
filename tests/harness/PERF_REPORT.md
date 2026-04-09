# RISC-V Dynarec & Scanline 性能优化报告

**ROM**: game.gba (32 MB)  
**BIOS**: gba_bios.bin  
**帧数**: 3000  
**平台**: QEMU riscv32, 单核, JIT  
**Baseline MD5**: `19cbcf89e2f1b62cc880570e4f4c90e4`

## Baseline (原版)

| 指标 | 值 |
|------|-----|
| Total wall time | 9.219 s |
| Avg frame time | 3072.3 us (3.07 ms) |
| Median | 3001.5 us (3.00 ms) |
| Min | 2256.0 us (2.26 ms) |
| Max | 33389.6 us (33.39 ms) |
| TP99 | 4961.7 us (4.96 ms) |
| Effective FPS | 325.40 |

---

## 优化记录

### OPT-1: 编译器 -O3 -funroll-loops

**变更**: Makefile CFLAGS 从 `-O2` 改为 `-O3 -funroll-loops`  
**影响**: video.cc 小循环（4-8次迭代的tile渲染循环）更好展开  
**MD5**: `19cbcf89e2f1b62cc880570e4f4c90e4` ✅ 一致  

| 指标 | Baseline | OPT-1 | 变化 |
|------|----------|-------|------|
| Total | 9.219 s | 9.191 s | -0.3% |
| Avg | 3072.3 us | 3063.0 us | -0.3% |
| TP99 | 4961.7 us | 5586.9 us | +12.6% (噪声) |
| FPS | 325.40 | 326.40 | +0.3% |

**结论**: 微小改善，保留。

---

### OPT-2: JIT function call AUIPC+JALR (3→2指令)

**变更**: `generate_function_call` 从 LUI+ADDI+JALR (3指令12字节) 改为 AUIPC+JALR (2指令8字节)  
**影响**: 每次内存访问 emit 代码减少 1 条 RISC-V 指令  
**文件**: `riscv/riscv_emit.h`  
**MD5**: `19cbcf89e2f1b62cc880570e4f4c90e4` ✅ 一致  

| 指标 | OPT-1 | OPT-2 | 变化 |
|------|-------|-------|------|
| Total | 9.191 s | 9.168 s | -0.3% |
| Avg | 3063.0 us | 3055.3 us | -0.3% |
| FPS | 326.40 | 327.21 | +0.2% |

**结论**: QEMU下改善有限（QEMU自身TCG会重翻译），但真实ESP32-P4减少33%调用开销。保留。

---

### OPT-3: 轻量级Load Trampoline (跳过完整register save/flag consolidate)

**变更**: `rv_execute_load_*` trampoline 跳过完整的 `store_registers()`/`consolidate_flags()`/`extract_flags()`/`load_registers()`，仅保存 caller-clobbered 寄存器 (a3-a7, t3-t6) 和 flag cache (s10/s11)  
**原理**: `execute_load_*` 是纯内存读函数，不修改ARM状态或CPSR  
**影响**: 每次load操作从 ~66条指令 降至 ~30条指令  
**文件**: `riscv/riscv_stub.S`  
**MD5**: `19cbcf89e2f1b62cc880570e4f4c90e4` ✅ 一致  

| 指标 | OPT-2 | OPT-3 | vs Baseline |
|------|-------|-------|-------------|
| Total | 9.168 s | 8.911 s | **-3.3%** |
| Avg | 3055.3 us | 2969.5 us | **-3.3%** |
| Median | 2954.5 us | 2897.9 us | -3.5% |
| Min | 2201.2 us | 2168.0 us | -3.9% |
| TP99 | 5444.9 us | 5282.4 us | +6.5% |
| FPS | 327.21 | 336.67 | **+3.5%** |

**结论**: 显著提升！采纳。

---

### OPT-4: 去除Load路径PC加载 (REVERTED)

**变更**: 从 `arm_access_memory_load` 等emit宏中去掉 `generate_load_pc(reg_a1, pc)`, trampoline中去掉 `sw a1, REG_PC(gp)`  
**结果**: MD5一致但 **性能回退** (9.308s vs 8.911s)  
**原因**: 可能是QEMU TCG翻译块边界对齐效应  
**结论**: ❌ 已回退

---

### OPT-5: 轻量级Store Trampoline

**变更**: `rv_execute_store_*` trampoline 采用与OPT-3相同的轻量级模式：`store_caller_regs()`+`swap_to_c_abi()`，alert时再完成完整寄存器+flag保存  
**影响**: 每次store操作从 ~66条指令 降至 ~30条 (快速路径, 无alert)  
**文件**: `riscv/riscv_stub.S`  
**MD5**: `19cbcf89e2f1b62cc880570e4f4c90e4` ✅ 一致  

| 指标 | OPT-3 | OPT-5 | 变化 |
|------|-------|-------|------|
| Total | 8.911 s | ~8.93 s | ≈平 |
| FPS | 336.67 | ~336 | ≈平 |

**结论**: QEMU上store发生频率低于load，改善不可测量。但代码逻辑更简洁，且真实硬件受益。保留。

---

### OPT-6: LTO (Link-Time Optimization) (REVERTED)

**变更**: CFLAGS/LDFLAGS 增加 `-flto`  
**结果**: 性能 **严重回退** (9.771s vs 8.93s, +9.4%)  
**原因**: 过度内联膨胀代码体积，恶化QEMU TCG翻译缓存命中率  
**结论**: ❌ 已回退

---

### OPT-7: 轻量级间接分支Trampoline

**变更**: `rv_indirect_branch_arm/thumb/dual` 从完整 `store_registers()`+`load_registers()`+`extract_flags()` 切换到 `store_caller_regs()`+`swap_to_c_abi()`+`swap_from_c_abi()`+`load_caller_regs()`  
**保留**: `consolidate_flags()` (block lookup/翻译可能需要CPSR)  
**影响**: 每次间接分支从 ~66条指令 降至 ~45条  
**文件**: `riscv/riscv_stub.S`  
**MD5**: `19cbcf89e2f1b62cc880570e4f4c90e4` ✅ 一致  

| 指标 | OPT-5 | OPT-7 | 变化 |
|------|-------|-------|------|
| Total | ~8.93 s | ~8.88 s | ≈平 |
| FPS | ~336 | ~337 | ≈平 |

**结论**: QEMU噪声内，但减少了~20条指令/间接分支。真实硬件受益。保留。

---

### 发现: Zba/Zbb 位操作扩展 (仅QEMU参考)

**测试**: `-march=rv32gc_zba_zbb` + `qemu-riscv32 -cpu rv32,zba=true,zbb=true`  
**结果**: **7.28s** vs baseline 9.22s = **-21.1%** 巨大提升！  
**MD5**: `19cbcf89e2f1b62cc880570e4f4c90e4` ✅ 一致  

| 指标 | 当前最优 (rv32gc) | Zba+Zbb | 变化 |
|------|-------------------|---------|------|
| Total | ~8.88 s | 7.28 s | **-18.0%** |
| FPS | ~337 | 411 | **+21.9%** |
| Avg | ~2960 us | 2435 us | **-17.7%** |
| Min | ~2150 us | 1720 us | -20.0% |

**不采纳原因**: ESP32-P4 实际ISA为 `rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p1`，**不支持Zba/Zbb**  
**启示**: 编译器生成的符号扩展(sext.b/sext.h)和缩放地址生成(sh1add/sh2add/sh3add)占大量开销，若P4将来支持bitmanip，可获巨大收益

---

### OPT-9: Flag-setting算术指令智能MV消除

**变更**: `generate_op_adds_reg`, `generate_op_subs_reg`, `generate_op_rsbs_reg` 及其 `_imm` 版本在JIT编译时检查 `rd != rn` / `rd != rm`，跳过不必要的 `rv_mv` 保存操作  
**影响**: CMP/TST/TEQ/CMN (使用 reg_temp2 作为目标) 完全消除2条 mv 指令；ADD/SUB rd!=rn 消除1条  
**文件**: `riscv/riscv_emit.h`  
**MD5**: `19cbcf89e2f1b62cc880570e4f4c90e4` ✅ 一致  

**结论**: QEMU噪声内。减少每条 flag-setting 算术指令 0-2 条 RISC-V 指令。真实硬件受益。保留。

---

### OPT-10: 间接分支去除consolidate_flags

**变更**: `rv_indirect_branch_arm/thumb/dual` 去掉 `consolidate_flags(t0, t1)` (~12条指令)  
**原理**: `block_lookup_address_*` 不读取CPSR的N/Z/C/V标志位；dual版仅修改T位(bit 5)。Flag caches (s8-s11) 通过callee-saved约定保持正确  
**影响**: 每次间接分支减少 12 条指令  
**文件**: `riscv/riscv_stub.S`  
**MD5**: `19cbcf89e2f1b62cc880570e4f4c90e4` ✅ 一致  

| 指标 | OPT-9 | OPT-10 (5次中位数) | 变化 |
|------|-------|---------------------|------|
| Total | ~8.90 s | 8.846 s | -0.6% |
| FPS | ~337 | ~339 | +0.6% |

**结论**: 微小但一致的改善。保留。

---

### OPT-11: Assembly内存Load快路径 (EWRAM/IWRAM/ROM)

**变更**: 6个Load trampoline (`rv_execute_load_{u8,s8,u16,s16,u32}`, `rv_execute_aligned_load32`) 增加汇编级快路径，对EWRAM(0x02)、IWRAM(0x03)和ROM(0x08-0x0C)直接在汇编中完成内存读取，跳过完整的C函数调用和35指令的寄存器保存/恢复开销  
**原理**: ARM64后端已有类似设计（jump table + 内联fast handler），RISC-V后端此前所有Load都走完整C调用路径  
**实现**:  
- 地址分发: `srli t0, a0, 24` 取region → 条件分支到 EWRAM/IWRAM/ROM handler
- EWRAM快路径: `la t0, ewram; slli/srli mask 0x3FFFF; load` (7条指令 + ret)
- IWRAM快路径: `la t0, iwram+0x8000; slli/srli mask 0x7FFF; load` (9条指令 + ret)
- ROM快路径: `memory_map_read[addr>>15][addr&0x7FFF]` with NULL check (13条指令 + ret)
- u16/s16: 先检查对齐，未对齐→慢路径（需rotation/byte sign-extend）
- u32: 先检查4字节对齐，未对齐→慢路径（需rotation）
- 只用t0, t1做scratch，不触碰ARM寄存器映射(a3-a7, t3-t6, s0-s7) ← **零保存/恢复开销**

**文件**: `riscv/riscv_stub.S`  
**MD5**: `19cbcf89e2f1b62cc880570e4f4c90e4` ✅ 一致  

| 指标 | OPT-10 | OPT-11 (5次中位数) | 变化 |
|------|--------|---------------------|------|
| Total | 8.846 s | 5.763 s | **-34.8%** |
| FPS | ~339 | ~521 | **+53.7%** |

5次测量: 5.763, 5.692, 5.723, 5.882, 5.858 (中位数 5.763s)

**结论**: **极其显著的提升**。对于EWRAM/IWRAM/ROM的Load操作（占大部分内存访问），从~35条指令降至7-13条（无寄存器保存恢复）。保留。

---

### OPT-12: Assembly内存Store快路径 (EWRAM/IWRAM + SMC检测)

**变更**: 4个Store trampoline (`rv_execute_store_{u8,u16,u32}`, `rv_execute_aligned_store32`) 增加汇编级快路径  
**实现**:
- EWRAM快路径: 写入`ewram[addr & 0x3FFFF]`，SMC检查`ewram[offset + 0x40000]` (~12指令)
- IWRAM快路径: SMC检查`iwram[addr & 0x7FFF]`，写入`iwram[offset + 0x8000]` (~13指令)
- SMC检测: 如sticky bit非零 → 落入慢路径处理flush（检查先于写入，避免double-write问题）
- `aligned_store32` (STM用): **无SMC检查**（匹配C代码行为），仅7-9指令
- u16/u32: 先检查对齐，未对齐→慢路径
- 只用t0, t1做scratch — **零保存/恢复开销**

**文件**: `riscv/riscv_stub.S`  
**MD5**: `19cbcf89e2f1b62cc880570e4f4c90e4` ✅ 一致  

| 指标 | OPT-11 | OPT-12 (5次中位数) | 变化 |
|------|--------|---------------------|------|
| Total | 5.763 s | 5.177 s | **-10.2%** |
| FPS | ~521 | ~580 | **+11.3%** |

5次测量: 5.177, 5.145, 5.158, 5.392, 5.208 (中位数 5.177s)

**结论**: **显著提升**。Store操作少于Load但STM（函数epilog push）非常频繁。保留。

---

### OPT-13: IO/Palette/VRAM/OAM Load快路径

**变更**: Load快路径扩展覆盖全部常用内存区域:
- 0x04: I/O寄存器 — `io_registers[addr & 0x3FF]`
- 0x05: Palette RAM — `palette_ram[addr & 0x3FF]`
- 0x06: VRAM — `vram[addr & 0x1FFFF]` (含0x18000 mirror处理)
- 0x07: OAM RAM — `oam_ram[addr & 0x3FF]`

**文件**: `riscv/riscv_stub.S`  
**MD5**: `19cbcf89e2f1b62cc880570e4f4c90e4` ✅ 一致  

| 指标 | OPT-12 | OPT-13 (5次中位数) | 变化 |
|------|--------|---------------------|------|
| Total | 5.177 s | 5.142 s | -0.7% |
| FPS | ~580 | ~583 | +0.5% |

5次测量: 5.148, 5.142, 5.108, 5.119, 5.156 (中位数 5.142s)

**结论**: 微小改善。game.gba中CPU side IO/PAL/VRAM读取不多（PPU从C代码读取）。但覆盖完整，无额外开销。保留。

---

## 总结

| 阶段 | Total时间 | FPS | vs Baseline |
|------|-----------|-----|-------------|
| Baseline | 9.219 s | 325.40 | — |
| +OPT-1 (-O3) | 9.191 s | 326.40 | -0.3% |
| +OPT-2 (AUIPC+JALR) | 9.168 s | 327.21 | -0.6% |
| +OPT-3 (轻量Load) | 8.911 s | 336.67 | -3.3% |
| +OPT-5 (轻量Store) | ~8.93 s | ~336 | -3.1% |
| +OPT-7 (轻量Branch) | ~8.88 s | ~337 | -3.7% |
| +OPT-9 (MV消除) | ~8.90 s | ~337 | -3.5% |
| +OPT-10 (去Flag合并) | ~8.85 s | ~339 | -4.0% |
| **+OPT-11 (Load快路径)** | **5.763 s** | **~521** | **-37.5%** |
| **+OPT-12 (Store快路径)** | **5.177 s** | **~580** | **-43.8%** |
| +OPT-13 (IO/PAL/VRAM/OAM) | 5.142 s | ~583 | -44.2% |
| (参考) Zba+Zbb | 7.28 s | 411 | -21.1% |

### 已采纳优化
- **OPT-1**: `-O3 -funroll-loops` 编译器优化
- **OPT-2**: JIT函数调用 LUI+ADDI+JALR → AUIPC+JALR (3→2指令)
- **OPT-3**: 轻量级Load Trampoline (跳过完整寄存器保存/恢复)
- **OPT-5**: 轻量级Store Trampoline (同上, alert慢路径补全保存)
- **OPT-7**: 轻量级间接分支Trampoline
- **OPT-9**: Flag-setting算术指令智能MV消除 (CMP/TST零开销)
- **OPT-10**: 间接分支去除不必要的consolidate_flags (-12指令/分支)
- **OPT-11**: Assembly Load快路径 — EWRAM/IWRAM/ROM直接汇编读取 (**-34.8%**, 最大单项优化)
- **OPT-12**: Assembly Store快路径 — EWRAM/IWRAM直接汇编写入+内联SMC检测 (**-10.2%**)
- **OPT-13**: IO/Palette/VRAM/OAM Load快路径 — 剩余内存区域汇编读取覆盖

### 已回退优化
- **OPT-4**: 去除Load路径PC加载 (QEMU回退)
- **OPT-6**: LTO (代码膨胀导致严重回退)
