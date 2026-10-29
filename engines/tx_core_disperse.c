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
    tx_zero(s, sizeof *s);
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
    i = 0;
    if (k == 1u && nd >= 4u)                                 /* k 1: four sections at once, as a wavefront (section */
        for (; i + 3u < m; i += 4u) {                        /* i + j runs j samples behind section i): four */
            int32_t x0 = s->xs[i][0], y0 = s->ys[i][0], x1 = s->xs[i + 1][0], y1 = s->ys[i + 1][0];   /* independent */
            int32_t x2 = s->xs[i + 2][0], y2 = s->ys[i + 2][0], x3 = s->xs[i + 3][0], y3 = s->ys[i + 3][0];   /* chains */
            int32_t z;                                       /* instead of one serial one, the same arithmetic */
            unsigned t;                                      /* (bit-exact) */
#define AP1(xx, yy, in) (z = ((a * ((in) - (yy)) + 8192) >> 14) + (xx), (xx) = (in), (yy) = z)
            AP1(x0, y0, v[0]);                               /* the prologue: the wave enters */
            AP1(x1, y1, y0); AP1(x0, y0, v[1]);
            AP1(x2, y2, y1); AP1(x1, y1, y0); AP1(x0, y0, v[2]);
            for (t = 3; t < nd; t++) {                       /* section j at sample t - j; descending, each takes */
                AP1(x3, y3, y2); AP1(x2, y2, y1); AP1(x1, y1, y0); AP1(x0, y0, v[t]);   /* its input of the step */
                v[t - 3] = y3;                               /* before (a section's y is its last output) */
            }
            AP1(x3, y3, y2); AP1(x2, y2, y1); AP1(x1, y1, y0); v[nd - 3] = y3;   /* the epilogue */
            AP1(x3, y3, y2); AP1(x2, y2, y1); v[nd - 2] = y3;
            AP1(x3, y3, y2); v[nd - 1] = y3;
#undef AP1
            s->xs[i][0] = x0; s->ys[i][0] = y0; s->xs[i + 1][0] = x1; s->ys[i + 1][0] = y1;
            s->xs[i + 2][0] = x2; s->ys[i + 2][0] = y2; s->xs[i + 3][0] = x3; s->ys[i + 3][0] = y3;
        }
    if (k == 2u && nd >= 4u)                                 /* k 2: the same, the history two deep */
        for (; i + 3u < m; i += 4u) {                        /* (section i + j runs j samples behind section i): */
            int32_t x[4][2], y[4][2], o[4];                  /* four independent recurrences instead of one serial */
            unsigned t, j, q, kq[4];                         /* chain, the same arithmetic (bit-exact) */
            for (j = 0; j < 4u; j++) {
                for (q = 0; q < k; q++) { x[j][q] = s->xs[i + j][q]; y[j][q] = s->ys[i + j][q]; }
                kq[j] = kp0;
            }
#define AP(j, in) do { int32_t z_ = ((a * ((in) - y[j][kq[j]]) + 8192) >> 14) + x[j][kq[j]]; \
                       x[j][kq[j]] = (in); y[j][kq[j]] = z_; o[j] = z_; kq[j] ^= k - 1u; } while (0)
            for (t = 0; t < 3u; t++) {                       /* the prologue: the wave enters */
                for (j = t + 1u; j-- > 1u;) AP(j, o[j - 1]);   /* (descending: each takes its input of the */
                AP(0, v[t]);                                 /* step before) */
            }
            for (t = 3; t < nd; t++) {                       /* steady state: section j at sample t - j */
                AP(3, o[2]); AP(2, o[1]); AP(1, o[0]); AP(0, v[t]);
                v[t - 3] = o[3];
            }
            for (t = nd; t < nd + 3u; t++) {                 /* the epilogue: the wave leaves */
                for (j = 3u; j > t - nd; j--) AP(j, o[j - 1]);
                v[t - 3] = o[3];
            }
#undef AP
            for (j = 0; j < 4u; j++) for (q = 0; q < k; q++) { s->xs[i + j][q] = x[j][q]; s->ys[i + j][q] = y[j][q]; }
        }
    if (k == 1u && nd)                                       /* k 1: two sections at once, as a wavefront (section */
        for (; i + 1u < m; i += 2u) {                        /* i + 1 one sample behind i): two independent */
            int32_t xa = s->xs[i][0], ya = s->ys[i][0];      /* recurrences instead of one serial chain, the same */
            int32_t xb = s->xs[i + 1][0], yb = s->ys[i + 1][0], pa;   /* arithmetic (bit-exact) */
            unsigned t;
            pa = ((a * (v[0] - ya) + 8192) >> 14) + xa;      /* prologue: section i at 0 */
            xa = v[0]; ya = pa;
            for (t = 1; t < nd; t++) {
                int32_t na = ((a * (v[t] - ya) + 8192) >> 14) + xa;   /* section i at t */
                int32_t nb = ((a * (pa - yb) + 8192) >> 14) + xb;     /* section i + 1 at t - 1 */
                xa = v[t]; ya = na;
                xb = pa; yb = nb; v[t - 1] = nb;
                pa = na;
            }
            {
                int32_t nb = ((a * (pa - yb) + 8192) >> 14) + xb;     /* epilogue: section i + 1 at nd - 1 */
                xb = pa; yb = nb; v[nd - 1] = nb;
            }
            s->xs[i][0] = xa; s->ys[i][0] = ya; s->xs[i + 1][0] = xb; s->ys[i + 1][0] = yb;
        }
    else if (k == 2u && nd)                                  /* k 2: the same wavefront, the history two deep */
        for (; i + 1u < m; i += 2u) {
            int32_t xa0 = s->xs[i][0], xa1 = s->xs[i][1], ya0 = s->ys[i][0], ya1 = s->ys[i][1];
            int32_t xb0 = s->xs[i + 1][0], xb1 = s->xs[i + 1][1], yb0 = s->ys[i + 1][0], yb1 = s->ys[i + 1][1];
            unsigned t, kpa = kp0, kpb;
            int32_t pa, na, nb;
            /* slot kp holds the sample k back; a section writes it and moves on: kp alternates 0 / 1 */
#define AP2(x0, x1, y0, y1, kq, in, out) do { if (kq) { out = ((a * ((in) - y1) + 8192) >> 14) + x1; x1 = (in); y1 = out; } \
                                              else { out = ((a * ((in) - y0) + 8192) >> 14) + x0; x0 = (in); y0 = out; } } while (0)
            AP2(xa0, xa1, ya0, ya1, kpa, v[0], pa);
            kpb = kpa; kpa ^= 1u;
            for (t = 1; t < nd; t++) {
                AP2(xa0, xa1, ya0, ya1, kpa, v[t], na);
                AP2(xb0, xb1, yb0, yb1, kpb, pa, nb);
                v[t - 1] = nb;
                pa = na; kpa ^= 1u; kpb ^= 1u;
            }
            AP2(xb0, xb1, yb0, yb1, kpb, pa, nb);
            v[nd - 1] = nb;
#undef AP2
            s->xs[i][0] = xa0; s->xs[i][1] = xa1; s->ys[i][0] = ya0; s->ys[i][1] = ya1;
            s->xs[i + 1][0] = xb0; s->xs[i + 1][1] = xb1; s->ys[i + 1][0] = yb0; s->ys[i + 1][1] = yb1;
        }
    else if (k == 3u && nd)                                  /* k 3: the wavefront with the histories in locals */
        for (; i + 1u < m; i += 2u) {
            int32_t xa[3], ya[3], xb[3], yb[3], pa, na, nb;
            unsigned t, q, kpa = kp0, kpb;
            for (q = 0; q < 3u; q++) { xa[q] = s->xs[i][q]; ya[q] = s->ys[i][q]; xb[q] = s->xs[i + 1][q]; yb[q] = s->ys[i + 1][q]; }
            pa = ((a * (v[0] - ya[kpa]) + 8192) >> 14) + xa[kpa];
            xa[kpa] = v[0]; ya[kpa] = pa;
            kpb = kpa; kpa = kpa == 2u ? 0u : kpa + 1u;
            for (t = 1; t < nd; t++) {
                na = ((a * (v[t] - ya[kpa]) + 8192) >> 14) + xa[kpa];
                nb = ((a * (pa - yb[kpb]) + 8192) >> 14) + xb[kpb];
                xa[kpa] = v[t]; ya[kpa] = na;
                xb[kpb] = pa; yb[kpb] = nb; v[t - 1] = nb;
                pa = na;
                kpa = kpa == 2u ? 0u : kpa + 1u; kpb = kpb == 2u ? 0u : kpb + 1u;
            }
            nb = ((a * (pa - yb[kpb]) + 8192) >> 14) + xb[kpb];
            xb[kpb] = pa; yb[kpb] = nb; v[nd - 1] = nb;
            for (q = 0; q < 3u; q++) { s->xs[i][q] = xa[q]; s->ys[i][q] = ya[q]; s->xs[i + 1][q] = xb[q]; s->ys[i + 1][q] = yb[q]; }
        }
    for (; i < m; i++) {                                     /* the rest (k 4, an odd last section) */
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
