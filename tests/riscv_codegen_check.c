#define u32 uint32_t
#define u8  uint8_t

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "riscv_codegen.h"

static int expect_word(const u32 *buffer, size_t index, u32 expected, const char *label)
{
  if (buffer[index] != expected) {
    fprintf(stderr, "%s mismatch at %zu: got 0x%08x expected 0x%08x\n",
            label, index, buffer[index], expected);
    return 0;
  }
  return 1;
}

int main(void)
{
  u32 buffer[64] = {0};
  u8 *translation_ptr = (u8 *)&buffer[0];
  int ok = 1;

  rv_load_imm32(rv_a0, 0x12345678);
  rv_load_imm32(rv_sp, 0x00000800);
  rv_load_imm32(rv_a0, 0xfffff800u);
  rv_load_imm32(rv_a0, 0x000007ff);

  rv_rori(rv_a0, rv_a1, 5, rv_t0);
  rv_rori(rv_sp, rv_ra, 0, rv_t0);
  rv_ror(rv_a0, rv_a1, rv_a2, rv_t0, rv_t1);

  ok &= expect_word(buffer, 0, 0x12345537u, "rv_load_imm32(0x12345678)/lui");
  ok &= expect_word(buffer, 1, 0x67850513u, "rv_load_imm32(0x12345678)/addi");

  ok &= expect_word(buffer, 2, 0x00001137u, "rv_load_imm32(0x00000800)/lui");
  ok &= expect_word(buffer, 3, 0x80010113u, "rv_load_imm32(0x00000800)/addi");

  ok &= expect_word(buffer, 4, 0x00000537u, "rv_load_imm32(0xfffff800)/lui");
  ok &= expect_word(buffer, 5, 0x80050513u, "rv_load_imm32(0xfffff800)/addi");

  ok &= expect_word(buffer, 6, 0x00000537u, "rv_load_imm32(0x000007ff)/lui");
  ok &= expect_word(buffer, 7, 0x7ff50513u, "rv_load_imm32(0x000007ff)/addi");

  ok &= expect_word(buffer, 8, 0x0055d513u, "rv_rori(shamt=5)/srli");
  ok &= expect_word(buffer, 9, 0x01b59293u, "rv_rori(shamt=5)/slli");
  ok &= expect_word(buffer, 10, 0x00556533u, "rv_rori(shamt=5)/or");

  ok &= expect_word(buffer, 11, 0x00008113u, "rv_rori(shamt=0)/mv");

  ok &= expect_word(buffer, 12, 0x01f67293u, "rv_ror/andi");
  ok &= expect_word(buffer, 13, 0x40500333u, "rv_ror/sub");
  ok &= expect_word(buffer, 14, 0x01f37313u, "rv_ror/andi tmp2");
  ok &= expect_word(buffer, 15, 0x0055d533u, "rv_ror/srl");
  ok &= expect_word(buffer, 16, 0x00659333u, "rv_ror/sll");
  ok &= expect_word(buffer, 17, 0x00656533u, "rv_ror/or");

  if (!ok)
    return EXIT_FAILURE;

  puts("RISC-V codegen semantic checks passed!");
  return EXIT_SUCCESS;
}