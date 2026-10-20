/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura WINGBEAT core: harmonic wavetables and the buzzer voice (tx_core_wingbeat.h). */
#include "tx_core_wingbeat.h"

#define TX_WING_HX 24u             /* harmonics in a table */

void tx_wing_table(int16_t *tab, const int8_t *db2, unsigned nh)
{
    int32_t amp[TX_WING_HX], acc[TX_WING_N], peak = 1, last = 0;
    int16_t sn[TX_WING_N];                                   /* one cycle of the sine: harmonic k is sn[(k i) mod N] */
    unsigned k, i;
    for (i = 0; i < TX_WING_N; i++) sn[i] = (int16_t)tx_sin_q15((uint32_t)i << 24);
    if (nh > TX_WING_H) nh = TX_WING_H;
    for (k = 0; k < TX_WING_HX; k++) {                       /* 10^(dB / 20) = 2^(dB x 0.1661), Q15 */
        int32_t d2 = k < nh && db2[k] > -120 ? db2[k] / 2 : last - 2;   /* (dB x 2: half the given dB; then -1.2 dB) */
        if (k && k >= nh) d2 = last - 2;
        if (k && k < nh && db2[k] <= -120) d2 = last - 2;
        last = d2;
        amp[k] = d2 < -120 ? 0 : (int32_t)tx_exp2((15 << 16) + d2 * 5443);   /* dB/2 x 2 x 0.1661 */
    }
    nh = TX_WING_HX;
    for (i = 0; i < TX_WING_N; i++) {
        int32_t s = 0;
        for (k = 0; k < nh; k++)                             /* sin((k + 1) 2 pi i / N), phases 0 */
            s += (sn[((k + 1u) * i) & (TX_WING_N - 1u)] * amp[k]) >> 15;   /* (a load, not a sine: cheaper than the
                                                              * 8 interpolated sines before, though 3x the harmonics) */
        acc[i] = s;
        if (s > peak) peak = s;
        if (-s > peak) peak = -s;
    }
    if (peak < 1024) peak = 1024;
    for (i = 0; i < TX_WING_N; i++) tab[i] = (int16_t)(((acc[i] >> 4) * 32000) / (peak >> 4));   /* |acc| < 2^18 */
    tab[TX_WING_N] = tab[0];
}

TX_AUDIO_CODE void tx_buzz_add(tx_buzz_t *b, const int16_t *front, const int16_t *side, int32_t g1, int32_t mix1,
                               int32_t *out, unsigned n)
{
    uint32_t ph = b->ph, inc = b->inc, cinc = b->cinc ? b->cinc : b->inc, x = b->nr ? b->nr : 0x9E3779B9u;
    int32_t g = b->g, mx = b->mix, gs = (g1 - g) / (int32_t)(n ? n : 1u), ms = (mix1 - mx) / (int32_t)(n ? n : 1u);
    int32_t ca = b->ca ? b->ca : 32768, n1 = b->n1, n2 = b->n2, jit = b->jit, nz = b->nz;
    unsigned j;
    if (!g && !g1) { b->ph += inc * n; b->n1 = b->n2 = 0; return; }
    for (j = 0; j < n; j++) {
        uint32_t k = ph >> 24, fr = (ph >> 16) & 0xFFu, old = ph;
        int32_t f = front[k] + (((front[k + 1] - front[k]) * (int32_t)fr) >> 8);
        int32_t s = side[k] + (((side[k + 1] - side[k]) * (int32_t)fr) >> 8);
        int32_t y = f + (((s - f) * (mx >> 1)) >> 14);         /* crossfade, Q15 */
        int32_t u, se;
        y = (y * (ca >> 1)) >> 14;                           /* this stroke's level */
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;             /* the stroke's turbulence */
        u = (int32_t)(x >> 17) - 16384;                      /* Q14 */
        n1 += (u - n1) >> 2;                                 /* low-pass ~2.5 kHz */
        n2 += (n1 - n2) >> 7;                                /* (its own low part, ~150 Hz: taken off) */
        se = (f * f) >> 15;                                  /* pulsed by the stroke (its own waveform, squared) */
        y += (((n1 - n2) * se) >> 14) * nz >> 15;
        out[j] += y * (g >> 7) + ((y * (g & 127)) >> 7);     /* Q15 x Q15 -> Q23, the gain's low bits kept: a fading
                                                              * insect falls to silence, not from -48 dB (session 5) */
        ph += cinc;
        if (ph < old) {                                      /* a new stroke: its own period and level */
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            cinc = inc + (uint32_t)((int32_t)(inc >> 15) * (((int32_t)(x >> 17) - 16384) * jit >> 14));
            ca = 32768 + ((((int32_t)(x & 0xFFFFu)) - 32768) * 3277 >> 15);
        }
        g += gs; mx += ms;
    }
    b->ph = ph; b->g = g1; b->mix = mix1;
    b->cinc = cinc; b->nr = x; b->ca = ca; b->n1 = n1; b->n2 = n2;
}
