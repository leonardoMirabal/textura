/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Textura DRIVE (a lane's insert): Felucca's track drive (track_dist) as Frodhes runs it, its classic FDRV
 * (fx.c fx_drive_classic: the gain law to ~18x at the top, level-neutral make-up, TONE after it: a shelf at ~1.8 kHz,
 * 0 = the highs gone, 64 = flat, 127 = +6 dB; MIX dry / driven), as one instance. TONE glides over ~23 ms, MIX and
 * on / off over ~46 ms: no zipper, no click. AMOUNT 0 = off (after its glide out): the lane untouched.
 * Precision: the drive works at Q15 (Frodhes' scale); the dry share of MIX stays at the lane's Q23. */
#include "tx_fx.h"

#define DR_TA 7700                  /* the shelf's one-pole: 1 - e^(-2 pi 1.8 kHz / fs), Q15 */
#define DR_S 16                     /* Frodhes' drive row 0 (the electric piano's: gain scale, bias, make-up) */
#define DR_BIAS 2400
#define DR_MU 4096

const tx_param_t tx_fx_drive_params[TX_DR_NP] = {
    {"AMOUNT", 127, 0, "the drive: off (0) .. ~18x into a soft clip (level-neutral: it adds tone, not volume)"},
    {"TONE", 127, 64, "after the drive: 0 the highs gone .. 64 flat .. 127 +6 dB above ~1.8 kHz"},
    {"MIX", 127, 127, "dry .. driven"},
};

void tx_fx_drive_init(tx_fx_drive_t *d)
{
    unsigned i;
    tx_fx_memset(d, 0, sizeof *d);
    for (i = 0; i < TX_DR_NP; i++) d->p[i] = tx_fx_drive_params[i].def;
    d->w = 32768;
    d->tg = 4096;
}
void tx_fx_drive_param(tx_fx_drive_t *d, unsigned id, unsigned v)
{
    if (id < TX_DR_NP) d->p[id] = (uint8_t)(v > 127u ? 127u : v);
}
static inline int32_t dr_cl(int32_t v) { v = v > -230000 ? v : -230000; return v < 230000 ? v : 230000; }

/* the glides at rest (TONE's gain and the wet share where they go): each sample as the loop below does then, its state
 * in locals (Frodhes' dr_settled); shelf and full are constants at each use */
static inline __attribute__((always_inline)) void dr_settled(tx_fx_drive_t *d, int32_t *x23, unsigned n, int32_t g,
                                                             int32_t k, int32_t mk, int32_t b0, int32_t tg, int32_t w,
                                                             const int shelf, const int full)
{
    int32_t hp = d->hp, lp1 = d->lp1, lp2 = d->lp2, tlp = d->tlp;
    unsigned i;
    for (i = 0; i < n; i++) {
        int32_t x0 = (x23[i] + 128) >> 8, x, y;
        hp += (x0 - hp + 64) >> 7;
        x = dr_cl(x0 - hp);
        y = tx_fx_softclip((int32_t)(((int64_t)(x >> 2) * g) >> 10) + DR_BIAS) - b0;
        lp1 += mulq15(y - lp1, k);
        lp2 += mulq15(lp1 - lp2, k);
        y = mulq15(lp2, mk);
        if (shelf) {
            tlp += mulq15(y - tlp, DR_TA);
            y = tlp + (int32_t)(((int64_t)(y - tlp) * tg) >> 12);
        }
        y *= 256;
        x23[i] = full ? y : x23[i] + (int32_t)(((int64_t)(y - x23[i]) * w) >> 15);
    }
    d->hp = hp; d->lp1 = lp1; d->lp2 = lp2; d->tlp = tlp;
}

TX_AUDIO_CODE void tx_fx_drive_process(tx_fx_drive_t *d, int32_t *x23, unsigned n)
{
    int32_t a = d->p[TX_DR_AMOUNT] ? d->p[TX_DR_AMOUNT] : d->la, tv = d->p[TX_DR_TONE], mv = d->p[TX_DR_MIX], g, k, mk, b0;
    int32_t wt = !d->p[TX_DR_AMOUNT] ? 0 : mv >= 127 ? 32768 : mv * 258;   /* the wet share it goes to (AMOUNT 0: 0) */
    int32_t tt = tv >= 64 ? 4096 + (((tv - 64) * 4161) >> 6) : tv * 64;   /* the shelf's gain: 0 .. 1 .. 2 (Q12) */
    unsigned i;
    if (!d->on) {                                               /* off: the lane untouched */
        if (!d->p[TX_DR_AMOUNT]) return;
        d->on = 1; d->w = 0; d->hp = d->lp1 = d->lp2 = 0; d->ton = 0; d->tg = 4096;   /* on: from dry, a 46 ms glide */
    }
    d->la = (uint8_t)a;
    g = 4096 + ((a * a * 2 * DR_S) >> 4);
    if (a > 16) g = 4096 + ((g - 4096) * (95 + a)) / 111;     /* (<= 32258 x 222: 32-bit) the gain to ~18x */
    k = 32000 - a * 95;
    mk = ((30000 - a * 120) * DR_MU) >> 12;
    mk = (mk * (int32_t)TX_FX_DRV_LVL[a]) >> 15;
    mk = (mk * 39000) >> 15;
    b0 = tx_fx_softclip(DR_BIAS);
    if (tt > 8192) tt = 8192;
    if (d->w == wt && wt && (d->ton ? d->tg == tt && tt != 4096 : tt == 4096)) {   /* settled */
        if (!d->ton) { if (wt == 32768) dr_settled(d, x23, n, g, k, mk, b0, tt, wt, 0, 1); else dr_settled(d, x23, n, g, k, mk, b0, tt, wt, 0, 0); }
        else if (wt == 32768) dr_settled(d, x23, n, g, k, mk, b0, tt, wt, 1, 1);
        else dr_settled(d, x23, n, g, k, mk, b0, tt, wt, 1, 0);
        return;
    }
    for (i = 0; i < n; i++) {
        int32_t x0 = (x23[i] + 128) >> 8, x, y;                 /* Q23 -> Q15 */
        d->hp += (x0 - d->hp + 64) >> 7;
        x = dr_cl(x0 - d->hp);
        y = tx_fx_softclip((int32_t)(((int64_t)(x >> 2) * g) >> 10) + DR_BIAS) - b0;
        d->lp1 += mulq15(y - d->lp1, k);
        d->lp2 += mulq15(d->lp1 - d->lp2, k);
        y = mulq15(d->lp2, mk);
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
