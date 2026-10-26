/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura GOV: the CPU governor across lanes (SOURCE-DESIGN 4e), v2. The lanes share one pool (TX_GOV_POOL_*: the
 * planned ~1700 us on the host, on the device what the measured interrupt leaves). Per block the host passes each
 * lane's cost (us on the device: measured, or estimated), its level and whether it sounds; the lane's inserts (DRIVE,
 * FILTER: tx_fx_lane_us) are given apart (tx_gov_fixed): they count against the pool but no hero buys them back.
 *
 *   over the pool (the ~8-block mean): one hero less, every 8 blocks, where it pays most: the lane with the most us
 *     saved per level lost (its us a hero / its level a hero), lifted heroes first (below);
 *     a lane whose heroes save under TX_GOV_SAVE_MIN us is left alone (cutting it would only cost sound)
 *   well under (3/4 of the pool) for a second: one hero back to the loudest lane that was cut; then, with every cut
 *     given back, headroom to the playing lanes: one hero more than set, the loudest first, up to its ceiling
 *     (tx_gov_ceil; none unless the host sets one), and only while the mean with that hero stays under 3/4 of the
 *     pool (the hysteresis between the two thresholds: a lift never brings the next cut)
 *   a lane over half the pool on its own (its ~8-block mean and its inserts): a hero less there first, every 8 blocks,
 *     whatever the rest costs; the headroom never lifts a lane past half
 *   tx_gov_extra: the pool's share no lane carries (the modulation), counted in the total
 *   the us a hero saves is learned per lane: a cut's cost before, its cost ~32 blocks after (the lane's own 32-block
 *     mean), the change per hero blended into the estimate (the first reading replaces the guess); until a lane's first cut has been measured, its mean
 *     cost / (heroes + 2) stands for it
 * Integers only; 64-bit products only (control rate), no divide in 64 bits. */
#ifndef TX_CORE_GOV_H
#define TX_CORE_GOV_H
#include <stdint.h>

#define TX_GOV_LANES 16u
#define TX_GOV_MIN 2u               /* a lane keeps at least this many heroes */
#define TX_GOV_POOL_PLANNED 1700u   /* the sources' pool decided on the host (make tx-budget: CPU-BUDGET 4) */
#define TX_GOV_POOL_DEVICE 2500u    /* what the device's measured interrupt leaves, typically (USB at 48 kHz) */
#define TX_GOV_ISR_TARGET 4200u     /* the whole interrupt's target on the device (the gate: 4500) */
#define TX_GOV_SAVE_MIN 8u          /* us a hero must save for a cut to be worth its sound */
#define TX_GOV_LEARN 32u            /* blocks from a cut to its measurement */
#define TX_GOV_WARM 96u             /* blocks a lane sounds before its means are taken for real */

typedef struct {
    uint32_t budget;                /* us per block for all the lanes (the pool) */
    uint32_t avg;                   /* the cost, smoothed over ~8 blocks (us x 8) */
    uint16_t calm;                  /* blocks the cost has stayed under 3/4 of the budget */
    uint8_t n, hold;                /* lanes; blocks before the next cut (a cut takes a few blocks to show) */
    uint8_t user[TX_GOV_LANES], cur[TX_GOV_LANES];   /* PARTICLES as set, and as the governor allows now */
    uint8_t ceil[TX_GOV_LANES];     /* the most heroes the headroom may give (0, or not above user: none) */
    uint8_t lcut[TX_GOV_LANES], lt[TX_GOV_LANES];    /* heroes cut since the measurement began; blocks to it */
    uint32_t fixed[TX_GOV_LANES];   /* the lane's inserts, us a block (no hero buys them back) */
    uint32_t extra;                 /* the pool's share no lane has (the modulation), us a block */
    uint32_t lavg[TX_GOV_LANES];    /* the lane's own cost, smoothed over ~32 blocks (us x 32) */
    uint32_t lsh[TX_GOV_LANES];     /* the lane's own cost over ~8 blocks (us x 8): a cut's before and after */
    uint32_t lpre[TX_GOV_LANES];    /* its lavg when the measurement began (x 32) */
    uint32_t lsum[TX_GOV_LANES];    /* its cost summed since the last cut (TX_GOV_LEARN blocks: x 32) */
    uint16_t warm[TX_GOV_LANES];    /* blocks the lane has sounded (its means settled: TX_GOV_WARM) */
    uint32_t uph[TX_GOV_LANES];     /* us a hero saves, x 16 (0: not measured yet) */
} tx_gov_t;

static inline void tx_gov_init(tx_gov_t *g, unsigned lanes, uint32_t budget_us)
{
    unsigned i;
    g->budget = budget_us; g->avg = 0; g->calm = 0; g->hold = 0; g->n = (uint8_t)(lanes > TX_GOV_LANES ? TX_GOV_LANES : lanes);
    for (i = 0; i < TX_GOV_LANES; i++) {
        g->user[i] = g->cur[i] = g->ceil[i] = 0;          /* 0: not set (the engine's default) */
        g->extra = 0;
        g->lcut[i] = g->lt[i] = 0; g->fixed[i] = g->lavg[i] = g->lsh[i] = g->lpre[i] = g->lsum[i] = g->uph[i] = 0; g->warm[i] = 0;
    }
}

/* the pool on the device: the target less the rest of the interrupt as measured (the platform, the mix, the effects) */
static inline uint32_t tx_gov_pool_device(uint32_t isr_rest_us)
{
    return isr_rest_us + 300u < TX_GOV_ISR_TARGET ? TX_GOV_ISR_TARGET - isr_rest_us : 300u;
}

/* the lane's PARTICLES as set by the player (or its default, def, when unset): what the governor gives back to */
static inline void tx_gov_user(tx_gov_t *g, unsigned k, unsigned v)
{
    if (k >= g->n) return;
    g->user[k] = (uint8_t)v;
    if (!g->cur[k] || g->cur[k] > v) g->cur[k] = (uint8_t)v;
    g->lcut[k] = g->lt[k] = 0; g->uph[k] = 0;            /* (another setting: its heroes cost anew) */
}
/* the most heroes the headroom may give the lane (the engine's own most, say; at least what it is set to) */
static inline void tx_gov_ceil(tx_gov_t *g, unsigned k, unsigned v)
{
    if (k >= g->n) return;
    g->ceil[k] = (uint8_t)(v > 255u ? 255u : v);
    if (g->cur[k] > g->user[k] && g->cur[k] > g->ceil[k]) g->cur[k] = g->ceil[k] > g->user[k] ? g->ceil[k] : g->user[k];
}
/* the lane's inserts (us a block, e.g. tx_fx_lane_us): part of the pool's cost */
static inline void tx_gov_fixed(tx_gov_t *g, unsigned k, uint32_t us)
{
    if (k < g->n) g->fixed[k] = us;
}
/* what the pool also carries but no lane (the modulation: tx_mod's LFOs and matrix), us a block */
static inline void tx_gov_extra(tx_gov_t *g, uint32_t us) { g->extra = us; }

/* us a hero saves on lane k, x 16: measured, or the mean cost over (heroes + 2) until it is */
static inline uint32_t tx_gov_uph(const tx_gov_t *g, unsigned k)
{
    return g->uph[k] ? g->uph[k] : (g->lavg[k] >> 1) / ((uint32_t)g->cur[k] + 2u);   /* (lavg x 32 / 2 = x 16) */
}

/* one block: cost[k] (us, the lane's engine), level[k] (any scale: louder = larger), on[k] (the lane sounds);
 * returns the lane whose PARTICLES changed (its new value in cur[k]), or -1 */
static inline int tx_gov_block(tx_gov_t *g, const uint32_t *cost, const uint32_t *level, const uint8_t *on)
{
    uint32_t total = 0;
    unsigned k;
    int pick = -1;
    for (k = 0; k < g->n; k++) {
        if (!on[k]) { g->lavg[k] = g->lsh[k] = 0; g->warm[k] = 0; g->lt[k] = g->lcut[k] = 0; continue; }
        total += cost[k] + g->fixed[k];
        if (!g->warm[k]) { g->lavg[k] = cost[k] << 5; g->lsh[k] = cost[k] << 3; }   /* (a lane starts: its first block) */
        g->lavg[k] += cost[k] - (g->lavg[k] >> 5);
        g->lsh[k] += cost[k] - (g->lsh[k] >> 3);
        if (g->warm[k] < 0xFFFFu) g->warm[k]++;
        if (g->lt[k] && g->warm[k] >= TX_GOV_WARM / 2u) g->lsum[k] += cost[k];   /* (the window after the cut) */
        if (g->lt[k] && g->warm[k] >= TX_GOV_WARM / 2u && !--g->lt[k]) {   /* a cut measured: us a hero, blended
                                                         * in (half a time); a lane just started lets its means settle
                                                         * first, so the reading is the cut's, not the start's. Before:
                                                         * the ~32-block mean at the cut; after: the mean of the
                                                         * TX_GOV_LEARN blocks since (an 8-block mean on either side
                                                         * read a lane whose cost swings, a stone avalanche, as a cut
                                                         * that saved nothing, and its cuts stopped for good) */
            uint32_t pre = g->lpre[k] >> 1, now = g->lsum[k] >> 1, d = pre > now ? (pre - now) / g->lcut[k] : 0;   /* (x 16) */
            uint32_t est = tx_gov_uph(g, k), hi = est * 4u + 64u;
            if (d > hi) d = hi;                          /* (one noisy reading moves it at most this far) */
            g->uph[k] = g->uph[k] ? (g->uph[k] + d) / 2u : d;   /* (the first reading replaces the guess) */
            if (!g->uph[k]) g->uph[k] = 1;               /* (measured: it saves next to nothing) */
            g->lcut[k] = 0;
        }
    }
    total += g->extra;
    g->avg += total - (g->avg >> 3);                     /* avg ~ 8 x the mean of the last ~8 blocks */
    if (g->hold) g->hold--;
    if (!g->hold) {                                      /* a lane over half the pool on its own: one hero less there
                                                         * first, whatever the others (CLAUDE.md: no lane over half) */
        uint32_t worst = g->budget >> 1;
        for (k = 0; k < g->n; k++)
            if (on[k] && g->cur[k] > TX_GOV_MIN && !(g->lt[k] && !g->uph[k]) && g->warm[k] >= TX_GOV_WARM / 2u &&
                (g->lsh[k] >> 3) + g->fixed[k] > worst && tx_gov_uph(g, k) >= TX_GOV_SAVE_MIN * 16u) {
                worst = (g->lsh[k] >> 3) + g->fixed[k]; pick = (int)k;
            }
        if (pick >= 0) {
            g->cur[pick]--; g->hold = 8; g->calm = 0;
            if (!g->lt[pick]) g->lpre[pick] = g->lavg[pick];
            g->lcut[pick]++; g->lt[pick] = TX_GOV_LEARN; g->lsum[pick] = 0;
            return pick;
        }
    }
    if ((g->avg >> 3) > g->budget) {                     /* over: one hero where it pays most */
        uint32_t bu = 0, bq = 1;
        int lifted = 0;
        g->calm = 0;
        if (g->hold) return -1;
        for (k = 0; k < g->n; k++) lifted |= on[k] && g->cur[k] > g->user[k];
        for (k = 0; k < g->n; k++) {
            uint32_t u, q;
            if (!on[k] || g->cur[k] <= TX_GOV_MIN || (lifted && g->cur[k] <= g->user[k])) continue;
            if (g->lt[k] && !g->uph[k]) continue;        /* (its first cut still being measured: its guess may be
                                                         * wrong, so not another on it yet) */
            u = tx_gov_uph(g, k);
            if (u < TX_GOV_SAVE_MIN * 16u) continue;
            q = level[k] / g->cur[k] + 1u;               /* the level a hero carries */
            if (pick < 0 || (uint64_t)u * bq > (uint64_t)bu * q) { bu = u; bq = q; pick = (int)k; }
        }
        if (pick >= 0) {
            g->cur[pick]--; g->hold = 8;
            if (!g->lt[pick]) g->lpre[pick] = g->lavg[pick];
            g->lcut[pick]++; g->lt[pick] = TX_GOV_LEARN; g->lsum[pick] = 0;
        }
        return pick;
    }
    if ((g->avg >> 3) < (g->budget >> 2) * 3u) {         /* well under for a second: back, then headroom */
        uint32_t hi = 0;
        if (++g->calm < 172u) return -1;
        g->calm = 0;
        for (k = 0; k < g->n; k++)                       /* (a cut given back: never past half the pool on its own) */
            if (on[k] && g->cur[k] < g->user[k] && level[k] >= hi &&
                (g->lsh[k] >> 3) + g->fixed[k] + (tx_gov_uph(g, k) >> 4) < (g->budget >> 1)) { hi = level[k]; pick = (int)k; }
        if (pick < 0)
            for (k = 0; k < g->n; k++)
                if (on[k] && g->cur[k] >= g->user[k] && g->cur[k] < g->ceil[k] && level[k] >= hi &&
                    (g->avg >> 3) + (tx_gov_uph(g, k) >> 4) < (g->budget >> 2) * 3u &&
                    (g->lsh[k] >> 3) + g->fixed[k] + (tx_gov_uph(g, k) >> 4) < (g->budget >> 1)) { hi = level[k]; pick = (int)k; }
        if (pick >= 0) { g->cur[pick]++; g->lt[pick] = g->lcut[pick] = 0; }
        return pick;
    }
    g->calm = 0;
    return -1;
}

#endif
