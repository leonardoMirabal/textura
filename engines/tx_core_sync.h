/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura SYNC core: phase-locked EVENTS, a population of event emitters that a resonance pulls into step (the song of
 * dunes).
 *
 *   emitters  N phase oscillators (grains in the flowing layer), each colliding once per cycle at its own rate
 *             (Gamma (1 + spread)); a collision is an impulse into the resonance, at its exact sample.
 *   resonance a complex one-pole (z <- r e^(i w) z + impulse): the surface elastic wave at f; its phase is explicit.
 *   coupling  per block every emitter's phase is pulled toward the wave's: dphi = K A sin(theta - phi), computed as
 *             K (y cos phi - x sin phi) with z = x + i y (no division, no atan): the pull grows with the wave (the
 *             feedback G5 compares with a phase-locked loop), and fades as the wave nears A_sat (the grains take off:
 *             the song's amplitude saturates there, G5). Below the coupling threshold the collisions stay random: a
 *             rumble of band-limited noise around f; above it they lock and the tone grows.
 *   jitter    a random phase kick per block (an inhomogeneous avalanche: coherence 100 ms steady, 20 ms intermittent).
 * Integer: phases 32-bit, the resonance Q15 coefficients and states saturated at +-2^15 (products < 2^31). Two
 * resonances (a near-threshold doublet: two patches of slightly different f), each fed by its
 * half of the emitters. */
#ifndef TX_CORE_SYNC_H
#define TX_CORE_SYNC_H
#include <stdint.h>
#include "tx_core_math.h"

#define TX_SYNC_N 24u

typedef struct {
    uint32_t ph[TX_SYNC_N], inc[TX_SYNC_N];   /* emitter phases and increments per sample */
    int32_t x[2], y[2];            /* the two resonances (Q14 = 1.0, saturated at +-2^15) */
    int32_t rc[2], rs[2];          /* r cos w, r sin w (Q15) */
    int32_t k;                     /* coupling (0 = none, <= 4096): x 256 phase units per block per Q14 unit of y cos - x sin */
    int32_t amp;                   /* impulse height (Q14) */
    int32_t asat;                  /* the amplitude where the grains take off (|x| + |y|, Q14 units) */
    uint32_t jit;                  /* phase jitter per block (sd, phase units >> 16) */
    uint8_t n, pad[3];
} tx_sync_t;

void tx_sync_init(tx_sync_t *s);
/* resonance j at f (Hz) with quality q (x16) */
void tx_sync_res(tx_sync_t *s, unsigned j, uint32_t f_hz, uint32_t q_x16);
/* emitters: n (<= TX_SYNC_N) at f_hz with a spread (Q15 relative sd); phases random */
void tx_sync_emitters(tx_sync_t *s, tx_rng_t *r, unsigned n, uint32_t f_hz, int32_t spread_q15);
/* render: ADDS the resonances' x (Q14 -> Q23 x out_q8 / 256) into out; returns the collisions of the block */
TX_AUDIO_CODE unsigned tx_sync_add(tx_sync_t *s, tx_rng_t *r, int32_t out_q8, int32_t *out, unsigned n);
static inline int tx_sync_quiet(const tx_sync_t *s) { return !(s->x[0] | s->y[0] | s->x[1] | s->y[1]); }

#endif
