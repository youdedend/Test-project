#define BB_UWP_ADAPTER_IMPLEMENTATION
#include "runtime_compat.h"
#include "dirent.h"
#include "overlay_index.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int uwp_errno_from_win32(DWORD error) {
    switch (error) {
    case ERROR_SUCCESS: return 0;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_NO_MORE_FILES:
        return ENOENT;
    case ERROR_INVALID_NAME:
        return EINVAL;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_WRITE_PROTECT:
        return EACCES;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return EEXIST;
    case ERROR_INVALID_HANDLE:
        return EBADF;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
    case ERROR_COMMITMENT_LIMIT:
        return ENOMEM;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return ENOSPC;
    case ERROR_DIR_NOT_EMPTY:
        return ENOTEMPTY;
    case ERROR_DIRECTORY:
        return ENOTDIR;
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_BUFFER_OVERFLOW:
        return ENAMETOOLONG;
    case ERROR_TOO_MANY_OPEN_FILES:
        return EMFILE;
    default:
        return EIO;
    }
}

static void log_unexpected_win32(const char *op, const char *path, DWORD err, int mapped_errno) {
    if (mapped_errno == ENOENT || mapped_errno == EEXIST) return;
    static volatile LONG logged = 0;
    if (InterlockedIncrement(&logged) <= 24) {
        printf("UWP file warning: %s('%s') failed (Win32=%lu, errno=%d)\n",
               op, path ? path : "", (unsigned long)err, mapped_errno);
    }
}

/* Normalize UTF-8 paths to canonical Win32 backslashes for UWP RuntimeBroker *FromAppW APIs:
 * forward slashes '/' or duplicate/trailing slashes on FolderPicker USB paths are rejected
 * by RuntimeBroker with ERROR_INVALID_NAME / ERROR_ACCESS_DENIED. */
static int wide(const char *path, wchar_t *out, int capacity) {
    wchar_t raw[4096];
    if (!path || capacity <= 0 ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, raw, 4096)) {
        errno = EINVAL;
        return 0;
    }
    int w = 0;
    for (int r = 0; raw[r] != L'\0'; ++r) {
        wchar_t ch = (raw[r] == L'/') ? L'\\' : raw[r];
        if (ch == L'\\' && w > 0 && out[w - 1] == L'\\') {
            /* Keep leading UNC "\\server" double backslash only at index 1. */
            if (!(w == 1 && r == 1 && (raw[0] == L'\\' || raw[0] == L'/'))) {
                continue;
            }
        }
        if (w + 1 >= capacity) {
            errno = ENAMETOOLONG;
            return 0;
        }
        out[w++] = ch;
    }
    while (w > 1 && out[w - 1] == L'\\') {
        if (w == 3 && out[1] == L':') break; /* Preserve drive root "E:\" */
        --w;
    }
    out[w] = L'\0';
    return 1;
}

/* ---- update overlay: a separately dumped update folder laid over the base game dump -----------
 * The player may keep the 01.09 update as its own PS4 folder next to the game dump. Copying a
 * merged multi-GB tree onto the console is not acceptable, so the launcher writes the list of
 * relative paths the update provides (LocalState\Prepared\overlay\files.txt) and calls
 * bb_uwp_overlay_configure() once before the guest starts: from then on
 *   - every open/stat/attribute query of <dump>\<rel> that the update also has is resolved to
 *     <update>\<rel> (the update wins),
 *   - a directory listing of <dump>\<dir> also returns the update's files, so patch-only files
 *     become visible,
 *   - everything else keeps reading the dump, so base-only files stay where they are.
 * The dump and the update folder are only ever read. The path rules live in overlay_index.h. */
static BbOverlayIndex bb_overlay;

void bb_uwp_overlay_configure(const char *base, const char *patch, const char *index_path) {
    bb_overlay_index_free(&bb_overlay);
    if (!base || !*base || !patch || !*patch || !index_path || !*index_path) return;

    FILE *f = bb_uwp_fopen(index_path, "rb");
    if (!f) {
        printf("Update overlay: %s is unreadable, running the dump as-is\n", index_path);
        return;
    }
    char *list = NULL;
    size_t used = 0, cap = 0;
    for (int ch; (ch = fgetc(f)) != EOF;) {
        if (used + 1 >= cap) {
            size_t want = cap ? cap * 2 : 65536;
            char *grown = (char *)realloc(list, want);
            if (!grown) {
                free(list);
                fclose(f);
                printf("Update overlay: no memory for %s, running the dump as-is\n", index_path);
                return;
            }
            list = grown;
            cap = want;
        }
        list[used++] = (char)ch;
    }
    fclose(f);
    if (bb_overlay_parse(&bb_overlay, base, patch, list, used) != 0) {
        printf("Update overlay: %s has no usable paths, running the dump as-is\n", index_path);
        free(list);
        return;
    }
    free(list);
    printf("Update overlay: %zu paths from %s over %s\n", bb_overlay.count, bb_overlay.patch,
           bb_overlay.base);
}

static int bb_overlay_map_path(const char *path, char *out, size_t cap) {
    return bb_overlay_maps_path(&bb_overlay, path, out, cap);
}

static int bb_overlay_map_dir(const char *path, char *out, size_t cap) {
    return bb_overlay_maps_dir(&bb_overlay, path, out, cap);
}

static int bb_overlay_dir_has(const char *base_dir, const wchar_t *leaf) {
    wchar_t wide_dir[4096];
    wchar_t probe[4300];
    if (!wide(base_dir, wide_dir, 4096)) return 0;
    if (_snwprintf(probe, 4300, L"%s\\%s", wide_dir, leaf) <= 0) return 0;
    WIN32_FILE_ATTRIBUTE_DATA data;
    return GetFileAttributesExFromAppW(probe, GetFileExInfoStandard, &data) != 0;
}

int bb_uwp_open(const char *path, int flags, ...) {
    char overlay_path[1024];
    if (bb_overlay_map_path(path, overlay_path, sizeof(overlay_path))) path = overlay_path;
    wchar_t name[4096];
    if (!wide(path, name, 4096)) return -1;
    DWORD access = (flags & _O_RDWR) ? (GENERIC_READ | GENERIC_WRITE)
                 : (flags & _O_WRONLY) ? GENERIC_WRITE
                 : GENERIC_READ;
    DWORD creation = (flags & _O_CREAT)
                   ? ((flags & _O_EXCL) ? CREATE_NEW
                      : (flags & _O_TRUNC) ? CREATE_ALWAYS
                      : OPEN_ALWAYS)
                   : (flags & _O_TRUNC) ? TRUNCATE_EXISTING
                   : OPEN_EXISTING;
    CREATEFILE2_EXTENDED_PARAMETERS params = {sizeof(params), FILE_ATTRIBUTE_NORMAL, 0, 0, NULL, NULL};
    HANDLE handle = CreateFile2FromAppW(
        name, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, creation, &params);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        errno = uwp_errno_from_win32(err);
        log_unexpected_win32("CreateFile2FromAppW", path, err, errno);
        return -1;
    }
    int fd = _open_osfhandle((intptr_t)handle,
                             flags & (_O_APPEND | _O_BINARY | _O_TEXT | _O_RDONLY | _O_WRONLY | _O_RDWR));
    if (fd < 0) CloseHandle(handle);
    return fd;
}

FILE *bb_uwp_fopen(const char *path, const char *mode) {
    int flags = strchr(mode, '+') ? _O_RDWR : (*mode == 'r' ? _O_RDONLY : _O_WRONLY);
    if (*mode == 'w') flags |= _O_CREAT | _O_TRUNC;
    if (*mode == 'a') flags |= _O_CREAT | _O_APPEND;
    flags |= strchr(mode, 'b') ? _O_BINARY : _O_TEXT;
    int fd = bb_uwp_open(path, flags);
    if (fd < 0) return NULL;
    FILE *f = _fdopen(fd, mode);
    if (!f) _close(fd);
    return f;
}

static __time64_t unix_time(FILETIME t) {
    ULARGE_INTEGER u;
    u.LowPart = t.dwLowDateTime;
    u.HighPart = t.dwHighDateTime;
    return (__time64_t)(u.QuadPart / 10000000ULL - 11644473600ULL);
}

int bb_uwp_stat(const char *path, struct _stat64 *out) {
    char overlay_path[1024];
    if (bb_overlay_map_path(path, overlay_path, sizeof(overlay_path))) path = overlay_path;
    wchar_t name[4096];
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!wide(path, name, 4096)) return -1;
    if (!GetFileAttributesExFromAppW(name, GetFileExInfoStandard, &data)) {
        DWORD err = GetLastError();
        errno = uwp_errno_from_win32(err);
        log_unexpected_win32("GetFileAttributesExFromAppW", path, err, errno);
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->st_mode = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                 ? (_S_IFDIR | _S_IREAD)
                 : (_S_IFREG | _S_IREAD);
    out->st_nlink = 1;
    out->st_size = ((int64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    out->st_atime = unix_time(data.ftLastAccessTime);
    out->st_mtime = unix_time(data.ftLastWriteTime);
    out->st_ctime = unix_time(data.ftCreationTime);
    return 0;
}

int bb_uwp_mkdir(const char *path) {
    wchar_t name[4096];
    if (!wide(path, name, 4096)) return -1;
    if (CreateDirectoryFromAppW(name, NULL)) return 0;
    DWORD err = GetLastError();
    errno = uwp_errno_from_win32(err);
    log_unexpected_win32("CreateDirectoryFromAppW", path, err, errno);
    return -1;
}

HANDLE bb_uwp_mapping(HANDLE file, LPSECURITY_ATTRIBUTES security, ULONG access, ULONG protect,
                      ULONG attributes, ULONG64 bytes, PCWSTR name,
                      MEM_EXTENDED_PARAMETER *params, ULONG count) {
    (void)access;
    (void)params;
    (void)count;
    return CreateFileMappingFromApp(file, security, protect | attributes, bytes, name);
}

DWORD bb_uwp_attributes(const char *path) {
    char overlay_path[1024];
    if (bb_overlay_map_path(path, overlay_path, sizeof(overlay_path))) path = overlay_path;
    wchar_t name[4096];
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!wide(path, name, 4096) ||
        !GetFileAttributesExFromAppW(name, GetFileExInfoStandard, &data)) {
        return INVALID_FILE_ATTRIBUTES;
    }
    return data.dwFileAttributes;
}

BOOL bb_uwp_delete(const char *path) {
    wchar_t name[4096];
    return wide(path, name, 4096) && DeleteFileFromAppW(name);
}

BOOL bb_uwp_remove_directory(const char *path) {
    wchar_t name[4096];
    return wide(path, name, 4096) && RemoveDirectoryFromAppW(name);
}

static void find_data(WIN32_FIND_DATAA *out, const WIN32_FIND_DATAW *in) {
    memset(out, 0, sizeof(*out));
    out->dwFileAttributes = in->dwFileAttributes;
    out->nFileSizeHigh = in->nFileSizeHigh;
    out->nFileSizeLow = in->nFileSizeLow;
    WideCharToMultiByte(CP_UTF8, 0, in->cFileName, -1, out->cFileName, MAX_PATH, NULL, NULL);
}

HANDLE bb_uwp_find(const char *pattern, WIN32_FIND_DATAA *out) {
    char overlay_path[1024];
    if (pattern && !strpbrk(pattern, "*?") &&
        bb_overlay_map_path(pattern, overlay_path, sizeof(overlay_path))) {
        pattern = overlay_path;
    }
    wchar_t name[4096];
    WIN32_FIND_DATAW data;
    if (!wide(pattern, name, 4096)) return INVALID_HANDLE_VALUE;
    HANDLE h = FindFirstFileExFromAppW(name, FindExInfoBasic, &data, FindExSearchNameMatch, NULL, 0);
    if (h != INVALID_HANDLE_VALUE) find_data(out, &data);
    return h;
}

BOOL bb_uwp_find_next(HANDLE h, WIN32_FIND_DATAA *out) {
    WIN32_FIND_DATAW data;
    if (!FindNextFileW(h, &data)) return FALSE;
    find_data(out, &data);
    return TRUE;
}

int bb_uwp_is_dir(const char *path) {
    const DWORD attr = bb_uwp_attributes(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

int bb_uwp_is_file(const char *path) {
    const DWORD attr = bb_uwp_attributes(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

/* Directory enumeration for the platform layer (dump layout detection, overlay manifests).
 * The callback keeps windows.h out of prepare_game.cpp. */
void bb_uwp_list_dir(const char *dir, void (*visit)(const char *name, int is_dir, void *user),
                     void *user) {
    if (!dir || !*dir || !visit) return;
    wchar_t name[4096];
    if (!wide(dir, name, 4096)) return;
    size_t n = wcslen(name);
    if (n + 3 >= 4096) return;
    if (n > 0 && name[n - 1] == L'\\') wcscat_s(name, 4096, L"*");
    else wcscat_s(name, 4096, L"\\*");
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileExFromAppW(name, FindExInfoBasic, &data, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char utf8[1024];
        if (!WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, utf8, (int)sizeof(utf8), NULL,
                                 NULL)) {
            continue;
        }
        if (!strcmp(utf8, ".") || !strcmp(utf8, "..")) continue;
        visit(utf8, (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, user);
    } while (FindNextFileW(h, &data));
    FindClose(h);
}

DIR *opendir(const char *path) {
    wchar_t name[4096];
    if (!wide(path, name, 4096)) return NULL;
    size_t n = wcslen(name);
    if (n + 3 >= 4096) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    if (n > 0 && name[n - 1] == L'\\') wcscat_s(name, 4096, L"*");
    else wcscat_s(name, 4096, L"\\*");
    DIR *d = calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->handle = FindFirstFileExFromAppW(name, FindExInfoBasic, &d->data, FindExSearchNameMatch, NULL, 0);
    if (d->handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        free(d);
        errno = uwp_errno_from_win32(err);
        log_unexpected_win32("opendir", path, err, errno);
        return NULL;
    }
    d->first = 1;
    d->patch_handle = INVALID_HANDLE_VALUE;
    d->patch_first = 1;
    d->phase = 0;
    /* The update's own version of this directory is listed after the dump's, so files only the
     * update carries become visible to the guest. */
    char mapped[1024];
    if (bb_overlay_map_dir(path, mapped, sizeof(mapped))) {
        wchar_t pname[4096];
        if (wide(mapped, pname, 4096)) {
            const size_t pn = wcslen(pname);
            if (pn + 3 < 4096) {
                wcscat_s(pname, 4096, L"\\*");
                d->patch_handle = FindFirstFileExFromAppW(pname, FindExInfoBasic, &d->patch_data,
                                                          FindExSearchNameMatch, NULL, 0);
                if (d->patch_handle == INVALID_HANDLE_VALUE) {
                    d->patch_first = 1;
                } else {
                    snprintf(d->base_dir, sizeof(d->base_dir), "%s", path);
                }
            }
        }
    }
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d) return NULL;
    if (d->phase == 0) {
        if (d->first) {
            d->first = 0;
        } else if (!FindNextFileW(d->handle, &d->data)) {
            if (d->patch_handle == INVALID_HANDLE_VALUE) return NULL;
            d->phase = 1;
        }
    }
    if (d->phase == 1) {
        for (;;) {
            if (d->patch_first) {
                d->patch_first = 0;
            } else if (!FindNextFileW(d->patch_handle, &d->patch_data)) {
                d->phase = 2;
                return NULL;
            }
            if (bb_overlay_dir_has(d->base_dir, d->patch_data.cFileName)) {
                continue; /* the dump already listed this name (its content is redirected) */
            }
            d->data = d->patch_data;
            break;
        }
    }
    if (d->phase == 2) return NULL;
    if (!WideCharToMultiByte(CP_UTF8, 0, d->data.cFileName, -1, d->entry.d_name,
                             sizeof(d->entry.d_name), NULL, NULL)) {
        return NULL;
    }
    d->entry.d_type = (d->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
    return &d->entry;
}

int closedir(DIR *d) {
    if (!d) return -1;
    if (d->patch_handle != INVALID_HANDLE_VALUE) FindClose(d->patch_handle);
    FindClose(d->handle);
    free(d);
    return 0;
}

int gettimeofday(struct timeval *value, void *zone) {
    (void)zone;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    uint64_t us = u.QuadPart / 10 - 11644473600000000ULL;
    value->tv_sec = (long)(us / 1000000);
    value->tv_usec = (long)(us % 1000000);
    return 0;
}

int bbport_uwp_storage_get_roots(char *app0_out, size_t app0_cap,
                                 char *user_out, size_t user_cap,
                                 char *temp_out, size_t temp_cap) {
    const char *game = getenv("BB_GAME_DIR");
    const char *user = getenv("BB_USER_DIR");
    const char *temp = getenv("TEMP");
    if (app0_out && app0_cap) snprintf(app0_out, app0_cap, "%s", game ? game : "");
    if (user_out && user_cap) snprintf(user_out, user_cap, "%s", user ? user : "");
    if (temp_out && temp_cap) snprintf(temp_out, temp_cap, "%s", temp ? temp : "");
    return (game && *game) ? 0 : -1;
}
