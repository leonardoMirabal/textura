/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura CHIRP core: dispersive flexural chirps (tx_core_chirp.h). */
#include "tx_core_chirp.h"

#define L2_100 435412              /* log2(100), Q16 */
#define L2_8PI 304820              /* log2(8 pi) = 4.651 */
#define L2_K951 (-4750)            /* log2(0.951) = -0.0725 */
#define L2_FS 1011124              /* log2(44100) = 15.4285 */
#define L2_INC_HZ 1086035          /* log2(2^32 / fs) = 16.5715: inc = f x 2^this */
#define L2_FSTART 915266           /* log2(16000) = 13.966 */
#define L2_FEND 348790             /* log2(40) = 5.3219 */

static int32_t log2_q16(uint32_t x) { return (32 << 16) - (int32_t)(tx_neglog2_q12(x ? x : 1u) << 4); }

void tx_chirp_start(tx_chirp_t *c, uint32_t x_cm, uint32_t h_mm, uint32_t q, int32_t amp_q15)
{
    int32_t lx = log2_q16(x_cm) - L2_100, lkap = log2_q16(h_mm) + L2_K951, lt0, lte, la;
    uint32_t T0, Tair, Tlow;
    c->on = 0;
    if (!x_cm || !h_mm || amp_q15 <= 0) return;
    c->lk = 2 * lx - L2_8PI - lkap + L2_FS + (32 << 16);     /* log2(K fs 2^32), K = x^2 / (8 pi kappa) */
    lt0 = (c->lk - (L2_FSTART + L2_INC_HZ)) / 2;             /* T0: f(T0) = 16 kHz */
    lte = (c->lk - (L2_FEND + L2_INC_HZ)) / 2;               /* f = 40 Hz */
    T0 = lt0 < (1 << 16) ? 2u : tx_exp2(lt0);
    Tlow = lte > (24 << 16) ? 0xFFFFFFu : tx_exp2(lte);
    Tair = tx_chirp_air(x_cm);
    c->Tend = Tlow < 4u * Tair ? Tlow : 4u * Tair;          /* the flexural chirp runs on past the air wave's arrival
                                                              * (ending it there cut a ring at ~-60 dBFS: session 5's
                                                              * tails), to 40 Hz, at most 4 x the air delay */
    if (T0 + 64u >= c->Tend) return;                         /* too close: the chirp is over before it starts */
    c->T = 0;
    c->wait = T0;
    c->l0 = log2_q16((uint32_t)amp_q15) + log2_q16(T0);      /* g = amp T0 / T */
    /* loss: log2 factor = -(x / (2 Q ln 2)) sqrt(pi f / (2 kappa)) = -|latt| sqrt(f) */
    la = lx - (1 << 16) - log2_q16(q ? q : 1u) + 34656 + (108233 - (1 << 16) - lkap) / 2;   /* -log2 ln 2 = 0.5288; log2 pi */
    c->latt = la;
    c->g = 0;
    c->inc = tx_exp2(c->lk - 2 * log2_q16(T0));
    c->ph = 0;
    c->on = 1;
}

/* the increment and amplitude at T samples after the crack */
static void at(const tx_chirp_t *c, uint32_t T, uint32_t *inc, int32_t *g)
{
    int32_t lT = log2_q16(T), li = c->lk - 2 * lT, lf = li - L2_INC_HZ, att, lg;
    *inc = li >= (31 << 16) ? 0x7FFFFFFFu : tx_exp2(li);
    att = c->latt + lf / 2;                                  /* log2(|latt| sqrt f) */
    att = att > (5 << 16) ? (32 << 16) : (int32_t)tx_exp2(att + (16 << 16));   /* the loss in octaves, Q16 */
    lg = c->l0 - lT - att;
    *g = lg > (15 << 16) ? 32767 : lg < 0 ? 0 : (int32_t)tx_exp2(lg);
}

TX_AUDIO_CODE void tx_chirp_add(tx_chirp_t *c, int32_t *out, unsigned n)
{
    unsigned j = 0;
    uint32_t inc1, ph = c->ph, inc, T1;
    int32_t g1, g, gs, is;
    if (!c->on) return;
    if (c->wait >= n) { c->wait -= n; c->T += n; return; }
    j = c->wait;
    c->T += c->wait;
    c->wait = 0;
    T1 = c->T + (n - j);
    if (T1 >= c->Tend) { at(c, c->T, &inc1, &g1); g1 = 0; c->on = 0; }   /* the end: fade out in this block */
    else at(c, T1, &inc1, &g1);
    inc = c->inc; g = c->g << 8;                             /* (the level ramp in Q23: a slow swell moves a few Q15 units */
    gs = ((g1 - c->g) * 256) / (int32_t)(n - j);              /* a block, which steps at the block's start in Q15) */
    is = (int32_t)(inc1 - inc) / (int32_t)(n - j);
    for (; j < n; j++) {
        ph += inc;
        {
            int32_t sv = tx_sin_q15(ph);
            out[j] += sv * (g >> 15) + ((sv * (g & 32767)) >> 15);   /* Q15 x Q23 -> Q23: a quiet chirp fades smoothly */
        }
        inc += (uint32_t)is;
        g += gs;
    }
    c->ph = ph; c->inc = inc1; c->g = c->on ? g1 : 0; c->T = T1;
}
