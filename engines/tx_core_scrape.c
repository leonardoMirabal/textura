/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura SCRAPE / ROLL core: impact-series contact force (tx_core_scrape.h). */
#include "tx_core_scrape.h"

#define DONE ((uint32_t)TX_PULSE_N << 16)

void tx_scrape_init(tx_scrape_t *s, uint32_t seed)
{
    uint8_t *b = (uint8_t *)s;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_rng_seed(&s->rng, seed);
    s->pos = DONE;
    s->kdur_q8 = 2u << 8;
    s->amod = 32768;
    tx_scrape_cutoff(s, 2000);
}

void tx_scrape_cutoff(tx_scrape_t *s, uint32_t fc_hz)
{
    if (fc_hz > 18000u) fc_hz = 18000u;
    if (fc_hz < 10u) fc_hz = 10u;
    s->lpa = 8192 - (int32_t)tx_exp2((13 << 16) - (int32_t)(fc_hz * 862u >> 6));   /* Q13 */
    if (s->lpa < 1) s->lpa = 1;
}

/* a triangular draw in -1 .. 1 (Q15): the sum of two uniforms, a cheap stand-in for F2's Gaussian */
static int32_t tri(tx_rng_t *r) { uint32_t x = tx_rand(r); return ((int32_t)(x >> 17) + (int32_t)(x & 0x7FFF)) - 32768; }

/* the next impact: amplitude and gap (scratch: |tri| and exponential; roll: AR(1) series), length K A^-1/5 */
static void spawn(tx_scrape_t *s)
{
    int32_t a, g;
    uint32_t gap, l, dur;
    if (s->corr) {
        int32_t c = s->corr, w = 32768 - (int32_t)(((uint32_t)c * (uint32_t)c) >> 16);   /* ~sqrt(1 - c^2) */
        s->ara = ((s->ara * (c >> 1)) >> 14) + ((tri(&s->rng) * (w >> 1)) >> 14);
        s->art = ((s->art * (c >> 1)) >> 14) + ((tri(&s->rng) * (w >> 1)) >> 14);
        a = 16384 + (s->ara >> 1);                           /* around 0.5 */
        g = 32768 + (s->art >> 1);                           /* gap x (1 +- 0.5) */
        gap = (uint32_t)(((uint64_t)s->gap_q8 * (uint32_t)(g < 4096 ? 4096 : g)) >> 15);
    } else {
        a = tri(&s->rng);
        a = a < 0 ? -a : a;
        gap = (uint32_t)(((uint64_t)((tx_neglog2_q12(tx_rand(&s->rng)) * TX_LN2_Q12) >> 12) * s->gap_q8) >> 12);
    }
    a = (int32_t)(((uint32_t)(a < 655 ? 655 : a > 32767 ? 32767 : a) * (uint32_t)s->amod) >> 15);
    if (a < 64) a = 64;
    l = tx_neglog2_q12((uint32_t)a << 17);                   /* -log2(A), Q12 */
    dur = (uint32_t)(((uint64_t)s->kdur_q8 * tx_exp2((int32_t)(l * 16u / 5u) + (16 << 16))) >> 16);   /* K A^-0.2 */
    if (dur < (2u << 8)) dur = 2u << 8;
    s->pos = 0;
    s->step = (1u << 30) / dur;                              /* (64 << 16) x 256 / dur_q8 */
    s->amp = (int32_t)(((uint32_t)a * (uint32_t)s->imp) >> 15);
    s->wait = (gap >> 8) ? (gap >> 8) : 1u;
    s->impacts++;
}

TX_AUDIO_CODE void tx_scrape_add(tx_scrape_t *s, int32_t *out, unsigned n)
{
    uint32_t x32 = s->rng.s, pos = s->pos, step = s->step, wait = s->wait;
    int32_t lp = s->lp, lpe = s->lpe, lpa = s->lpa, rub = s->rub, amp = s->amp, d;
    const int16_t *t = TX_PULSE_RCOS;
    unsigned j;
    if (tx_scrape_quiet(s)) return;
    if (!s->gap_q8 || !s->imp) wait = 0xFFFFFFFFu;           /* no new impacts */
    else if (!wait) wait = 1;                                /* impacts (re)enabled: the first one now */
    for (j = 0; j < n; j++) {
        int32_t y;
        x32 ^= x32 << 13; x32 ^= x32 >> 17; x32 ^= x32 << 5;
        y = (((int32_t)(x32 >> 16) - 32768) * rub) >> 15;    /* rub: one impact per sample */
        if (!--wait) {                                       /* (rare: out of line) */
            s->rng.s = x32 ? x32 : 1u;
            spawn(s);
            x32 = s->rng.s; pos = s->pos; step = s->step; wait = s->wait; amp = s->amp;
        }
        if (pos < DONE) {
            uint32_t k = pos >> 16, fr = (pos >> 8) & 0xFFu;
            int32_t p = t[k] + (((int32_t)t[k + 1] - t[k]) * (int32_t)fr >> 8);
            y += (p * amp) >> 15;
            pos += step;
        }
        lpe += (y - lp) * lpa;                               /* contact low-pass (|y - lp| <= 2^17, lpa <= 2^13), */
        d = lpe >> 13;                                       /* error feedback: no stall at low cutoffs */
        lpe -= d * 8192;
        lp += d;
        out[j] += lp * 256;                                  /* Q15 -> Q23 */
    }
    if (!rub && !s->imp && pos >= DONE && lp > -8 && lp < 8) lp = lpe = 0;
    s->rng.s = x32 ? x32 : 1u;
    s->lp = lp; s->lpe = lpe; s->pos = pos; s->step = step; s->amp = amp;
    s->wait = wait == 0xFFFFFFFFu ? 0u : wait;
}
