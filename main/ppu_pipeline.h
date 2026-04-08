/*
 * gpsp app support — PPU render pipeline
 *
 * Offloads scanline rendering from the emulation core to Core 0.
 * The emulation core pushes per-scanline IO register snapshots into a
 * double-buffered ring.  The render task on Core 0 consumes them,
 * calls update_scanline() with redirected IO, then drives PPA scaling,
 * LCD output, and audio output.
 */

#ifndef GPSP_MAIN_PPU_PIPELINE_H
#define GPSP_MAIN_PPU_PIPELINE_H

#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "esp_err.h"

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DUAL_CORE_PPU

typedef struct {
    size_t   task_stack_size;   /* render task stack (bytes) */
    BaseType_t render_core;    /* core ID for the render task (usually 0) */
    UBaseType_t task_priority; /* render task priority */
    bool     audio_enabled;    /* enable audio collection & output */
} ppu_pipeline_config_t;

/*
 * Initialise the render pipeline.  Creates the render task on the
 * specified core and allocates the double-buffered scanline queue.
 * Must be called after init_sound().
 */
esp_err_t ppu_pipeline_init(const ppu_pipeline_config_t *config);

/*
 * Shut down the render pipeline.  Signals the render task to exit
 * and waits for it to finish.
 */
void ppu_pipeline_deinit(void);

/*
 * Re-arm the frame pacing timer from "now" and discard any stale pace tick.
 * Call this immediately before starting the emulation frame loop so the
 * 59.7275 Hz pacing phase aligns with emulation start, not pipeline init.
 */
esp_err_t ppu_pipeline_reset_pace(void);

/*
 * Called by the emulation core at each visible H-Draw → HBlank
 * transition (vcount 0..159).  Snapshots io_registers and pushes
 * a descriptor into the current write buffer.
 */
void ppu_pipeline_submit_scanline(void);

/*
 * Called at vcount == 228 (end of VBlank) after all DMA and CPU
 * activity.  Generates audio (render_gbc_sound + drain), snapshots
 * OAM/palette/VRAM, flushes caches, and queues the frame (with
 * embedded audio) for the render core.
 */
void ppu_pipeline_flush_frame(bool skip_frame);

/*
 * Whether audio output is enabled on the render pipeline.
 */
bool ppu_pipeline_audio_enabled(void);

/*
 * Read the last render-task timing breakdown (microseconds).
 * Values are updated after each rendered frame.
 */
void ppu_pipeline_get_render_stats(int64_t *scanline_us, int64_t *video_us,
                                   int64_t *audio_us);

/*
 * Read the last begin_frame wait breakdown and audio queue stats.
 */
void ppu_pipeline_get_wait_stats(int64_t *wait_render_us,
                                 int64_t *wait_buf_us,
                                 int64_t *wait_pace_us,
                                 uint32_t *audio_drop_count,
                                 uint32_t *audio_queue_peak);

/*
 * Start recording frames to SD card.
 * Records 'count' frames starting after skipping 'skip' frames.
 * Set count=0 to disable. Auto-disables after completion.
 */
void ppu_pipeline_dump_start(uint32_t skip, uint32_t count);

#endif /* DUAL_CORE_PPU */

#ifdef __cplusplus
}
#endif

#endif /* GPSP_MAIN_PPU_PIPELINE_H */
