/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Textura DRIVE (the mix's: a pair of instances, L and R, one set of settings): Felucca's track drive (track_dist) as Frodhes runs it, its classic FDRV
 * (fx.c fx_drive_classic: the gain law to ~18x at the top, level-neutral make-up, TONE after it: a shelf at ~1.8 kHz,
 * 0 = the highs gone, 64 = flat, 127 = +6 dB; MIX dry / driven), as one instance. TONE glides over ~23 ms, MIX and
 * on / off over ~46 ms: no zipper, no click. AMOUNT 0 = off (after its glide out): the lane untouched.
 * Precision: the drive works at Q15 (Frodhes' scale); the dry share of MIX stays at the lane's Q23.
 *
 * TYPE: 0 FDRV is that classic drive (Frodhes' later fixes in: its fractions kept, AMOUNT glides); 1 KHLON, 2 TSH8,
 * 3 OHCD, 4 BHLUE are Frodhes' four circuit TYPEs (drive.c and drv_<type>.c there: our own), one file each here
 * (tx_fx_drv_<type>.c, included below; their tables tx_drive_tables.h, from tools/gen_drive_tables.py). The framework
 * is Frodhes' drive.c as an instance: a TYPE runs once a block on the lane's signal at Q15 (in place, all wet), then
 * the dry / wet blend at Q23 (w ramping every 4 samples: MIX and on / off over ~46 ms, as the classic drive), AMOUNT 0
 * glides out as the classic drive does. A TYPE change while the drive sounds fades the one that runs out (46 ms),
 * then the new one starts from rest and fades in. TSH8's diode tables are shared (tx_drv_shared_t, 12.2 KB once):
 * TSH8 belongs to the first instance that selects it until that one selects another TYPE; another that asks for it
 * meanwhile keeps the TYPE it runs (and takes TSH8 as soon as it is free). A pair (tx_fx_drive_pair: the mix's L and
 * R) owns it as one: the first builds and swaps the tables, the second runs on them in the same block. */
#include "tx_fx.h"
#define TX_FX_DRIVE_UNIT
#include "tx_drive_tables.h"
#include "tx_fx_drv_khlon.c"
#include "tx_fx_drv_tsh8.c"
#include "tx_fx_drv_ohcd.c"
#include "tx_fx_drv_bhlue.c"

#define DR_TA 7700                  /* the shelf's one-pole: 1 - e^(-2 pi 1.8 kHz / fs), Q15 */
#define DR_S 16                     /* Frodhes' drive row 0 (the electric piano's: gain scale, bias, make-up) */
#define DR_BIAS 2400
#define DR_MU 4096

const tx_param_t tx_fx_drive_params[TX_DR_NP] = {
    {"AMOUNT", 127, 0, TX_TEXT("the drive: off (0) .. ~18x into a soft clip (level-neutral: it adds tone, not volume)")},
    {"TONE", 127, 64, TX_TEXT("after the drive: 0 the highs gone .. 64 flat .. 127 +6 dB above ~1.8 kHz")},
    {"MIX", 127, 127, TX_TEXT("dry .. wet: the sound alone (0) .. the drive alone (127)")},
    {"TYPE", TX_DRT_N - 1, 0, TX_TEXT("0 FDRV (the classic drive) | 1 KHLON (a clean-blend overdrive: AMOUNT its gain, TONE its treble) | 2 TSH8 (a mid-hump overdrive; one lane at a time) | 3 OHCD (a MOSFET overdrive) | 4 BHLUE (a two-stage soft overdrive); TONE: 64 the circuit's middle")},
};

void tx_fx_drive_init(tx_fx_drive_t *d)
{
    unsigned i;
    tx_fx_memset(d, 0, sizeof *d);
    for (i = 0; i < TX_DR_NP; i++) d->p[i] = tx_fx_drive_params[i].def;
    d->w = 32768;
    d->tg = 4096;
    d->gl = -1;
    d->own = d;
}
void tx_fx_drive_param(tx_fx_drive_t *d, unsigned id, unsigned v)
{
    if (id < TX_DR_NP) d->p[id] = (uint8_t)(v > tx_fx_drive_params[id].max ? tx_fx_drive_params[id].max : v);
}
TX_INLINE int32_t dr_cl(int32_t v) { v = v > -230000 ? v : -230000; return v < 230000 ? v : 230000; }

/* FDRV keeps its fractions (Frodhes 0.9.8 .. 0.9.10, at 1x here): a quiet note through the drive came out of a
 * staircase. The high-pass's state is in Q7 (it moved only once the input was 64 LSB away: a 41 Hz sine of 160 LSB
 * came out at H3 -13 dB re H1); the shaper's input is x g >> 6 (DR_IN more bits from the product, not (x >> 2) g >> 10);
 * the curve reads its table between entries at 16 bits; its output and both low-passes carry DR_YS more bits (64-bit
 * products); the make-up rounds once to the output. b0 is the curve at the bias at the same precision: 0 in, 0 out. */
#define DR_IN 6u                    /* the shaper's input: bits below the LSB */
#define DR_YS 4u                    /* the curve's output and the low-passes: bits below the LSB */
#define DR_DSTEP 4                  /* AMOUNT glides at most this a block (its gain, low-pass and make-up ramp over it) */
TX_INLINE int32_t dr_curve(int32_t v)                    /* tx_fx_softclip on DR_IN more bits in, DR_YS more out */
{
    uint32_t u, i;
    int32_t f;
    v = v > -(131072 << DR_IN) ? v : -(131072 << DR_IN);
    v = v < (131071 << DR_IN) ? v : (131071 << DR_IN);
    u = (uint32_t)(v + (131072 << DR_IN)); i = u >> (10u + DR_IN); f = (int32_t)(u & ((1u << (10u + DR_IN)) - 1u));
    return TX_FX_TANH[i] * (1 << DR_YS) + (((TX_FX_TANH[i + 1] - TX_FX_TANH[i]) * f) >> (10u + DR_IN - DR_YS));
}
/* one sample of the drive up to its make-up: x23 in, y (Q15) out; hp, lp1, lp2 the states (locals or the struct's) */
#define DR_CORE(x23v, y, hp, lp1, lp2, g, k, mk)                                                                    \
    do {                                                                                                            \
        int32_t x0_ = ((x23v) + 128) >> 8, d_ = x0_ - (((hp) + 64) >> 7), s_;   /* Q23 -> Q15 */                    \
        (hp) += d_;                                                                                                 \
        s_ = dr_curve((int32_t)(((int64_t)dr_cl(d_) * (g)) >> (12u - DR_IN)) + (DR_BIAS << DR_IN)) - b0;           \
        (lp1) += (int32_t)(((int64_t)(s_ - (lp1)) * (k)) >> 15);                                                    \
        (lp2) += (int32_t)(((int64_t)((lp1) - (lp2)) * (k)) >> 15);                                                 \
        (y) = (int32_t)(((int64_t)(lp2) * (mk) + (1 << (14u + DR_YS))) >> (15u + DR_YS));                          \
    } while (0)

/* the glides at rest (TONE's gain and the wet share where they go, AMOUNT not moving): each sample as the loop below
 * does then, its state in locals (Frodhes' dr_settled); shelf and full are constants at each use */
static inline __attribute__((always_inline)) void dr_settled(tx_fx_drive_t *d, int32_t *x23, unsigned n, int32_t g,
                                                             int32_t k, int32_t mk, int32_t b0, int32_t tg, int32_t w,
                                                             const int shelf, const int full)
{
    int32_t hp = d->hp, lp1 = d->lp1, lp2 = d->lp2, tlp = d->tlp;
    unsigned i;
    for (i = 0; i < n; i++) {
        int32_t y;
        DR_CORE(x23[i], y, hp, lp1, lp2, g, k, mk);
        if (shelf) {
            tlp += mulq15(y - tlp, DR_TA);
            y = tlp + (int32_t)(((int64_t)(y - tlp) * tg) >> 12);
        }
        y *= 256;
        x23[i] = full ? y : x23[i] + (int32_t)(((int64_t)(y - x23[i]) * w) >> 15);
    }
    d->hp = hp; d->lp1 = lp1; d->lp2 = lp2; d->tlp = tlp;
}

/* the classic drive (TYPE 0); amt: AMOUNT, or 0 while it fades out for a TYPE change */
static void dr_classic(tx_fx_drive_t *d, int32_t *x23, unsigned n, int32_t amt)
{
    int32_t a = amt ? amt : d->la, tv = d->p[TX_DR_TONE], mv = d->p[TX_DR_MIX], g, k, mk, b0;
    int32_t wt = !amt ? 0 : mv >= 127 ? 32768 : mv * 258;      /* the wet share it goes to (AMOUNT 0: 0) */
    int32_t tt = tv >= 64 ? 4096 + (((tv - 64) * 4161) >> 6) : tv * 64;   /* the shelf's gain: 0 .. 1 .. 2 (Q12) */
    int32_t gq, kq, mq, dgq = 0, dkq = 0, dmq = 0;            /* the ramp (Q8) while AMOUNT moves */
    unsigned i;
    if (!d->on) {                                               /* off: the lane untouched */
        if (!amt) return;
        d->on = 1; d->w = 0; d->hp = d->lp1 = d->lp2 = 0; d->ton = 0; d->tg = 4096;   /* on: from dry, a 46 ms glide */
        d->gl = -1;
    }
    if (d->gl >= 0 && a != d->dl) a = d->dl + clampi(a - d->dl, -DR_DSTEP, DR_DSTEP);   /* (Frodhes 0.9.6, #5: AMOUNT
                                                 * stepped once a block, a zipper; the make-up matches the gain per
                                                 * AMOUNT only, so AMOUNT itself glides, its values ramp over the block) */
    d->dl = (uint8_t)a;
    d->la = (uint8_t)a;
    g = 4096 + ((a * a * 2 * DR_S) >> 4);
    if (a > 16) g = 4096 + ((g - 4096) * (95 + a)) / 111;     /* (<= 32258 x 222: 32-bit) the gain to ~18x */
    k = 32000 - a * 95;
    mk = ((30000 - a * 120) * DR_MU) >> 12;
    mk = (mk * (int32_t)TX_FX_DRV_LVL[a]) >> 15;
    mk = (mk * 39000) >> 15;
    b0 = dr_curve(DR_BIAS << DR_IN);
    if (tt > 8192) tt = 8192;
    gq = g << 8; kq = k << 8; mq = mk << 8;
    if (d->gl >= 0 && (d->gl != g || d->kl != k || d->ml != mk)) {   /* AMOUNT moved: from the last block's, over this */
        gq = d->gl << 8; kq = d->kl << 8; mq = d->ml << 8;
        dgq = ((g - d->gl) << 8) / (int32_t)n; dkq = ((k - d->kl) << 8) / (int32_t)n; dmq = ((mk - d->ml) << 8) / (int32_t)n;
    }
    d->gl = g; d->kl = k; d->ml = mk;
    if (!(dgq | dkq | dmq) && d->w == wt && wt && (d->ton ? d->tg == tt && tt != 4096 : tt == 4096)) {   /* settled */
        if (!d->ton) { if (wt == 32768) dr_settled(d, x23, n, g, k, mk, b0, tt, wt, 0, 1); else dr_settled(d, x23, n, g, k, mk, b0, tt, wt, 0, 0); }
        else if (wt == 32768) dr_settled(d, x23, n, g, k, mk, b0, tt, wt, 1, 1);
        else dr_settled(d, x23, n, g, k, mk, b0, tt, wt, 1, 0);
        return;
    }
    for (i = 0; i < n; i++) {
        int32_t y;
        gq += dgq; kq += dkq; mq += dmq;
        DR_CORE(x23[i], y, d->hp, d->lp1, d->lp2, gq >> 8, kq >> 8, mq >> 8);
        if (d->ton || tt != 4096) {                             /* TONE: lows + the highs x gain */
            if (!d->ton) { d->tlp = y; d->ton = 1; }
            d->tg += d->tg < tt ? (tt - d->tg > 8 ? 8 : tt - d->tg) : -(d->tg - tt > 8 ? 8 : d->tg - tt);
            d->tlp += mulq15(y - d->tlp, DR_TA);
            y = d->tlp + (int32_t)(((int64_t)(y - d->tlp) * d->tg) >> 12);
            if (d->tg == 4096 && tt == 4096) d->ton = 0;
        }
        if (d->w != wt) d->w += d->w < wt ? (wt - d->w > 16 ? 16 : wt - d->w) : -(d->w - wt > 16 ? 16 : d->w - wt);
        y *= 256;                                                /* Q15 -> Q23; the dry share at full precision */
        x23[i] = d->w == 32768 ? y : x23[i] + (int32_t)(((int64_t)(y - x23[i]) * d->w) >> 15);
    }
    if (!wt && !d->w) d->on = 0;                                /* AMOUNT 0: glided out, off again */
}

/* ---- the TYPEs (1..): Frodhes' drive framework, as an instance */
#define DRV_US_CORE 4u             /* the framework around a TYPE: Q23 -> Q15 in, the tests, the dispatch (~2 a sample) */
#define DRV_US_BLEND 16u           /* the Q15 -> Q23 out with the dry / wet blend (all wet: ~6 us of it) */
static const uint16_t DRV_US[TX_DRT_N] = {0, DRV_US_KHLON, DRV_US_TSH8, DRV_US_OHCD, DRV_US_BHLUE};
static void drv_reset(tx_fx_drive_t *d, unsigned t)
{
    switch (t) {
    case TX_DRT_KHLON: drv_khlon_reset(&d->u.kh); break;
    case TX_DRT_TSH8: drv_tsh8_reset(&d->u.ts); break;
    case TX_DRT_OHCD: drv_ohcd_reset(&d->u.oh); break;
    case TX_DRT_BHLUE: drv_bhlue_reset(&d->u.bh); break;
    default: break;
    }
}
/* the TYPE that runs over q (n even, >= 4), in place; 1: not ready (TSH8 built its first table: q untouched) */
static int drv_run(tx_fx_drive_t *d, tx_drv_scr_t *w, int32_t *q, unsigned n, int32_t a, int32_t t)
{
    switch (d->cur) {
    case TX_DRT_KHLON: drv_khlon_run(&d->u.kh, w, q, q, n, a, t); return 0;
    case TX_DRT_TSH8: return drv_tsh8_run(&d->u.ts, d->sh, w, q, q, n, a, t, !d->fol);
    case TX_DRT_OHCD: drv_ohcd_run(&d->u.oh, q, q, n, a, t); return 0;
    case TX_DRT_BHLUE: drv_bhlue_run(&d->u.bh, q, q, n, a, t); return 0;
    default: return 1;
    }
}
/* the TYPE that runs (d->cur >= 1) on x23; sw 0: fading out (a TYPE change). Off (on 0) costs nothing. (TX_SHARE:
 * core 1's twin of tx_fx_drive_run calls this copy) */
TX_SHARE void dr_types(tx_fx_drive_t *d, tx_drv_scr_t *w, int32_t *x23, unsigned n, int sw)
{
    int32_t amt = sw ? d->p[TX_DR_AMOUNT] : 0, mv = d->p[TX_DR_MIX], a, w0, w1, d4, *q = w->q;
    int32_t wt = !amt ? 0 : mv >= 127 ? 32768 : (mv * 258 + 128) & ~255;   /* (multiples of 256: the ramp is exact) */
    unsigned i;
    if (!d->on) {                                         /* off: the lane untouched; on: from rest, from dry */
        if (!wt) return;
        d->on = 1; d->w = 0; drv_reset(d, d->cur);
    }
    if (n > TX_DRV_BLK || n < 4u) return;                 /* (never: the lanes run whole blocks) */
    a = amt ? amt : d->la;
    d->la = (uint8_t)a;
    for (i = 0; i < n; i++) {                             /* Q23 -> Q15, held to 8 x full scale */
        int32_t v = (x23[i] + 128) >> 8;
        q[i] = v < -(1 << 18) ? -(1 << 18) : v > (1 << 18) ? 1 << 18 : v;
    }
    if (drv_run(d, w, q, n & ~1u, a, d->p[TX_DR_TONE])) return;   /* (not ready: dry, w held at 0) */
    w0 = d->w; w1 = w0 < wt ? (wt - w0 > (int32_t)(16u * n) ? w0 + (int32_t)(16u * n) : wt)
                            : (w0 - wt > (int32_t)(16u * n) ? w0 - (int32_t)(16u * n) : wt);
    if (w0 == 32768 && w1 == 32768) {
        for (i = 0; i < (n & ~1u); i++) x23[i] = q[i] * 256;
    } else {                                              /* the dry share at the lane's Q23 */
        int32_t w = w0;
        d4 = (w1 - w0) / (int32_t)(n >> 2);
        for (i = 0; i < (n & ~1u); i++) {
            x23[i] += (int32_t)(((int64_t)(q[i] * 256 - x23[i]) * w) >> 15);
            if ((i & 3u) == 3u) w += d4;
        }
    }
    d->w = w1;
    if (!wt && !w1) d->on = 0;                            /* glided out: off again */
}
/* the TYPE asked for (t) is not the one that runs: it starts at once while the drive is silent, else the one that
 * runs fades out first (sw 0) and t starts from rest in the block after; TSH8 taken by another lane: the running
 * TYPE goes on (sw 1) */
static int dr_switch(tx_fx_drive_t *d, unsigned t)
{
    tx_drv_shared_t *sh = d->sh;
    if (t >= TX_DRT_N || !sh) return 1;
    if (t == TX_DRT_TSH8 && sh->ts_own && sh->ts_own != d->own) return 1;
    if (d->on) return 0;
    if (d->cur == TX_DRT_TSH8 && sh->ts_own == d->own && !d->fol) sh->ts_own = 0;
    d->cur = (uint8_t)t;
    if (t == TX_DRT_TSH8) sh->ts_own = d->own;
    if (t) drv_reset(d, t);
    else { d->ton = 0; d->tg = 4096; }                    /* (the classic shelf at rest, as its own off leaves it) */
    return 1;
}
/* the two halves of a block: prep (a TYPE change: TSH8's owner, lane order) on one core for every lane first, then
 * run on the core that renders the lane, with that core's scratch (w; 0: the shared one) */
TX_AUDIO_CODE void tx_fx_drive_prep(tx_fx_drive_t *d)
{
    d->sw = 1;
    if (d->p[TX_DR_TYPE] != d->cur) d->sw = (uint8_t)TX_FAR(dr_switch)(d, d->p[TX_DR_TYPE]);
}
/* the rest (Frodhes 05c6e2b7, 935763a5, for every TYPE here): on zero input the rounding inside a TYPE's recursions
 * (every >> floors) holds fixed points that are not 0: a DC of 1 .. 75 LSB out after the music, for ever (KHLON's
 * coupling capacitor, FDRV's low-passes, a biquad's last LSB). A block of zero input whose output is one constant, not
 * 0, is that fixed point: the TYPE's states go to rest (exact 0 from the next block) and that next block ramps from
 * the constant to 0 (dropped in one sample, the DC was a tick after every last note). Once a block, no per-sample work
 * while anything sounds (the scans only on a block whose first, middle and last inputs are 0) */
static void dr_rest(tx_fx_drive_t *d)
{
    if (!d->cur) d->hp = d->lp1 = d->lp2 = d->tlp = 0;
    else drv_reset(d, d->cur);
}
TX_AUDIO_CODE void tx_fx_drive_run(tx_fx_drive_t *d, int32_t *x23, unsigned n, tx_drv_scr_t *w)
{   /* (in XIP, as the TYPEs: the mix's DRIVE runs with the bus on core 1, lanes.c LN_PIPE) */
    unsigned i, z = n && !(x23[0] | x23[n >> 1] | x23[n - 1]);
    for (i = 1; z && i < n; i++) z = !x23[i];
    if (!d->cur) dr_classic(d, x23, n, d->sw ? d->p[TX_DR_AMOUNT] : 0);
    else TX_FAR(dr_types)(d, w ? w : &d->sh->scr, x23, n, d->sw);
    if (d->rl) {                                          /* the block after a rest: the constant ramps out */
        for (i = 0; i < n; i++) x23[i] += d->rl * (int32_t)(n - 1u - i) / (int32_t)n;   /* (|rl| < 2^23 / 256) */
        d->rl = 0;
    } else if (z && d->on && x23[0]) {
        int32_t c = x23[0];
        for (i = 1; i < n && x23[i] == c; i++) {}
        if (i == n && c > -(1 << 15) && c < (1 << 15)) { dr_rest(d); d->rl = c; }   /* (a held DC, at most 128 LSB) */
    }
}
TX_AUDIO_CODE void tx_fx_drive_process(tx_fx_drive_t *d, int32_t *x23, unsigned n)
{
    TX_FAR(tx_fx_drive_prep)(d);                          /* (in flash) */
    tx_fx_drive_run(d, x23, n, 0);
}
void tx_fx_drive_share(tx_fx_drive_t *d, tx_drv_shared_t *sh) { d->sh = sh; }
void tx_fx_drive_pair(tx_fx_drive_t *second, const tx_fx_drive_t *first) { second->own = first; second->fol = 1; }
void tx_fx_drive_shared_init(tx_drv_shared_t *sh)
{
    tx_fx_memset(sh, 0, sizeof *sh);
    sh->ts_trow = -1; sh->ts_bpos = -1;
}
/* the FM-1 cost of this DRIVE (us a block; 0 off): FDRV TX_FX_US_FDRV; a TYPE its count (Frodhes, compiled)
 * + the framework, + the blend while it blends; while a TYPE change waits, the dearer of the two */
uint32_t tx_fx_drive_us(const tx_fx_drive_t *d)
{
    unsigned t = d->p[TX_DR_TYPE], c = d->cur;
    uint32_t us, ut;
    if (!d->on && !d->p[TX_DR_AMOUNT]) return 0;
    us = !c ? TX_FX_US_FDRV : DRV_US_CORE + DRV_US[c] + DRV_US_BLEND;
    if (t != c && t < TX_DRT_N && d->sh && !(t == TX_DRT_TSH8 && d->sh->ts_own && d->sh->ts_own != d->own)) {
        ut = !t ? TX_FX_US_FDRV : DRV_US_CORE + DRV_US[t] + DRV_US_BLEND;
        if (ut > us) us = ut;
    }
    return us;
}
