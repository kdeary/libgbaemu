/******************************************************************************\
**
**  Compatibility utilities for the Spark Mini Console emulator.
**
**  This header replaces the dependency on the original `hades.h` from the
**  upstream Hades GBA project by providing a minimal set of macros and
**  helpers that the extracted sources rely on.
**
\******************************************************************************/

#pragma once

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>

#if !defined(HS_ENABLE_LOGGING) && !defined(HS_DISABLE_LOGGING)
#define HS_DISABLE_LOGGING 1
#endif

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#endif

#ifndef __used
#define __used __attribute__((used))
#endif
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#ifndef HOT
#define HOT __attribute__((hot))
#endif
#ifndef FLATTEN
#define FLATTEN __attribute__((flatten))
#endif
#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#endif
#ifndef unlikely
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif
#ifndef LIKELY
#define LIKELY(x) likely(x)
#endif
#ifndef UNLIKELY
#define UNLIKELY(x) unlikely(x)
#endif
#ifndef __noreturn
#define __noreturn __attribute__((noreturn))
#endif
#ifndef __unreachable
#define __unreachable __builtin_unreachable()
#endif

#undef static_assert
#define static_assert(e)                                    \
    _Static_assert(                                         \
        (e),                                                \
        "(" #e ") evaluated to false (in " __FILE__ ")"     \
    )

#ifndef min
#define min(a, b)                               \
    ({                                          \
        __typeof__((a) + 0) _a = (a);           \
        __typeof__((b) + 0) _b = (b);           \
        _a < _b ? _a : _b;                      \
    })
#endif

#ifndef max
#define max(a, b)                               \
    ({                                          \
        __typeof__((a) + 0) _a = (a);           \
        __typeof__((b) + 0) _b = (b);           \
        _a > _b ? _a : _b;                      \
    })
#endif

#define array_length(array) (sizeof(array) / sizeof(*(array)))

#define XSTR(...)                #__VA_ARGS__
#define STR(...)                 XSTR(__VA_ARGS__)
#define XCONCAT(a, b)            a ## b
#define CONCAT(a, b)             XCONCAT(a, b)
#define NTH(_0, _1, _2, _3, _4, _5, N, ...) N
#define NARG(...)                NTH(, ##__VA_ARGS__, 5, 4, 3, 2, 1, 0)

static inline uint64_t
hs_mask_for_width(
    unsigned width
) {
    if (!width) {
        return (uint64_t)0;
    }

    if (width >= 64) {
        return UINT64_MAX;
    }

    return (UINT64_C(1) << width) - 1u;
}

#define bitfield_get(val, nth)                                                      \
    (__typeof__(val))(((uint64_t)(val) >> (nth)) & UINT64_C(1))

#define bitfield_get_range(val, start, end)                                         \
    (__typeof__(val))((((uint64_t)(val)) >> (start)) & hs_mask_for_width((end) - (start)))

#define align_on(x, y) ((x) & ~((y) - 1))
#define align(T, x) ((__typeof__(x))(align_on((x), sizeof(T))))

enum hs_module {
    HS_INFO = 0,

    HS_ERROR,
    HS_WARNING,

    HS_CORE,
    HS_IO,
    HS_VIDEO,
    HS_DMA,
    HS_IRQ,
    HS_MEMORY,
    HS_TIMER,

    HS_DEBUG,

    HS_END,
};

static inline char const *
hs_module_label(
    enum hs_module module
) {
    static char const *const labels[] = {
        [HS_INFO] = " INFO  ",
        [HS_ERROR] = " ERROR ",
        [HS_WARNING] = " WARN  ",
        [HS_CORE] = " CORE  ",
        [HS_IO] = " IO    ",
        [HS_VIDEO] = " VIDEO ",
        [HS_DMA] = " DMA   ",
        [HS_IRQ] = " IRQ   ",
        [HS_MEMORY] = " MEM   ",
        [HS_TIMER] = " TIMER ",
        [HS_DEBUG] = " DEBUG ",
    };

    if (module < 0 || module >= HS_END || labels[module] == NULL) {
        return " ????? ";
    }

    return labels[module];
}

static inline bool
hs_logging_enabled(
    void
) {
#ifdef HS_DISABLE_LOGGING
    return false;
#else
    return true;
#endif
}

static inline void
hs_vlog(
    enum hs_module module,
    char const *prefix,
    char const *fmt,
    va_list args
) {
    fputs("[", stderr);
    fputs(hs_module_label(module), stderr);
    fputs("] ", stderr);
    if (prefix && *prefix) {
        fputs(prefix, stderr);
    }
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    fflush(stderr);
}

static inline void
logln(
    enum hs_module module,
    char const *fmt,
    ...
) __attribute__((format(printf, 2, 3)));

static inline void
logln(
    enum hs_module module,
    char const *fmt,
    ...
) {
    if (!hs_logging_enabled()) {
        (void)module;
        (void)fmt;
        return;
    }

    va_list args;

    va_start(args, fmt);
    hs_vlog(module, "", fmt, args);
    va_end(args);
}

static inline void
hs_vpanic(
    enum hs_module module,
    char const *prefix,
    char const *fmt,
    va_list args
) __noreturn;

static inline void
hs_vpanic(
    enum hs_module module,
    char const *prefix,
    char const *fmt,
    va_list args
) {
    hs_vlog(module, prefix, fmt, args);
    abort();
#if defined(__GNUC__)
    __unreachable;
#endif
}

static inline void
panic(
    enum hs_module module,
    char const *fmt,
    ...
) __attribute__((format(printf, 2, 3))) __noreturn;

static inline void
panic(
    enum hs_module module,
    char const *fmt,
    ...
) {
    va_list args;

    va_start(args, fmt);
    hs_vpanic(module, "PANIC: ", fmt, args);
    va_end(args);
}

static inline void
unimplemented(
    enum hs_module module,
    char const *fmt,
    ...
) __attribute__((format(printf, 2, 3))) __noreturn;

static inline void
unimplemented(
    enum hs_module module,
    char const *fmt,
    ...
) {
    va_list args;

    va_start(args, fmt);
    hs_vpanic(module, "UNIMPLEMENTED: ", fmt, args);
    va_end(args);
}

#define hs_assert(expr)                                     \
    do {                                                    \
        if (unlikely(!(expr))) {                            \
            panic(                                          \
                HS_ERROR,                                   \
                "assert(%s) failed (in %s at line %u).",    \
                #expr,                                      \
                __FILE__,                                   \
                __LINE__                                    \
            );                                              \
        }                                                   \
    } while (0)

static inline char *
hs_format_impl(
    char const *fmt,
    ...
) __attribute__((format(printf, 1, 2)));

static inline char *
hs_format_impl(
    char const *fmt,
    ...
) {
    va_list args;
    va_list copy;
    int needed;
    char *out;

    va_start(args, fmt);
    va_copy(copy, args);
    needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    hs_assert(needed >= 0);

    out = malloc((size_t)needed + 1u);
    hs_assert(out);

    vsnprintf(out, (size_t)needed + 1u, fmt, args);
    va_end(args);

    return out;
}

#define hs_format(fmt, ...) hs_format_impl((fmt), ##__VA_ARGS__)

static inline uint64_t
hs_time(
    void
) {
#ifdef _WIN32
    static LARGE_INTEGER freq = {0};
    LARGE_INTEGER counter;

    if (!freq.QuadPart) {
        QueryPerformanceFrequency(&freq);
    }

    QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000000ULL) / freq.QuadPart);
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
#endif
}

static inline void
hs_usleep(
    uint64_t usec
) {
#ifdef _WIN32
    Sleep((DWORD)(usec / 1000ULL));
#else
    struct timespec ts;

    ts.tv_sec = (time_t)(usec / 1000000ULL);
    ts.tv_nsec = (long)((usec % 1000000ULL) * 1000ULL);

    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
        continue;
    }
#endif
}

static inline void
disable_colors(
    void
) {
    /* No-op placeholder for compatibility with the original API. */
}
