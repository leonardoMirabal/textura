/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura effects: the integer helpers and the reverb kernels' shared types (internal to the tx_fx_*.c files).
 * The helpers and the kernels' contract are Frodhes' (fx.c, rev_types.c: our own), taken over unchanged in what they
 * compute. Effects work at Q15 with headroom (Frodhes' scale: 32768 = full scale); tx_fx.c converts from and to the
 * mix's Q23 at its edges, and only the wet signals pass through them (the dry path stays Q23). */
#ifndef TX_FX_KERN_H
#define TX_FX_KERN_H
#include <stdint.h>
#include <stddef.h>
#include "tx_engine.h"
#include "tx_fx_tables.h"

/* no <string.h> in engines/ (freestanding on the device: its libc's memset / memcpy) */
#define tx_fx_memset __builtin_memset
#define tx_fx_memcpy __builtin_memcpy

#define TX_FX_FS 44100u
#define CTL 32u                     /* a control block: 32 frames (the reverb's step) */
#define HL_NC (CTL / 2u)            /* its half-rate samples */
#define RV_DZ 10u                   /* the 2:1 down's history: the send's 10 samples before the block */

static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int32_t mulq15(int32_t a, int32_t b) { return (a * b) >> 15; }          /* |a|, |b| small enough */
static inline int32_t mul15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
static inline int32_t sh_tz(int32_t m, uint32_t sh) { return (m + (int32_t)((uint32_t)(m >> 31) >> (32u - sh))) >> sh; }
static inline int32_t hl_c16(int32_t v) { v = v > -32768 ? v : -32768; return v < 32767 ? v : 32767; }
static inline int32_t tx_fx_softclip(int32_t x)             /* Q15 tanh: x in Q15, flat beyond +-4 */
{
    uint32_t i;
    int32_t f;
    x = clampi(x, -131072, 131071);
    i = (uint32_t)(x + 131072) >> 10;
    f = (x + 131072) & 1023;
    return TX_FX_TANH[i] + (((TX_FX_TANH[i + 1] - TX_FX_TANH[i]) * f) >> 10);
}
static inline int32_t tx_fx_sine(uint32_t ph)               /* Q32 phase -> +-32767 */
{
    uint32_t i = ph >> 22;
    int32_t f = (int32_t)((ph >> 7) & 0x7FFFu);
    return TX_FX_SINE[i] + (((TX_FX_SINE[i + 1] - TX_FX_SINE[i]) * f) >> 15);
}
static inline uint32_t exp2_neg(uint32_t x)                 /* 2^(-x / 65536) in Q15 */
{
    uint32_t i = x >> 16, u = 32768u - ((x & 0xFFFFu) >> 1), p = 2579u;
    p = ((p * u) >> 15) + 7409u;
    p = ((p * u) >> 15) + 22780u;
    p = ((p * u) >> 15) + 32768u;
    return i < 15u ? p >> (i + 1u) : 0u;
}

/* ---- the reverb kernels' contract (Frodhes rev_types.c): half rate (22.05 kHz), HL_NC samples a call ---- */
typedef struct {            /* filled by the framework once per control block */
    int32_t decay;          /* 0..127 DECAY, after the framework's slew */
    int32_t damp;           /* 0..150 (TONE: 150 dark, 60 at its middle, 0 bright) */
    int32_t size;           /* 0..127 */
    int32_t mod;            /* 0..127 (64 the default) */
    int32_t freeze;         /* the infinite kernel: 0 normal; 1 SUSTAIN (DECAY 120..126); 2 FREEZE (DECAY 127) */
    int32_t dark;           /* the infinite kernel: DARK (no wander, the top held down, the input darker) */
} tx_rev_ctl_t;

/* the 2:1 down into every kernel: an 11-tap half-band, the level into the lines, a DC blocker (Frodhes fx.c rev_down) */
#define RV_HB(c, d) (624 * ((c)[-(d)] + (c)[0]) - 152 * ((c)[-2 * (d)] + (c)[d]) + 40 * ((c)[-3 * (d)] + (c)[2 * (d)]))
#define HL_IN 10320                 /* the send's level into the int16 lines (4x Felucca's: 12 dB less rounding) */
void tx_rev_down(const int32_t *in, int32_t *php, int32_t *phe, int32_t *out);

/* HALL (tx_fx_hall.c: Felucca's hall, as Frodhes runs it) */
#define HL_NL 8u
#define HL_NAP 2u
#define HL_N 752u
typedef struct {
    int16_t comb[(HL_N + 1u) * HL_NL];   /* the 8 lines' ring: row t = their samples written at t; row HL_N repeats 0 */
    int16_t ap[556 + 441];
    uint16_t w, d[HL_NAP];
    int32_t lp[HL_NL], le[HL_NL];
    int32_t hp, he;
    int16_t g[HL_NL], sz;
    uint32_t ph;
    int32_t bx[HL_NC];
    int16_t y[HL_NC][HL_NL];
} tx_hall_t;
void tx_hall_start(tx_hall_t *h);
void tx_hall_run(tx_hall_t *h, const int32_t *send, int32_t size, int32_t damp, int32_t md, int32_t *mid, int32_t *side);

/* INFINITE (tx_fx_inf.c: Frodhes' infinite tank, the LITE build; DARK as its variant) */
#define RI_NL 8u
#define RI_CPT 4u
#define RI_NB (16u * RI_CPT)
#define RI_N 4544u
#define RI_NDIF 3u
#define RI_NLAP 4u
#define RI_APSUM 1750u
typedef struct {
    uint32_t w, age, quiet, apfull;
    int32_t il, ie, ik;
    int32_t xp, ilim, fin, c, ca, mp, sp, gdec, gsz, gi;
    int32_t xi;
    uint8_t half;
    int32_t te;
    int32_t dh, de;
    uint32_t hw;
    int32_t rd;
    int32_t lv, tgt, dq, dqi;
    uint8_t mlo, rr, fm, full;
    int32_t yp[RI_NL], op[RI_NL], oe[RI_NL];
    int32_t ma[RI_NL];
    uint32_t ph[RI_NL];
    uint16_t mI[RI_NL];
    uint16_t len[RI_NL], tlen[RI_NL];
    int16_t g[RI_NL];
    uint16_t apd[RI_NDIF + RI_NLAP];
    int32_t x[RI_NB];
    int32_t m[RI_NB], s[RI_NB];
    int32_t y[RI_NB][RI_NL];
    int16_t ap[RI_APSUM];
    int16_t ring[(RI_N + 1u) * RI_NL];
} tx_inf_t;
void tx_inf_start(tx_inf_t *s);
int tx_inf_active(const tx_inf_t *s);
void tx_inf_run(tx_inf_t *s, const int32_t *in, int32_t *mid, int32_t *side, const tx_rev_ctl_t *c);

#endif
