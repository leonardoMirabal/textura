/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Textura reverb TYPE HALL: Felucca's hall reverb as Frodhes runs it (fx.c rev_hall: 8 lines in one ring, a 2-allpass
 * diffuser, the 8 x 8 feedback matrix, the lines 4x hotter than Felucca's for 12 dB less rounding noise), taken over
 * as one instance (its state in tx_hall_t instead of statics). Textura runs it at the full rate (44.1 kHz): its
 * lengths x 2, its gains from the same times, its damping one-pole as at the half rate (rv_weight), no 2:1 down and
 * no 1:2 up; and every one of its 8 lines both damped and read modulated (the design: 4 damped, 4 modulated). Integers
 * only. */
#include "tx_fx_kern.h"

#define HL_MOD 8
#define HL_KD(d) (32767 - (d) * 180)
#define HL_LOUD 32
#define HL_OUT 1
#define HL_LFO 1153115u             /* 0.37 Hz a control block: 0.37 x 32 / 44100 x 2^32 */
static const uint16_t HL_LEN[HL_NL] = {467 * RV_X, 509 * RV_X, 547 * RV_X, 593 * RV_X, 631 * RV_X, 673 * RV_X, 709 * RV_X,
                                        743 * RV_X};
static const uint16_t HL_AP[HL_NAP] = {131 * RV_X, 307 * RV_X};
static const int16_t HL_APG[HL_NAP] = {11469, 10240};

/* the input (shared by every kernel): HL_IN, the DC blocker (its corner as at the half rate: 2^-6 a sample) */
TX_RAM TX_AUDIO_CODE void tx_rev_down(const int32_t *in, int32_t *php, int32_t *phe, int32_t *out)
{
    int32_t hp = *php, he = *phe;
    uint32_t i;
    for (i = 0; i < HL_NC; i++) {
        int32_t x = mulq15(clampi(in[i], -131071, 131071), HL_IN), v;
        v = x - hp + he;
        he = v & 63;
        hp += v >> 6;
        out[i] = x - hp;
    }
    *php = hp, *phe = he;
}

static void hall_gains(tx_hall_t *h, int32_t s)
{
    uint32_t q = exp2_neg((uint32_t)s * 65536u / 30u), k, g;
    for (k = 0; k < HL_NL; k++) {
        g = exp2_neg(((HL_LEN[k] / RV_X) * 59u * q) >> 15);   /* (a pass's time: the half-rate length) */
        h->g[k] = (int16_t)(((g > 32440u ? 32440u : g) * 6193u) >> 15);
    }
    h->sz = (int16_t)(s + 1);
}
TX_INLINE void hall_ap(int16_t *q, int32_t g, int32_t *x, uint32_t n)
{
    int16_t *e = q + n;
    for (; q < e; q++, x++) {
        int32_t y = *q, v = *x + sh_tz(y * g, 14);
        *q = (int16_t)hl_c16(v);
        *x = y - ((v * g + 8192) >> 14);
    }
}
/* a line's end: the modulated read (linear, f / 256 past b), the damping one-pole, the gain */
TX_INLINE void hall_end(const int16_t *b, int32_t f, int16_t *y, uint32_t n, int32_t g, int lo, int32_t *lp, int32_t *le,
                        int32_t kd)
{
    int32_t l = *lp, r = *le, e, p = *b, q, x;
    if (lo)
        do {
            b += HL_NL; q = *b; x = p + (((q - p) * f + 128) >> 8); p = q;
            e = (x - l) * kd + r, r = e & 32767, l += e >> 15;
            *y = (int16_t)sh_tz(l * g, 14); y += HL_NL / 2u;
        } while (--n);
    else
        do {
            b += HL_NL; q = *b; x = p + (((q - p) * f + 128) >> 8); p = q;
            e = (x - l) * kd + r, r = e & 32767, l += e >> 15;
            *y = (int16_t)((l * g + 8192) >> 14); y += HL_NL / 2u;
        } while (--n);
    *lp = l;
    *le = r;
}
#define HL_RUNS(d, len, k, s, body)                                                         \
    for (s = 0; s < HL_NC; s += k) {                                                        \
        k = (len) - (d) < HL_NC - s ? (len) - (d) : HL_NC - s;                              \
        body;                                                                               \
        d = (d) + k >= (len) ? 0u : (d) + k;                                                \
    }

void tx_hall_start(tx_hall_t *h)                           /* the lines cleared (27 KB: a TYPE change, not every block) */
{
    tx_fx_memset(h, 0, sizeof *h);
}

/* the ends of lines 4 hf .. 4 hf + 3, a control block, into half[hf] (its own state and buffers: the two halves may
 * run on two cores; every read is at least 866 samples old, older than the block) */
TX_RAM static void hall_half(tx_hall_t *h, uint32_t hf)
{
    uint32_t j, s, m, d, w = h->jw;
    int32_t kd = h->jkd, md = h->jmd;
    for (j = 0; j < HL_NL / 2u; j++) {
        uint32_t k = hf * (HL_NL / 2u) + j, len = HL_LEN[k], r;
        const int16_t *b = h->comb + k;
        int32_t g = h->g[k], f, lo;
        d = w + HL_N - len;
        r = (uint32_t)(((md + 2) << 8) + ((tx_fx_sine(h->ph * (1u + (k & 1u)) + k * 0x2C000000u) * md) >> 7)) * RV_X;
        f = (int32_t)(r & 255u);
        d += r >> 8;
        d = d >= HL_N ? d - HL_N : d;
        s = d + len / 2u;
        s = s >= HL_N ? s - HL_N : s;
        lo = (b[d * HL_NL] < 0 ? -b[d * HL_NL] : b[d * HL_NL]) + (b[s * HL_NL] < 0 ? -b[s * HL_NL] : b[s * HL_NL]) < HL_LOUD;
        HL_RUNS(d, HL_N, m, s, hall_end(b + d * HL_NL, f, h->half[hf].y[s] + j, m, g, lo, &h->half[hf].lp[j],
                                        &h->half[hf].le[j], kd));
    }
}
TX_RAM void tx_hall_job(void *a) { hall_half((tx_hall_t *)a, 1u); }   /* (the other core's: lines 4..7) */

/* one control block (CTL frames of the send in, HL_NC samples out): mid, side */
TX_RAM TX_AUDIO_CODE void tx_hall_run(tx_hall_t *h, const int32_t *send, const int32_t *add, int32_t size, int32_t damp, int32_t md,
                               int32_t *ol, int32_t *or, const tx_rv_par_t *par)
{
    int16_t *a = h->ap, *o;
    const int16_t *y, *z;
    uint32_t i, k, s, m, d, w = h->w;
    int taken;
    if (h->sz != size + 1) TX_FAR(hall_gains)(h, size);   /* (a SIZE change: in XIP) */
    h->ph += HL_LFO;
    h->jw = w; h->jkd = rv_weight(HL_KD(damp)); h->jmd = md;
    taken = rv_fork(par, TX_C1(tx_hall_job), h);           /* the line ends: 4..7 there (or here, after), 0..3 here */
    tx_rev_down(send, &h->hp, &h->he, h->bx);
    if (add) for (i = 0; i < HL_NC; i++) h->bx[i] += add[i];   /* (Textura: the shimmer, into the lines' input) */
    for (k = 0; k < HL_NAP; k++) {
        d = h->d[k];
        HL_RUNS(d, HL_AP[k], m, s, hall_ap(a + d, HL_APG[k], h->bx + s, m));
        h->d[k] = (uint16_t)d;
        a += HL_AP[k];
    }
    hall_half(h, 0u);
    rv_join(par, tx_hall_job, h, taken);
    /* the feedback matrix (8 x 8, 0 on the diagonal, +-1 elsewhere), into the ring's next rows, and the outputs */
    o = h->comb + w * HL_NL;
    for (i = 0, y = h->half[0].y[0], z = h->half[1].y[0]; i < HL_NC; i++, y += HL_NL / 2u, z += HL_NL / 2u, o += HL_NL) {
        int32_t y0 = y[0], y1 = y[1], y2 = y[2], y3 = y[3], y4 = z[0], y5 = z[1], y6 = z[2], y7 = z[3], x = h->bx[i];
        int32_t u = y3 + y4, r = y1 + y2 + y5 + y6, l = y0 + u - y7, t = r + u + y7, c = x - (y0 + t);
        ol[i] = (l + r) * HL_OUT;
        or[i] = (l - r) * HL_OUT;
        o[0] = (int16_t)hl_c16(t + x);
        o[1] = (int16_t)hl_c16(2 * (y2 + y3 + y5) + y1 + c);
        o[2] = (int16_t)hl_c16(2 * (y3 + y4 + y6) + y2 + c);
        o[3] = (int16_t)hl_c16(2 * (y4 + y5 + y7) + y3 + c);
        o[4] = (int16_t)hl_c16(2 * (y5 + y6 + y1) + y4 + c);
        o[5] = (int16_t)hl_c16(2 * (y6 + y7 + y2) + y5 + c);
        o[6] = (int16_t)hl_c16(2 * (y7 + y1 + y3) + y6 + c);
        o[7] = (int16_t)hl_c16(2 * (y1 + y2 + y4) + y7 + c - 2 * x);
    }
    if (!w) for (i = 0; i < HL_NL; i++) h->comb[HL_N * HL_NL + i] = h->comb[i];
    w += HL_NC;
    h->w = (uint16_t)(w >= HL_N ? 0u : w);
}
