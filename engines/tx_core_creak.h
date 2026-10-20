/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura CREAK core: a stick-slip trainjittered harmonic source.
 *
 * W7 models a creak as a harmonic source f0(t) = (1 + e(t)) Phi(pressure, speed) driving resonators, e(t) a noise
 * low-passed below 20 Hz: a low f0 sounds noisy (single slips heard), a high f0 "sings". Here the source is the
 * slip train itself: one impulse per slip (an impulse train carries every harmonic of f0; the resonators that
 * follow, the creaking body, shape them), the slip rate f0 (1 + j e(t)) with e(t) a normal draw low-passed at
 * ~20 Hz (one-pole per block), each slip's size jittered +-30 % (ours). Per block: the slips of the block as
 * (offset, amplitude) pairs for an impulse resonator (tx_ireso_add): ~10 ops per slip, nothing per sample. */
#ifndef TX_CORE_CREAK_H
#define TX_CORE_CREAK_H
#include <stdint.h>
#include "tx_core_math.h"

typedef struct {
    uint32_t ph;                   /* slip phase (a slip at each wrap) */
    int32_t e;                     /* the low-passed jitter, Q12 (SD ~1 = 4096) */
} tx_creak_t;

/* slips of the next n samples: rate f_hz (x (1 + jit_q15 e)), size amp (Q15) -> off[] / imp[] (ascending), count */
unsigned tx_creak_block(tx_creak_t *c, tx_rng_t *r, uint32_t f_hz, int32_t jit_q15, int32_t amp, unsigned n,
                        uint16_t *off, int16_t *imp, unsigned cap);

#endif
