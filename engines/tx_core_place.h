/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura PLACE: where a sound is (the SOURCE engine's stage 4). Everything here runs per event or per block, never per
 * sample: a particle carries its place in its own amplitude, so distance costs nothing while it sounds.
 *
 *   level    1 / r re 1 m (spherical spreading), closer than 0.25 m no louder.
 *   air      absorption grows with frequency and distance: alpha(f) ~ 0.005 (f / 1 kHz)^1.5 dB per m (ours: a fit
 *            to still air at 20 C, 50 % RH: ~0.01 dB/m at 2 kHz, 0.04 at 4 kHz, 0.11 at 8 kHz, 0.32 at 16 kHz). A mode,
 *            a bubble or a carrier is attenuated at its own frequency; a noise bed through a one-pole whose corner is
 *            where the loss reaches 3 dB: f3 = 1 kHz x (600 / r)^(2/3) (~15 kHz at 10 m, 3.3 kHz at 100 m, 700 Hz at 1 km).
 *   depth    a population spread around the distance: r = DIST x 2^(depth x (2 max(u1, u2) - 1)), biased to the far
 *            side (there is more ground farther out).
 * Distances are log2(r / 1 m) in Q16. */
#ifndef TX_CORE_PLACE_H
#define TX_CORE_PLACE_H
#include <stdint.h>
#include "tx_core_math.h"

#define TX_PLACE_R_MIN (-131072)   /* log2(0.25 m) */

/* log2(x) in Q16 for x >= 1 (Q12 precision) */
static inline int32_t tx_place_log2(uint32_t x) { return (32 << 16) - (int32_t)(tx_neglog2_q12(x ? x : 1u) << 4); }

/* the 1 / r gain re 1 m, Q12 (4096 at 1 m, 16384 at 0.25 m and closer) */
static inline int32_t tx_place_gain_q12(int32_t r_l2)
{
    if (r_l2 < TX_PLACE_R_MIN) r_l2 = TX_PLACE_R_MIN;
    if (r_l2 > (14 << 16)) return 0;
    return (int32_t)tx_exp2((12 << 16) - r_l2);
}

/* the air's loss at f_hz over r, as a Q15 gain */
static inline int32_t tx_place_air_q15(uint32_t f_hz, int32_t r_l2)
{
    /* loss in log2 units = 0.005 x 0.1661 x (f / 1000)^1.5 x r: log2 of it = -10.234 + 1.5 log2(f / 1000) + log2 r */
    int32_t lf = tx_place_log2(f_hz < 20u ? 20u : f_hz) - 653118;   /* log2(f / 1000 Hz) */
    int32_t l = -670680 + lf + lf / 2 + r_l2;
    uint32_t att;
    if (l < -(14 << 16)) return 32767;
    if (l > (4 << 16)) return 0;
    att = tx_exp2(l + (16 << 16));                         /* Q16 log2 units */
    if (att >= (15u << 16)) return 0;
    return (int32_t)tx_exp2((15 << 16) - (int32_t)att);
}

/* the one-pole coefficient (Q15) of the air at r for a noise bed: corner f3 = 1000 (600 / r)^(2/3), at most 20 kHz */
static inline int32_t tx_place_lp_q15(int32_t r_l2)
{
    int32_t l = 653118 + ((604820 - r_l2) * 2) / 3;        /* log2 1000 + 2/3 (log2 600 - log2 r) */
    uint32_t fc, x;
    if (l > 936360) l = 936360;                             /* log2 20000 */
    fc = tx_exp2(l);
    x = (fc * 862u) >> 6;                                   /* 2 pi fc / fs / ln 2, Q16 */
    return 32768 - (int32_t)tx_exp2((15 << 16) - (int32_t)x);
}

/* a member's distance: around dist_l2, spread +-depth_l2, biased to the far side */
static inline int32_t tx_place_draw(tx_rng_t *r, int32_t dist_l2, int32_t depth_l2)
{
    uint32_t u1 = tx_rand(r) >> 17, u2 = tx_rand(r) >> 17, u = u1 > u2 ? u1 : u2;   /* Q15 */
    return dist_l2 + (int32_t)(((int64_t)depth_l2 * ((int32_t)u * 2 - 32768)) >> 15);
}

#endif
