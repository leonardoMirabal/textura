/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura BIRD core: voices and songs (tx_core_bird.h). */
#include "tx_core_bird.h"
#include "tx_core_insect.h"

#define L2_INC 1086030              /* log2(2^32 / fs), Q16: a pitch (log2 Hz) + this = log2 of the phase increment */
#define L2_1K 653118                /* log2(1000), Q16 */

/* mean pause between bouts at the middle of the fader (0.1 s), base pitches (log2 Hz Q16) */
const uint16_t TX_BIRD_PAUSE[TX_BIRD_TYPES] = { 60, 80, 220, 200, 1500, 50, 90, 70, 50, 60, 80, 150 };
const int32_t TX_BIRD_F0[TX_BIRD_TYPES] = { 731868, 771564, 596593, 641031, 666332,   /* 2.3, 3.5, 0.55, 0.88, 1.15 kHz */
                                            731868, 771564, 750467, 781796, 805288, 768824, 779340 };
                                            /* 2.3, 3.5, 2.8, 3.9, 5.0, 3.4, 3.8 kHz */

enum { ST_BLACKBIRD, ST_THRUSH, ST_NIGHTINGALE, ST_ROBIN, ST_WREN, ST_HERMIT, ST_SKYLARK, ST_FINCH, ST_CROW, ST_OWL, ST_GULL };
enum { C_WHISTLE, C_NOTE, C_TRILL, C_HARSH, C_INTRO, C_SWEEP, C_CAW, C_HOOT, C_TREM, C_GLONG, C_KYOW, C_CHIRP, C_FLOURISH };
#define SYL(c, i) ((uint16_t)(((unsigned)(c) << 11) | ((unsigned)(i) & 2047u)))

/* the species: numbers from the references where measured (note 15: 10-90 % of the syllables' pitch, lengths,
 * glides, phrase lengths and gaps), ours otherwise */
typedef struct {
    uint8_t style;
    int16_t flo, fhi;               /* the syllables' pitches re the base, Q12 octaves */
    uint16_t dlo, dhi;              /* a note's length, ms */
    uint16_t llo, lhi;              /* a whistle's length, ms */
    int16_t glide;                  /* a note's largest glide, Q12 octaves */
    uint8_t kmax, two;              /* the harshest shape (Q5); the share of notes with a second voice (Q8) */
    uint8_t nsyl, nphr;             /* the repertoire: syllable types, phrase types */
    uint8_t plo, phi;               /* phrases in a bout */
    uint16_t pglo, pghi;            /* gaps between phrases, ms */
    uint8_t track, q, dlen, closed; /* the tract: tracking cavity, its Q (x16), the trachea (samples), a closed beak */
    int32_t oec;                    /* a fixed cavity (log2 Hz Q16) or a tracking offset */
    uint16_t jsd;                   /* the tension's roughness, Q16 log2 per step */
} sp_t;

static const sp_t SP[TX_BIRD_TYPES] = {
    /* 0: the woodland mix draws one of the species below per bird (never used itself) */
    {ST_BLACKBIRD, -2700, 3300, 20, 120, 120, 330, 1230, 70, 0, 24, 16, 2, 6, 1200, 5000, 1, 48, 5, 0, 0, 300},
    /* 1 chirper: chaffinch (trills falling, a flourish) or house sparrow (chirps); 1.5-5.6 kHz */
    {ST_FINCH, -4200, 3000, 15, 110, 250, 400, 900, 100, 0, 16, 4, 1, 4, 3000, 9000, 1, 40, 4, 0, 0, 400},
    /* 2 crow: caws in series */
    {ST_CROW, -800, 800, 220, 420, 220, 420, 900, 224, 0, 6, 4, 1, 3, 1500, 5000, 0, 32, 30, 0, 691454, 2600},
    /* 3 owl */
    {ST_OWL, -200, 200, 400, 600, 900, 1200, 200, 24, 0, 4, 4, 2, 2, 2500, 4000, 1, 64, 12, 1, 0, 150},
    /* 4 gull: the long call */
    {ST_GULL, -250, 250, 170, 240, 250, 350, 1600, 224, 0, 4, 4, 1, 2, 2000, 6000, 0, 24, 24, 0, 727665, 1000},
    /* 5 blackbird: fluted low whistles 1.5-3.2 kHz, then a quiet high twitter; strophes ~2 s, 1-5 s apart */
    {ST_BLACKBIRD, -2700, 3400, 14, 80, 120, 450, 1230, 110, 50, 24, 16, 2, 6, 1500, 10000, 1, 48, 5, 0, 0, 300},
    /* 6 song thrush: 2.8-8 kHz, a phrase of 1-4 notes repeated 2-4 x, phrases ~0.75 s, ~0.9 s apart, long bouts */
    {ST_THRUSH, -2400, 4300, 15, 140, 80, 200, 1100, 130, 40, 32, 24, 6, 16, 300, 1100, 1, 44, 4, 0, 0, 350},
    /* 7 nightingale: 1.6-5.9 kHz; strophes ~1.7 s: soft notes, a swelling whistle or a rattling trill, an end note */
    {ST_NIGHTINGALE, -3000, 4800, 15, 90, 40, 120, 1500, 140, 40, 40, 40, 3, 8, 1500, 4000, 1, 48, 5, 0, 0, 300},
    /* 8 robin: thin, high 2.8-7.7 kHz, strophes of 4-11 varied notes and sweeps, never the same twice */
    {ST_ROBIN, -2500, 3500, 15, 200, 120, 300, 1000, 90, 30, 40, 32, 3, 10, 300, 2000, 1, 48, 4, 0, 0, 250},
    /* 9 wren: loud fast trills 3.5-8 kHz (11-20 /s), a few song types each bird repeats before switching */
    {ST_WREN, -3200, 3000, 15, 100, 60, 120, 1200, 150, 20, 32, 6, 2, 5, 1500, 5000, 1, 40, 3, 0, 0, 300},
    /* 10 hermit thrush: a long pure note, then a flute-like cascade above it, two voices; each song at a new pitch */
    {ST_HERMIT, -1300, 1200, 15, 60, 200, 350, 300, 60, 150, 20, 10, 3, 10, 1800, 4500, 1, 56, 6, 0, 0, 200},
    /* 11 skylark: endless warbling 2.6-5.1 kHz, ~11 syllables /s, glides, rolling buzzes */
    {ST_SKYLARK, -2600, 1700, 15, 100, 80, 180, 1600, 140, 30, 48, 24, 30, 120, 20, 110, 1, 44, 4, 0, 0, 350},
};

/* each type's level (Q6), by render: the songs' levels spread downward from 0 dB (as the references'), the presets at
 * the fader's middle where they were */
static const uint8_t TRIM[TX_BIRD_TYPES] = { 150, 150, 100, 90, 83, 180, 120, 150, 150, 130, 100, 150 };

static uint32_t mix(uint32_t a, uint32_t b)
{
    a ^= b * 0x9E3779B1u; a ^= a >> 16; a *= 0x85EBCA6Bu; a ^= a >> 13; a *= 0xC2B2AE35u; a ^= a >> 16;
    return a;
}
static uint32_t rb(tx_rng_t *r, uint32_t lo, uint32_t hi) { return lo + (hi > lo ? tx_rand_below(r, hi - lo + 1u) : 0u); }
static int32_t rs(tx_rng_t *r, int32_t lo, int32_t hi) { return lo + (int32_t)(hi > lo ? tx_rand_below(r, (uint32_t)(hi - lo + 1)) : 0u); }
static int32_t lvl(tx_rng_t *r, int32_t db)                 /* a level 0 .. -db dB, uniform in dB, Q8 (255 = 0 dB) */
{
    uint32_t a = tx_exp2((8 << 16) - (int32_t)(tx_rand_below(r, (uint32_t)db * 10886u >> 6) << 6));   /* dB / 6.02 = octaves */
    return a > 255u ? 255 : (int32_t)a;
}
static uint32_t ms_n(const tx_bird_voice_t *v, uint32_t ms)   /* ms -> samples at the bird's tempo */
{
    return (((ms * 441u / 10u) >> 4) * (uint32_t)v->tempo) >> 8;
}

/* ---- a syllable: its type from the bird's seed (the repertoire), this rendition's variation from v->rng */
static void syl_make(tx_bird_voice_t *v)
{
    const sp_t *S = &SP[v->sp];
    const tx_bird_seg_t *g = &v->seg[v->si];
    tx_bird_syl_t *s = &v->s;
    tx_rng_t r;
    unsigned cls = g->id >> 11;
    int32_t span = S->fhi - S->flo, rel = 0, df = 0, arch = 0, k0 = 0, k1 = 0, kb = 0, amp = 255, amh = 0, amd = 0;
    int32_t two = 0, twa = 0, a;
    uint32_t len_ms = 100;
    tx_rng_seed(&r, mix(v->seed, g->id + ((uint32_t)v->sp << 16)));
    switch (cls) {
    case C_WHISTLE:                                          /* fluted, in the lower part of the range */
        len_ms = rb(&r, S->llo, S->lhi); rel = S->flo + rs(&r, 0, span * 5 / 8);
        df = rs(&r, -S->glide / 2, S->glide / 2); arch = rs(&r, -S->glide / 3, S->glide / 2);
        k0 = rs(&r, 0, 24); k1 = rs(&r, 0, 24); amp = lvl(&r, 6);
        break;
    case C_NOTE:
        len_ms = rb(&r, S->dlo, S->dhi); rel = S->flo + rs(&r, 0, span);
        df = rs(&r, -S->glide, S->glide); arch = rs(&r, -S->glide / 2, S->glide / 2);
        k0 = rs(&r, 0, S->kmax / 3); k1 = rs(&r, 0, S->kmax / 3); amp = lvl(&r, 16);
        break;
    case C_TRILL:                                            /* a trill's unit: short, often a fast sweep */
        len_ms = rb(&r, 12, 45); rel = S->flo + rs(&r, span / 8, span);
        df = rs(&r, -S->glide * 3 / 2, S->glide * 3 / 2); arch = rs(&r, -S->glide / 2, S->glide / 2);
        k0 = rs(&r, 0, S->kmax / 2); k1 = rs(&r, 0, S->kmax / 2); amp = lvl(&r, 14);
        break;
    case C_HARSH:                                            /* a twitter, a rattle, a buzz: the SNIC side */
        len_ms = rb(&r, 10, 45); rel = S->flo + rs(&r, span / 3, span);
        df = rs(&r, -S->glide, S->glide);
        k0 = rs(&r, S->kmax / 2, S->kmax); k1 = rs(&r, S->kmax / 2, S->kmax); amp = lvl(&r, 16) * 2 / 3;
        if (tx_rand(&r) & 1u) { amh = rs(&r, 60, 160); amd = rs(&r, 60, 160); }
        break;
    case C_INTRO:                                            /* the hermit thrush's long pure opening note */
        len_ms = rb(&r, S->llo, S->lhi); rel = S->flo + rs(&r, 0, span * 2 / 5);
        df = rs(&r, -60, 60); arch = rs(&r, -40, 60); k0 = k1 = rs(&r, 0, 8); amp = rs(&r, 200, 255);
        break;
    case C_SWEEP:                                            /* a wide fast slur */
        len_ms = rb(&r, 30, 120); rel = S->flo + rs(&r, span / 4, span * 3 / 4);
        df = rs(&r, 1000, 2400); if (tx_rand(&r) & 1u) df = -df;
        arch = rs(&r, -800, 800); k0 = rs(&r, 0, S->kmax / 4); k1 = rs(&r, 0, S->kmax / 4); amp = lvl(&r, 12);
        break;
    case C_CAW:
        len_ms = rb(&r, 180, 320); rel = rs(&r, -812, 812); df = rs(&r, -1200, -600); arch = rs(&r, 200, 600);
        k0 = rs(&r, 190, 224); k1 = rs(&r, 200, 224); amp = 255;
        break;
    case C_HOOT:                                             /* a tawny's first hoot, or an eagle owl's (1.5 octaves lower) */
        if (g->id & 1024u) { len_ms = rb(&r, 350, 500); rel = -6144 + rs(&r, -250, 250); df = -400; arch = 100; amp = 240; }
        else { len_ms = rb(&r, 400, 600); rel = rs(&r, -206, 206); df = 200; arch = 100; k0 = rs(&r, 0, 10); amp = 220; }
        break;
    case C_TREM:
        len_ms = rb(&r, 900, 1200); rel = -125; df = -400; arch = 200; amp = 200; amh = rs(&r, 7, 10); amd = 150;
        break;
    case C_GLONG:
        len_ms = rb(&r, 250, 350); rel = -1229; df = -800; arch = 600; k0 = 30; k1 = 200; kb = 128; amp = 220;
        break;
    case C_KYOW:
        len_ms = rb(&r, 170, 240); rel = rs(&r, -250, 250); df = rs(&r, -1600, -1200); arch = 400;
        k0 = 40; k1 = 210; kb = rs(&r, 90, 120); amp = 255;
        break;
    case C_CHIRP:
        len_ms = rb(&r, 25, 90); rel = rs(&r, -1200, 1000); df = rs(&r, -800, 800); arch = rs(&r, 800, 1600);
        k0 = rs(&r, 60, 100); k1 = rs(&r, 60, 100); amp = rs(&r, 110, 255);
        break;
    default:                                                 /* C_FLOURISH: the chaffinch's terminal flourish */
        len_ms = rb(&r, S->llo, S->lhi); rel = 1638; df = rs(&r, -4600, -3400); arch = rs(&r, 400, 1000);
        k0 = 40; k1 = 20; amp = 255;
        break;
    }
    if (S->two && (tx_rand(&r) & 255u) < S->two) {           /* the other side of the syrinx joins */
        two = rs(&r, 1000, 2400); if (tx_rand(&r) & 1u) two = -two;
        twa = rs(&r, 60, 150);
    }
    /* this rendition: pitch +-0.05 oct, glide +-15 %, length +-12 %, level -6 .. +2 dB (and the phrase's), the segment's swell */
    rel += g->lev * 128 + v->plev + rs(&v->rng, -205, 205);
    df += (df * rs(&v->rng, -614, 614)) >> 12;
    len_ms = (len_ms * (uint32_t)(4096 + rs(&v->rng, -492, 492))) >> 12;
    a = (amp * g->amp) >> 8;
    if (g->n > 1 && g->cres < 255) a = (a * (g->cres + (255 - g->cres) * v->sn / (g->n - 1u))) >> 8;
    a = (a * lvl(&v->rng, 8) * 5 / 4) >> 8;                 /* -6 .. +2 dB */
    a = (a * v->pamp) >> 8;
    {
        uint32_t n = ms_n(v, len_ms);
        s->len = (uint16_t)(n < 330u ? 330u : n > 65535u ? 65535u : n);
    }
    s->f0_l2 = TX_BIRD_F0[v->sp] + v->f_off + rel * 16;
    s->df = (int16_t)(df < -8000 ? -8000 : df > 8000 ? 8000 : df); s->arch = (int16_t)arch;
    s->k0 = (uint8_t)(k0 < 0 ? 0 : k0 > 224 ? 224 : k0); s->k1 = (uint8_t)(k1 < 0 ? 0 : k1 > 224 ? 224 : k1); s->kb = (uint8_t)kb;
    s->amp = (uint8_t)(a < 1 ? 1 : a > 255 ? 255 : a); s->am_hz = (uint8_t)amh; s->am_depth = (uint8_t)amd;
    s->two_l2 = (int16_t)two; s->two_amp = (uint8_t)twa;
    v->pos = 0;
}

/* ---- a phrase in the species' style: its structure from the phrase type (the bird's repertoire), varied */
static void seg(tx_bird_voice_t *v, uint16_t id, uint32_t n, uint32_t gap_ms, uint32_t amp, uint32_t cres, int32_t lev)
{
    tx_bird_seg_t *g;
    if (v->nseg >= TX_BIRD_SEG) return;
    g = &v->seg[v->nseg++];
    g->id = id; g->n = (uint8_t)(n < 1u ? 1u : n > 40u ? 40u : n); g->gap = (uint8_t)(gap_ms / 2u > 255u ? 255u : gap_ms / 2u);
    g->amp = (uint8_t)(amp > 255u ? 255u : amp); g->cres = (uint8_t)(cres > 255u ? 255u : cres);
    g->lev = (int8_t)(lev < -127 ? -127 : lev > 127 ? 127 : lev);
}
static unsigned pick(tx_bird_voice_t *v, unsigned n)        /* a phrase type, not one of the last two */
{
    unsigned p = 0, t;
    for (t = 0; t < 6u; t++) {
        p = tx_rand_below(&v->rng, n);
        if (n < 4u || (p != v->recent[0] && p != v->recent[1])) break;
    }
    v->recent[1] = v->recent[0]; v->recent[0] = (uint8_t)p;
    return p;
}
static uint16_t fresh(tx_bird_voice_t *v, unsigned cls) { return SYL(cls, 256u + tx_rand_below(&v->rng, 1792u)); }
static unsigned vary_n(tx_bird_voice_t *v, unsigned n)      /* repetitions this time: +-20 %, at least +-1 sometimes */
{
    int32_t d = ((int32_t)n * rs(&v->rng, -205, 205)) >> 10;
    if (!d && n > 2u && (tx_rand(&v->rng) & 3u) == 0u) d = (tx_rand(&v->rng) & 1u) ? 1 : -1;
    return (unsigned)((int32_t)n + d < 1 ? 1 : (int32_t)n + d);
}

static void phrase_start(tx_bird_voice_t *v)
{
    const sp_t *S = &SP[v->sp];
    tx_rng_t pr;
    unsigned i, m, pid, ns = S->nsyl;
    v->nseg = 0; v->si = 0; v->sn = 0; v->mrep = 1;
    switch (S->style) {
    case ST_BLACKBIRD:                                       /* fluted motif (a phrase type), then a fresh twitter */
        pid = pick(v, S->nphr);
        tx_rng_seed(&pr, mix(v->seed, 0x5000u + pid));
        v->plev = (int16_t)rs(&pr, -200, 200);
        m = (unsigned)((int32_t)rb(&pr, 4, 8) + (int8_t)v->verb * 2);   /* legato whistles, grace notes, a high note */
        if (!tx_rand_below(&v->rng, 4u)) m = m > 2u ? m - 1u : m + 1u;
        if (m < 2u || m > 10u) m = m > 10u && m < 100u ? 10u : 2u;
        for (i = 0; i < m; i++) {
            unsigned c = tx_rand_below(&pr, 20u);
            if (c < 6u) seg(v, SYL(C_NOTE, rb(&pr, 0, ns - 1u)), 1, rb(&pr, 0, 20), lvl(&pr, 10), 255, 0);   /* a grace note */
            seg(v, SYL(c == 19u ? C_NOTE : C_WHISTLE, rb(&pr, 0, ns - 1u)), 1, rb(&pr, 5, 50), 255, 255, 0);
        }
        if (tx_rand_below(&v->rng, 8u)) {                    /* the bird's habit: how long, how loud, a pause before */
            uint32_t h = mix(v->seed, 0x77u), t = rb(&v->rng, 1, 3) + (h & 3u), tl = 20u + ((h >> 4) & 63u);
            if (m && (h & 0x300u)) v->seg[v->nseg - 1u].gap = (uint8_t)rb(&v->rng, 40, 170);   /* 80 .. 340 ms */
            for (i = 0; i < t; i++) {
                unsigned c = tx_rand_below(&v->rng, 3u);
                seg(v, fresh(v, c == 0 ? C_HARSH : c == 1 ? C_TRILL : C_NOTE), rb(&v->rng, 1, 4), rb(&v->rng, 12, 35),
                    rb(&v->rng, tl, tl + 40u), 255, 8);
            }
        }
        break;
    case ST_THRUSH:                                          /* a phrase of 1-4 notes, repeated 2-4 times */
        pid = pick(v, S->nphr);
        tx_rng_seed(&pr, mix(v->seed, 0x5000u + pid));
        v->plev = (int16_t)rs(&pr, -300, 300);
        m = rb(&pr, 1, 4);
        for (i = 0; i < m; i++) {
            unsigned c = tx_rand_below(&pr, 20u), cls = c < 10u ? C_NOTE : c < 14u ? C_TRILL : c < 17u ? C_SWEEP : C_HARSH;
            seg(v, SYL(cls, rb(&pr, 0, ns - 1u)), cls == C_TRILL ? rb(&pr, 2, 4) : 1, rb(&pr, 15, 50), 255, 255, 0);
        }
        v->mrep = (uint8_t)rb(&pr, 2, 4);
        i = tx_rand_below(&v->rng, 8u);
        if (i == 0u && v->mrep > 1u) v->mrep--; else if (i == 1u) v->mrep++;
        break;
    case ST_NIGHTINGALE:                                     /* soft notes, a swelling whistle or a trill, an end */
        pid = pick(v, S->nphr);
        tx_rng_seed(&pr, mix(v->seed, 0x5000u + pid));
        v->plev = (int16_t)rs(&pr, -200, 200);
        if (tx_rand_below(&pr, 4u)) seg(v, SYL(C_NOTE, rb(&pr, 0, ns - 1u)), vary_n(v, rb(&pr, 1, 4)), rb(&pr, 60, 150), rb(&pr, 50, 110), 255, 0);
        for (m = 1u + (tx_rand_below(&pr, 8u) < 5u) + ((int8_t)v->verb > 0), i = 0; i < m; i++) {
            unsigned c = tx_rand_below(&pr, 100u);
            if (c < 35u) seg(v, SYL(C_WHISTLE, rb(&pr, 0, ns - 1u)), vary_n(v, rb(&pr, 4, 10)), rb(&pr, 40, 110), 255, rb(&pr, 40, 90), 0);
            else if (c < 80u) seg(v, SYL(C_TRILL, rb(&pr, 0, ns - 1u)), vary_n(v, rb(&pr, 8, 24)), rb(&pr, 15, 45), 230, 200, 0);
            else seg(v, SYL(C_HARSH, rb(&pr, 0, ns - 1u)), vary_n(v, rb(&pr, 3, 8)), rb(&pr, 30, 80), 220, 255, 0);
        }
        seg(v, SYL((tx_rand(&pr) & 1u) ? C_SWEEP : C_NOTE, rb(&pr, 0, ns - 1u)), 1, 40, 220, 255, 0);
        break;
    case ST_ROBIN:                                           /* a strophe of varied thin notes: half of them new */
    case ST_SKYLARK:                                         /* (the lark: short phrases strung together, endlessly) */
        if (tx_rand_below(&v->rng, 10u) < (S->style == ST_ROBIN ? 5u : 6u)) {
            uint32_t sd = tx_rand(&v->rng);
            tx_rng_seed(&pr, sd);
            ns = 1792u;                                      /* fresh: from the bird's endless store of variants */
            v->plev = (int16_t)rs(&pr, -250, 250);
        } else {
            pid = pick(v, S->nphr);
            tx_rng_seed(&pr, mix(v->seed, 0x5000u + pid));
            v->plev = (int16_t)rs(&pr, -250, 250);
            if (S->style == ST_SKYLARK) v->mrep = (uint8_t)rb(&pr, 1, 3);
        }
        m = (unsigned)((int32_t)(S->style == ST_ROBIN ? rb(&pr, 4, 11) : rb(&pr, 3, 9)) + (int8_t)v->verb * 2);
        if (m < 2u || m > TX_BIRD_SEG) m = m > TX_BIRD_SEG && m < 100u ? TX_BIRD_SEG : 2u;
        for (i = 0; i < m; i++) {
            unsigned c = tx_rand_below(&pr, 20u), cls = c < 9u ? C_NOTE : c < 14u ? C_SWEEP : c < 17u ? C_TRILL : C_HARSH;
            unsigned id = rb(&pr, 0, ns - 1u) + (ns > 255u ? 256u : 0u);
            if (S->style == ST_ROBIN && i == 0u && (tx_rand(&pr) & 3u) == 0u) cls = C_WHISTLE;
            seg(v, SYL(cls, id), cls == C_TRILL ? rb(&pr, 2, 4) : 1, S->style == ST_ROBIN ? rb(&pr, 25, 80) : rb(&pr, 15, 50), S->style == ST_ROBIN ? lvl(&pr, 12) : 255, 255, 0);
        }
        break;
    case ST_WREN:                                            /* its song type: notes, then trill after trill */
        tx_rng_seed(&pr, mix(v->seed, 0x5000u + v->song % S->nphr));
        v->plev = (int16_t)rs(&pr, -150, 150);
        seg(v, SYL(C_NOTE, rb(&pr, 0, ns - 1u)), vary_n(v, rb(&pr, 2, 4)), rb(&pr, 40, 80), 220, 255, 0);
        for (m = rb(&pr, 4, 6), i = 0; i < m; i++) {
            unsigned c = tx_rand_below(&pr, 10u), cls = c < 6u ? C_TRILL : c < 8u ? C_HARSH : C_NOTE;
            seg(v, SYL(cls, rb(&pr, 0, ns - 1u)), vary_n(v, rb(&pr, 3, 12)), rb(&pr, 35, 80), rb(&pr, 180, 255), 255, 0);
        }
        break;
    case ST_HERMIT:                                          /* a long pure note, then a cascade above; a new pitch level */
        pid = pick(v, S->nphr);
        tx_rng_seed(&pr, mix(v->seed, 0x5000u + pid));
        v->plev = (int16_t)rs(&pr, -1800, 1800);
        seg(v, SYL(C_INTRO, rb(&pr, 0, ns - 1u)), 1, rb(&pr, 10, 40), 255, 255, 0);
        for (m = rb(&pr, 2, 4), i = 0; i < m; i++)
            seg(v, SYL((tx_rand(&pr) & 1u) ? C_NOTE : C_TRILL, rb(&pr, 0, ns - 1u)), rb(&pr, 1, 3), rb(&pr, 8, 30),
                rb(&pr, 120, 220), 255, rs(&pr, 1, 8));
        break;
    case ST_FINCH:
        tx_rng_seed(&pr, mix(v->seed, 0x5000u + v->song % S->nphr));
        v->plev = (int16_t)rs(&pr, -180, 180);
        if ((mix(v->seed, 7u) & 3u) == 0u) {                 /* a house sparrow: chirps */
            seg(v, SYL(C_CHIRP, rb(&v->rng, 0, ns - 1u)), rb(&v->rng, 1, 3), rb(&v->rng, 30, 80), lvl(&v->rng, 12), 255, 0);
            if (tx_rand(&v->rng) & 1u) seg(v, SYL(C_CHIRP, rb(&v->rng, 0, ns - 1u)), rb(&v->rng, 1, 3), rb(&v->rng, 30, 80), lvl(&v->rng, 12), 255, 0);
            v->pgap = (uint16_t)((rb(&v->rng, 400, 2000) * v->pace) >> 8);
            v->pamp = (uint8_t)lvl(&v->rng, 9);
            return;
        } else if (v->nph > 1u && tx_rand_below(&v->rng, 10u) < 4u) {   /* between songs: its soft "pink" calls */
            seg(v, SYL(C_CHIRP, rb(&pr, 0, ns - 1u)), rb(&v->rng, 2, 5), rb(&v->rng, 300, 500), lvl(&v->rng, 10) * 2 / 5, 255, 8);
            v->pgap = (uint16_t)((rb(&v->rng, 600, 2500) * v->pace) >> 8);
            v->pamp = 255;
            return;
        } else {                                             /* a chaffinch: 2-3 falling trills and the flourish */
            for (m = rb(&pr, 2, 3), i = 0; i < m; i++)
                seg(v, SYL((tx_rand(&pr) & 1u) ? C_TRILL : C_NOTE, rb(&pr, 0, ns - 1u)), vary_n(v, rb(&pr, 4, 10)), rb(&pr, 20, 60),
                    lvl(&v->rng, 12), rb(&v->rng, 90, 255), 20 - (int32_t)i * 18 + rs(&pr, -3, 3));   /* each trill its own level, swelling */
            seg(v, SYL(C_FLOURISH, rb(&pr, 0, ns - 1u)), 1, 0, 255, 255, 0);
        }
        break;
    case ST_CROW:
        v->plev = 0;
        seg(v, SYL(C_CAW, rb(&v->rng, 0, ns - 1u)), rb(&v->rng, 2, 6), rb(&v->rng, 120, 320), 255, 255, 0);
        break;
    case ST_OWL:                                             /* a tawny's hoot, a pause, the tremolo; or an eagle owl */
        v->plev = 0;
        if ((mix(v->seed, 9u) % 3u) == 0u) seg(v, SYL(C_HOOT, 1024u + rb(&v->rng, 0, 3)), rb(&v->rng, 1, 2), 400, 255, 255, 0);
        else if (v->nph > 1u) seg(v, SYL(C_HOOT, rb(&v->rng, 0, 3)), 1, 0, 255, 255, 0);
        else seg(v, SYL(C_TREM, 0), 1, 0, 255, 255, 0);
        break;
    default:                                                 /* gull: mostly short calls; now and then the long call */
        v->plev = 0;
        if (tx_rand_below(&v->rng, 10u) < 7u)
            seg(v, SYL(C_KYOW, rb(&v->rng, 0, ns - 1u)), rb(&v->rng, 1, 3), rb(&v->rng, 100, 300), lvl(&v->rng, 8), 255, 0);
        else {
            seg(v, SYL(C_GLONG, 0), 2, 120, 255, 255, 0);
            seg(v, SYL(C_KYOW, rb(&v->rng, 0, ns - 1u)), rb(&v->rng, 6, 12), rb(&v->rng, 50, 110), 255, 255, 0);
        }
        break;
    }
    v->plev = (int16_t)(v->plev + rs(&v->rng, -60, 60));
    v->pgap = (uint16_t)((rb(&v->rng, S->pglo, S->pghi) * v->pace) >> 8);   /* (2 ms) */
    v->pamp = (uint8_t)lvl(&v->rng, 9);                    /* the bird turns: the phrase -9 .. 0 dB */
}

/* the syllable ended: the next one, the phrase again, the next phrase, or the song is over (0) */
static int advance(tx_bird_voice_t *v)
{
    uint32_t gap = ms_n(v, (uint32_t)v->seg[v->si].gap * 2u);
    if (++v->sn < v->seg[v->si].n) { v->wait = gap; syl_make(v); return 1; }
    v->sn = 0;
    if (++v->si < v->nseg) { v->wait = gap; syl_make(v); return 1; }
    v->si = 0;
    if (v->mrep > 1u) {                                      /* the phrase again (a thrush), never quite the same */
        v->mrep--;
        v->wait = gap + ms_n(v, rb(&v->rng, 20, 90));
        v->plev = (int16_t)(v->plev + rs(&v->rng, -150, 150));
        syl_make(v);
        return 1;
    }
    if (v->nph > 1u) {
        v->wait = ms_n(v, (uint32_t)v->pgap * 2u);
        v->nph--;
        phrase_start(v);
        syl_make(v);
        return 1;
    }
    return 0;
}

void tx_bird_song(tx_bird_voice_t *v, unsigned type, uint32_t bird_seed, unsigned song, uint32_t var_seed, int32_t f_off,
                  int32_t gain)
{
    tx_rng_t br;
    const sp_t *S;
    unsigned i, sp;
    if (type >= TX_BIRD_TYPES) type = 0;
    tx_rng_seed(&br, mix(bird_seed, 0xB1u));                 /* the individual */
    sp = type;
    if (!type) { unsigned c = tx_rand_below(&br, 100u); sp = c < 30u ? 5u : c < 50u ? 6u : c < 75u ? 8u : c < 90u ? 9u : 1u; }
    S = &SP[sp];
    tx_rng_seed(&v->rng, var_seed);
    v->type = (uint8_t)type; v->sp = (uint8_t)sp; v->seed = bird_seed; v->song = (uint8_t)song;
    v->f_off = f_off + rs(&br, -13107, 13107) + rs(&v->rng, -1300, 1300);   /* the bird's pitch (+-0.2 oct), today's (+-0.02) */
    v->tempo = (4096 + rs(&br, -614, 614)) * (4096 + rs(&v->rng, -200, 200)) >> 12;   /* +-15 %, +-5 % */
    v->pace = (uint8_t)rb(&br, 64, 255);                     /* gaps between phrases x 0.5 .. 2 */
    v->verb = (uint8_t)(int8_t)rs(&br, -1, 1);               /* shorter or longer phrases */
    v->pamp = 255;
    v->track = S->track; v->closed = S->closed; v->oec_l2 = S->oec;
    v->oec_q = (int32_t)(S->q * rb(&br, 75, 125) / 100u);
    v->jsd = (int32_t)(S->jsd * rb(&br, 70, 130) / 100u);
    i = S->dlen * rb(&br, 80, 120) / 100u;
    v->dlen = (uint8_t)(i < 2u ? 2u : i > TX_BIRD_DL ? TX_BIRD_DL : i);
    for (i = 0; i < TX_BIRD_DL; i++) v->dl[i] = 0;           /* a fresh tract: no state from another voice */
    v->dp = 0; v->y1 = v->y2 = v->x1 = 0; v->phase = v->phase2 = 0; v->jw = 0; v->last = 0;
    v->a1 = v->a2 = v->b0 = 0;
    v->recent[0] = v->recent[1] = 0xFF;
    v->nph = (uint8_t)(S->style == ST_OWL ? ((mix(bird_seed, 9u) % 3u) ? 2u : 1u) : rb(&v->rng, S->plo, S->phi));
    if (S->style == ST_FINCH) v->nph = (uint8_t)((mix(bird_seed, 7u) & 3u) == 0u ? rb(&v->rng, 4, 14) : rb(&v->rng, 2, 6));   /* a sparrow's chirping; a chaffinch's songs and calls */
    phrase_start(v);
    syl_make(v);
    v->wait = 0; v->singing = 1;
    gain = (int32_t)(((uint32_t)(gain < 0 ? 0 : gain) * TRIM[sp]) >> 6);   /* the type's level (Q6) */
    v->gain = gain > 16383 ? 16383 : gain;
    v->sub = 0;
}

/* ---- the flock (note 15: the dawn choruses hold 1.2-1.5 tonal voices at once, 17-25 % of the time none, 7-13 % of
 * the syllables within 6 dB of the loudest and 37-54 % more than 20 dB under it, onsets clumped (Fano 1.8-2.7); a gull
 * colony calls ~1.3 /s in bursts (61 % of its gaps < 0.6 s, Fano 1.9, its 10 s level swinging 15 dB); crows caw in
 * series ~0.47 s apart, answering each other) */
static const struct {
    uint8_t nmax;                   /* birds present at the top */
    uint8_t ov_lo, ov_hi;           /* singing at once: up to INTENSITY 40 .. at the top */
    uint8_t yield;                  /* 1: neighbours in a band take turns */
    uint8_t ans_mid, ans_top;       /* answers a call sets off (Q8: the branching ratio), at the middle, the top */
    uint16_t ans_lo, ans_hi;        /* its delay, ms */
    uint8_t span;                   /* the distances' spread added to DEPTH's (octaves Q4) */
} FL[TX_BIRD_TYPES] = {
    {9, 1, 3, 1, 50, 90, 1500, 5000, 8},     /* woodland mix */
    {10, 1, 5, 1, 60, 140, 800, 4000, 8},   /* chirper */
    {8, 1, 3, 0, 120, 200, 300, 1500, 8},   /* crow: answers, in series */
    {3, 1, 3, 1, 100, 150, 1000, 4000, 8},  /* owl: a duet */
    {10, 1, 4, 0, 150, 235, 80, 450, 16},    /* gull: excitement spreads */
    {6, 1, 4, 1, 60, 120, 1500, 5000, 8},   /* blackbird */
    {4, 1, 3, 1, 50, 100, 1500, 5000, 8},   /* song thrush */
    {4, 1, 3, 1, 80, 150, 1000, 4000, 8},   /* nightingale: countersinging */
    {6, 1, 4, 1, 60, 120, 1500, 5000, 8},   /* robin */
    {6, 1, 4, 1, 80, 150, 1000, 4000, 8},   /* wren */
    {4, 1, 3, 1, 50, 100, 1500, 5000, 8},   /* hermit thrush */
    {6, 1, 4, 0, 40, 100, 1500, 5000, 12},   /* skylark: overhead, they overlap */
};

unsigned tx_bird_want(unsigned type, int32_t act)
{
    int32_t lg;
    unsigned w;
    if (type >= TX_BIRD_TYPES) type = 0;
    act = act < 0 ? 0 : act > (127 << 8) ? 127 << 8 : act;
    lg = (32 << 16) - (int32_t)(tx_neglog2_q12(FL[type].nmax) << 4);   /* log2 nmax, Q16 */
    w = (tx_exp2(((lg >> 8) * act) / 127 + (8 << 16)) + 128u) >> 8;   /* 1 .. nmax, log (32-bit: < 2^25) */
    return w < 1u ? 1u : w > FL[type].nmax ? FL[type].nmax : w;
}

unsigned tx_bird_overlap(unsigned type, int32_t act)
{
    int32_t x = act - (40 << 8);
    if (type >= TX_BIRD_TYPES) type = 0;
    if (x <= 0) return FL[type].ov_lo;
    return FL[type].ov_lo + (unsigned)((x * (FL[type].ov_hi - FL[type].ov_lo) + (43 << 8)) / (87 << 8));
}

int32_t tx_bird_band(unsigned type, uint32_t seed, int32_t f_off)
{
    tx_rng_t br;
    unsigned sp = type;
    if (type >= TX_BIRD_TYPES) type = sp = 0;
    tx_rng_seed(&br, mix(seed, 0xB1u));                      /* as tx_bird_song draws the individual */
    if (!type) { unsigned c = tx_rand_below(&br, 100u); sp = c < 30u ? 5u : c < 50u ? 6u : c < 75u ? 8u : c < 90u ? 9u : 1u; }
    return TX_BIRD_F0[sp] + f_off + (SP[sp].flo + SP[sp].fhi) * 8;   /* the middle of its range (Q12 -> Q16 / 2) */
}

/* over the ground around (p(r) ~ r between the nearest and nearest x 2^span): most birds far, a few near. The denser
 * the birds (the more of them), the smaller their territories: the nearest ones come in as 1 / sqrt(density) (from
 * ~1.3 octaves farther than the preset's distance with one bird to half an octave nearer with all), so a growing
 * chorus is louder and a few of its birds are close */
int32_t tx_bird_dist(tx_rng_t *r, unsigned type, int32_t near_l2, int32_t depth_l2, int32_t act)
{
    int32_t span, ks, lg;
    uint32_t K, x;
    if (type >= TX_BIRD_TYPES) type = 0;
    act = act < 0 ? 0 : act > (127 << 8) ? 127 << 8 : act;
    span = depth_l2 + FL[type].span * 4096;                  /* Q4 octaves -> Q16 */
    lg = (32 << 16) - (int32_t)(tx_neglog2_q12(FL[type].nmax) << 4);   /* log2 nmax, Q16 */
    near_l2 += (((lg >> 8) * ((127 << 8) - act) / 127) >> 1) - 32768;   /* (log2 nmax - log2 want) / 2, - 0.5 oct */
    if (span <= 0) return near_l2;
    ks = span * 2;
    if (ks > (15 << 16)) ks = 15 << 16;
    K = tx_exp2(ks);
    x = 65536u + (tx_rand(r) >> 16) * (K - 1u);
    return near_l2 + ((32 << 16) - (int32_t)(tx_neglog2_q12(x) << 4) - (16 << 16)) / 2;
}

int tx_bird_yield(unsigned type, int32_t band, int32_t r_l2, int32_t band_o, int32_t r_o)
{
    int32_t d = band - band_o;
    if (type >= TX_BIRD_TYPES || !FL[type].yield) return 0;
    return (d < 0 ? -d : d) < 22938 && r_o < r_l2 + 65536;  /* within 0.35 oct, not much farther than itself */
}

uint32_t tx_bird_answer(tx_rng_t *r, unsigned type, int32_t act, uint32_t wait, unsigned nidle)
{
    int32_t p;
    uint32_t d;
    if (type >= TX_BIRD_TYPES) type = 0;
    if (act < (32 << 8)) return wait;                        /* one or two birds: no answering yet */
    p = act < (64 << 8) ? FL[type].ans_mid * (act - (32 << 8)) / (32 << 8)
                        : FL[type].ans_mid + (FL[type].ans_top - FL[type].ans_mid) * (act - (64 << 8)) / (63 << 8);
    if (nidle > 1u) p /= (int32_t)nidle;                    /* the branching ratio p / 256 shared */
    if ((int32_t)(tx_rand(r) >> 24) >= p) return wait;
    d = rb(r, FL[type].ans_lo, FL[type].ans_hi) * 441u / 10u;
    return d < wait ? d : wait;
}

void tx_bird_stop(tx_bird_voice_t *v)
{
    if (v->nph > 1u) v->nph = 1;
    if (v->mrep > 1u) v->mrep = 1;
}

/* the syllable's pitch (log2 Hz Q16), shape (Q5, capped by the pitch: no folding) and level (Q15) at its sample pos */
static void at(const tx_bird_voice_t *v, uint32_t pos, int32_t *f, int32_t *k, int32_t *a)
{
    const tx_bird_syl_t *s = &v->s;
    uint32_t t = (pos << 8) / (s->len ? s->len : 1u);         /* Q8 */
    int32_t e, tt = (int32_t)t, kc;
    *f = s->f0_l2 + ((s->df * tt) >> 4) + ((s->arch * tt * (256 - tt)) >> 10) + v->jw;   /* Q12 x Q8 -> Q16 (x 16) */
    if (s->kb) *k = tt < s->kb - 10 ? s->k0 : tt > s->kb + 10 ? s->k1 : s->k0 + ((s->k1 - s->k0) * (tt - s->kb + 10)) / 20;
    else *k = s->k0 + (((s->k1 - s->k0) * tt) >> 8);
    kc = 134 - ((((*f - L2_1K) >> 8) * 38) >> 8);            /* harmonics ~K^n under -40 dB at fs / 2 */
    if (*k > kc) *k = kc < 0 ? 0 : kc;
    {   /* smoothstep in over 12 % (at least 2 ms), out over 25 % (at least 3 ms): no splatter at a short note's edges */
        uint32_t na = s->len * 31u >> 8, nr = s->len >> 2, rem = s->len - (pos < s->len ? pos : s->len);
        if (na < 88u) na = 88u;
        if (nr < 132u) nr = 132u;
        if (na + nr > s->len) { na = s->len * 2u / 5u; nr = s->len - na; }
        if (pos < na) { int32_t u = (int32_t)((pos << 8) / na); e = (u * u * (768 - 2 * u)) >> 9; }
        else if (rem < nr) { int32_t u = (int32_t)((rem << 8) / nr); e = (u * u * (768 - 2 * u)) >> 9; }
        else e = 32768;
    }
    if (e > 32767) e = 32767;
    e = (e * s->amp) >> 8;
    if (s->am_depth) {                                       /* tremolo */
        uint32_t ph = pos * s->am_hz * 97391u;
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
            const tx_bird_syl_t *s = &v->s;
            int32_t f, k, a0, a1v, fi, ta = 0;
            uint32_t inc, inc2 = 0;
            unsigned m = 16u, q, si, sf;
            if (m > n - j) m = n - j;
            if (m > s->len - v->pos) m = s->len - v->pos;
            v->jw += ((tx_rand_s15(&v->rng) * v->jsd) >> 15) - (v->jw >> 6);   /* the tension's roughness */
            at(v, v->pos, &f, &k, &a0);
            at(v, v->pos + m, &fi, &k, &a1v);
            if (!(v->sub++ & 3u) || v->pos == 0u) {          /* the cavity, every 64 samples (and at a syllable's start) */
                uint32_t fc = tx_exp2(v->track ? f + v->oec_l2 : v->oec_l2);
                tx_ireso_coefs(fc > 12000u ? 12000u : fc, (uint32_t)v->oec_q, &a1, &a2, &b0);
            }
            inc = tx_exp2(f + L2_INC);
            if (s->two_amp) { int32_t f2 = f + s->two_l2 * 16; if (f2 < 784190 + 65536) { inc2 = tx_exp2(f2 + L2_INC); ta = s->two_amp; } }
            si = (unsigned)k >> 5; sf = ((unsigned)k & 31u) << 3;
            if (si >= 7u) { si = 6u; sf = 256u; }
            for (q = 0; q < m; q++, j++) {
                const int16_t *t0 = TX_SYRINX + si * 257u, *t1 = t0 + 257;
                uint32_t ix = v->phase >> 24, fr = (v->phase >> 16) & 255u;
                int32_t w0 = t0[ix] + (((t0[ix + 1] - t0[ix]) * (int32_t)fr) >> 8);
                int32_t w1 = t1[ix] + (((t1[ix + 1] - t1[ix]) * (int32_t)fr) >> 8);
                int32_t w = w0 + (((w1 - w0) * (int32_t)sf) >> 8), amp = a0 + (((a1v - a0) * (int32_t)q) >> 4), x, pin, y;
                v->phase += inc;
                if (ta) { w += (tx_sin_q15(v->phase2) * ta) >> 8; v->phase2 += inc2; }   /* the second voice */
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
            if (v->pos >= s->len && !advance(v)) v->singing = 0;   /* the next syllable, a gap, the next phrase */
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
