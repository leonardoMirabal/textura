/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura SHAKER core: PhISEM (Cook), an integer port of STK's Shakers tick (tx_core_shaker.c keeps STK's header
 * and licence). Many grains in a container: a decaying shake energy, a collision probability per sample, a sound
 * level bumped by the energy at each collision and decaying fast, and 1-3 two-pole resonators excited by an impulse
 * of that level at every collision, then STK's 3-tap equaliser (taps -1 / 0 / +1).
 *
 * Integer layout (32-bit products in the sample loop):
 *   energy, level  Q24 (1.0 = 2^24); the level is capped at 16.0. The energy decays by a Q16 factor per 32 samples,
 *                  stepped linearly; the level by a Q15 factor per sample (two 32-bit products).
 *   collisions     probability per sample Q16 (STK: N / 1024; N >= 1024 = every sample).
 *   resonators     y = b0 x - a1 y1 - a2 y2, Q14 coefficients and signal (1.0 = 2^14, saturated at +-4.0); b0 is the
 *                  unit-peak input gain (1 - r) 2 sin(theta), so every resonance has the same peak.
 *   vary           STK re-tunes the resonators at each collision (+-vary x f): here at most once per 8 samples.
 * ~20 ops per sample with one resonator, +6 per extra resonator. Silent when the energy is spent and the
 * resonators have rung out (exact zero). */
#ifndef TX_CORE_SHAKER_H
#define TX_CORE_SHAKER_H
#include <stdint.h>
#include "tx_core_math.h"

#define TX_SHAKER_RES 3u
#define TX_SHAKER_ONE (1 << 24)    /* energy 1.0 */

typedef struct {
    uint32_t inc;                  /* base frequency as a phase increment */
    int32_t r;                     /* pole radius, Q14 */
    int32_t a1, a2, b0;            /* Q14 */
    int32_t y1, y2;                /* Q14 signal */
} tx_shaker_res_t;

typedef struct {
    tx_rng_t rng;
    int32_t energy, snd;           /* Q24 */
    uint32_t edec;                 /* energy factor per 32 samples, Q16 */
    int32_t sdec;                  /* level factor per sample, Q15 */
    uint32_t p16;                  /* collision probability per sample, Q16 (65536 = every sample) */
    int32_t gain;                  /* impulse gain, Q11 (STK currentGain), <= 8.0 */
    int32_t vary;                  /* re-tuning depth, Q15 (STK varyFactor) */
    int32_t eq1, eq2;              /* equaliser taps 1 and 2 (-1, 0 or +1; tap 0 is +1) */
    int32_t h1, h2;                /* equaliser history */
    int32_t last;                  /* last output (Q14; for a declick when the voice is cut) */
    uint8_t nres, vcnt, pad[2];
    tx_shaker_res_t res[TX_SHAKER_RES];
} tx_shaker_t;

void tx_shaker_init(tx_shaker_t *s, uint32_t seed);
/* resonance i: frequency (Hz, <= 0.45 fs) and pole radius (Q14, < 1) */
void tx_shaker_res(tx_shaker_t *s, unsigned i, uint32_t f_hz, int32_t r_q14);
/* objects N (Q4, 1.1 .. 4096): collision probability N / 1024, gain = ln(N) x base / N (STK currentGain; base Q11 <= 16.0) */
void tx_shaker_objects(tx_shaker_t *s, uint32_t n_q4, int32_t base_q11);
/* system decay: the shake energy's T60 in ms; sound decay per sample (Q15, STK soundDecay) */
void tx_shaker_decay(tx_shaker_t *s, uint32_t t60_ms, int32_t sound_q15);
static inline void tx_shaker_eq(tx_shaker_t *s, int32_t t1, int32_t t2) { s->eq1 = t1; s->eq2 = t2; }
static inline void tx_shaker_vary(tx_shaker_t *s, int32_t vary_q15) { s->vary = vary_q15; }
/* add shake energy (Q24), capped at 1.0 (STK noteOn / energy controller) */
void tx_shaker_shake(tx_shaker_t *s, int32_t e_q24);
TX_AUDIO_CODE void tx_shaker_add(tx_shaker_t *s, int32_t *out, unsigned n);   /* ADDS Q23 */
int tx_shaker_quiet(const tx_shaker_t *s);

#endif
