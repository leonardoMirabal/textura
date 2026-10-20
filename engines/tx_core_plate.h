/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura PLATE core: a struck thin plate in two layers.
 *
 *   MODES  the lowest exact modes as lossless integer oscillators (the "magic circle": x += e y, y -= e x, a shear
 *          pair, invertible on the integers, so the amplitude neither grows nor dies by rounding) with an amplitude
 *          envelope set per block (decay, coupling losses) and ramped across the block. The frequency coefficient
 *          can move per block (the hardening glide). ~7 ops per mode per sample, nothing for a dead mode.
 *   BANDS  the dense rest as band energies W_b on a log grid (ratio RHO, 17 bands 60 Hz .. ~10 kHz), evolved per
 *          block by the wave-turbulence diffusion (D22 / D24): dE/dt = d/df (phi f E^2 dE/df) - damping + injection,
 *          E the energy per Hz. On the log grid the transfer from band b to b + 1 in one block is
 *            t = r (RHO W_b - W_(b+1)) / (1 + RHO),   r = min(1/4, C E_mean^2 RHO^-b)
 *          (r = 1 would make the two densities equal: the step is stable for any strength); C ~ phi (the CRASH
 *          knob), E_mean the two bands' mean density (W RHO^-b). Strong energy diffuses fast, weak barely: the
 *          spectrum's cut-off rises with the injected power (D24's Kolmogorov-Zakharov front), ~0.15-0.5 s to the
 *          top bands (D21: up to ~0.5 s in gongs). The bands sound as noise through the SVF bank (b < 16) and a
 *          high-pass noise (b = 16), gains sqrt(W_b).
 * Energies are Q30 (full scale squared = 2^30), amplitudes Q15. 64-bit products only per block. */
#ifndef TX_CORE_PLATE_H
#define TX_CORE_PLATE_H
#include <stdint.h>
#include "tx_core_math.h"

#define TX_PLATE_MODES 24u
#define TX_PLATE_BANDS 17u
#define TX_PLATE_F0 60u            /* band 0 centre, Hz */
#define TX_PLATE_RHO_Q16 90007u    /* band ratio 1.3734 (60 Hz .. 7 kHz over 15 steps), Q16 */

typedef struct {
    int32_t x, y;                  /* oscillator state, Q14 amplitude 16000 */
    int32_t e0, e;                 /* 2 sin(pi f / fs), Q16: at rest and now (glide) */
    int32_t g, gl;                 /* amplitude (Q15) now, and the level-scaled amplitude rendered last block */
    uint32_t dec;                  /* amplitude factor per block, Q16 */
    uint16_t hz;                   /* frequency, Hz */
    uint8_t band, pad;             /* the band its coupling losses go to */
} tx_pmode_t;

extern const uint16_t TX_PLATE_BAND_HZ[TX_PLATE_BANDS];
extern const uint32_t TX_PLATE_RINV[TX_PLATE_BANDS];      /* RHO^-b, Q16 */

uint32_t tx_isqrt32(uint32_t x);
/* amplitude decay per block (Q16) for a rate in 1/s (Q8); energy: call with the rate doubled */
uint32_t tx_plate_decay(uint32_t rate_q8);
/* the plate damping law (0.5 mm): xi omega = 8e-3 omega^0.6, as a rate in 1/s, Q8 */
uint32_t tx_plate_law_q8(uint32_t hz);
/* start / add a strike of amplitude a (Q15) to a mode (energies add: random relative phase) */
void tx_pmode_strike(tx_pmode_t *m, int32_t a);
/* render the modes: amplitude ramps from the last block's to g x level, into out (Q23) */
TX_AUDIO_CODE void tx_pmode_add(tx_pmode_t *m, unsigned nm, int32_t level_q15, int32_t *out, unsigned n);
/* one block of wave-turbulence diffusion over W[0..nb-1]: strength c_q0 (C, see above; 0 = linear plate) */
void tx_plate_diffuse(uint32_t *W, unsigned nb, uint32_t c);

#endif
