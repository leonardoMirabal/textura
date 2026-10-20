/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura NOISE core: filtered noise band with flicker (tx_core_noise.h). */
#include "tx_core_noise.h"

void tx_noise_init(tx_noise_t *z, uint32_t seed)
{
    tx_rng_seed(&z->rng, seed);
    z->low = z->band = 0;
    z->gain = z->target = 0;
    z->glide = 2;                                            /* ~4 blocks (23 ms) to settle */
    z->flick = 32768; z->flick_depth = 0;
    z->lp = 0; z->tilt = 0;
    tx_noise_band(z, 1000, 16, TX_NOISE_BAND);
}

void tx_noise_band(tx_noise_t *z, uint32_t fc_hz, uint32_t q_x16, uint8_t mode)
{
    uint32_t ph;
    if (fc_hz < 20u) fc_hz = 20u;
    if (fc_hz > TX_FS / 6u) fc_hz = TX_FS / 6u;              /* Chamberlin stays stable and close below fs/6 */
    if (q_x16 < 8u) q_x16 = 8u;
    if (q_x16 > 128u) q_x16 = 128u;
    ph = fc_hz * 97391u >> 1;                                /* (fc / fs) 2^32 / 2 = pi fc / fs as a phase: */
                                                             /* 2^32 / 44100 = 97391.5 */
    z->f = tx_sin_q15(ph);                                   /* sin in Q15 = 2 sin in Q14 */
    z->q = (int32_t)((16u << 14) / q_x16);
    z->mode = mode;
}

TX_AUDIO_CODE void tx_noise_add(tx_noise_t *z, int32_t *out, unsigned n)
{
    int32_t low = z->low, band = z->band, f = z->f, q = z->q, g0, g1, gstep, g, x, y;
    unsigned j;
    if (!z->gain && !z->target) { z->low = z->band = 0; return; }
    if (z->flick_depth) {                                    /* flicker: a new random gain each block, smoothed */
        int32_t t = 32768 - (int32_t)(((tx_rand(&z->rng) >> 17) * (uint32_t)z->flick_depth) >> 15);
        z->flick += (t - z->flick) >> 1;
    } else
        z->flick = 32768;
    g0 = z->gain;
    g1 = z->gain + ((z->target - z->gain) >> z->glide);
    if (g1 == g0 && g1 != z->target) g1 = z->target;         /* the last step */
    if (z->target == 0 && g1 < 64) g1 = 0;                   /* end on exact zero */
    z->gain = g1;
    g0 = (g0 >> 1) * (z->flick >> 1) >> 13;                  /* Q15 x Q15 -> Q15 (32-bit) */
    g1 = (g1 >> 1) * (z->flick >> 1) >> 13;
    gstep = (int32_t)((g1 - g0) / (int32_t)(n ? n : 1u));    /* 32-bit division, once per block */
    g = g0;
    for (j = 0; j < n; j++) {
        x = ((int32_t)(tx_rand(&z->rng) >> 21) - 1024) * 2;   /* white, +-2^11 = +-1.0 in Q11 */
        low += (f * band) >> 14;
        y = x - low - ((q * band) >> 14);                     /* high */
        band += (f * y) >> 14;
        switch (z->mode) {
        case TX_NOISE_LOW: y = low; break;
        case TX_NOISE_HIGH: break;
        case TX_NOISE_WHITE: y = x; break;
        default: y = band; break;
        }
        out[j] += y * (g >> 3);                              /* Q11 x Q12 -> Q23 */
        g += gstep;
    }
    if (!z->gain) low = band = 0;
    z->low = low; z->band = band;
}

void tx_noise_tilt(tx_noise_t *z, uint32_t fc_hz)
{
    uint32_t e;
    if (!fc_hz) { z->tilt = 0; return; }
    if (fc_hz > 20000u) fc_hz = 20000u;
    e = fc_hz * 862u >> 6;                                   /* 2 pi fc / (fs ln 2), Q16 (13.47 fc) */
    z->tilt = 32768 - (int32_t)tx_exp2((15 << 16) - (int32_t)e);   /* 1 - exp(-2 pi fc / fs), Q15 */
    if (z->tilt < 1) z->tilt = 1;
}

TX_AUDIO_CODE void tx_noise_roar_add(tx_noise_t *z, int32_t g_q15, int32_t *out, unsigned n)
{
    int32_t low = z->low, band = z->band, lp = z->lp, f = z->f, q = z->q, a = z->tilt ? z->tilt : 32768, g, gstep, x, y;
    unsigned j;
    if (!g_q15 && !z->gain) { z->low = z->band = z->lp = 0; return; }
    if (g_q15 > 524288) g_q15 = 524288;
    if (g_q15 < 0) g_q15 = 0;
    g = z->gain;
    gstep = (g_q15 - g) / (int32_t)(n ? n : 1u);             /* 32-bit division, once per block */
    for (j = 0; j < n; j++) {
        x = (int32_t)(tx_rand(&z->rng) >> 18) - 8192;          /* white, +-2^13 */
        low += (f * band) >> 14;
        y = x - low - ((q * band) >> 14);
        band += (f * y) >> 14;
        lp += ((band - lp) * a) >> 15;
        out[j] += lp * (g >> 5);                             /* Q13 x Q10 -> Q23 */
        g += gstep;
    }
    z->gain = g_q15;
    if (!g_q15) low = band = lp = 0;
    z->low = low; z->band = band; z->lp = lp;
}
