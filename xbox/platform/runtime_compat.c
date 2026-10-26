#define BB_UWP_ADAPTER_IMPLEMENTATION
#include "runtime_compat.h"
#include "dirent.h"
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

int bb_uwp_open(const char *path, int flags, ...) {
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
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d) return NULL;
    if (d->first) {
        d->first = 0;
    } else if (!FindNextFileW(d->handle, &d->data)) {
        return NULL;
    }
    if (!WideCharToMultiByte(CP_UTF8, 0, d->data.cFileName, -1, d->entry.d_name,
                             sizeof(d->entry.d_name), NULL, NULL)) {
        return NULL;
    }
    d->entry.d_type = (d->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
    return &d->entry;
}

int closedir(DIR *d) {
    if (!d) return -1;
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
