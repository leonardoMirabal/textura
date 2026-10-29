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

/* no <string.h> in engines/ (freestanding on the device: its libc's memset / memcpy, a byte at a time). Clears go
 * through tx_zero (words: a reverb TYPE's start clears 14 .. 30 KB in the audio interrupt); c must be 0 */
#define tx_fx_memset(p, c, n) (tx_zero((p), (n)), (void)sizeof(char[1 - 2 * ((c) != 0)]))
#define tx_fx_memcpy __builtin_memcpy
/* n words from s to d (TX_RAM code: libc's memcpy is out of its call's reach) */
TX_INLINE void tx_fx_copy32(int32_t *d, const int32_t *s, unsigned n) { unsigned i; for (i = 0; i < n; i++) d[i] = s[i]; }

#define TX_FX_FS 44100u
#define CTL 32u                     /* a control block: 32 frames (the reverb's step) */
#define HL_NC (CTL / 2u)            /* its half-rate samples */
#define RV_DZ 10u                   /* the 2:1 down's history: the send's 10 samples before the block */

TX_INLINE int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
TX_INLINE int32_t mulq15(int32_t a, int32_t b) { return (a * b) >> 15; }          /* |a|, |b| small enough */
TX_INLINE int32_t mul15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
TX_INLINE int32_t sh_tz(int32_t m, uint32_t sh) { return (m + (int32_t)((uint32_t)(m >> 31) >> (32u - sh))) >> sh; }
TX_INLINE int32_t hl_c16(int32_t v) { v = v > -32768 ? v : -32768; return v < 32767 ? v : 32767; }
TX_INLINE int32_t tx_fx_softclip(int32_t x)             /* Q15 tanh: x in Q15, flat beyond +-4 */
{
    uint32_t i;
    int32_t f;
    x = clampi(x, -131072, 131071);
    i = (uint32_t)(x + 131072) >> 10;
    f = (x + 131072) & 1023;
    return TX_FX_TANH[i] + (((TX_FX_TANH[i + 1] - TX_FX_TANH[i]) * f) >> 10);
}
TX_INLINE int32_t tx_fx_sine(uint32_t ph)               /* Q32 phase -> +-32767 */
{
    uint32_t i = ph >> 22;
    int32_t f = (int32_t)((ph >> 7) & 0x7FFFu);
    return TX_FX_SINE[i] + (((TX_FX_SINE[i + 1] - TX_FX_SINE[i]) * f) >> 15);
}
TX_INLINE uint32_t exp2_neg(uint32_t x)                 /* 2^(-x / 65536) in Q15 */
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
    uint32_t *drift;        /* the infinite kernel: DRIFT (the wander a slow random walk; the framework's random state) */
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
/* add: HL_NC half-rate samples added to the decimated input (the shimmer), or 0 */
void tx_hall_run(tx_hall_t *h, const int32_t *send, const int32_t *add, int32_t size, int32_t damp, int32_t md, int32_t *mid,
                 int32_t *side);

/* INFINITE (tx_fx_inf.c: Frodhes' infinite tank, the LITE build; DARK as its variant) */
#define RI_NL 8u
#define RI_CPT 4u
#define RI_NB (16u * RI_CPT)
#define RI_N 1984u                  /* the lines' shared ring, a multiple of RI_NB (the longest line + its swing below) */
#define RI_NDIF 3u
#define RI_NLAP 4u
#define RI_APSUM 597u               /* the diffusers' lines (127 + 193 + 277) */
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
    int32_t wu[RI_NL], wv[RI_NL];   /* DRIFT: each wandering read's walk (two one-poles toward a random target) */
    int32_t wt[RI_NL], wn[RI_NL];   /* its target, the reads until the next one */
    uint8_t wg;                     /* the reads on their walk (DRIFT, or gliding back onto the sine after it) */
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

/* PLATE (tx_fx_plate.c: Frodhes' plate kernel); its sizes here for the TYPEs' union (the same as in its file) */
#define PL_N 16384u                                    /* the tank's ring (int16) */
#define PL_IN 1024u                                    /* the input allpasses' ring */
#define PL_G 17u                                       /* the guard: a block's 16 samples + the interpolation's next */
enum { PL_S_N = 22 };
typedef struct {
    int16_t ring[PL_N + PL_G + 1u];                  /* (+ 1: whole words for pl_cpy) */
    int16_t id[PL_IN + PL_G + 1u];
    int16_t zero[2u * HL_NC];       /* what a read of content older than the start sees */
    uint32_t w, e;                  /* the rings' write index (one a sample), samples since the start (to PL_WARM) */
    uint32_t ph;                    /* the LFO */
    uint32_t quiet;                 /* samples with no input and no line write (to PL_N) */
    int32_t bw, dl, dr;             /* the bandwidth and the two damping one-poles */
    int32_t g, g2, pdm, pbw;        /* decay, allpass 2's gain, the damping / bandwidth poles (Q15): g g2 pdm in
                                     * this order (pl_half's k[]) */
    int32_t sz, szn, rr;            /* the size that runs, the step being made (szn, rr: its block), -1 at the start */
    int32_t cdec, cdamp;            /* the settings the gains were made for */
    int32_t ko;                     /* the output gain (Q12) */
    uint32_t fix;                   /* this block: a write ran into the guard (ring 1, input 2) */
    uint16_t d[PL_S_N];             /* the delays at this size */
    uint16_t tk[PL_S_N];            /* the reads: their segment's top - their delay */
    int32_t x[HL_NC];
} tx_plate_t;
void tx_plate_start(tx_plate_t *s);
int tx_plate_active(const tx_plate_t *s);
TX_AUDIO_CODE void tx_plate_run(tx_plate_t *s, const int32_t *in, int32_t *mid, int32_t *side, const tx_rev_ctl_t *c);

#endif
