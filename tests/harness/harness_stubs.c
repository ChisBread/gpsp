/*
 * Unified GBA Test Harness — platform stubs
 *
 * Minimal implementations of functions that the GBA core expects but
 * that are normally provided by libretro or ESP32 platform code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "main.h"
#include <streams/file_stream.h>
#include <vfs/vfs_implementation.h>

/* --- Global variables normally provided by app_main.c / libretro.c --- */
u32 skip_next_frame = 0;
int sprite_limit = 1;
boot_mode selected_boot_mode = boot_game;
u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_targets = 0;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
int dynarec_enable = 0;

/* --- Fastforward stub --- */
void set_fastforward_override(bool fastforward)
{
    (void)fastforward;
}

/* --- Direct ROM loading --- */
void harness_load_rom_direct(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("[harness] ERROR: cannot open ROM '%s'\n", path);
        exit(1);
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    gamepak_size = (u32)size;
    printf("[harness] ROM size: %ld bytes\n", size);

    extern u32 gamepak_buffer_blocksize;
    extern u8 *gamepak_buffers[];
    extern u32 gamepak_buffer_count;
    extern u8 *memory_map_read[];

    u32 remaining = (u32)size;
    u32 block = 0;
    while (remaining > 0 && block < gamepak_buffer_count && gamepak_buffers[block]) {
        u32 chunk = remaining > gamepak_buffer_blocksize ?
                    gamepak_buffer_blocksize : remaining;
        fread(gamepak_buffers[block], 1, chunk, f);
        remaining -= chunk;
        block++;
    }
    fclose(f);

    /* Reopen for demand paging (load_gamepak_page uses gamepak_file_large) */
    {
        extern RFILE *gamepak_file_large;
        gamepak_file_large = filestream_open(path,
            RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
    }

    /* Round size to 32KB pages */
    gamepak_size = (gamepak_size + 0x7FFF) & ~0x7FFF;

    /* Map ROM pages with proper mirroring */
    u32 rom_blocks = gamepak_size >> 15;
    u32 i, j;
    for (i = 0; i < block; i++) {
        for (j = 0; j < 32 && i * 32 + j < rom_blocks; j++) {
            u32 phyn = i * 32 + j;
            u8 *blkptr = &gamepak_buffers[i][32 * 1024 * j];
            unsigned mcount;
            for (mcount = 0; mcount < 1024; mcount += rom_blocks) {
                memory_map_read[(0x8000000 / (32 * 1024)) + phyn + mcount] = blkptr;
                memory_map_read[(0xA000000 / (32 * 1024)) + phyn + mcount] = blkptr;
            }
            for (mcount = 0; mcount < 512; mcount += rom_blocks) {
                memory_map_read[(0xC000000 / (32 * 1024)) + phyn + mcount] = blkptr;
            }
        }
    }

    printf("[harness] ROM loaded, %u pages mapped\n", rom_blocks);
}

void harness_load_bios_direct(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("[harness] WARNING: cannot open BIOS '%s', using open BIOS\n", path);
        return;
    }
    fread(bios_rom, 1, 0x4000, f);
    fclose(f);
    printf("[harness] BIOS loaded from '%s'\n", path);
}

/* --- Netplay stubs --- */
u32 netplay_client_id = 0;
u32 netplay_num_clients = 1;
void netpacket_send(u32 flags, const void *buf, u32 len) {
    (void)flags; (void)buf; (void)len;
}
void netpacket_poll_receive(void) {}

/* --- Instruction tracing stubs --- */
#ifdef TRACE_INSTRUCTIONS
static FILE *trace_file = NULL;
static u64 trace_count = 0;
#define TRACE_MAX 5000000  /* stop after 5M instructions to limit file size */

void trace_open(const char *path) { (void)path; }
void trace_close(void) {
    if (trace_file) { fclose(trace_file); trace_file = NULL; }
}
void trace_instruction(u32 pc, u32 cpsr) { (void)pc; (void)cpsr; }

void trace_jit_instruction(void) {
    if (trace_count >= TRACE_MAX) return;
    if (!trace_file) {
        trace_file = fopen("trace.bin", "wb");
        if (!trace_file) return;
    }
    /* reg[0..14] = r0-r14, reg[15] = REG_PC, reg[16] = REG_CPSR */
    /* Write: PC, r0-r14, CPSR  (17 x u32 = 68 bytes per record) */
    u32 record[17];
    record[0] = reg[REG_PC];
    for (int i = 0; i < 15; i++)
        record[1 + i] = reg[i];
    record[16] = reg[REG_CPSR];
    fwrite(record, sizeof(u32), 17, trace_file);
    trace_count++;
    if (trace_count >= TRACE_MAX) {
        fflush(trace_file);
        printf("[trace] reached %llu instructions, stopping trace\n",
               (unsigned long long)trace_count);
    }
}
#endif
