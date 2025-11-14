/******************************************************************************\
**
**  This file is part of the Hades GBA Emulator, and is made available under
**  the terms of the GNU General Public License version 2.
**
**  Copyright (C) 2021-2024 - The Hades Authors
**
\******************************************************************************/
/*
** Modifications by Korbin Deary (kdeary).
** Licensed under the same terms as the Hades emulator (GNU GPLv2).
*/


#include <string.h>
#include "gba/gba.h"
#include "gba/ppu.h"

static void ppu_merge_layer(struct gba const *gba, struct scanline *scanline, struct rich_color *layer);

/*
** Initialize the content of the given `scanline` to a default, sane and working value.
*/
static
void
ppu_initialize_scanline(
    struct gba const *gba,
    struct scanline *scanline
) {
    struct rich_color backdrop;
    uint32_t x;

    memset(scanline, 0x00, sizeof(*scanline));

    backdrop.visible = true;
    backdrop.idx = 5;
    backdrop.raw = (gba->io.dispcnt.blank ? 0x7fff : mem_palram_read16(gba, PALRAM_START));

    for (x = 0; x < GBA_SCREEN_WIDTH; ++x) {
        scanline->result[x] = backdrop;
    }

    /*
    ** The only layer that `ppu_merge_layer` will never merge is the backdrop layer so we force
    ** it here instead (if that's useful).
    */

    if (gba->io.bldcnt.mode == BLEND_LIGHT || gba->io.bldcnt.mode == BLEND_DARK) {
        scanline->top_idx = 5;
        memcpy(scanline->bg, scanline->result, sizeof(scanline->bg));
        memcpy(scanline->bot, scanline->result, sizeof(scanline->bot));
        ppu_merge_layer(gba, scanline, scanline->bg);
        scanline->top_idx = 0;
    }
}

/*
** Merge the current layer with any previous ones (using alpha blending) as stated in REG_BLDCNT.
*/
static
void
ppu_merge_layer(
    struct gba const *gba,
    struct scanline *scanline,
    struct rich_color *layer
) {
    struct io const *io = &gba->io;

    // Clamp to [0..16]
    const uint32_t eva = (io->bldalpha.top_coef  > 16) ? 16 : io->bldalpha.top_coef;
    const uint32_t evb = (io->bldalpha.bot_coef  > 16) ? 16 : io->bldalpha.bot_coef;
    const uint32_t evy = (io->bldy.coef          > 16) ? 16 : io->bldy.coef;

    const uint32_t bldcnt_raw = io->bldcnt.raw;
    const uint32_t base_mode  = io->bldcnt.mode;
    const uint32_t top_idx    = (uint32_t)scanline->top_idx;   // 0..4

    // Skip window logic entirely if no windows are active for this scanline
    const bool windows_any = (scanline->top_idx <= 4) && (io->dispcnt.win0 || io->dispcnt.win1 || io->dispcnt.winobj);

    // BLDCNT enable bit for this top layer
    const uint32_t top_bit = 1u << top_idx;
    const bool top_enabled_global = (bldcnt_raw & top_bit) != 0;

    struct rich_color *restrict res = scanline->result;
    struct rich_color *restrict bot = scanline->bot;
    struct rich_color *restrict top = layer;

    for (uint32_t x = 0; x < GBA_SCREEN_WIDTH; ++x) {
        struct rich_color topc = top[x];

        // Transparent? Nothing to do (still update "bot" like original)
        if (!topc.visible) {
            continue;
        }

        struct rich_color botc = bot[x];

        // Effective mode (may be turned off by windows or forced by sprite)
        uint32_t mode_eff = base_mode;

        if (windows_any) {
            uint8_t win_opts = ppu_find_top_window(gba, scanline, x);

            // If window hides this layer, skip entirely
            if (((win_opts >> top_idx) & 1u) == 0u) {
                continue;
            }
            // Windows can disable blending (bit 5)
            if (((win_opts >> 5) & 1u) == 0u) {
                mode_eff = BLEND_OFF;
            }
        }

        // Bottom enable bit depends on botc.idx (bits 8..13)
        const bool bot_enabled = ((bldcnt_raw >> (8u + botc.idx)) & 1u) != 0;

        // Maintain original "bot chain"
        bot[x] = topc;

        // Fast paths
        if (mode_eff == BLEND_OFF) {
            res[x] = topc;
            continue;
        }

        if (mode_eff == BLEND_ALPHA || topc.force_blend) {
            // If top isn’t blend-enabled (globally) and not forcing — or there’s no valid bottom — take top
            if (!(top_enabled_global || topc.force_blend) || !bot_enabled || !botc.visible) {
                res[x] = topc;
            } else {
                // Use true eva/evb weights: out = (eva * top + evb * bot) >> 4
                int32_t blended_r = ((int32_t)eva * (int32_t)topc.red   + (int32_t)evb * (int32_t)botc.red)   >> 4;
                int32_t blended_g = ((int32_t)eva * (int32_t)topc.green + (int32_t)evb * (int32_t)botc.green) >> 4;
                int32_t blended_b = ((int32_t)eva * (int32_t)topc.blue  + (int32_t)evb * (int32_t)botc.blue)  >> 4;

                if (blended_r > 31) blended_r = 31;
                if (blended_g > 31) blended_g = 31;
                if (blended_b > 31) blended_b = 31;

                struct rich_color out;
                out.red     = (uint8_t)blended_r;
                out.green   = (uint8_t)blended_g;
                out.blue    = (uint8_t)blended_b;
                out.visible = true;
                out.idx     = (uint8_t)top_idx;
                res[x] = out;
            }
            continue;
        }

        if (top_enabled_global) {
            if (mode_eff == BLEND_LIGHT) {
                struct rich_color out;
                out.red     = topc.red   + (((31 - topc.red)   * evy) >> 4);
                out.green   = topc.green + (((31 - topc.green) * evy) >> 4);
                out.blue    = topc.blue  + (((31 - topc.blue)  * evy) >> 4);
                out.visible = true;
                out.idx     = topc.idx;
                res[x] = out;
                continue;
            }

            // BLEND_DARK
            struct rich_color out;
            out.red     = topc.red   - ((topc.red   * evy) >> 4);
            out.green   = topc.green - ((topc.green * evy) >> 4);
            out.blue    = topc.blue  - ((topc.blue  * evy) >> 4);
            out.visible = true;
            out.idx     = topc.idx;
            res[x] = out;
        } else {
            res[x] = topc;
        }
    }
}


/*
** Render the current scanline and write the result in `gba->framebuffer`.
*/
static
void
ppu_render_scanline(
    struct gba *gba,
    struct scanline *scanline
) {
    struct io const *io;
    int32_t prio;
    uint32_t y;

    io = &gba->io;
    y = gba->io.vcount.raw;

    switch (io->dispcnt.bg_mode) {
        case 0: {
            for (prio = 3; prio >= 0; --prio) {
                int32_t bg_idx;

                for (bg_idx = 3; bg_idx >= 0; --bg_idx) {
                    if (bitfield_get((uint8_t)io->dispcnt.bg, bg_idx) && io->bgcnt[bg_idx].priority == prio && likely(gba->settings.ppu.enable_bg_layers[bg_idx])) {
                        ppu_render_background_text(gba, scanline, y, bg_idx);
                        ppu_merge_layer(gba, scanline, scanline->bg);
                    }
                }

                if (likely(gba->settings.ppu.enable_oam)) {
                    scanline->top_idx = 4;
                    ppu_merge_layer(gba, scanline, scanline->oam[prio]);
                }
            }
            break;
        };
        case 1: {
            for (prio = 3; prio >= 0; --prio) {
                int32_t bg_idx;

                for (bg_idx = 2; bg_idx >= 0; --bg_idx) {
                    if (bitfield_get((uint8_t)io->dispcnt.bg, bg_idx) && io->bgcnt[bg_idx].priority == prio && likely(gba->settings.ppu.enable_bg_layers[bg_idx])) {
                        if (bg_idx == 2) {
                            memset(scanline->bg, 0x00, sizeof(scanline->bg));
                            ppu_render_background_affine(gba, scanline, y, bg_idx);
                        } else {
                            ppu_render_background_text(gba, scanline, y, bg_idx);
                        }
                        ppu_merge_layer(gba, scanline, scanline->bg);
                    }
                }

                if (likely(gba->settings.ppu.enable_oam)) {
                    scanline->top_idx = 4;
                    ppu_merge_layer(gba, scanline, scanline->oam[prio]);
                }
            }
            break;
        };
        case 2: {
            for (prio = 3; prio >= 0; --prio) {
                int32_t bg_idx;

                for (bg_idx = 3; bg_idx >= 2; --bg_idx) {
                    if (bitfield_get((uint8_t)io->dispcnt.bg, bg_idx) && io->bgcnt[bg_idx].priority == prio && likely(gba->settings.ppu.enable_bg_layers[bg_idx])) {
                        memset(scanline->bg, 0x00, sizeof(scanline->bg));
                        ppu_render_background_affine(gba, scanline, y, bg_idx);
                        ppu_merge_layer(gba, scanline, scanline->bg);
                    }
                }

                if (likely(gba->settings.ppu.enable_oam)) {
                    scanline->top_idx = 4;
                    ppu_merge_layer(gba, scanline, scanline->oam[prio]);
                }
            }
            break;
        };
        case 3: {
            for (prio = 3; prio >= 0; --prio) {
                if (bitfield_get((uint8_t)io->dispcnt.bg, 2) && io->bgcnt[2].priority == prio && likely(gba->settings.ppu.enable_bg_layers[2])) {
                    memset(scanline->bg, 0x00, sizeof(scanline->bg));
                    ppu_render_background_bitmap(gba, scanline, false);
                    ppu_merge_layer(gba, scanline, scanline->bg);
                }

                if (likely(gba->settings.ppu.enable_oam)) {
                    scanline->top_idx = 4;
                    ppu_merge_layer(gba, scanline, scanline->oam[prio]);
                }
            }
            break;
        };
        case 4: {
            for (prio = 3; prio >= 0; --prio) {
                if (bitfield_get((uint8_t)io->dispcnt.bg, 2) && io->bgcnt[2].priority == prio && likely(gba->settings.ppu.enable_bg_layers[2])) {
                    memset(scanline->bg, 0x00, sizeof(scanline->bg));
                    ppu_render_background_bitmap(gba, scanline, true);
                    ppu_merge_layer(gba, scanline, scanline->bg);
                }

                if (likely(gba->settings.ppu.enable_oam)) {
                    scanline->top_idx = 4;
                    ppu_merge_layer(gba, scanline, scanline->oam[prio]);
                }
            }
            break;
        };
        case 5: {
            for (prio = 3; prio >= 0; --prio) {
                if (bitfield_get((uint8_t)io->dispcnt.bg, 2) && io->bgcnt[2].priority == prio && y < 128 && likely(gba->settings.ppu.enable_bg_layers[2])) {
                    memset(scanline->bg, 0x00, sizeof(scanline->bg));
                    ppu_render_background_bitmap_small(gba, scanline);
                    ppu_merge_layer(gba, scanline, scanline->bg);
                }

                if (likely(gba->settings.ppu.enable_oam)) {
                    scanline->top_idx = 4;
                    ppu_merge_layer(gba, scanline, scanline->oam[prio]);
                }
            }
            break;
        };
    }
}

/*
** Compose the content of the framebuffer based on the content of `scanline->result` and/or the backdrop color.
*/
static
void
ppu_draw_scanline(
    struct gba *gba,
    struct scanline const *scanline
) {
    uint32_t x;
    uint32_t y;
    uint16_t *dst;
    size_t base;

    y = gba->io.vcount.raw;
    dst = gba->shared_data.framebuffer.data;
    base = GBA_SCREEN_WIDTH * (size_t)y;

    for (x = 0; x < GBA_SCREEN_WIDTH; ++x) {
        struct rich_color c;

        c = scanline->result[x];
        dst[base + x] = (uint16_t)(
            ((uint32_t)c.red & 0x1F)
            | (((uint32_t)c.green & 0x1F) << 5)
            | (((uint32_t)c.blue & 0x1F) << 10)
        );
    }
}

/*
** Called when the PPU enters HDraw, this function updates some IO registers
** to reflect the progress of the PPU and eventually triggers an IRQ.
*/
void
ppu_hdraw(
    struct gba *gba,
    struct event_args args __unused
) {
    struct io *io;

    io = &gba->io;

    /* Increment VCOUNT */
    ++io->vcount.raw;

    if (io->vcount.raw >= GBA_SCREEN_REAL_HEIGHT) {
        io->vcount.raw = 0;
        atomic_fetch_add(&gba->shared_data.frame_counter, 1);
        atomic_fetch_add(&gba->shared_data.framebuffer.version, 1);

        if (gba->settings.enable_frame_skipping && gba->settings.frame_skip_counter > 0) {
            gba->ppu.current_frame_skip_counter = (gba->ppu.current_frame_skip_counter + 1) % gba->settings.frame_skip_counter;
            gba->ppu.skip_current_frame = (gba->ppu.current_frame_skip_counter != 0);
        } else {
            gba->ppu.skip_current_frame = false;
        }
    } else if (io->vcount.raw == GBA_SCREEN_HEIGHT) {
        atomic_store(&gba->shared_data.framebuffer.dirty, true);
        atomic_fetch_add(&gba->shared_data.framebuffer.version, 1);
    }

    io->dispstat.vcount_eq = (io->vcount.raw == io->dispstat.vcount_val);
    io->dispstat.vblank = (io->vcount.raw >= GBA_SCREEN_HEIGHT && io->vcount.raw < GBA_SCREEN_REAL_HEIGHT - 1);
    io->dispstat.hblank = false;

    /* Trigger the VBlank IRQ & DMA transfer */
    if (io->vcount.raw == GBA_SCREEN_HEIGHT) {
        if (io->dispstat.vblank_irq) {
            core_schedule_irq(gba, IRQ_VBLANK);
        }
        mem_schedule_dma_transfers(gba, DMA_TIMING_VBLANK);
        gba->ppu.reload_internal_affine_regs = true;
    }

    // This is set either on VBlank (see above) or when the affine registers are written to.
    if (gba->ppu.reload_internal_affine_regs) {
        ppu_reload_affine_internal_registers(gba, 0);
        ppu_reload_affine_internal_registers(gba, 1);
        gba->ppu.reload_internal_affine_regs = false;
    }

    /* Trigger the VCOUNT IRQ */
    if (io->dispstat.vcount_eq && io->dispstat.vcount_irq) {
        core_schedule_irq(gba, IRQ_VCOUNTER);
    }
}

/*
** Called when the PPU enters HBlank, this function updates some IO registers
** to reflect the progress of the PPU and eventually triggers an IRQ.
*/
void
ppu_hblank(
    struct gba *gba,
    struct event_args args __unused
) {
    struct io *io;

    io = &gba->io;

    if (io->vcount.raw < GBA_SCREEN_HEIGHT) {
        struct scanline scanline;

        if (!gba->ppu.skip_current_frame) {
            ppu_initialize_scanline(gba, &scanline);

            if (!gba->io.dispcnt.blank) {
                ppu_window_build_masks(gba, io->vcount.raw);
                ppu_prerender_oam(gba, &scanline, io->vcount.raw);
                ppu_render_scanline(gba, &scanline);
            }

            ppu_draw_scanline(gba, &scanline);
        }

        ppu_step_affine_internal_registers(gba);
    }

    io->dispstat.hblank = true;

    /*
    ** Trigger the HBLANK IRQ & DMA transfer
    */

    if (io->dispstat.hblank_irq) {
        core_schedule_irq(gba, IRQ_HBLANK);
    }

    if (io->vcount.raw < GBA_SCREEN_HEIGHT) {
        mem_schedule_dma_transfers(gba, DMA_TIMING_HBLANK);
    }

    if (gba->ppu.video_capture_enabled && io->vcount.raw >= 2 && io->vcount.raw < GBA_SCREEN_HEIGHT + 2) {
        mem_schedule_dma_transfers_for(gba, 3, DMA_TIMING_SPECIAL);  // Video DMA
    }

    /*
    ** Video mode is evaluated once at the beginning of the frame and can't be enabled/disabled mid-frame.
    **
    ** Reference:
    **   - https://github.com/mgba-emu/mgba/issues/2017
    **   - https://github.com/skylersaleh/SkyEmu/issues/104
    */
    if (io->vcount.raw == GBA_SCREEN_HEIGHT + 2) {
        gba->ppu.video_capture_enabled = gba->io.dma[3].control.enable && gba->io.dma[3].control.timing == DMA_TIMING_SPECIAL;
    }
}

/*
** Called when the CPU enters stop-mode to render the screen black.
*/
void
ppu_render_black_screen(
    struct gba *gba
) {
    pthread_mutex_lock(&gba->shared_data.framebuffer.lock);
    memset(gba->shared_data.framebuffer.data, 0x00, sizeof(gba->shared_data.framebuffer.data));
    pthread_mutex_unlock(&gba->shared_data.framebuffer.lock);
}
