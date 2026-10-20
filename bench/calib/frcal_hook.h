/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura calibration hook, force-included (-include) into a build/ copy of Frodhes' host build: times each audio
 * block (app_audio = eng_render + fx_process, what the FM-1's profiler measures) and writes the per-block ns to the
 * file named by FRCAL_OUT at exit. Frodhes' sources are only copied and compiled, never changed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif

#define FRCAL_MAX 40000u
static uint32_t frcal_ns[FRCAL_MAX], frcal_n;

static uint64_t frcal_now(void)
{
#ifdef __APPLE__
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
#endif
}

static void frcal_dump(void)
{
    const char *p = getenv("FRCAL_OUT");
    FILE *f = p ? fopen(p, "w") : 0;
    uint32_t i;
    if (!f) return;
    for (i = 0; i < frcal_n; i++) fprintf(f, "%u\n", frcal_ns[i]);
    fclose(f);
}

__attribute__((constructor)) static void frcal_init(void)     /* as txbench: a performance core, the clock warmed up */
{
    uint64_t t0 = frcal_now();
    volatile uint32_t x = 1;
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    while (frcal_now() - t0 < 300000000u) x = x * 1664525u + 1013904223u;
    atexit(frcal_dump);
}

/* host.c's call site `app_audio(blk, HALF_FRAMES);` is rewritten to FRCAL_AUDIO(blk, HALF_FRAMES); */
#define FRCAL_AUDIO(b, n) do { uint64_t t0_ = frcal_now(); app_audio(b, n); \
                               if (frcal_n < FRCAL_MAX) frcal_ns[frcal_n++] = (uint32_t)(frcal_now() - t0_); } while (0)
