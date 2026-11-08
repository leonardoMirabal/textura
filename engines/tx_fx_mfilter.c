/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura FILTER (the mix's, right after the DRIVE): a high-pass then a low-pass in series on the stereo pair, the two
 * cutoffs the edges of a band. LOW CUT (the high-pass) and HIGH CUT (the low-pass), 30 Hz .. 16 kHz (log, the filter
 * tables' laws: TX_FX_FL_G); each edge is off at its end (LOW CUT 0, HIGH CUT 127): the defaults, the mix untouched.
 * RESONANCE is shared and gentle (Q 0.707 .. ~4 at both edges); SLOPE 12 or 24 dB an octave (24: a second stage at
 * each edge, Q 0.707, so the resonance peaks once).
 *
 * Crossed edges (LOW CUT above HIGH CUT): the band collapses to its narrowest, both edges at the middle of the two
 * knobs (in octaves): a narrow, quieter band there (-6 dB at its peak, 12 dB; -12 dB, 24 dB), never silence and never
 * a jump as the knobs pass each other.
 *
 * Each edge is a TPT state-variable stage (the two integrators trapezoidal, solved at once), stable at any setting. The cutoffs glide (one pole, ~6 ms, coefficients every 32 samples:
 * no zipper, the matrix's steps smoothed); an edge turning on or off, and the 24 dB stage, cross-fade in 46 ms with
 * the signal before them (no click); a stage that starts again starts from rest. Each integrator steps one LSB toward 0
 * every 32 samples (far under the noise floor), so the rounding's dead band (a low cutoff's DC held) cannot outlast the sound: silence
 * in ends in exact zeros out. Precision: Q21 inside (the mix's Q23
 * / 4; the input held to 16x full scale), its products 64-bit (the FM-1's 64-bit multiply checked bit-exact against the host).
 * Both edges off and faded out: returns at once, no sample changed (the defaults' renders are the mix's). In XIP
 * flash on the FM-1, as the rest of the bus chain (core 1 runs it). */
#include "tx_fx.h"

const tx_param_t tx_fx_mfilter_params[TX_MF_NP] = {
    {"LOW CUT", 127, 0, TX_TEXT("the band's low edge, a high-pass: off (0) .. 30 Hz .. 16 kHz (log)")},
    {"HIGH CUT", 127, 127, TX_TEXT("the band's high edge, a low-pass: 30 Hz .. 16 kHz (log) .. off (127); below LOW CUT: a narrow band between the two")},
    {"RESONANCE", 127, 0, TX_TEXT("a peak at both edges, gentle: Q 0.707 .. ~4")},
    {"SLOPE", 1, 0, TX_TEXT("the edges' steepness: 0 12 dB / octave | 1 24 dB / octave")},
};

#define MF_SUB 32u                  /* the coefficients' step, samples */
#define MF_FADE 512                 /* a stage's fade a step (Q15): 64 steps, 46 ms */
#define MF_K0 23726566              /* k = sqrt 2 (Q 0.707), Q24: the 24 dB stage's */

void tx_fx_mfilter_init(tx_fx_mfilter_t *f)
{
    unsigned i;
    tx_fx_memset(f, 0, sizeof *f);
    for (i = 0; i < TX_MF_NP; i++) f->p[i] = tx_fx_mfilter_params[i].def;
}
void tx_fx_mfilter_param(tx_fx_mfilter_t *f, unsigned id, unsigned v)
{
    if (id < TX_MF_NP) f->p[id] = (uint8_t)(v > tx_fx_mfilter_params[id].max ? tx_fx_mfilter_params[id].max : v);
}
uint32_t tx_fx_mfilter_us(const tx_fx_mfilter_t *f)
{
    uint32_t e = (f->p[TX_MF_LOW] > 0 || f->w[0]) + (f->p[TX_MF_HIGH] < 127 || f->w[1]);
    return e * (f->p[TX_MF_SLOPE] || f->w2 ? 2u * TX_FX_US_MF_STAGE : TX_FX_US_MF_STAGE);
}

static int32_t mf_recip27(uint32_t den)                     /* 2^43 / den (den Q16) = 1 / den in Q27, 32-bit only */
{
    uint32_t q = 0x80000000u / den, r = 0x80000000u - q * den;
    return (int32_t)((q << 12) + (r << 12) / den);
}
typedef struct { int32_t a1, a2, a3, k; } mf_coef_t;       /* 1 / (1 + g (g + k)), g a1, g a2 (Q27); k (Q24) */
static void mf_coef(mf_coef_t *c, int32_t g24, int32_t k24)
{
    c->a1 = mf_recip27(65536u + (uint32_t)(((int64_t)g24 * (g24 + k24)) >> 32));
    c->a2 = (int32_t)(((int64_t)g24 * c->a1) >> 24);
    c->a3 = (int32_t)(((int64_t)g24 * c->a2) >> 24);
    c->k = k24;
}
TX_INLINE int32_t mf_cl(int32_t v) { v = v > -(1 << 27) ? v : -(1 << 27); return v < (1 << 27) ? v : 1 << 27; }
TX_INLINE int32_t mq15(int32_t a, int32_t q) { return (a >> 15) * q + (int32_t)((((uint32_t)a & 32767u) * (uint32_t)q) >> 15); }

/* one sample of one stage (Q21 in, the output Q21: hp 1 the high-pass's, else the low-pass's); its products side by side
 * (one multiply deep from state to state: the loop is not held up by a chain of them) */
TX_INLINE int32_t mf_tick(int32_t v, int32_t *s1, int32_t *s2, const mf_coef_t *c, const int hp)
{
    int32_t v3 = v - *s2, v1, v2;
    v1 = (int32_t)(((int64_t)c->a1 * *s1 + (int64_t)c->a2 * v3 + (1 << 26)) >> 27);   /* (rounded: no DC creep) */
    v2 = *s2 + (int32_t)(((int64_t)c->a2 * *s1 + (int64_t)c->a3 * v3 + (1 << 26)) >> 27);
    *s1 = 2 * v1 - *s1;                                     /* (bounded: 16x FS in, Q <= 4; clamped each run) */
    *s2 = 2 * v2 - *s2;
    return hp ? v - (int32_t)(((int64_t)c->k * v1 + (1 << 23)) >> 24) - v2 : v2;
}
TX_INLINE int32_t mf_in(int32_t x) { x >>= 2; x = x > -(1 << 25) ? x : -(1 << 25); return x < (1 << 25) ? x : 1 << 25; }   /* (Q21, 16x FS) */
/* one stage on n samples of both sides, in place (Q23; the two sides in one loop: two chains at once), its share w0 ..
 * w0 + n dw against its input (fade 0: the stage alone, 32768) */
TX_INLINE void mf_body(int32_t *L, int32_t *R, unsigned n, int32_t (*s)[2], const mf_coef_t *c, const int hp,
                       const int fade, int32_t w0, int32_t dw)
{
    int32_t l1 = s[0][0], l2 = s[0][1], r1 = s[1][0], r2 = s[1][1], w = w0;
    const mf_coef_t k = *c;
    unsigned j;
    for (j = 0; j < n; j++) {
        int32_t xl = L[j], xr = R[j];
        int32_t yl = mf_tick(mf_in(xl), &l1, &l2, &k, hp) * 4, yr = mf_tick(mf_in(xr), &r1, &r2, &k, hp) * 4;
        if (fade) { w += dw; yl = xl + mq15(yl - xl, w); yr = xr + mq15(yr - xr, w); }   /* (|y| < 2^29: no overflow) */
        L[j] = yl; R[j] = yr;
    }
    l1 = mf_cl(l1); l2 = mf_cl(l2); r1 = mf_cl(r1); r2 = mf_cl(r2);   /* (a guard: never reached in use) */
    l1 -= (l1 > 0) - (l1 < 0); l2 -= (l2 > 0) - (l2 < 0);   /* (a step to 0 each run: no dead band, silence ends in zeros) */
    r1 -= (r1 > 0) - (r1 < 0); r2 -= (r2 > 0) - (r2 < 0);
    s[0][0] = l1; s[0][1] = l2; s[1][0] = r1; s[1][1] = r2;
}
static void mf_run(int32_t *L, int32_t *R, unsigned n, int32_t (*s)[2], const mf_coef_t *c, int hp, int32_t w0, int32_t dw)
{
    if (dw || w0 != 32768) mf_body(L, R, n, s, c, hp, 1, w0, dw);   /* (fading: one loop for both kinds) */
    else if (hp) mf_body(L, R, n, s, c, 1, 0, w0, 0);
    else mf_body(L, R, n, s, c, 0, 0, w0, 0);
}

static int32_t mf_g(int32_t pos)                            /* g at a position 0 .. 65535 (Q24) */
{
    uint32_t i = (uint32_t)pos >> 8;
    int32_t fr = pos & 255;
    return (int32_t)TX_FX_FL_G[i] + ((((int32_t)TX_FX_FL_G[i + 1] - (int32_t)TX_FX_FL_G[i]) * fr) >> 8);
}

void tx_fx_mfilter_process(tx_fx_mfilter_t *f, int32_t *L, int32_t *R, unsigned n)
{
    int32_t tgt[2], k = (int32_t)TX_FX_FL_K[(f->p[TX_MF_RESO] * 66u) >> 7];   /* (Q 0.707 .. ~4) */
    int act[2], s24 = f->p[TX_MF_SLOPE] != 0;
    unsigned sb, e;
    act[0] = f->p[TX_MF_LOW] > 0; act[1] = f->p[TX_MF_HIGH] < 127;
    if (!act[0] && !act[1] && !f->w[0] && !f->w[1]) { f->w2 = s24 ? 32768 : 0; return; }   /* (off: untouched) */
    tgt[0] = f->p[TX_MF_LOW] * 516; tgt[1] = f->p[TX_MF_HIGH] * 516;
    if (tgt[0] > tgt[1]) tgt[0] = tgt[1] = (tgt[0] + tgt[1]) >> 1;   /* crossed: the narrowest band, at the middle */
    for (e = 0; e < 2u; e++)
        if (act[e] && !f->w[e]) {                           /* an edge comes on: from rest, at its place */
            f->pos[e] = tgt[e];
            tx_fx_memset(f->s[e], 0, sizeof f->s[e]);
            tx_fx_memset(f->s2[e], 0, sizeof f->s2[e]);
        }
    if (s24 && !f->w2) tx_fx_memset(f->s2, 0, sizeof f->s2);   /* (the 24 dB stages from rest) */
    for (sb = 0; sb < n; sb += MF_SUB) {
        unsigned len = sb + MF_SUB < n ? MF_SUB : n - sb;
        int32_t w2 = f->w2, w21 = s24 ? (w2 < 32768 - MF_FADE ? w2 + MF_FADE : 32768) : (w2 > MF_FADE ? w2 - MF_FADE : 0);
        for (e = 0; e < 2u; e++) {
            int32_t w = f->w[e], w1 = act[e] ? (w < 32768 - MF_FADE ? w + MF_FADE : 32768) : (w > MF_FADE ? w - MF_FADE : 0);
            mf_coef_t c, c2;
            if (!w && !w1) continue;
            f->pos[e] += (tgt[e] - f->pos[e]) >> 3;
            { int32_t g = mf_g(f->pos[e]); mf_coef(&c, g, k); if (w2 || w21) mf_coef(&c2, g, MF_K0); }
            mf_run(L + sb, R + sb, len, f->s[e], &c, e == 0u, w, (w1 - w) / (int32_t)MF_SUB);
            if (w2 || w21) {                                /* 24 dB: the second stage (its share x the edge's) */
                int32_t v0 = (int32_t)(((int64_t)w2 * w) >> 15), v1 = (int32_t)(((int64_t)w21 * w1) >> 15);
                mf_run(L + sb, R + sb, len, f->s2[e], &c2, e == 0u, v0, (v1 - v0) / (int32_t)MF_SUB);
            }
            f->w[e] = w1;
        }
        f->w2 = w21;
    }
}
