/* SPDX-License-Identifier: GPL-3.0-only */
/* The two cores' code on the host (make fw-c1map): the firmware (tests/host.c) built with lanes.c LN_PAR, its second
 * core's jobs run inline (c1prof_run: at once, before this core's own share, which is the same work: lanes.c's split is
 * a sum of int32 terms), and -finstrument-functions-after-inlining on every function but these hooks. Each function's
 * calls and self time (its own, less its callees') are counted in three places:
 *   0  core 0 outside a job (the rest of the interrupt, the main loop)
 *   1  core 1: inside a job (c1prof_run .. its return)
 *   2  core 0 while a job runs: after a job is handed over until the wait (what core 0 runs at the same time)
 * At the end: C1PROF_OUT (a text file) gets "addr place calls self_ns" lines and the address of main, for
 * tools/c1map.py (which names them with nm and sets them against the FM-1 image's RAM / XIP placement). */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define C1P_N 8192u
#define C1P_DEPTH 512u
typedef struct { void *fn; uint64_t calls[3], ns[3]; } c1p_ent_t;
static c1p_ent_t c1p_tab[C1P_N];
static struct { void *fn; uint64_t t0, child; int place; } c1p_stk[C1P_DEPTH];
static unsigned c1p_sp;
static int c1p_core1, c1p_window, c1p_init;
static uint64_t c1p_jobs, c1p_job_ns;

__attribute__((no_instrument_function)) static uint64_t c1p_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

__attribute__((no_instrument_function)) static c1p_ent_t *c1p_get(void *fn)
{
    uint32_t h = (uint32_t)(((uintptr_t)fn >> 2) * 2654435761u) & (C1P_N - 1u);
    while (c1p_tab[h].fn && c1p_tab[h].fn != fn) h = (h + 1u) & (C1P_N - 1u);
    c1p_tab[h].fn = fn;
    return &c1p_tab[h];
}

int main(int, char **);
__attribute__((no_instrument_function)) static void c1p_dump(void)
{
    const char *p = getenv("C1PROF_OUT");
    FILE *f;
    uint32_t i;
    if (!p || !(f = fopen(p, "w"))) return;
    fprintf(f, "main %lx\njobs %llu %llu\n", (unsigned long)(uintptr_t)&main, (unsigned long long)c1p_jobs,
            (unsigned long long)c1p_job_ns);
    for (i = 0; i < C1P_N; i++) {
        int k;
        if (!c1p_tab[i].fn) continue;
        for (k = 0; k < 3; k++)
            if (c1p_tab[i].calls[k])
                fprintf(f, "%lx %d %llu %llu\n", (unsigned long)(uintptr_t)c1p_tab[i].fn, k,
                        (unsigned long long)c1p_tab[i].calls[k], (unsigned long long)c1p_tab[i].ns[k]);
    }
    fclose(f);
}

__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void *fn, void *site)
{
    (void)site;
    if (!c1p_init) { c1p_init = 1; atexit(c1p_dump); }
    if (c1p_sp >= C1P_DEPTH) { c1p_sp++; return; }
    c1p_stk[c1p_sp].fn = fn;
    c1p_stk[c1p_sp].child = 0;
    c1p_stk[c1p_sp].place = c1p_core1 ? 1 : c1p_window ? 2 : 0;
    c1p_stk[c1p_sp].t0 = c1p_now();
    c1p_sp++;
}

__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void *fn, void *site)
{
    uint64_t t = c1p_now(), d;
    c1p_ent_t *e;
    (void)fn; (void)site;
    if (!c1p_sp) return;
    if (--c1p_sp >= C1P_DEPTH) return;
    d = t - c1p_stk[c1p_sp].t0;
    e = c1p_get(c1p_stk[c1p_sp].fn);
    e->calls[c1p_stk[c1p_sp].place]++;
    e->ns[c1p_stk[c1p_sp].place] += d > c1p_stk[c1p_sp].child ? d - c1p_stk[c1p_sp].child : 0u;
    if (c1p_sp) c1p_stk[c1p_sp - 1u].child += d;
}

/* lanes.c's second core, inline */
__attribute__((no_instrument_function)) int c1prof_run(void (*fn)(uint32_t), uint32_t a)
{
    uint64_t t0 = c1p_now();
    c1p_core1 = 1;
    fn(a);
    c1p_core1 = 0;
    c1p_jobs++;
    c1p_job_ns += c1p_now() - t0;
    c1p_window = 1;
    return 1;
}
__attribute__((no_instrument_function)) int c1prof_wait(void) { c1p_window = 0; return 0; }
