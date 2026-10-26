#pragma once
#include <windows.h>
struct dirent {
    char d_name[1024];
    unsigned char d_type;
};
typedef struct {
    HANDLE handle;
    WIN32_FIND_DATAW data;
    int first;
    struct dirent entry;
    /* Update overlay (runtime_compat.c): a second listing of the same directory inside the update
     * folder, merged into readdir() so files the update adds are visible. */
    HANDLE patch_handle;
    WIN32_FIND_DATAW patch_data;
    int patch_first;
    int phase;
    char base_dir[1024];
} DIR;
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10
#ifdef __cplusplus
extern "C" {
#endif
DIR *opendir(const char *path);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);
#ifdef __cplusplus
}
#endif
