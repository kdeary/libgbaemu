/******************************************************************************\
**
**  This file is part of the Hades GBA Emulator, and is made available under
**  the terms of the GNU General Public License version 2.
**
**  Copyright (C) 2021-2024 - The Hades Authors
**
\******************************************************************************/

#include <string.h>
#include <stdatomic.h>
#include "gba/gba.h"

// Not always true, but it's for optimization purposes so it's not a big deal
// if the page size isn't 4k.
#define PAGE_SIZE           4096u
#define PAGE_MASK           (PAGE_SIZE - 1)
#define PAGE_ALIGN(size)    ((size + PAGE_SIZE) & ~PAGE_MASK)

struct quicksave_buffer {
    uint8_t *data;
    size_t size;    // Allocated size
    size_t index;   // Read/Write index
};

enum quicksave_chunk_kind {
    QS_CHUNK_CORE = 1,
    QS_CHUNK_IO,
    QS_CHUNK_PPU,
    QS_CHUNK_GPIO,
    QS_CHUNK_APU,
    QS_CHUNK_SCHEDULER,
    QS_CHUNK_SCHED_EVENTS,
    QS_CHUNK_MEMORY_META,
    QS_CHUNK_EWRAM,
    QS_CHUNK_IWRAM,
    QS_CHUNK_VRAM,
    QS_CHUNK_PALRAM,
    QS_CHUNK_OAM,
    QS_CHUNK_BACKUP_STORAGE,
};

enum quicksave_region_encoding {
    QS_REGION_RAW = 0,
    QS_REGION_RLE = 1,
};

struct quicksave_header {
    char magic[4];
    uint32_t version;
    uint32_t rom_size;
    uint32_t rom_code;
};

struct quicksave_chunk_header {
    uint32_t kind;
    uint32_t size;
};

struct quicksave_region_header {
    uint32_t decoded_size;
    uint8_t encoding;
    uint8_t reserved[3];
};

struct quicksave_scheduler_snapshot {
    uint64_t cycles;
    uint64_t next_event;
    size_t events_len;
};

struct quicksave_memory_meta {
    struct flash flash;
    struct eeprom eeprom;
    enum backup_storage_types backup_type;
    struct prefetch_buffer pbuffer;
    uint32_t bios_bus;
    uint32_t dma_bus;
    bool was_last_access_from_dma;
    bool gamepak_bus_in_use;
};

struct quicksave_backup_snapshot {
    size_t size;
    bool dirty;
};

#define QUICKSAVE_MAGIC       "HSQS"
#define QUICKSAVE_VERSION     2u

static void quicksave_buffer_reserve(struct quicksave_buffer *buffer, size_t length) {
    if (buffer->index + length > buffer->size) {
        buffer->size = PAGE_ALIGN(buffer->index + length);
        buffer->data = realloc(buffer->data, buffer->size);
        hs_assert(buffer->data);
    }
}

static void quicksave_write(
    struct quicksave_buffer *buffer,
    uint8_t const *data,
    size_t length
) {
    quicksave_buffer_reserve(buffer, length);
    memcpy(buffer->data + buffer->index, data, length);
    buffer->index += length;
}

static bool quicksave_read(
    struct quicksave_buffer *buffer,
    uint8_t *data,
    size_t length
) {
    if (buffer->size < buffer->index + length) {
        return true;
    }

    memcpy(data, buffer->data + buffer->index, length);
    buffer->index += length;
    return false;
}

static void quicksave_buffer_free(struct quicksave_buffer *buffer) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
    buffer->index = 0;
}

static uint32_t quicksave_rom_code(struct rom_view const *rom) {
    uint32_t code = 0;

    if (rom->data && rom->size >= 0xC0) {
        memcpy(&code, rom->data + 0xAC, sizeof(code));
    }
    return code;
}

static void quicksave_write_chunk(
    struct quicksave_buffer *buffer,
    enum quicksave_chunk_kind kind,
    void const *data,
    size_t size
) {
    struct quicksave_chunk_header header;

    hs_assert(size <= UINT32_MAX);
    header.kind = (uint32_t)kind;
    header.size = (uint32_t)size;
    quicksave_write(buffer, (uint8_t *)&header, sizeof(header));
    if (size) {
        quicksave_write(buffer, data, size);
    }
}

static void quicksave_write_chunk_buffer(
    struct quicksave_buffer *buffer,
    enum quicksave_chunk_kind kind,
    struct quicksave_buffer const *chunk
) {
    struct quicksave_chunk_header header;

    hs_assert(chunk->index <= UINT32_MAX);
    header.kind = (uint32_t)kind;
    header.size = (uint32_t)chunk->index;
    quicksave_write(buffer, (uint8_t *)&header, sizeof(header));
    if (chunk->index) {
        quicksave_write(buffer, chunk->data, chunk->index);
    }
}

static void quicksave_encode_rle(
    struct quicksave_buffer *out,
    uint8_t const *data,
    size_t size
) {
    size_t i;

    for (i = 0; i < size;) {
        size_t run;
        uint8_t value;
        uint16_t chunk_len;

        value = data[i];
        run = 1;
        while (i + run < size && data[i + run] == value && run < UINT16_MAX) {
            ++run;
        }
        chunk_len = (uint16_t)run;
        quicksave_write(out, (uint8_t *)&chunk_len, sizeof(chunk_len));
        quicksave_write(out, &value, sizeof(value));
        i += run;
    }
}

static void quicksave_write_region_payload(
    struct quicksave_buffer *out,
    uint8_t const *data,
    size_t size
) {
    struct quicksave_region_header header;
    struct quicksave_buffer rle_payload = { 0 };

    header.decoded_size = (uint32_t)size;
    header.encoding = QS_REGION_RAW;
    memset(header.reserved, 0, sizeof(header.reserved));

    if (size) {
        quicksave_encode_rle(&rle_payload, data, size);
    }

    if (rle_payload.index > 0 && rle_payload.index < size) {
        header.encoding = QS_REGION_RLE;
        quicksave_write(out, (uint8_t *)&header, sizeof(header));
        quicksave_write(out, rle_payload.data, rle_payload.index);
    } else {
        header.encoding = QS_REGION_RAW;
        quicksave_write(out, (uint8_t *)&header, sizeof(header));
        if (size) {
            quicksave_write(out, data, size);
        }
    }

    quicksave_buffer_free(&rle_payload);
}

static void quicksave_write_region_chunk(
    struct quicksave_buffer *buffer,
    enum quicksave_chunk_kind kind,
    uint8_t const *data,
    size_t size
) {
    struct quicksave_buffer chunk = { 0 };

    quicksave_write_region_payload(&chunk, data, size);
    quicksave_write_chunk_buffer(buffer, kind, &chunk);
    quicksave_buffer_free(&chunk);
}

static bool quicksave_read_region(
    struct quicksave_buffer *buffer,
    size_t chunk_end,
    uint8_t *dst,
    size_t dst_size
) {
    struct quicksave_region_header header;

    if (dst_size && !dst) {
        return true;
    }

    if (quicksave_read(buffer, (uint8_t *)&header, sizeof(header))) {
        return true;
    }
    if (header.decoded_size != dst_size) {
        return true;
    }

    switch (header.encoding) {
        case QS_REGION_RAW: {
            if (buffer->index + dst_size > chunk_end) {
                return true;
            }
            if (dst_size) {
                return quicksave_read(buffer, dst, dst_size);
            }
            return false;
        };
        case QS_REGION_RLE: {
            size_t produced;

            produced = 0;
            while (produced < dst_size) {
                uint16_t run_len;
                uint8_t value;

                if (buffer->index + sizeof(run_len) + sizeof(value) > chunk_end) {
                    return true;
                }

                if (quicksave_read(buffer, (uint8_t *)&run_len, sizeof(run_len))) {
                    return true;
                }
                if (quicksave_read(buffer, &value, sizeof(value))) {
                    return true;
                }

                if ((size_t)run_len > dst_size - produced) {
                    return true;
                }

                memset(dst + produced, value, run_len);
                produced += run_len;
            }
            return false;
        };
        default:
            return true;
    }
}

static bool quickload_v1(
    struct gba *gba,
    uint8_t *data,
    size_t size
);

/*
** Save the current state of the emulator in the given buffer.
*/
void
quicksave(
    struct gba const *gba,
    uint8_t **data,
    size_t *size
) {
    struct quicksave_buffer buffer;
    struct quicksave_header header;
    struct quicksave_scheduler_snapshot sched;
    struct quicksave_memory_meta memory_meta;

    buffer.data = NULL;
    buffer.size = 0;
    buffer.index = 0;

    memcpy(header.magic, QUICKSAVE_MAGIC, sizeof(header.magic));
    header.version = QUICKSAVE_VERSION;
    header.rom_size = (uint32_t)min(gba->memory.rom.size, (size_t)UINT32_MAX);
    header.rom_code = quicksave_rom_code(&gba->memory.rom);
    quicksave_write(&buffer, (uint8_t *)&header, sizeof(header));

    quicksave_write_chunk(&buffer, QS_CHUNK_CORE, &gba->core, sizeof(gba->core));
    quicksave_write_chunk(&buffer, QS_CHUNK_IO, &gba->io, sizeof(gba->io));
    quicksave_write_chunk(&buffer, QS_CHUNK_PPU, &gba->ppu, sizeof(gba->ppu));
    quicksave_write_chunk(&buffer, QS_CHUNK_GPIO, &gba->gpio, sizeof(gba->gpio));
    quicksave_write_chunk(&buffer, QS_CHUNK_APU, &gba->apu, sizeof(gba->apu));

    sched.cycles = gba->scheduler.cycles;
    sched.next_event = gba->scheduler.next_event;
    sched.events_len = gba->scheduler.events_size;
    quicksave_write_chunk(&buffer, QS_CHUNK_SCHEDULER, &sched, sizeof(sched));
    if (sched.events_len && gba->scheduler.events) {
        quicksave_write_chunk(
            &buffer,
            QS_CHUNK_SCHED_EVENTS,
            gba->scheduler.events,
            sched.events_len * sizeof(struct scheduler_event)
        );
    }

    memory_meta.flash = gba->memory.backup_storage.chip.flash;
    memory_meta.eeprom = gba->memory.backup_storage.chip.eeprom;
    memory_meta.backup_type = gba->memory.backup_storage.type;
    memory_meta.pbuffer = gba->memory.pbuffer;
    memory_meta.bios_bus = gba->memory.bios_bus;
    memory_meta.dma_bus = gba->memory.dma_bus;
    memory_meta.was_last_access_from_dma = gba->memory.was_last_access_from_dma;
    memory_meta.gamepak_bus_in_use = gba->memory.gamepak_bus_in_use;
    quicksave_write_chunk(&buffer, QS_CHUNK_MEMORY_META, &memory_meta, sizeof(memory_meta));

    quicksave_write_region_chunk(&buffer, QS_CHUNK_EWRAM, gba->memory.ewram, sizeof(gba->memory.ewram));
    quicksave_write_region_chunk(&buffer, QS_CHUNK_IWRAM, gba->memory.iwram, sizeof(gba->memory.iwram));
    quicksave_write_region_chunk(&buffer, QS_CHUNK_VRAM, gba->memory.vram, sizeof(gba->memory.vram));
    quicksave_write_region_chunk(&buffer, QS_CHUNK_PALRAM, gba->memory.palram, sizeof(gba->memory.palram));
    quicksave_write_region_chunk(&buffer, QS_CHUNK_OAM, gba->memory.oam, sizeof(gba->memory.oam));

    if (gba->shared_data.backup_storage.size && gba->shared_data.backup_storage.data) {
        struct quicksave_backup_snapshot backup_meta;
        struct quicksave_buffer chunk = { 0 };

        backup_meta.size = gba->shared_data.backup_storage.size;
        backup_meta.dirty = atomic_load(&gba->shared_data.backup_storage.dirty);

        quicksave_write(&chunk, (uint8_t *)&backup_meta, sizeof(backup_meta));
        quicksave_write_region_payload(&chunk, gba->shared_data.backup_storage.data, backup_meta.size);
        quicksave_write_chunk_buffer(&buffer, QS_CHUNK_BACKUP_STORAGE, &chunk);
        quicksave_buffer_free(&chunk);
    }

    *data = buffer.data;
    *size = buffer.size;
}

/*
** Load a new state for the emulator from the given save state.
*/
bool
quickload(
    struct gba *gba,
    uint8_t *data,
    size_t size
) {
    struct quicksave_buffer buffer;
    struct quicksave_header header;
    struct quicksave_scheduler_snapshot sched = { 0 };
    struct scheduler_event *events_tmp = NULL;
    size_t events_tmp_len = 0;
    bool seen_core = false;
    bool seen_io = false;
    bool seen_ppu = false;
    bool seen_gpio = false;
    bool seen_apu = false;
    bool seen_sched = false;
    bool seen_memory_meta = false;
    bool seen_ewram = false;
    bool seen_iwram = false;
    bool seen_vram = false;
    bool seen_palram = false;
    bool seen_oam = false;
    bool seen_backup = false;

    buffer.data = data;
    buffer.size = size;
    buffer.index = 0;

    if (
        size < sizeof(struct quicksave_header)
        || quicksave_read(&buffer, (uint8_t *)&header, sizeof(header))
        || memcmp(header.magic, QUICKSAVE_MAGIC, sizeof(header.magic)) != 0
    ) {
        return quickload_v1(gba, data, size);
    }

    if (header.version != QUICKSAVE_VERSION) {
        return true;
    }

    if (
        header.rom_size != (uint32_t)min(gba->memory.rom.size, (size_t)UINT32_MAX)
        || header.rom_code != quicksave_rom_code(&gba->memory.rom)
    ) {
        return true;
    }

    free(gba->scheduler.events);
    gba->scheduler.events = NULL;
    gba->scheduler.events_size = 0;

    while (buffer.index < buffer.size) {
        struct quicksave_chunk_header chunk;
        size_t chunk_end;

        if (quicksave_read(&buffer, (uint8_t *)&chunk, sizeof(chunk))) {
            goto error;
        }
        chunk_end = buffer.index + chunk.size;
        if (chunk_end > buffer.size) {
            goto error;
        }

        switch (chunk.kind) {
            case QS_CHUNK_CORE: {
                if (chunk.size != sizeof(gba->core) || quicksave_read(&buffer, (uint8_t *)&gba->core, sizeof(gba->core))) {
                    goto error;
                }
                seen_core = true;
                break;
            };
            case QS_CHUNK_IO: {
                if (chunk.size != sizeof(gba->io) || quicksave_read(&buffer, (uint8_t *)&gba->io, sizeof(gba->io))) {
                    goto error;
                }
                seen_io = true;
                break;
            };
            case QS_CHUNK_PPU: {
                if (chunk.size != sizeof(gba->ppu) || quicksave_read(&buffer, (uint8_t *)&gba->ppu, sizeof(gba->ppu))) {
                    goto error;
                }
                seen_ppu = true;
                break;
            };
            case QS_CHUNK_GPIO: {
                if (chunk.size != sizeof(gba->gpio) || quicksave_read(&buffer, (uint8_t *)&gba->gpio, sizeof(gba->gpio))) {
                    goto error;
                }
                seen_gpio = true;
                break;
            };
            case QS_CHUNK_APU: {
                if (chunk.size != sizeof(gba->apu) || quicksave_read(&buffer, (uint8_t *)&gba->apu, sizeof(gba->apu))) {
                    goto error;
                }
                seen_apu = true;
                break;
            };
            case QS_CHUNK_SCHEDULER: {
                if (chunk.size != sizeof(sched) || quicksave_read(&buffer, (uint8_t *)&sched, sizeof(sched))) {
                    goto error;
                }
                seen_sched = true;
                break;
            };
            case QS_CHUNK_SCHED_EVENTS: {
                if (chunk.size % sizeof(struct scheduler_event) != 0) {
                    goto error;
                }
                events_tmp_len = chunk.size / sizeof(struct scheduler_event);
                if (!events_tmp_len) {
                    break;
                }
                events_tmp = calloc(events_tmp_len, sizeof(struct scheduler_event));
                hs_assert(events_tmp);
                if (quicksave_read(&buffer, (uint8_t *)events_tmp, chunk.size)) {
                    goto error;
                }
                break;
            };
            case QS_CHUNK_MEMORY_META: {
                struct quicksave_memory_meta meta;

                if (chunk.size != sizeof(meta) || quicksave_read(&buffer, (uint8_t *)&meta, sizeof(meta))) {
                    goto error;
                }

                gba->memory.backup_storage.chip.flash = meta.flash;
                gba->memory.backup_storage.chip.eeprom = meta.eeprom;
                gba->memory.backup_storage.type = meta.backup_type;
                gba->memory.pbuffer = meta.pbuffer;
                gba->memory.bios_bus = meta.bios_bus;
                gba->memory.dma_bus = meta.dma_bus;
                gba->memory.was_last_access_from_dma = meta.was_last_access_from_dma;
                gba->memory.gamepak_bus_in_use = meta.gamepak_bus_in_use;
                seen_memory_meta = true;
                break;
            };
            case QS_CHUNK_EWRAM: {
                if (quicksave_read_region(&buffer, chunk_end, gba->memory.ewram, sizeof(gba->memory.ewram))) {
                    goto error;
                }
                seen_ewram = true;
                break;
            };
            case QS_CHUNK_IWRAM: {
                if (quicksave_read_region(&buffer, chunk_end, gba->memory.iwram, sizeof(gba->memory.iwram))) {
                    goto error;
                }
                seen_iwram = true;
                break;
            };
            case QS_CHUNK_VRAM: {
                if (quicksave_read_region(&buffer, chunk_end, gba->memory.vram, sizeof(gba->memory.vram))) {
                    goto error;
                }
                seen_vram = true;
                break;
            };
            case QS_CHUNK_PALRAM: {
                if (quicksave_read_region(&buffer, chunk_end, gba->memory.palram, sizeof(gba->memory.palram))) {
                    goto error;
                }
                seen_palram = true;
                break;
            };
            case QS_CHUNK_OAM: {
                if (quicksave_read_region(&buffer, chunk_end, gba->memory.oam, sizeof(gba->memory.oam))) {
                    goto error;
                }
                seen_oam = true;
                break;
            };
            case QS_CHUNK_BACKUP_STORAGE: {
                struct quicksave_backup_snapshot meta;

                if (chunk.size < sizeof(meta) || quicksave_read(&buffer, (uint8_t *)&meta, sizeof(meta))) {
                    goto error;
                }

                if (meta.size) {
                    uint8_t *storage;

                    if (gba->shared_data.backup_storage.size != meta.size) {
                        free(gba->shared_data.backup_storage.data);
                        gba->shared_data.backup_storage.data = malloc(meta.size);
                        hs_assert(gba->shared_data.backup_storage.data);
                        gba->shared_data.backup_storage.size = meta.size;
                    }

                    storage = gba->shared_data.backup_storage.data;
                    if (quicksave_read_region(&buffer, chunk_end, storage, meta.size)) {
                        goto error;
                    }
                } else {
                    free(gba->shared_data.backup_storage.data);
                    gba->shared_data.backup_storage.data = NULL;
                    gba->shared_data.backup_storage.size = 0;
                    if (quicksave_read_region(&buffer, chunk_end, NULL, 0)) {
                        goto error;
                    }
                }

                atomic_store(&gba->shared_data.backup_storage.dirty, meta.dirty);
                seen_backup = true;
                break;
            };
            default: {
                buffer.index = chunk_end;
                break;
            };
        }

        buffer.index = chunk_end;
    }

    if (
           !seen_core
        || !seen_io
        || !seen_ppu
        || !seen_gpio
        || !seen_apu
        || !seen_sched
        || !seen_memory_meta
        || !seen_ewram
        || !seen_iwram
        || !seen_vram
        || !seen_palram
        || !seen_oam
    ) {
        goto error;
    }

    if (sched.events_len != events_tmp_len) {
        goto error;
    }

    gba->scheduler.cycles = sched.cycles;
    gba->scheduler.next_event = sched.next_event;
    gba->scheduler.events_size = sched.events_len;
    if (sched.events_len) {
        gba->scheduler.events = events_tmp;
        events_tmp = NULL;
    } else {
        gba->scheduler.events = NULL;
    }

    if (!seen_backup) {
        atomic_store(&gba->shared_data.backup_storage.dirty, false);
    }

    return (false);

error:
    free(events_tmp);
    return (true);
}

static bool
quickload_v1(
    struct gba *gba,
    uint8_t *data,
    size_t size
) {
    struct quicksave_buffer buffer;
    size_t i;
    struct rom_view rom_view;

    buffer.data = data;
    buffer.size = size;
    buffer.index = 0;

    free(gba->scheduler.events);
    gba->scheduler.events = NULL;
    gba->scheduler.events_size = 0;

    rom_view = gba->memory.rom;

    if (
           quicksave_read(&buffer, (uint8_t *)&gba->core, sizeof(gba->core))
        || quicksave_read(&buffer, (uint8_t *)&gba->memory, sizeof(gba->memory))
        || quicksave_read(&buffer, (uint8_t *)&gba->io, sizeof(gba->io))
        || quicksave_read(&buffer, (uint8_t *)&gba->ppu, sizeof(gba->ppu))
        || quicksave_read(&buffer, (uint8_t *)&gba->gpio, sizeof(gba->gpio))
        || quicksave_read(&buffer, (uint8_t *)&gba->apu, sizeof(gba->apu))
        || quicksave_read(&buffer, (uint8_t *)&gba->scheduler.cycles, sizeof(uint64_t))
        || quicksave_read(&buffer, (uint8_t *)&gba->scheduler.next_event, sizeof(uint64_t))
        || quicksave_read(&buffer, (uint8_t *)&gba->scheduler.events_size, sizeof(size_t))
    ) {
        return (true);
    }

    gba->memory.rom = rom_view;

    if (gba->scheduler.events_size) {
        gba->scheduler.events = calloc(gba->scheduler.events_size, sizeof(struct scheduler_event));
        hs_assert(gba->scheduler.events);
    }

    for (i = 0; i < gba->scheduler.events_size; ++i) {
        struct scheduler_event *event;

        event = &gba->scheduler.events[i];
        if (
               quicksave_read(&buffer, (uint8_t *)&event->kind, sizeof(enum sched_event_kind))
            || quicksave_read(&buffer, (uint8_t *)&event->active, sizeof(bool))
            || quicksave_read(&buffer, (uint8_t *)&event->repeat, sizeof(bool))
            || quicksave_read(&buffer, (uint8_t *)&event->at, sizeof(uint64_t))
            || quicksave_read(&buffer, (uint8_t *)&event->period, sizeof(uint64_t))
            || quicksave_read(&buffer, (uint8_t *)&event->args, sizeof(struct event_args))
        ) {
            return (true);
        }
    }

    return (false);
}
