/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura COMP core: the master bus compressor (after the bus effects, before the output limiter: tx_core_mix.c).
 *
 * Stereo-linked: one gain for both sides, from the louder side's power (max |L| |R|, squared), averaged over each
 * control block of 32 frames. Two power envelopes: a fast one (attack 20 ms, release 150 ms) and a slow one (attack
 * 400 ms, release 1.5 s); the detector is the larger, so a lone crack recovers in ~150 ms while a dense, steady sound
 * holds its gain (the release follows the programme, no pumping). A soft knee (12 dB wide); the gain glides to its
 * target over ~6 ms and ramps across each control block (no zipper). AMOUNT 1 .. 127: the ratio 1.03 .. 3 : 1, the
 * threshold -14 .. -30 dBFS, the makeup the gain lost at -12 dBFS, at most +6 dB (a quiet scene is lifted no more than
 * that: no noise brought up). AMOUNT 0: bypassed, bit-exact (once the gain is back at 1).
 *
 * Integers only, 32-bit products; the levels in "power log2" units (log2 of the power, Q12: 1 dB = 1360.6). */
#ifndef TX_CORE_COMP_H
#define TX_CORE_COMP_H
#include <stdint.h>
#include "tx_engine.h"

#define TX_CP_AMOUNT 0u
#define TX_CP_NP 1u
extern const tx_param_t tx_comp_params[TX_CP_NP];       /* COMP: the AMOUNT (the app's knob, its default) */

typedef struct {
    int32_t ef, es;                /* the fast and slow power envelopes (Q26: full scale = 2^26) */
    int32_t g;                     /* the gain now, Q14 (16384 = 1) */
    int32_t s, t, mk;              /* the slope 1 - 1/ratio (Q12), the threshold and the makeup (power log2 Q12) */
    int32_t gr;                    /* the gain reduction at the last control block (power log2 Q12, >= 0) */
    uint8_t amt, on;               /* AMOUNT; the detector running (0: its envelopes restart from the next block) */
} tx_comp_t;

void tx_comp_init(tx_comp_t *c);
void tx_comp_amount(tx_comp_t *c, unsigned amount);      /* 0 .. 127 (0: off) */
/* in place over n frames of the Q23 pair (|x| above 32 full scale is clamped) */
TX_AUDIO_CODE void tx_comp_process(tx_comp_t *c, int32_t *L, int32_t *R, unsigned n);
/* the gain reduction now, dB x 10 (the makeup not counted) */
static inline int32_t tx_comp_gr_db10(const tx_comp_t *c) { return (c->gr * 10 + 680) / 1361; }

#endif
