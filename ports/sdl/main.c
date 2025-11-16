/******************************************************************************\
**
**  Simple SDL front-end for the libgbaemu core.
**
**  Loads a ROM (and optional BIOS), runs the emulator on a background thread,
**  and blits the streamed scanline output to an SDL window. No input or audio.
**
\******************************************************************************/


#define HS_DISABLE_LOGGING 0

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <SDL2/SDL.h>

#include "gba/gba.h"
#include "gba/channel.h"
#include "gba/event.h"
#include "gba/memory.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

struct file_buffer {
    uint8_t *data;
    size_t size;
};

struct video_sink_ctx {
    uint16_t *raw;
    pthread_mutex_t lock;
    atomic_uint version;
};

struct sdl_port {
    struct gba *gba;
    pthread_t thread;
    bool thread_started;
    struct video_sink_ctx video;
    bool video_initialized;
};

static void
sdl_scanline_callback(
    struct gba *gba __unused,
    void *userdata,
    uint32_t y,
    uint16_t const *pixels,
    size_t count
) {
    struct video_sink_ctx *ctx = userdata;

    if (y >= GBA_SCREEN_HEIGHT) {
        return;
    }

    const size_t safe_count = count < (size_t)GBA_SCREEN_WIDTH ? count : (size_t)GBA_SCREEN_WIDTH;

    pthread_mutex_lock(&ctx->lock);
    memcpy(ctx->raw + y * GBA_SCREEN_WIDTH, pixels, safe_count * sizeof(uint16_t));
    atomic_fetch_add_explicit(&ctx->version, 1u, memory_order_release);
    pthread_mutex_unlock(&ctx->lock);
}

static void
teardown_video_sink(
    struct sdl_port *port
) {
    if (!port->video_initialized) {
        return;
    }
    pthread_mutex_destroy(&port->video.lock);
    free(port->video.raw);
    port->video.raw = NULL;
    port->video_initialized = false;
}

struct key_binding {
    SDL_Keycode keycode;
    enum keys key;
};

static struct key_binding const key_map[] = {
    { SDLK_z, KEY_A },
    { SDLK_x, KEY_B },
    { SDLK_a, KEY_L },
    { SDLK_s, KEY_R },
    { SDLK_UP, KEY_UP },
    { SDLK_DOWN, KEY_DOWN },
    { SDLK_LEFT, KEY_LEFT },
    { SDLK_RIGHT, KEY_RIGHT },
    { SDLK_RETURN, KEY_START },
    { SDLK_BACKSPACE, KEY_SELECT },
};

static bool
translate_keycode(
    SDL_Keycode keycode,
    enum keys *out_key
) {
    for (size_t i = 0; i < ARRAY_SIZE(key_map); ++i) {
        if (key_map[i].keycode == keycode) {
            *out_key = key_map[i].key;
            return true;
        }
    }
    return false;
}

static inline uint32_t
color555_to_argb(
    uint16_t color
) {
    uint32_t r = color & 0x1F;
    uint32_t g = (color >> 5) & 0x1F;
    uint32_t b = (color >> 10) & 0x1F;

    r = (r << 3) | (r >> 2);
    g = (g << 3) | (g >> 2);
    b = (b << 3) | (b >> 2);

    return 0xFF000000u | (r << 0) | (g << 8) | (b << 16);
}

static void
push_message(
    struct gba *gba,
    struct event_header const *event
);

static void
send_key_message(
    struct sdl_port *port,
    enum keys key,
    bool pressed
) {
    struct message_key msg;

    memset(&msg, 0, sizeof(msg));
    msg.header.kind = MESSAGE_KEY;
    msg.header.size = sizeof(msg);
    msg.key = key;
    msg.pressed = pressed;
    push_message(port->gba, &msg.header);
}

static void *
emulator_thread(
    void *userdata
) {
    struct gba *gba;

    gba = userdata;
    gba_run(gba);
    return NULL;
}

static void
push_message(
    struct gba *gba,
    struct event_header const *event
) {
    channel_lock(&gba->channels.messages);
    channel_push(&gba->channels.messages, event);
    channel_release(&gba->channels.messages);
}

static bool
read_entire_file(
    char const *path,
    struct file_buffer *out
) {
    FILE *file;
    uint8_t *data;
    off_t length;
    size_t size;
    size_t read_sz;

    file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Failed to open '%s': %s\n", path, strerror(errno));
        return false;
    }

    if (fseeko(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "Failed to seek '%s': %s\n", path, strerror(errno));
        fclose(file);
        return false;
    }

    length = ftello(file);
    if (length < 0) {
        fprintf(stderr, "Failed to determine size of '%s': %s\n", path, strerror(errno));
        fclose(file);
        return false;
    }
    size = (size_t)length;
    if (fseeko(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "Failed to rewind '%s': %s\n", path, strerror(errno));
        fclose(file);
        return false;
    }

    data = malloc(size ? size : 1);
    if (!data) {
        fprintf(stderr, "Out of memory while loading '%s'.\n", path);
        fclose(file);
        return false;
    }

    read_sz = fread(data, 1, size, file);
    fclose(file);

    if (read_sz != size) {
        fprintf(stderr, "Failed to read '%s'.\n", path);
        free(data);
        return false;
    }

    out->data = data;
    out->size = size;
    return true;
}

static void
free_file_buffer(
    struct file_buffer *buffer
) {
    free(buffer->data);
    buffer->data = NULL;
    buffer->size = 0;
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

static bool
launch_emulator(
    struct sdl_port *port,
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

    push_message(port->gba, &msg_reset.header);
    push_message(port->gba, &msg_run.header);

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
    struct sdl_port *port
) {
    if (!port->gba) {
        return;
    }

    struct message msg_exit;

    memset(&msg_exit, 0, sizeof(msg_exit));
    msg_exit.header.kind = MESSAGE_EXIT;
    msg_exit.header.size = sizeof(msg_exit);

    gba_set_video_sink(port->gba, NULL);
    push_message(port->gba, &msg_exit.header);

    if (port->thread_started) {
        pthread_join(port->thread, NULL);
        port->thread_started = false;
    }

    gba_delete(port->gba);
    port->gba = NULL;
    teardown_video_sink(port);
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
    struct file_buffer rom;
    struct file_buffer bios;
    struct sdl_port port;
    struct launch_config config;
    struct gba_settings settings;
    struct game_entry *entry;
    uint32_t *framebuffer_copy;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    bool skip_bios;
    char const *rom_path;
    char const *bios_path;
    int window_scale;
    bool running;
    size_t frame_size;

    rom.data = NULL;
    rom.size = 0;
    bios.data = NULL;
    bios.size = 0;
    memset(&port, 0, sizeof(port));
    skip_bios = false;
    rom_path = NULL;
    bios_path = NULL;
    window = NULL;
    renderer = NULL;
    texture = NULL;
    framebuffer_copy = NULL;

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
    if (pthread_mutex_init(&port.video.lock, NULL) != 0) {
        fprintf(stderr, "Failed to initialize video sink mutex.\n");
        gba_delete(port.gba);
        free_file_buffer(&rom);
        free_file_buffer(&bios);
        return EXIT_FAILURE;
    }
    port.video.raw = calloc(GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT, sizeof(uint16_t));
    if (!port.video.raw) {
        fprintf(stderr, "Out of memory allocating video sink buffer.\n");
        pthread_mutex_destroy(&port.video.lock);
        gba_delete(port.gba);
        free_file_buffer(&rom);
        free_file_buffer(&bios);
        return EXIT_FAILURE;
    }
    atomic_init(&port.video.version, 0);
    port.video_initialized = true;
    struct gba_video_sink video_sink = {
        .scanline = sdl_scanline_callback,
        .userdata = &port.video,
    };
    gba_set_video_sink(port.gba, &video_sink);

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

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        shutdown_emulator(&port);
        free_file_buffer(&rom);
        free_file_buffer(&bios);
        return EXIT_FAILURE;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

    window_scale = 1;
    window = SDL_CreateWindow(
        "libgbaemu - SDL Port",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        GBA_SCREEN_WIDTH * window_scale,
        GBA_SCREEN_HEIGHT * window_scale,
        SDL_WINDOW_RESIZABLE
    );
    if (!window) {
        fprintf(stderr, "Failed to create window: %s\n", SDL_GetError());
        SDL_Quit();
        shutdown_emulator(&port);
        free_file_buffer(&rom);
        free_file_buffer(&bios);
        return EXIT_FAILURE;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        fprintf(stderr, "Failed to create renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        shutdown_emulator(&port);
        free_file_buffer(&rom);
        free_file_buffer(&bios);
        return EXIT_FAILURE;
    }

    SDL_RenderSetLogicalSize(renderer, GBA_SCREEN_WIDTH, GBA_SCREEN_HEIGHT);

    texture = SDL_CreateTexture(
        renderer,
        SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STREAMING,
        GBA_SCREEN_WIDTH,
        GBA_SCREEN_HEIGHT
    );
    if (!texture) {
        fprintf(stderr, "Failed to create texture: %s\n", SDL_GetError());
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        shutdown_emulator(&port);
        free_file_buffer(&rom);
        free_file_buffer(&bios);
        return EXIT_FAILURE;
    }

    frame_size = GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT * sizeof(uint32_t);
    framebuffer_copy = malloc(frame_size);
    if (!framebuffer_copy) {
        fprintf(stderr, "Out of memory allocating framebuffer copy.\n");
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        shutdown_emulator(&port);
        free_file_buffer(&rom);
        free_file_buffer(&bios);
        return EXIT_FAILURE;
    }

    running = true;
    while (running) {
        SDL_Event event;

        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT: {
                    running = false;
                    break;
                }
                case SDL_KEYDOWN: {
                    SDL_Keycode keycode;
                    enum keys key;

                    keycode = event.key.keysym.sym;
                    if (keycode == SDLK_ESCAPE) {
                        running = false;
                        break;
                    }
                    if (event.key.repeat) {
                        break;
                    }

                    if (translate_keycode(keycode, &key)) {
                        send_key_message(&port, key, true);
                    }
                    break;
                }
                case SDL_KEYUP: {
                    enum keys key;

                    if (translate_keycode(event.key.keysym.sym, &key)) {
                        send_key_message(&port, key, false);
                    }
                    break;
                }
                default:
                    break;
            }
        }

        if (gba_shared_reset_frame_counter(port.gba) > 0) {
            size_t i;
            uint32_t version_before;
            uint32_t version_after;

            do {
                version_before = atomic_load_explicit(&port.video.version, memory_order_acquire);
                pthread_mutex_lock(&port.video.lock);
                for (i = 0; i < GBA_SCREEN_WIDTH * GBA_SCREEN_HEIGHT; ++i) {
                    framebuffer_copy[i] = color555_to_argb(port.video.raw[i]);
                }
                pthread_mutex_unlock(&port.video.lock);
                version_after = atomic_load_explicit(&port.video.version, memory_order_acquire);
                if (version_before != version_after) {
                    SDL_Delay(0);
                }
            } while (version_before != version_after);

            SDL_UpdateTexture(texture, NULL, framebuffer_copy, GBA_SCREEN_WIDTH * (int)sizeof(uint32_t));
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, texture, NULL, NULL);
            SDL_RenderPresent(renderer);
        } else {
            SDL_Delay(1);
        }
    }

    free(framebuffer_copy);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    shutdown_emulator(&port);
    free_file_buffer(&rom);
    free_file_buffer(&bios);
    return EXIT_SUCCESS;
}
