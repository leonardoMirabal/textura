/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura COMP core: the master bus compressor (tx_core_comp.h). */
#include "tx_core_comp.h"
#include "tx_core_math.h"

#define CB 32u                     /* the control block, frames */
#define DB(x10) ((x10) * 13606 / 100)   /* dB x 10 -> power log2 Q12 */
#define KNEE DB(120)               /* 12 dB */
#define MK_MAX DB(60)              /* the makeup's most: +6 dB */
/* the envelopes' one-pole coefficients a control block (0.726 ms), Q16: 1 - exp(-0.726 ms / tau) */
#define K_FA 2335                  /* fast attack, 20 ms */
#define K_FR 316                   /* fast release, 150 ms */
#define K_SA 119                   /* slow attack, 400 ms */
#define K_SR 32                    /* slow release, 1.5 s */

const tx_param_t tx_comp_params[TX_CP_NP] = {
    {"COMP", 127, 40, "the bus compressor: off (0) .. 3 : 1 from -30 dBFS (127); quiet scenes lifted up to +6 dB"},
};

/* the gain computer: the reduction (power log2 Q12, >= 0) at level lp, over a soft knee */
static int32_t gc(const tx_comp_t *c, int32_t lp)
{
    int32_t over = lp - c->t, x;
    if (over <= -KNEE / 2) return 0;
    if (over >= KNEE / 2) return (over >> 4) * c->s >> 8;
    x = over + KNEE / 2;                                      /* 0 .. KNEE: x^2 < 2^28 */
    return ((x * x) / (2 * KNEE) >> 4) * c->s >> 8;
}

void tx_comp_init(tx_comp_t *c)
{
    tx_zero(c, sizeof *c);
    c->g = 16384;
}

void tx_comp_amount(tx_comp_t *c, unsigned a)
{
    int32_t mk;
    if (a > 127u) a = 127u;
    c->amt = (uint8_t)a;
    if (!a) return;                                           /* (the gain glides back to 1, then the bypass) */
    c->s = (int32_t)(4096u * 2u * a / (127u + 2u * a));
    c->t = -DB(140) - DB(160) * (int32_t)a / 127;
    mk = gc(c, -DB(120));
    c->mk = mk > MK_MAX ? MK_MAX : mk;
}

static int32_t env(int32_t e, int32_t p, int32_t ka, int32_t kr)
{
    return e + ((((p - e) >> 10) * (p > e ? ka : kr)) >> 6);
}

TX_AUDIO_CODE void tx_comp_process(tx_comp_t *c, int32_t *L, int32_t *R, unsigned n)
{
    unsigned j0, j, m;
    if (!c->amt && c->g == 16384) { c->on = 0; c->gr = 0; return; }   /* off: the samples untouched */
    for (j0 = 0; j0 < n; j0 += m) {
        int32_t g0 = c->g, gt, dg, g, p = 0, det;
        m = n - j0 < CB ? n - j0 : CB;
        for (j = j0; j < j0 + m; j++) {                       /* the block's power: the louder side, squared */
            int32_t a = L[j] < 0 ? -L[j] : L[j], b = R[j] < 0 ? -R[j] : R[j], v = (a > b ? a : b) >> 10;
            v = v > 16383 ? 16383 : v;                        /* (2 full scale: v^2 < 2^28) */
            p += (v * v) >> 5;
        }
        if (m != CB) p = p / (int32_t)m * (int32_t)CB;
        if (!c->on) { c->ef = c->es = p; c->on = 1; }         /* (from the bypass: no stale level) */
        c->ef = env(c->ef, p, K_FA, K_FR);
        c->es = env(c->es, p, K_SA, K_SR);
        det = c->ef > c->es ? c->ef : c->es;
        det = det < 1 ? 1 : det > (1 << 26) - 1 ? (1 << 26) - 1 : det;
        if (c->amt) {
            int32_t lp = -(int32_t)tx_neglog2_q12((uint32_t)det << 6);   /* power log2 rel. full scale, Q12 */
            c->gr = gc(c, lp);
            gt = (int32_t)tx_exp2(((c->mk - c->gr) / 2) * 16 + (14 << 16));   /* the amplitude: half the power log2 */
        } else { c->gr = 0; gt = 16384; }
        g = g0 + ((gt - g0) >> 3);                            /* the glide: ~6 ms */
        if (g - gt < 8 && gt - g < 8) g = gt;
        c->g = g;
        dg = (g - g0) / (int32_t)m;
        for (j = j0, g = g0; j < j0 + m; j++) {                /* the gain ramped across the block */
            int32_t l = L[j], r = R[j];
            g += dg;
            if (j == j0 + m - 1u) g = c->g;
            l = l > (1 << 28) ? (1 << 28) : l < -(1 << 28) ? -(1 << 28) : l;
            r = r > (1 << 28) ? (1 << 28) : r < -(1 << 28) ? -(1 << 28) : r;
            L[j] = (l >> 14) * g + (int32_t)((((uint32_t)l & 16383u) * (uint32_t)g) >> 14);
            R[j] = (r >> 14) * g + (int32_t)((((uint32_t)r & 16383u) * (uint32_t)g) >> 14);
        }
    }
}
