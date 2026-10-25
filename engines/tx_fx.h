/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura effects (stage S3): instances, no globals (as the engines), integers only.
 *
 *   per lane, a mono insert on the lane's own signal before it is panned (the lanes are mono): DRIVE, then FILTER
 *     (their cost is the lane's: the governor's pool). Off (DRIVE AMOUNT 0, FILTER TYPE OFF) = the lane untouched.
 *   on the stereo mix, before the limiter: the REVERB (a mono send of the mid, a stereo return):
 *     TYPE  OFF | ROOM (the CHAMBER: the room's body modes and its short tail) | HALL | INFINITE | DARK
 *     (one TYPE runs at a time; their states share one memory union).
 *
 * The dry path stays Q23. Only the wet signals pass through the effects' precision (Frodhes' Q15 with headroom: a
 * floor ~-100 dBFS); an effect that is off costs nothing and changes no sample. Each effect's code and laws come from
 * Frodhes (fx.c, rev_types.c, rev_inf.c: our own) and Felucca (the HALL, the classic drive: their files keep
 * Felucca's header); see each file. */
#ifndef TX_FX_H
#define TX_FX_H
#include <stdint.h>
#include "tx_engine.h"
#include "tx_core_chamber.h"
#include "tx_fx_kern.h"

/* ---- FILTER (per lane): a TPT state-variable stage, stable at any setting (Frodhes' FILTER) */
enum { TX_FL_TYPE, TX_FL_FREQ, TX_FL_RESO, TX_FL_LFO, TX_FL_RATE, TX_FL_SHAPE, TX_FL_NP };
extern const tx_param_t tx_fx_filter_params[TX_FL_NP];
typedef struct {
    uint8_t p[TX_FL_NP];
    uint8_t was_on;
    uint32_t lfo_ph, lfo_rnd;
    int32_t lfo_sh, ic[2], op, pos, ow;
    int32_t dry[32];
} tx_fx_filter_t;
void tx_fx_filter_init(tx_fx_filter_t *f);
void tx_fx_filter_param(tx_fx_filter_t *f, unsigned id, unsigned v);
TX_AUDIO_CODE void tx_fx_filter_process(tx_fx_filter_t *f, int32_t *x, unsigned n);   /* mono Q23, in place */

/* ---- DRIVE (per lane): Felucca's track drive as Frodhes' classic FDRV (tx_fx_drive.c) */
enum { TX_DR_AMOUNT, TX_DR_TONE, TX_DR_MIX, TX_DR_NP };
extern const tx_param_t tx_fx_drive_params[TX_DR_NP];
typedef struct {
    uint8_t p[TX_DR_NP];
    uint8_t ton, on, la;           /* the shelf runs; the drive runs (or glides out); the last AMOUNT */
    int32_t hp, lp1, lp2, w, tg, tlp;
} tx_fx_drive_t;
void tx_fx_drive_init(tx_fx_drive_t *d);
void tx_fx_drive_param(tx_fx_drive_t *d, unsigned id, unsigned v);
TX_AUDIO_CODE void tx_fx_drive_process(tx_fx_drive_t *d, int32_t *x, unsigned n);     /* mono Q23, in place */

/* ---- REVERB (on the mix) */
enum { TX_RV_TYPE, TX_RV_MIX, TX_RV_DECAY, TX_RV_TONE, TX_RV_PREDELAY, TX_RV_SIZE, TX_RV_MOD, TX_RV_LOWCUT, TX_RV_NP };
enum { TX_RVT_OFF, TX_RVT_ROOM, TX_RVT_HALL, TX_RVT_INF, TX_RVT_DARK, TX_RVT_N };
extern const tx_param_t tx_fx_reverb_params[TX_RV_NP];
#define TX_PD_LEN 4096u            /* the pre-delay's line: 92.9 ms (int32: the send has headroom) */
#define TX_RV_QZ 5u
typedef struct {
    uint8_t p[TX_RV_NP];
    uint8_t cur, fade, running, pd_on;
    int32_t off, damp, dk;                       /* DECAY's size offset, TONE's damping, DECAY for the kernels (glide) */
    int32_t hp, he, lc, lca, lcv;                /* the kernels' DC blocker; LOW CUT */
    uint32_t pd_w, pd_d, pd_d0, pd_x, pd_fill;
    int32_t inb[RV_DZ + CTL];                    /* the send: the 2:1 down's history, the block */
    int32_t x[HL_NC];
    int32_t m[TX_RV_QZ + HL_NC], s[TX_RV_QZ + HL_NC];   /* the TYPE's mid / side (history, then the block) */
    int32_t rm[TX_BLOCK];                        /* ROOM: the send at full rate, gathered over the block */
    uint32_t r0, rn;                             /* ROOM: the gathered run's start and length */
    uint8_t rf[TX_BLOCK / CTL];                  /* ROOM: each control block's fade (0: none) */
    int32_t pd[TX_PD_LEN];
    union { tx_hall_t h; tx_inf_t i; tx_chamber_t c; } u;
} tx_fx_reverb_t;
void tx_fx_reverb_init(tx_fx_reverb_t *r);
void tx_fx_reverb_param(tx_fx_reverb_t *r, unsigned id, unsigned v);
TX_AUDIO_CODE void tx_fx_reverb_process(tx_fx_reverb_t *r, int32_t *L, int32_t *R, unsigned n);   /* Q23, adds */
int tx_fx_reverb_quiet(const tx_fx_reverb_t *r);
int tx_fx_reverb_frozen(const tx_fx_reverb_t *r);   /* holding its tail (SUSTAIN / FREEZE): never quiet */

/* ---- the chain: TX_FX_LANES lane inserts and the reverb; the mix calls it through tx_mix_t's hooks */
#define TX_FX_LANES 4u
typedef struct {
    tx_fx_drive_t drive[TX_FX_LANES];
    tx_fx_filter_t filter[TX_FX_LANES];
    tx_fx_reverb_t rev;
} tx_fx_t;
void tx_fx_init(tx_fx_t *fx);
TX_AUDIO_CODE void tx_fx_lane(void *fx, unsigned k, int32_t *x, unsigned n);          /* tx_mix_t.ins */
TX_AUDIO_CODE void tx_fx_bus(void *fx, int32_t *L, int32_t *R, unsigned n);           /* tx_mix_t.bus */
int tx_fx_bus_quiet(void *fx);                                                         /* tx_mix_t.busq */

#endif
