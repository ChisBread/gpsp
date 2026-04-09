/*
 * gpsp — Dual-core PPU render pipeline (ESP32-P4)
 *
 * Architecture:
 *
 *   Core 1 (CPU/emu):                    Core 0 (render):
 *   ─────────────────                    ────────────────
 *   for line 0..159:                     ← wait for frame + pace tick
 *     submit_scanline()                  ppu_begin_render_frame()
 *       ├ snapshot IO regs                 ├ copy OAM  → DRAM
 *       └ capture OAM_UPDATED              └ copy pal  → DRAM
 *     HBlank DMA (may write BG2X)        for line 0..159:
 *   flush_frame()  [vcount 228]            compute affine from IO diffs
 *     ├ render_gbc_sound()                 ppu_begin_render_line()
 *     ├ drain audio → frame desc           update_scanline()
 *     ├ snapshot OAM + palette           ppu_end_render_frame()
 *     ├ snapshot VRAM                     PPA scale + LCD output
 *     ├ cache writeback                   play audio from frame desc
 *     ├ enqueue →─────────────────────→   release buffer
 *     └ acquire next buffer (blocks)
 *
 * Key design decisions:
 *
 *   1. Affine references are reconstructed on the render core from
 *      per-scanline IO snapshots.  The render core initialises from
 *      BG2X/Y at line 0 and accumulates +=PB/PD each line.  HBlank
 *      DMA writes to BG2X are detected by comparing adjacent IO
 *      snapshots — a change resets the accumulator.
 *
 *   2. OAM, palette, and VRAM are all snapshotted once in flush_frame()
 *      at vcount 228, after all VBlank DMA and CPU writes complete.
 *      Mid-frame OAM writes set a per-line dirty flag so order_obj()
 *      re-sorts on the render side.
 *
 *   3. IO registers are snapshotted per scanline (128 B) in
 *      submit_scanline().  The render core reads its own copy.
 *
 *   4. Audio samples are generated and drained into the frame
 *      descriptor in flush_frame(), guaranteeing 1:1 AV sync.
 *      Frame pacing runs on the render core so the CPU never
 *      idles waiting for the 59.7275 Hz tick.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"

#include "ppu_pipeline.h"

#ifdef DUAL_CORE_PPU

#include "audio_driver.h"
#include "gba_memory.h"
#include "sound.h"
#include "video.h"
#include "video_driver.h"
#include "storage.h"

#define TAG "ppu_pipe"

/* ── constants ─────────────────────────────────────────────────────── */

#define GBA_LINES                  160
#define IO_SNAP_U16                64      /* io_registers[0..63] */
#define OAM_U16                    512     /* 1 KB */
#define PAL_U16                    512     /* 1 KB */
#define VRAM_SNAPSHOT_BYTES        (1024 * 96)
#define BUF_COUNT                  2
#define SHUTDOWN_SENTINEL          0xFFFFFFFFu
#define AUDIO_QUEUE_DEPTH          4
#define AUDIO_SHUTDOWN_SENTINEL    0xFFFFFFFFu

#define GBA_FRAME_RATE             59.7275f
#define GBA_SOUND_HZ               GBA_SOUND_FREQUENCY
#define AUDIO_FRAME_MAX            ((GBA_SOUND_HZ / 50) + 1)

/* ── per-scanline descriptor ───────────────────────────────────────── */

typedef struct {
    u16  io[IO_SNAP_U16];       /* 128 B — full IO register snapshot     */
    u32  oam_updated;           /*   4 B — OAM dirty flag               */
} ppu_line_t;

/* ── per-frame descriptor (lives in PSRAM, double-buffered) ────────── */

typedef struct {
    ppu_line_t  line[GBA_LINES];
    u16         oam[OAM_U16];           /* OAM snapshot (1 KB)   */
    u16         palette[PAL_U16];       /* palette snapshot (1 KB) */
    u8          vram[VRAM_SNAPSHOT_BYTES];
    uint32_t    audio_frames;           /* audio frames in this packet */
    int16_t     audio_samples[AUDIO_FRAME_MAX * 2]; /* stereo interleaved */
    u8          skip;                   /* non-zero → skip rendering */
    u8          pad[3];
    int         next_line;              /* write cursor 0..160 */
} ppu_frame_t;

typedef struct {
    uint32_t frames;
    int16_t samples[AUDIO_FRAME_MAX * 2];
} audio_packet_t;

/* ── module state ──────────────────────────────────────────────────── */

static GPSP_EXTRAM_BSS ppu_frame_t s_frames[BUF_COUNT] __attribute__((aligned(64)));
static GPSP_EXTRAM_BSS u16 s_render_fb_storage[GBA_SCREEN_WIDTH * (GBA_SCREEN_HEIGHT + 1)] __attribute__((aligned(64)));
static int               s_wr;                /* write buffer index   */

static SemaphoreHandle_t s_buf_sem[BUF_COUNT]; /* "buffer free" tokens */
static QueueHandle_t     s_queue;              /* completed-buf index  */
static QueueHandle_t     s_audio_queue;        /* queued audio packet indices */
static QueueHandle_t     s_audio_free_queue;   /* free audio packet indices */
static TaskHandle_t      s_task;
static TaskHandle_t      s_audio_task;

static bool              s_audio_on;
static float             s_audio_spf;          /* samples per frame    */
static float             s_audio_frac;
static GPSP_EXTRAM_BSS audio_packet_t    s_audio_packets[AUDIO_QUEUE_DEPTH];

static u16              *s_render_fb = s_render_fb_storage; /* render framebuffer */

/* Render timing stats */
static volatile int64_t  s_stat_scanline_us;
static volatile int64_t  s_stat_video_us;
static volatile int64_t  s_stat_audio_us;
static volatile int64_t  s_stat_wait_render_us;
static volatile int64_t  s_stat_wait_buf_us;
static volatile int64_t  s_stat_wait_pace_us;
static volatile uint32_t s_stat_audio_drop_count;
static volatile uint32_t s_stat_audio_queue_peak;

/* ── frame dump (diagnostic) ───────────────────────────────────────── */

/*
 * Dump rendered pixels + snapshot CRC log to SD card.
 *
 *   /sdcard/dump_pixels.bin — 240×160 u16 pixels per frame
 *   /sdcard/dump_crc.bin   — per-frame: {u32 frame_nr, u32 oam_crc, u32 io0_crc, u32 pixel_crc}
 *
 * Only records frames [DUMP_SKIP .. DUMP_SKIP+DUMP_COUNT).
 */
static uint32_t s_dump_skip;
static uint32_t s_dump_count;

static FILE *s_dump_pixel_fp;
static FILE *s_dump_crc_fp;
static uint32_t s_dump_frame_nr;

/* Simple CRC-32 (Castagnoli polynomial, no table — speed doesn't matter here) */
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

static void dump_open(void)
{
    s_dump_skip     = 0;
    s_dump_count    = 0;
    s_dump_frame_nr = 0;
    s_dump_pixel_fp = NULL;
    s_dump_crc_fp   = NULL;
}

static void dump_frame(const ppu_frame_t *f, const u16 *pixels)
{
    if (s_dump_count == 0) return;
    uint32_t nr = s_dump_frame_nr++;
    if (nr < s_dump_skip) return;
    if (nr >= s_dump_skip + s_dump_count) return;

    /* Lazy open on first in-range frame */
    if (!s_dump_pixel_fp) {
        s_dump_pixel_fp = fopen(STORAGE_MOUNT_POINT "/dump_pixels.bin", "wb");
        s_dump_crc_fp   = fopen(STORAGE_MOUNT_POINT "/dump_crc.bin",    "wb");
        if (!s_dump_pixel_fp || !s_dump_crc_fp) {
            ESP_LOGW(TAG, "dump: failed to open files");
            return;
        }
        ESP_LOGI(TAG, "dump: recording started at frame %u", nr);
    }

    fwrite(pixels, sizeof(u16), 240 * 160, s_dump_pixel_fp);

    /* Write compact CRC record: frame_nr, oam_crc, io_line0_crc, pixel_crc */
    uint32_t rec[4] = {
        nr,
        crc32_simple(f->oam, sizeof(f->oam)),
        crc32_simple(f->line[0].io, sizeof(f->line[0].io)),
        crc32_simple(pixels, 240 * 160 * sizeof(u16)),
    };
    fwrite(rec, sizeof(rec), 1, s_dump_crc_fp);

    if (nr == s_dump_skip + s_dump_count - 1) {
        fclose(s_dump_pixel_fp); s_dump_pixel_fp = NULL;
        fclose(s_dump_crc_fp);   s_dump_crc_fp   = NULL;
        ESP_LOGI(TAG, "dump: %u frames written, files closed", s_dump_count);
        s_dump_count = 0;  /* auto-disable after completion */
    }
}

/* ── helpers ───────────────────────────────────────────────────────── */

/* Sign-extend 28-bit value (same logic as video.cc signext28). */
static inline s32 signext28(u32 v)
{
    return (s32)(v << 4) >> 4;
}

/* Collect one frame's worth of audio samples. */
static uint32_t collect_audio(int16_t *buf, size_t max)
{
    if (!s_audio_on)
        return 0;

    uint32_t n = (uint32_t)s_audio_spf;
    s_audio_frac += s_audio_spf - (float)n;
    if (s_audio_frac >= 1.0f) { n++; s_audio_frac -= 1.0f; }
    if (n > max) n = (uint32_t)max;

    uint32_t got = sound_read_samples(buf, n);
    if (got < n)
        memset(buf + got * 2, 0, (n - got) * 2 * sizeof(*buf));
    return n;
}

static void audio_task(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t packet_idx;
        if (xQueueReceive(s_audio_queue, &packet_idx, portMAX_DELAY) != pdTRUE)
            continue;

        if (packet_idx == AUDIO_SHUTDOWN_SENTINEL)
            break;

        audio_packet_t *packet = &s_audio_packets[packet_idx];

        int64_t t0 = esp_timer_get_time();
        esp_err_t err = audio_driver_write(packet->samples, packet->frames);
        int64_t t1 = esp_timer_get_time();
        s_stat_audio_us = t1 - t0;

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "audio write: %s", esp_err_to_name(err));
            s_audio_on = false;
        }

        xQueueSend(s_audio_free_queue, &packet_idx, portMAX_DELAY);
    }

    ESP_LOGI(TAG, "audio task exit");
    vTaskDelete(NULL);
}

/* ── D-cache sync helpers ──────────────────────────────────────────── */

/*
 * ESP32-P4 L1 D-caches are NOT hardware-coherent between cores.
 * The frame descriptor (PSRAM) is written by Core 1 and read by Core 0.
 * Without explicit cache management Core 0 may read stale L1 data →
 * wrong sprite positions / scrolling offsets.
 */

static inline void ppu_dcache_writeback(void *addr, size_t size)
{
    const size_t linemask = 63;  /* 64-byte cache line */
    uintptr_t start = (uintptr_t)addr & ~linemask;
    size_t aligned  = (((uintptr_t)addr + size + linemask) & ~linemask) - start;
    esp_cache_msync((void *)start, aligned, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

static inline void ppu_dcache_invalidate(void *addr, size_t size)
{
    const size_t linemask = 63;
    uintptr_t start = (uintptr_t)addr & ~linemask;
    size_t aligned  = (((uintptr_t)addr + size + linemask) & ~linemask) - start;
    esp_cache_msync((void *)start, aligned, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
}

/* Read a 32-bit IO value from a per-scanline snapshot. */
static inline u32 snap_ioreg32(const u16 *io, int reg)
{
    return io[reg] | ((u32)io[reg + 1] << 16);
}

/* ── render task (Core 0) ──────────────────────────────────────────── */

static void render_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "render task on core %d", xPortGetCoreID());

    for (;;) {
        uint32_t idx;
        if (xQueueReceive(s_queue, &idx, portMAX_DELAY) != pdTRUE)
            continue;
        if (idx == SHUTDOWN_SENTINEL)
            break;

        /* Complete the previous frame's async PPA and wait for DPI
         * VSYNC.  The VSYNC wait (~16.5 ms @ ~60.5 Hz) replaces the
         * separate pace timer — the LCD panel drives the cadence. */
        int64_t tp0 = esp_timer_get_time();
        video_driver_await_frame();
        s_stat_wait_pace_us = esp_timer_get_time() - tp0;

        ppu_frame_t *f = &s_frames[idx];

        ppu_dcache_invalidate(f, sizeof(ppu_frame_t));

        int64_t t0 = esp_timer_get_time(), t1, t2;

        if (!f->skip) {
            gba_screen_pixels = s_render_fb;
            ppu_begin_render_frame(f->oam, f->palette, f->vram);

            u32 saved_skip = skip_next_frame;
            skip_next_frame = 0;

            /* Initialise affine accumulators from line 0's BG2X/BG2Y
             * (equivalent to video_reload_counters at VBlank). */
            s32 aff_x[2], aff_y[2];
            aff_x[0] = signext28(snap_ioreg32(f->line[0].io, REG_BG2X_L));
            aff_y[0] = signext28(snap_ioreg32(f->line[0].io, REG_BG2Y_L));
            aff_x[1] = signext28(snap_ioreg32(f->line[0].io, REG_BG3X_L));
            aff_y[1] = signext28(snap_ioreg32(f->line[0].io, REG_BG3Y_L));

            for (int y = 0; y < GBA_LINES; y++) {
                const u16 *io = f->line[y].io;

                /* Detect mid-frame BG2X/BG2Y/BG3X/BG3Y writes (HBlank
                 * DMA or CPU store) by comparing adjacent IO snapshots.
                 * A change resets the accumulator to the new value. */
                if (y > 0) {
                    const u16 *prev = f->line[y - 1].io;
                    if (io[REG_BG2X_L] != prev[REG_BG2X_L] ||
                        io[REG_BG2X_L + 1] != prev[REG_BG2X_L + 1])
                        aff_x[0] = signext28(snap_ioreg32(io, REG_BG2X_L));
                    if (io[REG_BG2Y_L] != prev[REG_BG2Y_L] ||
                        io[REG_BG2Y_L + 1] != prev[REG_BG2Y_L + 1])
                        aff_y[0] = signext28(snap_ioreg32(io, REG_BG2Y_L));
                    if (io[REG_BG3X_L] != prev[REG_BG3X_L] ||
                        io[REG_BG3X_L + 1] != prev[REG_BG3X_L + 1])
                        aff_x[1] = signext28(snap_ioreg32(io, REG_BG3X_L));
                    if (io[REG_BG3Y_L] != prev[REG_BG3Y_L] ||
                        io[REG_BG3Y_L + 1] != prev[REG_BG3Y_L + 1])
                        aff_y[1] = signext28(snap_ioreg32(io, REG_BG3Y_L));
                }

                s32 line_x[2] = { aff_x[0], aff_x[1] };
                s32 line_y[2] = { aff_y[0], aff_y[1] };

                ppu_begin_render_line(io, line_x, line_y,
                                      y == 0 ? 1 : f->line[y].oam_updated);
                update_scanline();

                /* Accumulate affine for next line (modes 1-5 only). */
                u32 vmode = io[REG_DISPCNT] & 0x07;
                if (vmode) {
                    const u32 mosv = ((io[REG_MOSAIC] >> 4) & 0xF) + 1;

                    if (io[REG_BG2CNT] & 0x40) {
                        if (((u32)y % mosv) == mosv - 1) {
                            aff_x[0] += (s16)io[REG_BG2PB] * mosv;
                            aff_y[0] += (s16)io[REG_BG2PD] * mosv;
                        }
                    } else {
                        aff_x[0] += (s16)io[REG_BG2PB];
                        aff_y[0] += (s16)io[REG_BG2PD];
                    }

                    if (io[REG_BG3CNT] & 0x40) {
                        if (((u32)y % mosv) == mosv - 1) {
                            aff_x[1] += (s16)io[REG_BG3PB] * mosv;
                            aff_y[1] += (s16)io[REG_BG3PD] * mosv;
                        }
                    } else {
                        aff_x[1] += (s16)io[REG_BG3PB];
                        aff_y[1] += (s16)io[REG_BG3PD];
                    }
                }
            }

            skip_next_frame = saved_skip;
            ppu_end_render_frame();

            /* Dump snapshot + rendered pixels for offline comparison */
            dump_frame(f, s_render_fb);

            t1 = esp_timer_get_time();

            /* Rendering is finished at this point: the frame descriptor
             * is no longer read after dump_frame(). Let the emu core
             * start reusing this buffer while video submit pushes the
             * already-rendered framebuffer to the panel. */
            xSemaphoreGive(s_buf_sem[idx]);

            esp_err_t err = video_driver_submit_frame(s_render_fb);
            if (err != ESP_OK)
                ESP_LOGW(TAG, "video submit: %s", esp_err_to_name(err));
        } else {
            t1 = t0;

            /* Skipped frames do not touch the framebuffer. Release the
             * frame descriptor immediately so the emu core can continue. */
            xSemaphoreGive(s_buf_sem[idx]);
        }

        t2 = esp_timer_get_time();

        /* Audio: push pre-captured samples from the frame descriptor
         * to the audio task for I2S output. */
        if (s_audio_on && f->audio_frames > 0 &&
            s_audio_queue && s_audio_free_queue) {
            uint32_t packet_idx;
            if (xQueueReceive(s_audio_free_queue, &packet_idx, 0) == pdTRUE) {
                audio_packet_t *packet = &s_audio_packets[packet_idx];
                uint32_t n = f->audio_frames;
                if (n > AUDIO_FRAME_MAX) n = AUDIO_FRAME_MAX;
                memcpy(packet->samples, f->audio_samples, n * 2 * sizeof(int16_t));
                packet->frames = n;
                if (xQueueSend(s_audio_queue, &packet_idx, 0) != pdTRUE) {
                    xQueueSend(s_audio_free_queue, &packet_idx, 0);
                    s_stat_audio_drop_count++;
                    s_stat_audio_us = 0;
                } else {
                    uint32_t queued = (uint32_t)uxQueueMessagesWaiting(s_audio_queue);
                    if (queued > s_stat_audio_queue_peak)
                        s_stat_audio_queue_peak = queued;
                }
            } else {
                s_stat_audio_us = 0;
            }
        } else {
            s_stat_audio_us = 0;
        }

        s_stat_scanline_us = t1 - t0;
        s_stat_video_us    = t2 - t1;
    }

    ESP_LOGI(TAG, "render task exit");
    vTaskDelete(NULL);
}

/* ── public API ────────────────────────────────────────────────────── */

esp_err_t ppu_pipeline_init(const ppu_pipeline_config_t *cfg)
{
    if (!cfg)
        return ESP_ERR_INVALID_ARG;

    memset(s_frames, 0, sizeof(s_frames));

    s_wr = 0;
    s_render_fb = s_render_fb_storage;
    s_audio_on   = cfg->audio_enabled;
    s_audio_spf  = (float)GBA_SOUND_HZ / GBA_FRAME_RATE;
    s_audio_frac = 0.0f;
    s_stat_scanline_us = s_stat_video_us = s_stat_audio_us = 0;
    s_stat_wait_render_us = s_stat_wait_buf_us = s_stat_wait_pace_us = 0;
    s_stat_audio_drop_count = 0;
    s_stat_audio_queue_peak = 0;

    s_queue = xQueueCreate(BUF_COUNT, sizeof(uint32_t));
    s_audio_queue = s_audio_on ? xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(uint32_t)) : NULL;
    s_audio_free_queue = s_audio_on ? xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(uint32_t)) : NULL;
    for (int i = 0; i < BUF_COUNT; i++)
        s_buf_sem[i] = xSemaphoreCreateBinary();

    if (!s_queue || (s_audio_on && (!s_audio_queue || !s_audio_free_queue)) || !s_buf_sem[0] || !s_buf_sem[1])
        return ESP_ERR_NO_MEM;

    if (s_audio_on) {
        for (uint32_t i = 0; i < AUDIO_QUEUE_DEPTH; i++)
            xQueueSend(s_audio_free_queue, &i, portMAX_DELAY);
    }

    for (int i = 0; i < BUF_COUNT; i++)
        xSemaphoreGive(s_buf_sem[i]);

    /* Pre-acquire buf 0 so the first submit_scanline has a valid buffer
     * without needing a separate begin_frame call. */
    xSemaphoreTake(s_buf_sem[0], 0);
    s_frames[0].next_line = 0;
    s_frames[0].skip      = 0;

    BaseType_t r = xTaskCreatePinnedToCoreWithCaps(
        render_task, "ppu_render",
        cfg->task_stack_size, NULL,
        cfg->task_priority, &s_task,
        cfg->render_core,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (r != pdPASS) return ESP_FAIL;

    if (s_audio_on) {
        r = xTaskCreatePinnedToCoreWithCaps(
            audio_task, "ppu_audio",
            4096, NULL,
            cfg->task_priority > 0 ? cfg->task_priority - 1 : cfg->task_priority,
            &s_audio_task,
            cfg->render_core,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (r != pdPASS) return ESP_FAIL;
    }

    ESP_LOGI(TAG, "init OK (core %d, buf %zu B × %d)",
             cfg->render_core, sizeof(ppu_frame_t), BUF_COUNT);
    dump_open();
    return ESP_OK;
}

void ppu_pipeline_deinit(void)
{
    if (s_task) {
        uint32_t x = SHUTDOWN_SENTINEL;
        xQueueSend(s_queue, &x, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(50));
        s_task = NULL;
    }

    if (s_audio_task && s_audio_queue) {
        uint32_t packet_idx = AUDIO_SHUTDOWN_SENTINEL;
        xQueueSend(s_audio_queue, &packet_idx, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(50));
        s_audio_task = NULL;
    }

    ppu_end_render_frame();

    if (s_queue) { vQueueDelete(s_queue); s_queue = NULL; }
    if (s_audio_queue) { vQueueDelete(s_audio_queue); s_audio_queue = NULL; }
    if (s_audio_free_queue) { vQueueDelete(s_audio_free_queue); s_audio_free_queue = NULL; }
    for (int i = 0; i < BUF_COUNT; i++)
        if (s_buf_sem[i]) { vSemaphoreDelete(s_buf_sem[i]); s_buf_sem[i] = NULL; }
}

esp_err_t ppu_pipeline_reset_pace(void)
{
    /* Pace is now driven by LCD VSYNC — nothing to reset. */
    return ESP_OK;
}

/*
 * submit_scanline — called at each H-Draw → HBlank transition (vcount 0..159).
 *
 * Snapshots IO registers and captures the OAM dirty flag.
 * On the first visible line (y==0), also snapshots VRAM, OAM and palette
 * so that the bulk-data snapshot is taken at the same point in time as
 * the IO snapshot — i.e. AFTER the previous frame's VBlank processing
 * but BEFORE the current frame's HBlank DMAs.  This keeps the tile-map
 * content consistent with the scroll register values captured per-line.
 *
 * After this function returns, HBlank DMA may fire and write BG2X/VRAM.
 */
void ppu_pipeline_submit_scanline(void)
{
    ppu_frame_t *f = &s_frames[s_wr];
    int y = f->next_line;
    if (y >= GBA_LINES) return;

    /* Snapshot bulk data at the start of the visible frame so that
     * VRAM/OAM/palette are coherent with the line-0 IO snapshot. */
    if (y == 0) {
        memcpy(f->oam,     oam_ram,              sizeof(f->oam));
        memcpy(f->palette, palette_ram_converted, sizeof(f->palette));
        memcpy(f->vram,    vram,                  sizeof(f->vram));
    }

    ppu_line_t *L = &f->line[y];

    memcpy(L->io, io_registers, sizeof(L->io));

    L->oam_updated = reg[OAM_UPDATED];
    if (L->oam_updated)
        reg[OAM_UPDATED] = 0;

    f->next_line = y + 1;
}

/*
 * flush_frame — called at vcount == 228 (end of VBlank).
 *
 * Generates audio, flushes L1 D-cache, then queues the frame
 * (with embedded audio) for the render core.
 *
 * NOTE: VRAM/OAM/palette are snapshotted at submit_scanline(y==0)
 * so that their content is coherent with the per-scanline IO snapshots.
 */
void ppu_pipeline_flush_frame(bool skip)
{
    uint32_t done = (uint32_t)s_wr;
    ppu_frame_t *f = &s_frames[done];

    f->skip = skip ? 1 : 0;

    /* Generate and drain audio — even for skipped frames the sound
     * state machine must advance so we don't lose samples. */
    render_gbc_sound();
    f->audio_frames = s_audio_on ? collect_audio(f->audio_samples, AUDIO_FRAME_MAX) : 0;

    asm volatile ("fence rw, rw" ::: "memory");
    ppu_dcache_writeback(f, sizeof(*f));

    s_wr ^= 1;
    xQueueSend(s_queue, &done, portMAX_DELAY);

    /* Acquire the next buffer.  Blocks if the render core hasn't
     * finished the previous frame occupying this slot. */
    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(s_buf_sem[s_wr], portMAX_DELAY);
    int64_t t1 = esp_timer_get_time();

    ppu_frame_t *nf = &s_frames[s_wr];
    nf->next_line = 0;
    nf->skip      = 0;

    s_stat_wait_buf_us = t1 - t0;
}

/* ── query API ─────────────────────────────────────────────────────── */

bool ppu_pipeline_audio_enabled(void)
{
    return s_audio_on;
}

void ppu_pipeline_get_render_stats(int64_t *sl, int64_t *vid, int64_t *aud)
{
    if (sl)  *sl  = s_stat_scanline_us;
    if (vid) *vid = s_stat_video_us;
    if (aud) *aud = s_stat_audio_us;
}

void ppu_pipeline_get_wait_stats(int64_t *wait_render_us,
                                 int64_t *wait_buf_us,
                                 int64_t *wait_pace_us,
                                 uint32_t *audio_drop_count,
                                 uint32_t *audio_queue_peak)
{
    if (wait_render_us)  *wait_render_us  = s_stat_wait_render_us;
    if (wait_buf_us)     *wait_buf_us     = s_stat_wait_buf_us;
    if (wait_pace_us)    *wait_pace_us    = s_stat_wait_pace_us;
    if (audio_drop_count) *audio_drop_count = s_stat_audio_drop_count;
    if (audio_queue_peak) *audio_queue_peak = s_stat_audio_queue_peak;
}

void ppu_pipeline_dump_start(uint32_t skip, uint32_t count)
{
    s_dump_frame_nr = 0;
    s_dump_skip     = skip;
    s_dump_count    = count;
    ESP_LOGI(TAG, "dump: will record frames %u..%u to SD",
             skip, skip + count - 1);
}

#endif /* DUAL_CORE_PPU */
