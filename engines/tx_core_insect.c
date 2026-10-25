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
        if (t->rasp) { t->nz = t->nz * 1664525u + 1013904223u; y += (((int32_t)(t->nz >> 17) - 16384) * t->rasp) >> 14; }
        out[j] += (y * ((e * amp) >> 15)) >> 7;              /* Q15 x Q15 -> Q23 */
        ph += inc;
        inc = (uint32_t)((int32_t)inc + dinc);
        epos += estep;
    }
    t->ph = ph; t->inc = inc; t->epos = epos;
}

void tx_ireso_coefs(uint32_t f_hz, uint32_t q_x16, int32_t *a1, int32_t *a2, int32_t *b0)
{
    int32_t x, rq;
    uint32_t w;
    if (f_hz < 20u) f_hz = 20u;
    if (f_hz > 21000u) f_hz = 21000u;
    if (q_x16 < 8u) q_x16 = 8u;
    x = (int32_t)(f_hz * 1078u / (q_x16 * 10u));            /* pi f / (Q fs ln 2) in Q16: 107.8 f / q_x16 */
    rq = (int32_t)(tx_exp2((30 << 16) - x) >> 15);          /* r = exp(-pi f / (Q fs)), Q15 */
    w = f_hz * 97391u;                                       /* 2 pi f / fs as a 32-bit phase */
    *a1 = (rq * tx_sin_q15(w + 0x40000000u)) >> 15;         /* 2 r cos w in Q14 = r cos w in Q15 */
    *a2 = (rq * rq) >> 16;                                   /* r^2, Q14 */
    *b0 = tx_sin_q15(w);
    if (*b0 < 0) *b0 = -*b0;
}

void tx_ireso_set(tx_ireso_t *r, uint32_t f_hz, uint32_t q_x16) { tx_ireso_coefs(f_hz, q_x16, &r->a1, &r->a2, &r->b0); }

TX_AUDIO_CODE void tx_tymbal_add(tx_tymbal_t *t, const tx_tev_t *ev, unsigned nev, int32_t *out, unsigned n)
{
    int32_t y1 = t->y1, y2 = t->y2, s1 = t->s1, s2 = t->s2, a1 = t->a1, a1e = t->a1_end, da1 = t->da1, fr = t->frac;
    int32_t a2 = t->a2, b0 = t->b0, sa1 = t->sa1, sa2 = t->sa2, sb0 = t->sb0, g = t->g;
    int32_t z0 = y1 | y2 | s1 | s2, x1 = t->x1, x2 = t->x2, d1 = t->d1, d2 = t->d2, zz = t->zin > 1;
    unsigned j, k = 0;
    if (!nev && !(z0 | x1 | x2 | d1 | d2)) return;
    if (t->zin) {                                            /* the input through (1 - z^-2) (once or twice): every */
        for (j = 0; j < n; j++) {                            /* impulse also pulls back two samples later */
            int32_t y, s, x = 0, d;
            while (k < nev && ev[k].off == j) {
                if (ev[k].a1) { a1 = ev[k].a1; fr = 0; }
                x += (ev[k].amp * b0) >> 15;
                k++;
            }
            d = x - x2; x2 = x1; x1 = x;
            if (zz) { int32_t e = d - d2; d2 = d1; d1 = d; d = e; }
            y1 += d;
            y = ((a1 * y1) >> 14) - ((a2 * y2) >> 14);
            y = y > 65535 ? 65535 : y < -65535 ? -65535 : y;
            y2 = y1; y1 = y;
            s = ((sa1 * s1) >> 14) - ((sa2 * s2) >> 14) + ((y * sb0) >> 15);
            s = s > 65535 ? 65535 : s < -65535 ? -65535 : s;
            s2 = s1; s1 = s;
            out[j] += (s * g) >> 4;
            if (a1 < a1e) { fr += da1; a1 += fr >> 8; fr &= 0xFF; if (a1 > a1e) a1 = a1e; }
        }
    } else
    for (j = 0; j < n; j++) {
        int32_t y, s;
        while (k < nev && ev[k].off == j) {
            if (ev[k].a1) { a1 = ev[k].a1; fr = 0; }
            y1 += (ev[k].amp * b0) >> 15;
            k++;
        }
        y = ((a1 * y1) >> 14) - ((a2 * y2) >> 14);
        y = y > 65535 ? 65535 : y < -65535 ? -65535 : y;
        y2 = y1; y1 = y;
        s = ((sa1 * s1) >> 14) - ((sa2 * s2) >> 14) + ((y * sb0) >> 15);
        s = s > 65535 ? 65535 : s < -65535 ? -65535 : s;
        s2 = s1; s1 = s;
        out[j] += (s * g) >> 4;
        if (a1 < a1e) { fr += da1; a1 += fr >> 8; fr &= 0xFF; if (a1 > a1e) a1 = a1e; }   /* the glide (pitch down) */
    }
    if (!nev && !(x1 | x2 | d1 | d2)) {                    /* silent input: small or frozen -> exact zero */
        int32_t m = (y1 < 0 ? -y1 : y1) | (y2 < 0 ? -y2 : y2) | (s1 < 0 ? -s1 : s1) | (s2 < 0 ? -s2 : s2);
        if (m < 8 || (y1 == t->y1 && y2 == t->y2 && s1 == t->s1 && s2 == t->s2)) y1 = y2 = s1 = s2 = 0;
    }
    t->y1 = y1; t->y2 = y2; t->s1 = s1; t->s2 = s2; t->a1 = a1; t->frac = fr; t->x1 = x1; t->x2 = x2; t->d1 = d1; t->d2 = d2;
}

TX_AUDIO_CODE void tx_ireso_add(tx_ireso_t *r, const uint16_t *off, const int16_t *imp, unsigned nimp, int32_t *out,
                                unsigned n)
{
    int32_t y1 = r->y1, y2 = r->y2, p1 = r->p1, p2 = r->p2, a1 = r->a1, a2 = r->a2, g = r->g, y01 = y1, y02 = y2;
    unsigned j, k = 0;
    if (!nimp && !(y1 | y2 | p1 | p2)) return;
    for (j = 0; j < n; j++) {
        int32_t x = 0, y;
        while (k < nimp && off[k] == j) { x += imp[k]; k++; }
        y = ((a1 * y1) >> 14) - ((a2 * y2) >> 14) + ((x - p2) >> 1);   /* (1 - z^-2) / 2: a band-pass, no DC */
        y = y > 65535 ? 65535 : y < -65535 ? -65535 : y;
        p2 = p1; p1 = x;
        y2 = y1; y1 = y;
        out[j] += y * g;
    }
    {   /* silent input: small (under zlim, default 8) or frozen -> exact zero. Rounding holds limit cycles of up to
         * ~1 / (2 (1 - r)) LSB in a high-r two-pole: a caller with such poles raises zlim. */
        int32_t zl = r->zlim ? r->zlim : 8;
        if (!nimp && !(p1 | p2) && ((y1 > -zl && y1 < zl && y2 > -zl && y2 < zl) || (y1 == y01 && y2 == y02))) y1 = y2 = 0;
    }
    r->y1 = y1; r->y2 = y2; r->p1 = p1; r->p2 = p2;
}
