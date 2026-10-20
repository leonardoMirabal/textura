/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura DISPERSE core: a dispersive waveguide loop, the spring ( Parker,
 *).
 *
 * A helical spring returns an impulse as a train of chirped echoes every T_D (30-60 ms), the low frequencies (below the
 * transition F_C, 2-5 kHz) dominating (D6). The efficient model (D2b): only the low chirp chain, at a decimated rate
 * (fs / 4): M stretched first-order allpasses A(z) = (a + z^-k) / (1 + a z^-k) in a feedback loop with a delay that
 * makes the loop T_D long, a loop low-pass at F_C and a loop gain (decay). Group delay of one section:
 * k (1 - a^2) / (1 + 2 a cos(k w) + a^2): with a = -0.6 the lowest frequencies take 4 k samples, those near F_C 0.25 k,
 * so each echo is a descending chirp spread over ~M (4 - 0.25) k samples. k = fs/4 / (2 F_C), at least 1. D2b's high
 * chain is >= 30 dB down: dropped (D2b).
 * Integer: Q14 coefficients, the signal Q15 in int32 (allpass states) and int16 (the loop delay); one multiply per
 * section and decimated sample: ~M x 5 ops per decimated sample (M 40: ~50 ops per output sample). Output: linear
 * interpolation x 4 and a one-pole image low-pass. */
#ifndef TX_CORE_DISPERSE_H
#define TX_CORE_DISPERSE_H
#include <stdint.h>
#include "tx_core_math.h"

#define TX_DSP_M 64u               /* sections, at most */
#define TX_DSP_K 4u                /* stretch, at most */
#define TX_DSP_D 1024u             /* loop delay (decimated samples: 93 ms) */

typedef struct {
    int32_t xs[TX_DSP_M][TX_DSP_K], ys[TX_DSP_M][TX_DSP_K];   /* section histories (Q15) */
    int16_t dl[TX_DSP_D];
    uint16_t m, k, kp, dlen, dpos, quiet_n;
    int32_t a;                     /* allpass coefficient, Q14 */
    int32_t g;                     /* loop gain, Q15 */
    int32_t lpa, lp;               /* loop low-pass (Q15 coefficient) */
    int32_t kick;                  /* pending excitation for the next decimated samples (Q15) */
    int32_t y0, y1;                /* the last two decimated outputs (for the interpolation) */
    int32_t olp, ola;              /* output image low-pass */
    int32_t peak;                  /* largest |value| written into the loop over the current pass */
    uint8_t phase, active, pad[2];
} tx_spring_t;

void tx_spring_init(tx_spring_t *s);
/* geometry: echo period T_D (us), transition F_C (Hz), sections M, loop gain (Q15) */
void tx_spring_set(tx_spring_t *s, uint32_t td_us, uint32_t fc_hz, unsigned m, int32_t g_q15);
static inline void tx_spring_kick(tx_spring_t *s, int32_t amp_q15) { s->kick += amp_q15; s->active = 1; }
TX_AUDIO_CODE void tx_spring_add(tx_spring_t *s, int32_t *out, unsigned n);
static inline int tx_spring_quiet(const tx_spring_t *s) { return !s->active; }

#endif
