/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura WIND core (tx_core_wind.h): the speed, then five noise paths shaped by it. */
#include "tx_core_wind.h"
#include "tx_core_life.h"

#define RUSH_REF 8200              /* the rush's gain at 10 m/s (Q15; calibrated by render against the references) */
#define RUST_REF 1040              /* the rustle's at 10 m/s, FOLIAGE 127 */
#define BUF_REF 9500               /* the buffeting's at 10 m/s, BUFFET 127, a burst of 1 */
#define WH_REF 8000                /* a whoosh's at 10 m/s, WHOOSH 127, size 1 */

/* log2 of an integer, Q16 */
static int32_t ilog2(uint32_t x) { return (32 << 16) - (int32_t)(tx_neglog2_q12(x) << 4); }
/* a one-pole's coefficient for a corner at f Hz, Q15: 1 - exp(-2 pi f / fs) */
static int32_t op_coef(uint32_t f)
{
    int32_t e = (15 << 16) - (int32_t)((f * 862u) >> 6);     /* log2(e) 2 pi / fs = 13.47 / Hz in Q16 */
    return 32768 - (int32_t)tx_exp2(e);
}
/* (u / 10 m/s)^(p / 4), Q16, from l2 = log2(u / 10) Q16 */
static int32_t upow(int32_t l2, int32_t p4)
{
    if (l2 > (3 << 15)) l2 = 3 << 15;                        /* 28 m/s: the scale's top */
    if (l2 < -(7 << 16)) return 0;
    return (int32_t)tx_exp2((l2 * p4) / 4 + (16 << 16));
}
static int32_t clampi(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }
/* an exponential draw of mean 1, Q12 */
static int32_t expo(tx_rng_t *r) { return (int32_t)((tx_neglog2_q12(tx_rand(r) | 1u) * 2839u) >> 12); }
/* g x (Q15 a) x (Q15 b) as a Q15 gain (64-bit: per block) */
static int32_t gmul(int32_t g, int32_t a) { return (int32_t)(((int64_t)g * a) >> 15); }

void tx_wind_init(tx_wind_t *w, uint32_t seed)
{
    uint8_t *b = (uint8_t *)w;
    unsigned i;
    for (i = 0; i < sizeof *w; i++) b[i] = 0;
    tx_rng_seed(&w->rng, seed ^ 0x57A1Du);
    tx_rng_seed(&w->nr, seed * 3u + 11u);
    tx_rng_seed(&w->nl, seed * 5u + 23u);
    tx_rng_seed(&w->nb, seed * 7u + 41u);
    tx_rng_seed(&w->ne, seed * 11u + 59u);
    for (i = 0; i < TX_WIND_NW; i++) tx_rng_seed(&w->wv[i].rng, seed * 13u + i * 101u + 7u);
    for (i = 0; i < 5; i++) w->w[i] = tx_rand_s15(&w->rng);  /* the walks start somewhere */
    w->kr = 1000; w->kd = 200;
    w->limit = 3;
    w->level = 32768;
}

static void whoosh_spawn(tx_wind_t *w, int32_t size_q15, int32_t lfc)
{
    unsigned i;
    tx_wwhoosh_t *v = 0;
    for (i = 0; i < w->limit && i < TX_WIND_NW; i++) if (!w->wv[i].on) { v = &w->wv[i]; break; }
    if (!v || !w->whoosh) return;
    v->on = 1;
    v->k = 65536 / (34 + (int32_t)tx_rand_below(&w->rng, 104u));   /* the pulse's pole: 0.2 .. 0.8 s */
    v->x1 = 1 << 19; v->x2 = 0;
    v->lf0 = lfc + (int32_t)tx_rand_below(&w->rng, 65536u) * 2;   /* 0 .. +2 octaves over the rush */
    v->dep = 39322 + (int32_t)tx_rand_below(&w->rng, 65536u);  /* its sweep: 0.6 .. 1.6 octaves */
    v->q = 262144 / (19 + (int32_t)tx_rand_below(&w->rng, 22u));   /* Q 1.2 .. 2.5 */
    v->amp = size_q15;
    v->g = 0;
    w->events++; w->total++;
}

/* a gust (a > 0) or a lull (a < 0), Q15 of the mean speed; its rise and decay, blocks */
static void gust(tx_wind_t *w, int32_t a, int32_t rise, int32_t decay)
{
    w->gtgt = clampi(w->gtgt + a, -26214, 32768);
    w->kr = 65536 / rise; w->kd = 65536 / decay;
    w->gcol = (int32_t)tx_rand_below(&w->rng, 65536u) - 32768;   /* its colour: +-0.5 octave (it comes from its own
                                                              * direction, through other trees and grass) */
    w->clu = 516u;                                           /* ~3 s: gusts come in groups */
    w->events++; w->total++;
}

void tx_wind_press(tx_wind_t *w, uint8_t vel)
{
    int32_t a = (8000 + (int32_t)vel * 200) * (19661 + (int32_t)tx_rand_below(&w->rng, 26214u)) >> 15;   /* x 0.6 .. 1.4 */
    w->held = 1;
    if (w->press_wait && tx_rand_below(&w->rng, 3u)) return;   /* presses within ~0.4 s of a gust: a third of them a
                                                              * whoosh, none a new gust (a burst of keys is not a gale) */
    if (!w->press_wait) {
        w->press_wait = 70u;
        gust(w, a, 26 + (int32_t)tx_rand_below(&w->rng, 43u), 138 + (int32_t)tx_rand_below(&w->rng, 206u));
    }
    whoosh_spawn(w, 16384 + (int32_t)vel * 160, w->lfr ? w->lfr : 539284 /* 300 Hz */);
}

int tx_wind_quiet(const tx_wind_t *w)
{
    unsigned i;
    int32_t z = w->rg | w->lg | w->bg | w->eg | w->eon | w->r.low | w->r.band | w->llp2 | w->b2 | w->env;
    for (i = 0; i < TX_WIND_NW; i++) z |= w->wv[i].on;
    return !z;
}

/* the Chamberlin band over white noise (>> shift), added with a gain ramped g0 -> g1 (Q15 -> Q23 through >> 5) */
TX_AUDIO_CODE static void svf_noise(tx_wsvf_t *s, tx_rng_t *r, int32_t f, int32_t q, int32_t g0, int32_t g1,
                                    int32_t *out, unsigned n, unsigned shift)
{
    int32_t low = s->low, band = s->band, g = g0, d = (g1 - g0) / (int32_t)n;
    unsigned j;
    for (j = 0; j < n; j++) {
        int32_t x = ((int32_t)(tx_rand(r) >> 18) - 8192) >> shift, y;
        low += (f * band) >> 14;
        y = x - low - ((q * band) >> 14);
        band += (f * y) >> 14;
        out[j] += band * (g >> 5);
        g += d;
    }
    s->low = low; s->band = band;
}

TX_AUDIO_CODE void tx_wind_block(tx_wind_t *w, int32_t *out, unsigned n)
{
    tx_rng_t *r = &w->rng;
    int32_t t, ti, rel, u, l2, lfr, rg, lg, bg, be, lfl;
    unsigned i, j;
    if (!n) return;
    w->events = 0;
    /* ---- the speed: the walks (16, 4, 1, 0.25, 0.06 s: k = 1 / tau, g = sqrt(2 / tau), per block) */
    {
        static const int32_t K[5] = { 24, 95, 381, 1524, 6096 }, G[5] = { 883, 1766, 3533, 7067, 14130 };
        static const int32_t W[5] = { 23593, 17695, 11796, 7209, 6554 };   /* -5/3-like: the slow ones weigh most */
        t = 0;
        for (i = 0; i < 5; i++) {
            w->w[i] += -((w->w[i] * K[i]) >> 16) + ((tx_life_gauss(r) * G[i]) >> 12);
            w->w[i] = clampi(w->w[i], -98304, 98304);
            t += ((w->w[i] >> 3) * W[i]) >> 12;
        }
        w->fast += -((w->fast * 15240) >> 16) + ((tx_life_gauss(r) * 22346) >> 12);   /* tau 25 ms */
        w->fast = clampi(w->fast, -98304, 98304);
        for (i = 0; i < 2; i++) {                            /* the flutter of small eddies (tau 40 ms) */
            w->fl[i] += -((w->fl[i] * 9362) >> 16) + ((tx_life_gauss(r) * 17514) >> 12);
            w->fl[i] = clampi(w->fl[i], -98304, 98304);
        }
    }
    /* gusts and lulls */
    if (w->press_wait) w->press_wait--;
    if (w->U) {
        uint32_t pr = (uint32_t)w->grate;
        if (w->clu) { w->clu--; pr *= 3u; }
        if ((tx_rand(r) >> 16) < pr) {
            int32_t e = expo(r), a = (int32_t)tx_exp2((e * 3 / 2) * 16 + (15 << 16) - 200463);   /* 0.12 x 2^(1.5 E) */
            if (a > 22938) a = 22938;
            a = (a * (24 + w->gusty)) / 151;                 /* (x 0.16 .. 1 with GUST: a steady wind's gusts are small) */
            if (tx_rand_below(r, 5u) == 0u) a = -(a * 3 / 5);    /* a lull */
            gust(w, a, 34 + (int32_t)tx_rand_below(r, 138u), 138 + (int32_t)tx_rand_below(r, 465u));
            if (a > 6554 && (tx_rand(r) & 1u)) whoosh_spawn(w, 16384 + a, w->lfr);
        }
    }
    w->gtgt -= (w->gtgt * w->kd) >> 16;
    w->gcur += ((w->gtgt - w->gcur) * w->kr) >> 16;
    w->gmean += (w->gcur - w->gmean) >> 12;                 /* (~24 s: U stays the mean, gusts over lulls) */
    w->gcol -= w->gcol >> 9;                                 /* (the colour fades over ~3 s) */
    /* the key's envelope: on in ~0.4 s; released, the wind dies over ~4 s */
    if (w->held) w->env += (32768 - w->env) >> 6;
    else { w->env -= (w->env >> 8) + 1; if (w->env < 0) w->env = 0; }
    /* u = U (1 + TI t) + G max(U, 3 m/s), light air more turbulent (it comes and goes) */
    ti = w->ti;
    if (w->U < 32768) { int32_t x = 32768 - w->U; ti += ((x * x) >> 15) * 3 / 5; }   /* + 0.6 (1 - U / 8 m/s)^2 */
    rel = 32768 + (int32_t)(((int64_t)ti * t) >> 15);
    {
        int32_t fl = w->U * 4 < 12288 ? w->U * 4 : 12288;   /* the gusts' floor: 3 m/s (0 in a calm) */
        int64_t uu = ((int64_t)w->U * (rel > 0 ? rel : 0) >> 15) + ((int64_t)(w->gcur - w->gmean) * (w->U > fl ? w->U : fl) >> 15);
        uu = (uu * w->env) >> 15;
        u = uu < 0 ? 0 : uu > 327680 ? 327680 : (int32_t)uu;    /* m/s Q12, <= 80 */
    }
    w->u = u;
    /* ---- the paths' levels and colours */
    l2 = u > 40 ? ilog2((uint32_t)u) - (12 << 16) - 217706 : -(8 << 16);   /* log2(u / 10) */
    rg = lg = bg = 0;
    if (u > 40) {
        int32_t a2 = upow(l2, 8), a25 = upow(l2, 10);
        int32_t fol = 6554 + w->foliage * 206;                  /* 0.2 .. 1 */
        rg = (int32_t)(((int64_t)RUSH_REF * a2) >> 16);
        lg = (int32_t)(((int64_t)RUST_REF * a25) >> 16);
        lg = gmul(lg, fol);
        {   /* the buffeting: (fast walk + gusts)+ squared */
            int32_t b = w->fast + ((t * 3) >> 2) + 16384;
            b = b < 0 ? 0 : b > 65536 ? 65536 : b;
            be = ((b >> 2) * (b >> 2)) >> 11;                    /* Q15, <= 4 */
            bg = (int32_t)(((int64_t)BUF_REF * a25) >> 16);
            bg = gmul(bg, be);
            bg = (bg * w->buffet) / 127;
        }
    }
    {   /* the flutter: the rush +-22 .. 44 %, the leaves +-28 .. 56 % (more with GUST) */
        int32_t d0 = (7 * (127 + w->gusty)) / 254, d1 = (9 * (127 + w->gusty)) / 254;
        rg = gmul(rg, clampi(32768 + ((w->fl[0] * d0) >> 4), 6554, 65536));
        lg = gmul(lg, clampi(32768 + ((w->fl[1] * d1) >> 4), 3277, 65536));
    }
    rg = clampi(gmul(rg, w->level), 0, 524288);
    lg = clampi(gmul(lg, w->level), 0, 524288);
    bg = clampi(gmul(bg, w->level), 0, 524288);
    /* rush: white through a high-pass at fc / 4 and a low-pass at fc = 60 u^0.75 Hz (x SCALE): the band widens and
     * brightens with u; its power held to the speed's law (x sqrt(1 kHz / fc)) */
    lfr = 387114 + ((l2 + 217706) * 3) / 4 + w->scale + w->gcol;   /* log2(60) + 0.75 log2 u, the gust's colour */
    lfr = clampi(lfr, 314572 /* 28 Hz */, 750467 /* 2.8 kHz */);
    w->lfr = lfr;
    if (rg) rg = clampi((int32_t)(((int64_t)rg * tx_exp2((653124 - lfr) / 2 + (15 << 16))) >> 15), 0, 524288);
    if (rg || w->rg) {
        int32_t al = op_coef(tx_exp2(lfr)), ah = op_coef(tx_exp2(lfr - (2 << 16)) + 20u), h1 = w->r.low, l1 = w->r.band;
        int32_t g = w->rg, d = (rg - w->rg) / (int32_t)n;
        for (j = 0; j < n; j++) {
            int32_t x = (int32_t)(tx_rand(&w->nr) >> 18) - 8192;
            h1 += ((x - h1) * ah) >> 15;
            l1 += ((x - h1 - l1) * al) >> 15;
            out[j] += l1 * (g >> 5);
            g += d;
        }
        if (!rg) h1 = l1 = 0;
        w->r.low = h1; w->r.band = l1;
    }
    w->rg = rg;
    /* rustle: flaps at R = 600 (0.3 + 1.2 FOLIAGE) (u / 5)^2 /s, each a 3 ms decay of white; HP 400 Hz, two
     * low-pass poles at 1.5 kHz (u / 4)^0.7 (x SCALE, up to x 1.6 with FOLIAGE: leaves are brighter than grass) */
    if (lg || w->lg) {
        int32_t lR = 604820 + ilog2((uint32_t)(77 + w->foliage * 2)) - (8 << 16) + 2 * (l2 + 65536);   /* log2 R */
        int32_t mean_q4, norm, a400 = op_coef(400), lk, dg = (lg - w->lg) / (int32_t)n, g = w->lg;
        int32_t le = w->le, lhp = w->lhp, llp = w->llp, ll2 = w->llp2;
        uint32_t sb;
        lR = clampi(lR, 2 << 16, 980930 /* 20000 /s */);
        mean_q4 = (int32_t)tx_exp2(1011122 + (4 << 16) - lR);   /* 44100 / R samples, Q4 */
        norm = (int32_t)tx_exp2((15 << 16) - (lR - 552370) / 2);   /* 1 / sqrt(R tau): tau = 2.9 ms (log2 -8.4) */
        norm = clampi(norm, 0, 98304);
        lfl = 691450 + (((l2 + 217706 - 131072) * 7) / 10) + w->scale + w->foliage * 352 + w->gcol;   /* log2(1500) + 0.7 log2(u / 4) */
        lk = op_coef(tx_exp2(clampi(lfl, 10 << 16, 890000 /* 12.3 kHz */)));
        for (sb = 0; sb < n; sb += 16u) {
            unsigned m = n - sb < 16u ? n - sb : 16u, c = 0;
            int32_t acc = 0;
            if (lg) while ((int32_t)w->lacc < (16 << 4) && c < 32u) {   /* the flaps arriving in these 16 samples */
                int32_t u1 = (int32_t)(tx_rand(&w->nl) >> 17);
                acc += (((u1 * u1) >> 17) * norm) >> 13;
                w->lacc += (uint32_t)((((mean_q4 > 176400 ? 176400 : mean_q4)) * (expo(&w->nl) >> 4)) >> 8);
                c++;
            }
            if ((int32_t)w->lacc >= (16 << 4)) w->lacc -= 16u << 4; else w->lacc = 0;
            le += acc;
            if (le > (1 << 22)) le = 1 << 22;
            for (j = sb; j < sb + m; j++) {
                int32_t x = (int32_t)(tx_rand(&w->nl) >> 18) - 8192, h;
                x = (x * (le >> 4)) >> 11;
                x = clampi(x, -32767, 32767);
                le -= le >> 7;
                lhp += ((x - lhp) * a400) >> 15;
                h = x - lhp;
                llp += ((h - llp) * (lk >> 1)) >> 14;           /* (|h - llp| < 2^17: lk halved, < 2^31) */
                ll2 += ((llp - ll2) * (lk >> 1)) >> 14;
                out[j] += ll2 * (g >> 5);
                g += dg;
            }
        }
        if (!lg) le = lhp = llp = ll2 = 0;
        w->le = le; w->lhp = lhp; w->llp = llp; w->llp2 = ll2;
    }
    w->lg = lg;
    /* buffet: two one-poles at 90 Hz (x SCALE / 2) on white */
    if (bg || w->bg) {
        int32_t b1 = w->b1, b2 = w->b2, ab = op_coef(tx_exp2(425000 + w->scale / 2)), g = w->bg, d = (bg - w->bg) / (int32_t)n;
        for (j = 0; j < n; j++) {
            int32_t x = (int32_t)(tx_rand(&w->nb) >> 16) - 32768;
            b1 += ((x - b1) * ab) >> 15;
            b2 += ((b1 - b2) * ab) >> 15;
            out[j] += b2 * (g >> 5);
            g += d;
        }
        if (!bg) b1 = b2 = 0;
        w->b1 = b1; w->b2 = b2;
    }
    w->bg = bg;
    /* the wind's own whooshes (rate ~ WHOOSH (0.1 + 0.9 min(1, u / 15)) /s), then every voice */
    if (u > 400 && w->whoosh) {
        int32_t uu = u > 61440 ? 61440 : u;
        uint32_t pr = (uint32_t)((w->whoosh * (19661 + ((uu * 46) >> 4))) >> 16);   /* /65536 per block */
        if ((tx_rand(r) >> 16) < pr) whoosh_spawn(w, 9830 + (int32_t)tx_rand_below(r, 32768u), lfr);
    }
    w->nwh = 0;
    for (i = 0; i < TX_WIND_NW; i++) {
        tx_wwhoosh_t *v = &w->wv[i];
        int32_t g, lc, f, pn;
        if (!v->on) continue;
        v->x2 += ((v->x1 - v->x2) * v->k) >> 16;
        v->x1 -= ((v->x1 * v->k) >> 16) + (v->x1 > 0);
        pn = ((v->x2 >> 4) * 89) >> 5;                        /* the pulse, its peak at 1 (Q15) */
        g = (int32_t)(((int64_t)WH_REF * upow(l2, 8)) >> 16);
        g = gmul(gmul(gmul(g, v->amp), pn), w->level);
        g = (g * w->whoosh) / 127;
        if (u <= 40 || i >= w->limit) g = 0;   /* (over PARTICLES: out in this block) */
        g = clampi(g, 0, 524288);
        lc = v->lf0 + (int32_t)(((int64_t)v->dep * (v->x1 >> 4)) >> 15);
        f = tx_sin_q15(tx_exp2(clampi(lc, 348778, 756990)) * 97391u >> 1);   /* 40 Hz .. 3 kHz */
        svf_noise(&v->f, &v->rng, f, v->q, v->g, g, out, n, 0);
        v->g = g;
        if ((v->x1 < 64 && v->x2 < 256) || i >= w->limit) { v->on = 0; v->f.low = v->f.band = 0; v->g = 0; }
        else w->nwh++;
    }
    /* the edge tone: rare, in strong gusts only */
    if (!w->eon && w->edge && u > 24576 && w->gcur > 6554 && (tx_rand(r) >> 16) < (uint32_t)((w->edge * 29) >> 6)) {
        w->eon = 1; w->ex1 = 1 << 19; w->ex2 = 0;
        w->ek = 65536 / (17 + (int32_t)tx_rand_below(r, 44u));   /* 0.1 .. 0.35 s */
        w->ed_l2 = -549246 + (int32_t)tx_rand_below(r, 65536u) * 152 / 65;   /* d: 3 .. 15 mm (log2 m: -8.4 .. -6.1) */
        w->eamp = 2000 + (int32_t)tx_rand_below(r, 3200u);   /* 0.06 .. 0.16 of the rush (the band: -24 .. -16 dB) */
        w->events++; w->total++;
    }
    if (w->eon) {
        int32_t g, f, lf;
        w->ex2 += ((w->ex1 - w->ex2) * w->ek) >> 16;
        w->ex1 -= ((w->ex1 * w->ek) >> 16) + (w->ex1 > 0);
        g = gmul(gmul(rg, w->eamp), ((w->ex2 >> 4) * 89) >> 5);
        lf = -152045 + l2 + 217706 - w->ed_l2;                 /* log2(St u / d), St 0.2 */
        f = tx_sin_q15(tx_exp2(clampi(lf, 500812 /* 200 Hz */, 739770 /* 2.5 kHz */)) * 97391u >> 1);
        svf_noise(&w->e, &w->ne, f, 655, w->eg, g, out, n, 2);   /* Q 25 */
        w->eg = g;
        if (w->ex1 < 64 && w->ex2 < 256) { w->eon = 0; w->e.low = w->e.band = 0; w->eg = 0; }
    }
}
