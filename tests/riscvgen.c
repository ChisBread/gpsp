#define u32 uint32_t
#define u8  uint8_t

#include <stdio.h>
#include <stdint.h>
#include "riscv_codegen.h"

int main() {
  u32 buffer[1024];
  u8 *translation_ptr = (u8 *)&buffer[0];

  rv_nop();

  rv_add(rv_a0, rv_a1, rv_a2);
  rv_sub(rv_sp, rv_ra, rv_s4);
  rv_addi(rv_a0, rv_s6, -1);
  rv_addi(rv_a0, rv_s6, 0x7ff);

  rv_and(rv_a0, rv_a1, rv_a2);
  rv_or(rv_sp, rv_ra, rv_s4);
  rv_xor(rv_a0, rv_a1, rv_a2);
  rv_andi(rv_a0, rv_s6, -1);
  rv_ori(rv_a0, rv_s6, 0x7ff);
  rv_xori(rv_a0, rv_s6, -1);

  rv_sll(rv_a0, rv_a1, rv_a2);
  rv_srl(rv_sp, rv_ra, rv_s4);
  rv_sra(rv_a0, rv_a1, rv_a2);
  rv_slli(rv_a0, rv_a1, 1);
  rv_srli(rv_a0, rv_a1, 31);
  rv_srai(rv_sp, rv_ra, 7);

  rv_slt(rv_a0, rv_a1, rv_a2);
  rv_sltu(rv_sp, rv_ra, rv_s4);
  rv_slti(rv_a0, rv_s6, -1);
  rv_sltiu(rv_a0, rv_s6, 0x7ff);

  rv_lw(rv_a0, rv_a1, 16);
  rv_lh(rv_a0, rv_a1, -16);
  rv_lhu(rv_a0, rv_a1, 16);
  rv_lb(rv_a0, rv_a1, -16);
  rv_lbu(rv_a0, rv_a1, 16);
  rv_sw(rv_a0, rv_a1, 16);
  rv_sh(rv_a0, rv_a1, -16);
  rv_sb(rv_a0, rv_a1, 16);

  rv_beqz(rv_a0, 16);
  rv_nop();
  rv_nop();
  rv_nop();
  rv_nop();
  rv_nop();
  rv_nop();
  rv_bnez(rv_sp, -12);

  rv_jal(rv_ra, 16);
  rv_nop();
  rv_nop();
  rv_nop();
  rv_j(16);
  rv_nop();
  rv_nop();
  rv_nop();
  rv_jalr(rv_a0, rv_a1, 16);

  rv_lui(rv_a0, 0x12345000);
  rv_auipc(rv_sp, 0xabcdf000);

  rv_mul(rv_a0, rv_a1, rv_a2);
  rv_mulh(rv_sp, rv_ra, rv_s4);
  rv_mulhu(rv_a0, rv_a1, rv_a2);
  rv_div(rv_sp, rv_ra, rv_s4);
  rv_divu(rv_a0, rv_a1, rv_a2);
  rv_rem(rv_sp, rv_ra, rv_s4);
  rv_remu(rv_a0, rv_a1, rv_a2);

  rv_mv(rv_a0, rv_a1);
  rv_not(rv_sp, rv_ra);
  rv_neg(rv_a0, rv_a1);
  rv_li(rv_a0, -1);
  rv_seqz(rv_a0, rv_a1);
  rv_snez(rv_sp, rv_ra);

  rv_ecall();
  rv_ebreak();
  rv_fence_i();
  rv_fence();

  rv_rori(rv_a0, rv_a1, 5, rv_t0);
  rv_rori(rv_sp, rv_ra, 0, rv_t0);
  rv_ror(rv_a0, rv_a1, rv_a2, rv_t0, rv_t1);

  rv_load_imm32(rv_a0, 0x12345678);
  rv_load_imm32(rv_sp, 0x00000800);
  rv_load_imm32(rv_a0, 0xfffff800);

  fwrite(buffer, 1, translation_ptr - (u8 *)buffer, stdout);
}
