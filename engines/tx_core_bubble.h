/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura BUBBLE core: the bubble hero, an air bubble ringing in water (the "plink" of a drip, a raindrop, a brook).
 *
 * Physics: a bubble of radius a rings at the Minnaert frequency f0 = 3.28 m/s / a and decays at
 *   d = 0.13 / a + 0.0072 a^-1.5   [1/s, a in m; valid for a > 0.15 mm]           (Q = pi f0 / d, 16 .. 45)
 * so its sound is a sin(2 pi f t) e^(-d t), starting at zero phase (no click). Near the surface it rises:
 *   f(t) = f0 (1 + xi d t), xi ~ 0.1 for drops, capped at min(2 f0, 0.45 fs) ("an octave or less" observed).
 * The amplitude of a bubble grows as a^1.5 (energy 2 rho a^3 u^2 for a size-independent inward speed u).
 *
 * Integer voice: 32-bit phase and increment (the frequency is exact and linear in the increment, so the rise is a
 * constant increment step per sample and the cap is a frequency, never unstable), the quarter-sine table, a Q30
 * envelope stepped linearly over 32-sample sub-blocks by a Q16 factor (32-bit products). ~12 ops per sample.
 * Radii are given as log2(a / 1 mm) in Q16: every law above is a power of a, so each is one tx_exp2(). 32 B. */
#ifndef TX_CORE_BUBBLE_H
#define TX_CORE_BUBBLE_H
#include <stdint.h>
#include "tx_core_cloud.h"

#define TX_BUBBLE_FLOOR (1u << 16)   /* Q30 envelope under which a bubble is over (under 2 LSB of Q23) */
#define TX_BUBBLE_A_MIN_L2 (-180427) /* log2(0.149 mm): the damping law holds above 0.15 mm */
#define TX_BUBBLE_A_MAX_L2 (262144)  /* log2(16 mm): 205 Hz */

typedef struct {
    uint32_t ph, inc;              /* phase and increment (2^32 = one cycle) */
    uint32_t dinc, cap;            /* rise: increment step per sample, and the increment's cap */
    uint32_t env;                  /* amplitude, Q30 of full scale */
    uint32_t d32;                  /* envelope factor per 32 samples, Q16 */
    int32_t last;                  /* last output sample (for the declick if the bubble is cut) */
    uint32_t wait;                 /* samples before it starts (may span several blocks) */
} tx_bubble_t;

/* the physics of a radius (log2(a / 1 mm), Q16), at control or event rate */
uint32_t tx_bubble_inc(int32_t a_l2);            /* Minnaert f0 as a phase increment (capped at 0.45 fs) */
uint32_t tx_bubble_damp_q8(int32_t a_l2);        /* d in 1/s, Q8 */

/* start a bubble: radius, rise xi (Q16; 0 = none), start amplitude (Q30 of full scale), start delay in samples */
void tx_bubble_start(tx_bubble_t *b, int32_t a_l2, uint32_t xi_q16, uint32_t env_q30, uint32_t wait);
TX_AUDIO_CODE void tx_bubble_render(tx_bubble_t *b, int32_t *out, unsigned n);

/* every live bubble of a pool: render, free the finished ones (their last sample to the declick), keep the pool's
 * levels current (stealing is quietest first) */
TX_AUDIO_CODE void tx_bubbles_render(tx_bubble_t *b, tx_pool_t *p, tx_declick_t *dc, int32_t *out, unsigned n);

/* LEVEL changed: rescale every live bubble's amplitude by new / old (Q15 levels) */
void tx_bubbles_rescale(tx_bubble_t *b, tx_pool_t *p, int32_t old_q15, int32_t new_q15);

#endif
