/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura "test" engine: decaying sine particles from the CLOUD core; proves keys -> emitter -> particles -> mix.
 *
 * Each event becomes one hero: a sine at the key's pitch (+ a random spread, + lower for bigger events), amplitude
 * from its power-law size and the key velocity, decaying exponentially (T60 = DECAY). Optional: a click per event
 * (PULSE core) and a band of noise while keys are held (NOISE core), so the three wave-0 cores are all exercised.
 *
 * Hero voice: 32-bit phase, quarter-sine table, Q30 envelope stepped linearly over 32-sample sub-blocks (the decay
 * factor per sub-block is Q16, applied with 32-bit products). ~15 ops per sample per hero. */
#include "tx_engine.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_pulse.h"

enum { P_RATE, P_BURST, P_DECAY, P_SPREAD, P_SIZES, P_CLICK, P_AIR, P_AFTER, NP };

static const tx_param_t PARAMS[NP] = {
    {"RATE", 127, 64, TX_TEXT("events per second while a key is held: 0.5 .. 2000 (log)")},
    {"BURST", 127, 16, TX_TEXT("particles fired by a key press: 0 .. 31")},
    {"DECAY", 127, 64, TX_TEXT("particle ring time T60: 20 ms .. 4 s (log)")},
    {"SPREAD", 127, 32, TX_TEXT("random pitch spread around the key: 0 .. +-24 semitones")},
    {"SIZES", 127, 64, TX_TEXT("power-law size exponent: alpha 3.0 (mostly small) .. 1.1 (many big)")},
    {"CLICK", 127, 0, TX_TEXT("a contact click at each particle's start (PULSE core)")},
    {"AIR", 127, 0, TX_TEXT("a noise band at the key's pitch while keys are held (NOISE core)")},
    {"AFTER", 127, 0, TX_TEXT("Omori aftershocks after each key press: 0 .. 15 on average")},
};

#define ENV_FLOOR (1u << 16)       /* a hero below this (Q30) ends: its output is under 2 LSB of Q23 */
#define ENV_BASE 26                /* the smallest particle at velocity 127: 2^26 Q30 = -24 dBFS */
#define SIZE_RANGE (6u << 12)      /* sizes span 6 octaves of energy: the biggest is 18 dB louder (-6 dBFS) */
#define SUB 32u

typedef struct {
    uint32_t ph, inc;              /* 32-bit phase and increment */
    uint32_t env;                  /* amplitude, Q30 */
    int32_t last;                  /* last output sample (for the declick if it is cut) */
    uint16_t wait;                 /* start offset in its first block */
} hero_t;

typedef struct {
    tx_cloud_t cloud;
    tx_pool_t pool;
    tx_declick_t dc;
    tx_dcblock_t hp;               /* the cloud's sum passes a DC blocker before the mix */
    tx_noise_t air;
    tx_pulses_t clicks;
    hero_t h[TX_POOL_MAX];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK];         /* this instance's sum, before the DC blocker */
    uint8_t p[NP], level_v, quiet, air_note;
    int32_t level;                 /* Q15 */
    uint32_t d32;                  /* Q16 envelope factor per 32 samples */
    int32_t spread_q16;
    uint32_t rejected_seen;
    uint16_t last_dropped;
} test_t;

static void set_param(test_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_RATE: tx_cloud_rate(&s->cloud, tx_knob_exp(v, 128u, 12 << 16)); break;   /* 0.5 * 2^(12 v / 127) /s */
    case P_BURST: tx_cloud_burst(&s->cloud, v >> 2, 40); break;
    case P_DECAY: {
        uint32_t t60 = tx_knob_exp(v, 20u, 500695);          /* ms: 20 * 2^(7.64 v / 127) */
        s->d32 = tx_exp2((16 << 16) - (int32_t)(473915u / t60));   /* 2^(-6.908 * 32 / (fs T60 ln 2)), Q16 */
        break;
    }
    case P_SPREAD: s->spread_q16 = (int32_t)v * 12384; break;   /* 24 semitones * 65536 / 127 */
    case P_SIZES: tx_cloud_sizes(&s->cloud, 12288u - v * 61u, SIZE_RANGE); break;
    case P_AFTER: tx_cloud_omori(&s->cloud, v >> 3, 6881, 70, 7168); break;   /* p 1.68, c 70 ms, gap 5.8 oct */
    default: break;
    }
}

static void set_level(test_t *s, unsigned v)
{
    int32_t nl = tx_level_q15(v > 127u ? 127u : v);
    unsigned i;
    if (nl != s->level)                                      /* rescale the sounding heroes (control rate): */
        for (i = 0; i < TX_POOL_MAX; i++)                    /* env * new / old, no 64-bit division */
            if (s->pool.alive >> i & 1u) {
                uint32_t q12 = s->level ? ((uint32_t)nl << 12) / (uint32_t)s->level : 0;   /* ratio, Q12 */
                uint64_t e = ((uint64_t)s->h[i].env * q12) >> 12;
                s->h[i].env = e > (1u << 30) ? (1u << 30) : (uint32_t)e;
            }
    s->level = nl;
    s->level_v = (uint8_t)v;
}

static void test_init(void *st, uint32_t seed)
{
    test_t *s = (test_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_pool_init(&s->pool, TX_POOL_MAX, 16);
    tx_noise_init(&s->air, seed ^ 0xA5A5A5A5u);
    tx_noise_flicker(&s->air, 8000);
    tx_pulses_init(&s->clicks);
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->level_v = 100;
    s->quiet = 1;
}

static void test_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    test_t *s = (test_t *)st;
    tx_cloud_key(&s->cloud, note, vel, on);
    if (on) s->air_note = note;
}

static void test_param(void *st, uint8_t id, uint8_t val)
{
    test_t *s = (test_t *)st;
    if (tx_cloud_keyat(&s->cloud, id, val)) return;          /* (a press's own sample: TX_PID_KEYAT) */
    if (id == TX_PID_PARTICLES) {
        uint32_t cut = tx_pool_set_limit(&s->pool, val), i;
        for (i = 0; i < TX_POOL_MAX; i++)
            if (cut >> i & 1u) tx_declick_add(&s->dc, s->h[i].last);
    } else if (id == TX_PID_LEVEL)
        set_level(s, val);
    else
        set_param(s, id, val);
}

/* one event -> a hero (or rejected when every hero is louder) */
static void spawn(test_t *s, const tx_event_t *e)
{
    int32_t amp_l2 = e->size_l2 / 2;                             /* amplitude ~ sqrt(E): +3 dB per energy octave */
    uint32_t g = tx_exp2(amp_l2 * 16 + (ENV_BASE << 16));        /* 2^(26 + amp_l2) <= 2^29 */
    uint32_t vq = (uint32_t)e->vel * e->vel * 2u;                /* velocity^2, ~Q15 */
    uint32_t env = (g >> 15) * vq, stolen;                     /* Q30 x Q15 (32-bit: g <= 2^28) */
    int32_t note_q16;
    int slot;
    env = (uint32_t)(((uint64_t)env * (uint32_t)s->level) >> 15);   /* LEVEL (per event: 64-bit product) */
    if (env <= ENV_FLOOR) { s->pool.rejected++; return; }
    slot = tx_pool_take(&s->pool, env, &stolen);
    if (slot < 0) return;
    if (stolen) tx_declick_add(&s->dc, s->h[slot].last);
    note_q16 = (int32_t)e->note * 65536
             + (int32_t)(((int64_t)s->spread_q16 * tx_rand_s15(&s->cloud.rng)) >> 15)
             - e->size_l2 * 16;                                    /* bigger: lower, 1 semitone per energy octave */
    s->h[slot].ph = 0;
    s->h[slot].inc = tx_note_inc(note_q16);
    s->h[slot].env = env;
    s->h[slot].last = 0;
    s->h[slot].wait = e->offset;
    if (s->p[P_CLICK]) {
        int32_t a = (int32_t)(((env >> 15) * (uint32_t)s->p[P_CLICK] * 258u) >> 15);
        tx_pulse_start(&s->clicks, TX_PULSE_SHAPE_NWAVE, 44, a, e->offset);   /* zero-mean: no DC in dense clouds */
    }
}

TX_AUDIO_CODE static void hero_render(hero_t *h, uint32_t d32, int32_t *out, unsigned n)
{
    uint32_t ph = h->ph, inc = h->inc, env = h->env;
    unsigned j = h->wait < n ? h->wait : n;
    int32_t y = h->last;
    h->wait = 0;
    while (j < n) {
        unsigned end = j + SUB < n ? j + SUB : n, len = end - j;
        uint32_t e1 = (env >> 16) * d32 + (((env & 0xFFFFu) * d32) >> 16);   /* env * d32 (Q16), 32-bit products */
        int32_t e = (int32_t)env, step = len == SUB ? ((int32_t)e1 - e) / (int32_t)SUB : ((int32_t)e1 - e) / (int32_t)len;
        for (; j < end; j++) {
            ph += inc;
            y = (tx_sin_q15(ph) * (e >> 15)) >> 7;            /* Q15 x Q15 -> Q23 */
            out[j] += y;
            e += step;
        }
        env = e1;
    }
    h->ph = ph; h->env = env; h->last = y;
}

TX_AUDIO_CODE static void test_render(void *st, int32_t *mix, unsigned n)
{
    test_t *s = (test_t *)st;
    unsigned k, i, held;
    uint32_t rej0 = s->pool.rejected;
    int32_t *buf = s->buf;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (i = 0; i < n; i++) buf[i] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    tx_pool_tick(&s->pool);
    for (i = 0; i < k; i++) spawn(s, &s->ev[i]);
    for (i = 0; i < TX_POOL_MAX; i++) {
        hero_t *h = &s->h[i];
        if (!(s->pool.alive >> i & 1u)) continue;
        hero_render(h, s->d32, buf, n);
        if (h->env < ENV_FLOOR) { tx_declick_add(&s->dc, h->last); h->last = 0; tx_pool_free(&s->pool, i); }
        else s->pool.level[i] = h->env;
    }
    held = tx_cloud_held(&s->cloud);                       /* the AIR band follows the last key pressed */
    if (s->p[P_AIR] && held) {
        uint32_t fc = tx_note_inc((int32_t)s->air_note << 16) / 97391u;   /* Hz (2^32 / fs = 97391.5) */
        tx_noise_band(&s->air, fc * 2u, 32, TX_NOISE_BAND);
        tx_noise_level(&s->air, (int32_t)(((uint32_t)s->p[P_AIR] * 129u * (uint32_t)s->level) >> 15));
    } else
        tx_noise_level(&s->air, 0);
    tx_noise_add(&s->air, buf, n);
    tx_pulses_add(&s->clicks, buf, n);
    tx_declick_run(&s->dc, buf, n);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->last_dropped = (uint16_t)(s->cloud.last_dropped + (s->pool.rejected - rej0));
    s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !s->pool.alive && tx_noise_quiet(&s->air)
                         && !tx_pulses_active(&s->clicks) && !s->dc.v && !s->hp.x1 && !s->hp.y1);
}

static void test_stats(const void *st, tx_stats_t *o)
{
    const test_t *s = (const test_t *)st;
    o->particles = (uint16_t)tx_pool_count(&s->pool);
    o->limit = s->pool.limit;
    o->events = s->cloud.last_events;
    o->dropped = s->last_dropped;
    o->steals = s->pool.steals;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: every hero, every key slot, the densest stream, bursts and aftershocks re-fired, clicks and air */
static const char SCENE_MAX[] =
    TX_TEXT("0 particles 32\n0 param RATE 127\n0 param BURST 127\n0 param DECAY 127\n0 param AFTER 127\n"
    "0 param CLICK 127\n0 param AIR 127\n0 param SIZES 127\n0 param SPREAD 127\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "500 on 53 127\n500 on 60 127\n500 on 67 127\n500 on 74 127\n"
    "1000 on 57 127\n1000 on 64 127\n1000 on 71 127\n1000 on 79 127\n"
    "1500 on 53 127\n1500 on 60 127\n1500 on 67 127\n1500 on 74 127\n"
    "2000 on 57 127\n2000 on 64 127\n2000 on 71 127\n2000 on 79 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4000 end\n");

const tx_engine_t tx_eng_test = {
    "test", TX_TEXT("Test tone (sine particles)"), NP, PARAMS, TX_POOL_MAX, 16, TX_STATE_SIZE(test_t), SCENE_MAX,
    test_init, test_key, test_param, test_render, test_stats,
};
