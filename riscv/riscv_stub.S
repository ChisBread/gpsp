#include "../gpsp_config.h"

#define defsymbl(symbol) \
.align 2;                \
.type symbol, %function ;\
.global symbol ;         \
.global _##symbol ;      \
symbol:                  \
_##symbol:

#define defobj(symbol)    \
.type symbol, %object ;   \
.global symbol ;          \
.global _##symbol ;       \
symbol:                   \
_##symbol:

        .text
        .align 2

#define REG_R0             (0 * 4)
#define REG_R1             (1 * 4)
#define REG_R2             (2 * 4)
#define REG_R3             (3 * 4)
#define REG_R4             (4 * 4)
#define REG_R5             (5 * 4)
#define REG_R6             (6 * 4)
#define REG_R7             (7 * 4)
#define REG_R8             (8 * 4)
#define REG_R9             (9 * 4)
#define REG_R10            (10 * 4)
#define REG_R11            (11 * 4)
#define REG_R12            (12 * 4)
#define REG_R13            (13 * 4)
#define REG_R14            (14 * 4)
#define REG_PC             (15 * 4)
#define REG_CPSR           (16 * 4)
#define CPU_MODE           (17 * 4)
#define CPU_HALT_STATE     (18 * 4)
#define REG_BUS_VALUE      (19 * 4)
#define REG_N_FLAG         (20 * 4)
#define REG_Z_FLAG         (21 * 4)
#define REG_C_FLAG         (22 * 4)
#define REG_V_FLAG         (23 * 4)
#define REG_SAVE           (26 * 4)
#define REG_SAVE2          (27 * 4)
#define REG_SAVE3          (28 * 4)
#define REG_SAVE4          (29 * 4)
#define REG_SAVE5          (30 * 4)
#define REG_SAVE6          (31 * 4)

#define MODE_SUPERVISOR    0x13
#define SPSR_RAM_OFF       0x100
#define REGMODE_RAM_OFF    0x118
#define SUPERVISOR_SPSR    (SPSR_RAM_OFF + 3 * 4)
#define SUPERVISOR_LR      (REGMODE_RAM_OFF + (3 * (7 * 4)) + (6 * 4))

/* Stack frame layout (bytes from sp):
 *   0:  s0
 *   4:  s1
 *   8:  s2
 *  12:  s3
 *  16:  s4
 *  20:  s5
 *  24:  s6
 *  28:  s7
 *  32:  s8
 *  36:  s9
 *  40:  s10
 *  44:  s11
 *  48:  ra
 *  52:  gp
 *  56:  tp
 *  60:  jit tp (cycles)
 *  64:  jit gp (reg base)
 *  68-79: padding to preserve 16-byte stack alignment across C calls
 */
#define FRAME_SIZE         80

#define store_registers()                       \
    sw t3, REG_SAVE2(gp);                        \
    sw s0,  REG_R0(gp);                          \
    sw s1,  REG_R1(gp);                          \
    sw a3,  REG_R2(gp);                          \
    sw a4,  REG_R3(gp);                          \
    sw a5,  REG_R4(gp);                          \
    sw a6,  REG_R5(gp);                          \
    sw s2,  REG_R6(gp);                          \
    sw a7,  REG_R7(gp);                          \
    sw t4,  REG_R8(gp);                          \
    sw s3,  REG_R9(gp);                          \
    sw s4,  REG_R10(gp);                         \
    sw t5,  REG_R11(gp);                         \
    sw s5,  REG_R12(gp);                         \
    sw t6,  REG_R13(gp);                         \
    sw s6,  REG_R14(gp);                         \
    sw s7,  REG_PC(gp)

#define load_registers()                        \
    lw t3, REG_SAVE2(gp);                        \
    lw s0,  REG_R0(gp);                          \
    lw s1,  REG_R1(gp);                          \
    lw a3,  REG_R2(gp);                          \
    lw a4,  REG_R3(gp);                          \
    lw a5,  REG_R4(gp);                          \
    lw a6,  REG_R5(gp);                          \
    lw s2,  REG_R6(gp);                          \
    lw a7,  REG_R7(gp);                          \
    lw t4,  REG_R8(gp);                          \
    lw s3,  REG_R9(gp);                          \
    lw s4,  REG_R10(gp);                         \
    lw t5,  REG_R11(gp);                         \
    lw s5,  REG_R12(gp);                         \
    lw t6,  REG_R13(gp);                         \
    lw s6,  REG_R14(gp);                         \
    lw s7,  REG_PC(gp)

#define extract_flags(tmpreg)                   \
    lw   tmpreg, REG_CPSR(gp);                   \
    srli s8, tmpreg, 31;                         \
    andi s8, s8, 1;                              \
    srli s9, tmpreg, 30;                         \
    andi s9, s9, 1;                              \
    srli s10, tmpreg, 29;                        \
    andi s10, s10, 1;                            \
    srli s11, tmpreg, 28;                        \
    andi s11, s11, 1

#define consolidate_flags(tmpreg, tmpreg2)      \
    lw   tmpreg, REG_CPSR(gp);                    \
    li   tmpreg2, 0x0fffffff;                     \
    and  tmpreg, tmpreg, tmpreg2;                 \
    slli tmpreg2, s8, 31;                         \
    or   tmpreg, tmpreg, tmpreg2;                 \
    slli tmpreg2, s9, 30;                         \
    or   tmpreg, tmpreg, tmpreg2;                 \
    slli tmpreg2, s10, 29;                        \
    or   tmpreg, tmpreg, tmpreg2;                 \
    slli tmpreg2, s11, 28;                        \
    or   tmpreg, tmpreg, tmpreg2;                 \
    sw   tmpreg, REG_CPSR(gp)

/*
 * execute_arm_translate_internal(u32 cycles, void *reg_ptr)
 *   a0 = cycles
 *   a1 = &reg[0]
 */
defsymbl(execute_arm_translate_internal)
    addi sp, sp, -FRAME_SIZE
    sw   ra,   0(sp)
    sw   s0,   4(sp)
    sw   s1,   8(sp)
    sw   s2,  12(sp)
    sw   s3,  16(sp)
    sw   s4,  20(sp)
    sw   s5,  24(sp)
    sw   s6,  28(sp)
    sw   s7,  32(sp)
    sw   s8,  36(sp)
    sw   s9,  40(sp)
    sw   s10, 44(sp)
    sw   s11, 48(sp)
    sw   gp,  52(sp)
    sw   tp,  56(sp)

    mv   t0, gp
    mv   t1, tp
    mv   gp, a1
    sw   t0, REG_SAVE5(gp)
    sw   t1, REG_SAVE6(gp)
    mv   tp, a0
    sw   tp, 60(sp)
    sw   gp, 64(sp)

    load_registers()
    extract_flags(t0)

    lw   t0, CPU_HALT_STATE(gp)
    bnez t0, .Lcpu_sleep_loop

.Llookup_pc:
    lw   a0, REG_PC(gp)
    lw   t0, REG_CPSR(gp)
    extract_flags(t1)
    andi t0, t0, 0x20
    lw   gp, 52(sp)
    lw   tp, 56(sp)
    beqz t0, 1f
    call block_lookup_address_thumb
    lw   gp, 64(sp)
    lw   tp, 60(sp)
    load_registers()
    jalr zero, a0, 0
1:
    call block_lookup_address_arm
    lw   gp, 64(sp)
    lw   tp, 60(sp)
    load_registers()
    jalr zero, a0, 0

.Lcpu_sleep_loop:
    sw   tp, 60(sp)
    sw   gp, 64(sp)
    lw   gp, 52(sp)
    lw   tp, 56(sp)
    lw   a0, 60(sp)
    call update_gba
    lw   gp, 64(sp)
    mv   tp, a0
    sw   tp, 60(sp)
    blt  a0, zero, .Lreturn_cycles
    lw   t0, CPU_HALT_STATE(gp)
    bnez t0, .Lcpu_sleep_loop
    j    .Llookup_pc

.Lreturn_cycles:
    mv   a0, tp

.Lreturn_to_main:
    lw   ra,   0(sp)
    lw   s0,   4(sp)
    lw   s1,   8(sp)
    lw   s2,  12(sp)
    lw   s3,  16(sp)
    lw   s4,  20(sp)
    lw   s5,  24(sp)
    lw   s6,  28(sp)
    lw   s7,  32(sp)
    lw   s8,  36(sp)
    lw   s9,  40(sp)
    lw   s10, 44(sp)
    lw   s11, 48(sp)
    lw   gp,  52(sp)
    lw   tp,  56(sp)
    addi sp, sp, FRAME_SIZE
    ret

    .globl return_from_jit
    .type  return_from_jit, @function
return_from_jit:
    mv   a0, tp

.Lexit_jit:
    store_registers()
    consolidate_flags(t0, t1)

    lw   ra,   0(sp)
    lw   s0,   4(sp)
    lw   s1,   8(sp)
    lw   s2,  12(sp)
    lw   s3,  16(sp)
    lw   s4,  20(sp)
    lw   s5,  24(sp)
    lw   s6,  28(sp)
    lw   s7,  32(sp)
    lw   s8,  36(sp)
    lw   s9,  40(sp)
    lw   s10, 44(sp)
    lw   s11, 48(sp)
    lw   gp,  52(sp)
    lw   tp,  56(sp)
    addi sp, sp, FRAME_SIZE
    ret

defsymbl(rv_update_gba)
    mv   s7, a0                  /* update s7 so store_registers saves correct PC */
    sw   ra, REG_SAVE(gp)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    mv   a0, s10
    call update_gba
    blt  a0, zero, .Lreturn_to_main

    /* Clean cycle count: clear bits 30-31 (flags), keep bits 0-29 */
    slli s10, a0, 2
    srli s10, s10, 2

    mv   gp, s11
    srli t0, a0, 30
    andi t0, t0, 1
    beqz t0, 1f

    lw   a0, REG_PC(gp)
    lw   t0, REG_CPSR(gp)
    andi t0, t0, 0x20
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    beqz t0, 2f
    call block_lookup_address_thumb
    mv   gp, s11
    mv   tp, s10
    extract_flags(t0)
    load_registers()
    jalr zero, a0, 0
2:
    call block_lookup_address_arm
    mv   gp, s11
    mv   tp, s10
    extract_flags(t0)
    load_registers()
    jalr zero, a0, 0
1:
    mv   tp, s10
    extract_flags(t0)
    load_registers()
    lw   ra, REG_SAVE(gp)
    ret

defsymbl(rv_indirect_branch_arm)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    call block_lookup_address_arm
    mv   gp, s11
    mv   tp, s10
    extract_flags(t0)
    load_registers()
    jalr zero, a0, 0

defsymbl(rv_indirect_branch_thumb)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    call block_lookup_address_thumb
    mv   gp, s11
    mv   tp, s10
    extract_flags(t0)
    load_registers()
    jalr zero, a0, 0

defsymbl(rv_indirect_branch_dual)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    call block_lookup_address_dual
    mv   gp, s11
    mv   tp, s10
    extract_flags(t0)
    load_registers()
    jalr zero, a0, 0

defsymbl(rv_execute_load_u8)
    sw   ra, REG_SAVE(gp)
    sw   a1, REG_PC(gp)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    call execute_load_u8
    mv   t0, a0
    mv   gp, s11
    mv   tp, s10
    extract_flags(t1)
    load_registers()
    lw   ra, REG_SAVE(gp)
    mv   a0, t0
    ret

defsymbl(rv_execute_load_s8)
    sw   ra, REG_SAVE(gp)
    sw   a1, REG_PC(gp)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    call execute_load_s8
    mv   t0, a0
    mv   gp, s11
    mv   tp, s10
    extract_flags(t1)
    load_registers()
    lw   ra, REG_SAVE(gp)
    mv   a0, t0
    ret

defsymbl(rv_execute_load_u16)
    sw   ra, REG_SAVE(gp)
    sw   a1, REG_PC(gp)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    call execute_load_u16
    mv   t0, a0
    mv   gp, s11
    mv   tp, s10
    extract_flags(t1)
    load_registers()
    lw   ra, REG_SAVE(gp)
    mv   a0, t0
    ret

defsymbl(rv_execute_load_s16)
    sw   ra, REG_SAVE(gp)
    sw   a1, REG_PC(gp)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    call execute_load_s16
    mv   t0, a0
    mv   gp, s11
    mv   tp, s10
    extract_flags(t1)
    load_registers()
    lw   ra, REG_SAVE(gp)
    mv   a0, t0
    ret

defsymbl(rv_execute_load_u32)
    sw   ra, REG_SAVE(gp)
    sw   a1, REG_PC(gp)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    call execute_load_u32
    mv   t0, a0
    mv   gp, s11
    mv   tp, s10
    extract_flags(t1)
    load_registers()
    lw   ra, REG_SAVE(gp)
    mv   a0, t0
    ret

defsymbl(rv_execute_aligned_load32)
    sw   ra, REG_SAVE(gp)
    sw   a1, REG_PC(gp)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    call execute_aligned_load32
    mv   t0, a0
    mv   gp, s11
    mv   tp, s10
    extract_flags(t1)
    load_registers()
    lw   ra, REG_SAVE(gp)
    mv   a0, t0
    ret

defsymbl(rv_execute_store_u8)
    sw   ra, REG_SAVE(gp)
    sw   a2, REG_PC(gp)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    call execute_store_u8
    mv   gp, s11
    mv   tp, s10
    extract_flags(t1)
    load_registers()
    lw   ra, REG_SAVE(gp)
    ret

defsymbl(rv_execute_store_u16)
    sw   ra, REG_SAVE(gp)
    sw   a2, REG_PC(gp)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    call execute_store_u16
    mv   gp, s11
    mv   tp, s10
    extract_flags(t1)
    load_registers()
    lw   ra, REG_SAVE(gp)
    ret

defsymbl(rv_execute_store_u32)
    sw   ra, REG_SAVE(gp)
    sw   a2, REG_PC(gp)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    call execute_store_u32
    mv   gp, s11
    mv   tp, s10
    extract_flags(t1)
    load_registers()
    lw   ra, REG_SAVE(gp)
    ret

defsymbl(rv_execute_aligned_store32)
    sw   ra, REG_SAVE(gp)
    sw   a2, REG_PC(gp)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    call execute_aligned_store32
    mv   gp, s11
    mv   tp, s10
    extract_flags(t1)
    load_registers()
    lw   ra, REG_SAVE(gp)
    ret

defsymbl(rv_cheat_hook)
    sw   ra, REG_SAVE(gp)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    call process_cheats
    mv   gp, s11
    mv   tp, s10
    extract_flags(t1)
    load_registers()
    lw   ra, REG_SAVE(gp)
    ret

defsymbl(execute_read_cpsr)
    consolidate_flags(a0, t0)
    ret

defsymbl(execute_read_spsr)
    lw   t0, CPU_MODE(gp)
    andi t0, t0, 0x0f
    addi t1, gp, SPSR_RAM_OFF
    slli t0, t0, 2
    add  t1, t1, t0
    lw   a0, 0(t1)
    ret

defsymbl(execute_store_cpsr)
    /* a0=new cpsr, a1=current pc, a2=user mask, t2=privileged mask */
    sw   a1, REG_PC(gp)
    lw   t1, CPU_MODE(gp)
    andi t1, t1, 0x10
    beqz t1, 1f
    mv   t1, t2
    j    2f
1:
    mv   t1, a2
2:
    lw   t2, REG_CPSR(gp)
    and  a0, a0, t1
    xori t0, t1, -1
    and  t2, t2, t0
    or   a0, a0, t2
    sw   a0, REG_CPSR(gp)
    extract_flags(a2)

    andi t2, t1, 0xff
    beqz t2, 3f

    sw   ra, REG_SAVE3(gp)
    store_registers()
    consolidate_flags(t2, t3)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    mv   a2, t1
    call execute_store_cpsr_body
    beqz a0, 4f
    call block_lookup_address_arm
    mv   gp, s11
    mv   tp, s10
    extract_flags(t2)
    load_registers()
    lw   ra, REG_SAVE3(gp)
    jalr zero, a0, 0
4:
    mv   gp, s11
    mv   tp, s10
    extract_flags(t2)
    load_registers()
    lw   ra, REG_SAVE3(gp)
3:
    ret

defsymbl(execute_store_spsr)
    /* a0=new spsr, a1=store mask */
    lw   t0, CPU_MODE(gp)
    andi t0, t0, 0x0f
    slli t0, t0, 2
    addi t1, gp, SPSR_RAM_OFF
    add  t1, t1, t0
    lw   t2, 0(t1)
    and  a0, a0, a1
    xori t0, a1, -1
    and  t2, t2, t0
    or   a0, a0, t2
    sw   a0, 0(t1)
    ret

defsymbl(execute_spsr_restore)
    /* a0=current pc */
    sw   a0, REG_PC(gp)
    lw   t0, CPU_MODE(gp)
    andi t0, t0, 0x0f
    beqz t0, 1f
    slli t1, t0, 2
    addi t1, t1, SPSR_RAM_OFF
    add  t1, gp, t1
    lw   t2, 0(t1)
    sw   t2, REG_CPSR(gp)
    extract_flags(t2)

    sw   ra, REG_SAVE3(gp)
    store_registers()
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    call execute_spsr_restore_body
    mv   gp, s11
    mv   tp, s10
    lw   ra, REG_SAVE3(gp)
    extract_flags(t2)
    load_registers()
1:
    ret

defsymbl(execute_swi)
    /* a0=current pc */
    sw   ra, REG_SAVE(gp)
    sw   a0, REG_PC(gp)
    sw   a0, SUPERVISOR_LR(gp)
    consolidate_flags(t0, t1)
    sw   t0, SUPERVISOR_SPSR(gp)
    li   t1, -64
    and  t0, t0, t1
    li   t1, (MODE_SUPERVISOR | 0x80)
    or   t0, t0, t1
    sw   t0, REG_CPSR(gp)
    store_registers()
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    li   a0, MODE_SUPERVISOR
    call set_cpu_mode
    mv   gp, s11
    mv   tp, s10
    li   t0, 0xe3a02004
    sw   t0, REG_BUS_VALUE(gp)
    lw   ra, REG_SAVE(gp)
    extract_flags(t0)
    load_registers()
    ret

defsymbl(jit_call_c_trampoline)
    /* a0 = function pointer, a1-a3 = C arguments */
    sw   ra, REG_SAVE(gp)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    mv   t0, a0
    mv   a0, a1
    mv   a1, a2
    mv   a2, a3
    jalr ra, t0, 0
    mv   t0, a0
    mv   gp, s11
    mv   tp, s10
    extract_flags(t1)
    load_registers()
    lw   ra, REG_SAVE(gp)
    mv   a0, t0
    ret

#ifdef TRACE_INSTRUCTIONS
/*
 * rv_trace_instruction:
 *   Called from JIT-generated code after each ARM instruction.
 *   Saves all JIT registers to reg[], consolidates flags to CPSR,
 *   calls trace_jit_instruction(), then restores everything.
 */
defsymbl(rv_trace_instruction)
    sw   ra, REG_SAVE(gp)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    call trace_jit_instruction
    mv   gp, s11
    mv   tp, s10
    extract_flags(t1)
    load_registers()
    lw   ra, REG_SAVE(gp)
    ret
#endif

        .bss
        .align 6

    defobj(memory_map_read)
        .space 0x8000
    defobj(iwram)
        .space 0x10000
    defobj(vram)
        .space 0x18000
    defobj(reg)
        .space 0x100
    defobj(spsr)
        .space 24
    defobj(reg_mode)
        .space 196
    defobj(oam_ram)
        .space 0x400
    defobj(palette_ram)
        .space 0x400
    defobj(io_registers)
        .space 0x400
    defobj(palette_ram_converted)
        .space 0x400

        .section .ext_ram.bss,"aw",@nobits
        .align 6
    defobj(ewram)
        .space 0x80000

        .align 12
    defobj(rom_translation_cache)
        .space ROM_TRANSLATION_CACHE_SIZE
        .align 12
    defobj(ram_translation_cache)
        .space RAM_TRANSLATION_CACHE_SIZE

        .text
        .align 2
