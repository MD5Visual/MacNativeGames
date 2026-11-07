/* RomFS path validation shared by the runtime and game-data-free regressions. */
#pragma once
#include <stddef.h>
#include <string.h>

static inline int romfs_path_valid_relative(const char *path) {
    if (!path || !*path || *path == '/' || strchr(path, '\\')) return 0;
    for (const char *part = path; *part;) {
        const char *end = strchr(part, '/');
        size_t size = end ? (size_t)(end - part) : strlen(part);
        if (!size || (size == 1 && part[0] == '.') ||
            (size == 2 && part[0] == '.' && part[1] == '.')) return 0;
        if (!end) break;
        part = end + 1;
    }
    return 1;
}

static inline const char *romfs_path_relative(const char *mount, const char *path,
                                              char *scratch, size_t capacity) {
    if (!mount || !*mount || !path) return NULL;
    size_t size = strlen(mount);
    if (strncmp(path, mount, size) || path[size] != ':' || path[size + 1] != '/')
        return NULL;
    path += size + 2;
    /* Audio stream requests can join a mount root and a leading-slash resource. */
    while (*path == '/') ++path;
    if (romfs_path_valid_relative(path)) return path;
    if (!scratch || !strstr(path, "//")) return NULL;

    /* OoT also joins audio subdirectories with duplicate separators, e.g.
       Voice/Resource//Stream. Validate the collapsed path before lookup. */
    size_t used = 0;
    for (const char *p = path; *p; ++p) {
        if (used + 1 >= capacity) return NULL;
        scratch[used++] = *p;
        if (*p == '/') while (p[1] == '/') ++p;
    }
    scratch[used] = '\0';
    return romfs_path_valid_relative(scratch) ? scratch : NULL;
}
