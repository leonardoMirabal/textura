/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura CHORUS core: when each caller of an insect chorus calls.
 *
 * Callers reset their rhythm when they hear a neighbour (inhibitory resetting, N19 Eq. 1): a caller i calls at its
 * own period T_i (+ a noise eps, SD = 2 CV x T: tx_gauss_q12 is SD 2); when caller j's call reaches i (j's call
 * time + the propagation delay l / v between them) at d = arrival - i's last call, i's next call moves to
 *        next_i = last_i + s d + T_i + eps_i,
 * unless the stimulus is masked: it arrives while i is still calling (d < the call length x) or too late to change a
 * call already committed (within the effector delay t of i's next call). N19's defaults: T 500 ms, t 60 ms,
 * x 50 ms, CV 0.06; s ~ 1 gives synchrony, s <= 0.5 alternation. Positions on a plane (mm) give each pair's delay
 * (computed when a caller is placed: an isqrt per pair, never per sample). Per block: the due calls in time order,
 * each broadcast to the other callers (~32 x 32 checks in a block where all call: control rate, ~1-2 k ops). */
#ifndef TX_CORE_CHORUS_H
#define TX_CORE_CHORUS_H
#include <stdint.h>
#include "tx_core_math.h"

#define TX_CHORUS_MAX 32u

typedef struct {
    uint32_t last, next;           /* the last call and the next one, on the chorus clock (samples) */
    int32_t T, eps;                /* own period and the current period noise, samples */
    uint8_t alive, pad[3];
} tx_caller_t;

typedef struct {
    tx_caller_t c[TX_CHORUS_MAX];
    int32_t x[TX_CHORUS_MAX], y[TX_CHORUS_MAX];    /* positions, mm */
    uint16_t dly[TX_CHORUS_MAX][TX_CHORUS_MAX];    /* propagation delays, samples (symmetric) */
    int32_t s_q15, cv_q15, t_eff, x_len;           /* N19: s, CV of eps (its SD 2 x), effector delay t, call length x */
    uint32_t now;
} tx_chorus_t;

void tx_chorus_init(tx_chorus_t *ch);
void tx_chorus_set(tx_chorus_t *ch, int32_t s_q15, int32_t cv_q15, uint32_t t_eff, uint32_t x_len);
/* place caller i at (x, y) mm: its delays to every singing caller (a 2-D plane, sound at 343 m/s) */
void tx_chorus_place(tx_chorus_t *ch, unsigned i, int32_t x_mm, int32_t y_mm);
/* caller i sings: period T samples, the first call first samples from now */
void tx_chorus_start(tx_chorus_t *ch, unsigned i, int32_t T, uint32_t first);
static inline void tx_chorus_stop(tx_chorus_t *ch, unsigned i) { ch->c[i].alive = 0; }
/* the calls of the next n samples in time order: who[k], off[k] (k < cap); then the clock moves on by n */
unsigned tx_chorus_block(tx_chorus_t *ch, tx_rng_t *rng, unsigned n, uint8_t *who, uint16_t *off, unsigned cap);
/* a normal-ish draw (sum of 4 uniforms), Q12 with SD 2.0: 8192, not 4096 (+-28376 at most: 3.46 SD); a draw of SD 1.0 is
 * tx_gauss_q12(r) >> 1 */
int32_t tx_gauss_q12(tx_rng_t *r);

#endif
