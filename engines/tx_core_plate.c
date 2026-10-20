/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura PLATE core: exact low modes + band-energy diffusion (tx_core_plate.h). */
#include "tx_core_plate.h"

const uint16_t TX_PLATE_BAND_HZ[TX_PLATE_BANDS] = {
    60, 82, 113, 155, 213, 293, 403, 553, 759, 1043, 1433, 1967, 2702, 3711, 5097, 7000, 9614,
};
const uint32_t TX_PLATE_RINV[TX_PLATE_BANDS] = {
    65536, 47718, 34745, 25298, 18420, 13412, 9766, 7111, 5177, 3770, 2745, 1999, 1455, 1060, 771, 562, 409,
};

uint32_t tx_isqrt32(uint32_t x)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) { if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return r;
}

uint32_t tx_plate_decay(uint32_t rate_q8)
{
    /* exp(-rate T) = 2^(-rate T log2 e), T = 256 / 44100 s: T log2 e = 0.0083749 -> x 256 (Q8 -> Q16) = 2.14397 */
    uint32_t x = (uint32_t)(((uint64_t)rate_q8 * 140507u) >> 16);
    if (x > (16u << 16)) return 0;
    return tx_exp2((int32_t)((16u << 16) - x));
}

uint32_t tx_plate_law_q8(uint32_t hz)
{
    /* 8e-3 (2 pi f)^0.6 (1/s), Q8: 2^(0.6 (log2 f + log2 2 pi) + log2 2.048) */
    int64_t l2 = ((int64_t)(131072 - (int32_t)tx_neglog2_q12(hz ? hz : 1u)) << 4) + 173768;   /* log2 omega, Q16 */
    int32_t x = (int32_t)((l2 * 3) / 5) + 67844;            /* 8e-3 x 256 = 2.048: the Q8 is in the constant */
    return tx_exp2(x);
}

void tx_pmode_strike(tx_pmode_t *m, int32_t a)
{
    uint32_t e;
    if (a < 0) a = -a;
    if (a > 32767) a = 32767;
    if (!m->g && !m->gl) { m->x = 16000; m->y = 0; }        /* from rest: the displacement starts at zero */
    e = (uint32_t)(m->g * m->g) + (uint32_t)(a * a);         /* energies add (the strike's phase is random) */
    e = tx_isqrt32(e);
    m->g = (int32_t)(e > 32767u ? 32767u : e);
}

TX_AUDIO_CODE void tx_pmode_add(tx_pmode_t *m, unsigned nm, int32_t level_q15, int32_t *out, unsigned n)
{
    tx_pmode_t *live[TX_PLATE_MODES + 3u];
    tx_pmode_t dummy;
    unsigned i, j, k = 0;
    int32_t dn = (int32_t)(n ? n : 1u);
    for (i = 0; i < nm; i++) {
        int32_t gt = (m[i].g * level_q15) >> 15;
        if (gt || m[i].gl) live[k++] = &m[i];
    }
    if (!k) return;
    dummy.x = dummy.y = dummy.e = dummy.g = dummy.gl = 0;
    while (k & 3u) live[k++] = &dummy;                       /* four modes side by side: the four recurrences */
    for (i = 0; i < k; i += 4) {                             /* overlap (one alone is latency-bound) */
        tx_pmode_t *p0 = live[i], *p1 = live[i + 1], *p2 = live[i + 2], *p3 = live[i + 3];
        int32_t x0 = p0->x, y0 = p0->y, e0 = p0->e, x1 = p1->x, y1 = p1->y, e1 = p1->e;
        int32_t x2 = p2->x, y2 = p2->y, e2 = p2->e, x3 = p3->x, y3 = p3->y, e3 = p3->e;
        int32_t t0 = (p0->g * level_q15) >> 15, t1 = (p1->g * level_q15) >> 15;
        int32_t t2 = (p2->g * level_q15) >> 15, t3 = (p3->g * level_q15) >> 15;
        int32_t a0 = p0->gl * 256, a1 = p1->gl * 256, a2 = p2->gl * 256, a3 = p3->gl * 256;
        int32_t s0 = (t0 - p0->gl) * 256 / dn, s1 = (t1 - p1->gl) * 256 / dn;
        int32_t s2 = (t2 - p2->gl) * 256 / dn, s3 = (t3 - p3->gl) * 256 / dn;
        for (j = 0; j < n; j++) {                            /* the magic circle: two shears, invertible */
            x0 += (e0 * y0) >> 16; y0 -= (e0 * x0) >> 16;
            x1 += (e1 * y1) >> 16; y1 -= (e1 * x1) >> 16;
            x2 += (e2 * y2) >> 16; y2 -= (e2 * x2) >> 16;
            x3 += (e3 * y3) >> 16; y3 -= (e3 * x3) >> 16;
            out[j] += ((y0 * (a0 >> 8)) >> 6) + ((y1 * (a1 >> 8)) >> 6) + ((y2 * (a2 >> 8)) >> 6) + ((y3 * (a3 >> 8)) >> 6);
            a0 += s0; a1 += s1; a2 += s2; a3 += s3;          /* Q14 x Q15 >> 6 -> Q23 */
        }
        p0->x = x0; p0->y = y0; p0->gl = t0; p1->x = x1; p1->y = y1; p1->gl = t1;
        p2->x = x2; p2->y = y2; p2->gl = t2; p3->x = x3; p3->y = y3; p3->gl = t3;
    }
}

void tx_plate_diffuse(uint32_t *W, unsigned nb, uint32_t c)
{
    unsigned b;
    if (!c) return;
    for (b = 0; b + 1u < nb; b++) {
        uint32_t w0 = W[b], w1 = W[b + 1], em, u;
        uint64_t r;
        int64_t t;
        if (!(w0 | w1)) continue;
        em = (uint32_t)(((uint64_t)w0 * TX_PLATE_RINV[b] + (uint64_t)w1 * TX_PLATE_RINV[b + 1]) >> 17);   /* mean density */
        u = em;
        r = ((uint64_t)u * u) >> 16;                         /* C E^2 (E Q30) in Q16: E^2 c / 2^30 */
        r = r >= (1ull << 32) ? 16384u : (r * c) >> 14;
        r = (r * TX_PLATE_RINV[b]) >> 16;                    /* x RHO^-b: the f in the flux */
        if (r > 16384u) r = 16384u;                          /* at most a quarter of the way to equal densities */
        t = ((int64_t)w0 * 37923 - (int64_t)w1 * 27613) >> 16;   /* (RHO W_b - W_(b+1)) / (1 + RHO) */
        t = (t * (int64_t)r) >> 16;
        W[b] = (uint32_t)((int64_t)w0 - t);
        W[b + 1] = (uint32_t)((int64_t)w1 + t);
    }
}
