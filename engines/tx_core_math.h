/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura integer math shared by the cores: PRNG, clz, -log2(u), 2^x, sine, pitch. No float, no libgcc calls. */
#ifndef TX_CORE_MATH_H
#define TX_CORE_MATH_H
#include <stdint.h>
#include "tx_engine.h"
#include "tx_tables.h"

/* ---- deterministic PRNG: xorshift32 (3 shifts, 3 xors per draw). Never 0. */
typedef struct { uint32_t s; } tx_rng_t;
static inline void tx_rng_seed(tx_rng_t *r, uint32_t seed)
{
    uint32_t s = seed * 2654435761u + 0x6D2B79F5u;          /* spread small seeds over the state */
    s ^= s >> 15;
    r->s = s ? s : 0x9E3779B9u;
}
static inline uint32_t tx_rand(tx_rng_t *r)
{
    uint32_t x = r->s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return r->s = x;
}
/* uniform 0 .. n-1 for n < 65536 (32-bit product, no division) */
static inline uint32_t tx_rand_below(tx_rng_t *r, uint32_t n) { return ((tx_rand(r) >> 16) * n) >> 16; }
/* signed uniform in [-2^15, 2^15) */
static inline int32_t tx_rand_s15(tx_rng_t *r) { return (int32_t)(tx_rand(r) >> 16) - 32768; }

/* ---- count leading zeros (portable: no __builtin, which may become a libcall on the device) */
static inline uint32_t tx_clz32(uint32_t x)
{
    uint32_t n = 0;
    if (!x) return 32;
    if (!(x & 0xFFFF0000u)) { n += 16; x <<= 16; }
    if (!(x & 0xFF000000u)) { n += 8; x <<= 8; }
    if (!(x & 0xF0000000u)) { n += 4; x <<= 4; }
    if (!(x & 0xC0000000u)) { n += 2; x <<= 2; }
    if (!(x & 0x80000000u)) n += 1;
    return n;
}

/* ---- -log2(u / 2^32) in Q12 for u in 1 .. 2^32-1 (u = 0 reads as 1): 0 .. 32.0 (131072).
 * The exponential and Pareto draws start here: -ln(u) = -log2(u) * ln 2. */
static inline uint32_t tx_neglog2_q12(uint32_t u)
{
    uint32_t n, m, i, fr, l;
    if (!u) u = 1u;
    n = tx_clz32(u);
    m = u << n;                                             /* the mantissa, top bit set */
    i = (m >> 23) & 0xFFu;
    fr = (m >> 15) & 0xFFu;
    l = TX_LOG2_FRAC[i] + (((uint32_t)(TX_LOG2_FRAC[i + 1] - TX_LOG2_FRAC[i]) * fr) >> 8);   /* Q15 */
    return ((1u + n) << 12) - (l >> 3);
}

/* ---- 2^(x / 65536) as an integer (floor), for x < 32 << 16 (saturates above; 0 below 2^-1).
 * tx_exp2(x + (16 << 16)) is 2^x in Q16, and so on. */
static inline uint32_t tx_exp2(int32_t x_q16)
{
    int32_t ip = x_q16 >> 16;                               /* floor (arithmetic shift) */
    uint32_t f = (uint32_t)x_q16 & 0xFFFFu, i = f >> 8, fr = f & 0xFFu;
    uint32_t m = TX_EXP2_FRAC[i] + (((TX_EXP2_FRAC[i + 1] - TX_EXP2_FRAC[i]) * fr) >> 8);   /* Q30 */
    if (ip >= 32) return 0xFFFFFFFFu;
    if (ip >= 30) return m << (ip - 30);
    if (ip < -1) return 0;
    return m >> (30 - ip);
}

/* ---- sine of a 32-bit phase, Q15 (quarter table, linear interpolation) */
TX_AUDIO_CODE static inline int32_t tx_sin_q15(uint32_t ph)
{
    uint32_t p = (ph >> 14) & 0xFFFFu, i, fr;
    int32_t v;
    if (ph & 0x40000000u) p = 65536u - p;                  /* 2nd and 4th quarters: mirrored */
    i = p >> 8; fr = p & 0xFFu;
    v = i >= 256u ? TX_QSIN[256] : TX_QSIN[i] + (((int32_t)TX_QSIN[i + 1] - TX_QSIN[i]) * (int32_t)fr >> 8);
    return (ph & 0x80000000u) ? -v : v;
}

/* ---- pitch: a note in semitones Q16 (MIDI numbering, 69 = A4 = 440 Hz) -> 32-bit phase increment per sample.
 * Capped below fs/2. */
static inline uint32_t tx_note_inc(int32_t note_q16)
{
    int32_t l = TX_LOG2_INC_A4 + (note_q16 - (69 << 16)) / 12;
    if (l > (31 << 16) - 1) l = (31 << 16) - 1;
    return tx_exp2(l);
}

/* ---- a value v in 0..127 mapped exponentially from lo to lo * 2^oct_q16 (for log-law knobs), in lo's unit */
static inline uint32_t tx_knob_exp(uint32_t v, uint32_t lo, int32_t oct_q16)
{
    uint32_t g = tx_exp2(oct_q16 * (int32_t)v / 127 + (16 << 16));   /* Q16; oct_q16 <= 16 octaves: no overflow */
    return (uint32_t)(((uint64_t)lo * g) >> 16);
}

#endif
