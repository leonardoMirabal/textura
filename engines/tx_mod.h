/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura modulation sources (the firmware's mod.c runs them; make tx-budget times them): the LFOs and the matrix's
 * sum. Instances, no globals, integers only, one call a block (172 Hz control rate).
 *
 * An LFO: 12 shapes, free (0.0005 .. 20 Hz, about 8 steps an octave) or synced to the song position (1/32T .. 64 BAR,
 * dotted and triplet), restarted by keys / a lane / a one-shot, its start phase, its WIDTH (the shape's skew), JITTER
 * (a free cycle's length varies at random by up to +-50 %: a free LFO never becomes a beat), FADE (the depth rises
 * after a restart), LEVEL. Its value is bipolar Q15. A ring of its last values (every 1/32 cycle) lets each lane read
 * it a little later (the matrix's SPREAD: the lanes move alike but never in step).
 *
 * The defaults are organic, never a beat: tx_lfo_init(n) gives LFO 1..4 SMOOTH / DRIFT / GUSTS / S&H at 0.03 .. 0.2 Hz
 * and LFO 5..8 SMOOTH at 0.1 Hz, all free, JITTER 15 %. */
#ifndef TX_MOD_H
#define TX_MOD_H
#include <stdint.h>
#include "tx_engine.h"

#define TX_LFO_N 8u
enum { TX_LS_SINE, TX_LS_TRI, TX_LS_SAWUP, TX_LS_SAWDN, TX_LS_SQUARE, TX_LS_PULSE, TX_LS_SH, TX_LS_SMOOTH, TX_LS_DRIFT,
       TX_LS_GUSTS, TX_LS_CHAOS, TX_LS_STEPS, TX_LS_N };
enum { TX_LM_FREE, TX_LM_KEY, TX_LM_LANE1, TX_LM_LANE2, TX_LM_LANE3, TX_LM_LANE4, TX_LM_ONESHOT, TX_LM_N };
enum { TX_LP_SHAPE, TX_LP_RATE, TX_LP_SYNC, TX_LP_MODE, TX_LP_PHASE, TX_LP_WIDTH, TX_LP_JITTER, TX_LP_FADE,
       TX_LP_RESTART, TX_LP_LEVEL, TX_LP_N };   /* (append only) */
extern const tx_param_t tx_lfo_params[TX_LP_N];
#define TX_LFO_NSYNC 24u                         /* SYNC: 0 FREE, then the divisions */
extern const uint16_t TX_LFO_SYNC_UNITS[TX_LFO_NSYNC];   /* in 1/96 quarter */
#define TX_POS_WRAP 73728u                       /* the song position's wrap (192 bars: every division divides it) */
#define TX_LFO_RING 32u

typedef struct {
    uint8_t p[TX_LP_N];
    uint32_t ph;                                 /* the phase, Q32 */
    uint32_t pb;                                 /* synced: the last block's phase before PHASE (a cycle's end) */
    uint32_t acc;                                /* CHAOS: the steps owed (Q32 of a cycle) */
    uint32_t jf;                                 /* this cycle's length factor (JITTER), Q15 */
    uint32_t rng;
    int32_t a, b;                                /* the random shapes: the segment's start and end (Q15) */
    int32_t x, y, z;                             /* DRIFT's walk; GUSTS' swell and its peak; CHAOS's state (Q10) */
    uint8_t stage;                               /* GUSTS: 0 resting, 1 rising, 2 falling */
    uint8_t held;                                /* ONE-SHOT: its cycle done */
    uint8_t fresh;                               /* a restart since the last block */
    uint32_t fade;                               /* Q16, 0 .. 65536 */
    int32_t out;                                 /* the value now, Q15 (LEVEL and FADE applied) */
    int16_t ring[TX_LFO_RING];                   /* the values at the last 32 1/32-cycle points */
    uint8_t rw;                                  /* the ring's next write */
    uint32_t rpt;                                /* the phase's 1/32 at the last write */
} tx_lfo_t;

void tx_lfo_init(tx_lfo_t *l, unsigned n, uint32_t seed);   /* LFO n (0..7)'s defaults */
void tx_lfo_param(tx_lfo_t *l, unsigned id, unsigned v);
void tx_lfo_restart(tx_lfo_t *l);                /* a key, a lane, a one-shot's trigger, START (RESTART ON START) */
uint32_t tx_lfo_inc(unsigned rate);              /* RATE (0..127) -> the phase step a block, Q32 */
uint32_t tx_lfo_period_ms(unsigned rate);        /* RATE -> its period in ms (the card's text) */
/* one block: rate and level as modulated (0..127), pos = the song position (1/96 quarter, Q8, < TX_POS_WRAP << 8) */
TX_AUDIO_CODE void tx_lfo_block(tx_lfo_t *l, unsigned rate, unsigned level, uint32_t pos);
TX_AUDIO_CODE int32_t tx_lfo_lag(const tx_lfo_t *l, uint32_t lag_q8);   /* its value lag (1/32 cycles, Q8) ago */

/* the matrix's sum: route r adds depth (Q15, +-1) x its source (Q15) x the target's range (hi) to sum[r->t] (Q8 of
 * the target's units). Sources 0..7 are the LFOs (read lag 1/32 cycles later: SPREAD), 8.. ext[src - 8]. */
typedef struct { int16_t depth; uint8_t src, t, lag, hi; } tx_mod_rt_t;
TX_AUDIO_CODE void tx_mod_sum(const tx_mod_rt_t *r, unsigned n, const tx_lfo_t *lfo, const int16_t *ext, int32_t *sum);
#endif
