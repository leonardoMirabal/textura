/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura SYNC core: phase-locked emitters into a complex resonance (tx_core_sync.h). */
#include "tx_core_sync.h"

#define SAT (1 << 15)

static int32_t gauss_q12(tx_rng_t *r)
{
    uint32_t a = tx_rand(r), b = tx_rand(r);
    int32_t s = (int32_t)((a >> 16) + (a & 0xFFFFu) + (b >> 16) + (b & 0xFFFFu)) - 131072;
    return (s * 7094) >> 15;
}

void tx_sync_init(tx_sync_t *s)
{
    tx_zero(s, sizeof *s);
    s->asat = 1 << 14;
}

void tx_sync_res(tx_sync_t *s, unsigned j, uint32_t f_hz, uint32_t q_x16)
{
    uint32_t w = f_hz * 97391u;                              /* phase per sample */
    int32_t rr, c, sn;
    if (j > 1u) return;
    if (q_x16 < 16u) q_x16 = 16u;
    /* r = exp(-pi f / (Q fs)) = 2^(-f / (9731 Q)), Q15: exponent Q16 = f x 107.75 x 16 / q_x16 / 16 */
    rr = (int32_t)tx_exp2((15 << 16) - (int32_t)(f_hz * 1724u / q_x16));
    if (rr > 32760) rr = 32760;
    c = tx_sin_q15(w + 0x40000000u); sn = tx_sin_q15(w);
    s->rc[j] = (rr * c) >> 15;
    s->rs[j] = (rr * sn) >> 15;
}

void tx_sync_emitters(tx_sync_t *s, tx_rng_t *r, unsigned n, uint32_t f_hz, int32_t spread_q15)
{
    unsigned i;
    uint32_t inc = f_hz * 97391u;
    if (n > TX_SYNC_N) n = TX_SYNC_N;
    s->n = (uint8_t)n;
    for (i = 0; i < n; i++) {
        int32_t d = (gauss_q12(r) * (spread_q15 >> 3)) >> 9;   /* relative, Q15 */
        s->inc[i] = (uint32_t)((int64_t)inc + (((int64_t)inc * d) >> 15));
        s->ph[i] = tx_rand(r);
    }
}

TX_AUDIO_CODE unsigned tx_sync_add(tx_sync_t *s, tx_rng_t *r, int32_t out_q8, int32_t *out, unsigned n)
{
    int32_t e[2][TX_BLOCK];
    unsigned i, j, hits = 0, h;
    int32_t a, kpull[2];
    for (h = 0; h < 2u; h++) for (j = 0; j < n; j++) e[h][j] = 0;
    for (h = 0; h < 2u; h++) {                               /* the pull and the impulse fade as the wave nears A_sat */
        int32_t A = (s->x[h] < 0 ? -s->x[h] : s->x[h]) + (s->y[h] < 0 ? -s->y[h] : s->y[h]);
        int32_t room = s->asat - A;
        kpull[h] = room > 0 ? s->k * room / s->asat : 0;     /* k <= 2^12, room <= 2^15: 32-bit */
    }
    a = s->amp;
    for (i = 0; i < s->n; i++) {
        uint32_t ph = s->ph[i], inc = s->inc[i];
        unsigned g = i & 1u;
        int32_t A = (s->x[g] < 0 ? -s->x[g] : s->x[g]) + (s->y[g] < 0 ? -s->y[g] : s->y[g]);
        int32_t ai = A < s->asat ? a * (s->asat - A) / s->asat : 0;   /* a <= 2^14 */
        if (kpull[g]) {                                      /* dphi = K (y cos phi - x sin phi), per block */
            int32_t t = (s->y[g] * (tx_sin_q15(ph + 0x40000000u) >> 2) - s->x[g] * (tx_sin_q15(ph) >> 2)) >> 13;   /* Q14 */
            ph += (uint32_t)(t * kpull[g]) * 256u;            /* (a phase: wraps by design) */
        }
        if (s->jit) ph += (uint32_t)((gauss_q12(r) * (int32_t)s->jit) >> 12) * 65536u;
        /* collisions in this block: the phase wraps */
        {
            uint32_t left = 0xFFFFFFFFu - ph;                /* phase to the next wrap, minus 1 */
            uint32_t off = left / (inc ? inc : 1u);
            while (off < n) {
                e[g][off] += ai;
                hits++;
                off += 0xFFFFFFFFu / (inc ? inc : 1u) + 1u;
            }
        }
        s->ph[i] = ph + inc * n;
    }
    for (h = 0; h < 2u; h++) {                               /* the resonances */
        int32_t x = s->x[h], y = s->y[h], rc = s->rc[h], rs = s->rs[h];
        if (!x && !y) { int32_t any = 0; for (j = 0; j < n; j++) any |= e[h][j]; if (!any) continue; }
        for (j = 0; j < n; j++) {
            int32_t mx = rc * x - rs * y, my = rs * x + rc * y;   /* rounded toward zero: |z| only shrinks, no */
            int32_t nx = ((mx + ((mx >> 31) & 32767)) >> 15) + e[h][j];   /* limit cycle, exact zero */
            int32_t ny = (my + ((my >> 31) & 32767)) >> 15;
            x = nx > SAT - 1 ? SAT - 1 : nx < -(SAT - 1) ? -(SAT - 1) : nx;
            y = ny > SAT - 1 ? SAT - 1 : ny < -(SAT - 1) ? -(SAT - 1) : ny;
            out[j] += x * out_q8 * 2;                        /* Q14 x Q8 x 2 -> Q23 */
        }
        if (x > -4 && x < 4 && y > -4 && y < 4) {            /* rung out (or a rounding cycle): exact zero */
            int32_t any = 0;
            for (j = 0; j < n; j++) any |= e[h][j];
            if (!any) x = y = 0;
        }
        s->x[h] = x; s->y[h] = y;
    }
    return hits;
}
