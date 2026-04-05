/*
 * JIT Trace — lightweight execution trace for dynarec debugging.
 *
 * Records translation and dispatch events into a ring buffer.
 * Prints concise lines to serial in real time.
 * Dumps full trace to /sdcard/tests/trace.log on request.
 */

#ifndef JIT_TRACE_H
#define JIT_TRACE_H

#include "common.h"

#ifdef JIT_TRACE_ENABLED

/* Event types stored in the ring buffer */
enum jit_trace_event_type {
    JT_TRANSLATE_ARM   = 'A',   /* ARM block translated  */
    JT_TRANSLATE_THUMB = 'T',   /* Thumb block translated */
    JT_DISPATCH_ARM    = 'a',   /* ARM block dispatched  */
    JT_DISPATCH_THUMB  = 't',   /* Thumb block dispatched */
    JT_DISPATCH_DUAL   = 'd',   /* Dual block dispatched */
    JT_UPDATE_GBA      = 'U',   /* update_gba called */
    JT_INDIRECT_ARM    = 'I',   /* indirect branch ARM   */
    JT_INDIRECT_THUMB  = 'i',   /* indirect branch Thumb */
    JT_INDIRECT_DUAL   = 'D',   /* indirect branch Dual  */
    JT_FRAME           = 'F',   /* frame complete */
};

typedef struct {
    u8  type;           /* jit_trace_event_type */
    u8  extra;          /* flags / sub-info */
    u16 seq;            /* sequence number (wraps at 65536) */
    u32 pc;             /* GBA PC */
    u32 host;           /* host address or block size */
    u32 cycles;         /* cycle count or timestamp */
} jit_trace_entry_t;

/* Ring buffer size — must be power of 2.
 * 4096 entries × 16 bytes = 64 KB */
#define JIT_TRACE_RING_SIZE   4096
#define JIT_TRACE_RING_MASK   (JIT_TRACE_RING_SIZE - 1)

/* ---- API ---- */

void jit_trace_init(void);

/* Record events (inline-friendly, called from hot paths) */
void jit_trace_translate(u32 pc, u8 *host_ptr, u32 block_size, int is_thumb);
void jit_trace_dispatch(u32 pc, u8 *host_ptr, int type);
void jit_trace_indirect(u32 pc, u8 *host_ptr, int type);
void jit_trace_update_gba(int remaining_cycles);
void jit_trace_frame(void);

/* Dump GBA opcodes for a translated block */
void jit_trace_block_opcodes(u32 start_pc, u32 end_pc, int is_thumb);

/* Dump trace ring buffer to /sdcard/tests/trace.log */
void jit_trace_dump_to_sd(void);

/* Print last N entries to serial */
void jit_trace_print_last(int n);

/* Get total event count */
u32 jit_trace_count(void);

#else /* !JIT_TRACE_ENABLED */

#define jit_trace_init()
#define jit_trace_translate(pc, host, size, thumb)
#define jit_trace_dispatch(pc, host, type)
#define jit_trace_indirect(pc, host, type)
#define jit_trace_update_gba(rem)
#define jit_trace_frame()
#define jit_trace_block_opcodes(start, end, thumb)
#define jit_trace_dump_to_sd()
#define jit_trace_print_last(n)
#define jit_trace_count() 0

#endif /* JIT_TRACE_ENABLED */

#endif /* JIT_TRACE_H */
