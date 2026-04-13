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

/* Symbol aliases for frequently-used offsets within large arrays.
 * With -fno-pic, `la reg, sym` resolves to auipc+addi (2 instr)
 * instead of auipc+lw (GOT-indirect). These aliases save additional
 * lui+add pairs that were needed to add constant offsets. */
.set iwram_data,  iwram + 0x8000
.set ewram_tags,  ewram + 0x40000
.set ram_tag_table, ram_translation_cache + RAM_TRANSLATION_CACHE_SIZE

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
#define OAM_UPDATED        (25 * 4)
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
    srli s9, tmpreg, 30;                         \
    andi s9, s9, 1;                              \
    srli s10, tmpreg, 29;                        \
    andi s10, s10, 1;                            \
    srli s11, tmpreg, 28;                        \
    andi s11, s11, 1

#define consolidate_flags(tmpreg, tmpreg2)      \
    lw   tmpreg, REG_CPSR(gp);                    \
    slli tmpreg, tmpreg, 4;                       \
    srli tmpreg, tmpreg, 4;                       \
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
 * Interrupt safety for JIT execution (gp / tp repurposed).
 *
 * The JIT uses gp as the ARM register-file base and tp as the cycle counter.
 * ESP-IDF's _interrupt_handler (vectors.S) saves gp/tp on entry and restores
 * them on return.  However it does NOT reload gp to the ABI __global_pointer$
 * before calling C functions (rtos_int_enter, _global_interrupt_handler, etc.).
 *
 * If the linker applied relaxation, many C functions — including ISR handlers
 * like mipi_dsi_bridge_isr_handler, task_wdt_isr, serial_ll_rx_handler — would
 * contain gp-relative load/store instructions (e.g. lw a5, offset(gp)).
 * Running those while gp holds the JIT register base crashes immediately.
 *
 * Solution: the top-level CMakeLists.txt passes -Wl,--no-relax to the linker,
 * which prevents ALL relaxation of auipc+addi → gp-relative addressing.
 * With this flag, no non-JIT code references gp, so it is safe for the JIT
 * to hold non-ABI values in gp/tp while interrupts remain enabled.
 *
 * No csrci/csrsi mstatus bracketing is needed — interrupts stay enabled.
 */
#define jit_disable_interrupts()
#define jit_enable_interrupts()

    .globl _jit_trampoline_start
    .type  _jit_trampoline_start, @function
_jit_trampoline_start:

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

    jit_disable_interrupts()

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
    jit_enable_interrupts()
    beqz t0, 1f
    call block_lookup_address_thumb
    beqz a0, .Lbad_pc_abort
    jit_disable_interrupts()
    lw   gp, 64(sp)
    lw   tp, 60(sp)
    load_registers()
    jalr zero, a0, 0
1:
    call block_lookup_address_arm
    beqz a0, .Lbad_pc_abort
    jit_disable_interrupts()
    lw   gp, 64(sp)
    lw   tp, 60(sp)
    load_registers()
    jalr zero, a0, 0

/* Common abort path: block_lookup returned NULL (bad PC).
 * gp/tp still hold C ABI values here.  Return 0 cycles to main loop. */
.Lbad_pc_abort:
    jit_disable_interrupts()
    li   a0, 0
    j    .Lreturn_to_main

.Lcpu_sleep_loop:
    sw   tp, 60(sp)
    sw   gp, 64(sp)
    lw   gp, 52(sp)
    lw   tp, 56(sp)
    jit_enable_interrupts()
    lw   a0, 60(sp)
    call update_gba
    jit_disable_interrupts()
    lw   gp, 64(sp)
    blt  a0, zero, .Lreturn_cycles_sleep
    /* Clean cycle count: clear flag bits 30-31 (changed_pc, frame_complete) */
    slli tp, a0, 2
    srli tp, tp, 2
    sw   tp, 60(sp)
    lw   t0, CPU_HALT_STATE(gp)
    bnez t0, .Lcpu_sleep_loop
    j    .Llookup_pc

.Lreturn_cycles_sleep:
    /* frame_complete (bit 31) is set — clean cycle count and exit JIT */
    slli tp, a0, 2
    srli tp, tp, 2

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
    jit_enable_interrupts()
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
    jit_enable_interrupts()
    addi sp, sp, FRAME_SIZE
    ret

defsymbl(rv_update_gba)
    sw   ra, REG_SAVE(gp)
    store_registers()
    sw   a0, REG_PC(gp)         /* store branch target PC (don't clobber s7) */
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    jit_enable_interrupts()
    mv   a0, s10
    call update_gba
    blt  a0, zero, .Lreturn_to_main

    /* Clean cycle count: clear bits 30-31 (flags), keep bits 0-29 */
    slli s10, a0, 2
    srli s10, s10, 2

    jit_disable_interrupts()
    mv   gp, s11
    srli t0, a0, 30
    andi t0, t0, 1
    beqz t0, 1f

    lw   a0, REG_PC(gp)
    lw   t0, REG_CPSR(gp)
    andi t0, t0, 0x20
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    jit_enable_interrupts()
    beqz t0, 2f
    call block_lookup_address_thumb
    beqz a0, .Lbad_pc_abort_ugba
    jit_disable_interrupts()
    mv   gp, s11
    mv   tp, s10
    extract_flags(t0)
    load_registers()
    jalr zero, a0, 0
2:
    call block_lookup_address_arm
    beqz a0, .Lbad_pc_abort_ugba
    jit_disable_interrupts()
    mv   gp, s11
    mv   tp, s10
    extract_flags(t0)
    load_registers()
    jalr zero, a0, 0

/* Bad PC during rv_update_gba — return to main via .Lreturn_to_main.
 * s10 holds the saved tp (unused here), s11 holds saved gp (unused). */
.Lbad_pc_abort_ugba:
    jit_disable_interrupts()
    li   a0, 0
    j    .Lreturn_to_main
1:
    mv   tp, s10
    mv   a0, s7                  /* save s7 (stored_pc) before load_registers */
    extract_flags(t0)
    load_registers()
    mv   s7, a0                  /* restore s7 = stored_pc (not reg[REG_PC]) */
    lw   ra, REG_SAVE(gp)
    ret

/*
 * Lightweight helper macros.
 *
 * These save/restore only caller-clobbered ARM registers (a3-a7, t3-t6)
 * and the flag caches (s10/s11) before/after calling C code. Callee-saved
 * registers (s0-s9) are preserved by the C ABI automatically.
 */

#define store_caller_regs()                     \
    sw t3,  REG_SAVE2(gp);                       \
    sw a3,  REG_R2(gp);                          \
    sw a4,  REG_R3(gp);                          \
    sw a5,  REG_R4(gp);                          \
    sw a6,  REG_R5(gp);                          \
    sw a7,  REG_R7(gp);                          \
    sw t4,  REG_R8(gp);                          \
    sw t5,  REG_R11(gp);                         \
    sw t6,  REG_R13(gp)

#define load_caller_regs()                      \
    lw t3,  REG_SAVE2(gp);                       \
    lw a3,  REG_R2(gp);                          \
    lw a4,  REG_R3(gp);                          \
    lw a5,  REG_R4(gp);                          \
    lw a6,  REG_R5(gp);                          \
    lw a7,  REG_R7(gp);                          \
    lw t4,  REG_R8(gp);                          \
    lw t5,  REG_R11(gp);                         \
    lw t6,  REG_R13(gp)

/* Save flag caches (s10=C, s11=V) before we reuse them for gp/tp,
 * then swap to C ABI gp/tp for the call. */
#define swap_to_c_abi()                         \
    sw   s10, REG_C_FLAG(gp);                     \
    sw   s11, REG_V_FLAG(gp);                     \
    mv   s11, gp;                                 \
    mv   s10, tp;                                 \
    lw   gp, REG_SAVE5(s11);                      \
    lw   tp, REG_SAVE6(s11)

/* Restore JIT gp/tp from s10/s11, then restore flag caches. */
#define swap_from_c_abi()                       \
    mv   gp, s11;                                 \
    mv   tp, s10;                                 \
    lw   s10, REG_C_FLAG(gp);                     \
    lw   s11, REG_V_FLAG(gp)

/* ── Indirect branch fast path ─────────────────────────────────────
 *
 * For RAM regions (EWRAM 0x02, IWRAM 0x03) we inline the block lookup
 * tag check so that cache hits avoid the ~30-instruction register
 * save/restore + C call overhead entirely.
 *
 * Scratch registers used: t0, t1, a1 (none are live ARM state here).
 *
 * RAM tag layout:
 *   EWRAM tag mirror: ewram + (pc & 0x3FFFF) + 0x40000
 *   IWRAM tag mirror: iwram + (pc & 0x7FFF)
 *   Tag  = u16 at that address.  VALID_TAG: tag > 0x0101
 *   ramtag_type table sits at ram_translation_cache + RAM_TC_SIZE,
 *   indexed by (s16)tag >> 1.  Each entry is 8 bytes:
 *     +0: offset_arm,  +4: offset_thumb
 *   Translated block = ram_translation_cache + offset.
 */

#define LAST_TAG_NUM  0x0101

/* ---- ARM indirect branch ---- */
defsymbl(rv_indirect_branch_arm)
    andi  a0, a0, -4              /* pc &= ~3 (ARM align) */
    srli  t0, a0, 24              /* t0 = region */
    addi  t1, zero, 2
    beq   t0, t1, .Lfast_ewram_arm
    addi  t1, zero, 3
    beq   t0, t1, .Lfast_iwram_arm

    /* ROM regions: 0x00, 0x08-0x0D */
    beqz  t0, .Lfast_rom_arm
    addi  t1, zero, 8
    blt   t0, t1, .Lslow_arm      /* 0x01-0x07 (excl. 0x02,0x03 already handled) */
    addi  t1, zero, 0x0E
    blt   t0, t1, .Lfast_rom_arm   /* 0x08-0x0D */
    j     .Lslow_arm

.Lfast_rom_arm:
    /* key = a0 (thumb=0 for ARM) */
    li    t0, 0x9E3779B1          /* Knuth multiplicative hash constant */
    mul   t0, a0, t0
    srli  t0, t0, (32 - ROM_BRANCH_HASH_BITS)
    slli  t0, t0, 2              /* * sizeof(u32) */
    la    t1, rom_branch_hash
    add   t0, t0, t1
    lw    t0, 0(t0)              /* blk_offset */
    beqz  t0, .Lslow_arm

    la    t1, rom_translation_cache
    add   a1, t1, t0             /* a1 = &bhdr */
    lw    t0, 0(a1)              /* bhdr->pc_value */
    bne   t0, a0, .Lslow_arm     /* pc mismatch → collision or wrong entry */

    addi  a0, a1, 8              /* skip hashhdr_type (8 bytes) */
    jalr  zero, a0, 0

.Lfast_ewram_arm:
    slli  t1, a0, 14
    srli  t1, t1, 14              /* t1 = pc & 0x3FFFF */
    la    t0, ewram_tags
    j     .Lfast_tagchk_arm

.Lfast_iwram_arm:
    slli  t1, a0, 17
    srli  t1, t1, 17              /* t1 = pc & 0x7FFF */
    la    t0, iwram

.Lfast_tagchk_arm:
    add   t0, t0, t1              /* t0 = tagp */
    lhu   t1, 0(t0)              /* t1 = tag */
    addi  t0, zero, LAST_TAG_NUM
    bgeu  t0, t1, .Lslow_arm     /* tag <= 0x0101 → invalid */

    /* Compute trentry: tbl = ram_tc + RAM_TC_SIZE; &tbl[(s16)tag >> 1] */
    slli  t0, t1, 16
    srai  t0, t0, 17             /* sign-extend to s16 AND >>1 in one step */
    slli  t0, t0, 3              /* * sizeof(ramtag_type) = 8 */
    la    a1, ram_translation_cache
    la    t1, ram_tag_table
    add   t0, t1, t0             /* t0 = &trentry */
    lw    t0, 0(t0)              /* t0 = trentry->offset_arm */
    beqz  t0, .Lslow_arm         /* offset 0 → not translated yet */

    add   a0, a1, t0             /* a0 = translated block address */
    jalr  zero, a0, 0

.Lslow_arm:
    store_caller_regs()
    swap_to_c_abi()
    call  block_lookup_address_arm
    beqz  a0, .Lbad_pc_abort_indirect
    swap_from_c_abi()
    load_caller_regs()
    jalr  zero, a0, 0

/* ---- Thumb indirect branch ---- */
defsymbl(rv_indirect_branch_thumb)
    andi  a0, a0, -2              /* pc &= ~1 (Thumb align) */
    srli  t0, a0, 24
    addi  t1, zero, 2
    beq   t0, t1, .Lfast_ewram_thumb
    addi  t1, zero, 3
    beq   t0, t1, .Lfast_iwram_thumb

    /* ROM regions: 0x00, 0x08-0x0D */
    beqz  t0, .Lfast_rom_thumb
    addi  t1, zero, 8
    blt   t0, t1, .Lslow_thumb
    addi  t1, zero, 0x0E
    blt   t0, t1, .Lfast_rom_thumb
    j     .Lslow_thumb

.Lfast_rom_thumb:
    /* key = pc | 1 (thumb flag) */
    ori   a1, a0, 1               /* a1 = key */
    li    t0, 0x9E3779B1
    mul   t0, a1, t0
    srli  t0, t0, (32 - ROM_BRANCH_HASH_BITS)
    slli  t0, t0, 2
    la    t1, rom_branch_hash
    add   t0, t0, t1
    lw    t0, 0(t0)
    beqz  t0, .Lslow_thumb

    la    t1, rom_translation_cache
    add   a1, t1, t0
    lw    t0, 0(a1)              /* bhdr->pc_value */
    ori   t1, a0, 1              /* rebuild key for comparison */
    bne   t0, t1, .Lslow_thumb

    addi  a0, a1, 8
    jalr  zero, a0, 0

.Lfast_ewram_thumb:
    slli  t1, a0, 14
    srli  t1, t1, 14
    la    t0, ewram_tags
    j     .Lfast_tagchk_thumb

.Lfast_iwram_thumb:
    slli  t1, a0, 17
    srli  t1, t1, 17
    la    t0, iwram

.Lfast_tagchk_thumb:
    add   t0, t0, t1
    lhu   t1, 0(t0)
    addi  t0, zero, LAST_TAG_NUM
    bgeu  t0, t1, .Lslow_thumb

    slli  t0, t1, 16
    srai  t0, t0, 17             /* sign-extend to s16 AND >>1 in one step */
    slli  t0, t0, 3
    la    a1, ram_translation_cache
    la    t1, ram_tag_table
    add   t0, t1, t0
    lw    t0, 4(t0)              /* trentry->offset_thumb (at +4) */
    beqz  t0, .Lslow_thumb

    add   a0, a1, t0
    jalr  zero, a0, 0

.Lslow_thumb:
    store_caller_regs()
    swap_to_c_abi()
    call  block_lookup_address_thumb
    beqz  a0, .Lbad_pc_abort_indirect
    swap_from_c_abi()
    load_caller_regs()
    jalr  zero, a0, 0

/* Bad PC during indirect branch slow path.
 * C ABI gp/tp are active (swap_to_c_abi done), s11=jit_gp, s10=jit_tp. */
.Lbad_pc_abort_indirect:
    /* Restore JIT gp/tp, store regs, and exit with 0 cycles */
    mv   gp, s11
    mv   tp, s10
    store_registers()
    consolidate_flags(t0, t1)
    li   a0, 0
    j    .Lexit_jit

/* ---- Dual (ARM/Thumb) indirect branch ---- */
defsymbl(rv_indirect_branch_dual)
    andi  t0, a0, 1              /* t0 = thumb bit */
    bnez  t0, .Ldual_thumb

    /* ARM path: bit 0 already 0, pc = (pc + 2) & ~3, clear CPSR Thumb bit */
    addi  a0, a0, 2
    andi  a0, a0, -4
    lw    t0, REG_CPSR(gp)
    andi  t0, t0, -33            /* clear bit 5 (0x20) */
    sw    t0, REG_CPSR(gp)

    /* Inline RAM/ROM lookup for ARM */
    srli  t0, a0, 24
    addi  t1, zero, 2
    beq   t0, t1, .Lfast_ewram_arm
    addi  t1, zero, 3
    beq   t0, t1, .Lfast_iwram_arm
    beqz  t0, .Lfast_rom_arm
    addi  t1, zero, 8
    blt   t0, t1, .Lslow_arm
    addi  t1, zero, 0x0E
    blt   t0, t1, .Lfast_rom_arm
    j     .Lslow_arm

.Ldual_thumb:
    andi  a0, a0, -2             /* clear bit 0 for Thumb alignment */
    /* Thumb path: set CPSR Thumb bit */
    lw    t0, REG_CPSR(gp)
    ori   t0, t0, 0x20
    sw    t0, REG_CPSR(gp)

    /* Inline RAM/ROM lookup for Thumb */
    srli  t0, a0, 24
    addi  t1, zero, 2
    beq   t0, t1, .Lfast_ewram_thumb
    addi  t1, zero, 3
    beq   t0, t1, .Lfast_iwram_thumb
    beqz  t0, .Lfast_rom_thumb
    addi  t1, zero, 8
    blt   t0, t1, .Lslow_thumb
    addi  t1, zero, 0x0E
    blt   t0, t1, .Lfast_rom_thumb
    j     .Lslow_thumb

/* ── Memory load fast-path macros ──────────────────────────────────
 *
 * For common memory regions (EWRAM, IWRAM, ROM via memory_map_read),
 * we can read directly from the memory buffer in assembly, bypassing
 * the full C call and its ~35-instruction register save/restore overhead.
 *
 * Fast path uses ONLY t0, t1 as scratch — all ARM-mapped registers
 * (a3-a7, t3-t6, s0-s7) and flag caches (s8-s11) are untouched.
 *
 * Regions handled:
 *   0x02: EWRAM  — ewram[addr & 0x3FFFF]
 *   0x03: IWRAM  — iwram[(addr & 0x7FFF) + 0x8000]  (shadow copy)
 *   0x08-0x0C: ROM — memory_map_read[addr >> 15][addr & 0x7FFF]
 *
 * All other regions fall through to the slow C path.
 */

/* Fast-path I/O registers handler: io_registers[addr & 0x3FF] */
.macro load_fast_ioreg ldop, suffix
.Lioreg_\suffix:
    la    t0, io_registers
    andi  t1, a0, 0x3FF
    add   t0, t0, t1
    \ldop a0, 0(t0)
    ret
.endm

/* Fast-path Palette RAM handler: palette_ram[addr & 0x3FF] */
.macro load_fast_palram ldop, suffix
.Lpalram_\suffix:
    la    t0, palette_ram
    andi  t1, a0, 0x3FF
    add   t0, t0, t1
    \ldop a0, 0(t0)
    ret
.endm

/* Fast-path VRAM handler: vram[addr & 0x1FFFF] with 0x18000 mirror */
.macro load_fast_vram ldop, suffix
.Lvram_\suffix:
    la    t0, vram
    slli  t1, a0, 15
    srli  t1, t1, 15            /* t1 = addr & 0x1FFFF */
    li    a0, 0x18000
    bltu  t1, a0, 1f
    lui   a0, 8                 /* a0 = 0x8000 */
    sub   t1, t1, a0
1:
    add   t0, t0, t1
    \ldop a0, 0(t0)
    ret
.endm

/* Fast-path OAM RAM handler: oam_ram[addr & 0x3FF] */
.macro load_fast_oamram ldop, suffix
.Loamram_\suffix:
    la    t0, oam_ram
    andi  t1, a0, 0x3FF
    add   t0, t0, t1
    \ldop a0, 0(t0)
    ret
.endm

/* Fast-path EWRAM handler: ewram[addr & 0x3FFFF] */
.macro load_fast_ewram ldop, suffix
.Lewram_\suffix:
    la    t0, ewram
    slli  t1, a0, 14
    srli  t1, t1, 14            /* t1 = addr & 0x3FFFF */
    add   t0, t0, t1
    \ldop a0, 0(t0)
    ret
.endm

/* Fast-path IWRAM handler: iwram[(addr & 0x7FFF) + 0x8000] */
.macro load_fast_iwram ldop, suffix
.Liwram_\suffix:
    la    t0, iwram_data
    slli  t1, a0, 17
    srli  t1, t1, 17            /* t1 = addr & 0x7FFF */
    add   t0, t0, t1
    \ldop a0, 0(t0)
    ret
.endm

/* Fast-path ROM handler: memory_map_read[addr >> 15][addr & 0x7FFF] */
.macro load_fast_rom ldop, suffix
.Lrom_\suffix:
    srli  t1, a0, 15            /* page index */
    slli  t1, t1, 2             /* * sizeof(u8*) */
    la    t0, memory_map_read
    add   t0, t0, t1
    lw    t0, 0(t0)             /* page pointer */
    beqz  t0, .Lslow_\suffix    /* unmapped → slow path */
    slli  t1, a0, 17
    srli  t1, t1, 17            /* t1 = addr & 0x7FFF */
    add   t0, t0, t1
    \ldop a0, 0(t0)
    ret
.endm

/* Region dispatch: branches to fast paths for known regions.
 * Order: EWRAM/IWRAM first (most common RAM targets), then ROM
 * (most common data source), then remaining regions. */
.macro load_region_dispatch suffix
    srli  t0, a0, 24
    addi  t1, t0, -2
    beqz  t1, .Lewram_\suffix
    addi  t1, t0, -3
    beqz  t1, .Liwram_\suffix
    addi  t1, t0, -8
    sltiu t1, t1, 7
    bnez  t1, .Lrom_\suffix
    addi  t1, t0, -4
    beqz  t1, .Lioreg_\suffix
    addi  t1, t0, -5
    beqz  t1, .Lpalram_\suffix
    addi  t1, t0, -6
    beqz  t1, .Lvram_\suffix
    addi  t1, t0, -7
    beqz  t1, .Loamram_\suffix
.endm

/* Slow-path: full C call with register save/restore */
.macro load_slow_path cfunc, suffix
.Lslow_\suffix:
    sw   ra, REG_SAVE(gp)
    sw   s7, REG_PC(gp)         /* use stored_pc (s7) — saves 2 JIT instr per load */
    store_caller_regs()
    swap_to_c_abi()
    call \cfunc
    mv   t0, a0
    swap_from_c_abi()
    load_caller_regs()
    lw   ra, REG_SAVE(gp)
    mv   a0, t0
    ret
.endm

/* ── Load trampolines with fast paths ─────────────────────────── */

defsymbl(rv_execute_load_u8)
    load_region_dispatch u8
    load_slow_path execute_load_u8, u8
    load_fast_ewram lbu, u8
    load_fast_iwram lbu, u8
    load_fast_ioreg lbu, u8
    load_fast_palram lbu, u8
    load_fast_vram lbu, u8
    load_fast_oamram lbu, u8
    load_fast_rom   lbu, u8

defsymbl(rv_execute_load_s8)
    load_region_dispatch s8
    load_slow_path execute_load_s8, s8
    load_fast_ewram lb, s8
    load_fast_iwram lb, s8
    load_fast_ioreg lb, s8
    load_fast_palram lb, s8
    load_fast_vram lb, s8
    load_fast_oamram lb, s8
    load_fast_rom   lb, s8

defsymbl(rv_execute_load_u16)
    andi  t1, a0, 1             /* unaligned? → slow path (needs rotation) */
    bnez  t1, .Lslow_u16
    load_region_dispatch u16
    load_slow_path execute_load_u16, u16
    load_fast_ewram lhu, u16
    load_fast_iwram lhu, u16
    load_fast_ioreg lhu, u16
    load_fast_palram lhu, u16
    load_fast_vram lhu, u16
    load_fast_oamram lhu, u16
    load_fast_rom   lhu, u16

defsymbl(rv_execute_load_s16)
    andi  t1, a0, 1             /* unaligned? → slow path (byte load + sign-ext) */
    bnez  t1, .Lslow_s16
    load_region_dispatch s16
    load_slow_path execute_load_s16, s16
    load_fast_ewram lh, s16
    load_fast_iwram lh, s16
    load_fast_ioreg lh, s16
    load_fast_palram lh, s16
    load_fast_vram lh, s16
    load_fast_oamram lh, s16
    load_fast_rom   lh, s16

defsymbl(rv_execute_load_u32)
    andi  t1, a0, 3             /* unaligned? → slow path (needs rotation) */
    bnez  t1, .Lslow_u32
    load_region_dispatch u32
    load_slow_path execute_load_u32, u32
    load_fast_ewram lw, u32
    load_fast_iwram lw, u32
    load_fast_ioreg lw, u32
    load_fast_palram lw, u32
    load_fast_vram lw, u32
    load_fast_oamram lw, u32
    load_fast_rom   lw, u32

defsymbl(rv_execute_aligned_load32)
    load_region_dispatch al32
    load_slow_path execute_aligned_load32, al32
    load_fast_ewram lw, al32
    load_fast_iwram lw, al32
    load_fast_ioreg lw, al32
    load_fast_palram lw, al32
    load_fast_vram lw, al32
    load_fast_oamram lw, al32
    load_fast_rom   lw, al32

/* ── Memory store fast-path macros ─────────────────────────────────
 *
 * For EWRAM and IWRAM stores, we can write directly in assembly with
 * inline SMC (self-modifying code) detection. Only t0, t1 used as scratch.
 *
 * SMC detection:
 *   EWRAM: sticky bits at ewram[offset + 0x40000] (second 256KB)
 *   IWRAM: sticky bits at iwram[offset] (first 32KB)
 *   If sticky bit is non-zero → fall to slow C path for full alert handling.
 *
 * aligned_store32 (STM): NO SMC check (matches C code behavior).
 */

/* Store EWRAM with SMC check */
.macro store_fast_ewram stop, smc_ldop, suffix
.Lst_ewram_\suffix:
    la    t0, ewram
    slli  t1, a0, 14
    srli  t1, t1, 14            /* t1 = addr & 0x3FFFF */
    add   t0, t0, t1            /* t0 = &ewram[offset] */
    lui   t1, 0x40              /* t1 = 0x40000 */
    add   t1, t0, t1            /* t1 = &ewram[offset + 0x40000] */
    \smc_ldop t1, 0(t1)         /* load sticky bits */
    bnez  t1, .Lslow_\suffix
    \stop a1, 0(t0)
    ret
.endm

/* Store IWRAM with SMC check */
.macro store_fast_iwram stop, smc_ldop, suffix
.Lst_iwram_\suffix:
    la    t0, iwram
    slli  t1, a0, 17
    srli  t1, t1, 17            /* t1 = addr & 0x7FFF */
    add   t0, t0, t1            /* t0 = &iwram[offset] (sticky region) */
    \smc_ldop t1, 0(t0)         /* load sticky bits */
    bnez  t1, .Lslow_\suffix
    lui   t1, 8                 /* t1 = 0x8000 */
    add   t0, t0, t1            /* t0 = &iwram[offset + 0x8000] (shadow) */
    \stop a1, 0(t0)
    ret
.endm

/* Store EWRAM WITHOUT SMC check (for aligned_store32) */
.macro store_fast_ewram_nosmc stop, suffix
.Lst_ewram_\suffix:
    la    t0, ewram
    slli  t1, a0, 14
    srli  t1, t1, 14
    add   t0, t0, t1
    \stop a1, 0(t0)
    ret
.endm

/* Store IWRAM WITHOUT SMC check (for aligned_store32) */
.macro store_fast_iwram_nosmc stop, suffix
.Lst_iwram_\suffix:
    la    t0, iwram_data
    slli  t1, a0, 17
    srli  t1, t1, 17
    add   t0, t0, t1
    \stop a1, 0(t0)
    ret
.endm

/* Store VRAM with 0x18000 mirror (u16/u32/aligned32 only) */
.macro store_fast_vram stop, suffix
.Lst_vram_\suffix:
    la    t0, vram
    slli  t1, a0, 15
    srli  t1, t1, 15            /* t1 = addr & 0x1FFFF */
    li    a0, 0x18000
    bltu  t1, a0, 1f
    lui   a0, 8                 /* a0 = 0x8000 */
    sub   t1, t1, a0
1:
    add   t0, t0, t1
    \stop a1, 0(t0)
    ret
.endm

/* Store VRAM u8: byte→halfword duplex mirror
 * GBA spec: byte writes to VRAM duplicate the byte to both halves
 * of a 16-bit unit. addr &= ~1; val16 = (val << 8) | val */
.macro store_fast_vram_u8 suffix
.Lst_vram_\suffix:
    la    t0, vram
    slli  t1, a0, 15
    srli  t1, t1, 15            /* t1 = addr & 0x1FFFF */
    li    a0, 0x18000
    bltu  t1, a0, 1f
    lui   a0, 8
    sub   t1, t1, a0
1:
    andi  t1, t1, -2            /* align to halfword boundary */
    add   t0, t0, t1
    andi  a1, a1, 0xFF          /* ensure single byte */
    slli  a0, a1, 8
    or    a1, a1, a0             /* val16 = (val << 8) | val */
    sh    a1, 0(t0)             /* store as halfword */
    ret
.endm

/* Store OAM (u16/u32/aligned32): write + set OAM_UPDATED flag
 * Note: u8 writes to OAM are no-op per GBA spec → use slow path (returns 0) */
.macro store_fast_oam stop, suffix
.Lst_oam_\suffix:
    la    t0, oam_ram
    andi  t1, a0, 0x3FF
    add   t0, t0, t1
    \stop a1, 0(t0)
    li    t0, 1
    sw    t0, OAM_UPDATED(gp)   /* reg[OAM_UPDATED] = 1 */
    ret
.endm

/* Store region dispatch: EWRAM, IWRAM, VRAM, OAM */
.macro store_region_dispatch suffix
    srli  t0, a0, 24
    addi  t1, t0, -2
    beqz  t1, .Lst_ewram_\suffix
    addi  t1, t0, -3
    beqz  t1, .Lst_iwram_\suffix
    addi  t1, t0, -6
    beqz  t1, .Lst_vram_\suffix
    addi  t1, t0, -7
    beqz  t1, .Lst_oam_\suffix
.endm

/* u8 dispatch: no OAM (byte writes to OAM are no-ops per GBA spec) */
.macro store_region_dispatch_u8 suffix
    srli  t0, a0, 24
    addi  t1, t0, -2
    beqz  t1, .Lst_ewram_\suffix
    addi  t1, t0, -3
    beqz  t1, .Lst_iwram_\suffix
    addi  t1, t0, -6
    beqz  t1, .Lst_vram_\suffix
.endm

/* Store slow-path: full C call with alert check */
.macro store_slow_path cfunc, suffix
.Lslow_\suffix:
    sw   ra, REG_SAVE(gp)
    sw   a2, REG_PC(gp)
    store_caller_regs()
    swap_to_c_abi()
    call \cfunc
    bnez a0, .Lstore_alert_slow
    swap_from_c_abi()
    load_caller_regs()
    lw   ra, REG_SAVE(gp)
    ret
.endm

/* ── Store trampolines with fast paths ────────────────────────── */

defsymbl(rv_execute_store_u8)
    store_region_dispatch_u8 st_u8
    store_slow_path execute_store_u8, st_u8
    store_fast_ewram  sb, lbu, st_u8
    store_fast_iwram  sb, lbu, st_u8
    store_fast_vram_u8 st_u8

defsymbl(rv_execute_store_u16)
    andi  t1, a0, 1             /* unaligned? → slow path */
    bnez  t1, .Lslow_st_u16
    store_region_dispatch st_u16
    store_slow_path execute_store_u16, st_u16
    store_fast_ewram  sh, lhu, st_u16
    store_fast_iwram  sh, lhu, st_u16
    store_fast_vram   sh, st_u16
    store_fast_oam    sh, st_u16

defsymbl(rv_execute_store_u32)
    andi  t1, a0, 3             /* unaligned? → slow path */
    bnez  t1, .Lslow_st_u32
    store_region_dispatch st_u32
    store_slow_path execute_store_u32, st_u32
    store_fast_ewram  sw, lw, st_u32
    store_fast_iwram  sw, lw, st_u32
    store_fast_vram   sw, st_u32
    store_fast_oam    sw, st_u32

defsymbl(rv_execute_aligned_store32)
    store_region_dispatch st_al32
    store_slow_path execute_aligned_store32, st_al32
    store_fast_ewram_nosmc  sw, st_al32
    store_fast_iwram_nosmc  sw, st_al32
    store_fast_vram   sw, st_al32
    store_fast_oam    sw, st_al32

/*
 * Store alert slow path: complete the full register+flag save that the
 * lightweight fast path skipped, then fall through to .Lstore_alert.
 *
 * On entry (C ABI context):
 *   a0  = cpu_alert_type (non-zero)
 *   s11 = saved JIT gp (register base)  [from swap_to_c_abi]
 *   s10 = saved JIT tp (cycle count)    [from swap_to_c_abi]
 *   s0-s7 still hold their JIT values (callee-saved through C call)
 *   s8 = N_flag, s9 = Z_flag (callee-saved, still valid)
 *   C_flag/V_flag saved to REG_C_FLAG/REG_V_FLAG by swap_to_c_abi
 *   Caller-clobbered regs already saved to reg[] by store_caller_regs
 *   REG_PC already saved at trampoline entry
 */
.Lstore_alert_slow:
    /* Save callee-saved ARM regs to reg[] (s11 = reg_base) */
    sw   s0, REG_R0(s11)
    sw   s1, REG_R1(s11)
    sw   s2, REG_R6(s11)
    sw   s3, REG_R9(s11)
    sw   s4, REG_R10(s11)
    sw   s5, REG_R12(s11)
    sw   s6, REG_R14(s11)
    /* REG_PC already saved; s7 (next-PC) not needed */

    /* Consolidate flags to CPSR:
     * s8=N, s9=Z are live in regs; C/V in REG_C_FLAG/REG_V_FLAG. */
    lw   t0, REG_CPSR(s11)
    slli t0, t0, 4
    srli t0, t0, 4
    slli t1, s8, 31
    or   t0, t0, t1
    slli t1, s9, 30
    or   t0, t0, t1
    lw   t1, REG_C_FLAG(s11)
    slli t1, t1, 29
    or   t0, t0, t1
    lw   t1, REG_V_FLAG(s11)
    slli t1, t1, 28
    or   t0, t0, t1
    sw   t0, REG_CPSR(s11)

    /* Fall through to existing store alert handler */

/*
 * Store alert handler (shared by all rv_execute_store_* stubs).
 *
 * On entry (C ABI context, interrupts enabled):
 *   a0  = cpu_alert_type (non-zero, bitmask of HALT/SMC/IRQ)
 *   s11 = saved JIT gp (register base)
 *   s10 = saved JIT tp (cycle count)
 *   gp/tp = C ABI values (restored from REG_SAVE5/6)
 *   Registers and flags already saved to reg[] / CPSR
 *
 * Mirrors x86 write_epilogue behaviour:
 *   1. CPU_ALERT_SMC  (bit 1) → flush_translation_cache_ram
 *   2. CPU_ALERT_IRQ  (bit 2) → check_and_raise_interrupts
 *   3. CPU_ALERT_HALT (bit 0) → update_gba (sleep loop)
 *   Then re-enter translation via .Llookup_pc / .Lcpu_sleep_loop.
 */
.Lstore_alert:
    /* a0 = alert type, s11 = JIT gp, s10 = JIT tp (cycles) */
    /* Call C helper: rv_handle_store_alert(alert, cycles) */
    mv   a1, s10                /* arg1 = remaining cycles */
    call rv_handle_store_alert
    /* Return value: updated cycle count after alert handling */
    blt  a0, zero, .Lreturn_cycles_sleep

    /* SMC may have flushed the translation cache.
     * Force a block re-lookup by jumping to .Llookup_pc.
     * Set the new cycle count and restore JIT context first. */
    slli s10, a0, 2
    srli s10, s10, 2

.Lstore_alert_done:
    jit_disable_interrupts()
    mv   gp, s11
    mv   tp, s10
    sw   tp, 60(sp)
    sw   gp, 64(sp)
    lw   t0, CPU_HALT_STATE(gp)
    bnez t0, .Lcpu_sleep_loop
    j    .Llookup_pc

defsymbl(rv_cheat_hook)
    sw   ra, REG_SAVE(gp)
    store_registers()
    consolidate_flags(t0, t1)
    mv   s11, gp
    mv   s10, tp
    lw   gp, REG_SAVE5(s11)
    lw   tp, REG_SAVE6(s11)
    jit_enable_interrupts()
    call process_cheats
    jit_disable_interrupts()
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
    jit_enable_interrupts()
    mv   a2, t1
    call execute_store_cpsr_body
    beqz a0, 4f
    call block_lookup_address_arm
    beqz a0, .Lbad_pc_abort_cpsr
    jit_disable_interrupts()
    mv   gp, s11
    mv   tp, s10
    extract_flags(t2)
    load_registers()
    lw   ra, REG_SAVE3(gp)
    jalr zero, a0, 0

.Lbad_pc_abort_cpsr:
    jit_disable_interrupts()
    li   a0, 0
    j    .Lreturn_to_main
4:
    jit_disable_interrupts()
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
    jit_enable_interrupts()
    call execute_spsr_restore_body
    jit_disable_interrupts()
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
    jit_enable_interrupts()
    li   a0, MODE_SUPERVISOR
    call set_cpu_mode
    jit_disable_interrupts()
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
    jit_enable_interrupts()
    mv   t0, a0
    mv   a0, a1
    mv   a1, a2
    mv   a2, a3
    jalr ra, t0, 0
    jit_disable_interrupts()
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
    jit_enable_interrupts()
    call trace_jit_instruction
    jit_disable_interrupts()
    mv   gp, s11
    mv   tp, s10
    extract_flags(t1)
    load_registers()
    lw   ra, REG_SAVE(gp)
    ret
#endif

    .globl _jit_trampoline_end
_jit_trampoline_end:

        .bss
        .align 6

#ifndef JIT_CACHE_PSRAM
    /* Place JIT caches early in .bss so they're within JAL range (±1MB)
       of .text trampolines, enabling 1-instr JAL instead of 2-instr
       AUIPC+JALR for generate_function_call. */
        .align 12
    defobj(rom_translation_cache)
        .space ROM_TRANSLATION_CACHE_SIZE
        .align 12
    defobj(ram_translation_cache)
        .space RAM_TRANSLATION_CACHE_SIZE
        .align 6
#endif

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
#ifdef JIT_CACHE_PSRAM
        .section .ext_ram.bss,"aw",@nobits
#endif
        .align 6
    defobj(ewram)
        .space 0x80000
#ifdef JIT_TRAMPOLINE_PSRAM
        .align 6
    defobj(trampoline_psram_buf)
        .space 8192
#endif
#ifdef JIT_CACHE_PSRAM
        .align 12
    defobj(rom_translation_cache)
        .space ROM_TRANSLATION_CACHE_SIZE
        .align 12
    defobj(ram_translation_cache)
        .space RAM_TRANSLATION_CACHE_SIZE
#endif

        .text
        .align 2
