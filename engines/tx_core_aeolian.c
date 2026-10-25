/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura AEOLIAN core: Strouhal law and the narrowband tone voice (tx_core_aeolian.h). */
#include "tx_core_aeolian.h"

/* A2b Table 2: St = lambda + tau / sqrt(Re), by Re range (lambda Q15, tau Q12 signed) */
typedef struct { uint32_t re_hi; int32_t lam, tau; } st_row_t;
static const st_row_t ST[] = {
    {180, 8795, -4242},            /* 47-180: 0.2684, -1.0356 */
    {230, 7986, -3525},            /* 180-230: 0.2437, -0.8607 */
    {240, 14060, -15047},          /* 230-240: 0.4291, -3.6735 */
    {360, 8166, -3629},            /* 240-360: 0.2492, -0.8861 (interpolated in A2b) */
    {1300, 7396, -1803},           /* 360-1300: 0.2257, -0.4402 */
    {5000, 6685, 1378},            /* 1300-5000: 0.2040, +0.3364 */
    {200000, 5820, 9021},          /* 5000-2e5: 0.1776, +2.2023 */
};

static uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) { if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return r;
}

uint32_t tx_aeol_shed(uint32_t u_mms, uint32_t d_um, uint32_t *q_x16)
{
    uint32_t re = (uint32_t)(((uint64_t)u_mms * d_um * 4369u) >> 26);   /* u d / nu = (mm/s x um) / 15000 */
    int32_t st, i;
    uint32_t sq, f, pct;
    if (re < 47u) { *q_x16 = 16u; return 0; }
    for (i = 0; ST[i].re_hi < re && i < 6; i++) { }
    sq = isqrt32(re << 8);                                   /* sqrt(Re), Q4 */
    st = ST[i].lam + (int32_t)((ST[i].tau * 8 * 16) / (int32_t)(sq ? sq : 1u));   /* tau / sqrt(Re): Q12 x 8 / Q4 -> Q15 */
    if (st < 3277) st = 3277;
    /* f = St u / d: (Q15 St) x u_mm/s / d_um x 1000 */
    f = ((uint32_t)st * u_mms / d_um) * 1000u / 32768u;      /* (St Q15 x u / d) <= 1.5e6: x 1000 fits 32 bits */
    /* Q = 100 / (4.624e-5 Re + 0.9797), x 16: 1600 / (0.9797 + Re / 21626) */
    pct = 64205u + (uint32_t)(((uint64_t)re * 3030u) >> 10);   /* (0.9797 + 4.624e-5 Re) x 65536 */
    *q_x16 = 104857600u / pct;                               /* 1600 x 65536 / pct */
    return f;
}

void tx_aeol_width(tx_aeol_t *v, uint32_t f_hz, uint32_t q_x16)
{
    /* phase diffusion D = 2 pi B rad^2/s gives a Lorentzian of FWHM B: per sample sd = sqrt(2 pi B / fs) rad
     * = sqrt(B / (2 pi fs)) = sqrt(B / 277088) cycles; B = 16 f / q_x16. The kicks are uniform (sd 1/sqrt 3 of their
     * range): sig = sd (cycles, Q12) x sqrt(3) x 32 = phase units per unit of a Q15 uniform. */
    uint32_t b_q8 = (f_hz * 4096u) / (q_x16 ? q_x16 : 1u);  /* B, Hz Q8 */
    uint32_t c2 = (b_q8 * 242u) >> 10;                       /* B / 277088 in Q24 cycles^2 */
    v->sig = (int32_t)(isqrt32(c2) * 55u);
}

TX_AUDIO_CODE void tx_aeol_add(tx_aeol_t *v, int32_t a1, int32_t a2, int32_t a3, int32_t *out, unsigned n)
{
    uint32_t th = v->th, inc = v->inc, x32 = v->rng.s, psi = v->psi;
    int32_t sig = v->sig;
    int32_t c1 = v->a1, c2 = v->a2, c3 = v->a3;
    int32_t s1 = (a1 - c1) / (int32_t)(n ? n : 1u), s2 = (a2 - c2) / (int32_t)(n ? n : 1u), s3 = (a3 - c3) / (int32_t)(n ? n : 1u);
    unsigned j;
    if (!c1 && !c2 && !c3 && !a1 && !a2 && !a3) { v->th = th + inc * n; return; }
    if (!c2 && !c3 && !a2 && !a3) {                          /* the fundamental alone (a quiet tone: the caller drops its
                                                              * upper partials; 1 sine a sample instead of 3) */
        for (j = 0; j < n; j++) {
            x32 ^= x32 << 13; x32 ^= x32 >> 17; x32 ^= x32 << 5;
            psi += (uint32_t)(((int32_t)(x32 >> 16) - 32768) * sig);
            th += inc;
            out[j] += tx_sin_q15(th + psi) * (c1 >> 7);
            c1 += s1;
        }
        v->th = th; v->psi = psi; v->rng.s = x32 ? x32 : 1u;
        v->a1 = a1; v->a2 = a2; v->a3 = a3;
        return;
    }
    for (j = 0; j < n; j++) {
        uint32_t p;
        x32 ^= x32 << 13; x32 ^= x32 >> 17; x32 ^= x32 << 5;
        psi += (uint32_t)(((int32_t)(x32 >> 16) - 32768) * sig);   /* uniform kicks (< 2^31); a phase: wraps */
        th += inc;
        p = th + psi;
        out[j] += ((tx_sin_q15(p) * (c1 >> 7)) + (tx_sin_q15(p * 2u) * (c2 >> 7)) + (tx_sin_q15(p * 3u) * (c3 >> 7)));
        c1 += s1; c2 += s2; c3 += s3;
    }
    v->th = th; v->psi = psi; v->rng.s = x32 ? x32 : 1u;
    v->a1 = a1; v->a2 = a2; v->a3 = a3;
}
