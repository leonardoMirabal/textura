/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura INSECT core: the periodic sound sources of singing insects.
 *
 *   TONE   a cricket pulse (one wing closing). The clockwork escapement makes one tooth = one cycle of the harp
 *          resonance, so a steady pulse is a near-pure tone at the carrier: a 32-bit phase, the pitch falling
 *          linearly by a glide over the pulse (5-15 %), an envelope table (65 points, Q15) read across the pulse,
 *          and phase-locked H2 / H3 (sin 2ph, sin 3ph) at their own levels. ~14 ops per sample while it sounds.
 *          tx_itone_env builds the envelope: sin^2 rise to the peak position, sin^2 fall to zero.
 *   RESO   a two-pole resonator (y = a1 y1 - a2 y2 + x; a1 = 2 r cos w, a2 = r^2, Q14) excited by impulses at
 *          given sample offsets: a tooth strike, a tymbal rib, a peg impact. Any frequency up to fs / 2 (the
 *          katydid's 14.7 kHz band); state Q15 (|y| clamped below 2^16), so every product is < 2^31. Silent input and a
 *          small or frozen state: exact zero (as the SVF bank: rounding cannot hold a limit cycle).
 *          ~5 ops per sample per resonator. */
#ifndef TX_CORE_INSECT_H
#define TX_CORE_INSECT_H
#include <stdint.h>
#include "tx_core_math.h"

#define TX_ITONE_ENV 64u           /* envelope points (+1 guard) */

typedef struct {
    uint32_t ph, inc;              /* phase, increment now */
    int32_t dinc;                  /* increment change per sample (the glide, negative = falling) */
    uint32_t epos, estep;          /* envelope position (Q16 index), step per sample; done at TX_ITONE_ENV << 16 */
    int32_t amp, h2, h3;           /* Q15: level (of full scale), H2 / H3 re H1 */
    uint16_t wait;                 /* samples to wait in the next block (the start offset) */
    uint8_t on, pad;
} tx_itone_t;

/* env[0..64]: sin^2 rise to peak_q15 (position of the peak, 0..1 of the pulse), sin^2 fall to zero */
void tx_itone_env(int16_t *env, int32_t peak_q15);
/* start a pulse of len samples (>= 16) at offset in the next block; the increment falls by glide_q15 x inc over it */
void tx_itone_start(tx_itone_t *t, uint32_t inc, int32_t glide_q15, uint32_t len, int32_t amp_q15, int32_t h2_q15,
                    int32_t h3_q15, unsigned offset);
TX_AUDIO_CODE void tx_itone_add(tx_itone_t *t, const int16_t *env, int32_t *out, unsigned n);

typedef struct {
    int32_t y1, y2;                /* state, Q15 scale */
    int32_t a1, a2;                /* Q14 */
    int32_t b0;                    /* input gain sin w (Q15): an impulse of A rings at amplitude ~A */
    int32_t g;                     /* output gain, Q8 (Q15 state x Q8 = Q23) */
} tx_ireso_t;

/* frequency (Hz, < fs / 2) and Q (x 16) -> coefficients (control rate); keeps the state */
void tx_ireso_set(tx_ireso_t *r, uint32_t f_hz, uint32_t q_x16);
/* run over n samples: impulses imp[k] (Q15 amplitude, signed) at offsets off[k] (ascending, k < nimp), output added */
TX_AUDIO_CODE void tx_ireso_add(tx_ireso_t *r, const uint16_t *off, const int16_t *imp, unsigned nimp, int32_t *out,
                                unsigned n);
static inline int tx_ireso_quiet(const tx_ireso_t *r) { return !(r->y1 | r->y2); }

#endif
