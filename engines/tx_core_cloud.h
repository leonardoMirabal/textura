/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura CLOUD / EVENTS core: when particles happen and how big they are, and which hero voice sounds each one.
 *
 *   EMITTERS  one per held key (up to TX_CLOUD_EMITTERS): a key press fires a BURST (its first event is the MAIN
 *             event), a held key a Poisson STREAM at a mean period; release stops the stream.
 *   SIZES     power law p(E) ~ E^-alpha on [E_min, E_max], drawn in log2: size_l2 = log2(E / E_min), Q12.
 *   OMORI     optional aftershocks after a MAIN event: count ~ binomial(2n, 1/2) (mean n), times from the normalised
 *             Omori law t = c ((1 - u)^(-1/(p-1)) - 1), sizes from the same power law, capped a gap below the main.
 *   PENDING   bursts and aftershocks wait in a small store with their due sample; each block takes the due ones.
 *   CAP       at most TX_CLOUD_EV_MAX explicit events per block (~44 000 /s); the rest are counted as dropped
 *             (later waves turn their energy into the statistical bed, as note 01 plans).
 *   POOL      a fixed hero pool (TX_POOL_MAX) with a PARTICLES limit: a new event takes a free hero, else steals the
 *             quietest one if the new event is louder, else it is rejected (counted). A hero is safe from stealing
 *             for its first full block (tx_pool_tick once per block). Engines keep each hero's level
 *             (amplitude, any monotonic unit) current once per block.
 *   DECLICK   a stolen or cut hero leaves its last output value here; it decays to exact zero in ~2 ms.
 *   DCBLOCK   a one-pole high-pass (~3.4 Hz, no multiplies) for an engine's sum: every damped sine started at zero
 *             phase carries a mean of A / omega, which a dense cloud piles into DC.
 *
 * Draws use -log2(u) (tx_neglog2_q12): one table lookup, a multiply by the slope, 2^x back where needed. 64-bit
 * products only per event (never per sample). */
#ifndef TX_CORE_CLOUD_H
#define TX_CORE_CLOUD_H
#include <stdint.h>
#include "tx_core_math.h"

#define TX_CLOUD_EMITTERS 8u
#define TX_CLOUD_PENDING 64u
#define TX_CLOUD_EV_MAX 32u
#define TX_POOL_MAX 32u

enum { TX_EV_STREAM = 0, TX_EV_MAIN = 1, TX_EV_BURST = 2, TX_EV_AFTER = 3 };

typedef struct {                   /* an event handed to the engine for this block */
    uint16_t offset;               /* sample in the block where it starts */
    uint8_t note, vel;
    uint8_t kind, emitter;
    int32_t size_l2;               /* log2(E / E_min), Q12, 0 .. size_max_l2 */
} tx_event_t;

typedef struct {                   /* a burst / aftershock event waiting for its sample */
    uint32_t t;                    /* due time on the cloud's sample clock */
    int32_t size_l2;
    uint8_t note, vel, kind, emitter;
} tx_pend_t;

typedef struct {
    uint32_t next_q8;              /* samples to the next stream event, Q8 */
    uint8_t note, vel, held, pad;
} tx_emitter_t;

typedef struct {
    /* configuration, set by the engine at control rate (tx_cloud_* helpers below) */
    uint32_t period_q8;            /* stream: mean samples between events of one held key, Q8; 0 = no stream */
    uint32_t burst_q8;             /* burst: mean samples between its events, Q8 */
    int32_t size_inv_q12;          /* 1 / (alpha - 1), Q12: the power-law slope of the sizes */
    int32_t size_max_l2;           /* sizes are truncated here (log2(E_max / E_min), Q12) */
    int32_t after_inv_q12;         /* Omori 1 / (p - 1), Q12 */
    uint32_t after_c;              /* Omori c, samples */
    int32_t after_gap_l2;          /* aftershocks at most this far below their main event (Q12 log2 energy) */
    uint8_t burst_n;               /* events per key press (0 = none) */
    uint8_t after_n;               /* mean aftershocks per MAIN event (0 = off: the Omori hook is idle) */
    uint8_t after_stream;          /* 1: stream events trigger aftershocks too */
    uint8_t npend;
    uint16_t jitter_q8;            /* stream spacing: 256 = Poisson (exponential gaps, the default), 0 = periodic; */
                                   /* between: gap = mean (1 - j) + exponential draw x j */
    /* state */
    tx_rng_t rng;
    uint32_t now;                  /* sample clock (wraps; compared by signed difference) */
    uint32_t em_rr;                /* next emitter to take when all are held */
    tx_emitter_t em[TX_CLOUD_EMITTERS];
    tx_pend_t pend[TX_CLOUD_PENDING];
    /* counters */
    uint32_t total;                /* events since init */
    uint32_t pend_full;            /* burst / aftershock events lost to a full store */
    uint16_t last_events, last_dropped;   /* of the last block (dropped: over TX_CLOUD_EV_MAX) */
} tx_cloud_t;

typedef struct {
    uint32_t level[TX_POOL_MAX];   /* each hero's current level (engine units; 0 when free) */
    uint32_t alive;                /* bit i: hero i sounds */
    uint32_t young, young2;        /* heroes started this block / the block before: never stolen (a hero cut in its
                                    * first half-period leaves a one-sided lobe: dense stealing became DC) */
    uint32_t steals, rejected;     /* since init */
    uint8_t limit, max;            /* PARTICLES (1..max) and the pool's size */
} tx_pool_t;

typedef struct { int32_t v; } tx_declick_t;
typedef struct { int32_t x1, y1; } tx_dcblock_t;

/* cloud */
void tx_cloud_init(tx_cloud_t *c, uint32_t seed);
void tx_cloud_key(tx_cloud_t *c, uint8_t note, uint8_t vel, uint8_t on);
TX_AUDIO_CODE unsigned tx_cloud_block(tx_cloud_t *c, tx_event_t *ev, unsigned n);   /* -> events, by offset */
unsigned tx_cloud_held(const tx_cloud_t *c);                   /* keys held now */
int tx_cloud_idle(const tx_cloud_t *c);                         /* nothing held, nothing pending */
void tx_cloud_rate(tx_cloud_t *c, uint32_t rate_q8);           /* stream rate per held key, events/s Q8 (0 = off) */
void tx_cloud_regular(tx_cloud_t *c, uint32_t jitter_q8);      /* 256 = Poisson stream (default) .. 0 = periodic */
void tx_cloud_burst(tx_cloud_t *c, uint32_t n, uint32_t spread_ms);   /* n events per press over ~spread_ms */
void tx_cloud_sizes(tx_cloud_t *c, uint32_t alpha_q12, uint32_t range_l2_q12);   /* alpha > 1, E_max / E_min */
void tx_cloud_omori(tx_cloud_t *c, uint32_t n, uint32_t p_q12, uint32_t c_ms, uint32_t gap_l2_q12);
uint32_t tx_cloud_exp_q8(tx_cloud_t *c, uint32_t mean_q8);     /* an exponential draw, Q8 samples */
int32_t tx_cloud_pareto(tx_cloud_t *c, int32_t inv_q12, int32_t max_l2);   /* a power-law size, Q12 log2 */

/* hero pool */
void tx_pool_init(tx_pool_t *p, uint32_t max, uint32_t limit);
int tx_pool_take(tx_pool_t *p, uint32_t level, uint32_t *stolen_level);   /* slot, or -1 (rejected) */
static inline void tx_pool_free(tx_pool_t *p, unsigned i) { p->alive &= ~(1u << i); p->level[i] = 0; }
static inline void tx_pool_tick(tx_pool_t *p) { p->young2 = p->young; p->young = 0; }   /* once per block */
uint32_t tx_pool_set_limit(tx_pool_t *p, uint32_t limit);     /* -> the heroes cut (bit mask) */
unsigned tx_pool_count(const tx_pool_t *p);

/* declick: a cut voice's last sample, decaying to exact zero (tau 64 samples) */
static inline void tx_declick_add(tx_declick_t *d, int32_t last) { d->v += last; }
TX_AUDIO_CODE static inline void tx_declick_run(tx_declick_t *d, int32_t *out, unsigned n)
{
    int32_t v = d->v;
    unsigned j;
    if (!v) return;
    for (j = 0; j < n; j++) {
        out[j] += v;
        v -= v >> 6;                                         /* toward zero (arithmetic shift: floor) */
        if (v > -64 && v < 64) { v = 0; break; }            /* the last step is under -100 dBFS */
    }
    d->v = v;
}

/* dcblock: y = x - x1 + y1 - leak, leak = y1 / 2048 (at least 1 toward zero), over buf, ADDED into out. The
 * leak never stalls, so silence in reaches exact zero (within 2048 samples of the last LSB). */
TX_AUDIO_CODE static inline void tx_dcblock_add(tx_dcblock_t *d, const int32_t *buf, int32_t *out, unsigned n)
{
    int32_t x1 = d->x1, y1 = d->y1;
    unsigned j;
    if (!x1 && !y1) {                                        /* silent in, silent state: skip the zeros */
        for (j = 0; j < n && !buf[j]; j++) { }
        if (j == n) return;
    }
    for (j = 0; j < n; j++) {
        int32_t x = buf[j];
        y1 = x - x1 + y1 - ((y1 >> 11) | (int32_t)(y1 > 0));
        x1 = x;
        out[j] += y1;
    }
    d->x1 = x1; d->y1 = y1;
}

#endif
