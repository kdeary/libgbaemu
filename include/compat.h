/******************************************************************************\
**
**  Minimal compatibility helpers for building the core emulator without the
**  original build system.
**
\******************************************************************************/

#pragma once

#include "hs.h"

#ifdef _WIN32
#error "Windows compatibility layer not implemented for this build."
#else

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HS_PATH_SEPARATOR       '/'

static inline bool
hs_isatty(
    int fd
) {
    return isatty(fd);
}

static inline FILE *
hs_fopen(
    char const *path,
    char const *mode
) {
    return fopen(path, mode);
}

static inline bool
hs_mkdir(
    char const *path
) {
    if (mkdir(path, 0755) == 0) {
        return true;
    }

    return errno == EEXIST;
}

static inline bool
hs_fexists(
    char const *path
) {
    return access(path, F_OK) == 0;
}

static inline char *
hs_abspath(
    char const *path
) {
    return realpath(path, NULL);
}

static inline char const *
hs_basename(
    char const *path
) {
    char const *base;

    base = strrchr(path, HS_PATH_SEPARATOR);
    return base ? base + 1 : path;
}

static inline char *
hs_fmtime(
    char *path
) {
    struct stat stbuf;
    struct tm *tm;
    char *out;

    if (stat(path, &stbuf)) {
        return NULL;
    }

    out = malloc(sizeof(char) * 128);
    hs_assert(out);

    tm = localtime(&stbuf.st_mtime);
    if (!tm) {
        free(out);
        return NULL;
    }

    strftime(out, 128, "%c", tm);
    return out;
}

static inline void
hs_open_url(
    char const *url
) {
    int rc __unused;
    char command[256];

    snprintf(
        command,
        sizeof(command),
        "%s \"%s\"",
#if __APPLE__
        "open",
#else
        "xdg-open",
#endif
        url
    );

    rc = system(command);
}

#endif /* !_WIN32 */
