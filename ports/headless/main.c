/******************************************************************************\
**
**  Minimal headless front-end for libgbaemu.
**
**  Loads a ROM (and optional BIOS), runs the emulator on a background thread,
**  and prints frame statistics to stdout using a single line that is refreshed
**  in place.
**
\******************************************************************************/

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "gba/gba.h"
#include "gba/channel.h"
#include "gba/event.h"
#include "gba/memory.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

struct file_buffer {
    uint8_t *data;
    size_t size;
};

struct headless_port {
    struct gba *gba;
    pthread_t thread;
    bool thread_started;
};

static atomic_bool keep_running = true;

static void
signal_handler(
    int signo __unused
) {
    keep_running = false;
}

static void *
emulator_thread(
    void *arg
) {
    struct gba *gba = arg;
    gba_run(gba);
    return NULL;
}

static bool
read_entire_file(
    char const *path,
    struct file_buffer *out
) {
    FILE *fp;
    long size;

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "Failed to open %s: %s\n", path, strerror(errno));
        return false;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "Failed to seek %s\n", path);
        fclose(fp);
        return false;
    }

    size = ftell(fp);
    if (size < 0) {
        fprintf(stderr, "Failed to ftell %s\n", path);
        fclose(fp);
        return false;
    }
    rewind(fp);

    out->data = malloc((size_t)size);
    if (!out->data) {
        fprintf(stderr, "Out of memory allocating %ld bytes for %s\n", size, path);
        fclose(fp);
        return false;
    }
    out->size = (size_t)size;

    if (fread(out->data, 1, out->size, fp) != out->size) {
        fprintf(stderr, "Failed to read %s\n", path);
        free(out->data);
        out->data = NULL;
        out->size = 0;
        fclose(fp);
        return false;
    }

    fclose(fp);
    return true;
}

static void
free_file_buffer(
    struct file_buffer *buf
) {
    free(buf->data);
    buf->data = NULL;
    buf->size = 0;
}

static struct gba_settings
default_settings(
    void
) {
    struct gba_settings settings;
    size_t i;

    memset(&settings, 0, sizeof(settings));
    settings.fast_forward = false;
    settings.speed = 1.0f;
    settings.prefetch_buffer = true;
    settings.enable_frame_skipping = false;
    settings.frame_skip_counter = 0;

    for (i = 0; i < ARRAY_SIZE(settings.ppu.enable_bg_layers); ++i) {
        settings.ppu.enable_bg_layers[i] = true;
    }
    settings.ppu.enable_oam = true;

    for (i = 0; i < ARRAY_SIZE(settings.apu.enable_psg_channels); ++i) {
        settings.apu.enable_psg_channels[i] = true;
    }
    for (i = 0; i < ARRAY_SIZE(settings.apu.enable_fifo_channels); ++i) {
        settings.apu.enable_fifo_channels[i] = true;
    }

    return settings;
}

static void
push_message(
    struct gba *gba,
    struct message const *msg
) {
    channel_lock(&gba->channels.messages);
    channel_push(&gba->channels.messages, &msg->header);
    channel_release(&gba->channels.messages);
}

static bool
launch_emulator(
    struct headless_port *port,
    struct launch_config const *config
) {
    struct message_reset msg_reset;
    struct message msg_run;
    int err;

    memset(&msg_reset, 0, sizeof(msg_reset));
    msg_reset.header.kind = MESSAGE_RESET;
    msg_reset.header.size = sizeof(msg_reset);
    msg_reset.config = *config;

    memset(&msg_run, 0, sizeof(msg_run));
    msg_run.header.kind = MESSAGE_RUN;
    msg_run.header.size = sizeof(msg_run);

    push_message(port->gba, (struct message const *)&msg_reset);
    push_message(port->gba, (struct message const *)&msg_run);

    err = pthread_create(&port->thread, NULL, emulator_thread, port->gba);
    if (err) {
        fprintf(stderr, "Failed to start emulator thread: %s\n", strerror(err));
        return false;
    }

    port->thread_started = true;
    return true;
}

static void
shutdown_emulator(
    struct headless_port *port
) {
    if (!port->gba) {
        return;
    }

    struct message msg_exit;

    memset(&msg_exit, 0, sizeof(msg_exit));
    msg_exit.header.kind = MESSAGE_EXIT;
    msg_exit.header.size = sizeof(msg_exit);

    push_message(port->gba, (struct message const *)&msg_exit);

    if (port->thread_started) {
        pthread_join(port->thread, NULL);
        port->thread_started = false;
    }

    gba_delete(port->gba);
    port->gba = NULL;
}

static double
monotonic_seconds(
    void
) {
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static void
print_usage(
    char const *prog
) {
    fprintf(stderr, "Usage: %s <rom> [--bios <bios>] [--skip-bios]\n", prog);
}

int
main(
    int argc,
    char **argv
) {
    struct file_buffer rom = { 0 };
    struct file_buffer bios = { 0 };
    struct headless_port port = { 0 };
    struct launch_config config;
    struct gba_settings settings;
    struct game_entry *entry;
    char const *rom_path = NULL;
    char const *bios_path = NULL;
    bool skip_bios = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--bios") == 0) {
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return EXIT_FAILURE;
            }
            bios_path = argv[++i];
        } else if (strcmp(argv[i], "--skip-bios") == 0) {
            skip_bios = true;
        } else if (!rom_path) {
            rom_path = argv[i];
        } else {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (!rom_path) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (!read_entire_file(rom_path, &rom)) {
        return EXIT_FAILURE;
    }

    if (bios_path) {
        if (!read_entire_file(bios_path, &bios)) {
            free_file_buffer(&rom);
            return EXIT_FAILURE;
        }
    } else {
        bios.data = calloc(1, BIOS_SIZE);
        bios.size = BIOS_SIZE;
        if (!bios.data) {
            fprintf(stderr, "Out of memory allocating BIOS buffer.\n");
            free_file_buffer(&rom);
            return EXIT_FAILURE;
        }
        skip_bios = true;
    }

    port.gba = gba_create();
    if (!port.gba) {
        fprintf(stderr, "Failed to create GBA instance.\n");
        free_file_buffer(&rom);
        free_file_buffer(&bios);
        return EXIT_FAILURE;
    }

    settings = default_settings();

    memset(&config, 0, sizeof(config));
    config.rom.data = rom.data;
    config.rom.size = rom.size;
    config.rom.fd = -1;
    config.rom.fd_offset = 0;
    config.bios.data = bios.data;
    config.bios.size = bios.size;
    config.skip_bios = skip_bios;
    config.audio_frequency = 0;
    config.settings = settings;

    entry = db_autodetect_game_features(rom.data, rom.size);
    if (entry) {
        config.backup_storage.type = entry->storage;
        config.gpio_device_type = entry->gpio;
    } else {
        config.backup_storage.type = BACKUP_NONE;
        config.gpio_device_type = GPIO_NONE;
    }

    if (!launch_emulator(&port, &config)) {
        shutdown_emulator(&port);
        free_file_buffer(&rom);
        free_file_buffer(&bios);
        return EXIT_FAILURE;
    }

    signal(SIGINT, signal_handler);
    double window_start = monotonic_seconds();
    uint64_t total_frames = 0;
    double fps = 0.0;
    uint32_t frame_window = 0;

    while (atomic_load(&keep_running) && (total_frames < 1500)) {
        uint32_t frames = gba_shared_reset_frame_counter(port.gba);
        if (frames) {
            total_frames += frames;
            frame_window += frames;
            double now = monotonic_seconds();
            double dt = now - window_start;
            if (dt >= 0.25) {
                fps = frame_window / dt;
                frame_window = 0;
                window_start = now;
            }
            size_t ewram = port.gba->memory.ewram.used_pages * MEM_PAGE_SIZE;
            size_t iwram = port.gba->memory.iwram.used_pages * MEM_PAGE_SIZE;
            size_t vram = port.gba->memory.vram.used_pages * MEM_PAGE_SIZE;
            printf("\rFrames: %-12llu | FPS: %-8.2f | RAM usage (KiB): E=%-5zu I=%-5zu V=%-5zu",
                (unsigned long long)total_frames,
                fps,
                ewram / 1024,
                iwram / 1024,
                vram / 1024);
            fflush(stdout);
        }
        usleep(5 * 1000);
    }

    printf("\nStopping...\n");

    shutdown_emulator(&port);
    free_file_buffer(&rom);
    free_file_buffer(&bios);
    return EXIT_SUCCESS;
}
