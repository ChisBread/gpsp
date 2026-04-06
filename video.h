/* gameplaySP
 *
 * Copyright (C) 2006 Exophase <exophase@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef VIDEO_H
#define VIDEO_H

void update_scanline(void);
void video_reload_counters(void);

extern s32 affine_reference_x[2];
extern s32 affine_reference_y[2];

extern u16* gba_screen_pixels;

#ifdef DUAL_CORE_PPU
/*
 * Render-side API — called by the render task (ppu_pipeline.c) to copy
 * per-frame / per-line snapshot data into DRAM-local arrays before
 * calling update_scanline().
 */

/* Copy OAM + palette snapshots into render-local arrays. */
void ppu_begin_render_frame(const u16 *oam_snap, const u16 *pal_snap);

/* Copy per-line IO snapshot and affine refs; set OAM_UPDATED flag. */
void ppu_begin_render_line(const u16 *io_snap,
                           const s32 *ref_x, const s32 *ref_y,
                           u32 oam_updated);

/* Restore all redirections to live data after rendering a frame. */
void ppu_end_render_frame(void);

/* Called by update_gba() — implemented in ppu_pipeline.c */
void ppu_pipeline_submit_scanline(void);
void ppu_pipeline_end_frame(bool skip_frame);
void ppu_pipeline_post_vblank(void);
#endif

#endif
