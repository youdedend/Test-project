#pragma once
/* UWP-only Win32-FromApp adapters for the C runtime. */
#include <winsock2.h>
#include <windows.h>
#include <memoryapi.h>
#include <fileapifromapp.h>
#include <io.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <pthread_compat.h>
#include <pthread_time.h>

#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#ifndef ELOOP
#define ELOOP 114
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif

#ifdef __cplusplus
extern "C" {
#endif
FILE *bb_uwp_fopen(const char *path, const char *mode);
int bb_uwp_open(const char *path, int flags, ...);
int bb_uwp_stat(const char *path, struct _stat64 *out);
int bb_uwp_mkdir(const char *path);
HANDLE bb_uwp_mapping(HANDLE file, LPSECURITY_ATTRIBUTES security, ULONG access, ULONG protect,
                      ULONG attributes, ULONG64 bytes, PCWSTR name,
                      MEM_EXTENDED_PARAMETER *params, ULONG count);
DWORD bb_uwp_attributes(const char *path);
BOOL bb_uwp_delete(const char *path);
BOOL bb_uwp_remove_directory(const char *path);
HANDLE bb_uwp_find(const char *pattern, WIN32_FIND_DATAA *out);
BOOL bb_uwp_find_next(HANDLE handle, WIN32_FIND_DATAA *out);
#ifdef __cplusplus
}
#endif

#ifndef BB_UWP_ADAPTER_IMPLEMENTATION
#define fopen bb_uwp_fopen
#define open bb_uwp_open
#define _stat64(path, out) bb_uwp_stat(path, out)
#define _mkdir bb_uwp_mkdir
#define VirtualAlloc VirtualAllocFromApp
#define VirtualProtect VirtualProtectFromApp
#define VirtualAlloc2 VirtualAlloc2FromApp
#define MapViewOfFile3 MapViewOfFile3FromApp
#define CreateFileMapping2 bb_uwp_mapping
#define GetFileAttributesA bb_uwp_attributes
#define DeleteFileA bb_uwp_delete
#define RemoveDirectoryA bb_uwp_remove_directory
#define FindFirstFileA bb_uwp_find
#define FindNextFileA bb_uwp_find_next
#endif
