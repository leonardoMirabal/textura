/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura SCRAPE / ROLL core: the contact force of one sliding or rolling contact, as a series of micro-impacts
 * (an impact-series model, after the classic scraping model).
 *
 *   rub      dense impacts = white noise (one impact per sample);
 *   impacts  sparse ones: exponential gaps (scratch, corr = 0) or amplitudes and gaps that follow an AR(1) series
 *            (roll / grind, corr > 0: a correlated surface); each impact a raised cosine (TX_PULSE_RCOS) of length
 *            N = K A^(-1/5) (Hertz: a harder impact is shorter; F2 nu = 1/5);
 *   contact  the sum through a one-pole low-pass whose cutoff rises with the speed (I1, F2).
 * The engine sets the gains, the mean gap and K from its physics (speed, roughness, ball size); the output (Q23,
 * ADDED) is a force to drive a body (SVF bank, modes). Signal Q15 inside, every product < 2^31. ~15 ops / sample. */
#ifndef TX_CORE_SCRAPE_H
#define TX_CORE_SCRAPE_H
#include <stdint.h>
#include "tx_core_math.h"

typedef struct {
    tx_rng_t rng;
    /* configuration */
    int32_t lpa;                   /* contact low-pass coefficient, Q13 (1 - exp(-2 pi fc / fs)) */
    int32_t rub;                   /* dense path gain, Q15 */
    int32_t imp;                   /* impact path gain, Q15 */
    uint32_t gap_q8;               /* mean samples between impacts, Q8 (0 = no impacts) */
    int32_t corr;                  /* AR(1) correlation of impact amplitudes and gaps, Q15 (0 = scratch) */
    uint32_t kdur_q8;              /* impact length at amplitude 1.0, samples Q8 (>= 2) */
    int32_t amod;                  /* amplitude modulation of the impacts (e.g. out-of-roundness), Q15 (32768 = 1) */
    /* state */
    int32_t lp, lpe, ara, art;     /* low-pass (+ its error feedback), AR states (Q15) */
    uint32_t wait;                 /* samples to the next impact */
    uint32_t pos, step;            /* current impact: Q16 index into the 64-point shape, step per sample */
    int32_t amp;                   /* its amplitude, Q15 */
    uint32_t impacts;              /* since init */
} tx_scrape_t;

void tx_scrape_init(tx_scrape_t *s, uint32_t seed);
void tx_scrape_cutoff(tx_scrape_t *s, uint32_t fc_hz);       /* contact low-pass */
TX_AUDIO_CODE void tx_scrape_add(tx_scrape_t *s, int32_t *out, unsigned n);
static inline int tx_scrape_quiet(const tx_scrape_t *s)
{
    return !s->rub && !s->imp && !s->lp && s->pos >= ((uint32_t)TX_PULSE_N << 16);
}

#endif
