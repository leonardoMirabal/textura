/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura reverb tank HQ: the HALL. Our own design, integers only:
 *
 *   send -> DC blocker -> the early reflections (12 taps a side on the undiffused send, 4 .. 84 ms)
 *        -> 4 series allpasses (the input diffusion) -> 16 delay lines (35 .. 109 ms, each its own
 *           ring) -> each line's end: a modulated read (8 of them, slow sines of their own rates), a one-pole whose
 *           pole is made for that line's length (the high end decays by TONE's ratio of the time, the same on every
 *           line: no line rings on), its gain for the time -> a 16 x 16 Hadamard (orthogonal: lossless) -> the lines
 *   out: L and R from two orthogonal sign patterns over the 16 lines (decorrelated), + the early reflections.
 *
 * The time is T60 in ms (the framework's mapping from SIZE / MIX and DECAY), its gains made once per change. The line
 * ends run in two halves (lines 0..7 here, 8..15 on the other core: tx_hq_job): each half writes only its own state
 * and buffers, every read is older than the block, so one core or two give the same samples. */
#include "tx_fx_kern.h"

static const uint16_t HQ_LEN[HQ_NL] = {1543, 1667, 1801, 1949, 2089, 2267, 2437, 2621,
                                       2833, 3049, 3299, 3547, 3833, 4127, 4451, 4801};   /* (primes, geometric) */
static const uint16_t HQ_AP[HQ_NAP] = {211, 157, 563, 409};                              /* the input diffusers */
static const int16_t HQ_APG[HQ_NAP] = {12288, 12288, 10240, 10240};                      /* 0.75, 0.75, 0.625, 0.625 (Q14) */
/* the early reflections (samples) and their gains (Q15, the sign the wall's) */
static const uint16_t HQ_ET[2][HQ_NT] = {
    {190, 348, 494, 736, 944, 1230, 1460, 1835, 2126, 2553, 2928, 3475},
    {225, 410, 600, 803, 1089, 1336, 1649, 1980, 2320, 2703, 3162, 3682}};
static const int16_t HQ_EG[2][HQ_NT] = {
    {9200, -8100, 7300, -6400, 5900, 5100, -4700, 4000, -3600, 3100, -2700, 2300},
    {-8800, 7800, -7000, 6300, -5600, 5000, 4500, -3900, 3500, -3000, 2600, -2200}};
#define HQ_LFO 1153115u             /* 0.37 Hz a control block: each line's rate HQ_RATE / 8 of it */
static const uint8_t HQ_RATE[HQ_NL] = {8, 11, 13, 9, 15, 10, 12, 17, 7, 14, 19, 10, 16, 9, 13, 21};   /* (0.32 .. 0.97 Hz) */
#define HQ_INJ 32767                /* the send into each line (whole: the lines hot, their rounding low) */
#define HQ_SHC 14220                /* the decay shelf's corner: 4 kHz (1 - e^(-2 pi 4000 / 44100), Q15) */
#define HQ_QUIET 3                  /* a line's level (int16 steps) under which it rounds toward zero */
#define HQ_OUT 1                    /* the outputs: the 16 lines' sum x 4 / 16 (the old HALL's level, near) */
#define HQ_ERG 1                    /* the early reflections' level */

void tx_hq_start(tx_hq_t *h, int16_t *mem, uint32_t words)
{
    uint32_t k, o = 0;
    tx_fx_memset(h, 0, sizeof *h);
    tx_fx_memset(mem, 0, words * 2u);
    h->mem = mem;
    for (k = 0; k < HQ_NL; k++) {
        h->len[k] = HQ_LEN[k];
        h->rl[k] = HQ_LEN[k] + HQ_MODMAX + 2u;
        h->off[k] = o;
        o += h->rl[k];
        h->half[k >> 3].ph[k & 7u] = k * 0x2C000000u;
    }
    h->sz = -1;
    (void)words;                                   /* (o = words: HQ_HALL_WORDS, checked by txtest) */
}
uint32_t tx_hq_words(void) { uint32_t k, o = 0; for (k = 0; k < HQ_NL; k++) o += HQ_LEN[k] + HQ_MODMAX + 2u; return o; }

/* the gains for T60 t (ms) and TONE's damping d (0 bright .. 60 .. 150 dark): line k's gain 10^(-3 len / (fs T60))
 * below the shelf's corner (HQ_SHC), above it the gain for T60 x ratio (g^(1 / ratio)): the same on every line */
static void hq_gains(tx_hq_t *h, int32_t t, int32_t d)
{
    uint32_t k, rq = (31130u * exp2_neg((uint32_t)d * 65536u / 31u)) >> 15;   /* the ratio (Q15): 0.95 x 2^(-d / 31) */
    if (rq < 1966u) rq = 1966u;                    /* (0.06: TONE's darkest) */
    for (k = 0; k < HQ_NL; k++) {
        uint32_t x = (uint32_t)h->len[k] * 14810u / (uint32_t)t;   /* -log2 of the gain, Q16 */
        uint32_t xh = x / rq * 32768u + (x % rq) * 32768u / rq;     /* x / ratio */
        int32_t g = (int32_t)exp2_neg(x), gh = (int32_t)exp2_neg(xh);
        g = g > 32700 ? 32700 : g;
        h->g[k] = g;
        h->a[k] = gh > g ? g : gh;
    }
    h->damp = d;
}

/* the ends of lines 8 hf .. 8 hf + 7, a control block, into half[hf] */
static void hq_half(tx_hq_t *h, uint32_t hf)
{
    uint32_t j, i;
    for (j = 0; j < HQ_NL / 2u; j++) {
        uint32_t k = hf * (HQ_NL / 2u) + j, rl = h->rl[k], r, i0;
        const int16_t *b = h->mem + h->off[k];
        int32_t gh = h->a[k], gd = h->g[k] - gh, l = h->half[hf].lp[j], c = h->half[hf].ge[j], f, p, q, x, lo;
        int32_t *y = h->half[hf].y[0] + j;
        r = (uint32_t)h->len[k] << 8;
        uint32_t ph = h->half[hf].ph[j] += HQ_LFO / 8u * HQ_RATE[k];   /* every read modulated, its own rate: */
        r -= (uint32_t)((h->jmd << 8) + ((tx_fx_sine(ph) * h->jmd) >> 7));   /* up to 2 x depth nearer */
        i0 = h->w[k] + rl - (r >> 8) - 1u;          /* (r's fraction from i0 toward i0 + 1) */
        i0 = i0 >= rl ? i0 - rl : i0;
        f = 256 - (int32_t)(r & 255u);
        if (f == 256) { f = 0; i0 = i0 + 1u >= rl ? 0u : i0 + 1u; }
        p = b[i0];
        lo = (p < 0 ? -p : p) + (l < 0 ? -l : l) <= HQ_QUIET;   /* quiet: rounds toward zero this block (it ends) */
        if (lo) c = 0;
        i0 = i0 + 1u >= rl ? 0u : i0 + 1u;          /* (the next read; runs up to the ring's end, then from its start) */
        for (i = 0; i < HL_NC; ) {
            uint32_t m = rl - i0 < HL_NC - i ? rl - i0 : HL_NC - i;
            const int16_t *bq = b + i0, *be = bq + m;
            if (!lo)
                for (; bq < be; bq++, y += HQ_NL / 2u) {
                    q = *bq;
                    x = p + (((q - p) * f + 128) >> 8);
                    p = q;
                    l += ((x - l) * HQ_SHC + 16384) >> 15;   /* the shelf's low part (a one-pole; rounded: the
                                                     * floor's bias held a limit cycle under TONE 0, -95 dBFS for ever) */
                    c += x * gh + l * gd;           /* the gains (low x g, high x g_hi), / 4 for the Hadamard, the */
                    *y = c >> 17; c &= 131071;      /* remainder carried (no loss a pass) */
                }
            else
                for (; bq < be; bq++, y += HQ_NL / 2u) {
                    q = *bq;
                    x = p + (((q - p) * f + 128) >> 8);
                    p = q;
                    l += sh_tz((x - l) * HQ_SHC, 15);
                    *y = sh_tz(x * gh + l * gd, 17);
                }
            i += m;
            i0 = i0 + m >= rl ? 0u : i0 + m;
        }
        h->half[hf].lp[j] = l; h->half[hf].ge[j] = c;
    }
}
/* the early reflections: the undiffused send (written into its ring before), 12 taps a side (/ 16 each: no overflow),
 * a tap's 32 samples split at the ring's wrap; into half[1] (the other core's lines) */
static void hq_er(tx_hq_t *h)
{
    int32_t *el = h->half[1].el, *er = h->half[1].er;
    uint32_t i, k;
    for (i = 0; i < HL_NC; i++) el[i] = er[i] = 0;
    for (k = 0; k < 2u * HQ_NT; k++) {
        uint32_t c = k & 1u, t = (h->ew - HQ_ET[c][k >> 1]) & (HQ_ERN - 1u), m = HQ_ERN - t < HL_NC ? HQ_ERN - t : HL_NC;
        int32_t g = HQ_EG[c][k >> 1], *o = c ? er : el;
        const int16_t *q = h->er + t;
        for (i = 0; i < m; i++) o[i] += (q[i] * g) >> 4;
        for (q = h->er; i < HL_NC; i++) o[i] += (q[i - m] * g) >> 4;
    }
}
void tx_hq_job(void *a) { hq_half((tx_hq_t *)a, 1u); hq_er((tx_hq_t *)a); }   /* (the other core's: lines 8..15, the
                                                     * early reflections) */

/* one control block: CTL frames of the send, HL_NC samples out (mid, side); sz the size step 0 .. 140: T60 12.8 s x
 * 2^((sz - 140) / 30), 0.5 .. 12.8 s (the old HALL's law: DECAY x0.5 .. x2) */
void tx_hq_run(tx_hall_t *hv, const int32_t *send, const int32_t *add, int32_t sz, int32_t damp, int32_t md, int32_t *om,
               int32_t *os, const tx_rv_par_t *par)
{
    tx_hq_t *h = (tx_hq_t *)(void *)hv;             /* (tx_hall_run's type: the caller's one call takes either) */
    int16_t *a = h->apb, vb[HL_NC][HQ_NL];
    uint32_t i, k;
    int taken;
    const int32_t *el = h->half[1].el, *er = h->half[1].er;
    sz = sz < 0 ? 0 : sz > 140 ? 140 : sz;
    if (h->sz != sz || h->damp != damp) {
        int32_t t = (int32_t)((12800u * exp2_neg((uint32_t)(140 - sz) * 65536u / 30u)) >> 15);
        hq_gains(h, t < 100 ? 100 : t, damp);
        h->sz = sz;
    }
    h->jmd = md > 12 ? 12 : md;
    for (i = 0; i < HL_NC; i++) h->bx[i] = send[i] + (add ? add[i] : 0);   /* (the caller's DC blocker before: no RAM
                                                     * code called from here, either core) */
    for (i = 0; i < HL_NC; i++) h->er[(h->ew + i) & (HQ_ERN - 1u)] = (int16_t)hl_c16(h->bx[i]);
    taken = rv_fork(par, tx_hq_job, h);             /* lines 8..15 and the early reflections there (or here, after; all
                                                     * in flash: core 1 runs the same code from XIP) */
    for (k = 0; k < HQ_NAP; k++) {                  /* the input diffusion, each allpass's run split at its ring's end */
        uint32_t d = h->apd[k], n = HQ_AP[k], m, j = 0;
        int32_t g = HQ_APG[k], *x = h->bx;
        while (j < HL_NC) {
            int16_t *q = a + d, *e;
            m = n - d < HL_NC - j ? n - d : HL_NC - j;
            for (e = q + m; q < e; q++, x++) {
                int32_t y = *q, v = *x + sh_tz(y * g, 14);
                *q = (int16_t)hl_c16(v);
                *x = y - ((v * g + 8192) >> 14);
            }
            j += m;
            d = d + m >= n ? 0u : d + m;
        }
        h->apd[k] = (uint16_t)d;
        a += n;
    }
    hq_half(h, 0u);
    rv_join(par, tx_hq_job, h, taken);
    h->ew = (h->ew + HL_NC) & (HQ_ERN - 1u);
    for (i = 0; i < HL_NC; i++) {                   /* the Hadamard (16: 4 stages of sums); its rows 1 and 2 (the signs
                                                     * of k's bits 0, 1: orthogonal) are the outputs L, R */
        const int32_t *y0 = h->half[0].y[i], *y1 = h->half[1].y[i];
        int32_t v0 = y0[0], v1 = y0[1], v2 = y0[2], v3 = y0[3], v4 = y0[4], v5 = y0[5], v6 = y0[6], v7 = y0[7];
        int32_t v8 = y1[0], v9 = y1[1], va = y1[2], vb_ = y1[3], vc = y1[4], vd = y1[5], ve = y1[6], vf = y1[7];
        int32_t x = (h->bx[i] * HQ_INJ) >> 15, l, r, s;
        int16_t *o = vb[i];
#define BF(a, b) (s = a, a = s + b, b = s - b)
        BF(v0, v1); BF(v2, v3); BF(v4, v5); BF(v6, v7); BF(v8, v9); BF(va, vb_); BF(vc, vd); BF(ve, vf);
        BF(v0, v2); BF(v1, v3); BF(v4, v6); BF(v5, v7); BF(v8, va); BF(v9, vb_); BF(vc, ve); BF(vd, vf);
        BF(v0, v4); BF(v1, v5); BF(v2, v6); BF(v3, v7); BF(v8, vc); BF(v9, vd); BF(va, ve); BF(vb_, vf);
        BF(v0, v8); BF(v1, v9); BF(v2, va); BF(v3, vb_); BF(v4, vc); BF(v5, vd); BF(v6, ve); BF(v7, vf);
#undef BF
        l = v1; r = v2;
        o[0] = (int16_t)hl_c16(v0 + x); o[1] = (int16_t)hl_c16(v1 + x); o[2] = (int16_t)hl_c16(v2 - x); o[3] = (int16_t)hl_c16(v3 - x);
        o[4] = (int16_t)hl_c16(v4 + x); o[5] = (int16_t)hl_c16(v5 + x); o[6] = (int16_t)hl_c16(v6 - x); o[7] = (int16_t)hl_c16(v7 - x);
        o[8] = (int16_t)hl_c16(v8 + x); o[9] = (int16_t)hl_c16(v9 + x); o[10] = (int16_t)hl_c16(va - x); o[11] = (int16_t)hl_c16(vb_ - x);
        o[12] = (int16_t)hl_c16(vc + x); o[13] = (int16_t)hl_c16(vd + x); o[14] = (int16_t)hl_c16(ve - x); o[15] = (int16_t)hl_c16(vf - x);
        l = l * HQ_OUT + (el[i] >> 11) * HQ_ERG;    /* (lines at / 4 already: x 4 x HQ_OUT / 16) */
        r = r * HQ_OUT + (er[i] >> 11) * HQ_ERG;
        om[i] = ((l + r) * 19) >> 5;                /* (x 19 / 32, not / 2: +1.5 dB, the old HALL's wet level) */
        os[i] = ((l - r) * 19) >> 5;
    }
    for (k = 0; k < HQ_NL; k++) {                   /* into the rings: each line's block, split at its ring's end */
        uint32_t w = h->w[k], rl = h->rl[k], m = rl - w < HL_NC ? rl - w : HL_NC;
        int16_t *q = h->mem + h->off[k] + w;
        for (i = 0; i < m; i++) q[i] = vb[i][k];
        for (q = h->mem + h->off[k]; i < HL_NC; i++) q[i - m] = vb[i][k];
        h->w[k] = w + HL_NC >= rl ? w + HL_NC - rl : w + HL_NC;
    }
}
