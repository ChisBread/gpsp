#define u32 uint32_t
#define u8  uint8_t

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "riscv_emit.h"

u32 reg[64];

u32 execute_arm_translate_internal(u32 cycles, void *regptr)
{
  (void)cycles;
  (void)regptr;
  return 0;
}

static int expect_s32(s32 got, s32 expected, const char *label)
{
  if (got != expected) {
    fprintf(stderr, "%s mismatch: got %d expected %d\n", label, got, expected);
    return 0;
  }
  return 1;
}

static int expect_u32(u32 got, u32 expected, const char *label)
{
  if (got != expected) {
    fprintf(stderr, "%s mismatch: got 0x%08x expected 0x%08x\n",
            label, got, expected);
    return 0;
  }
  return 1;
}

static s32 decode_jal_offset(u32 inst)
{
  u32 imm20 = (inst >> 31) & 0x1;
  u32 imm10_1 = (inst >> 21) & 0x3ff;
  u32 imm11 = (inst >> 20) & 0x1;
  u32 imm19_12 = (inst >> 12) & 0xff;
  u32 imm = (imm20 << 20) | (imm19_12 << 12) | (imm11 << 11) | (imm10_1 << 1);
  return (s32)(imm << 11) >> 11;
}

static s32 decode_branch_offset(u32 inst)
{
  u32 imm12 = (inst >> 31) & 0x1;
  u32 imm10_5 = (inst >> 25) & 0x3f;
  u32 imm4_1 = (inst >> 8) & 0xf;
  u32 imm11 = (inst >> 7) & 0x1;
  u32 imm = (imm12 << 12) | (imm11 << 11) | (imm10_5 << 5) | (imm4_1 << 1);
  return (s32)(imm << 19) >> 19;
}

int main(void)
{
  u32 buffer[16] = {0};
  u8 *translation_ptr = (u8 *)&buffer[0];
  int ok = 1;

  rv_j(0);
  rv_beq(rv_a0, rv_a1, 0);
  rv_nop();
  rv_nop();
  rv_nop();
  rv_nop();

  ok &= expect_s32(rv_jal_offset(&buffer[4], &buffer[0]), 16, "rv_jal_offset forward");
  ok &= expect_s32(rv_jal_offset(&buffer[0], &buffer[4]), -16, "rv_jal_offset backward");

  rv_patch_jal(&buffer[0], &buffer[4]);
  ok &= expect_s32(decode_jal_offset(buffer[0]), 16, "rv_patch_jal forward offset");
  ok &= expect_u32(buffer[0] & 0x7f, 0x6f, "rv_patch_jal opcode");

  rv_patch_jal(&buffer[0], &buffer[0]);
  ok &= expect_s32(decode_jal_offset(buffer[0]), 0, "rv_patch_jal zero offset");

  rv_patch_branch(&buffer[1], &buffer[5]);
  ok &= expect_s32(decode_branch_offset(buffer[1]), 16, "rv_patch_branch forward offset");
  ok &= expect_u32(buffer[1] & 0x707f, 0x63, "rv_patch_branch opcode/funct preservation");

  rv_patch_branch(&buffer[1], &buffer[0]);
  ok &= expect_s32(decode_branch_offset(buffer[1]), -4, "rv_patch_branch backward offset");

  if (!ok)
    return EXIT_FAILURE;

  puts("RISC-V emit patch helper checks passed!");
  return EXIT_SUCCESS;
}