/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura WIND core: turbulent air heard outdoors. Noise is the excitation here (turbulence IS broadband), and every
 * path of it is shaped, coloured and moved by the wind speed u(t): no path runs at a fixed level or colour.
 *
 *   speed     u = U (1 + TI t(t)) + G(t) max(U, 3 m/s), clipped at 0, times the key's envelope. t: five
 *             Ornstein-Uhlenbeck walks (16, 4, 1, 0.25, 0.06 s) weighted toward the slow ones (a -5/3-like spectrum:
 *             more slow motion than fast); TI (turbulence intensity) by GUST, larger in light air (light winds come and
 *             go: a breath, then calm). G: gust and lull events (Poisson, three times as likely for ~3 s after one;
 *             heavy-tailed sizes, smaller in a steady wind; each its own rise 0.2 .. 1 s, decay 0.8 .. 3.5 s and colour
 *             +-0.5 octave, from its own direction; a fifth of them lulls), less their running mean (U stays the mean).
 *   rush      the air over the ground and through the canopy: white -> a high-pass at fc / 4 and a low-pass at
 *             fc = 60 u^0.75 Hz: the band widens and brightens as it blows harder; amplitude ~ u^2, fluttering
 *             +-22 .. 44 % with the small eddies (an OU walk of 40 ms).
 *   rustle    leaves and grass blades flapping: white x a sparse envelope (flaps arriving as a Poisson stream, rate
 *             600 (0.3 + 1.2 FOLIAGE) (u / 5)^2 /s, each a 3 ms decay, sizes uniform squared), high-passed at 400 Hz,
 *             two low-pass poles at 1.5 kHz (u / 4)^0.7 (brighter with speed and with leaves). A few flaps a second
 *             in light air (ticks), thousands in a gale (a seething rush that still flickers). Amplitude ~ u^2.5.
 *   buffet    the pressure bursts of eddies at the listener: low noise (two one-poles at ~90 Hz) x the square of a
 *             fast walk (tau 25 ms) leaning on the gusts, ~ u^2.5: intermittent thumps, as the recordings' 20-120 Hz.
 *   whoosh    gusts passing by: band noise voices (Q 1.2 .. 2.5) whose loudness is a gamma pulse (two poles, 0.2 ..
 *             0.8 s) and whose centre follows the pulse's first pole (brightest while it comes, falling as it passes:
 *             a Doppler-like sweep of 0.6 .. 1.6 octaves). Spawned by the wind (WHOOSH x (0.1 + 0.9 min(1, u / 15))
 *             /s), by half the gusts, and by key presses. Mono: the engine API has one channel.
 *   edge      rarely, a faint edge tone: narrow band noise (Q 25) at St u / d (St 0.2, d 3 .. 15 mm: a twig, a fence
 *             wire), only in strong gusts over 6 m/s, a 0.1 .. 0.35 s pulse, 0.06 .. 0.16 of the rush's gain. Never
 *             steady.
 * Levels at 10 m/s, FOLIAGE 64: within 3 dB (rms over 1/3 octaves, 50 Hz .. 12.7 kHz) of the wind references' mean
 * spectrum; the envelope's modulation (0.02 .. 50 Hz) and kurtosis in their range (txcover: 7 of 8 reached).
 * Integer only, 32-bit products per sample; 64-bit only per block. All state in tx_wind_t. */
#ifndef TX_CORE_WIND_H
#define TX_CORE_WIND_H
#include <stdint.h>
#include "tx_core_math.h"

#define TX_WIND_NW 4u              /* whoosh voices */

typedef struct {                   /* a two-state Chamberlin band (Q14 coefficients) */
    int32_t low, band;
} tx_wsvf_t;

typedef struct {
    tx_rng_t rng;
    tx_wsvf_t f;
    int32_t x1, x2;                /* the gamma pulse: first and second pole, Q19 (x1 starts at 1.0) */
    int32_t k;                     /* its pole coefficient per block, Q16 */
    int32_t lf0, dep;              /* the centre: log2 Hz Q16 at rest, its sweep with x1 (Q16 octaves) */
    int32_t amp;                   /* peak amplitude, Q15 of the rush's gain scale */
    int32_t q;                     /* 1 / Q, Q14 */
    int32_t g;                     /* the gain now (ramped) */
    uint8_t on, pad[3];
} tx_wwhoosh_t;

typedef struct {
    tx_rng_t rng, nr, nl, nb, ne;  /* control; rush, rustle, buffet, edge noise */
    /* the speed */
    int32_t w[5];                  /* the OU walks, Q15 (sd 1) */
    int32_t fast;                  /* the buffeting walk, Q15 */
    int32_t fl[2];                 /* the rush's and the leaves' flutter walks, Q15 */
    int32_t gtgt, gcur, kr, kd;    /* gust/lull events: target, now (Q15 of U), rise and decay per block (Q16) */
    int32_t gmean;                 /* their running mean (Q15) */
    int32_t gcol;                  /* the last gust's colour, Q16 octaves */
    uint32_t clu;                  /* blocks the gusts stay clustered */
    int32_t env;                   /* the key's envelope, Q15 */
    int32_t u;                     /* the speed now, m/s Q12 */
    int32_t lfr;                   /* the rush's centre now, log2 Hz Q16 */
    uint32_t press_wait;           /* blocks a press waits before its gust counts fully again */
    /* controls (set by the engine) */
    int32_t U;                     /* mean speed, m/s Q12 (0: calm) */
    int32_t ti;                    /* turbulence intensity, Q15 */
    int32_t grate;                 /* gust events per block, Q16 */
    int32_t gusty, foliage, buffet, whoosh, edge;   /* 0 .. 127 */
    int32_t scale;                 /* log2 of the frequency scale, Q16 (-1 .. +1 octave) */
    int32_t level;                 /* Q15 */
    uint8_t held, limit;
    /* rush */
    tx_wsvf_t r;                   /* (low: the high-pass state, band: the low-pass) */
    int32_t rg;                    /* gain now */
    /* rustle */
    int32_t le, lhp, llp, llp2, lg;   /* flap envelope (Q15), HP and two LP states, gain now */
    uint32_t lacc;                 /* flap arrivals: Q16 fractional count carried */
    int32_t lnorm;                 /* flap size normalisation, Q15 */
    /* buffet */
    int32_t b1, b2, bg;
    /* whooshes */
    tx_wwhoosh_t wv[TX_WIND_NW];
    /* edge tone */
    tx_wsvf_t e;
    int32_t ex1, ex2, ek, eg, ed_l2, eamp;   /* its pulse (Q19), coefficient, gain now, log2 of the edge size, peak */
    uint8_t eon, pad[3];
    /* bookkeeping */
    uint16_t events, nwh;          /* events this block; whooshes sounding */
    uint32_t total;
} tx_wind_t;

void tx_wind_init(tx_wind_t *w, uint32_t seed);
void tx_wind_press(tx_wind_t *w, uint8_t vel);             /* a key: a gust and a whoosh */
TX_AUDIO_CODE void tx_wind_block(tx_wind_t *w, int32_t *out, unsigned n);   /* ADDS n samples, Q23 */
int tx_wind_quiet(const tx_wind_t *w);

#endif
