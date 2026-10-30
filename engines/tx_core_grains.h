/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura GRAINS: a dense cloud's far events as grains, never a noise bed. Where a source has more events than heroes
 * (a downpour's thousands of drops, a slide's knocks), each event beyond the heroes still arrives as its own impulse at
 * its own time and level, rings a resonator in its band (a bubble's pitch, a splash's body) and decays: the mass keeps
 * the cloud's texture (its level spread, its near and far, its gaps) and its colour, where a noise bed of the same power
 * would read as a flat hiss (the user, 2026-10-25: "a kind of white noise in the background when clustering
 * increases").
 *
 *   bands    up to TX_GR_BANKS resonators, each over a frequency band (lo .. hi) with its Q; each grain moves its
 *            band to a new pitch inside it (log-uniform: the grains' sizes differ), so the mass never rings at one
 *            pitch.
 *   grains   an event: tx_gr_add(offset in the block, frequency, amplitude Q15): it goes to the band that holds its
 *            frequency (else the nearest). At most TX_GR_MAX per block; beyond, an event joins a random earlier one of
 *            its band with a random sign (power adds), so the cost stays flat however dense the cloud.
 *   render   per band, its impulses in time order through the resonator ((1 - z^-2) / 2 band-pass: no DC).
 * Cost: per sample 2 multiplies per sounding band; per event a few operations (each goes into its block's time slot of 8
 * samples: no sorting). Header-only, integer, an instance (~0.9 KB). */
#ifndef TX_CORE_GRAINS_H
#define TX_CORE_GRAINS_H
#include <stdint.h>
#include "tx_core_math.h"
#include "tx_core_insect.h"

#define TX_GR_BANKS 6u
#define TX_GR_MAX 96u
#define TX_GR_SLOTS 32u           /* a block's time slots (8 samples, 0.18 ms: a far grain's onset is placed in its slot) */
#define TX_GR_PITCH 16u           /* pitches per band (coefficients made once: a grain picks one) */

typedef struct {
    tx_ireso_t r[TX_GR_BANKS];
    int16_t c1[TX_GR_BANKS][TX_GR_PITCH], c2[TX_GR_BANKS][TX_GR_PITCH];   /* a1, a2 per pitch, Q14 */
    uint16_t lo[TX_GR_BANKS], hi[TX_GR_BANKS];   /* band, Hz */
    uint16_t q[TX_GR_BANKS];                     /* Q x16 */
    int16_t amp[TX_GR_MAX];
    uint8_t bank[TX_GR_MAX], next[TX_GR_MAX];   /* a grain's band; the next grain in its slot (0xFF: none) */
    uint8_t head[TX_GR_SLOTS];                   /* each slot's first grain (0xFF: none) */
    uint8_t n, nb, pad[2];
} tx_grains_t;

static inline void tx_gr_init(tx_grains_t *g)
{
    uint8_t *b = (uint8_t *)g;
    unsigned i;
    tx_zero(b, sizeof *g);
    for (i = 0; i < TX_GR_SLOTS; i++) g->head[i] = 0xFF;
}

/* band i: lo .. hi Hz, Q (x16), output gain (Q8) */
static inline void tx_gr_band(tx_grains_t *g, unsigned i, uint32_t lo, uint32_t hi, uint32_t q_x16, int32_t gain_q8)
{
    if (i >= TX_GR_BANKS) return;
    g->lo[i] = (uint16_t)lo; g->hi[i] = (uint16_t)hi; g->q[i] = (uint16_t)q_x16;
    g->r[i].g = gain_q8 * 4;                                 /* (the grains go in at a quarter: headroom for a pile) */
    g->r[i].zlim = 16;
    {   /* the band's pitches, log-spaced lo .. hi (a grain picks one: no per-grain trigonometry) */
        int32_t l0 = (32 << 16) - (int32_t)(tx_neglog2_q12(lo ? lo : 1u) << 4);
        int32_t l1 = (32 << 16) - (int32_t)(tx_neglog2_q12(hi ? hi : 1u) << 4);
        unsigned k;
        for (k = 0; k < TX_GR_PITCH; k++) {
            uint32_t f = tx_exp2(l0 + (int32_t)(((l1 - l0) * (int32_t)(2u * k + 1u)) / (int32_t)(2u * TX_GR_PITCH)));
            tx_ireso_set(&g->r[i], f, q_x16);
            g->c1[i][k] = (int16_t)g->r[i].a1; g->c2[i][k] = (int16_t)g->r[i].a2;
        }
    }
    if (i >= g->nb) g->nb = (uint8_t)(i + 1u);
}

/* a grain at offset off (< the block), frequency f_hz, amplitude Q15 (signed); the list stays in time order */
static inline void tx_gr_add(tx_grains_t *g, tx_rng_t *r, unsigned off, uint32_t f_hz, int32_t amp)
{
    unsigned b = 0, i, best = 0xFFFFFFFFu;
    if (!g->nb || !amp) return;
    for (i = 0; i < g->nb; i++) {                            /* its band (else the nearest) */
        unsigned d = f_hz < g->lo[i] ? g->lo[i] - f_hz : f_hz > g->hi[i] ? f_hz - g->hi[i] : 0u;
        if (d < best) { best = d; b = i; }
    }
    amp = amp > 32767 ? 32767 : amp < -32767 ? -32767 : amp;
    amp /= 4;
    if (off >= TX_BLOCK) off = TX_BLOCK - 1u;
    if (g->n < TX_GR_MAX) {                                  /* into its time slot */
        unsigned sl = off * TX_GR_SLOTS / TX_BLOCK;
        i = g->n++;
        g->amp[i] = (int16_t)amp; g->bank[i] = (uint8_t)b;
        g->next[i] = g->head[sl]; g->head[sl] = (uint8_t)i;
    } else {                                                 /* full: joins an earlier one (power adds) */
        unsigned k = tx_rand_below(r, TX_GR_MAX);
        int32_t x = g->amp[k] + ((tx_rand(r) & 1u) ? amp : -amp);
        g->amp[k] = (int16_t)(x > 32767 ? 32767 : x < -32767 ? -32767 : x);
    }
}

/* the block: each band with grains moves to a new pitch in its range; its grains in time order through it, into out */
TX_AUDIO_CODE static inline void tx_gr_render(tx_grains_t *g, tx_rng_t *r, int32_t *out, unsigned n)
{
    unsigned j, b, sl;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (b = 0; b < g->nb; b++) {
        tx_ireso_t *q = &g->r[b];
        int32_t y1 = q->y1, y2 = q->y2, p1 = q->p1, p2 = q->p2, a1 = q->a1, a2 = q->a2, gg = q->g, y01 = y1, y02 = y2, zl;
        unsigned has = 0, i;
        for (i = 0; i < g->n; i++) if (g->bank[i] == b) { has = 1; break; }
        if (!has && !(y1 | y2 | p1 | p2)) continue;
        j = 0;
        for (sl = 0; sl <= TX_GR_SLOTS; sl++) {              /* slot by slot: a tight loop to the slot's start, then */
            unsigned end = sl < TX_GR_SLOTS ? sl * n / TX_GR_SLOTS : n;   /* its grains of this band at once */
            int32_t x = 0;
            unsigned k;
            for (; j < end && (p1 | p2); j++) {             /* (the two samples after a grain: its input's tail) */
                int32_t y = ((a1 * y1) >> 14) - ((a2 * y2) >> 14) - (p2 >> 1);
                y = y > 65535 ? 65535 : y < -65535 ? -65535 : y;
                p2 = p1; p1 = 0;
                y2 = y1; y1 = y;
                out[j] += y * gg;
            }
            for (; j < end; j++) {                           /* (then the ring alone) */
                int32_t y = ((a1 * y1) >> 14) - ((a2 * y2) >> 14);
                y = y > 65535 ? 65535 : y < -65535 ? -65535 : y;
                y2 = y1; y1 = y;
                out[j] += y * gg;
            }
            if (sl == TX_GR_SLOTS || !has) { if (sl < TX_GR_SLOTS) continue; break; }
            for (k = g->head[sl]; k != 0xFFu; k = g->next[k]) if (g->bank[k] == b) {
                unsigned pk = tx_rand(r) >> 28;                  /* each grain its own pitch in the band */
                a1 = g->c1[b][pk]; a2 = g->c2[b][pk];
                x += g->amp[k];
            }
            if (x && j < n) {
                int32_t y = ((a1 * y1) >> 14) - ((a2 * y2) >> 14) + ((x - p2) >> 1);
                y = y > 65535 ? 65535 : y < -65535 ? -65535 : y;
                p2 = p1; p1 = x;
                y2 = y1; y1 = y;
                out[j] += y * gg;
                j++;
            }
        }
        zl = q->zlim ? q->zlim : 8;                          /* silent: small or frozen -> exact zero */
        if (!has && !(p1 | p2) && ((y1 > -zl && y1 < zl && y2 > -zl && y2 < zl) || (y1 == y01 && y2 == y02))) y1 = y2 = 0;
        q->y1 = y1; q->y2 = y2; q->p1 = p1; q->p2 = p2; q->a1 = a1; q->a2 = a2;
    }
    for (sl = 0; sl < TX_GR_SLOTS; sl++) g->head[sl] = 0xFF;
    g->n = 0;
}

static inline int tx_gr_quiet(const tx_grains_t *g)
{
    unsigned b;
    if (g->n) return 0;
    for (b = 0; b < g->nb; b++) if (!tx_ireso_quiet(&g->r[b])) return 0;
    return 1;
}

#endif
