/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura WINGBEAT core: harmonic wavetables and the buzzer voice (tx_core_wingbeat.h). */
#include "tx_core_wingbeat.h"

void tx_wing_table(int16_t *tab, const int8_t *db2, unsigned nh)
{
    int32_t amp[TX_WING_H], acc[TX_WING_N], peak = 1;
    unsigned k, i;
    if (nh > TX_WING_H) nh = TX_WING_H;
    for (k = 0; k < nh; k++)                                 /* 10^(dB / 20) = 2^(dB x 0.1661), Q15 */
        amp[k] = db2[k] <= -120 ? 0 : (int32_t)tx_exp2((15 << 16) + (int32_t)db2[k] * 5443);   /* dB/2 x 2 x 0.1661 */
    for (i = 0; i < TX_WING_N; i++) {
        int32_t s = 0;
        for (k = 0; k < nh; k++)                             /* sin((k + 1) 2 pi i / N), phases 0 */
            s += (tx_sin_q15((uint32_t)((k + 1u) * i) << 24) * amp[k]) >> 15;
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
    uint32_t ph = b->ph, inc = b->inc;
    int32_t g = b->g, mx = b->mix, gs = (g1 - g) / (int32_t)(n ? n : 1u), ms = (mix1 - mx) / (int32_t)(n ? n : 1u);
    unsigned j;
    if (!g && !g1) { b->ph += inc * n; return; }
    for (j = 0; j < n; j++) {
        uint32_t k = ph >> 24, fr = (ph >> 16) & 0xFFu;
        int32_t f = front[k] + (((front[k + 1] - front[k]) * (int32_t)fr) >> 8);
        int32_t s = side[k] + (((side[k + 1] - side[k]) * (int32_t)fr) >> 8);
        int32_t y = f + (((s - f) * (mx >> 1)) >> 14);         /* crossfade, Q15 */
        out[j] += y * (g >> 7);                              /* Q15 x Q8 -> Q23 */
        ph += inc;
        g += gs; mx += ms;
    }
    b->ph = ph; b->g = g1; b->mix = mix1;
}
