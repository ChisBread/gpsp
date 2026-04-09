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

/* System */
#define rv_ecall()               rv_emit(0x00000073)
#define rv_ebreak()              rv_emit(0x00100073)
#define rv_fence_i()             rv_emit(0x0000100F)
#define rv_fence()               rv_emit(0x0FF0000F)

/* ---- Rotate helpers (synthesized for RV32IMAC without Zbb) ---- */
/* NOTE: slli/sll MUST come before srli/srl so that rd==rs is safe. */
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

#endif /* RISCV_CODEGEN_H */
