/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura effects (stage S3): instances, no globals (as the engines), integers only.
 *
 *   per lane: no insert (tx_fx_lane: the lane untouched; each lane's FILTER is retired, the mix has one).
 *   on the stereo mix: the DRIVE first (tx_fx_drive_mix: one DRIVE, a pair of instances for L and R with the same
 *   settings; tx_mix_t.pre, before the mix's compressor; AMOUNT 0 = the mix untouched), the FILTER after it (a band:
 *   LOW CUT, HIGH CUT; both at their ends = the mix untouched), then (tx_core_mix.c) the
 *   compressor and the CHAMBER, then, before the limiter, in series, the GRAIN DELAY (a delay whose repeats are
 *   grained) and the REVERB (a mono send of the mid, a stereo return), each with a dry / wet MIX (below): the reverb
 *   takes what the delay gives (GRAIN MIX 127 + REVERB MIX 127: the grains' reverb alone):
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

/* ---- MIX, dry / wet (the DRIVE, the GRAIN DELAY, the REVERB): 0 the dry alone .. 127 the wet alone. The GRAIN DELAY
 * and the REVERB crossfade at equal power (dry x cos, wet x sin of MIX's place): their wet parts are delayed (echoes,
 * a tail), so they add to the dry in power, and the sum holds its level through the middle (-3 dB each at 64): no
 * dip, no bump. The DRIVE's wet is the dry signal shaped, in phase with it: a straight crossfade (tx_fx_drive.c).
 * tx_fx_eqp(m) = sin(m pi / 2), m and the gain Q15 (0 .. 32768; 129 points, straight between them). */
extern const uint16_t tx_fx_eqp_tab[129];
TX_INLINE int32_t tx_fx_eqp(int32_t m)
{
    int32_t i, a;
    if (m <= 0) return 0;
    if (m >= 32768) return 32768;
    i = m >> 8; a = tx_fx_eqp_tab[i];
    return a + (((tx_fx_eqp_tab[i + 1] - a) * (m & 255)) >> 8);
}
#define TX_FX_MIXQ(v) ((v) >= 127 ? 32768 : (int32_t)(v) * 258)   /* MIX 0 .. 127 -> its place, Q15 */
/* MIX's gains along a block (in flash: TX_FAR from the RAM code): its place *x glides toward t, by step at most a
 * point; at each of the np + 1 points (the first: where it starts) the sin into gs, the cos into gc (0: not wanted) */
void tx_fx_mix_glide(int32_t *x, int32_t t, int32_t step, unsigned np, int32_t *gs, int32_t *gc);
/* the blend, n samples in place: L, R = dry x d + wet (wl, wr; 0: none) x w x k, the gains (Q15) straight from d0, w0
 * to d1, w1 (in RAM on the FM-1) */
TX_AUDIO_CODE void tx_fx_drywet(int32_t *L, int32_t *R, const int32_t *wl, const int32_t *wr, unsigned n, int32_t d0,
                                int32_t d1, int32_t w0, int32_t w1, int32_t k);

/* ---- the lane inserts: none now (each lane's FILTER retired: one FILTER on the mix, below). tx_fx_filter_t and
 * tx_fx_filter_param stay as empty stubs only while firmware/src/lanes.c still names them (its LN_FX_FILTER, never
 * sent any more); they go with that line */
typedef struct { uint8_t none; } tx_fx_filter_t;
static inline void tx_fx_filter_param(tx_fx_filter_t *f, unsigned id, unsigned v) { (void)f; (void)id; (void)v; }

/* ---- DRIVE (one, on the mix: a pair of these, L and R): Felucca's track drive as Frodhes' classic FDRV (TYPE 0) and Frodhes' four circuit TYPEs
 * (tx_fx_drive.c, tx_fx_drv_<type>.c; their state: tx_fx_drv.h) */
enum { TX_DR_AMOUNT, TX_DR_TONE, TX_DR_MIX, TX_DR_TYPE, TX_DR_NP };   /* (append only) */
extern const tx_param_t tx_fx_drive_params[TX_DR_NP];
#define TX_FX_US_FDRV 85u          /* the classic drive's FM-1 cost a block (make tx-budget section 4) */
typedef struct {
    uint8_t p[TX_DR_NP];
    uint8_t ton, on, la;           /* the shelf runs; the drive runs (or glides out); the last AMOUNT */
    uint8_t cur;                   /* the TYPE that runs (p[TX_DR_TYPE] after a change has faded through) */
    int32_t hp, lp1, lp2, w, tg, tlp;
    tx_drv_shared_t *sh;           /* what the TYPEs share (tx_fx_t; 0: the classic drive only) */
    const void *own;               /* TSH8's owner as this one (itself; the pair's second: the first's, tx_fx_drive_pair) */
    uint8_t fol;                   /* 1: the pair's second (TSH8: runs on the tables the first builds) */
    uint8_t sw;                    /* (tx_fx_drive_prep: the TYPE that runs goes on; 0: it fades out) */
    tx_drv_state_t u;              /* the TYPE's state */
} TX_LINE tx_fx_drive_t;   /* (TX_LINE: its own cache lines on the FM-1) */
void tx_fx_drive_init(tx_fx_drive_t *d);
void tx_fx_drive_param(tx_fx_drive_t *d, unsigned id, unsigned v);
TX_AUDIO_CODE void tx_fx_drive_process(tx_fx_drive_t *d, int32_t *x, unsigned n);     /* mono Q23, in place */
/* or in two halves (the lanes on two cores): prep for every lane in lane order, then run on any core with its own
 * scratch (w; 0: the shared one): the same samples */
TX_AUDIO_CODE void tx_fx_drive_prep(tx_fx_drive_t *d);
TX_AUDIO_CODE void tx_fx_drive_run(tx_fx_drive_t *d, int32_t *x, unsigned n, tx_drv_scr_t *w);
void tx_fx_drive_shared_init(tx_drv_shared_t *sh);
void tx_fx_drive_share(tx_fx_drive_t *d, tx_drv_shared_t *sh);
void tx_fx_drive_pair(tx_fx_drive_t *second, const tx_fx_drive_t *first);   /* one DRIVE on a stereo pair: second
                                                 * takes TSH8 with first (both run with the same settings, first first) */
uint32_t tx_fx_drive_us(const tx_fx_drive_t *d);  /* its FM-1 cost now, us a block (0: off) */

/* ---- FILTER (the mix's, right after the DRIVE; tx_fx_mfilter.c): a high-pass then a low-pass on the pair, LOW CUT and
 * HIGH CUT the edges of a band; both at their ends (the defaults) = the mix untouched */
enum { TX_MF_LOW, TX_MF_HIGH, TX_MF_RESO, TX_MF_SLOPE, TX_MF_NP };   /* (append only) */
extern const tx_param_t tx_fx_mfilter_params[TX_MF_NP];
#define TX_FX_US_MF_STAGE 140u     /* an edge's stage on the pair, FM-1 us a block (make tx-budget section 4) */
typedef struct {
    uint8_t p[TX_MF_NP];
    int32_t pos[2];                /* the edges' places, gliding (0 .. 65535: 30 Hz .. 16 kHz): LOW CUT, HIGH CUT */
    int32_t w[2];                  /* each edge's share (Q15: 0 off .. 32768 on; 46 ms fades) */
    int32_t w2;                    /* the 24 dB stages' share */
    int32_t s[2][2][2];            /* the stages' integrators: [edge][side][s1, s2] */
    int32_t s2[2][2][2];           /* the 24 dB stages' */
} tx_fx_mfilter_t;
void tx_fx_mfilter_init(tx_fx_mfilter_t *f);
void tx_fx_mfilter_param(tx_fx_mfilter_t *f, unsigned id, unsigned v);
void tx_fx_mfilter_process(tx_fx_mfilter_t *f, int32_t *L, int32_t *R, unsigned n);   /* Q23, in place (in XIP) */
uint32_t tx_fx_mfilter_us(const tx_fx_mfilter_t *f);   /* its FM-1 cost now, us a block (0: off) */

/* ---- GRAIN DELAY (on the mix, before the reverb; tx_fx_grain.c): a delay whose repeats are grained */
enum { TX_GD_MIX, TX_GD_TIME, TX_GD_FEEDBACK, TX_GD_GRAINS, TX_GD_SIZE, TX_GD_SPRAY, TX_GD_PITCH, TX_GD_REVERSE,
       TX_GD_SPREAD, TX_GD_TONE, TX_GD_GRAIN, TX_GD_FREEZE, TX_GD_NP };
extern const tx_param_t tx_fx_grain_params[TX_GD_NP];
#define TX_GD_MAXG 30u                           /* (GRAINS 1 .. 30) */
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
    int32_t mix;                                 /* MIX's place, gliding (Q15: 0 dry .. 32768 wet) */
    int32_t gbl;                                 /* GRAIN, gliding (Q15) */
    int32_t gc, gct, gcr;                        /* the loop's make-up (Q12: 4096 = none), its target, the measured one */
    int32_t sae, saf;                            /* the echo's and the loop's level (|x| / 4 summed a block, smoothed) */
    int32_t hot, slv;                            /* FEEDBACK past 1: the share of its excess let through (Q12; under 0
                                                  * down to -0.75 dB a pass), the loop's peak as heard (Q22, falling back
                                                  * over ~3 s) */
    tx_gd_grain_t g[TX_GD_MAXG];
    uint32_t sync;                               /* the echo's delay from the tempo, samples (0: TIME's) */
    uint8_t ahead;                               /* (begin: the voices run before the input, this block) */
    uint32_t vact, vact2;                        /* (the voices: grain samples sounding this block; the second half's) */
    uint8_t vk;                                  /* (tx_fx_grain_split: the first half's voice slots; 0: one pass) */
    uint32_t rng2;                               /* (the second half's rng: where the first half's ends) */
    int32_t we[TX_BLOCK], wl[TX_BLOCK], wr[TX_BLOCK];   /* (the voices' block: the echo, the grains L, R) */
    int32_t wl2[TX_BLOCK], wr2[TX_BLOCK];        /* (the second half's grains, L, R) */
} TX_LINE tx_fx_grain_t;   /* (TX_LINE: its own cache lines on the FM-1) */
void tx_fx_grain_init(tx_fx_grain_t *d);
void tx_fx_grain_param(tx_fx_grain_t *d, unsigned id, unsigned v);
void tx_fx_grain_sync(tx_fx_grain_t *d, unsigned samples);   /* the echo on the tempo (0: TIME again; at most TIME's top) */
TX_AUDIO_CODE void tx_fx_grain_process(tx_fx_grain_t *d, int32_t *L, int32_t *R, unsigned n);   /* Q23, adds */
/* or in three steps (the voices ahead of the input, on another core): begin (1: the voices may run now), voices, end
 * (it runs the voices itself when begin said 0): the same samples */
TX_AUDIO_CODE int tx_fx_grain_begin(tx_fx_grain_t *d, unsigned n);
TX_AUDIO_CODE void tx_fx_grain_voices(tx_fx_grain_t *d, unsigned n);
/* or the voices in two halves at once (two cores): split (one core, first), then voices_a and voices_b, then end */
TX_AUDIO_CODE void tx_fx_grain_split(tx_fx_grain_t *d, unsigned n);
TX_AUDIO_CODE void tx_fx_grain_voices_a(tx_fx_grain_t *d, unsigned n);
TX_AUDIO_CODE void tx_fx_grain_voices_b(tx_fx_grain_t *d, unsigned n);
TX_TWIN(tx_fx_grain_voices_a);
TX_TWIN(tx_fx_grain_voices_b);
TX_AUDIO_CODE void tx_fx_grain_end(tx_fx_grain_t *d, int32_t *L, int32_t *R, unsigned n);
TX_TWIN(tx_fx_grain_end);         /* (the bus one block behind on core 1: tx_fx_bus_end_c1) */
int tx_fx_grain_quiet(const tx_fx_grain_t *d);

/* ---- REVERB (on the mix) */
enum { TX_RV_TYPE, TX_RV_MIX, TX_RV_DECAY, TX_RV_TONE, TX_RV_PREDELAY, TX_RV_SIZE, TX_RV_MOD, TX_RV_LOWCUT, TX_RV_SHIMMER,
       TX_RV_SHIFT, TX_RV_NP };
enum { TX_RVT_OFF, TX_RVT_ROOM, TX_RVT_HALL, TX_RVT_INF, TX_RVT_DARK, TX_RVT_DRIFT, TX_RVT_SWELL, TX_RVT_PLATE, TX_RVT_N };   /* (append only) */
extern const tx_param_t tx_fx_reverb_params[TX_RV_NP];
#define TX_PD_LEN 3536u            /* the pre-delay's line: its longest tap (80 ms, 3528) + 1, to 8 (int32: the send has
                                    * headroom) */
typedef struct {
    uint8_t p[TX_RV_NP];
    uint8_t cur, fade, running, pd_on;
    int32_t off, damp, dk;                       /* DECAY's size offset, TONE's damping, DECAY for the kernels (glide) */
    int32_t hp, he, lc, lca, lcv;                /* the kernels' DC blocker; LOW CUT */
    uint32_t pd_w, pd_d, pd_d0, pd_x, pd_fill;
    int32_t inb[CTL];                            /* the send, a control block */
    int32_t x[HL_NC];
#define SHN 2056u                                /* (the span's 2048 + the interpolation's 2 + the head's sample) */
    int32_t shb[SHN];                            /* SHIMMER: the tank's mid (46 ms window + its reach) */
    uint32_t shw, shph;                          /* its write index, the two heads' phase (Q32) */
    int32_t shlp, she, shpk, shg;                /* its input low-pass and remainder, the block's peak, its guard gain */
    int32_t sh[HL_NC];                           /* the shifted mid, into the next control block's input */
    int32_t m[HL_NC], s[HL_NC];                  /* the TYPE's mid / side, a control block */
    uint32_t r0, rn;                             /* ROOM: the gathered run's start and length */
    uint8_t rf[TX_BLOCK / CTL];                  /* ROOM: each control block's fade (0: none) */
    uint32_t rng;                                /* DRIFT: its walks' random numbers (never reseeded) */
    int32_t swe, swr;                            /* SWELL: the send's level (its envelope), the return's root gain (Q15) */
    int32_t pz;                                  /* PLATE's AUTO size: MIX latched while it rings (-1: take MIX now) */
    int32_t xd, xw;                              /* MIX's place for the dry (TYPE OFF: 0, the dry whole) and for the
                                                  * wet, gliding (Q15) */
    int32_t gw[TX_BLOCK / CTL + 1u];             /* the wet's gain at each control block's edge, this block (Q15) */
    int32_t pd[TX_PD_LEN];
    /* ROOM on two cores (bit-exact): the block's sends first (sv; each control block's
     * SIZE and TONE glide in svo / svd), then the body (the CHAMBER, tx_fx_reverb_body: its wet part into rw) on the
     * other core while this one runs the tail; body_run hands it over (1: taken), body_wait waits (1: lost, run it
     * here). 0: one core. Set by the app after tx_fx_init. */
    int (*body_run)(void);
    int (*body_wait)(void);
    tx_rv_par_t par;                             /* the kernels' own halves on the other core (0: one core; the app) */
    /* the TYPE's state. ROOM: the body, a short hall, and its block's buffers (only ROOM uses them: in its union
     * member): rm the send at full rate, gathered over the block (the other core's body works on it in place), rw the
     * body's wet part (written by the other core), sv / svo / svd the sends (two cores); each on its own cache lines */
    union {
        struct { tx_hq_t q; int16_t mem[HQ_HALL_WORDS]; } hq;   /* HALL: the HQ tank at its full scale */
        tx_inf_t i; tx_plate_t p;
        struct {
            tx_chamber_t c; tx_hall_t h TX_LINE;
            int32_t rm[TX_BLOCK] TX_LINE, rw[TX_BLOCK] TX_LINE, sv[TX_BLOCK] TX_LINE;
            int32_t svo[TX_BLOCK / CTL], svd[TX_BLOCK / CTL];
        } rm;
    } u TX_LINE;
} tx_fx_reverb_t;
void tx_fx_reverb_init(tx_fx_reverb_t *r);
void tx_fx_reverb_param(tx_fx_reverb_t *r, unsigned id, unsigned v);
TX_AUDIO_CODE void tx_fx_reverb_process(tx_fx_reverb_t *r, int32_t *L, int32_t *R, unsigned n);   /* Q23, adds */
TX_AUDIO_CODE void tx_fx_reverb_body(tx_fx_reverb_t *r);   /* ROOM's body on the other core (body_run's job) */
int tx_fx_reverb_quiet(const tx_fx_reverb_t *r);
void tx_fx_reverb_relatch(tx_fx_reverb_t *r);   /* a whole sound loaded (a scene): PLATE's AUTO size from MIX at once */
int tx_fx_reverb_frozen(const tx_fx_reverb_t *r);   /* holding its tail (SUSTAIN / FREEZE): never quiet */

/* ---- the chain: TX_FX_LANES lane inserts, the mix's DRIVE, the grain delay and the reverb; the mix calls it through
 * tx_mix_t's hooks */
#define TX_FX_LANES 4u
#define TX_FX_RESERVE_US 880u     /* the effects' share of the block (CPU-BUDGET.md: 500 + the CHAMBER's, now a TYPE) */
typedef struct {
    tx_fx_drive_t drive[2];        /* the mix's DRIVE: L, R (one set of settings: tx_fx_drive_set) */
    tx_drv_shared_t drv;           /* the DRIVE TYPEs' scratch and TSH8's tables */
    tx_drv_scr_t *scr1;            /* the other core's DRIVE scratch (the caller's, free while the DRIVE runs; 0: the
                                    * pair's TYPEs stay on one core, the classic drive needs none) */
    int32_t *dr_x;                 /* (tx_fx_drive_mix: the side the other core drives this block) */
    unsigned dr_n;
    tx_fx_mfilter_t mf;            /* the mix's FILTER, after the DRIVE (tx_fx_drive_set: TX_FX_MF_ID + TX_MF_*) */
    tx_fx_filter_t filter[TX_FX_LANES];   /* (retired: empty stubs, lanes.c's) */
    tx_fx_grain_t gd;
    tx_fx_reverb_t rev;
    uint32_t reserve_us;           /* the effects reserve (TX_FX_RESERVE_US): the grain delay gets what the reverb leaves */
    int32_t room_us;               /* < 0: the cap from the reserve and the cost model (the host); else the grain delay's
                                    * measured room this block, FM-1 us (the device: what the interrupt's target leaves) */
    uint8_t up;                    /* (measured room: blocks since the cap last rose) */
    uint8_t two;                   /* (the cost model) 1: the FM-1's two cores, the grains on the second: the cap is
                                    * every grain asked (the device trims them only on a busy block or on one core) */
} tx_fx_t;
void tx_fx_init(tx_fx_t *fx);
TX_AUDIO_CODE void tx_fx_lane(void *fx, unsigned k, int32_t *x, unsigned n);          /* tx_mix_t.ins (no insert now:
                                                 * the lane untouched) */
/* or on the core (0, 1) that renders the lane (the lanes on two cores): the same samples */
TX_AUDIO_CODE void tx_fx_lane_run(void *fx, unsigned k, int32_t *x, unsigned n, unsigned core);
/* the mix's first stage, DRIVE then FILTER: TX_DR_* the DRIVE on both sides, TX_FX_MF_ID + TX_MF_* the FILTER */
#define TX_FX_MF_ID 16u
void tx_fx_drive_set(tx_fx_t *fx, unsigned id, unsigned v);
TX_AUDIO_CODE void tx_fx_drive_mix(void *fx, int32_t *L, int32_t *R, unsigned n);    /* tx_mix_t.pre: L, R in place, the
                                                 * DRIVE then the FILTER (with rev.par set: the DRIVE's R on the other
                                                 * core, L here; the same samples) */
uint32_t tx_fx_drive_mix_us(const tx_fx_t *fx);   /* the mix's DRIVE and FILTER now, FM-1 us a block (0: off) */
TX_AUDIO_CODE void tx_fx_bus(void *fx, int32_t *L, int32_t *R, unsigned n);           /* tx_mix_t.bus */
/* or split (the grains ahead, on another core, while the lanes render): begin (the cap, then 1: the grain voices may
 * run now: tx_fx_grain_voices(&fx->gd, n)), then the lanes and the mix, then end (as tx_mix_t.bus): the same samples */
TX_AUDIO_CODE int tx_fx_bus_begin(void *fx, unsigned n);
TX_AUDIO_CODE void tx_fx_bus_end(void *fx, int32_t *L, int32_t *R, unsigned n);
TX_AUDIO_CODE void tx_fx_bus_end_c1(void *fx, int32_t *L, int32_t *R, unsigned n);   /* (tx_mix_t.bus1: as core 1
                                   * runs the bus one block behind; the reverb's hooks then hand nothing on) */
TX_AUDIO_CODE void tx_fx_drive_mix_c1(void *fx, int32_t *L, int32_t *R, unsigned n);   /* (tx_mix_t.pre1: the same) */
TX_AUDIO_CODE uint32_t tx_fx_gd_dev_us(const tx_fx_t *fx, uint32_t cap);   /* the grain delay at cap grains, FM-1 us */
TX_AUDIO_CODE void tx_fx_gd_cap(tx_fx_t *fx);     /* the grain delay's cap for this block (tx_fx_bus calls it; an app
                                                   * that times the grains and the reverb apart calls it, then each) */
int tx_fx_bus_quiet(void *fx);                                                         /* tx_mix_t.busq */
uint32_t tx_fx_lane_us(const tx_fx_t *fx, unsigned k);    /* lane k's insert now, FM-1 us a block (the governor's
                                                          * tx_gov_fixed: 0, none) */

#endif
