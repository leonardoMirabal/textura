/* SPDX-License-Identifier: GPL-3.0-only */
/* txbudget: Textura's CPU budget for the whole audio interrupt on the FM-1, from evidence (make tx-budget).
 *
 * A report: it measures, prints and writes build/bench/budget.txt / budget.csv; it never fails a build and changes no
 * engine or governor constant.
 *
 * The interrupt's time, per 256-frame block (5805 us):
 *   platform   what Frodhes' platform does in the same interrupt (its FM-1 measurements, see PLAT_* below): the entry
 *              and the output passes, the USB tap (+ the 48 kHz resampler when a computer records at 48 kHz), the
 *              timer tick that preempts the render ~58 times a block (with the key-velocity scan)
 *   output     Textura's mix (pan + sum per lane), CHAMBER, limiter: measured here
 *   effects    a reserve for the planned effects (the granular delay): an estimate until they exist
 *   sources    the lanes: the pool the governor shares (tx_core_gov.h)
 * The gate (Frodhes' rule, measured as CC 116's whole-interrupt maximum): <= 4500 us with the USB stream open at
 * 48 kHz, 300 us of margin (plan to <= 4200 us).
 *
 * Host times are scaled to the FM-1 with the calibration (Frodhes' worst scene, make tx-calib): +-30 %. So the pool
 * is given twice: "planned" divides Textura's measured costs by UNCERT (1.3: what fits even if the factor is 30 %
 * low), "device" is the same sum without it (what the governor gets when it measures the real time on the FM-1).
 * Code from flash (cache misses) is not modelled at all: the 300 us margin is kept for it.
 *
 * Each SOURCE preset's cost curve: worst block, the worst 8-block mean (what the governor sees, ~46 ms) and the
 * mean, over ACTIVITY 0..127, PARTICLES 2 / 6 (default) / 16, PROXIMITY, and every timbre knob at its extremes;
 * the fastest of RUNS runs per block (host noise out). */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tx_host.h"
#include "tx_core_mix.h"
#include "tx_core_chamber.h"
#include "tx_fx.h"

#define FM1_WORST_US 3620.0        /* as txbench: Frodhes' worst block on the FM-1 at the pinned revision */
#define FR_SKIP_BLOCKS 20u
#define SECS 8u                    /* each configuration's render */
#define NBLK ((SECS * TX_FS) / TX_BLOCK)
#define MAXR 16u

/* ---- the platform, from Frodhes (FM-1 measurements, 0.9.5 integration fe0b436, tools/cpu_costs.json) ---- */
#define BLOCK_US 5805.0
#define GATE_US 4500.0             /* AGENTS.md 5 (the user, 2026-10-24); tools/cpu_check.sh bff7959 */
#define MARGIN_US 300.0
#define PLAT_FIXED_US 230.0        /* [M-] idle, no effects, 312-316 us = the entry + the output passes + the Rhodes'
                                    * empty block (~85 us): ~230 without the engine (cpu_costs.json app.idle_no_fx) */
#define PLAT_TAIL48_US 376.0       /* [M] the USB tap + the 48 kHz FIR, 368-376 us (tail_us.48000; 44.1 kHz: 88) */
#define PLAT_TICK_US 178.0         /* [E] the timer tick with the velocity scan: 699 instructions x 58 x 4.4 ns
                                    * (AGENTS.md 5: 263 -> 699 in 0.9.5); without velocity ~67 */
#define FX_RESERVE_US 500.0        /* [E] the granular delay, not designed: Frodhes' delay ~155 us + ~8 grains */
#define UNCERT 1.3                 /* the host -> FM-1 factor's uncertainty (+-30 %) */

static unsigned runs = 3, passes = 3;
static double factor;
/* each configuration's per-block minimum over every run of every pass: the passes go over all configurations in turn,
 * so a busy moment of the host (other jobs: seconds long) cannot inflate one configuration's every run */
#define MAXCFG 1024u
static float *bestb[MAXCFG];
static unsigned cidx, pass;

typedef struct { double worst, worst8, mean; } cost_t;   /* FM-1 us */

static double tx_max(double a, double b) { return a > b ? a : b; }

/* one script on one engine: per-block minimum over runs, then the worst, worst 8-block mean and mean (FM-1 us) */
static int measure(const tx_engine_t *e, const char *text, cost_t *out)
{
    tx_script_t *s = malloc(sizeof *s);
    void *st = tx_state_new(e);
    static uint64_t t[MAXR][NBLK + 8];
    static double lo[NBLK + 8];
    int32_t mix[TX_BLOCK];
    unsigned r, b, nb;
    double sum = 0, w = 0, w8 = 0;
    if (!s || !st || tx_script_parse(s, e, text)) {
        fprintf(stderr, "txbudget: %s: %s\n", e->name, s ? s->err : "no memory");
        free(s); free(st); return -1;
    }
    nb = s->blocks < NBLK ? s->blocks : NBLK;
    for (r = 0; r <= runs; r++) {                       /* run 0: the warm-up */
        unsigned next = 0;
        e->init(st, s->seed);
        for (b = 0; b < nb; b++) {
            uint64_t t0 = tx_now_ns();
            tx_script_step(e, st, s, b, &next, mix);
            if (r) t[r - 1][b] = tx_now_ns() - t0;
        }
    }
    if (cidx >= MAXCFG) { free(s); free(st); return -1; }
    if (!bestb[cidx] && !(bestb[cidx] = malloc(sizeof(float) * (NBLK + 8)))) { free(s); free(st); return -1; }
    for (b = 0; b < nb; b++) {
        uint64_t m = t[0][b];
        float v;
        for (r = 1; r < runs; r++) if (t[r][b] < m) m = t[r][b];
        v = (float)(m / 1000.0 * factor);
        if (!pass || v < bestb[cidx][b]) bestb[cidx][b] = v;
        lo[b] = bestb[cidx][b];
        sum += lo[b];
        w = tx_max(w, lo[b]);
        if (b >= 7) {
            double a = 0;
            unsigned j;
            for (j = b - 7; j <= b; j++) a += lo[j];
            w8 = tx_max(w8, a / 8);
        }
    }
    out->worst = w; out->worst8 = w8; out->mean = nb ? sum / nb : 0;
    cidx++;
    free(s); free(st);
    return 0;
}

/* the calibration factor (FM-1 us per host us), as txbench computes it from build/calib/run */
static int calib(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    double worst = 0;
    unsigned n = 0;
    static uint64_t v[64][40000];
    static uint32_t len[64];
    uint32_t nb = 0xFFFFFFFFu, b;
    if (!d) return -1;
    while ((de = readdir(d)) && n < 64) {
        char path[600];
        FILE *f;
        unsigned long x;
        uint32_t k = 0;
        if (de->d_name[0] != 'w' || !strstr(de->d_name, ".txt")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (!(f = fopen(path, "r"))) continue;
        while (k < 40000 && fscanf(f, "%lu", &x) == 1) v[n][k++] = x;
        fclose(f);
        len[n] = k;
        if (k < nb) nb = k;
        n++;
    }
    closedir(d);
    if (n < 3 || nb <= FR_SKIP_BLOCKS) return -1;
    for (b = FR_SKIP_BLOCKS; b < nb; b++) {
        uint64_t m = v[0][b];
        unsigned r;
        for (r = 1; r < n; r++) if (v[r][b] < m) m = v[r][b];
        if (m / 1000.0 > worst) worst = m / 1000.0;
    }
    (void)len;
    factor = FM1_WORST_US / worst;
    return 0;
}

/* ---- the SOURCE presets' curves ---- */
#define NI 5
static const unsigned INT_V[NI] = {0, 32, 64, 96, 127};
typedef struct {
    char name[24];
    cost_t def;                    /* ACTIVITY 64, PARTICLES 6, PROXIMITY 64: the preset as it comes */
    cost_t lo;                     /* ACTIVITY 16 (drips, embers, one caller) */
    cost_t in[NI];                 /* ACTIVITY sweep, PARTICLES 6, PROXIMITY 127 (the closest: the most heroes heard) */
    cost_t p2, p16;                /* ACTIVITY 127, PROXIMITY 127, PARTICLES 2 / 16 */
    cost_t knob;                   /* the worst of the timbre knobs' extremes (ACTIVITY 127, PARTICLES 6) */
    char knob_by[24];
    double worst6, worst8_6;       /* the worst over every PARTICLES 6 configuration */
    char worst_by[24];             /* the configuration of that worst block */
} preset_cost_t;

static const char *const KNOBS[][2] = {
    {"EXTRA", "127"}, {"EXTRA", "0"}, {"SPACING", "0"}, {"SPACING", "127"}, {"SIZE", "0"}, {"SIZE", "127"},
    {"SPREAD", "127"}, {"TONE", "127"}, {"HARD", "127"}, {"DEPTH", "127"}, {"LIFE", "127"}, {"TYPE", "40"},
    {"TYPE", "80"}, {"TYPE", "127"},
};
#define NKNOB (sizeof KNOBS / sizeof KNOBS[0])

static int src_run(const tx_engine_t *e, unsigned pre, unsigned in, unsigned prox, unsigned part,
                   const char *kn, const char *kv, cost_t *c)
{
    char text[400];
    int n = snprintf(text, sizeof text, "0 particles %u\n0 param PRESET %u\n0 param ACTIVITY %u\n0 param PROXIMITY %u\n",
                     part, pre, in, prox);
    if (kn) n += snprintf(text + n, sizeof text - (size_t)n, "0 param %s %s\n", kn, kv);
    snprintf(text + n, sizeof text - (size_t)n, "0 on 60 127\n%u end\n", SECS * 1000u);
    return measure(e, text, c);
}

static void keep6(preset_cost_t *p, const cost_t *c, const char *by)
{
    if (c->worst > p->worst6) snprintf(p->worst_by, sizeof p->worst_by, "%s", by);
    p->worst6 = tx_max(p->worst6, c->worst);
    p->worst8_6 = tx_max(p->worst8_6, c->worst8);
}

/* ---- the output chain: the mix with L lanes (pan + sum), CHAMBER, limiter ---- */
static double chain_us(unsigned lanes, int chamber_on)
{
    tx_mix_t *m = malloc(sizeof *m);
    tx_chamber_t *ch = malloc(sizeof *ch);
    int32_t x[TX_BLOCK], L[TX_BLOCK], R[TX_BLOCK];
    double best = 1e30;
    unsigned r, b, k, j;
    uint32_t seed = 1;
    for (j = 0; j < TX_BLOCK; j++) { seed = seed * 1103515245u + 12345u; x[j] = (int32_t)(seed >> 9) - (1 << 22); }
    for (r = 0; r <= runs; r++) {
        uint64_t t0;
        tx_mix_init(m);
        tx_chamber_init(ch);
        m->ch = ch; m->ch_on = chamber_on;
        for (k = 0; k < TX_MIX_SLOTS; k++) tx_mix_pan(m, k, 20 + 30 * k);
        t0 = tx_now_ns();
        for (b = 0; b < 400; b++) {
            tx_mix_begin(m, TX_BLOCK);
            for (k = 0; k < lanes; k++) tx_mix_add(m, k % TX_MIX_SLOTS, x, TX_BLOCK);   /* the mix has 4 slots today: */
            tx_mix_end(m, L, R, TX_BLOCK);                                            /* more lanes reuse their adds */
        }
        if (r) { double us = (tx_now_ns() - t0) / 1000.0 / 400; if (us < best) best = us; }
    }
    free(m); free(ch);
    return best * factor;
}

/* ---- the mixes the user asked for ---- */
typedef struct { const char *lane; int preset; unsigned intensity; const char *engine; } lane_t;
typedef struct { const char *name; lane_t l[4]; } mixdef_t;
static const mixdef_t MIXES[] = {
    {"forest",  {{"songbird", 15, 64, 0}, {"wind", 4, 64, 0}, {"crickets", 6, 64, 0}, {"stream", 3, 64, 0}}},
    {"storm",   {{"rain", 0, 96, 0}, {"thunder", -1, 0, "thunder"}, {"wind", 4, 96, 0}, {"water", 3, 96, 0}}},
    {"cave",    {{"drips", 3, 16, 0}, {"minerals", 13, 64, 0}, {"water", 3, 64, 0}, {"wind", 4, 32, 0}}},
};

/* ---- the effects (tx_fx.h): one effect on a busy input (noise at -12 dBFS, all the time), its block's cost on the
 * FM-1 (the per-block minimum over runs, then the 75th percentile: the cost is flat, the host is not). kind 0 drive, 1
 * filter, 2 reverb, 3 grain delay */
static int fxc_cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double fx_cost(int kind, unsigned type, unsigned decay)
{
    static tx_fx_t fx;
    static uint64_t t[MAXR][1500];
    static double v[1500];
    int32_t L[TX_BLOCK], R[TX_BLOCK];
    unsigned r, b, j, nb = 1500, nv = 0;
    uint32_t rng;
    for (r = 0; r <= runs; r++) {
        rng = 99u;
        tx_fx_init(&fx);
        if (kind == 0) { tx_fx_drive_param(&fx.drive[0], TX_DR_AMOUNT, 90); tx_fx_drive_param(&fx.drive[0], TX_DR_TONE, 100);
                         tx_fx_drive_param(&fx.drive[0], TX_DR_MIX, type ? 127 : 100); tx_fx_drive_param(&fx.drive[0], TX_DR_TYPE, type); }
        if (kind == 1) { tx_fx_filter_param(&fx.filter[0], TX_FL_TYPE, 2); tx_fx_filter_param(&fx.filter[0], TX_FL_RESO, 90);
                         tx_fx_filter_param(&fx.filter[0], TX_FL_LFO, 90); tx_fx_filter_param(&fx.filter[0], TX_FL_FREQ, 70); }
        if (kind == 2) { tx_fx_reverb_param(&fx.rev, TX_RV_TYPE, type); tx_fx_reverb_param(&fx.rev, TX_RV_MIX, 90);
                         tx_fx_reverb_param(&fx.rev, TX_RV_DECAY, decay < 127 ? decay : 100);
                         tx_fx_reverb_param(&fx.rev, TX_RV_PREDELAY, 40); tx_fx_reverb_param(&fx.rev, TX_RV_LOWCUT, 30);
                         if (decay == 1) tx_fx_reverb_param(&fx.rev, TX_RV_DECAY, 64), tx_fx_reverb_param(&fx.rev, TX_RV_SHIMMER, 127); }
        if (kind == 3) { fx.reserve_us = 100000u;                       /* (no cap: the cost of every grain asked) */
                         tx_fx_grain_param(&fx.gd, TX_GD_MIX, 100); tx_fx_grain_param(&fx.gd, TX_GD_GRAINS, type - 1u);
                         tx_fx_grain_param(&fx.gd, TX_GD_PITCH, decay ? 71 : 64); tx_fx_grain_param(&fx.gd, TX_GD_FEEDBACK, 70);
                         tx_fx_grain_param(&fx.gd, TX_GD_SPRAY, 30); }
        for (b = 0; b < nb; b++) {
            uint64_t t0;
            if (b == 200u && decay == 127) tx_fx_reverb_param(&fx.rev, TX_RV_DECAY, 127);
            for (j = 0; j < TX_BLOCK; j++) { rng = rng * 1664525u + 1013904223u; L[j] = R[j] = (int32_t)(rng >> 10) - (1 << 21); }
            t0 = tx_now_ns();
            if (kind >= 2) tx_fx_bus(&fx, L, R, TX_BLOCK); else tx_fx_lane(&fx, 0, L, TX_BLOCK);
            if (r) t[r - 1][b] = tx_now_ns() - t0;
        }
    }
    for (b = 300; b < nb; b++) {
        uint64_t m = t[0][b];
        for (r = 1; r < runs; r++) if (t[r][b] < m) m = t[r][b];
        v[nv++] = m / 1000.0 * factor;
    }
    qsort(v, nv, sizeof v[0], fxc_cmp);
    return v[nv * 3 / 4];                                /* (flat costs: the 75th percentile, robust to the host's bursts) */
}

int main(int argc, char **argv)
{
    const tx_engine_t *src = tx_engine_find("source");
    static preset_cost_t P[64];
    unsigned np, i, k, j;
    int a;
    double ch_on4, ch_off4, ch_off0, ch_off11, lane_us, chamber, plat, pool4p, pool4d, pool11p, pool11d;
    FILE *csv;
    static cost_t MC[8][4];
    for (a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--runs") && a + 1 < argc) runs = (unsigned)atoi(argv[++a]);
        else if (!strcmp(argv[a], "--passes") && a + 1 < argc) passes = (unsigned)atoi(argv[++a]);
    }
    if (passes < 1) passes = 1;
    if (runs < 2) runs = 2;
    if (runs > MAXR) runs = MAXR;
    if (calib("build/calib/run")) { fprintf(stderr, "txbudget: no calibration in build/calib/run (make tx-calib)\n"); return 2; }
    if (!src) { fprintf(stderr, "txbudget: no source engine\n"); return 2; }
    tx_bench_prepare();
    np = (unsigned)src->params[tx_param_find(src, "PRESET")].max + 1u;
    if (np > 64) np = 64;

    ch_off0 = ch_off4 = ch_on4 = ch_off11 = 1e30;
    for (pass = 0; pass < passes; pass++) {
    cidx = 0;
    memset(P, 0, sizeof P);
    for (i = 0; i < np; i++) {
        preset_cost_t *p = &P[i];
        const char *h = src->params[tx_param_find(src, "PRESET")].help, *q;
        char tag[8];
        snprintf(tag, sizeof tag, "%u ", i);
        q = strstr(h, tag);                                 /* the preset's name from the PRESET help line */
        while (q && q != h && q[-1] != ' ') q = strstr(q + 1, tag);
        if (q) { q += strlen(tag); for (k = 0; k < sizeof p->name - 1 && q[k] && q[k] != '|'; k++) p->name[k] = q[k]; while (k && p->name[k - 1] == ' ') k--; p->name[k] = 0; }
        else snprintf(p->name, sizeof p->name, "preset %u", i);
        for (k = 0; p->name[k]; k++) if (p->name[k] == ' ') p->name[k] = '_';
        src_run(src, i, 64, 64, 6, 0, 0, &p->def); keep6(p, &p->def, "defaults");
        src_run(src, i, 16, 64, 6, 0, 0, &p->lo); keep6(p, &p->lo, "ACTIVITY 16");
        for (k = 0; k < NI; k++) {
            char by[24];
            snprintf(by, sizeof by, "ACTIVITY %u", INT_V[k]);
            src_run(src, i, INT_V[k], 127, 6, 0, 0, &p->in[k]);
            keep6(p, &p->in[k], by);
        }
        src_run(src, i, 127, 127, 2, 0, 0, &p->p2);
        src_run(src, i, 127, 127, 16, 0, 0, &p->p16);
        for (k = 0; k < NKNOB; k++) {
            cost_t c;
            char by[24];
            snprintf(by, sizeof by, "%s %s", KNOBS[k][0], KNOBS[k][1]);
            src_run(src, i, 127, 127, 6, KNOBS[k][0], KNOBS[k][1], &c);
            keep6(p, &c, by);
            if (c.worst > p->knob.worst) { p->knob = c; snprintf(p->knob_by, sizeof p->knob_by, "%s %s", KNOBS[k][0], KNOBS[k][1]); }
        }
        fprintf(stderr, "\r  pass %u / %u: %u / %u presets", pass + 1, passes, i + 1, np);
    }
    for (k = 0; k < sizeof MIXES / sizeof MIXES[0] && k < 8; k++)
        for (j = 0; j < 4; j++) {
            const lane_t *l = &MIXES[k].l[j];
            cost_t *c = &MC[k][j];
            if (l->engine) {
                const tx_engine_t *e = tx_engine_find(l->engine);
                char *t = e ? tx_scene_text(e, "max") : 0;
                if (!t || measure(e, t, c)) memset(c, 0, sizeof *c);
                free(t);
            } else src_run(src, (unsigned)l->preset, l->intensity, 64, 6, 0, 0, c);
        }
    { double v;
      if ((v = chain_us(0, 0)) < ch_off0) ch_off0 = v;
      if ((v = chain_us(4, 0)) < ch_off4) ch_off4 = v;
      if ((v = chain_us(4, 1)) < ch_on4) ch_on4 = v;
      if ((v = chain_us(11, 0)) < ch_off11) ch_off11 = v; }
    }
    fprintf(stderr, "\n");

    lane_us = (ch_off11 - ch_off0) / 11;
    chamber = ch_on4 - ch_off4;
    plat = PLAT_FIXED_US + PLAT_TAIL48_US + PLAT_TICK_US;
    /* the sources' pool: the plan ceiling less the platform, then Textura's own (measured) parts */
    pool4d = GATE_US - MARGIN_US - plat - (ch_off0 + 4 * lane_us + chamber + FX_RESERVE_US);
    pool11d = GATE_US - MARGIN_US - plat - (ch_off0 + 11 * lane_us + chamber + FX_RESERVE_US);
    pool4p = (GATE_US - MARGIN_US - plat) / UNCERT - (ch_off0 + 4 * lane_us + chamber + FX_RESERVE_US);
    pool11p = (GATE_US - MARGIN_US - plat) / UNCERT - (ch_off0 + 11 * lane_us + chamber + FX_RESERVE_US);

    printf("txbudget: Textura's whole audio interrupt on the FM-1 (a report; nothing is changed). Calibration factor %.1f"
           " (+-30 %%), the fastest of %u runs x %u passes per block, %u s per configuration\n\n", factor, runs, passes, SECS);
    printf("1. the interrupt, per block (%.0f us)                          us     %% block\n", BLOCK_US);
    printf("   gate (whole interrupt, USB at 48 kHz)                    %6.0f    %5.1f   Frodhes' rule\n", GATE_US, 100 * GATE_US / BLOCK_US);
    printf("   margin (cache misses, code from flash: not modelled)     %6.0f    %5.1f\n", MARGIN_US, 100 * MARGIN_US / BLOCK_US);
    printf("   platform: entry + output passes                          %6.0f    %5.1f   [M] Frodhes idle 312-316 less its engine\n", PLAT_FIXED_US, 100 * PLAT_FIXED_US / BLOCK_US);
    printf("   platform: USB tap + 48 kHz resampler                     %6.0f    %5.1f   [M] Frodhes 368-376 (44.1 kHz: 88)\n", PLAT_TAIL48_US, 100 * PLAT_TAIL48_US / BLOCK_US);
    printf("   platform: timer tick with the velocity scan              %6.0f    %5.1f   [E] 699 insns x 58 x 4.4 ns\n", PLAT_TICK_US, 100 * PLAT_TICK_US / BLOCK_US);
    printf("   output: mix fixed + limiter                              %6.0f    %5.1f   measured\n", ch_off0, 100 * ch_off0 / BLOCK_US);
    printf("   output: per lane (pan + sum)                             %6.1f    %5.2f   measured\n", lane_us, 100 * lane_us / BLOCK_US);
    printf("   output: CHAMBER                                          %6.0f    %5.1f   measured\n", chamber, 100 * chamber / BLOCK_US);
    printf("   effects reserve (granular delay)                         %6.0f    %5.1f   [E] not designed yet\n", FX_RESERVE_US, 100 * FX_RESERVE_US / BLOCK_US);
    printf("   SOURCES POOL, 4 lanes: device %5.0f us (%4.1f %%), planned on host estimates (/ %.1f) %5.0f us (%4.1f %%)\n",
           pool4d, 100 * pool4d / BLOCK_US, UNCERT, pool4p, 100 * pool4p / BLOCK_US);
    printf("   SOURCES POOL, 11 lanes: device %5.0f us (%4.1f %%), planned %5.0f us (%4.1f %%)   (the governor today: 3193 us, 55 %%)\n\n",
           pool11d, 100 * pool11d / BLOCK_US, pool11p, 100 * pool11p / BLOCK_US);

    printf("2. each SOURCE preset (FM-1 us): typical = mean at its defaults; worst / w8 = the worst block / the worst"
           " 8-block mean over every PARTICLES 6 configuration (ACTIVITY 0..127, PROXIMITY, knob extremes)\n");
    printf("   %-14s %7s %7s %7s %-15s | ACTIVITY 0 32 64 96 127 (w8, PARTICLES 6) | P2 / P16 worst | per hero | sanity\n",
           "preset", "typical", "worst", "w8", "(worst at)");
    csv = fopen("build/bench/budget.csv", "w");
    if (csv) fprintf(csv, "preset,typical_us,lo16_mean_us,worst6_us,worst8_6_us,int0_w8,int32_w8,int64_w8,int96_w8,int127_w8,p2_worst,p16_worst,knob_worst,knob,p2_mean,p16_mean,worst_at\n");
    for (i = 0; i < np; i++) {
        preset_cost_t *p = &P[i];
        double hero = (p->p16.mean - p->p2.mean) / 14.0;
        const char *flag = p->worst6 > pool4p ? "OVER: a block over the whole pool" : p->p16.worst > pool4p ? "OVER at P16"
                         : p->worst6 > pool4p / 2 ? "over half the pool" : "ok";
        printf("   %-14s %7.0f %7.0f %7.0f %-15s | %5.0f %5.0f %5.0f %5.0f %5.0f | %5.0f / %5.0f | %6.1f | %s\n", p->name,
               p->def.mean, p->worst6, p->worst8_6, p->worst_by, p->in[0].worst8, p->in[1].worst8, p->in[2].worst8,
               p->in[3].worst8, p->in[4].worst8, p->p2.worst, p->p16.worst, hero, flag);
        if (csv) fprintf(csv, "%s,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%s,%.1f,%.1f,%s\n", p->name, p->def.mean,
                         p->lo.mean, p->worst6, p->worst8_6, p->in[0].worst8, p->in[1].worst8, p->in[2].worst8,
                         p->in[3].worst8, p->in[4].worst8, p->p2.worst, p->p16.worst, p->knob.worst, p->knob_by,
                         p->p2.mean, p->p16.mean, p->worst_by);
    }
    if (csv) fclose(csv);
    printf("   per hero = (mean at PARTICLES 16 - at 2) / 14 at ACTIVITY 127: what one hero more costs (host noise: +-5 us)\n"
           "   sanity (a limit, not the rule): no block of a lane alone over the planned pool, at its default or at PARTICLES"
           " 16; flagged above half of it\n\n");

    printf("3. mixes (4 lanes; typical = the means at the lane's ACTIVITY, worst = every lane at its w8 at once)\n");
    for (k = 0; k < sizeof MIXES / sizeof MIXES[0]; k++) {
        const mixdef_t *mx = &MIXES[k];
        double typ = 0, w8 = 0, cmin = 1e30;
        char names[160] = "";
        for (j = 0; j < 4; j++) {
            const lane_t *l = &mx->l[j];
            cost_t c = MC[k][j];
            typ += c.mean; w8 += c.worst8;
            if (c.mean < cmin) cmin = c.mean;
            snprintf(names + strlen(names), sizeof names - strlen(names), "%s%s %.0f/%.0f", j ? ", " : "", l->lane, c.mean, c.worst8);
        }
        printf("   %-7s typical %5.0f us, worst %5.0f us: %s of the planned pool (%0.f) | lanes of this mix that fit, typical:"
               " %u (planned) / %u (device) (11 at most)\n           (%s)\n", mx->name, typ, w8,
               w8 <= pool4p ? "inside" : typ <= pool4p ? "typical inside, worst over" : "over", pool4p,
               (unsigned)(4.0 * pool11p / typ), (unsigned)(4.0 * pool11d / typ), names);
    }
    {   /* the effects: each one's cost, the chain against the effects reserve */
        static const char *const RT[TX_RVT_N] = {"off", "ROOM (the CHAMBER)", "HALL", "INFINITE", "DARK", "DRIFT", "SWELL", "PLATE"};
        double dr = fx_cost(0, 0, 0), fl = fx_cost(1, 0, 0), rv[TX_RVT_N], fz = fx_cost(2, TX_RVT_INF, 127), worst = 0;
        unsigned t;
        printf("\n4. effects (FM-1 us a block on a busy input, the 75th percentile of the per-block minima; tx_fx.h)\n");
        printf("   DRIVE (a lane, AMOUNT 90, TONE 100, MIX 100)            %5.0f us  (%4.1f %%)  per lane, from the lanes' pool\n", dr, 100 * dr / BLOCK_US);
        {   /* the circuit TYPEs, all wet (MIX 127): measured here, and Frodhes' compiled count (tx_fx_drive_us) */
            static const char *const DT[TX_DRT_N] = {"FDRV", "KHLON", "TSH8", "OHCD", "BHLUE"};
            unsigned u;
            for (u = 1; u < TX_DRT_N; u++) {
                tx_fx_drive_t dd;
                double c = fx_cost(0, u, 0);
                tx_fx_drive_init(&dd); dd.p[TX_DR_AMOUNT] = 90; dd.p[TX_DR_TYPE] = (uint8_t)u; dd.cur = (uint8_t)u; dd.on = 1;
                printf("   DRIVE TYPE %-6s (AMOUNT 90, TONE 100, MIX 127)        %5.0f us  (%4.1f %%)  counted: %u us\n", DT[u],
                       c, 100 * c / BLOCK_US, (unsigned)tx_fx_drive_us(&dd));
            }
        }
        printf("   FILTER (a lane, LP18, RESO 90, LFO 90)                  %5.0f us  (%4.1f %%)  per lane, from the lanes' pool\n", fl, 100 * fl / BLOCK_US);
        for (t = TX_RVT_ROOM; t < TX_RVT_N; t++) {
            rv[t] = fx_cost(2, t, 64);
            if (rv[t] > worst) worst = rv[t];
            printf("   REVERB %-22s (MIX 90, PREDELAY, LOWCUT) %5.0f us  (%4.1f %%)\n", RT[t], rv[t], 100 * rv[t] / BLOCK_US);
        }
        if (fz > worst) worst = fz;
        {
            double shh = fx_cost(2, TX_RVT_HALL, 1), shi = fx_cost(2, TX_RVT_INF, 1);
            printf("   REVERB HALL / INFINITE + SHIMMER 127                %5.0f / %5.0f us  (+%.0f / +%.0f)\n", shh, shi, shh - rv[TX_RVT_HALL], shi - rv[TX_RVT_INF]);
            if (shi > worst) worst = shi;
        }
        printf("   REVERB INFINITE frozen (DECAY 127)                  %5.0f us  (%4.1f %%)\n", fz, 100 * fz / BLOCK_US);
        {
            static const unsigned NG[4] = {1, 4, 8, 16};
            unsigned q;
            for (q = 0; q < 4; q++)
                printf("   GRAIN DELAY, %2u grains sounding (rate 1 / pitched)         %5.0f / %5.0f us  (%4.1f %%)\n", NG[q],
                       fx_cost(3, NG[q], 0), fx_cost(3, NG[q], 1), 100 * fx_cost(3, NG[q], 1) / BLOCK_US);
        }
        printf("   the reverb's worst TYPE %.0f us of the effects reserve (%.0f us, CHAMBER's %.0f folded in: it is a TYPE now)\n",
               worst, FX_RESERVE_US + chamber, chamber);
    }
    printf("\nEstimates from host times x the calibration (+-30 %%); the FM-1 decides at the port (whole-interrupt "
           "counters, CC 116). Platform figures are Frodhes' 0.9.5 measurements; Textura's own come at S4.\n");
    return 0;
}
