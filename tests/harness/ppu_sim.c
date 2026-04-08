/*
 * Threaded dual-core PPU simulation for frame comparison testing.
 *
 * Drop-in replacement for main/ppu_pipeline.c — provides the same API
 * using pthreads instead of FreeRTOS to match the real ESP32-P4 behavior:
 *
 *   Main thread (CPU):    runs GBA emulation, pushes snapshots
 *   Render thread:        consumes frames, calls update_scanline()
 *
 * The key difference from sequential simulation: while the render thread
 * is rendering frame N, the CPU thread is already emulating frame N+1.
 * VRAM is snapshotted into the per-frame descriptor at post_vblank(),
 * matching the aggressive dual-core pipeline used on ESP32-P4.
 */

#ifdef DUAL_CORE_PPU

#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include "common.h"
#include "gba_memory.h"
#include "video.h"
#include "sound.h"

/* ── constants ─────────────────────────────────────────────────────── */

#define GBA_LINES       160
#define IO_SNAP_U16     64
#define OAM_U16         512
#define PAL_U16         512
#define VRAM_SNAPSHOT_BYTES (1024 * 96)
#define BUF_COUNT       2

#define GBA_FRAME_RATE  59.7275f
#define GBA_SOUND_HZ    GBA_SOUND_FREQUENCY
#define AUDIO_FRAME_MAX ((GBA_SOUND_HZ / 50) + 1)

/* ── per-scanline descriptor ───────────────────────────────────────── */

typedef struct {
    u16  io[IO_SNAP_U16];
    u32  oam_updated;
} ppu_line_t;

/* ── per-frame descriptor (double-buffered) ────────────────────────── */

typedef struct {
    ppu_line_t  line[GBA_LINES];
    u16         oam[OAM_U16];
    u16         palette[PAL_U16];
    u8          vram[VRAM_SNAPSHOT_BYTES];
    uint32_t    audio_frames;
    int16_t     audio_samples[AUDIO_FRAME_MAX * 2];
    u8          skip;
    int         next_line;
} ppu_frame_t;

/* ── synchronization ───────────────────────────────────────────────── */

static ppu_frame_t       s_frames[BUF_COUNT];
static int               s_wr;                /* CPU write buffer index */

/* Queue: completed frame indices for render thread */
static pthread_mutex_t   s_queue_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    s_queue_cond = PTHREAD_COND_INITIALIZER;
static int               s_queue_buf[BUF_COUNT + 1]; /* ring buffer */
static int               s_queue_head, s_queue_tail, s_queue_count;

/* Buffer-free semaphores (one per buffer) */
static pthread_mutex_t   s_buf_mtx[BUF_COUNT];
static pthread_cond_t    s_buf_cond[BUF_COUNT];
static int               s_buf_free[BUF_COUNT];

/* Render-done notification: render thread signals after each frame */
static pthread_mutex_t   s_done_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    s_done_cond = PTHREAD_COND_INITIALIZER;
static volatile int      s_done_flag;

static pthread_t         s_render_thread;
static volatile int      s_shutdown;

/* Render framebuffer (private to render thread) */
static u16               s_render_fb[240 * 161];

/* Per-frame CRC snapshot (set by render thread, read after wait_render) */
static uint32_t          s_last_oam_crc;
static uint32_t          s_last_io0_crc;

/* Audio state — mirrors ppu_pipeline.c */
static float             s_audio_spf;
static float             s_audio_frac;

/* Last frame's audio (set by render thread, read after wait_render) */
static uint32_t          s_last_audio_frames;
static int16_t           s_last_audio_samples[AUDIO_FRAME_MAX * 2];

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

/* ── queue helpers ─────────────────────────────────────────────────── */

static void queue_push(int val)
{
    pthread_mutex_lock(&s_queue_mtx);
    s_queue_buf[s_queue_tail] = val;
    s_queue_tail = (s_queue_tail + 1) % (BUF_COUNT + 1);
    s_queue_count++;
    pthread_cond_signal(&s_queue_cond);
    pthread_mutex_unlock(&s_queue_mtx);
}

static int queue_pop(void)
{
    pthread_mutex_lock(&s_queue_mtx);
    while (s_queue_count == 0)
        pthread_cond_wait(&s_queue_cond, &s_queue_mtx);
    int val = s_queue_buf[s_queue_head];
    s_queue_head = (s_queue_head + 1) % (BUF_COUNT + 1);
    s_queue_count--;
    pthread_mutex_unlock(&s_queue_mtx);
    return val;
}

static void buf_take(int idx)
{
    pthread_mutex_lock(&s_buf_mtx[idx]);
    while (!s_buf_free[idx])
        pthread_cond_wait(&s_buf_cond[idx], &s_buf_mtx[idx]);
    s_buf_free[idx] = 0;
    pthread_mutex_unlock(&s_buf_mtx[idx]);
}

static void buf_give(int idx)
{
    pthread_mutex_lock(&s_buf_mtx[idx]);
    s_buf_free[idx] = 1;
    pthread_cond_signal(&s_buf_cond[idx]);
    pthread_mutex_unlock(&s_buf_mtx[idx]);
}

/* ── helpers ───────────────────────────────────────────────────────── */

static inline s32 signext28(u32 v)
{
    return (s32)(v << 4) >> 4;
}

static inline u32 snap_ioreg32(const u16 *io, int reg)
{
    return io[reg] | ((u32)io[reg + 1] << 16);
}

/* ── render thread ─────────────────────────────────────────────────── */

static void *render_thread_func(void *arg)
{
    (void)arg;

    while (!s_shutdown) {
        int idx = queue_pop();
        if (idx < 0) break;  /* shutdown sentinel */

        ppu_frame_t *f = &s_frames[idx];

        if (!f->skip) {
            gba_screen_pixels = s_render_fb;
            ppu_begin_render_frame(f->oam, f->palette, f->vram);

            u32 saved_skip = skip_next_frame;
            skip_next_frame = 0;

            s32 aff_x[2], aff_y[2];
            aff_x[0] = signext28(snap_ioreg32(f->line[0].io, REG_BG2X_L));
            aff_y[0] = signext28(snap_ioreg32(f->line[0].io, REG_BG2Y_L));
            aff_x[1] = signext28(snap_ioreg32(f->line[0].io, REG_BG3X_L));
            aff_y[1] = signext28(snap_ioreg32(f->line[0].io, REG_BG3Y_L));

            for (int y = 0; y < GBA_LINES; y++) {
                const u16 *io = f->line[y].io;

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
        }

        /* Record CRC of what render thread received */
        s_last_oam_crc = crc32_simple(f->oam, sizeof(f->oam));
        s_last_io0_crc = crc32_simple(f->line[0].io, sizeof(f->line[0].io));

        /* Copy audio from frame descriptor for external consumption */
        s_last_audio_frames = f->audio_frames;
        if (f->audio_frames > 0)
            memcpy(s_last_audio_samples, f->audio_samples,
                   f->audio_frames * 2 * sizeof(int16_t));

        /* Release buffer back to CPU thread */
        buf_give(idx);

        /* Signal frame done */
        pthread_mutex_lock(&s_done_mtx);
        s_done_flag = 1;
        pthread_cond_signal(&s_done_cond);
        pthread_mutex_unlock(&s_done_mtx);
    }

    return NULL;
}

/* ── init ──────────────────────────────────────────────────────────── */

void ppu_sim_init(void)
{
    memset(s_frames, 0, sizeof(s_frames));
    s_wr = 0;
    s_shutdown = 0;
    s_audio_spf  = (float)GBA_SOUND_HZ / GBA_FRAME_RATE;
    s_audio_frac = 0.0f;
    s_queue_head = s_queue_tail = s_queue_count = 0;
    s_done_flag = 0;

    for (int i = 0; i < BUF_COUNT; i++) {
        pthread_mutex_init(&s_buf_mtx[i], NULL);
        pthread_cond_init(&s_buf_cond[i], NULL);
        s_buf_free[i] = 1;  /* all buffers start free */
    }

    /* Pre-acquire buf 0 for the first frame */
    buf_take(0);
    s_frames[0].next_line = 0;
    s_frames[0].skip      = 0;

    pthread_create(&s_render_thread, NULL, render_thread_func, NULL);
    printf("[ppu_sim] Threaded dual-core PPU simulation ready\n");
}

/* ── submit_scanline ───────────────────────────────────────────────── */

void ppu_pipeline_submit_scanline(void)
{
    ppu_frame_t *f = &s_frames[s_wr];
    int y = f->next_line;
    if (y >= GBA_LINES) return;

    ppu_line_t *L = &f->line[y];

    memcpy(L->io, io_registers, sizeof(L->io));

    L->oam_updated = reg[OAM_UPDATED];
    if (L->oam_updated)
        reg[OAM_UPDATED] = 0;

    f->next_line = y + 1;
}

/* ── flush_frame ───────────────────────────────────────────────────── */

static uint32_t collect_audio(int16_t *buf, size_t max)
{
    uint32_t n = (uint32_t)s_audio_spf;
    s_audio_frac += s_audio_spf - (float)n;
    if (s_audio_frac >= 1.0f) { n++; s_audio_frac -= 1.0f; }
    if (n > max) n = (uint32_t)max;

    uint32_t got = sound_read_samples(buf, n);
    if (got < n)
        memset(buf + got * 2, 0, (n - got) * 2 * sizeof(*buf));
    return n;
}

void ppu_pipeline_flush_frame(bool skip)
{
    ppu_frame_t *f = &s_frames[s_wr];
    f->skip = skip ? 1 : 0;

    /* Generate and drain audio — mirrors ppu_pipeline.c */
    render_gbc_sound();
    f->audio_frames = collect_audio(f->audio_samples, AUDIO_FRAME_MAX);

    if (!skip) {
        memcpy(f->oam,     oam_ram,              sizeof(f->oam));
        memcpy(f->palette, palette_ram_converted, sizeof(f->palette));
        memcpy(f->vram,    vram,                  sizeof(f->vram));
    }

    int done = s_wr;
    s_wr ^= 1;

    pthread_mutex_lock(&s_done_mtx);
    s_done_flag = 0;
    pthread_mutex_unlock(&s_done_mtx);

    queue_push(done);

    /* Acquire the next buffer for the upcoming frame */
    buf_take(s_wr);
    ppu_frame_t *nf = &s_frames[s_wr];
    nf->next_line = 0;
    nf->skip      = 0;
}

/* ── wait for render to finish current frame ───────────────────────── */

void ppu_sim_wait_render(void)
{
    pthread_mutex_lock(&s_done_mtx);
    while (!s_done_flag)
        pthread_cond_wait(&s_done_cond, &s_done_mtx);
    pthread_mutex_unlock(&s_done_mtx);
}

/* Return pointer to render framebuffer */
u16 *ppu_sim_render_fb(void)
{
    return s_render_fb;
}

void ppu_sim_shutdown(void)
{
    s_shutdown = 1;
    queue_push(-1);  /* sentinel */
    pthread_join(s_render_thread, NULL);
}

void ppu_sim_get_last_crc(uint32_t *oam_crc, uint32_t *io0_crc)
{
    if (oam_crc) *oam_crc = s_last_oam_crc;
    if (io0_crc) *io0_crc = s_last_io0_crc;
}

uint32_t ppu_sim_get_audio(int16_t **samples)
{
    if (samples) *samples = s_last_audio_samples;
    return s_last_audio_frames;
}

/* ── stubs for unused pipeline query API ───────────────────────────── */

bool ppu_pipeline_audio_enabled(void)
{
    return false;
}

void ppu_pipeline_get_render_stats(int64_t *sl, int64_t *vid, int64_t *aud)
{
    if (sl)  *sl  = 0;
    if (vid) *vid = 0;
    if (aud) *aud = 0;
}

#endif /* DUAL_CORE_PPU */
