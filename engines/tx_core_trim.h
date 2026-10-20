/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura TRIM: an engine's output gain constant (the cross-engine loudness calibration, LOUDNESS.md), Q12
 * (4096 = 1.0, at most 65535 = +24.1 dB). Applied once to the engine's own block before it is added to the mix: the engine's
 * internal paths keep their designed ranges. 32-bit split products. */
#ifndef TX_CORE_TRIM_H
#define TX_CORE_TRIM_H
#include <stdint.h>
#include "tx_core_math.h"

TX_AUDIO_CODE static inline int32_t tx_trim1(int32_t x, int32_t g_q12)
{
    int32_t hi = x >> 12;                                    /* |x| <= 16 full scale here: products < 2^31 */
    hi = hi > 32767 ? 32767 : hi < -32768 ? -32768 : hi;
    return hi * g_q12 + (int32_t)(((uint32_t)x & 0xFFFu) * (uint32_t)g_q12 >> 12);
}

/* in place */
TX_AUDIO_CODE static inline void tx_trim(int32_t *x, unsigned n, int32_t g_q12)
{
    unsigned j;
    if (g_q12 == 4096) return;
    for (j = 0; j < n; j++) x[j] = tx_trim1(x[j], g_q12);
}

/* add a trimmed block into the mix */
TX_AUDIO_CODE static inline void tx_trim_add(const int32_t *x, int32_t *mix, unsigned n, int32_t g_q12)
{
    unsigned j;
    for (j = 0; j < n; j++) mix[j] += tx_trim1(x[j], g_q12);
}

#endif
