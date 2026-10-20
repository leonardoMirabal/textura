/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura DISPERSE core: the decimated stretched-allpass spring loop (tx_core_disperse.h). */
#include "tx_core_disperse.h"

#define FS4 11025u

static void clear(tx_spring_t *s)
{
    unsigned i, j;
    for (i = 0; i < TX_DSP_M; i++) for (j = 0; j < TX_DSP_K; j++) s->xs[i][j] = s->ys[i][j] = 0;
    for (i = 0; i < TX_DSP_D; i++) s->dl[i] = 0;
    s->lp = s->y0 = s->y1 = s->olp = s->peak = 0;
    s->kp = s->dpos = s->quiet_n = 0;
    s->phase = 0;
}

void tx_spring_init(tx_spring_t *s)
{
    uint8_t *b = (uint8_t *)s;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    s->a = -9830;                                            /* -0.6, Q14 */
    s->ola = 15500;                                          /* 4.5 kHz one-pole at fs (image low-pass) */
    tx_spring_set(s, 40000, 3000, 40, 26214);
}

void tx_spring_set(tx_spring_t *s, uint32_t td_us, uint32_t fc_hz, unsigned m, int32_t g_q15)
{
    uint32_t k = fc_hz ? (FS4 + fc_hz) / (2u * fc_hz) : 1u, T, d;
    k = k < 1u ? 1u : k > TX_DSP_K ? TX_DSP_K : k;
    m = m < 4u ? 4u : m > TX_DSP_M ? TX_DSP_M : m;
    if (k != s->k || m != s->m) { clear(s); s->k = (uint16_t)k; s->m = (uint16_t)m; }
    T = td_us / 1000u * FS4 / 1000u + (td_us % 1000u) * FS4 / 1000000u;   /* T_D in decimated samples (32-bit) */
    d = T > m * k / 2u + 64u ? T - m * k / 2u : 64u;         /* the chain's own mean delay is ~ m k / 2; >= 64
                                                              * (one block of decimated samples: block processing) */
    d = d > TX_DSP_D - 1u ? TX_DSP_D - 1u : d;
    if (d != s->dlen) { s->dlen = (uint16_t)d; if (s->dpos >= d) s->dpos = 0; }
    s->g = g_q15 < 0 ? 0 : g_q15 > 32000 ? 32000 : g_q15;
    s->lpa = 32768 - (int32_t)tx_exp2((15 << 16) - (int32_t)((fc_hz > 5000u ? 5000u : fc_hz) * 3448u >> 6));   /* at fs / 4 */
}

TX_AUDIO_CODE void tx_spring_add(tx_spring_t *s, int32_t *out, unsigned n)
{
    int32_t v[TX_BLOCK / 4u + 1u], ylp[TX_BLOCK / 4u + 1u];
    unsigned j, i, nd = 0, ph = s->phase, kp0 = s->kp, k = s->k, m = s->m, dlen = s->dlen, dpos = s->dpos;
    int32_t a = s->a, g = s->g, lpa = s->lpa, ola = s->ola, lp = s->lp, peak = s->peak;
    if (!s->active) return;
    for (j = 0; j < n; j++) if (((ph + j) & 3u) == 0u) nd++;  /* decimated samples in this block */
    /* the loop delay is >= 64 decimated samples (tx_spring_set), so this block's chain inputs were all written
     * earlier: the chain runs section by section over the block, its state in locals */
    for (i = 0; i < nd; i++) { unsigned q = dpos + i; v[i] = s->dl[q >= dlen ? q - dlen : q]; }
    for (i = 0; i < m; i++) {
        int32_t xs[TX_DSP_K], ys[TX_DSP_K];                  /* the section's history in locals */
        unsigned kp = kp0, t, q;
        for (q = 0; q < k; q++) { xs[q] = s->xs[i][q]; ys[q] = s->ys[i][q]; }
        if (k == 1u) {                                       /* (the common k = 1 case in registers) */
            int32_t x1 = xs[0], y1 = ys[0];
            for (t = 0; t < nd; t++) {
                int32_t yy = ((a * (v[t] - y1) + 8192) >> 14) + x1;
                x1 = v[t]; y1 = yy; v[t] = yy;
            }
            xs[0] = x1; ys[0] = y1;
        } else
            for (t = 0; t < nd; t++) {                       /* y = a (x - y_k) + x_k, rounded: floor's -1/2 LSB */
                int32_t yy = ((a * (v[t] - ys[kp]) + 8192) >> 14) + xs[kp];   /* per section built a DC in the loop */
                xs[kp] = v[t];
                ys[kp] = yy;
                v[t] = yy;
                kp = kp + 1u >= k ? 0u : kp + 1u;
            }
        for (q = 0; q < k; q++) { s->xs[i][q] = xs[q]; s->ys[i][q] = ys[q]; }
    }
    for (i = 0; i < nd; i++) {                               /* loop low-pass, gain (toward zero), kick, write */
        int32_t w, mm;
        lp += ((v[i] - lp) * (lpa >> 1) + 8192) >> 14;
        mm = lp * (g >> 1);
        w = (i == 0 ? s->kick : 0) + ((mm + ((mm >> 31) & 16383)) >> 14);   /* -1 x g must not stay -1 */
        w = w > 32767 ? 32767 : w < -32767 ? -32767 : w;
        s->dl[dpos] = (int16_t)w;
        if ((w < 0 ? -w : w) > peak) peak = w < 0 ? -w : w;
        if (++dpos >= dlen) {                                /* a pass: still ringing? */
            dpos = 0;
            if (peak < 8) s->quiet_n++; else s->quiet_n = 0;   /* -72 dBFS: rounding noise */
            peak = 0;
        }
        ylp[i] = lp;
    }
    if (nd) s->kick = 0;
    s->kp = (uint16_t)((kp0 + nd) % (k ? k : 1u));
    s->dpos = (uint16_t)dpos; s->lp = lp; s->peak = peak;
    {   /* x 4 interpolation + the image low-pass */
        int32_t y0 = s->y0, y1 = s->y1, olp = s->olp;
        unsigned d = 0;
        for (j = 0; j < n; j++) {
            int32_t y;
            if (!ph) { y0 = y1; y1 = ylp[d++]; }
            y = y0 + (((y1 - y0) * (int32_t)ph) >> 2);
            olp += ((y - olp) * (ola >> 1)) >> 14;
            out[j] += olp * 256;                             /* Q15 -> Q23 */
            ph = (ph + 1u) & 3u;
        }
        s->y0 = y0; s->y1 = y1; s->olp = olp;
    }
    s->phase = (uint8_t)ph;
    if (s->quiet_n > 3u) { clear(s); s->active = 0; }
}
