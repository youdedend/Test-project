#pragma clang system_header
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
#include_next <pthread.h>
