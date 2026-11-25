/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #2 Rain on water: rain falling on a lake, a pond, a puddle.
 *
 * A held key = rain (it starts in ~0.1 s and passes over ~3-4 s); a press also drops one big drop near you (velocity = size).
 * Physics: in water the sound is the bubbles, and the four drop classes behave differently (L21, L18):
 *   small      0.7-1.2 mm: a tiny "type I" bubble (a 0.16-0.25 mm: 13-20 kHz, T60 ~2 ms), with probability p_I
 *              (note 02 open question: 0.5, ours); wind (oblique drops) suppresses it (L21). The shimmer of light rain.
 *   medium     1.2-2.4 mm: only the weak impact click.
 *   large      2.4-4.1 mm: a click, then a "type II" bubble 50-80 ms later (1 to > 30 kHz, mostly 2-10 kHz).
 *   very large > 4.1 mm: a click, a type II bubble, and a "type III" one 100-250 ms after impact (3 to > 20 kHz).
 * Arrival rates per m^2 per s from Marshall-Palmer x Gunn-Kinzer (L15 x L16, note 02's table, log-log in R), times
 * the listening AREA; STORM shifts the size mix (Lambda). Energy per drop VL : L : S : M ~ 400 : 25 : 1 : 0.25 (L21,
 * underwater; used as amplitude ratios, adjusted against the references). Bubbles are BUBBLE heroes (Minnaert pitch,
 * Longuet-Higgins damping, amplitude ~ a^1.5, near ones rise, L2); type I bubbles beyond the hero pool become a noise
 * bed of the same power (Campbell: rate x A^2 / 4d) high-passed at 12 kHz; the medium clicks are a shot train.
 * DIST: the air between (a high-cut, ours) and 1 / r. */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_bubble.h"

#define OUT_TRIM 5653               /* output gain, Q12 (+0.8 dB): integrated (a key held 10 s): -26.8 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +2.0 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 120
#define VARY_DRIFT 60
#define VARY_SPLIT 0

enum { P_RATE, P_STORM, P_AREA, P_WIND, P_DIST, NP };

static const tx_param_t PARAMS[NP] = {
    {"RATE", 127, 70, TX_TEXT("rain rate R: 0.1 .. 100 mm/h (log): drizzle .. downpour")},
    {"STORM", 127, 64, TX_TEXT("storm type: stratiform (small drops: the 15 kHz shimmer) .. convective (big drops: plinks)")},
    {"AREA", 127, 100, TX_TEXT("listening area: 0.1 .. 20 m^2 (log): a few drops near you .. a whole pond")},
    {"WIND", 127, 0, TX_TEXT("wind: calm .. windy (oblique drops lose the type I shimmer; gusts)")},
    {"DIST", 127, 20, TX_TEXT("distance: close (all the highs) .. across the lake (high-cut 2.5 kHz, quieter)")},
};

#define NC 4u
#define NH TX_POOL_MAX
#define A_I_L2 ((28 << 16) + 32728)   /* a type I bubble at a 1 mm (Q30 log2; x a^1.5): set so the energy per drop
                                     * is L21's 1 : 25 against type II (calibrated against the references) */
#define A_II_L2 ((28 << 16) + 10460)   /* a type II bubble at a 1 mm: -17 dBFS */
#define CLICK_M 4000               /* a medium drop's click (Q15) */
#define CLICK_L 4700               /* a large drop's click (Q15) */

/* arrivals per m^2 per s at R = 1, 5, 25, 100 mm/h (note 02), x 100: type I, click, type II, very large */
static const uint32_t RATE_TAB[NC][4] = {
    {34000, 97700, 211100, 345000}, {7400, 42800, 159800, 378100}, {80, 1900, 19500, 89400}, {1, 20, 670, 7700}
};

typedef struct {
    tx_cloud_t cloud;
    tx_pool_t pool;
    tx_declick_t dc;
    tx_dcblock_t hp;
    tx_bubble_t b[NH];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK], ck[TX_BLOCK];
    uint32_t lam_q8[NC];           /* arrivals per second over the AREA, Q8 */
    uint32_t pI_q16;               /* probability of a type I bubble */
    int32_t wet, gust;             /* rain on (Q15 ramp); gust (Q15 signed) */
    int32_t clp, bhp, blp, olp, olpa, ga;   /* click low-pass, bed filter's last two inputs, the distance low-pass */
    int32_t bed_sd;                /* the type I bed's sd now (Q15) */
    uint32_t x32;
    uint8_t p[NP], limit, quiet, pad;
    int32_t level;
    uint16_t dropped;
    tx_vary_t vy;
} rw_t;

static uint32_t tx_isqrt_rw(uint32_t x)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) { if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return r;
}

static int32_t log2_q16(uint32_t x) { return (32 << 16) - (int32_t)(tx_neglog2_q12(x ? x : 1u) << 4); }

static void rates(rw_t *s)
{
    static const int32_t LR[4] = { 0, 152170, 304340, 435412 };   /* log2 1, 5, 25, 100 */
    int32_t lr = -217706 + (int32_t)(s->p[P_RATE] * 5142u);   /* log2(0.1) + v log2(1000) / 127 */
    int32_t la = -217706 + (int32_t)(s->p[P_AREA] * 3896u);   /* log2(0.1 m^2) + v log2(200) / 127 */
    int32_t shift = ((int32_t)s->p[P_STORM] - 64) * 2048;     /* +-2 octaves of the size mix */
    unsigned c, k;
    for (c = 0; c < NC; c++) {
        int32_t l0, l1, lv;
        k = lr < LR[1] ? 0u : lr < LR[2] ? 1u : 2u;
        l0 = log2_q16(RATE_TAB[c][k]); l1 = log2_q16(RATE_TAB[c][k + 1]);
        lv = l0 + ((((l1 - l0) >> 4) * ((lr - LR[k]) >> 4)) / ((LR[k + 1] - LR[k]) >> 8));   /* log-log, 32-bit */
        lv += la - 435412 + (8 << 16);                       /* / 100 (the table), x area, Q8 */
        lv += c >= 2u ? shift : -shift / 2;                  /* convective: more big drops, fewer small */
        s->lam_q8[c] = lv > (26 << 16) ? (1u << 26) : lv < 0 ? 0u : tx_exp2(lv);
    }
    s->pI_q16 = (uint32_t)(32768 - (int32_t)s->p[P_WIND] * 250);   /* 0.5 calm .. ~0.02 windy */
}

static void set_param(rw_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    if (id == P_DIST) {                                      /* high-cut 20 kHz .. 2.5 kHz (log), level 1 .. 1/4 */
        uint32_t fc = tx_knob_exp(127u - v, 2500u, 196608);
        s->olpa = 32768 - (int32_t)tx_exp2((15 << 16) - (int32_t)((fc > 20000u ? 20000u : fc) * 862u >> 6));
        if (s->olpa < 64) s->olpa = 64;
        s->ga = (int32_t)tx_exp2((15 << 16) - (int32_t)v * 1032);   /* -12 dB over the range */
    } else rates(s);
}

static void rw_init(void *st, uint32_t seed)
{
    rw_t *s = (rw_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    s->limit = 16;
    tx_pool_init(&s->pool, NH, s->limit);
    s->x32 = seed * 2654435761u + 99u;
    if (!s->x32) s->x32 = 1;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((rw_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void rw_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((rw_t *)st)->vy);
    tx_cloud_key(&((rw_t *)st)->cloud, note, vel, on);
}

static void rw_param(void *st, uint8_t id, uint8_t val)
{
    rw_t *s = (rw_t *)st;
    if (tx_cloud_keyat(&s->cloud, id, val)) return;          /* (a press's own sample: TX_PID_KEYAT) */
    if (id == TX_PID_PARTICLES) {
        uint32_t cut, i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NH ? NH : val);
        cut = tx_pool_set_limit(&s->pool, s->limit);
        for (i = 0; i < NH; i++) if (cut & (1u << i)) { tx_declick_add(&s->dc, s->b[i].last); s->b[i].env = 0; }
    } else if (id == TX_PID_LEVEL) {
        int32_t nl = tx_level_q15(val > 127u ? 127u : val);
        tx_bubbles_rescale(s->b, &s->pool, s->level, nl);
        s->level = nl;
    } else
        set_param(s, id, val);
}

/* a bubble hero: radius log2(a / 1 mm) Q16 uniform in [lo, hi], amplitude (Q30 log2 at a 1 mm) x a^(pw / 2), after
 * wait */
static void bubble(rw_t *s, int32_t lo, int32_t hi, int32_t amp_l2, int pw, uint32_t wait, int rise)
{
    tx_rng_t *r = &s->cloud.rng;
    int32_t a_l2 = lo + (int32_t)(((uint32_t)(hi - lo) >> 8) * (tx_rand(r) >> 24));
    int32_t l = amp_l2 + (pw * a_l2) / 2 - (int32_t)(tx_rand(r) >> 16);   /* x a^(pw/2), x 2^(0 .. -1) (depth) */
    uint32_t env, stolen;
    int slot;
    l = l > (29 << 16) ? (29 << 16) : l;
    env = tx_exp2(l);
    env = (uint32_t)(((uint64_t)env * (uint32_t)s->level) >> 15);
    if (!env) return;
    slot = tx_pool_take(&s->pool, env, &stolen);
    if (slot < 0) { s->dropped++; return; }
    if (stolen) tx_declick_add(&s->dc, s->b[slot].last);
    tx_bubble_start(&s->b[slot], a_l2, rise && (tx_rand(r) & 1u) ? 6554u : 0u, env, wait);
}

/* a big drop: its click now, a type II bubble after 50-80 ms, and (very large) a type III one after 100-250 ms */
static void big_drop(rw_t *s, unsigned off, int32_t click, int vl)
{
    tx_rng_t *r = &s->cloud.rng;
    int32_t c = (int32_t)(((int64_t)click * s->level) >> 15);
    s->ck[off] += (tx_rand(r) & 1u) ? c : -c;
    /* type II: a 0.33 .. 1.1 mm (3 .. 10 kHz, the bulk of L21's 1 to > 30 kHz), amplitude ~ a^0.5 (ours: the bubble
     * sizes of rain are an open question, note 02; a^1.5 made the 2 kHz ones ring 15 dB over a flat reference) */
    bubble(s, -104858, 9011, A_II_L2, 1, off + 2205u + tx_rand_below(r, 1323u), 1);
    if (vl) bubble(s, -170000, 9830, A_II_L2 - 65536, 1, off + 4410u + tx_rand_below(r, 6615u), 1);   /* 3 .. 20 kHz */
}

/* a Poisson count with mean mu (Q8), up to cap: exponential gaps */
static unsigned poisson(rw_t *s, uint32_t mu_q8, unsigned cap)
{
    uint32_t mean, acc;
    unsigned k = 0;
    if (!mu_q8) return 0;
    mean = 65536u / mu_q8;
    acc = tx_cloud_exp_q8(&s->cloud, mean);
    while (acc < 256u && k < cap) { k++; acc += tx_cloud_exp_q8(&s->cloud, mean); }
    return k;
}

TX_AUDIO_CODE static void rw_render(void *st, int32_t *mix, unsigned n)
{
    rw_t *s = (rw_t *)st;
    tx_rng_t *r = &s->cloud.rng;
    unsigned k, i, j;
    int32_t *buf = s->buf, *ck = s->ck, z;
    uint32_t x32 = s->x32;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;                              /* silent and nothing new */
    for (j = 0; j < n; j++) buf[j] = ck[j] = 0;
    for (i = 0; i < k; i++) {                                /* a press: one big drop near you (velocity = size) */
        const tx_event_t *e = &s->ev[i];
        big_drop(s, e->offset, CLICK_L * 2 * e->vel / 127, e->vel > 90);
    }
    {   /* rain: on while a key is held; released, the shower passes over ~3-4 s (session 5: no hard stop): the drop rate
     * thins out, the last drops sparse; wind gusts the rate */
        unsigned held = tx_cloud_held(&s->cloud);
        s->wet += held ? (32768 - s->wet) >> 4 : -((s->wet >> 7) + 1);   /* (released: tau 0.74 s, the rate thins) */
        if (s->wet < 0) s->wet = 0;                          /* (released: it thins out to nothing, no last step) */
        s->gust += ((tx_rand_s15(r) * (int32_t)s->p[P_WIND]) >> 10) - (s->gust >> 6);
        s->gust = s->gust > 16384 ? 16384 : s->gust < -16384 ? -16384 : s->gust;
    }
    s->bed_sd = 0;
    if (s->wet) {
        int32_t w = (int32_t)(((int64_t)s->wet * (32768 + s->gust)) >> 15);
        uint32_t lam[NC], mu[NC];
        unsigned c, room;
        for (c = 0; c < NC; c++) {
            lam[c] = (uint32_t)(((uint64_t)s->lam_q8[c] * (uint32_t)w) >> 15);
            mu[c] = (lam[c] >> 4) * n / 2756u;               /* expected in this block, Q8 (fs / 16 = 2756) */
        }
        /* large and very large: few, all explicit */
        {
            unsigned nl = poisson(s, mu[2], 8u), nv = poisson(s, mu[3], 4u);
            for (i = 0; i < nl; i++) big_drop(s, tx_rand_below(r, n), CLICK_L, 0);
            for (i = 0; i < nv; i++) big_drop(s, tx_rand_below(r, n), CLICK_L * 2, 1);
        }
        /* small: type I bubbles, explicit while the pool has room (at most 12 a block), the rest a bed */
        {
            uint32_t muI = (uint32_t)(((uint64_t)mu[0] * s->pI_q16) >> 16), want = 0, rest_q8 = 0;
            room = tx_pool_count(&s->pool) < s->limit ? s->limit - tx_pool_count(&s->pool) : 0u;
            if (room > 12u) room = 12u;
            if (muI >= (16u << 8)) rest_q8 = muI;
            else {
                want = poisson(s, muI, 64u);
                if (want > room) { rest_q8 = (want - room) << 8; want = room; }
            }
            for (i = 0; i < want; i++) bubble(s, -170000, -127000, A_I_L2, 3, tx_rand_below(r, n), 0);   /* 0.17 .. 0.25 mm */
            if (rest_q8) {                                   /* Campbell: var = rate E[A^2] / (4 d), d ~ 3200 / s */
                uint32_t dps_q16 = ((rest_q8 > (1u << 22) ? (1u << 22) : rest_q8) << 8) / n;   /* per sample, Q16 */
                /* the explicit ones: A = 2^-3.16 x a^1.5 (a ~ 0.2 mm: 2^-3.5) x 2^(-U): E[A^2] = 0.541 x 2^-13.3 FS^2,
                 * so var = p x 3.45 x 0.541 x 2^-13.3 and sd (Q15) = 2.48 sqrt(p_q16); / 1.155 for the uniform source,
                 * x 1.8 for the share of the filtered noise outside 13-20 kHz (by render): 3.84 sqrt(p_q16) */
                uint32_t sd = (tx_isqrt_rw(dps_q16) * 795000u) >> 16;   /* x 3.16: A_I is +10 dB on that */
                s->bed_sd = (int32_t)((sd * (uint32_t)s->level) >> 15);
            }
        }
        /* medium: the impact clicks, a shot train (Bernoulli per sample), and the clicks of the dropped small ones */
        {
            uint32_t p16 = ((lam[1] >> 6) * 380u) >> 10;      /* per sample, Q16: lam / fs (380 / 2^16 = 256 / fs) */
            int32_t a = (int32_t)(((int64_t)CLICK_M * s->level) >> 15);
            if (p16 > 16384u) p16 = 16384u;
            for (j = 0; j < n; j++) {
                x32 ^= x32 << 13; x32 ^= x32 >> 17; x32 ^= x32 << 5;
                if ((x32 & 0xFFFFu) < p16) ck[j] += (((int32_t)(x32 >> 16) - 32768) * a) >> 15;
            }
        }
    }
    /* the clicks: a soft impact (one-pole ~4 kHz), Q15 -> Q23 */
    {
        int32_t lp = s->clp, in = 0;
        for (j = 0; j < n; j++) { in |= ck[j]; lp += ((ck[j] - lp) * 15) >> 5; buf[j] += lp * 256; }
        s->clp = (lp > -8 && lp < 8 && !in) ? 0 : lp;        /* floor rounding sticks it at -2 .. 2 */
    }
    /* the type I bed: white (sd) through a second difference / 3 (+12 dB / oct, unity near 15 kHz, -22 dB at 2 kHz:
     * the bubbles sit at 13-20 kHz) */
    if (s->bed_sd || s->bhp || s->blp) {
        int32_t sd = s->bed_sd > 32767 ? 32767 : s->bed_sd, x1 = s->blp, x2 = s->bhp;
        for (j = 0; j < n; j++) {
            int32_t x;
            x32 ^= x32 << 13; x32 ^= x32 >> 17; x32 ^= x32 << 5;
            x = (((int32_t)(x32 >> 17) - 16384) * sd) >> 13;    /* uniform, sd ~ bed_sd x 2 / sqrt 3 */
            buf[j] += (x - 2 * x1 + x2) * 85;                /* / 3, Q15 -> Q23 */
            x2 = x1; x1 = x;
        }
        if (!sd) x1 = x2 = 0;
        s->blp = x1; s->bhp = x2;
    }
    s->x32 = x32 ? x32 : 1u;
    tx_bubbles_render(s->b, &s->pool, &s->dc, buf, n);
    tx_pool_tick(&s->pool);
    tx_declick_run(&s->dc, buf, n);
    {   /* distance: the air's high-cut and 1 / r (32-bit split products) */
        int32_t lp = s->olp, a = s->olpa, g = s->ga, any = 0;
        for (j = 0; j < n; j++) {
            int32_t d = buf[j] - lp;
            any |= buf[j];                                   /* the input: silent in, small state -> exact zero */
            lp += ((d >> 15) * a) + (((d & 0x7FFF) * a) >> 15);
            buf[j] = ((lp >> 15) * g) + (((lp & 0x7FFF) * g) >> 15);
        }
        if (lp > -64 && lp < 64 && !any) {                   /* floor rounding holds a one-pole a few LSB off zero */
            lp = 0;
            for (j = 0; j < n; j++) buf[j] = 0;
        }
        s->olp = lp;
    }
    tx_vary_block(&s->vy, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    z = s->hp.x1 | s->hp.y1 | s->wet | s->olp | s->clp | s->blp | s->bhp | s->dc.v | (int32_t)s->pool.alive;
    s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
}

static void rw_stats(const void *st, tx_stats_t *o)
{
    const rw_t *s = (const rw_t *)st;
    o->particles = (uint16_t)tx_pool_count(&s->pool);
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = s->pool.steals;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: a convective downpour over a big pond, every hero busy, big drops pressed */
static const char SCENE_MAX[] =
    TX_TEXT("0 particles 32\n0 param RATE 127\n0 param STORM 127\n0 param AREA 127\n0 param DIST 0\n"
    "0 on 53 127\n0 on 60 127\n0 on 67 127\n0 on 79 127\n1000 on 53 127\n1500 param STORM 0\n2000 param AREA 40\n"
    "3000 off 53\n3000 off 60\n3000 off 67\n3000 off 79\n"
    "4000 end\n");

const tx_engine_t tx_eng_rainwater = {
    "rainwater", TX_TEXT("#2 Rain on water"), NP, PARAMS, NH, 16, TX_STATE_SIZE(rw_t), SCENE_MAX,
    rw_init, rw_key, rw_param, rw_render, rw_stats,
};
