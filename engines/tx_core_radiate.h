/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura RADIATE: a small source radiates its low frequencies poorly. A body of radius a is a weak radiator below
 * ka ~ 1 (f ~ c / (2 pi a): ~5 kHz for a 1 cm cicada or cricket harp), its pressure falling ~12 dB per octave below
 * (a monopole's radiation efficiency ~ (ka)^2). Two one-pole high-passes in series at the corner (2^-k: k 2 ~ 2 kHz,
 * k 3 ~ 0.9 kHz, k 4 ~ 0.45 kHz), in place on a block; rounding away from zero, so silence stays exact zero.
 * (Spectrogram review, session 5: the insects' clicks reached 20 Hz and their pulse rates showed as low lines; the
 * references have nothing there.) */
#ifndef TX_CORE_RADIATE_H
#define TX_CORE_RADIATE_H
#include <stdint.h>
#include "tx_engine.h"

typedef struct { int32_t l1, l2; } tx_rad_t;

TX_AUDIO_CODE static inline void tx_rad_hp(tx_rad_t *r, int32_t *x, unsigned n, unsigned k)
{
    int32_t l1 = r->l1, l2 = r->l2, rd = (1 << k) - 1;
    unsigned j;
    for (j = 0; j < n; j++) {
        int32_t h = x[j] - l1, y;
        l1 += (h + (h > 0 ? rd : 0)) >> k;
        y = x[j] - l1;
        h = y - l2;
        l2 += (h + (h > 0 ? rd : 0)) >> k;
        x[j] = y - l2;
    }
    r->l1 = l1; r->l2 = l2;
}
static inline int tx_rad_quiet(const tx_rad_t *r) { return !(r->l1 | r->l2); }

#endif
