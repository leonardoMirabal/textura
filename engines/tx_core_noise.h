/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura NOISE core: a filtered noise band (roar, flow, hiss, bed) with flicker and a gate to exact zero.
 *
 *   white (xorshift32) -> state-variable filter (Chamberlin: low / band / high outputs) -> x gain (smoothed per
 *   block, linear ramp across the block) x flicker (a slow random gain, per block) -> added to the mix, Q23.
 *
 * Integer layout (32-bit products only): the white input is +-2^11, the filter coefficients Q14 (f <= 1, q <= 2),
 * so a state stays near 2^15 even at Q = 8 and every product is < 2^30. The output gain is Q15 and lifts the
 * Q11 noise to Q23 (>> 3). When the gain and its target are 0 the band stops and its state is cleared (idle = 0). */
#ifndef TX_CORE_NOISE_H
#define TX_CORE_NOISE_H
#include <stdint.h>
#include "tx_core_math.h"

enum { TX_NOISE_BAND = 0, TX_NOISE_LOW = 1, TX_NOISE_HIGH = 2, TX_NOISE_WHITE = 3 };

typedef struct {
    tx_rng_t rng;
    int32_t low, band;             /* filter state (~Q11 scale) */
    int32_t f, q;                  /* Q14: 2 sin(pi fc / fs) and 1 / Q */
    int32_t gain, target;          /* Q15 output gain now, and where it goes */
    int32_t glide;                 /* gain smoothing per block: the step toward target is (target - gain) >> glide */
    int32_t flick, flick_depth;    /* Q15 flicker gain (1 = 32768) and its depth */
    int32_t lp, tilt;              /* roar path: one-pole low-pass state and coefficient (Q15; 0 = off) */
    uint8_t mode;
} tx_noise_t;

void tx_noise_init(tx_noise_t *z, uint32_t seed);
void tx_noise_band(tx_noise_t *z, uint32_t fc_hz, uint32_t q_x16, uint8_t mode);   /* q_x16: Q * 16 (8 .. 128) */
static inline void tx_noise_level(tx_noise_t *z, int32_t target_q15) { z->target = target_q15; }
static inline void tx_noise_flicker(tx_noise_t *z, int32_t depth_q15) { z->flick_depth = depth_q15; }
TX_AUDIO_CODE void tx_noise_add(tx_noise_t *z, int32_t *out, unsigned n);
/* ROAR path (fire, flames, wind beds): the band-pass output (white +-2^13, so keep Q <= 2) through an optional
 * one-pole low-pass (tilt: the band's -6 dB/oct above its centre becomes -12 above fc), times a gain the caller sets
 * every block (Q15, up to 16.0 = 2^19), ramped linearly across the block. No smoothing, no flicker: the caller
 * modulates. Gain 0 now and before: the state is cleared and nothing is added. ~14 ops per sample. */
void tx_noise_tilt(tx_noise_t *z, uint32_t fc_hz);       /* 0 = off */
TX_AUDIO_CODE void tx_noise_roar_add(tx_noise_t *z, int32_t g_q15, int32_t *out, unsigned n);
static inline int tx_noise_quiet(const tx_noise_t *z) { return !z->gain && !z->target; }

#endif
