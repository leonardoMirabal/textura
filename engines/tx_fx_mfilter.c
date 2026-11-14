/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura FILTER (the mix's, right after the DRIVE): a high-pass then a low-pass in series on the stereo pair, each
 * a real filter over its whole knob. HIGH PASS (K1) and LOW PASS (K2), 20 Hz .. 20 kHz
 * (log, the filter tables' law: TX_FX_FL_G), -3 dB where the knob shows at every step; each is off at its open end
 * (HIGH PASS 0, LOW PASS 127): the defaults, the mix untouched. At the other end each is still a filter (HIGH PASS 127
 * a high-pass at 20 kHz, LOW PASS 0 a low-pass at 20 Hz): nothing fades to silence.
 * RESONANCE is shared and gentle (Q 0.707 .. ~4 at both edges); SLOPE 12, 18 or 24 dB an octave (24: a second stage
 * at each edge; 18: a one-pole stage at each edge, 6 dB; either way the resonance peaks once). At RESONANCE 0 each
 * SLOPE is a Butterworth edge (18: the first stage Q 1; 24: Q 0.541 then 1.307), so each edge is -3 dB where its knob
 * shows at every SLOPE. SLOPE's values keep 24 dB at 1 (what scenes saved before 18 dB came) and add 18 dB at 2: the
 * panel steps them in steepness order (params.c p_ord).
 *
 * A bell, the classic high-pass into low-pass: the edges met make a band at their place, -6 dB at its peak at every
 * SLOPE (RESONANCE lifts it, both edges peaking there: capped at ~+11.5 dB, mf_kmin); crossed (HIGH PASS above LOW
 * PASS) each edge stays where its knob is and the band closes the further they cross (12 dB: ~-11 dB 5 steps across,
 * ~-60 dB 35 steps), no jump as the knobs pass each other.
 *
 * Each edge is a TPT state-variable stage (the two integrators trapezoidal, solved at once), stable at any setting. The cutoffs glide (one pole, ~6 ms, coefficients every 32 samples:
 * no zipper, the matrix's steps smoothed); an edge turning on or off, and the 24 dB and 18 dB stages, cross-fade in 46 ms with
 * the signal before them (no click); a stage that starts again starts from rest. Each integrator steps one LSB toward 0
 * every 32 samples (far under the noise floor), so the rounding's dead band (a low cutoff's DC held) cannot outlast the sound: silence
 * in ends in exact zeros out. Precision: Q21 inside (the mix's Q23
 * / 4; the input held to 16x full scale), its products 64-bit (the FM-1's 64-bit multiply checked bit-exact against the host).
 * Both edges off and faded out: returns at once, no sample changed (the defaults' renders are the mix's). In XIP
 * flash on the FM-1, as the rest of the bus chain (core 1 runs it). */
#include "tx_fx.h"

const tx_param_t tx_fx_mfilter_params[TX_MF_NP] = {
    {"HIGH PASS", 127, 0, TX_TEXT("a high-pass, the band's low edge: off (0), 20 Hz .. 20 kHz (log)")},
    {"LOW PASS", 127, 127, TX_TEXT("a low-pass after it, the band's high edge: 20 Hz .. 20 kHz (log), off (127); at HIGH PASS: a band (a bell), below it: closing")},
    {"RESONANCE", 127, 0, TX_TEXT("a peak at both edges, gentle: Q 0.707 .. ~4")},
    {"SLOPE", TX_MF_SN - 1, 0, TX_TEXT("the edges' steepness: 0 12 dB / octave | 1 24 dB / octave | 2 18 dB / octave")},
};

#define MF_SUB 32u                  /* the coefficients' step, samples */
#define MF_FADE 512                 /* a stage's fade a step (Q15): 64 steps, 46 ms */
#define MF_K24 12841549             /* k = 0.765 (Q 1.307), Q24: the 24 dB second stage's (Butterworth with the first) */

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
    return e * (TX_FX_US_MF_STAGE + (f->p[TX_MF_SLOPE] == TX_MF_S24 || f->w2 ? TX_FX_US_MF_STAGE : 0u)
                + (f->p[TX_MF_SLOPE] == TX_MF_S18 || f->w3 ? TX_FX_US_MF_POLE : 0u));
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

/* the 18 dB one-pole stage (TPT: G = g / (1 + g), Q24), n samples of both sides in place (Q23), its share w0 ..
 * w0 + n dw against its input; s its states (Q21), each stepped one LSB toward 0 a run as the SVF's */
TX_INLINE int32_t mf_pole(int32_t v, int32_t *s, int32_t G, const int hp)
{
    int32_t d = (int32_t)(((int64_t)(v - *s) * G + (1 << 23)) >> 24), y = d + *s;   /* (rounded: no DC creep) */
    *s = y + d;
    return hp ? v - y : y;
}
static void mf_run1(int32_t *L, int32_t *R, unsigned n, int32_t *s, int32_t G, int hp, int32_t w0, int32_t dw)
{
    int32_t l = s[0], r = s[1], w = w0;
    unsigned j;
    for (j = 0; j < n; j++) {
        int32_t xl = L[j], xr = R[j], yl = mf_pole(mf_in(xl), &l, G, hp) * 4, yr = mf_pole(mf_in(xr), &r, G, hp) * 4;
        if (dw || w != 32768) { w += dw; yl = xl + mq15(yl - xl, w); yr = xr + mq15(yr - xr, w); }
        L[j] = yl; R[j] = yr;
    }
    l = mf_cl(l); r = mf_cl(r);
    l -= (l > 0) - (l < 0); r -= (r > 0) - (r < 0);
    s[0] = l; s[1] = r;
}

/* the edges near each other: both peak at one place and their peaks multiply (RESONANCE 127 met: +24 dB at 12 dB, +28
 * at 24 dB). A floor under the first stage's k by the edges' distance (pos[1] - pos[0]) caps it: the meeting point
 * +11.5 dB at every SLOPE (crossed: the same), rising to what the edges give alone 48 steps apart (12 dB +12.0, 24 dB
 * +14.6; 18 dB under +11.5 anyway from 12 steps on). Where they peak lower anyway the floor is RESONANCE 127's own k
 * (16317: no floor), and under the floor RESONANCE is as it was. k in Q16, from 12 steps crossed to 48 apart every 2
 * steps (fitted on the filter itself at RESONANCE 127, the edges around step 64: the peak barely moves with the
 * place); 12 dB, 18 dB, 24 dB */
#define MF_KD0 (12 * 516)
#define MF_KDS (2 * 516)
#define MF_KDN 31
static const uint32_t MF_KMIN[3][MF_KDN] = {
    {16317, 16317, 16317, 21621, 27726, 31518, 33819, 34889, 34909, 33863, 31640, 28100, 25210, 23274, 21867, 20824,
     20044, 19422, 18916, 18478, 18214, 17964, 17723, 17534, 17409, 17314, 17151, 17027, 17008, 16933, 16317},
    {16317, 16317, 16317, 16317, 16317, 21216, 23923, 24906, 24432, 22387, 19119, 17069, 16317, 16317, 16317, 16317,
     16317, 16317, 16317, 16317, 16317, 16317, 16317, 16317, 16317, 16317, 16317, 16317, 16317, 16317, 16317},
    {16317, 16317, 16317, 23528, 32589, 39165, 44190, 47591, 49704, 50547, 50133, 48417, 45334, 40615, 34748, 30656,
     27788, 25754, 24174, 22913, 21878, 21035, 20257, 19595, 19094, 18629, 18213, 17752, 17360, 17056, 16317},
};
static int32_t mf_kmin(unsigned si, int32_t d)              /* the k floor (Q24) at the edges' distance d; 0: none */
{
    int32_t x = d + MF_KD0, i, a, b;
    if (x <= 0 || x >= (MF_KDN - 1) * MF_KDS) return 0;
    i = x / MF_KDS; x -= i * MF_KDS;
    a = (int32_t)MF_KMIN[si][i]; b = (int32_t)MF_KMIN[si][i + 1];
    return (a + (b - a) * x / MF_KDS) << 8;                 /* (|b - a| < 2^17, x < 2^11: 32-bit) */
}
static int32_t mf_g(int32_t pos)                            /* g at a position 0 .. 65535 (Q24) */
{
    uint32_t i = (uint32_t)pos >> 8;
    int32_t fr = pos & 255;
    return (int32_t)TX_FX_FL_G[i] + (((((int32_t)TX_FX_FL_G[i + 1] - (int32_t)TX_FX_FL_G[i]) >> 4) * fr) >> 4);   /* (a step
                                                             * near 20 kHz is up to 2^25: 32-bit) */
}

void tx_fx_mfilter_process(tx_fx_mfilter_t *f, int32_t *L, int32_t *R, unsigned n)
{
    int32_t tgt[2], k = (int32_t)TX_FX_FL_K[(f->p[TX_MF_RESO] * 66u) >> 7];   /* (Q 0.707 .. ~4) */
    int32_t kf = (int32_t)TX_FX_FL_K[65], ks;           /* (RESONANCE 127's k) */
    int act[2], s24 = f->p[TX_MF_SLOPE] == TX_MF_S24, s18 = f->p[TX_MF_SLOPE] == TX_MF_S18;
    unsigned si = s18 ? 1u : s24 ? 2u : 0u;
    unsigned sb, e;
    act[0] = f->p[TX_MF_LOW] > 0; act[1] = f->p[TX_MF_HIGH] < 127;
    if (!act[0] && !act[1] && !f->w[0] && !f->w[1]) {  /* (off: untouched) */
        f->w2 = s24 ? 32768 : 0; f->w3 = s18 ? 32768 : 0;
        return;
    }
    /* the first stage's k at 18 and 24 dB: RESONANCE 0 the Butterworth's (18: Q 1 with the one-pole; 24: Q 0.541 with
     * the second stage's 1.307), so each edge is -3 dB where its knob shows at every SLOPE; RESONANCE 127 the same Q ~4 */
    ks = s18 ? kf + (int32_t)(((int64_t)(k - kf) * 10548) >> 14)            /* (x 0.6438: 1.414 -> 1.0) */
       : s24 ? kf + (int32_t)(((int64_t)(k - kf) * 22494) >> 14) : k;       /* (x 1.3729: 1.414 -> 1.848) */
    tgt[0] = f->p[TX_MF_LOW] * 516; tgt[1] = f->p[TX_MF_HIGH] * 516;    /* (crossed: each where it is, the band closes) */
    for (e = 0; e < 2u; e++)
        if (act[e] && !f->w[e]) {                           /* an edge comes on: from rest, at its place */
            f->pos[e] = tgt[e];
            tx_fx_memset(f->s[e], 0, sizeof f->s[e]);
            tx_fx_memset(f->s2[e], 0, sizeof f->s2[e]);
            f->s3[e][0] = f->s3[e][1] = 0;
        }
    if (s24 && !f->w2) tx_fx_memset(f->s2, 0, sizeof f->s2);   /* (the 24 dB stages from rest) */
    if (s18 && !f->w3) tx_fx_memset(f->s3, 0, sizeof f->s3);   /* (the 18 dB stages from rest) */
    for (sb = 0; sb < n; sb += MF_SUB) {
        unsigned len = sb + MF_SUB < n ? MF_SUB : n - sb;
        int32_t w2 = f->w2, w21 = s24 ? (w2 < 32768 - MF_FADE ? w2 + MF_FADE : 32768) : (w2 > MF_FADE ? w2 - MF_FADE : 0);
        int32_t w3 = f->w3, w31 = s18 ? (w3 < 32768 - MF_FADE ? w3 + MF_FADE : 32768) : (w3 > MF_FADE ? w3 - MF_FADE : 0);
        for (e = 0; e < 2u; e++) {
            int32_t w = f->w[e], w1 = act[e] ? (w < 32768 - MF_FADE ? w + MF_FADE : 32768) : (w > MF_FADE ? w - MF_FADE : 0);
            mf_coef_t c, c2;
            int32_t g, kk;
            if (!w && !w1) continue;
            f->pos[e] += (tgt[e] - f->pos[e]) >> 3;
            g = mf_g(f->pos[e]);
            kk = ks;
            if (act[0] && act[1]) { int32_t m = mf_kmin(si, f->pos[1] - f->pos[0]); if (m > kk) kk = m; }   /* (both on: near, capped) */
            mf_coef(&c, g, kk);
            if (w2 || w21) mf_coef(&c2, g, MF_K24);
            mf_run(L + sb, R + sb, len, f->s[e], &c, e == 0u, w, (w1 - w) / (int32_t)MF_SUB);
            if (w2 || w21) {                                /* 24 dB: the second stage (its share x the edge's) */
                int32_t v0 = (int32_t)(((int64_t)w2 * w) >> 15), v1 = (int32_t)(((int64_t)w21 * w1) >> 15);
                mf_run(L + sb, R + sb, len, f->s2[e], &c2, e == 0u, v0, (v1 - v0) / (int32_t)MF_SUB);
            }
            if (w3 || w31) {                                /* 18 dB: the one-pole stage (its share x the edge's) */
                int32_t v0 = (int32_t)(((int64_t)w3 * w) >> 15), v1 = (int32_t)(((int64_t)w31 * w1) >> 15);
                int32_t G = (int32_t)(((int64_t)g * mf_recip27(65536u + ((uint32_t)g >> 8))) >> 27);   /* (Q24) */
                mf_run1(L + sb, R + sb, len, f->s3[e], G, e == 0u, v0, (v1 - v0) / (int32_t)MF_SUB);
            }
            f->w[e] = w1;
        }
        f->w2 = w21; f->w3 = w31;
    }
}
