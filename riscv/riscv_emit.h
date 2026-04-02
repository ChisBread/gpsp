/*
 * gpsp RISC-V Dynamic Recompiler — Translation Emit Layer
 *
 * This file translates GBA ARM7TDMI instructions into RISC-V machine code.
 * It is included by cpu_threaded.c when RISCV_ARCH is defined.
 *
 * STATUS: STUB — Not yet implemented.
 * Each macro/function is defined with a placeholder that falls back to
 * the interpreter, allowing the project to compile.
 *
 * To implement: fill in each emit_* macro with the rv_* codegen macros
 * from riscv_codegen.h.
 */

#ifndef RISCV_EMIT_H
#define RISCV_EMIT_H

#include "riscv_codegen.h"

/* ================================================================
 * GBA Register → RISC-V Host Register Mapping
 *
 * GBA ARM7TDMI has 16 general-purpose registers (R0-R15) + CPSR.
 * We cache the most frequently used ones in RISC-V callee-saved regs.
 * ================================================================ */

/* Cached GBA registers in RISC-V saved registers */
#define reg_r0      rv_s0     /* GBA R0  */
#define reg_r1      rv_s1     /* GBA R1  */
#define reg_r6      rv_s2     /* GBA R6  */
#define reg_r9      rv_s3     /* GBA R9  */
#define reg_r10     rv_s4     /* GBA R10 */
#define reg_r12     rv_s5     /* GBA R12 */
#define reg_r14     rv_s6     /* GBA R14 (LR) */

/* Special-purpose host registers */
#define reg_pc      rv_s7     /* GBA R15 (PC) — always tracks PC */
#define reg_n_flag  rv_s8     /* CPSR N flag (0 or 1) */
#define reg_z_flag  rv_s9     /* CPSR Z flag (0 or 1) */
#define reg_c_flag  rv_s10    /* CPSR C flag (0 or 1) */
#define reg_v_flag  rv_s11    /* CPSR V flag (0 or 1) */

/* Pointer registers — held across JIT blocks */
#define reg_base    rv_gp     /* Pointer to GBA register file (reg[]) */
#define reg_mem     rv_tp     /* Pointer to memory access table */

/* Scratch registers for translation */
#define reg_temp    rv_t0
#define reg_temp2   rv_t1
#define reg_temp3   rv_t2
#define reg_scratch rv_t3

/* Argument registers for calling C functions */
#define reg_arg0    rv_a0
#define reg_arg1    rv_a1
#define reg_arg2    rv_a2
#define reg_rv      rv_a0     /* Return value register */

/* Number of cached GBA registers (used + special) */
#define REGISTER_CACHE_COUNT 7

/* GBA register index for each cached host register */
static const u32 reg_map[] = {
    0,   /* reg_r0  -> GBA R0  */
    1,   /* reg_r1  -> GBA R1  */
    6,   /* reg_r6  -> GBA R6  */
    9,   /* reg_r9  -> GBA R9  */
    10,  /* reg_r10 -> GBA R10 */
    12,  /* reg_r12 -> GBA R12 */
    14,  /* reg_r14 -> GBA R14 */
};

/* ================================================================
 * Stub translation macros
 *
 * TODO: Replace each stub with actual RISC-V code generation.
 * The stubs are structured to match the interface expected by
 * cpu_threaded.c's translate_block_arm() and translate_block_thumb().
 * ================================================================ */

/* Emit prologue: save callee-saved regs, set up reg_base/reg_mem */
#define emit_prologue()  /* TODO */

/* Emit epilogue: restore callee-saved regs, return to C */
#define emit_epilogue()  /* TODO */

/* Flush cached GBA registers back to memory (reg[]) */
#define emit_flush_regs()  /* TODO */

/* Load GBA registers from memory into host register cache */
#define emit_load_regs()   /* TODO */

/* ---- Data processing stubs ---- */
#define emit_mov(rd, rs)              /* TODO: rv_mv(rd, rs) */
#define emit_add(rd, rs1, rs2)        /* TODO */
#define emit_sub(rd, rs1, rs2)        /* TODO */
#define emit_and(rd, rs1, rs2)        /* TODO */
#define emit_or(rd, rs1, rs2)         /* TODO */
#define emit_xor(rd, rs1, rs2)        /* TODO */

/* ---- Immediate operations ---- */
#define emit_mov_imm(rd, imm)         /* TODO: rv_load_imm32 or rv_li */
#define emit_add_imm(rd, rs, imm)     /* TODO */
#define emit_sub_imm(rd, rs, imm)     /* TODO */
#define emit_and_imm(rd, rs, imm)     /* TODO */
#define emit_or_imm(rd, rs, imm)      /* TODO */

/* ---- Shifts ---- */
#define emit_lsl(rd, rs, shamt)       /* TODO */
#define emit_lsr(rd, rs, shamt)       /* TODO */
#define emit_asr(rd, rs, shamt)       /* TODO */
#define emit_ror(rd, rs, shamt)       /* TODO: RISC-V has no ROR, but Zbb ext does */

/* ---- Flag-setting variants ---- */
/* ARM updates N/Z/C/V flags on many operations.
 * RISC-V has no flags register — we must compute them explicitly.
 *
 * Example for ADDS (add with flag update):
 *   rv_add(rd, rs1, rs2)
 *   rv_slt(reg_v_flag, rd, rs1)     // overflow detect (simplified)
 *   rv_sltu(reg_c_flag, rd, rs1)    // carry detect
 *   rv_srai(reg_n_flag, rd, 31)     // N = sign bit
 *   rv_seqz(reg_z_flag, rd)         // Z = (result == 0)
 */
#define emit_adds(rd, rs1, rs2)       /* TODO */
#define emit_subs(rd, rs1, rs2)       /* TODO */
#define emit_ands(rd, rs1, rs2)       /* TODO */
#define emit_orrs(rd, rs1, rs2)       /* TODO */
#define emit_eors(rd, rs1, rs2)       /* TODO */

/* ---- ADC/SBC (with carry) ---- */
/* These are the hardest to translate since RISC-V has no carry flag.
 *
 * ADC: rd = rs1 + rs2 + C
 *   rv_add(rd, rs1, rs2)
 *   rv_add(rd, rd, reg_c_flag)
 *   // Then update C/V flags considering both additions
 */
#define emit_adc(rd, rs1, rs2)        /* TODO */
#define emit_sbc(rd, rs1, rs2)        /* TODO */

/* ---- Multiply ---- */
#define emit_mul(rd, rs1, rs2)        /* TODO: rv_mul(rd, rs1, rs2) */
#define emit_mla(rd, rs1, rs2, rs3)   /* TODO: mul + add */
#define emit_umull(rdlo, rdhi, rs1, rs2)  /* TODO: rv_mul + rv_mulhu */
#define emit_smull(rdlo, rdhi, rs1, rs2)  /* TODO: rv_mul + rv_mulh */

/* ---- Memory access ---- */
#define emit_ldr(rd, base, offset)    /* TODO */
#define emit_ldrh(rd, base, offset)   /* TODO */
#define emit_ldrb(rd, base, offset)   /* TODO */
#define emit_ldrsh(rd, base, offset)  /* TODO */
#define emit_ldrsb(rd, base, offset)  /* TODO */
#define emit_str(rs, base, offset)    /* TODO */
#define emit_strh(rs, base, offset)   /* TODO */
#define emit_strb(rs, base, offset)   /* TODO */

/* ---- Branching ---- */
#define emit_branch(target)           /* TODO: rv_j(offset) */
#define emit_call(target)             /* TODO: rv_call(offset) */
#define emit_return()                 rv_ret()

/* ---- Conditional branch (check flag regs) ---- */
/* Map ARM condition codes to RISC-V branch sequences:
 *   EQ: beqz(reg_z_flag)   -> bnez(reg_z_flag, target)
 *   NE: bnez(reg_z_flag)   -> beqz(reg_z_flag, target)
 *   CS: bnez(reg_c_flag)
 *   CC: beqz(reg_c_flag)
 *   MI: bnez(reg_n_flag)
 *   PL: beqz(reg_n_flag)
 *   VS: bnez(reg_v_flag)
 *   VC: beqz(reg_v_flag)
 *   ... (HI, LS, GE, LT, GT, LE need compound conditions)
 */
#define emit_branch_cond(cond, target)  /* TODO */

/* ---- I-cache synchronization ---- */
#define emit_icache_sync() \
    asm volatile ("fence.i" ::: "memory")

/* Sync translation cache for ESP32-P4 */
static inline void translate_icache_sync(void)
{
    asm volatile ("fence.i" ::: "memory");
}

#endif /* RISCV_EMIT_H */
