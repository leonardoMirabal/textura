/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura effects (stage S3): instances, no globals (as the engines), integers only.
 *
 *   per lane, a mono insert on the lane's own signal before it is panned (the lanes are mono): DRIVE, then FILTER
 *     (their cost is the lane's: the governor's pool). Off (DRIVE AMOUNT 0, FILTER TYPE OFF) = the lane untouched.
 *   on the stereo mix, before the limiter: the GRAIN DELAY (a delay whose repeats are grained), then the REVERB (a mono
 *   send of the mid, a stereo return):
 *     TYPE  OFF | ROOM (the CHAMBER's body modes + a short hall tail) | HALL | INFINITE | DARK | DRIFT | SWELL | PLATE
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
#include "tx_fx_drv.h"
#include "tx_fx_gd_len.h"

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
#define TX_FX_US_FILTER 96u        /* the filter's FM-1 cost a block, its dearest TYPE (make tx-budget section 4) */
static inline uint32_t tx_fx_filter_us(const tx_fx_filter_t *f) { return f->p[TX_FL_TYPE] ? TX_FX_US_FILTER : 0u; }

/* ---- DRIVE (per lane): Felucca's track drive as Frodhes' classic FDRV (TYPE 0) and Frodhes' four circuit TYPEs
 * (tx_fx_drive.c, tx_fx_drv_<type>.c; their state: tx_fx_drv.h) */
enum { TX_DR_AMOUNT, TX_DR_TONE, TX_DR_MIX, TX_DR_TYPE, TX_DR_NP };   /* (append only) */
extern const tx_param_t tx_fx_drive_params[TX_DR_NP];
#define TX_FX_US_FDRV 85u          /* the classic drive's FM-1 cost a block (make tx-budget section 4) */
typedef struct {
    uint8_t p[TX_DR_NP];
    uint8_t ton, on, la;           /* the shelf runs; the drive runs (or glides out); the last AMOUNT */
    uint8_t cur;                   /* the TYPE that runs (p[TX_DR_TYPE] after a change has faded through) */
    int32_t hp, lp1, lp2, w, tg, tlp;
    tx_drv_shared_t *sh;           /* what the lanes' TYPEs share (tx_fx_t; 0: the classic drive only) */
    tx_drv_state_t u;              /* the TYPE's state */
} tx_fx_drive_t;
void tx_fx_drive_init(tx_fx_drive_t *d);
void tx_fx_drive_param(tx_fx_drive_t *d, unsigned id, unsigned v);
TX_AUDIO_CODE void tx_fx_drive_process(tx_fx_drive_t *d, int32_t *x, unsigned n);     /* mono Q23, in place */
void tx_fx_drive_shared_init(tx_drv_shared_t *sh);
void tx_fx_drive_share(tx_fx_drive_t *d, tx_drv_shared_t *sh);
uint32_t tx_fx_drive_us(const tx_fx_drive_t *d);  /* its FM-1 cost now, us a block (0: off) */

/* ---- GRAIN DELAY (on the mix, before the reverb; tx_fx_grain.c): a delay whose repeats are grained */
enum { TX_GD_MIX, TX_GD_TIME, TX_GD_FEEDBACK, TX_GD_GRAINS, TX_GD_SIZE, TX_GD_SPRAY, TX_GD_PITCH, TX_GD_REVERSE,
       TX_GD_SPREAD, TX_GD_TONE, TX_GD_GRAIN, TX_GD_FREEZE, TX_GD_NP };
extern const tx_param_t tx_fx_grain_params[TX_GD_NP];
#define TX_GD_MAXG 16u
typedef struct {
    uint32_t pos;                  /* the read position, whole samples (0 .. TX_GD_N - 1) */
    uint32_t frac;                 /* its fraction, Q16 */
    int32_t rate;                  /* Q16 a sample (negative: reversed) */
    int16_t gl, gr;                /* pan, Q15 */
    uint8_t on;                    /* sounding (its window not ended) */
    uint32_t vp;                   /* its window's phase at the block's start (the shared clock's, or its own while
                                   * it finishes a window begun on the clock before GRAINS moved) */
} tx_gd_grain_t;
typedef struct {
    uint8_t p[TX_GD_NP];
    uint8_t pk[TX_GD_N / 2u * 3u];               /* the mantissas: 2 samples in 3 bytes */
    uint8_t ex[TX_GD_N / 32u];                   /* the shifts: 2 blocks of 16 in a byte */
    int32_t stg[16], stpk;                       /* the block being gathered (Q22), its peak */
    uint32_t w;                                  /* the write index */
    uint32_t zrun;                               /* zeros written in a row (>= TX_GD_N: the line is silent) */
    uint32_t ph;                                 /* the grains' clock (Q32) */
    uint32_t rng;
    uint32_t tap, tap0, tx;                      /* the echo's delay, the old one, the crossfade (0 = none) */
    int32_t lp, dcy, lpe, dce;                   /* the feedback: its low-pass, its DC block, their remainders */
    int32_t wg;                                  /* the write's gain (Q15: FREEZE fades it to 0) */
    int32_t gn;                                  /* the grains' level (Q15, glides: 2 / sounding) */
    uint8_t cap;                                 /* grains allowed to start now (the chain's dynamic cap) */
    uint8_t act;                                 /* grains sounding in the last block (on average) */
    uint8_t on;                                  /* running (MIX up, or its fade out) */
    uint32_t fill;                               /* samples written since it came on (older reads are 0) */
    int32_t mix;                                 /* MIX, gliding (Q15) */
    int32_t gbl;                                 /* GRAIN, gliding (Q15) */
    tx_gd_grain_t g[TX_GD_MAXG];
    uint32_t sync;                               /* the echo's delay from the tempo, samples (0: TIME's) */
} tx_fx_grain_t;
void tx_fx_grain_init(tx_fx_grain_t *d);
void tx_fx_grain_param(tx_fx_grain_t *d, unsigned id, unsigned v);
void tx_fx_grain_sync(tx_fx_grain_t *d, unsigned samples);   /* the echo on the tempo (0: TIME again; at most TIME's top) */
TX_AUDIO_CODE void tx_fx_grain_process(tx_fx_grain_t *d, int32_t *L, int32_t *R, unsigned n);   /* Q23, adds */
int tx_fx_grain_quiet(const tx_fx_grain_t *d);

/* ---- REVERB (on the mix) */
enum { TX_RV_TYPE, TX_RV_MIX, TX_RV_DECAY, TX_RV_TONE, TX_RV_PREDELAY, TX_RV_SIZE, TX_RV_MOD, TX_RV_LOWCUT, TX_RV_SHIMMER,
       TX_RV_SHIFT, TX_RV_NP };
enum { TX_RVT_OFF, TX_RVT_ROOM, TX_RVT_HALL, TX_RVT_INF, TX_RVT_DARK, TX_RVT_DRIFT, TX_RVT_SWELL, TX_RVT_PLATE, TX_RVT_N };   /* (append only) */
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
    int32_t shb[2048];                           /* SHIMMER: the tank's mid at half rate (46 ms window x 2) */
    uint32_t shw, shph;                          /* its write index, the two heads' phase (Q32) */
    int32_t shlp, she, shpk, shg;                /* its input low-pass and remainder, the block's peak, its guard gain */
    int32_t sh[HL_NC];                           /* the shifted mid, into the next control block's input */
    int32_t m[TX_RV_QZ + HL_NC], s[TX_RV_QZ + HL_NC];   /* the TYPE's mid / side (history, then the block) */
    int32_t rm[TX_BLOCK];                        /* ROOM: the send at full rate, gathered over the block */
    uint32_t r0, rn;                             /* ROOM: the gathered run's start and length */
    uint8_t rf[TX_BLOCK / CTL];                  /* ROOM: each control block's fade (0: none) */
    uint32_t rng;                                /* DRIFT: its walks' random numbers (never reseeded) */
    int32_t swe, swr;                            /* SWELL: the send's level (its envelope), the return's root gain (Q15) */
    int32_t pz;                                  /* PLATE's AUTO size: MIX latched while it rings (-1: take MIX now) */
    int32_t pd[TX_PD_LEN];
    union { tx_hall_t h; tx_inf_t i; tx_plate_t p; struct { tx_chamber_t c; tx_hall_t h; } rm; } u;   /* (ROOM: the body + a short hall) */
} tx_fx_reverb_t;
void tx_fx_reverb_init(tx_fx_reverb_t *r);
void tx_fx_reverb_param(tx_fx_reverb_t *r, unsigned id, unsigned v);
TX_AUDIO_CODE void tx_fx_reverb_process(tx_fx_reverb_t *r, int32_t *L, int32_t *R, unsigned n);   /* Q23, adds */
int tx_fx_reverb_quiet(const tx_fx_reverb_t *r);
void tx_fx_reverb_relatch(tx_fx_reverb_t *r);   /* a whole sound loaded (a scene): PLATE's AUTO size from MIX at once */
int tx_fx_reverb_frozen(const tx_fx_reverb_t *r);   /* holding its tail (SUSTAIN / FREEZE): never quiet */

/* ---- the chain: TX_FX_LANES lane inserts and the reverb; the mix calls it through tx_mix_t's hooks */
#define TX_FX_LANES 4u
#define TX_FX_RESERVE_US 880u     /* the effects' share of the block (CPU-BUDGET.md: 500 + the CHAMBER's, now a TYPE) */
typedef struct {
    tx_fx_drive_t drive[TX_FX_LANES];
    tx_drv_shared_t drv;           /* the DRIVE TYPEs' scratch and TSH8's tables (one lane at a time) */
    tx_fx_filter_t filter[TX_FX_LANES];
    tx_fx_grain_t gd;
    tx_fx_reverb_t rev;
    uint32_t reserve_us;           /* the effects reserve (TX_FX_RESERVE_US): the grain delay gets what the reverb leaves */
} tx_fx_t;
void tx_fx_init(tx_fx_t *fx);
TX_AUDIO_CODE void tx_fx_lane(void *fx, unsigned k, int32_t *x, unsigned n);          /* tx_mix_t.ins */
TX_AUDIO_CODE void tx_fx_bus(void *fx, int32_t *L, int32_t *R, unsigned n);           /* tx_mix_t.bus */
int tx_fx_bus_quiet(void *fx);                                                         /* tx_mix_t.busq */
uint32_t tx_fx_lane_us(const tx_fx_t *fx, unsigned k);    /* lane k's inserts now, FM-1 us a block (the governor's
                                                          * tx_gov_fixed: DRIVE tx_fx_drive_us + FILTER) */

#endif
