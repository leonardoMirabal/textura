/* SPDX-License-Identifier: GPL-3.0-only */
/* txtest: the checks every engine must pass (built with UBSan: any signed overflow or bad shift aborts the run).
 *
 *   txtest                  all checks, every engine; exit 1 on any failure
 *   txtest --src DIR        only the no-float source check, on DIR (to test the check itself)
 *   txtest --update         rewrite bench/baselines/renders.sha256 from the current renders (after a reviewed change)
 *
 * Per engine:
 *   state     state_bytes <= TX_STATE_MAX; particles_def within 1..particles_max
 *   idle      a fresh instance renders exact zeros for 2 s and says quiet
 *   tail      after a tap the output reaches exact zero (quiet) within 30 s and stays zero
 *   quiet     each own parameter at 0 / 64 / 90 / 127 (2 seeds, v100 / v127): a tap is quiet within 20 s
 *   overflow  the worst-case scene runs without signed overflow (UBSan) and peaks under 2^27 (16x full scale, so 4
 *             layers stay 4x inside int32); the chord scene at the default level stays under full scale
 *   cap       in the cap and max scenes the sounding particles never exceed the PARTICLES limit in force
 *   instance  two instances rendered block-interleaved give the same samples as each alone (no hidden globals)
 *   determinism  each standard scene twice from scratch: same sha256; and equal to the committed baseline
 * CHAMBER: idle zero, a quiet sweep (each knob at 0/64/90/127 after a drip tap), overflow (test max at its loudest),
 *          determinism + baseline (drip hold / max, gravel tap, test chord through the chamber at its defaults).
 * NOISE: the band filter at every corner (20 Hz .. 20 kHz, Q 0.5 .. 8, both paths, full gain) stays bounded.
 * FX: off = untouched; silence in = exact silence out; each reverb TYPE's tail ends in exact zeros; FREEZE holds its level
 *     for 3 minutes; renders against the baselines (fx-*).
 * Keys: every engine over the SMK-37's keybed (notes 48..84) at v1 / v127: no overflow, quiet after.
 * Sources: no float / double / floating literal in engines/ (comments and strings stripped). */
#include <dirent.h>
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tx_host.h"
#include "tx_core_noise.h"
#include "tx_core_limit.h"
#include "tx_core_mix.h"
#include "tx_core_gen.h"
#include "tx_core_gov.h"
#include "tx_fx.h"

#define BASELINE "bench/baselines/renders.sha256"
static int fails;

static void verdict(const char *eng, const char *check, int ok, const char *fmt, const char *detail)
{
    printf("%-5s %-8s %-12s %s%s\n", ok ? "ok" : "FAIL", eng, check, fmt ? fmt : "", detail ? detail : "");
    if (!ok) fails++;
}

/* ---- per-scene accumulator */
typedef struct {
    tx_sha256_t sha;
    int32_t peak, lpeak;           /* raw mix peak; after the output limiter */
    uint32_t over_cap, maxp, over_fs, nonzero;
    tx_limit_t lim;
    uint32_t lim_blocks, lim_db10, blocks;
} acc_t;

static void on_block(void *u, uint32_t blk, const int32_t *mix, const tx_stats_t *st)
{
    acc_t *a = u;
    unsigned j;
    (void)blk;
    tx_sha256_q23(&a->sha, mix, TX_BLOCK);
    for (j = 0; j < TX_BLOCK; j++) {
        int32_t v = mix[j] < 0 ? -mix[j] : mix[j];
        if (v > a->peak) a->peak = v;
        if (v >= TX_Q23) a->over_fs++;
        if (v) a->nonzero++;
    }
    {   /* the output limiter (the player's last stage): its peak and activity */
        int32_t l[TX_BLOCK];
        uint32_t gr;
        for (j = 0; j < TX_BLOCK; j++) l[j] = mix[j];
        tx_limit_process(&a->lim, l, TX_BLOCK);
        for (j = 0; j < TX_BLOCK; j++) { int32_t v = l[j] < 0 ? -l[j] : l[j]; if (v > a->lpeak) a->lpeak = v; }
        gr = tx_limit_take(&a->lim);
        if (gr) { a->lim_blocks++; if (gr > a->lim_db10) a->lim_db10 = gr; }
        a->blocks++;
    }
    if (st->particles > st->limit) a->over_cap++;
    if (st->particles > a->maxp) a->maxp = st->particles;
}

static int run_scene(const tx_engine_t *e, const char *scene, acc_t *a, char hex[65])
{
    tx_script_t *s = malloc(sizeof *s);
    void *st = tx_state_new(e);
    memset(a, 0, sizeof *a);
    tx_limit_init(&a->lim);
    tx_sha256_init(&a->sha);
    if (tx_scene_load(s, e, scene)) { free(s); free(st); return -1; }
    tx_script_run(e, st, s, on_block, a);
    tx_sha256_hex(&a->sha, hex);
    free(s); free(st);
    return 0;
}

/* ---- baseline file: "<sha256>  <engine>-<scene>" per line */
static char base_names[512][48], base_hex[512][65];
static unsigned base_n;
static void baseline_load(void)
{
    FILE *f = fopen(BASELINE, "r");
    char line[256];
    if (!f) return;
    while (base_n < 512 && fgets(line, sizeof line, f))
        if (sscanf(line, "%64s %47s", base_hex[base_n], base_names[base_n]) == 2) base_n++;
    fclose(f);
}
static const char *baseline_get(const char *name)
{
    unsigned i;
    for (i = 0; i < base_n; i++) if (!strcmp(base_names[i], name)) return base_hex[i];
    return 0;
}

static void check_engine(const tx_engine_t *e, FILE *update)
{
    char hex[65], hex2[65], msg[200], name[64];
    acc_t a;
    unsigned sc;
    const char *n = e->name;

    snprintf(msg, sizeof msg, "%u B (max %u), PARTICLES 1..%u default %u", e->state_bytes, TX_STATE_MAX,
             e->particles_max, e->particles_def);
    verdict(n, "state", e->state_bytes <= TX_STATE_MAX && e->particles_def >= 1 && e->particles_def <= e->particles_max
            && e->particles_max <= 32, msg, 0);

    {   /* idle: fresh instance, no events */
        void *st = tx_state_new(e);
        int32_t mix[TX_BLOCK];
        uint32_t b, nz = 0, j;
        tx_stats_t s;
        e->init(st, 1);
        for (b = 0; b < 345; b++) {
            for (j = 0; j < TX_BLOCK; j++) mix[j] = 0;
            e->render(st, mix, TX_BLOCK);
            for (j = 0; j < TX_BLOCK; j++) nz += mix[j] != 0;
        }
        e->stats(st, &s);
        snprintf(msg, sizeof msg, "%u nonzero samples in 2 s, quiet %u", nz, s.quiet);
        verdict(n, "idle", !nz && s.quiet, msg, 0);
        free(st);
    }
    {   /* tail: a tap, then render until quiet, then 1 s more must be zero */
        void *st = tx_state_new(e);
        int32_t mix[TX_BLOCK];
        uint32_t b, j, nz = 0, quiet_at = 0;
        tx_stats_t s;
        e->init(st, 7);
        e->key(st, 60, 127, 1);
        for (b = 0; b < 5168u; b++) {                       /* 30 s */
            for (j = 0; j < TX_BLOCK; j++) mix[j] = 0;
            if (b == 26) e->key(st, 60, 0, 0);              /* released after 150 ms */
            e->render(st, mix, TX_BLOCK);
            if (quiet_at) for (j = 0; j < TX_BLOCK; j++) nz += mix[j] != 0;
            e->stats(st, &s);
            if (!quiet_at && s.quiet && b > 26) quiet_at = b;
            if (quiet_at && b > quiet_at + 172u) break;
        }
        snprintf(msg, sizeof msg, "quiet after %.2f s, %u nonzero samples after", quiet_at * 256.0 / 44100.0, nz);
        verdict(n, "tail", quiet_at && !nz, msg, 0);
        free(st);
    }
    {   /* quiet sweep: each own parameter at 0 / 64 / 90 / 127 (the others at default), a tap: quiet within 20 s.
         * Catches states that never reach exact zero (e.g. a resonator frozen by rounding) in non-default corners. */
        static const uint8_t VALS[4] = {0, 64, 90, 127};
        void *st = tx_state_new(e);
        int32_t mix[TX_BLOCK];
        unsigned p, k, bad = 0, worst_b = 0;
        char where[80] = "";
        for (p = 0; p < e->n_params; p++)
            for (k = 0; k < 8; k++) {                        /* 4 values x (seed 3 v100, seed 7 v127) */
                uint32_t b, j, quiet_at = 0;
                tx_stats_t s;
                uint8_t v = VALS[k & 3] > e->params[p].max ? e->params[p].max : VALS[k & 3];
                e->init(st, k < 4 ? 3 : 7);
                e->param(st, (uint8_t)p, v);
                e->key(st, 60, k < 4 ? 100 : 127, 1);
                for (b = 0; b < 3446u && !quiet_at; b++) {       /* 20 s */
                    for (j = 0; j < TX_BLOCK; j++) mix[j] = 0;
                    if (b == 26) e->key(st, 60, 0, 0);
                    e->render(st, mix, TX_BLOCK);
                    e->stats(st, &s);
                    if (b > 26 && s.quiet) quiet_at = b;
                }
                if (!quiet_at) { if (!bad++) snprintf(where, sizeof where, " (first: %s %u)", e->params[p].name, v); }
                else if (quiet_at > worst_b) worst_b = quiet_at;
            }
        snprintf(msg, sizeof msg, "every parameter at 0/64/90/127, 2 seeds: a tap is quiet within 20 s (%u never; slowest %.1f s)%s",
                 bad, worst_b * 256.0 / 44100.0, where);
        verdict(n, "quiet", !bad, msg, 0);
        free(st);
    }
    if (!run_scene(e, "max", &a, hex)) {
        snprintf(msg, sizeof msg, "max scene peak %.1f dBFS (limit +24.1), %u samples over full scale (info)",
                 a.peak ? 20.0 * log10(a.peak / 8388608.0) : -999.0, a.over_fs);
        verdict(n, "overflow", a.peak < (1 << 27), msg, 0);
        snprintf(msg, sizeof msg, "max scene: particles <= limit at every block (most %u)", a.maxp);
        verdict(n, "cap", !a.over_cap, msg, 0);
    } else verdict(n, "overflow", 0, "max scene does not load", 0);
    if (!run_scene(e, "chord", &a, hex)) {           /* the limited chord under full scale, the raw one <= +12 dBFS */
        snprintf(msg, sizeof msg, "chord at the default level: raw peak %.1f dBFS (<= +12), limited %.1f dBFS",
                 a.peak ? 20.0 * log10(a.peak / 8388608.0) : -999.0, a.lpeak ? 20.0 * log10(a.lpeak / 8388608.0) : -999.0);
        verdict(n, "level", a.peak <= 33410000 && a.lpeak < TX_Q23, msg, 0);
    }
    {   /* how often and how deep the limiter acts at the defaults (info: a safety net, it should rarely act) */
        static const char *const SC[3] = { "tap", "hold", "chord" };
        unsigned q, lb = 0, nb = 0, deep = 0;
        for (q = 0; q < 3; q++)
            if (!run_scene(e, SC[q], &a, hex)) { lb += a.lim_blocks; nb += a.blocks; if (a.lim_db10 > deep) deep = a.lim_db10; }
        snprintf(msg, sizeof msg, "output limiter at the defaults (tap, hold, chord): %u of %u blocks (%.1f %%), deepest %.1f dB (info)",
                 lb, nb, nb ? 100.0 * lb / nb : 0.0, deep / 10.0);
        verdict(n, "limiter", 1, msg, 0);
    }
    if (!run_scene(e, "cap", &a, hex)) {
        snprintf(msg, sizeof msg, "cap scene (32 -> 4 -> 1 -> 32 -> 8): %u blocks over the limit, most %u", a.over_cap, a.maxp);
        verdict(n, "cap", !a.over_cap, msg, 0);
    }
    {   /* instance independence: max and chord interleaved block by block vs alone */
        tx_script_t *s1 = malloc(sizeof *s1), *s2 = malloc(sizeof *s2);
        void *A = tx_state_new(e), *B = tx_state_new(e);
        int32_t m1[TX_BLOCK], m2[TX_BLOCK];
        tx_sha256_t h1, h2;
        char x1[65], x2[65], y1[65], y2[65];
        unsigned i1 = 0, i2 = 0;
        uint32_t b;
        acc_t a1, a2;
        if (!tx_scene_load(s1, e, "max") && !tx_scene_load(s2, e, "chord")) {
            tx_sha256_init(&h1); tx_sha256_init(&h2);
            e->init(A, s1->seed); e->init(B, s2->seed);
            for (b = 0; b < s1->blocks || b < s2->blocks; b++) {
                if (b < s1->blocks) { tx_script_step(e, A, s1, b, &i1, m1); tx_sha256_q23(&h1, m1, TX_BLOCK); }
                if (b < s2->blocks) { tx_script_step(e, B, s2, b, &i2, m2); tx_sha256_q23(&h2, m2, TX_BLOCK); }
            }
            tx_sha256_hex(&h1, x1); tx_sha256_hex(&h2, x2);
            run_scene(e, "max", &a1, y1); run_scene(e, "chord", &a2, y2);
            verdict(n, "instance", !strcmp(x1, y1) && !strcmp(x2, y2), "two instances interleaved = each alone", 0);
        }
        free(s1); free(s2); free(A); free(B);
    }
    for (sc = 0; tx_std_scenes[sc]; sc++) {                 /* determinism and the baseline */
        const char *b;
        snprintf(name, sizeof name, "%s-%s", n, tx_std_scenes[sc]);
        if (run_scene(e, tx_std_scenes[sc], &a, hex) || run_scene(e, tx_std_scenes[sc], &a, hex2)) {
            verdict(n, "determinism", 0, name, " does not load");
            continue;
        }
        if (strcmp(hex, hex2)) { verdict(n, "determinism", 0, name, ": two renders differ"); continue; }
        if (update) { fprintf(update, "%s  %s\n", hex, name); continue; }
        b = baseline_get(name);
        snprintf(msg, sizeof msg, "%s %.16s", name, hex);
        verdict(n, "determinism", b && !strcmp(b, hex), msg,
                !b ? " (no baseline: make tx-baseline)" : strcmp(b, hex) ? " != baseline (a change moved the sound)" : " = baseline");
    }
}

/* ---- CHAMBER (the post-stage): idle zero, a quiet sweep, overflow under the loudest input, determinism / baseline */
static int run_ch(const tx_engine_t *e, const char *scene, const char *extra, acc_t *a, char hex[65])
{
    tx_script_t *s = malloc(sizeof *s);
    void *st = tx_state_new(e);
    tx_chamber_t *ch = malloc(sizeof *ch);
    char *t = tx_scene_text(e, scene), *u;
    memset(a, 0, sizeof *a);
    tx_limit_init(&a->lim);
    tx_sha256_init(&a->sha);
    if (!t) { free(s); free(st); free(ch); return -1; }
    u = malloc(strlen(t) + strlen(extra) + 2);
    sprintf(u, "%s\n%s", extra, t);
    if (tx_script_parse(s, e, u)) { fprintf(stderr, "%s\n", s->err); free(t); free(u); free(s); free(st); free(ch); return -1; }
    tx_script_run_ch(e, st, ch, s, on_block, a);
    tx_sha256_hex(&a->sha, hex);
    free(t); free(u); free(s); free(st); free(ch);
    return 0;
}

/* ---- variation: no two events may sound the same. The same key (C4) at the same velocity (100) is pressed 8 times,
 * each press released after 150 ms and the next one only once the engine is quiet again (at most 25 s); the first
 * 1.5 s after each press is the event's window. Every pair must differ: the normalized cross-correlation (the best
 * lag within +-1 ms) under 0.9, and their dB envelopes (512-sample frames, louder than -70 dBFS) or their 1/3-octave
 * spectra (100 Hz .. 16 kHz) at least 0.5 dB apart on average. The spread is reported: the std of the event's level (dB), brightness (zero-crossing rate, Hz) and
 * length (to -40 dB under its peak, ms). */
#define VAR_N 8
#define VAR_WIN 66150u             /* 1.5 s */
#define VAR_LAG 44
static int32_t *var_buf[VAR_N];

/* the window's 1/3-octave band levels (dB, 100 Hz .. 16 kHz: 23 bands) from an averaged 4096-point power spectrum */
#define VAR_NB 23
static void var_bands(const int32_t *x, double *db)
{
    static double re[4096], im[4096], pw[2049];
    unsigned f, i, j, k, m, nfr = VAR_WIN / 4096u;
    for (i = 0; i <= 2048; i++) pw[i] = 0;
    for (f = 0; f < nfr; f++) {
        for (i = 0; i < 4096; i++) { re[i] = x[f * 4096 + i] * (0.5 - 0.5 * cos(2 * M_PI * i / 4096.0)); im[i] = 0; }
        for (i = 1, j = 0; i < 4096; i++) {                  /* bit reversal, then radix-2 butterflies */
            unsigned b = 2048;
            for (; j & b; b >>= 1) j ^= b;
            j |= b;
            if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
        }
        for (m = 2; m <= 4096; m <<= 1)
            for (k = 0; k < 4096; k += m)
                for (i = 0; i < m / 2; i++) {
                    double wr = cos(-2 * M_PI * i / m), wi = sin(-2 * M_PI * i / m);
                    double xr = re[k + i + m / 2] * wr - im[k + i + m / 2] * wi, xi = re[k + i + m / 2] * wi + im[k + i + m / 2] * wr;
                    re[k + i + m / 2] = re[k + i] - xr; im[k + i + m / 2] = im[k + i] - xi;
                    re[k + i] += xr; im[k + i] += xi;
                }
        for (i = 0; i <= 2048; i++) pw[i] += re[i] * re[i] + im[i] * im[i];
    }
    for (k = 0; k < VAR_NB; k++) {
        double fc = 100.0 * pow(2.0, k / 3.0), lo = fc * pow(2.0, -1.0 / 6), hi = fc * pow(2.0, 1.0 / 6), e = 0;
        for (i = 1; i <= 2048; i++) { double fr = i * 44100.0 / 4096; if (fr >= lo && fr < hi) e += pw[i]; }
        db[k] = 10.0 * log10(e / (7.03687e13 * 4096.0 * nfr) + 1e-20);
    }
}

/* band centroid (Hz) of a band-level vector */
static double var_centroid(const double *db)
{
    double w = 0, sw = 0;
    unsigned q;
    for (q = 0; q < VAR_NB; q++) { double e = pow(10.0, db[q] / 10.0); sw += e * 100.0 * pow(2.0, q / 3.0); w += e; }
    return w > 0 ? sw / w : 0;
}

static double var_std(const double *x, unsigned n, double *mean)
{
    double m = 0, v = 0;
    unsigned i;
    for (i = 0; i < n; i++) m += x[i];
    m /= n;
    for (i = 0; i < n; i++) v += (x[i] - m) * (x[i] - m);
    if (mean) *mean = m;
    return sqrt(v / n);
}

/* what the ear hears between 8 presses of the same key (C4 v100), each event's first 1.5 s: the waveforms (pairwise
 * correlation, best lag within +-1 ms: mean and max), the level (dB), the brightness (1/3-octave centroid), the
 * length (to -20 dB under the event's peak, 512-sample frames) and the envelope shape (mean |dB| difference of
 * 512-sample frames: the event pattern). First a key held 16 s tells a sustained texture (still sounding at the end)
 * from an event engine; a texture's presses are held 1 s, an event's released after 150 ms, the next press once
 * quiet. A texture must also drift while held (2 s segments: a level or a brightness spread): it never loops.
 * Thresholds (VAR_*): the stone cloud, which the user praised for its variety (LISTENING.md, session 5), passes them
 * all with room; the held drift is set by the references (real rain on a roof or a pond is steady: 0.1-0.7 dB,
 * 2-9 %; brooks 0.4-1.1 dB, 5-11 %; surf 1.6-4 dB, 15-22 %): ours wanders at least like a brook. */
#define VAR_MEANC 0.5              /* mean pairwise correlation under this */
#define VAR_MAXC 0.9               /* and no pair over this */
#define VAR_LVL 2.0                /* level spread (std, dB) at least (2 JND: the "same" ones were 0.2-1.7) */
#define VAR_CEN 0.10               /* brightness spread (std / mean) at least */
#define VAR_LEN 0.12               /* events: length spread (std / mean) at least (when shorter than 1.2 s:
                                    * longer ones fill the window; their pattern tells them apart) */
#define VAR_SHAPE 3.0              /* mean envelope distance (dB) at least: the pattern differs */
#define VAR_HLVL 1.0               /* textures held: level drift (std of 2 s segments, dB) at least */
#define VAR_HCEN 0.05              /* or brightness drift (std / mean) at least */
static void check_variation(const tx_engine_t *e)
{
    void *st = tx_state_new(e);
    int32_t mix[TX_BLOCK];
    double lv[VAR_N], cen[VAR_N], du[VAR_N], sb[VAR_N][VAR_NB], maxc = 0, sumc = 0, sumd = 0;
    double env[VAR_N][VAR_WIN / 512u], hl, hc;
    unsigned i, j, k, nfr = VAR_WIN / 512u, np = 0, sus, hold;
    char msg[400];
    tx_stats_t s;
    {   /* held: 16 s, 2 s segments */
        double sl2[8], sc2[8], mm;
        unsigned seg;
        static int32_t sbuf[VAR_WIN];
        e->init(st, 13);
        e->key(st, 60, 100, 1);
        for (seg = 0; seg < 8; seg++) {
            uint32_t w = 0;
            double se = 0, bd[VAR_NB];
            while (w < 88200u) {
                for (j = 0; j < TX_BLOCK; j++) mix[j] = 0;
                e->render(st, mix, TX_BLOCK);
                for (j = 0; j < TX_BLOCK; j++, w++) { se += (double)mix[j] * mix[j]; if (w < VAR_WIN) sbuf[w] = mix[j]; }
            }
            sl2[seg] = 10.0 * log10(se / w / 7.03687e13 + 1e-20);
            var_bands(sbuf, bd);
            sc2[seg] = var_centroid(bd);
        }
        sus = sl2[7] > -70 && sl2[7] > sl2[0] - 20;
        hl = var_std(sl2, 8, &mm);
        hc = var_std(sc2, 8, &mm); hc = mm > 0 ? hc / mm : 0;
    }
    hold = sus ? 172u : 26u;
    e->init(st, 11);
    for (i = 0; i < VAR_N; i++) {
        uint32_t b, w = 0, q;
        double pk = -200;
        if (!var_buf[i]) var_buf[i] = malloc(sizeof(int32_t) * VAR_WIN);
        e->key(st, 60, 100, 1);
        for (b = 0; w < VAR_WIN; b++) {
            for (j = 0; j < TX_BLOCK; j++) mix[j] = 0;
            if (b == hold) e->key(st, 60, 0, 0);
            e->render(st, mix, TX_BLOCK);
            for (j = 0; j < TX_BLOCK && w < VAR_WIN; j++) var_buf[i][w++] = mix[j];
        }
        if (b <= hold) e->key(st, 60, 0, 0);
        for (q = 0; q < 4307u; q++) {
            for (j = 0; j < TX_BLOCK; j++) mix[j] = 0;
            e->render(st, mix, TX_BLOCK);
            e->stats(st, &s);
            if (s.quiet) break;
        }
        {
            double se = 0;
            unsigned f, last = 0;
            for (j = 0; j < VAR_WIN; j++) se += (double)var_buf[i][j] * var_buf[i][j];
            lv[i] = 10.0 * log10(se / VAR_WIN / 7.03687e13 + 1e-20);
            var_bands(var_buf[i], sb[i]);
            cen[i] = var_centroid(sb[i]);
            for (f = 0; f < nfr; f++) {
                double a = 0;
                for (j = f * 512u; j < f * 512u + 512u; j++) a += (double)var_buf[i][j] * var_buf[i][j];
                env[i][f] = 10.0 * log10(a / 512.0 / 7.03687e13 + 1e-20);
                if (env[i][f] < -80) env[i][f] = -80;
                if (env[i][f] > pk) pk = env[i][f];
            }
            for (f = 0; f < nfr; f++) if (env[i][f] > pk - 20) last = f;
            du[i] = (last + 1) * 512 / 44.1;
        }
    }
    for (i = 0; i < VAR_N; i++)
        for (k = i + 1; k < VAR_N; k++) {
            double best = 0, ea = 0, eb = 0, dd = 0;
            int lag;
            unsigned f, nf = 0;
            for (j = 0; j < VAR_WIN; j++) { ea += (double)var_buf[i][j] * var_buf[i][j]; eb += (double)var_buf[k][j] * var_buf[k][j]; }
            if (ea > 0 && eb > 0)
                for (lag = -VAR_LAG; lag <= VAR_LAG; lag++) {
                    double c = 0;
                    for (j = VAR_LAG; j < VAR_WIN - VAR_LAG; j++) c += (double)var_buf[i][j] * var_buf[k][(int)j + lag];
                    c /= sqrt(ea * eb);
                    if (c > best) best = c;
                }
            else if (ea == eb) best = 1;
            for (f = 0; f < nfr; f++) { if (env[i][f] <= -79 && env[k][f] <= -79) continue; dd += fabs(env[i][f] - env[k][f]); nf++; }
            dd = nf ? dd / nf : 0;
            if (best > maxc) maxc = best;
            sumc += best; sumd += dd; np++;
        }
    {
        double ml, mc, md, sl = var_std(lv, VAR_N, &ml), sc = var_std(cen, VAR_N, &mc), sd = var_std(du, VAR_N, &md);
        double meanc = sumc / np, shape = sumd / np, rc = mc > 0 ? sc / mc : 0, rd = md > 0 ? sd / md : 0;
        int ok = meanc < VAR_MEANC && maxc < VAR_MAXC && sl >= VAR_LVL && rc >= VAR_CEN && shape >= VAR_SHAPE &&
                 (sus ? hl >= VAR_HLVL || hc >= VAR_HCEN : rd >= VAR_LEN || md >= 1200);
        if (!strcmp(e->name, "test")) ok = 1;            /* the calibration tone: alike on purpose */
        if (sus)
            snprintf(msg, sizeof msg, "texture, 8 presses: corr %.2f / %.2f | level %.1f dB | brightness %.0f %% of %.0f Hz | pattern %.1f dB"
                     " | held 16 s: level %.1f dB, brightness %.0f %%", meanc, maxc, sl, 100 * rc, mc, shape, hl, 100 * hc);
        else
            snprintf(msg, sizeof msg, "event, 8 presses: corr %.2f / %.2f | level %.1f dB | brightness %.0f %% of %.0f Hz | pattern %.1f dB"
                     " | length %.0f %% of %.0f ms", meanc, maxc, sl, 100 * rc, mc, shape, 100 * rd, md);
        verdict(e->name, "variation", ok, msg, 0);
    }
    free(st);
}

static void check_chamber(FILE *update)
{
    static const char *const CH_SCENES[][2] = { {"drip", "hold"}, {"gravel", "tap"}, {"test", "chord"}, {"drip", "max"}, {0, 0} };
    tx_chamber_t *ch = malloc(sizeof *ch);
    int32_t buf[TX_BLOCK];
    char hex[65], hex2[65], msg[200], name[64];
    acc_t a;
    unsigned b, j, nz = 0, p, k, bad = 0, i;
    uint32_t worst = 0;
    const tx_engine_t *drip = tx_engine_find("drip");
    tx_chamber_init(ch);
    for (b = 0; b < 345; b++) {                             /* idle: silence in -> exact zeros, never active */
        for (j = 0; j < TX_BLOCK; j++) buf[j] = 0;
        tx_chamber_process(ch, buf, TX_BLOCK);
        for (j = 0; j < TX_BLOCK; j++) nz += buf[j] != 0;
    }
    snprintf(msg, sizeof msg, "%u nonzero samples in 2 s of silence, idle %d", nz, tx_chamber_quiet(ch));
    verdict("chamber", "idle", !nz && tx_chamber_quiet(ch), msg, 0);
    if (drip) {                                             /* quiet sweep: a drip tap, each chamber knob at 0/64/90/127 */
        void *st = tx_state_new(drip);
        for (p = 0; p < TX_CH_NP; p++)
            for (k = 0; k < 4; k++) {
                static const uint8_t VALS[4] = {0, 64, 90, 127};
                uint32_t quiet_at = 0;
                tx_stats_t s;
                drip->init(st, 3);
                tx_chamber_init(ch);
                tx_chamber_param(ch, p, VALS[k]);
                drip->key(st, 60, 127, 1);
                for (b = 0; b < 3446u && !quiet_at; b++) {
                    for (j = 0; j < TX_BLOCK; j++) buf[j] = 0;
                    if (b == 26) drip->key(st, 60, 0, 0);
                    drip->render(st, buf, TX_BLOCK);
                    tx_chamber_process(ch, buf, TX_BLOCK);
                    drip->stats(st, &s);
                    if (b > 26 && s.quiet && tx_chamber_quiet(ch)) quiet_at = b;
                }
                if (!quiet_at) bad++; else if (quiet_at > worst) worst = quiet_at;
            }
        snprintf(msg, sizeof msg, "a drip tap, each chamber knob at 0/64/90/127: idle within 20 s (%u never; slowest %.1f s)",
                 bad, worst * 256.0 / 44100.0);
        verdict("chamber", "quiet", !bad, msg, 0);
        free(st);
    }
    if (!run_ch(tx_engine_find("test"), "max", "0 chamber SIZE 127\n0 chamber ABSORB 0\n0 chamber BODY 127\n0 chamber TAIL 127",
                &a, hex)) {
        snprintf(msg, sizeof msg, "test max scene through the chamber at its loudest: peak %.1f dBFS (limit +24.1)",
                 a.peak ? 20.0 * log10(a.peak / 8388608.0) : -999.0);
        verdict("chamber", "overflow", a.peak < (1 << 27), msg, 0);
    }
    for (i = 0; CH_SCENES[i][0]; i++) {
        const tx_engine_t *e = tx_engine_find(CH_SCENES[i][0]);
        const char *bl;
        if (!e) continue;
        snprintf(name, sizeof name, "chamber+%s-%s", CH_SCENES[i][0], CH_SCENES[i][1]);
        if (run_ch(e, CH_SCENES[i][1], "", &a, hex) || run_ch(e, CH_SCENES[i][1], "", &a, hex2)) {
            verdict("chamber", "determinism", 0, name, " does not load"); continue;
        }
        if (strcmp(hex, hex2)) { verdict("chamber", "determinism", 0, name, ": two renders differ"); continue; }
        if (update) { fprintf(update, "%s  %s\n", hex, name); continue; }
        bl = baseline_get(name);
        snprintf(msg, sizeof msg, "%s %.16s", name, hex);
        verdict("chamber", "determinism", bl && !strcmp(bl, hex), msg,
                !bl ? " (no baseline: make tx-baseline)" : strcmp(bl, hex) ? " != baseline (a change moved the sound)" : " = baseline");
    }
    free(ch);
}

/* ---- NOISE core: the band filter (Chamberlin) driven at every corner (centre 20 Hz .. 20 kHz, Q 0.5 .. 8, both
 * paths at full gain for 1 s): its states must stay bounded (|band|, |low| < 2^17, so f x band < 2^31). An unstable
 * corner grows without bound (Aeolian's wake reached +48 dBFS before the clamp in tx_noise_band). */
static void check_noise_core(void)
{
    static const uint32_t FC[] = {20, 1000, 5000, 7350, 12000, 20000}, QX[] = {8, 11, 16, 32, 128};
    unsigned a, b, j, k, bad = 0;
    int32_t worst = 0, out[TX_BLOCK];
    char msg[200], where[64] = "";
    for (a = 0; a < sizeof FC / sizeof FC[0]; a++)
        for (b = 0; b < sizeof QX / sizeof QX[0]; b++)
            for (k = 0; k < 2u; k++) {                      /* k 0: the roar path (+ tilt), k 1: the classic band path */
                tx_noise_t z;
                int32_t m = 0;
                tx_noise_init(&z, 99u + a * 7u + b);
                tx_noise_band(&z, FC[a], QX[b], TX_NOISE_BAND);
                if (!k) tx_noise_tilt(&z, FC[a]);
                tx_noise_level(&z, 32767);
                for (j = 0; j < 172u; j++) {
                    unsigned i;
                    for (i = 0; i < TX_BLOCK; i++) out[i] = 0;
                    if (!k) tx_noise_roar_add(&z, 524288, out, TX_BLOCK); else tx_noise_add(&z, out, TX_BLOCK);
                    if ((z.band < 0 ? -z.band : z.band) > m) m = z.band < 0 ? -z.band : z.band;
                    if ((z.low < 0 ? -z.low : z.low) > m) m = z.low < 0 ? -z.low : z.low;
                    if (m >= (1 << 17)) break;
                }
                if (m > worst) worst = m;
                if (m >= (1 << 17) && !bad++) snprintf(where, sizeof where, " (first: %u Hz, Q x16 %u, %s)", FC[a], QX[b], k ? "band" : "roar");
            }
    snprintf(msg, sizeof msg, "band filter at every corner: states bounded (largest %d, limit 131072; %u unbounded)%s", worst, bad, where);
    verdict("noise", "stable", !bad, msg, 0);
}

/* ---- GEN: the lane generator, 10 minutes alone at EVOLVE 2 h and at 10 s, twice each: deterministic, the activity
 * inside 0 .. 127 and wandering (every minute moves), notes bounded (at most 40 in a second), pitches inside the span,
 * sounding notes released (txgen runs the long version through a source) */
static void check_gen(void)
{
    static const uint8_t EV[2] = {127, 0};
    unsigned c;
    for (c = 0; c < 2; c++) {
        tx_sha256_t h[2];
        char hx[2][65], msg[220];
        uint32_t still = 0, maxn = 0, bad_pitch = 0, notes = 0, on = 0, mute = 0, worst_mute = 0;
        int32_t amin = 1 << 30, amax = -1;
        unsigned run;
        for (run = 0; run < 2; run++) {
            tx_gen_t *g = malloc(sizeof *g);
            uint32_t b, nb = 600u * TX_FS / TX_BLOCK, secn = 0, sample = 0;
            int32_t mlo = 1 << 30, mhi = -1;
            tx_sha256_init(&h[run]);
            tx_gen_init(g, 9);
            tx_gen_param(g, TX_GEN_EVOLVE, EV[c]);
            tx_gen_param(g, TX_GEN_RANDOM, 100);
            for (b = 0; b < nb; b++) {
                tx_gen_ev_t ev[TX_GEN_EV_MAX];
                uint8_t in;
                unsigned ne = tx_gen_block(g, TX_BLOCK, ev, TX_GEN_EV_MAX, &in), k;
                tx_sha256_add(&h[run], ev, ne * sizeof ev[0]);
                tx_sha256_add(&h[run], &in, 1);
                for (k = 0; k < ne; k++) {
                    if (ev[k].vel) { secn++; notes++; on++; if (ev[k].note < 60 || ev[k].note > 72) bad_pitch++; }
                    else if (on) on--;
                }
                if (!run) {
                    if (in < amin) amin = in;
                    if (in > amax) amax = in;
                    if (in < mlo) mlo = in;
                    if (in > mhi) mhi = in;
                }
                sample += TX_BLOCK;
                if (sample >= TX_FS) {
                    sample -= TX_FS;
                    if (secn > maxn) maxn = secn;
                    if (!run) { if (!secn && in >= 40) { if (++mute > worst_mute) worst_mute = mute; } else if (secn) mute = 0; }
                    secn = 0;
                }
                if (!run && b % (60u * TX_FS / TX_BLOCK) == 60u * TX_FS / TX_BLOCK - 1u) { if (mhi - mlo < 2) still++; mlo = 1 << 30; mhi = -1; }
            }
            tx_sha256_hex(&h[run], hx[run]);
            free(g);
        }
        snprintf(msg, sizeof msg, "EVOLVE %u, 10 min: activity %d .. %d, %u still minutes, up to %u notes/s, %u notes (%u off the span),"
                 " at most %u s without a note (activity >= 40), deterministic %s",
                 EV[c], amin, amax, still, maxn, notes / 2u, bad_pitch, worst_mute, strcmp(hx[0], hx[1]) ? "NO" : "yes");
        verdict("gen", "longrun", !strcmp(hx[0], hx[1]) && amin >= 0 && amax <= 127 && amax - amin >= 8 && !still && maxn <= 40
                && !bad_pitch && worst_mute <= 60, msg, 0);
    }
}

/* ---- GOV: the CPU governor. Three lanes over budget: it takes heroes from the quietest lane first, one every 8
 * blocks, never below 2; back under 3/4 of the budget it gives one back a second, the loudest lane first, up to each
 * lane's own setting; and a source asked for fewer heroes keeps its loudest (the quietest go: tx_eng_source) */
static void check_gov(void)
{
    tx_gov_t g;
    uint32_t cost[3] = {1500, 1500, 1500}, lvl[3] = {300, 100, 200}, b, cuts = 0, first = 9;
    uint8_t on[3] = {1, 1, 1};
    char msg[220];
    int ok = 1;
    tx_gov_init(&g, 3, 3193);
    tx_gov_user(&g, 0, 6); tx_gov_user(&g, 1, 6); tx_gov_user(&g, 2, 6);
    for (b = 0; b < 200; b++) {                              /* 4500 us: over */
        int k = tx_gov_block(&g, cost, lvl, on);
        if (k >= 0) { cuts++; if (first == 9) first = (uint32_t)k; }
    }
    ok = first == 1 && g.cur[1] == TX_GOV_MIN && g.cur[0] >= g.cur[2] && g.cur[2] >= TX_GOV_MIN;
    cost[0] = cost[1] = cost[2] = 500;                       /* 1500 us: well under */
    for (b = 0; b < 172u * 30u; b++) tx_gov_block(&g, cost, lvl, on);
    ok = ok && g.cur[0] == 6 && g.cur[1] == 6 && g.cur[2] == 6;
    snprintf(msg, sizeof msg, "over budget: %u cuts, the quietest lane first (lane %u), none under %u; under: all back to 6 (%u %u %u)",
             cuts, first + 1u, TX_GOV_MIN, g.cur[0], g.cur[1], g.cur[2]);
    verdict("gov", "lanes", ok, msg, 0);
    {   /* the source keeps its loudest heroes when the pool shrinks */
        const tx_engine_t *e = tx_engine_find("source");
        if (e) {
            void *st = tx_state_new(e);
            int32_t mix[TX_BLOCK];
            tx_stats_t s1, s2;
            unsigned j;
            e->init(st, 4);
            e->param(st, TX_PID_PARTICLES, 16);
            e->param(st, (uint8_t)tx_param_find(e, "PRESET"), 1);
            e->param(st, (uint8_t)tx_param_find(e, "INTENSITY"), 120);
            e->key(st, 60, 127, 1);
            for (b = 0; b < 200; b++) { for (j = 0; j < TX_BLOCK; j++) mix[j] = 0; e->render(st, mix, TX_BLOCK); }
            e->stats(st, &s1);
            e->param(st, TX_PID_PARTICLES, 3);
            for (j = 0; j < TX_BLOCK; j++) mix[j] = 0;
            e->render(st, mix, TX_BLOCK);
            e->stats(st, &s2);
            snprintf(msg, sizeof msg, "the source's stone cloud 16 -> 3 heroes: %u sounding before, %u after (<= 3), still sounding",
                     s1.particles, s2.particles);
            verdict("gov", "source", s2.particles <= 3 && s2.particles > 0 && !s2.quiet, msg, 0);
            free(st);
        }
    }
}

/* ---- SWITCH: the source switched between its presets every 40 blocks (400 times, a lane's generator pressing its
 * notes, PARTICLES moved now and then), under UBSan: no undefined behaviour, the output bounded, quiet at the end */
static void check_switch(void)
{
    const tx_engine_t *e = tx_engine_find("source");
    tx_gen_t *g = malloc(sizeof *g);
    void *st;
    int32_t mix[TX_BLOCK], peak = 0;
    unsigned it, b, j, seed = 1, np;
    int pp, pi;
    char msg[160];
    if (!e || !g) { free(g); return; }
    st = tx_state_new(e);
    pp = tx_param_find(e, "PRESET"); pi = tx_param_find(e, "INTENSITY");
    np = e->params[pp].max + 1u;
    tx_gen_init(g, 3);
    e->init(st, 3);
    e->key(st, 60, 100, 1);
    for (it = 0; it < 400; it++) {
        seed = seed * 1103515245u + 12345u;
        e->param(st, (uint8_t)pp, (uint8_t)((seed >> 16) % np));
        e->param(st, (uint8_t)pi, (uint8_t)((seed >> 8) & 127u));
        if (!((seed >> 4) & 7u)) e->param(st, TX_PID_PARTICLES, (uint8_t)(1u + (seed >> 12) % 16u));
        for (b = 0; b < 40; b++) {
            tx_gen_ev_t ev[TX_GEN_EV_MAX];
            uint8_t in;
            unsigned ne = tx_gen_block(g, TX_BLOCK, ev, TX_GEN_EV_MAX, &in), k;
            for (k = 0; k < ne; k++) e->key(st, ev[k].note, ev[k].vel, ev[k].vel != 0);
            for (j = 0; j < TX_BLOCK; j++) mix[j] = 0;
            e->render(st, mix, TX_BLOCK);
            for (j = 0; j < TX_BLOCK; j++) { int32_t v = mix[j] < 0 ? -mix[j] : mix[j]; if (v > peak) peak = v; }
        }
    }
    {   /* released: quiet within 30 s */
        tx_gen_ev_t ev[TX_GEN_OFFS];
        unsigned ne = tx_gen_stop(g, ev, TX_GEN_OFFS), k;
        tx_stats_t sx;
        for (k = 0; k < ne; k++) e->key(st, ev[k].note, 0, 0);
        e->key(st, 60, 0, 0);
        for (b = 0; b < 5168u; b++) { for (j = 0; j < TX_BLOCK; j++) mix[j] = 0; e->render(st, mix, TX_BLOCK); e->stats(st, &sx); if (sx.quiet) break; }
        snprintf(msg, sizeof msg, "400 preset switches with a lane's notes: peak %.1f dBFS, quiet %.1f s after the release",
                 peak ? 20.0 * log10(peak / 8388608.0) : -999.0, b * 256.0 / 44100.0);
        verdict("source", "switch", peak < (1 << 27) && sx.quiet, msg, 0);
    }
    free(st); free(g);
}

/* ---- the source check: no float in engines/ */
static void strip(char *t)                                   /* comments, strings, chars -> spaces */
{
    char *p = t;
    while (*p) {
        if (p[0] == '/' && p[1] == '*') { char *e = strstr(p + 2, "*/"); char *end = e ? e + 2 : p + strlen(p);
                                          while (p < end) { if (*p != '\n') *p = ' '; p++; } }
        else if (p[0] == '/' && p[1] == '/') { while (*p && *p != '\n') *p++ = ' '; }
        else if (*p == '"' || *p == '\'') { char q = *p; *p++ = ' ';
                                            while (*p && *p != q) { if (*p == '\\' && p[1]) *p++ = ' '; if (*p != '\n') *p = ' '; p++; }
                                            if (*p) *p++ = ' '; }
        else p++;
    }
}
static void check_sources(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    unsigned files = 0;
    int f0 = fails;
    if (!d) { verdict("-", "no-float", 0, "cannot open ", dir); return; }
    while ((de = readdir(d))) {
        size_t l = strlen(de->d_name);
        char path[512], *t, *p;
        unsigned line = 1;
        if (l < 3 || (strcmp(de->d_name + l - 2, ".c") && strcmp(de->d_name + l - 2, ".h"))) continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (!(t = tx_read_file(path))) continue;
        files++;
        strip(t);
        for (p = t; *p; p++) {
            if (*p == '\n') { line++; continue; }
            if ((p == t || !(isalnum((unsigned char)p[-1]) || p[-1] == '_'))
                && ((!strncmp(p, "float", 5) && !(isalnum((unsigned char)p[5]) || p[5] == '_'))
                    || (!strncmp(p, "double", 6) && !(isalnum((unsigned char)p[6]) || p[6] == '_')))) {
                char m[600]; snprintf(m, sizeof m, "%s:%u: float type", path, line); verdict("-", "no-float", 0, m, 0);
            }
            if (isdigit((unsigned char)*p) && (p == t || !(isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '.'))) {
                char *q = p;
                if (q[0] == '0' && (q[1] == 'x' || q[1] == 'X')) { p = q + 1; while (isxdigit((unsigned char)p[1])) p++; continue; }
                while (isdigit((unsigned char)*q)) q++;
                if (*q == '.' || ((*q == 'e' || *q == 'E') && (isdigit((unsigned char)q[1]) || q[1] == '-' || q[1] == '+'))) {
                    char m[600]; snprintf(m, sizeof m, "%s:%u: floating literal", path, line); verdict("-", "no-float", 0, m, 0);
                }
                p = q - 1;
            }
        }
        free(t);
    }
    closedir(d);
    {
        char m[600]; snprintf(m, sizeof m, "%u files in %s/ (comments stripped)", files, dir);
        if (fails == f0) verdict("-", "no-float", 1, m, 0);
    }
}

/* ---- S3: four engines at their worst cases (max scenes at the default PARTICLES) together through the MIX core:
 * panned apart, summed, the CHAMBER (on or off), the linked limiter. The limited pair never passes -1 dBFS, the raw
 * pair keeps int32 headroom, and the render is deterministic (baseline "layers4-max" / "layers4+chamber-max"). */
static const char *const LAYERS4[4] = { "plate", "storm", "mineral", "spring" };

static int run_layers(int chamber_on, char hex[65], int32_t *raw_peak, int32_t *lim_peak, uint32_t *lim_blocks)
{
    tx_mix_t *m = malloc(sizeof *m);
    tx_chamber_t *ch = malloc(sizeof *ch);
    tx_script_t *s[4];
    void *st[4];
    unsigned next[4] = {0, 0, 0, 0}, k, i, j;
    uint32_t blocks = 0, b;
    int32_t blk[TX_BLOCK], L[TX_BLOCK], R[TX_BLOCK];
    tx_sha256_t sha;
    static const unsigned PAN[4] = {20, 50, 80, 110};
    tx_mix_init(m);
    tx_chamber_init(ch);
    m->ch = ch; m->ch_on = chamber_on;
    for (k = 0; k < 4; k++) {
        const tx_engine_t *e = tx_engine_find(LAYERS4[k]);
        s[k] = malloc(sizeof *s[k]);
        if (!e || tx_scene_load(s[k], e, "max")) { for (i = 0; i <= k; i++) free(s[i]); free(m); free(ch); return -1; }
        for (i = 0; i < s[k]->n; i++)
            if (s[k]->ev[i].kind == TX_SEV_PARAM && s[k]->ev[i].id == TX_PID_PARTICLES) s[k]->ev[i].a = e->particles_def;
        st[k] = tx_state_new(e);
        tx_mix_set(m, k, e, st[k], s[k]->seed);
        e->param(st[k], TX_PID_PARTICLES, e->particles_def);
        tx_mix_pan(m, k, PAN[k]);
        if (s[k]->blocks > blocks) blocks = s[k]->blocks;
    }
    tx_sha256_init(&sha);
    *raw_peak = *lim_peak = 0; *lim_blocks = 0;
    for (b = 0; b < blocks; b++) {
        tx_mix_begin(m, TX_BLOCK);
        for (k = 0; k < 4; k++) {
            tx_script_step(m->e[k], st[k], s[k], b, &next[k], blk);
            tx_mix_add(m, k, blk, TX_BLOCK);
        }
        tx_mix_end(m, L, R, TX_BLOCK);
        if (m->raw_peak > *raw_peak) *raw_peak = m->raw_peak;
        for (j = 0; j < TX_BLOCK; j++) {
            int32_t a = L[j] < 0 ? -L[j] : L[j], c = R[j] < 0 ? -R[j] : R[j];
            if (a > *lim_peak) *lim_peak = a;
            if (c > *lim_peak) *lim_peak = c;
        }
        if (tx_limit_take(&m->lim)) (*lim_blocks)++;
        tx_sha256_q23(&sha, L, TX_BLOCK);
        tx_sha256_q23(&sha, R, TX_BLOCK);
    }
    tx_sha256_hex(&sha, hex);
    for (k = 0; k < 4; k++) { free(s[k]); free(st[k]); }
    free(m); free(ch);
    return (int)blocks;
}

static void check_layers(FILE *update)
{
    unsigned c;
    for (c = 0; c < 2; c++) {
        char hex[65], hex2[65], msg[240], name[48];
        int32_t rp, lp, rp2, lp2;
        uint32_t lb, lb2;
        int nb = run_layers((int)c, hex, &rp, &lp, &lb);
        const char *bl;
        snprintf(name, sizeof name, "layers4%s-max", c ? "+chamber" : "");
        if (nb < 0 || run_layers((int)c, hex2, &rp2, &lp2, &lb2) < 0) { verdict("layers", "load", 0, name, " does not load"); continue; }
        snprintf(msg, sizeof msg, "%s+%s+%s+%s at their worst%s: raw pair peak %.1f dBFS (int32 headroom +48), limited %.2f dBFS "
                 "(<= -1), the limiter in %u of %d blocks", LAYERS4[0], LAYERS4[1], LAYERS4[2], LAYERS4[3],
                 c ? " + CHAMBER" : "", rp ? 20.0 * log10(rp / 8388608.0) : -999.0, lp ? 20.0 * log10(lp / 8388608.0) : -999.0, lb, nb);
        verdict("layers", c ? "chamber" : "mix", rp < (1 << 29) && lp <= 7476717, msg, 0);
        if (strcmp(hex, hex2)) { verdict("layers", "determinism", 0, name, ": two renders differ"); continue; }
        if (update) { fprintf(update, "%s  %s\n", hex, name); continue; }
        bl = baseline_get(name);
        snprintf(msg, sizeof msg, "%s %.16s %s", name, hex, bl ? (strcmp(bl, hex) ? "!= baseline (a change moved the sound)" : "= baseline")
                 : "(no baseline: make tx-baseline)");
        verdict("layers", "determinism", bl && !strcmp(bl, hex), msg, 0);
    }
}

/* keys: the SMK-37's whole keybed (C3..C6, notes 48..84; the FM-1's own keys are 53..79) at velocity 1 and 127 (0 is
 * the release, sent after every press), on every engine at its defaults: each note pressed for 2 blocks, released,
 * the next one at once; no overflow (UBSan, peak under 2^27 as the overflow check) and quiet within 30 s after the
 * last release. */
static void check_keyrange(void)
{
    static const uint8_t VEL[2] = {1, 127};
    unsigned k;
    for (k = 0; tx_engines[k]; k++) {
        const tx_engine_t *e = tx_engines[k];
        void *st = tx_state_new(e);
        int32_t mix[TX_BLOCK], peak = 0;
        tx_stats_t s;
        unsigned v, i, b, j, quiet = 0;
        char msg[160];
        if (!st) continue;
        e->init(st, 5);
        for (v = 0; v < 2; v++)
            for (i = 0; i < 37; i++) {
                uint8_t note = (uint8_t)(48 + i);           /* C3 .. C6 */
                e->key(st, note, VEL[v], 1);
                for (b = 0; b < 3; b++) {
                    if (b == 2) e->key(st, note, 0, 0);
                    for (j = 0; j < TX_BLOCK; j++) mix[j] = 0;
                    e->render(st, mix, TX_BLOCK);
                    for (j = 0; j < TX_BLOCK; j++) { int32_t x = mix[j] < 0 ? -mix[j] : mix[j]; if (x > peak) peak = x; }
                }
            }
        for (b = 0; b < 5168u && !quiet; b++) {             /* 30 s */
            for (j = 0; j < TX_BLOCK; j++) mix[j] = 0;
            e->render(st, mix, TX_BLOCK);
            for (j = 0; j < TX_BLOCK; j++) { int32_t x = mix[j] < 0 ? -mix[j] : mix[j]; if (x > peak) peak = x; }
            e->stats(st, &s);
            quiet = s.quiet ? b + 1 : 0;
        }
        snprintf(msg, sizeof msg, "notes 48..84 at v1 / v127: peak %.1f dBFS, %s", 20 * log10((peak + 1) / 8388608.0),
                 quiet ? "quiet after the last release" : "NOT quiet 30 s after the last release");
        verdict(e->name, "keyrange", peak < (1 << 27) && quiet, msg, 0);
        free(st);
    }
}

/* ---- the effects (tx_fx.h): off = the signal untouched; silence in = exact silence out with every effect on; each
 * reverb TYPE's tail ends in exact zeros (no DC left, nothing drifting); FREEZE holds its level for minutes; renders
 * against the baselines (fx-*). Input: a deterministic burst (noise and a decaying 220 Hz tone, -12 dBFS, 0.5 s) */
static int32_t fx_in(uint32_t i, uint32_t *rng)
{
    int32_t n, t;
    if (i >= 22050u) return 0;
    *rng = *rng * 1664525u + 1013904223u;
    n = (int32_t)(*rng >> 9) - (1 << 22);                                  /* +-0.5 */
    t = tx_sin_q15(i * 21426130u) * 128;                                  /* 220 Hz, 0.5 of full scale */
    return (int32_t)(((int64_t)((n >> 1) + t) * (int32_t)(22050u - i)) / 22050) >> 1;
}
static void fx_hash(tx_sha256_t *h, const int32_t *x, unsigned n) { tx_sha256_q23(h, x, n); }
/* one reverb run: TYPE t, DECAY d, MIX 90, the burst, then up to secs s; returns the block the tail ended in (0: never),
 * the peak, the hash of the first 10 s; rms[] per 10 s window (if rms) */
static uint32_t fx_run_rev(unsigned t, unsigned d, unsigned secs, int zero, int32_t *peak, char hex[65], double *rms,
                           unsigned nrms)
{
    tx_fx_reverb_t *r = malloc(sizeof *r);
    int32_t L[TX_BLOCK], R[TX_BLOCK];
    uint32_t b, j, nb = secs * 44100u / TX_BLOCK, end = 0, rng = 7u;
    double acc = 0;
    tx_sha256_t h;
    tx_sha256_init(&h);
    *peak = 0;
    tx_fx_reverb_init(r);
    tx_fx_reverb_param(r, TX_RV_TYPE, t); tx_fx_reverb_param(r, TX_RV_MIX, 90);
    tx_fx_reverb_param(r, TX_RV_DECAY, d < 127 ? d : 100);                 /* FREEZE: played into, then frozen at 1 s */
    for (b = 0; b < nb; b++) {
        if (b == 172u) tx_fx_reverb_param(r, TX_RV_DECAY, d);
        for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = zero ? 0 : fx_in(b * TX_BLOCK + j, &rng);
        tx_fx_reverb_process(r, L, R, TX_BLOCK);
        for (j = 0; j < TX_BLOCK; j++) {
            int32_t a = L[j] < 0 ? -L[j] : L[j], c = R[j] < 0 ? -R[j] : R[j];
            if (a > *peak) *peak = a;
            if (c > *peak) *peak = c;
            acc += (double)L[j] * L[j] + (double)R[j] * R[j];
        }
        if (b < 1723u) fx_hash(&h, L, TX_BLOCK), fx_hash(&h, R, TX_BLOCK);   /* 10 s */
        if (rms && (b + 1) % 1723u == 0 && (b + 1) / 1723u - 1 < nrms) { rms[(b + 1) / 1723u - 1] = sqrt(acc / (1723.0 * 512)); acc = 0; }
        else if ((b + 1) % 1723u == 0) acc = 0;
        if (!rms && b > 90u) {                                    /* the tail's end: exact zeros from here on */
            unsigned z = 1;
            for (j = 0; j < TX_BLOCK; j++) z &= !L[j] && !R[j];
            if (!z) end = 0;
            else if (!end) end = b;
            if (end && b > end + 344u && b >= 1723u) break;     /* 2 s of exact zeros (and the 10 s hashed) */
        }
    }
    tx_sha256_hex(&h, hex);
    free(r);
    return end;
}
static void check_fx(FILE *update)
{
    static const char *const TN[TX_RVT_N] = {"off", "room", "hall", "infinite", "dark", "drift", "swell", "plate"};
    tx_fx_t *fx = malloc(sizeof *fx);
    char msg[200], hex[65], hex2[65], name[48];
    int32_t x[TX_BLOCK], y[TX_BLOCK], L[TX_BLOCK], R[TX_BLOCK], pk, pk2;
    uint32_t b, j, rng = 7u, bad = 0, nz = 0;
    unsigned t;
    if (!fx) return;
    /* off: the defaults change no sample (lanes and bus) */
    tx_fx_init(fx);
    for (b = 0; b < 200u; b++) {
        for (j = 0; j < TX_BLOCK; j++) x[j] = y[j] = L[j] = fx_in(b * TX_BLOCK + j, &rng) * 3, R[j] = -L[j];
        tx_fx_lane(fx, 0, y, TX_BLOCK);
        tx_fx_bus(fx, L, R, TX_BLOCK);
        for (j = 0; j < TX_BLOCK; j++) bad += y[j] != x[j] || L[j] != x[j] || R[j] != -x[j];
    }
    snprintf(msg, sizeof msg, "every effect at its default (off): %u samples changed", bad);
    verdict("fx", "passthrough", !bad, msg, 0);
    /* silence in, every effect on: exact zeros out */
    tx_fx_init(fx);
    tx_fx_drive_param(&fx->drive[0], TX_DR_AMOUNT, 90);
    tx_fx_filter_param(&fx->filter[0], TX_FL_TYPE, 2); tx_fx_filter_param(&fx->filter[0], TX_FL_FREQ, 60);
    tx_fx_filter_param(&fx->filter[0], TX_FL_RESO, 90); tx_fx_filter_param(&fx->filter[0], TX_FL_LFO, 80);
    for (t = TX_RVT_ROOM; t < TX_RVT_N; t++) {
        tx_fx_reverb_init(&fx->rev);
        tx_fx_reverb_param(&fx->rev, TX_RV_TYPE, t); tx_fx_reverb_param(&fx->rev, TX_RV_MIX, 127);
        tx_fx_reverb_param(&fx->rev, TX_RV_PREDELAY, 60); tx_fx_reverb_param(&fx->rev, TX_RV_LOWCUT, 40);
        tx_fx_reverb_param(&fx->rev, TX_RV_SHIMMER, 100);
        tx_fx_grain_param(&fx->gd, TX_GD_MIX, 100); tx_fx_grain_param(&fx->gd, TX_GD_PITCH, 75);
        for (b = 0; b < 400u; b++) {
            for (j = 0; j < TX_BLOCK; j++) y[j] = L[j] = R[j] = 0;
            tx_fx_lane(fx, 0, y, TX_BLOCK);
            tx_fx_bus(fx, L, R, TX_BLOCK);
            for (j = 0; j < TX_BLOCK; j++) nz += y[j] != 0 || L[j] != 0 || R[j] != 0;
        }
    }
    snprintf(msg, sizeof msg, "zeros in, drive + filter (LP18, LFO) + grain delay + every reverb TYPE with shimmer: %u nonzero samples out", nz);
    verdict("fx", "silence", !nz, msg, 0);
    /* each TYPE: the burst, its tail ends in exact zeros; renders against the baselines */
    for (t = TX_RVT_ROOM; t < TX_RVT_N; t++) {
        const char *bl;
        uint32_t end = fx_run_rev(t, 64, 120, 0, &pk, hex, 0, 0);
        fx_run_rev(t, 64, 11, 0, &pk2, hex2, 0, 0);
        snprintf(msg, sizeof msg, "%s, DECAY 64: the tail ends in exact zeros after %.1f s, peak %.1f dBFS", TN[t],
                 end * 256.0 / 44100.0, 20 * log10((pk + 1) / 8388608.0));
        verdict("fx", "tail", end && pk < (1 << 27), msg, end ? 0 : " (never within 120 s)");
        snprintf(name, sizeof name, "fx-%s", TN[t]);
        if (strcmp(hex, hex2)) { verdict("fx", "determinism", 0, name, ": two renders differ"); continue; }
        if (update) { fprintf(update, "%s  %s\n", hex, name); continue; }
        bl = baseline_get(name);
        snprintf(msg, sizeof msg, "%s %.16s", name, hex);
        verdict("fx", "determinism", bl && !strcmp(bl, hex), msg,
                !bl ? " (no baseline: make tx-baseline)" : strcmp(bl, hex) ? " != baseline (a change moved the sound)" : " = baseline");
    }
    /* FREEZE (DECAY 127) for 3 minutes: the held level stays within 1.5 dB, never silent, bounded */
    for (t = TX_RVT_INF; t <= TX_RVT_SWELL; t++) {
        double rms[18] = {0}, lo = 1e30, hi = 0;
        unsigned k;
        fx_run_rev(t, 127, 181, 0, &pk, hex, rms, 18);            /* 18 windows of 10 s: 0 .. 180 s */
        double lo2 = 1e30, hi2 = 0;
        for (k = 1; k < 18; k++) { if (rms[k] < lo) lo = rms[k]; if (rms[k] > hi) hi = rms[k]; }
        for (k = 6; k < 18; k++) { if (rms[k] < lo2) lo2 = rms[k]; if (rms[k] > hi2) hi2 = rms[k]; }
        snprintf(msg, sizeof msg, "%s FREEZE: 10 .. 180 s %.1f .. %.1f dBFS (%.2f dB apart; settled 60 .. 180 s: %.2f dB), peak %.1f dBFS",
                 TN[t], 20 * log10(lo / 8388608.0 + 1e-12), 20 * log10(hi / 8388608.0 + 1e-12), 20 * log10(hi / (lo + 1e-9)),
                 20 * log10(hi2 / (lo2 + 1e-9)), 20 * log10((pk + 1) / 8388608.0));
        /* (the held level settles over the first minute as the top fades: the least damping, by design; then it
         * holds) never silent, within 3 dB over 3 minutes (DRIFT 3.5: its random bends beat wider than the sines), 2 dB
         * once settled (its slow beating), bounded */
        verdict("fx", "freeze", lo > 0 && hi / lo < (t == TX_RVT_DRIFT ? 1.496 : 1.413) && hi2 / lo2 < 1.259 && pk < (1 << 23), msg, 0);
    }
    /* PLATE starts in O(1) over another TYPE's state: started over garbage = started over zeros */
    {
        tx_fx_reverb_t *ra = malloc(sizeof *ra), *rb = malloc(sizeof *rb);
        uint32_t diff = 0, rng1 = 7u, nzs = 0;
        if (ra && rb) {
            tx_fx_reverb_init(ra); tx_fx_reverb_init(rb);
            memset(&ra->u, 0, sizeof ra->u); memset(&rb->u, 0xA5, sizeof rb->u);
            tx_fx_reverb_param(ra, TX_RV_TYPE, TX_RVT_PLATE); tx_fx_reverb_param(ra, TX_RV_MIX, 90);
            tx_fx_reverb_param(rb, TX_RV_TYPE, TX_RVT_PLATE); tx_fx_reverb_param(rb, TX_RV_MIX, 90);
            for (b = 0; b < 1034u; b++) {
                for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = x[j] = y[j] = fx_in(b * TX_BLOCK + j, &rng1);
                tx_fx_reverb_process(ra, L, R, TX_BLOCK);
                tx_fx_reverb_process(rb, x, y, TX_BLOCK);
                for (j = 0; j < TX_BLOCK; j++) diff += L[j] != x[j] || R[j] != y[j], nzs += L[j] != 0;
            }
        }
        snprintf(msg, sizeof msg, "plate started over garbage = over zeros: %u of 6 s's samples differ (%u nonzero)", diff, nzs);
        verdict("fx", "plate", ra && rb && !diff && nzs, msg, 0);
        free(ra); free(rb);
    }
    /* SWELL: 3 s played (the burst again and again), then silence: the wet part (out - in) after the playing against
     * during it, SWELL against INFINITE (DECAY 100): the tail held down while played, swelling in after */
    {
        double ratio[2];
        for (t = 0; t < 2; t++) {
            tx_fx_reverb_t *r = &fx->rev;
            double dur = 0, aft = 0, acc = 0;
            uint32_t rngs = 7u;
            tx_fx_reverb_init(r);
            tx_fx_reverb_param(r, TX_RV_TYPE, t ? TX_RVT_SWELL : TX_RVT_INF); tx_fx_reverb_param(r, TX_RV_MIX, 90);
            tx_fx_reverb_param(r, TX_RV_DECAY, 100);
            for (b = 0; b < 1034u; b++) {                       /* 6 s: 3 played, 3 after; 0.25 s windows (43 blocks) */
                for (j = 0; j < TX_BLOCK; j++) x[j] = L[j] = R[j] = b < 517u ? fx_in((b * TX_BLOCK + j) % 22050u, &rngs) : 0;
                tx_fx_reverb_process(r, L, R, TX_BLOCK);
                for (j = 0; j < TX_BLOCK; j++) acc += (double)(L[j] - x[j]) * (L[j] - x[j]);
                if ((b + 1u) % 43u == 0) {
                    double v = sqrt(acc / (43.0 * TX_BLOCK));
                    if (b >= 344u && b < 516u) dur = v > dur ? v : dur;   /* 2 .. 3 s: played */
                    if (b >= 559u && v > aft) aft = v;                      /* 3.25 .. 6 s: after */
                    acc = 0;
                }
            }
            ratio[t] = 20 * log10((aft + 1e-9) / (dur + 1e-9));
        }
        snprintf(msg, sizeof msg, "the tail after the playing against during it: SWELL %+.1f dB, INFINITE %+.1f dB", ratio[1], ratio[0]);
        verdict("fx", "swell", ratio[1] > 6.0 && ratio[1] > ratio[0] + 10.0, msg, 0);
    }
    /* SHIMMER: a HALL with it ends in exact zeros; an INFINITE SUSTAIN with it at its top stays bounded for a minute */
    for (t = 0; t < 2; t++) {
        tx_fx_reverb_t *r = &fx->rev;
        uint32_t last = 0, rngs = 7u;
        int32_t pk3 = 0;
        double a3 = 0;
        tx_fx_init(fx);
        tx_fx_reverb_param(r, TX_RV_TYPE, t ? TX_RVT_INF : TX_RVT_HALL); tx_fx_reverb_param(r, TX_RV_MIX, 90);
        tx_fx_reverb_param(r, TX_RV_DECAY, t ? 124 : 64); tx_fx_reverb_param(r, TX_RV_SHIMMER, t ? 127 : 100);
        for (b = 0; b < 172u * 60u; b++) {
            unsigned z = 0;
            for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = fx_in(b * TX_BLOCK + j, &rngs);
            tx_fx_bus(fx, L, R, TX_BLOCK);
            for (j = 0; j < TX_BLOCK; j++) { int32_t a = L[j] < 0 ? -L[j] : L[j]; z |= L[j] | R[j]; if (a > pk3) pk3 = a; if (b >= 172u * 50u) a3 += (double)L[j] * L[j]; }
            if (z) last = b;
        }
        if (!t) {
            snprintf(msg, sizeof msg, "HALL + SHIMMER 100: the tail ends in exact zeros after %.1f s, peak %.1f dBFS", last * 256.0 / 44100.0,
                     20 * log10((pk3 + 1) / 8388608.0));
            verdict("fx", "shimmer", last < 172u * 55u && pk3 < (1 << 23), msg, 0);
        } else {
            double rm = sqrt(a3 / (1720.0 * 256));
            snprintf(msg, sizeof msg, "INFINITE SUSTAIN + SHIMMER 127 for 60 s: peak %.1f dBFS, the last 10 s %.1f dBFS (bounded, sounding)",
                     20 * log10((pk3 + 1) / 8388608.0), 20 * log10(rm / 8388608.0 + 1e-12));
            verdict("fx", "shimmer", pk3 < (1 << 23) && rm > 0, msg, 0);
        }
    }
    /* the GRAIN DELAY: the echo exact at TIME; a grained tail (FEEDBACK 90, pitched, some reversed) ends in exact zeros;
     * FREEZE holds its loop; the dynamic cap under the reverb; a render against the baseline */
    {
        uint32_t tm = TX_FX_GD_TIME[tx_fx_grain_params[TX_GD_TIME].def], at = 0, last = 0, maxact = 0;
        int32_t got = 0;
        double rms[12] = {0}, acc = 0, lo = 1e30, hi = 0;
        unsigned k;
        tx_sha256_t h;
        const char *bl;
        tx_fx_init(fx);
        tx_fx_grain_param(&fx->gd, TX_GD_MIX, 127); tx_fx_grain_param(&fx->gd, TX_GD_GRAIN, 0);
        tx_fx_grain_param(&fx->gd, TX_GD_FEEDBACK, 0);
        for (b = 0; b < 200u; b++) {
            for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = (b == 2u && j == 10u) ? 4194304 : 0;
            tx_fx_bus(fx, L, R, TX_BLOCK);
            for (j = 0; j < TX_BLOCK; j++) if (!at && L[j] && !(b == 2u && j == 10u)) { at = b * TX_BLOCK + j; got = L[j]; }
        }
        snprintf(msg, sizeof msg, "grain delay, GRAIN 0: an impulse back after %u samples (TIME %u), level %d / 4194304", at - 522u, tm, got);
        verdict("fx", "echo", at - 522u == tm && got > 4190000 && got <= 4194304, msg, 0);
        for (t = 0; t < 2; t++) {                              /* 0: the tail; 1: FREEZE at 1 s, held */
            tx_fx_init(fx);
            tx_fx_grain_param(&fx->gd, TX_GD_MIX, 100); tx_fx_grain_param(&fx->gd, TX_GD_FEEDBACK, 90);
            tx_fx_grain_param(&fx->gd, TX_GD_PITCH, 71); tx_fx_grain_param(&fx->gd, TX_GD_REVERSE, 40);
            tx_fx_grain_param(&fx->gd, TX_GD_SPRAY, 40);
            rng = 7u; last = 0; acc = 0;
            if (!t) tx_sha256_init(&h);
            for (b = 0; b < (t ? 172u * 121u : 172u * 60u); b++) {
                unsigned z = 0;
                if (t && b == 172u) tx_fx_grain_param(&fx->gd, TX_GD_FREEZE, 1);
                for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = fx_in(b * TX_BLOCK + j, &rng);
                tx_fx_bus(fx, L, R, TX_BLOCK);
                for (j = 0; j < TX_BLOCK; j++) { z |= L[j] | R[j]; acc += (double)L[j] * L[j]; }
                if (z) last = b;
                if (!t && b < 1723u) fx_hash(&h, L, TX_BLOCK), fx_hash(&h, R, TX_BLOCK);
                if (t && (b + 1) % 1720u == 0 && (b + 1) / 1720u - 1 < 12u) { rms[(b + 1) / 1720u - 1] = sqrt(acc / (1720.0 * 256)); acc = 0; }
                else if ((b + 1) % 1720u == 0) acc = 0;
            }
            if (!t) {
                snprintf(msg, sizeof msg, "grain delay, FEEDBACK 90, +7 semitones, 30 %% reversed: the tail ends in exact zeros after %.1f s", last * 256.0 / 44100.0);
                verdict("fx", "tail", last < 172u * 50u, msg, 0);
                tx_sha256_hex(&h, hex);
                if (update) fprintf(update, "%s  %s\n", hex, "fx-graindelay");
                else {
                    bl = baseline_get("fx-graindelay");
                    snprintf(msg, sizeof msg, "fx-graindelay %.16s", hex);
                    verdict("fx", "determinism", bl && !strcmp(bl, hex), msg,
                            !bl ? " (no baseline: make tx-baseline)" : strcmp(bl, hex) ? " != baseline (a change moved the sound)" : " = baseline");
                }
            } else {
                for (k = 1; k < 12; k++) { if (rms[k] < lo) lo = rms[k]; if (rms[k] > hi) hi = rms[k]; }
                snprintf(msg, sizeof msg, "grain delay FREEZE: 10 .. 120 s %.1f .. %.1f dBFS (%.2f dB apart)",
                         20 * log10(lo / 8388608.0 + 1e-12), 20 * log10(hi / 8388608.0 + 1e-12), 20 * log10(hi / (lo + 1e-9)));
                verdict("fx", "freeze", lo > 0 && hi / lo < 1.189, msg, 0);
            }
        }
        tx_fx_init(fx);                                    /* the cap: 16 pitched grains asked, INFINITE on */
        tx_fx_grain_param(&fx->gd, TX_GD_MIX, 100); tx_fx_grain_param(&fx->gd, TX_GD_GRAINS, 15);
        tx_fx_grain_param(&fx->gd, TX_GD_PITCH, 76);
        tx_fx_reverb_param(&fx->rev, TX_RV_TYPE, TX_RVT_INF); tx_fx_reverb_param(&fx->rev, TX_RV_MIX, 60);
        rng = 7u;
        for (b = 0; b < 400u; b++) {
            for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = fx_in((b % 100u) * TX_BLOCK + j, &rng);
            tx_fx_bus(fx, L, R, TX_BLOCK);
            if (b > 100u && fx->gd.act > maxact) maxact = fx->gd.act;
        }
        snprintf(msg, sizeof msg, "grain delay, 16 pitched grains asked with INFINITE on: at most %u sounding (cap %u)", maxact, fx->gd.cap);
        verdict("fx", "cap", maxact <= fx->gd.cap && fx->gd.cap < 16u && maxact > 0, msg, 0);
    }
    /* the DRIVE TYPEs: zeros in, zeros out; AMOUNT 0 glides out to the lane untouched; a TYPE change while it
     * sounds fades through (no step); TSH8 on one lane at a time */
    {
        static const char *const DN[TX_DRT_N] = {"FDRV", "KHLON", "TSH8", "OHCD", "BHLUE"};
        unsigned nzt = 0, dry = 0, own = 0;
        int32_t step = 0, pv = 0;
        for (t = 1; t < TX_DRT_N; t++) {
            tx_fx_init(fx);
            tx_fx_drive_param(&fx->drive[0], TX_DR_TYPE, t); tx_fx_drive_param(&fx->drive[0], TX_DR_AMOUNT, 127);
            for (b = 0; b < 100u; b++) {
                for (j = 0; j < TX_BLOCK; j++) y[j] = 0;
                tx_fx_lane(fx, 0, y, TX_BLOCK);
                for (j = 0; j < TX_BLOCK; j++) nzt += y[j] != 0;
            }
            rng = 5u;
            for (b = 0; b < 300u; b++) {                 /* on, then AMOUNT 0 at 100: dry again after its glide */
                if (b == 100u) tx_fx_drive_param(&fx->drive[0], TX_DR_AMOUNT, 0);
                for (j = 0; j < TX_BLOCK; j++) x[j] = y[j] = fx_in(b * TX_BLOCK + j, &rng) * 4;
                tx_fx_lane(fx, 0, y, TX_BLOCK);
                if (b > 110u) for (j = 0; j < TX_BLOCK; j++) dry += y[j] != x[j];
            }
        }
        snprintf(msg, sizeof msg, "DRIVE TYPEs %s .. %s at AMOUNT 127: zeros in, %u nonzero out; AMOUNT 0: %u samples "
                 "not dry after the glide", DN[1], DN[TX_DRT_N - 1], nzt, dry);
        verdict("fx", "drive-types", !nzt && !dry, msg, 0);
        tx_fx_init(fx);                                  /* a sine through every TYPE in turn, switched while it sounds */
        tx_fx_drive_param(&fx->drive[0], TX_DR_AMOUNT, 100);
        for (b = 0; b < 1200u; b++) {
            if (b % 200u == 100u) tx_fx_drive_param(&fx->drive[0], TX_DR_TYPE, (b / 200u + 1u) % TX_DRT_N);
            for (j = 0; j < TX_BLOCK; j++) y[j] = (int32_t)(sin((b * TX_BLOCK + j) * 0.0142) * 2000000.0);
            tx_fx_lane(fx, 0, y, TX_BLOCK);
            for (j = 0; j < TX_BLOCK; j++) { int32_t d = y[j] - pv; d = d < 0 ? -d : d; if ((b || j) && d > step) step = d; pv = y[j]; }
        }
        snprintf(msg, sizeof msg, "DRIVE TYPE changed while it sounds (a 100 Hz sine at -12 dBFS): the largest step "
                 "%.2f %% of full scale (the sine's own: 0.34 %%)", 100.0 * step / 8388608.0);
        verdict("fx", "drive-switch", step < 8388608 / 66, msg, 0);
        tx_fx_init(fx);                                  /* TSH8: lane 0 has it; lane 1 keeps FDRV until lane 0 leaves */
        tx_fx_drive_param(&fx->drive[0], TX_DR_AMOUNT, 80); tx_fx_drive_param(&fx->drive[0], TX_DR_TYPE, TX_DRT_TSH8);
        tx_fx_drive_param(&fx->drive[1], TX_DR_AMOUNT, 80); tx_fx_drive_param(&fx->drive[1], TX_DR_TYPE, TX_DRT_TSH8);
        for (b = 0; b < 60u; b++) {
            unsigned k;
            if (b == 30u) tx_fx_drive_param(&fx->drive[0], TX_DR_TYPE, TX_DRT_OHCD);
            for (k = 0; k < 2u; k++) {
                for (j = 0; j < TX_BLOCK; j++) y[j] = fx_in(b * TX_BLOCK + j, &rng) * 4;
                tx_fx_lane(fx, k, y, TX_BLOCK);
            }
            if (b == 20u) own += fx->drive[0].cur == TX_DRT_TSH8 && fx->drive[1].cur == TX_DRT_FDRV;
        }
        own += fx->drive[0].cur == TX_DRT_OHCD && fx->drive[1].cur == TX_DRT_TSH8 && fx->drv.ts_own == &fx->drive[1];
        snprintf(msg, sizeof msg, "TSH8 asked on two lanes: the first has it, the second keeps FDRV, then takes it when the "
                 "first leaves (%u of 2)", own);
        verdict("fx", "drive-tsh8", own == 2u, msg, 0);
    }
    /* the lane inserts: renders against the baselines (a drive at 90 / TONE 100 / MIX 100, the classic and each TYPE;
     * LP18 with its LFO) */
    for (t = 0; t < 2u + TX_DRT_N - 1u; t++) {
        tx_sha256_t h, h2;
        const char *bl;
        int pass;
        for (pass = 0; pass < 2; pass++) {
            tx_fx_init(fx);
            if (t != 1u) { tx_fx_drive_param(&fx->drive[1], TX_DR_AMOUNT, 90); tx_fx_drive_param(&fx->drive[1], TX_DR_TONE, 100);
                           tx_fx_drive_param(&fx->drive[1], TX_DR_MIX, 100); tx_fx_drive_param(&fx->drive[1], TX_DR_TYPE, t ? t - 1u : 0u); }
            else { tx_fx_filter_param(&fx->filter[1], TX_FL_TYPE, 2); tx_fx_filter_param(&fx->filter[1], TX_FL_FREQ, 70);
                   tx_fx_filter_param(&fx->filter[1], TX_FL_RESO, 100); tx_fx_filter_param(&fx->filter[1], TX_FL_LFO, 90); }
            tx_sha256_init(pass ? &h2 : &h);
            rng = 7u; pk = 0;
            for (b = 0; b < 400u; b++) {
                for (j = 0; j < TX_BLOCK; j++) y[j] = fx_in((b % 200u) * TX_BLOCK + j, &rng) * 4;
                tx_fx_lane(fx, 1, y, TX_BLOCK);
                for (j = 0; j < TX_BLOCK; j++) { int32_t a = y[j] < 0 ? -y[j] : y[j]; if (a > pk) pk = a; }
                fx_hash(pass ? &h2 : &h, y, TX_BLOCK);
            }
        }
        tx_sha256_hex(&h, hex); tx_sha256_hex(&h2, hex2);
        {
            static const char *const FN[2u + TX_DRT_N - 1u] = {"drive", "filter", "khlon", "tsh8", "ohcd", "bhlue"};
            snprintf(name, sizeof name, "fx-%s", FN[t]);
        }
        if (strcmp(hex, hex2)) { verdict("fx", "determinism", 0, name, ": two renders differ"); continue; }
        if (update) { fprintf(update, "%s  %s\n", hex, name); continue; }
        bl = baseline_get(name);
        snprintf(msg, sizeof msg, "%s %.16s (peak %.1f dBFS)", name, hex, 20 * log10((pk + 1) / 8388608.0));
        verdict("fx", "determinism", bl && !strcmp(bl, hex) && pk < (1 << 27), msg,
                !bl ? " (no baseline: make tx-baseline)" : strcmp(bl, hex) ? " != baseline (a change moved the sound)" : " = baseline");
    }
    free(fx);
}

int main(int argc, char **argv)
{
    unsigned k;
    FILE *update = 0;
    if (argc > 2 && !strcmp(argv[1], "--src")) {            /* the source check alone, on another folder */
        check_sources(argv[2]);
        return fails ? 1 : 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--update")) {
        if (!(update = fopen(BASELINE, "w"))) { perror(BASELINE); return 2; }
    } else
        baseline_load();
    check_sources("engines");
    for (k = 0; tx_engines[k]; k++) check_engine(tx_engines[k], update);
    if (!update) for (k = 0; tx_engines[k]; k++) check_variation(tx_engines[k]);
    check_chamber(update);
    check_layers(update);
    check_fx(update);
    check_noise_core();
    if (!update) { check_gen(); check_gov(); check_switch(); check_keyrange(); }
    if (update) { fclose(update); printf("wrote %s\n", BASELINE); }
    printf(fails ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", fails);
    return fails ? 1 : 0;
}
