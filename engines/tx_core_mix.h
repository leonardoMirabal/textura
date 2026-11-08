/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura MIX core: stage S3, four layers. Up to TX_MIX_SLOTS engine instances (each in its own state arena, the
 * caller's), each panned (constant power, unity at the centre: gains sqrt(2) sin / cos), summed into a stereo pair;
 * the pair's DRIVE (pre: tx_fx.h's, the first effect; AMOUNT 0: bypassed); the bus compressor (tx_core_comp.h: comp,
 * after the DRIVE; COMP off: bypassed); the CHAMBER (optional, mono) on the pair's mid, its added body and tail fed
 * back to both sides; then the bus effects (the grain delay, the reverb) and the output limiter, linked across the
 * two channels. Integer, Q23 in int32 like every mix here.
 *
 *   for each block: tx_mix_begin(m, n); tx_mix_slot_render(m, k, n) for each slot (the caller may time each one);
 *                   tx_mix_end(m, L, R, n) -> L, R limited (|x| <= -1 dBFS); m->raw_peak = the pair's peak before it */
#ifndef TX_CORE_MIX_H
#define TX_CORE_MIX_H
#include <stdint.h>
#include "tx_engine.h"
#include "tx_core_chamber.h"
#include "tx_core_limit.h"
#include "tx_core_comp.h"

#define TX_MIX_SLOTS 4u
#define TX_MIX_GATE_N 512u                 /* the gate's ramp: 11.6 ms at 44.1 kHz, no click */
#ifndef TX_LINE                    /* (as tx_fx_drv.h's: a buffer one core writes while the other works, its own lines) */
#ifdef __PI32V2__
#define TX_LINE __attribute__((aligned(64)))
#else
#define TX_LINE
#endif
#endif

typedef struct {
    const tx_engine_t *e[TX_MIX_SLOTS];  /* 0 = an empty slot */
    void *st[TX_MIX_SLOTS];              /* each slot's state (TX_STATE_MAX bytes, the caller's) */
    int32_t gl[TX_MIX_SLOTS], gr[TX_MIX_SLOTS];   /* pan gains, Q14 (16384 = 1.0) */
    int32_t tg[TX_MIX_SLOTS];            /* the slot's trim before its pan, Q12 (4096 = 1.0: none; up to +24 dB) */
    int32_t gc[TX_MIX_SLOTS], gt[TX_MIX_SLOTS];   /* the slot's gate (a lane's mute), Q15: now and where it ramps to
                                                   * (32768 open, 0 shut), TX_MIX_GATE_N samples end to end */
    int32_t slot_peak[TX_MIX_SLOTS];     /* each slot's peak since the caller last cleared it */
    tx_chamber_t *ch;                    /* optional (the caller's, ~22 KB), used while ch_on */
    int ch_on;
    /* optional effects (tx_fx.h; 0 = none): ins on each slot's block before it is panned, pre on the stereo pair
     * first (the DRIVE, before comp), bus on the stereo pair before the limiter, busq: the bus quiet; fx their state */
    void (*ins)(void *fx, unsigned k, int32_t *x, unsigned n);
    void (*pre)(void *fx, int32_t *L, int32_t *R, unsigned n);
    void (*bus)(void *fx, int32_t *L, int32_t *R, unsigned n);
    int (*busq)(void *fx);
    void (*pre1)(void *fx, int32_t *L, int32_t *R, unsigned n);   /* pre and bus as core 1 runs them (tx_mix_run's c:
                                                                   * the XIP twins, no job handed on; 0 = none) */
    void (*bus1)(void *fx, int32_t *L, int32_t *R, unsigned n);
    void *fx;
    tx_comp_t comp;                      /* the bus compressor, after the pair's DRIVE (COMP off: bypassed) */
    tx_limit_t lim;
    int32_t raw_peak;                    /* the pair's peak before the limiter, this block */
    int32_t L[TX_BLOCK];
    int32_t R[TX_BLOCK] TX_LINE;         /* (its own lines: the pair's DRIVE may run R on the other core) */
    int32_t blk[TX_BLOCK], mid[TX_BLOCK], ilv[2 * TX_BLOCK];   /* (free while pre runs: TX_MIX_SCR) */
} tx_mix_t;
/* blk, mid, ilv in a row: 4 KB that pre may use as the other core's scratch (they start on a line after R's) */
#define TX_MIX_SCR(m) ((void *)(m)->blk)
#define TX_MIX_SCR_BYTES (4u * TX_BLOCK * sizeof(int32_t))

void tx_mix_init(tx_mix_t *m);
/* put an engine in slot k (initialised here with seed), or empty it (e = 0) */
void tx_mix_set(tx_mix_t *m, unsigned k, const tx_engine_t *e, void *st, uint32_t seed);
void tx_mix_pan(tx_mix_t *m, unsigned k, unsigned pan);   /* 0 left .. 64 centre .. 127 right */
void tx_mix_trim(tx_mix_t *m, unsigned k, unsigned q12);  /* the slot's level trim, Q12 (a landscape sound's own level) */
/* the slot's gate: shut (a muted lane: the engine runs on, its output ramps to silence, the slot's peak is still the
 * engine's) or open again; either way a TX_MIX_GATE_N ramp */
void tx_mix_gate(tx_mix_t *m, unsigned k, int open);
TX_AUDIO_CODE void tx_mix_begin(tx_mix_t *m, unsigned n);
TX_AUDIO_CODE void tx_mix_slot_render(tx_mix_t *m, unsigned k, unsigned n);
/* or: the caller rendered slot k's block itself (a scene script, say): pan it and add it */
TX_AUDIO_CODE void tx_mix_add(tx_mix_t *m, unsigned k, const int32_t *x, unsigned n);
TX_AUDIO_CODE void tx_mix_end(tx_mix_t *m, int32_t *L, int32_t *R, unsigned n);
/* tx_mix_end in two: run (the DRIVE, the compressor, the chamber, the bus, the limiter: its result kept in ilv) on
 * core c (1: core 1's twins in XIP and pre1 / bus1, as one block behind: firmware/src/lanes.c LN_PIPE), then out (the
 * pair from ilv, any time before the next run) */
TX_AUDIO_CODE void tx_mix_run(tx_mix_t *m, unsigned n, unsigned c);
TX_AUDIO_CODE void tx_mix_out(const tx_mix_t *m, int32_t *L, int32_t *R, unsigned n);
/* all slots quiet (and the chamber idle) */
int tx_mix_quiet(const tx_mix_t *m);

#endif
