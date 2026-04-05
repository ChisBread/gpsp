#ifndef TRACE_INSTR_H
#define TRACE_INSTR_H

#include "common.h"

#ifdef TRACE_INSTRUCTIONS

extern void trace_instruction(u32 pc, u32 cpsr);
extern void trace_open(const char *path);
extern void trace_close(void);

/* Called from JIT-generated code. Reads state from reg[] array.
   The JIT stub must store_registers + consolidate_flags before calling. */
void trace_jit_instruction(void);

#else

#define trace_instruction(pc, cpsr) do {} while(0)
#define trace_open(path) do {} while(0)
#define trace_close() do {} while(0)

#endif

#endif /* TRACE_INSTR_H */
