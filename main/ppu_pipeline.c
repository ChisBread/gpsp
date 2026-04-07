/*
 * gpsp — Dual-core PPU render pipeline (ESP32-P4)
 *
 * Architecture (modeled after mGBA's renderer separation):
 *
 *   Core 1 (CPU/emu):                    Core 0 (render):
 *   ─────────────────                    ────────────────
 *   begin_frame()                        ← wait for frame
 *     ├ wait for buffer                  ppu_begin_render_frame()
 *     ├ wait for pace                      ├ copy OAM  → DRAM
 *     └ snapshot OAM + palette             └ copy pal  → DRAM
 *   for line 0..159:                     for line 0..159:
 *     submit_scanline()                    ppu_begin_render_line()
 *       ├ snapshot IO regs                   ├ copy IO → DRAM
 *       ├ snapshot affine refs               ├ load affine refs
 *       ├ capture OAM_UPDATED                └ set OAM_UPDATED
 *       └ accumulate affine +=PB/PD        update_scanline()
 *     HBlank DMA (may write BG2X)            └ renders to framebuf
 *   end_frame()                          ppu_end_render_frame()
 *     ├ reload affine from BG2X/Y        PPA scale + LCD output
 *     └ send to queue →──────────────→   audio collect + output
 *                                        release buffer
 *
 * Key design decisions based on GBA hardware timing:
 *
 *   1. Affine reference (BG2X/Y → affine_reference_x/y) is a *running
 *      accumulator*.  Each scanline it steps by +=PB/PD (with mosaic).
 *      The CPU may also override it at any time via BG2X register writes
 *      (e.g. HBlank DMA for Mode 7 effects).
 *
 *      We snapshot the accumulator's value *before* stepping, then step
 *      it on the CPU core synchronously.  The render core loads the
 *      pre-computed per-line value — no accumulation on the render side.
 *
 *   2. OAM and palette change infrequently (usually once per VBlank).
 *      We snapshot them at frame start (begin_frame) and copy into
 *      DRAM-local render buffers.  Mid-frame OAM writes set a per-line
 *      dirty flag so order_obj() re-sorts on the render side, but the
 *      snapshot itself stays the same for the whole frame.
 *
 *   3. VRAM (96 KB) is NOT snapshotted — too large.  The render core
 *      reads VRAM directly.  Serialization (s_render_done) ensures the
 *      render core finishes before the emu core modifies VRAM for the
 *      next frame.
 *
 *   4. IO registers are snapshotted per scanline (128 B) and copied
 *      into a DRAM-local buffer before rendering each line.
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"

/* Frame pacing: 1000000 / 59.7275 ≈ 16743 µs */
#define PPU_FRAME_PERIOD_US        16743

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
#define BUF_COUNT                  2
#define SHUTDOWN_SENTINEL          0xFFFFFFFFu

#define GBA_FRAME_RATE             59.7275f
#define GBA_SOUND_HZ               GBA_SOUND_FREQUENCY
#define AUDIO_FRAME_MAX            ((GBA_SOUND_HZ / 50) + 1)

/* ── per-scanline descriptor ───────────────────────────────────────── */

typedef struct {
    u16  io[IO_SNAP_U16];       /* 128 B — full IO register snapshot     */
    s32  affine_x[2];           /*   8 B — BG2/BG3 affine reference X   */
    s32  affine_y[2];           /*   8 B — BG2/BG3 affine reference Y   */
    u32  oam_updated;           /*   4 B — OAM dirty flag               */
} ppu_line_t;                   /* 148 B per line, 23 680 B for 160 lines */

/* ── per-frame descriptor (lives in PSRAM, double-buffered) ────────── */

typedef struct {
    ppu_line_t  line[GBA_LINES];
    u16         oam[OAM_U16];           /* OAM snapshot (1 KB)   */
    u16         palette[PAL_U16];       /* palette snapshot (1 KB) */
    u8          skip;                   /* non-zero → skip rendering */
    u8          pad[3];
    int         next_line;              /* write cursor 0..160 */
} ppu_frame_t;

/* ── module state ──────────────────────────────────────────────────── */

static GPSP_EXTRAM_BSS ppu_frame_t s_frames[BUF_COUNT] __attribute__((aligned(64)));
static GPSP_EXTRAM_BSS u16 s_render_fb_storage[GBA_SCREEN_WIDTH * (GBA_SCREEN_HEIGHT + 1)] __attribute__((aligned(64)));
static int               s_wr;                /* write buffer index   */

static SemaphoreHandle_t s_buf_sem[BUF_COUNT]; /* "buffer free" tokens */
static QueueHandle_t     s_queue;              /* completed-buf index  */
static SemaphoreHandle_t s_pace;               /* frame-pacing tick    */
static SemaphoreHandle_t s_render_done;        /* serialization: render done */
static esp_timer_handle_t s_timer;
static TaskHandle_t      s_task;

static bool              s_audio_on;
static float             s_audio_spf;          /* samples per frame    */
static float             s_audio_frac;
static int16_t           s_audio_buf[AUDIO_FRAME_MAX * 2];

static u16              *s_render_fb = s_render_fb_storage; /* render framebuffer */

/* Render timing stats */
static volatile int64_t  s_stat_scanline_us;
static volatile int64_t  s_stat_video_us;
static volatile int64_t  s_stat_audio_us;

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

/* Frame-pacing timer fires at GBA refresh rate. */
static void pace_cb(void *arg) { (void)arg; xSemaphoreGive(s_pace); }

/* ── D-cache sync helpers ──────────────────────────────────────────── */

/*
 * ESP32-P4 L1 D-caches are NOT hardware-coherent between cores.
 * The frame descriptor (PSRAM) and VRAM (SRAM) are written by Core 1
 * and read by Core 0.  Without explicit cache management Core 0 may
 * read stale L1 data → wrong sprite positions / scrolling offsets.
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

        ppu_frame_t *f = &s_frames[idx];

        /* Invalidate Core 0 L1 D-cache for the frame descriptor
         * and VRAM so we read Core 1's latest data. */
        ppu_dcache_invalidate(f, sizeof(ppu_frame_t));
        ppu_dcache_invalidate(vram, 1024 * 96);

        int64_t t0 = esp_timer_get_time(), t1, t2, t3;

        if (!f->skip) {
            /*
             * Point the renderer at our private framebuffer and copy
             * per-frame OAM + palette + VRAM into DRAM-local render arrays.
             */
            gba_screen_pixels = s_render_fb;
            ppu_begin_render_frame(f->oam, f->palette);

            u32 saved_skip = skip_next_frame;
            skip_next_frame = 0;

            for (int y = 0; y < GBA_LINES; y++) {
                /*
                 * Copy per-line state into DRAM-local buffers and load
                 * the pre-computed affine reference points.
                 *
                 * Force oam_updated = 1 on line 0 so order_obj() runs
                 * once at frame start with this frame's OAM snapshot.
                 */
                ppu_begin_render_line(
                    f->line[y].io,
                    f->line[y].affine_x,
                    f->line[y].affine_y,
                    y == 0 ? 1 : f->line[y].oam_updated);

                update_scanline();
            }

            skip_next_frame = saved_skip;
            ppu_end_render_frame();

            /* Dump snapshot + rendered pixels for offline comparison */
            dump_frame(f, s_render_fb);

            t1 = esp_timer_get_time();

            /* Rendering is finished at this point: VRAM and the frame
             * descriptor are no longer read after dump_frame(). Let the
             * emu core start building the next frame while video submit
             * pushes the already-rendered framebuffer to the panel. */
            xSemaphoreGive(s_buf_sem[idx]);
            xSemaphoreGive(s_render_done);

            esp_err_t err = video_driver_submit_frame(s_render_fb);
            if (err != ESP_OK)
                ESP_LOGW(TAG, "video submit: %s", esp_err_to_name(err));
        } else {
            t1 = t0;

            /* Skipped frames do not touch the framebuffer. Release the
             * frame descriptor immediately so the emu core can continue. */
            xSemaphoreGive(s_buf_sem[idx]);
            xSemaphoreGive(s_render_done);
        }

        t2 = esp_timer_get_time();

        /* Audio */
        uint32_t af = collect_audio(s_audio_buf, AUDIO_FRAME_MAX);
        if (s_audio_on && af > 0) {
            esp_err_t err = audio_driver_write(s_audio_buf, af);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "audio write: %s", esp_err_to_name(err));
                s_audio_on = false;
            }
        }

        t3 = esp_timer_get_time();
        s_stat_scanline_us = t1 - t0;
        s_stat_video_us    = t2 - t1;
        s_stat_audio_us    = t3 - t2;
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

    s_queue = xQueueCreate(BUF_COUNT, sizeof(uint32_t));
    for (int i = 0; i < BUF_COUNT; i++)
        s_buf_sem[i] = xSemaphoreCreateBinary();

    if (!s_queue || !s_buf_sem[0] || !s_buf_sem[1])
        return ESP_ERR_NO_MEM;

    s_pace = xSemaphoreCreateBinary();
    if (!s_pace) return ESP_ERR_NO_MEM;

    s_render_done = xSemaphoreCreateBinary();
    if (!s_render_done) return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_render_done);  /* first frame has no prior render */

    const esp_timer_create_args_t targs = {
        .callback = pace_cb,
        .name     = "ppu_pace",
    };
    esp_err_t e = esp_timer_create(&targs, &s_timer);
    if (e != ESP_OK) return e;
    e = esp_timer_start_periodic(s_timer, PPU_FRAME_PERIOD_US);
    if (e != ESP_OK) return e;

    for (int i = 0; i < BUF_COUNT; i++)
        xSemaphoreGive(s_buf_sem[i]);

    BaseType_t r = xTaskCreatePinnedToCore(
        render_task, "ppu_render",
        cfg->task_stack_size, NULL,
        cfg->task_priority, &s_task,
        cfg->render_core);
    if (r != pdPASS) return ESP_FAIL;

    ESP_LOGI(TAG, "init OK (core %d, buf %zu B × %d, pace %d us)",
             cfg->render_core, sizeof(ppu_frame_t), BUF_COUNT,
             PPU_FRAME_PERIOD_US);
    dump_open();
    return ESP_OK;
}

void ppu_pipeline_deinit(void)
{
    if (s_timer) {
        esp_timer_stop(s_timer);
        esp_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_pace) { vSemaphoreDelete(s_pace); s_pace = NULL; }
    if (s_render_done) { vSemaphoreDelete(s_render_done); s_render_done = NULL; }

    if (s_task) {
        uint32_t x = SHUTDOWN_SENTINEL;
        xQueueSend(s_queue, &x, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(50));
        s_task = NULL;
    }

    ppu_end_render_frame();

    if (s_queue) { vQueueDelete(s_queue); s_queue = NULL; }
    for (int i = 0; i < BUF_COUNT; i++)
        if (s_buf_sem[i]) { vSemaphoreDelete(s_buf_sem[i]); s_buf_sem[i] = NULL; }
}

/*
 * begin_frame — called at vcount ≈ 0 (start of visible area).
 *
 * Blocks until:
 *   1. The render task has released the write buffer (back-pressure).
 *   2. The frame-pacing timer has ticked (caps to 59.7275 Hz).
 *
 * Snapshots OAM and palette into the frame descriptor.  Games update
 * these during VBlank (which just ended), so this captures the latest
 * state before the first scanline.
 */
void ppu_pipeline_begin_frame(void)
{
    /* Wait for previous render to finish — serializes emu+render
     * to eliminate any pipelining-induced state divergence. */
    xSemaphoreTake(s_render_done, portMAX_DELAY);

    xSemaphoreTake(s_buf_sem[s_wr], portMAX_DELAY);
    xSemaphoreTake(s_pace, portMAX_DELAY);

    ppu_frame_t *f = &s_frames[s_wr];
    f->next_line = 0;
    f->skip      = 0;

    /* Snapshot OAM and palette (read REAL globals — no redirect here). */
    memcpy(f->oam,       oam_ram,               sizeof(f->oam));
    memcpy(f->palette,   palette_ram_converted,  sizeof(f->palette));
}

/*
 * submit_scanline — called at each H-Draw → HBlank transition (vcount 0..159).
 *
 * Timing (matches single-core update_gba exactly):
 *   1. Snapshot IO registers (includes current VCOUNT set at prev H-Blank end)
 *   2. Snapshot affine_reference_x/y (the running accumulator)
 *   3. Capture and clear OAM_UPDATED
 *   4. Step the accumulator: affine_reference += PB/PD (with mosaic)
 *      This prepares the state for the NEXT scanline.
 *
 * After this function returns, HBlank DMA may fire and override
 * affine_reference via the BG2X write handler — same as single-core.
 */
void ppu_pipeline_submit_scanline(void)
{
    ppu_frame_t *f = &s_frames[s_wr];
    int y = f->next_line;
    if (y >= GBA_LINES) return;

    ppu_line_t *L = &f->line[y];

    /* 1. IO snapshot */
    memcpy(L->io, io_registers, sizeof(L->io));

    /* 2. Affine reference snapshot (current accumulator value). */
    L->affine_x[0] = affine_reference_x[0];
    L->affine_y[0] = affine_reference_y[0];
    L->affine_x[1] = affine_reference_x[1];
    L->affine_y[1] = affine_reference_y[1];

    /* 3. OAM dirty flag */
    L->oam_updated = reg[OAM_UPDATED];
    if (L->oam_updated)
        reg[OAM_UPDATED] = 0;

    /* 4. Accumulate affine references for next scanline.
     *
     * Matches the logic at the end of update_scanline() in video.cc.
     * Only modes 1-5 use affine backgrounds; mode 0 doesn't. */
    u32 vmode = read_ioreg(REG_DISPCNT) & 0x07;
    if (vmode) {
        const u32 mosv = ((read_ioreg(REG_MOSAIC) >> 4) & 0xF) + 1;

        if (read_ioreg(REG_BG2CNT) & 0x40) {    /* mosaic enabled */
            if (((u32)y % mosv) == mosv - 1) {
                affine_reference_x[0] += (s16)read_ioreg(REG_BG2PB) * mosv;
                affine_reference_y[0] += (s16)read_ioreg(REG_BG2PD) * mosv;
            }
        } else {
            affine_reference_x[0] += (s16)read_ioreg(REG_BG2PB);
            affine_reference_y[0] += (s16)read_ioreg(REG_BG2PD);
        }

        if (read_ioreg(REG_BG3CNT) & 0x40) {
            if (((u32)y % mosv) == mosv - 1) {
                affine_reference_x[1] += (s16)read_ioreg(REG_BG3PB) * mosv;
                affine_reference_y[1] += (s16)read_ioreg(REG_BG3PD) * mosv;
            }
        } else {
            affine_reference_x[1] += (s16)read_ioreg(REG_BG3PB);
            affine_reference_y[1] += (s16)read_ioreg(REG_BG3PD);
        }
    }

    f->next_line = y + 1;
}

/*
 * end_frame — called at vcount == 160 (VBlank entry), BEFORE VBlank DMA.
 *
 * Reloads the global affine reference accumulators from BG2X/Y registers
 * (same as video_reload_counters in single-core).  This must write to the
 * REAL globals so that submit_scanline's accumulation and the BG2X write
 * handler in gba_memory.c start from the correct base next frame.
 *
 * VBlank DMA fires AFTER this, which may further override the values
 * via the BG2X handler — exactly matching single-core timing.
 *
 * The actual VRAM writeback + queue send is deferred to post_vblank()
 * so that VBlank DMA writes are captured consistently.
 */
void ppu_pipeline_end_frame(bool skip)
{
    ppu_frame_t *f = &s_frames[s_wr];
    f->skip = skip ? 1 : 0;

    /* Reload affine accumulators for the next frame (writes REAL globals). */
    affine_reference_x[0] = signext28(read_ioreg32(REG_BG2X_L));
    affine_reference_y[0] = signext28(read_ioreg32(REG_BG2Y_L));
    affine_reference_x[1] = signext28(read_ioreg32(REG_BG3X_L));
    affine_reference_y[1] = signext28(read_ioreg32(REG_BG3Y_L));
}

/*
 * post_vblank — called AFTER VBlank DMA completes (still at vcount == 160).
 *
 * Flushes the frame descriptor and VRAM to SRAM, then queues the frame
 * for the render core.  By deferring until after VBlank DMA, the render
 * core sees a consistent VRAM snapshot that includes VBlank DMA changes.
 *
 * Without this split, VBlank DMA writes to VRAM (in Core 1 L1) could be
 * partially evicted to SRAM while the render core reads VRAM, creating a
 * random mix of pre- and post-DMA data — the root cause of sprite
 * flickering on frames where VBlank DMA updates tile data.
 */
void ppu_pipeline_post_vblank(void)
{
    uint32_t done = (uint32_t)s_wr;
    asm volatile ("fence rw, rw" ::: "memory");
    ppu_dcache_writeback(&s_frames[done], sizeof(ppu_frame_t));
    ppu_dcache_writeback(vram, 1024 * 96);

    /* Send completed buffer to render task, switch to other buffer. */
    s_wr ^= 1;
    xQueueSend(s_queue, &done, portMAX_DELAY);
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

void ppu_pipeline_dump_start(uint32_t skip, uint32_t count)
{
    s_dump_frame_nr = 0;
    s_dump_skip     = skip;
    s_dump_count    = count;
    ESP_LOGI(TAG, "dump: will record frames %u..%u to SD",
             skip, skip + count - 1);
}

#endif /* DUAL_CORE_PPU */
