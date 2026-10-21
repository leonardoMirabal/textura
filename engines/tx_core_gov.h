/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura GOV: the CPU governor across lanes (SOURCE-DESIGN 4e). When the lanes' cost nears the sources' budget it
 * lowers one lane's PARTICLES (the hero pool) by one, the quietest lane first; the engine then lets its quietest
 * heroes go (the source: farthest and quietest first, into its bed where it has one: power kept, never a dropout);
 * when the cost has stayed well under the budget for a second it gives one back, up to what the lane was set to.
 * Per block: the host passes each lane's cost (us on the device: measured, or estimated) and level. Integers only. */
#ifndef TX_CORE_GOV_H
#define TX_CORE_GOV_H
#include <stdint.h>

#define TX_GOV_LANES 16u
#define TX_GOV_MIN 2u               /* a lane keeps at least this many heroes */

typedef struct {
    uint32_t budget;                /* us per block for all the lanes (55 % of 5805: 3193) */
    uint32_t avg;                   /* the cost, smoothed over ~8 blocks (us x 8) */
    uint16_t calm;                  /* blocks the cost has stayed under 3/4 of the budget */
    uint8_t n, hold;                /* lanes; blocks before the next cut (a cut takes a few blocks to show) */
    uint8_t user[TX_GOV_LANES], cur[TX_GOV_LANES];   /* PARTICLES as set, and as the governor allows now */
} tx_gov_t;

static inline void tx_gov_init(tx_gov_t *g, unsigned lanes, uint32_t budget_us)
{
    unsigned i;
    g->budget = budget_us; g->avg = 0; g->calm = 0; g->hold = 0; g->n = (uint8_t)(lanes > TX_GOV_LANES ? TX_GOV_LANES : lanes);
    for (i = 0; i < TX_GOV_LANES; i++) g->user[i] = g->cur[i] = 0;   /* 0: not set (the engine's default) */
}

/* the lane's PARTICLES as set by the player (or its default, def, when unset): the governor's ceiling for it */
static inline void tx_gov_user(tx_gov_t *g, unsigned k, unsigned v)
{
    if (k >= g->n) return;
    g->user[k] = (uint8_t)v;
    if (!g->cur[k] || g->cur[k] > v) g->cur[k] = (uint8_t)v;
}

/* one block: cost[k] (us), level[k] (any scale: louder = larger), on[k] (the lane sounds); returns the lane whose
 * PARTICLES changed (its new value in cur[k]), or -1 */
static inline int tx_gov_block(tx_gov_t *g, const uint32_t *cost, const uint32_t *level, const uint8_t *on)
{
    uint32_t total = 0;
    unsigned k;
    int pick = -1;
    for (k = 0; k < g->n; k++) if (on[k]) total += cost[k];
    g->avg += total - (g->avg >> 3);                         /* avg ~ 8 x the mean of the last ~8 blocks */
    if (g->hold) g->hold--;
    if ((g->avg >> 3) > g->budget) {                         /* over: the quietest lane gives one hero */
        uint32_t lo = 0xFFFFFFFFu;
        g->calm = 0;
        if (g->hold) return -1;
        for (k = 0; k < g->n; k++) if (on[k] && g->cur[k] > TX_GOV_MIN && level[k] < lo) { lo = level[k]; pick = (int)k; }
        if (pick >= 0) { g->cur[pick]--; g->hold = 8; }
        return pick;
    }
    if ((g->avg >> 3) < (g->budget >> 2) * 3u) {             /* well under for a second: the loudest lane cut gets one back */
        if (++g->calm < 172u) return -1;
        g->calm = 0;
        {
            uint32_t hi = 0;
            for (k = 0; k < g->n; k++) if (on[k] && g->cur[k] < g->user[k] && level[k] >= hi) { hi = level[k]; pick = (int)k; }
        }
        if (pick >= 0) g->cur[pick]++;
        return pick;
    }
    g->calm = 0;
    return -1;
}

#endif
