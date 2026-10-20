/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #1 Drip: water drops falling into a pool, each one a weak impact click and then the "plink" of the air
 * bubble it traps.
 *
 * A key press lets one drop fall; a held key keeps dripping at RATE, from a periodic tap to a random cave drip
 * (REGULAR). Each key is a bubble size: the radius at C4 is SIZE, one semitone higher = a 2^(1/12) smaller bubble
 * (pitch f0 = 3.28 m/s / a), SCATTER varies it drop to drop. Per drop:
 *   impact   a zero-mean force pulse (PULSE, N-wave), length ~0.3 D / U, level ~ U^2 (U = impact speed);
 *   bubble   after the crater collapses (25 ms at 1.4 m/s, 10 ms at 2.36 m/s: t ~ U^-1.75), with probability ~0.9
 *            in the regular-entrainment range (ours: falling off below 1 m/s); a BUBBLE hero, amplitude ~ a^1.5 x U;
 *            near the surface it rises (xi 0.05-0.15, cap 2 f0), deep ones (DEPTH) hold their pitch and are quieter;
 *   second   fast drops sometimes pinch off a second, bigger bubble (ours: irregular entrainment, P up to 0.4).
 * U from the fall height (HEIGHT) for a 4 mm drop: U = V_T sqrt(1 - exp(-2 g h / V_T^2)), V_T = 8.83 m/s; the key
 * velocity scales U (100 = the HEIGHT's speed). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_cloud.h"
#include "tx_core_bubble.h"
#include "tx_core_pulse.h"

#define OUT_TRIM 8460               /* output gain, Q12 (+6.3 dB): max momentary (one velocity-100 press): -37.6 LUFS at LEVEL 100 -> target -20 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter;
                                    * lowered 8.7 dB: the default chord <= +12 dBFS, the max scene <= +24 dBFS) */

enum { P_SIZE, P_SCATTER, P_RATE, P_REGULAR, P_DEPTH, P_HEIGHT, NP };

static const tx_param_t PARAMS[NP] = {
    {"SIZE", 127, 55, "bubble radius at C4: 0.25 .. 6 mm (pitch 13 kHz .. 550 Hz; keys: a semitone smaller each)"},
    {"SCATTER", 127, 16, "drop-to-drop bubble size spread: 0 .. +-1 octave"},
    {"RATE", 127, 72, "drops per second while a key is held: 0.1 .. 20 (log)"},
    {"REGULAR", 127, 80, "drip timing: 0 = random (cave, Poisson) .. 127 = periodic (a tap)"},
    {"DEPTH", 127, 40, "how deep bubbles pinch off: 0 = at the surface (all rise, plink) .. 127 = deep (no glide, softer)"},
    {"HEIGHT", 127, 48, "fall height: 1 cm .. 3 m (log) -> impact speed 0.4 .. 8.6 m/s: louder, harder click, faster bubble"},
};

#define U_REF_Q8 330               /* 1.29 m/s (a 4 mm drop from 86 mm), m/s Q8: the reference level */
#define AMP_L2 (28 << 16)          /* a bubble of the SIZE radius at U_REF: 2^28 Q30 = -12 dBFS */
#define CLICK_L2 (10 << 16)        /* the impact at U_REF: 2^10 Q15 = -30 dBFS */

typedef struct {
    tx_cloud_t cloud;
    tx_pool_t pool;
    tx_declick_t dc;
    tx_dcblock_t hp;
    tx_pulses_t clicks;
    tx_bubble_t b[TX_POOL_MAX];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK];
    uint8_t p[NP], quiet, pad;
    int32_t level;                 /* Q15 */
    int32_t size_l2;               /* log2(a_C4 / 1 mm), Q16 */
    int32_t scatter_q16;           /* +- spread, log2 units Q16 */
    uint32_t u_q8;                 /* impact speed at velocity 100, m/s Q8 */
    uint32_t rise_p16;             /* probability that a bubble rises, Q16 */
    uint16_t last_dropped;
} drip_t;

static uint32_t isqrt32(uint32_t x)                          /* floor(sqrt(x)), control rate */
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return r;
}

static int32_t log2_q16(uint32_t x)                         /* log2(x) in Q16, x >= 1 */
{
    return (32 << 16) - (int32_t)(tx_neglog2_q12(x) << 4);
}

static void set_param(drip_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_SIZE: s->size_l2 = -131072 + (int32_t)(v * 300480u / 127u); break;   /* log2(0.25 mm) + v log2(24) */
    case P_SCATTER: s->scatter_q16 = (int32_t)(v * 516u); break;              /* 0 .. 1 octave */
    case P_RATE: tx_cloud_rate(&s->cloud, tx_knob_exp(v, 26u, 500695)); break;  /* 0.1 * 2^(7.64 v / 127) /s, Q8 */
    case P_REGULAR: tx_cloud_regular(&s->cloud, 256u - v * 2u); break;
    case P_DEPTH: s->rise_p16 = 65535u - v * 516u; break;
    case P_HEIGHT: {
        uint32_t h = tx_knob_exp(v, 655u, 539301);           /* m Q16: 0.01 * 2^(8.23 v / 127) = 1 cm .. 3 m */
        uint32_t x = (uint32_t)(((uint64_t)h * 23789u) >> 16); /* 2 g h / V_T^2 / ln 2, Q16 */
        uint32_t e = x >= (16u << 16) ? 0u : tx_exp2((16 << 16) - (int32_t)x);   /* exp(-2gh/V_T^2), Q16 */
        s->u_q8 = (2260u * isqrt32((65536u - e) << 16)) >> 16;                     /* 8.83 m/s Q8 x sqrt, Q16 */
        if (s->u_q8 < 8u) s->u_q8 = 8u;
        break;
    }
    default: break;
    }
}

static void drip_init(void *st, uint32_t seed)
{
    drip_t *s = (drip_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);                         /* a key press: one drop at once */
    tx_cloud_sizes(&s->cloud, 6144, 0);                      /* every drop the same "size": the key sets it */
    tx_pool_init(&s->pool, TX_POOL_MAX, 12);
    tx_pulses_init(&s->clicks);
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
}

static void drip_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    tx_cloud_key(&((drip_t *)st)->cloud, note, vel, on);
}

static void drip_param(void *st, uint8_t id, uint8_t val)
{
    drip_t *s = (drip_t *)st;
    if (id == TX_PID_PARTICLES) {
        uint32_t cut = tx_pool_set_limit(&s->pool, val), i;
        for (i = 0; i < TX_POOL_MAX; i++)
            if (cut >> i & 1u) tx_declick_add(&s->dc, s->b[i].last);
    } else if (id == TX_PID_LEVEL) {
        int32_t nl = tx_level_q15(val > 127u ? 127u : val);
        tx_bubbles_rescale(s->b, &s->pool, s->level, nl);
        s->level = nl;
    } else
        set_param(s, id, val);
}

/* one bubble -> a hero (or rejected when every hero is louder) */
static void bubble(drip_t *s, int32_t a_l2, int32_t amp_l2, uint32_t xi_q16, uint32_t wait)
{
    uint32_t env, stolen;
    int slot;
    if (amp_l2 > (30 << 16)) amp_l2 = 30 << 16;
    env = tx_exp2(amp_l2);
    env = (uint32_t)(((uint64_t)env * (uint32_t)s->level) >> 15);
    if (env <= TX_BUBBLE_FLOOR) { s->pool.rejected++; return; }
    slot = tx_pool_take(&s->pool, env, &stolen);
    if (slot < 0) return;
    if (stolen) tx_declick_add(&s->dc, s->b[slot].last);
    tx_bubble_start(&s->b[slot], a_l2, xi_q16, env, wait);
}

static uint32_t chance(drip_t *s, uint32_t p16) { return (tx_rand(&s->cloud.rng) >> 16) < p16; }

/* one drop: the impact click now, the bubble(s) after the crater collapses */
static void drop(drip_t *s, const tx_event_t *e)
{
    tx_rng_t *r = &s->cloud.rng;
    uint32_t u = (s->u_q8 * e->vel) / 100u, p_bub, wait, dur;
    int32_t lu = log2_q16(u) - log2_q16(U_REF_Q8);           /* log2(U / U_ref), Q16 */
    int32_t a_l2, rel, amp, cl;
    if (u < 4u) u = 4u;
    /* impact: level ~ U^2, length ~ 0.3 D / U (D = 4 mm): 52.9 / U samples */
    cl = CLICK_L2 + 2 * lu;
    if (cl > (15 << 16)) cl = 15 << 16;
    dur = 13547u / u;
    dur = dur < 4u ? 4u : dur > 200u ? 200u : dur;
    tx_pulse_start(&s->clicks, TX_PULSE_SHAPE_NWAVE, dur,
                   (int32_t)(((uint64_t)tx_exp2(cl) * (uint32_t)s->level) >> 15), e->offset);
    /* the bubble: P ~0.9 above 1 m/s, down to 0.2 at 0.3 m/s (ours) */
    p_bub = u >= 256u ? 58982u : u <= 77u ? 13107u : 13107u + (u - 77u) * 256u;
    if (!chance(s, p_bub)) return;
    a_l2 = s->size_l2 - ((int32_t)e->note - 60) * 5461
         + (int32_t)(((int64_t)s->scatter_q16 * tx_rand_s15(r)) >> 15);
    rel = a_l2 - s->size_l2;                                 /* amplitude ~ a^1.5 against the SIZE radius, +-18 dB */
    rel = rel + rel / 2;
    rel = rel > (3 << 16) ? (3 << 16) : rel < -(3 << 16) ? -(3 << 16) : rel;
    amp = AMP_L2 + rel + (lu > (5 << 15) ? (5 << 15) : lu < -(6 << 16) ? -(6 << 16) : lu);
    /* crater collapse delay: 25 ms (1102 samples) x (1.4 / U)^1.75, +-25 %, 4 .. 45 ms */
    {
        int32_t l = 662300 + (7 * (log2_q16(358u) - log2_q16(u))) / 4;   /* log2(1102.5) = 10.106 Q16 */
        wait = tx_exp2(l);
        wait = wait + (uint32_t)(((int32_t)wait * (tx_rand_s15(r) / 4)) >> 15);
        wait = wait < 176u ? 176u : wait > 1984u ? 1984u : wait;
        wait += e->offset;
    }
    if (chance(s, s->rise_p16))                              /* near the surface: it rises, xi 0.05 .. 0.15 */
        bubble(s, a_l2, amp, 3277u + ((tx_rand(r) >> 16) * 6554u >> 16), wait);
    else                                                     /* deep: no rise, half the amplitude */
        bubble(s, a_l2, amp - 65536, 0, wait);
    /* a fast drop may pinch off a second, bigger bubble (ours): P = (U - 2 m/s) / 8, at most 0.4 */
    if (u > 512u) {
        uint32_t p2 = (u - 512u) * 32u;
        if (chance(s, p2 > 26214u ? 26214u : p2)) {
            int32_t big = 39322 + (int32_t)((tx_rand(r) >> 16) * 65536u >> 16);   /* 0.6 .. 1.6 octaves larger */
            bubble(s, a_l2 + big, amp - 65536 + (big + big / 2), 0, wait + 220u + tx_rand_below(r, 441u));
        }
    }
}

TX_AUDIO_CODE static void drip_render(void *st, int32_t *mix, unsigned n)
{
    drip_t *s = (drip_t *)st;
    unsigned k, i;
    uint32_t rej0 = s->pool.rejected;
    int32_t *buf = s->buf;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (i = 0; i < n; i++) buf[i] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    tx_pool_tick(&s->pool);
    for (i = 0; i < k; i++) drop(s, &s->ev[i]);
    tx_bubbles_render(s->b, &s->pool, &s->dc, buf, n);
    tx_pulses_add(&s->clicks, buf, n);
    tx_declick_run(&s->dc, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->last_dropped = (uint16_t)(s->cloud.last_dropped + (s->pool.rejected - rej0));
    s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !s->pool.alive && !tx_pulses_active(&s->clicks)
                         && !s->dc.v && !s->hp.x1 && !s->hp.y1);
}

static void drip_stats(const void *st, tx_stats_t *o)
{
    const drip_t *s = (const drip_t *)st;
    o->particles = (uint16_t)tx_pool_count(&s->pool);
    o->limit = s->pool.limit;
    o->events = s->cloud.last_events;
    o->dropped = s->last_dropped;
    o->steals = s->pool.steals;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: every hero, every key slot at the fastest rate, the biggest (longest) bubbles, the hardest drops */
static const char SCENE_MAX[] =
    "0 particles 32\n0 param RATE 127\n0 param SIZE 127\n0 param HEIGHT 127\n0 param SCATTER 127\n"
    "0 param DEPTH 0\n0 param REGULAR 0\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "500 on 53 127\n500 on 60 127\n500 on 67 127\n500 on 74 127\n"
    "1000 on 57 127\n1000 on 64 127\n1000 on 71 127\n1000 on 79 127\n"
    "2000 param SIZE 0\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_drip = {
    "drip", "#1 Drip (bubbles in a pool)", NP, PARAMS, TX_POOL_MAX, 12, sizeof(drip_t), SCENE_MAX,
    drip_init, drip_key, drip_param, drip_render, drip_stats,
};
