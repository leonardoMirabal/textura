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

/* an engine's loud corner (every key at once, many big events landing together) kept inside 16x full scale: above 8x
 * (+18 dBFS) a quarter of the excess, at most TX_HEAD_MAX (~+22.6 dBFS; a DC blocker after it may add a little). One
 * press at an engine's defaults never gets near it (raw peaks about 0 dBFS by design), so those renders are unchanged;
 * the output limiter after does the rest */
#define TX_HEAD_KNEE (1 << 26)
#define TX_HEAD_MAX ((1 << 27) - (1 << 25))
TX_AUDIO_CODE static inline void tx_headroom(int32_t *x, unsigned n)
{
    unsigned j;
    for (j = 0; j < n; j++) {
        int32_t v = x[j];
        if (v > TX_HEAD_KNEE) { v = TX_HEAD_KNEE + ((v - TX_HEAD_KNEE) >> 2); x[j] = v > TX_HEAD_MAX ? TX_HEAD_MAX : v; }
        else if (v < -TX_HEAD_KNEE) { v = -TX_HEAD_KNEE - ((-TX_HEAD_KNEE - v) >> 2); x[j] = v < -TX_HEAD_MAX ? -TX_HEAD_MAX : v; }
    }
}

#endif
