/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura GROOVE: a pulse the landscape's discrete events lean toward, felt rather than heard. Never a beat, never a
 * loop: it only biases the RATE of a lane's random events (a density over time), so they stay a natural cloud.
 *
 *   the ensemble   one TYPE, AMOUNT and SHIFT for the landscape; each lane plays a PART of it (PULSE, OFFBEAT, FILL;
 *                  FREE: none), or its own CYCLE (2 .. 9 even pulses over the span: polyrhythms between lanes)
 *   a TYPE         a span (beats) and either three cycles (pulses over the span for PULSE, OFFBEAT and FILL: 3:4:5
 *                  and the like, the polyrhythms) or a grid of n steps with strong / medium / weak steps and a
 *                  swing (from rhythm traditions, and Euclidean E(k, n) families: they only set where the pulse
 *                  leans); after the named ones, E(k, n) for n 2 .. 19
 *   the density    a soft bump (a triangle W ticks wide each side) at each slot, its height the slot's weight for the
 *                  lane's part, over a floor: events still fall between slots. The rate's multiplier is the density
 *                  over its own mean, so a lane keeps its mean rate: m = 1 + d (g / mean - 1)
 *   organic        d = AMOUNT x the sound's weight (none for flows: water, wind, rain beds; light for drips and
 *                  sparks; more for stones, knocks, chimes; most for insects, birds, cracks and crackles; W the same
 *                  way: loose for drips, tighter for calls) x a breath that wanders bar by bar (0.35 .. 1: bars
 *                  breathe in and out of it); each lane's grid drifts slowly in phase (+-1/6 beat over minutes), so
 *                  lanes realign seldom and the whole never repeats
 * Times in ticks: 1/96 of a beat (the song position's unit). Integers only, 32-bit; a few dozen operations a block. */
#ifndef TX_GROOVE_H
#define TX_GROOVE_H
#include <stdint.h>
#include "tx_engine.h"
#include "tx_core_math.h"

enum { TX_GRP_AUTO, TX_GRP_PULSE, TX_GRP_OFFBEAT, TX_GRP_FILL, TX_GRP_FREE, TX_GRP_N };   /* a lane's PART */
#define TX_GR_AMOUNT_DEF 35u        /* light: a faint pulse */
#define TX_GR_TYPE_DEF 0u           /* LOOSE 16 */
#define TX_GR_EU_N0 2u              /* E(k, n): n from 2 .. */
#define TX_GR_EU_N1 19u             /* .. to 19, k 1 .. n - 1 */
#define TX_GR_NTYPES 244u           /* the TYPES: 73 named, then the 171 E(k, n) */

typedef struct {
    uint8_t part;                   /* PULSE, OFFBEAT, FILL (FREE or 0: none) */
    uint8_t cycle;                  /* 0: the TYPE's; 2 .. 9 even pulses over the span */
    uint8_t wl;                     /* the sound's weight: 0 none (flows) .. 3 most */
    int32_t d;                      /* the depth now (Q15, glides) */
    int32_t b, bt;                  /* the breath and its target (Q15) */
    int32_t ph, dph;                /* the phase drift (ticks Q16) and its speed a block */
    uint32_t bar;                   /* the bar last seen */
    tx_rng_t rng;
} tx_groove_lane_t;

unsigned tx_groove_ntypes(void);
/* TYPE t's name into s (12 bytes at least) */
void tx_groove_name(unsigned t, char *s);
void tx_groove_lane_init(tx_groove_lane_t *l, uint32_t seed);
/* one block at the song position pos (ticks Q8), AMOUNT 0 .. 127: the lane's rate multiplier (Q12: 4096 = as it is;
 * 1 .. under 24000: the floor over the sparsest mean, one heavy slot's bump at most over it). AMOUNT 0 with the depth
 * settled, a FREE part or a weight 0: 4096 exactly */
int32_t tx_groove_mul(tx_groove_lane_t *l, unsigned type, unsigned amount, unsigned shift, uint32_t pos_q8);
/* how far tick is from the lane's pulse: its nearest heavy slot (3/4 of the part's heaviest weight or more), ticks;
 * the tests: how close events fall (0x7FFFFFFF: no pulse, a FREE part) */
int32_t tx_groove_dist(const tx_groove_lane_t *l, unsigned type, unsigned shift, uint32_t tick);

#endif
