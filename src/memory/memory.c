/******************************************************************************\
**
**  This file is part of the Hades GBA Emulator, and is made available under
**  the terms of the GNU General Public License version 2.
**
**  Copyright (C) 2021-2024 - The Hades Authors
**
\******************************************************************************/

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
    uint32_t page
) {
    struct prefetch_buffer *p = &gba->memory.pbuffer;

    if (LIKELY(p->tail == addr)) {
        // Sequential hit
        if (p->size == 0) {
            // We're still finishing the fetch
            gba->memory.gamepak_bus_in_use = false;
            core_idle_for(gba, p->countdown);
            p->tail += p->insn_len;
            p->size = (uint8_t)(p->size - 1); // becomes 0xFF but ignored if unsigned? If signed, keep original.
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
    const bool thumb = gba->core.cpsr.thumb;
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
    // Align cheaply for 1/2/4
    addr = align_addr_pow2(addr, size);

    // Page decode once
    const uint32_t page = (addr >> 24) & 0xF;

    // Fast range test: (page in [CART_REGION_START..CART_REGION_END])
    const uint32_t cart_lo = CART_REGION_START;
    const uint32_t cart_hi = CART_REGION_END;
    const bool in_cart = (uint32_t)(page - cart_lo) <= (cart_hi - cart_lo);

    // Non-sequential on every 128 KiB boundary for cart
    if (UNLIKELY(in_cart && ((addr & 0x1FFFFu) == 0))) {
        access_type = NON_SEQUENTIAL;
    }

    // Pick row once, avoid 2D index twice
    const uint32_t *row16 = access_time16[access_type];
    const uint32_t *row32 = access_time32[access_type];

    const uint32_t cycles = (size <= sizeof(uint16_t)) ? row16[page] : row32[page];

    // If not on cart bus, or prefetch disabled, or DMA active -> simple idle
    if (!in_cart || !gba->memory.pbuffer.enabled || gba->core.is_dma_running) {
        gba->memory.gamepak_bus_in_use = in_cart;
        core_idle_for(gba, cycles);
        return;
    }

    // Prefetch path (cart + prefetch enabled + no DMA)
    gba->memory.gamepak_bus_in_use = true;
    mem_prefetch_buffer_access_fast(gba, addr, cycles, page);
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
            case EWRAM_REGION:                                                              \
                _ret = *(T *)((uint8_t *)((gba)->memory.ewram) + (_addr & EWRAM_MASK));     \
                break;                                                                      \
            case IWRAM_REGION:                                                              \
                _ret = *(T *)((uint8_t *)((gba)->memory.iwram) + (_addr & IWRAM_MASK));     \
                break;                                                                      \
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
            case PALRAM_REGION:                                                             \
                _ret = *(T *)((uint8_t *)((gba)->memory.palram) + (_addr & PALRAM_MASK));   \
                break;                                                                      \
            case VRAM_REGION:                                                               \
                _ret = *(T *)((uint8_t *)((gba)->memory.vram) + (_addr & ((_addr & 0x10000) ? VRAM_MASK_1 : VRAM_MASK_2))); \
                break;                                                                      \
            case OAM_REGION:                                                                \
                _ret = *(T *)((uint8_t *)((gba)->memory.oam) + (_addr & OAM_MASK));         \
                break;                                                                      \
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
            case EWRAM_REGION:                                                                  \
                *(T *)((uint8_t *)((gba)->memory.ewram) + (_addr & EWRAM_MASK)) = (T)(val);     \
                break;                                                                          \
            case IWRAM_REGION:                                                                  \
                *(T *)((uint8_t *)((gba)->memory.iwram) + (_addr & IWRAM_MASK)) = (T)(val);     \
                break;                                                                          \
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
                _Generic(val,                                                                   \
                    uint32_t: ({                                                                \
                        *(T *)((uint8_t *)((gba)->memory.palram) + (_addr & PALRAM_MASK)) = (T)(val); \
                    }),                                                                         \
                    uint16_t: ({                                                                \
                        *(T *)((uint8_t *)((gba)->memory.palram) + (_addr & PALRAM_MASK)) = (T)(val); \
                    }),                                                                         \
                    default: ({                                                                 \
                        /* u8 writes to PALRAM are writting to both the upper/lower bytes */    \
                        addr &= ~(sizeof(uint16_t) - 1);                                        \
                        *(T *)((uint8_t *)((gba)->memory.palram) + (_addr & PALRAM_MASK)) = (T)(val); \
                        *(T *)((uint8_t *)((gba)->memory.palram) + ((_addr + 1) & PALRAM_MASK)) = (T)(val); \
                    })                                                                          \
                );                                                                              \
                break;                                                                          \
            };                                                                                  \
            case VRAM_REGION: {                                                                 \
                _Generic(val,                                                                   \
                    uint32_t: ({                                                                \
                        *(T *)((uint8_t *)((gba)->memory.vram) + (_addr & ((_addr & 0x10000) ? VRAM_MASK_1 : VRAM_MASK_2))) = (T)(val); \
                    }),                                                                         \
                    uint16_t: ({                                                                \
                        *(T *)((uint8_t *)((gba)->memory.vram) + (_addr & ((_addr & 0x10000) ? VRAM_MASK_1 : VRAM_MASK_2))) = (T)(val); \
                    }),                                                                         \
                    default: ({                                                                 \
                        uint32_t new_addr;                                                      \
                                                                                                \
                        new_addr = _addr & 0x1FFFF;                                             \
                        /*
                        ** Ignore u8 write attemps to OBJ VRAM memory
                        ** OBJ VRAM size is different depending on the BG mode.
                        */                                                                      \
                        if (                                                                    \
                            ((gba)->io.dispcnt.bg_mode <= 2 && (new_addr) < 0x10000)            \
                            || ((gba)->io.dispcnt.bg_mode >= 3 && (new_addr) < 0x14000)         \
                        ) {                                                                     \
                            addr &= ~(sizeof(uint16_t) - 1);                                    \
                            *(T *)((uint8_t *)((gba)->memory.vram) + (_addr & ((_addr & 0x10000) ? VRAM_MASK_1 : VRAM_MASK_2))) = (T)(val); \
                            *(T *)((uint8_t *)((gba)->memory.vram) + ((_addr + 1) & (((_addr + 1) & 0x10000) ? VRAM_MASK_1 : VRAM_MASK_2))) = (T)(val); \
                        }                                                                       \
                    })                                                                          \
                );                                                                              \
                break;                                                                          \
            };                                                                                  \
            case OAM_REGION: {                                                                  \
                _Generic(val,                                                                   \
                    uint32_t: ({                                                                \
                        *(T *)((uint8_t *)((gba)->memory.oam) + (_addr & OAM_MASK)) = (T)(val); \
                    }),                                                                         \
                    uint16_t: ({                                                                \
                        *(T *)((uint8_t *)((gba)->memory.oam) + (_addr & OAM_MASK)) = (T)(val); \
                    }),                                                                         \
                    default: ({                                                                 \
                        /* Ignore u8 write attemps to OAM memory */                             \
                    })                                                                          \
                );                                                                              \
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
    debugger_eval_read_watchpoints(gba, addr, sizeof(uint8_t));
#endif

    mem_access(gba, addr, sizeof(uint8_t), access_type);
    return (template_read(uint8_t, gba, addr));
}

uint16_t
mem_read16_raw(
    struct gba *gba,
    uint32_t addr
) {
    return (template_read(uint16_t, gba, addr));
}

/*
** Read the half-word at the given address.
*/
uint16_t
mem_read16(
    struct gba *gba,
    uint32_t addr,
    enum access_types access_type
) {
#ifdef WITH_DEBUGGER
    debugger_eval_read_watchpoints(gba, addr, sizeof(uint16_t));
#endif

    mem_access(gba, addr, sizeof(uint16_t), access_type);
    return (template_read(uint16_t, gba, addr));
}

/*
** Read the half-word at the given address and ROR it if the
** address isn't aligned.
*/
uint32_t
mem_read16_ror(
    struct gba *gba,
    uint32_t addr,
    enum access_types access_type
) {
    uint32_t rotate;
    uint32_t value;

#ifdef WITH_DEBUGGER
    debugger_eval_read_watchpoints(gba, addr, sizeof(uint16_t));
#endif

    mem_access(gba, addr, sizeof(uint16_t), access_type);

    rotate = (addr & 0b1) * 8;
    value = template_read(uint16_t, gba, addr);

    /* Unaligned 16-bits loads are supposed to be unpredictable, but in practise the GBA rotates them */
    return (ror32(value, rotate));
}

uint32_t
mem_read32_raw(
    struct gba *gba,
    uint32_t addr
) {
    return (template_read(uint32_t, gba, addr));
}

/*
** Read the word at the given address.
*/
uint32_t
mem_read32(
    struct gba *gba,
    uint32_t addr,
    enum access_types access_type
) {
#ifdef WITH_DEBUGGER
    debugger_eval_read_watchpoints(gba, addr, sizeof(uint32_t));
#endif

    mem_access(gba, addr, sizeof(uint32_t), access_type);
    return (template_read(uint32_t, gba, addr));
}

/*
** Read the word at the given address and ROR it if the
** address isn't aligned.
*/
uint32_t
mem_read32_ror(
    struct gba *gba,
    uint32_t addr,
    enum access_types access_type
) {
    uint32_t rotate;
    uint32_t value;

#ifdef WITH_DEBUGGER
    debugger_eval_read_watchpoints(gba, addr, sizeof(uint32_t));
#endif

    mem_access(gba, addr, sizeof(uint32_t), access_type);

    rotate = (addr % 4) << 3;
    value = template_read(uint32_t, gba, addr);

    return (ror32(value, rotate));
}

void
mem_write8_raw(
    struct gba *gba,
    uint32_t addr,
    uint8_t val
) {
    template_write(uint8_t, gba, addr, val);
}

/*
** Write a byte at the given address.
*/
void
mem_write8(
    struct gba *gba,
    uint32_t addr,
    uint8_t val,
    enum access_types access_type
) {
#ifdef WITH_DEBUGGER
    debugger_eval_write_watchpoints(gba, addr, sizeof(uint8_t), val);
#endif

    mem_access(gba, addr, sizeof(uint8_t), access_type);
    template_write(uint8_t, gba, addr, val);
}

void
mem_write16_raw(
    struct gba *gba,
    uint32_t addr,
    uint16_t val
) {
    template_write(uint16_t, gba, addr, val);
}


/*
** write a half-word at the given address.
*/
void
mem_write16(
    struct gba *gba,
    uint32_t addr,
    uint16_t val,
    enum access_types access_type
) {
#ifdef WITH_DEBUGGER
    debugger_eval_write_watchpoints(gba, addr, sizeof(uint16_t), val);
#endif

    mem_access(gba, addr, sizeof(uint16_t), access_type);
    template_write(uint16_t, gba, addr, val);
}

void
mem_write32_raw(
    struct gba *gba,
    uint32_t addr,
    uint32_t val
) {
    template_write(uint32_t, gba, addr, val);
}

/*
** Write a word at the given address.
*/
void
mem_write32(
    struct gba *gba,
    uint32_t addr,
    uint32_t val,
    enum access_types access_type
) {
#ifdef WITH_DEBUGGER
    debugger_eval_write_watchpoints(gba, addr, sizeof(uint32_t), val);
#endif

    mem_access(gba, addr, sizeof(uint32_t), access_type);
    template_write(uint32_t, gba, addr, val);
}
