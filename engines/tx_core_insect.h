/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura INSECT core: the periodic sound sources of singing insects.
 *
 *   TONE   a cricket pulse (one wing closing). The clockwork escapement makes one tooth = one cycle of the harp
 *          resonance, so a steady pulse is a near-pure tone at the carrier: a 32-bit phase, the pitch falling
 *          linearly by a glide over the pulse (5-15 %), an envelope table (65 points, Q15) read across the pulse,
 *          and phase-locked H2 / H3 (sin 2ph, sin 3ph) at their own levels, plus an optional rasp: noise under the
 *          same envelope (the scraper's irregular teeth). ~14-17 ops per sample while it sounds.
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
    int32_t rasp;                  /* the scraper's broadband part under the tone, Q15 of the tone (0 = pure) */
    uint32_t nz;                   /* its noise state (LCG) */
} tx_itone_t;

/* env[0..64]: sin^2 rise to peak_q15 (position of the peak, 0..1 of the pulse), sin^2 fall to zero */
void tx_itone_env(int16_t *env, int32_t peak_q15);
/* start a pulse of len samples (>= 16) at offset in the next block; the increment falls by glide_q15 x inc over it */
void tx_itone_start(tx_itone_t *t, uint32_t inc, int32_t glide_q15, uint32_t len, int32_t amp_q15, int32_t h2_q15,
                    int32_t h3_q15, unsigned offset);
TX_AUDIO_CODE void tx_itone_add(tx_itone_t *t, const int16_t *env, int32_t *out, unsigned n);

typedef struct {
    int32_t y1, y2;                /* state, Q15 scale */
    int32_t p1, p2;                /* the last two inputs (the band-pass zeros) */
    int32_t a1, a2;                /* Q14 */
    int32_t b0;                    /* (set by tx_ireso_coefs; the impulse input uses (1 - z^-2) / 2 instead) */
    int32_t g;                     /* output gain, Q8 (Q15 state x Q8 = Q23) */
    int32_t zlim;                  /* the zero test's level (0 = 8 LSB) */
} tx_ireso_t;

/* frequency (Hz, < fs / 2) and Q (x 16) -> coefficients (control rate); keeps the state */
void tx_ireso_set(tx_ireso_t *r, uint32_t f_hz, uint32_t q_x16);
/* run over n samples: impulses imp[k] (Q15 amplitude, signed) at offsets off[k] (ascending, k < nimp), output added.
 * The input passes (1 - z^-2) / 2 (zeros at DC and fs / 2: a force impulse radiates no DC): an impulse of A rings at
 * amplitude ~A at any frequency. */
TX_AUDIO_CODE void tx_ireso_add(tx_ireso_t *r, const uint16_t *off, const int16_t *imp, unsigned nimp, int32_t *out,
                                unsigned n);
static inline int tx_ireso_quiet(const tx_ireso_t *r) { return !(r->y1 | r->y2 | r->p1 | r->p2); }
/* two resonators on the same impulses, one pass (the same as tx_ireso_add on each; their input states stay equal) */
TX_AUDIO_CODE void tx_ireso_add2(tx_ireso_t *r, tx_ireso_t *q, const uint16_t *off, const int16_t *imp, unsigned nimp,
                                 int32_t *out, unsigned n);

/* TYMBAL (the cicada, N4-N6): rib impulses into a tymbal resonance whose pitch glides down within each click (a1
 * moves linearly per sample from the click's start value toward a1_end: the buckling tymbal's 5.5 -> 4.3 kHz and
 * the in-click tail's -17 %), in series with the abdominal air sac (a low-Q two-pole at the carrier). An event with
 * a1 != 0 restarts the glide (a click's first rib); amp is the impulse (Q15, signed). ~12 ops per sample. */
typedef struct { uint16_t off; int16_t amp; int32_t a1; } tx_tev_t;
typedef struct {
    int32_t y1, y2, s1, s2;        /* tymbal and sac states (Q15 scale, clamped below 2^16) */
    int32_t a1, a1_end, da1;       /* tymbal a1 now, where its glide stops, its step per sample (Q14 / Q14 x 2^-8) */
    int32_t a2, b0;                /* tymbal r^2 (Q14), input gain sin w (Q15) */
    int32_t sa1, sa2, sb0;         /* sac coefficients */
    int32_t g;                     /* output gain, Q12 (Q15 x Q12 >> 4 = Q23), <= 16383 */
    int32_t frac;                  /* the glide's fractional accumulator */
    int32_t zin, x1, x2, d1, d2;   /* zin 1: the input passes (1 - z^-2) (no DC, no low leak: a two-pole fed directly
                                    * passes its input's lows only ~10-30 dB under the ring); 2: twice (a high carrier's
                                    * clean ring); the last inputs and differences */
} tx_tymbal_t;

/* coefficient helper: a1 (Q14) and a2 (Q14) and b0 (Q15) of a two-pole at f, Q */
void tx_ireso_coefs(uint32_t f_hz, uint32_t q_x16, int32_t *a1, int32_t *a2, int32_t *b0);
TX_AUDIO_CODE void tx_tymbal_add(tx_tymbal_t *t, const tx_tev_t *ev, unsigned nev, int32_t *out, unsigned n);
static inline int tx_tymbal_quiet(const tx_tymbal_t *t) { return !(t->y1 | t->y2 | t->s1 | t->s2 | t->x1 | t->x2 | t->d1 | t->d2); }

#endif
