/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura BIRD core: voices and songs (tx_core_bird.h). */
#include "tx_core_bird.h"
#include "tx_core_insect.h"

#define L2_INC 1086030              /* log2(2^32 / fs), Q16: a pitch (log2 Hz) + this = log2 of the phase increment */
#define OCT12 4096                  /* an octave, Q12 */

const uint16_t TX_BIRD_PAUSE[TX_BIRD_TYPES] = { 60, 80, 150, 200, 250 };
const int32_t TX_BIRD_F0[TX_BIRD_TYPES] = { 731868, 771564, 596593, 641031, 666332 };   /* 2.3, 3.5, 0.55, 0.88, 1.15 kHz */

/* per type: the cavity (track 1: at the pitch + oec_l2; 0: fixed at oec_l2), its Q (x16), the trachea's delay
 * (samples: 2 L / c, L ~2 .. 12 cm), a closed beak, the roughness walk's depth (Q16 log2 per step) */
static const struct { uint8_t track, q, dlen, closed; int32_t oec_l2, jsd; } TYPE[TX_BIRD_TYPES] = {
    {1, 48, 5, 0, 0, 300},          /* songbird: the cavity tracks the pitch (B5, B6): a near-pure whistle */
    {1, 40, 4, 0, 0, 400},          /* chirper */
    {0, 32, 30, 0, 691454, 2600},   /* crow: a fixed formant ~1.5 kHz (the references' peak), a long trachea, rough */
    {1, 64, 12, 1, 0, 150},         /* owl: closed beak, an inflated throat at the hoot */
    {0, 24, 24, 0, 727665, 1000},   /* gull: a wide gape, a fixed resonance ~2.2 kHz */
};

static uint32_t rb(tx_rng_t *r, uint32_t lo, uint32_t hi) { return lo + (hi > lo ? tx_rand_below(r, hi - lo + 1u) : 0u); }
static int32_t rs(tx_rng_t *r, int32_t lo, int32_t hi) { return lo + (int32_t)(hi > lo ? tx_rand_below(r, (uint32_t)(hi - lo + 1)) : 0u); }

static tx_bird_syl_t syl(uint32_t len_ms, int32_t f0_l2, int32_t df, int32_t arch, uint32_t k0, uint32_t k1, uint32_t kb,
                         uint32_t amp, uint32_t am_hz, uint32_t am_d)
{
    tx_bird_syl_t s;
    uint32_t len = len_ms * 441u / 10u;
    s.len = (uint16_t)(len > 65535u ? 65535u : len);
    s.f0_l2 = f0_l2; s.df = (int16_t)df; s.arch = (int16_t)arch;
    s.k0 = (uint8_t)(k0 > 224u ? 224u : k0); s.k1 = (uint8_t)(k1 > 224u ? 224u : k1); s.kb = (uint8_t)kb;
    s.amp = (uint8_t)(amp > 255u ? 255u : amp); s.am_hz = (uint8_t)am_hz; s.am_depth = (uint8_t)am_d;
    return s;
}

static void phrase(tx_bird_voice_t *v, tx_bird_syl_t s, uint32_t reps, uint32_t gap_ms, uint32_t after_ms)
{
    tx_bird_phrase_t *p;
    if (v->nph >= TX_BIRD_PHR) return;
    p = &v->ph[v->nph++];
    p->s = s; p->reps = (uint8_t)(reps < 1u ? 1u : reps > 20u ? 20u : reps);
    p->gap = (uint16_t)(gap_ms * 441u / 40u); p->after = (uint16_t)(after_ms * 441u / 40u);
}

/* the song types: the structure from the bird's own draws (sr), so a bird sings its songs again */
static void compose(tx_bird_voice_t *v, unsigned type, tx_rng_t *sr)
{
    int32_t b = TX_BIRD_F0[type];
    unsigned i, n;
    v->nph = 0;
    switch (type) {
    case 0:                                                  /* songbird: a thrush's repeats or a blackbird's motif + twitter */
        n = rb(sr, 3, 5);
        if (tx_rand(sr) & 1u) {                              /* thrush */
            for (i = 0; i < n; i++)
                phrase(v, syl(rb(sr, 90, 300), b + rs(sr, -26000, 46000), rs(sr, -2400, 2400), rs(sr, -1200, 1600), rb(sr, 0, 40), rb(sr, 0, 40), 0, rb(sr, 170, 255), 0, 0),
                       rb(sr, 2, 4), rb(sr, 40, 90), rb(sr, 100, 250));
        } else {                                             /* blackbird: fluted notes, then a quiet twitter */
            for (i = 0; i + 1 < n; i++)
                phrase(v, syl(rb(sr, 150, 350), b + rs(sr, -20000, 30000), rs(sr, -1800, 1800), rs(sr, -800, 1600), rb(sr, 0, 30), rb(sr, 0, 30), 0, rb(sr, 190, 255), 0, 0),
                       1, 50, rb(sr, 50, 140));
            phrase(v, syl(rb(sr, 30, 60), b + 65536 + rs(sr, -10000, 20000), rs(sr, -2000, 1000), rs(sr, 0, 800), rb(sr, 60, 120), rb(sr, 60, 120), 0, rb(sr, 70, 110), 0, 0),
                   rb(sr, 4, 8), rb(sr, 15, 35), 50);
        }
        break;
    case 1:                                                  /* chirper: a chaffinch's falling trills and flourish, or a sparrow */
        if (tx_rand(sr) & 3u) {
            n = rb(sr, 2, 3);
            for (i = 0; i < n; i++)
                phrase(v, syl(rb(sr, 50, 90), b + 19661 - (int32_t)i * 9830 + rs(sr, -3000, 3000), rs(sr, -2400, -1200), rs(sr, 0, 400), rb(sr, 20, 60), rb(sr, 20, 60), 0, rb(sr, 180, 240), 0, 0),
                       rb(sr, 4, 10), rb(sr, 20, 45), rb(sr, 30, 70));
            phrase(v, syl(rb(sr, 250, 400), b + 26214, rs(sr, -4600, -3400), rs(sr, 400, 1000), 40, 20, 0, 255, 0, 0), 1, 0, 0);
        } else {
            phrase(v, syl(rb(sr, 80, 140), b + rs(sr, -6000, 12000), rs(sr, -800, 800), rs(sr, 800, 1600), rb(sr, 60, 100), rb(sr, 60, 100), 0, 230, 0, 0),
                   rb(sr, 2, 6), rb(sr, 150, 400), 0);
        }
        break;
    case 2:                                                  /* crow: caws in a series */
        phrase(v, syl(rb(sr, 220, 420), b + rs(sr, -13000, 13000), rs(sr, -1200, -600), rs(sr, 200, 600), rb(sr, 190, 224), rb(sr, 200, 224), 0, 255, 0, 0),
               rb(sr, 2, 6), rb(sr, 250, 550), 0);
        break;
    case 3:                                                  /* owl: a tawny's hoot, a pause, the tremolo; or an eagle owl */
        if (tx_rand_below(sr, 3u)) {
            phrase(v, syl(rb(sr, 400, 600), b + rs(sr, -3300, 3300), 200, 100, rb(sr, 0, 10), 0, 0, 220, 0, 0), 1, 0, rb(sr, 2500, 4000));
            phrase(v, syl(rb(sr, 900, 1200), b - 2000, -400, 200, 0, 0, 0, 200, rb(sr, 7, 10), 150), 1, 0, 0);
        } else
            phrase(v, syl(rb(sr, 350, 500), b - 98304 + rs(sr, -4000, 4000), -400, 100, 0, 0, 0, 240, 0, 0), rb(sr, 1, 2), 400, 0);
        break;
    default:                                                 /* gull: the long call, each note breaking */
        phrase(v, syl(rb(sr, 250, 350), b - 19661, -800, 600, 30, 200, 128, 220, 0, 0), 2, 120, 80);
        phrase(v, syl(rb(sr, 170, 240), b + rs(sr, -4000, 4000), rs(sr, -1600, -1200), 400, 40, 210, rb(sr, 90, 120), 255, 0, 0),
               rb(sr, 6, 12), rb(sr, 50, 110), 0);
        break;
    }
}

void tx_bird_song(tx_bird_voice_t *v, unsigned type, uint32_t bird_seed, unsigned song, uint32_t var_seed, int32_t f_off,
                  int32_t gain)
{
    tx_rng_t sr;
    unsigned i;
    if (type >= TX_BIRD_TYPES) type = 0;
    tx_rng_seed(&sr, bird_seed * 2654435761u + song * 40503u + 17u);
    tx_rng_seed(&v->rng, var_seed);
    compose(v, type, &sr);
    for (i = 0; i < v->nph; i++) {                           /* this rendition: timing +-8 %, repetitions +-1, pitch +-2 % */
        tx_bird_phrase_t *p = &v->ph[i];
        uint32_t len = p->s.len + (uint32_t)((((int32_t)p->s.len >> 2) * tx_rand_s15(&v->rng)) / 102400);
        p->s.len = (uint16_t)(len < 441u ? 441u : len > 65535u ? 65535u : len);
        if (p->reps > 2u) p->reps = (uint8_t)(p->reps + (int32_t)tx_rand_below(&v->rng, 3u) - 1);
        p->s.f0_l2 += f_off + (tx_rand_s15(&v->rng) >> 4);
    }
    v->type = (uint8_t)type;
    v->track = TYPE[type].track; v->oec_q = TYPE[type].q; v->oec_l2 = TYPE[type].oec_l2;
    v->closed = TYPE[type].closed; v->jsd = TYPE[type].jsd;
    if (v->dlen != TYPE[type].dlen) { for (i = 0; i < TX_BIRD_DL; i++) v->dl[i] = 0; v->dp = 0; }
    v->dlen = TYPE[type].dlen;
    v->pi = 0; v->rep = v->ph[0].reps; v->pos = 0; v->wait = 0; v->singing = 1;
    v->gain = gain > 16383 ? 16383 : gain < 0 ? 0 : gain;
    v->sub = 0;
}

/* the syllable's pitch (log2 Hz Q16), shape (Q5) and level (Q15) at its sample pos */
static void at(const tx_bird_voice_t *v, const tx_bird_syl_t *s, uint32_t pos, int32_t *f, int32_t *k, int32_t *a)
{
    uint32_t t = (pos << 8) / (s->len ? s->len : 1u);         /* Q8 */
    int32_t e, tt = (int32_t)t;
    *f = s->f0_l2 + ((s->df * tt) >> 4) + ((s->arch * tt * (256 - tt)) >> 10) + v->jw;   /* Q12 x Q8 -> Q16 (x 16) */
    if (s->kb) *k = tt < s->kb - 10 ? s->k0 : tt > s->kb + 10 ? s->k1 : s->k0 + ((s->k1 - s->k0) * (tt - s->kb + 10)) / 20;
    else *k = s->k0 + (((s->k1 - s->k0) * tt) >> 8);
    if (tt < 31) { int32_t u = tt * 8; e = (u * u * (768 - 2 * u)) >> 9; }   /* smoothstep in, 12 % */
    else if (tt > 192) { int32_t u = (256 - tt) * 4; e = (u * u * (768 - 2 * u)) >> 9; }   /* out, 25 % */
    else e = 32768;
    if (e > 32767) e = 32767;
    e = (e * s->amp) >> 8;
    if (s->am_depth) {                                       /* tremolo */
        uint32_t ph = (uint32_t)(((uint64_t)pos * s->am_hz * 97391u));
        int32_t c = tx_sin_q15(ph + 0x40000000u);            /* cos */
        e -= (e * ((s->am_depth * (32768 - c)) >> 9)) >> 15;
    }
    *a = e;
}

TX_AUDIO_CODE int tx_bird_render(tx_bird_voice_t *v, int32_t *out, unsigned n)
{
    unsigned j = 0;
    int32_t y1 = v->y1, y2 = v->y2, x1 = v->x1, a1 = v->a1, a2 = v->a2, b0 = v->b0, g = v->gain, last = v->last;
    while (j < n && v->singing) {
        tx_bird_phrase_t *p = &v->ph[v->pi];
        if (v->wait) {                                       /* a gap: the tract rings out */
            unsigned m = v->wait < n - j ? v->wait : n - j, q;
            for (q = 0; q < m; q++, j++) {
                int32_t pin = -((v->dl[v->dp] * 22938) >> 15), y;   /* r 0.7 */
                v->dl[v->dp] = pin; if (++v->dp >= v->dlen) v->dp = 0;
                y = ((a1 * y1 - a2 * y2) >> 14) + ((b0 * pin) >> 15);
                y = y > 65535 ? 65535 : y < -65535 ? -65535 : y;
                y2 = y1; y1 = y;
                last = (((v->closed ? y : y - x1) >> 1) * g) >> 1; x1 = y;
                out[j] += last;
            }
            v->wait -= m;
            continue;
        }
        {   /* a syllable: 16-sample steps */
            const tx_bird_syl_t *s = &p->s;
            int32_t f, k, a0, a1v, fi;
            uint32_t inc;
            unsigned m = 16u, q, si, sf;
            if (m > n - j) m = n - j;
            if (m > s->len - v->pos) m = s->len - v->pos;
            v->jw += ((tx_rand_s15(&v->rng) * v->jsd) >> 15) - (v->jw >> 6);   /* the tension's roughness */
            at(v, s, v->pos, &f, &k, &a0);
            at(v, s, v->pos + m, &fi, &k, &a1v);
            if (!(v->sub++ & 3u)) {                          /* the cavity, every 64 samples */
                uint32_t fc = tx_exp2(v->track ? f + v->oec_l2 : v->oec_l2);
                tx_ireso_coefs(fc > 12000u ? 12000u : fc, (uint32_t)v->oec_q, &a1, &a2, &b0);
            }
            inc = tx_exp2(f + L2_INC);
            si = (unsigned)k >> 5; sf = ((unsigned)k & 31u) << 3;
            if (si >= 7u) { si = 6u; sf = 256u; }
            for (q = 0; q < m; q++, j++) {
                const int16_t *t0 = TX_SYRINX + si * 257u, *t1 = t0 + 257;
                uint32_t ix = v->phase >> 24, fr = (v->phase >> 16) & 255u;
                int32_t w0 = t0[ix] + (((t0[ix + 1] - t0[ix]) * (int32_t)fr) >> 8);
                int32_t w1 = t1[ix] + (((t1[ix + 1] - t1[ix]) * (int32_t)fr) >> 8);
                int32_t w = w0 + (((w1 - w0) * (int32_t)sf) >> 8), amp = a0 + (((a1v - a0) * (int32_t)q) >> 4), x, pin, y;
                v->phase += inc;
                x = (w * (amp >> 2)) >> 15;                      /* Q13 */
                pin = x - ((v->dl[v->dp] * 22938) >> 15);        /* the trachea: p = s - r p(t - T) */
                v->dl[v->dp] = pin; if (++v->dp >= v->dlen) v->dp = 0;
                y = ((a1 * y1 - a2 * y2) >> 14) + ((b0 * pin) >> 15);   /* the cavity */
                y = y > 65535 ? 65535 : y < -65535 ? -65535 : y;
                y2 = y1; y1 = y;
                last = (((v->closed ? y : y - x1) >> 1) * g) >> 1; x1 = y;   /* the beak (or a closed one) */
                out[j] += last;
            }
            v->pos += m;
            if (v->pos >= s->len) {                          /* the syllable ends: the next one, a gap, the next phrase */
                v->pos = 0; v->phase = 0;
                if (v->rep > 1u) { v->rep--; v->wait = (uint32_t)p->gap * 4u; }
                else if (v->pi + 1u < v->nph) { v->wait = (uint32_t)p->after * 4u; v->pi++; v->rep = v->ph[v->pi].reps; }
                else v->singing = 0;
            }
        }
    }
    /* after the song, the tract rings out to silence */
    if (!v->singing && j < n) {
        for (; j < n; j++) {
            int32_t y = (a1 * y1 - a2 * y2) >> 14;
            y = y > 65535 ? 65535 : y < -65535 ? -65535 : y;
            y2 = y1; y1 = y;
            last = (((v->closed ? y : y - x1) >> 1) * g) >> 1; x1 = y;
            out[j] += last;
        }
        v->wait += n;                                        /* (the tail's length: rounding may hold a tiny cycle) */
        if ((y1 > -4 && y1 < 4 && y2 > -4 && y2 < 4) || v->wait > 4410u) { y1 = y2 = 0; x1 = 0; }
    }
    v->y1 = y1; v->y2 = y2; v->x1 = x1; v->a1 = a1; v->a2 = a2; v->b0 = b0; v->last = last;
    return v->singing || y1 || y2;                          /* (a cut tail: the engine declicks v->last) */
}
