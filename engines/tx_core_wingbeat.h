/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura WINGBEAT core: a flying insect's buzz.
 *
 * The beating wings are a periodic force: lift and drag oscillate at the wingbeat f0 and its harmonics, each a dipole;
 * ~4 harmonics carry ~99 % of the power (N14). H1 is loudest in front of the insect, H2 at its sides (N22, N27), so a
 * buzzer reads two wavetables (front / side) and crossfades them by the angle between its heading and the listener.
 * Wavetables: 256 points + 1 guard, int16, built at control rate from harmonic levels in dB (tx_wing_table).
 * Voice: 32-bit phase, both tables interpolated, the mix and the gain ramped linearly across the block (the caller
 * moves the insect per block: Doppler in the increment, 1 / r in the gain).
 * A buzz, not a note (the user, session 5: "bees sound like an oboe / an electric piano"; the references: bumblebees,
 * honeybees, flies, a swarm show 15-25 harmonics over a noisy floor, blurred at the top, the pitch wobbling):
 *   - the tables carry 24 harmonics: the given levels at half their dB, then -1.2 dB per harmonic (ours);
 *   - every wing stroke is its own: each cycle draws its period (+-JIT, ~4 %) and level (+-10 %), so the waveform
 *     never repeats and the upper harmonics smear into bands;
 *   - the stroke's turbulence: noise band-passed by the body (~150 Hz .. 2.5 kHz, two one-poles) and pulsed by the
 *     stroke (sin^2 of the phase), at NZ (Q15) of the buzz.
 * ~30 ops per sample. 40 B per buzzer. */
#ifndef TX_CORE_WINGBEAT_H
#define TX_CORE_WINGBEAT_H
#include <stdint.h>
#include "tx_core_math.h"

#define TX_WING_N 256u
#define TX_WING_H 8u

typedef struct {
    uint32_t ph, inc;              /* phase, increment (the caller sets inc each block: f0 with Doppler) */
    int32_t g, mix;                /* gain (Q15, of full scale) and side mix (Q15: 0 = front table, 32768 = side) */
    uint32_t cinc, nr;             /* this stroke's increment; the noise state */
    int32_t ca, n1, n2;            /* this stroke's level (Q15); the turbulence band's states */
    int16_t jit, nz;               /* per-stroke period jitter (Q15 of the period, +-) and turbulence level (Q15) */
} tx_buzz_t;

/* a table from harmonic levels: db[k] = level of harmonic k+1 in dB x 2 (int8: -128 = absent), the peak normalised
 * to 32000 */
void tx_wing_table(int16_t *tab, const int8_t *db2, unsigned nh);
/* render one buzzer into out: gain and mix move linearly from their current values to g1 / mix1 over the block */
TX_AUDIO_CODE void tx_buzz_add(tx_buzz_t *b, const int16_t *front, const int16_t *side, int32_t g1, int32_t mix1,
                               int32_t *out, unsigned n);

#endif
