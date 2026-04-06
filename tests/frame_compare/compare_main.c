/*
 * Frame Comparison Test — main entry point
 *
 * Runs the GBA emulator for N frames and dumps each rendered frame's
 * pixel data to a binary file.  Built in two variants:
 *
 *   test_single  (no DUAL_CORE_PPU)  — renders inline at each HBlank,
 *                                       same as the original single-core path.
 *
 *   test_dual    (DUAL_CORE_PPU)     — captures per-scanline snapshots
 *                                       and replays them through the
 *                                       dual-core renderer API at VBlank.
 *
 * Comparing the two output files reveals any pixel divergence caused
 * by the snapshot / redirect mechanism.
 *
 * Usage: ./test_single [--interp] [bios.bin] <rom.gba> [frames] [output.bin]
 *        ./test_dual   [--interp] [bios.bin] <rom.gba> [frames] [output.bin]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"

/* Defined in compare_stubs.c */
extern void harness_load_rom_direct(const char *path);
extern void harness_load_bios_direct(const char *path);

/* Forward declarations from gpsp */
extern void init_main(void);
extern void reset_gba(void);
extern u32  execute_arm_translate(u32 cycles);
extern void execute_arm(u32 cycles);
extern void init_emitter(bool must_swap);
extern void clear_gamepak_stickybits(void);

/* Screen buffer — video.cc's update_scanline() writes here */
u16 screen_pixels[240 * 161];
extern u16 *gba_screen_pixels;

u32 num_skipped_frames = 0;

#ifdef DUAL_CORE_PPU
/* Threaded PPU simulation (ppu_sim.c) */
extern void ppu_sim_init(void);
extern void ppu_sim_wait_render(void);
extern u16 *ppu_sim_render_fb(void);
extern void ppu_sim_shutdown(void);
extern void ppu_pipeline_begin_frame(void);
extern void ppu_sim_get_last_crc(uint32_t *oam_crc, uint32_t *io0_crc);
/* submit_scanline and end_frame are called from within update_gba() */
#endif

/* Frame dump state */
static FILE       *s_dump_fp;
static FILE       *s_crc_fp;
static uint32_t    s_dump_frame;
static uint32_t    s_dump_total;

static uint32_t crc32_simple(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

/* Called after each frame completes (from update_gba via frame_complete). */
static void dump_current_frame(void)
{
    if (!s_dump_fp)
        return;

    fwrite(gba_screen_pixels, sizeof(u16), 240 * 160, s_dump_fp);
    s_dump_frame++;

    if ((s_dump_frame % 600) == 0) {
        printf("[compare] frame %u / %u (%.1f%%)\n",
               s_dump_frame, s_dump_total,
               100.0f * s_dump_frame / s_dump_total);
        fflush(stdout);
    }
}

int main(int argc, char **argv)
{
    const char *bios_path   = NULL;
    const char *rom_path    = NULL;
    const char *output_path = NULL;
    int frames = 18000;   /* ~5 min */
    int use_jit = 1;

    /* --- Parse arguments --- */
    int argi = 1;

    if (argc < 2) {
        printf("Usage: %s [--interp] [bios.bin] <rom.gba> [frames] [output.bin]\n",
               argv[0]);
        return 1;
    }

    if (argi < argc && strcmp(argv[argi], "--interp") == 0) {
        use_jit = 0;
        argi++;
    }

    int remaining = argc - argi;
    switch (remaining) {
    case 1:
        rom_path = argv[argi];
        break;
    case 2:
        bios_path = argv[argi];
        rom_path  = argv[argi + 1];
        break;
    case 3:
        bios_path = argv[argi];
        rom_path  = argv[argi + 1];
        frames    = atoi(argv[argi + 2]);
        break;
    case 4:
    default:
        bios_path   = argv[argi];
        rom_path    = argv[argi + 1];
        frames      = atoi(argv[argi + 2]);
        output_path = argv[argi + 3];
        break;
    }

    /* Default output filenames */
    if (!output_path) {
#ifdef DUAL_CORE_PPU
        output_path = "frames_dual.bin";
#else
        output_path = "frames_single.bin";
#endif
    }

#ifdef DUAL_CORE_PPU
    printf("[compare] Mode: DUAL-CORE SIMULATION\n");
#else
    printf("[compare] Mode: SINGLE-CORE (reference)\n");
#endif
    printf("[compare] ROM:    %s\n", rom_path);
    if (bios_path)
        printf("[compare] BIOS:   %s\n", bios_path);
    printf("[compare] Frames: %d\n", frames);
    printf("[compare] Output: %s\n", output_path);
    printf("[compare] Engine: %s\n", use_jit ? "JIT" : "Interpreter");
    fflush(stdout);

    /* --- Initialize emulator --- */
    printf("[compare] init_gamepak_buffer...\n"); fflush(stdout);
    init_gamepak_buffer();

#ifdef HAVE_DYNAREC
    /* Allocate JIT caches via mmap (RWX) — MUST be done before reset_gba()
     * because init_dynarec_caches() (called from init_main/reset_gba)
     * dereferences rom_translation_cache to set rom_translation_ptr. */
    {
        extern void *map_jit_block(unsigned size);
        extern u8* rom_translation_cache;
        extern u8* ram_translation_cache;
        rom_translation_cache = (u8 *)map_jit_block(
            ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE);
        if (!rom_translation_cache) {
            printf("[compare] ERROR: failed to allocate JIT caches\n");
            return 1;
        }
        ram_translation_cache = &rom_translation_cache[ROM_TRANSLATION_CACHE_SIZE];
        printf("[compare] JIT caches allocated at %p (ROM %u + RAM %u bytes)\n",
               (void *)rom_translation_cache,
               ROM_TRANSLATION_CACHE_SIZE, RAM_TRANSLATION_CACHE_SIZE);
    }
#endif

    printf("[compare] reset_gba...\n"); fflush(stdout);
    reset_gba();

    if (bios_path)
        harness_load_bios_direct(bios_path);

    printf("[compare] loading ROM...\n"); fflush(stdout);
    harness_load_rom_direct(rom_path);

    gba_screen_pixels = screen_pixels;

#ifdef HAVE_DYNAREC
    init_emitter(gamepak_must_swap());
    dynarec_enable = use_jit;
#else
    dynarec_enable = 0;
    (void)use_jit;
#endif

#ifdef DUAL_CORE_PPU
    ppu_sim_init();
#endif

    /* --- Open output file --- */
    s_dump_fp    = fopen(output_path, "wb");
    s_dump_frame = 0;
    s_dump_total = (uint32_t)frames;
    if (!s_dump_fp) {
        perror("[compare] cannot open output file");
        return 1;
    }

#ifdef DUAL_CORE_PPU
    s_crc_fp = fopen("crc_dual.bin", "wb");
    if (!s_crc_fp) {
        perror("[compare] cannot open CRC file");
        return 1;
    }
#endif

    printf("[compare] Starting emulation...\n"); fflush(stdout);

    /* --- Main loop --- */
    for (int f = 0; f < frames; f++) {
#ifdef DUAL_CORE_PPU
        ppu_pipeline_begin_frame();
#endif

        if (dynarec_enable) {
            execute_arm_translate(execute_cycles);
        } else {
            clear_gamepak_stickybits();
            execute_arm(execute_cycles);
        }

#ifdef DUAL_CORE_PPU
        /* Wait for render thread to finish this frame, then dump
         * from the render thread's private framebuffer. */
        ppu_sim_wait_render();
        {
            u16 *rfb = ppu_sim_render_fb();
            fwrite(rfb, sizeof(u16), 240 * 160, s_dump_fp);

            uint32_t oam_crc, io0_crc;
            ppu_sim_get_last_crc(&oam_crc, &io0_crc);
            uint32_t rec[4] = {
                (uint32_t)f,
                oam_crc,
                io0_crc,
                crc32_simple(rfb, 240 * 160 * sizeof(u16)),
            };
            fwrite(rec, sizeof(rec), 1, s_crc_fp);
        }
        s_dump_frame++;
        if ((s_dump_frame % 600) == 0) {
            printf("[compare] frame %u / %u (%.1f%%)\n",
                   s_dump_frame, s_dump_total,
                   100.0f * s_dump_frame / s_dump_total);
            fflush(stdout);
        }
#else
        dump_current_frame();
#endif
    }

    /* --- Cleanup --- */
#ifdef DUAL_CORE_PPU
    ppu_sim_shutdown();
    if (s_crc_fp) fclose(s_crc_fp);
#endif
    fclose(s_dump_fp);
    printf("[compare] Done. Wrote %u frames to %s\n",
           s_dump_frame, output_path);
    return 0;
}
