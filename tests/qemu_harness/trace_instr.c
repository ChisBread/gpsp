#include <stdio.h>
#include "common.h"

#ifdef TRACE_INSTRUCTIONS

static FILE *trace_file;
static u32 trace_count;

void trace_open(const char *path)
{
    trace_file = fopen(path, "w");
    if (!trace_file) {
        fprintf(stderr, "[trace] ERROR: cannot open '%s'\n", path);
        return;
    }
    trace_count = 0;
    fprintf(trace_file, "# seq  PC       CPSR     r0       r1       r2       r3       "
            "r4       r5       r6       r7       r8       r9       r10      "
            "r11      r12      SP       LR\n");
}

void trace_close(void)
{
    if (trace_file) {
        fprintf(stderr, "[trace] %u instructions traced\n", trace_count);
        fclose(trace_file);
        trace_file = NULL;
    }
}

/* Called from interpreter: individual flags are passed collapsed in cpsr param */
void trace_instruction(u32 pc, u32 cpsr)
{
    if (!trace_file) return;
    fprintf(trace_file,
        "%07u %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x "
        "%08x %08x %08x %08x %08x %08x %08x\n",
        trace_count++, pc, cpsr,
        reg[0], reg[1], reg[2], reg[3],
        reg[4], reg[5], reg[6], reg[7],
        reg[8], reg[9], reg[10], reg[11],
        reg[12], reg[13], reg[14]);
}

/* Called from JIT stubs — registers already written back to reg[] */
void trace_jit_instruction(void)
{
    if (!trace_file) return;
    fprintf(trace_file,
        "%07u %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x "
        "%08x %08x %08x %08x %08x %08x %08x\n",
        trace_count++,
        reg[REG_PC], reg[REG_CPSR],
        reg[0], reg[1], reg[2], reg[3],
        reg[4], reg[5], reg[6], reg[7],
        reg[8], reg[9], reg[10], reg[11],
        reg[12], reg[13], reg[14]);
}

#endif /* TRACE_INSTRUCTIONS */
