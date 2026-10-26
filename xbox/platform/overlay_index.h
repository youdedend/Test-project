// SPDX-License-Identifier: GPL-2.0-or-later
// Index of the files a separately dumped update container provides over a base game dump.
//
// The platform file layer (xbox/platform/runtime_compat.c) lays the update over the dump without
// copying either folder: the launcher writes one relative path per line into
// LocalState\Prepared\overlay\files.txt, this header turns that list into a sorted, lower-cased
// index, and every file the runtime opens under the dump is redirected to the update folder when
// the update carries the same relative path. The path rules (prefix match, case folding, both
// separators, "E:\" roots) are the part that is easy to get wrong, so they live here as pure
// C functions with no Win32 dependency and are covered by xbox/tests/overlay_index_test.cpp.
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BB_OVERLAY_MAX_FILES 32768
#define BB_OVERLAY_MAX_REL 512
#define BB_OVERLAY_MAX_ROOT 1024

typedef struct {
    char base[BB_OVERLAY_MAX_ROOT];  /* dump root, no trailing separator, '\\' separated */
    char patch[BB_OVERLAY_MAX_ROOT]; /* update root, same normalization */
    char *files;                     /* count * BB_OVERLAY_MAX_REL, lower-case, sorted */
    size_t count;
} BbOverlayIndex;

static void bb_overlay_index_free(BbOverlayIndex *index) {
    if (!index) return;
    free(index->files);
    index->files = NULL;
    index->count = 0;
    index->base[0] = '\0';
    index->patch[0] = '\0';
}

static void bb_overlay_fold(char *text) {
    for (; *text; ++text) {
        if (*text >= 'A' && *text <= 'Z') *text = (char)(*text - 'A' + 'a');
    }
}

/* Trailing separators removed and '/' turned into '\', so a prefix compare against guest paths
 * (which use either separator) is enough. */
static void bb_overlay_trim(char *path) {
    size_t n;
    if (!path) return;
    n = strlen(path);
    while (n > 3 && (path[n - 1] == '\\' || path[n - 1] == '/')) path[--n] = '\0';
    for (size_t i = 0; i < n; ++i) {
        if (path[i] == '/') path[i] = '\\';
    }
}

static int bb_overlay_cmp(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* Case-insensitive, separator-insensitive ASCII compare of the first `n` bytes: the guest's host
 * paths mix "dump/guest/rel" (runtime_file.c concatenates the mount with '/' ) and Windows stores
 * "dump\rel", and both have to match. */
static int bb_overlay_prefix_eq(const char *path, const char *root, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        char a = path[i], b = root[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a == '/') a = '\\';
        if (b == '/') b = '\\';
        if (a != b) return 0;
    }
    return 1;
}

/* Parses `size` bytes of LF/CRLF separated relative paths. Returns 0 on success; on failure the
 * index stays empty and the caller runs the dump without an overlay. */
static int bb_overlay_parse(BbOverlayIndex *index, const char *base, const char *patch,
                            const char *list, size_t size) {
    size_t lines = 0, pos = 0, entry = 0;
    if (!index) return -1;
    bb_overlay_index_free(index);
    if (!base || !*base || !patch || !*patch || !list || !size) return -1;

    for (size_t i = 0; i < size; ++i) {
        if (list[i] == '\n') ++lines;
    }
    if (size && list[size - 1] != '\n') ++lines;
    if (!lines || lines > BB_OVERLAY_MAX_FILES) return -1;

    index->files = (char *)calloc(lines, BB_OVERLAY_MAX_REL);
    if (!index->files) return -1;

    while (pos < size && entry < lines) {
        size_t end = pos;
        size_t len;
        while (end < size && list[end] != '\n' && list[end] != '\r') ++end;
        len = end - pos;
        if (len > 0 && len < BB_OVERLAY_MAX_REL) {
            char *slot = index->files + entry * BB_OVERLAY_MAX_REL;
            for (size_t i = 0; i < len; ++i) {
                slot[i] = (list[pos + i] == '/') ? '\\' : list[pos + i];
            }
            slot[len] = '\0';
            bb_overlay_fold(slot);
            ++entry;
        }
        pos = end;
        while (pos < size && (list[pos] == '\n' || list[pos] == '\r')) ++pos;
    }
    index->count = entry;
    if (!index->count) {
        bb_overlay_index_free(index);
        return -1;
    }
    qsort(index->files, index->count, BB_OVERLAY_MAX_REL, bb_overlay_cmp);
    snprintf(index->base, sizeof(index->base), "%s", base);
    snprintf(index->patch, sizeof(index->patch), "%s", patch);
    bb_overlay_trim(index->base);
    bb_overlay_trim(index->patch);
    return 0;
}

/* Relative path of `path` inside the dump root, lower-cased and '\' separated. NULL when `path`
 * is not under the root (including the root itself). */
static const char *bb_overlay_rel(const BbOverlayIndex *index, const char *path, char *rel,
                                  size_t cap) {
    const char *rest;
    size_t root_len, i = 0;
    if (!index || !index->count || !path || !rel || cap < 2) return NULL;
    root_len = strlen(index->base);
    if (strlen(path) <= root_len + 1) return NULL;
    if (!bb_overlay_prefix_eq(path, index->base, root_len)) return NULL;
    rest = path + root_len;
    if (*rest != '\\' && *rest != '/') return NULL;
    ++rest;
    for (; *rest && i + 1 < cap; ++rest) {
        rel[i++] = (*rest == '/') ? '\\' : *rest;
    }
    if (*rest) return NULL; /* too long to be one of ours */
    rel[i] = '\0';
    bb_overlay_fold(rel);
    return rel;
}

static int bb_overlay_listed(const BbOverlayIndex *index, const char *rel) {
    if (!index || !index->count || !rel) return 0;
    return bsearch(rel, index->files, index->count, BB_OVERLAY_MAX_REL, bb_overlay_cmp) != NULL;
}

/* <dump>\<rel> that the update also provides -> <update>\<rel>. */
static int bb_overlay_maps_path(const BbOverlayIndex *index, const char *path, char *out,
                                size_t cap) {
    char rel[BB_OVERLAY_MAX_REL];
    if (!bb_overlay_rel(index, path, rel, sizeof(rel)) || !bb_overlay_listed(index, rel)) return 0;
    if ((size_t)snprintf(out, cap, "%s\\%s", index->patch, rel) >= cap) return 0;
    return 1;
}

/* <dump>\<dir> -> <update>\<dir>. Directories have no entry in the file list, so this only checks
 * that the path is under the dump root; the caller lets the directory open fail if the update has
 * no such directory. */
static int bb_overlay_maps_dir(const BbOverlayIndex *index, const char *path, char *out,
                               size_t cap) {
    char rel[BB_OVERLAY_MAX_REL];
    if (!bb_overlay_rel(index, path, rel, sizeof(rel))) return 0;
    if ((size_t)snprintf(out, cap, "%s\\%s", index->patch, rel) >= cap) return 0;
    return 1;
}
