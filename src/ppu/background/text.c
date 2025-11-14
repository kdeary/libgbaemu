#include "gba/gba.h"
#include "gba/ppu.h"

// Helper: compute screen-block offsets once per BG size
static inline uint32_t bg_horz_off(uint32_t bg_size) { return (bg_size == 0b01 || bg_size == 0b11) ? 1024u : 0u; }
static inline uint32_t bg_vert_off(uint32_t bg_size) { return (bg_size == 0b10) ? 1024u : (bg_size == 0b11 ? 2048u : 0u); }

/*
** Render the bitmap background of given index.
*/
void
ppu_render_background_text(
    struct gba const *gba,
    struct scanline *scanline,
    uint32_t line,
    uint32_t bg_idx
) {
    struct io const *io = &gba->io;

    scanline->top_idx = bg_idx;

    // Hoist invariants
    const bool mosaic        = io->bgcnt[bg_idx].mosaic;
    const bool palette_256   = io->bgcnt[bg_idx].palette_type;     // 1 = 256-color, 0 = 16-color
    const uint32_t bg_size   = io->bgcnt[bg_idx].size;
    const uint32_t screen_addr = (uint32_t)io->bgcnt[bg_idx].screen_base * 0x800u;
    const uint32_t chrs_addr   = (uint32_t)io->bgcnt[bg_idx].character_base * 0x4000u;

    const uint32_t hoff = io->bg_hoffset[bg_idx].raw;
    const uint32_t voff = io->bg_voffset[bg_idx].raw;

    // Precompute screen-block offsets per quadrant
    const uint32_t HORZ_BLK = bg_horz_off(bg_size); // 0 or 1024
    const uint32_t VERT_BLK = bg_vert_off(bg_size); // 0 or 1024 or 2048

    // Mosaic extents (>=1)
    const uint32_t mos_h = (uint32_t)io->mosaic.bg_hsize + 1u;
    const uint32_t mos_v = (uint32_t)io->mosaic.bg_vsize + 1u;

    // ----- Y math (only once per scanline) -----
    // rel_y within BG (with mosaic snap if enabled)
    uint32_t rel_y = mosaic ? (line / mos_v) * mos_v : line;
    rel_y = (rel_y + voff) & 0x1FFu;                // wrap to 9 bits (0..511)

    // Tile coords and intra-tile y
    const uint32_t tile_y = (rel_y >> 3) & 31u;     // 0..31
    const uint32_t chr_y  =  rel_y        & 7u;     // 0..7
    const uint32_t up_y   = (rel_y >> 8) & 1u;      // 0 or 1 (512 tall quadrant)

    // Precompute row base index inside a 32x32 screen block
    const uint32_t row_base = tile_y * 32u;

    // ----- X loop -----
    // If mosaic, render in horizontal blocks: compute once, splat N pixels.
    for (uint32_t x = 0; x < GBA_SCREEN_WIDTH; ) {
        const uint32_t run = mosaic ? ((x / mos_h) * mos_h + mos_h - x) : 1u; // remaining pixels in this mosaic block
        const uint32_t count = (x + run > GBA_SCREEN_WIDTH) ? (GBA_SCREEN_WIDTH - x) : run;

        // Compute rel_x for this block's representative pixel
        uint32_t rel_x0 = mosaic ? (x / mos_h) * mos_h : x;
        rel_x0 = (rel_x0 + hoff) & 0x1FFu;          // wrap to 9 bits (0..511)

        // Tile/intra-tile for the representative pixel
        const uint32_t tile_x0 = (rel_x0 >> 3) & 31u; // 0..31 within screen block
        const uint32_t chr_x0  =  rel_x0        & 7u; // 0..7
        const uint32_t up_x0   = (rel_x0 >> 8) & 1u;  // 0 or 1 (512 wide quadrant)

        // Select screen block (0..3) via up_x/up_y and bg_size
        const uint32_t screen_block_offset =
            up_x0 * HORZ_BLK + up_y * VERT_BLK;

        // Final screen entry index
        const uint32_t screen_idx = row_base + tile_x0 + screen_block_offset;

        // Fetch tile entry
        union tile tile;
        tile.raw = mem_vram_read16(gba, screen_addr + screen_idx * sizeof(union tile));

        // Compute effective (x,y) inside tile with flips
        const uint32_t chr_vy = tile.vflip ? (7u - chr_y) : chr_y;
        uint32_t chr_x = tile.hflip ? (7u - chr_x0) : chr_x0;

        // ----- Fetch palette index -----
        uint8_t palette_idx;
        if (palette_256) {
            // 256-color (8bpp): 64 bytes per tile
            const uint32_t addr = chrs_addr + (uint32_t)tile.number * 64u + chr_vy * 8u + chr_x;
            palette_idx = mem_vram_read8(gba, addr);
        } else {
            // 16-color (4bpp): 32 bytes per tile; reuse the fetched byte for adjacent pixel if not mosaic
            // Since we're splatting the same pixel across the mosaic run, just read once
            const uint32_t byte_addr = chrs_addr + (uint32_t)tile.number * 32u + chr_vy * 4u + (chr_x >> 1);
            uint8_t packed = mem_vram_read8(gba, byte_addr);
            if (chr_x & 1u) packed >>= 4;
            palette_idx = packed & 0xFu;
        }

        // ----- Write results -----
        if (palette_idx) {
            // Resolve RGB from palette (note: in 256-color mode, palette field ignored)
            const uint32_t pal_index = (palette_256 ? 0u : (uint32_t)tile.palette * 16u) + (uint32_t)palette_idx;
            const union color rawc = { .raw = mem_palram_read16(gba, pal_index * sizeof(union color)) };

            struct rich_color c;
            c.raw         = rawc.raw;
            c.visible     = true;
            c.idx         = (uint8_t)bg_idx;
            c.force_blend = false;

            // Splat across the mosaic run (or one pixel if not mosaic)
            for (uint32_t i = 0; i < count; ++i) scanline->bg[x + i] = c;
        } else {
            // Transparent palette index 0 ⇒ invisible
            for (uint32_t i = 0; i < count; ++i) scanline->bg[x + i].visible = false;
        }

        x += count;
    }
}
