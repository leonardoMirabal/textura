/* SPDX-License-Identifier: GPL-3.0-only */
/* txbench: CPU per 256-frame block on this computer, per engine and scene, and an FM-1 estimate calibrated against
 * Frodhes, whose worst block was measured on the FM-1.
 *
 *   txbench [--runs N] [--calib DIR] [--json OUT]       (make tx-bench runs the calibration first: make tx-calib)
 *
 * Method. Every scene is rendered N times (default 9) from a fresh instance after one untimed warm-up; each block is
 * timed alone (the key events of the block included). Per block the fastest of the N runs is kept (that removes
 * interrupts and other processes), then "mean" is the average over the blocks and "worst" the slowest block.
 * Calibration: Frodhes' host build (its own sources, compiled read-only into build/calib/ with a timing hook, see
 * the Makefile) plays its FM-1 worst-case scene (9 bass notes at v127 + pedal, every effect at its heaviest), timed
 * the same way (app_audio per block, fastest of N runs). That scene measured 3620 us per block on the FM-1, so
 *   FM-1 estimate = host us x (3620 / Frodhes' host worst us).
 * Assumption: Textura's integer code runs at the same Mac-to-FM-1 speed ratio as Frodhes' Rhodes + effects code.
 * Both are 32-bit integer DSP, but Frodhes leans on 32x32->64 products and Textura on 32-bit ones, the clock and the
 * compiler (JieLi clang -Os vs host -O2) differ: read it as +-30 % until the engines are measured on the device.
 * Without the calibration files only host times are printed. */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tx_host.h"

#define FM1_WORST_US 3620.0        /* Frodhes' worst block on the FM-1 (tools/hil_cpu.py, 9 voices, every effect) */
#define FR_SKIP_BLOCKS 20u         /* Frodhes' boot and effect set-up blocks, before its notes (110 ms) */
#define MAXB 4096u

static unsigned runs = 9;

typedef struct { double mean_us, worst_us, run_us; uint32_t blocks; } meas_t;   /* run_us: fastest whole run / blocks */

/* per-block minimum over runs -> mean and worst */
static meas_t reduce(uint64_t *const *t, unsigned nruns, uint32_t nb, uint32_t skip)
{
    meas_t m = {0, 0, 0, 0};
    uint32_t b;
    double sum = 0;
    for (b = skip; b < nb; b++) {
        uint64_t lo = t[0][b];
        unsigned r;
        for (r = 1; r < nruns; r++) if (t[r][b] < lo) lo = t[r][b];
        sum += lo / 1000.0;
        if (lo / 1000.0 > m.worst_us) m.worst_us = lo / 1000.0;
    }
    m.blocks = nb > skip ? nb - skip : 0;
    m.mean_us = m.blocks ? sum / m.blocks : 0;
    return m;
}

/* one scene; particles_override > 0 replaces every PARTICLES event with it */
static int bench_scene(const tx_engine_t *e, const char *scene, int particles_override, meas_t *out)
{
    tx_script_t *s = malloc(sizeof *s);
    void *st = tx_state_new(e);
    uint64_t *t[64];
    int32_t mix[TX_BLOCK];
    unsigned r, i;
    if (tx_scene_load(s, e, scene)) { free(s); free(st); return -1; }
    if (particles_override > 0) {
        for (i = 0; i < s->n; i++)
            if (s->ev[i].kind == TX_SEV_PARAM && s->ev[i].id == TX_PID_PARTICLES) s->ev[i].a = (uint8_t)particles_override;
    }
    if (s->blocks > MAXB) s->blocks = MAXB;
    double best = 1e30;
    for (r = 0; r <= runs; r++) {                            /* run 0: the warm-up, not kept */
        unsigned next = 0;
        uint32_t b;
        uint64_t tr = tx_now_ns();
        uint64_t *row = r ? (t[r - 1] = malloc(sizeof(uint64_t) * s->blocks)) : 0;
        e->init(st, s->seed);
        if (particles_override > 0) e->param(st, TX_PID_PARTICLES, (uint8_t)particles_override);
        for (b = 0; b < s->blocks; b++) {
            uint64_t t0 = tx_now_ns();
            tx_script_step(e, st, s, b, &next, mix);
            if (row) row[b] = tx_now_ns() - t0;
        }
        tr = tx_now_ns() - tr;
        if (r && tr / 1000.0 / s->blocks < best) best = tr / 1000.0 / s->blocks;
    }
    *out = reduce(t, runs, s->blocks, 0);
    out->run_us = best;
    for (r = 0; r < runs; r++) free(t[r]);
    free(s); free(st);
    return 0;
}

/* the calibration: build/calib/run/w*.txt, one file per run of Frodhes' worst scene, ns per block */
static int load_calib(const char *dir, meas_t *m, unsigned *nfiles)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    uint64_t *t[64];
    unsigned n = 0;
    uint32_t nb = 0xFFFFFFFFu;
    if (!d) return -1;
    while ((de = readdir(d)) && n < 64) {
        char path[600];
        FILE *f;
        uint32_t k = 0;
        unsigned long v;
        if (de->d_name[0] != 'w' || !strstr(de->d_name, ".txt")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (!(f = fopen(path, "r"))) continue;
        t[n] = malloc(sizeof(uint64_t) * 40000);
        while (k < 40000 && fscanf(f, "%lu", &v) == 1) t[n][k++] = v;
        fclose(f);
        if (k < nb) nb = k;
        n++;
    }
    closedir(d);
    *nfiles = n;
    if (n < 3 || nb <= FR_SKIP_BLOCKS) { while (n) free(t[--n]); return -1; }
    *m = reduce(t, n, nb, FR_SKIP_BLOCKS);
    while (n) free(t[--n]);
    return 0;
}

int main(int argc, char **argv)
{
    const char *cdir = "build/calib/run", *json = "build/bench/calib.json";
    meas_t cal, ref = {0, 0, 0, 0};
    unsigned ncal = 0, k;
    double factor = 0;
    int i;
    FILE *jf;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--runs") && i + 1 < argc) runs = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--calib") && i + 1 < argc) cdir = argv[++i];
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) json = argv[++i];
    }
    tx_bench_prepare();
    if (runs < 1) runs = 1;
    if (runs > 63) runs = 63;
    printf("txbench: %u runs per scene, the fastest run per block; 256-frame blocks (budget %u us on the FM-1)\n", runs, TX_BLOCK_US);
    if (!load_calib(cdir, &cal, &ncal)) {
        factor = FM1_WORST_US / cal.worst_us;
        printf("calibration: Frodhes' worst scene (9 bass v127 + pedal, every effect) on this host: worst %.1f us, mean %.1f us"
               " (%u runs, %u blocks)\n             FM-1 measured 3620 us -> factor %.1f (FM-1 us per host us)\n",
               cal.worst_us, cal.mean_us, ncal, cal.blocks, factor);
    } else
        printf("calibration: none (%s has no Frodhes runs: make tx-calib). Host times only.\n", cdir);
    printf("\n%-8s %-14s %9s %9s %11s %8s\n", "engine", "scene", "mean us", "worst us", "FM-1 worst", "% block");
    for (k = 0; tx_engines[k]; k++) {
        const tx_engine_t *e = tx_engines[k];
        static const char *const sc[] = {"idle", "tap", "hold", "chord", "max", "max@def", 0};
        unsigned j;
        for (j = 0; sc[j]; j++) {
            meas_t m;
            char label[32];
            int def = !strcmp(sc[j], "max@def");
            if (bench_scene(e, def ? "max" : sc[j], def ? e->particles_def : 0, &m)) continue;
            if (def) snprintf(label, sizeof label, "max@P%u", e->particles_def);
            else if (!strcmp(sc[j], "max")) snprintf(label, sizeof label, "max@P%u", e->particles_max);
            else snprintf(label, sizeof label, "%s", sc[j]);
            if (factor > 0)
                printf("%-8s %-14s %9.2f %9.2f %8.0f us %7.1f%%%s\n", e->name, label, m.mean_us, m.worst_us,
                       m.worst_us * factor, 100.0 * m.worst_us * factor / TX_BLOCK_US,
                       def && m.worst_us * factor > 0.18 * TX_BLOCK_US ? "  OVER the 18 % layer budget" : "");
            else
                printf("%-8s %-14s %9.2f %9.2f %11s %8s\n", e->name, label, m.mean_us, m.worst_us, "-", "-");
            if (k == 0 && def) ref = m;                      /* the player's reference: engine 0, max@default */
        }
        printf("%-8s state %u B\n", e->name, e->state_bytes);
    }
    {   /* CHAMBER (the post-stage): a busy input (white noise at -12 dBFS), 690 blocks, defaults and the heaviest */
        static const char *const lab[2] = {"defaults", "max"};
        unsigned c2;
        for (c2 = 0; c2 < 2; c2++) {
            tx_chamber_t *ch = malloc(sizeof *ch);
            int32_t buf[TX_BLOCK];
            uint64_t *t[64];
            unsigned r;
            uint32_t b, rng = 12345u;
            meas_t m;
            for (r = 0; r <= runs; r++) {
                uint64_t *row = r ? (t[r - 1] = malloc(sizeof(uint64_t) * 690u)) : 0;
                tx_chamber_init(ch);
                if (c2) { tx_chamber_param(ch, TX_CH_SIZE, 127); tx_chamber_param(ch, TX_CH_ABSORB, 0);
                          tx_chamber_param(ch, TX_CH_BODY, 127); tx_chamber_param(ch, TX_CH_TAIL, 127); }
                for (b = 0; b < 690u; b++) {
                    unsigned j;
                    uint64_t t0;
                    for (j = 0; j < TX_BLOCK; j++) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; buf[j] = (int32_t)(rng >> 10) - (1 << 21); }
                    t0 = tx_now_ns();
                    tx_chamber_process(ch, buf, TX_BLOCK);
                    if (row) row[b] = tx_now_ns() - t0;
                }
            }
            m = reduce(t, runs, 690u, 0);
            for (r = 0; r < runs; r++) free(t[r]);
            free(ch);
            if (factor > 0)
                printf("%-8s %-14s %9.2f %9.2f %8.0f us %7.1f%%\n", "chamber", lab[c2], m.mean_us, m.worst_us,
                       m.worst_us * factor, 100.0 * m.worst_us * factor / TX_BLOCK_US);
            else
                printf("%-8s %-14s %9.2f %9.2f %11s %8s\n", "chamber", lab[c2], m.mean_us, m.worst_us, "-", "-");
        }
        printf("%-8s state %u B (one per output, outside the engine arenas)\n", "chamber", (unsigned)sizeof(tx_chamber_t));
    }
    if ((jf = fopen(json, "w"))) {                           /* for the browser player */
        fprintf(jf, "{\n  \"fm1_worst_us\": 3620,\n  \"frodhes_host_worst_us\": %.3f,\n  \"factor\": %.4f,\n"
                    "  \"ref_engine\": \"%s\",\n  \"ref_scene\": \"max\",\n  \"ref_particles\": %u,\n"
                    "  \"ref_native_run_us\": %.4f,\n  \"ref_native_mean_us\": %.4f,\n  \"ref_native_worst_us\": %.4f,\n"
                    "  \"method\": \"FM-1 us = host us x factor; factor = 3620 us (Frodhes' worst block measured on the FM-1) / the same scene's worst block in Frodhes' host build on this computer\"\n}\n",
                factor > 0 ? cal.worst_us : 0.0, factor, tx_engines[0]->name, tx_engines[0]->particles_def,
                ref.run_us, ref.mean_us, ref.worst_us);
        fclose(jf);
        printf("\nwrote %s\n", json);
    }
    return 0;
}
