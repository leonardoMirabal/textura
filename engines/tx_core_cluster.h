/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura CLUSTER: when events come, how near each one is, and how each differs from the last (the SOURCE engine's
 * stage 1, shared by its presets; header-only, integer, a few multiplies per event and one walk step per block).
 *
 * Natural events are neither even nor plain Poisson. Three mechanisms, each a knob of a preset's character
 * (tx_clu_cfg_t, set per block from the activity, so the character moves with it: sparse individuals .. bunched ..
 * a fused cloud):
 *   COX      the immigrants' rate rides a slow Ornstein-Uhlenbeck walk in log2 (a doubly stochastic Poisson process):
 *            gusts and lulls, runs of drips, a slope that quiets and starts again. Mean-preserving: the walk's
 *            log-normal mean is taken out (-ln2 sd^2 / 2 octaves).
 *   BRANCH   self-excitation in its cluster form (Hawkes): every event, immigrant or child, sets off Binomial(2, n / 2)
 *            children (mean n, the branching ratio < 1; clusters of mean size 1 / (1 - n), heavy-tailed near 1:
 *            avalanches), after an exponential delay (mean tau: crackle, splashes) or an Omori one (p = 1.5, c = tau:
 *            t = c (u^-2 - 1), aftershocks), each a level step (child_l2, sd child_sd) re its parent. The immigrants
 *            are thinned by (1 - n), so the mean rate stays the fader's.
 *   SPACING  the immigrants' gap law: reg -64 clumped (bursts) .. 0 Poisson .. 63 even (as before).
 * Salience: an event's distance drawn over the ground or the volume around the listener (p(r) ~ r^(dim - 1) between a
 * near and a far radius): most events are far and quiet, a few are near and loud, so as the density rises the many
 * fuse into a mass while the few near ones still stand out; the total power still grows with the count.
 * Uniqueness: tx_clu_vary draws an event's (or, with a wider spread, an individual's) offsets in size, hardness,
 * decay, brightness (where it is struck) and mode detuning, correlated as the physics couples them (a bigger piece:
 * lower, softer contact, longer ring). Individuals (callers) keep a trait drawn once and add a small per-call one.
 * State: 208 bytes. */
#ifndef TX_CORE_CLUSTER_H
#define TX_CORE_CLUSTER_H
#include <stdint.h>
#include "tx_core_math.h"
#include "tx_core_life.h"

#define TX_CLU_Q 24u               /* children waiting */

typedef struct {                   /* a preset's clustering character at the activity now */
    int32_t cox;                   /* sd of the rate's walk, octaves Q16 (0: steady) */
    int32_t cox_tau_l2;            /* its time constant, log2 blocks Q16 */
    int32_t branch;                /* branching ratio, Q16 (<= 0.95) */
    int32_t tau;                   /* children's delay: mean (exponential) or c (Omori), samples */
    int32_t child_l2, child_sd;    /* children's level re the parent: mean and sd, log2 Q16 */
    int32_t reg;                   /* immigrants' spacing: -64 clumped .. 0 Poisson .. 63 even */
    uint8_t omori, gens;           /* delay law (1 Omori); generations at most */
} tx_clu_cfg_t;

typedef struct { uint32_t t; int16_t lv; uint8_t gen, tag; } tx_clu_kid_t;   /* lv: log2 Q10 */

typedef struct {
    int32_t w;                     /* the Cox walk, Q15 (sd 1) */
    uint32_t rate_q8;              /* the immigrants' rate this block, events/s Q8 */
    uint32_t next_q8;              /* samples to the next immigrant, Q8 */
    uint32_t now;                  /* sample clock */
    uint8_t nk, itag, pad[2];      /* (itag: the next immigrant's tag, set by the caller: its individual) */
    tx_clu_kid_t k[TX_CLU_Q];
} tx_clu_t;

typedef struct { int32_t lv; uint8_t gen, tag, kid, pad; } tx_clu_ev_t;   /* lv: level step re a fresh event, log2 Q16 */

static inline void tx_clu_init(tx_clu_t *c)
{
    tx_zero(c, sizeof *c);
}

/* an immigrant's gap at the rate now, Q8 samples (the SPACING law) */
static inline uint32_t tx_clu_gap(tx_rng_t *r, uint32_t rate_q8, int32_t reg)
{
    uint32_t mean = (44100u << 16) / (rate_q8 ? rate_q8 : 1u);
    uint32_t e = (uint32_t)(((uint64_t)mean * (tx_neglog2_q12(tx_rand(r)) * 2839u >> 12)) >> 12);   /* exponential */
    if (reg > 63) reg = 63;
    if (reg < -64) reg = -64;
    if (reg > 0) {                                           /* toward even: mean (1 - j) + j Exp */
        uint32_t j = (uint32_t)(64 - reg) * 1024u;
        return (uint32_t)(((uint64_t)mean * (65536u - j) + (uint64_t)e * j) >> 16);
    }
    if (reg < 0 && (tx_rand(r) >> 16) < (uint32_t)(-reg) * 900u) return e >> 4;   /* clumped: a burst */
    return reg < 0 ? (uint32_t)(((uint64_t)e * (65536u + (uint32_t)(-reg) * 900u)) >> 16) : e;
}

/* once per block: the walk, and the immigrants' rate from the fader's rate (events/s Q8) */
static inline void tx_clu_block(tx_clu_t *c, tx_rng_t *r, const tx_clu_cfg_t *f, uint32_t rate_q8)
{
    int32_t l;
    if (f->cox) {
        int32_t t = f->cox_tau_l2 < (2 << 16) ? (2 << 16) : f->cox_tau_l2;
        int32_t k = (int32_t)tx_exp2((31 << 16) - t) >> 15;          /* 1 / tau, Q16 */
        int32_t g = (int32_t)tx_exp2((15 << 16) + 32768 - t / 2);    /* sqrt(2 / tau), Q15 */
        c->w += -(int32_t)(((int64_t)c->w * k) >> 16) + ((tx_life_gauss(r) * g) >> 12);
        c->w = c->w > 98304 ? 98304 : c->w < -98304 ? -98304 : c->w;
    }
    if (!rate_q8) { c->rate_q8 = 0; return; }
    l = (int32_t)(((int64_t)f->cox * c->w) >> 15)                    /* the walk, octaves */
        - (int32_t)((((int64_t)f->cox * f->cox) >> 16) * 22713 >> 16);   /* its log-normal mean out: ln2 / 2 sd^2 */
    if (f->branch > 0) {                                             /* immigrants thinned by (1 - n) */
        int32_t one_n = 65536 - (f->branch > 62259 ? 62259 : f->branch);
        l -= (int32_t)tx_neglog2_q12((uint32_t)one_n << 16) << 4;
    }
    l = l > (8 << 16) ? (8 << 16) : l < -(16 << 16) ? -(16 << 16) : l;
    c->rate_q8 = (uint32_t)(((uint64_t)rate_q8 * tx_exp2(l + (16 << 16))) >> 16);
    {
        uint32_t mean = (44100u << 16) / (c->rate_q8 ? c->rate_q8 : 1u);
        if (c->next_q8 / 4u > mean) c->next_q8 = tx_clu_gap(r, c->rate_q8, f->reg);   /* a much faster rate: now */
    }
}

/* children for an event at offset off of the block (lv: its level step, log2 Q16; gen: its generation) */
static inline void tx_clu_spawn(tx_clu_t *c, tx_rng_t *r, const tx_clu_cfg_t *f, unsigned off, int32_t lv, unsigned gen,
                                unsigned tag)
{
    unsigned m, i;
    uint32_t p = (uint32_t)(f->branch > 62259 ? 62259 : f->branch < 0 ? 0 : f->branch) >> 1;
    if (!p || gen >= f->gens) return;
    m = ((tx_rand(r) >> 16) < p) + ((tx_rand(r) >> 16) < p);
    for (i = 0; i < m && c->nk < TX_CLU_Q; i++) {
        uint32_t dt, x;
        int32_t l;
        if (f->omori) {                                      /* t = c (u^-2 - 1): u^-2 = 2^(2 (-log2 u)) */
            x = tx_neglog2_q12((tx_rand(r) | 1u)) << 5;      /* 2 (-log2 u), Q16 */
            dt = x > (6u << 16) ? 63u * (uint32_t)f->tau
                 : (uint32_t)(((uint64_t)(uint32_t)f->tau * (tx_exp2((int32_t)x + (16 << 16)) - 65536u)) >> 16);
        } else
            dt = (uint32_t)(((uint64_t)(uint32_t)f->tau * (tx_neglog2_q12(tx_rand(r)) * 2839u >> 12)) >> 12);
        if (dt > 220500u) dt = 220500u;                      /* 5 s at most */
        l = lv + f->child_l2 + (int32_t)(((int64_t)tx_life_gauss(r) * f->child_sd) >> 12);
        l = l > (4 << 16) ? (4 << 16) : l < -(30 << 16) ? -(30 << 16) : l;
        c->k[c->nk].t = c->now + off + 1u + dt;
        c->k[c->nk].lv = (int16_t)(l >> 6);
        c->k[c->nk].gen = (uint8_t)(gen + 1u);
        c->k[c->nk].tag = (uint8_t)tag;
        c->nk++;
    }
}

/* the next event in this block of n samples, in time order (immigrants and children due): its offset, or -1 when the
 * block has no more. Every event returned sets off its own children. */
static inline int tx_clu_next(tx_clu_t *c, tx_rng_t *r, const tx_clu_cfg_t *f, unsigned n, tx_clu_ev_t *ev)
{
    unsigned i, kb = TX_CLU_Q;
    int32_t kd = (int32_t)n;
    uint32_t im = c->rate_q8 ? c->next_q8 >> 8 : 0xFFFFFFFFu;
    for (i = 0; i < c->nk; i++) {                            /* the earliest child due */
        int32_t d = (int32_t)(c->k[i].t - c->now);
        if (d < kd) { kd = d; kb = i; }
    }
    if (kb < TX_CLU_Q && (kd < 0 || (uint32_t)kd <= im)) {
        unsigned off = kd < 0 ? 0u : (unsigned)kd;
        ev->lv = (int32_t)c->k[kb].lv * 64; ev->gen = c->k[kb].gen; ev->tag = c->k[kb].tag; ev->kid = 1;
        c->k[kb] = c->k[--c->nk];
        tx_clu_spawn(c, r, f, off, ev->lv, ev->gen, ev->tag);
        return (int)off;
    }
    if (im < n) {
        ev->lv = 0; ev->gen = 0; ev->tag = c->itag; ev->kid = 0;
        c->next_q8 += tx_clu_gap(r, c->rate_q8, f->reg);
        tx_clu_spawn(c, r, f, im, 0, 0, c->itag);
        return (int)im;
    }
    return -1;
}

/* the block is over */
static inline void tx_clu_end(tx_clu_t *c, unsigned n)
{
    if (c->rate_q8) c->next_q8 = c->next_q8 > (n << 8) ? c->next_q8 - (n << 8) : 0;
    c->now += n;
}

/* salience: a distance (log2 m Q16) over the ground (dim 2) or a volume (dim 3) or a line (1) around the listener,
 * p(r) ~ r^(dim - 1) from near_l2 out to near_l2 + span_l2: r^d = near^d (1 + u (K - 1)), K = 2^(d span) (<= 2^15) */
static inline int32_t tx_clu_dist(tx_rng_t *r, int32_t near_l2, int32_t span_l2, unsigned dim)
{
    int32_t ks = span_l2 * (int32_t)dim, lk;
    uint32_t K, x;
    if (span_l2 <= 0 || !dim) return near_l2;
    if (ks > (15 << 16)) ks = 15 << 16;
    K = tx_exp2(ks);                                         /* 1 .. 32768 */
    x = 65536u + (tx_rand(r) >> 16) * (K - 1u);
    lk = (32 << 16) - (int32_t)(tx_neglog2_q12(x) << 4) - (16 << 16);   /* log2(x / 65536), Q16 */
    return near_l2 + lk / (int32_t)dim;
}

/* uniqueness: an event's (or an individual's) offsets, log2 Q16, correlated as the physics couples them */
typedef struct {
    int32_t size, hard, decay, bright, detune;   /* sd of each (octaves Q16; detune: per mode) */
    int32_t k_hard, k_decay;                     /* their coupling to the size, Q8 (e.g. -128: half an octave softer
                                                  * contact per octave bigger is +128 on the contact time) */
} tx_clu_spread_t;
typedef struct { int32_t size, hard, decay, bright, detune; } tx_clu_var_t;

static inline int32_t tx_clu_g(tx_rng_t *r, int32_t sd) { return (int32_t)(((int64_t)tx_life_gauss(r) * sd) >> 12); }

static inline void tx_clu_vary(tx_rng_t *r, const tx_clu_spread_t *sp, tx_clu_var_t *o)
{
    o->size = tx_clu_g(r, sp->size);
    o->hard = ((o->size * sp->k_hard) >> 8) + tx_clu_g(r, sp->hard);
    o->decay = ((o->size * sp->k_decay) >> 8) + tx_clu_g(r, sp->decay);
    o->bright = tx_clu_g(r, sp->bright);
    o->detune = sp->detune;
}

/* a mode's detune for an event: +-detune (log2 Q16), drawn per mode */
static inline int32_t tx_clu_detune(tx_rng_t *r, const tx_clu_var_t *v) { return tx_clu_g(r, v->detune); }

/* a value along three points (at a = 0, 64 and 127 << 8; |hi - lo| < 2^20) */
static inline int32_t tx_clu_at(int32_t v0, int32_t v64, int32_t v127, int32_t a_q8)
{
    a_q8 = a_q8 < 0 ? 0 : a_q8 > (127 << 8) ? (127 << 8) : a_q8;
    if (a_q8 < (64 << 8)) return v0 + ((v64 - v0) * (a_q8 >> 4)) / (64 << 4);
    return v64 + ((v127 - v64) * ((a_q8 - (64 << 8)) >> 4)) / (63 << 4);
}

#endif
