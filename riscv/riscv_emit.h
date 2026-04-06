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

void function_cc execute_store_u8(u32 address, u32 source)
{
    write_memory8(address, source);
}

void function_cc execute_store_u16(u32 address, u32 source)
{
    write_memory16(address, source);
}

void function_cc execute_store_u32(u32 address, u32 source)
{
    write_memory32(address, source);
}

void function_cc execute_store_aligned_u32(u32 address, u32 source)
{
    write_memory32(address, source);
}

void execute_aligned_store32(u32 addr, u32 data)
{
    execute_store_aligned_u32(addr, data);
}

u32 execute_aligned_load32(u32 addr)
{
    return read_memory32(addr);
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
        reg[REG_CPSR] = 0xD2;
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
            reg[REG_CPSR] = 0xD2;
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

u32 arm_to_rv_reg[] =
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
    rv_mv(ireg, arm_to_rv_reg[reg_index])                                       \

#define generate_load_imm(ireg, imm)                                          \
    rv_load_imm32(ireg, imm)                                                    \

#define generate_load_pc_2inst(ireg, new_pc)                                  \
    rv_load_imm32(ireg, new_pc)                                                 \

#define generate_load_pc(ireg, new_pc)                                        \
{                                                                             \
    generate_load_imm(ireg, (new_pc));                                          \
}

#define generate_store_reg(ireg, reg_index)                                   \
    rv_mv(arm_to_rv_reg[reg_index], ireg)                                       \

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
    rv_mv(arm_to_rv_reg[ireg_dest], arm_to_rv_reg[ireg_src])                    \

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

#define generate_function_call(function_location)                             \
{                                                                             \
    rv_load_imm32(reg_temp, (u32)(uintptr_t)(function_location));               \
    rv_jalr(rv_ra, reg_temp, 0);                                                \
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

#define emit_branch_filler(writeback_location)                                \
    (writeback_location) = translation_ptr;                                   \
    rv_j(0);                                                                  \

#define generate_branch_patch_unconditional(dest, target)                     \
    rv_patch_jal((u32 *)(dest), (target))                                     \

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
        rv_bge(reg_cycles, reg_zero, 24);                                     \
        generate_load_pc_2inst(reg_a0, new_pc);                               \
        generate_function_call(rv_update_gba);                                \
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
    generate_indirect_branch_no_cycle_update(arm)                              \

#define generate_indirect_branch_dual()                                        \
    generate_indirect_branch_no_cycle_update(dual)                             \

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
    rv_bgeu(reg_a1, reg_temp2, 12);                                            \
    rv_sll(reg_a0, reg_a0, reg_a1);                                            \
    rv_j(8);                                                                   \
    rv_mv(reg_a0, reg_zero);                                                   \
}                                                                             \

#define generate_shift_reg_lsr_no_flags(_rm, _rs)                             \
{                                                                             \
    generate_load_reg_pc(reg_a1, _rs, 8);                                      \
    rv_andi(reg_a1, reg_a1, 0xFF);                                             \
    generate_load_reg_pc(reg_a0, _rm, 12);                                     \
    generate_load_imm(reg_temp2, 32);                                          \
    rv_bgeu(reg_a1, reg_temp2, 12);                                            \
    rv_srl(reg_a0, reg_a0, reg_a1);                                            \
    rv_j(8);                                                                   \
    rv_mv(reg_a0, reg_zero);                                                   \
}                                                                             \

#define generate_shift_reg_asr_no_flags(_rm, _rs)                             \
{                                                                             \
    generate_load_reg_pc(reg_a1, _rs, 8);                                      \
    rv_andi(reg_a1, reg_a1, 0xFF);                                             \
    generate_load_reg_pc(reg_a0, _rm, 12);                                     \
    generate_load_imm(reg_temp2, 32);                                          \
    rv_bltu(reg_a1, reg_temp2, 12);                                            \
    rv_srai(reg_a0, reg_a0, 31);                                               \
    rv_j(8);                                                                   \
    rv_sra(reg_a0, reg_a0, reg_a1);                                            \
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
    rv_beqz(reg_a1, 32);                                                       \
    generate_load_imm(reg_temp2, 32);                                          \
    rv_bgeu(reg_a1, reg_temp2, 24);                                            \
    rv_addi(reg_temp, reg_a1, -1);                                             \
    rv_sll(reg_c_cache, reg_a0, reg_temp);                                     \
    rv_srli(reg_c_cache, reg_c_cache, 31);                                     \
    rv_sll(reg_a0, reg_a0, reg_a1);                                            \
    rv_j(20);                                                                  \
    /* amt >= 32: C = bit0(Rm) if amt==32, else 0 */                           \
    rv_andi(reg_c_cache, reg_a0, 1);                                           \
    rv_beq(reg_a1, reg_temp2, 8);                                              \
    rv_mv(reg_c_cache, reg_zero);                                              \
    rv_mv(reg_a0, reg_zero);                                                   \
}                                                                             \

#define generate_shift_reg_lsr_flags(_rm, _rs)                                \
{                                                                             \
    generate_load_reg_pc(reg_a1, _rs, 8);                                      \
    rv_andi(reg_a1, reg_a1, 0xFF);                                             \
    generate_load_reg_pc(reg_a0, _rm, 12);                                     \
    rv_beqz(reg_a1, 28);                                                       \
    generate_load_imm(reg_temp2, 32);                                          \
    rv_bgeu(reg_a1, reg_temp2, 24);                                            \
    rv_addi(reg_temp, reg_a1, -1);                                             \
    rv_srl(reg_c_cache, reg_a0, reg_temp);                                     \
    rv_andi(reg_c_cache, reg_c_cache, 1);                                      \
    rv_srl(reg_a0, reg_a0, reg_a1);                                            \
    rv_j(20);                                                                  \
    /* amt >= 32: C = bit31(Rm) if amt==32, else 0 */                          \
    rv_srli(reg_c_cache, reg_a0, 31);                                          \
    rv_beq(reg_a1, reg_temp2, 8);                                              \
    rv_mv(reg_c_cache, reg_zero);                                              \
    rv_mv(reg_a0, reg_zero);                                                   \
}                                                                             \

#define generate_shift_reg_asr_flags(_rm, _rs)                                \
{                                                                             \
    generate_load_reg_pc(reg_a1, _rs, 8);                                      \
    rv_andi(reg_a1, reg_a1, 0xFF);                                             \
    generate_load_reg_pc(reg_a0, _rm, 12);                                     \
    rv_beqz(reg_a1, 24);                                                       \
    generate_load_imm(reg_temp2, 32);                                          \
    rv_bltu(reg_a1, reg_temp2, 16);                                            \
    rv_srli(reg_c_cache, reg_a0, 31);                                          \
    rv_srai(reg_a0, reg_a0, 31);                                               \
    rv_j(20);                                                                  \
    rv_addi(reg_temp, reg_a1, -1);                                             \
    rv_srl(reg_c_cache, reg_a0, reg_temp);                                     \
    rv_andi(reg_c_cache, reg_c_cache, 1);                                      \
    rv_sra(reg_a0, reg_a0, reg_a1);                                            \
}                                                                             \

#define generate_shift_reg_ror_flags(_rm, _rs)                                \
{                                                                             \
    generate_load_reg_pc(reg_a1, _rs, 8);                                      \
    rv_andi(reg_a1, reg_a1, 0xFF);                                             \
    generate_load_reg_pc(reg_a0, _rm, 12);                                     \
    rv_beqz(reg_a1, 16);                                                       \
    rv_addi(reg_temp, reg_a1, -1);                                             \
    rv_srl(reg_c_cache, reg_a0, reg_temp);                                     \
    rv_andi(reg_c_cache, reg_c_cache, 1);                                      \
    rv_ror(reg_a0, reg_a0, reg_a1, reg_temp, reg_temp2);                      \
    rv_j(20);                                                                  \
    rv_andi(reg_c_cache, reg_a0, 1);                                           \
    rv_srli(reg_a0, reg_a0, 1);                                                \
    rv_slli(reg_temp, reg_c_cache, 31);                                        \
    rv_or(reg_a0, reg_a0, reg_temp);                                           \
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

#define generate_op_bic_imm(_rd, _rn)                                         \
{                                                                             \
    generate_load_imm(reg_temp3, ~(imm));                                     \
    rv_and(_rd, _rn, reg_temp3);                                              \
}                                                                             \

#define generate_op_and_reg(_rd, _rn, _rm)                                    \
    rv_and(_rd, _rn, _rm)                                                     \

#define generate_op_orr_reg(_rd, _rn, _rm)                                    \
    rv_or(_rd, _rn, _rm)                                                      \

#define generate_op_eor_reg(_rd, _rn, _rm)                                    \
    rv_xor(_rd, _rn, _rm)                                                     \

#define generate_op_bic_reg(_rd, _rn, _rm)                                    \
{                                                                             \
    rv_xori(reg_temp3, _rm, -1);                                              \
    rv_and(_rd, _rn, reg_temp3);                                              \
}                                                                             \

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
    rv_mv(reg_temp3, _rn);                                                    \
    rv_add(_rd, _rn, _rm);                                                    \
    generate_op_add_flags(_rd, reg_temp3, _rm)                                \

#define generate_op_subs_reg(_rd, _rn, _rm)                                   \
    rv_mv(reg_temp3, _rn);                                                    \
    rv_sub(_rd, _rn, _rm);                                                    \
    generate_op_sub_flags(_rd, reg_temp3, _rm)                                \

#define generate_op_rsbs_reg(_rd, _rn, _rm)                                   \
    rv_mv(reg_temp3, _rm);                                                    \
    rv_sub(_rd, _rm, _rn);                                                    \
    generate_op_sub_flags(_rd, reg_temp3, _rn)                                      \

#define generate_op_adcs_reg(_rd, _rn, _rm)                                   \
{                                                                             \
    rv_add(reg_temp3, _rm, reg_c_cache);                                      \
    rv_sltu(reg_temp2, reg_temp3, _rm);                                       \
    rv_mv(reg_save0, _rn);                                                    \
    rv_add(_rd, _rn, reg_temp3);                                              \
    rv_sltu(reg_c_cache, _rd, reg_save0);                                     \
    rv_or(reg_c_cache, reg_c_cache, reg_temp2);                               \
    if (check_generate_v_flag)                                                \
    {                                                                         \
        rv_xor(reg_temp, reg_save0, _rd);                                     \
        rv_xor(reg_temp2, _rm, _rd);                                          \
        rv_and(reg_v_cache, reg_temp, reg_temp2);                             \
        rv_srli(reg_v_cache, reg_v_cache, 31);                                \
    }                                                                         \
    generate_op_logic_flags(_rd)                                              \
}                                                                             \

#define generate_op_sbcs_reg(_rd, _rn, _rm)                                   \
{                                                                             \
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
        rv_xor(reg_temp, reg_save0, _rm);                                     \
        rv_xor(reg_temp2, reg_save0, _rd);                                    \
        rv_and(reg_v_cache, reg_temp, reg_temp2);                             \
        rv_srli(reg_v_cache, reg_v_cache, 31);                                \
    }                                                                         \
    generate_op_logic_flags(_rd)                                              \
}                                                                             \

#define generate_op_rscs_reg(_rd, _rn, _rm)                                   \
{                                                                             \
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
        rv_xor(reg_temp, reg_save0, _rn);                                     \
        rv_xor(reg_temp2, reg_save0, _rd);                                    \
        rv_and(reg_v_cache, reg_temp, reg_temp2);                             \
        rv_srli(reg_v_cache, reg_v_cache, 31);                                \
    }                                                                         \
    generate_op_logic_flags(_rd)                                              \
}                                                                             \

#define generate_op_neg_reg(_rd, _rn, _rm)                                    \
    generate_op_subs_reg(_rd, reg_zero, _rm)                                  \

#define generate_op_adds_imm(_rd, _rn)                                        \
    generate_load_imm(reg_temp3, imm);                                        \
    rv_mv(reg_save0, _rn);                                                    \
    rv_add(_rd, _rn, reg_temp3);                                              \
    generate_op_add_flags(_rd, reg_save0, reg_temp3)                          \

#define generate_op_subs_imm(_rd, _rn)                                        \
    generate_load_imm(reg_temp3, imm);                                        \
    rv_mv(reg_save0, _rn);                                                    \
    rv_sub(_rd, _rn, reg_temp3);                                              \
    generate_op_sub_flags(_rd, reg_save0, reg_temp3)                          \

#define generate_op_rsbs_imm(_rd, _rn)                                        \
    generate_load_imm(reg_temp3, imm);                                        \
    rv_mv(reg_save0, _rn);                                                    \
    rv_sub(_rd, reg_temp3, _rn);                                              \
    generate_op_sub_flags(_rd, reg_temp3, reg_save0)                          \

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

#define generate_op_cmp_reg(_rd, _rn, _rm)                                    \
    generate_op_subs_reg(reg_temp2, _rn, _rm)                                 \

#define generate_op_cmn_reg(_rd, _rn, _rm)                                    \
    generate_op_adds_reg(reg_temp2, _rn, _rm)                                 \

#define generate_op_tst_reg(_rd, _rn, _rm)                                    \
    generate_op_ands_reg(reg_temp2, _rn, _rm)                                 \

#define generate_op_teq_reg(_rd, _rn, _rm)                                    \
    generate_op_eors_reg(reg_temp2, _rn, _rm)                                 \

#define generate_op_cmp_imm(_rd, _rn)                                         \
    generate_op_subs_imm(reg_temp2, _rn)                                      \

#define generate_op_cmn_imm(_rd, _rn)                                         \
    generate_op_adds_imm(reg_temp2, _rn)                                      \

#define generate_op_tst_imm(_rd, _rn)                                         \
    generate_op_ands_imm(reg_temp2, _rn)                                      \

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
    generate_load_pc(reg_a1, (pc));                                           \
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
    rv_mul(arm_to_rv_reg[rd], arm_to_rv_reg[rm], arm_to_rv_reg[rs]);          \
    rv_add(arm_to_rv_reg[rd], arm_to_rv_reg[rd], arm_to_rv_reg[rn]);          \
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
    generate_load_pc(reg_a1, (pc));                                           \
    generate_function_call(rv_execute_load_##mem_type);                       \
    generate_store_reg(reg_res, rd);                                          \
    check_store_reg_pc_no_flags(rd);                                          \
}                                                                             \

#define arm_access_memory_store(mem_type)                                     \
{                                                                             \
    cycle_count++;                                                            \
    generate_load_pc(reg_a2, (pc + 4));                                       \
    generate_load_reg_pc(reg_a1, rd, 12);                                     \
    generate_function_call(rv_execute_store_##mem_type);                      \
}                                                                             \

#define arm_access_memory_reg_pre_up()                                        \
    rv_add(reg_a0, arm_to_rv_reg[rn], arm_to_rv_reg[rm])                      \

#define arm_access_memory_reg_pre_down()                                      \
    rv_sub(reg_a0, arm_to_rv_reg[rn], arm_to_rv_reg[rm])                      \

#define arm_access_memory_reg_pre(adjust_dir)                                 \
    check_load_reg_pc(arm_reg_a0, rn, 8);                                     \
    arm_access_memory_reg_pre_##adjust_dir()                                  \

#define arm_access_memory_reg_pre_wb(adjust_dir)                              \
    arm_access_memory_reg_pre(adjust_dir);                                    \
    generate_store_reg(reg_a0, rn)                                            \

#define arm_access_memory_reg_post_up()                                       \
    rv_add(arm_to_rv_reg[rn], arm_to_rv_reg[rn], arm_to_rv_reg[rm])           \

#define arm_access_memory_reg_post_down()                                     \
    rv_sub(arm_to_rv_reg[rn], arm_to_rv_reg[rn], arm_to_rv_reg[rm])           \

#define arm_access_memory_reg_post(adjust_dir)                                \
    generate_load_reg(reg_a0, rn);                                            \
    arm_access_memory_reg_post_##adjust_dir()                                 \

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
    generate_load_pc(reg_a1, (pc));                                           \
    generate_function_call(rv_execute_aligned_load32);                        \
    generate_store_reg(reg_res, i)                                            \

#define arm_block_memory_store()                                              \
    generate_load_reg_pc(reg_a1, i, 8);                                       \
    generate_load_pc(reg_a2, (pc + 4));                                       \
    generate_function_call(rv_execute_aligned_store32)                        \

#define arm_block_memory_final_load(writeback_type)                           \
    arm_block_memory_load()                                                   \

#define arm_block_memory_final_store(writeback_type)                          \
    generate_load_pc(reg_a2, (pc + 4));                                       \
    generate_load_reg(reg_a1, i);                                             \
    arm_block_memory_writeback_post_store(writeback_type);                    \
    generate_function_call(rv_execute_store_u32)                              \

#define arm_block_memory_adjust_pc_store()                                    \

#define arm_block_memory_adjust_pc_load()                                     \
    if (reg_list & 0x8000)                                                    \
    {                                                                         \
        generate_indirect_branch_arm();                                       \
    }                                                                         \

#define arm_block_memory_offset_down_a()                                      \
    generate_sub_imm(reg_save0, base_reg, ((word_bit_count(reg_list) - 1) * 4)) \

#define arm_block_memory_offset_down_b()                                      \
    generate_sub_imm(reg_save0, base_reg, (word_bit_count(reg_list) * 4))     \

#define arm_block_memory_offset_no()                                          \
    generate_add_imm(reg_save0, base_reg, 0)                                  \

#define arm_block_memory_offset_up()                                          \
    generate_add_imm(reg_save0, base_reg, 4)                                  \

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
                                                                              \
    generate_load_imm(reg_temp, ~3u);                                         \
    rv_and(reg_save0, reg_save0, reg_temp);                                   \
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
    generate_load_pc(reg_r14, (pc + 4));                                      \
    generate_branch();                                                        \
}                                                                             \

#define arm_bx()                                                              \
    arm_decode_branchx(opcode);                                               \
    generate_load_reg_pc(reg_a0, rn, 8);                                      \
    generate_indirect_branch_dual()                                           \

#define arm_swi()                                                             \
    generate_load_pc(reg_a0, (pc + 4));                                       \
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
    if (check_generate_c_flag)                                                \
    {                                                                         \
        generate_shift_imm_##name##_flags(rd, rs, imm);                       \
    }                                                                         \
    else                                                                      \
    {                                                                         \
        generate_shift_imm_##name##_no_flags(rd, rs, imm);                    \
    }                                                                         \
    if (rs != rd)                                                             \
    {                                                                         \
        generate_mov(rd, rs);                                                 \
    }                                                                         \

#define thumb_generate_shift_reg(name)                                        \
{                                                                             \
    u32 original_rd = rd;                                                     \
    if (check_generate_c_flag)                                                \
    {                                                                         \
        generate_shift_reg_##name##_flags(rd, rs);                            \
    }                                                                         \
    else                                                                      \
    {                                                                         \
        generate_shift_reg_##name##_no_flags(rd, rs);                         \
    }                                                                         \
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
    generate_load_pc(reg_a1, (pc));                                           \
    generate_function_call(rv_execute_load_##mem_type);                       \
    generate_store_reg(reg_res, reg_rd);                                      \
}                                                                             \

#define thumb_access_memory_store(mem_type, reg_rd)                           \
{                                                                             \
    cycle_count++;                                                            \
    generate_load_reg(reg_a1, reg_rd);                                        \
    generate_load_pc(reg_a2, (pc + 2));                                       \
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
    generate_add_imm(reg_save0, base_reg, 0)                                  \

#define thumb_block_address_preadjust_down(base_reg)                          \
    generate_sub_imm(reg_save0, base_reg, (bit_count[reg_list] * 4));         \
    generate_add_imm(base_reg, reg_save0, 0)                                  \

#define thumb_block_address_preadjust_push_lr(base_reg)                       \
    generate_sub_imm(reg_save0, base_reg, ((bit_count[reg_list] + 1) * 4));   \
    generate_add_imm(base_reg, reg_save0, 0)                                  \

#define thumb_block_address_postadjust_no(base_reg)                           \

#define thumb_block_address_postadjust_up(base_reg)                           \
    generate_add_imm(base_reg, reg_save0, (bit_count[reg_list] * 4))          \

#define thumb_block_address_postadjust_pop_pc(base_reg)                       \
    generate_add_imm(base_reg, reg_save0, ((bit_count[reg_list] + 1) * 4))    \

#define thumb_block_address_postadjust_push_lr(base_reg)                      \

#define thumb_block_memory_load()                                             \
    generate_load_pc(reg_a1, (pc));                                           \
    generate_function_call(rv_execute_aligned_load32);                        \
    generate_store_reg(reg_res, i)                                            \

#define thumb_block_memory_store()                                            \
    generate_load_reg(reg_a1, i);                                             \
    generate_load_pc(reg_a2, (pc + 2));                                       \
    generate_function_call(rv_execute_aligned_store32);                       \

#define thumb_block_memory_final_load()                                       \
    thumb_block_memory_load()                                                 \

#define thumb_block_memory_final_store()                                      \
    generate_load_pc(reg_a2, (pc + 2));                                       \
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

#define thumb_block_memory_extra_no()                                         \

#define thumb_block_memory_extra_up()                                         \

#define thumb_block_memory_extra_down()                                       \

#define thumb_block_memory_extra_push_lr()                                    \
    generate_add_imm(reg_a0, reg_save0, (bit_count[reg_list] * 4));           \
    generate_load_reg(reg_a1, REG_LR);                                        \
    generate_load_pc(reg_a2, (pc + 2));                                       \
    generate_function_call(rv_execute_aligned_store32);                       \

#define thumb_block_memory_extra_pop_pc()                                     \
    generate_add_imm(reg_a0, reg_save0, (bit_count[reg_list] * 4));           \
    generate_load_pc(reg_a1, (pc));                                           \
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
    thumb_block_address_postadjust_##post_op(base_reg);                       \
                                                                              \
    generate_load_imm(reg_temp, ~3u);                                         \
    rv_and(reg_save0, reg_save0, reg_temp);                                   \
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

#define thumb_conditional_branch(condition)                                   \
{                                                                             \
    generate_cycle_update();                                                  \
    generate_condition_##condition();                                         \
    generate_branch_no_cycle_update(                                          \
        block_exits[block_exit_position].branch_source,                       \
        block_exits[block_exit_position].branch_target);                      \
    generate_branch_patch_conditional(backpatch_address, translation_ptr);    \
    block_exit_position++;                                                    \
}                                                                             \

#define generate_block_extra_vars()                                           \
    u32 stored_pc = pc;                                                         \

#define generate_block_extra_vars_arm()                                       \
    generate_block_extra_vars();                                                \

#define generate_block_extra_vars_thumb()                                     \
    generate_block_extra_vars();                                                \

#define generate_translation_gate(type)                                       \
    generate_load_pc(reg_a0, pc);                                             \
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
    rv_xori(reg_temp, reg_c_cache, 1);                                        \
    rv_or(reg_temp, reg_temp, reg_z_cache);                                   \
    (backpatch_address) = translation_ptr;                                    \
    rv_bnez(reg_temp, 0)                                                      \

#define generate_condition_ls()                                               \
    rv_xori(reg_temp, reg_c_cache, 1);                                        \
    rv_or(reg_temp, reg_temp, reg_z_cache);                                   \
    (backpatch_address) = translation_ptr;                                    \
    rv_beqz(reg_temp, 0)                                                      \

#define generate_condition_ge()                                               \
    rv_sub(reg_temp, reg_n_cache, reg_v_cache);                               \
    (backpatch_address) = translation_ptr;                                    \
    rv_bnez(reg_temp, 0)                                                      \

#define generate_condition_lt()                                               \
    rv_sub(reg_temp, reg_n_cache, reg_v_cache);                               \
    (backpatch_address) = translation_ptr;                                    \
    rv_beqz(reg_temp, 0)                                                      \

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
    // jit_selftest();
    init_bios_hooks();
}

u32 execute_arm_translate_internal(u32 cycles, void *regptr);

u32 execute_arm_translate(u32 cycles) {
    return execute_arm_translate_internal(cycles, &reg[0]);
}

#endif /* RISCV_EMIT_H */
