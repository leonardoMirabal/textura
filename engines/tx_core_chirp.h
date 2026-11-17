/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura CHIRP core: the flexural "pew" of a crack on floating ice.
 *
 * Bending waves in a thin plate are dispersive, omega = kappa k^2 with kappa = h sqrt(E / (12 rho (1 - s^2))) ~ 951 h
 * m^2/s (E 9 GPa): the group velocity is twice the phase velocity and grows with f, so at distance x the frequency f
 * arrives at t = x / (2 sqrt(2 pi f kappa)): f(t) = x^2 / (8 pi kappa t^2), falling (the exact Green's function's phase
 * is x^2 / (4 kappa t)). A chirp starts when f(t) falls below f_start (16 kHz) and ends at the air wave
 * (x / 343 m/s) or below 40 Hz.
 *   frequency   per block, exactly in the log domain: inc = K fs 2^32 / T^2 (T = samples since the crack), ramped
 *               linearly across the block (no 1/t^2 table: its resolution problem does not arise).
 *   amplitude   ~ t0 / t (a point source on the plate), times the ice's loss along the path
 *               exp(-pi f x / (Q c_g)), c_g = 2 sqrt(2 pi f kappa) (high frequencies fade first; colder ice rings on);
 *               a fade-in over its first block, a fade-out over its last.
 * ~10 ops / sample per chirp. */
#ifndef TX_CORE_CHIRP_H
#define TX_CORE_CHIRP_H
#include <stdint.h>
#include "tx_core_math.h"

typedef struct {
    uint32_t ph, inc;              /* phase, increment */
    uint32_t T, Tend;              /* samples since the crack (the chirp starts at T0 > 0), the end */
    int32_t lk;                    /* log2(K fs 2^32), Q16 */
    int32_t latt;                  /* attenuation: log2 factor per sqrt(Hz), Q16 (negative) */
    int32_t l0;                    /* log2 of the amplitude at T0 x T0, Q16 */
    int32_t g;                     /* amplitude now, Q15 of full scale */
    uint32_t wait;                 /* samples before the chirp starts (T0) */
    uint8_t on, pad[3];
} tx_chirp_t;

/* start: distance x (cm), thickness h (mm), ice Q, start amplitude (Q15) */
void tx_chirp_start(tx_chirp_t *c, uint32_t x_cm, uint32_t h_mm, uint32_t q, int32_t amp_q15);
TX_AUDIO_CODE void tx_chirp_add(tx_chirp_t *c, int32_t *out, unsigned n);
/* the air wave's delay for a distance (samples) */
static inline uint32_t tx_chirp_air(uint32_t x_cm) { return x_cm * 1286u / 1000u; }   /* fs / 34300 cm/s */

#endif
