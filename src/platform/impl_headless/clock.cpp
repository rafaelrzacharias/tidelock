// clock.cpp - the headless ClockApi (docs/PLATFORM.md §9.4: real, OS-direct - QueryPerformanceCounter
// / clock_gettime(MONOTONIC), identical contracts to sdl3's SDL_GetPerformanceCounter).
#include "platform/impl_headless/headless_apis.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

namespace {

u64 hc_ticks(void*) {
#ifdef _WIN32
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return (u64)li.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
#endif
}

u64 hc_frequency(void*) {
#ifdef _WIN32
    LARGE_INTEGER li;
    QueryPerformanceFrequency(&li);
    return (u64)li.QuadPart;
#else
    return 1000000000ull;   // clock_gettime is nanosecond-resolution by construction
#endif
}

u64 hc_wall_unix_ms(void*) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const u64 ticks_100ns = ((u64)ft.dwHighDateTime << 32) | (u64)ft.dwLowDateTime;
    const u64 EPOCH_DIFF_100NS = 116444736000000000ull;   // 1601-01-01 -> 1970-01-01
    return (ticks_100ns - EPOCH_DIFF_100NS) / 10000ull;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (u64)ts.tv_sec * 1000ull + (u64)(ts.tv_nsec / 1000000);
#endif
}

}  // namespace

ClockApi headless_clock_api(HeadlessState* s) {
    return ClockApi{ s, hc_ticks, hc_frequency, hc_wall_unix_ms };
}
