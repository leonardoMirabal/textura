/* SPDX-License-Identifier: GPL-3.0-only */
/* txgrain: a granular-delay PROTOTYPE for costing only (make tx-grain): what grains on a delay line cost on
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
 * The structure is a DELAY (the user, 2026-10-25: a delay, not a grain texture): one line, a clean echo tap at TIME, and N
 * grain readers on the delayed signal around TIME (spray, pitch / detune, reverse, pan); their sum and the tap go
 * through the feedback path (one-pole low-pass, soft clip, DC block) back into the line, so every repeat is grained
 * again. Options measured (each at its worst: every grain reader sounding):
 *   1 ECHO          the tap + 2 grain readers, mono out
 *   2 GRAIN DELAY   the tap + 4 grain readers, stereo out (pan spread)
 *   3 SCATTER       the tap + 8 grain readers, stereo out
 * each on a 16-bit line and on a block-float line (12-bit mantissas, a 4-bit shift per 16 samples), and the tap alone
 * (no reader sounding: the floor a grain cap returns to).
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
    uint8_t pk[LEN / 2u * 3u], ex[LEN / 16u];   /* block float: 12-bit mantissas, 2 in 3 bytes; an exponent per 16 */
    int32_t stg[16], stpk;                      /* the 16 samples being gathered (Q23), their peak */
    int bf;                                     /* the line is block float (else 16-bit) */
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

/* a line sample at the 16-bit scale: 16-bit, or block float decoded (12-bit mantissa, the block's shift) */
static inline int32_t rd(const gd_t *s, uint32_t i)
{
    const uint8_t *b;
    int32_t v;
    if (!s->bf) return s->buf[i];
    b = s->pk + ((i * 3u) >> 1);                                      /* the sample's 12 bits start in this byte */
    v = (int32_t)(((uint32_t)b[0] | ((uint32_t)b[1] << 8)) >> ((i & 1u) << 2));   /* branch-free: 2 byte loads */
    v = (int32_t)((uint32_t)v << 20) >> 20;                           /* the 12 bits, sign-extended */
    return ((v << 12) >> s->ex[i >> 4]) >> 8;                         /* Q23 -> 16-bit scale */
}

/* write the sample at index w (Q23, saturated to 16 bits' range): 16-bit at once; block float every 16th sample */
static inline void wr(gd_t *s, uint32_t w, int32_t x)
{
    if (!s->bf) { x = (x + 128) >> 8; s->buf[w] = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x); return; }
    x = x > 8388607 ? 8388607 : x < -8388608 ? -8388608 : x;
    s->stg[w & 15u] = x;
    { int32_t a = x < 0 ? -x : x; if (a > s->stpk) s->stpk = a; }
    if ((w & 15u) == 15u) {                                           /* the block: its shift, then 16 mantissas */
        uint32_t sh = 0, k, b0 = (w - 15u);
        uint8_t *b = s->pk + (b0 >> 1) * 3u;
        while (sh < 11u && (s->stpk << (sh + 1u)) < (1 << 22)) sh++;
        s->ex[b0 >> 4] = (uint8_t)sh;
        for (k = 0; k < 16u; k += 2u, b += 3) {
            int32_t m0 = ((s->stg[k] << sh) + 2048) >> 12, m1 = ((s->stg[k + 1u] << sh) + 2048) >> 12;
            m0 = m0 > 2047 ? 2047 : m0; m1 = m1 > 2047 ? 2047 : m1;
            b[0] = (uint8_t)m0; b[1] = (uint8_t)(((uint32_t)m0 >> 8 & 15u) | ((uint32_t)m1 << 4)); b[2] = (uint8_t)((uint32_t)m1 >> 4);
        }
        s->stpk = 0;
    }
}

static void gd_init(gd_t *s, int ng, int stereo, int echo_on, int32_t pitch_inc)
{
    unsigned i;
    memset(s, 0, sizeof *s);
    s->ng = ng; s->stereo = stereo; s->echo_on = echo_on & 3; s->pitch_inc = pitch_inc; s->bf = (echo_on & 4) != 0;
    for (i = 0; i <= 256; i++) {                     /* (4p(1-p))^2 ~ Hann, integer */
        int32_t p = (int32_t)(i << 7);               /* Q15 */
        int32_t q = (int32_t)(((int64_t)4 * p * (32768 - p)) >> 15);
        s->win[i] = (int16_t)(((int64_t)q * q) >> 15 > 32767 ? 32767 : ((int64_t)q * q) >> 15);
    }
    s->rng = 12345u; s->fb_q15 = 16000; s->lp_k = 12000; s->wgain = 32767;
    for (i = 0; i < LEN; i++) s->buf[i] = (int16_t)((int32_t)(rnd(s) >> 17) - 16384);   /* a busy buffer */
    for (i = 0; i < LEN / 2u * 3u; i++) s->pk[i] = (uint8_t)(rnd(s) >> 24);
    for (i = 0; i < LEN / 16u; i++) s->ex[i] = (uint8_t)((rnd(s) >> 24) & 3u);
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
            if (g->exact) v = rd(s, i);
            else {
                int32_t a = rd(s, i), b = rd(s, (i + 1u) & MSK);
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
        if (s->echo_on & 2) {                                         /* the delay's clean echo at TIME (0.2 s) */
            int32_t t = rd(s, (s->w - 8820u) & MSK);
            al += t >> 1; ar += t >> 1;                               /* (a CLEAN / GRAIN blend: half each here) */
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
        wr(s, s->w, x);
        s->w = (s->w + 1u) & MSK;
        if (s->echo_on & 1) {                                         /* a stereo post-delay */
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
        {"tap + 2 readers, mono, 16-bit", 2, 0, 2}, {"tap + 4 readers, stereo, 16-bit", 4, 1, 2},
        {"tap + 8 readers, stereo, 16-bit", 8, 1, 2}, {"tap + 16 readers, stereo, 16-bit", 16, 1, 2},
        {"tap + 4 readers, stereo, block float", 4, 1, 6}, {"tap + 8 readers, stereo, block float", 8, 1, 6},
        {"tap + 16 readers, stereo, block float", 16, 1, 6}, {"tap alone, block float (0 sounding)", 0, 1, 6},
    };
    unsigned o;
    if (calib("build/calib/run")) { fprintf(stderr, "txgrain: no calibration in build/calib/run (make tx-calib)\n"); return 2; }
    tx_bench_prepare();
    printf("txgrain: granular delay prototype on the FM-1 (calibration factor %.1f, +-30 %%; fastest of %u runs per"
           " block). Every grain slot sounding.\n\n", factor, RUNS);
    printf("%-38s %22s %22s\n", "granular delay", "rate 1 (no interp.)", "pitched (interp.)");
    printf("%-38s %10s %11s %10s %11s\n", "", "p99 us", "mean us", "p99 us", "mean us");
    for (o = 0; o < sizeof OPT / sizeof OPT[0]; o++) {
        double m1, m2, w1 = bench(OPT[o].ng, OPT[o].stereo, OPT[o].echo, 32768, &m1),
               w2 = bench(OPT[o].ng, OPT[o].stereo, OPT[o].echo, 49152, &m2);
        printf("%-38s %7.0f %5.1f%% %8.0f   %7.0f %5.1f%% %8.0f\n", OPT[o].name, w1, 100 * w1 / 5805, m1, w2,
               100 * w2 / 5805, m2);
    }
    printf("\nbuffer format: the error of a stored sine, dBFS (the hiss it adds; Q23 full scale = 0 dBFS)\n");
    printf("%-34s %12s %12s\n", "format", "-40 dBFS in", "-70 dBFS in");
    printf("%-34s %12.1f %12.1f\n", "16-bit", noise_db(0, -40), noise_db(0, -70));
    printf("%-34s %12.1f %12.1f\n", "12-bit packed", noise_db(1, -40), noise_db(1, -70));
    printf("%-34s %12.1f %12.1f\n", "12-bit + exponent per 16 samples", noise_db(2, -40), noise_db(2, -70));
    return 0;
}
