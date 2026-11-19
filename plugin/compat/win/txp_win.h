/* SPDX-License-Identifier: GPL-3.0-only */
/* Windows: force-included (clang-cl /FI) into every C file of the plugin's Windows build (plugin/CMakeLists.txt), so
 * the shared sources build unchanged against the MSVC CRT. */
#ifndef TEXTURA_PLUGIN_TXP_WIN_H
#define TEXTURA_PLUGIN_TXP_WIN_H
#define _USE_MATH_DEFINES 1              /* M_PI */
#define _CRT_SECURE_NO_WARNINGS 1        /* (snprintf, strcpy: the CRT's "use the _s forms" notes) */
#define NOMINMAX 1
#define WIN32_LEAN_AND_MEAN 1
#include <malloc.h>
#include <stdlib.h>
/* C11 aligned_alloc: the MSVC CRT has none (its free() cannot release an aligned block; the plugin's only
 * aligned_alloc, the coroutine stack in slot_bridge.h, is never freed and unused on Windows: Fibers have their own) */
#define aligned_alloc(al, n) _aligned_malloc((n), (al))
/* <sys/stat.h>'s POSIX test (the test hosts) */
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
/* POSIX time: nanosleep (slot_bridge.h's wait for a state capture, the test host's waits) over Sleep, and
 * clock_gettime(CLOCK_MONOTONIC) (the test host's CPU timing) over the CRT's precise clock */
#include <time.h>
__declspec(dllimport) void __stdcall Sleep(unsigned long ms);   /* (windows.h's own declaration, without it) */
static __inline int nanosleep(const struct timespec *t, struct timespec *rem)
{
    unsigned long ms = (unsigned long)(t->tv_sec * 1000 + (t->tv_nsec + 999999) / 1000000);
    (void)rem;
    Sleep(ms);
    return 0;
}
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
static __inline int clock_gettime(int clk, struct timespec *ts) { (void)clk; return timespec_get(ts, TIME_UTC) ? 0 : -1; }
#endif
#endif
