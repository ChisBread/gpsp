/*
 * Instruction tracing declarations.
 *
 * Implementations are provided by the test harness stubs
 * (harness_stubs.c) when TRACE_INSTRUCTIONS is defined.
 */

#ifndef TRACE_INSTR_H
#define TRACE_INSTR_H

#include "common.h"

void trace_open(const char *path);
void trace_close(void);
void trace_instruction(u32 pc, u32 cpsr);
void trace_jit_instruction(void);

#endif /* TRACE_INSTR_H */
