/* SPDX-License-Identifier: GPL-3.0-only */
/* txfm1: the engines' cost per 256-frame block, the same table on the FM-1 (bench/fm1/txfm1.c) and on the host
 * (bench/fm1/txfm1_host.c). A configuration is a scene (events per block) on one engine, an effect on a busy input,
 * or four lanes through the mix and the effects. The runner steps one block at a time (the device: one per audio
 * interrupt) and keeps the stats in 24 MHz ticks (the FM-1's clock; the host converts its ns).
 *
 * The table is generated on the host (txfm1_gen.c -> build/fm1/gen/fm1_cfg.h) from the same scripts as make
 * tx-budget, so the device needs no script parser. Integers only (the device has no FPU, no 64-bit division). */
#ifndef TXFM1_CORE_H
#define TXFM1_CORE_H
#include <stdint.h>
#include "tx_engine.h"

#define FM_TPU 24u                 /* ticks per us (the FM-1's timer) */
#define FM_SECS 8u                 /* a scene's render (as tx-budget) */
#define FM_WARM 344u               /* the warm-up: the scene's first 2 s, then a fresh start */

enum { FM_K_SCENE, FM_K_FX, FM_K_LANES, FM_K_C1 };   /* C1: a scene twice, core 0 and core 1 (fm_par_t), compared */
enum { FM_FX_DRIVE, FM_FX_FILTER, FM_FX_REVERB, FM_FX_GRAIN, FM_FX_COMP, FM_FX_CLOCK };   /* COMP: TYPE 0 its defaults,
                                    * 1 the hardest; CLOCK: the clock probe (FM_CLK_OPS dependent adds a block) */
#define FM_CLK_OPS 16384u          /* probe adds a block: us = FM_CLK_OPS / MHz */
enum { FM_EV_ON, FM_EV_OFF, FM_EV_PARAM };
enum { FM_NO_GRAIN = 1, FM_NO_REVERB = 2, FM_NO_DRIVE = 4, FM_NO_FILTER = 8 };   /* LANES: TYPE, effects left out */
#define FM_RUN_MAX (380u * 1024u)  /* the run's state (fm_run_t: the lanes' arenas, the effects with the grain delay's
                                    * line) in the pool: txfm1_core.c will not compile past it (the image would start,
                                    * show RUN STATE TOO BIG and never answer) */
enum { FM_G15 = 16, FM_PAR = 32, FM_G30 = 64 };   /* LANES: TYPE also: the grain delay's 15 grains forced (no cap); the
                                    * lanes split across the two cores (fm_par_t: the engines' renders, as
                                    * firmware/src/lanes.c); its 30 grains forced (GRAINS at the top, no cap) */

typedef struct { uint16_t blk; uint8_t kind, id, a, b; } fm_ev_t;   /* ON: a note, b vel; PARAM: id, a */

typedef struct {
    uint8_t kind;                  /* FM_K_* */
    uint8_t eng;                   /* SCENE: index in tx_engines[]; FX: FM_FX_*; LANES: the number of lanes */
    uint8_t type, decay;           /* FX: the TYPE (grain delay: the grains), DECAY (grain delay: 1 = pitched);
                                    * LANES: TYPE the effects left out (FM_NO_*); C1: TYPE 1 the ping (no engine), 2 the
                                    * math ping (64-bit products, divisions), 3 flash data, 4 data core 0 just wrote,
                                    * 5 load (a pointer chase over its own arena, no engine code) */
    uint16_t ev, nev;              /* SCENE: its events in FM_EV[] */
    uint32_t nblk;                 /* blocks measured (a soak row: 2 h = 1.24 M) */
    uint16_t lane[4];              /* LANES: the scene configurations of the lanes */
    uint32_t seed;
} fm_cfg_t;

/* the stats of one run (ticks per block) */
typedef struct {
    uint32_t worst, w8, sum, raw_worst, raw_w8, n;   /* (sum, c0_sum, c1_sum: ticks >> sum_sh) */
    uint32_t sum_sh;               /* 4, or 8 for a row over 65535 blocks (32 bits hold a 2 h soak) */
    uint32_t hash;                 /* FNV-1a over the output samples (Q23, as int32) */
    uint32_t ring[8], rraw[8];
    uint32_t lane_worst[4], lane_w8[4], lane_ring[4][8];
    uint32_t c1_jobs, c1_mis, c1_first, c1_solo;   /* C1: blocks core 1 rendered; blocks whose two renders differ, the
                                    * first of them (+ 1, 0 none); blocks core 0 rendered both (core 1 off or held).
                                    * LANES with FM_PAR: c1_jobs the blocks core 1 rendered lanes in, c1_mis the jobs
                                    * lost (held mid-job), c1_first the block of the loss (+ 1), c1_solo the blocks
                                    * core 0 rendered all */
    uint32_t c0_worst, c1_worst, c0_sum, c1_sum;   /* LANES FM_PAR: each core's busy ticks in the render phase (the
                                    * lanes with their inserts, the grain voices): the worst, the sum (ticks / 16) */
    uint32_t b1_worst, b1_sum, mix_worst, mix_sum;   /* LANES: core 1's busy ticks in the whole block (every job it
                                    * ran: the lanes, the reverb's halves, the mix's halves; 0 on one core), core 0's
                                    * ticks in the serial mix phase (the pans, the mix, the bus's end): worst, sum */
} fm_stats_t;

/* the clock hooks: the caller's (device: fm1_ticks and the timer's nested time; host: ns x 24 / 1000) */
typedef struct {
    uint32_t (*now)(void);         /* ticks */
    uint32_t (*nested)(void);      /* ticks of other interrupts inside the current block so far (0 on the host) */
} fm_clock_t;

/* the second core (FM_K_C1): run hands it fn(arg) (1 = taken, 0 = not running: the caller does the work), wait
 * waits for it (0 = done, else it was held: the work is lost). None (the host): core 0 renders both */
typedef struct {
    int (*run)(void (*fn)(uint32_t), uint32_t arg);
    int (*wait)(void);
} fm_par_t;

/* the state: arenas for the engines and the effects (the caller places it: .pool on the device) */
typedef struct fm_run_s fm_run_t;
uint32_t fm_run_bytes(void);
void fm_begin(fm_run_t *r, const fm_cfg_t *tab, const fm_ev_t *ev, unsigned cfg, unsigned warm);
/* one block: out gets the output (mono Q23, or L/R for the lanes); 1 = more to come, 0 = done */
int fm_step(fm_run_t *r, const fm_clock_t *ck, int32_t *L, int32_t *R);
const fm_stats_t *fm_stats(const fm_run_t *r);
void fm_set_par(fm_run_t *r, const fm_par_t *p);

#endif
