// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <time.h>
#if !defined(__MINGW32__) && !defined(_BB_TIMESPEC64_DEFINED)
#define _BB_TIMESPEC64_DEFINED
struct _timespec32 {
    long tv_sec;
    long tv_nsec;
};
struct _timespec64 {
    __time64_t tv_sec;
    long tv_nsec;
};
#endif
#include "bb_win_compat.h"
#include <io.h>
#ifndef ssize_t
typedef intptr_t ssize_t;
#endif
#define fseeko _fseeki64
#define ftello _ftelli64
#define fileno _fileno
