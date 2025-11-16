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


#include <stdlib.h>
#include <string.h>
#include "hs.h"
#include "gba/gba.h"
#include "gba/core.h"
#include "gba/core/helpers.h"
#include "gba/memory.h"
#include "gba/gpio.h"

/*
** Region        Bus   Read      Write     Cycles   Note
** ==================================================
** BIOS ROM      32    8/16/32   -         1/1/1
** Work RAM 32K  32    8/16/32   8/16/32   1/1/1
** I/O           32    8/16/32   8/16/32   1/1/1
** OAM           32    8/16/32   16/32     1/1/1    a
** Work RAM 256K 16    8/16/32   8/16/32   3/3/6    b
** Palette RAM   16    8/16/32   16/32     1/1/2    a
** VRAM          16    8/16/32   16/32     1/1/2    a
** GamePak ROM   16    8/16/32   -         5/5/8    b/c
** GamePak Flash 16    8/16/32   16/32     5/5/8    b/c
** GamePak SRAM  8     8         8         5        b
**
** Timing Notes:
**
**  a   Plus 1 cycle if GBA accesses video memory at the same time.
**  b   Default waitstate settings, see System Control chapter.
**  c   Separate timings for sequential, and non-sequential accesses.
**
** Source: GBATek
*/
static uint32_t access_time16[2][16] = {
    [NON_SEQUENTIAL]    = { 1, 1, 3, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1 },
    [SEQUENTIAL]        = { 1, 1, 3, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1 },
};

static uint32_t access_time32[2][16] = {
    [NON_SEQUENTIAL]    = { 1, 1, 6, 1, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 1 },
    [SEQUENTIAL]        = { 1, 1, 6, 1, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 1 },
};

static uint32_t gamepak_nonseq_waitstates[4] = { 4, 3, 2, 8 };

static inline size_t
mem_region_calc_pages(
    size_t size
) {
    return (size + MEM_PAGE_SIZE - 1u) >> MEM_PAGE_SHIFT;
}

void
mem_region_init(
    struct mem_region *region,
    size_t size
) {
    region->size = size;
    region->page_count = mem_region_calc_pages(size);
    region->used_pages = 0;
    region->pages = calloc(region->page_count, sizeof(uint8_t *));
    hs_assert(region->pages);
}

void
mem_region_reset(
    struct mem_region *region
) {
    if (!region->pages) {
        return;
    }

    for (size_t i = 0; i < region->page_count; ++i) {
        free(region->pages[i]);
        region->pages[i] = NULL;
    }
    region->used_pages = 0;
}

void
mem_region_release(
    struct mem_region *region
) {
    mem_region_reset(region);
    free(region->pages);
    region->pages = NULL;
    region->page_count = 0;
    region->size = 0;
    region->used_pages = 0;
}

static inline uint8_t *
mem_region_page_ptr(
    struct mem_region *region,
    size_t page_index,
    bool alloc
) {
    hs_assert(page_index < region->page_count);
    uint8_t *page = region->pages[page_index];
    if (!page && alloc) {
        page = calloc(1u, MEM_PAGE_SIZE);
        hs_assert(page);
        region->pages[page_index] = page;
        region->used_pages++;
    }
    return page;
}

void
mem_region_read(
    struct mem_region const *region,
    uint32_t offset,
    void *dst,
    size_t len
) {
    uint8_t *out = dst;
    size_t remaining = len;

    while (remaining) {
        const size_t page_index = offset >> MEM_PAGE_SHIFT;
        const size_t page_offset = offset & (MEM_PAGE_SIZE - 1u);
        const size_t chunk = min(remaining, MEM_PAGE_SIZE - page_offset);
        uint8_t const *page = NULL;

        if (page_index < region->page_count && region->pages) {
            page = region->pages[page_index];
        }

        if (page) {
            memcpy(out, page + page_offset, chunk);
        } else {
            memset(out, 0, chunk);
        }

        out += chunk;
        offset += (uint32_t)chunk;
        remaining -= chunk;
    }
}

void
mem_region_write(
    struct mem_region *region,
    uint32_t offset,
    void const *src,
    size_t len
) {
    uint8_t const *in = src;
    size_t remaining = len;

    while (remaining) {
        const size_t page_index = offset >> MEM_PAGE_SHIFT;
        const size_t page_offset = offset & (MEM_PAGE_SIZE - 1u);
        const size_t chunk = min(remaining, MEM_PAGE_SIZE - page_offset);
        uint8_t *page = mem_region_page_ptr(region, page_index, true);

        memcpy(page + page_offset, in, chunk);

        in += chunk;
        offset += (uint32_t)chunk;
        remaining -= chunk;
    }
}

// Optional hot/inline hints (GCC/Clang)

static inline uint32_t
align_addr_pow2(
    uint32_t addr,
    uint32_t size
) {
    // Fast path for 1/2/4; fall back if odd size appears.
    switch (size) {
        case 1: return addr;
        case 2: return addr & ~1u;
        case 4: return addr & ~3u;
        default: return align_on(addr, size);
    }
}

/*
** Set the waitstates for ROM/SRAM memory according to the content of REG_WAITCNT.
*/
void
mem_update_waitstates(
    struct gba const *gba
) {
    struct io const *io;
    uint32_t x;

    io = &gba->io;

    // 16 bit, non seq
    access_time16[NON_SEQUENTIAL][CART_0_REGION_1] = 1 + gamepak_nonseq_waitstates[io->waitcnt.ws0_nonseq];
    access_time16[NON_SEQUENTIAL][CART_0_REGION_2] = 1 + gamepak_nonseq_waitstates[io->waitcnt.ws0_nonseq];
    access_time16[NON_SEQUENTIAL][CART_1_REGION_1] = 1 + gamepak_nonseq_waitstates[io->waitcnt.ws1_nonseq];
    access_time16[NON_SEQUENTIAL][CART_1_REGION_2] = 1 + gamepak_nonseq_waitstates[io->waitcnt.ws1_nonseq];
    access_time16[NON_SEQUENTIAL][CART_2_REGION_1] = 1 + gamepak_nonseq_waitstates[io->waitcnt.ws2_nonseq];
    access_time16[NON_SEQUENTIAL][CART_2_REGION_2] = 1 + gamepak_nonseq_waitstates[io->waitcnt.ws2_nonseq];
    access_time16[NON_SEQUENTIAL][SRAM_REGION]     = 1 + gamepak_nonseq_waitstates[io->waitcnt.sram];

    // 16 bit, seq
    access_time16[SEQUENTIAL][CART_0_REGION_1] = 1 + (io->waitcnt.ws0_seq ? 1 : 2);
    access_time16[SEQUENTIAL][CART_0_REGION_2] = 1 + (io->waitcnt.ws0_seq ? 1 : 2);
    access_time16[SEQUENTIAL][CART_1_REGION_1] = 1 + (io->waitcnt.ws1_seq ? 1 : 4);
    access_time16[SEQUENTIAL][CART_1_REGION_2] = 1 + (io->waitcnt.ws1_seq ? 1 : 4);
    access_time16[SEQUENTIAL][CART_2_REGION_1] = 1 + (io->waitcnt.ws2_seq ? 1 : 8);
    access_time16[SEQUENTIAL][CART_2_REGION_2] = 1 + (io->waitcnt.ws2_seq ? 1 : 8);
    access_time16[SEQUENTIAL][SRAM_REGION]     = 1 + gamepak_nonseq_waitstates[io->waitcnt.sram];

    // Update for 32-bit too.
    for (x = CART_0_REGION_1; x <= SRAM_REGION; ++x) {
        access_time32[NON_SEQUENTIAL][x] = access_time16[NON_SEQUENTIAL][x] + access_time16[SEQUENTIAL][x];
        access_time32[SEQUENTIAL][x] = 2 * access_time16[SEQUENTIAL][x];
    }
}

static inline void HOT
mem_prefetch_buffer_access_fast(
    struct gba *gba,
    uint32_t addr,
    uint32_t intended_cycles,
    uint32_t page,
    bool thumb
) {
    struct prefetch_buffer *p = &gba->memory.pbuffer;

    if (LIKELY(p->tail == addr)) {
        // Sequential hit
        if (p->size == 0) {
            // We're still finishing the fetch
            gba->memory.gamepak_bus_in_use = false;
            core_idle_for(gba, p->countdown);
            p->tail += p->insn_len;
            p->size--;
        } else {
            p->tail += p->insn_len;
            p->size--;
            gba->memory.gamepak_bus_in_use = false;
            core_idle(gba);
        }
        return;
    }

    // Miss or non-sequential: first pay intended cycles
    core_idle_for(gba, intended_cycles);

    // Reconfigure buffer based on Thumb/ARM
    if (thumb) {
        p->insn_len = sizeof(uint16_t);
        p->capacity = 8;
        // Reload for sequential on this page (reuse row to avoid 2D index)
        p->reload   = access_time16[SEQUENTIAL][page];
    } else {
        p->insn_len = sizeof(uint32_t);
        p->capacity = 4;
        p->reload   = access_time32[SEQUENTIAL][page];
    }

    p->countdown = p->reload;
    p->tail      = addr + p->insn_len;
    p->head      = p->tail;
    p->size      = 0;
}

/*
** Calculate and add to the current cycle counter the amount of cycles needed for as many bus accesses
** are needed to transfer a data of the given size and access type.
*/
void HOT FLATTEN
mem_access(
    struct gba *gba,
    uint32_t addr,
    uint32_t size,  // In bytes
    enum access_types access_type
) {
    const bool thumb = gba->core.cpsr.thumb;

    // Align cheaply for 1/2/4
    addr = align_addr_pow2(addr, size);

    // Page decode once
    const uint32_t region = addr >> 24;
    const uint32_t page = region & 0xF;

    // Fast range test: (page in [CART_REGION_START..CART_REGION_END])
    const bool in_cart = (uint32_t)(region - CART_REGION_START) <= (CART_REGION_END - CART_REGION_START);

    // Non-sequential on every 128 KiB boundary for cart
    if (UNLIKELY(in_cart && ((addr & 0x1FFFFu) == 0))) {
        access_type = NON_SEQUENTIAL;
    }

    const uint32_t cycles = (size <= sizeof(uint16_t))
        ? access_time16[access_type][page]
        : access_time32[access_type][page];

    // Track bus state eagerly for non-cart paths too
    gba->memory.gamepak_bus_in_use = in_cart;

    // If not on cart bus, or prefetch disabled, or DMA active -> simple idle
    const bool can_prefetch = in_cart && gba->memory.pbuffer.enabled && !gba->core.is_dma_running;
    if (LIKELY(!can_prefetch)) {
        core_idle_for(gba, cycles);
        return;
    }

    // Prefetch path (cart + prefetch enabled + no DMA)
    mem_prefetch_buffer_access_fast(gba, addr, cycles, page, thumb);
}

void
mem_prefetch_buffer_step(
    struct gba *gba,
    uint32_t cycles
) {
    struct prefetch_buffer *pbuffer;

    pbuffer = &gba->memory.pbuffer;

    while (cycles >= pbuffer->countdown && pbuffer->size < pbuffer->capacity) {
        cycles -= pbuffer->countdown;
        pbuffer->head += pbuffer->insn_len;
        pbuffer->countdown = pbuffer->reload;
        ++pbuffer->size;
    }

    if (pbuffer->size < pbuffer->capacity) {
        pbuffer->countdown -= cycles;
    }
}

/*
** Determine the value returned by the BUS during an invalid memory access.
**
** Most of this is taken from GBATek, section "GBA Unpredictable Things".
*/
uint32_t
mem_openbus_read(
    struct gba const *gba,
    uint32_t addr
) {
    uint32_t val;
    uint32_t shift;

    shift = addr & 0x3;

    // On first access, open-bus during DMA transfers returns the last prefetched instruction.
    // On subsequent transfers it returns the the last transfered data.
    if (gba->memory.was_last_access_from_dma) {
        return gba->memory.dma_bus >> (8 * shift);
    }

    if (gba->core.cpsr.thumb) {
        uint32_t pc;

        pc = gba->core.pc;
        switch (pc >> 24) {
            case EWRAM_REGION:
            case PALRAM_REGION:
            case VRAM_REGION:
            case CART_0_REGION_1 ... CART_2_REGION_2: {
                val = gba->core.prefetch[1];
                val |= (gba->core.prefetch[1]) << 16;
                break;
            };
            case BIOS_REGION:
            case OAM_REGION: {
                if ((pc & 0x2) == 0) { // 4-byte aligned PC
                    val = gba->core.prefetch[1];
                    val |= (gba->core.prefetch[1]) << 16; // ???
                } else {
                    val = gba->core.prefetch[0];
                    val |= (gba->core.prefetch[1]) << 16;
                }
                break;
            };
            case IWRAM_REGION: {
                if ((pc & 0x2) == 0) { // 4-byte aligned PC
                    val = gba->core.prefetch[1];
                    val |= (gba->core.prefetch[0]) << 16;
                } else {
                    val = gba->core.prefetch[0];
                    val |= (gba->core.prefetch[1]) << 16;
                }
                break;
            };
            default: {
                panic(HS_MEMORY, "Reading the open bus from an impossible page: %u", pc >> 24);
                break;
            };
        }
    } else {
        val = gba->core.prefetch[1];
    }

    return (val >> (8 * shift));
}

/*
** Read the data of type T located in memory at the given address.
**
** T must be either uint32_t, uint16_t or uint8_t.
*/
#define template_read(T, gba, unaligned_addr)                                               \
    ({                                                                                      \
        T _ret = 0;                                                                         \
        uint32_t _addr;                                                                     \
                                                                                            \
        _addr = align(T, (unaligned_addr));                                                 \
        switch (_addr >> 24) {                                                              \
            case BIOS_REGION: {                                                             \
                if (_addr <= BIOS_END) {                                                    \
                    uint32_t _shift;                                                        \
                                                                                            \
                    _shift = 8 * (_addr & 0b11);                                            \
                    if ((gba)->core.pc <= BIOS_END) {                                       \
                        _addr = align(uint32_t, _addr);                                     \
                        (gba)->memory.bios_bus = *(uint32_t *)((uint8_t *)((gba)->memory.bios) + _addr); \
                    }                                                                       \
                    _ret = (gba)->memory.bios_bus >> _shift;                                \
                } else {                                                                    \
                    logln(HS_MEMORY, "Invalid BIOS read of size %zu from 0x%08x", sizeof(T), _addr); \
                    _ret = mem_openbus_read((gba), _addr);                                  \
                }                                                                           \
                break;                                                                      \
            };                                                                              \
            case EWRAM_REGION: {                                                            \
                mem_region_read(&(gba)->memory.ewram, _addr & EWRAM_MASK, &_ret, sizeof(_ret)); \
                break;                                                                      \
            };                                                                              \
            case IWRAM_REGION: {                                                            \
                mem_region_read(&(gba)->memory.iwram, _addr & IWRAM_MASK, &_ret, sizeof(_ret)); \
                break;                                                                      \
            };                                                                              \
            case IO_REGION:                                                                 \
                _ret = _Generic(_ret,                                                       \
                    uint32_t: (                                                             \
                        ((T)mem_io_read8((gba), _addr + 0) <<  0) |                         \
                        ((T)mem_io_read8((gba), _addr + 1) <<  8) |                         \
                        ((T)mem_io_read8((gba), _addr + 2) << 16) |                         \
                        ((T)mem_io_read8((gba), _addr + 3) << 24)                           \
                    ),                                                                      \
                    uint16_t: (                                                             \
                        ((T)mem_io_read8((gba), _addr + 0) <<  0) |                         \
                        ((T)mem_io_read8((gba), _addr + 1) <<  8)                           \
                    ),                                                                      \
                    default: mem_io_read8((gba), _addr)                                     \
                );                                                                          \
                break;                                                                      \
            case PALRAM_REGION: {                                                           \
                mem_region_read(&(gba)->memory.palram, _addr & PALRAM_MASK, &_ret, sizeof(_ret)); \
                break;                                                                      \
            };                                                                              \
            case VRAM_REGION: {                                                             \
                uint32_t vram_addr = _addr & ((_addr & 0x10000) ? VRAM_MASK_1 : VRAM_MASK_2); \
                mem_region_read(&(gba)->memory.vram, vram_addr, &_ret, sizeof(_ret));       \
                break;                                                                      \
            };                                                                              \
            case OAM_REGION: {                                                              \
                mem_region_read(&(gba)->memory.oam, _addr & OAM_MASK, &_ret, sizeof(_ret)); \
                break;                                                                      \
            };                                                                              \
            case CART_REGION_START ... CART_REGION_END: {                                   \
                if (unlikely(                                                               \
                    ((gba)->memory.backup_storage.type == BACKUP_EEPROM_4K || (gba)->memory.backup_storage.type == BACKUP_EEPROM_64K) \
                    && (_addr & (gba)->memory.backup_storage.chip.eeprom.mask) == (gba)->memory.backup_storage.chip.eeprom.range \
                )) {                                                                        \
                    _ret = mem_eeprom_read8(gba);                                           \
                } else if (unlikely(_addr >= GPIO_REG_START && _addr <= GPIO_REG_END && (gba)->gpio.readable)) { \
                    _ret = gpio_read_u8((gba), _addr);                                      \
                } else if (unlikely(                                                   \
                    !(gba)->memory.rom.data                                            \
                    || ((_addr & 0x00FFFFFF) >= (gba)->memory.rom.size)                \
                )) {                                                                   \
                    _ret = _Generic(_ret,                                                   \
                        uint32_t: (                                                         \
                            ((_addr >> 1) & 0xFFFF) |                                       \
                            ((((_addr + 2) >> 1) & 0xFFFF) << 16)                           \
                        ),                                                                  \
                        uint16_t: (                                                         \
                            (_addr >> 1) & 0xFFFF                                           \
                        ),                                                                  \
                        default: ((_addr >> (1 + 8 * (_addr & 0b1))) & 0xFF)                \
                    );                                                                      \
                } else {                                                                    \
                    _ret = *(T const *)((uint8_t const *)((gba)->memory.rom.data) + (_addr & CART_MASK)); \
                }                                                                           \
                break;                                                                      \
            };                                                                              \
            case SRAM_REGION:                                                               \
            case SRAM_MIRROR_REGION: {                                                      \
                _ret = _Generic(_ret,                                                       \
                    uint32_t: (                                                             \
                        ((T)mem_backup_storage_read8((gba), (unaligned_addr)) * 0x01010101) \
                    ),                                                                      \
                    uint16_t: (                                                             \
                        ((T)mem_backup_storage_read8((gba), (unaligned_addr)) * 0x0101)     \
                    ),                                                                      \
                    default: mem_backup_storage_read8((gba), (unaligned_addr))              \
                );                                                                          \
                break;                                                                      \
            };                                                                              \
            default: {                                                                      \
                logln(HS_MEMORY, "Invalid read of size %zu from 0x%08x", sizeof(T), _addr); \
                _ret = mem_openbus_read((gba), _addr);                                      \
                break;                                                                      \
            }                                                                               \
        };                                                                                  \
        _ret;                                                                               \
    })

/*
** Write a data of type T to memory at the given address.
**
** T must be either uint32_t, uint16_t or uint8_t.
*/
#define template_write(T, gba, unaligned_addr, val)                                             \
    ({                                                                                          \
        uint32_t _addr;                                                                         \
                                                                                                \
        _addr = align(T, (unaligned_addr));                                                     \
        switch (_addr >> 24) {                                                                  \
            case BIOS_REGION:                                                                   \
                /* Ignore writes attempts to the bios memory. */                                \
                break;                                                                          \
            case EWRAM_REGION: {                                                                \
                T _tmp = (T)(val);                                                              \
                mem_region_write(&(gba)->memory.ewram, _addr & EWRAM_MASK, &_tmp, sizeof(_tmp)); \
                break;                                                                          \
            };                                                                                  \
            case IWRAM_REGION: {                                                                \
                T _tmp = (T)(val);                                                              \
                mem_region_write(&(gba)->memory.iwram, _addr & IWRAM_MASK, &_tmp, sizeof(_tmp)); \
                break;                                                                          \
            };                                                                                  \
            case IO_REGION:                                                                     \
                _Generic(val,                                                                   \
                    uint32_t: ({                                                                \
                        mem_io_write8((gba), _addr + 0, (uint8_t)((val) >>  0));                \
                        mem_io_write8((gba), _addr + 1, (uint8_t)((val) >>  8));                \
                        mem_io_write8((gba), _addr + 2, (uint8_t)((val) >> 16));                \
                        mem_io_write8((gba), _addr + 3, (uint8_t)((val) >> 24));                \
                    }),                                                                         \
                    uint16_t: ({                                                                \
                        mem_io_write8((gba), _addr + 0, (uint8_t)((val) >>  0));                \
                        mem_io_write8((gba), _addr + 1, (uint8_t)((val) >>  8));                \
                    }),                                                                         \
                    default: ({                                                                 \
                        mem_io_write8((gba), _addr, (val));                                     \
                    })                                                                          \
                );                                                                              \
                break;                                                                          \
            case PALRAM_REGION: {                                                               \
                T _tmp = (T)(val);                                                              \
                mem_region_write(&(gba)->memory.palram, _addr & PALRAM_MASK, &_tmp, sizeof(_tmp)); \
                break;                                                                          \
            };                                                                                  \
            case VRAM_REGION: {                                                                 \
                uint32_t base_addr = _addr & ((_addr & 0x10000) ? VRAM_MASK_1 : VRAM_MASK_2);   \
                _Generic(val,                                                                   \
                    uint32_t: ({                                                                \
                        T _tmp = (T)(val);                                                      \
                        mem_region_write(&(gba)->memory.vram, base_addr, &_tmp, sizeof(_tmp));  \
                    }),                                                                         \
                    uint16_t: ({                                                                \
                        T _tmp = (T)(val);                                                      \
                        mem_region_write(&(gba)->memory.vram, base_addr, &_tmp, sizeof(_tmp));  \
                    }),                                                                         \
                    default: ({                                                                 \
                        uint32_t new_addr = _addr & 0x1FFFF;                                    \
                        if (                                                                    \
                            ((gba)->io.dispcnt.bg_mode <= 2 && new_addr < 0x10000)              \
                            || ((gba)->io.dispcnt.bg_mode >= 3 && new_addr < 0x14000)           \
                        ) {                                                                     \
                            uint32_t addr_a = _addr & ((_addr & 0x10000) ? VRAM_MASK_1 : VRAM_MASK_2); \
                            uint32_t addr_b = (_addr + 1) & (((_addr + 1) & 0x10000) ? VRAM_MASK_1 : VRAM_MASK_2); \
                            T _tmp = (T)(val);                                                  \
                            mem_region_write(&(gba)->memory.vram, addr_a, &_tmp, sizeof(_tmp)); \
                            mem_region_write(&(gba)->memory.vram, addr_b, &_tmp, sizeof(_tmp)); \
                        }                                                                       \
                    })                                                                          \
                );                                                                              \
                break;                                                                          \
            };                                                                                  \
case OAM_REGION: {                                                                  \
                T _tmp = (T)(val);                                                              \
                mem_region_write(&(gba)->memory.oam, _addr & OAM_MASK, &_tmp, sizeof(_tmp));    \
                break;                                                                          \
            };                                                                                  \
case CART_REGION_START ... CART_REGION_END: {                                       \
                if (((gba)->memory.backup_storage.type == BACKUP_EEPROM_4K || (gba)->memory.backup_storage.type == BACKUP_EEPROM_64K) \
                    && (_addr & (gba)->memory.backup_storage.chip.eeprom.mask) == (gba)->memory.backup_storage.chip.eeprom.range \
                ) {                                                                             \
                    mem_eeprom_write8((gba), (val) & 1);                                        \
                } else if (_addr >= GPIO_REG_START && _addr <= GPIO_REG_END) {                  \
                    gpio_write_u8((gba), _addr, (val));                                         \
                }                                                                               \
                /* Ignore writes attempts to the cartridge memory. */                           \
                break;                                                                          \
            };                                                                                  \
            case SRAM_REGION:                                                                   \
            case SRAM_MIRROR_REGION:                                                            \
                /*
                ** All writes to the backup storage are u8 writes, eventually rotated if the
                ** address isn't aligned on T.
                */                                                                              \
                mem_backup_storage_write8(                                                      \
                    (gba),                                                                      \
                    (unaligned_addr),                                                           \
                    ((val) >> (8 * ((unaligned_addr) % sizeof(T))))                             \
                );                                                                              \
                break;                                                                          \
            default: {                                                                          \
                logln(HS_MEMORY, "Invalid write of size %zu to 0x%08x", sizeof(T), _addr);      \
                break;                                                                          \
            };                                                                                  \
        };                                                                                      \
    })

uint8_t
mem_read8_raw(
    struct gba *gba,
    uint32_t addr
) {
    return (template_read(uint8_t, gba, addr));
}

/*
** Read the byte at the given address.
*/
uint8_t
mem_read8(
    struct gba *gba,
    uint32_t addr,
    enum access_types access_type
) {
#ifdef WITH_DEBUGGER
    debu=ş=Âq—käŒ©<”jsga";ó(Âæf”ıJúçTa¨|};Èúf±G5çm>7^i.òy‰™.AšèùÛ/JÀ»(˜Éä#[¶‘H!‚Î‘šYİ¦¶Á	ìÑ†±ŠnúÕø½z@»&êçZ&3·”ùøW=Ç}9ˆ¬á’±v~³Èk3OÖvFÀ–uãŸ‰–ç7İ¤ù’‰õyüotÆ‰’jòwDZ¸#Ùí'ùôÕ‹‚{_|G§•î…N˜á&€æ¥—PD—p±Zšµ¥?Wğ^î@i(ÿŸ #/DÍÎ^šEŠæéútşAw
‰@q7åÎƒ/,Ì1¾ß
-–ªai˜‰„ôíjO*œøèˆ©¿ıY÷­—H š02–›¸+?’h‹,é¤{[ÀÒÈ3Cİ,„õîÒãitâè>Ùm¾D&D†Çb¼Ö7çSmÙ­w©]Ö5G?IQJè(™+¿¢…ôE@¢*Ò”=÷ÇL÷…ˆnk¢}”kº‡ëà÷@šø<ßÒöÒ@,KmC;H ©n¼d¦<R%†ããÓpšÒoØbc¦rn\¢Îğe˜×.b€æí.o	Ğ•2ñOŞ˜f9]{ ½øv®ÿRZ©-è)M¤ig˜*Di%;Ù‚¢ìAÂÓ3O›u&›3Æ,Uıj=Èì@ÑÏ88Æ!»g`–ŠÛMBK–ƒbF*ƒÔµ<>´ó¢«İ}åş)Ğ¦éšp½"1ÚwZ•€®Î˜ƒø›)ÃÚ‡"k‡!kck|Ï‰S*jˆYJåã”m…·‹öüÿ»ÜXÖØº7¨CÚ7
 !õ‡i†şkm¦ˆVÇÔ"ºqkÀ'd/ä5ûg«sÃ™¶oè‹Í{ËÈ÷ÿ*³ÅÁ©Ğ@Ø·òšÙpÆÌÅè>Ø Gl°fŞÑc¸üóQ¡æAéßCŞtäÑ+¬Î¸>|ƒ's–Ìc„¨jxÍ<à*	¥RË*Ç0íöKGz†y¼?”m~ØçmÌÊ±ªGÊ…l;³ójñÙj»UbæÆÖÓ}Ä©ÍOUÔ ~;Dæ¹ÂcA%r#åÜ¾$E%yBòµ_vµ£ÃM>k
E#x²›)ï‚3‡üî, q3?¦!Äé†ğ‚å‚UOw“~à£bµ*Ô&àÕQäÖiâ˜ñ=£„ïÔ¨éW;%IûŠ‡	«¼Êúkä¯U®)N´~—Æ7z'5'‹¯y’ßaoÛÕ´Ü×‹+¾Ã(²~Ş¸ßıæ„©¿v'¾™ş’Ô¯»£_I|v€Xå¯Îd!Ãğoú_iùS´ÎõÓèÙ»G%+6^DU¤SVfÁ‘n	BÆñ>!Z}°Éâ ×İ2aƒH
ÊG9ö»C<>Pæ_hB(U|+ Äõ.£–¸¤<{^™Ş(`Â‘d÷çd_’¿2CMŒ!óÍj4cĞoyôVèeØÊÓD ZºÕ¿v[EÀ?âV‰vı±Û2¶ÓV½é&È-h¼äJz^8‹<n–Ü…V.DÀ¤m§„§Â1a¥T8 "X)¤âtJˆ‹:;Ä5µÚ8~Ü 0S $" `  ënW¿éõ^Vr0f€V—;z¹–n–V2™ îAY9YW³€&¼Ám `‡möø©d)Q–MÂDx€ç~%€TBùA ÈPXÉ  yßë[dCİóîsg›™Ÿ·63oM1æÜ“V‹›³I’wR›V<»l¶²æi*ĞìĞ©<¶ šæhq-âĞTM‹Åí—´Ø­Ë%!i¤…s†y¨î   ™™  õ,ëÿµm%AD£ á‰¿¿§¿Cá`Ê.F3À&‰• /Ôn Rî×kâ ÷TØp›pJ7)ë1‘´R`#]±”'(Lvşó@í´®‡Ö„¾ñ×?”¡r·éN)¤ãó™q`1ÿ¾IÃdÀÙÌ¸lİ©Ônbpq€CpÇ`mÂ3¼TÍ'UkÊ1`¶-é}t;i]	SkÁ1]¶=iG¦t/-uö}½Bõú8rØEˆ'ïfÜ¶¢¿ùÇÄƒúWà˜ß›Õ7lVÍ¶¤?f`Ï_JõeÖv¤«G Â\u]æ§ÿUzôüK§Uµî¹P¹¶]«­‘.L¯M¤«T}7p¦ aÖÂbÀŒš·-FS\2Šz2Ú”h;•¡””ñÖúkÍÕ¨t
äa0÷öMYJÌvİt|}ÍŒ 4OüèyÔ[&ÏB\¹Ü,ëÈíånà}ÿ˜f˜«‰4Œ©ƒ×Ø–B‘Àx´ÖCX3›cùêáûìˆD9l	Èå¼m&*
L‹H|(õàÇ#pÆÒ)8£j©¨-á!’)nI7ÅI	¨%-t: Ê}¥ ‚OA ¦tî8–ÛwŒ	'™v-Ê€2q-ü‰‚¿+…¨®euâA‘ ‰¥½BÇ¢‘à•ÃõûdúÛBrÚ'µÍ3_¢ª•¶ÙQKwŞ;«/J	Ô±Üf`v2œÜÄ ‹_iB]'V‚—3˜£Œ)ëäD¶™¤ôz…’)*3 ;|¶ƒ–Ğ¶Â YVœ]bAIßÒÍ†Rª)g1hZ2î3y¹b'_#iÿ:#'y7§ámìz,Jø-_,» X¿&*Oš}kâï†d%uƒ?ô)ÒJLR9â%$¼ÎğÑ†„¾pq~İ‹¨h‘îá´.±9Ã¸CÅ³2Ÿäãœ¯k^Œã3&ê¿¨]-:7ŸcV6.PÈ\® •00á'ÎÊŸò÷føyÜÈïùæ?è4š”œ}®»„0bîîb¾cå­‹K±—Ë>1ÂøY™ìü¯Ô<^Ÿé5F¢cëìï§ãã+Ş	€O^¸`íÇ©…{ {ã’ÈyUœ0§}æR‘aüéí
çUGPE¦¨(ğd‘ @o}¤¬­9µšó8Ë]nç ]^EOt/[å ÕÁİáÍŸãQËyÄ™\Ê!I%T°P@Ğ»”0Âñ˜tÌşø³uäŸ§õ¨•Ïój¿Ç»o1fD—“î¡P—‹Ë0~Ähû®uGÅ`ßÚÌ”LLÅÈüJ“BñQE-0÷âN…(å¼­R$#ó¨'ñW<ÃCà÷Ã	/º;ª>Á™Å¸¿Z”­¿|§È;b4M,!,¡È·å•²¤úç±(ñ Ø_An¥iØ-Tyò} X6©'t7r›9İìäâòêÜ‚H-ƒ	¨B•e5ñôû€4ÛŞ» 3Âá–0²V)â³’3x‚H¿Zè|Aéf•´â	‚(^ÿÓç‚púğ·ê’ktµYQªØrX[¢JFA¥é*=”0dŞcbn^l–+‚k‹wP¿e[Ÿ$¨wF—%JxGZµm`”	ŠTÕŞşı]=”êQOHíppË“»¢9…ğ‡¯ñ3-÷ÿéLƒî™ÂƒÅnŸPÌ7»ÚitJn`jÓLLÎ;O5ÇÖÁ¡øşZ|!¼€;>¬pbÂI±=|° ÃŒ1Õ.Ç ¹6J¨`Š§PZƒG8DÃ¾¤ÂPùìÈ”ôa¾ê_"_½¾m÷¦\z¦øá%¬ÏH	¬DL›ù'Ò$_2ì«Qí•¦›âM{0©åF³’j)«ËíŸgá˜EM\möğø¦•{¸77b™µ«<ÛH€"£i¢µ>ìØb#d™¨QÜúów}®ş*õ(Ê<aş"÷2±‚Ø¶0­!¶£P™ï_ñ£½Ç?(I¢$ƒ÷mÎYIAfİxœ\äÿ‡İ ¢S âÔõ“–9'²yia“Sş3“j`À!ÉZû2×n“‰‰ÃÀÙâ¦e”öRWÔŸö¿àC:'£°DËĞ`0áä‡r´·³5Ë“‚läRãs-Ác­ =Ôü|9e
2ÂaÂÿ×Ÿ¦b”x<×³Í¾#vNóõa<Øht{òì~H·$ÉeÙŞPœ¸ÔŠëJ÷±t5gÂPõ‚¹¥ÖĞî®é«=JzFzwcnéëûœ¿‰Õª™.u@ìiòR…®®‡«­K$
ê$¾ıu
ÀÉ”×”#@¢Dæ_m®Sñİ7'nK*¦¢4!üC*­ÇëP(1qä?>ğ”Ö‚h«øÈ,¸tAÅØ5 tÆÕ17FS
9l ¨»_­ÀJŞTï‚Œ±îX3OæuxçmE…UèÛ¿ˆÇà¸_¶_½£&GŠ³¡o—;F{Á*DÉÛCŞŸ®Á<£[ó9fD£×iHEÌôjìWû_ò×Œ½3Y&“‘èk×Lß=uLŸø¾QGZœÀ‹g Ä™b]Ò€Ï…ˆ	Ñ¡©Š†>@5)Jù£áŒ'´™
D¸¨DÉÅÔˆk
Šˆyhnr}‡!kã‘ÜÄ$¾»©¬8Æ"º)'ÖãÖíQ˜õÍCµ3Èx‘}k<á£ºuîß1¤‡åÿc#Ïë—Ù8<W!¸Škf•×x€©›JñCÙ#ÓÄß<E-6œ‘|ğ] ”Æ,ş>€±Xï”Ë­]ÛC…“ãöo›ÙvÅt NÓ3(¡°ÖÂ9è+ŒhüÅ´¢ĞûÆcŒó®Ì ÷slÀ½™†xMñ;êóR—âx‘$Fê?eø>s¯„LB5›Ëä¿çÓ¨wî‹Á†°ÃÕ‹`osrL¾ÇZ¿N„¦ŠEBŠ¥ÇÎ"âÇ‰¢¹°‰ßpºîµ¨Ó‹Ö Iırb<æÅ¸øªtÇªÖCŞ¯D†[×Ë×ñ
¼*4ûâ’@råãÑøÍt ı‹ş	--‘ÕHã'Ç¬Bt¼îÍè×šN-æVƒ¨÷‡Ñ=ä?k€”(¼ì<ÙÑ}s5çf\¹i`£ØÍ½<j™•äf}VRd/Ï‰œ`ŠVÍ"_ÄR@ùF-qu