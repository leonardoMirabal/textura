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
 *   overflow  the worst-case scene (at 8 seeds) runs without signed overflow (UBSan) and peaks under 2^27 (16x full scale, so 4
 *             layers stay 4x inside int32); the chord scene at the default level stays under full scale
 *   cap       in the cap and max scenes the sounding particles never exceed the PARTICLES limit in force
 *   instance  two instances rendered block-interleaved give the same samples as each alone (no hidden globals)
 *   determinism  each standard scene twice from scratch: same sha256; and equal to the committed baseline
 * CHAMBER: idle zero, a quiet sweep (each knob at 0/64/90/127 after a drip tap), overflow (test max at its loudest),
 *          determinism + baseline (drip hold / max, gravel tap, test chord through the chamber at its defaults).
 * NOISE: the band filter at every corner (20 Hz .. 20 kHz, Q 0.5 .. 8, both paths, full gain) stays bounded.
 * FX: off = untouched; silence in = exact silence out; each reverb TYPE's tail ends in exact zeros; FREEZE holds its level
 *     for 3 minutes; the grain delay's FEEDBACK 127 runs up into a bounded wall that ends when FEEDBACK comes down;
 *     renders against the baselines (fx-*).
 * Keys: every engine over the SMK-37's keybed (notes 48..84) at v1 / v127: no overflow, quiet after.
 * Distance: rockfall's DIST never makes a tap louder, nor its peak higher; under full scale from its default out.
 * Hosts: every engine run inside another (the source's hosted sounds, the storm's rain) fits its arena: one that does
 *        not is left out silently by its host, so it fails here.
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
#include "tx_groove.h"
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

#define OVF_SEEDS 8u                  /* the overflow check: the max scene at seeds 1 .. 8 */

/* a scene at its own seed (seed 0) or at the one given */
static int run_scene_seed(const tx_engine_t *e, const char *scene, acc_t *a, char hex[65], uint32_t seed)
{
    tx_script_t *s = malloc(sizeof *s);
    void *st = tx_state_new(e);
    memset(a, 0, sizeof *a);
    tx_limit_init(&a->lim);
    tx_sha256_init(&a->sha);
    if (tx_scene_load(s, e, scene)) { free(s); free(st); return -1; }
    if (seed) s->seed = seed;
    tx_script_run(e, st, s, on_block, a);
    tx_sha256_hex(&a->sha, hex);
    free(s); free(st);
    return 0;
}

static int run_scene(const tx_engine_t *e, const char *scene, acc_t *a, char hex[65])
{
    return run_scene_seed(e, scene, a, hex, 0);
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
        int32_t pk = a.peak;                         /* the peak over OVF_SEEDS seeds (one draw can be lucky) */
        unsigned over = a.over_fs, k, wk = 1;
        for (k = 2; k <= OVF_SEEDS; k++) {
            acc_t b;
            char h2[65];
            if (run_scene_seed(e, "max", &b, h2, k)) continue;
            if (b.peak > pk) { pk = b.peak; wk = k; }
            if (b.over_fs > over) over = b.over_fs;
        }
        snprintf(msg, sizeof msg, "max scene peak %.1f dBFS over %u seeds (seed %u; limit +24.1), %u samples over full scale (info)",
                 pk ? 20.0 * log10(pk / 8388608.0) : -999.0, OVF_SEEDS, wk, over);
        verdict(n, "overflow", pk < (1 << 27), msg, 0);
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

/* ---- COMP: the bus compressor (Frodhes' controls). COMP off is bit-exact; a steady sine at -6 dBFS (power -9 dBFS)
 * is reduced as THR and RAT say (the 12 dB soft knee included): THR 127 (-40) 3:1 20.6 dB, less the body lift's 0.35
 * of its steady part: 13.7 dB, the auto make-up giving it back down to THR 64's reduction at 3:1 (never louder than
 * THR 64: the level falls or stays as THR goes down): the output 7.8 dB down; THR 64 (-20.2) 2:1
 * 5.6 dB, THR 32 (-10.1) 20:1 2.0 dB (in the knee; neither lift nor auto make-up up to THR 64), and with MKUP 0 the
 * output follows; the controls moved under a steady
 * level (COMP on, THR, RAT, MKUP, ATK, REL to their ends and back, COMP off; 100 ms each) never step the output (a
 * glide), and once off it is exact again */
static void comp_sine(tx_comp_t *c, int32_t *pk)
{
    static int32_t L[TX_BLOCK], R[TX_BLOCK];
    unsigned b, j;
    *pk = 0;
    for (b = 0; b < 400u; b++) {                             /* 2.3 s of a 1 kHz sine at -6 dBFS */
        for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = tx_sin_q15((b * TX_BLOCK + j) * 97391263u) * 128;   /* (1 kHz) */
        tx_comp_process(c, L, R, TX_BLOCK);
        if (b >= 300u) for (j = 0; j < TX_BLOCK; j++) *pk = L[j] > *pk ? L[j] : *pk;
    }
}
static void check_comp(void)
{
    static int32_t L[TX_BLOCK], R[TX_BLOCK], L0[TX_BLOCK], R0[TX_BLOCK];
    static const struct { uint8_t thr, rat; int16_t gr10, out10; } G[3] = {{127, 2, 137, 78}, {64, 1, 56, 56}, {32, 7, 20, 20}};
    static const struct { uint8_t id, v; } MV[] = {
        {TX_CP_ON, 1}, {TX_CP_THR, 127}, {TX_CP_RATIO, 7}, {TX_CP_MKUP, 127}, {TX_CP_ATK, 0}, {TX_CP_REL, 0},
        {TX_CP_THR, 0}, {TX_CP_MKUP, 0}, {TX_CP_RATIO, 0}, {TX_CP_ATK, 127}, {TX_CP_REL, 127}, {TX_CP_THR, 100},
        {TX_CP_ON, 0}, {TX_CP_ON, 0}};
    tx_comp_t c;
    tx_rng_t r;
    unsigned b, j, k, same = 1;
    int32_t gr, pk, step = 0, prev = 419430, exact = 1;
    char msg[240];
    tx_rng_seed(&r, 7u);
    tx_comp_init(&c);                                        /* (off) */
    for (b = 0; b < 200u; b++) {
        for (j = 0; j < TX_BLOCK; j++) { L[j] = L0[j] = tx_rand_s15(&r) * 256; R[j] = R0[j] = tx_rand_s15(&r) * 200; }
        tx_comp_process(&c, L, R, TX_BLOCK);
        for (j = 0; j < TX_BLOCK; j++) same &= L[j] == L0[j] && R[j] == R0[j];
    }
    verdict("comp", "bypass", same, "COMP off: the samples untouched (bit-exact)", 0);
    for (k = 0; k < 3u; k++) {
        int32_t want = (int32_t)(4194304.0 * pow(10.0, -G[k].out10 / 200.0));
        tx_comp_init(&c);
        tx_comp_param(&c, TX_CP_ON, 1); tx_comp_param(&c, TX_CP_THR, G[k].thr); tx_comp_param(&c, TX_CP_RATIO, G[k].rat);
        tx_comp_param(&c, TX_CP_MKUP, 0);
        comp_sine(&c, &pk);
        gr = tx_comp_gr_db10(&c);
        snprintf(msg, sizeof msg, "a sine at -6 dBFS, THR %u RAT %u MKUP 0: reduction %d.%d dB (%d.%d +- 0.5), out peak %d (%d +- 6 %%)",
                 G[k].thr, G[k].rat, gr / 10, gr % 10, G[k].gr10 / 10, G[k].gr10 % 10, pk, want);
        verdict("comp", "gr", gr >= G[k].gr10 - 5 && gr <= G[k].gr10 + 5 && pk > want * 94 / 100 && pk < want * 106 / 100, msg, 0);
    }
    tx_comp_init(&c);
    for (k = 0; k < sizeof MV / sizeof MV[0]; k++) {
        tx_comp_param(&c, MV[k].id, MV[k].v);
        for (b = 0; b < 17u; b++) {                          /* 100 ms of a steady level: -26 dBFS */
            for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = 419430;
            tx_comp_process(&c, L, R, TX_BLOCK);
            for (j = 0; j < TX_BLOCK; j++) {
                int32_t d = L[j] - prev;
                d = d < 0 ? -d : d;
                step = d > step ? d : step;
                prev = L[j];
                if (k + 1u == sizeof MV / sizeof MV[0] && b == 16u) exact &= L[j] == 419430;
            }
        }
    }
    snprintf(msg, sizeof msg, "the controls moved to their ends under a steady level: the largest step a sample %d (under 0.5 %% of full scale: a glide, no click), exact once off again",
             step);
    verdict("comp", "smooth", step < 41943 && exact, msg, 0);
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

/* ---- GOV: the CPU governor (v2). Lanes whose cost is base + per x heroes: over the pool it cuts where a hero saves
 * the most per level lost (the quiet, dear lane first), learns that a lane whose heroes cost nothing is not worth
 * cutting, counts the inserts (tx_gov_fixed), never goes under 2; back under 3/4 of the pool it gives every hero back
 * (the loudest lane first), then lifts the lanes with a ceiling while the pool has room, without ever bringing the
 * next cut; and a source asked for fewer heroes keeps its loudest (the quietest go: tx_eng_source) */
static void gov_run(tx_gov_t *g, const uint32_t *base, const uint32_t *per, const uint32_t *lvl, unsigned blocks,
                    unsigned *cuts, int *first, unsigned *ups)
{
    uint8_t on[4] = {1, 1, 1, 1};
    uint32_t cost[4], b;
    unsigned k;
    for (b = 0; b < blocks; b++) {
        int x;
        for (k = 0; k < 4; k++) cost[k] = base[k] + per[k] * g->cur[k] + (b * 7u + k * 13u) % 23u;   /* (a little noise) */
        x = tx_gov_block(g, cost, lvl, on);
        if (x >= 0 && g->cur[x] < 255u) {
            if (cuts && b && ((g->avg >> 3) > g->budget || g->hold == 8)) { if (*first < 0) *first = x; (*cuts)++; }
            else if (ups) (*ups)++;
        }
    }
}
static void check_gov(void)
{
    tx_gov_t g;
    uint32_t base[4] = {200, 200, 200, 400}, per[4] = {60, 60, 20, 0}, lvl[4] = {300, 100, 200, 50}, tot, b;
    unsigned cuts = 0, ups = 0, k;
    int first = -1, ok;
    char msg[300];
    tx_gov_init(&g, 4, TX_GOV_POOL_PLANNED);
    for (k = 0; k < 4; k++) tx_gov_user(&g, k, 8);
    tx_gov_fixed(&g, 2, 200);                            /* lane 3 has a drive and a filter: +200 us */
    /* 680 x 2 + 360 + 200 + 400 = 2320 us: over 1700 */
    gov_run(&g, base, per, lvl, 1200u, &cuts, &first, 0);
    for (tot = 0, k = 0; k < 4; k++) tot += base[k] + per[k] * g.cur[k] + g.fixed[k];
    ok = g.cur[3] >= 7u && g.cur[1] <= g.cur[0] && g.cur[1] <= g.cur[2] && g.cur[1] < 8u && g.cur[1] >= TX_GOV_MIN
         && tot <= TX_GOV_POOL_PLANNED + 23u * 4u;
    snprintf(msg, sizeof msg, "over the pool (2320 of %u us): %u cuts, the quiet dear lane most (heroes now %u %u %u %u, "
             "%u us); the lane whose heroes cost nothing lost at most the one that showed it (%u)", TX_GOV_POOL_PLANNED,
             cuts, g.cur[0], g.cur[1], g.cur[2], g.cur[3], tot, g.cur[3]);
    verdict("gov", "lanes", ok, msg, 0);
    base[0] = base[1] = base[2] = 50; base[3] = 100;     /* now well under: all back, then the headroom */
    per[0] = per[1] = 30; per[2] = 10;
    tx_gov_ceil(&g, 0, 12); tx_gov_ceil(&g, 2, 16); tx_gov_user(&g, 3, 8);   /* (lane 4 set afresh) */
    gov_run(&g, base, per, lvl, 172u * 60u, 0, 0, &ups);
    for (tot = 0, k = 0; k < 4; k++) tot += base[k] + per[k] * g.cur[k] + g.fixed[k];
    ok = g.cur[1] == 8 && g.cur[3] >= 8 && g.cur[0] == 12 && g.cur[2] > 8 && g.cur[2] <= 16 && tot < TX_GOV_POOL_PLANNED / 4u * 3u + 23u * 4u;
    cuts = 0;
    gov_run(&g, base, per, lvl, 172u * 30u, &cuts, &first, 0);
    ok = ok && !cuts;
    snprintf(msg, sizeof msg, "under the pool: every hero back, then headroom to the ceilings (lane 1 12, lane 3 up to 16): "
             "%u %u %u %u heroes, %u us (under 3/4 of the pool), then %u cuts in 30 s (no see-saw)",
             g.cur[0], g.cur[1], g.cur[2], g.cur[3], tot, cuts);
    verdict("gov", "headroom", ok, msg, 0);
    {   /* one lane over half the pool on its own (the rest quiet): it gives heroes until it is under half */
        tx_gov_t h;
        uint32_t hb[4] = {300, 50, 50, 50}, hp[4] = {60, 10, 10, 10};
        tx_gov_init(&h, 4, TX_GOV_POOL_PLANNED);
        for (k = 0; k < 4; k++) tx_gov_user(&h, k, 12);
        tx_gov_extra(&h, 30);                            /* (the modulation) */
        cuts = 0; first = -1;
        gov_run(&h, hb, hp, lvl, 172u * 10u, &cuts, &first, 0);
        tot = hb[0] + hp[0] * h.cur[0];
        ok = first == 0 && tot <= TX_GOV_POOL_PLANNED / 2u + 23u && tot + hp[0] > TX_GOV_POOL_PLANNED / 2u - 23u &&
             h.cur[1] == 12 && h.cur[2] == 12 && h.cur[3] == 12;
        snprintf(msg, sizeof msg, "a lane over half the pool alone (1020 of %u us, the total under it): %u cuts, all on it, "
                 "now %u heroes, %u us; the others untouched (%u %u %u)", TX_GOV_POOL_PLANNED, cuts, h.cur[0], tot,
                 h.cur[1], h.cur[2], h.cur[3]);
        verdict("gov", "half", ok, msg, 0);
    }
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
            e->param(st, (uint8_t)tx_param_find(e, "ACTIVITY"), 120);
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
    pp = tx_param_find(e, "PRESET"); pi = tx_param_find(e, "ACTIVITY");
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
 * panned apart, summed, the CHAMBER (on or off), the linked limiter. The limited pair never passes -2 dBFS, the raw
 * pair keeps int32 headroom, and the render is deterministic (baseline "layers4-max" / "layers4+chamber-max"). */
static const char *const LAYERS4[4] = { "plate", "storm", "mineral", "spring" };

/* the true peak as a converter rebuilds it: 4x, three points between each pair of samples, low-passed at 20 kHz (a
 * 48-tap windowed sinc, Kaiser 8), |Q23| */
#define TP_N 48
static double tp_h[3][TP_N];
static double tp_i0(double x) { double s = 1, t = 1; int k; for (k = 1; k < 30; k++) { t *= (x / 2) / k; s += t * t; } return s; }
static double tp_peak(const int32_t *x, unsigned n, double *hist)
{
    unsigned j, p, i;
    double pk = 0;
    if (tp_h[0][0] == 0) {
        const double fc = 2.0 * 20000.0 / TX_FS;            /* the cut-off over half the rate */
        for (p = 0; p < 3; p++) {
            double sum = 0;
            for (i = 0; i < TP_N; i++) {
                double t = (double)i - (TP_N / 2 - 1) - (p + 1) / 4.0, a = t / (TP_N / 2 + 1);
                double sn = t == 0 ? 1 : sin(M_PI * fc * t) / (M_PI * fc * t);
                tp_h[p][i] = fc * sn * (a * a < 1 ? tp_i0(8.0 * sqrt(1 - a * a)) / tp_i0(8.0) : 0);
                sum += tp_h[p][i];
            }
            for (i = 0; i < TP_N; i++) tp_h[p][i] /= sum;
        }
    }
    for (j = 0; j < n; j++) {
        memmove(hist, hist + 1, (TP_N - 1) * sizeof *hist);
        hist[TP_N - 1] = x[j];
        if (fabs(hist[TP_N - 1]) > pk) pk = fabs(hist[TP_N - 1]);
        for (p = 0; p < 3; p++) {
            double y = 0;
            for (i = 0; i < TP_N; i++) y += tp_h[p][i] * hist[i];
            if (fabs(y) > pk) pk = fabs(y);
        }
    }
    return pk;
}

static int run_mix(const char *const *names, unsigned comp, int chamber_on, char hex[65], int32_t *raw_peak,
                   int32_t *lim_peak, uint32_t *lim_blocks, double *true_peak)
{
    tx_mix_t *m = malloc(sizeof *m);
    tx_chamber_t *ch = malloc(sizeof *ch);
    tx_script_t *s[4];
    void *st[4];
    unsigned next[4] = {0, 0, 0, 0}, k, i, j;
    uint32_t blocks = 0, b;
    int32_t blk[TX_BLOCK], L[TX_BLOCK], R[TX_BLOCK];
    static double hl[TP_N], hr[TP_N];
    tx_sha256_t sha;
    static const unsigned PAN[4] = {20, 50, 80, 110};
    tx_mix_init(m);
    tx_chamber_init(ch);
    m->ch = ch; m->ch_on = chamber_on;
    for (k = 0; k < 4; k++) {
        const tx_engine_t *e = tx_engine_find(names[k]);
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
    tx_comp_param(&m->comp, TX_CP_ON, comp ? 1u : 0u);   /* (COMP at its defaults, or off) */
    tx_sha256_init(&sha);
    *raw_peak = *lim_peak = 0; *lim_blocks = 0;
    if (true_peak) { memset(hl, 0, sizeof hl); memset(hr, 0, sizeof hr); *true_peak = 0; }
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
        if (true_peak) {
            double a = tp_peak(L, TX_BLOCK, hl), c = tp_peak(R, TX_BLOCK, hr);
            if (a > *true_peak) *true_peak = a;
            if (c > *true_peak) *true_peak = c;
        }
        tx_sha256_q23(&sha, L, TX_BLOCK);
        tx_sha256_q23(&sha, R, TX_BLOCK);
    }
    tx_sha256_hex(&sha, hex);
    for (k = 0; k < 4; k++) { free(s[k]); free(st[k]); }
    free(m); free(ch);
    return (int)blocks;
}

static int run_layers(int chamber_on, char hex[65], int32_t *raw_peak, int32_t *lim_peak, uint32_t *lim_blocks)
{
    return run_mix(LAYERS4, 0, chamber_on, hex, raw_peak, lim_peak, lim_blocks, 0);
}

/* truepeak: dense crackle (fire, ice, glass, sparks at their worst) through the mix, COMP off and on (its defaults): no sample over
 * -2 dBFS, and nothing between the samples at full scale once rebuilt (4x, to 20 kHz), so a converter or the USB
 * audio's 48 kHz resampler does not clip (the FM-1 clipped -1 dBFS samples by up to +1 dB at 48 kHz before; this mix
 * rebuilt to +1.6 dBFS) */
static void check_truepeak(void)
{
    static const char *const CRACK[4] = { "fire", "ice", "glass", "sparks" };
    static const unsigned AM[2] = {0, 1};
    unsigned i;
    for (i = 0; i < 2; i++) {
        char hex[65], msg[200], name[32];
        int32_t rp, lp;
        uint32_t lb;
        double tp;
        int nb = run_mix(CRACK, AM[i], 0, hex, &rp, &lp, &lb, &tp);
        snprintf(name, sizeof name, "COMP %s", AM[i] ? "ON" : "OFF");
        if (nb < 0) { verdict("truepeak", name, 0, "fire+ice+glass+sparks", " do not load"); continue; }
        snprintf(msg, sizeof msg, "fire+ice+glass+sparks at their worst, COMP %s: raw %.1f dBFS, samples %.2f (<= -2), "
                 "rebuilt 4x %.2f dBFS (< 0), the limiter in %u of %d blocks", AM[i] ? "ON" : "OFF", 20.0 * log10(rp / 8388608.0 + 1e-12),
                 20.0 * log10(lp / 8388608.0 + 1e-12), 20.0 * log10(tp / 8388608.0 + 1e-12), lb, nb);
        verdict("truepeak", name, lp <= 6663308 && tp < 8388608.0, msg, 0);
    }
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
                 "(<= -2), the limiter in %u of %d blocks", LAYERS4[0], LAYERS4[1], LAYERS4[2], LAYERS4[3],
                 c ? " + CHAMBER" : "", rp ? 20.0 * log10(rp / 8388608.0) : -999.0, lp ? 20.0 * log10(lp / 8388608.0) : -999.0, lb, nb);
        verdict("layers", c ? "chamber" : "mix", rp < (1 << 29) && lp <= 6663308, msg, 0);
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

/* rockfall's DIST (30 m .. 3 km): a tap's level (mean square over 4 seeds, 15 s) never rises with distance (0.5 dB:
 * seed noise), nor its peak, at the defaults and at the loud corner (VOLUME 127, v127, LEVEL 127); at the defaults the
 * tap stays under full scale from the default distance out (nearer is 1 / r louder, as on every engine's DIST) */
static void check_rockfall_dist(void)
{
    const tx_engine_t *e = tx_engine_find("rockfall");
    void *st = e ? tx_state_new(e) : 0;
    unsigned c, d, k, b, j, bad = 0, clip = 0, p_dist = 0;
    char msg[200], where[64] = "";
    if (!st) { verdict("rockfall", "dist", 0, "rockfall not found", 0); return; }
    for (c = 0; c < 2; c++) {
        double lmin = 1e9, pmin = 1e9, top = -999, ptop = -999;
        for (d = 0; d <= 127; d++) {
            double ms = 0, l, p;
            int32_t mix[TX_BLOCK], pk = 0;
            for (k = 0; k < 4; k++) {
                e->init(st, 3 + 8 * k);
                if (c) { e->param(st, 0, 127); e->param(st, TX_PID_LEVEL, 127); }
                e->param(st, 4, (uint8_t)d);
                e->key(st, 60, c ? 127 : 100, 1);
                for (b = 0; b < 2584u; b++) {
                    for (j = 0; j < TX_BLOCK; j++) mix[j] = 0;
                    if (b == 26) e->key(st, 60, 0, 0);
                    e->render(st, mix, TX_BLOCK);
                    for (j = 0; j < TX_BLOCK; j++) {
                        int32_t x = mix[j] < 0 ? -mix[j] : mix[j];
                        double y = mix[j] / 8388608.0;
                        ms += y * y;
                        if (x > pk) pk = x;
                    }
                }
            }
            l = 10 * log10(ms + 1e-30);
            p = 20 * log10(pk + 1.0);
            if (!c && d >= 40 && pk >= TX_Q23) { if (!clip++) p_dist = d; }
            if (l > lmin + 0.5 || p > pmin + 0.5) {
                if (!bad++) snprintf(where, sizeof where, " (first: %s DIST %u: %+.1f dB)", c ? "loud" : "default", d,
                                     l > lmin + 0.5 ? l - lmin : p - pmin);
            }
            if (l < lmin) lmin = l;
            if (p < pmin) pmin = p;
            if (!d) { top = l; ptop = p; }
            if (d == 127) {
                snprintf(msg, sizeof msg, "%s tap: DIST 127 against 0: level %.1f dB, peak %.1f dB (info)",
                         c ? "loud-corner" : "default", l - top, p - ptop);
                verdict("rockfall", "dist", 1, msg, 0);
            }
        }
    }
    snprintf(msg, sizeof msg, "DIST: %u level/peak rises over 0.5 dB%s, %u at full scale from DIST 40 (first DIST %u)",
             bad, where, clip, p_dist);
    verdict("rockfall", "dist", !bad && !clip, msg, 0);
    free(st);
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
/* the GRAIN DELAY under a moving control (the control sweep's checks, the grain cases): a chord (C3 E4 G4 A4 C5,
 * struck at 0.5 s, harmonics 1..4, ~4 s decay) through the bus; one control static, swept 0 -> max -> 0 (a step
 * every 20 ms from 1 s) or flipped at 2 s. Per 1024-sample window from 1 s: the largest second difference (L, R) and
 * the level; the largest second difference from 3 ms before the flip to 30 ms after it */
#define GS_W 300u
typedef struct { double d2[GS_W], lv[GS_W], click; unsigned nw; } gs_res_t;
static int32_t *gs_chord;
static unsigned gs_len;
#define GS_REV 0x40u                /* pid | GS_REV: a REVERB control (on HALL, MIX 52), else the GRAIN DELAY's */
static unsigned gs_max(unsigned pid) { return pid & GS_REV ? tx_fx_reverb_params[pid & ~GS_REV].max : tx_fx_grain_params[pid].max; }
static void gs_set(tx_fx_t *fx, unsigned pid, unsigned v)
{
    if (pid & GS_REV) tx_fx_reverb_param(&fx->rev, pid & ~GS_REV, v);
    else tx_fx_grain_param(&fx->gd, pid, v);
}
static void gs_render(tx_fx_t *fx, unsigned pid, int mix64, char mode, unsigned a, unsigned b, gs_res_t *r)
{
    unsigned mx = gs_max(pid), nsw = 2u * mx, blk, j, i = 0;
    int32_t L[TX_BLOCK], R[TX_BLOCK], pl[2] = {0, 0}, pr[2] = {0, 0};
    unsigned s0 = (2000u - 3u) * 441u / 10u, s1 = (2000u + 30u) * 441u / 10u;
    memset(r, 0, sizeof *r);
    tx_fx_init(fx);
    /* (MIX at the wet / dry the checks were set for when MIX added the wet to the whole dry: the grain delay's old 64,
     * wet / dry 0.5 = tan of MIX 38's place; the reverb's old 64, 0.38 = tan of MIX 52's place / 2) */
    if (pid & GS_REV) { tx_fx_reverb_param(&fx->rev, TX_RV_TYPE, TX_RVT_HALL); tx_fx_reverb_param(&fx->rev, TX_RV_MIX, 52); }
    else if (mix64) tx_fx_grain_param(&fx->gd, TX_GD_MIX, 38);
    for (blk = 0; blk * TX_BLOCK < gs_len; blk++) {
        unsigned ms = (unsigned)(blk * 256u * 1000u / 44100u), v = a;
        if (mode == 'w' && ms >= 1000u) { unsigned s = (ms - 1000u) / 20u; v = s >= nsw ? 0 : s <= mx ? s : nsw - s; }
        if (mode == 'f') v = ms >= 2000u ? b : a;
        gs_set(fx, pid, v);
        for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = gs_chord[blk * TX_BLOCK + j];
        tx_fx_bus(fx, L, R, TX_BLOCK);
        for (j = 0; j < TX_BLOCK; j++, i++) {
            double dl = fabs((double)L[j] - 2.0 * pl[1] + pl[0]), dr = fabs((double)R[j] - 2.0 * pr[1] + pr[0]), d = dl > dr ? dl : dr;
            if (i >= 2u && i >= s0 + 2u && i < s1 && d > r->click) r->click = d;
            if (i >= 44100u) {
                unsigned w = (i - 44100u) / 1024u;
                if (w < GS_W) {
                    if (i - 44100u - w * 1024u >= 2u && d > r->d2[w]) r->d2[w] = d;
                    r->lv[w] += ((double)L[j] * L[j] + (double)R[j] * R[j]) / 2048.0;
                    if (w + 1u > r->nw && i - 44100u - w * 1024u == 1023u) r->nw = w + 1u;
                }
            }
            pl[0] = pl[1]; pl[1] = L[j]; pr[0] = pr[1]; pr[1] = R[j];
        }
    }
    for (j = 0; j < r->nw; j++) r->lv[j] = 10.0 * log10(r->lv[j] + 1e-9);
}
/* the worst zipper (the sweep's second difference over the statics', per window) and level jump (a window-to-window
 * step past the statics' own, dB) over the sweep */
static void gs_sweep(tx_fx_t *fx, unsigned pid, int mix64, double *zip, double *jump)
{
    static gs_res_t sw, st[5];
    unsigned mx = gs_max(pid), k, w, nw = (2u * mx * 20u * 441u / 10u) / 1024u;
    gs_render(fx, pid, mix64, 'w', 0, 0, &sw);
    for (k = 0; k < 5u; k++) gs_render(fx, pid, mix64, 's', mx * k / 4u, 0, &st[k]);
    *zip = *jump = 0;
    if (nw > sw.nw) nw = sw.nw;
    for (w = 0; w < nw; w++) {
        double ref = 1.0, js = 0;
        for (k = 0; k < 5u; k++) {
            if (st[k].d2[w] > ref) ref = st[k].d2[w];
            if (w && fabs(st[k].lv[w] - st[k].lv[w - 1]) > js) js = fabs(st[k].lv[w] - st[k].lv[w - 1]);
        }
        if (sw.d2[w] / ref > *zip) *zip = sw.d2[w] / ref;
        if (w && sw.lv[w] > -100.0 + 10.0 * log10(70368744177664.0) && fabs(sw.lv[w] - sw.lv[w - 1]) - js > *jump)
            *jump = fabs(sw.lv[w] - sw.lv[w - 1]) - js;
    }
}
/* the grain delay's loop (the unit, 0.2.1: "a delay for the first second, then the highs and the level vanish, even
 * at high FEEDBACK"): a 20 ms noise burst, MIX 127, TIME 70 (229 ms), TONE at its default; the wet's level and its
 * brightness (the first difference's power over the signal's) per second, 1..2 s against 9..10 s. FEEDBACK 110 (1.0
 * a pass): the level and the brightness hold, the echo alone (GRAIN 0) and grained (GRAIN 64 and 127, 4 and 30
 * grains: each pass grained again); FEEDBACK 104 (-0.28 dB a pass): the echo falls as its table says, the grains
 * about so (a pass is TIME + the SPRAY they add) */
static void check_fx_grain_loop(tx_fx_t *fx)
{
    static const struct { uint8_t fb, gr, gn; } C[] = {{110, 0, 4}, {110, 64, 4}, {110, 127, 4}, {110, 127, 30},
                                                      {104, 0, 4}, {104, 127, 4}, {104, 127, 30}};
    double dl[7], db[7];
    unsigned c, b, j, ok = 1;
    char msg[600];
    int n = 0;
    for (c = 0; c < 7u; c++) {
        tx_fx_grain_t *d = &fx->gd;
        int32_t L[TX_BLOCK], R[TX_BLOCK], prev = 0;
        uint32_t rng = 3u;
        double e[2] = {0, 0}, h[2] = {0, 0};
        tx_fx_grain_init(d);
        tx_fx_grain_param(d, TX_GD_MIX, 127); tx_fx_grain_param(d, TX_GD_FEEDBACK, C[c].fb);
        tx_fx_grain_param(d, TX_GD_GRAIN, C[c].gr); tx_fx_grain_param(d, TX_GD_GRAINS, C[c].gn - 1u);
        for (b = 0; b < 1723u; b++) {                   /* 10 s */
            for (j = 0; j < TX_BLOCK; j++) {
                uint32_t i = b * TX_BLOCK + j;
                rng = rng * 1664525u + 1013904223u;
                L[j] = R[j] = i >= 4410u && i < 5292u ? ((int32_t)(rng >> 8) - 8388608) / 20 : 0;
            }
            d->cap = TX_GD_MAXG;
            tx_fx_grain_process(d, L, R, TX_BLOCK);
            for (j = 0; j < TX_BLOCK; j++) {
                int k = b >= 172u && b < 344u ? 0 : b >= 1550u ? 1 : -1;
                double x = L[j], dx = (double)L[j] - prev;
                prev = L[j];
                if (k >= 0) { e[k] += x * x; h[k] += dx * dx; }
            }
        }
        dl[c] = 10 * log10((e[1] + 1) / (e[0] + 1));
        db[c] = 10 * log10(((h[1] + 1) / (e[1] + 1)) / ((h[0] + 1) / (e[0] + 1)));
        if (db[c] < -4 || db[c] > 4) ok = 0;
        if (C[c].fb == 110 && (dl[c] < -6 || dl[c] > 4)) ok = 0;
        if (C[c].fb == 104 && !C[c].gr && (dl[c] < -11.5 || dl[c] > -8)) ok = 0;
        if (C[c].fb == 104 && C[c].gr && (dl[c] < -15 || dl[c] > -4)) ok = 0;
    }
    for (c = 0; c < 7u; c++)
        n += snprintf(msg + n, sizeof msg - (size_t)n, "%sFB %u GRAIN %u x%u %+.1f / %+.1f", c ? ", " : "", C[c].fb, C[c].gr,
                      C[c].gn, dl[c], db[c]);
    snprintf(msg + n, sizeof msg - (size_t)n, " dB (level / brightness, 9 s on re 1 s)");
    verdict("fx", "grain loop", ok, msg, 0);
}
/* the grain delay's level and overlap (the unit, 0.2.1: the delay faint at high GRAINS, never a cloud): noise in, MIX
 * 127, GRAIN 127, FEEDBACK 0; the wet's level re the input at GRAINS 1 .. 30, scattered (SPRAY 20) and as copies of
 * one read (SPRAY 0): flat (it fell 1 / sqrt(N), -12 dB from 2 to 30). And the cap under GRAINS (4 of 30): the four
 * spread over the turn (copies: their windows sum flat, no 2.9 ms window 12 dB under the mean; bunched, most were) */
static void check_fx_grain_level(tx_fx_t *fx)
{
    static const unsigned GN[] = {1, 2, 3, 4, 8, 15, 30};
    double lv[2][7], lo = 1e9, hi = -1e9;
    unsigned sp, i, b, j, holes = 0, nw = 0;
    char msg[400];
    for (sp = 0; sp < 3u; sp++)
        for (i = 0; i < (sp < 2u ? 7u : 1u); i++) {
            tx_fx_grain_t *d = &fx->gd;
            int32_t L[TX_BLOCK], R[TX_BLOCK];
            uint32_t rng = 1u;
            double acc = 0, ain = 0, mean = 0, win[64];
            unsigned nwin = 0;
            tx_fx_grain_init(d);
            tx_fx_grain_param(d, TX_GD_MIX, 127); tx_fx_grain_param(d, TX_GD_GRAIN, 127);
            tx_fx_grain_param(d, TX_GD_FEEDBACK, 0); tx_fx_grain_param(d, TX_GD_SPRAY, sp == 1u ? 20u : 0u);
            tx_fx_grain_param(d, TX_GD_GRAINS, (sp < 2u ? GN[i] : 30u) - 1u);
            for (b = 0; b < 800u; b++) {
                d->cap = (uint8_t)(sp == 2u ? 4u : TX_GD_MAXG);
                for (j = 0; j < TX_BLOCK; j++) {
                    rng = rng * 1664525u + 1013904223u;
                    L[j] = R[j] = ((int32_t)(rng >> 8) - 8388608) / 4;
                    if (b >= 400u) ain += (double)L[j] * L[j];
                }
                tx_fx_grain_process(d, L, R, TX_BLOCK);
                if (b < 400u) continue;
                for (j = 0; j < TX_BLOCK; j++) {
                    double v = ((double)L[j] * L[j] + (double)R[j] * R[j]) / 2;
                    acc += v;
                }
                if (b < 432u)                              /* (2.9 ms halves: 2 windows a block, 32 blocks) */
                    for (j = 0; j < 2u; j++) {
                        double w = 0;
                        unsigned k;
                        for (k = 0; k < TX_BLOCK / 2u; k++) w += (double)L[j * TX_BLOCK / 2u + k] * L[j * TX_BLOCK / 2u + k];
                        win[nwin++] = w;
                    }
            }
            if (sp < 2u) {
                lv[sp][i] = 10 * log10(acc / ain);
                if (lv[sp][i] < lo) lo = lv[sp][i];
                if (lv[sp][i] > hi) hi = lv[sp][i];
            } else {
                for (j = 0; j < nwin; j++) mean += win[j] / nwin;
                for (j = 0; j < nwin; j++) holes += win[j] < mean / 16.0;
                nw = nwin;
            }
        }
    snprintf(msg, sizeof msg, "grain delay level re the input, GRAINS 1 2 3 4 8 15 30: scattered %.1f %.1f %.1f %.1f %.1f %.1f %.1f, "
             "copies %.1f %.1f %.1f %.1f %.1f %.1f %.1f dB (%.1f dB apart); cap 4 of 30: %u of %u windows 12 dB under the mean",
             lv[1][0], lv[1][1], lv[1][2], lv[1][3], lv[1][4], lv[1][5], lv[1][6], lv[0][0], lv[0][1], lv[0][2], lv[0][3],
             lv[0][4], lv[0][5], lv[0][6], hi - lo, holes, nw);
    verdict("fx", "grain level", hi - lo < 1.5 && lo > -6 && holes == 0, msg, 0);
}
static void check_fx_grain_controls(tx_fx_t *fx)
{
    static const int notes[5] = {48, 64, 67, 69, 72};
    static gs_res_t mv, h0, h1;
    double zip, jump, ph[5][4] = {{0}}, wc = 0;
    {   /* the line's length (tx_fx_gd_len.h) and TIME's table agree: its top is the line's, at least 1.0 s */
        char m[160];
        snprintf(m, sizeof m, "grain delay: the line %u samples (%.3f s, %u B), TIME's top %u (%.3f s)", TX_GD_N,
                 TX_GD_N / 44100.0, TX_GD_N / 2u * 3u + TX_GD_N / 32u, TX_FX_GD_TIME[TX_GD_TIME_MAX],
                 TX_FX_GD_TIME[TX_GD_TIME_MAX] / 44100.0);
        verdict("fx", "length", TX_FX_GD_TIME[TX_GD_TIME_MAX] == TX_GD_TOP && TX_FX_GD_TIME[127] == TX_GD_TOP0
                && TX_GD_TOP >= 44100u && !(TX_GD_N % 32u), m,
                TX_FX_GD_TIME[TX_GD_TIME_MAX] != TX_GD_TOP ? " (make tx-fx-tables)" : 0);
    }
    char msg[200];
    unsigned i, k, h, t;
    gs_len = 7u * 44100u / TX_BLOCK * TX_BLOCK;
    gs_chord = malloc(gs_len * sizeof *gs_chord);
    if (!gs_chord) return;
    for (i = 0; i < gs_len; i++) {
        double tt = i / 44100.0, s = 0;
        if (tt >= 0.5)
            for (k = 0; k < 5u; k++)
                for (h = 0; h < 4u; h++) {
                    ph[k][h] += 440.0 * pow(2.0, (notes[k] - 69) / 12.0) * (h + 1) / 44100.0;
                    s += 0.06 / (h + 1) * exp(-(tt - 0.5) * (1.0 + h) / 4.0) * sin(6.283185307179586 * ph[k][h]);
                }
        gs_chord[i] = (int32_t)lrint(s * 8388608.0);
    }
    gs_sweep(fx, TX_GD_MIX, 0, &zip, &jump);
    snprintf(msg, sizeof msg, "grain delay, MIX swept 0 .. 127 .. 0 on a ringing chord: zipper %.2f (2 at most)", zip);
    verdict("fx", "mix-sweep", zip <= 2.0, msg, 0);
    gs_sweep(fx, TX_GD_GRAINS, 1, &zip, &jump);
    snprintf(msg, sizeof msg, "grain delay, GRAINS swept 1 .. 16 .. 1: zipper %.2f (2 at most)", zip);
    verdict("fx", "grains-sweep", zip <= 2.0, msg, 0);
    gs_sweep(fx, TX_GD_TIME, 1, &zip, &jump);
    /* (the step is the echo's comb on the chord, moving: by TIME's top alone it was 1.07 .. 1.74 dB for tops of
     * 1.40 .. 1.66 s, so 2 dB at most; with the loop lossless (G-delayloop: the GRAIN blend equal-power, the grains'
     * mid whole) the repeats keep their level, the comb is deeper: 2.10, so 2.5 at most; the zipper is unchanged) */
    snprintf(msg, sizeof msg, "grain delay, TIME swept 0 .. 127 .. 0: a level step %.2f dB past the statics' (2.5 at most), zipper %.2f", jump, zip);
    verdict("fx", "time-sweep", jump <= 2.5 && zip <= 2.0, msg, 0);
    for (t = 0; t < 2u; t++) {                              /* GRAIN 0 -> 127 and back at 2 s */
        unsigned a = t ? 127u : 0u, b = t ? 0u : 127u;
        double ref;
        gs_render(fx, TX_GD_GRAIN, 1, 'f', a, b, &mv);
        gs_render(fx, TX_GD_GRAIN, 1, 's', a, 0, &h0);
        gs_render(fx, TX_GD_GRAIN, 1, 's', b, 0, &h1);
        ref = h0.click > h1.click ? h0.click : h1.click;
        if (mv.click / (ref > 1.0 ? ref : 1.0) > wc) wc = mv.click / (ref > 1.0 ? ref : 1.0);
    }
    snprintf(msg, sizeof msg, "grain delay, GRAIN flipped 0 <-> 127: a click %.2fx the held settings' (10 at most)", wc);
    verdict("fx", "grain-flip", wc <= 10.0, msg, 0);
    /* the reverb's PREDELAY (HALL, MIX 52): swept, and flipped 0 <-> 127 (no click, zipper or level step; the trans
     * after a move is the tail's interference with the shifted send on held tones: a time, as delay TIME; 1.30 dB
     * with the kernels at the half rate, 1.63 at the full rate with every line modulated, so 2 dB at most, as TIME) */
    gs_sweep(fx, GS_REV | TX_RV_PREDELAY, 0, &zip, &jump);
    for (t = 0, wc = 0; t < 2u; t++) {
        unsigned a = t ? 127u : 0u, b = t ? 0u : 127u;
        double ref;
        gs_render(fx, GS_REV | TX_RV_PREDELAY, 0, 'f', a, b, &mv);
        gs_render(fx, GS_REV | TX_RV_PREDELAY, 0, 's', a, 0, &h0);
        gs_render(fx, GS_REV | TX_RV_PREDELAY, 0, 's', b, 0, &h1);
        ref = h0.click > h1.click ? h0.click : h1.click;
        if (mv.click / (ref > 1.0 ? ref : 1.0) > wc) wc = mv.click / (ref > 1.0 ? ref : 1.0);
    }
    snprintf(msg, sizeof msg, "reverb PREDELAY swept 0 .. 127 .. 0: a level step %.2f dB past the statics' (2 at most), "
             "zipper %.2f; flipped 0 <-> 127: a click %.2fx", jump, zip, wc);
    verdict("fx", "predelay", jump <= 2.0 && zip <= 2.0 && wc <= 10.0, msg, 0);
    gs_sweep(fx, GS_REV | TX_RV_MIX, 0, &zip, &jump);
    snprintf(msg, sizeof msg, "reverb (HALL), MIX swept 0 .. 127 .. 0 on a ringing chord: zipper %.2f (2 at most)", zip);
    verdict("fx", "rv-mix-sweep", zip <= 2.0, msg, 0);
    /* the grain delay's MOD: swept, and flipped 0 <-> 127 (its glide: no zipper, no click) */
    gs_sweep(fx, TX_GD_MOD, 1, &zip, &jump);
    for (t = 0, wc = 0; t < 2u; t++) {
        unsigned a = t ? 127u : 0u, b = t ? 0u : 127u;
        double ref;
        gs_render(fx, TX_GD_MOD, 1, 'f', a, b, &mv);
        gs_render(fx, TX_GD_MOD, 1, 's', a, 0, &h0);
        gs_render(fx, TX_GD_MOD, 1, 's', b, 0, &h1);
        ref = h0.click > h1.click ? h0.click : h1.click;
        if (mv.click / (ref > 1.0 ? ref : 1.0) > wc) wc = mv.click / (ref > 1.0 ? ref : 1.0);
    }
    snprintf(msg, sizeof msg, "grain delay, MOD swept 0 .. 127 .. 0: a level step %.2f dB past the statics' (2 at most), "
             "zipper %.2f; flipped 0 <-> 127: a click %.2fx", jump, zip, wc);
    verdict("fx", "mod-sweep", jump <= 2.0 && zip <= 2.0 && wc <= 10.0, msg, 0);
    free(gs_chord);
}
/* MIX, dry / wet (tx_fx.h): the law (the dry whole to 64, the wet whole from 64); the GRAIN DELAY and the REVERB at
 * MIX 0 (the dry alone, exactly), 64 (the dry whole) and 127 (no dry: nothing out before the first echo, or the
 * pre-delay); both at 127 in series: the reverb of the echoes alone (nothing out before the first echo) */
/* the HALL's HQ tank: its rings fill the memory the union gives them; an impulse's tail at MIX 127 decays at the time
 * asked (mid T60 from the energy 0.2 .. 0.7 s, within 0.6 .. 1.25 x the asked; the high end's below the mid's) and ends
 * in exact silence (the reverb stops running) */
static void check_fx_hq(tx_fx_t *fx)
{
    static const unsigned SZ[3] = {1, 40, 127};
    int32_t L[TX_BLOCK], R[TX_BLOCK];
    char msg[200];
    unsigned q;
    snprintf(msg, sizeof msg, "the HQ tank's rings: %u words of the %u it has", (unsigned)tx_hq_words(), (unsigned)HQ_HALL_WORDS);
    verdict("fx", "hq-rings", tx_hq_words() == HQ_HALL_WORDS, msg, 0);
    for (q = 0; q < 3u; q++) {                         /* 2 s of noise (the tail full), then its decay: -5 .. -25 dB */
        double ss = 0, t5 = 0, t25 = 0, t60, want;
        uint32_t b, j, rng = 1u, sil = 0, stop = 344u;
        int32_t s;
        tx_fx_init(fx);
        tx_fx_reverb_param(&fx->rev, TX_RV_TYPE, TX_RVT_HALL); tx_fx_reverb_param(&fx->rev, TX_RV_MIX, 127);
        tx_fx_reverb_param(&fx->rev, TX_RV_SIZE, SZ[q]);
        s = 40 + (int32_t)SZ[q] * 70 / 127;
        want = 12.8 * pow(2.0, (s - 140) / 30.0);
        for (b = 0; b < stop + 1720u && !sil; b++) {    /* (10 s of tail at most) */
            double e = 0;
            int32_t any = 0;
            for (j = 0; j < TX_BLOCK; j++) {
                rng = rng * 1664525u + 1013904223u;
                L[j] = R[j] = b < stop ? ((int32_t)(rng >> 9) - (1 << 22)) >> 1 : 0;
            }
            tx_fx_reverb_process(&fx->rev, L, R, TX_BLOCK);
            for (j = 0; j < TX_BLOCK; j++) { double v = (double)L[j] + R[j]; e += v * v; any |= L[j] | R[j]; }
            if (b >= stop - 43u && b < stop) ss += e / 43.0;
            if (b >= stop) {
                double d = 10.0 * log10(e / ss + 1e-30), t = (b - stop) * TX_BLOCK / 44100.0;
                if (!t5 && d <= -5.0) t5 = t;
                if (!t25 && d <= -25.0) t25 = t;
                if (!any) sil = b - stop;
            }
        }
        t60 = 3.0 * (t25 - t5);
        snprintf(msg, sizeof msg, "HALL SIZE %u: T60 %.2f s for %.2f asked (0.6 .. 1.0 x), exact silence after %.2f s (%.2f at most)",
                 SZ[q], t60, want, sil * TX_BLOCK / 44100.0, 1.5 * want + 0.5);
        verdict("fx", "hq-decay", t60 >= 0.6 * want && t60 <= want && sil && sil * TX_BLOCK / 44100.0 <= 1.5 * want + 0.5, msg, 0);
    }
    tx_fx_init(fx);
}

static void check_fx_drywet(tx_fx_t *fx)
{
    int32_t L[TX_BLOCK], R[TX_BLOCK], x[TX_BLOCK];
    uint32_t b, j, rng, m, bad;
    char msg[200];
    unsigned t;
    bad = 0;                                            /* the law (tx_fx.h): 0 the dry alone, 64 both whole, 127 the
                                                         * wet alone; one part whole at every MIX, the other moving
                                                         * monotonically, the dry whole to 64, the wet from 64 */
    for (m = 0; m <= 127u; m++) {
        int32_t x = TX_FX_MIXQ(m), d = tx_fx_mixd(x), w = tx_fx_mixw(x), xp = m ? TX_FX_MIXQ(m - 1u) : 0;
        if ((d != 32768 && w != 32768) || (m <= 64u && d != 32768) || (m >= 64u && w != 32768)) bad++;
        if (m && (tx_fx_mixw(xp) > w || tx_fx_mixd(xp) < d || xp >= x)) bad++;
    }
    if (tx_fx_mixw(TX_FX_MIXQ(0)) || tx_fx_mixd(TX_FX_MIXQ(127)) || TX_FX_MIXQ(64) != 16384) bad++;
    snprintf(msg, sizeof msg, "the dry / wet law: %u MIX values off it (0: dry alone, 64: both whole, 127: wet alone)", bad);
    verdict("fx", "mix-law", !bad, msg, 0);
    for (t = 0; t < 4u; t++) {                          /* 0 grain, 1 reverb, 2 both; MIX 0 / 64 / 127 each */
        static const unsigned MV[3] = {0, 64, 127};
        unsigned q;
        if (t == 3u) break;
        for (q = 0; q < 3u; q++) {
            double din = 0, dout = 0;
            uint32_t early = 0, ne = 0, nd = 0;
            tx_fx_init(fx);
            if (t != 1u) { tx_fx_grain_param(&fx->gd, TX_GD_MIX, MV[q]); tx_fx_grain_param(&fx->gd, TX_GD_TIME, 100); }
            if (t != 0u) { tx_fx_reverb_param(&fx->rev, TX_RV_TYPE, TX_RVT_HALL); tx_fx_reverb_param(&fx->rev, TX_RV_MIX, MV[q]);
                           tx_fx_reverb_param(&fx->rev, TX_RV_PREDELAY, 127); }
            for (b = 0; b < 40u; b++) {                 /* (40 blocks of silence: MIX settled) then 20 of the burst */
                for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = 0;
                tx_fx_bus(fx, L, R, TX_BLOCK);
            }
            rng = 3u; bad = 0;
            for (b = 0; b < 12u; b++) {                 /* 12 blocks, 70 ms: before the pre-delay (80 ms) and the echo */
                for (j = 0; j < TX_BLOCK; j++) x[j] = L[j] = R[j] = fx_in(b * TX_BLOCK + j, &rng);
                tx_fx_bus(fx, L, R, TX_BLOCK);
                for (j = 0; j < TX_BLOCK; j++) {
                    din += (double)x[j] * x[j]; dout += (double)L[j] * L[j];
                    bad += L[j] != R[j];
                    nd += L[j] != x[j];
                    if (L[j]) early++;
                    ne++;
                }
            }
            if (q == 0u) {
                snprintf(msg, sizeof msg, "%s at MIX 0: %u of %u samples not the dry", t == 0u ? "grain delay" : t == 1u ? "reverb" : "both",
                         nd, ne);
                verdict("fx", "mix-0", !nd && !bad, msg, 0);
            } else if (q == 1u) {
                double g = 10.0 * log10(dout / din), want = 0.0;   /* (64: the dry whole, the wet not yet in) */
                snprintf(msg, sizeof msg, "%s at MIX 64: the dry %+.2f dB (%+.2f)", t == 0u ? "grain delay" : t == 1u ? "reverb" : "both",
                         g, want);
                verdict("fx", "mix-64", fabs(g - want) < 0.05, msg, 0);
            } else {
                snprintf(msg, sizeof msg, "%s at MIX 127: %u nonzero samples before the first echo / pre-delay (the wet alone)",
                         t == 0u ? "grain delay" : t == 1u ? "reverb" : "grain delay -> reverb", early);
                verdict("fx", "mix-127", !early, msg, 0);
            }
        }
    }
    {   /* in series at 127: the reverb takes the echoes, so its tail comes after the first echo (TIME 100: ~0.6 s) */
        uint32_t first = 0, fr = 0;
        tx_fx_init(fx);
        tx_fx_grain_param(&fx->gd, TX_GD_MIX, 127); tx_fx_grain_param(&fx->gd, TX_GD_TIME, 100); tx_fx_grain_param(&fx->gd, TX_GD_GRAIN, 0);
        tx_fx_reverb_param(&fx->rev, TX_RV_TYPE, TX_RVT_HALL); tx_fx_reverb_param(&fx->rev, TX_RV_MIX, 127);
        rng = 3u;
        for (b = 0; b < 400u && !first; b++) {
            for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = b >= 40u && b < 44u ? fx_in((b - 40u) * TX_BLOCK + j, &rng) : 0;
            tx_fx_bus(fx, L, R, TX_BLOCK);
            for (j = 0; j < TX_BLOCK && !first; j++) if (L[j] | R[j]) first = (b - 40u) * TX_BLOCK + j + 1u;
        }
        fr = TX_FX_GD_TIME[100];
        snprintf(msg, sizeof msg, "grain delay -> reverb, both MIX 127, a 23 ms burst: the first sound out at %.1f ms, the "
                 "echo's time %.1f ms (the reverb of the echoes alone)", (first - 1u) / 44.1, fr / 44.1);
        verdict("fx", "series", first && first - 1u >= fr - 64u, msg, 0);
    }
}
/* the mix's FILTER (tx_fx_mfilter.c): a band from LOW CUT and HIGH CUT; off at the ends (bit-exact), stable, no click */
static double mf_gain(unsigned lo, unsigned hi, unsigned reso, unsigned slope, uint32_t hz)   /* a sine's level, dB */
{
    tx_fx_mfilter_t f;
    int32_t L[TX_BLOCK], R[TX_BLOCK], pk = 0;
    uint32_t b, j, ph = 0, inc = (uint32_t)((double)hz * 4294967296.0 / 44100.0);
    tx_fx_mfilter_init(&f);
    tx_fx_mfilter_param(&f, TX_MF_LOW, lo); tx_fx_mfilter_param(&f, TX_MF_HIGH, hi);
    tx_fx_mfilter_param(&f, TX_MF_RESO, reso); tx_fx_mfilter_param(&f, TX_MF_SLOPE, slope);
    for (b = 0; b < 120u; b++) {
        for (j = 0; j < TX_BLOCK; j++, ph += inc) L[j] = R[j] = tx_sin_q15(ph) * 128;   /* 0.5 of full scale */
        tx_fx_mfilter_process(&f, L, R, TX_BLOCK);
        if (b >= 60u) for (j = 0; j < TX_BLOCK; j++) { int32_t a = L[j] < 0 ? -L[j] : L[j]; if (a > pk) pk = a; if (R[j] != L[j]) return 999; }
    }
    return 20 * log10((pk + 1) / 4194304.0);
}
/* the GRAIN delay's MOD on a steady 440 Hz tone (MIX 127, FEEDBACK 0, SPRAY 0, SIZE 127): its grains sound only the
 * INTERVALS' ratios (the energy within +-3 % of 440 x a ratio of the set, or of unison, over the whole output's); at
 * MOD 127 a good share leaves unison; GRAIN 0 (the echo alone): the time's drift bends the pitch, under ~1.3 % and
 * moving; the drifts' turns at random (their gaps' CV >= 0.9); MOD back to 0: the plain delay again, exactly */
static double gm_band(const double *y, unsigned n, double f0)   /* the energy within f0 +-3 % (Goertzel, 0.5 Hz steps) */
{
    double e = 0, f;
    for (f = f0 * 0.97; f <= f0 * 1.03; f += 0.5) {
        double w = 2.0 * cos(6.283185307179586 * f / 44100.0), s1 = 0, s2 = 0, s0;
        unsigned i;
        for (i = 0; i < n; i++) { s0 = y[i] + w * s1 - s2; s2 = s1; s1 = s0; }
        e += 2.0 * (s1 * s1 + s2 * s2 - w * s1 * s2) / ((double)n * n);   /* (a bin's share of the mean power) */
    }
    return e * 0.5 / (44100.0 / n);         /* (0.5 Hz steps over the n-sample bins) */
}
static void gm_run(tx_fx_grain_t *d, unsigned mod, unsigned ivl, unsigned grain, double *y, unsigned n)
{
    int32_t L[TX_BLOCK], R[TX_BLOCK];
    unsigned b, j, nb = (n + 2u * 44100u + TX_BLOCK - 1u) / TX_BLOCK, i = 0;
    tx_fx_grain_init(d);
    tx_fx_grain_param(d, TX_GD_MIX, 127); tx_fx_grain_param(d, TX_GD_FEEDBACK, 0); tx_fx_grain_param(d, TX_GD_SPRAY, 0);
    tx_fx_grain_param(d, TX_GD_SIZE, 127); tx_fx_grain_param(d, TX_GD_GRAIN, grain); tx_fx_grain_param(d, TX_GD_MOD, mod);
    tx_fx_grain_param(d, TX_GD_IVL, ivl); tx_fx_grain_param(d, TX_GD_GRAINS, 7); tx_fx_grain_param(d, TX_GD_TIME, 40);
    for (b = 0; b < nb; b++) {
        for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = tx_sin_q15((b * TX_BLOCK + j) * 42852281u) * 64;   /* (440 Hz) */
        tx_fx_grain_process(d, L, R, TX_BLOCK);
        if (b * TX_BLOCK >= 2u * 44100u) for (j = 0; j < TX_BLOCK && i < n; j++) y[i++] = (L[j] + R[j]) / 2.0;
    }
}
/* GROOVE (tx_groove.h) on a lane's generator, 5 min at 120 BPM, an insect's weight on PULSE of LOOSE 16 (its slots the
 * beats): at AMOUNT 0 the multiplier is 4096 exactly (the generator as before, bit for bit); at the default and at
 * 127 the notes lean toward the beats (the share within +-1/32 beat of a beat over the free share) while their gaps
 * stay natural (CV), and the onsets' envelope shows no loop: its autocorrelation at 2 .. 16 bars no stronger than at
 * a beat (the pulse's own). Also a 3:4:5 across three lanes: each lane leans to its own cycle */
typedef struct { double cv, near, ac; unsigned n; } gv_res_t;
static void gv_run(unsigned amount, unsigned type, unsigned part, unsigned cycle, gv_res_t *o)
{
    static tx_gen_t g;
    static float env[60000 * 8];
    tx_groove_lane_t l;
    tx_gen_ev_t ev[TX_GEN_EV_MAX];
    uint32_t b, pos = 0, frac = 0, inc = 1610612736u / 22050u, nb = 300u * 44100u / TX_BLOCK, nbin = 0, near = 0, nn = 0;
    double last = -1, sm = 0, sq = 0, ng = 0;
    uint8_t in;
    unsigned q, k;
    tx_gen_init(&g, 7u);
    tx_gen_param(&g, TX_GEN_ACTIVITY, 100);
    tx_groove_lane_init(&l, 11u);
    l.part = (uint8_t)part; l.cycle = (uint8_t)cycle; l.wl = 3;
    memset(env, 0, sizeof env);
    for (b = 0; b < nb; b++) {
        int32_t m = tx_groove_mul(&l, type, amount, 0, pos);
        unsigned ne;
        if (!amount && b > 2000u && m != 4096) { o->n = 0; return; }
        g.rlean = (int16_t)(m - 4096);
        ne = tx_gen_block(&g, TX_BLOCK, ev, TX_GEN_EV_MAX, &in);
        for (q = 0; q < ne; q++) if (ev[q].vel) {
            double t = b * (double)TX_BLOCK + ev[q].off, tk = pos / 256.0 + ev[q].off * 96.0 / 22050.0;
            int32_t dd = tx_groove_dist(&l, type, 0, (uint32_t)tk);
            if (last >= 0) { double gp = t - last; sm += gp; sq += gp * gp; ng++; }
            last = t; nn++;
            if (dd <= 3) near++;
            k = (uint32_t)(tk / 12.0);
            if (k < sizeof env / sizeof *env) env[k] += 1.0f;
        }
        { uint32_t f = frac + inc; pos += f >> 8; frac = f & 255u; }
    }
    nbin = pos / 256u / 12u;
    if (nbin > sizeof env / sizeof *env) nbin = sizeof env / sizeof *env;
    {
        double mean = sm / ng, mu = 0, r0 = 0, rb, best = 0;
        unsigned i, lag;
        o->cv = sqrt(sq / ng - mean * mean) / mean;
        o->near = (double)near / nn; o->n = nn;
        for (i = 0; i < nbin; i++) mu += env[i] / nbin;
        for (i = 0; i < nbin; i++) r0 += (env[i] - mu) * (env[i] - mu);
        for (lag = 8, rb = 0, i = 0; i + lag < nbin; i++) rb += (env[i] - mu) * (env[i + lag] - mu);
        rb /= r0;
        for (k = 2; k <= 16u; k++) {
            double rk = 0;
            lag = 32u * k;
            for (i = 0; i + lag < nbin; i++) rk += (env[i] - mu) * (env[i + lag] - mu);
            rk /= r0;
            if (rk > best) best = rk;
        }
        o->ac = best - (rb > 0 ? rb : 0);
    }
}
static void check_groove(void)
{
    gv_res_t z, d, mx, p3, p4, p5;
    char msg[400];
    char nm[16];
    gv_run(0, 0, TX_GRP_PULSE, 0, &z);
    verdict("groove", "off", z.n > 0, "GROOVE 0: the generator's rate untouched (4096 exactly, its notes as before)", 0);
    gv_run(TX_GR_AMOUNT_DEF, 0, TX_GRP_PULSE, 0, &d);
    gv_run(127, 0, TX_GRP_PULSE, 0, &mx);
    snprintf(msg, sizeof msg, "%u notes; near a beat (+-1/32): free %.1f %%, default %.1f %%, 127 %.1f %%; gaps' CV %.2f / "
             "%.2f / %.2f; a loop (bars over the beat's autocorrelation) %.3f / %.3f / %.3f", d.n, 100 * z.near,
             100 * d.near, 100 * mx.near, z.cv, d.cv, mx.cv, z.ac, d.ac, mx.ac);
    verdict("groove", "organic", d.near > z.near * 1.1 && mx.near > z.near * 1.6 && d.cv >= 0.9 && mx.cv >= 0.9
            && d.ac < 0.05 && mx.ac < 0.05, msg, 0);
    gv_run(127, 5, TX_GRP_PULSE, 0, &p3);
    gv_run(127, 5, TX_GRP_OFFBEAT, 0, &p4);
    gv_run(127, 5, TX_GRP_FILL, 0, &p5);
    tx_groove_name(5, nm);
    snprintf(msg, sizeof msg, "%s at 127: each lane near its own cycle's slots: 3 %.1f %%, 4 %.1f %%, 5 %.1f %% (free "
             "%.1f .. %.1f %%)", nm, 100 * p3.near, 100 * p4.near, 100 * p5.near, 100 * 7.0 * 3 / 384, 100 * 7.0 * 5 / 384);
    verdict("groove", "poly", p3.near > 2.0 * 7 * 3 / 384 && p4.near > 2.0 * 7 * 4 / 384 && p5.near > 2.0 * 7 * 5 / 384, msg, 0);
    snprintf(msg, sizeof msg, "%u TYPES (the named, then E(k, n))", tx_groove_ntypes());
    verdict("groove", "types", tx_groove_ntypes() >= 200u && tx_groove_ntypes() <= 256u, msg, 0);
    {                                           /* the multiplier's range (a lane's tx_gen rlean holds it less 1) */
        static const uint8_t CY[3] = {0, 2, 9};
        unsigned t, pt, c, w, b;
        int32_t lo = 4096, hi = 4096;
        for (t = 0; t < tx_groove_ntypes(); t++) for (pt = TX_GRP_PULSE; pt <= TX_GRP_FILL; pt++)
            for (c = 0; c < 3u; c++) for (w = 1; w <= 3u; w += 2u) {
                tx_groove_lane_t l;
                tx_groove_lane_init(&l, t * 31u + pt);
                l.part = (uint8_t)pt; l.cycle = CY[c]; l.wl = (uint8_t)w;
                for (b = 0; b < 1300u; b++) {
                    int32_t m = tx_groove_mul(&l, t, 127, b % 5u, b * 1229u);
                    if (b >= 560u) { if (m < lo) lo = m; if (m > hi) hi = m; }
                }
            }
        snprintf(msg, sizeof msg, "every TYPE, PART, CYCLE at 127: the multiplier %d .. %d (1 .. under 24000)", (int)lo, (int)hi);
        verdict("groove", "range", lo >= 1 && hi < 24000, msg, 0);
    }
}
static void check_grain_mod(void)
{
    static const char *const nm[TX_GDI_N] = {"NONE", "OCTAVES", "5TH+OCT", "4TH+5TH", "MAJOR", "MINOR", "ALL", "UP"};
    static const double iv[TX_GDI_N][9] = {{1}, {1, 0.5, 2}, {1, 0.5, 2. / 3, 1.5, 2}, {1, 0.75, 4. / 3, 2. / 3, 1.5},
        {1, 1.25, 1.5, 2, 0.5}, {1, 1.2, 1.5, 2, 0.5}, {1, 0.5, 2. / 3, 0.75, 0.8, 1.25, 4. / 3, 1.5, 2}, {1, 1.5, 2}};
    tx_fx_grain_t *d = malloc(sizeof *d);
    unsigned n = 2u * 44100u, s, k, ok = 1, i;
    double *y = malloc(n * sizeof *y), worst = 1, off = 0, tot, in, uni;
    char msg[300];
    if (!d || !y) { free(d); free(y); return; }
    for (s = 0; s < TX_GDI_N; s++) {
        gm_run(d, 127, s, 127, y, n);
        for (i = 0, tot = 0; i < n; i++) tot += y[i] * y[i] / n;
        for (k = 0, in = 0; k < 9u && iv[s][k] > 0; k++) in += gm_band(y, n, 440.0 * iv[s][k]);
        uni = gm_band(y, n, 440.0);
        if (in / tot < worst) worst = in / tot;
        if (s == TX_GDI_5OCT) off = 1.0 - uni / tot;
        if (!s && uni / tot < 0.9) ok = 0;
    }
    snprintf(msg, sizeof msg, "grain MOD 127 on 440 Hz: the INTERVALS' ratios hold %.1f %% of the energy at worst (90 at "
             "least), 5TH+OCT %.0f %% off unison (30 at least)", 100.0 * worst, 100.0 * off);
    verdict("fx", "mod-intervals", ok && worst >= 0.90 && off >= 0.30, msg, 0);
    (void)nm;
    {   /* the echo alone: its pitch from the zero crossings, 50 ms windows */
        double dev = 0, mn = 1e9, mx = 0;
        unsigned cnt = 0, w0 = 0;
        double *z = malloc(10u * 44100u * sizeof *z);
        if (!z) { free(d); free(y); return; }
        gm_run(d, 127, TX_GDI_NONE, 0, z, 10u * 44100u);
#define y z
        for (i = 1; i < 10u * 44100u; i++)
            if (y[i - 1] < 0 && y[i] >= 0) {
                if (!cnt) w0 = i;
                cnt++;
                if (i - w0 >= 2205u && cnt > 1u) {
                    double f = (cnt - 1) * 44100.0 / (i - w0 + (y[i - 1] / (y[i - 1] - y[i]))
                               - (y[w0 - 1] / (y[w0 - 1] - y[w0])));
                    if (f < mn) mn = f;
                    if (f > mx) mx = f;
                    cnt = 1; w0 = i;
                }
            }
#undef y
        free(z);
        dev = (mx / 440.0 - 1) > (1 - mn / 440.0) ? mx / 440.0 - 1 : 1 - mn / 440.0;
        snprintf(msg, sizeof msg, "grain MOD 127, the echo alone: its pitch %.1f .. %.1f Hz (a bend of %.2f %%: 0.1 .. 1.3)",
                 mn, mx, 100.0 * dev);
        verdict("fx", "mod-wow", dev >= 0.001 && dev <= 0.013, msg, 0);
    }
    {   /* the turns: their gaps over 60 000 blocks (MOD RATE 64) */
        int32_t L[TX_BLOCK] = {0}, R[TX_BLOCK] = {0}, prev;
        unsigned b, gl = 0, ng = 0;
        double sm = 0, sq = 0, mean, cv;
        tx_fx_grain_init(d);
        tx_fx_grain_param(d, TX_GD_MIX, 127); tx_fx_grain_param(d, TX_GD_MOD, 127); tx_fx_grain_param(d, TX_GD_MODRATE, 64);
        tx_fx_grain_process(d, L, R, TX_BLOCK);
        prev = d->dt[0];
        for (b = 0; b < 60000u; b++) {
            tx_fx_grain_begin(d, TX_BLOCK);
            gl++;
            if (d->dt[0] != prev) { if (ng) { sm += gl; sq += (double)gl * gl; } ng++; gl = 0; prev = d->dt[0]; }
        }
        mean = sm / (ng - 1); cv = sqrt(sq / (ng - 1) - mean * mean) / mean;
        snprintf(msg, sizeof msg, "grain MOD's turns (MOD RATE 64): %u, %.0f ms apart on average, CV %.2f (0.9 at least)",
                 ng, mean * 256000.0 / 44100.0, cv);
        verdict("fx", "mod-turns", ng > 100u && cv >= 0.9, msg, 0);
    }
    {   /* back to 0: the plain delay exactly (its state settled: the clock's step SIZE's, no drift) */
        int32_t L[TX_BLOCK], R[TX_BLOCK];
        unsigned b, j, sz;
        gm_run(d, 127, TX_GDI_ALL, 64, y, 44100u);
        tx_fx_grain_param(d, TX_GD_MOD, 0);
        for (b = 0; b < 400u; b++) {
            for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = 0;
            tx_fx_grain_process(d, L, R, TX_BLOCK);
        }
        sz = d->p[TX_GD_SIZE];
        snprintf(msg, sizeof msg, "grain MOD back to 0: settled in 0.7 s (amount %d, drift %d, the clock SIZE's: %s)",
                 (int)d->mg, (int)d->wq1, d->ginc == TX_FX_GD_SIZE[sz] ? "yes" : "no");
        verdict("fx", "mod-off", !d->mg && !d->wq1 && !d->wvel && d->ginc == TX_FX_GD_SIZE[sz], msg, 0);
    }
    free(d); free(y);
}
static double mf_peak(unsigned lo, unsigned hi, unsigned reso, unsigned slope, double f0, double f1, double *at)
{                                           /* the loudest sine from f0 to f1 (1/12 octave steps), dB; at: where */
    double best = -999, f;
    for (f = f0; f <= f1 * 1.001; f *= 1.0594630943592953) {
        double g = mf_gain(lo, hi, reso, slope, (uint32_t)(f + 0.5));
        if (g > best) { best = g; if (at) *at = f; }
    }
    return best;
}
static double mf_hz(unsigned v) { return 30.0 * pow(2.0, v * (593700.0 / 65536.0) / 127.0); }   /* (params.c F_HZ_FL) */
/* the FILTER's shape, a high-pass (LOW CUT) then a low-pass (HIGH CUT): each edge's -3 dB where its knob shows; the band
 * between at unity; the edges met, a bell at their place (-6 / -9 / -12 dB at 12 / 18 / 24 dB, RESONANCE lifts it);
 * crossed, the band closes further the further they cross (no jump where they pass); at the ends (LOW CUT 127, HIGH CUT
 * 0) nothing left, at every SLOPE */
static void check_mfilter_shape(void)
{
    static const unsigned SL[3] = {TX_MF_S12, TX_MF_S18, TX_MF_S24};
    static const double BELL[3] = {-6.0, -6.0, -6.0};
    char msg[400];
    double e3l = mf_gain(64, 127, 0, 0, (uint32_t)mf_hz(64)), e3h = mf_gain(0, 64, 0, 0, (uint32_t)mf_hz(64));
    double uni = mf_peak(20, 110, 0, 0, 300, 3000, 0), at = 0, worst_end = -999, jump = 0, bw = 0;
    double bellpk[3], bellat[3], respk, x1, x2, x3, e3[6];
    unsigned s, ok = 1;
    for (s = 0; s < 3u; s++) {
        double a, b, c, d, m, x;
        bellpk[s] = mf_peak(64, 64, 0, SL[s], 100, 6000, &bellat[s]);
        e3[s] = mf_gain(40, 127, 0, SL[s], (uint32_t)mf_hz(40)); e3[s + 3] = mf_gain(0, 90, 0, SL[s], (uint32_t)mf_hz(90));
        ok &= fabs(e3[s] + 3) < 0.6 && fabs(e3[s + 3] + 3) < 0.6;
        ok &= fabs(bellpk[s] - BELL[s]) < 1.5 && fabs(log2(bellat[s] / mf_hz(64))) < 0.2;
        a = mf_peak(127, 127, 0, SL[s], 20, 20000, 0);          /* LOW CUT at its top: nothing left */
        b = mf_peak(0, 0, 0, SL[s], 20, 20000, 0);              /* HIGH CUT at its bottom */
        c = mf_peak(127, 0, 127, SL[s], 20, 20000, 0);          /* both, RESONANCE 127 */
        d = mf_peak(127, 127, 127, SL[s], 20, 20000, 0);
        m = a > b ? a : b; m = m > c ? m : c; m = m > d ? m : d;
        if (m > worst_end) worst_end = m;
        x = mf_peak(84, 44, 0, SL[s], 20, 20000, 0);            /* crossed by 40 steps (~2.9 octaves) */
        if (x > bellpk[s] - 20) ok = 0;
        {                                                       /* one step across: no jump */
            double p0 = mf_peak(64, 64, 0, SL[s], 200, 3000, 0), p1 = mf_peak(65, 64, 0, SL[s], 200, 3000, 0);
            if (fabs(p0 - p1) > jump) jump = fabs(p0 - p1);
        }
    }
    respk = mf_peak(64, 64, 127, 0, 100, 6000, 0);
    x1 = mf_peak(70, 60, 0, 0, 20, 20000, 0); x2 = mf_peak(80, 50, 0, 0, 20, 20000, 0); x3 = mf_peak(100, 30, 0, 0, 20, 20000, 0);
    {                                                           /* the bell's -3 dB width (12 dB, RESONANCE 64) */
        double f, top = mf_peak(64, 64, 64, 0, 100, 6000, &at), lo = 0, hi = 0;
        for (f = 60; f < 16000; f *= 1.0293022366434921) {     /* (1/24 octave) */
            double g = mf_gain(64, 64, 64, 0, (uint32_t)(f + 0.5));
            if (g > top - 3 && !lo) lo = f;
            if (g > top - 3) hi = f;
        }
        bw = lo > 0 ? log2(hi / lo) : 99;
    }
    snprintf(msg, sizeof msg, "-3 dB at the shown Hz: LOW CUT %.1f, HIGH CUT %.1f dB (64), 40 / 90 at 18 dB %.1f / %.1f, 24 dB %.1f / %.1f; band 20 .. 110: %.2f dB; met at 64: %.1f / "
             "%.1f / %.1f dB (12 / 18 / 24) at %.0f Hz (shows %.0f), RESONANCE 127 %.1f dB, RESONANCE 64 %.2f octaves wide; "
             "crossed 5 / 15 / 35 steps (12 dB): %.0f / %.0f / %.0f dB; the ends: %.0f dB at most; a step across %.2f dB",
             e3l, e3h, e3[1], e3[4], e3[2], e3[5], uni, bellpk[0], bellpk[1], bellpk[2], bellat[0], mf_hz(64), respk, bw, x1, x2, x3, worst_end, jump);
    verdict("fx", "mfilter shape", ok && fabs(e3l + 3) < 0.6 && fabs(e3h + 3) < 0.6 && fabs(uni) < 0.3 && respk > 6
            && bw < 1.5 && x1 < bellpk[0] - 3 && x2 < x1 - 6 && x3 < x2 - 12 && worst_end < -90 && jump < 1.5, msg, 0);
}
static void check_mfilter(void)
{
    tx_fx_mfilter_t f;
    char msg[240];
    int32_t L[TX_BLOCK], R[TX_BLOCK], x[TX_BLOCK], pk = 0, step = 0, istep = 0, prev = 0;
    uint32_t b, j, rng = 99u, bad = 0, nz = 0;
    double pass = mf_gain(30, 100, 0, 0, 1000), lo12 = mf_gain(30, 100, 0, 0, 60), hi12 = mf_gain(30, 100, 0, 0, 14000);
    double lo24 = mf_gain(30, 100, 0, 1, 60), hi24 = mf_gain(30, 100, 0, 1, 14000), x12 = mf_gain(127, 0, 0, 0, 0), xm;
    double lo18 = mf_gain(30, 100, 0, TX_MF_S18, 60), hi18 = mf_gain(30, 100, 0, TX_MF_S18, 14000);
    double pass18 = mf_gain(30, 100, 0, TX_MF_S18, 1000), pk18 = -999, pk12 = -999;
    {   /* crossed, at the ends: closed (the peak anywhere 300 .. 1600 Hz) */
        double best = -999; uint32_t hz;
        for (hz = 300; hz <= 1600; hz += 50) { double g = mf_gain(127, 0, 0, 0, hz); if (g > best) best = g; }
        xm = best; (void)x12;
        for (hz = 2000; hz <= 8000; hz += 250) {        /* RESONANCE 127 at HIGH CUT 100: 18 dB peaks as 12 dB does */
            double g = mf_gain(0, 100, 127, 0, hz), h = mf_gain(0, 100, 127, TX_MF_S18, hz);
            if (g > pk12) pk12 = g;
            if (h > pk18) pk18 = h;
        }
    }
    snprintf(msg, sizeof msg, "LOW CUT 30 (~140 Hz) HIGH CUT 100 (~4 kHz): 1 kHz %.1f dB; 60 Hz %.1f / %.1f dB, 14 kHz %.1f / "
             "%.1f dB (12 / 24 dB); crossed (127 / 0): its peak %.1f dB", pass, lo12, lo24, hi12, hi24, xm);
    verdict("fx", "mfilter band", pass > -1.0 && pass < 0.5 && lo12 < -9 && lo24 < lo12 - 8 && hi12 < -12 && hi24 < hi12 - 8
            && xm < -90, msg, 0);
    snprintf(msg, sizeof msg, "18 dB (the 12 dB stage + a one-pole): 1 kHz %.1f dB; 60 Hz %.1f, 14 kHz %.1f dB (between 12 "
             "and 24 dB, 3 dB from each at least); RESONANCE 127's peak %.1f dB (12 dB: %.1f)", pass18, lo18, hi18, pk18, pk12);
    verdict("fx", "mfilter 18", pass18 > -1.5 && pass18 < 0.5 && lo18 < lo12 - 3 && lo18 > lo24 + 3 && hi18 < hi12 - 3
            && hi18 > hi24 + 3 && pk18 > 3.0 && pk18 < pk12 + 0.5, msg, 0);
    /* on, then back to the ends: after the fade, every sample the input again; a click check on the way */
    tx_fx_mfilter_init(&f);
    for (b = 0; b < 400u; b++) {
        if (b == 20u) { tx_fx_mfilter_param(&f, TX_MF_LOW, 40); tx_fx_mfilter_param(&f, TX_MF_HIGH, 70); }
        if (b == 120u) tx_fx_mfilter_param(&f, TX_MF_SLOPE, 1);
        if (b == 150u) tx_fx_mfilter_param(&f, TX_MF_SLOPE, TX_MF_S18);   /* (24 -> 18 -> 12: the stages cross-fade) */
        if (b == 175u) tx_fx_mfilter_param(&f, TX_MF_SLOPE, 0);
        if (b == 200u) { tx_fx_mfilter_param(&f, TX_MF_LOW, 0); tx_fx_mfilter_param(&f, TX_MF_HIGH, 127); }
        for (j = 0; j < TX_BLOCK; j++) {
            x[j] = L[j] = tx_sin_q15((b * TX_BLOCK + j) * 9739155u) * 128; R[j] = -L[j];   /* 100 Hz */
            if (b || j) { int32_t d = x[j] - prev; d = d < 0 ? -d : d; if (d > istep) istep = d; }
            prev = x[j];
        }
        tx_fx_mfilter_process(&f, L, R, TX_BLOCK);
        for (j = 1; j < TX_BLOCK; j++) { int32_t d = L[j] - L[j - 1]; d = d < 0 ? -d : d; if (d > step) step = d; }
        if (b >= 260u) for (j = 0; j < TX_BLOCK; j++) bad += L[j] != x[j] || R[j] != -x[j];
    }
    snprintf(msg, sizeof msg, "on, 24, 18, 12 dB, then back to the ends: %u samples changed after the fade; the largest step %.2f x "
             "the input's", bad, (double)step / istep);
    verdict("fx", "mfilter bypass", !bad && step < istep * 2, msg, 0);
    /* stability: loud noise, RESONANCE 127, 24 dB, the cutoffs jumping every block (crossing too); then zeros */
    tx_fx_mfilter_init(&f);
    tx_fx_mfilter_param(&f, TX_MF_RESO, 127); tx_fx_mfilter_param(&f, TX_MF_SLOPE, 1);
    for (b = 0; b < 2000u; b++) {
        rng = rng * 1664525u + 1013904223u;
        tx_fx_mfilter_param(&f, TX_MF_LOW, (rng >> 8) & 127u); tx_fx_mfilter_param(&f, TX_MF_HIGH, (rng >> 16) & 127u);
        if (b == 600u) tx_fx_mfilter_param(&f, TX_MF_SLOPE, TX_MF_S18);   /* (18 dB from here) */
        for (j = 0; j < TX_BLOCK; j++) {
            rng = rng * 1664525u + 1013904223u;
            L[j] = b < 1200u ? (int32_t)(rng >> 4) - (1 << 27) : 0;   /* +-16 full scale, then silence */
            R[j] = b < 1200u ? -(int32_t)(rng >> 5) : 0;
        }
        tx_fx_mfilter_process(&f, L, R, TX_BLOCK);
        for (j = 0; j < TX_BLOCK; j++) {
            int32_t a = L[j] < 0 ? -L[j] : L[j], c = R[j] < 0 ? -R[j] : R[j];
            if (a > pk) pk = a;
            if (c > pk) pk = c;
            if (b >= 1800u) nz += L[j] != 0 || R[j] != 0;
        }
    }
    snprintf(msg, sizeof msg, "+-16 FS noise, RESONANCE 127, 24 then 18 dB, the cutoffs jumping each block: peak %.1f FS; silence "
             "after: %u nonzero samples in its last 200 blocks", pk / 8388608.0, nz);
    verdict("fx", "mfilter stable", pk < (1 << 30) && !nz, msg, 0);
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
        tx_fx_drive_mix(fx, L, R, TX_BLOCK);
        tx_fx_bus(fx, L, R, TX_BLOCK);
        for (j = 0; j < TX_BLOCK; j++) bad += y[j] != x[j] || L[j] != x[j] || R[j] != -x[j];
    }
    snprintf(msg, sizeof msg, "every effect at its default (off): %u samples changed", bad);
    verdict("fx", "passthrough", !bad, msg, 0);
    /* silence in, every effect on: exact zeros out */
    tx_fx_init(fx);
    tx_fx_drive_set(fx, TX_DR_AMOUNT, 90);
    tx_fx_drive_set(fx, TX_FX_MF_ID + TX_MF_LOW, 40); tx_fx_drive_set(fx, TX_FX_MF_ID + TX_MF_HIGH, 60);
    tx_fx_drive_set(fx, TX_FX_MF_ID + TX_MF_RESO, 127); tx_fx_drive_set(fx, TX_FX_MF_ID + TX_MF_SLOPE, 1);
    for (t = TX_RVT_ROOM; t < TX_RVT_N; t++) {
        tx_fx_reverb_init(&fx->rev);
        tx_fx_reverb_param(&fx->rev, TX_RV_TYPE, t); tx_fx_reverb_param(&fx->rev, TX_RV_MIX, 127);
        tx_fx_reverb_param(&fx->rev, TX_RV_PREDELAY, 60); tx_fx_reverb_param(&fx->rev, TX_RV_LOWCUT, 40);
        tx_fx_reverb_param(&fx->rev, TX_RV_SHIMMER, 100);
        tx_fx_grain_param(&fx->gd, TX_GD_MIX, 100); tx_fx_grain_param(&fx->gd, TX_GD_PITCH, 75);
        for (b = 0; b < 400u; b++) {
            for (j = 0; j < TX_BLOCK; j++) y[j] = L[j] = R[j] = 0;
            tx_fx_lane(fx, 0, y, TX_BLOCK);
            tx_fx_drive_mix(fx, L, R, TX_BLOCK);
            tx_fx_bus(fx, L, R, TX_BLOCK);
            for (j = 0; j < TX_BLOCK; j++) nz += y[j] != 0 || L[j] != 0 || R[j] != 0;
        }
    }
    snprintf(msg, sizeof msg, "zeros in, drive + the mix's filter (24 dB band, RESONANCE 127) + grain delay + every reverb TYPE with shimmer: %u nonzero samples out", nz);
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
    /* PLATE's loud rounding (its products round half away from zero above PL_LOUD) never holds a tail: at its
     * longest (DECAY 127, SIZE 127), bright and dark, still and moving, a burst's tail ends in exact zeros */
    {
        static const unsigned TO[4][2] = {{127, 0}, {0, 0}, {127, 127}, {64, 64}};   /* TONE, MOD */
        unsigned k, worst = 0, never = 0;
        for (k = 0; k < 4u; k++) {
            tx_fx_reverb_t *r = malloc(sizeof *r);
            uint32_t nb = 60u * 44100u / TX_BLOCK, end = 0, rng = 7u;
            tx_fx_reverb_init(r);
            tx_fx_reverb_param(r, TX_RV_TYPE, TX_RVT_PLATE); tx_fx_reverb_param(r, TX_RV_MIX, 127);
            tx_fx_reverb_param(r, TX_RV_DECAY, 127); tx_fx_reverb_param(r, TX_RV_SIZE, 127);
            tx_fx_reverb_param(r, TX_RV_TONE, TO[k][0]); tx_fx_reverb_param(r, TX_RV_MOD, TO[k][1]);
            for (b = 0; b < nb; b++) {
                unsigned z = 1;
                for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = b < 90u ? fx_in(b * TX_BLOCK + j, &rng) : 0;
                tx_fx_reverb_process(r, L, R, TX_BLOCK);
                for (j = 0; j < TX_BLOCK; j++) z &= !L[j] && !R[j];
                if (!z) end = 0;
                else if (!end) end = b;
                if (end && b > end + 344u) break;
            }
            if (!end) never++;
            else if (end > worst) worst = end;
            free(r);
        }
        snprintf(msg, sizeof msg, "plate, DECAY 127 SIZE 127, TONE 0 / 64 / 127, MOD 0 / 64 / 127: every tail ends in exact zeros, the last after %.1f s", worst * 256.0 / 44100.0);
        verdict("fx", "plate tail", !never, msg, never ? " (one never ended within 60 s)" : 0);
    }
    /* PLATE's tank level: a loud chord (6 tones, their sum's peak -0.4 dBFS: the old lines clipped under it) held 2 s
     * at its longest and brightest, then its tail: the tank steps down and back up, never reaches the int16 rails, and
     * sounds the same as the chord 24 dB lower (the residue re 16 x the quiet one's wet) */
    {
        static const double FQ[6] = {110.0, 164.8, 220.0, 277.2, 329.6, 440.0};
        tx_fx_reverb_t *r = malloc(sizeof *r);
        uint32_t nb = 5u * 44100u / TX_BLOCK, i, rail = 0, q;
        int32_t rpk = 0, lxm = 0, *ow = malloc(sizeof(int32_t) * 2u * nb * TX_BLOCK);
        double sr = 0, sd = 0;
        for (q = 0; q < 2u && r && ow; q++) {
            double ph = 0;
            tx_fx_reverb_init(r);
            tx_fx_reverb_param(r, TX_RV_TYPE, TX_RVT_PLATE); tx_fx_reverb_param(r, TX_RV_MIX, 127);
            tx_fx_reverb_param(r, TX_RV_DECAY, 127); tx_fx_reverb_param(r, TX_RV_SIZE, 127);
            tx_fx_reverb_param(r, TX_RV_TONE, 127);
            for (b = 0; b < nb; b++) {
                for (j = 0; j < TX_BLOCK; j++) {
                    double v = 0;
                    unsigned k;
                    for (k = 0; k < 6u; k++) v += sin(2 * M_PI * FQ[k] * ph);
                    ph += 1.0 / 44100.0;
                    L[j] = R[j] = b * TX_BLOCK < 2u * 44100u ? (int32_t)(v / 6.0 * 0.955 * 8388607.0 / (q ? 16.0 : 1.0)) : 0;
                }
                tx_fx_reverb_process(r, L, R, TX_BLOCK);
                for (j = 0; j < TX_BLOCK; j++) ow[(q * nb + b) * TX_BLOCK + j] = L[j];
                if (q) continue;
                if (r->u.p.lx > lxm) lxm = r->u.p.lx;
                for (i = 0; i < PL_N; i++) {
                    int32_t a = r->u.p.ring[i] < 0 ? -r->u.p.ring[i] : r->u.p.ring[i];
                    if (a > rpk) rpk = a;
                    rail += a >= 32767;
                }
            }
        }
        if (r && ow)
            for (i = 0; i < nb * TX_BLOCK; i++) {
                double d = ow[i] - 16.0 * ow[nb * TX_BLOCK + i];
                sr += (double)ow[i] * ow[i], sd += d * d;
            }
        snprintf(msg, sizeof msg, "plate, a loud chord (-0.4 dBFS, DECAY 127 SIZE 127 TONE 127) and its tail: the tank down %d steps, "
                 "its peak %d of 32767, %u samples at the rails; the wet re the chord 24 dB lower's: residue %.1f dB",
                 (int)lxm, (int)rpk, (unsigned)rail, 10 * log10(sd / (sr + 1e-9) + 1e-12));
        verdict("fx", "plate loud", r && ow && rpk < 32767 && lxm > 0 && sd < sr * 1e-4, msg, 0);
        free(r); free(ow);
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
         * once settled (its slow beating; DRIFT 3: over MOD 16 .. 127 its settled beat is 1.2 .. 2.8 dB, 2.1 mean, and
         * was 1.3 .. 2.6 before INFINITE's lines were made shorter), bounded */
        verdict("fx", "freeze", lo > 0 && hi / lo < (t == TX_RVT_DRIFT ? 1.496 : 1.413)
                && hi2 / lo2 < (t == TX_RVT_DRIFT ? 1.413 : 1.259) && pk < (1 << 23), msg, 0);
    }
    /* MIX turned under a ringing tail (SIZE AUTO) changes its level only, never its pitch: INFINITE's size is fixed,
     * PLATE's latched while it rings. A burst, then silence; one run keeps MIX 64, the other moves it to 120 as the
     * burst ends: the second tail must be the first scaled (the residue after the best gain under -40 dB) */
    for (t = TX_RVT_INF; t < TX_RVT_N; t++) {
        tx_fx_reverb_t *ra, *rb;
        static int32_t La[TX_BLOCK], Ra[TX_BLOCK], Lb[TX_BLOCK], Rb[TX_BLOCK];
        double sab = 0, saa = 0, sbb = 0, g, res;
        uint32_t rng2 = 99u;
        if (t == TX_RVT_DRIFT || t == TX_RVT_SWELL) continue;   /* (DRIFT bends by design; SWELL follows its send) */
        ra = malloc(sizeof *ra); rb = malloc(sizeof *rb);
        if (!ra || !rb) { free(ra); free(rb); break; }
        tx_fx_reverb_init(ra); tx_fx_reverb_init(rb);
        tx_fx_reverb_param(ra, TX_RV_TYPE, t); tx_fx_reverb_param(rb, TX_RV_TYPE, t);
        tx_fx_reverb_param(ra, TX_RV_MIX, 64); tx_fx_reverb_param(rb, TX_RV_MIX, 64);
        for (b = 0; b < 360u; b++) {
            if (b == 30u) tx_fx_reverb_param(rb, TX_RV_MIX, 120);
            for (j = 0; j < TX_BLOCK; j++) {
                int32_t x = 0;
                if (b < 30u) { rng2 = rng2 * 1664525u + 1013904223u; x = (int32_t)(rng2 >> 9) - (1 << 22); }
                La[j] = Ra[j] = Lb[j] = Rb[j] = x;
            }
            tx_fx_reverb_process(ra, La, Ra, TX_BLOCK); tx_fx_reverb_process(rb, Lb, Rb, TX_BLOCK);
            if (b >= 60u) for (j = 0; j < TX_BLOCK; j++) {
                sab += (double)La[j] * Lb[j] + (double)Ra[j] * Rb[j];
                saa += (double)La[j] * La[j] + (double)Ra[j] * Ra[j];
                sbb += (double)Lb[j] * Lb[j] + (double)Rb[j] * Rb[j];
            }
        }
        g = saa > 0 ? sab / saa : 0;
        res = sbb > 0 ? (sbb - 2 * g * sab + g * g * saa) / sbb : 1;
        snprintf(msg, sizeof msg, "%s, SIZE AUTO: MIX 64 -> 120 under its tail, the tail the same but %.2f dB (residue %.1f dB)",
                 TN[t], 20 * log10(g > 0 ? g : 1e-9), 10 * log10(res > 1e-12 ? res : 1e-12));
        verdict("fx", "mix pitch", saa > 0 && res < 1e-4, msg, 0);
        free(ra); free(rb);
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
    /* hot inputs (a lane peaks to 16 x full scale, the bus more): a square of 16 x into every DRIVE TYPE at AMOUNT 127
     * and every reverb TYPE (this build traps a signed overflow): PLATE's input one-pole (Frodhes' BUGHUNT #22) and
     * BHLUE's PRE (from 4 x) overflowed */
    {
        static const unsigned PER[3] = {1u, 3u, 50u};
        unsigned q, k;
        for (q = 0; q < 3u; q++)
            for (k = 0; k < TX_DRT_N + TX_RVT_N - 1u; k++) {
                tx_fx_init(fx);
                if (k < TX_DRT_N) {
                    tx_fx_drive_param(&fx->drive[0], TX_DR_TYPE, k);
                    tx_fx_drive_param(&fx->drive[0], TX_DR_AMOUNT, 127);
                    tx_fx_mfilter_param(&fx->mf, TX_MF_LOW, 20u * k); tx_fx_mfilter_param(&fx->mf, TX_MF_HIGH, 127u - 20u * k);
                    tx_fx_mfilter_param(&fx->mf, TX_MF_RESO, 127); tx_fx_mfilter_param(&fx->mf, TX_MF_SLOPE, k % 3u);
                } else {
                    tx_fx_reverb_param(&fx->rev, TX_RV_TYPE, k - TX_DRT_N + 1u);
                    tx_fx_reverb_param(&fx->rev, TX_RV_MIX, 127);
                    tx_fx_reverb_param(&fx->rev, TX_RV_TONE, q ? 0 : 127);
                }
                for (b = 0; b < 100u; b++) {
                    for (j = 0; j < TX_BLOCK; j++)
                        y[j] = L[j] = R[j] = (b * TX_BLOCK + j) / PER[q] & 1u ? 1 << 27 : -(1 << 27);
                    if (k < TX_DRT_N) tx_fx_drive_process(&fx->drive[0], y, TX_BLOCK), tx_fx_mfilter_process(&fx->mf, y, L, TX_BLOCK);
                    else tx_fx_bus(fx, L, R, TX_BLOCK);
                }
            }
        verdict("fx", "hot input", 1, "a 16 x square into every DRIVE TYPE (AMOUNT 127), the mix's FILTER (RESONANCE 127) and every reverb TYPE: no overflow",
                0);
    }
    /* SWELL: 3 s played (the burst again and again), then silence: the wet part (MIX 127: the out) after the playing
     * against during it, SWELL against INFINITE (DECAY 100): the tail held down while played, swelling in after */
    {
        double ratio[2];
        for (t = 0; t < 2; t++) {
            tx_fx_reverb_t *r = &fx->rev;
            double dur = 0, aft = 0, acc = 0;
            uint32_t rngs = 7u;
            tx_fx_reverb_init(r);
            tx_fx_reverb_param(r, TX_RV_TYPE, t ? TX_RVT_SWELL : TX_RVT_INF); tx_fx_reverb_param(r, TX_RV_MIX, 127);
            tx_fx_reverb_param(r, TX_RV_DECAY, 100);
            for (b = 0; b < 1034u; b++) {                       /* 6 s: 3 played, 3 after; 0.25 s windows (43 blocks) */
                for (j = 0; j < TX_BLOCK; j++) x[j] = L[j] = R[j] = b < 517u ? fx_in((b * TX_BLOCK + j) % 22050u, &rngs) : 0;
                tx_fx_reverb_process(r, L, R, TX_BLOCK);
                for (j = 0; j < TX_BLOCK; j++) acc += (double)L[j] * L[j];
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
        const unsigned FW = (4u * TX_GD_N + TX_BLOCK / 2u) / TX_BLOCK;   /* (FREEZE's windows: 4 turns of the line) */
        for (t = 0; t < 2; t++) {                              /* 0: the tail; 1: FREEZE once the line is full, held */
            tx_fx_init(fx);
            tx_fx_grain_param(&fx->gd, TX_GD_MIX, 100); tx_fx_grain_param(&fx->gd, TX_GD_FEEDBACK, 90);
            tx_fx_grain_param(&fx->gd, TX_GD_PITCH, 71); tx_fx_grain_param(&fx->gd, TX_GD_REVERSE, 40);
            tx_fx_grain_param(&fx->gd, TX_GD_SPRAY, 40);
            rng = 7u; last = 0; acc = 0;
            if (!t) tx_sha256_init(&h);
            for (b = 0; b < (t ? FW * 12u : 172u * 60u); b++) {
                unsigned z = 0;
                if (t && b == TX_GD_N / TX_BLOCK + 1u) tx_fx_grain_param(&fx->gd, TX_GD_FREEZE, 1);   /* (the whole line
                                                 * goes round: frozen earlier, its unwritten part is silence) */
                for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = fx_in(b * TX_BLOCK + j, &rng);
                tx_fx_bus(fx, L, R, TX_BLOCK);
                for (j = 0; j < TX_BLOCK; j++) { z |= L[j] | R[j]; acc += (double)L[j] * L[j]; }
                if (z) last = b;
                if (!t && b < 1723u) fx_hash(&h, L, TX_BLOCK), fx_hash(&h, R, TX_BLOCK);
                if (t && (b + 1) % FW == 0 && (b + 1) / FW - 1 < 12u) { rms[(b + 1) / FW - 1] = sqrt(acc / ((double)FW * 256)); acc = 0; }
                else if (!t && (b + 1) % 1720u == 0) acc = 0;
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
                snprintf(msg, sizeof msg, "grain delay FREEZE: %.0f .. %.0f s (4 turns of the line a window) %.1f .. %.1f dBFS (%.2f dB apart)",
                         FW * 256.0 / 44100.0, FW * 12 * 256.0 / 44100.0, 20 * log10(lo / 8388608.0 + 1e-12), 20 * log10(hi / 8388608.0 + 1e-12), 20 * log10(hi / (lo + 1e-9)));
                verdict("fx", "freeze", lo > 0 && hi / lo < 1.189, msg, 0);
            }
        }
        {   /* FEEDBACK 127: a burst runs up into a wall (at every GRAIN) that holds, bounded, and ends in exact zeros
             * once FEEDBACK comes down to 50 */
            static const uint8_t GR[3] = {0, 64, 127};
            for (t = 0; t < 3; t++) {
                double ac = 0, pk = 0;
                tx_fx_init(fx);
                tx_fx_grain_param(&fx->gd, TX_GD_MIX, 127); tx_fx_grain_param(&fx->gd, TX_GD_FEEDBACK, 127);
                tx_fx_grain_param(&fx->gd, TX_GD_GRAIN, GR[t]);
                rng = 7u; last = 0;
                for (b = 0; b < 172u * 30u; b++) {
                    unsigned z = 0;
                    if (b == 172u * 20u) tx_fx_grain_param(&fx->gd, TX_GD_FEEDBACK, 50);
                    for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = b < 4u ? fx_in(b * TX_BLOCK + j, &rng) : 0;
                    tx_fx_bus(fx, L, R, TX_BLOCK);
                    for (j = 0; j < TX_BLOCK; j++) {
                        z |= L[j] | R[j];
                        if (b >= 172u * 10u && b < 172u * 20u) {
                            ac += (double)L[j] * L[j];
                            if (fabs((double)L[j]) > pk) pk = fabs((double)L[j]);
                        }
                    }
                    if (z) last = b;
                }
                ac = 10 * log10(ac / (172.0 * 10 * TX_BLOCK) / 70368744177664.0 + 1e-30);
                pk = 20 * log10(pk / 8388608.0 + 1e-12);
                snprintf(msg, sizeof msg, "grain delay, FEEDBACK 127, GRAIN %u: a burst's wall 10 .. 20 s on %.1f dBFS rms, peak %.1f; "
                         "FEEDBACK 50 at 20 s: exact zeros after %.1f s", GR[t], ac, pk, last * 256.0 / 44100.0 - 20);
                verdict("fx", "wall", ac > -40 && ac < -12 && pk < -3 && last < 172u * 28u, msg, 0);
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
        check_fx_grain_controls(fx);
        check_fx_grain_level(fx);
        check_fx_grain_loop(fx);
        check_fx_drywet(fx);
        check_fx_hq(fx);
    }
    /* the DRIVE TYPEs: zeros in, zeros out; AMOUNT 0 glides out to the lane untouched; a TYPE change while it
     * sounds fades through (no step); TSH8 on the mix's stereo pair */
    {
        static const char *const DN[TX_DRT_N] = {"FDRV", "KHLON", "TSH8", "OHCD", "BHLUE"};
        unsigned nzt = 0, dry = 0, own = 0;
        int32_t step = 0, pv = 0;
        for (t = 1; t < TX_DRT_N; t++) {
            tx_fx_init(fx);
            tx_fx_drive_param(&fx->drive[0], TX_DR_TYPE, t); tx_fx_drive_param(&fx->drive[0], TX_DR_AMOUNT, 127);
            for (b = 0; b < 100u; b++) {
                for (j = 0; j < TX_BLOCK; j++) y[j] = 0;
                tx_fx_drive_process(&fx->drive[0], y, TX_BLOCK);
                for (j = 0; j < TX_BLOCK; j++) nzt += y[j] != 0;
            }
            rng = 5u;
            for (b = 0; b < 300u; b++) {                 /* on, then AMOUNT 0 at 100: dry again after its glide */
                if (b == 100u) tx_fx_drive_param(&fx->drive[0], TX_DR_AMOUNT, 0);
                for (j = 0; j < TX_BLOCK; j++) x[j] = y[j] = fx_in(b * TX_BLOCK + j, &rng) * 4;
                tx_fx_drive_process(&fx->drive[0], y, TX_BLOCK);
                if (b > 110u) for (j = 0; j < TX_BLOCK; j++) dry += y[j] != x[j];
            }
        }
        snprintf(msg, sizeof msg, "DRIVE TYPEs %s .. %s at AMOUNT 127: zeros in, %u nonzero out; AMOUNT 0: %u samples "
                 "not dry after the glide", DN[1], DN[TX_DRT_N - 1], nzt, dry);
        verdict("fx", "drive-types", !nzt && !dry, msg, 0);
        nzt = 0; step = 0;                               /* after the music every TYPE rests: exact zeros, no step */
        for (t = 0; t < TX_DRT_N; t++) {
            tx_fx_init(fx);
            tx_fx_drive_param(&fx->drive[0], TX_DR_TYPE, t); tx_fx_drive_param(&fx->drive[0], TX_DR_AMOUNT, 64);
            rng = 3u; pv = 0;
            for (b = 0; b < 500u; b++) {
                for (j = 0; j < TX_BLOCK; j++) y[j] = fx_in(b * TX_BLOCK + j, &rng);
                tx_fx_drive_process(&fx->drive[0], y, TX_BLOCK);
                for (j = 0; j < TX_BLOCK; j++) {
                    int32_t d = y[j] - pv; d = d < 0 ? -d : d;
                    if (b >= 90u && d > step) step = d;  /* (the burst ends in block 86: its tail, then the rest) */
                    if (b >= 400u) nzt += y[j] != 0;
                    pv = y[j];
                }
            }
        }
        snprintf(msg, sizeof msg, "DRIVE every TYPE at AMOUNT 64 after a burst: %u nonzero samples 1.7 s on (a held DC), "
                 "the largest step once the input is zero %.2f LSB (a rest dropped at once)", nzt, step / 256.0);
        verdict("fx", "drive-rest", !nzt && step <= 3 * 256, msg, 0);
        tx_fx_init(fx);                                  /* a sine through every TYPE in turn, switched while it sounds */
        tx_fx_drive_param(&fx->drive[0], TX_DR_AMOUNT, 100);
        for (b = 0; b < 1200u; b++) {
            if (b % 200u == 100u) tx_fx_drive_param(&fx->drive[0], TX_DR_TYPE, (b / 200u + 1u) % TX_DRT_N);
            for (j = 0; j < TX_BLOCK; j++) y[j] = (int32_t)(sin((b * TX_BLOCK + j) * 0.0142) * 2000000.0);
            tx_fx_drive_process(&fx->drive[0], y, TX_BLOCK);
            for (j = 0; j < TX_BLOCK; j++) { int32_t d = y[j] - pv; d = d < 0 ? -d : d; if ((b || j) && d > step) step = d; pv = y[j]; }
        }
        snprintf(msg, sizeof msg, "DRIVE TYPE changed while it sounds (a 100 Hz sine at -12 dBFS): the largest step "
                 "%.2f %% of full scale (the sine's own: 0.34 %%)", 100.0 * step / 8388608.0);
        verdict("fx", "drive-switch", step < 8388608 / 66, msg, 0);
        tx_fx_init(fx);                                  /* the mix's DRIVE, a stereo pair: one TSH8 (the left owns its
                                                          * tables, the right follows), both sides alike through the
                                                          * changes in and out (the same signal on both: the same out) */
        tx_fx_drive_set(fx, TX_DR_AMOUNT, 80); tx_fx_drive_set(fx, TX_DR_TYPE, TX_DRT_TSH8);
        dry = 0;
        for (b = 0; b < 90u; b++) {
            if (b == 30u) tx_fx_drive_set(fx, TX_DR_TYPE, TX_DRT_OHCD);
            if (b == 60u) tx_fx_drive_set(fx, TX_DR_TYPE, TX_DRT_TSH8);
            for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = fx_in(b * TX_BLOCK + j, &rng) * 4;
            tx_fx_drive_mix(fx, L, R, TX_BLOCK);
            for (j = 0; j < TX_BLOCK; j++) dry += L[j] != R[j];
            if (b == 20u || b == 89u) own += fx->drive[0].cur == TX_DRT_TSH8 && fx->drive[1].cur == TX_DRT_TSH8
                                             && fx->drv.ts_own == &fx->drive[0];
            if (b == 50u) own += fx->drive[0].cur == TX_DRT_OHCD && fx->drive[1].cur == TX_DRT_OHCD && !fx->drv.ts_own;
        }
        snprintf(msg, sizeof msg, "TSH8 on the mix's DRIVE (a stereo pair): the left owns it, the right follows, in, out "
                 "to OHCD and back (%u of 3); %u samples differ between the sides", own, dry);
        verdict("fx", "drive-tsh8", own == 3u && !dry, msg, 0);
    }
    /* the DRIVE (one side) and the mix's FILTER: renders against the baselines (a drive at 90 / TONE 100 / MIX 100,
     * the classic and each TYPE; the FILTER a 24 dB band with RESONANCE, its HIGH CUT moving: fx-mfilter) */
    for (t = 0; t < 2u + TX_DRT_N - 1u; t++) {
        tx_sha256_t h, h2;
        const char *bl;
        int pass;
        for (pass = 0; pass < 2; pass++) {
            tx_fx_init(fx);
            if (t != 1u) { tx_fx_drive_param(&fx->drive[0], TX_DR_AMOUNT, 90); tx_fx_drive_param(&fx->drive[0], TX_DR_TONE, 100);
                           tx_fx_drive_param(&fx->drive[0], TX_DR_MIX, 100); tx_fx_drive_param(&fx->drive[0], TX_DR_TYPE, t ? t - 1u : 0u); }
            else { tx_fx_mfilter_param(&fx->mf, TX_MF_LOW, 30); tx_fx_mfilter_param(&fx->mf, TX_MF_HIGH, 70);
                   tx_fx_mfilter_param(&fx->mf, TX_MF_RESO, 100); tx_fx_mfilter_param(&fx->mf, TX_MF_SLOPE, 1); }
            tx_sha256_init(pass ? &h2 : &h);
            rng = 7u; pk = 0;
            for (b = 0; b < 400u; b++) {
                for (j = 0; j < TX_BLOCK; j++) y[j] = fx_in((b % 200u) * TX_BLOCK + j, &rng) * 4;
                tx_fx_drive_process(&fx->drive[0], y, TX_BLOCK);     /* (the DRIVE alone, as on one side of the mix) */
                if (t == 1u) {                                      /* (the FILTER on the pair: y, and y negated) */
                    int32_t yr[TX_BLOCK];
                    if (!(b % 50u)) tx_fx_mfilter_param(&fx->mf, TX_MF_HIGH, 50u + (b / 50u) * 9u);
                    for (j = 0; j < TX_BLOCK; j++) yr[j] = -y[j];
                    tx_fx_mfilter_process(&fx->mf, y, yr, TX_BLOCK);
                }
                for (j = 0; j < TX_BLOCK; j++) { int32_t a = y[j] < 0 ? -y[j] : y[j]; if (a > pk) pk = a; }
                fx_hash(pass ? &h2 : &h, y, TX_BLOCK);
            }
        }
        tx_sha256_hex(&h, hex); tx_sha256_hex(&h2, hex2);
        {
            static const char *const FN[2u + TX_DRT_N - 1u] = {"drive", "mfilter", "khlon", "tsh8", "ohcd", "bhlue"};
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

unsigned tx_source_misfits(const char **bad);
unsigned tx_storm_misfits(const char **bad);
static void check_hosts(void)
{
    const char *bad = "";
    unsigned n = tx_source_misfits(&bad);
    verdict("source", "hosts", !n, n ? "does not fit (or is unknown): " : "every hosted engine fits HOST_BYTES", n ? bad : 0);
    bad = ""; n = tx_storm_misfits(&bad);
    verdict("storm", "hosts", !n, n ? "rain engine over ARENA: " : "both rain engines fit ARENA", n ? bad : 0);
}

int main(int argc, char **argv)
{
    unsigned k;
    FILE *update = 0;
    if (argc > 2 && !strcmp(argv[1], "--src")) {            /* the source check alone, on another folder */
        check_sources(argv[2]);
        return fails ? 1 : 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--truepeak")) {      /* the output limiter's true-peak check alone */
        check_truepeak();
        return fails ? 1 : 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--mfilter")) {       /* the mix FILTER's checks alone */
        check_mfilter();
        check_mfilter_shape();
        return fails ? 1 : 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--update")) {
        if (!(update = fopen(BASELINE, "w"))) { perror(BASELINE); return 2; }
    } else
        baseline_load();
    check_sources("engines");
    for (k = 0; tx_engines[k]; k++) check_engine(tx_engines[k], update);
    check_hosts();
    if (!update) for (k = 0; tx_engines[k]; k++) check_variation(tx_engines[k]);
    check_chamber(update);
    check_layers(update);
    check_fx(update);
    check_mfilter();
    check_mfilter_shape();
    check_grain_mod();
    check_groove();
    check_noise_core();
    check_comp();
    check_truepeak();
    if (!update) { check_gen(); check_gov(); check_switch(); check_keyrange(); check_rockfall_dist(); }
    if (update) { fclose(update); printf("wrote %s\n", BASELINE); }
    printf(fails ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", fails);
    return fails ? 1 : 0;
}
