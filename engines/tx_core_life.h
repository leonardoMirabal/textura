/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura LIFE: organic modulation for any parameter (the SOURCE engine; later the lanes, the cross-lane coupling and
 * the auto-generative sequencer use the same modulator). Never a sine LFO: nature wanders and comes back.
 *
 *   walk     three Ornstein-Uhlenbeck walks with time constants T, T / 4 and T / 16 (T = TIME), summed with weights
 *            1/2, 1/4, 1/8: a random wander that always returns to the knob's value, with more slow motion than fast
 *            (close to 1/f over two decades). Each walk: x += -x / tau + sqrt(2 / tau) N(0, 1) per block, so its
 *            spread stays 1 (Q15) whatever tau is.
 *   swells   now and then (Poisson, one per ~3 T on average) a swell or a lull: the target moves +-0.4 .. 1 away for
 *            T / 2 .. 2 T and comes back, reached through a one-pole of T / 4 (a gust, a weather front, a chorus dying
 *            down and coming back).
 * Output: Q15, about +-1 (clamped). Per block: ~12 multiplies; 40 bytes. */
#ifndef TX_CORE_LIFE_H
#define TX_CORE_LIFE_H
#include <stdint.h>
#include "tx_core_math.h"

typedef struct {
    tx_rng_t rng;
    int32_t w[3];                  /* the walks, Q15 */
    int32_t sw, sw_t;              /* the swell now and its target, Q15 */
    uint32_t sw_wait;              /* blocks to the next swell change */
    int32_t out;                   /* the last output, Q15 */
} tx_life_t;

static inline void tx_life_init(tx_life_t *l, uint32_t seed)
{
    uint8_t *b = (uint8_t *)l;
    unsigned i;
    for (i = 0; i < sizeof *l; i++) b[i] = 0;
    tx_rng_seed(&l->rng, seed ^ 0x11FE5EEDu);
    for (i = 0; i < 3; i++) l->w[i] = (int32_t)(tx_rand(&l->rng) >> 18) - 8192;   /* start somewhere, not at 0 */
    l->sw_wait = 64u + (tx_rand(&l->rng) >> 22);
}

/* N(0, 1) in Q12: the sum of four uniforms (sd 1.155 x 2^12 / 2, scaled) */
static inline int32_t tx_life_gauss(tx_rng_t *r)
{
    uint32_t a = tx_rand(r), b = tx_rand(r);
    int32_t s = (int32_t)(a >> 16) + (int32_t)(a & 0xFFFFu) + (int32_t)(b >> 16) + (int32_t)(b & 0xFFFFu) - 131072;
    return (s * 3547) >> 15;                                 /* sd of s = 37837 -> 4096 */
}

/* one block; tau_l2 = log2(T in blocks), Q16 (>= 4 << 16). Returns the modulation, Q15. */
static inline int32_t tx_life_step(tx_life_t *l, int32_t tau_l2)
{
    unsigned i;
    int32_t s = 0;
    if (tau_l2 < (4 << 16)) tau_l2 = 4 << 16;
    for (i = 0; i < 3; i++) {
        int32_t t = tau_l2 - (int32_t)i * (2 << 16);        /* T, T / 4, T / 16 */
        int32_t k, g;
        if (t < (2 << 16)) t = 2 << 16;
        k = (int32_t)tx_exp2((31 << 16) - t) >> 15;        /* 1 / tau, Q16 */
        g = (int32_t)tx_exp2((15 << 16) + 32768 - t / 2);   /* sqrt(2 / tau), Q15 */
        l->w[i] += -(int32_t)(((int64_t)l->w[i] * k) >> 16) + ((tx_life_gauss(&l->rng) * g) >> 12);
        if (l->w[i] > 98304) l->w[i] = 98304;
        if (l->w[i] < -98304) l->w[i] = -98304;
        s += l->w[i] >> (i + 1);
    }
    if (!l->sw_wait--) {                                     /* a swell, a lull, or back to the knob */
        uint32_t tb = tx_exp2(tau_l2);                       /* T in blocks */
        if (l->sw_t) { l->sw_t = 0; l->sw_wait = tb + (uint32_t)(((uint64_t)tb * (tx_rand(&l->rng) >> 16)) >> 14); }
        else {
            int32_t a = 13107 + (int32_t)tx_rand_below(&l->rng, 19660u);   /* 0.4 .. 1 */
            l->sw_t = (tx_rand(&l->rng) & 1u) ? a : -a;
            l->sw_wait = tb / 2u + (uint32_t)(((uint64_t)tb * (tx_rand(&l->rng) >> 16)) * 3u >> 17);   /* T/2 .. 2T */
        }
    }
    {
        int32_t k = (int32_t)tx_exp2((33 << 16) - tau_l2) >> 15;   /* 4 / T, Q16 */
        if (k > 65536) k = 65536;
        l->sw += (int32_t)(((int64_t)(l->sw_t - l->sw) * k) >> 16);
    }
    s += l->sw;
    l->out = s > 32767 ? 32767 : s < -32767 ? -32767 : s;
    return l->out;
}

#endif
