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
static char base_names[256][48], base_hex[256][65];
static unsigned base_n;
static void baseline_load(void)
{
    FILE *f = fopen(BASELINE, "r");
    char line[256];
    if (!f) return;
    while (base_n < 256 && fgets(line, sizeof line, f))
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
    check_chamber(update);
    check_layers(update);
    check_noise_core();
    if (update) { fclose(update); printf("wrote %s\n", BASELINE); }
    printf(fails ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", fails);
    return fails ? 1 : 0;
}
