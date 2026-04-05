/*
 * JIT Trace — implementation
 */

#include "jit_trace.h"

#ifdef JIT_TRACE_ENABLED

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* ---- Ring buffer state ---- */
static jit_trace_entry_t s_ring[JIT_TRACE_RING_SIZE];
static u32 s_ring_head;          /* next write index */
static u16 s_seq;                /* sequence counter */
static u32 s_total_events;       /* total events recorded */
static int s_serial_verbose;     /* 1 = print every event to serial */

/* Max events to print to serial per frame to avoid flooding */
#define SERIAL_MAX_PER_TRANSLATE  1  /* only log translates, not dispatches */

/* ---- Helper: push one entry ---- */
static inline void push_entry(u8 type, u32 pc, u32 host, u32 cycles, u8 extra)
{
    u32 idx = s_ring_head & JIT_TRACE_RING_MASK;
    s_ring[idx].type   = type;
    s_ring[idx].extra  = extra;
    s_ring[idx].seq    = s_seq++;
    s_ring[idx].pc     = pc;
    s_ring[idx].host   = host;
    s_ring[idx].cycles = cycles;
    s_ring_head++;
    s_total_events++;
}

/* ---- API implementation ---- */

void jit_trace_init(void)
{
    memset(s_ring, 0, sizeof(s_ring));
    s_ring_head = 0;
    s_seq = 0;
    s_total_events = 0;
    s_serial_verbose = 1;
    printf("[jit_trace] initialized, ring=%u entries\n", JIT_TRACE_RING_SIZE);
}

void jit_trace_translate(u32 pc, u8 *host_ptr, u32 block_size, int is_thumb)
{
    u8 type = is_thumb ? JT_TRANSLATE_THUMB : JT_TRANSLATE_ARM;
    push_entry(type, pc, (u32)(uintptr_t)host_ptr, block_size, 0);

    /* Always print translations to serial — they are infrequent */
    printf("JT %c pc=%08x host=%08x size=%u\n",
           (char)type, pc, (unsigned)(uintptr_t)host_ptr, block_size);
}

void jit_trace_dispatch(u32 pc, u8 *host_ptr, int type)
{
    u8 etype;
    switch (type) {
        case 0: etype = JT_DISPATCH_ARM;   break;
        case 1: etype = JT_DISPATCH_THUMB; break;
        default: etype = JT_DISPATCH_DUAL; break;
    }
    push_entry(etype, pc, (u32)(uintptr_t)host_ptr, 0, 0);

    /* Print first 200 dispatches to serial, then only every 1000th */
    if (s_total_events < 200 || (s_total_events % 1000) == 0) {
        printf("JT %c pc=%08x host=%08x [#%u]\n",
               (char)etype, pc, (unsigned)(uintptr_t)host_ptr, s_total_events);
    }
}

void jit_trace_indirect(u32 pc, u8 *host_ptr, int type)
{
    u8 etype;
    switch (type) {
        case 0: etype = JT_INDIRECT_ARM;   break;
        case 1: etype = JT_INDIRECT_THUMB; break;
        default: etype = JT_INDIRECT_DUAL; break;
    }
    push_entry(etype, pc, (u32)(uintptr_t)host_ptr, 0, 0);

    /* Always print indirect branches — these are the suspect */
    printf("JT %c pc=%08x host=%08x\n",
           (char)etype, pc, (unsigned)(uintptr_t)host_ptr);
}

void jit_trace_update_gba(int remaining_cycles)
{
    push_entry(JT_UPDATE_GBA, 0, 0, (u32)remaining_cycles, 0);
}

void jit_trace_frame(void)
{
    push_entry(JT_FRAME, 0, 0, 0, 0);
    /* Print frame marker with event count */
    printf("JT F total=%u\n", s_total_events);
}

/* ---- Block opcode dump ---- */

extern u8 *memory_map_read[];

void jit_trace_block_opcodes(u32 start_pc, u32 end_pc, int is_thumb)
{
    u32 region = start_pc >> 15;
    u8 *page = memory_map_read[region];
    if (!page) {
        printf("JT OPC %08x: <unmapped>\n", start_pc);
        return;
    }

    printf("JT OPC %08x-%08x:", start_pc, end_pc);
    u32 pc = start_pc;
    int n = 0;
    if (is_thumb) {
        while (pc < end_pc && n < 20) {
            /* Re-lookup page if crossed boundary */
            if ((pc >> 15) != region) {
                region = pc >> 15;
                page = memory_map_read[region];
                if (!page) break;
            }
            u16 op = *(u16 *)(page + (pc & 0x7FFF));
            printf(" %04x", op);
            pc += 2;
            n++;
        }
    } else {
        while (pc < end_pc && n < 16) {
            if ((pc >> 15) != region) {
                region = pc >> 15;
                page = memory_map_read[region];
                if (!page) break;
            }
            u32 op = *(u32 *)(page + (pc & 0x7FFF));
            printf(" %08x", op);
            pc += 4;
            n++;
        }
    }
    if (pc < end_pc) printf(" ...");
    printf("\n");
}

/* ---- Dump to SD card ---- */

void jit_trace_dump_to_sd(void)
{
    /* Ensure /sdcard/tests/ exists */
    mkdir("/sdcard/tests", 0755);

    FILE *f = fopen("/sdcard/tests/trace.log", "w");
    if (!f) {
        printf("[jit_trace] ERROR: cannot open /sdcard/tests/trace.log\n");
        return;
    }

    fprintf(f, "=== JIT TRACE DUMP ===\n");
    fprintf(f, "Total events: %u\n", s_total_events);
    fprintf(f, "Ring size: %u\n", JIT_TRACE_RING_SIZE);
    fprintf(f, "\n");

    /* Determine how many entries to dump */
    u32 count = s_total_events;
    if (count > JIT_TRACE_RING_SIZE) count = JIT_TRACE_RING_SIZE;

    /* Start from oldest entry in ring */
    u32 start = s_ring_head - count;

    fprintf(f, "seq  type  pc        host      cycles/size extra\n");
    fprintf(f, "---- ----  --------  --------  ----------  -----\n");

    for (u32 i = 0; i < count; i++) {
        u32 idx = (start + i) & JIT_TRACE_RING_MASK;
        jit_trace_entry_t *e = &s_ring[idx];

        const char *type_str;
        switch (e->type) {
            case JT_TRANSLATE_ARM:   type_str = "TR_A"; break;
            case JT_TRANSLATE_THUMB: type_str = "TR_T"; break;
            case JT_DISPATCH_ARM:    type_str = "DS_A"; break;
            case JT_DISPATCH_THUMB:  type_str = "DS_T"; break;
            case JT_DISPATCH_DUAL:   type_str = "DS_D"; break;
            case JT_UPDATE_GBA:      type_str = "UGBA"; break;
            case JT_INDIRECT_ARM:    type_str = "IN_A"; break;
            case JT_INDIRECT_THUMB:  type_str = "IN_T"; break;
            case JT_INDIRECT_DUAL:   type_str = "IN_D"; break;
            case JT_FRAME:           type_str = "FRAM"; break;
            default:                 type_str = "????"; break;
        }

        fprintf(f, "%04x %-4s  %08x  %08x  %10u  %u\n",
                e->seq, type_str, e->pc, e->host, e->cycles, e->extra);
    }

    fprintf(f, "\n=== END TRACE ===\n");
    fclose(f);

    printf("[jit_trace] dumped %u entries to /sdcard/tests/trace.log\n", count);
}

/* ---- Print last N to serial ---- */

void jit_trace_print_last(int n)
{
    u32 count = s_total_events;
    if (count > JIT_TRACE_RING_SIZE) count = JIT_TRACE_RING_SIZE;
    if ((u32)n > count) n = count;

    u32 start = s_ring_head - n;
    printf("[jit_trace] last %d of %u events:\n", n, s_total_events);

    for (int i = 0; i < n; i++) {
        u32 idx = (start + i) & JIT_TRACE_RING_MASK;
        jit_trace_entry_t *e = &s_ring[idx];
        printf("  %04x %c pc=%08x host=%08x cyc=%u\n",
               e->seq, e->type, e->pc, e->host, e->cycles);
    }
}

u32 jit_trace_count(void)
{
    return s_total_events;
}

#endif /* JIT_TRACE_ENABLED */
