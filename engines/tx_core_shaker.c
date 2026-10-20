/* SPDX-License-Identifier: GPL-3.0-only AND MIT */
/***************************************************/
/*! \class Shakers
    \brief PhISEM and PhOLIES class.

    PhISEM (Physically Informed Stochastic Event Modeling) is an
    algorithmic approach for simulating collisions of multiple
    independent sound producing objects.  This class is a meta-model
    that can simulate a Maraca, Sekere, Cabasa, Bamboo Wind Chimes,
    Water Drops, Tambourine, Sleighbells, and a Guiro.

    PhOLIES (Physically-Oriented Library of Imitated Environmental
    Sounds) is a similar approach for the synthesis of environmental
    sounds.  This class implements simulations of breaking sticks,
    crunchy snow (or not), a wrench, sandpaper, and more.

    by Perry R. Cook with updates by Gary Scavone, 1995--2023.
*/
/***************************************************/
/* The Synthesis ToolKit in C++ (STK)

   Copyright (c) 1995-2023 Perry R. Cook and Gary P. Scavone

   Permission is hereby granted, free of charge, to any person obtaining
   a copy of this software and associated documentation files (the
   "Software"), to deal in the Software without restriction, including
   without limitation the rights to use, copy, modify, merge, publish,
   distribute, sublicense, and/or sell copies of the Software, and to
   permit persons to whom the Software is furnished to do so, subject to
   the following conditions:

   The above copyright notice and this permission notice shall be
   included in all copies or substantial portions of the Software.

   Any person wishing to distribute modifications to the Software is
   asked to send the modifications to the original developer so that they
   can be incorporated into the canonical version.  This is, however, not
   a binding provision of this license.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
   MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
   IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR
   ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
   WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */

/* Textura: an integer, instance-based port of Shakers::tick / setResonance / noteOn / the objects and decay
 * controls, for Textura's SHAKER core (tx_core_shaker.h). Changes from STK: fixed point (no float), a caller-supplied
 * PRNG, the energy decay stepped per 32 samples, unit-peak resonator input gain, re-tuning at most once per 8 samples,
 * a ring-out to exact zero instead of STK's hard stop at MIN_ENERGY. The preset numbers live in the engines. */
#include "tx_core_shaker.h"

#define SUB 32u
#define MIN_ENERGY 16777           /* STK MIN_ENERGY 0.001, Q24 */
#define SND_MAX (16 << 24)
#define Y_MAX ((4 << 14) - 1)    /* |a1 y| < 2^31 */

static void coefs(tx_shaker_res_t *r, uint32_t inc)          /* a1 = -2 r cos(theta), a2 = r^2, b0 = (1-r) 2 sin */
{
    int32_t c = tx_sin_q15(inc + 0x40000000u), sn = tx_sin_q15(inc);
    r->a1 = -((2 * r->r * c) >> 15);
    r->a2 = (r->r * r->r) >> 14;
    r->b0 = (((16384 - r->r) * 2) * sn) >> 15;
    if (r->b0 < 1) r->b0 = 1;
}

void tx_shaker_init(tx_shaker_t *s, uint32_t seed)
{
    uint8_t *b = (uint8_t *)s;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_rng_seed(&s->rng, seed);
    s->eq1 = 0; s->eq2 = -1;                                  /* STK rocks: 1, 0, -1 */
    s->sdec = 32113;                                          /* 0.98 */
    s->edec = 65536;
    s->nres = 1;
    tx_shaker_objects(s, 23u << 4, 2048);
    tx_shaker_decay(s, 45, 32113);
    tx_shaker_res(s, 0, 6460, 15270);
}

void tx_shaker_res(tx_shaker_t *s, unsigned i, uint32_t f_hz, int32_t r_q14)
{
    tx_shaker_res_t *r;
    if (i >= TX_SHAKER_RES) return;
    r = &s->res[i];
    if (f_hz > 19845u) f_hz = 19845u;
    if (f_hz < 20u) f_hz = 20u;
    r->inc = f_hz * 97391u;                                  /* 2^32 / fs = 97391.5 */
    r->r = r_q14 < 0 ? 0 : r_q14 > 16370 ? 16370 : r_q14;
    coefs(r, r->inc);
    if (i >= s->nres) s->nres = (uint8_t)(i + 1u);
    {   /* ring-out bound: a ring from the +-4.0 limit (2^16 LSB) falls under 1 LSB in ln(2^16) / (1 - r) ~ 12 / (1 - r)
         * samples; past that only a zero-input limit cycle of the rounding is left (direct form, r near 1) */
        unsigned k;
        uint32_t m = 0;
        for (k = 0; k < s->nres; k++) {
            uint32_t t = (12u << 14) / (uint32_t)(16384 - s->res[k].r);
            if (t > m) m = t;
        }
        s->tail_max = m;
    }
}

void tx_shaker_objects(tx_shaker_t *s, uint32_t n_q4, int32_t base_q11)
{
    uint32_t p, l;
    int32_t g;
    if (n_q4 < 18u) n_q4 = 18u;                              /* N >= 1.1 */
    if (n_q4 > (4096u << 4)) n_q4 = 4096u << 4;
    p = n_q4 * 4u;                                           /* N / 1024 in Q16 = N x 64 = n_q4 x 4 */
    s->p16 = p > 65536u ? 65536u : p;
    /* ln N = log2 N x ln 2: log2(n_q4) - 4, Q12 */
    l = (uint32_t)((32 << 12) - (int32_t)tx_neglog2_q12(n_q4) - (4 << 12));
    l = (l * TX_LN2_Q12) >> 12;                              /* ln N, Q12 (<= 8.3) */
    if (base_q11 > 32767) base_q11 = 32767;                 /* l x base < 2^30 */
    g = (int32_t)(((l * (uint32_t)base_q11) / n_q4) >> 8);    /* ln N x base / N: Q23 / Q4 = Q19 -> Q11 */
    s->gain = g < 1 ? 1 : g > (8 << 11) ? (8 << 11) : g;
}

void tx_shaker_decay(tx_shaker_t *s, uint32_t t60_ms, int32_t sound_q15)
{
    if (t60_ms < 2u) t60_ms = 2u;
    s->edec = tx_exp2((16 << 16) - (int32_t)(473915u / t60_ms));   /* 2^(-6.908 x 32 / (fs T60 ln 2)), Q16 */
    s->sdec = sound_q15 < 0 ? 0 : sound_q15 > 32767 ? 32767 : sound_q15;
}

void tx_shaker_shake(tx_shaker_t *s, int32_t e_q24)
{
    int32_t e = s->energy + (e_q24 > 0 ? e_q24 : 0);
    s->energy = e > TX_SHAKER_ONE || e < 0 ? TX_SHAKER_ONE : e;
}

int tx_shaker_quiet(const tx_shaker_t *s)
{
    unsigned i;
    if (s->energy || s->h1 || s->h2) return 0;
    for (i = 0; i < s->nres; i++) if (s->res[i].y1 || s->res[i].y2) return 0;
    return 1;
}

TX_AUDIO_CODE void tx_shaker_add_exc(tx_shaker_t *s, int32_t *out, int32_t *exc, unsigned n)
{
    uint32_t x32 = s->rng.s, p16 = s->p16, edec = s->edec;
    int32_t e = s->energy, snd = s->snd, sdec = s->sdec, g = s->gain, h1 = s->h1, h2 = s->h2, eq1 = s->eq1, eq2 = s->eq2;
    int32_t lastv = s->last, vary = s->vary;
    int32_t a1[TX_SHAKER_RES], a2[TX_SHAKER_RES], b0[TX_SHAKER_RES], y1[TX_SHAKER_RES], y2[TX_SHAKER_RES];
    unsigned j = 0, i, nres = s->nres, vcnt = s->vcnt;
    if (!e && tx_shaker_quiet(s)) return;
    for (i = 0; i < nres; i++) {                             /* locals: out[] stores cannot alias them */
        a1[i] = s->res[i].a1; a2[i] = s->res[i].a2; b0[i] = s->res[i].b0; y1[i] = s->res[i].y1; y2[i] = s->res[i].y2;
    }
    while (j < n) {
        unsigned end = j + SUB < n ? j + SUB : n;
        int32_t e1 = (int32_t)(((uint32_t)e >> 16) * edec + ((((uint32_t)e & 0xFFFFu) * edec) >> 16));
        int32_t estep;
        if (e1 < MIN_ENERGY) e1 = 0;                          /* the shake is over: no more collisions */
        estep = (e1 - e) / (int32_t)(end - j);
        for (; j < end; j++) {
            int32_t x = 0, acc = 0, o;
            e += estep;
            x32 ^= x32 << 13; x32 ^= x32 >> 17; x32 ^= x32 << 5;
            if (e && (x32 >> 16) < p16) {                    /* a collision: the level takes the energy */
                snd += e;
                if (snd > SND_MAX) snd = SND_MAX;
                x = ((snd >> 12) * g) >> 9;                  /* Q12 x Q11 >> 9 = Q14 */
                if (exc) exc[j] += x * 512;                  /* the raw collision impulses, Q23 */
                if (vary && vcnt >= 8u) {                    /* re-tune: f (1 + vary x noise) */
                    vcnt = 0;
                    x32 ^= x32 << 13; x32 ^= x32 >> 17; x32 ^= x32 << 5;
                    for (i = 0; i < nres; i++) {
                        const tx_shaker_res_t *r = &s->res[i];
                        int32_t nz = (int32_t)(x32 >> 16) - 32768;   /* -1 .. 1, Q15 */
                        uint32_t inc = r->inc + (uint32_t)((int32_t)(r->inc >> 15) * ((nz * vary) >> 15));
                        a1[i] = -((2 * r->r * tx_sin_q15(inc + 0x40000000u)) >> 15);
                        x32 = (x32 >> 7) | (x32 << 25);
                    }
                }
            }
            vcnt += vcnt < 255u;
            snd = (snd >> 15) * sdec + (((snd & 0x7FFF) * sdec) >> 15);   /* level x soundDecay */
            for (i = 0; i < nres; i++) {
                int32_t y = (((x >> 7) * b0[i]) >> 7) - ((a1[i] * y1[i]) >> 14) - ((a2[i] * y2[i]) >> 14);
                y = y > Y_MAX ? Y_MAX : y < -Y_MAX ? -Y_MAX : y;
                y2[i] = y1[i]; y1[i] = y;
                acc += y;
            }
            o = acc + eq1 * h1 + eq2 * h2;                   /* STK equaliser: taps 1, eq1, eq2 */
            h2 = h1; h1 = acc;
            out[j] += o * 512;                               /* Q14 -> Q23 */
            lastv = o;
        }
        e = e1;
    }
    if (e) s->tail = 0;
    else if (s->tail < s->tail_max) s->tail += n;
    if (!e) {                                                /* rung out: exact zero (integer limit cycles) */
        int32_t z = 0;
        if (s->tail >= s->tail_max)
            for (i = 0; i < nres; i++) y1[i] = y2[i] = 0;
        for (i = 0; i < nres; i++) {
            if (y1[i] > -4 && y1[i] < 4 && y2[i] > -4 && y2[i] < 4) y1[i] = y2[i] = 0;
            z |= y1[i] | y2[i];
        }
        if (!z) { h1 = h2 = 0; snd = 0; lastv = 0; }
    }
    for (i = 0; i < nres; i++) { s->res[i].a1 = a1[i]; s->res[i].y1 = y1[i]; s->res[i].y2 = y2[i]; }
    s->rng.s = x32 ? x32 : 0x9E3779B9u;
    s->energy = e; s->snd = snd; s->h1 = h1; s->h2 = h2; s->vcnt = (uint8_t)vcnt; s->last = lastv;
}

TX_AUDIO_CODE void tx_shaker_add(tx_shaker_t *s, int32_t *out, unsigned n) { tx_shaker_add_exc(s, out, 0, n); }
