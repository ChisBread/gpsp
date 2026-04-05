/*
 * Stubs and platform replacements for standalone QEMU harness.
 *
 * Provides minimal implementations of functions that the GBA core
 * expects but that are normally provided by libretro or ESP32 platform code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "main.h"

/* --- Global variables normally provided by app_main.c / libretro.c --- */
u32 skip_next_frame = 0;
int sprite_limit = 1;
boot_mode selected_boot_mode = boot_game;
u32 idle_loop_target_pc = 0xFFFFFFFF;
u32 translation_gate_targets = 0;
u32 translation_gate_target_pc[MAX_TRANSLATION_GATES];
int dynarec_enable = 0;

/* --- Fastforward stub (called from input.c) --- */
void set_fastforward_override(bool fastforward)
{
    (void)fastforward;
}

/* --- Direct ROM loading (bypass libretro VFS for simplicity) --- */
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

    /* Read into gamepak_buffers (already allocated by init_gamepak_buffer) */
    extern u32 gamepak_buffer_blocksize;
    extern u8 *gamepak_buffers[];
    extern u32 gamepak_buffer_count;
    extern u8 *memory_map_read[];
    printf("[harness] gamepak_buffer_count=%u blocksize=%u\n",
           gamepak_buffer_count, gamepak_buffer_blocksize);
    fflush(stdout);

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
    printf("[harness] ROM read into %u blocks, %u bytes remaining\n", block, remaining);
    fflush(stdout);

    /* Round size to 32KB pages */
    gamepak_size = (gamepak_size + 0x7FFF) & ~0x7FFF;

    /* Map ROM pages directly into memory_map_read
       (don't use load_gamepak_page which relies on file I/O) */
    u32 rom_blocks = gamepak_size >> 15;
    u32 i, j;
    for (i = 0; i < block; i++) {
        for (j = 0; j < 32 && i * 32 + j < rom_blocks; j++) {
            u32 phyn = i * 32 + j;
            u8 *blkptr = &gamepak_buffers[i][32 * 1024 * j];
            /* Map to 0x08000000, 0x0A000000, 0x0C000000 regions with mirroring */
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

/* --- input_driver stubs (ESP32 platform code) --- */
#ifdef ESP_PLATFORM
/* These are only needed on ESP32 */
#endif

/* --- Netplay stubs (serial/rfu) --- */
u32 netplay_client_id = 0;
u32 netplay_num_clients = 1;
void netpacket_send(u32 flags, const void *buf, u32 len) {
    (void)flags; (void)buf; (void)len;
}
void netpacket_poll_receive(void) {}
