/* SPDX-License-Identifier: GPL-3.0-only */
/* txaudit: three audits of every landscape sound, on our own renders only (the source held at key 60, velocity 100,
 * PARTICLES 6, the way a lane plays it with Notes off), at the defaults and at ACTIVITY 0 and 127 (the classic engines:
 * the defaults), SEEDS runs of SECS s each (seeds 1 .. SEEDS):
 *
 *   beat    no hidden steady rhythm. Per band (<300, 0.3-2k, 2-8k, >8k Hz) the 10 ms rms envelope (after 3 s: the start
 *           is not a beat), its modulation spectrum 0.2 .. 20 Hz, each line's prominence over its local floor (the median
 *           over f / 1.5 .. 1.5 f), dB; a run's 4 strongest lines per band over 10 dB are kept (bands within 25 dB of
 *           the run's loudest). One run's strongest line reaches ~11 dB by chance and single lines near 17 dB turn up
 *           across thousands of tests, so a line counts only when the seeds agree on it: BEAT when two thirds of the
 *           seeds (3 at least) have a line within 6 % of one frequency, their median prominence 12 dB or more (the
 *           steady beats we have fixed read 12-22 dB, most seeds at one frequency).
 *   alike   no two events alike. Onsets (2.5 ms frames after a 200 Hz high-pass: 9 dB over the last 20 ms' minimum,
 *           within 35 dB of the render's top, 30 ms apart, after the first second), merged inside one ring (a new onset
 *           counts only when the level, smoothed over 10 ms, fell 15 dB under the last event's peak since: a ring's own
 *           beating is not a new event). Each event's first 60 ms, mean removed, unit norm; every pair's largest
 *           normalized cross-correlation over lags of +-3 ms. ALIKE when, over the seeds, more than 4 % of the pairs
 *           correlate over 0.95 or the events fall into fewer than 0.4 shapes per event (shapes: events joined at
 *           0.95), given 40 events or more. Natural sources stay at 0 .. 5.6 % of pairs over 0.95 (a bird's call at the
 *           top), 0.47 shapes per event at the fewest (a cricket's chirps).
 *   level   never silent, never hot. SILENT when half the seeds or more never reach AUDIBLE_DB (a 10 ms rms) in the
 *           whole run: most times the sound is switched on, 30 s of nothing (the user's rule); the lowest corners are
 *           the test. HOT when a seed's peak passes HOT_DB: a source may pass full scale (the mix's 8 bits of headroom,
 *           the output limiter), but past +24 dBFS the limiter's estimate clamps, and four lanes that loud near the
 *           headroom.
 *
 * Classic engines: their defaults; under -x also ACTIVITY 0 / 127 through the player's activity map (LS_QUICK,
 * generated from app.js QUICK: its parameters at C4 / C6, a one-shot pressed after the map's gaps).
 *
 * A short allowlist (ALLOW) keeps a species' own rhythm or call (birds, insects, a singing dune), each with its
 * reason; OPEN lists a known finding whose fix is to come (reported, not failed); everything else that is flagged
 * fails (exit 1).
 *
 *   txaudit [-s SECS] [-n SEEDS] [-j THREADS] [-f SUBSTRING] [-v] [-d] [-x] [-e]   (defaults 33 s, 6 seeds, every
 *            core, every sound; -v: every row, not only the flagged and allowed ones; -d: each seed's lines and events
 *            too; -x: the corners EXTRA 0 / 127 and COUNT 1 / 127 too, the classic engines' ACTIVITY 0 / 127; -e: the
 *            classic engines no landscape plays; with -x or -e, 8 seeds unless -n says) */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tx_host.h"
#include "../firmware/src/landscapes.h"   /* LS_QUICK: a classic engine's activity map (app.js QUICK) */

#define FS 44100.0
#define NBAND 4
#define NCOR 7
#define MAXSEED 8
#define BEAT_DB 12.0                /* the agreeing lines' median prominence, dB */
#define LINE_DB 10.0                /* a seed's line counts toward agreement from here */
#define NLINE 4                     /* lines kept per band and seed */
#define BEAT_AGREE 0.06
#define BEAT_RANGE_DB 25.0
#define ALIKE_X 0.95
#define ALIKE_SHARE 0.04
#define ALIKE_SHAPES 0.40
#define ALIKE_MIN_EV 40
#define RING_DB 15.0
#define MAXEV 100                   /* events per render (evenly picked when more) */
#define SEG 2646                    /* 60 ms */
#define PRE 44                      /* 1 ms before the onset */
#define LAG 132                     /* +-3 ms */
#define AUDIBLE_DB -60.0            /* a 10 ms rms this loud counts as heard (dBFS) */
#define HOT_DB 24.0                 /* a peak over this (dBFS) is HOT: the output limiter's estimate clamps there, and
                                     * four lanes that loud reach the mix's 8 bits of headroom */
#define NF 4096                     /* the correlation's FFT (>= SEG + LAG: no wrap) */

/* ---- the landscape sounds: name, source PRESET (or a classic engine's name), TYPE */
typedef struct { const char *name; int pre; const char *eng; unsigned type; } snd_t;
static const snd_t SND[] = {
    {"rain on water", 0, 0, 0}, {"rain on a roof", 0, 0, 50}, {"water drops", 3, 0, 0}, {"cave drips", 3, 0, 16},
    {"eaves dripping", 3, 0, 32}, {"brook", 3, 0, 48}, {"bubbling spring", 3, 0, 64}, {"waterfall", 3, 0, 80},
    {"waves on sand", 3, 0, 96}, {"waves on shingle", 3, 0, 112}, {"lake lapping", 3, 0, 120}, {"lake hull", 3, 0, 127},
    {"stones", 1, 0, 0}, {"rockfall", 1, 0, 32}, {"ringing rocks", 1, 0, 100}, {"gravel", 8, 0, 0},
    {"marbles", 8, 0, 96}, {"sand", 7, 0, 0}, {"singing dune", 7, 0, 96}, {"minerals", 13, 0, 0},
    {"stone friction", 13, 0, 96}, {"icicles", 11, 0, 8}, {"ice in a glass", 11, 0, 24}, {"frost tinkling", 11, 0, 40},
    {"snow underfoot", 11, 0, 56}, {"lake ice", 11, 0, 72}, {"frozen lake booming", 11, 0, 104},
    {"thin ice underfoot", 11, 0, 120}, {"glass pane", 14, 0, 0}, {"bottles", 14, 0, 64}, {"shards settling", 14, 0, 110},
    {"campfire", 5, 0, 0}, {"bonfire", 5, 0, 12}, {"fireplace", 5, 0, 20}, {"embers", 5, 0, 28}, {"kindling", 5, 0, 36},
    {"pine fire", 5, 0, 44}, {"leaves, paper", 5, 0, 52}, {"torch", 5, 0, 60}, {"flame", 5, 0, 96},
    {"sparks", -1, "sparks", 0}, {"spruce", 12, 0, 9}, {"pine", 12, 0, 27}, {"maple", 12, 0, 46}, {"oak", 12, 0, 64},
    {"green branch", 12, 0, 82}, {"tree trunk", 12, 0, 100}, {"twigs underfoot", 12, 0, 118}, {"wind", 4, 0, 0},
    {"wind in pines", 4, 0, 16}, {"leafy trees", 4, 0, 22}, {"open moor", 4, 0, 28}, {"sea wind", 4, 0, 35},
    {"ridge gale", 4, 0, 41}, {"gusty autumn", 4, 0, 48}, {"winter storm", 4, 0, 54}, {"desert wind", 4, 0, 61},
    {"storm", -1, "storm", 0}, {"thunder", -1, "thunder", 0}, {"hail on a roof", 0, 0, 103}, {"hail on a car", 0, 0, 110},
    {"hail in leaves", 0, 0, 117}, {"sleet on a window", 0, 0, 124}, {"field cricket", 6, 0, 10},
    {"tree cricket", 6, 0, 53}, {"mole cricket", 6, 0, 74}, {"cicada", 2, 0, 0}, {"small cicada", 2, 0, 20},
    {"cicada chorus", 2, 0, 64}, {"katydid", 9, 0, 16}, {"grasshopper", 9, 0, 80}, {"honeybee", 10, 0, 10},
    {"bumblebee", 10, 0, 53}, {"songbird", 15, 0, 0}, {"chirper", 16, 0, 0}, {"crow", 17, 0, 0}, {"gull", 18, 0, 0},
    {"blackbird", 19, 0, 0}, {"song thrush", 20, 0, 0}, {"nightingale", 21, 0, 0}, {"robin", 22, 0, 0},
    {"wren", 23, 0, 0}, {"hermit thrush", 24, 0, 0}, {"skylark", 25, 0, 0}, {"wood thrush", 26, 0, 0},
    {"reed warbler", 27, 0, 0}, {"curlew", 28, 0, 0}, {"oystercatcher", 29, 0, 0}, {"cuckoo", 30, 0, 0},
    {"woodpecker", 31, 0, 0}, {"wood pigeon", 32, 0, 0}, {"nightjar", 33, 0, 0}, {"bittern", 34, 0, 0},
};
#define NSND (sizeof SND / sizeof SND[0])
/* the corners: the defaults and ACTIVITY 0 / 127 always; -x adds EXTRA 0 / 127 and COUNT 1 / 127 (at the default
 * ACTIVITY; COUNT 0 is silence by design: no one sounds) */
typedef struct { const char *label, *param; unsigned val; } cor_t;
static const cor_t COR[NCOR] = {
    {"default", 0, 0}, {"ACT 0", "ACTIVITY", 0}, {"ACT 127", "ACTIVITY", 127},
    {"EXTRA 0", "EXTRA", 0}, {"EXTRA 127", "EXTRA", 127}, {"COUNT 1", "COUNT", 1}, {"COUNT 127", "COUNT", 127},
};
static unsigned ncor = 3;

/* -e: every classic engine in the build that no landscape sound plays (not the source, nor the test engine), at its
 * defaults, its key held and pressed again after random gaps (exponential, mean PRESS_S s, velocity 70 .. 126): a
 * one-shot engine sounds, and the presses keep no beat */
#define PRESS_S 1.5
#define MAXSND 160u
static snd_t all[MAXSND];
static unsigned nall;

/* ---- the allowlist: a species' own rhythm or call; test 1 = beat, 2 = alike, 3 = both */
typedef struct { const char *name; unsigned test; const char *why; } allow_t;
static const allow_t ALLOW[] = {
    {"field cricket", 1, "chirps at the cricket's own rate"},
    {"tree cricket", 1, "a trill: the wing's teeth struck at the cricket's rate"},
    {"cicada", 1, "the tymbal's pulse groups at the insect's rate"},
    {"small cicada", 1, "the tymbal's pulse groups at the insect's rate"},
    {"cicada chorus", 1, "a chorus pulls into a shared throb"},
    {"katydid", 1, "its call: a pulse train at the insect's own rate"},
    {"grasshopper", 1, "stridulation: the legs' strokes at the insect's own rate"},
    {"songbird", 1, "trills"},
    {"chirper", 1, "a chirp series"},
    {"gull", 1, "the long call: a series of notes at the bird's pace"},
    {"song thrush", 1, "each phrase repeated 2-4 times"},
    {"nightingale", 1, "trills"},
    {"wren", 1, "trills"},
    {"oystercatcher", 1, "the piping trill"},
    {"cuckoo", 3, "one two-note call, nearly pure, repeated at the bird's clock"},
    {"woodpecker", 1, "a drum roll: strikes at the bird's rate"},
    {"bittern", 2, "one boom, repeated in a series by the same throat"},
    {"singing dune", 1, "two avalanches singing apart: their tones beat"},
    {"spring", 1, "a kicked spring's echo: the wave's round trip along the coil (-e)"},
    {"chimes", 2, "one held key strikes one tube: its ring repeats (-e; the user's timbre, ~5-6 % of pairs)"},
    {"cicadas", 1, "the tymbal's pulse groups at the insect's rate (-e)"},
    {"katydids", 1, "its call: a pulse train at the insect's own rate (-e)"},
};
#define NALLOW (sizeof ALLOW / sizeof ALLOW[0])

/* ---- open findings: known, a fix to come; reported as "open", not failed, so the audit guards everything else
 * (test 4 = silent, 8 = hot). The silences are the hosted event engines' press clock with Notes off (key 60 held, the
 * fader the source's ACTIVITY: scape.c), where nothing presses but the clock: its mean gap at the corner is longer than
 * the run. With Notes on (the generator's notes, txactivity) each sounds. The fix is the ACTIVITY / COUNT -> rate map
 * (host_t e0 .. e127, count_q8 in tx_eng_source.c), left to the knob-mapping pass. */
static const allow_t OPEN[] = {
    {"rockfall", 4, "COUNT 1, Notes off: a press every ~80 s (10 s x 8 / COUNT); the rate map's pass"},
    {"glass pane", 4, "ACT 0, Notes off: a press every ~40 s (e0); the rate map's pass"},
    {"bottles", 4, "ACT 0, Notes off: a press every ~40 s (e0); the rate map's pass"},
    {"shards settling", 4, "ACT 0, Notes off: a press every ~40 s (e0); the rate map's pass"},
};
#define NOPEN (sizeof OPEN / sizeof OPEN[0])

static unsigned secs = 33, nseeds = 6, verbose, detail, engines;
static const char *filter;

/* ---- biquads (Butterworth sections, bilinear) */
typedef struct { double b0, b1, b2, a1, a2, z1, z2; } bq_t;
static void bq_set(bq_t *q, int hp, double fc, double Q)
{
    double w = 2 * M_PI * fc / FS, c = cos(w), al = sin(w) / (2 * Q), a0 = 1 + al;
    q->b1 = (hp ? -(1 + c) : (1 - c)) / a0; q->b0 = q->b2 = (hp ? (1 + c) : (1 - c)) / 2 / a0;
    q->a1 = -2 * c / a0; q->a2 = (1 - al) / a0; q->z1 = q->z2 = 0;
}
static double bq_run(bq_t *q, double x)
{
    double y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2; q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

/* ---- FFT (radix 2, in place) */
static void fft(double *re, double *im, unsigned n, int inv)
{
    unsigned i, j, k, m;
    for (i = 1, j = 0; i < n; i++) {
        unsigned b = n >> 1;
        for (; j & b; b >>= 1) j ^= b;
        j |= b;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (m = 2; m <= n; m <<= 1) {
        double a = (inv ? 2 : -2) * M_PI / m, wr = cos(a), wi = sin(a);
        for (k = 0; k < n; k += m) {
            double cr = 1, ci = 0;
            for (j = 0; j < m / 2; j++) {
                unsigned p = k + j, q = p + m / 2;
                double tr = re[q] * cr - im[q] * ci, ti = re[q] * ci + im[q] * cr, t;
                re[q] = re[p] - tr; im[q] = im[p] - ti; re[p] += tr; im[p] += ti;
                t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
}

static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double median(double *v, unsigned n)               /* (sorts v) */
{
    qsort(v, n, sizeof *v, dcmp);
    return n & 1u ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/* ---- one render's results */
typedef struct {
    int ok;
    double bp[NBAND], bf[NBAND], blev[NBAND];             /* the strongest line: prominence dB, Hz; the band's level dB */
    double lp[NBAND][NLINE], lf[NBAND][NLINE];            /* the band's strongest lines over LINE_DB (0: none) */
    unsigned nev, nshape, npair, nalike;                  /* events, shapes, pairs, pairs over ALIKE_X */
    double peak, first;                                   /* level: |x| max (1 = full scale), s to the first audible
                                                           * 10 ms (-1: never) */
    unsigned nclip;                                       /* samples at or over full scale (the limiter's work) */
} res_t;

static const ls_quick_t *quick_of(const char *eng)
{
    unsigned i;
    for (i = 0; eng && i < LS_NQUICK; i++) if (!strcmp(LS_QUICK[i].eng, eng)) return &LS_QUICK[i];
    return 0;
}

static double *render(const snd_t *s, const cor_t *cor, uint32_t seed, unsigned *n_out)
{
    const tx_engine_t *e = tx_engine_find(s->pre >= 0 ? "source" : s->eng);
    const ls_quick_t *q = s->pre < 0 && cor->param ? quick_of(s->eng) : 0;   /* a classic engine's ACTIVITY corner */
    uint32_t pr = seed * 2654435761u + 12345u;
    double next = -1, gap = PRESS_S;
    void *st;
    unsigned nb = (unsigned)(secs * FS) / TX_BLOCK, b, j;
    int32_t mix[TX_BLOCK];
    double *x;
    if (!e) return 0;
    st = tx_state_new(e);
    x = malloc(sizeof *x * nb * TX_BLOCK);
    e->init(st, seed);
    if (s->pre >= 0) {
        e->param(st, (uint8_t)tx_param_find(e, "PRESET"), (uint8_t)s->pre);
        e->param(st, (uint8_t)tx_param_find(e, "TYPE"), (uint8_t)s->type);
        if (cor->param) e->param(st, (uint8_t)tx_param_find(e, cor->param), (uint8_t)cor->val);
    }
    if (q) {                                                 /* its activity map at C4 (ACT 0) or C6 (ACT 127) */
        unsigned k;
        for (k = 0; k < q->np; k++) {
            int id = tx_param_find(e, q->p[k].name);
            if (id >= 0) e->param(st, (uint8_t)id, (uint8_t)(cor->val ? q->p[k].hi : q->p[k].lo));
        }
        if (q->every_ms) {                                   /* a one-shot: pressed after the map's gaps */
            gap = q->every_ms / 1000.0 * (cor->val ? pow(2.0, q->every_oct_q16 / 65536.0) : 1.0);
            next = 0;
        }
    }
    e->param(st, TX_PID_PARTICLES, 6);
    e->key(st, 60, 100, 1);
    if (s->pre == -2) next = 0;
    for (b = 0; b < nb; b++) {
        if (next >= 0 && b * TX_BLOCK / FS >= next) {        /* -e: pressed again */
            double u;
            pr = pr * 1664525u + 1013904223u; u = ((pr >> 8) + 0.5) / 16777216.0;
            if (next > 0) { e->key(st, 60, 0, 0); e->key(st, 60, (uint8_t)(70u + (pr >> 26)), 1); }
            next += -log(u) * gap;
        }
        memset(mix, 0, sizeof mix);
        e->render(st, mix, TX_BLOCK);
        for (j = 0; j < TX_BLOCK; j++) x[b * TX_BLOCK + j] = mix[j] / 8388608.0;
    }
    free(st);
    *n_out = nb * TX_BLOCK;
    return x;
}

static void beat(const double *x, unsigned n, res_t *r)
{
    static const double LO[NBAND] = {30, 300, 2000, 8000}, HI[NBAND] = {300, 2000, 8000, 18000};
    unsigned m = n / 441u, skip = 300, len, N, band, i, k;
    double *e = malloc(sizeof *e * m), *re, *im, *S, *w;
    for (band = 0; band < NBAND; band++) {
        bq_t q[4];
        double mean = 0, best = 0, bestf = 0;
        unsigned t;
        for (t = 0; t < NLINE; t++) r->lp[band][t] = r->lf[band][t] = 0;
        bq_set(&q[0], 1, LO[band], 0.5412); bq_set(&q[1], 1, LO[band], 1.3066);
        bq_set(&q[2], 0, HI[band], 0.5412); bq_set(&q[3], 0, HI[band], 1.3066);
        for (i = 0; i < m; i++) {
            double acc = 0;
            for (k = 0; k < 441u; k++) {
                double y = x[i * 441u + k];
                y = bq_run(&q[0], y); y = bq_run(&q[1], y); y = bq_run(&q[2], y); y = bq_run(&q[3], y);
                acc += y * y;
            }
            e[i] = sqrt(acc / 441.0);
        }
        len = m - skip;
        for (i = 0; i < len; i++) mean += e[skip + i];
        mean /= len;
        r->blev[band] = 20 * log10(mean + 1e-12);
        for (N = 1; N < 2 * len; N <<= 1) {}
        re = calloc(N, sizeof *re); im = calloc(N, sizeof *im); S = malloc(sizeof *S * (N / 2 + 1)); w = malloc(sizeof *w * N);
        for (i = 0; i < len; i++) re[i] = (e[skip + i] - mean) * (0.5 - 0.5 * cos(2 * M_PI * i / (len - 1)));
        fft(re, im, N, 0);
        for (i = 0; i <= N / 2; i++) S[i] = re[i] * re[i] + im[i] * im[i];
        if (mean > 0)
            for (k = 1; k < N / 2; k++) {
                double f = k * 100.0 / N;
                unsigned nw = 0, a, lo, hi;
                if (f < 0.2 || f > 20) continue;
                if (S[k] < S[k - 1] || S[k] < S[k + 1]) continue;
                lo = (unsigned)ceil(f / 1.5 * N / 100.0); hi = (unsigned)floor(f * 1.5 * N / 100.0);
                if (hi > N / 2) hi = N / 2;
                for (a = lo; a <= hi; a++) if (a + 3 < k || a > k + 3) w[nw++] = S[a];
                if (nw) {
                    double fl = median(w, nw), p;
                    if (fl <= 0) continue;
                    p = 10 * log10(S[k] / fl);
                    if (p > best) { best = p; bestf = f; }
                    if (p >= LINE_DB && p > r->lp[band][NLINE - 1]) {   /* into the band's top lines */
                        for (t = NLINE - 1; t > 0 && p > r->lp[band][t - 1]; t--) { r->lp[band][t] = r->lp[band][t - 1]; r->lf[band][t] = r->lf[band][t - 1]; }
                        r->lp[band][t] = p; r->lf[band][t] = f;
                    }
                }
            }
        r->bp[band] = best; r->bf[band] = bestf;
        free(re); free(im); free(S); free(w);
    }
    free(e);
}

static int inrange(const res_t *r, unsigned b)             /* a band within BEAT_RANGE_DB of the run's loudest */
{
    double top = -1e9;
    unsigned q;
    for (q = 0; q < NBAND; q++) if (r->blev[q] > top) top = r->blev[q];
    return r->blev[b] >= top - BEAT_RANGE_DB;
}

static unsigned uf_find(unsigned *p, unsigned i) { while (p[i] != i) i = p[i] = p[p[i]]; return i; }

static void alike(const double *x, unsigned n, res_t *r)
{
    unsigned h = 110, m = n / h, i, k, nev = 0, *on = malloc(sizeof *on * m), *par;
    double *L = malloc(sizeof *L * m), *P = malloc(sizeof *P * m), *Ls = malloc(sizeof *Ls * m), *tmp = malloc(sizeof *tmp * m);
    double top, pk = -999, low = 999;
    double (*Fr)[NF], (*Fi)[NF], *re, *im;
    int last = -1;
    bq_t q;
    bq_set(&q, 1, 200, M_SQRT1_2);
    for (i = 0; i < m; i++) {
        double acc = 0;
        for (k = 0; k < h; k++) { double y = bq_run(&q, x[i * h + k]); acc += y * y; }
        P[i] = acc / h; L[i] = 10 * log10(P[i] + 1e-30);
    }
    for (i = 0; i < m; i++) {                             /* 10 ms smoothed: a ring's beating does not count as a fall */
        double s = 0; int a;
        for (a = (int)i - 2; a <= (int)i + 1; a++) if (a >= 0 && a < (int)m) s += P[a];
        Ls[i] = 10 * log10(s / 4 + 1e-30);
    }
    memcpy(tmp, L, sizeof *L * m);
    qsort(tmp, m, sizeof *tmp, dcmp);
    { double pos = 0.995 * (m - 1); unsigned a = (unsigned)pos; top = tmp[a] + (pos - a) * (tmp[a + 1 < m ? a + 1 : a] - tmp[a]); }
    for (i = 400; i + 30 < m; i++) {
        double mn = 1e30;
        if (last >= 0) { if (Ls[i] < low) low = Ls[i]; if (i - (unsigned)last <= 8u && L[i] > pk) pk = L[i]; }
        if (last >= 0 && i - (unsigned)last < 12u) continue;
        for (k = i - 8; k < i; k++) if (L[k] < mn) mn = L[k];
        if (L[i] > top - 35 && L[i] - mn > 9) {
            if (last >= 0 && low > pk - RING_DB) continue;   /* inside the last event's ring */
            if (i * h >= PRE && i * h - PRE + SEG <= n) on[nev++] = i * h;
            last = (int)i; pk = L[i]; low = 999;
        }
    }
    if (nev > MAXEV) { for (k = 0; k < MAXEV; k++) on[k] = on[(unsigned)((double)k * nev / MAXEV)]; nev = MAXEV; }
    r->nev = r->nshape = r->npair = r->nalike = 0;
    if (nev >= 4) {
        unsigned a, b, np = 0, nn = 0;
        Fr = malloc(sizeof *Fr * nev); Fi = malloc(sizeof *Fi * nev);
        re = malloc(sizeof *re * NF); im = malloc(sizeof *im * NF);
        par = malloc(sizeof *par * nev);
        for (a = 0; a < nev; a++) {
            double mu = 0, nr = 0;
            const double *s = x + on[a] - PRE;
            for (k = 0; k < SEG; k++) mu += s[k];
            mu /= SEG;
            for (k = 0; k < SEG; k++) nr += (s[k] - mu) * (s[k] - mu);
            nr = nr > 0 ? 1 / sqrt(nr) : 0;
            for (k = 0; k < NF; k++) { Fr[a][k] = k < SEG ? (s[k] - mu) * nr : 0; Fi[a][k] = 0; }
            fft(Fr[a], Fi[a], NF, 0);
            par[a] = a;
        }
        /* the pairs, two per inverse FFT (each product's inverse is real: one in re, one in im) */
        {
            unsigned npairs = nev * (nev - 1) / 2, p;
            unsigned *pa = malloc(sizeof *pa * npairs), *pb = malloc(sizeof *pb * npairs);
            for (a = 0, p = 0; a < nev; a++) for (b = a + 1; b < nev; b++) { pa[p] = a; pb[p] = b; p++; }
            for (p = 0; p < npairs; p += 2) {
                unsigned two = p + 1 < npairs, t;
                for (k = 0; k < NF; k++) {
                    double xr = Fr[pa[p]][k] * Fr[pb[p]][k] + Fi[pa[p]][k] * Fi[pb[p]][k];
                    double xi = Fi[pa[p]][k] * Fr[pb[p]][k] - Fr[pa[p]][k] * Fi[pb[p]][k];
                    double yr = 0, yi = 0;
                    if (two) {
                        yr = Fr[pa[p + 1]][k] * Fr[pb[p + 1]][k] + Fi[pa[p + 1]][k] * Fi[pb[p + 1]][k];
                        yi = Fi[pa[p + 1]][k] * Fr[pb[p + 1]][k] - Fr[pa[p + 1]][k] * Fi[pb[p + 1]][k];
                    }
                    re[k] = xr - yi; im[k] = xi + yr;     /* X + i Y */
                }
                fft(re, im, NF, 1);
                for (t = 0; t <= two; t++) {
                    double mx = 0, *v = t ? im : re;
                    for (k = 0; k <= LAG; k++) { double c = fabs(v[k]) / NF; if (c > mx) mx = c; }
                    for (k = NF - LAG; k < NF; k++) { double c = fabs(v[k]) / NF; if (c > mx) mx = c; }
                    np++;
                    if (mx > ALIKE_X) {
                        unsigned ra = uf_find(par, pa[p + t]), rb = uf_find(par, pb[p + t]);
                        nn++;
                        if (ra != rb) par[ra] = rb;
                    }
                }
            }
            free(pa); free(pb);
        }
        for (a = 0, b = 0; a < nev; a++) if (uf_find(par, a) == a) b++;
        r->nev = nev; r->nshape = b; r->npair = np; r->nalike = nn;
        free(Fr); free(Fi); free(re); free(im); free(par);
    }
    free(on); free(L); free(P); free(Ls); free(tmp);
}

static void level(const double *x, unsigned n, res_t *r)
{
    unsigned i, k, h = 441u;
    r->peak = 0; r->first = -1; r->nclip = 0;
    for (i = 0; i < n; i++) {
        double a = fabs(x[i]);
        if (a > r->peak) r->peak = a;
        if (a >= 1.0) r->nclip++;
    }
    for (i = 0; i + h <= n && r->first < 0; i += h) {
        double e = 0;
        for (k = 0; k < h; k++) e += x[i + k] * x[i + k];
        if (10 * log10(e / h + 1e-30) >= AUDIBLE_DB) r->first = i / FS;
    }
}

/* ---- the jobs */
typedef struct { unsigned s, c, sd; } job_t;
static job_t *jobs;
static res_t *res;
static unsigned njobs, next_job;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        unsigned j, n;
        double *x;
        pthread_mutex_lock(&mu); j = next_job++; pthread_mutex_unlock(&mu);
        if (j >= njobs) return 0;
        x = render(&all[jobs[j].s], &COR[jobs[j].c], 1u + jobs[j].sd, &n);
        if (!x) continue;
        beat(x, n, &res[j]); alike(x, n, &res[j]); level(x, n, &res[j]); res[j].ok = 1;
        free(x);
    }
}

static const allow_t *listed(const allow_t *l, unsigned n, const char *name, unsigned test)
{
    unsigned i;
    for (i = 0; i < n; i++) if (!strcmp(l[i].name, name) && (l[i].test & test)) return &l[i];
    return 0;
}

int main(int argc, char **argv)
{
    static const char *const BN[NBAND] = {"<300", "0.3-2k", "2-8k", ">8k"};
    unsigned nth = (unsigned)sysconf(_SC_NPROCESSORS_ONLN), i, s, c, sd, fails = 0, allows = 0, opens = 0;
    pthread_t *th;
    uint64_t t0 = tx_now_ns();
    int a, nset = 0;
    for (a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "-s") && a + 1 < argc) secs = (unsigned)atoi(argv[++a]);
        else if (!strcmp(argv[a], "-n") && a + 1 < argc) nseeds = (unsigned)atoi(argv[++a]), nset = 1;
        else if (!strcmp(argv[a], "-j") && a + 1 < argc) nth = (unsigned)atoi(argv[++a]);
        else if (!strcmp(argv[a], "-f") && a + 1 < argc) filter = argv[++a];
        else if (!strcmp(argv[a], "-v")) verbose = 1;
        else if (!strcmp(argv[a], "-d")) verbose = detail = 1;
        else if (!strcmp(argv[a], "-x")) ncor = NCOR;
        else if (!strcmp(argv[a], "-e")) engines = 1;
        else { fprintf(stderr, "txaudit [-s SECS] [-n SEEDS] [-j THREADS] [-f SUBSTRING] [-v] [-d] [-x] [-e]\n"); return 2; }
    }
    if (!nset && (ncor > 3u || engines)) nseeds = 8;   /* the corners and the engines: 8 seeds (at 6, a lone call or a
                                                         * wave's run-up agrees in 4 of 6 by chance: skylark, curlew, wood
                                                         * thrush at EXTRA / COUNT, waves on shingle, cave drips COUNT
                                                         * 127; none at 8) */
    if (secs < 10) secs = 10;
    if (nseeds < 3) nseeds = 3;
    if (nseeds > MAXSEED) nseeds = MAXSEED;
    if (nth < 1) nth = 1;
    for (s = 0; s < NSND; s++) all[nall++] = SND[s];
    if (engines)
        for (i = 0; tx_engines[i] && nall < MAXSND; i++) {
            const char *nm = tx_engines[i]->name;
            unsigned q, seen = !strcmp(nm, "source") || !strcmp(nm, "test");
            for (q = 0; q < NSND; q++) if (SND[q].eng && !strcmp(SND[q].eng, nm)) seen = 1;
            if (!seen) { all[nall].name = nm; all[nall].pre = -2; all[nall].eng = nm; all[nall].type = 0; nall++; }
        }
    jobs = malloc(sizeof *jobs * MAXSND * NCOR * MAXSEED);
    for (s = 0; s < nall; s++) {
        if (filter && !strstr(all[s].name, filter)) continue;
        for (c = 0; c < ncor; c++) {
            if (all[s].pre < 0 && c && !(ncor > 3u && c <= 2u && quick_of(all[s].eng))) continue;   /* a classic
                                                             * engine: ACTIVITY 0 / 127 under -x through its activity
                                                             * map (LS_QUICK), no EXTRA or COUNT */
            for (sd = 0; sd < nseeds; sd++) { jobs[njobs].s = s; jobs[njobs].c = c; jobs[njobs].sd = sd; njobs++; }
        }
    }
    res = calloc(njobs ? njobs : 1, sizeof *res);
    th = malloc(sizeof *th * nth);
    for (i = 0; i < nth; i++) pthread_create(&th[i], 0, worker, 0);
    for (i = 0; i < nth; i++) pthread_join(th[i], 0);
    printf("txaudit: %u renders (%u s, %u seeds); BEAT: lines of >= 2/3 of the seeds within %.0f %%, median >= %.1f dB; "
           "ALIKE: > %.0f %% of pairs over %.2f or < %.2f shapes / event; SILENT: half the seeds never over %.0f dBFS; "
           "HOT: a peak over %+.0f dBFS\n", njobs, secs, nseeds, BEAT_AGREE * 100, BEAT_DB, ALIKE_SHARE * 100, ALIKE_X,
           ALIKE_SHAPES, AUDIBLE_DB, HOT_DB);
    printf("%-20s %-9s | %-30s | %-26s | %-21s | verdict\n", "sound", "corner", "beat: band, Hz, dB, seeds",
           "events, >0.95, shapes/ev", "peak dB, >0dB, first");
    for (i = 0; i < njobs; i += nseeds) {
        const snd_t *S = &all[jobs[i].s];
        unsigned b, bb = 0, bagree = 0, nev = 0, nsh = 0, npr = 0, nal = 0, beatf = 0, alikef = 0;
        double bmed = 0, bfm = 0, pk, pkdb, late;
        unsigned nclipsd, nsil, nok, silf, hotf;
        {   /* every line of every seed is a candidate: how many seeds have a line within BEAT_AGREE of it? */
            unsigned nv = 0, need, sa, ba, ta;
            for (sd = 0; sd < nseeds; sd++) nv += res[i + sd].ok;
            need = (2 * nv + 2) / 3; if (need < 3) need = 3;
            for (sa = 0; sa < nseeds; sa++) for (ba = 0; ba < NBAND; ba++) for (ta = 0; ta < NLINE; ta++) {
                const res_t *r = &res[i + sa];
                double fc = r->lf[ba][ta], pp[MAXSEED], pm;
                unsigned na = 0;
                if (!r->ok || r->lp[ba][ta] <= 0 || !inrange(r, ba)) continue;
                for (sd = 0; sd < nseeds; sd++) {
                    const res_t *q = &res[i + sd];
                    double hit = 0;
                    unsigned bq, tq;
                    if (!q->ok) continue;
                    for (bq = 0; bq < NBAND; bq++) if (inrange(q, bq))
                        for (tq = 0; tq < NLINE; tq++)
                            if (q->lp[bq][tq] > hit && fabs(q->lf[bq][tq] - fc) <= BEAT_AGREE * fc) hit = q->lp[bq][tq];
                    if (hit > 0) pp[na++] = hit;
                }
                if (na < need) continue;
                pm = median(pp, na);
                if (pm > bmed || (pm == bmed && na > bagree)) { bmed = pm; bfm = fc; bb = ba; bagree = na; }
            }
        }
        beatf = bmed >= BEAT_DB;
        {   /* the level: the loudest seed's peak, the seeds over full scale, the seeds silent all through, the latest
             * first sound of the others */
            pk = 0; late = 0; nclipsd = nsil = nok = 0;
            for (sd = 0; sd < nseeds; sd++) {
                const res_t *r = &res[i + sd];
                if (!r->ok) continue;
                nok++;
                if (r->peak > pk) pk = r->peak;
                nclipsd += r->nclip > 0;
                if (r->first < 0) nsil++; else if (r->first > late) late = r->first;
            }
            pkdb = 20 * log10(pk + 1e-12);
            hotf = pkdb > HOT_DB;
            silf = nok && 2 * nsil >= nok;
        }
        for (sd = 0; sd < nseeds; sd++) { nev += res[i + sd].nev; nsh += res[i + sd].nshape; npr += res[i + sd].npair; nal += res[i + sd].nalike; }
        alikef = nev >= ALIKE_MIN_EV && npr && ((double)nal / npr > ALIKE_SHARE || (double)nsh / nev < ALIKE_SHAPES);
        {
            const allow_t *ab = beatf ? listed(ALLOW, NALLOW, S->name, 1) : 0, *aa = alikef ? listed(ALLOW, NALLOW, S->name, 2) : 0;
            const allow_t *ob = beatf && !ab ? listed(OPEN, NOPEN, S->name, 1) : 0;
            const allow_t *oa = alikef && !aa ? listed(OPEN, NOPEN, S->name, 2) : 0;
            const allow_t *os = silf ? listed(OPEN, NOPEN, S->name, 4) : 0;
            const allow_t *oh = hotf ? listed(OPEN, NOPEN, S->name, 8) : 0;
            const allow_t *why = ab ? ab : aa ? aa : ob ? ob : oa ? oa : os ? os : oh;
            int bad = (beatf && !ab && !ob) || (alikef && !aa && !oa) || (silf && !os) || (hotf && !oh);
            int flag = beatf || alikef || silf || hotf;
            char bs[48], es[40], ls[40];
            if (bagree) snprintf(bs, sizeof bs, "%-6s %6.2f %5.1f %u/%u", BN[bb], bfm, bmed, bagree, nseeds);
            else snprintf(bs, sizeof bs, "-");
            if (nev) snprintf(es, sizeof es, "%4u %6.3f %5.2f", nev, npr ? (double)nal / npr : 0, (double)nsh / nev);
            else snprintf(es, sizeof es, "-");
            if (nsil) snprintf(ls, sizeof ls, "%+6.1f %u/%u %u silent", pkdb, nclipsd, nok, nsil);
            else snprintf(ls, sizeof ls, "%+6.1f %u/%u %5.1f s", pkdb, nclipsd, nok, late);
            if (verbose || flag)
                printf("%-20s %-9s | %-30s | %-26s | %-21s | %s%s%s%s%s%s\n", S->name, COR[jobs[i].c].label, bs, es, ls,
                       beatf ? (ab ? "beat (allowed) " : ob ? "beat (open) " : "BEAT ") : "",
                       alikef ? (aa ? "alike (allowed) " : oa ? "alike (open) " : "ALIKE ") : "",
                       silf ? (os ? "silent (open) " : "SILENT ") : "", hotf ? (oh ? "hot (open) " : "HOT ") : "",
                       flag ? "" : "ok", why ? why->why : "");
            if (detail)
                for (sd = 0; sd < nseeds; sd++) {
                    const res_t *r = &res[i + sd];
                    printf("    seed %u:", sd + 1);
                    for (b = 0; b < NBAND; b++) printf("  %s %5.1f dB @ %5.2f Hz (%5.1f)", BN[b], r->bp[b], r->bf[b], r->blev[b]);
                    printf("  | %u events, %u / %u pairs, %u shapes | peak %+.1f dBFS, %u samples over, first %.2f s\n",
                           r->nev, r->nalike, r->npair, r->nshape, 20 * log10(r->peak + 1e-12), r->nclip, r->first);
                }
            fails += (unsigned)bad;
            allows += (unsigned)(!bad && (ab || aa));
            opens += (unsigned)(!bad && (ob || oa || os || oh));
        }
    }
    printf("txaudit: %u flagged, %u allowed (a species' own rhythm or call), %u open, %.1f s\n", fails, allows, opens,
           (tx_now_ns() - t0) / 1e9);
    free(jobs); free(res); free(th);
    return fails ? 1 : 0;
}
