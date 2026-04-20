/*
 * gpsp RISC-V (RV32IMAC) Instruction Encoding Macros
 *
 * This file provides low-level instruction encoding for generating RISC-V
 * machine code at runtime (JIT/Dynamic Recompilation).
 *
 * Reference: RISC-V Unprivileged ISA Specification v20191213
 *
 * Instruction formats:
 *   R-type:  funct7[31:25] | rs2[24:20] | rs1[19:15] | funct3[14:12] | rd[11:7] | opcode[6:0]
 *   I-type:  imm[31:20]   | rs1[19:15] | funct3[14:12] | rd[11:7] | opcode[6:0]
 *   S-type:  imm[31:25]   | rs2[24:20] | rs1[19:15] | funct3[14:12] | imm[11:7] | opcode[6:0]
 *   B-type:  imm[12|10:5] | rs2[24:20] | rs1[19:15] | funct3[14:12] | imm[4:1|11] | opcode[6:0]
 *   U-type:  imm[31:12]   | rd[11:7]   | opcode[6:0]
 *   J-type:  imm[20|10:1|11|19:12] | rd[11:7] | opcode[6:0]
 */

#ifndef RISCV_CODEGEN_H
#define RISCV_CODEGEN_H

#include "../common.h"

/* ---- RISC-V register names ---- */
#define rv_zero  0
#define rv_ra    1
#define rv_sp    2
#define rv_gp    3
#define rv_tp    4
#define rv_t0    5
#define rv_t1    6
#define rv_t2    7
#define rv_s0    8
#define rv_fp    8   /* s0 is also frame pointer */
#define rv_s1    9
#define rv_a0    10
#define rv_a1    11
#define rv_a2    12
#define rv_a3    13
#define rv_a4    14
#define rv_a5    15
#define rv_a6    16
#define rv_a7    17
#define rv_s2    18
#define rv_s3    19
#define rv_s4    20
#define rv_s5    21
#define rv_s6    22
#define rv_s7    23
#define rv_s8    24
#define rv_s9    25
#define rv_s10   26
#define rv_s11   27
#define rv_t3    28
#define rv_t4    29
#define rv_t5    30
#define rv_t6    31

/* ---- Opcode fields ---- */
#define RV_OP_LUI      0x37
#define RV_OP_AUIPC    0x17
#define RV_OP_JAL      0x6F
#define RV_OP_JALR     0x67
#define RV_OP_BRANCH   0x63
#define RV_OP_LOAD     0x03
#define RV_OP_STORE    0x23
#define RV_OP_IMM      0x13
#define RV_OP_REG      0x33
#define RV_OP_FENCE    0x0F
#define RV_OP_SYSTEM   0x73

/* ---- funct3 for branch ---- */
#define RV_BEQ   0x0
#define RV_BNE   0x1
#define RV_BLT   0x4
#define RV_BGE   0x5
#define RV_BLTU  0x6
#define RV_BGEU  0x7

/* ---- funct3 for load ---- */
#define RV_LB    0x0
#define RV_LH    0x1
#define RV_LW    0x2
#define RV_LBU   0x4
#define RV_LHU   0x5

/* ---- funct3 for store ---- */
#define RV_SB    0x0
#define RV_SH    0x1
#define RV_SW    0x2

/* ---- funct3 for ALU immediate ---- */
#define RV_ADDI  0x0
#define RV_SLTI  0x2
#define RV_SLTIU 0x3
#define RV_XORI  0x4
#define RV_ORI   0x6
#define RV_ANDI  0x7
#define RV_SLLI  0x1
#define RV_SRLI  0x5   /* also SRAI with funct7=0x20 */

/* ---- funct3/funct7 for ALU register ---- */
#define RV_ADD   0x0   /* funct7=0x00 */
#define RV_SUB   0x0   /* funct7=0x20 */
#define RV_SLL   0x1
#define RV_SLT   0x2
#define RV_SLTU  0x3
#define RV_XOR   0x4
#define RV_SRL   0x5   /* funct7=0x00 */
#define RV_SRA   0x5   /* funct7=0x20 */
#define RV_OR    0x6
#define RV_AND   0x7

/* ---- M extension funct3 (funct7 = 0x01) ---- */
#define RV_MUL    0x0
#define RV_MULH   0x1
#define RV_MULHSU 0x2
#define RV_MULHU  0x3
#define RV_DIV    0x4
#define RV_DIVU   0x5
#define RV_REM    0x6
#define RV_REMU   0x7

/* ========================================================
 * Instruction emission macros
 * All macros emit a 32-bit instruction at *translation_ptr
 * and advance translation_ptr by 4 bytes.
 * ======================================================== */

#define rv_emit(inst) do {                  \
    *((u32 *)translation_ptr) = (inst);     \
    translation_ptr += 4;                   \
} while(0)

/* ---- R-type ---- */
#define rv_r_type(funct7, rs2, rs1, funct3, rd, opcode) \
    rv_emit( ((funct7) << 25) | ((rs2) << 20) | ((rs1) << 15) | \
             ((funct3) << 12) | ((rd) << 7) | (opcode) )

/* ---- I-type ---- */
#define rv_i_type(imm12, rs1, funct3, rd, opcode) \
    rv_emit( (((imm12) & 0xFFF) << 20) | ((rs1) << 15) | \
             ((funct3) << 12) | ((rd) << 7) | (opcode) )

/* ---- S-type ---- */
#define rv_s_type(imm12, rs2, rs1, funct3, opcode) \
    rv_emit( ((((imm12) >> 5) & 0x7F) << 25) | ((rs2) << 20) | ((rs1) << 15) | \
             ((funct3) << 12) | (((imm12) & 0x1F) << 7) | (opcode) )

/* ---- B-type ---- */
#define rv_b_type(imm13, rs2, rs1, funct3, opcode) \
    rv_emit( ((((imm13) >> 12) & 0x1) << 31) | ((((imm13) >> 5) & 0x3F) << 25) | \
             ((rs2) << 20) | ((rs1) << 15) | ((funct3) << 12) | \
             ((((imm13) >> 1) & 0xF) << 8) | ((((imm13) >> 11) & 0x1) << 7) | (opcode) )

/* ---- U-type ---- */
#define rv_u_type(imm20, rd, opcode) \
    rv_emit( ((imm20) & 0xFFFFF000) | ((rd) << 7) | (opcode) )

/* ---- J-type ---- */
#define rv_j_type(imm21, rd, opcode) \
    rv_emit( ((((imm21) >> 20) & 0x1) << 31) | ((((imm21) >> 1) & 0x3FF) << 21) | \
             ((((imm21) >> 11) & 0x1) << 20) | ((((imm21) >> 12) & 0xFF) << 12) | \
             ((rd) << 7) | (opcode) )

/* ========================================================
 * High-level instruction macros
 * ======================================================== */

/* Arithmetic */
#define rv_add(rd, rs1, rs2)     rv_r_type(0x00, rs2, rs1, RV_ADD, rd, RV_OP_REG)
#define rv_sub(rd, rs1, rs2)     rv_r_type(0x20, rs2, rs1, RV_SUB, rd, RV_OP_REG)
#define rv_addi(rd, rs1, imm)    rv_i_type(imm, rs1, RV_ADDI, rd, RV_OP_IMM)
#define rv_and(rd, rs1, rs2)     rv_r_type(0x00, rs2, rs1, RV_AND, rd, RV_OP_REG)
#define rv_or(rd, rs1, rs2)      rv_r_type(0x00, rs2, rs1, RV_OR,  rd, RV_OP_REG)
#define rv_xor(rd, rs1, rs2)     rv_r_type(0x00, rs2, rs1, RV_XOR, rd, RV_OP_REG)
#define rv_andi(rd, rs1, imm)    rv_i_type(imm, rs1, RV_ANDI, rd, RV_OP_IMM)
#define rv_ori(rd, rs1, imm)     rv_i_type(imm, rs1, RV_ORI,  rd, RV_OP_IMM)
#define rv_xori(rd, rs1, imm)    rv_i_type(imm, rs1, RV_XORI, rd, RV_OP_IMM)

/* Shifts */
#define rv_sll(rd, rs1, rs2)     rv_r_type(0x00, rs2, rs1, RV_SLL, rd, RV_OP_REG)
#define rv_srl(rd, rs1, rs2)     rv_r_type(0x00, rs2, rs1, RV_SRL, rd, RV_OP_REG)
#define rv_sra(rd, rs1, rs2)     rv_r_type(0x20, rs2, rs1, RV_SRA, rd, RV_OP_REG)
#define rv_slli(rd, rs1, shamt)  rv_i_type(shamt, rs1, RV_SLLI, rd, RV_OP_IMM)
#define rv_srli(rd, rs1, shamt)  rv_i_type(shamt, rs1, RV_SRLI, rd, RV_OP_IMM)
#define rv_srai(rd, rs1, shamt)  rv_i_type((shamt) | 0x400, rs1, RV_SRLI, rd, RV_OP_IMM)

/* Compare */
#define rv_slt(rd, rs1, rs2)     rv_r_type(0x00, rs2, rs1, RV_SLT,  rd, RV_OP_REG)
#define rv_sltu(rd, rs1, rs2)    rv_r_type(0x00, rs2, rs1, RV_SLTU, rd, RV_OP_REG)
#define rv_slti(rd, rs1, imm)    rv_i_type(imm, rs1, RV_SLTI,  rd, RV_OP_IMM)
#define rv_sltiu(rd, rs1, imm)   rv_i_type(imm, rs1, RV_SLTIU, rd, RV_OP_IMM)

/* Load/Store */
#define rv_lw(rd, rs1, offset)   rv_i_type(offset, rs1, RV_LW,  rd, RV_OP_LOAD)
#define rv_lh(rd, rs1, offset)   rv_i_type(offset, rs1, RV_LH,  rd, RV_OP_LOAD)
#define rv_lhu(rd, rs1, offset)  rv_i_type(offset, rs1, RV_LHU, rd, RV_OP_LOAD)
#define rv_lb(rd, rs1, offset)   rv_i_type(offset, rs1, RV_LB,  rd, RV_OP_LOAD)
#define rv_lbu(rd, rs1, offset)  rv_i_type(offset, rs1, RV_LBU, rd, RV_OP_LOAD)
#define rv_sw(rs2, rs1, offset)  rv_s_type(offset, rs2, rs1, RV_SW, RV_OP_STORE)
#define rv_sh(rs2, rs1, offset)  rv_s_type(offset, rs2, rs1, RV_SH, RV_OP_STORE)
#define rv_sb(rs2, rs1, offset)  rv_s_type(offset, rs2, rs1, RV_SB, RV_OP_STORE)

/* Branches */
#define rv_beq(rs1, rs2, offset)  rv_b_type(offset, rs2, rs1, RV_BEQ,  RV_OP_BRANCH)
#define rv_bne(rs1, rs2, offset)  rv_b_type(offset, rs2, rs1, RV_BNE,  RV_OP_BRANCH)
#define rv_blt(rs1, rs2, offset)  rv_b_type(offset, rs2, rs1, RV_BLT,  RV_OP_BRANCH)
#define rv_bge(rs1, rs2, offset)  rv_b_type(offset, rs2, rs1, RV_BGE,  RV_OP_BRANCH)
#define rv_bltu(rs1, rs2, offset) rv_b_type(offset, rs2, rs1, RV_BLTU, RV_OP_BRANCH)
#define rv_bgeu(rs1, rs2, offset) rv_b_type(offset, rs2, rs1, RV_BGEU, RV_OP_BRANCH)

/* Jumps */
#define rv_jal(rd, offset)       rv_j_type(offset, rd, RV_OP_JAL)
#define rv_jalr(rd, rs1, offset) rv_i_type(offset, rs1, 0x0, rd, RV_OP_JALR)
#define rv_j(offset)             rv_jal(rv_zero, offset)
#define rv_call(offset)          rv_jal(rv_ra, offset)
#define rv_ret()                 rv_jalr(rv_zero, rv_ra, 0)

/* Upper immediate */
#define rv_lui(rd, imm)          rv_u_type(imm, rd, RV_OP_LUI)
#define rv_auipc(rd, imm)        rv_u_type(imm, rd, RV_OP_AUIPC)

/* M extension */
#define rv_mul(rd, rs1, rs2)     rv_r_type(0x01, rs2, rs1, RV_MUL,    rd, RV_OP_REG)
#define rv_mulh(rd, rs1, rs2)    rv_r_type(0x01, rs2, rs1, RV_MULH,   rd, RV_OP_REG)
#define rv_mulhu(rd, rs1, rs2)   rv_r_type(0x01, rs2, rs1, RV_MULHU,  rd, RV_OP_REG)
#define rv_div(rd, rs1, rs2)     rv_r_type(0x01, rs2, rs1, RV_DIV,    rd, RV_OP_REG)
#define rv_divu(rd, rs1, rs2)    rv_r_type(0x01, rs2, rs1, RV_DIVU,   rd, RV_OP_REG)
#define rv_rem(rd, rs1, rs2)     rv_r_type(0x01, rs2, rs1, RV_REM,    rd, RV_OP_REG)
#define rv_remu(rd, rs1, rs2)    rv_r_type(0x01, rs2, rs1, RV_REMU,   rd, RV_OP_REG)

/* Pseudo-instructions */
#define rv_nop()                 rv_addi(rv_zero, rv_zero, 0)
#define rv_mv(rd, rs)            rv_addi(rd, rs, 0)
#define rv_not(rd, rs)           rv_xori(rd, rs, -1)
#define rv_neg(rd, rs)           rv_sub(rd, rv_zero, rs)
#define rv_li(rd, imm)           rv_addi(rd, rv_zero, imm)
#define rv_seqz(rd, rs)          rv_sltiu(rd, rs, 1)
#define rv_snez(rd, rs)          rv_sltu(rd, rv_zero, rs)
#define rv_beqz(rs, offset)      rv_beq(rs, rv_zero, offset)
#define rv_bnez(rs, offset)      rv_bne(rs, rv_zero, offset)

/* Explicit 32-bit variants (unaffected by HAVE_RVC compression) */
#define rv_nop_32()       rv_i_type(0, rv_zero, RV_ADDI, rv_zero, RV_OP_IMM)
#define rv_mv_32(rd, rs)  rv_i_type(0, rs, RV_ADDI, rd, RV_OP_IMM)

/* ---- Patch-safe forward-branch slot emission ----
 * These emit an uncompressed 32-bit B-type/J-type placeholder with
 * offset=0, record the slot address, and return a u32* for later use
 * with rv_patch_branch / rv_patch_jal.
 *
 * They go through rv_b_type / rv_j_type directly, bypassing any
 * pseudo-instruction overrides (e.g. HAVE_RVC).  This is the contract
 * that makes patch-slot correctness a property of the emitter, not a
 * runtime assertion: if these helpers are the ONLY way code obtains a
 * patch slot, the slot is guaranteed to be a 32-bit instruction.
 *
 * Usage:
 *     u32 *skip = rv_fwd_branch_slot(RV_BEQ, rs1, rs2);
 *     ... fall-through code ...
 *     rv_patch_branch(skip, translation_ptr);
 */
#define rv_fwd_branch_slot(funct3, rs1, rs2)                                  \
    ({ u32 *_slot = (u32 *)translation_ptr;                                   \
       rv_b_type(0, (rs2), (rs1), (funct3), RV_OP_BRANCH);                    \
       _slot; })

#define rv_fwd_beq_slot(rs1, rs2)  rv_fwd_branch_slot(RV_BEQ,  rs1, rs2)
#define rv_fwd_bne_slot(rs1, rs2)  rv_fwd_branch_slot(RV_BNE,  rs1, rs2)
#define rv_fwd_blt_slot(rs1, rs2)  rv_fwd_branch_slot(RV_BLT,  rs1, rs2)
#define rv_fwd_bge_slot(rs1, rs2)  rv_fwd_branch_slot(RV_BGE,  rs1, rs2)
#define rv_fwd_bltu_slot(rs1, rs2) rv_fwd_branch_slot(RV_BLTU, rs1, rs2)
#define rv_fwd_bgeu_slot(rs1, rs2) rv_fwd_branch_slot(RV_BGEU, rs1, rs2)
#define rv_fwd_beqz_slot(rs)       rv_fwd_branch_slot(RV_BEQ,  rs, rv_zero)
#define rv_fwd_bnez_slot(rs)       rv_fwd_branch_slot(RV_BNE,  rs, rv_zero)

/* Unconditional forward JAL (J pseudo) slot. */
#define rv_fwd_j_slot()                                                       \
    ({ u32 *_slot = (u32 *)translation_ptr;                                   \
       rv_j_type(0, rv_zero, RV_OP_JAL);                                      \
       _slot; })

/* System */
#define rv_ecall()               rv_emit(0x00000073)
#define rv_ebreak()              rv_emit(0x00100073)
#define rv_fence_i()             rv_emit(0x0000100F)
#define rv_fence()               rv_emit(0x0FF0000F)

/* ---- Rotate helpers (synthesized for RV32IMAC without Zbb) ---- */
/* NOTE: slli/sll MUST come before srli/srl so that rd==rs is safe. */

#ifdef HAVE_ZBB
/* Zbb native rotate: single instruction */
/* ROR: funct7=0x30, funct3=0x5, opcode=0x33 */
#define rv_ror_native(rd, rs1, rs2) rv_r_type(0x30, rs2, rs1, 0x5, rd, RV_OP_REG)
/* RORI: funct7=0x30, funct3=0x5, opcode=0x13 (I-type with shamt) */
#define rv_rori_native(rd, rs, shamt) \
    rv_i_type((0x600 | ((shamt) & 0x1F)), rs, 0x5, rd, RV_OP_IMM)
/* ROL: funct7=0x30, funct3=0x1, opcode=0x33 */
#define rv_rol_native(rd, rs1, rs2) rv_r_type(0x30, rs2, rs1, 0x1, rd, RV_OP_REG)

/* SEXT.B: funct7=0x30, rs2=0x04, funct3=0x1, opcode=0x13 */
#define rv_sext_b(rd, rs) rv_i_type(0x604, rs, 0x1, rd, RV_OP_IMM)
/* SEXT.H: funct7=0x30, rs2=0x05, funct3=0x1, opcode=0x13 */
#define rv_sext_h(rd, rs) rv_i_type(0x605, rs, 0x1, rd, RV_OP_IMM)
/* ZEXT.H: funct7=0x04, rs2=0x00, funct3=0x4, opcode=0x33 (pack) */
#define rv_zext_h(rd, rs) rv_r_type(0x04, rv_zero, rs, 0x4, rd, RV_OP_REG)
/* MIN/MAX */
#define rv_min(rd, rs1, rs2)  rv_r_type(0x05, rs2, rs1, 0x4, rd, RV_OP_REG)
#define rv_max(rd, rs1, rs2)  rv_r_type(0x05, rs2, rs1, 0x6, rd, RV_OP_REG)
#define rv_minu(rd, rs1, rs2) rv_r_type(0x05, rs2, rs1, 0x5, rd, RV_OP_REG)
#define rv_maxu(rd, rs1, rs2) rv_r_type(0x05, rs2, rs1, 0x7, rd, RV_OP_REG)
/* ANDN/ORN/XNOR */
#define rv_andn(rd, rs1, rs2) rv_r_type(0x20, rs2, rs1, 0x7, rd, RV_OP_REG)
#define rv_orn(rd, rs1, rs2)  rv_r_type(0x20, rs2, rs1, 0x6, rd, RV_OP_REG)
#define rv_xnor(rd, rs1, rs2) rv_r_type(0x20, rs2, rs1, 0x4, rd, RV_OP_REG)
/* CLZ/CTZ/CPOP */
#define rv_clz(rd, rs)  rv_i_type(0x600, rs, 0x1, rd, RV_OP_IMM)
#define rv_ctz(rd, rs)  rv_i_type(0x601, rs, 0x1, rd, RV_OP_IMM)
#define rv_cpop(rd, rs) rv_i_type(0x602, rs, 0x1, rd, RV_OP_IMM)
/* REV8 (byte-reverse) */
#define rv_rev8(rd, rs) rv_i_type(0x698, rs, 0x5, rd, RV_OP_IMM)
/* ORC.B (or-combine bytes) */
#define rv_orc_b(rd, rs) rv_i_type(0x287, rs, 0x5, rd, RV_OP_IMM)

#define rv_rori(rd, rs, shamt, tmp) do {                   \
    u32 _shamt = (u32)(shamt) & 31;                        \
    if (_shamt == 0) {                                     \
        rv_mv(rd, rs);                                     \
    } else {                                               \
        rv_rori_native(rd, rs, _shamt);                    \
    }                                                      \
} while(0)

#define rv_ror(rd, rs, shreg, tmp, tmp2) do {              \
    rv_ror_native(rd, rs, shreg);                          \
} while(0)

#else /* !HAVE_ZBB */

#define rv_rori(rd, rs, shamt, tmp) do {                   \
    u32 _shamt = (u32)(shamt) & 31;                        \
    if (_shamt == 0) {                                     \
        rv_mv(rd, rs);                                     \
    } else {                                               \
        rv_slli(tmp, rs, 32 - _shamt);                     \
        rv_srli(rd, rs, _shamt);                           \
        rv_or(rd, rd, tmp);                                \
    }                                                      \
} while(0)

#define rv_ror(rd, rs, shreg, tmp, tmp2) do {              \
    rv_andi(tmp, shreg, 31);                               \
    rv_sub(tmp2, rv_zero, tmp);                            \
    rv_andi(tmp2, tmp2, 31);                               \
    rv_sll(tmp2, rs, tmp2);                                \
    rv_srl(rd, rs, tmp);                                   \
    rv_or(rd, rd, tmp2);                                   \
} while(0)

#endif /* HAVE_ZBB */

/* ---- Load 32-bit immediate (variable length: 1 or 2 instructions) ----
 * Zero -> mv (1 inst), small +/-2047 -> addi (1), upper-only -> lui (1),
 * otherwise -> lui+addi (2).
 *
 * Any codegen macro with a hardcoded branch offset that spans across a
 * generate_load_imm() expansion must either inline the load (rv_addi for
 * small constants) or use rv_load_imm32_2inst.  Affected sites:
 *   - shift_reg_{lsl,lsr,asr}_flags  (use rv_addi for constant 32)
 *   - generate_function_call          (uses rv_load_imm32_2inst)
 */
#define rv_load_imm32(rd, imm32) do {                       \
    u32 _val = (u32)(imm32);                                \
    if (_val == 0) {                                        \
        rv_mv(rd, rv_zero);                                 \
    } else if ((s32)_val >= -2048 && (s32)_val <= 2047) {   \
        rv_addi(rd, rv_zero, (s32)_val);                    \
    } else if ((_val & 0xFFF) == 0) {                       \
        rv_lui(rd, _val);                                   \
    } else {                                                \
        u32 _hi = _val & 0xFFFFF000;                        \
        u32 _lo = _val & 0xFFF;                             \
        if (_lo & 0x800) _hi += 0x1000;                     \
        rv_lui(rd, _hi);                                    \
        rv_addi(rd, rd, (s32)(_lo << 20) >> 20);            \
    }                                                       \
} while(0)

/* Always emits exactly 2 instructions (8 bytes). */
#define rv_load_imm32_2inst(rd, imm32) do {                 \
    u32 _val = (u32)(imm32);                                \
    u32 _hi = _val & 0xFFFFF000;                            \
    u32 _lo = _val & 0xFFF;                                 \
    if (_lo & 0x800) _hi += 0x1000;                         \
    rv_lui(rd, _hi);                                        \
    rv_addi(rd, rd, (s32)(_lo << 20) >> 20);                \
} while(0)

/* ======================================================================
 * RVC (Compressed) extension — 16-bit instruction emission
 *
 * When HAVE_RVC is defined, common pseudo-instructions (rv_mv, rv_nop,
 * rv_load_imm32, etc.) auto-select 16-bit compressed forms when the
 * operands qualify, reducing JIT code size and improving I-cache usage.
 *
 * Use rv_nop_32() / rv_mv_32() in patch-sensitive code (branch fillers,
 * fixed-size instruction slots that will be overwritten later).
 *
 * Individual rvc_c_*() macros are always available under HAVE_RVC for
 * explicit use in emitter code.
 * ==================================================================== */
#ifdef HAVE_RVC

/* ---- 16-bit instruction emission ---- */
#define rv_emit16(inst) do {                    \
    *((u16 *)translation_ptr) = (u16)(inst);    \
    translation_ptr += 2;                       \
} while(0)

/* Compressed register helpers: x8-x15 map to 3-bit encoding 0-7 */
#define RVC_CREG_OK(r) ((u32)(r) >= 8 && (u32)(r) <= 15)
#define RVC_CREG(r)    ((u32)(r) - 8)

/* ---- RVC encoding format macros ---- */

/* CR-type: [15:12]=funct4 [11:7]=rd/rs1 [6:2]=rs2 [1:0]=op */
#define rvc_cr(funct4, rd, rs2, op) \
    rv_emit16(((funct4)<<12) | ((rd)<<7) | ((rs2)<<2) | (op))

/* CI-type: [15:13]=funct3 [12]=imm[5] [11:7]=rd [6:2]=imm[4:0] [1:0]=op */
#define rvc_ci(funct3, rd, imm6, op) \
    rv_emit16(((funct3)<<13) | ((((imm6)>>5)&1)<<12) | \
              ((rd)<<7) | (((imm6)&0x1F)<<2) | (op))

/* CSS-type: [15:13]=funct3 [12:7]=imm [6:2]=rs2 [1:0]=op */
#define rvc_css(funct3, rs2, imm6, op) \
    rv_emit16(((funct3)<<13) | (((imm6)&0x3F)<<7) | ((rs2)<<2) | (op))

/* CA-type: [15:10]=funct6 [9:7]=rd'/rs1' [6:5]=funct2 [4:2]=rs2' [1:0]=op */
#define rvc_ca(funct6, rd_p, funct2, rs2_p, op) \
    rv_emit16(((funct6)<<10) | (((rd_p)&7)<<7) | \
              (((funct2)&3)<<5) | (((rs2_p)&7)<<2) | (op))

/* CL/CS-type for C.LW / C.SW:
 * [15:13]=funct3 [12:10]=off[5:3] [9:7]=rs1' [6]=off[2] [5]=off[6]
 * [4:2]=rd'/rs2' [1:0]=op (=00) */
#define rvc_cls_lw(funct3, rs1_p, rd_rs2_p, off7) \
    rv_emit16(((funct3)<<13) | \
              ((((off7)>>3)&7)<<10) | (((rs1_p)&7)<<7) | \
              ((((off7)>>2)&1)<<6) | ((((off7)>>6)&1)<<5) | \
              (((rd_rs2_p)&7)<<2) | 0x0)

/* ---- Quadrant 2 (op = 0b10) ---- */
#define rvc_c_mv(rd, rs2)       rvc_cr(0x8, rd, rs2, 0x2)
#define rvc_c_add(rd, rs2)      rvc_cr(0x9, rd, rs2, 0x2)
#define rvc_c_jr(rs1)           rvc_cr(0x8, rs1, 0, 0x2)
#define rvc_c_jalr(rs1)         rvc_cr(0x9, rs1, 0, 0x2)
#define rvc_c_ebreak()          rvc_cr(0x9, 0, 0, 0x2)
#define rvc_c_slli(rd, shamt)   rvc_ci(0x0, rd, shamt, 0x2)

/* C.LWSP rd, off(sp): off = {off[5], off[4:2], off[7:6]} */
#define rvc_c_lwsp(rd, off) \
    rvc_ci(0x2, rd, \
           ((((off)>>5)&1)<<5) | ((((off)>>2)&7)<<2) | (((off)>>6)&3), 0x2)

/* C.SWSP rs2, off(sp): off = {off[5:2], off[7:6]} */
#define rvc_c_swsp(rs2, off) \
    rvc_css(0x6, rs2, ((((off)>>2)&0xF)<<2) | (((off)>>6)&3), 0x2)

/* ---- Quadrant 1 (op = 0b01) ---- */
#define rvc_c_nop()             rvc_ci(0x0, 0, 0, 0x1)
#define rvc_c_addi(rd, nzimm)   rvc_ci(0x0, rd, nzimm, 0x1)
#define rvc_c_li(rd, imm)       rvc_ci(0x2, rd, imm, 0x1)
#define rvc_c_lui(rd, val)      rvc_ci(0x3, rd, ((u32)(val) >> 12) & 0x3F, 0x1)

/* C.SRLI/SRAI/ANDI (CB-type ALU): funct3=100, [12:10]=sub-func/imm5, op=01
 * rd' must be in x8-x15 (pass compressed reg number 0-7). */
#define rvc_c_srli(rd_p, shamt) \
    rv_emit16((0x4<<13) | ((((shamt)>>5)&1)<<12) | (0x0<<10) | \
              (((rd_p)&7)<<7) | (((shamt)&0x1F)<<2) | 0x1)
#define rvc_c_srai(rd_p, shamt) \
    rv_emit16((0x4<<13) | ((((shamt)>>5)&1)<<12) | (0x1<<10) | \
              (((rd_p)&7)<<7) | (((shamt)&0x1F)<<2) | 0x1)
#define rvc_c_andi(rd_p, imm) \
    rv_emit16((0x4<<13) | ((((imm)>>5)&1)<<12) | (0x2<<10) | \
              (((rd_p)&7)<<7) | (((imm)&0x1F)<<2) | 0x1)

/* CA-type: C.SUB / C.XOR / C.OR / C.AND (both regs in x8-x15) */
#define rvc_c_sub(rd_p, rs2_p)  rvc_ca(0x23, rd_p, 0x0, rs2_p, 0x1)
#define rvc_c_xor(rd_p, rs2_p)  rvc_ca(0x23, rd_p, 0x1, rs2_p, 0x1)
#define rvc_c_or(rd_p, rs2_p)   rvc_ca(0x23, rd_p, 0x2, rs2_p, 0x1)
#define rvc_c_and(rd_p, rs2_p)  rvc_ca(0x23, rd_p, 0x3, rs2_p, 0x1)

/* CL/CS-type: C.LW / C.SW (both regs in x8-x15, off word-aligned 0..124) */
#define rvc_c_lw(rd_p, rs1_p, off)   rvc_cls_lw(0x2, rs1_p, rd_p, off)
#define rvc_c_sw(rs2_p, rs1_p, off)  rvc_cls_lw(0x6, rs1_p, rs2_p, off)

/* CJ-type: C.J / C.JAL (RV32 only)
 * offset range: ±2 KiB, encoding: imm[11|4|9:8|10|6|7|3:1|5] */
#define rvc_c_j(off) \
    rv_emit16((0x5<<13) | \
              ((((off)>>11)&1)<<12) | ((((off)>>4)&1)<<11) | \
              ((((off)>>8)&3)<<9) | ((((off)>>10)&1)<<8) | \
              ((((off)>>6)&1)<<7) | ((((off)>>7)&1)<<6) | \
              ((((off)>>1)&7)<<3) | ((((off)>>5)&1)<<2) | 0x1)
#define rvc_c_jal(off) \
    rv_emit16((0x1<<13) | \
              ((((off)>>11)&1)<<12) | ((((off)>>4)&1)<<11) | \
              ((((off)>>8)&3)<<9) | ((((off)>>10)&1)<<8) | \
              ((((off)>>6)&1)<<7) | ((((off)>>7)&1)<<6) | \
              ((((off)>>1)&7)<<3) | ((((off)>>5)&1)<<2) | 0x1)

/* CB-type: C.BEQZ / C.BNEZ (rs1' in x8-x15, offset ±256)
 * Encoding: imm[8|4:3] at [12:10], imm[7:6|2:1|5] at [6:2] */
#define rvc_c_beqz(rs1_p, off) \
    rv_emit16((0x6<<13) | \
              ((((off)>>8)&1)<<12) | ((((off)>>3)&3)<<10) | \
              (((rs1_p)&7)<<7) | \
              ((((off)>>6)&3)<<5) | ((((off)>>1)&3)<<3) | \
              ((((off)>>5)&1)<<2) | 0x1)
#define rvc_c_bnez(rs1_p, off) \
    rv_emit16((0x7<<13) | \
              ((((off)>>8)&1)<<12) | ((((off)>>3)&3)<<10) | \
              (((rs1_p)&7)<<7) | \
              ((((off)>>6)&3)<<5) | ((((off)>>1)&3)<<3) | \
              ((((off)>>5)&1)<<2) | 0x1)

/* C.JR / C.JALR (full register range) */
/* Already defined above as rvc_c_jr / rvc_c_jalr */

/* ---- Auto-selecting overrides ----
 * Replace common pseudo-instructions with versions that prefer 16-bit
 * compressed forms when operands qualify.  Conditions are constant-folded
 * by the compiler when register numbers are known (which they always are
 * in JIT emitter macros). */

#undef rv_nop
#define rv_nop()  rvc_c_nop()

#undef rv_mv
#define rv_mv(rd, rs) do {                              \
    if ((u32)(rd) != 0 && (u32)(rs) != 0)               \
        rvc_c_mv(rd, rs);                               \
    else                                                 \
        rv_mv_32(rd, rs);                                \
} while(0)

/* rv_add: use C.ADD when rd == rs1, rd != 0, rs2 != 0 */
#undef rv_add
#define rv_add(rd, rs1, rs2) do {                        \
    if ((u32)(rd) == (u32)(rs1) &&                       \
        (u32)(rd) != 0 && (u32)(rs2) != 0)               \
        rvc_c_add(rd, rs2);                              \
    else                                                 \
        rv_r_type(0x00, rs2, rs1, RV_ADD, rd, RV_OP_REG);\
} while(0)

/* rv_sub/and/or/xor: use CA-type when rd == rs1, both in x8-x15 */
#undef rv_sub
#define rv_sub(rd, rs1, rs2) do {                        \
    if ((u32)(rd) == (u32)(rs1) &&                       \
        RVC_CREG_OK(rd) && RVC_CREG_OK(rs2))             \
        rvc_c_sub(RVC_CREG(rd), RVC_CREG(rs2));         \
    else                                                 \
        rv_r_type(0x20, rs2, rs1, RV_SUB, rd, RV_OP_REG);\
} while(0)

#undef rv_and
#define rv_and(rd, rs1, rs2) do {                        \
    if ((u32)(rd) == (u32)(rs1) &&                       \
        RVC_CREG_OK(rd) && RVC_CREG_OK(rs2))             \
        rvc_c_and(RVC_CREG(rd), RVC_CREG(rs2));         \
    else                                                 \
        rv_r_type(0x00, rs2, rs1, RV_AND, rd, RV_OP_REG);\
} while(0)

#undef rv_or
#define rv_or(rd, rs1, rs2) do {                         \
    if ((u32)(rd) == (u32)(rs1) &&                       \
        RVC_CREG_OK(rd) && RVC_CREG_OK(rs2))             \
        rvc_c_or(RVC_CREG(rd), RVC_CREG(rs2));          \
    else                                                 \
        rv_r_type(0x00, rs2, rs1, RV_OR, rd, RV_OP_REG); \
} while(0)

#undef rv_xor
#define rv_xor(rd, rs1, rs2) do {                        \
    if ((u32)(rd) == (u32)(rs1) &&                       \
        RVC_CREG_OK(rd) && RVC_CREG_OK(rs2))             \
        rvc_c_xor(RVC_CREG(rd), RVC_CREG(rs2));         \
    else                                                 \
        rv_r_type(0x00, rs2, rs1, RV_XOR, rd, RV_OP_REG);\
} while(0)

/* rv_slli: use C.SLLI when rd == rs1, rd != 0, shamt != 0 */
#undef rv_slli
#define rv_slli(rd, rs1, shamt) do {                     \
    if ((u32)(rd) == (u32)(rs1) &&                       \
        (u32)(rd) != 0 && (u32)(shamt) != 0)             \
        rvc_c_slli(rd, shamt);                           \
    else                                                 \
        rv_i_type(shamt, rs1, RV_SLLI, rd, RV_OP_IMM);  \
} while(0)

/* rv_srli: use C.SRLI when rd == rs1, rd in x8-x15, shamt != 0 */
#undef rv_srli
#define rv_srli(rd, rs1, shamt) do {                     \
    if ((u32)(rd) == (u32)(rs1) &&                       \
        RVC_CREG_OK(rd) && (u32)(shamt) != 0)            \
        rvc_c_srli(RVC_CREG(rd), shamt);                 \
    else                                                 \
        rv_i_type(shamt, rs1, RV_SRLI, rd, RV_OP_IMM);  \
} while(0)

/* rv_srai: use C.SRAI when rd == rs1, rd in x8-x15, shamt != 0 */
#undef rv_srai
#define rv_srai(rd, rs1, shamt) do {                     \
    if ((u32)(rd) == (u32)(rs1) &&                       \
        RVC_CREG_OK(rd) && (u32)(shamt) != 0)            \
        rvc_c_srai(RVC_CREG(rd), shamt);                 \
    else                                                 \
        rv_i_type((shamt) | 0x400, rs1, RV_SRLI, rd, RV_OP_IMM); \
} while(0)

/* rv_andi: use C.ANDI when rd == rs1, rd in x8-x15, imm in [-32,31] */
#undef rv_andi
#define rv_andi(rd, rs1, imm) do {                       \
    if ((u32)(rd) == (u32)(rs1) && RVC_CREG_OK(rd) &&    \
        (s32)(imm) >= -32 && (s32)(imm) <= 31)            \
        rvc_c_andi(RVC_CREG(rd), imm);                   \
    else                                                  \
        rv_i_type(imm, rs1, RV_ANDI, rd, RV_OP_IMM);    \
} while(0)

/* rv_addi: use C.ADDI when rd == rs1, rd != 0, nzimm in [-32,31]\{0} */
#undef rv_addi
#define rv_addi(rd, rs1, imm) do {                       \
    if ((u32)(rd) == (u32)(rs1) && (u32)(rd) != 0 &&     \
        (s32)(imm) != 0 &&                                \
        (s32)(imm) >= -32 && (s32)(imm) <= 31)            \
        rvc_c_addi(rd, imm);                              \
    else                                                  \
        rv_i_type(imm, rs1, RV_ADDI, rd, RV_OP_IMM);    \
} while(0)

/* Override rv_load_imm32 to use C.LI / C.LUI for small constants */
#undef rv_load_imm32
#define rv_load_imm32(rd, imm32) do {                       \
    u32 _val = (u32)(imm32);                                \
    if ((u32)(rd) != 0 &&                                   \
        (s32)_val >= -32 && (s32)_val <= 31) {              \
        rvc_c_li(rd, (s32)_val);                            \
    } else if (_val == 0) {                                 \
        rv_mv(rd, rv_zero);                                 \
    } else if ((s32)_val >= -2048 && (s32)_val <= 2047) {   \
        rv_addi(rd, rv_zero, (s32)_val);                    \
    } else if ((_val & 0xFFF) == 0) {                       \
        s32 _nzhi = ((s32)_val) >> 12;                      \
        if ((u32)(rd) != 0 && (u32)(rd) != 2 &&             \
            _nzhi != 0 && _nzhi >= -32 && _nzhi <= 31)      \
            rvc_c_lui(rd, _val);                            \
        else                                                \
            rv_lui(rd, _val);                               \
    } else {                                                \
        u32 _hi = _val & 0xFFFFF000;                        \
        u32 _lo = _val & 0xFFF;                             \
        if (_lo & 0x800) _hi += 0x1000;                     \
        rv_lui(rd, _hi);                                    \
        rv_addi(rd, rd, (s32)(_lo << 20) >> 20);            \
    }                                                       \
} while(0)

#endif /* HAVE_RVC */

#endif /* RISCV_CODEGEN_H */
