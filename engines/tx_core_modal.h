/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura MODAL core: struck bodies as sums of exponentially decaying sinusoids.
 *
 *   y(t) = sum_n a_n e^(-d_n t) sin(2 pi f_n t),  d_n = pi f_n / Q + sigma0   (constant-Q damping, I6 / I7)
 *
 * Each mode is an oscillator, not a recursive filter: a 32-bit phase (any frequency up to fs / 2, exact) and an
 * amplitude (Q30 of full scale) that decays by a factor m16 every 16 samples (computed at control rate, applied with
 * one 64-bit product per 16 samples, linear in between: within 0.6 % of the exponential even for a 1.6 ms decay).
 * So any Q works (a granite boulder at Q 600 or quartz at 2000 at 100 Hz needs 1 - r ~ 1e-6: no two-pole in 32 bits
 * holds that), there is no limit cycle, and a mode is quiet when its amplitude falls under -108 dBFS.
 * A strike adds an impulse response a sin(phi - phi_now) to the sounding mode: the phasor sum A + a e^(-i phi_now)
 * gives the new amplitude and a phase step (CORDIC, control rate), so restriking a ringing stone is phase-correct
 * (no click, partial cancellation as in a real restrike). Strikes inside a block: the caller renders up to the
 * strike's offset, strikes, then renders on (tx_modal_run takes a sample range).
 * ~10 ops per sample per sounding mode (phase, a 512-point sine table interpolated (1 KB, -90 dB), amplitude
 * ramp, multiply-add). */
#ifndef TX_CORE_MODAL_H
#define TX_CORE_MODAL_H
#include <stdint.h>
#include "tx_core_math.h"

#define TX_MODAL_QUIET (1 << 12)   /* amplitude floor (Q30): -108 dBFS */

typedef struct {
    uint32_t ph, inc;              /* phase, increment (the caller may move inc per block: pitch glides) */
    int32_t amp;                   /* Q30 of full scale, >= 0 */
    uint32_t m16;                  /* decay over 16 samples, Q30 (<= 1) */
} tx_mode_t;

/* m16 for a mode at f_hz with quality q_x16 (Q x 16) and extra damping sigma0 (1/s, Q8) */
uint32_t tx_mode_decay(uint32_t f_hz, uint32_t q_x16, uint32_t sigma0_q8);
/* strike: add an impulse response of amplitude a (Q30, signed: polarity) at the current phase */
void tx_mode_strike(tx_mode_t *m, int32_t a_q30);
/* render modes m[0..nm) over samples j0..j1 of out (Q23, added) */
TX_AUDIO_CODE void tx_modal_run(tx_mode_t *m, unsigned nm, int32_t *out, unsigned j0, unsigned j1);
/* a strike at sample o of the block on modes m[0..nm) rendered so far up to *done (0 at the block's start): render
 * them on to o first, so the strike lands at its own sample, never on the block grid; at the block's end the caller
 * renders from *done to n and sets *done = 0 */
TX_INLINE void tx_modal_upto(tx_mode_t *m, unsigned nm, int32_t *out, uint8_t *done, unsigned o)
{
    if (o > *done) { tx_modal_run(m, nm, out, *done, o); *done = (uint8_t)(o < 255u ? o : 255u); }
}
/* log2(x) in Q16 for x >= 1 (control rate) */
int32_t tx_log2_q16(uint32_t x);

#endif
