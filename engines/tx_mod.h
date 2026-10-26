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

/* ---- ENV: an envelope, ATTACK DECAY (to SUSTAIN) RELEASE, each 1 ms .. 60 s; CURVE linear .. eased (a fast start,
 * as an exponential); VARY: each trigger varies its times (+-50 %) and its peak (down to half) at random, so no two are
 * alike; LOOP: once done it starts again after a random rest (0 .. 2 x DECAY), on its own. Unipolar, Q15 (0 .. 32767).
 * The trigger is chosen by TRIGGER and detected by the caller (tx_env_block's trig); RANDOM fires on its own (a mean
 * gap of 4 x DECAY), and EVENTS (a lane's particles) retrigger only once the attack and decay are done. The sustain
 * holds while the caller's gate is open (KEY / LANE: notes held); other triggers have none: decay, then release. */
#define TX_ENV_N 2u
enum { TX_ET_KEY, TX_ET_LANE1, TX_ET_EV1 = TX_ET_LANE1 + 4, TX_ET_RANDOM = TX_ET_EV1 + 4, TX_ET_LFO1,
       TX_ET_START = TX_ET_LFO1 + TX_LFO_N, TX_ET_N };
enum { TX_EP_TRIGGER, TX_EP_ATTACK, TX_EP_DECAY, TX_EP_RELEASE, TX_EP_SUSTAIN, TX_EP_CURVE, TX_EP_VARY, TX_EP_LOOP,
       TX_EP_N };   /* (append only) */
extern const tx_param_t tx_env_params[TX_EP_N];
enum { TX_ES_IDLE, TX_ES_ATT, TX_ES_DEC, TX_ES_SUS, TX_ES_REL, TX_ES_REST };
typedef struct {
    uint8_t p[TX_EP_N];
    uint8_t stage;
    uint32_t ph;                                 /* the stage's progress, Q24 */
    int32_t a, b;                                /* the stage's start and end values (Q15) */
    int32_t peak;                                /* this cycle's peak (VARY) */
    uint32_t vf;                                 /* this cycle's time factor (VARY), Q12 */
    uint32_t rest;                               /* LOOP: blocks before the next cycle */
    uint32_t rng;
    int32_t out;                                 /* the value now, Q15 */
} tx_env_t;
void tx_env_init(tx_env_t *e, uint32_t seed);
void tx_env_param(tx_env_t *e, unsigned id, unsigned v);
uint32_t tx_env_ms(unsigned v);                  /* ATTACK / DECAY / RELEASE -> ms (1 .. 60000) */
/* one block: trig = the trigger came (an edge), gate = notes held (KEY / LANE triggers) */
TX_AUDIO_CODE void tx_env_block(tx_env_t *e, unsigned trig, unsigned gate);

/* ---- FOLLOW: an envelope follower. Its input (chosen by INPUT, measured by the caller, Q15: a lane's level or the
 * mix's in dB over 60 dB, a lane's event rate over 0.1 .. 1000 a second, the grain delay's grains sounding) rises
 * with ATTACK and falls with RELEASE (5 ms .. 10 s), times GAIN (64 = x1, 127 = x2). Unipolar, Q15. */
#define TX_FOL_N 2u
enum { TX_FI_LEVEL1, TX_FI_RATE1 = TX_FI_LEVEL1 + 4, TX_FI_MIX = TX_FI_RATE1 + 4, TX_FI_GRAIN, TX_FI_N };
enum { TX_FP_INPUT, TX_FP_ATTACK, TX_FP_RELEASE, TX_FP_GAIN, TX_FP_N };   /* (append only) */
extern const tx_param_t tx_fol_params[TX_FP_N];
typedef struct {
    uint8_t p[TX_FP_N];
    int32_t env;                                 /* Q23 */
    int32_t out;                                 /* Q15 */
} tx_fol_t;
void tx_fol_init(tx_fol_t *f);
void tx_fol_param(tx_fol_t *f, unsigned id, unsigned v);
uint32_t tx_fol_ms(unsigned v);                  /* ATTACK / RELEASE -> ms (5 .. 10000) */
TX_AUDIO_CODE void tx_fol_block(tx_fol_t *f, int32_t x_q15);
/* a level (a peak, Q23 full scale) -> Q15 over -60 .. 0 dBFS; an event count a block (Q8) -> Q15 over 0.1 .. 1000 /s */
int32_t tx_fol_level_q15(uint32_t peak_q23);
int32_t tx_fol_rate_q15(uint32_t ev_q8);

/* the matrix's sum: route r adds depth (Q15, +-1) x its source (Q15) x the target's range (hi) to sum[r->t] (Q8 of
 * the target's units). Sources 0..7 are the LFOs (read lag 1/32 cycles later: SPREAD), 8.. ext[src - 8]. */
typedef struct { int16_t depth; uint8_t src, t, lag, hi; } tx_mod_rt_t;
TX_AUDIO_CODE void tx_mod_sum(const tx_mod_rt_t *r, unsigned n, const tx_lfo_t *lfo, const int16_t *ext, int32_t *sum);
#endif
