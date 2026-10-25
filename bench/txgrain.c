/* SPDX-License-Identifier: GPL-3.0-only */
/* txgrain: a granular-delay PROTOTYPE for costing only (make tx-grain): what a grain cloud on a delay line costs on
 * the FM-1, and what the buffer's sample format does to the noise floor. Not an effect: no parameters, no
 * smoothing, no player. Integer C as engines/ (32-bit products in the per-sample loops), timed like txbudget (the
 * fastest of several runs per block, scaled to the FM-1 by the Frodhes calibration in build/calib/run).
 *
 * The grain loop per sample and grain: read the buffer (two 16-bit samples when the grain is pitched, one when it
 * plays at rate 1 from a whole sample), interpolate linearly (Q15 fraction), window from a 256-entry Q15 table,
 * pan to two gains, accumulate; the phase and the position advance. Around it: the write (with feedback: one-pole
 * low-pass, soft clip, DC block), the scheduler (a grain started when one ends, so the worst case keeps every slot
 * busy), the output sum.
 *
 * Options measured (each at its worst: every grain slot sounding):
 *   A  4 grains, mono out, 16-bit mono line (as the cheapest designs: a few grains, fixed size)
 *   B  8 grains, scheduled (density, jitter, size, spray, pitch / detune, reverse, pan), stereo out, freeze, feedback
 *   C  16 grains as B, plus a stereo post-delay echo (two more 16-bit lines, read + write, its own feedback)
 * Noise: a -40 dBFS sine and a -70 dBFS sine stored and read back through each format (16-bit, 12-bit packed, and
 * 12-bit with a shared 4-bit exponent per 16 samples), the error against the input in dBFS. */
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tx_host.h"

#define FM1_WORST_US 3620.0
#define FR_SKIP_BLOCKS 20u
#define LEN 65536u                 /* buffer samples (1.49 s at 44.1 kHz) */
#define MSK (LEN - 1u)
#define NBLK 1000u
#define RUNS 7u

static double factor;

/* ---- the calibration, as txbench / txbudget ---- */
static int calib(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    static uint64_t v[64][40000];
    unsigned n = 0;
    uint32_t nb = 0xFFFFFFFFu, b;
    double worst = 0;
    if (!d) return -1;
    while ((de = readdir(d)) && n < 64) {
        char p[600];
        FILE *f;
        unsigned long x;
        uint32_t k = 0;
        if (de->d_name[0] != 'w' || !strstr(de->d_name, ".txt")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, de->d_name);
        if (!(f = fopen(p, "r"))) continue;
        while (k < 40000 && fscanf(f, "%lu", &x) == 1) v[n][k++] = x;
        fclose(f);
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
    factor = FM1_WORST_US / worst;
    return 0;
}

/* ---- the prototype ---- */
typedef struct {
    uint32_t pos;                  /* Q15 buffer position (17 integer bits) */
    int32_t inc;                   /* Q15 per sample, signed (reverse) */
    uint32_t ph, step;             /* window phase, Q24 over the grain */
    int32_t gl, gr;                /* pan gains, Q15 */
    uint8_t on, exact;             /* exact: rate 1 from a whole sample (no interpolation) */
} grain_t;

typedef struct {
    int16_t buf[LEN];
    int16_t echo[2][16384];        /* option C's post-delay (0.37 s stereo) */
    uint32_t w, we;
    grain_t g[16];
    int ng, stereo, echo_on;
    int16_t win[257];
    uint32_t rng;
    int32_t lp, dcx, dcy, fb_q15, lp_k, wgain;   /* feedback filter; freeze write gain (Q15) */
    int32_t er[2];
    unsigned next;                 /* samples to the next scheduled grain */
    int32_t pitch_inc;             /* the grains' base rate, Q15 */
} gd_t;

static uint32_t rnd(gd_t *s) { s->rng = s->rng * 1664525u + 1013904223u; return s->rng; }

static void gd_init(gd_t *s, int ng, int stereo, int echo_on, int32_t pitch_inc)
{
    unsigned i;
    memset(s, 0, sizeof *s);
    s->ng = ng; s->stereo = stereo; s->echo_on = echo_on; s->pitch_inc = pitch_inc;
    for (i = 0; i <= 256; i++) {                     /* (4p(1-p))^2 ~ Hann, integer */
        int32_t p = (int32_t)(i << 7);               /* Q15 */
        int32_t q = (int32_t)(((int64_t)4 * p * (32768 - p)) >> 15);
        s->win[i] = (int16_t)(((int64_t)q * q) >> 15 > 32767 ? 32767 : ((int64_t)q * q) >> 15);
    }
    s->rng = 12345u; s->fb_q15 = 16000; s->lp_k = 12000; s->wgain = 32767;
    for (i = 0; i < LEN; i++) s->buf[i] = (int16_t)((int32_t)(rnd(s) >> 17) - 16384);   /* a busy buffer */
}

static void spawn(gd_t *s, grain_t *g)
{
    uint32_t r = rnd(s);
    uint32_t len = 2205u + ((r >> 8) & 4095u);                       /* 50 .. 143 ms */
    uint32_t behind = 8820u + ((r >> 4) & 16383u);                   /* the delay 0.2 s + spray */
    int rev = (r & 7u) == 0u;
    int32_t inc = s->pitch_inc + (int32_t)((rnd(s) >> 24) & 63u) - 32;   /* detune */
    g->pos = ((s->w - behind) & MSK) << 15;
    g->inc = rev ? -inc : inc;
    g->ph = 0; g->step = (1u << 24) / len;
    g->gl = 16384 + (int32_t)((r >> 20) & 8191u); g->gr = 32767 - g->gl;
    g->exact = inc == 32768;
    g->on = 1;
}

/* one block: in (Q23) -> L, R (Q23) */
static void gd_block(gd_t *s, const int32_t *in, int32_t *L, int32_t *R, unsigned n)
{
    unsigned j, k;
    for (j = 0; j < n; j++) {
        int32_t al = 0, ar = 0, x, mono;
        /* the scheduler (B / C): a grain every 'next' samples, into a free slot; A: fixed overlap, one clock */
        if (!s->next--) {                            /* the worst case: every free slot refilled at once */
            for (k = 0; k < (unsigned)s->ng; k++) if (!s->g[k].on) spawn(s, &s->g[k]);
            s->next = 64u;
        }
        for (k = 0; k < (unsigned)s->ng; k++) {
            grain_t *g = &s->g[k];
            int32_t v, w;
            uint32_t i;
            if (!g->on) continue;
            i = (g->pos >> 15) & MSK;
            if (g->exact) v = s->buf[i];
            else {
                int32_t a = s->buf[i], b = s->buf[(i + 1u) & MSK];
                v = a + (((b - a) * (int32_t)(g->pos & 32767u)) >> 15);
            }
            w = s->win[g->ph >> 16];                                  /* 256 steps over the grain */
            v = (v * w) >> 15;                                        /* 16 x 15 bits */
            al += (v * g->gl) >> 15;                                  /* 16-bit, summed */
            if (s->stereo) ar += (v * g->gr) >> 15;
            g->pos = (g->pos + (uint32_t)g->inc) & ((MSK << 15) | 32767u);
            g->ph += g->step;
            if (g->ph >= (1u << 24)) g->on = 0;                       /* ended: the scheduler refills it */
        }
        al <<= 8; ar <<= 8;                                           /* -> Q23 (16-bit << 8) */
        mono = s->stereo ? (al >> 1) + (ar >> 1) : al;
        /* feedback: one-pole low-pass, soft clip, DC block, into the write */
        s->lp += (int32_t)(((int64_t)(mono - s->lp) * s->lp_k) >> 15);
        x = (int32_t)(((int64_t)s->lp * s->fb_q15) >> 15);
        if (x > (6 << 20)) x = (6 << 20) + ((x - (6 << 20)) >> 2);
        else if (x < -(6 << 20)) x = -(6 << 20) + ((x + (6 << 20)) >> 2);
        { int32_t y = x - s->dcx + s->dcy - (s->dcy >> 9); s->dcx = x; s->dcy = y; x = y; }
        x += in[j];
        x = (int32_t)(((int64_t)x * s->wgain) >> 15);                 /* freeze: the write fades out */
        x = (x + 128) >> 8;
        s->buf[s->w] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
        s->w = (s->w + 1u) & MSK;
        if (s->echo_on) {                                             /* C: a stereo post-delay */
            uint32_t r = (s->we - 12000u) & 16383u;
            int32_t el = s->echo[0][r] << 8, er2 = s->echo[1][r] << 8, t;
            t = ((al + ((el * 3) >> 2)) + 128) >> 8; s->echo[0][s->we] = (int16_t)(t > 32767 ? 32767 : t < -32768 ? -32768 : t);
            t = ((ar + ((er2 * 3) >> 2)) + 128) >> 8; s->echo[1][s->we] = (int16_t)(t > 32767 ? 32767 : t < -32768 ? -32768 : t);
            s->we = (s->we + 1u) & 16383u;
            al += el; ar += er2;
        }
        L[j] = al; R[j] = s->stereo ? ar : al;
    }
}

static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

static double bench(int ng, int stereo, int echo_on, int32_t inc, double *mean)
{
    gd_t *s = malloc(sizeof *s);
    static uint64_t t[RUNS][NBLK];
    int32_t in[TX_BLOCK], L[TX_BLOCK], R[TX_BLOCK];
    unsigned r, b, j;
    double sum = 0, v[NBLK];
    unsigned nv = 0;
    for (r = 0; r <= RUNS; r++) {
        gd_init(s, ng, stereo, echo_on, inc);
        for (b = 0; b < NBLK; b++) {
            uint64_t t0;
            for (j = 0; j < TX_BLOCK; j++) in[j] = (int32_t)(rnd(s) >> 10) - (1 << 21);
            t0 = tx_now_ns();
            gd_block(s, in, L, R, TX_BLOCK);
            if (r) t[r - 1][b] = tx_now_ns() - t0;
        }
    }
    for (b = 50; b < NBLK; b++) {                                     /* every slot busy after ~50 blocks */
        uint64_t m = t[0][b];
        for (r = 1; r < RUNS; r++) if (t[r][b] < m) m = t[r][b];
        sum += m / 1000.0 * factor;
        v[nv++] = m / 1000.0 * factor;
    }
    *mean = sum / nv;
    free(s);
    qsort(v, nv, sizeof v[0], dcmp);
    return v[nv * 99 / 100];                                          /* the 99th percentile: the cost is flat when */
                                                                      /* every slot is busy; the top 1 % is the host */
}

/* ---- the formats' noise: store a sine, read it back, the error in dBFS ---- */
static int32_t q16(int32_t x) { x = (x + 128) >> 8; x = x > 32767 ? 32767 : x < -32768 ? -32768 : x; return x << 8; }
static int32_t q12(int32_t x) { x = (x + 2048) >> 12; x = x > 2047 ? 2047 : x < -2048 ? -2048 : x; return x << 12; }
static double noise_db(int fmt, double amp_db)
{
    double a = 8388608.0 * pow(10, amp_db / 20), err = 0;
    unsigned i, n = 44100, k;
    int32_t x[16];
    for (i = 0; i < n; i += 16) {
        int sh = 0;
        int32_t pk = 0;
        for (k = 0; k < 16; k++) { x[k] = (int32_t)lrint(a * sin(2 * M_PI * 441.0 * (i + k) / 44100.0)); if (abs(x[k]) > pk) pk = abs(x[k]); }
        if (fmt == 2) while (sh < 11 && (pk << (sh + 1)) < (1 << 22)) sh++;   /* the block's exponent (4 bits) */
        for (k = 0; k < 16; k++) {
            int32_t y = fmt == 0 ? q16(x[k]) : fmt == 1 ? q12(x[k]) : (q12(x[k] << sh) >> sh);
            err += (double)(y - x[k]) * (y - x[k]);
        }
    }
    return 10 * log10(err / n / (8388608.0 * 8388608.0) + 1e-30);
}

int main(void)
{
    static const struct { const char *name; int ng, stereo, echo; } OPT[] = {
        {"A  4 grains, mono", 4, 0, 0}, {"B  8 grains, stereo", 8, 1, 0}, {"B' 12 grains, stereo", 12, 1, 0},
        {"C  16 grains, stereo + echo", 16, 1, 1},
    };
    unsigned o;
    if (calib("build/calib/run")) { fprintf(stderr, "txgrain: no calibration in build/calib/run (make tx-calib)\n"); return 2; }
    tx_bench_prepare();
    printf("txgrain: granular delay prototype on the FM-1 (calibration factor %.1f, +-30 %%; fastest of %u runs per"
           " block). Every grain slot sounding.\n\n", factor, RUNS);
    printf("%-30s %22s %22s\n", "option", "rate 1 (no interp.)", "pitched (interp.)");
    printf("%-30s %10s %11s %10s %11s\n", "", "p99 us", "mean us", "p99 us", "mean us");
    for (o = 0; o < sizeof OPT / sizeof OPT[0]; o++) {
        double m1, m2, w1 = bench(OPT[o].ng, OPT[o].stereo, OPT[o].echo, 32768, &m1),
               w2 = bench(OPT[o].ng, OPT[o].stereo, OPT[o].echo, 49152, &m2);
        printf("%-30s %7.0f %5.1f%% %8.0f   %7.0f %5.1f%% %8.0f\n", OPT[o].name, w1, 100 * w1 / 5805, m1, w2,
               100 * w2 / 5805, m2);
    }
    printf("\nbuffer format: the error of a stored sine, dBFS (the hiss it adds; Q23 full scale = 0 dBFS)\n");
    printf("%-34s %12s %12s\n", "format", "-40 dBFS in", "-70 dBFS in");
    printf("%-34s %12.1f %12.1f\n", "16-bit", noise_db(0, -40), noise_db(0, -70));
    printf("%-34s %12.1f %12.1f\n", "12-bit packed", noise_db(1, -40), noise_db(1, -70));
    printf("%-34s %12.1f %12.1f\n", "12-bit + exponent per 16 samples", noise_db(2, -40), noise_db(2, -70));
    return 0;
}
