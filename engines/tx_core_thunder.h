/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura THUNDER core: one lightning flash heard from a distance.
 *
 *   channel   a tortuous return-stroke channel grown downward in steps of 16 m (after the classic tortuous-channel models and
 *             Hill: each step a cone deflection theta' ~ N(-Theta / 20, 27.08 deg x TORT), phi' ~ N(+-Psi / 20, 45 deg)
 *             around the memory orientation Theta = the mean of the last 4 theta', Psi = a slow mean of the phi'), from
 *             a start height to the ground (the channel model: 8 m steps; the deflection does not depend on the step). Grown over
 *             the first blocks after the key (GEN steps per block), then it plays.
 *   segments  every step is a line source: its N-wave reaches the listener after r / c. Seen side-on, all its points
 *             arrive together (a clap); seen end-on they spread over l |cos a| / c (the Wright-Medendorp wave's
 *             smearing): each segment is a box of that width and constant area, written as two steps into a derivative
 *             buffer (O(1) per segment) and integrated exactly in integers. Level ~ 1 / r to 3.6 km, 1 / r^2 beyond.
 *   N-wave    the summed excitation through one N-wave of half-length T: an odd ramp +1 .. -1 over 2L = 2 T fs samples,
 *             computed recursively (S1 = sum x, S2 = sum k x over the window; y = S1 - 2 S2 / (2L - 1)): O(1) per
 *             sample. T = 0.33 / f_m (Few: f_m = 0.63 c0 sqrt(P0 / E_l)) at 1 km, lengthened with distance as
 *             sqrt(1 + 0.524 ln(r / 10 m)) (E2). The excitation is bounded to 13 bits so S2 fits int32.
 *   air       two one-pole low-passes at f_c = 1 kHz (0.6 km / r)^(2/3) (air absorption ~5 dB/km at 1 kHz, ours).
 *   strokes   a flash may restrike the same channel (E2: 40-50 ms apart, 10-240): up to TX_TH_STROKES, each re-emits the
 *             segments with its own start and gain.
 * State ~7 KB per flash. Arrival times are relative to the first arrival (the flash-to-bang delay is not waited). */
#ifndef TX_CORE_THUNDER_H
#define TX_CORE_THUNDER_H
#include <stdint.h>
#include "tx_core_math.h"

#define TX_TH_SEGS 640u            /* channel steps (16 m: up to 10 km of channel) */
#define TX_TH_LMAX 640u            /* N-wave window 2L, samples */
#define TX_TH_SMEAR 255u           /* the longest segment box, samples */
#define TX_TH_STROKES 6u          /* return strokes and terrain echoes (each re-emits the channel) */
#define TX_TH_GEN 48u              /* channel steps grown per block (640 steps: ~77 ms before the first sound) */

typedef struct {
    uint32_t seg_t[TX_TH_SEGS];    /* arrival (samples after the first) << 8 | box width (1 .. 255) */
    int16_t seg_a[TX_TH_SEGS];     /* box area (excitation units) */
    int16_t dl[TX_TH_LMAX];        /* the N-wave window's history of x */
    int32_t d[TX_BLOCK + TX_TH_SMEAR + 1];   /* derivative of the excitation (the boxes' edges), with carry */
    uint32_t st_t[TX_TH_STROKES];  /* stroke starts on the flash clock */
    int16_t st_g[TX_TH_STROKES];   /* stroke gains, Q15 */
    uint8_t nst, active, gen_done, necho;   /* strokes (echoes among them), ...; echoes added */
    /* the channel grower */
    int32_t px, py, pz;            /* tip position, cm */
    int32_t ux, uy, uz;            /* step direction, Q14 (z up) */
    int32_t th[4];                 /* the last 4 deflections theta' (phase units >> 8) */
    int32_t psi;                   /* the azimuth memory (phase units >> 8) */
    int32_t lx;                    /* listener: (lx, 0, 150) cm */
    uint32_t tmin, tmax;           /* first / last arrival (absolute until the channel is done) */
    int32_t sd_q16;                /* deflection sd, phase units >> 16 */
    int32_t area0;                 /* area at 1 km */
    uint16_t nseg, dlpos, l2;      /* segments, window position, window length 2L */
    int32_t recip;                 /* 2 / (2L - 1), Q15 */
    /* playback */
    uint32_t clock;                /* samples since the channel was done */
    int32_t x, s1, s2;             /* excitation and the window sums */
    int32_t lp1, lp2, lpa;         /* air low-passes (Q15 coefficient) */
    int32_t out_g;                 /* output multiplier (calibrated by render) */
    int32_t env, n1, n2;           /* the rolling tail: the output's envelope (slow release), its low noise */
    uint32_t nr;                   /* (the tail's noise state) */
    uint32_t st_end;               /* the latest stroke's start (the flash ends after it) */
    int32_t trel, tgain, ta;       /* the tail by distance: release (Q12 of 1 / 2^12), gain (Q8), noise pole (Q15) */
    uint32_t aref, knee;           /* the excitation's distance scale (m: the flash's, 1 km at least), the 1 / r^2 knee */
    int32_t oatt;                  /* the distance's loss past aref, at the output (Q15, 32768: none) */
    int32_t epk;                   /* the tail's envelope peak (the roll ends some dB under it) */
} tx_flash_t;

/* start a flash: distance (m), start height (m), tortuosity (Q8, 256 = the channel model), energy per length E_l (log2 of
 * E_l / 1e4 J/m, Q16, 0 .. 6.64), level (Q15) */
void tx_flash_start(tx_flash_t *f, tx_rng_t *r, uint32_t dist_m, uint32_t height_m, uint32_t tort_q8, int32_t el_l2,
                    int32_t level_q15);
void tx_flash_restrike(tx_flash_t *f, int32_t gain_q15);      /* another stroke now (if room) */
void tx_flash_echo(tx_flash_t *f, uint32_t delay, int32_t gain_q15);   /* the channel again from the terrain, delay samples
                                                              * after the first arrival (if room) */
TX_AUDIO_CODE void tx_flash_add(tx_flash_t *f, tx_rng_t *r, int32_t *out, unsigned n);
static inline int tx_flash_active(const tx_flash_t *f) { return f->active; }
/* the direct sound's sample now, as tx_flash_add outputs it (the air's states are x 64; the distance's loss): for a
 * declick when a flash is cut */
static inline int32_t tx_flash_now(const tx_flash_t *f)
{
    int32_t o = f->lp2 * (f->out_g >> 6);
    return ((o >> 15) * f->oatt) + (((o & 0x7FFF) * f->oatt) >> 15);
}

#endif
