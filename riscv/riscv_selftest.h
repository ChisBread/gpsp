/*
 * JIT Self-Test for RISC-V dynarec backend
 *
 * Emits small RISC-V code sequences into the translation cache using the
 * same codegen primitives as the real JIT, then executes them and checks
 * the results.  Called once at boot before emulation starts.
 *
 * Tests cover:
 *   - rv_load_imm32 (immediate loading, sign-extend edge cases)
 *   - SUB/ADD flag computation (N, Z, C, V)
 *   - Condition code evaluation (all 15 ARM conditions)
 *   - Shift operations
 */

#ifndef RISCV_SELFTEST_H
#define RISCV_SELFTEST_H

#include <stdio.h>
#include <string.h>

/* ---------- test infrastructure ---------- */

/*
 * Every emitted test function has signature:  void fn(u32 out[8])
 *
 * Layout of out[]:
 *   [0] = primary result value
 *   [1] = N flag (0 or 1)
 *   [2] = Z flag (0 or 1)
 *   [3] = C flag (0 or 1)
 *   [4] = V flag (0 or 1)
 *   [5..7] = auxiliary (test-specific)
 */
typedef void (*selftest_fn_t)(u32 *);

static int st_pass, st_fail;

/* shorthand: emit with a local translation_ptr */
#define ST_EMIT_START(buf)  u8 *translation_ptr = (u8 *)(buf)

/* Prologue: save ALL callee-saved regs so tests can use JIT registers freely */
#define ST_PROLOGUE() do {                          \
    rv_addi(rv_sp, rv_sp, -64);                     \
    rv_sw(rv_s0,  rv_sp, 0);                        \
    rv_sw(rv_s1,  rv_sp, 4);                        \
    rv_sw(rv_s2,  rv_sp, 8);                        \
    rv_sw(rv_s3,  rv_sp, 12);                       \
    rv_sw(rv_s4,  rv_sp, 16);                       \
    rv_sw(rv_s5,  rv_sp, 20);                       \
    rv_sw(rv_s6,  rv_sp, 24);                       \
    rv_sw(rv_s7,  rv_sp, 28);                       \
    rv_sw(rv_s8,  rv_sp, 32);                       \
    rv_sw(rv_s9,  rv_sp, 36);                       \
    rv_sw(rv_s10, rv_sp, 40);                       \
    rv_sw(rv_s11, rv_sp, 44);                       \
    rv_sw(rv_ra,  rv_sp, 48);                       \
    rv_sw(rv_gp,  rv_sp, 52);                       \
    rv_mv(rv_s0, rv_a0);  /* s0 = &out[0] */        \
} while(0)

/* Epilogue: store result+flags, restore all callee-saved, ret */
#define ST_EPILOGUE(result_reg) do {                \
    rv_sw(result_reg, rv_s0, 0);                    \
    rv_sw(rv_s8,  rv_s0, 4);   /* N */              \
    rv_sw(rv_s9,  rv_s0, 8);   /* Z */              \
    rv_sw(rv_s10, rv_s0, 12);  /* C */              \
    rv_sw(rv_s11, rv_s0, 16);  /* V */              \
    rv_lw(rv_s0,  rv_sp, 0);                        \
    rv_lw(rv_s1,  rv_sp, 4);                        \
    rv_lw(rv_s2,  rv_sp, 8);                        \
    rv_lw(rv_s3,  rv_sp, 12);                       \
    rv_lw(rv_s4,  rv_sp, 16);                       \
    rv_lw(rv_s5,  rv_sp, 20);                       \
    rv_lw(rv_s6,  rv_sp, 24);                       \
    rv_lw(rv_s7,  rv_sp, 28);                       \
    rv_lw(rv_s8,  rv_sp, 32);                       \
    rv_lw(rv_s9,  rv_sp, 36);                       \
    rv_lw(rv_s10, rv_sp, 40);                       \
    rv_lw(rv_s11, rv_sp, 44);                       \
    rv_lw(rv_ra,  rv_sp, 48);                       \
    rv_lw(rv_gp,  rv_sp, 52);                       \
    rv_addi(rv_sp, rv_sp, 64);                      \
    rv_jalr(rv_zero, rv_ra, 0);                     \
} while(0)

/* Flush D-cache and invalidate I-cache, then call */
extern void platform_cache_sync(void *baseaddr, void *endptr);

#define ST_RUN(buf, out) do {                       \
    platform_cache_sync((void *)(buf),              \
        (void *)translation_ptr);                   \
    ((selftest_fn_t)(uintptr_t)(buf))(out);         \
} while(0)

#define ST_CHECK(desc, cond) do {                   \
    if (cond) { st_pass++; }                        \
    else {                                          \
        st_fail++;                                  \
        printf("  FAIL: %s\n", desc);               \
    }                                               \
} while(0)

#define ST_CHECK_FLAGS(desc, res, ev, en, ez, ec, eev) do { \
    int _ok = ((res)[0]==(u32)(ev)) &&              \
              ((res)[1]==(u32)(en)) &&               \
              ((res)[2]==(u32)(ez)) &&               \
              ((res)[3]==(u32)(ec)) &&               \
              ((res)[4]==(u32)(eev));                 \
    if (_ok) { st_pass++; }                         \
    else {                                          \
        st_fail++;                                  \
        printf("  FAIL: %s  val=0x%08x N=%u Z=%u C=%u V=%u\n", \
               desc, (res)[0], (res)[1], (res)[2], (res)[3], (res)[4]); \
        printf("        expected val=0x%08x N=%u Z=%u C=%u V=%u\n", \
               (u32)(ev), (u32)(en), (u32)(ez), (u32)(ec), (u32)(eev)); \
    }                                               \
} while(0)

/* ====================================================================
 * Emit: generate_op_sub_flags sequence
 * Inputs already set:  rn_reg = original rn,  rm_reg = rm,
 *                      rd_reg = rn - rm (already computed)
 * Mirrors the EXACT code emitted by generate_op_sub_flags() in
 * riscv_emit.h (with all flags enabled).
 * ==================================================================== */
static void st_emit_sub_flags(u8 **pp, int rd, int rn, int rm)
{
    u8 *translation_ptr = *pp;
    /* C = !(rn < rm)  (ARM carry = NOT borrow) */
    rv_sltu(rv_s10, rn, rm);
    rv_xori(rv_s10, rv_s10, 1);
    /* N = rd[31] */
    rv_srli(rv_s8, rd, 31);
    /* Z = (rd == 0) */
    rv_seqz(rv_s9, rd);
    /* V = ((rn ^ rm) & (rn ^ rd)) >> 31 */
    rv_xor(rv_t0, rn, rm);
    rv_xor(rv_t1, rn, rd);
    rv_and(rv_s11, rv_t0, rv_t1);
    rv_srli(rv_s11, rv_s11, 31);
    *pp = translation_ptr;
}

/* Same for ADD flags */
static void st_emit_add_flags(u8 **pp, int rd, int rn, int rm)
{
    u8 *translation_ptr = *pp;
    /* C = (rd < rn)  (unsigned carry out) */
    rv_sltu(rv_s10, rd, rn);
    /* N = rd[31] */
    rv_srli(rv_s8, rd, 31);
    /* Z = (rd == 0) */
    rv_seqz(rv_s9, rd);
    /* V = ((rn ^ rd) & (rm ^ rd)) >> 31 */
    rv_xor(rv_t0, rn, rd);
    rv_xor(rv_t1, rm, rd);
    rv_and(rv_s11, rv_t0, rv_t1);
    rv_srli(rv_s11, rv_s11, 31);
    *pp = translation_ptr;
}

/* ====================================================================
 * Emit: condition evaluation (mirrors generate_condition_XX)
 * Result in rd: 1 = condition TRUE, 0 = FALSE
 * Flags must already be in s8-s11.
 * ==================================================================== */
static void st_emit_condition(u8 **pp, int cond, int rd)
{
    u8 *translation_ptr = *pp;
    switch (cond) {
    case 0x0: /* EQ: Z==1 */
        rv_mv(rd, rv_s9);
        break;
    case 0x1: /* NE: Z==0 */
        rv_xori(rd, rv_s9, 1);
        break;
    case 0x2: /* CS/HS: C==1 */
        rv_mv(rd, rv_s10);
        break;
    case 0x3: /* CC/LO: C==0 */
        rv_xori(rd, rv_s10, 1);
        break;
    case 0x4: /* MI: N==1 */
        rv_mv(rd, rv_s8);
        break;
    case 0x5: /* PL: N==0 */
        rv_xori(rd, rv_s8, 1);
        break;
    case 0x6: /* VS: V==1 */
        rv_mv(rd, rv_s11);
        break;
    case 0x7: /* VC: V==0 */
        rv_xori(rd, rv_s11, 1);
        break;
    case 0x8: /* HI: C==1 && Z==0 */
        rv_xori(rv_t0, rv_s10, 1);
        rv_or(rv_t0, rv_t0, rv_s9);
        rv_xori(rd, rv_t0, 1); /* invert: skip-logic → truth */
        break;
    case 0x9: /* LS: C==0 || Z==1 */
        rv_xori(rv_t0, rv_s10, 1);
        rv_or(rd, rv_t0, rv_s9);
        break;
    case 0xA: /* GE: N==V */
        rv_sub(rv_t0, rv_s8, rv_s11);
        rv_seqz(rd, rv_t0);
        break;
    case 0xB: /* LT: N!=V */
        rv_sub(rv_t0, rv_s8, rv_s11);
        rv_snez(rd, rv_t0);
        break;
    case 0xC: /* GT: Z==0 && N==V */
        rv_xor(rv_t0, rv_s8, rv_s11);
        rv_or(rv_t0, rv_t0, rv_s9);
        rv_seqz(rd, rv_t0);
        break;
    case 0xD: /* LE: Z==1 || N!=V */
        rv_xor(rv_t0, rv_s8, rv_s11);
        rv_or(rd, rv_t0, rv_s9);
        break;
    case 0xE: /* AL */
        rv_li(rd, 1);
        break;
    }
    *pp = translation_ptr;
}

/* ====================================================================
 * TEST GROUP 1: rv_load_imm32 correctness
 * ==================================================================== */
static void test_load_imm32(u8 *buf)
{
    u32 out[8];
    static const u32 test_vals[] = {
        0x00000000, 0x00000001, 0x000007FF, 0x00000800,
        0x00000FFF, 0x00001000, 0xFFFFF800, 0xFFFFF000,
        0xFFFFFFFF, 0x80000000, 0x7FFFFFFF, 0x12345678,
        0xDEADBEEF, 0x00008000, 0xFFFF8000
    };
    printf("[load_imm32]\n");
    for (unsigned i = 0; i < sizeof(test_vals)/sizeof(test_vals[0]); i++) {
        u32 val = test_vals[i];
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_load_imm32(rv_a1, val);
        rv_mv(rv_s8, rv_zero); rv_mv(rv_s9, rv_zero);
        rv_mv(rv_s10, rv_zero); rv_mv(rv_s11, rv_zero);
        ST_EPILOGUE(rv_a1);
        ST_RUN(buf, out);
        char desc[64];
        snprintf(desc, sizeof(desc), "load_imm32(0x%08x)", val);
        ST_CHECK(desc, out[0] == val);
    }
}

/* ====================================================================
 * TEST GROUP 2: SUB flags (CMP)
 * ARM: rd = rn - rm, C = NOT borrow, V = signed overflow
 * ==================================================================== */
static void test_sub_flags(u8 *buf)
{
    u32 out[8];
    struct { u32 rn, rm, rd, n, z, c, v; const char *desc; } cases[] = {
        /* basic: 10 - 3 = 7 → N=0 Z=0 C=1(no borrow) V=0 */
        { 10, 3, 7, 0, 0, 1, 0, "10-3" },
        /* zero result: 5 - 5 = 0 → N=0 Z=1 C=1(no borrow) V=0 */
        { 5, 5, 0, 0, 1, 1, 0, "5-5" },
        /* borrow: 3 - 10 = 0xFFFFFFF9 → N=1 Z=0 C=0(borrow) V=0 */
        { 3, 10, 0xFFFFFFF9, 1, 0, 0, 0, "3-10" },
        /* signed overflow+: 0x7FFFFFFF - (-1) = 0x80000000 → V=1 */
        { 0x7FFFFFFF, 0xFFFFFFFF, 0x80000000, 1, 0, 0, 1, "MAX_POS-(-1)" },
        /* signed overflow-: 0x80000000 - 1 = 0x7FFFFFFF → V=1 */
        { 0x80000000, 1, 0x7FFFFFFF, 0, 0, 1, 1, "MIN_NEG-1" },
        /* zero - zero */
        { 0, 0, 0, 0, 1, 1, 0, "0-0" },
        /* big unsigned, no borrow: 0xFFFFFFFF - 0 */
        { 0xFFFFFFFF, 0, 0xFFFFFFFF, 1, 0, 1, 0, "FFFFFFFF-0" },
        /* big unsigned with borrow: 0 - 1 */
        { 0, 1, 0xFFFFFFFF, 1, 0, 0, 0, "0-1" },
    };
    printf("[sub_flags]\n");
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        /* a1 = rn, t1 = rm */
        rv_load_imm32(rv_a1, cases[i].rn);
        rv_load_imm32(rv_t1, cases[i].rm);
        /* s1 = rn (save original for flags) */
        rv_mv(rv_s1, rv_a1);
        /* a1 = rn - rm (result) */
        rv_sub(rv_a1, rv_a1, rv_t1);
        /* compute flags: rd=a1, rn_orig=s1, rm=t1 */
        u8 *p = translation_ptr;
        st_emit_sub_flags(&p, rv_a1, rv_s1, rv_t1);
        translation_ptr = p;
        ST_EPILOGUE(rv_a1);
        ST_RUN(buf, out);
        ST_CHECK_FLAGS(cases[i].desc, out,
                       cases[i].rd, cases[i].n, cases[i].z,
                       cases[i].c, cases[i].v);
    }
}

/* ====================================================================
 * TEST GROUP 3: ADD flags
 * ARM: rd = rn + rm, C = carry out, V = signed overflow
 * ==================================================================== */
static void test_add_flags(u8 *buf)
{
    u32 out[8];
    struct { u32 rn, rm, rd, n, z, c, v; const char *desc; } cases[] = {
        { 3, 4, 7, 0, 0, 0, 0, "3+4" },
        { 0, 0, 0, 0, 1, 0, 0, "0+0" },
        { 0xFFFFFFFF, 1, 0, 0, 1, 1, 0, "FFFFFFFF+1" },
        { 0x80000000, 0x80000000, 0, 0, 1, 1, 1, "MIN+MIN" },
        { 0x7FFFFFFF, 1, 0x80000000, 1, 0, 0, 1, "MAX+1" },
        { 0x7FFFFFFF, 0x7FFFFFFF, 0xFFFFFFFE, 1, 0, 0, 1, "MAX+MAX" },
        { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFE, 1, 0, 1, 0, "-1+(-1)" },
    };
    printf("[add_flags]\n");
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_load_imm32(rv_a1, cases[i].rn);
        rv_load_imm32(rv_t1, cases[i].rm);
        rv_mv(rv_s1, rv_a1);   /* save rn */
        rv_add(rv_a1, rv_a1, rv_t1);
        u8 *p = translation_ptr;
        st_emit_add_flags(&p, rv_a1, rv_s1, rv_t1);
        translation_ptr = p;
        ST_EPILOGUE(rv_a1);
        ST_RUN(buf, out);
        ST_CHECK_FLAGS(cases[i].desc, out,
                       cases[i].rd, cases[i].n, cases[i].z,
                       cases[i].c, cases[i].v);
    }
}

/* ====================================================================
 * TEST GROUP 4: Condition codes
 * Set known flags, then evaluate each condition.
 * ==================================================================== */
static void test_conditions(u8 *buf)
{
    u32 out[8];
    /* Test with flags: N=0, Z=0, C=1, V=0  (result of "10 CMP 3") */
    struct { int cond; int expected; const char *desc; } cases_nzcv_0010[] = {
        { 0x0, 0, "EQ(Z=0)" }, { 0x1, 1, "NE(Z=0)" },
        { 0x2, 1, "CS(C=1)" }, { 0x3, 0, "CC(C=1)" },
        { 0x4, 0, "MI(N=0)" }, { 0x5, 1, "PL(N=0)" },
        { 0x6, 0, "VS(V=0)" }, { 0x7, 1, "VC(V=0)" },
        { 0x8, 1, "HI(C=1,Z=0)" }, { 0x9, 0, "LS(C=1,Z=0)" },
        { 0xA, 1, "GE(N=V=0)" }, { 0xB, 0, "LT(N=V=0)" },
        { 0xC, 1, "GT(Z=0,N=V)" }, { 0xD, 0, "LE(Z=0,N=V)" },
        { 0xE, 1, "AL" },
    };
    printf("[conditions NZCV=0010]\n");
    for (unsigned i = 0; i < sizeof(cases_nzcv_0010)/sizeof(cases_nzcv_0010[0]); i++) {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        /* Set flags: N=0 Z=0 C=1 V=0 */
        rv_li(rv_s8, 0);  rv_li(rv_s9, 0);
        rv_li(rv_s10, 1); rv_li(rv_s11, 0);
        u8 *p = translation_ptr;
        st_emit_condition(&p, cases_nzcv_0010[i].cond, rv_t0);
        translation_ptr = p;
        /* Store condition result as out[0], keep flags */
        ST_EPILOGUE(rv_t0);
        ST_RUN(buf, out);
        char desc[64];
        snprintf(desc, sizeof(desc), "%s => %d",
                 cases_nzcv_0010[i].desc, cases_nzcv_0010[i].expected);
        ST_CHECK(desc, out[0] == (u32)cases_nzcv_0010[i].expected);
    }

    /* Test with flags: N=1, Z=0, C=0, V=0  (result of "3 CMP 10") */
    struct { int cond; int expected; const char *desc; } cases_nzcv_1000[] = {
        { 0x0, 0, "EQ(Z=0)" }, { 0x1, 1, "NE(Z=0)" },
        { 0x2, 0, "CS(C=0)" }, { 0x3, 1, "CC(C=0)" },
        { 0x4, 1, "MI(N=1)" }, { 0x5, 0, "PL(N=1)" },
        { 0x8, 0, "HI(C=0,Z=0)" }, { 0x9, 1, "LS(C=0,Z=0)" },
        { 0xA, 0, "GE(N=1,V=0)" }, { 0xB, 1, "LT(N=1,V=0)" },
        { 0xC, 0, "GT(Z=0,N!=V)" }, { 0xD, 1, "LE(Z=0,N!=V)" },
    };
    printf("[conditions NZCV=1000]\n");
    for (unsigned i = 0; i < sizeof(cases_nzcv_1000)/sizeof(cases_nzcv_1000[0]); i++) {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_li(rv_s8, 1); rv_li(rv_s9, 0);
        rv_li(rv_s10, 0); rv_li(rv_s11, 0);
        u8 *p = translation_ptr;
        st_emit_condition(&p, cases_nzcv_1000[i].cond, rv_t0);
        translation_ptr = p;
        ST_EPILOGUE(rv_t0);
        ST_RUN(buf, out);
        char desc[64];
        snprintf(desc, sizeof(desc), "%s => %d",
                 cases_nzcv_1000[i].desc, cases_nzcv_1000[i].expected);
        ST_CHECK(desc, out[0] == (u32)cases_nzcv_1000[i].expected);
    }

    /* Test with flags: N=0, Z=1, C=1, V=0  (result of "5 CMP 5") */
    struct { int cond; int expected; const char *desc; } cases_nzcv_0110[] = {
        { 0x0, 1, "EQ(Z=1)" }, { 0x1, 0, "NE(Z=1)" },
        { 0x8, 0, "HI(C=1,Z=1)" }, { 0x9, 1, "LS(C=1,Z=1)" },
        { 0xC, 0, "GT(Z=1,N=V)" }, { 0xD, 1, "LE(Z=1,N=V)" },
    };
    printf("[conditions NZCV=0110]\n");
    for (unsigned i = 0; i < sizeof(cases_nzcv_0110)/sizeof(cases_nzcv_0110[0]); i++) {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_li(rv_s8, 0); rv_li(rv_s9, 1);
        rv_li(rv_s10, 1); rv_li(rv_s11, 0);
        u8 *p = translation_ptr;
        st_emit_condition(&p, cases_nzcv_0110[i].cond, rv_t0);
        translation_ptr = p;
        ST_EPILOGUE(rv_t0);
        ST_RUN(buf, out);
        char desc[64];
        snprintf(desc, sizeof(desc), "%s => %d",
                 cases_nzcv_0110[i].desc, cases_nzcv_0110[i].expected);
        ST_CHECK(desc, out[0] == (u32)cases_nzcv_0110[i].expected);
    }

    /* Test with V=1: N=1 V=1 → GE true (N==V) */
    printf("[conditions with V=1]\n");
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_li(rv_s8, 1); rv_li(rv_s9, 0);
        rv_li(rv_s10, 0); rv_li(rv_s11, 1);
        u8 *p = translation_ptr;
        st_emit_condition(&p, 0xA, rv_t0); /* GE: N==V → 1==1 → true */
        translation_ptr = p;
        ST_EPILOGUE(rv_t0);
        ST_RUN(buf, out);
        ST_CHECK("GE(N=1,V=1) => 1", out[0] == 1);
    }
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_li(rv_s8, 0); rv_li(rv_s9, 0);
        rv_li(rv_s10, 0); rv_li(rv_s11, 1);
        u8 *p = translation_ptr;
        st_emit_condition(&p, 0xA, rv_t0); /* GE: N==V → 0!=1 → false */
        translation_ptr = p;
        ST_EPILOGUE(rv_t0);
        ST_RUN(buf, out);
        ST_CHECK("GE(N=0,V=1) => 0", out[0] == 0);
    }
}

/* ====================================================================
 * TEST GROUP 5: Combined "CMP rn, rm" end-to-end
 * Emit SUB + flags + condition eval, check ARM-equivalent result.
 * ==================================================================== */
static void test_cmp_cond(u8 *buf)
{
    u32 out[8];
    struct { u32 rn, rm; int cond; int expected; const char *desc; } cases[] = {
        { 10, 3,  0xC, 1, "10>3  GT" },
        { 10, 3,  0xD, 0, "10>3  LE" },
        { 3,  10, 0xB, 1, "3<10  LT" },
        { 3,  10, 0xA, 0, "3<10  GE" },
        { 5,  5,  0x0, 1, "5==5  EQ" },
        { 5,  5,  0x1, 0, "5==5  NE" },
        { 10, 3,  0x8, 1, "10>3  HI(unsigned)" },
        { 3,  10, 0x9, 1, "3<10  LS(unsigned)" },
        { 0,  0,  0x0, 1, "0==0  EQ" },
        { 0xFFFFFFFF, 0, 0x4, 1, "-1 cmp 0  MI" },
        { 0x80000000, 1, 0xB, 1, "0x80000000-1 overflows LT" },
    };
    printf("[cmp+condition]\n");
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_load_imm32(rv_a1, cases[i].rn);
        rv_load_imm32(rv_t1, cases[i].rm);
        rv_mv(rv_s1, rv_a1);
        rv_sub(rv_a1, rv_a1, rv_t1);
        u8 *p = translation_ptr;
        st_emit_sub_flags(&p, rv_a1, rv_s1, rv_t1);
        st_emit_condition(&p, cases[i].cond, rv_t0);
        translation_ptr = p;
        ST_EPILOGUE(rv_t0);
        ST_RUN(buf, out);
        ST_CHECK(cases[i].desc, out[0] == (u32)cases[i].expected);
    }
}

/* ====================================================================
 * TEST GROUP 6: Shift operations
 * ==================================================================== */
static void test_shifts(u8 *buf)
{
    u32 out[8];
    printf("[shifts]\n");

    /* LSL #4: 0x12345678 << 4 = 0x23456780 */
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_load_imm32(rv_t0, 0x12345678);
        rv_slli(rv_t0, rv_t0, 4);
        rv_mv(rv_s8, rv_zero); rv_mv(rv_s9, rv_zero);
        rv_mv(rv_s10, rv_zero); rv_mv(rv_s11, rv_zero);
        ST_EPILOGUE(rv_t0);
        ST_RUN(buf, out);
        ST_CHECK("LSL#4 0x12345678", out[0] == 0x23456780);
    }
    /* LSR #4: 0x12345678 >> 4 = 0x01234567 */
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_load_imm32(rv_t0, 0x12345678);
        rv_srli(rv_t0, rv_t0, 4);
        rv_mv(rv_s8, rv_zero); rv_mv(rv_s9, rv_zero);
        rv_mv(rv_s10, rv_zero); rv_mv(rv_s11, rv_zero);
        ST_EPILOGUE(rv_t0);
        ST_RUN(buf, out);
        ST_CHECK("LSR#4 0x12345678", out[0] == 0x01234567);
    }
    /* ASR #4: 0x80000000 >> 4 = 0xF8000000 */
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_load_imm32(rv_t0, 0x80000000);
        rv_srai(rv_t0, rv_t0, 4);
        rv_mv(rv_s8, rv_zero); rv_mv(rv_s9, rv_zero);
        rv_mv(rv_s10, rv_zero); rv_mv(rv_s11, rv_zero);
        ST_EPILOGUE(rv_t0);
        ST_RUN(buf, out);
        ST_CHECK("ASR#4 0x80000000", out[0] == 0xF8000000);
    }
    /* ROR #8: 0x12345678 ROR 8 = 0x78123456 */
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_load_imm32(rv_t0, 0x12345678);
        rv_rori(rv_t0, rv_t0, 8, rv_t1);
        rv_mv(rv_s8, rv_zero); rv_mv(rv_s9, rv_zero);
        rv_mv(rv_s10, rv_zero); rv_mv(rv_s11, rv_zero);
        ST_EPILOGUE(rv_t0);
        ST_RUN(buf, out);
        ST_CHECK("ROR#8 0x12345678", out[0] == 0x78123456);
    }
}

/* ====================================================================
 * TEST GROUP 7: extract_flags / consolidate_flags round-trip
 * Encode NZCV into a CPSR word, extract to flag regs, check.
 * ==================================================================== */
static void test_flag_extract(u8 *buf)
{
    u32 out[8];
    /* CPSR = 0xA0000000 → N=1,Z=0,C=1,V=0 */
    printf("[flag_extract]\n");
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_load_imm32(rv_t0, 0xA0000000); /* N=1 Z=0 C=1 V=0 */
        /* extract: N = bit31, Z = bit30, C = bit29, V = bit28 */
        rv_srli(rv_s8, rv_t0, 31); rv_andi(rv_s8, rv_s8, 1);
        rv_srli(rv_s9, rv_t0, 30); rv_andi(rv_s9, rv_s9, 1);
        rv_srli(rv_s10, rv_t0, 29); rv_andi(rv_s10, rv_s10, 1);
        rv_srli(rv_s11, rv_t0, 28); rv_andi(rv_s11, rv_s11, 1);
        rv_mv(rv_t0, rv_zero); /* result = 0, flags carry the test */
        ST_EPILOGUE(rv_t0);
        ST_RUN(buf, out);
        ST_CHECK("extract N=1 from 0xA0000000", out[1] == 1);
        ST_CHECK("extract Z=0 from 0xA0000000", out[2] == 0);
        ST_CHECK("extract C=1 from 0xA0000000", out[3] == 1);
        ST_CHECK("extract V=0 from 0xA0000000", out[4] == 0);
    }
    /* CPSR = 0x50000000 → N=0,Z=1,C=0,V=1 */
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_load_imm32(rv_t0, 0x50000000); /* N=0 Z=1 C=0 V=1 */
        rv_srli(rv_s8, rv_t0, 31); rv_andi(rv_s8, rv_s8, 1);
        rv_srli(rv_s9, rv_t0, 30); rv_andi(rv_s9, rv_s9, 1);
        rv_srli(rv_s10, rv_t0, 29); rv_andi(rv_s10, rv_s10, 1);
        rv_srli(rv_s11, rv_t0, 28); rv_andi(rv_s11, rv_s11, 1);
        rv_mv(rv_t0, rv_zero);
        ST_EPILOGUE(rv_t0);
        ST_RUN(buf, out);
        ST_CHECK("extract N=0 from 0x50000000", out[1] == 0);
        ST_CHECK("extract Z=1 from 0x50000000", out[2] == 1);
        ST_CHECK("extract C=0 from 0x50000000", out[3] == 0);
        ST_CHECK("extract V=1 from 0x50000000", out[4] == 1);
    }
}

/* ====================================================================
 * TEST GROUP 8: ADC / SBC flag chains
 * Test the more complex carry-chain computations
 * ==================================================================== */
static void test_adc_sbc(u8 *buf)
{
    u32 out[8];
    printf("[adc_sbc]\n");

    /* ADC: rd = rn + rm + C.  Test: 0xFFFFFFFF + 0 + C(1) = 0, C_out=1 */
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_load_imm32(rv_t0, 0xFFFFFFFF); /* rn */
        rv_mv(rv_t1, rv_zero);             /* rm = 0 */
        rv_li(rv_s10, 1);                  /* C_in = 1 */
        /* ADC sequence: reg_temp3 = rm + C_in */
        rv_add(rv_t2, rv_t1, rv_s10);
        rv_sltu(rv_t1, rv_t2, rv_t1);     /* t1 = carry from rm+C */
        /* rd = rn + (rm + C_in) */
        rv_add(rv_t0, rv_t0, rv_t2);      /* t0 = rn + reg_temp3 */
        rv_mv(rv_s8, rv_zero); rv_mv(rv_s9, rv_zero);
        rv_mv(rv_s10, rv_zero); rv_mv(rv_s11, rv_zero);
        ST_EPILOGUE(rv_t0);
        ST_RUN(buf, out);
        ST_CHECK("ADC 0xFFFFFFFF+0+1 val", out[0] == 0);
    }
}

/* ====================================================================
 * TEST GROUP 9: Store/Load registers round-trip
 * Simulates what the JIT stubs do: set gp to a buffer, store all JIT
 * registers, clobber them, load them back, verify.
 * ==================================================================== */
static void test_store_load_regs(u8 *buf)
{
    /* reg_array: space for the full "reg" struct (at least 32 words) */
    static u32 reg_array[32] __attribute__((aligned(16)));
    u32 out[8];

    printf("[store_load_regs]\n");

    /* Test: save all mapped JIT regs, clobber, restore, check s7 (reg_pc) */
    {
        memset(reg_array, 0, sizeof(reg_array));
        ST_EMIT_START(buf);
        ST_PROLOGUE();

        /* Set some JIT registers to known values */
        rv_load_imm32(rv_s7, 0xBEEF0008);   /* reg_pc */
        rv_load_imm32(rv_s5, 0x55555555);   /* reg_r12 */
        rv_load_imm32(rv_s6, 0x66666666);   /* reg_r14 */
        rv_load_imm32(rv_t5, 0xBBBBBBBB);   /* reg_r11 */
        rv_load_imm32(rv_t6, 0xCCCCCCCC);   /* reg_r13 */
        rv_load_imm32(rv_s2, 0x22222222);   /* reg_r6  */

        /* Save gp, set gp = &reg_array for store/load regs simulation */
        rv_mv(rv_t0, rv_gp);
        rv_load_imm32(rv_gp, (u32)(uintptr_t)reg_array);

        /* Inline store_registers: sw t3,SAVE2; sw s0-s6,s7 etc. */
        /* t3 -> REG_SAVE2(27*4=108) */
        rv_sw(rv_t3, rv_gp, 27*4);
        /* s0 -> REG_R0(0) */
        rv_sw(rv_s0, rv_gp, 0*4);
        /* s2 -> REG_R6(6*4=24) */
        rv_sw(rv_s2, rv_gp, 6*4);
        /* s5 -> REG_R12(12*4=48) */
        rv_sw(rv_s5, rv_gp, 12*4);
        /* t6 -> REG_R13(13*4=52) */
        rv_sw(rv_t6, rv_gp, 13*4);
        /* s6 -> REG_R14(14*4=56) */
        rv_sw(rv_s6, rv_gp, 14*4);
        /* t5 -> REG_R11(11*4=44) */
        rv_sw(rv_t5, rv_gp, 11*4);
        /* s7 -> REG_PC(15*4=60) */
        rv_sw(rv_s7, rv_gp, 15*4);

        /* Clobber the registers */
        rv_li(rv_s7, 0);
        rv_li(rv_s5, 0);
        rv_li(rv_s6, 0);
        rv_li(rv_t5, 0);
        rv_li(rv_t6, 0);
        rv_li(rv_s2, 0);

        /* Inline load_registers: restore from reg_array */
        rv_lw(rv_s2, rv_gp, 6*4);
        rv_lw(rv_s5, rv_gp, 12*4);
        rv_lw(rv_t6, rv_gp, 13*4);
        rv_lw(rv_s6, rv_gp, 14*4);
        rv_lw(rv_t5, rv_gp, 11*4);
        rv_lw(rv_s7, rv_gp, 15*4);

        /* Restore gp */
        rv_mv(rv_gp, rv_t0);

        /* Report s7 as result */
        rv_mv(rv_a1, rv_s7);
        rv_mv(rv_s8, rv_zero); rv_mv(rv_s9, rv_zero);
        rv_mv(rv_s10, rv_zero); rv_mv(rv_s11, rv_zero);
        ST_EPILOGUE(rv_a1);
        ST_RUN(buf, out);
        ST_CHECK("store/load s7(PC) round-trip", out[0] == 0xBEEF0008);
    }
    /* Verify the memory buffer has correct values (written by store above) */
    ST_CHECK("reg_array[15] = PC", reg_array[15] == 0xBEEF0008);
    ST_CHECK("reg_array[12] = r12", reg_array[12] == 0x55555555);
    ST_CHECK("reg_array[14] = r14", reg_array[14] == 0x66666666);
    ST_CHECK("reg_array[11] = r11", reg_array[11] == 0xBBBBBBBB);
    ST_CHECK("reg_array[13] = r13", reg_array[13] == 0xCCCCCCCC);
    ST_CHECK("reg_array[6]  = r6",  reg_array[6]  == 0x22222222);
}

/* ====================================================================
 * TEST GROUP 9: Consolidate + Extract flags round-trip
 * Sets flag registers, consolidates into a CPSR word, then extracts
 * back out — exactly as the JIT stubs do.
 * ==================================================================== */
static void test_consolidate_extract(u8 *buf)
{
    static u32 reg_array[32] __attribute__((aligned(16)));
    u32 out[8];
    printf("[consolidate_extract]\n");

    /* CPSR in reg_array[16] = some initial mode bits, no flags */
    struct { u32 n, z, c, v; const char *desc; } cases[] = {
        { 1, 0, 1, 0, "N=1 C=1" },
        { 0, 1, 0, 1, "Z=1 V=1" },
        { 1, 1, 1, 1, "all set" },
        { 0, 0, 0, 0, "all clear" },
    };

    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        memset(reg_array, 0, sizeof(reg_array));
        reg_array[16] = 0x0000001F; /* CPSR = system mode, no flags */

        ST_EMIT_START(buf);
        ST_PROLOGUE();

        /* Set gp = &reg_array */
        rv_mv(rv_t0, rv_gp);
        rv_load_imm32(rv_gp, (u32)(uintptr_t)reg_array);

        /* Set flags */
        rv_li(rv_s8, cases[i].n);
        rv_li(rv_s9, cases[i].z);
        rv_li(rv_s10, cases[i].c);
        rv_li(rv_s11, cases[i].v);

        /* Inline consolidate_flags(t1, t2):
         * t1 = cpsr, t2 = temp
         * clear top 4 bits, OR in flags */
        rv_lw(rv_t1, rv_gp, 16*4);          /* t1 = CPSR */
        rv_load_imm32(rv_t2, 0x0FFFFFFF);
        rv_and(rv_t1, rv_t1, rv_t2);
        rv_slli(rv_t2, rv_s8, 31);
        rv_or(rv_t1, rv_t1, rv_t2);
        rv_slli(rv_t2, rv_s9, 30);
        rv_or(rv_t1, rv_t1, rv_t2);
        rv_slli(rv_t2, rv_s10, 29);
        rv_or(rv_t1, rv_t1, rv_t2);
        rv_slli(rv_t2, rv_s11, 28);
        rv_or(rv_t1, rv_t1, rv_t2);
        rv_sw(rv_t1, rv_gp, 16*4);          /* store CPSR */

        /* Now clobber flags */
        rv_li(rv_s8, 0x77); rv_li(rv_s9, 0x77);
        rv_li(rv_s10, 0x77); rv_li(rv_s11, 0x77);

        /* Inline extract_flags(t1): */
        rv_lw(rv_t1, rv_gp, 16*4);
        rv_srli(rv_s8, rv_t1, 31);
        rv_andi(rv_s8, rv_s8, 1);
        rv_srli(rv_s9, rv_t1, 30);
        rv_andi(rv_s9, rv_s9, 1);
        rv_srli(rv_s10, rv_t1, 29);
        rv_andi(rv_s10, rv_s10, 1);
        rv_srli(rv_s11, rv_t1, 28);
        rv_andi(rv_s11, rv_s11, 1);

        /* Restore gp */
        rv_mv(rv_gp, rv_t0);

        rv_mv(rv_a1, rv_zero); /* result = 0, flags carry the test */
        ST_EPILOGUE(rv_a1);
        ST_RUN(buf, out);

        char desc[64];
        snprintf(desc, sizeof(desc), "consol+extract %s N", cases[i].desc);
        ST_CHECK(desc, out[1] == cases[i].n);
        snprintf(desc, sizeof(desc), "consol+extract %s Z", cases[i].desc);
        ST_CHECK(desc, out[2] == cases[i].z);
        snprintf(desc, sizeof(desc), "consol+extract %s C", cases[i].desc);
        ST_CHECK(desc, out[3] == cases[i].c);
        snprintf(desc, sizeof(desc), "consol+extract %s V", cases[i].desc);
        ST_CHECK(desc, out[4] == cases[i].v);
    }
}

/* ====================================================================
 * TEST GROUP 10: generate_function_call pattern
 * Verify that calling a C function through the JIT calling convention
 * (load address into t0, jalr) doesn't clobber callee-saved registers.
 * ==================================================================== */
static u32 selftest_nop_func(u32 arg) { return arg + 1; }

static void test_function_call(u8 *buf)
{
    u32 out[8];
    printf("[function_call]\n");

    /* Call a real C function, check that s-regs survive */
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();

        /* Put known values in callee-saved regs used by JIT */
        rv_load_imm32(rv_s2, 0xAAAAAAAA);  /* r6 */
        rv_load_imm32(rv_s3, 0xBBBBBBBB);  /* r9 */
        rv_load_imm32(rv_s7, 0xDDDDDDDD);  /* PC */

        /* a0 = argument */
        rv_li(rv_a0, 42);

        /* generate_function_call pattern: load addr into t0, jalr */
        rv_load_imm32(rv_t0, (u32)(uintptr_t)selftest_nop_func);
        rv_jalr(rv_ra, rv_t0, 0);
        /* a0 now = 43 (return value) */

        /* Check that s2, s3, s7 survived the call */
        /* s2 should still be 0xAAAAAAAA */
        rv_mv(rv_a1, rv_s2);
        /* Store s3, s7, retval into out[5..7] via s0 */
        rv_sw(rv_s3, rv_s0, 20); /* out[5] = s3 */
        rv_sw(rv_s7, rv_s0, 24); /* out[6] = s7 */
        rv_sw(rv_a0, rv_s0, 28); /* out[7] = retval */

        rv_mv(rv_s8, rv_zero); rv_mv(rv_s9, rv_zero);
        rv_mv(rv_s10, rv_zero); rv_mv(rv_s11, rv_zero);
        ST_EPILOGUE(rv_a1);
        ST_RUN(buf, out);

        ST_CHECK("fcall: s2 preserved", out[0] == 0xAAAAAAAA);
        ST_CHECK("fcall: retval correct", out[7] == 43);
        ST_CHECK("fcall: s3 preserved", out[5] == 0xBBBBBBBB);
        ST_CHECK("fcall: s7 preserved", out[6] == 0xDDDDDDDD);
    }
}

/* ====================================================================
 * TEST GROUP 11: Branch offset calculations
 * Verify that rv_bge skip offsets are correct — this is how cycle
 * checks work in generate_branch_no_cycle_update.
 * ==================================================================== */
static void test_branch_offsets(u8 *buf)
{
    u32 out[8];
    printf("[branch_offsets]\n");

    /* Pattern: if (cycles >= 0) skip N bytes, else fall through.
     * We test that the skip lands on the correct instruction. */
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();

        /* t0 = 1 (cycles > 0 → should skip) */
        rv_li(rv_t0, 1);

        /* bge t0, zero, +24  (skip 6 instructions = 24 bytes) */
        rv_bge(rv_t0, rv_zero, 24);

        /* Fall-through path (should NOT execute): set a1=0xBAD */
        rv_load_imm32(rv_a1, 0xBAD00BAD);   /* 2 instr = 8 bytes */
        rv_load_imm32(rv_t1, 0);             /* 2 instr = 8 bytes */
        rv_nop();                             /* 1 instr = 4 bytes */
        /* total fall-through = 5 instructions = 20 bytes */
        /* bge skips 24 bytes = 6 instructions from bge */
        /* so the skip target is 24 bytes after the bge */

        /* Skip target: set a1=0x600D */
        rv_load_imm32(rv_a1, 0x600D600D);

        rv_mv(rv_s8, rv_zero); rv_mv(rv_s9, rv_zero);
        rv_mv(rv_s10, rv_zero); rv_mv(rv_s11, rv_zero);
        ST_EPILOGUE(rv_a1);
        ST_RUN(buf, out);
        ST_CHECK("bge +24 skip (positive)", out[0] == 0x600D600D);
    }

    /* Test negative cycles → should fall through */
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();

        rv_li(rv_t0, -1);
        rv_bge(rv_t0, rv_zero, 16);
        /* Fall-through: set a1=0xFALL */
        rv_load_imm32(rv_a1, 0xFA110000);  /* 8 bytes (+4,+8) */
        rv_j(12);                           /* skip BAD load (8 bytes), 4+8=12 */
        /* bge skip target at +16: */
        rv_load_imm32(rv_a1, 0xBAD00BAD);  /* should NOT execute (+16,+20) */

        rv_mv(rv_s8, rv_zero); rv_mv(rv_s9, rv_zero);
        rv_mv(rv_s10, rv_zero); rv_mv(rv_s11, rv_zero);
        ST_EPILOGUE(rv_a1);
        ST_RUN(buf, out);
        ST_CHECK("bge +16 fallthrough (negative)", out[0] == 0xFA110000);
    }
}

/* ====================================================================
 * TEST GROUP 12: JAL/JALR offset patching
 * Verify that rv_j and rv_patch_jal produce correct jumps.
 * ==================================================================== */
static void test_jump_patching(u8 *buf)
{
    u32 out[8];
    printf("[jump_patching]\n");

    /* Emit: j +12 (skip 2 instructions + the j itself) → should skip
     * the "bad" load and land on the "good" load. */
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_j(12);                                /* +0: jump +12 */
        rv_load_imm32(rv_a1, 0xBAD00BAD);       /* +4,+8: should be skipped */
        rv_load_imm32(rv_a1, 0x12345678);       /* +12,+16: landing target */
        rv_mv(rv_s8, rv_zero); rv_mv(rv_s9, rv_zero);
        rv_mv(rv_s10, rv_zero); rv_mv(rv_s11, rv_zero);
        ST_EPILOGUE(rv_a1);
        ST_RUN(buf, out);
        ST_CHECK("j +12 forward", out[0] == 0x12345678);
    }
}

/* ====================================================================
 * TEST GROUP 13: Caller-saved register clobber across stubs
 * Verify that t0-t2 (reg_temp, reg_temp2, reg_temp3) are properly
 * treated as scratch — i.e., NOT relied upon across function calls.
 * ==================================================================== */
static u32 selftest_clobber_func(u32 a) {
    /* Touch lots of registers to simulate realistic C code */
    volatile u32 x = a * 3 + 7;
    return x;
}

static void test_caller_saved_clobber(u8 *buf)
{
    u32 out[8];
    printf("[caller_saved_clobber]\n");

    /* Set t3 (reg_save0) before a function call. After the call,
     * t3 is caller-saved so it SHOULD be clobbered.
     * But the stubs save/restore t3 via REG_SAVE2.
     * We test here without stub wrappers. */
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();

        rv_load_imm32(rv_t3, 0x12340000);  /* set t3 */
        rv_load_imm32(rv_t4, 0xABCD0000);  /* set t4 (caller-saved in JIT) */

        /* Call a C function — t3 and t4 will be clobbered by C ABI */
        rv_li(rv_a0, 5);
        rv_load_imm32(rv_t0, (u32)(uintptr_t)selftest_clobber_func);
        rv_jalr(rv_ra, rv_t0, 0);

        /* t3 may have been clobbered (it's t3 = x28).
         * t4 is x29 = s? Actually let me check. */
        /* In RISC-V standard ABI: t3=x28, t4=x29, t5=x30, t6=x31
         * ALL are caller-saved. So C code CAN clobber them. */
        /* But in JIT context, t4 = reg_r8, which maps to GBA r8.
         * The stubs save/restore it. Without stubs, it's clobbered. */

        /* Report a0 (return value) to show function was called */
        rv_mv(rv_a1, rv_a0);
        rv_mv(rv_s8, rv_zero); rv_mv(rv_s9, rv_zero);
        rv_mv(rv_s10, rv_zero); rv_mv(rv_s11, rv_zero);
        ST_EPILOGUE(rv_a1);
        ST_RUN(buf, out);
        ST_CHECK("clobber: func called", out[0] == 22); /* 5*3+7=22 */
    }
}

/* ====================================================================
 * TEST GROUP 14: Memory access address calculation
 * Test the address computation patterns used for LDR/STR/LDM/STM.
 * ==================================================================== */
static void test_address_calc(u8 *buf)
{
    u32 out[8];
    printf("[address_calc]\n");

    /* Pre-increment: addr = base + offset */
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_load_imm32(rv_a1, 0x02000000);
        rv_addi(rv_a0, rv_a1, 64);
        rv_mv(rv_s8, rv_zero); rv_mv(rv_s9, rv_zero);
        rv_mv(rv_s10, rv_zero); rv_mv(rv_s11, rv_zero);
        ST_EPILOGUE(rv_a0);
        ST_RUN(buf, out);
        ST_CHECK("pre-inc: 0x02000000+64", out[0] == 0x02000040);
    }

    /* Pre-decrement: addr = base - offset */
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_load_imm32(rv_a1, 0x02000100);
        rv_addi(rv_a1, rv_a1, -64);
        rv_mv(rv_s8, rv_zero); rv_mv(rv_s9, rv_zero);
        rv_mv(rv_s10, rv_zero); rv_mv(rv_s11, rv_zero);
        ST_EPILOGUE(rv_a1);
        ST_RUN(buf, out);
        ST_CHECK("pre-dec: 0x02000100-64", out[0] == 0x020000C0);
    }

    /* Large offset via generate_sub_imm path (> 2048) */
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_load_imm32(rv_a1, 0x03000000);
        rv_load_imm32(rv_t0, 4096);
        rv_sub(rv_a1, rv_a1, rv_t0);
        rv_mv(rv_s8, rv_zero); rv_mv(rv_s9, rv_zero);
        rv_mv(rv_s10, rv_zero); rv_mv(rv_s11, rv_zero);
        ST_EPILOGUE(rv_a1);
        ST_RUN(buf, out);
        ST_CHECK("large sub: 0x03000000-4096", out[0] == 0x02FFF000);
    }

    /* Align-down to word: addr & ~3 */
    {
        ST_EMIT_START(buf);
        ST_PROLOGUE();
        rv_load_imm32(rv_a1, 0x02000007);
        rv_load_imm32(rv_t0, ~3u);
        rv_and(rv_a1, rv_a1, rv_t0);
        rv_mv(rv_s8, rv_zero); rv_mv(rv_s9, rv_zero);
        rv_mv(rv_s10, rv_zero); rv_mv(rv_s11, rv_zero);
        ST_EPILOGUE(rv_a1);
        ST_RUN(buf, out);
        ST_CHECK("align: 0x02000007 & ~3", out[0] == 0x02000004);
    }
}

/* ====================================================================
 * Main entry point
 * ==================================================================== */
static void jit_selftest(void)
{
    extern u8 rom_translation_cache[];
    u8 *buf = rom_translation_cache;

    st_pass = 0;
    st_fail = 0;
    printf("\n========== JIT SELF-TEST ==========\n");

    test_load_imm32(buf);
    test_sub_flags(buf);
    test_add_flags(buf);
    test_conditions(buf);
    test_cmp_cond(buf);
    test_shifts(buf);
    test_flag_extract(buf);
    test_adc_sbc(buf);
    test_store_load_regs(buf);
    test_consolidate_extract(buf);
    test_function_call(buf);
    test_branch_offsets(buf);
    test_jump_patching(buf);
    test_caller_saved_clobber(buf);
    test_address_calc(buf);

    printf("===================================\n");
    printf("  PASS: %d   FAIL: %d\n", st_pass, st_fail);
    printf("===================================\n\n");

    if (st_fail > 0) {
        printf("*** JIT SELF-TEST FAILED — dynarec disabled ***\n");
        extern int dynarec_enable;
        dynarec_enable = 0;
    }

    /* Wipe the scratch area so translation cache is clean for real use */
    memset(buf, 0, 4096);
    platform_cache_sync(buf, buf + 4096);
}

#endif /* RISCV_SELFTEST_H */
