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

enum { FM_K_SCENE, FM_K_FX, FM_K_LANES };
enum { FM_FX_DRIVE, FM_FX_FILTER, FM_FX_REVERB, FM_FX_GRAIN, FM_FX_COMP };   /* COMP: the TYPE is the AMOUNT */
enum { FM_EV_ON, FM_EV_OFF, FM_EV_PARAM };

typedef struct { uint16_t blk; uint8_t kind, id, a, b; } fm_ev_t;   /* ON: a note, b vel; PARAM: id, a */

typedef struct {
    uint8_t kind;                  /* FM_K_* */
    uint8_t eng;                   /* SCENE: index in tx_engines[]; FX: FM_FX_*; LANES: the number of lanes */
    uint8_t type, decay;           /* FX: the TYPE (grain delay: the grains), DECAY (grain delay: 1 = pitched) */
    uint16_t ev, nev;              /* SCENE: its events in FM_EV[] */
    uint16_t nblk;                 /* blocks measured */
    uint16_t lane[4];              /* LANES: the scene configurations of the lanes */
    uint32_t seed;
} fm_cfg_t;

/* the stats of one run (ticks per block) */
typedef struct {
    uint32_t worst, w8, sum, raw_worst, raw_w8, n;
    uint32_t hash;                 /* FNV-1a over the output samples (Q23, as int32) */
    uint32_t ring[8], rraw[8];
    uint32_t lane_worst[4], lane_w8[4], lane_ring[4][8];
} fm_stats_t;

/* the clock hooks: the caller's (device: fm1_ticks and the timer's nested time; host: ns x 24 / 1000) */
typedef struct {
    uint32_t (*now)(void);         /* ticks */
    uint32_t (*nested)(void);      /* ticks of other interrupts inside the current block so far (0 on the host) */
} fm_clock_t;

/* the state: arenas for the engines and the effects (the caller places it: .pool on the device) */
typedef struct fm_run_s fm_run_t;
uint32_t fm_run_bytes(void);
void fm_begin(fm_run_t *r, const fm_cfg_t *tab, const fm_ev_t *ev, unsigned cfg, unsigned warm);
/* one block: out gets the output (mono Q23, or L/R for the lanes); 1 = more to come, 0 = done */
int fm_step(fm_run_t *r, const fm_clock_t *ck, int32_t *L, int32_t *R);
const fm_stats_t *fm_stats(const fm_run_t *r);

#endif
