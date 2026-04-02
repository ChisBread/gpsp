/*
 * gpsp RISC-V Dynamic Recompiler — Runtime Stub
 *
 * This file contains the assembly glue between C code and JIT-compiled
 * RISC-V code blocks. It handles:
 *   - Entry into translated blocks (execute_arm_translate / execute_thumb)
 *   - Return from translated blocks back to C
 *   - Register save/restore across JIT boundaries
 *
 * STATUS: STUB — Minimal framework, to be implemented alongside riscv_emit.h
 *
 * Calling convention: RISC-V ILP32 (standard ESP-IDF toolchain)
 *   a0-a7:  argument/return registers (caller-saved)
 *   t0-t6:  temporary registers (caller-saved)
 *   s0-s11: callee-saved registers (used for GBA register cache)
 *   ra:     return address
 *   sp:     stack pointer
 *   gp:     global pointer (repurposed: GBA reg[] base pointer)
 *   tp:     thread pointer (repurposed: memory map table pointer)
 */

    .text
    .align 2

/* ================================================================
 * GBA register ↔ RISC-V host register mapping (must match riscv_emit.h)
 *
 *   s0  = GBA R0         s7  = GBA R15 (PC)
 *   s1  = GBA R1         s8  = CPSR N flag
 *   s2  = GBA R6         s9  = CPSR Z flag
 *   s3  = GBA R9         s10 = CPSR C flag
 *   s4  = GBA R10        s11 = CPSR V flag
 *   s5  = GBA R12
 *   s6  = GBA R14 (LR)
 *   gp  = reg[] base address
 *   tp  = memory map table
 * ================================================================ */

/* Stack frame layout (bytes from sp):
 *   0:   ra
 *   4:   s0
 *   8:   s1
 *  12:   s2
 *  16:   s3
 *  20:   s4
 *  24:   s5
 *  28:   s6
 *  32:   s7
 *  36:   s8
 *  40:   s9
 *  44:   s10
 *  48:   s11
 *  52:   gp (original, to restore)
 *  56:   tp (original, to restore)
 *  Total: 64 bytes (16-byte aligned)
 */
#define FRAME_SIZE  64

/* ================================================================
 * execute_arm_translate(u32 cycles, void *reg_ptr, void *mem_table)
 *
 * Entry from C into the JIT execution engine.
 *   a0 = number of cycles to execute
 *   a1 = pointer to reg[] (GBA register file)
 *   a2 = pointer to memory access table
 *
 * Returns remaining cycles in a0.
 * ================================================================ */

    .globl execute_arm_translate
    .type  execute_arm_translate, @function
execute_arm_translate:
    /* ---- Save callee-saved registers ---- */
    addi    sp, sp, -FRAME_SIZE
    sw      ra,   0(sp)
    sw      s0,   4(sp)
    sw      s1,   8(sp)
    sw      s2,  12(sp)
    sw      s3,  16(sp)
    sw      s4,  20(sp)
    sw      s5,  24(sp)
    sw      s6,  28(sp)
    sw      s7,  32(sp)
    sw      s8,  36(sp)
    sw      s9,  40(sp)
    sw      s10, 44(sp)
    sw      s11, 48(sp)
    sw      gp,  52(sp)
    sw      tp,  56(sp)

    /* ---- Set up JIT environment ---- */
    mv      gp, a1          /* gp = reg[] base pointer */
    mv      tp, a2          /* tp = memory map table */

    /* ---- Load cached GBA registers from reg[] ---- */
    lw      s0,  (0*4)(gp)   /* GBA R0 */
    lw      s1,  (1*4)(gp)   /* GBA R1 */
    lw      s2,  (6*4)(gp)   /* GBA R6 */
    lw      s3,  (9*4)(gp)   /* GBA R9 */
    lw      s4, (10*4)(gp)   /* GBA R10 */
    lw      s5, (12*4)(gp)   /* GBA R12 */
    lw      s6, (14*4)(gp)   /* GBA R14 */
    lw      s7, (15*4)(gp)   /* GBA R15 (PC) */

    /* ---- TODO: Load CPSR flags into s8-s11 ---- */
    /* The CPSR is stored at reg[REG_CPSR].
     * Extract N, Z, C, V bits into separate registers. */
    li      s8, 0             /* N flag placeholder */
    li      s9, 0             /* Z flag placeholder */
    li      s10, 0            /* C flag placeholder */
    li      s11, 0            /* V flag placeholder */

    /* ---- TODO: Look up translated block and jump to it ---- */
    /* For now, just return immediately (interpreter fallback) */
    mv      a0, a0            /* Pass cycles through unchanged */
    j       .Lexit_jit

    .globl return_from_jit
    .type  return_from_jit, @function
return_from_jit:
    /* ---- JIT blocks jump here when done ---- */
    /* a0 = remaining cycles */

.Lexit_jit:
    /* ---- Flush cached GBA registers back to reg[] ---- */
    sw      s0,  (0*4)(gp)
    sw      s1,  (1*4)(gp)
    sw      s2,  (6*4)(gp)
    sw      s3,  (9*4)(gp)
    sw      s4, (10*4)(gp)
    sw      s5, (12*4)(gp)
    sw      s6, (14*4)(gp)
    sw      s7, (15*4)(gp)

    /* ---- TODO: Write back CPSR flags from s8-s11 ---- */

    /* ---- Restore callee-saved registers ---- */
    lw      ra,   0(sp)
    lw      s0,   4(sp)
    lw      s1,   8(sp)
    lw      s2,  12(sp)
    lw      s3,  16(sp)
    lw      s4,  20(sp)
    lw      s5,  24(sp)
    lw      s6,  28(sp)
    lw      s7,  32(sp)
    lw      s8,  36(sp)
    lw      s9,  40(sp)
    lw      s10, 44(sp)
    lw      s11, 48(sp)
    lw      gp,  52(sp)
    lw      tp,  56(sp)
    addi    sp, sp, FRAME_SIZE

    ret

    .size execute_arm_translate, .-execute_arm_translate

/* ================================================================
 * Trampoline stubs for calling C functions from JIT code
 *
 * When JIT code needs to call a C function (e.g., memory access),
 * it jumps to these trampolines which save JIT state first.
 * ================================================================ */

    .globl jit_call_c_trampoline
    .type  jit_call_c_trampoline, @function
jit_call_c_trampoline:
    /* a0 = C function pointer, a1-a3 = arguments */
    /* TODO: implement save-state + call + restore-state */
    ret

    .size jit_call_c_trampoline, .-jit_call_c_trampoline
