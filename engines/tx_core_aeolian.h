/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura AEOLIAN core: the tone of air flowing past a wire.
 *
 *   shedding  f_l = St(Re) u / d, Re = u d / nu (nu = 1.5e-5 m^2/s), St = lambda + tau / sqrt(Re) by Re range (A2b
 *             Table 2); no shedding below Re 47. Tone width (A2b Eq. 11): df/f = 4.624e-5 Re + 0.9797 % (Q ~ 101 at Re
 *             200, ~52 at 2e4).
 *   voice     a narrowband tone: an oscillator whose phase diffuses (a Lorentzian line of width B = f / Q exactly,
 *             at any f below Nyquist: no fs/6 limit), with three coherent components: lift at f and 3 f,
 *             drag at 2 f (-10 dB in intensity: amplitude 0.32; 3 f by ear). Their levels
 *             are set by the caller per block (ramped). ~25 ops / sample.
 * The wake noise (intensity ~ u^8, -12 dB/oct above f_l) is the NOISE core's roar path in the engine. */
#ifndef TX_CORE_AEOLIAN_H
#define TX_CORE_AEOLIAN_H
#include <stdint.h>
#include "tx_core_math.h"

typedef struct {
    uint32_t th, inc;              /* phase (2^32 = one cycle of f) and increment */
    uint32_t psi;                  /* the diffusing phase (phase units) */
    int32_t sig;                   /* phase diffusion: phase units per unit of a Q15 uniform kick */
    int32_t a1, a2, a3;            /* component amplitudes now (Q15 of full scale): lift f, drag 2f, lift 3f */
    tx_rng_t rng;
} tx_aeol_t;

/* St and Q for a flow: u in mm/s, d in um; returns f_l in Hz (0: no shedding) and sets *q_x16 (Q x 16) */
uint32_t tx_aeol_shed(uint32_t u_mms, uint32_t d_um, uint32_t *q_x16);
/* tone width: the phase diffusion for a line of width B = f / Q (Hz) */
void tx_aeol_width(tx_aeol_t *v, uint32_t f_hz, uint32_t q_x16);
/* render: the components move linearly to a1 / a2 / a3 over the block */
TX_AUDIO_CODE void tx_aeol_add(tx_aeol_t *v, int32_t a1, int32_t a2, int32_t a3, int32_t *out, unsigned n);

#endif
