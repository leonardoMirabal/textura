/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura POP: a population of calling animals, each an individual that sings in bouts (insect choruses).
 *
 * A caller does not call on a loop: it sings a bout, falls silent, sings again. Each bout's length and each pause are
 * drawn afresh (a bout: a gamma of shape 2 around its mean, so neither a single chirp nor a run of equal ones; a
 * pause: exponential, the irregular waits of an animal that calls when it is ready). ACTIVITY moves both means on a
 * log scale (rare: short bouts, long pauses; busy: long bouts, short pauses); it never moves the call rate: the
 * rhythm is the animal's own (its temperature, its species). How many there are is the engine's COUNT: the
 * individuals each drawn with their own distance, carrier and period, placed on an area law around the nearest
 * one (more far ones than near ones: a field), entering after a pause of their own, leaving now and then after a
 * bout (a new one comes with fresh draws). Control rate only (per block). */
#ifndef TX_CORE_POP_H
#define TX_CORE_POP_H
#include <stdint.h>
#include "tx_core_math.h"

#define TX_POP_BPS 172                 /* blocks per second (44100 / 256) */

typedef struct {
    int32_t b0_l2, b1_l2;              /* log2 of the mean bout (blocks), Q16, at ACTIVITY 0 and 127 */
    int32_t p0_l2, p1_l2;              /* the same for the mean pause */
} tx_pop_law_t;

/* an exponential draw, Q12 (mean 4096), capped at 8 x the mean */
static inline uint32_t tx_pop_exp_q12(tx_rng_t *r)
{
    uint32_t e = (tx_neglog2_q12(tx_rand(r)) * 2839u) >> 12;      /* -ln u = -log2 u x ln 2 */
    return e > 32768u ? 32768u : e;
}

/* the mean in blocks at activity a (0 .. 127) between two log2 ends */
static inline uint32_t tx_pop_mean(int32_t l0, int32_t l1, unsigned a)
{
    return tx_exp2(l0 + ((l1 - l0) * (int32_t)a) / 127);   /* (|l1 - l0| < 2^23: 32 bits) */
}

/* a bout's length, blocks (at least lo) */
static inline uint32_t tx_pop_bout(const tx_pop_law_t *w, tx_rng_t *r, unsigned a, uint32_t lo)
{
    uint32_t m = tx_pop_mean(w->b0_l2, w->b1_l2, a), e = (tx_pop_exp_q12(r) + tx_pop_exp_q12(r)) >> 1;
    uint32_t b = (m * e) >> 12;                                  /* (m < 2^16, e <= 2^15) */
    return b < lo ? lo : b;
}

/* a pause, blocks (at least ~0.25 s, at most 2.5 x the mean) */
static inline uint32_t tx_pop_pause(const tx_pop_law_t *w, tx_rng_t *r, unsigned a)
{
    uint32_t m = tx_pop_mean(w->p0_l2, w->p1_l2, a);
    uint32_t e = tx_pop_exp_q12(r), b = (m * (e > 10240u ? 10240u : e)) >> 12;   /* (at most 2.5 x the mean: a */
    return b < 43u ? 43u : b;                                                       /* rare one is not gone) */
}

/* how much farther than the nearest one an individual sits, log2 Q16: uniform on the area of a ring from 1 to 2^k_l2
 * times its radius (k_l2 Q16, at most 4 octaves): sqrt(1 + u (K^2 - 1)) */
static inline int32_t tx_pop_far_l2(tx_rng_t *r, int32_t k_l2)
{
    uint32_t K2 = tx_exp2(2 * k_l2 + (8 << 16));                  /* K^2, Q8 (K <= 16: K^2 <= 256) */
    uint32_t x = 256u + (((K2 - 256u) * (tx_rand(r) >> 16)) >> 16);   /* 1 + u (K^2 - 1), Q8 */
    if (x >= K2) return k_l2;
    uint32_t y = ((x << 15) / K2) << 17;                         /* x / K^2 in 0 .. 1, Q32 */
    int32_t lx = 2 * k_l2 - (int32_t)(tx_neglog2_q12(y) << 4);    /* log2 x = log2 K^2 + log2 (x / K^2), Q16 */
    return lx < 0 ? 0 : lx >> 1;
}

#endif
