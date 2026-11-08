/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura LIN: a knob made perceptually even in what it does. A sound's activity quantity (events/s, a singing share,
 * a speed) rises along its own path as the knob turns, most of them exponentially: the bottom half of the knob held
 * almost nothing, the top an explosion. A straight line in the quantity is no better: all the audible change lands in
 * the first step or two (one stone 0.05 -> 7.6 knocks/s). Here the knob u is turned into the place v on that path
 * where the quantity is q0 + (q127 - q0) x^2.5 (x = u / 127), between the two: by the renders (every sound's loudness
 * and its rate on the 9 HOW ACTIVE keys) 2.5 gives the first key 1/8 of the loudness range, and the steps' spread in
 * loudness and in log rate within 0.1 of the most even exponent. Both ends unchanged. The path is given as l[17] =
 * log2 of the quantity (Q16, any unit: it cancels) at v = 0, 8 .. 120, 127, monotone; between the points log-linear
 * (exact for a log scale). All l equal (zeros): v = u, the knob as it is. tx_lin_pow is the curve itself, for a
 * quantity given directly (a rate between two ends, a count, a blink). tx_lin_tab reads a sound's own measured curve
 * instead (tools/gen_lin.py), where one was measured: each sound is different, so each gets its own.
 * Integers, 32-bit; a log2, an exp2 and a short search: per block, never per sample. */
#ifndef TX_CORE_LIN_H
#define TX_CORE_LIN_H
#include <stdint.h>
#include "tx_core_math.h"
#include "tx_core_place.h"

/* the curve: x^2.5 for x in 0 .. 1 (Q16 in, Q16 out) */
static inline uint32_t tx_lin_pow(uint32_t x_q16)
{
    int32_t l;
    if (!x_q16) return 0;
    if (x_q16 >= 65536u) return 65536u;
    l = tx_place_log2(x_q16) - (16 << 16);                   /* log2 x, Q16 (<= 0) */
    return tx_exp2((l * 5) / 2 + (16 << 16));                /* (|l| < 2^20: 32 bits) */
}
/* the curve at a knob u (Q8, 0 .. 127 << 8): (u / 127)^2.5, Q16 */
static inline uint32_t tx_lin_frac(int32_t u_q8)
{
    return u_q8 <= 0 ? 0u : tx_lin_pow(((uint32_t)u_q8 << 8) / 127u);   /* (u / 127, Q16: 127 exactly 1) */
}

/* a knob through a measured table (a sound's own curve: tools/gen_lin.py from its measured loudness and sounding share,
 * so each step sounds the same more, the top at the sound's audible top): t[17] = the place (whole knob units) at
 * u = 0, 8 .. 120, 127, rising; the place (Q8) at the knob u (Q8), straight between the points */
static inline int32_t tx_lin_tab(const uint8_t *t, int32_t u_q8)
{
    unsigned k;
    int32_t a, w;
#ifdef TX_LIN_IDENTITY
    (void)t; (void)k; (void)a; (void)w;
    return u_q8;                                             /* (a measurement build: the knob is the place) */
#endif
    if (u_q8 <= 0) return (int32_t)t[0] << 8;
    if (u_q8 >= (127 << 8)) return (int32_t)t[16] << 8;
    k = (unsigned)u_q8 >> 11;
    if (k > 15u) k = 15u;
    a = (int32_t)k << 11;
    w = k < 15u ? 8 << 8 : 7 << 8;
    return ((int32_t)t[k] << 8) + ((((int32_t)t[k + 1] - (int32_t)t[k]) << 8) * (u_q8 - a)) / w;   /* (< 2^26) */
}

/* the place v (Q8 knob units, 0 .. 127 << 8) for the knob u (Q8, 0 .. 127 << 8) */
static inline int32_t tx_lin_knob(const int32_t *l, int32_t u_q8)
{
    int32_t lo = l[0], hi = l[16], lt, d;
    uint32_t f0, fr, qt;
    unsigned k;
#ifdef TX_LIN_IDENTITY
    (void)lo; (void)hi; (void)lt; (void)d; (void)f0; (void)fr; (void)qt; (void)k;
    return u_q8;                                             /* (a measurement build: the knob is the place) */
#endif
    if (hi <= lo) return u_q8;
    if (u_q8 <= 0) return 0;
    if (u_q8 >= (127 << 8)) return 127 << 8;
    d = lo - hi;
    if (d < -(23 << 16)) d = -(23 << 16);                    /* (over 23 octaves: the bottom read as 2^-23 of the top) */
    f0 = tx_exp2(d + (24 << 16));                             /* the quantity at 0 over the top's, Q24 */
    fr = tx_lin_frac(u_q8);                                  /* (u / 127)^2.5, Q16 */
    qt = f0 + ((((1u << 24) - f0) >> 9) * fr >> 7);          /* the curve, Q24 (< 2^15 x 2^16: 32 bits) */
    lt = tx_place_log2(qt) - (24 << 16) + hi;                /* log2 of the quantity wanted */
    if (lt <= lo) return 0;
    for (k = 0; k < 16u; k++) {
        int32_t a = l[k], b = l[k + 1], va = (int32_t)k * 8, vb = k < 15u ? va + 8 : 127, r;
        if (lt > b || b <= a) continue;
        r = (int32_t)(((uint32_t)(lt - a) << 12) / (uint32_t)(b - a));   /* Q12 (a step under 2^19: 8 octaves) */
        return (va << 8) + (((vb - va) << 8) * r >> 12);
    }
    return 127 << 8;
}

/* the same for a plain log scale of oct_q16 octaves over the knob (a rate x 2^(oct v / 127)): v for u, both 0 .. 127
 * (whole knob units, rounded) */
static inline unsigned tx_lin_exp(int32_t oct_q16, unsigned u)
{
    int32_t l[17];
    unsigned k;
    for (k = 0; k < 17u; k++) l[k] = (oct_q16 / 127) * (int32_t)(k < 16u ? k * 8u : 127u);
    return (unsigned)((tx_lin_knob(l, (int32_t)(u > 127u ? 127u : u) << 8) + 128) >> 8);
}

#endif
