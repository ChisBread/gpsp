/*
 * Standalone GBA emulator harness for QEMU RV32 — main entry
 *
 * Usage: ./test_gba [bios.bin] <rom.gba>
 *
 * Runs the GBA emulator (interpreter or JIT depending on build mode)
 * for a fixed number of frames and dumps register/flag state after each
 * ARM instruction for comparison/debugging.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "common.h"

/* Defined in harness_stubs.c */
extern void harness_load_rom_direct(const char *path);
extern void harness_load_bios_direct(const char *path);

/* Forward declarations from gpsp */
extern void init_main(void);
extern void reset_gba(void);
extern u32  update_gba(int remaining_cycles);
extern u32  execute_arm_translate(u32 cycles);
extern void execute_arm(u32 cycles);
extern void init_emitter(bool must_swap);
extern void clear_gamepak_stickybits(void);

/* Screen buffer (video.cc needs this) */
u16 screen_pixels[240 * 161];

/* gba_screen_pixels is defined in video.cc as NULL, we must point it here */
extern u16 *gba_screen_pixels;

/* Some globals that the emulator expects */
u32 num_skipped_frames = 0;

/* Frame buffer dump: writes raw 240x160 RGB565 files */
static void dump_framebuffer(int frame_num, const char *tag)
{
    char path[256];
    snprintf(path, sizeof(path), "frames_%s/frame_%05d.raw", tag, frame_num);
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fwrite(screen_pixels, sizeof(u16), 240 * 160, fp);
    fclose(fp);
}

static void print_regs(void)
{
    int i;
    for (i = 0; i < 16; i++) {
        printf("r%-2d=%08x ", i, reg[i]);
        if ((i & 3) == 3) printf("\n");
    }
    printf("cpsr=%08x  spsr=%08x\n", reg[REG_CPSR], spsr[reg[CPU_MODE]]);
    printf("cycles=%d  pc=%08x\n", reg[REG_CPSR], reg[REG_PC]);
}

int main(int argc, char **argv)
{
    const char *bios_path = NULL;
    const char *rom_path = NULL;
    int frames = 10;
    int use_jit = 1; /* default: JIT */
    int dump_from = -1; /* frame to start dumping framebuffer, -1 = disabled */

    if (argc < 2) {
        printf("Usage: %s [--interp] [--dump-from N] [bios.bin] <rom.gba> [frames]\n", argv[0]);
        return 1;
    }

    /* Parse flags */
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] == '-') {
        if (strcmp(argv[argi], "--interp") == 0) {
            use_jit = 0;
            argi++;
        } else if (strcmp(argv[argi], "--dump-from") == 0 && argi + 1 < argc) {
            dump_from = atoi(argv[argi + 1]);
            argi += 2;
        } else {
            break;
        }
    }

    int remaining = argc - argi;
    if (remaining == 1) {
        rom_path = argv[argi];
    } else if (remaining == 2) {
        rom_path = argv[argi];
        frames = atoi(argv[argi + 1]);
    } else if (remaining >= 3) {
        bios_path = argv[argi];
        rom_path = argv[argi + 1];
        frames = atoi(argv[argi + 2]);
    }

    if (!rom_path) {
        printf("[harness] ERROR: missing ROM path\n");
        return 1;
    }

    printf("[harness] ROM: %s\n", rom_path);
    if (bios_path)
        printf("[harness] BIOS: %s\n", bios_path);
    printf("[harness] Frames: %d\n", frames);
    printf("[harness] Mode: %s\n", use_jit ? "JIT" : "Interpreter");
    fflush(stdout);

    /* Initialize memory subsystem */
    printf("[harness] init_gamepak_buffer...\n"); fflush(stdout);
    init_gamepak_buffer();

#ifdef MMAP_JIT_CACHE
    /* Allocate JIT caches via mmap (RWX) BEFORE reset_gba(), since
       init_dynarec_caches() dereferences rom_translation_cache. */
    {
        extern void *map_jit_block(unsigned size);
        extern u8* rom_translation_cache;
        extern u8* ram_translation_cache;
        rom_translation_cache = (u8 *)map_jit_block(
            ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE);
        if (!rom_translation_cache) {
            printf("[harness] ERROR: failed to allocate JIT caches\n");
            return 1;
        }
        ram_translation_cache = &rom_translation_cache[ROM_TRANSLATION_CACHE_SIZE];
        printf("[harness] JIT caches allocated at %p (ROM %u + RAM %u bytes)\n",
               (void *)rom_translation_cache,
               ROM_TRANSLATION_CACHE_SIZE, RAM_TRANSLATION_CACHE_SIZE);
    }
#endif

    /* reset_gba calls init_memory, init_main, init_cpu, reset_sound */
    printf("[harness] reset_gba...\n"); fflush(stdout);
    reset_gba();
    printf("[harness] reset_gba done.\n"); fflush(stdout);

    /* Load BIOS */
    if (bios_path) {
        harness_load_bios_direct(bios_path);
    }
    /* otherwise open_gba_bios_rom[] is used (already in bios_data.S) */

    /* Load ROM directly into gamepak buffers */
    printf("[harness] loading ROM...\n"); fflush(stdout);
    harness_load_rom_direct(rom_path);

    /* Set up screen buffer for video rendering */
    gba_screen_pixels = screen_pixels;

    /* Mark translation caches as executable.
       With MMAP_JIT_CACHE, caches are mmap'd with PROT_EXEC already. */
#ifndef MMAP_JIT_CACHE
    {
        extern u8 rom_translation_cache[];
        extern u8 ram_translation_cache[];
        uintptr_t page_mask = ~(uintptr_t)(4096 - 1);
        uintptr_t rom_start = (uintptr_t)rom_translation_cache & page_mask;
        uintptr_t ram_start = (uintptr_t)ram_translation_cache & page_mask;
        size_t rom_len = ROM_TRANSLATION_CACHE_SIZE + ((uintptr_t)rom_translation_cache - rom_start);
        size_t ram_len = RAM_TRANSLATION_CACHE_SIZE + ((uintptr_t)ram_translation_cache - ram_start);
        rom_len = (rom_len + 4095) & ~(size_t)4095;
        ram_len = (ram_len + 4095) & ~(size_t)4095;
        if (mprotect((void*)rom_start, rom_len, PROT_READ|PROT_WRITE|PROT_EXEC) != 0) {
            perror("[harness] mprotect rom_translation_cache");
        }
        if (mprotect((void*)ram_start, ram_len, PROT_READ|PROT_WRITE|PROT_EXEC) != 0) {
            perror("[harness] mprotect ram_translation_cache");
        }
        printf("[harness] translation caches marked RWX\n");
    }
#endif
    init_emitter(gamepak_must_swap());
    dynarec_enable = use_jit;

    printf("[harness] Initial state:\n");
    print_regs();
    printf("[harness] Starting emulation for %d frames...\n", frames);
    fflush(stdout);

    /* Create frame dump directory if needed */
    if (dump_from >= 0) {
        const char *tag = use_jit ? "jit" : "interp";
        char mkdir_cmd[256];
        snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p frames_%s", tag);
        system(mkdir_cmd);
        printf("[harness] Will dump framebuffers from frame %d into frames_%s/\n",
               dump_from, tag);
        fflush(stdout);
    }

    int f;
    for (f = 0; f < frames; f++) {
        if (dynarec_enable) {
            execute_arm_translate(execute_cycles);
        } else {
            clear_gamepak_stickybits();
            execute_arm(execute_cycles);
        }
        if (f % 100 == 0 || f < 5) {
            printf("[harness] Frame %d done, PC=%08x CPSR=%08x cycles_left=%d\n",
                   f, reg[REG_PC], reg[REG_CPSR], execute_cycles);
            fflush(stdout);
        }
        if (dump_from >= 0 && f >= dump_from) {
            dump_framebuffer(f, use_jit ? "jit" : "interp");
        }
    }

    printf("[harness] Final state:\n");
    print_regs();

    return 0;
}
