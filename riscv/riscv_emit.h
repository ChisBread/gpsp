/* gameplaySP
 *
 * RISC-V dynarec emit layer.
 *
 * This backend follows the same integration model as mips_emit.h and
 * arm64_emit.h: cpu_threaded.c expands high-level translation macros into
 * arch-specific generate_* macros defined here, which in turn emit machine
 * code through riscv_codegen.h.
 *
 * STATUS: Phase 1.1 foundation only. Register contract, immediate loads,
 * PC loads, save/restore, and function-call helpers are wired. The actual
 * ARM/Thumb operation lowering macros are still pending.
 */

#ifndef RISCV_EMIT_H
#define RISCV_EMIT_H

#include "riscv_codegen.h"

u32 rv_update_gba(u32 pc);

/* Although these are defined as functions, dynarec code jumps to them. */
void rv_indirect_branch_arm(u32 address);
void rv_indirect_branch_thumb(u32 address);
void rv_indirect_branch_dual(u32 address);

u32 execute_read_cpsr(void);
u32 execute_read_spsr(void);
void execute_swi(u32 pc);
void rv_cheat_hook(void);
u32 jit_call_c_trampoline(uintptr_t function_ptr, u32 arg0, u32 arg1, u32 arg2);

u32 rv_execute_load_u8(u32 address);
u32 rv_execute_load_s8(u32 address);
u32 rv_execute_load_u16(u32 address);
u32 rv_execute_load_s16(u32 address);
u32 rv_execute_load_u32(u32 address);
u32 rv_execute_aligned_load32(u32 address);

void rv_execute_store_u8(u32 address, u32 source);
void rv_execute_store_u16(u32 address, u32 source);
void rv_execute_store_u32(u32 address, u32 source);
void rv_execute_aligned_store32(u32 address, u32 source);

u32 execute_spsr_restore(u32 address);
void execute_store_cpsr(u32 new_cpsr, u32 address, u32 store_mask);
void execute_store_spsr(u32 new_spsr, u32 store_mask);

u32 function_cc execute_load_u8(u32 address)
{
    return read_memory8(address);
}

u32 function_cc execute_load_s8(u32 address)
{
    return read_memory8s(address);
}

u32 function_cc execute_load_u16(u32 address)
{
    return read_memory16(address);
}

u32 function_cc execute_load_s16(u32 address)
{
    return read_memory16s(address);
}

u32 function_cc execute_load_u32(u32 address)
{
    return read_memory32(address);
}

cpu_alert_type function_cc execute_store_u8(u32 address, u32 source)
{
    cpu_alert_type alert = write_memory8(address, source);
    u8 region = address >> 24;
    if (region == 0x03) {
        u32 offset = address & 0x7FFF;
        if (iwram[offset]) {
            alert |= CPU_ALERT_SMC;
        }
    } else if (region == 0x02) {
        u32 offset = (address & 0x3FFFF) + 0x40000;
        if (ewram[offset]) {
            alert |= CPU_ALERT_SMC;
        }
    }
    return alert;
}

cpu_alert_type function_cc execute_store_u16(u32 address, u32 source)
{
    cpu_alert_type alert = write_memory16(address, source);
    u8 region = address >> 24;
    if (region == 0x03) {
        u32 offset = (address & 0x7FFF) & ~1;
        if (*(u16*)(iwram + offset)) {
            alert |= CPU_ALERT_SMC;
        }
    } else if (region == 0x02) {
        u32 offset = ((address & 0x3FFFF) & ~1) + 0x40000;
        if (*(u16*)(ewram + offset)) {
            alert |= CPU_ALERT_SMC;
        }
    }
    return alert;
}

cpu_alert_type function_cc execute_store_u32(u32 address, u32 source)
{
    cpu_alert_type alert = write_memory32(address, source);
    u8 region = address >> 24;
    if (region == 0x03) {
        u32 offset = (address & 0x7FFF) & ~3;
        if (*(u32*)(iwram + offset)) {
            alert |= CPU_ALERT_SMC;
        }
    } else if (region == 0x02) {
        u32 offset = ((address & 0x3FFFF) & ~3) + 0x40000;
        if (*(u32*)(ewram + offset)) {
            alert |= CPU_ALERT_SMC;
        }
    }
    return alert;
}

cpu_alert_type function_cc execute_store_aligned_u32(u32 address, u32 source)
{
    return write_memory32(address, source);
}

cpu_alert_type execute_aligned_store32(u32 addr, u32 data)
{
    return execute_store_aligned_u32(addr, data);
}

u32 execute_aligned_load32(u32 addr)
{
    return read_memory32(addr);
}

u32 rv_handle_store_alert(u32 alert, int cycles)
{
    if (alert & CPU_ALERT_SMC)
        flush_translation_cache_ram();
    if (alert & CPU_ALERT_IRQ)
        check_and_raise_interrupts();
    if (alert & CPU_ALERT_HALT) {
        return update_gba(cycles);
    }
    /* Non-HALT path: return cycle count with bits 30-31 cleared.
     * The asm caller uses 'blt a0,zero' to detect frame_complete (bit 31).
     * A raw negative cycle count (overdrawn) would falsely trigger that exit,
     * causing the JIT to return mid-frame.  Clamping to 0 is safe because
     * the JIT will call rv_update_gba on the very next block boundary. */
    return (cycles < 0) ? 0 : (u32)cycles;
}

void rv_bad_pc_trap(u32 bad_pc)
{
    printf("DYNAREC BAD PC: 0x%08x (REG_PC=0x%08x CPSR=0x%08x)\n",
           bad_pc, reg[REG_PC], reg[REG_CPSR]);
    printf("  r0=%08x r1=%08x r2=%08x r3=%08x\n", reg[0], reg[1], reg[2], reg[3]);
    printf("  r4=%08x r5=%08x r6=%08x r7=%08x\n", reg[4], reg[5], reg[6], reg[7]);
    printf("  r8=%08x r9=%08x r10=%08x r11=%08x\n", reg[8], reg[9], reg[10], reg[11]);
    printf("  r12=%08x sp=%08x lr=%08x\n", reg[12], reg[13], reg[14]);
    fflush(stdout);
}

u32 execute_spsr_restore_body(u32 address)
{
    set_cpu_mode(cpu_modes[reg[REG_CPSR] & 0xF]);
    if((io_registers[REG_IE] & io_registers[REG_IF]) &&
     io_registers[REG_IME] && ((reg[REG_CPSR] & 0x80) == 0))
    {
        REG_MODE(MODE_IRQ)[6] = address + 4;
        REG_SPSR(MODE_IRQ) = reg[REG_CPSR];
        reg[REG_CPSR] = (reg[REG_CPSR] & 0xF0000000) | 0xD2;
        address = 0x00000018;
        set_cpu_mode(MODE_IRQ);
    }

    if(reg[REG_CPSR] & 0x20)
        address |= 0x01;

    return address;
}

u32 execute_store_cpsr_body(u32 _cpsr, u32 address, u32 store_mask)
{
    reg[REG_CPSR] = _cpsr;
    if(store_mask & 0xFF)
    {
        set_cpu_mode(cpu_modes[_cpsr & 0xF]);
        if((io_registers[REG_IE] & io_registers[REG_IF]) &&
         io_registers[REG_IME] && ((_cpsr & 0x80) == 0))
        {
            REG_MODE(MODE_IRQ)[6] = address + 4;
            REG_SPSR(MODE_IRQ) = _cpsr;
            reg[REG_CPSR] = (reg[REG_CPSR] & 0xF0000000) | 0xD2;
            set_cpu_mode(MODE_IRQ);
            return 0x00000018;
        }
    }

    return 0;
}

/* ================================================================
 * GBA Register → RISC-V Host Register Mapping
 *
 * Mirror the style used by MIPS/ARM64 backends: every GBA register gets a
 * host register slot, plus a0-a2 pseudo-register slots used by translation.
 *
 * NOTE: This mapping is the Phase 1.1 emit-side contract. riscv_stub.S still
 * needs to be updated to load/save the full set consistently.
 * ================================================================ */

/* Base/flag/temporary registers. */
#define reg_base    rv_gp
#define reg_cycles  rv_tp
#define reg_res     rv_a0
#define reg_a0      rv_a0
#define reg_a1      rv_a1
#define reg_a2      rv_a2
#define reg_temp    rv_t0
#define reg_temp2   rv_t1
#define reg_temp3   rv_t2
#define reg_save0   rv_t3
#define reg_pc      rv_s7
#define reg_c_cache rv_s10
#define reg_v_cache rv_s11
#define reg_z_cache rv_s9
#define reg_n_cache rv_s8

/* Full GBA register mapping, matching the style of other backends. */
#define reg_r0      rv_s0
#define reg_r1      rv_s1
#define reg_r2      rv_a3
#define reg_r3      rv_a4
#define reg_r4      rv_a5
#define reg_r5      rv_a6
#define reg_r6      rv_s2
#define reg_r7      rv_a7
#define reg_r8      rv_t4
#define reg_r9      rv_s3
#define reg_r10     rv_s4
#define reg_r11     rv_t5
#define reg_r12     rv_s5
#define reg_r13     rv_t6
#define reg_r14     rv_s6

#define reg_zero    rv_zero

#define rv_orr      rv_or
#define rv_eor      rv_xor

const u32 arm_to_rv_reg[] =
{
    reg_r0,
    reg_r1,
    reg_r2,
    reg_r3,
    reg_r4,
    reg_r5,
    reg_r6,
    reg_r7,
    reg_r8,
    reg_r9,
    reg_r10,
    reg_r11,
    reg_r12,
    reg_r13,
    reg_r14,
    reg_a0,
    reg_a1,
    reg_a2
};

#define arm_reg_a0   15
#define arm_reg_a1   16
#define arm_reg_a2   17

static inline s32 rv_jal_offset(const void *target, const void *source)
{
    return (s32)((const u8 *)target - (const u8 *)source);
}

static inline void rv_patch_jal(u32 *inst, const void *target)
{
    u32 base = *inst & 0x00000fff;
    s32 offset = rv_jal_offset(target, inst);
    u32 imm = (u32)offset;

    base |= ((imm >> 20) & 0x1) << 31;
    base |= ((imm >> 1) & 0x3ff) << 21;
    base |= ((imm >> 11) & 0x1) << 20;
    base |= ((imm >> 12) & 0xff) << 12;
    *inst = base;
}

/* Patch a 2-instruction AUIPC+JALR sequence (±2 GB range).
 * inst points to the AUIPC; inst+1 is the JALR.  reg_temp (t0) is
 * used as the scratch register — it is caller-saved and safe to
 * clobber at block-exit points where branch fillers are emitted. */
static inline void rv_patch_far_jump(u32 *inst, const void *target)
{
    s32 offset = rv_jal_offset(target, inst);
    u32 hi = (u32)offset & 0xFFFFF000;
    u32 lo = (u32)offset & 0x00000FFF;
    /* Sign-extend correction: if lo bit 11 is set, JALR sign-extends
     * the 12-bit immediate, so AUIPC needs +0x1000 to compensate. */
    if (lo & 0x800) hi += 0x1000;
    /* AUIPC t0, hi  (opcode 0x17, rd=t0=5) */
    inst[0] = hi | (rv_t0 << 7) | RV_OP_AUIPC;
    /* JALR  zero, t0, lo  (opcode 0x67, rd=0, rs1=t0=5, funct3=0) */
    inst[1] = ((lo & 0xFFF) << 20) | (rv_t0 << 15) | (rv_zero << 7) | RV_OP_JALR;
}

static inline void rv_patch_branch(u32 *inst, const void *target)
{
    u32 base = *inst & 0x01fff07f;
    s32 offset = rv_jal_offset(target, inst);
    u32 imm = (u32)offset;

    base |= ((imm >> 12) & 0x1) << 31;
    base |= ((imm >> 5) & 0x3f) << 25;
    base |= ((imm >> 1) & 0xf) << 8;
    base |= ((imm >> 11) & 0x1) << 7;
    *inst = base;
}

#define generate_save_reg(regnum)                                             \
    rv_sw(arm_to_rv_reg[regnum], reg_base, ((regnum) * 4))                     \

#define generate_restore_reg(regnum)                                          \
    rv_lw(arm_to_rv_reg[regnum], reg_base, ((regnum) * 4))                     \

#define emit_save_regs()                                                      \
{                                                                             \
    unsigned i;                                                                 \
    for (i = 0; i < 15; i++) {                                                  \
        generate_save_reg(i);                                                     \
    }                                                                           \
}

#define emit_restore_regs()                                                   \
{                                                                             \
    unsigned i;                                                                 \
    for (i = 0; i < 15; i++) {                                                  \
        generate_restore_reg(i);                                                  \
    }                                                                           \
}

#define generate_load_reg(ireg, reg_index)                                    \
    do { if ((u32)(ireg) != arm_to_rv_reg[reg_index])                           \
        rv_mv(ireg, arm_to_rv_reg[reg_index]); } while(0)

#define generate_load_imm(ireg, imm)                                          \
    rv_load_imm32(ireg, imm)                                                    \

#define generate_load_pc_2inst(ireg, new_pc)                                  \
    rv_load_imm32_2inst(ireg, new_pc)                                                 \

#define generate_load_pc(ireg, new_pc)                                        \
{                                                                             \
    s32 _delta = (s32)((u32)(new_pc) - stored_pc);                              \
    if (_delta >= -2048 && _delta <= 2047)                                      \
    {                                                                           \
        rv_addi(ireg, reg_pc, _delta);                                          \
    }                                                                           \
    else                                                                        \
    {                                                                           \
        generate_load_imm(ireg, (new_pc));                                      \
    }                                                                           \
}

#define generate_store_reg(ireg, reg_index)                                   \
    do { if ((u32)(ireg) != arm_to_rv_reg[reg_index])                           \
        rv_mv(arm_to_rv_reg[reg_index], ireg); } while(0)

#define generate_logical_imm(optype, ireg_dest, ireg_src, imm)                \
    generate_load_imm(reg_temp, imm);                                           \
    rv_##optype(ireg_dest, ireg_src, reg_temp)                                  \

#define generate_alu_imm(imm_type, reg_type, ireg_dest, ireg_src, imm)        \
    if (((s32)(imm) >= -2048) && ((s32)(imm) <= 2047))                          \
    {                                                                           \
        rv_##imm_type(ireg_dest, ireg_src, imm);                                  \
    }                                                                           \
    else                                                                        \
    {                                                                           \
        generate_load_imm(reg_temp, imm);                                         \
        rv_##reg_type(ireg_dest, ireg_src, reg_temp);                             \
    }                                                                           \

#define generate_mov(ireg_dest, ireg_src)                                     \
    do { if ((ireg_dest) != (ireg_src))                                         \
        rv_mv(arm_to_rv_reg[ireg_dest], arm_to_rv_reg[ireg_src]); } while(0)

#define generate_add_imm(ireg_dest, ireg_src, imm)                            \
    generate_alu_imm(addi, add, ireg_dest, ireg_src, imm)                      \

#define generate_sub_imm(ireg_dest, ireg_src, imm)                            \
    if (((s32)(imm) >= -2047) && ((s32)(imm) <= 2048))                         \
    {                                                                           \
        rv_addi(ireg_dest, ireg_src, -(s32)(imm));                              \
    }                                                                           \
    else                                                                        \
    {                                                                           \
        generate_load_imm(reg_temp, imm);                                       \
        rv_sub(ireg_dest, ireg_src, reg_temp);                                   \
    }                                                                           \

/* ================================================================
 * PSRAM trampoline relocation
 *
 * On ESP32-P4 the trampoline functions live in flash (0x40xx_xxxx) while the
 * JIT caches are in PSRAM (0x48xx_xxxx) — ~129 MB apart, far beyond JAL's
 * ±1 MB range.
 *
 * Fix: at init time, memcpy the trampoline code to a buffer in PSRAM
 * (.ext_ram.bss, right before the translation caches) and fix up all
 * AUIPC instructions so their PC-relative offsets point to the correct
 * targets from the new address.  JIT generate_function_call then targets
 * the PSRAM copies, which are within JAL range of the JIT code.
 * ================================================================ */

/* Delta to add to flash trampoline addresses to get PSRAM copy addresses.
   0 on QEMU (trampolines stay in .text, caches are nearby in .bss). */
static s32 trampoline_reloc_delta;

extern u8 _jit_trampoline_start[];
extern u8 _jit_trampoline_end[];
#ifdef JIT_TRAMPOLINE_PSRAM
extern u8 trampoline_psram_buf[];
#endif

/* Fix up AUIPC instructions after memcpy to a new address.
 * Every AUIPC+ADDI/JALR pair encodes a PC-relative offset; when the code
 * moves, the offset must be adjusted by (old_pc - new_pc).
 */
static void fixup_auipc_relocations(u8 *code, u32 old_base, u32 new_base, u32 size_bytes)
{
    s32 delta = (s32)(old_base - new_base);
    u32 off = 0;
    while (off + 8 <= size_bytes) {
        u16 half = *(u16 *)(code + off);
        if ((half & 3) != 3) {
            off += 2;  /* compressed instruction */
            continue;
        }
        u32 inst = *(u32 *)(code + off);
        if ((inst & 0x7F) == 0x17) {  /* AUIPC opcode */
            u32 next = *(u32 *)(code + off + 4);
            /* Extract original hi20 (already in upper 20 bits position) */
            s32 hi20 = (s32)(inst & 0xFFFFF000);
            /* Extract lo12 from the paired I-type instruction (bits[31:20]) */
            s32 lo12 = (s32)next >> 20;
            /* Original combined PC-relative offset */
            s32 orig_offset = hi20 + lo12;
            /* Adjust for new location */
            s32 new_offset = orig_offset + delta;
            /* Recompute hi20 and lo12 */
            s32 new_hi20 = (new_offset + 0x800) & (s32)0xFFFFF000;
            s32 new_lo12 = new_offset - new_hi20;
            /* Patch AUIPC: replace imm[31:12], keep rd and opcode */
            *(u32 *)(code + off) = (inst & 0xFFF) | (u32)new_hi20;
            /* Patch paired instruction: replace imm[31:20], keep rest */
            *(u32 *)(code + off + 4) = (next & 0x000FFFFF) | ((u32)new_lo12 << 20);
            off += 8;
        } else {
            off += 4;
        }
    }
}

#define generate_function_call(function_location)                             \
{                                                                             \
    u32 _fc_target = (u32)(uintptr_t)(function_location)                        \
                   + trampoline_reloc_delta;                                     \
    u32 _fc_pc    = (u32)(uintptr_t)translation_ptr;                            \
    s32 _fc_delta = (s32)(_fc_target - _fc_pc);                                 \
    if (_fc_delta >= -(1 << 20) && _fc_delta < (1 << 20))                       \
    {                                                                           \
        rv_call(_fc_delta);                                                     \
    }                                                                           \
    else                                                                        \
    {                                                                           \
        u32 _fc_hi = (u32)_fc_delta & 0xFFFFF000;                              \
        u32 _fc_lo = (u32)_fc_delta & 0xFFF;                                   \
        if (_fc_lo & 0x800) _fc_hi += 0x1000;                                  \
        rv_auipc(reg_temp, _fc_hi);                                            \
        rv_jalr(rv_ra, reg_temp, (s32)(_fc_lo << 20) >> 20);                   \
    }                                                                           \
}

#define generate_raw_u32(value)                                               \
    *((u32 *)translation_ptr) = (value);                                        \
    translation_ptr += 4                                                        \

#define generate_cycle_update()                                               \
    if (cycle_count != 0)                                                      \
    {                                                                          \
        if (((s32)cycle_count >= -2048) && ((s32)cycle_count <= 2047))         \
        {                                                                      \
            rv_addi(reg_cycles, reg_cycles, -(s32)cycle_count);                \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            generate_load_imm(reg_temp, cycle_count);                          \
            rv_sub(reg_cycles, reg_cycles, reg_temp);                          \
        }                                                                      \
        cycle_count = 0;                                                       \
    }

#define generate_cycle_update_force()                                         \
{                                                                             \
    if (((s32)cycle_count >= -2048) && ((s32)cycle_count <= 2047))            \
    {                                                                         \
        rv_addi(reg_cycles, reg_cycles, -(s32)cycle_count);                   \
    }                                                                         \
    else                                                                      \
    {                                                                         \
        generate_load_imm(reg_temp, cycle_count);                             \
        rv_sub(reg_cycles, reg_cycles, reg_temp);                             \
    }                                                                         \
    cycle_count = 0;                                                          \
}

#define generate_branch_patch_conditional(dest, label)                        \
    rv_patch_branch((u32 *)(dest), (label))                                   \

/* Reserve space for a 2-instruction far-jump sequence (AUIPC+JALR).      
 * The actual target is filled later by generate_branch_patch_unconditional. */
#define emit_branch_filler(writeback_location)                                \
    (writeback_location) = translation_ptr;                                   \
    rv_nop_32();                                                              \
    rv_nop_32();                                                              \

#define generate_branch_patch_unconditional(dest, target)                     \
{                                                                             \
    s32 _bp_off = rv_jal_offset((target), (u32 *)(dest));                     \
    if (_bp_off >= -(1 << 20) && _bp_off < (1 << 20))                        \
    {                                                                         \
        /* Short range: single JAL x0, offset (2nd slot stays NOP) */         \
        u32 *_bp_inst = (u32 *)(dest);                                        \
        _bp_inst[0] = (rv_zero << 7) | RV_OP_JAL;                            \
        rv_patch_jal(_bp_inst, (target));                                     \
    }                                                                         \
    else                                                                      \
    {                                                                         \
        rv_patch_far_jump((u32 *)(dest), (target));                           \
    }                                                                         \
}

#define generate_branch_no_cycle_update(writeback_location, new_pc)           \
    if (pc == idle_loop_target_pc)                                            \
    {                                                                         \
        generate_load_imm(reg_cycles, 0);                                     \
        generate_load_pc(reg_a0, new_pc);                                     \
        generate_function_call(rv_update_gba);                                \
        emit_branch_filler(writeback_location);                               \
    }                                                                         \
    else                                                                      \
    {                                                                         \
        /* If cycles >= 0, skip the update_gba() call and jump to target. */  \
        u8 *_bge_ptr = translation_ptr;                                       \
        rv_bge(reg_cycles, reg_zero, 0);       /* placeholder offset */       \
        generate_load_pc(reg_a0, new_pc);                                     \
        generate_function_call(rv_update_gba);                                \
        rv_patch_branch((u32 *)_bge_ptr, translation_ptr);                    \
        emit_branch_filler(writeback_location);                               \
    }

#define generate_branch_cycle_update(writeback_location, new_pc)              \
    generate_cycle_update();                                                  \
    generate_branch_no_cycle_update(writeback_location, new_pc)               \

#define generate_indirect_branch_no_cycle_update(type)                        \
    generate_function_call(rv_indirect_branch_##type)                           \

#define generate_indirect_branch_cycle_update(type)                           \
    generate_cycle_update();                                                    \
    generate_indirect_branch_no_cycle_update(type)                              \

#define block_prologue_size 0
#define generate_block_prologue()                                             \
    generate_load_imm(reg_pc, stored_pc)

#define check_generate_n_flag                                                 \
    (flag_status & 0x08)                                                        \

#define check_generate_z_flag                                                 \
    (flag_status & 0x04)                                                        \

#define check_generate_c_flag                                                 \
    (flag_status & 0x02)                                                        \

#define check_generate_v_flag                                                 \
    (flag_status & 0x01)                                                        \

#define generate_load_reg_pc(ireg, reg_index, pc_offset)                      \
    if (reg_index == REG_PC)                                                    \
    {                                                                           \
        generate_load_pc(ireg, (pc + pc_offset));                                 \
    }                                                                           \
    else                                                                        \
    {                                                                           \
        generate_load_reg(ireg, reg_index);                                       \
    }                                                                           \

#define check_load_reg_pc(arm_reg, reg_index, pc_offset)                      \
    if (reg_index == REG_PC)                                                    \
    {                                                                           \
        reg_index = arm_reg;                                                      \
        generate_load_pc(arm_to_rv_reg[arm_reg], (pc + pc_offset));               \
    }                                                                           \

#define generate_indirect_branch_arm()                                         \
    {                                                                          \
        if (condition == 0x0E)                                                 \
        {                                                                      \
            generate_indirect_branch_cycle_update(arm);                        \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            generate_indirect_branch_no_cycle_update(arm);                     \
        }                                                                      \
    }

#define generate_indirect_branch_dual()                                        \
    {                                                                          \
        if (condition == 0x0E)                                                 \
        {                                                                      \
            generate_indirect_branch_cycle_update(dual);                       \
        }                                                                      \
        else                                                                   \
        {                                                                      \
            generate_indirect_branch_no_cycle_update(dual);                    \
        }                                                                      \
    }

#define check_store_reg_pc_no_flags(reg_index)                                 \
    if (reg_index == REG_PC)                                                   \
    {                                                                          \
        generate_indirect_branch_arm();                                        \
    }                                                                          \

#define check_store_reg_pc_flags(reg_index)                                    \
    if (reg_index == REG_PC)                                                   \
    {                                                                          \
        generate_function_call(execute_spsr_restore);                          \
        generate_indirect_branch_dual();                                       \
    }                                                                          \

#define generate_shift_imm_lsl_no_flags(arm_reg, _rm, _shift)                 \
    check_load_reg_pc(arm_reg, _rm, 8);                                        \
    if (_shift != 0)                                                            \
    {                                                                           \
        rv_slli(arm_to_rv_reg[arm_reg], arm_to_rv_reg[_rm], _shift);           \
        _rm = arm_reg;                                                          \
    }                                                                           \

#define generate_shift_imm_lsr_no_flags(arm_reg, _rm, _shift)                 \
    if (_shift != 0)                                                            \
    {                                                                           \
        check_load_reg_pc(arm_reg, _rm, 8);                                    \
        rv_srli(arm_to_rv_reg[arm_reg], arm_to_rv_reg[_rm], _shift);           \
    }                                                                           \
    else                                                                        \
    {                                                                           \
        rv_mv(arm_to_rv_reg[arm_reg], reg_zero);                               \
    }                                                                           \
    _rm = arm_reg                                                               \

#define generate_shift_imm_asr_no_flags(arm_reg, _rm, _shift)                 \
    check_load_reg_pc(arm_reg, _rm, 8);                                        \
    rv_srai(arm_to_rv_reg[arm_reg], arm_to_rv_reg[_rm],                        \
            (_shift) ? (_shift) : 31);                                         \
    _rm = arm_reg                                                               \

#define generate_shift_imm_ror_no_flags(arm_reg, _rm, _shift)                 \
    check_load_reg_pc(arm_reg, _rm, 8);                                        \
    if (_shift != 0)                                                            \
    {                                                                           \
        rv_rori(arm_to_rv_reg[arm_reg], arm_to_rv_reg[_rm], _shift, reg_temp);\
    }                                                                           \
    else                                                                        \
    {                                                                           \
        rv_srli(arm_to_rv_reg[arm_reg], arm_to_rv_reg[_rm], 1);                \
        rv_slli(reg_temp, reg_c_cache, 31);                                    \
        rv_or(arm_to_rv_reg[arm_reg], arm_to_rv_reg[arm_reg], reg_temp);       \
    }                                                                           \
    _rm = arm_reg                                                               \

#define generate_shift_imm_lsl_flags(arm_reg, _rm, _shift)                    \
    check_load_reg_pc(arm_reg, _rm, 8);                                       \
    if (_shift != 0)                                                           \
    {                                                                          \
        rv_srli(reg_c_cache, arm_to_rv_reg[_rm], 32 - (_shift));              \
        rv_andi(reg_c_cache, reg_c_cache, 1);                                 \
        rv_slli(arm_to_rv_reg[arm_reg], arm_to_rv_reg[_rm], _shift);          \
        _rm = arm_reg;                                                         \
    }

#define generate_shift_imm_lsr_flags(arm_reg, _rm, _shift)                    \
    check_load_reg_pc(arm_reg, _rm, 8);                                       \
    if (_shift != 0)                                                           \
    {                                                                          \
        rv_srli(reg_c_cache, arm_to_rv_reg[_rm], (_shift) - 1);               \
        rv_andi(reg_c_cache, reg_c_cache, 1);                                 \
        rv_srli(arm_to_rv_reg[arm_reg], arm_to_rv_reg[_rm], _shift);          \
    }                                                                          \
    else                                                                       \
    {                                                                          \
        rv_srli(reg_c_cache, arm_to_rv_reg[_rm], 31);                         \
        rv_mv(arm_to_rv_reg[arm_reg], reg_zero);                              \
    }                                                                          \
    _rm = arm_reg                                                              \

#define generate_shift_imm_asr_flags(arm_reg, _rm, _shift)                    \
    check_load_reg_pc(arm_reg, _rm, 8);                                       \
    if (_shift != 0)                                                           \
    {                                                                          \
        rv_srli(reg_c_cache, arm_to_rv_reg[_rm], (_shift) - 1);               \
        rv_andi(reg_c_cache, reg_c_cache, 1);                                 \
        rv_srai(arm_to_rv_reg[arm_reg], arm_to_rv_reg[_rm], _shift);          \
    }                                                                          \
    else                                                                       \
    {                                                                          \
        rv_srli(reg_c_cache, arm_to_rv_reg[_rm], 31);                         \
        rv_srai(arm_to_rv_reg[arm_reg], arm_to_rv_reg[_rm], 31);              \
    }                                                                          \
    _rm = arm_reg                                                              \

#define generate_shift_imm_ror_flags(arm_reg, _rm, _shift)                    \
    check_load_reg_pc(arm_reg, _rm, 8);                                       \
    if (_shift != 0)                                                           \
    {                                                                          \
        rv_srli(reg_c_cache, arm_to_rv_reg[_rm], (_shift) - 1);               \
        rv_andi(reg_c_cache, reg_c_cache, 1);                                 \
        rv_rori(arm_to_rv_reg[arm_reg], arm_to_rv_reg[_rm], _shift, reg_temp);\
    }                                                                          \
    else                                                                       \
    {                                                                          \
        /* RRX: save old C, set new C = bit0(Rm), result = (old_C<<31)|(Rm>>1) */ \
        rv_slli(reg_temp, reg_c_cache, 31);                                   \
        rv_andi(reg_c_cache, arm_to_rv_reg[_rm], 1);                          \
        rv_srli(arm_to_rv_reg[arm_reg], arm_to_rv_reg[_rm], 1);               \
        rv_or(arm_to_rv_reg[arm_reg], arm_to_rv_reg[arm_reg], reg_temp);      \
    }                                                                          \
    _rm = arm_reg                                                              \

#define generate_shift_reg_lsl_no_flags(_rm, _rs)                             \
{                                                                             \
    generate_load_reg_pc(reg_a1, _rs, 8);                                      \
    rv_andi(reg_a1, reg_a1, 0xFF);                                             \
    generate_load_reg_pc(reg_a0, _rm, 12);                                     \
    generate_load_imm(reg_temp2, 32);                                          \
    u8 *_sr1 = translation_ptr;                                                \
    rv_bgeu(reg_a1, reg_temp2, 0);                                             \
    rv_sll(reg_a0, reg_a0, reg_a1);                                            \
    u8 *_sr2 = translation_ptr;                                                \
    rv_j(0);                                                                   \
    rv_patch_branch((u32 *)_sr1, translation_ptr);                             \
    rv_mv(reg_a0, reg_zero);                                                   \
    rv_patch_jal((u32 *)_sr2, translation_ptr);                                \
}                                                                             \

#define generate_shift_reg_lsr_no_flags(_rm, _rs)                             \
{                                                                             \
    generate_load_reg_pc(reg_a1, _rs, 8);                                      \
    rv_andi(reg_a1, reg_a1, 0xFF);                                             \
    generate_load_reg_pc(reg_a0, _rm, 12);                                     \
    generate_load_imm(reg_temp2, 32);                                          \
    u8 *_sr1 = translation_ptr;                                                \
    rv_bgeu(reg_a1, reg_temp2, 0);                                             \
    rv_srl(reg_a0, reg_a0, reg_a1);                                            \
    u8 *_sr2 = translation_ptr;                                                \
    rv_j(0);                                                                   \
    rv_patch_branch((u32 *)_sr1, translation_ptr);                             \
    rv_mv(reg_a0, reg_zero);                                                   \
    rv_patch_jal((u32 *)_sr2, translation_ptr);                                \
}                                                                             \

#define generate_shift_reg_asr_no_flags(_rm, _rs)                             \
{                                                                             \
    generate_load_reg_pc(reg_a1, _rs, 8);                                      \
    rv_andi(reg_a1, reg_a1, 0xFF);                                             \
    generate_load_reg_pc(reg_a0, _rm, 12);                                     \
    generate_load_imm(reg_temp2, 32);                                          \
    u8 *_sr1 = translation_ptr;                                                \
    rv_bltu(reg_a1, reg_temp2, 0);                                             \
    rv_srai(reg_a0, reg_a0, 31);                                               \
    u8 *_sr2 = translation_ptr;                                                \
    rv_j(0);                                                                   \
    rv_patch_branch((u32 *)_sr1, translation_ptr);                             \
    rv_sra(reg_a0, reg_a0, reg_a1);                                            \
    rv_patch_jal((u32 *)_sr2, translation_ptr);                                \
}                                                                             \

#define generate_shift_reg_ror_no_flags(_rm, _rs)                             \
{                                                                             \
    generate_load_reg_pc(reg_a1, _rs, 8);                                      \
    rv_andi(reg_a1, reg_a1, 0xFF);                                             \
    generate_load_reg_pc(reg_a0, _rm, 12);                                     \
    rv_ror(reg_a0, reg_a0, reg_a1, reg_temp, reg_temp2);                      \
}                                                                             \

#define generate_shift_reg_lsl_flags(_rm, _rs)                                \
{                                                                             \
    generate_load_reg_pc(reg_a1, _rs, 8);                                      \
    rv_andi(reg_a1, reg_a1, 0xFF);                                             \
    generate_load_reg_pc(reg_a0, _rm, 12);                                     \
    u8 *_sr0 = translation_ptr;                                                \
    rv_beqz(reg_a1, 0);                                                        \
    rv_addi(reg_temp2, rv_zero, 32);                                           \
    u8 *_sr1 = translation_ptr;                                                \
    rv_bgeu(reg_a1, reg_temp2, 0);                                             \
    rv_addi(reg_temp, reg_a1, -1);                                             \
    rv_sll(reg_c_cache, reg_a0, reg_temp);                                     \
    rv_srli(reg_c_cache, reg_c_cache, 31);                                     \
    rv_sll(reg_a0, reg_a0, reg_a1);                                            \
    u8 *_sr2 = translation_ptr;                                                \
    rv_j(0);                                                                   \
    /* amt >= 32: C = bit0(Rm) if amt==32, else 0 */                           \
    rv_patch_branch((u32 *)_sr1, translation_ptr);                             \
    rv_andi(reg_c_cache, reg_a0, 1);                                           \
    u8 *_sr3 = translation_ptr;                                                \
    rv_beq(reg_a1, reg_temp2, 0);                                              \
    rv_mv(reg_c_cache, reg_zero);                                              \
    rv_patch_branch((u32 *)_sr3, translation_ptr);                             \
    rv_mv(reg_a0, reg_zero);                                                   \
    rv_patch_jal((u32 *)_sr2, translation_ptr);                                \
    rv_patch_branch((u32 *)_sr0, translation_ptr);                             \
}                                                                             \

#define generate_shift_reg_lsr_flags(_rm, _rs)                                \
{                                                                             \
    generate_load_reg_pc(reg_a1, _rs, 8);                                      \
    rv_andi(reg_a1, reg_a1, 0xFF);                                             \
    generate_load_reg_pc(reg_a0, _rm, 12);                                     \
    u8 *_sr0 = translation_ptr;                                                \
    rv_beqz(reg_a1, 0);                                                        \
    rv_addi(reg_temp2, rv_zero, 32);                                           \
    u8 *_sr1 = translation_ptr;                                                \
    rv_bgeu(reg_a1, reg_temp2, 0);                                             \
    rv_addi(reg_temp, reg_a1, -1);                                             \
    rv_srl(reg_c_cache, reg_a0, reg_temp);                                     \
    rv_andi(reg_c_cache, reg_c_cache, 1);                                      \
    rv_srl(reg_a0, reg_a0, reg_a1);                                            \
    u8 *_sr2 = translation_ptr;                                                \
    rv_j(0);                                                                   \
    /* amt >= 32: C = bit31(Rm) if amt==32, else 0 */                          \
    rv_patch_branch((u32 *)_sr1, translation_ptr);                             \
    rv_srli(reg_c_cache, reg_a0, 31);                                          \
    u8 *_sr3 = translation_ptr;                                                \
    rv_beq(reg_a1, reg_temp2, 0);                                              \
    rv_mv(reg_c_cache, reg_zero);                                              \
    rv_patch_branch((u32 *)_sr3, translation_ptr);                             \
    rv_mv(reg_a0, reg_zero);                                                   \
    rv_patch_jal((u32 *)_sr2, translation_ptr);                                \
    rv_patch_branch((u32 *)_sr0, translation_ptr);                             \
}                                                                             \

#define generate_shift_reg_asr_flags(_rm, _rs)                                \
{                                                                             \
    generate_load_reg_pc(reg_a1, _rs, 8);                                      \
    rv_andi(reg_a1, reg_a1, 0xFF);                                             \
    generate_load_reg_pc(reg_a0, _rm, 12);                                     \
    u8 *_sr0 = translation_ptr;                                                \
    rv_beqz(reg_a1, 0);                                                        \
    rv_addi(reg_temp2, rv_zero, 32);                                           \
    u8 *_sr1 = translation_ptr;                                                \
    rv_bltu(reg_a1, reg_temp2, 0);                                             \
    rv_srli(reg_c_cache, reg_a0, 31);                                          \
    rv_srai(reg_a0, reg_a0, 31);                                               \
    u8 *_sr2 = translation_ptr;                                                \
    rv_j(0);                                                                   \
    rv_patch_branch((u32 *)_sr1, translation_ptr);                             \
    rv_addi(reg_temp, reg_a1, -1);                                             \
    rv_srl(reg_c_cache, reg_a0, reg_temp);                                     \
    rv_andi(reg_c_cache, reg_c_cache, 1);                                      \
    rv_sra(reg_a0, reg_a0, reg_a1);                                            \
    rv_patch_jal((u32 *)_sr2, translation_ptr);                                \
    rv_patch_branch((u32 *)_sr0, translation_ptr);                             \
}                                                                             \

#define generate_shift_reg_ror_flags(_rm, _rs)                                \
{                                                                             \
    generate_load_reg_pc(reg_a1, _rs, 8);                                      \
    rv_andi(reg_a1, reg_a1, 0xFF);                                             \
    generate_load_reg_pc(reg_a0, _rm, 12);                                     \
    u8 *_sr0 = translation_ptr;                                                \
    rv_beqz(reg_a1, 0);                                                        \
    rv_addi(reg_temp, reg_a1, -1);                                             \
    rv_srl(reg_c_cache, reg_a0, reg_temp);                                     \
    rv_andi(reg_c_cache, reg_c_cache, 1);                                      \
    rv_ror(reg_a0, reg_a0, reg_a1, reg_temp, reg_temp2);                      \
    u8 *_sr1 = translation_ptr;                                                \
    rv_j(0);                                                                   \
    /* amt == 0: RRX (rotate right extended) */                                \
    rv_andi(reg_c_cache, reg_a0, 1);                                           \
    rv_srli(reg_a0, reg_a0, 1);                                                \
    rv_slli(reg_temp, reg_c_cache, 31);                                        \
    rv_or(reg_a0, reg_a0, reg_temp);                                           \
    rv_patch_jal((u32 *)_sr1, translation_ptr);                                \
    rv_patch_branch((u32 *)_sr0, translation_ptr);                             \
}                                                                             \

#define generate_shift_imm(arm_reg, name, flags_op)                           \
{                                                                             \
    u32 shift = (opcode >> 7) & 0x1F;                                          \
    generate_shift_imm_##name##_##flags_op(arm_reg, rm, shift);               \
}                                                                             \

#define generate_shift_reg(arm_reg, name, flags_op)                           \
{                                                                             \
    u32 rs = ((opcode >> 8) & 0x0F);                                           \
    generate_shift_reg_##name##_##flags_op(rm, rs);                           \
    rm = arm_reg;                                                              \
}                                                                             \

#define generate_load_rm_sh(flags_op)                                         \
{                                                                             \
    switch ((opcode >> 4) & 0x07)                                             \
    {                                                                         \
        case 0x0: generate_shift_imm(arm_reg_a0, lsl, flags_op); break;      \
        case 0x1: generate_shift_reg(arm_reg_a0, lsl, flags_op); break;      \
        case 0x2: generate_shift_imm(arm_reg_a0, lsr, flags_op); break;      \
        case 0x3: generate_shift_reg(arm_reg_a0, lsr, flags_op); break;      \
        case 0x4: generate_shift_imm(arm_reg_a0, asr, flags_op); break;      \
        case 0x5: generate_shift_reg(arm_reg_a0, asr, flags_op); break;      \
        case 0x6: generate_shift_imm(arm_reg_a0, ror, flags_op); break;      \
        case 0x7: generate_shift_reg(arm_reg_a0, ror, flags_op); break;      \
    }                                                                         \
}

#define generate_load_offset_sh()                                             \
    generate_load_rm_sh(no_flags)

#define generate_op_and_imm(_rd, _rn)                                         \
    generate_alu_imm(andi, and, _rd, _rn, imm)                                \

#define generate_op_orr_imm(_rd, _rn)                                         \
    generate_alu_imm(ori, or, _rd, _rn, imm)                                  \

#define generate_op_eor_imm(_rd, _rn)                                         \
    generate_alu_imm(xori, xor, _rd, _rn, imm)                                \

#ifdef HAVE_ZBB
#define generate_op_bic_imm(_rd, _rn)                                         \
{                                                                             \
    generate_load_imm(reg_temp3, (imm));                                      \
    rv_andn(_rd, _rn, reg_temp3);                                             \
}                                                                             \

#else
#define generate_op_bic_imm(_rd, _rn)                                         \
{                                                                             \
    if (((s32)(~(imm)) >= -2048) && ((s32)(~(imm)) <= 2047))                  \
    {                                                                         \
        rv_andi(_rd, _rn, ~(imm));                                            \
    }                                                                         \
    else                                                                      \
    {                                                                         \
        generate_load_imm(reg_temp3, ~(imm));                                 \
        rv_and(_rd, _rn, reg_temp3);                                          \
    }                                                                         \
}                                                                             \

#endif

#define generate_op_and_reg(_rd, _rn, _rm)                                    \
    rv_and(_rd, _rn, _rm)                                                     \

#define generate_op_orr_reg(_rd, _rn, _rm)                                    \
    rv_or(_rd, _rn, _rm)                                                      \

#define generate_op_eor_reg(_rd, _rn, _rm)                                    \
    rv_xor(_rd, _rn, _rm)                                                     \

#ifdef HAVE_ZBB
#define generate_op_bic_reg(_rd, _rn, _rm)                                    \
    rv_andn(_rd, _rn, _rm)                                                    \

#else
#define generate_op_bic_reg(_rd, _rn, _rm)                                    \
{                                                                             \
    rv_xori(reg_temp3, _rm, -1);                                              \
    rv_and(_rd, _rn, reg_temp3);                                              \
}                                                                             \

#endif

#define generate_op_muls_reg(_rd, _rn, _rm)                                   \
    rv_mul(_rd, _rn, _rm);                                                    \
    generate_op_logic_flags(_rd)                                              \

#define generate_op_add_reg(_rd, _rn, _rm)                                    \
    rv_add(_rd, _rn, _rm)                                                     \

#define generate_op_sub_reg(_rd, _rn, _rm)                                    \
    rv_sub(_rd, _rn, _rm)                                                     \

#define generate_op_rsb_reg(_rd, _rn, _rm)                                    \
    rv_sub(_rd, _rm, _rn)                                                     \

#define generate_op_adc_reg(_rd, _rn, _rm)                                    \
{                                                                             \
    rv_add(_rd, _rn, _rm);                                                    \
    rv_add(_rd, _rd, reg_c_cache);                                            \
}                                                                             \

#define generate_op_sbc_reg(_rd, _rn, _rm)                                    \
{                                                                             \
    rv_sub(_rd, _rn, _rm);                                                    \
    rv_addi(reg_temp3, reg_c_cache, -1);                                      \
    rv_add(_rd, _rd, reg_temp3);                                              \
}                                                                             \

#define generate_op_rsc_reg(_rd, _rn, _rm)                                    \
{                                                                             \
    rv_sub(_rd, _rm, _rn);                                                    \
    rv_addi(reg_temp3, reg_c_cache, -1);                                      \
    rv_add(_rd, _rd, reg_temp3);                                              \
}                                                                             \

#define generate_op_add_imm(_rd, _rn)                                         \
    generate_alu_imm(addi, add, _rd, _rn, imm)                                \

#define generate_op_sub_imm(_rd, _rn)                                         \
    if (((s32)(imm) >= -2047) && ((s32)(imm) <= 2048))                        \
    {                                                                         \
        rv_addi(_rd, _rn, -(s32)(imm));                                       \
    }                                                                         \
    else                                                                      \
    {                                                                         \
        generate_load_imm(reg_temp, imm);                                     \
        rv_sub(_rd, _rn, reg_temp);                                           \
    }                                                                         \

#define generate_op_rsb_imm(_rd, _rn)                                         \
    if (imm)                                                                  \
    {                                                                         \
        generate_load_imm(reg_temp3, imm);                                    \
        rv_sub(_rd, reg_temp3, _rn);                                          \
    }                                                                         \
    else                                                                      \
    {                                                                         \
        rv_sub(_rd, reg_zero, _rn);                                           \
    }                                                                         \

#define generate_op_adc_imm(_rd, _rn)                                         \
    if (imm)                                                                  \
    {                                                                         \
        generate_alu_imm(addi, add, _rd, _rn, imm);                           \
        rv_add(_rd, _rd, reg_c_cache);                                        \
    }                                                                         \
    else                                                                      \
    {                                                                         \
        rv_add(_rd, _rn, reg_c_cache);                                        \
    }                                                                         \

#define generate_op_sbc_imm(_rd, _rn)                                         \
{                                                                             \
    generate_sub_imm(_rd, _rn, (imm) + 1);                                    \
    rv_add(_rd, _rd, reg_c_cache);                                            \
}                                                                             \

#define generate_op_rsc_imm(_rd, _rn)                                         \
{                                                                             \
    generate_load_imm(reg_temp3, (imm) - 1);                                  \
    rv_sub(_rd, reg_temp3, _rn);                                              \
    rv_add(_rd, _rd, reg_c_cache);                                            \
}                                                                             \

#define generate_op_mov_imm(_rd, _rn)                                         \
    generate_load_imm(_rd, imm)                                               \

#define generate_op_movs_imm(_rd, _rn)                                        \
    generate_load_imm(_rd, imm);                                              \
    generate_op_logic_flags(_rd)                                              \

#define generate_op_mvn_imm(_rd, _rn)                                         \
    generate_load_imm(_rd, ~(imm))                                            \

#define generate_op_mvns_imm(_rd, _rn)                                        \
    generate_load_imm(_rd, ~(imm));                                           \
    generate_op_logic_flags(_rd)                                              \

#define generate_op_mov_reg(_rd, _rn, _rm)                                    \
    rv_mv(_rd, _rm)                                                           \

#define generate_op_movs_reg(_rd, _rn, _rm)                                   \
    rv_mv(_rd, _rm);                                                          \
    generate_op_logic_flags(_rd)                                              \

#define generate_op_mvn_reg(_rd, _rn, _rm)                                    \
    rv_xori(_rd, _rm, -1)                                                     \

#define generate_op_mvns_reg(_rd, _rn, _rm)                                   \
    rv_xori(_rd, _rm, -1);                                                    \
    generate_op_logic_flags(_rd)                                              \

#define generate_op_ands_imm(_rd, _rn)                                        \
    generate_op_and_imm(_rd, _rn);                                            \
    generate_op_logic_flags(_rd)                                              \

#define generate_op_orrs_imm(_rd, _rn)                                        \
    generate_op_orr_imm(_rd, _rn);                                            \
    generate_op_logic_flags(_rd)                                              \

#define generate_op_eors_imm(_rd, _rn)                                        \
    generate_op_eor_imm(_rd, _rn);                                            \
    generate_op_logic_flags(_rd)                                              \

#define generate_op_bics_imm(_rd, _rn)                                        \
    generate_op_bic_imm(_rd, _rn);                                            \
    generate_op_logic_flags(_rd)                                              \

#define generate_op_ands_reg(_rd, _rn, _rm)                                   \
    generate_op_and_reg(_rd, _rn, _rm);                                       \
    generate_op_logic_flags(_rd)                                              \

#define generate_op_orrs_reg(_rd, _rn, _rm)                                   \
    generate_op_orr_reg(_rd, _rn, _rm);                                       \
    generate_op_logic_flags(_rd)                                              \

#define generate_op_eors_reg(_rd, _rn, _rm)                                   \
    generate_op_eor_reg(_rd, _rn, _rm);                                       \
    generate_op_logic_flags(_rd)                                              \

#define generate_op_bics_reg(_rd, _rn, _rm)                                   \
    generate_op_bic_reg(_rd, _rn, _rm);                                       \
    generate_op_logic_flags(_rd)                                              \

#define generate_op_adds_reg(_rd, _rn, _rm)                                   \
{                                                                             \
    u32 _need_cv = (check_generate_c_flag) || (check_generate_v_flag);        \
    u32 _rn_f = ((_rd) == (_rn) && _need_cv) ? (u32)reg_temp3 : (u32)(_rn);  \
    u32 _rm_f = ((_rd) == (_rm) && (check_generate_v_flag))                   \
                ? (u32)reg_save0 : (u32)(_rm);                                \
    if ((_rd) == (_rn) && _need_cv) rv_mv(reg_temp3, _rn);                    \
    if ((_rd) == (_rm) && (check_generate_v_flag)) rv_mv(reg_save0, _rm);     \
    rv_add(_rd, _rn, _rm);                                                    \
    generate_op_add_flags(_rd, _rn_f, _rm_f);                                 \
}

#define generate_op_subs_reg(_rd, _rn, _rm)                                   \
{                                                                             \
    u32 _need_cv = (check_generate_c_flag) || (check_generate_v_flag);        \
    u32 _rn_f = ((_rd) == (_rn) && _need_cv) ? (u32)reg_temp3 : (u32)(_rn);  \
    u32 _rm_f = ((_rd) == (_rm) && _need_cv) ? (u32)reg_save0 : (u32)(_rm);  \
    if ((_rd) == (_rn) && _need_cv) rv_mv(reg_temp3, _rn);                    \
    if ((_rd) == (_rm) && _need_cv) rv_mv(reg_save0, _rm);                    \
    rv_sub(_rd, _rn, _rm);                                                    \
    generate_op_sub_flags(_rd, _rn_f, _rm_f);                                 \
}

#define generate_op_rsbs_reg(_rd, _rn, _rm)                                   \
{                                                                             \
    u32 _need_cv = (check_generate_c_flag) || (check_generate_v_flag);        \
    u32 _rm_f = ((_rd) == (_rm) && _need_cv) ? (u32)reg_temp3 : (u32)(_rm);  \
    u32 _rn_f = ((_rd) == (_rn) && _need_cv) ? (u32)reg_save0 : (u32)(_rn);  \
    if ((_rd) == (_rm) && _need_cv) rv_mv(reg_temp3, _rm);                    \
    if ((_rd) == (_rn) && _need_cv) rv_mv(reg_save0, _rn);                    \
    rv_sub(_rd, _rm, _rn);                                                    \
    generate_op_sub_flags(_rd, _rm_f, _rn_f);                                 \
}

#define generate_op_adcs_reg(_rd, _rn, _rm)                                   \
{                                                                             \
    if (check_generate_v_flag)                                                \
        rv_mv(reg_v_cache, _rm);         /* save _rm for V flag */            \
    rv_add(reg_temp3, _rm, reg_c_cache);                                      \
    rv_sltu(reg_temp2, reg_temp3, _rm);                                       \
    rv_mv(reg_save0, _rn);                                                    \
    rv_add(_rd, _rn, reg_temp3);                                              \
    rv_sltu(reg_c_cache, _rd, reg_save0);                                     \
    rv_or(reg_c_cache, reg_c_cache, reg_temp2);                               \
    if (check_generate_v_flag)                                                \
    {                                                                         \
        rv_xor(reg_temp, reg_save0, _rd);                                     \
        rv_xor(reg_temp2, reg_v_cache, _rd);                                  \
        rv_and(reg_v_cache, reg_temp, reg_temp2);                             \
        rv_srli(reg_v_cache, reg_v_cache, 31);                                \
    }                                                                         \
    generate_op_logic_flags(_rd)                                              \
}                                                                             \

#define generate_op_sbcs_reg(_rd, _rn, _rm)                                   \
{                                                                             \
    if (check_generate_v_flag)                                                \
        rv_mv(reg_v_cache, _rm);         /* save _rm for V flag */            \
    rv_xori(reg_temp3, reg_c_cache, 1);                                       \
    rv_add(reg_temp2, _rm, reg_temp3);                                        \
    rv_sltu(reg_temp3, reg_temp2, _rm);                                       \
    rv_mv(reg_save0, _rn);                                                    \
    rv_sub(_rd, _rn, reg_temp2);                                              \
    rv_sltu(reg_c_cache, reg_save0, reg_temp2);                               \
    rv_or(reg_c_cache, reg_c_cache, reg_temp3);                               \
    rv_xori(reg_c_cache, reg_c_cache, 1);                                     \
    if (check_generate_v_flag)                                                \
    {                                                                         \
        rv_xor(reg_temp, reg_save0, reg_v_cache);                             \
        rv_xor(reg_temp2, reg_save0, _rd);                                    \
        rv_and(reg_v_cache, reg_temp, reg_temp2);                             \
        rv_srli(reg_v_cache, reg_v_cache, 31);                                \
    }                                                                         \
    generate_op_logic_flags(_rd)                                              \
}                                                                             \

#define generate_op_rscs_reg(_rd, _rn, _rm)                                   \
{                                                                             \
    if (check_generate_v_flag)                                                \
        rv_mv(reg_v_cache, _rn);         /* save _rn for V flag */            \
    rv_xori(reg_temp3, reg_c_cache, 1);                                       \
    rv_add(reg_temp2, _rn, reg_temp3);                                        \
    rv_sltu(reg_temp3, reg_temp2, _rn);                                       \
    rv_mv(reg_save0, _rm);                                                    \
    rv_sub(_rd, _rm, reg_temp2);                                              \
    rv_sltu(reg_c_cache, reg_save0, reg_temp2);                               \
    rv_or(reg_c_cache, reg_c_cache, reg_temp3);                               \
    rv_xori(reg_c_cache, reg_c_cache, 1);                                     \
    if (check_generate_v_flag)                                                \
    {                                                                         \
        rv_xor(reg_temp, reg_save0, reg_v_cache);                             \
        rv_xor(reg_temp2, reg_save0, _rd);                                    \
        rv_and(reg_v_cache, reg_temp, reg_temp2);                             \
        rv_srli(reg_v_cache, reg_v_cache, 31);                                \
    }                                                                         \
    generate_op_logic_flags(_rd)                                              \
}                                                                             \

#define generate_op_neg_reg(_rd, _rn, _rm)                                    \
    generate_op_subs_reg(_rd, reg_zero, _rm)                                  \

#define generate_op_adds_imm(_rd, _rn)                                        \
{                                                                             \
    if ((s32)(imm) >= 1 && (s32)(imm) <= 2047 && !check_generate_v_flag)     \
    {                                                                         \
        /* Small positive imm, no V needed: addi avoids loading imm */        \
        u32 _rn_f = ((_rd) == (_rn) && (check_generate_c_flag))              \
                    ? (u32)reg_save0 : (u32)(_rn);                            \
        if ((_rd) == (_rn) && (check_generate_c_flag)) rv_mv(reg_save0, _rn); \
        rv_addi(_rd, _rn, (s32)(imm));                                        \
        if (check_generate_c_flag)                                            \
            rv_sltu(reg_c_cache, _rd, _rn_f);                                 \
        generate_op_logic_flags(_rd)                                          \
    }                                                                         \
    else                                                                      \
    {                                                                         \
        u32 _need_cv = (check_generate_c_flag) || (check_generate_v_flag);    \
        u32 _rn_f = ((_rd) == (_rn) && _need_cv)                             \
                    ? (u32)reg_save0 : (u32)(_rn);                            \
        generate_load_imm(reg_temp3, imm);                                    \
        if ((_rd) == (_rn) && _need_cv) rv_mv(reg_save0, _rn);               \
        rv_add(_rd, _rn, reg_temp3);                                          \
        generate_op_add_flags(_rd, _rn_f, reg_temp3);                         \
    }                                                                         \
}

#define generate_op_subs_imm(_rd, _rn)                                        \
{                                                                             \
    if ((s32)(imm) >= 1 && (s32)(imm) <= 2047)                               \
    {                                                                         \
        /* Small positive imm: use addi/sltiu to avoid loading imm */         \
        u32 _need_cv = (check_generate_c_flag) || (check_generate_v_flag);    \
        u32 _rn_f = ((_rd) == (_rn) && _need_cv)                             \
                    ? (u32)reg_save0 : (u32)(_rn);                            \
        if ((_rd) == (_rn) && _need_cv) rv_mv(reg_save0, _rn);               \
        rv_addi(_rd, _rn, -(s32)(imm));                                       \
        if (check_generate_c_flag)                                            \
        {                                                                     \
            rv_sltiu(reg_c_cache, _rn_f, (s32)(imm));                         \
            rv_xori(reg_c_cache, reg_c_cache, 1);                             \
        }                                                                     \
        generate_op_logic_flags(_rd)                                          \
        if (check_generate_v_flag)                                            \
        {                                                                     \
            /* imm>0 ⇒ imm[31]=0, so V = rn[31] & ~rd[31] */                 \
            rv_xor(reg_temp, _rn_f, _rd);                                     \
            rv_and(reg_v_cache, _rn_f, reg_temp);                             \
            rv_srli(reg_v_cache, reg_v_cache, 31);                            \
        }                                                                     \
    }                                                                         \
    else                                                                      \
    {                                                                         \
        u32 _need_cv2 = (check_generate_c_flag) || (check_generate_v_flag);   \
        u32 _rn_f = ((_rd) == (_rn) && _need_cv2)                            \
                    ? (u32)reg_save0 : (u32)(_rn);                            \
        generate_load_imm(reg_temp3, imm);                                    \
        if ((_rd) == (_rn) && _need_cv2) rv_mv(reg_save0, _rn);              \
        rv_sub(_rd, _rn, reg_temp3);                                          \
        generate_op_sub_flags(_rd, _rn_f, reg_temp3);                         \
    }                                                                         \
}

#define generate_op_rsbs_imm(_rd, _rn)                                        \
{                                                                             \
    u32 _need_cv = (check_generate_c_flag) || (check_generate_v_flag);        \
    u32 _rn_f = ((_rd) == (_rn) && _need_cv)                                 \
                ? (u32)reg_save0 : (u32)(_rn);                                \
    generate_load_imm(reg_temp3, imm);                                        \
    if ((_rd) == (_rn) && _need_cv) rv_mv(reg_save0, _rn);                   \
    rv_sub(_rd, reg_temp3, _rn);                                              \
    generate_op_sub_flags(_rd, reg_temp3, _rn_f);                             \
}

#define generate_op_adcs_imm(_rd, _rn)                                        \
{                                                                             \
    generate_load_imm(reg_a0, imm);                                           \
    generate_op_adcs_reg(_rd, _rn, reg_a0);                                   \
}                                                                             \

#define generate_op_sbcs_imm(_rd, _rn)                                        \
{                                                                             \
    generate_load_imm(reg_a0, imm);                                           \
    generate_op_sbcs_reg(_rd, _rn, reg_a0);                                   \
}                                                                             \

#define generate_op_rscs_imm(_rd, _rn)                                        \
{                                                                             \
    generate_load_imm(reg_a0, imm);                                           \
    generate_op_rscs_reg(_rd, _rn, reg_a0);                                   \
}                                                                             \

/* CMP/TST+Branch fusion: if next Thumb instruction is a fusable Bcc whose
 * flag requirements exactly match the current instruction's flag_status,
 * skip flag generation and emit a direct RISC-V branch instead.
 *
 * cmp_fuse_active values:
 *   0 = no fusion
 *   1 = CMP fusion: EQ/NE/CS/CC/HI/LS/GE/LT/GT/LE use direct rn,rm compare
 *   2 = TST fusion: EQ/NE only — use bnez/beqz on AND result register
 */
#define next_is_fusable_bcc(_required_flags)                                  \
({                                                                            \
    u32 _ok = 0;                                                              \
    if (pc + 2 < block_end_pc && ((pc & 0x7FFF) < 0x7FFE) &&                  \
        !block_data[block_data_position + 1].update_cycles)                   \
    {                                                                         \
        u16 _nx = address16(pc_address_block, ((pc + 2) & 0x7FFF));           \
        u32 _nc = (_nx >> 8) & 0x0F;                                          \
        if ((_nx >> 12) == 0xD && _nc <= 0x0D) {                              \
            u32 _nf;                                                          \
            switch (_nc) {                                                    \
                case 0x0: case 0x1: _nf = 0x04; break;                       \
                case 0x2: case 0x3: _nf = 0x02; break;                       \
                case 0x8: case 0x9: _nf = 0x06; break;                       \
                case 0xA: case 0xB: _nf = 0x09; break;                       \
                case 0xC: case 0xD: _nf = 0x0D; break;                       \
                default: _nf = 0; break;                                      \
            }                                                                 \
            if (_nf == (_required_flags) &&                                   \
                (flag_status & 0x0F) == (_required_flags)) {                  \
                _ok = 1;                                                      \
            }                                                                 \
        }                                                                     \
    }                                                                         \
    _ok;                                                                      \
})

#define cmp_try_fuse(_rn_rv, _rm_rv)                                          \
({                                                                            \
    u32 _ok = 0;                                                              \
    u32 _fs = flag_status & 0x0F;                                             \
    u32 _req = (_fs == 0x04) ? 0x04 :                                         \
               (_fs == 0x02) ? 0x02 :                                         \
               (_fs == 0x06) ? 0x06 :                                         \
               (_fs == 0x09) ? 0x09 :                                         \
               (_fs == 0x0D) ? 0x0D : 0;                                      \
    if (_req && next_is_fusable_bcc(_req)) {                                  \
        cmp_fuse_rn_rv = (_rn_rv);                                            \
        cmp_fuse_rm_rv = (_rm_rv);                                            \
        cmp_fuse_active = 1;                                                  \
        _ok = 1;                                                              \
    }                                                                         \
    _ok;                                                                      \
})

/* TST/TEQ fusion: only EQ/BNE (Z flag only) */
#define tst_try_fuse(_result_rv)                                              \
({                                                                            \
    u32 _ok = 0;                                                              \
    if ((flag_status & 0x0F) == 0x04 && next_is_fusable_bcc(0x04)) {          \
        cmp_fuse_rn_rv = (_result_rv);                                        \
        cmp_fuse_active = 2;                                                  \
        _ok = 1;                                                              \
    }                                                                         \
    _ok;                                                                      \
})

#define generate_op_cmp_reg(_rd, _rn, _rm)                                    \
    if (!cmp_try_fuse(_rn, _rm))                                              \
        generate_op_subs_reg(reg_temp2, _rn, _rm)                             \

#define generate_op_cmn_reg(_rd, _rn, _rm)                                    \
    generate_op_adds_reg(reg_temp2, _rn, _rm)                                 \

#define generate_op_tst_reg(_rd, _rn, _rm)                                    \
{                                                                             \
    rv_and(reg_temp2, _rn, _rm);                                              \
    if (!tst_try_fuse(reg_temp2))                                             \
        generate_op_logic_flags(reg_temp2)                                    \
}

#define generate_op_teq_reg(_rd, _rn, _rm)                                    \
    generate_op_eors_reg(reg_temp2, _rn, _rm)                                 \

#define generate_op_cmp_imm(_rd, _rn)                                         \
{                                                                             \
    u32 _fuse_imm_ok = 0;                                                     \
    if (imm == 0) {                                                           \
        _fuse_imm_ok = cmp_try_fuse(_rn, rv_zero);                            \
    } else {                                                                  \
        /* For nonzero imm, load into reg_temp3 before attempting fusion */    \
        u32 _temp_rm = reg_temp3;                                             \
        generate_load_imm(reg_temp3, imm);                                    \
        _fuse_imm_ok = cmp_try_fuse(_rn, _temp_rm);                           \
    }                                                                         \
    if (!_fuse_imm_ok) {                                                      \
        cmp_fuse_active = 0;  /* safety clear */                              \
        if (imm == 0) {                                                       \
            generate_op_logic_flags(_rn)                                      \
            if (check_generate_c_flag)                                        \
                rv_addi(reg_c_cache, rv_zero, 1);                             \
            if (check_generate_v_flag)                                        \
                rv_mv(reg_v_cache, rv_zero);                                  \
        } else {                                                              \
            generate_op_subs_imm(reg_temp2, _rn)                              \
        }                                                                     \
    }                                                                         \
}

#define generate_op_cmn_imm(_rd, _rn)                                         \
    generate_op_adds_imm(reg_temp2, _rn)                                      \

#define generate_op_tst_imm(_rd, _rn)                                         \
{                                                                             \
    generate_op_and_imm(reg_temp2, _rn);                                      \
    if (!tst_try_fuse(reg_temp2))                                             \
        generate_op_logic_flags(reg_temp2)                                    \
}

#define generate_op_teq_imm(_rd, _rn)                                         \
    generate_op_eors_imm(reg_temp2, _rn)                                      \

#define arm_generate_op_load_yes()                                            \
    generate_load_reg_pc(reg_a1, rn, 8)                                       \

#define arm_generate_op_load_no()                                             \

#define arm_op_check_yes()                                                    \
    check_load_reg_pc(arm_reg_a1, rn, 8)                                      \

#define arm_op_check_no()                                                     \

#define arm_generate_op_reg(name, load_op)                                    \
    arm_decode_data_proc_reg(opcode);                                         \
    generate_load_rm_sh(no_flags);                                            \
    arm_op_check_##load_op();                                                 \
    generate_op_##name##_reg(arm_to_rv_reg[rd], arm_to_rv_reg[rn],            \
                             arm_to_rv_reg[rm])                               \

#define arm_generate_op_reg_flags(name, load_op)                              \
    arm_decode_data_proc_reg(opcode);                                         \
    if (check_generate_c_flag)                                                \
    {                                                                         \
        generate_load_rm_sh(flags);                                           \
    }                                                                         \
    else                                                                      \
    {                                                                         \
        generate_load_rm_sh(no_flags);                                        \
    }                                                                         \
    arm_op_check_##load_op();                                                 \
    generate_op_##name##_reg(arm_to_rv_reg[rd], arm_to_rv_reg[rn],            \
                             arm_to_rv_reg[rm])                               \

#define arm_generate_op_imm(name, load_op)                                    \
    arm_decode_data_proc_imm(opcode);                                         \
    ror(imm, imm, imm_ror);                                                   \
    arm_op_check_##load_op();                                                 \
    generate_op_##name##_imm(arm_to_rv_reg[rd], arm_to_rv_reg[rn])            \

#define arm_generate_op_imm_flags(name, load_op)                              \
    arm_decode_data_proc_imm(opcode);                                         \
    ror(imm, imm, imm_ror);                                                   \
    if (check_generate_c_flag && (imm_ror != 0))                              \
    {                                                                         \
        generate_load_imm(reg_c_cache, (imm >> 31) & 1);                      \
    }                                                                         \
    arm_op_check_##load_op();                                                 \
    generate_op_##name##_imm(arm_to_rv_reg[rd], arm_to_rv_reg[rn])            \

#define arm_data_proc(name, type, flags_op)                                   \
{                                                                             \
    arm_generate_op_##type(name, yes);                                        \
    check_store_reg_pc_##flags_op(rd);                                        \
}                                                                             \

#define arm_data_proc_test(name, type)                                        \
{                                                                             \
    arm_generate_op_##type(name, yes);                                        \
}                                                                             \

#define arm_data_proc_unary(name, type, flags_op)                             \
{                                                                             \
    arm_generate_op_##type(name, no);                                         \
    check_store_reg_pc_##flags_op(rd);                                        \
}                                                                             \

#define arm_psr_read(op_type, psr_reg)                                        \
    generate_function_call(execute_read_##psr_reg);                           \
    generate_store_reg(reg_res, rd)                                           \

#define arm_psr_load_new_reg()                                                \
    generate_load_reg(reg_a0, rm)                                             \

#define arm_psr_load_new_imm()                                                \
    ror(imm, imm, imm_ror);                                                   \
    generate_load_imm(reg_a0, imm)                                            \

#define arm_psr_store_cpsr(op_type)                                           \
    generate_load_pc(reg_a1, (pc));                                     \
    generate_load_imm(reg_a2, cpsr_masks[psr_pfield][0]);                     \
    generate_load_imm(reg_temp3, cpsr_masks[psr_pfield][1]);                  \
    generate_function_call(execute_store_cpsr)                                \

#define arm_psr_store_spsr(op_type)                                           \
    generate_load_imm(reg_a1, spsr_masks[psr_pfield]);                        \
    generate_function_call(execute_store_spsr)                                \

#define arm_psr_store(op_type, psr_reg)                                       \
    arm_psr_load_new_##op_type();                                             \
    arm_psr_store_##psr_reg(op_type)

#define arm_psr(op_type, transfer_type, psr_reg)                              \
{                                                                             \
    arm_decode_psr_##op_type(opcode);                                         \
    arm_psr_##transfer_type(op_type, psr_reg);                                \
}                                                                             \

#define arm_multiply_flags_yes(_rd)                                           \
    generate_op_logic_flags(_rd)                                              \

#define arm_multiply_flags_no(_rd)                                            \

#define arm_multiply_add_no()                                                 \
    rv_mul(arm_to_rv_reg[rd], arm_to_rv_reg[rm], arm_to_rv_reg[rs])           \

#define arm_multiply_add_yes()                                                \
{                                                                             \
    rv_mul(reg_save0, arm_to_rv_reg[rm], arm_to_rv_reg[rs]);                  \
    rv_add(arm_to_rv_reg[rd], reg_save0, arm_to_rv_reg[rn]);                  \
}                                                                             \

#define arm_multiply(add_op, flags)                                           \
{                                                                             \
    arm_decode_multiply();                                                    \
    arm_multiply_add_##add_op();                                              \
    arm_multiply_flags_##flags(arm_to_rv_reg[rd]);                            \
}                                                                             \

#define generate_multiply_s64()                                               \
{                                                                             \
    rv_mul(reg_temp, arm_to_rv_reg[rm], arm_to_rv_reg[rs]);                   \
    rv_mulh(reg_temp2, arm_to_rv_reg[rm], arm_to_rv_reg[rs]);                 \
}                                                                             \

#define generate_multiply_u64()                                               \
{                                                                             \
    rv_mul(reg_temp, arm_to_rv_reg[rm], arm_to_rv_reg[rs]);                   \
    rv_mulhu(reg_temp2, arm_to_rv_reg[rm], arm_to_rv_reg[rs]);                \
}                                                                             \

#define generate_multiply_s64_add()                                           \
{                                                                             \
    rv_mul(reg_temp, arm_to_rv_reg[rm], arm_to_rv_reg[rs]);                   \
    rv_mulh(reg_temp2, arm_to_rv_reg[rm], arm_to_rv_reg[rs]);                 \
    rv_add(reg_temp3, arm_to_rv_reg[rdlo], reg_temp);                         \
    rv_sltu(reg_save0, reg_temp3, reg_temp);                                  \
    rv_add(reg_temp2, reg_temp2, arm_to_rv_reg[rdhi]);                        \
    rv_add(reg_temp2, reg_temp2, reg_save0);                                  \
    rv_mv(reg_temp, reg_temp3);                                               \
}                                                                             \

#define generate_multiply_u64_add()                                           \
{                                                                             \
    rv_mul(reg_temp, arm_to_rv_reg[rm], arm_to_rv_reg[rs]);                   \
    rv_mulhu(reg_temp2, arm_to_rv_reg[rm], arm_to_rv_reg[rs]);                \
    rv_add(reg_temp3, arm_to_rv_reg[rdlo], reg_temp);                         \
    rv_sltu(reg_save0, reg_temp3, reg_temp);                                  \
    rv_add(reg_temp2, reg_temp2, arm_to_rv_reg[rdhi]);                        \
    rv_add(reg_temp2, reg_temp2, reg_save0);                                  \
    rv_mv(reg_temp, reg_temp3);                                               \
}                                                                             \

#define arm_multiply_long_flags_yes(_rdlo, _rdhi)                             \
{                                                                             \
    rv_or(reg_z_cache, _rdlo, _rdhi);                                         \
    rv_seqz(reg_z_cache, reg_z_cache);                                        \
    rv_srli(reg_n_cache, _rdhi, 31);                                          \
}                                                                             \

#define arm_multiply_long_flags_no(_rdlo, _rdhi)                              \

#define arm_multiply_long_add_yes(name)                                       \
    generate_multiply_##name()                                                \

#define arm_multiply_long_add_no(name)                                        \
    generate_multiply_##name()                                                \

#define arm_multiply_long(name, add_op, flags)                                \
{                                                                             \
    arm_decode_multiply_long();                                               \
    arm_multiply_long_add_##add_op(name);                                     \
    rv_mv(arm_to_rv_reg[rdlo], reg_temp);                                     \
    rv_mv(arm_to_rv_reg[rdhi], reg_temp2);                                    \
    arm_multiply_long_flags_##flags(arm_to_rv_reg[rdlo], arm_to_rv_reg[rdhi]);\
}                                                                             \

#define thumb_load_pc_pool_const(rd, value)                                   \
    generate_load_imm(arm_to_rv_reg[rd], (value));                            \

#define arm_access_memory_load(mem_type)                                      \
{                                                                             \
    cycle_count += 2;                                                         \
    generate_function_call(rv_execute_load_##mem_type);                       \
    generate_store_reg(reg_res, rd);                                          \
    check_store_reg_pc_no_flags(rd);                                          \
}                                                                             \

#define arm_access_memory_store(mem_type)                                     \
{                                                                             \
    cycle_count++;                                                            \
    generate_load_pc(reg_a2, (pc + 4));                                 \
    generate_load_reg_pc(reg_a1, rd, 12);                                     \
    generate_function_call(rv_execute_store_##mem_type);                      \
}                                                                             \

#define arm_access_memory_reg_pre_up()                                        \
    rv_add(reg_a0, arm_to_rv_reg[rn], arm_to_rv_reg[rm])                      \

#define arm_access_memory_reg_pre_down()                                      \
    rv_sub(reg_a0, arm_to_rv_reg[rn], arm_to_rv_reg[rm])                      \

/* When rn==PC and the shifted rm is already in a0 (rm==arm_reg_a0),          \
   check_load_reg_pc would clobber a0 with the PC value, destroying the       \
   shift result. Save shift to reg_temp first to avoid the clobber. */        \
#define arm_access_memory_reg_pre_tmp_up()                                    \
    rv_add(reg_a0, reg_a0, reg_temp)                                          \

#define arm_access_memory_reg_pre_tmp_down()                                  \
    rv_sub(reg_a0, reg_a0, reg_temp)                                          \

#define arm_access_memory_reg_pre(adjust_dir)                                 \
    if (rm == arm_reg_a0 && rn == REG_PC)                                     \
    {                                                                         \
        rv_mv(reg_temp, reg_a0);                                              \
        generate_load_pc(reg_a0, (pc + 8));                                   \
        arm_access_memory_reg_pre_tmp_##adjust_dir();                         \
    }                                                                         \
    else                                                                      \
    {                                                                         \
        check_load_reg_pc(arm_reg_a0, rn, 8);                                \
        arm_access_memory_reg_pre_##adjust_dir();                             \
    }                                                                         \

#define arm_access_memory_reg_pre_wb(adjust_dir)                              \
    arm_access_memory_reg_pre(adjust_dir);                                    \
    generate_store_reg(reg_a0, rn)                                            \

#define arm_access_memory_reg_post_up()                                       \
    rv_add(arm_to_rv_reg[rn], arm_to_rv_reg[rn], arm_to_rv_reg[rm])           \

#define arm_access_memory_reg_post_down()                                     \
    rv_sub(arm_to_rv_reg[rn], arm_to_rv_reg[rn], arm_to_rv_reg[rm])           \

/* When the shifted rm is in a0 (rm==arm_reg_a0), generate_load_reg(a0, rn)  \
   clobbers a0 with rn's value, destroying the shift result. Save to         \
   reg_temp first, then use it for the post-update. */                        \
#define arm_access_memory_reg_post_tmp_up()                                   \
    rv_add(arm_to_rv_reg[rn], arm_to_rv_reg[rn], reg_temp)                    \

#define arm_access_memory_reg_post_tmp_down()                                 \
    rv_sub(arm_to_rv_reg[rn], arm_to_rv_reg[rn], reg_temp)                    \

#define arm_access_memory_reg_post(adjust_dir)                                \
    if (rm == arm_reg_a0)                                                     \
    {                                                                         \
        rv_mv(reg_temp, reg_a0);                                              \
        generate_load_reg(reg_a0, rn);                                        \
        arm_access_memory_reg_post_tmp_##adjust_dir();                        \
    }                                                                         \
    else                                                                      \
    {                                                                         \
        generate_load_reg(reg_a0, rn);                                        \
        arm_access_memory_reg_post_##adjust_dir();                            \
    }                                                                         \

#define arm_access_memory_imm_pre_up()                                        \
    generate_add_imm(reg_a0, arm_to_rv_reg[rn], offset)                       \

#define arm_access_memory_imm_pre_down()                                      \
    generate_sub_imm(reg_a0, arm_to_rv_reg[rn], offset)                       \

#define arm_access_memory_imm_pre(adjust_dir)                                 \
    check_load_reg_pc(arm_reg_a0, rn, 8);                                     \
    arm_access_memory_imm_pre_##adjust_dir()                                  \

#define arm_access_memory_imm_pre_wb(adjust_dir)                              \
    arm_access_memory_imm_pre(adjust_dir);                                    \
    generate_store_reg(reg_a0, rn)                                            \

#define arm_access_memory_imm_post_up()                                       \
    generate_add_imm(arm_to_rv_reg[rn], arm_to_rv_reg[rn], offset)            \

#define arm_access_memory_imm_post_down()                                     \
    generate_sub_imm(arm_to_rv_reg[rn], arm_to_rv_reg[rn], offset)            \

#define arm_access_memory_imm_post(adjust_dir)                                \
    generate_load_reg(reg_a0, rn);                                            \
    arm_access_memory_imm_post_##adjust_dir()                                 \

#define arm_data_trans_reg(adjust_op, adjust_dir)                             \
    arm_decode_data_trans_reg();                                              \
    generate_load_offset_sh();                                                \
    arm_access_memory_reg_##adjust_op(adjust_dir)                             \

#define arm_data_trans_imm(adjust_op, adjust_dir)                             \
    arm_decode_data_trans_imm();                                              \
    arm_access_memory_imm_##adjust_op(adjust_dir)                             \

#define arm_data_trans_half_reg(adjust_op, adjust_dir)                        \
    arm_decode_half_trans_r();                                                \
    arm_access_memory_reg_##adjust_op(adjust_dir)                             \

#define arm_data_trans_half_imm(adjust_op, adjust_dir)                        \
    arm_decode_half_trans_of();                                               \
    arm_access_memory_imm_##adjust_op(adjust_dir)                             \

#define arm_access_memory(access_type, direction, adjust_op, mem_type,        \
 offset_type)                                                                 \
{                                                                             \
    arm_data_trans_##offset_type(adjust_op, direction);                       \
    arm_access_memory_##access_type(mem_type);                                \
}                                                                             \

#define word_bit_count(word)                                                  \
    (bit_count[(word) >> 8] + bit_count[(word) & 0xFF])                       \

#define arm_block_memory_load()                                               \
    generate_function_call(rv_execute_aligned_load32);                        \
    generate_store_reg(reg_res, i)                                            \

#define arm_block_memory_store()                                              \
    generate_load_reg_pc(reg_a1, i, 8);                                       \
    generate_function_call(rv_execute_aligned_store32)                        \

#define arm_block_memory_final_load(writeback_type)                           \
    arm_block_memory_load()                                                   \

#define arm_block_memory_final_store(writeback_type)                          \
    generate_load_reg(reg_a1, i);                                             \
    arm_block_memory_writeback_post_store(writeback_type);                    \
    generate_function_call(rv_execute_store_u32)                              \

#define arm_block_memory_preload_pc_store()                                   \
    generate_load_pc(reg_a2, (pc + 4))                                        \

#define arm_block_memory_preload_pc_load()                                    \

#define arm_block_memory_adjust_pc_store()                                    \

#define arm_block_memory_adjust_pc_load()                                     \
    if (reg_list & 0x8000)                                                    \
    {                                                                         \
        generate_indirect_branch_arm();                                       \
    }                                                                         \

#define arm_block_memory_offset_down_a()                                      \
    generate_sub_imm(reg_save0, base_reg, ((word_bit_count(reg_list) - 1) * 4)); \
    rv_andi(reg_save0, reg_save0, -4)                                          \

#define arm_block_memory_offset_down_b()                                      \
    generate_sub_imm(reg_save0, base_reg, (word_bit_count(reg_list) * 4));     \
    rv_andi(reg_save0, reg_save0, -4)                                          \

#define arm_block_memory_offset_no()                                          \
    rv_andi(reg_save0, base_reg, -4)                                  \

#define arm_block_memory_offset_up()                                          \
    generate_add_imm(reg_save0, base_reg, 4);                                  \
    rv_andi(reg_save0, reg_save0, -4)                                          \

#define arm_block_memory_writeback_down()                                     \
    generate_sub_imm(base_reg, base_reg, (word_bit_count(reg_list) * 4))      \

#define arm_block_memory_writeback_up()                                       \
    generate_add_imm(base_reg, base_reg, (word_bit_count(reg_list) * 4))      \

#define arm_block_memory_writeback_no()                                       \

#define arm_block_memory_writeback_pre_load(writeback_type)                   \
    if (!((reg_list >> rn) & 0x01))                                           \
    {                                                                         \
        arm_block_memory_writeback_##writeback_type();                        \
    }                                                                         \

#define arm_block_memory_writeback_pre_store(writeback_type)                  \

#define arm_block_memory_writeback_post_store(writeback_type)                 \
    arm_block_memory_writeback_##writeback_type()                             \

#define arm_block_memory(access_type, offset_type, writeback_type, s_bit)     \
{                                                                             \
    arm_decode_block_trans();                                                 \
    u32 i;                                                                    \
    u32 offset = 0;                                                           \
    u32 base_reg = arm_to_rv_reg[rn];                                         \
                                                                              \
    arm_block_memory_offset_##offset_type();                                  \
    arm_block_memory_writeback_pre_##access_type(writeback_type);             \
    arm_block_memory_preload_pc_##access_type();                              \
                                                                              \
    for (i = 0; i < 16; i++)                                                  \
    {                                                                         \
        if ((reg_list >> i) & 0x01)                                           \
        {                                                                     \
            cycle_count++;                                                    \
            generate_add_imm(reg_a0, reg_save0, offset);                     \
            if (reg_list & ~((2 << i) - 1))                                   \
            {                                                                 \
                arm_block_memory_##access_type();                             \
                offset += 4;                                                  \
            }                                                                 \
            else                                                              \
            {                                                                 \
                arm_block_memory_final_##access_type(writeback_type);         \
                break;                                                        \
            }                                                                 \
        }                                                                     \
    }                                                                         \
                                                                              \
    arm_block_memory_adjust_pc_##access_type();                               \
}                                                                             \

#define arm_swap(type)                                                        \
{                                                                             \
    arm_decode_swap();                                                        \
    cycle_count += 3;                                                         \
    generate_load_reg(reg_a0, rn);                                            \
    generate_function_call(rv_execute_load_##type);                           \
    generate_load_reg(reg_a1, rm);                                            \
    generate_store_reg(reg_res, rd);                                          \
    generate_load_reg(reg_a0, rn);                                            \
    generate_function_call(rv_execute_store_##type);                          \
}                                                                             \

#define arm_b()                                                               \
{                                                                             \
    generate_branch();                                                        \
}                                                                             \

#define arm_bl()                                                              \
{                                                                             \
    generate_load_pc(reg_r14, (pc + 4));                                \
    generate_branch();                                                        \
}                                                                             \

#define arm_bx()                                                              \
    arm_decode_branchx(opcode);                                               \
    generate_load_reg_pc(reg_a0, rn, 8);                                      \
    generate_indirect_branch_dual()                                           \

#define arm_swi()                                                             \
    generate_load_pc(reg_a0, (pc + 4));                                 \
    generate_function_call(execute_swi);                                      \
    generate_branch()                                                         \

#define thumb_generate_op_load_yes(_rs)                                       \
    generate_load_reg(reg_a1, _rs)                                            \

#define thumb_generate_op_load_no(_rs)                                        \

#define thumb_generate_op_reg(name, _rd, _rs, _rn)                            \
    generate_op_##name##_reg(arm_to_rv_reg[_rd],                              \
                             arm_to_rv_reg[_rs], arm_to_rv_reg[_rn])          \

#define thumb_generate_op_imm(name, _rd, _rs, _rn)                            \
    generate_op_##name##_imm(arm_to_rv_reg[_rd], arm_to_rv_reg[_rs])          \

#define thumb_data_proc(type, name, rn_type, _rd, _rs, _rn)                   \
{                                                                             \
    thumb_decode_##type();                                                    \
    thumb_generate_op_##rn_type(name, _rd, _rs, _rn);                         \
}                                                                             \

#define thumb_data_proc_test(type, name, rn_type, _rs, _rn)                   \
{                                                                             \
    thumb_decode_##type();                                                    \
    thumb_generate_op_##rn_type(name, 0, _rs, _rn);                           \
}                                                                             \

#define thumb_data_proc_unary(type, name, rn_type, _rd, _rn)                  \
{                                                                             \
    thumb_decode_##type();                                                    \
    thumb_generate_op_##rn_type(name, _rd, 0, _rn);                           \
}                                                                             \

#define check_store_reg_pc_thumb(_rd)                                         \
    if ((_rd) == REG_PC)                                                      \
    {                                                                         \
        generate_indirect_branch_cycle_update(thumb);                         \
    }                                                                         \

#define thumb_data_proc_hi(name)                                              \
{                                                                             \
    thumb_decode_hireg_op();                                                  \
    u32 dest_rd = rd;                                                         \
    check_load_reg_pc(arm_reg_a0, rs, 4);                                     \
    check_load_reg_pc(arm_reg_a1, rd, 4);                                     \
    generate_op_##name##_reg(arm_to_rv_reg[dest_rd], arm_to_rv_reg[rd],       \
                             arm_to_rv_reg[rs]);                              \
    check_store_reg_pc_thumb(dest_rd);                                        \
}                                                                             \

#define thumb_data_proc_test_hi(name)                                         \
{                                                                             \
    thumb_decode_hireg_op();                                                  \
    check_load_reg_pc(arm_reg_a0, rs, 4);                                     \
    check_load_reg_pc(arm_reg_a1, rd, 4);                                     \
    generate_op_##name##_reg(reg_temp2, arm_to_rv_reg[rd],                    \
                             arm_to_rv_reg[rs]);                              \
}                                                                             \

#define thumb_data_proc_mov_hi()                                              \
{                                                                             \
    thumb_decode_hireg_op();                                                  \
    check_load_reg_pc(arm_reg_a0, rs, 4);                                     \
    generate_mov(rd, rs);                                                     \
    check_store_reg_pc_thumb(rd);                                             \
}                                                                             \

#define thumb_b()                                                             \
    generate_branch_cycle_update(                                             \
        block_exits[block_exit_position].branch_source,                       \
        block_exits[block_exit_position].branch_target);                      \
    block_exit_position++                                                     \

#define thumb_bl()                                                            \
    generate_load_pc(reg_r14, ((pc + 2) | 0x01));                             \
    generate_branch_cycle_update(                                             \
        block_exits[block_exit_position].branch_source,                       \
        block_exits[block_exit_position].branch_target);                      \
    block_exit_position++                                                     \

#define thumb_blh()                                                           \
{                                                                             \
    thumb_decode_branch();                                                    \
    generate_add_imm(reg_a0, reg_r14, (offset * 2));                          \
    generate_load_pc(reg_r14, ((pc + 2) | 0x01));                             \
    generate_indirect_branch_cycle_update(thumb);                             \
    break;                                                                    \
}                                                                             \

#define thumb_bx()                                                            \
{                                                                             \
    thumb_decode_hireg_op();                                                  \
    generate_load_reg_pc(reg_a0, rs, 4);                                      \
    generate_indirect_branch_cycle_update(dual);                              \
}                                                                             \

#define thumb_process_cheats()                                                \
    generate_function_call(rv_cheat_hook)                                     \

#define arm_process_cheats()                                                  \
    generate_function_call(rv_cheat_hook)                                     \

/* Instruction tracing - currently disabled */
#define emit_trace_instruction(pc, mode) do {} while(0)
#ifdef TRACE_INSTRUCTIONS
#define emit_trace_thumb_instruction(_pc)                                       \
  do {                                                                        \
    extern void rv_trace_instruction(void);                                   \
    generate_load_imm(reg_pc, (_pc));                                         \
    generate_function_call(rv_trace_instruction);                             \
    generate_load_imm(reg_pc, stored_pc);                                     \
  } while(0)
#define emit_trace_arm_instruction(_pc)                                        \
  do {                                                                        \
    extern void rv_trace_instruction(void);                                   \
    generate_load_imm(reg_pc, (_pc));                                         \
    generate_function_call(rv_trace_instruction);                             \
    generate_load_imm(reg_pc, stored_pc);                                     \
  } while(0)
#else
#define emit_trace_thumb_instruction(pc) do {} while(0)
#define emit_trace_arm_instruction(pc)   do {} while(0)
#endif

#define thumb_swi()                                                           \
    generate_load_pc(reg_a0, (pc + 2));                                       \
    generate_function_call(execute_swi);                                      \
    generate_branch_cycle_update(                                             \
        block_exits[block_exit_position].branch_source,                       \
        block_exits[block_exit_position].branch_target);                      \
    block_exit_position++                                                     \

#define arm_hle_div(cpu_mode)                                                 \
    rv_div(reg_r3, reg_r0, reg_r1);                                           \
    rv_rem(reg_r1, reg_r0, reg_r1);                                           \
    rv_mv(reg_r0, reg_r3);                                                    \
    rv_srai(reg_a0, reg_r0, 31);                                              \
    rv_xor(reg_r3, reg_r0, reg_a0);                                           \
    rv_sub(reg_r3, reg_r3, reg_a0)                                            \

#define arm_hle_div_arm(cpu_mode)                                             \
    rv_div(reg_r3, reg_r1, reg_r0);                                           \
    rv_rem(reg_r1, reg_r1, reg_r0);                                           \
    rv_mv(reg_r0, reg_r3);                                                    \
    rv_srai(reg_a0, reg_r0, 31);                                              \
    rv_xor(reg_r3, reg_r0, reg_a0);                                           \
    rv_sub(reg_r3, reg_r3, reg_a0)                                            \

#define thumb_load_pc(_rd)                                                    \
{                                                                             \
    thumb_decode_imm();                                                       \
    generate_load_pc(arm_to_rv_reg[_rd], (((pc & ~2) + 4) + (imm * 4)));      \
}                                                                             \

#define thumb_load_sp(_rd)                                                    \
{                                                                             \
    thumb_decode_imm();                                                       \
    generate_add_imm(arm_to_rv_reg[_rd], reg_r13, (imm * 4));                 \
}                                                                             \

#define thumb_adjust_sp_up()                                                  \
    generate_add_imm(reg_r13, reg_r13, (imm * 4))                             \

#define thumb_adjust_sp_down()                                                \
    generate_sub_imm(reg_r13, reg_r13, (imm * 4))                             \

#define thumb_adjust_sp(direction)                                            \
{                                                                             \
    thumb_decode_add_sp();                                                    \
    thumb_adjust_sp_##direction();                                            \
}                                                                             \

#define thumb_generate_shift_imm(name)                                        \
    generate_shift_imm_##name##_flags(rd, rs, imm);                           \
    if (rs != rd)                                                             \
    {                                                                         \
        generate_mov(rd, rs);                                                 \
    }                                                                         \

#define thumb_generate_shift_reg(name)                                        \
{                                                                             \
    u32 original_rd = rd;                                                     \
    generate_shift_reg_##name##_flags(rd, rs);                                \
    rv_mv(arm_to_rv_reg[original_rd], reg_a0);                                \
}                                                                             \

#define thumb_shift(decode_type, op_type, value_type)                         \
{                                                                             \
    thumb_decode_##decode_type();                                             \
    thumb_generate_shift_##value_type(op_type);                               \
    generate_op_logic_flags(arm_to_rv_reg[rd]);                               \
}                                                                             \

#define thumb_access_memory_load(mem_type, reg_rd)                            \
{                                                                             \
    cycle_count += 2;                                                         \
    generate_function_call(rv_execute_load_##mem_type);                       \
    generate_store_reg(reg_res, reg_rd);                                      \
}                                                                             \

#define thumb_access_memory_store(mem_type, reg_rd)                           \
{                                                                             \
    cycle_count++;                                                            \
    generate_load_reg(reg_a1, reg_rd);                                        \
    generate_load_pc(reg_a2, (pc + 2));                                 \
    generate_function_call(rv_execute_store_##mem_type);                      \
}                                                                             \

#define thumb_access_memory_generate_address_pc_relative(offset, reg_rb, reg_ro) \
    generate_load_pc(reg_a0, (offset))                                        \

#define thumb_access_memory_generate_address_reg_imm(offset, reg_rb, reg_ro)  \
    generate_add_imm(reg_a0, arm_to_rv_reg[reg_rb], (offset))                 \

#define thumb_access_memory_generate_address_reg_imm_sp(offset, reg_rb, reg_ro) \
    generate_add_imm(reg_a0, arm_to_rv_reg[reg_rb], ((offset) * 4))           \

#define thumb_access_memory_generate_address_reg_reg(offset, reg_rb, reg_ro)  \
    rv_add(reg_a0, arm_to_rv_reg[reg_rb], arm_to_rv_reg[reg_ro])              \

#define thumb_access_memory(access_type, op_type, reg_rd, reg_rb, reg_ro,     \
 address_type, offset, mem_type)                                              \
{                                                                             \
    thumb_decode_##op_type();                                                 \
    thumb_access_memory_generate_address_##address_type(offset, reg_rb,       \
                                                         reg_ro);             \
    thumb_access_memory_##access_type(mem_type, reg_rd);                      \
}                                                                             \

#define thumb_block_address_preadjust_no(base_reg)                            \
    rv_andi(reg_save0, base_reg, -4)                                  \

#define thumb_block_address_preadjust_down(base_reg)                          \
    generate_sub_imm(base_reg, base_reg, (bit_count[reg_list] * 4));          \
    rv_andi(reg_save0, base_reg, -4)                                  \

#define thumb_block_address_preadjust_push_lr(base_reg)                       \
    generate_sub_imm(base_reg, base_reg, ((bit_count[reg_list] + 1) * 4));    \
    rv_andi(reg_save0, base_reg, -4)                                  \

#define thumb_block_address_postadjust_no(base_reg)                           \

#define thumb_block_address_postadjust_up(base_reg)                           \
    generate_add_imm(base_reg, reg_save0, (bit_count[reg_list] * 4))          \

#define thumb_block_address_postadjust_pop_pc(base_reg)                       \
    generate_add_imm(base_reg, reg_save0, ((bit_count[reg_list] + 1) * 4))    \

#define thumb_block_address_postadjust_push_lr(base_reg)                      \

#define thumb_block_memory_load()                                             \
    generate_function_call(rv_execute_aligned_load32);                        \
    generate_store_reg(reg_res, i)                                            \

#define thumb_block_memory_store()                                            \
    generate_load_reg(reg_a1, i);                                             \
    generate_function_call(rv_execute_aligned_store32);                       \

#define thumb_block_memory_final_load()                                       \
    thumb_block_memory_load()                                                 \

#define thumb_block_memory_final_store()                                      \
    generate_load_reg(reg_a1, i);                                             \
    generate_function_call(rv_execute_store_u32);                             \

#define thumb_block_memory_final_no(access_type)                              \
    thumb_block_memory_final_##access_type()                                  \

#define thumb_block_memory_final_up(access_type)                              \
    thumb_block_memory_final_##access_type()                                  \

#define thumb_block_memory_final_down(access_type)                            \
    thumb_block_memory_final_##access_type()                                  \

#define thumb_block_memory_final_push_lr(access_type)                         \
    thumb_block_memory_##access_type()                                        \

#define thumb_block_memory_final_pop_pc(access_type)                          \
    thumb_block_memory_##access_type()                                        \

#define thumb_block_memory_preload_pc_store()                                 \
    generate_load_pc(reg_a2, (pc + 2))                                        \

#define thumb_block_memory_preload_pc_load()                                  \

#define thumb_block_memory_extra_no()                                         \

#define thumb_block_memory_extra_up()                                         \

#define thumb_block_memory_extra_down()                                       \

#define thumb_block_memory_extra_push_lr()                                    \
    generate_add_imm(reg_a0, reg_save0, (bit_count[reg_list] * 4));           \
    generate_load_reg(reg_a1, REG_LR);                                        \
    generate_function_call(rv_execute_aligned_store32);                       \

#define thumb_block_memory_extra_pop_pc()                                     \
    generate_add_imm(reg_a0, reg_save0, (bit_count[reg_list] * 4));           \
    generate_function_call(rv_execute_aligned_load32);                        \
    generate_indirect_branch_cycle_update(thumb)                              \

#define thumb_block_memory(access_type, pre_op, post_op, arm_base_reg)        \
{                                                                             \
    thumb_decode_rlist();                                                     \
    u32 i;                                                                    \
    u32 offset = 0;                                                           \
    u32 base_reg = arm_to_rv_reg[arm_base_reg];                               \
                                                                              \
    thumb_block_address_preadjust_##pre_op(base_reg);                         \
    thumb_block_memory_preload_pc_##access_type();                            \
                                                                              \
    thumb_block_address_postadjust_##post_op(base_reg);                       \
                                                                              \
    for (i = 0; i < 8; i++)                                                   \
    {                                                                         \
        if ((reg_list >> i) & 0x01)                                           \
        {                                                                     \
            cycle_count++;                                                    \
            generate_add_imm(reg_a0, reg_save0, offset);                     \
            if (reg_list & ~((2 << i) - 1))                                   \
            {                                                                 \
                thumb_block_memory_##access_type();                           \
                offset += 4;                                                  \
            }                                                                 \
            else                                                              \
            {                                                                 \
                thumb_block_memory_final_##post_op(access_type);              \
                break;                                                        \
            }                                                                 \
        }                                                                     \
    }                                                                         \
                                                                              \
    thumb_block_memory_extra_##post_op();                                     \
}                                                                             \

#define arm_conditional_block_header()                                        \
    generate_cycle_update();                                                  \
    generate_condition();                                                     \

/* Fused condition macros: emit direct RISC-V comparison branch using
 * saved CMP operands.  The branch sense is INVERTED (skip logic). */
#define generate_fused_condition_eq()                                         \
    (backpatch_address) = translation_ptr;                                    \
    rv_bne(cmp_fuse_rn_rv, cmp_fuse_rm_rv, 0)                                \

#define generate_fused_condition_ne()                                         \
    (backpatch_address) = translation_ptr;                                    \
    rv_beq(cmp_fuse_rn_rv, cmp_fuse_rm_rv, 0)                                \

#define generate_fused_condition_cs()                                         \
    (backpatch_address) = translation_ptr;                                    \
    rv_bltu(cmp_fuse_rn_rv, cmp_fuse_rm_rv, 0)                               \

#define generate_fused_condition_cc()                                         \
    (backpatch_address) = translation_ptr;                                    \
    rv_bgeu(cmp_fuse_rn_rv, cmp_fuse_rm_rv, 0)                               \

#define generate_fused_condition_ge()                                         \
    (backpatch_address) = translation_ptr;                                    \
    rv_blt(cmp_fuse_rn_rv, cmp_fuse_rm_rv, 0)                                \

#define generate_fused_condition_lt()                                         \
    (backpatch_address) = translation_ptr;                                    \
    rv_bge(cmp_fuse_rn_rv, cmp_fuse_rm_rv, 0)                                \

/* Non-fusable conditions: fallback to normal flag-based branch */
#define generate_fused_condition_mi()    generate_condition_mi()
#define generate_fused_condition_pl()    generate_condition_pl()
#define generate_fused_condition_vs()    generate_condition_vs()
#define generate_fused_condition_vc()    generate_condition_vc()
/* HI = unsigned rn > rm → skip if NOT HI = LS = rm >= rn unsigned */
#define generate_fused_condition_hi()                                         \
    (backpatch_address) = translation_ptr;                                    \
    rv_bgeu(cmp_fuse_rm_rv, cmp_fuse_rn_rv, 0)                               \

/* LS = unsigned rn <= rm → skip if NOT LS = HI = rm < rn unsigned */
#define generate_fused_condition_ls()                                         \
    (backpatch_address) = translation_ptr;                                    \
    rv_bltu(cmp_fuse_rm_rv, cmp_fuse_rn_rv, 0)                               \

/* GT = signed rn > rm → skip if NOT GT = LE = rm >= rn signed */
#define generate_fused_condition_gt()                                         \
    (backpatch_address) = translation_ptr;                                    \
    rv_bge(cmp_fuse_rm_rv, cmp_fuse_rn_rv, 0)                                \

/* LE = signed rn <= rm → skip if NOT LE = GT = rm < rn signed */
#define generate_fused_condition_le()                                         \
    (backpatch_address) = translation_ptr;                                    \
    rv_blt(cmp_fuse_rm_rv, cmp_fuse_rn_rv, 0)

/* TST-fused conditions: branch on AND result register directly.
 * Only EQ/NE are valid for TST fusion. */
#define generate_tst_fused_condition_eq()                                     \
    (backpatch_address) = translation_ptr;                                    \
    rv_bnez(cmp_fuse_rn_rv, 0)                                                \

#define generate_tst_fused_condition_ne()                                     \
    (backpatch_address) = translation_ptr;                                    \
    rv_beqz(cmp_fuse_rn_rv, 0)                                                \

/* Non-fusable via TST: fallback */
#define generate_tst_fused_condition_cs()    generate_condition_cs()
#define generate_tst_fused_condition_cc()    generate_condition_cc()
#define generate_tst_fused_condition_mi()    generate_condition_mi()
#define generate_tst_fused_condition_pl()    generate_condition_pl()
#define generate_tst_fused_condition_vs()    generate_condition_vs()
#define generate_tst_fused_condition_vc()    generate_condition_vc()
#define generate_tst_fused_condition_hi()    generate_condition_hi()
#define generate_tst_fused_condition_ls()    generate_condition_ls()
#define generate_tst_fused_condition_ge()    generate_condition_ge()
#define generate_tst_fused_condition_lt()    generate_condition_lt()
#define generate_tst_fused_condition_gt()    generate_condition_gt()
#define generate_tst_fused_condition_le()    generate_condition_le()

#define thumb_conditional_branch(condition)                                   \
{                                                                             \
    generate_cycle_update();                                                  \
    if (cmp_fuse_active == 1) {                                               \
        cmp_fuse_active = 0;                                                  \
        generate_fused_condition_##condition();                               \
    } else if (cmp_fuse_active == 2) {                                        \
        cmp_fuse_active = 0;                                                  \
        generate_tst_fused_condition_##condition();                           \
    } else {                                                                  \
        generate_condition_##condition();                                     \
    }                                                                         \
    generate_branch_no_cycle_update(                                          \
        block_exits[block_exit_position].branch_source,                       \
        block_exits[block_exit_position].branch_target);                      \
    generate_branch_patch_conditional(backpatch_address, translation_ptr);    \
    block_exit_position++;                                                    \
}                                                                             \

#define generate_block_extra_vars()                                           \
    u32 stored_pc = pc;                                                         \
    u32 cmp_fuse_active = 0;                                                    \
    u32 cmp_fuse_rn_rv = 0;                                                     \
    u32 cmp_fuse_rm_rv = 0;                                                     \

#define generate_block_extra_vars_arm()                                       \
    generate_block_extra_vars();                                                \

#define generate_block_extra_vars_thumb()                                     \
    generate_block_extra_vars();                                                \

#define generate_translation_gate(type)                                       \
    generate_load_pc(reg_a0, pc);                                       \
    generate_indirect_branch_no_cycle_update(type)                            \

#define generate_branch()                                                     \
{                                                                             \
    if (condition == 0x0E)                                                    \
    {                                                                         \
        generate_branch_cycle_update(                                         \
            block_exits[block_exit_position].branch_source,                   \
            block_exits[block_exit_position].branch_target);                  \
    }                                                                         \
    else                                                                      \
    {                                                                         \
        generate_branch_no_cycle_update(                                      \
            block_exits[block_exit_position].branch_source,                   \
            block_exits[block_exit_position].branch_target);                  \
    }                                                                         \
    block_exit_position++;                                                    \
}

/* Generate the opposite condition to skip the block. */
#define generate_condition_eq()                                               \
    (backpatch_address) = translation_ptr;                                    \
    rv_beqz(reg_z_cache, 0)                                                   \

#define generate_condition_ne()                                               \
    (backpatch_address) = translation_ptr;                                    \
    rv_bnez(reg_z_cache, 0)                                                   \

#define generate_condition_cs()                                               \
    (backpatch_address) = translation_ptr;                                    \
    rv_beqz(reg_c_cache, 0)                                                   \

#define generate_condition_cc()                                               \
    (backpatch_address) = translation_ptr;                                    \
    rv_bnez(reg_c_cache, 0)                                                   \

#define generate_condition_mi()                                               \
    (backpatch_address) = translation_ptr;                                    \
    rv_beqz(reg_n_cache, 0)                                                   \

#define generate_condition_pl()                                               \
    (backpatch_address) = translation_ptr;                                    \
    rv_bnez(reg_n_cache, 0)                                                   \

#define generate_condition_vs()                                               \
    (backpatch_address) = translation_ptr;                                    \
    rv_beqz(reg_v_cache, 0)                                                   \

#define generate_condition_vc()                                               \
    (backpatch_address) = translation_ptr;                                    \
    rv_bnez(reg_v_cache, 0)                                                   \

#define generate_condition_hi()                                               \
    (backpatch_address) = translation_ptr;                                    \
    rv_bgeu(reg_z_cache, reg_c_cache, 0)                                      \

#define generate_condition_ls()                                               \
    (backpatch_address) = translation_ptr;                                    \
    rv_bltu(reg_z_cache, reg_c_cache, 0)                                      \

#define generate_condition_ge()                                               \
    (backpatch_address) = translation_ptr;                                    \
    rv_bne(reg_n_cache, reg_v_cache, 0)                                       \

#define generate_condition_lt()                                               \
    (backpatch_address) = translation_ptr;                                    \
    rv_beq(reg_n_cache, reg_v_cache, 0)                                       \

#define generate_condition_gt()                                               \
    rv_xor(reg_temp, reg_n_cache, reg_v_cache);                               \
    rv_or(reg_temp, reg_temp, reg_z_cache);                                   \
    (backpatch_address) = translation_ptr;                                    \
    rv_bnez(reg_temp, 0)                                                      \

#define generate_condition_le()                                               \
    rv_xor(reg_temp, reg_n_cache, reg_v_cache);                               \
    rv_or(reg_temp, reg_temp, reg_z_cache);                                   \
    (backpatch_address) = translation_ptr;                                    \
    rv_beqz(reg_temp, 0)                                                      \

#define generate_condition()                                                  \
    switch (condition)                                                        \
    {                                                                         \
        case 0x0: generate_condition_eq(); break;                             \
        case 0x1: generate_condition_ne(); break;                             \
        case 0x2: generate_condition_cs(); break;                             \
        case 0x3: generate_condition_cc(); break;                             \
        case 0x4: generate_condition_mi(); break;                             \
        case 0x5: generate_condition_pl(); break;                             \
        case 0x6: generate_condition_vs(); break;                             \
        case 0x7: generate_condition_vc(); break;                             \
        case 0x8: generate_condition_hi(); break;                             \
        case 0x9: generate_condition_ls(); break;                             \
        case 0xA: generate_condition_ge(); break;                             \
        case 0xB: generate_condition_lt(); break;                             \
        case 0xC: generate_condition_gt(); break;                             \
        case 0xD: generate_condition_le(); break;                             \
        case 0xE: break;                                                      \
        case 0xF:                                                             \
            /* NV condition (ARMv4: never execute) — emit unconditional skip */\
            (backpatch_address) = translation_ptr;                            \
            rv_beqz(reg_zero, 0);                                             \
            break;                                                           \
    }

#define generate_op_logic_flags(_reg)                                         \
    if (check_generate_n_flag)                                                \
    {                                                                         \
        rv_srli(reg_n_cache, _reg, 31);                                       \
    }                                                                         \
    if (check_generate_z_flag)                                                \
    {                                                                         \
        rv_seqz(reg_z_cache, _reg);                                           \
    }

#define generate_op_add_flags(_rd, _rn, _rm)                                 \
    if (check_generate_c_flag)                                                \
    {                                                                         \
        rv_sltu(reg_c_cache, _rd, _rn);                                       \
    }                                                                         \
    generate_op_logic_flags(_rd)                                              \
    if (check_generate_v_flag)                                                \
    {                                                                         \
        rv_xor(reg_temp, _rn, _rd);                                           \
        rv_xor(reg_temp2, _rm, _rd);                                          \
        rv_and(reg_v_cache, reg_temp, reg_temp2);                             \
        rv_srli(reg_v_cache, reg_v_cache, 31);                                \
    }

#define generate_op_sub_flags(_rd, _rn, _rm)                                 \
    if (check_generate_c_flag)                                                \
    {                                                                         \
        rv_sltu(reg_c_cache, _rn, _rm);                                       \
        rv_xori(reg_c_cache, reg_c_cache, 1);                                 \
    }                                                                         \
    generate_op_logic_flags(_rd)                                              \
    if (check_generate_v_flag)                                                \
    {                                                                         \
        rv_xor(reg_temp, _rn, _rm);                                           \
        rv_xor(reg_temp2, _rn, _rd);                                          \
        rv_and(reg_v_cache, reg_temp, reg_temp2);                             \
        rv_srli(reg_v_cache, reg_v_cache, 31);                                \
    }

#define generate_op_arith_flags()                                             \
    /* RISC-V computes arithmetic flags directly in per-op helpers */

#define load_c_flag()                                                         \
    rv_mv(reg_temp, reg_c_cache)

/* ---- I-cache synchronization ---- */
#define emit_icache_sync()                                                    \
    asm volatile ("fence.i" ::: "memory")

#include "riscv_selftest.h"

void init_emitter(bool must_swap)
{
    (void)must_swap;

#ifdef JIT_TRAMPOLINE_PSRAM
    /* Copy trampoline code from flash to PSRAM buffer, then fix up all
       AUIPC instructions so PC-relative offsets are correct at the new
       address.  This puts the trampolines within JAL range of JIT code. */
    {
        u32 tramp_size = (u32)(_jit_trampoline_end - _jit_trampoline_start);
        if (tramp_size > 8192)
        {
            printf("FATAL: trampoline size %u exceeds psram buf (8192)\n", tramp_size);
            abort();
        }
        u32 old_base = (u32)(uintptr_t)_jit_trampoline_start;
        u32 new_base = (u32)(uintptr_t)trampoline_psram_buf;
        memcpy(trampoline_psram_buf, _jit_trampoline_start, tramp_size);
        fixup_auipc_relocations(trampoline_psram_buf, old_base, new_base, tramp_size);
        trampoline_reloc_delta = (s32)(new_base - old_base);
        /* Flush D-cache → PSRAM, invalidate I-cache, then fence.i.
           Plain fence.i is insufficient on ESP32-P4's split L1 caches. */
        platform_cache_sync(trampoline_psram_buf,
                            trampoline_psram_buf + tramp_size);
    }
#else
    trampoline_reloc_delta = 0;
#endif

    // jit_selftest();
    init_bios_hooks();
}

u32 execute_arm_translate_internal(u32 cycles, void *regptr);

u32 execute_arm_translate(u32 cycles) {
    return execute_arm_translate_internal(cycles, &reg[0]);
}

#endif /* RISCV_EMIT_H */
