/*
 * Unified GBA Test Harness — main entry point
 *
 * Runs the GBA emulator for N frames and captures output.  Can be built
 * for any supported architecture (x86/ARM/RISC-V) and optionally enables
 * dual-core PPU simulation and/or instruction tracing.
 *
 * Usage: ./test_gba_<arch> [flags] [bios.bin] <rom.gba> [frames] [output.bin]
 *
 * Flags:
 *   --interp        Use interpreter instead of JIT
 *   --regs          Print register state every 100 frames
 *   --dump-from N   Dump individual frame files starting at frame N
 *
 * Outputs:
 *   frames_*.bin    240×160 u16 RGB565LE pixel stream (all frames concatenated)
 *   audio_*.pcm     s16le stereo @ 65536 Hz
 *   crc_*.bin       Per-frame CRC records (dual-core mode only)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "common.h"
#include "sound.h"

/* Defined in harness_stubs.c */
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
extern void     ppu_sim_init(void);
extern void     ppu_sim_wait_render(void);
extern u16     *ppu_sim_render_fb(void);
extern void     ppu_sim_shutdown(void);
extern void     ppu_sim_get_last_crc(uint32_t *oam_crc, uint32_t *io0_crc);
extern uint32_t ppu_sim_get_audio(int16_t **samples);
#endif

/* ── state ─────────────────────────────────────────────────────────── */

static FILE       *s_dump_fp;
static FILE       *s_crc_fp;
static FILE       *s_audio_fp;
static uint32_t    s_dump_frame;
static uint32_t    s_dump_total;

/* Audio collection (single-core path only — dual-core uses ppu_sim) */
#ifndef DUAL_CORE_PPU
#define AUDIO_SPF_MAX   1200
static int16_t     s_audio_buf[AUDIO_SPF_MAX * 2];
static float       s_audio_spf;
static float       s_audio_frac;
#endif

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

static void print_regs(void)
{
    for (int i = 0; i < 16; i++) {
        printf("r%-2d=%08x ", i, reg[i]);
        if ((i & 3) == 3) printf("\n");
    }
    printf("cpsr=%08x  spsr=%08x\n", reg[REG_CPSR], spsr[reg[CPU_MODE]]);
}

/* Dump a single frame to an individual file (--dump-from) */
static void dump_frame_file(int frame_num, const u16 *pixels, const char *tag)
{
    char path[256];
    snprintf(path, sizeof(path), "frames_%s/frame_%05d.raw", tag, frame_num);
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    fwrite(pixels, sizeof(u16), 240 * 160, fp);
    fclose(fp);
}

int main(int argc, char **argv)
{
    const char *bios_path   = NULL;
    const char *rom_path    = NULL;
    const char *output_path = NULL;
    int frames    = 18000;   /* ~5 min */
    int use_jit   = 1;
    int show_regs = 0;
    int dump_from = -1;

    if (argc < 2) {
        printf("Usage: %s [flags] [bios.bin] <rom.gba> [frames] [output.bin]\n"
               "  --interp        interpreter mode\n"
               "  --regs          print registers every 100 frames\n"
               "  --dump-from N   dump individual frame files from frame N\n",
               argv[0]);
        return 1;
    }

    /* --- Parse flags --- */
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-' && argv[argi][1] == '-') {
        if (strcmp(argv[argi], "--interp") == 0) {
            use_jit = 0; argi++;
        } else if (strcmp(argv[argi], "--regs") == 0) {
            show_regs = 1; argi++;
        } else if (strcmp(argv[argi], "--dump-from") == 0 && argi + 1 < argc) {
            dump_from = atoi(argv[argi + 1]); argi += 2;
        } else {
            break;
        }
    }

    /* --- Positional arguments --- */
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
    case 4: default:
        bios_path   = argv[argi];
        rom_path    = argv[argi + 1];
        frames      = atoi(argv[argi + 2]);
        output_path = argv[argi + 3];
        break;
    }

    if (!rom_path) {
        printf("[harness] ERROR: missing ROM path\n");
        return 1;
    }

    /* --- Default filenames --- */
    const char *tag;
#ifdef DUAL_CORE_PPU
    tag = "dual";
    if (!output_path) output_path = "frames_dual.bin";
#else
    tag = "single";
    if (!output_path) output_path = "frames_single.bin";
#endif

    printf("[harness] Mode:   %s\n",
#ifdef DUAL_CORE_PPU
           "DUAL-CORE SIMULATION"
#else
           "SINGLE-CORE"
#endif
    );
    printf("[harness] ROM:    %s\n", rom_path);
    if (bios_path)
        printf("[harness] BIOS:   %s\n", bios_path);
    printf("[harness] Frames: %d\n", frames);
    printf("[harness] Output: %s\n", output_path);
    printf("[harness] Engine: %s\n", use_jit ? "JIT" : "Interpreter");
    fflush(stdout);

    /* --- Initialize emulator --- */
    printf("[harness] init_gamepak_buffer...\n"); fflush(stdout);
    init_gamepak_buffer();

#if defined(MMAP_JIT_CACHE) && defined(HAVE_DYNAREC)
    {
        extern void *map_jit_block(unsigned size);
        extern u8 *rom_translation_cache;
        extern u8 *ram_translation_cache;
        rom_translation_cache = (u8 *)map_jit_block(
            ROM_TRANSLATION_CACHE_SIZE + RAM_TRANSLATION_CACHE_SIZE);
        if (!rom_translation_cache) {
            printf("[harness] ERROR: failed to allocate JIT caches\n");
            return 1;
        }
        ram_translation_cache = &rom_translation_cache[ROM_TRANSLATION_CACHE_SIZE];
        printf("[harness] JIT caches at %p (ROM %u + RAM %u)\n",
               (void *)rom_translation_cache,
               ROM_TRANSLATION_CACHE_SIZE, RAM_TRANSLATION_CACHE_SIZE);
    }
#elif defined(HAVE_DYNAREC)
    /* Non-mmap JIT cache: mark static arrays RWX */
    {
        #include <sys/mman.h>
        extern u8 rom_translation_cache[];
        extern u8 ram_translation_cache[];
        uintptr_t page = ~(uintptr_t)(4096 - 1);
        uintptr_t rs = (uintptr_t)rom_translation_cache & page;
        uintptr_t rr = (uintptr_t)ram_translation_cache & page;
        size_t rl = (ROM_TRANSLATION_CACHE_SIZE + ((uintptr_t)rom_translation_cache - rs) + 4095) & ~(size_t)4095;
        size_t al = (RAM_TRANSLATION_CACHE_SIZE + ((uintptr_t)ram_translation_cache - rr) + 4095) & ~(size_t)4095;
        mprotect((void *)rs, rl, PROT_READ | PROT_WRITE | PROT_EXEC);
        mprotect((void *)rr, al, PROT_READ | PROT_WRITE | PROT_EXEC);
        printf("[harness] translation caches marked RWX\n");
    }
#endif

    printf("[harness] reset_gba...\n"); fflush(stdout);
    reset_gba();
    init_sound();

    if (bios_path)
        harness_load_bios_direct(bios_path);

    printf("[harness] loading ROM...\n"); fflush(stdout);
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

    /* --- Open output files --- */
    s_dump_fp    = fopen(output_path, "wb");
    s_dump_frame = 0;
    s_dump_total = (uint32_t)frames;
    if (!s_dump_fp) {
        perror("[harness] cannot open output file");
        return 1;
    }

    /* Audio PCM output */
    {
        char apath[64];
        snprintf(apath, sizeof(apath), "audio_%s.pcm", tag);
        s_audio_fp = fopen(apath, "wb");
        if (!s_audio_fp) {
            perror("[harness] cannot open audio file");
            return 1;
        }
#ifndef DUAL_CORE_PPU
        s_audio_spf = (float)GBA_SOUND_FREQUENCY / 59.7275f;
        s_audio_frac = 0.0f;
#endif
        printf("[harness] Audio:  %s (%.1f samples/frame)\n",
               apath, (float)GBA_SOUND_FREQUENCY / 59.7275f);
        fflush(stdout);
    }

#ifdef DUAL_CORE_PPU
    s_crc_fp = fopen("crc_dual.bin", "wb");
#endif

    /* Individual frame dump directory */
    if (dump_from >= 0) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "mkdir -p frames_%s", tag);
        system(cmd);
        printf("[harness] Dumping individual frames from %d into frames_%s/\n",
               dump_from, tag);
    }

    if (show_regs) {
        printf("[harness] Initial registers:\n");
        print_regs();
    }

    /* --- Per-frame timing --- */
    double *frame_times_us = (double *)calloc(frames, sizeof(double));

    printf("[harness] Starting emulation...\n"); fflush(stdout);

    struct timespec ts_total_start, ts_total_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_total_start);

    /* --- Main loop --- */
    for (int f = 0; f < frames; f++) {
        struct timespec ts_start, ts_end;
        clock_gettime(CLOCK_MONOTONIC, &ts_start);
#ifdef HAVE_DYNAREC
        if (dynarec_enable) {
            execute_arm_translate(execute_cycles);
        } else
#endif
        {
            clear_gamepak_stickybits();
            execute_arm(execute_cycles);
        }

        const u16 *pixels;

#ifdef DUAL_CORE_PPU
        ppu_sim_wait_render();
        pixels = ppu_sim_render_fb();

        fwrite(pixels, sizeof(u16), 240 * 160, s_dump_fp);

        if (s_crc_fp) {
            uint32_t oam_crc, io0_crc;
            ppu_sim_get_last_crc(&oam_crc, &io0_crc);
            uint32_t rec[4] = {
                (uint32_t)f, oam_crc, io0_crc,
                crc32_simple(pixels, 240 * 160 * sizeof(u16)),
            };
            fwrite(rec, sizeof(rec), 1, s_crc_fp);
        }
#else
        pixels = gba_screen_pixels;
        fwrite(pixels, sizeof(u16), 240 * 160, s_dump_fp);
#endif

        s_dump_frame++;

        /* Progress + optional register dump */
        if ((s_dump_frame % 600) == 0) {
            printf("[harness] frame %u / %u (%.1f%%)\n",
                   s_dump_frame, s_dump_total,
                   100.0f * s_dump_frame / s_dump_total);
            fflush(stdout);
        }
        if (show_regs && (f % 100 == 0))
            print_regs();

        /* Individual frame file dump */
        if (dump_from >= 0 && f >= dump_from)
            dump_frame_file(f, pixels, tag);

        /* --- Audio --- */
#ifdef DUAL_CORE_PPU
        {
            int16_t *samples;
            uint32_t n = ppu_sim_get_audio(&samples);
            if (n > 0)
                fwrite(samples, sizeof(int16_t), n * 2, s_audio_fp);
        }
#else
        {
            render_gbc_sound();
            uint32_t n = (uint32_t)s_audio_spf;
            s_audio_frac += s_audio_spf - (float)n;
            if (s_audio_frac >= 1.0f) { n++; s_audio_frac -= 1.0f; }
            if (n > AUDIO_SPF_MAX) n = AUDIO_SPF_MAX;

            uint32_t got = sound_read_samples(s_audio_buf, n);
            if (got < n)
                memset(s_audio_buf + got * 2, 0, (n - got) * 2 * sizeof(int16_t));
            fwrite(s_audio_buf, sizeof(int16_t), n * 2, s_audio_fp);
        }
#endif

        clock_gettime(CLOCK_MONOTONIC, &ts_end);
        frame_times_us[f] = (ts_end.tv_sec - ts_start.tv_sec) * 1e6
                          + (ts_end.tv_nsec - ts_start.tv_nsec) / 1e3;
    }

    clock_gettime(CLOCK_MONOTONIC, &ts_total_end);
    double total_sec = (ts_total_end.tv_sec - ts_total_start.tv_sec)
                     + (ts_total_end.tv_nsec - ts_total_start.tv_nsec) / 1e9;

    /* --- Frame time statistics --- */
    {
        /* Sort frame_times_us for percentile computation */
        for (int i = 0; i < frames - 1; i++) {
            for (int j = i + 1; j < frames; j++) {
                if (frame_times_us[j] < frame_times_us[i]) {
                    double tmp = frame_times_us[i];
                    frame_times_us[i] = frame_times_us[j];
                    frame_times_us[j] = tmp;
                }
            }
        }

        double sum = 0;
        for (int i = 0; i < frames; i++) sum += frame_times_us[i];
        double avg_us = sum / frames;
        int p99_idx = (int)(frames * 0.99);
        if (p99_idx >= frames) p99_idx = frames - 1;
        double tp99_us = frame_times_us[p99_idx];
        double median_us = frame_times_us[frames / 2];
        double min_us = frame_times_us[0];
        double max_us = frame_times_us[frames - 1];

        printf("\n[harness] ═══════════════════════════════════════════\n");
        printf("[harness]  TIMING REPORT  (%d frames)\n", frames);
        printf("[harness] ═══════════════════════════════════════════\n");
        printf("[harness]  Total wall time : %.3f s\n", total_sec);
        printf("[harness]  Avg  frame time : %.1f us (%.2f ms)\n", avg_us, avg_us / 1000.0);
        printf("[harness]  Median           : %.1f us (%.2f ms)\n", median_us, median_us / 1000.0);
        printf("[harness]  Min              : %.1f us (%.2f ms)\n", min_us, min_us / 1000.0);
        printf("[harness]  Max              : %.1f us (%.2f ms)\n", max_us, max_us / 1000.0);
        printf("[harness]  TP99             : %.1f us (%.2f ms)\n", tp99_us, tp99_us / 1000.0);
        printf("[harness]  Effective FPS    : %.2f\n", frames / total_sec);
        printf("[harness] ═══════════════════════════════════════════\n\n");
    }
    free(frame_times_us);

    /* --- Cleanup --- */
#ifdef DUAL_CORE_PPU
    ppu_sim_shutdown();
    if (s_crc_fp) fclose(s_crc_fp);
#endif
    if (s_audio_fp) fclose(s_audio_fp);
    fclose(s_dump_fp);
    printf("[harness] Done. Wrote %u frames to %s\n",
           s_dump_frame, output_path);
    return 0;
}
