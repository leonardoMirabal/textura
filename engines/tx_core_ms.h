/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura M/S core: the mix's last effect, on the stereo pair after the REVERB and before the output limiter
 * (tx_core_mix.c tx_mix_run). The pair is split into its middle and its sides, M = (L + R) / 2, S = (L - R) / 2; each
 * gets its own gain, then L = M + S, R = M - S again.
 *
 *   MID, SIDE   0 .. 127: 0 is -inf (silent), v (1 .. 127) is (v - 115) / 2 dB: -57 .. +6 dB in 0.5 dB steps,
 *               115 = 0 dB (the default)
 *   WIDTH       0 .. 100: 0 .. 200 % in 2 % steps (50 = 100 %, as it is; the default): the sides' gain after SIDE,
 *               x WIDTH / 50 (0 mono, 100 twice as wide: +6.02 dB). A parameter of its own: MID, SIDE and WIDTH never
 *               set each other
 *   MONO        OFF / ON: the sides' gain 0 (SIDE kept for when it goes off)
 *
 * MID and SIDE at 0 dB, WIDTH 100 % and MONO off: the pair passes untouched, bit for bit (the gains are compared, not applied).
 * A gain change glides: each block moves a quarter of the way to the new gain (~23 ms; the last steps exactly there),
 * ramped sample by sample inside the block, so a knob never zips and MONO fades instead of cutting.
 *
 * The meters (meter on: the screen showing them asks; the plugin always): each block the peaks of L, R, M and S as they
 * leave (Q23, held and falling 1/8 a block) and the correlation, (EM - ES) / (EM + ES) from the middle's and the sides'
 * energies (Q15: +32767 mono, 0 wide, below 0 the sides cancel; it equals the L / R correlation while the two sides
 * are as loud), smoothed over ~4 blocks; silence leaves it where it was. Integers only, 32-bit products. */
#ifndef TX_CORE_MS_H
#define TX_CORE_MS_H
#include <stdint.h>
#include "tx_engine.h"

enum { TX_MS_MID, TX_MS_SIDE, TX_MS_WIDTH, TX_MS_MONO, TX_MS_NP };
#define TX_MS_0DB 115u                  /* MID / SIDE: 0 dB */
#define TX_MS_W100 50u                  /* WIDTH: 100 % */
#define TX_MS_UNITY 16384               /* a half gain, Q15: 0 dB (M and S are L +- R, the halves in the gains) */
extern const tx_param_t tx_ms_params[TX_MS_NP];

typedef struct {
    int32_t gm, gs;                     /* the half gains now, Q15 (16384 = 0 dB; the sides' up to 65380: +6 dB SIDE at
                                         * WIDTH 200 %) */
    int32_t tm, ts;                     /* where they glide to */
    uint8_t p[TX_MS_NP];
    uint8_t meter;                      /* the meters wanted, by whom: bit 0 the FM-1's screen (its M/S page shown,
                                         * ui.c), bit 1 the desktop plugin's window; 0: none kept */
    int32_t pk[4];                      /* L R M S peaks, Q23 (held, falling 1/8 a block) */
    int32_t corr;                       /* the correlation, Q15 (-32767 .. 32767) */
} tx_ms_t;

void tx_ms_init(tx_ms_t *m);
void tx_ms_param(tx_ms_t *m, unsigned id, unsigned v);
/* in place on the pair's block (Q23) */
TX_AUDIO_CODE void tx_ms_process(tx_ms_t *m, int32_t *L, int32_t *R, unsigned n);
/* MID / SIDE value v as a half gain, Q15 (0: -inf) */
int32_t tx_ms_gain(unsigned v);
/* a MID / SIDE value's level in tenths of a dB (v 1 .. 127; 0: -inf, not asked) */
static inline int32_t tx_ms_db10(unsigned v) { return ((int32_t)v - (int32_t)TX_MS_0DB) * 5; }

#endif
