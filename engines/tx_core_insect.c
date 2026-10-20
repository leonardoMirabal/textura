/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura INSECT core: cricket pulse tones and impulse-driven resonators (tx_core_insect.h). */
#include "tx_core_insect.h"

void tx_itone_env(int16_t *env, int32_t peak_q15)
{
    unsigned i, pk = (unsigned)((peak_q15 * (int32_t)TX_ITONE_ENV) >> 15);
    if (pk < 1u) pk = 1u;
    if (pk > TX_ITONE_ENV - 1u) pk = TX_ITONE_ENV - 1u;
    for (i = 0; i <= TX_ITONE_ENV; i++) {
        int32_t s;
        uint32_t ph = i <= pk ? (uint32_t)((0x40000000u / pk) * i)                         /* 0 .. pi/2 */
                              : 0x40000000u + (uint32_t)((0x40000000u / (TX_ITONE_ENV - pk)) * (i - pk));
        s = tx_sin_q15(ph);
        env[i] = (int16_t)((s * s) >> 15);
    }
    env[TX_ITONE_ENV] = 0;
}

void tx_itone_start(tx_itone_t *t, uint32_t inc, int32_t glide_q15, uint32_t len, int32_t amp_q15, int32_t h2_q15,
                    int32_t h3_q15, unsigned offset)
{
    if (len < 16u) len = 16u;
    if (!t->on) t->ph = 0;
    t->inc = inc;
    if (glide_q15 > 16384) glide_q15 = 16384;
    t->dinc = -(int32_t)(((inc >> 15) * (uint32_t)glide_q15) / len);   /* the whole fall inc x glide, spread */
    t->epos = 0;
    t->estep = (TX_ITONE_ENV << 16) / len;
    t->amp = amp_q15; t->h2 = h2_q15; t->h3 = h3_q15;
    t->wait = (uint16_t)offset;
    t->on = 1;
}

TX_AUDIO_CODE void tx_itone_add(tx_itone_t *t, const int16_t *env, int32_t *out, unsigned n)
{
    uint32_t ph = t->ph, inc = t->inc, epos = t->epos, estep = t->estep;
    int32_t dinc = t->dinc, amp = t->amp, h2 = t->h2, h3 = t->h3;
    unsigned j = t->wait;
    if (!t->on) return;
    if (j >= n) { t->wait = (uint16_t)(j - n); return; }
    t->wait = 0;
    for (; j < n; j++) {
        uint32_t i = epos >> 16, fr = (epos >> 8) & 0xFFu;
        int32_t e, y;
        if (i >= TX_ITONE_ENV) { t->on = 0; break; }
        e = env[i] + (((env[i + 1] - env[i]) * (int32_t)fr) >> 8);
        y = tx_sin_q15(ph);
        if (h2) y += (tx_sin_q15(ph << 1) * h2) >> 15;
        if (h3) y += (tx_sin_q15(ph * 3u) * h3) >> 15;
        out[j] += (y * ((e * amp) >> 15)) >> 7;              /* Q15 x Q15 -> Q23 */
        ph += inc;
        inc = (uint32_t)((int32_t)inc + dinc);
        epos += estep;
    }
    t->ph = ph; t->inc = inc; t->epos = epos;
}

void tx_ireso_set(tx_ireso_t *r, uint32_t f_hz, uint32_t q_x16)
{
    int32_t x, rq;
    uint32_t w;
    if (f_hz < 20u) f_hz = 20u;
    if (f_hz > 21000u) f_hz = 21000u;
    if (q_x16 < 8u) q_x16 = 8u;
    x = (int32_t)(f_hz * 1078u / (q_x16 * 10u));            /* pi f / (Q fs ln 2) in Q16: 107.8 f / q_x16 */
    rq = (int32_t)(tx_exp2((30 << 16) - x) >> 15);          /* r = exp(-pi f / (Q fs)), Q15 */
    w = f_hz * 97391u;                                       /* 2 pi f / fs as a 32-bit phase */
    r->a1 = (rq * tx_sin_q15(w + 0x40000000u)) >> 15;       /* 2 r cos w in Q14 = r cos w in Q15 */
    r->a2 = (rq * rq) >> 16;                                 /* r^2, Q14 */
    r->b0 = tx_sin_q15(w);
    if (r->b0 < 0) r->b0 = -r->b0;
}

TX_AUDIO_CODE void tx_ireso_add(tx_ireso_t *r, const uint16_t *off, const int16_t *imp, unsigned nimp, int32_t *out,
                                unsigned n)
{
    int32_t y1 = r->y1, y2 = r->y2, a1 = r->a1, a2 = r->a2, b0 = r->b0, g = r->g, y01 = y1, y02 = y2;
    unsigned j, k = 0;
    if (!nimp && !(y1 | y2)) return;
    for (j = 0; j < n; j++) {
        int32_t y = ((a1 * y1) >> 14) - ((a2 * y2) >> 14);
        while (k < nimp && off[k] == j) { y += (imp[k] * b0) >> 15; k++; }
        y = y > 65535 ? 65535 : y < -65535 ? -65535 : y;
        y2 = y1; y1 = y;
        out[j] += y * g;
    }
    if (!nimp && ((y1 > -8 && y1 < 8 && y2 > -8 && y2 < 8) || (y1 == y01 && y2 == y02))) y1 = y2 = 0;
    r->y1 = y1; r->y2 = y2;
}
