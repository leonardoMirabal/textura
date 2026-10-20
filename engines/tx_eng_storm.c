/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #26 Storm (scene): rain, wind and thunder under one intensity control (02, 03, 11).
 *
 * A held key = the storm (it builds and fades over ~1 s); while it lasts, lightning strikes at random (Poisson, a few
 * per minute, more and nearer as the storm grows) and the rain and wind follow INTENSITY. A press also strikes a
 * flash now (velocity = how close). The parts:
 *   rain     a rain engine run inside this one through the public engine API (its own state in an arena here):
 *            #2 rain on water, or #3 rain on surfaces (leaves .. metal roof), by SURFACE. INTENSITY sets the rain
 *            rate (0.5 .. 60 mm/h); AGE moves its size mix from a young convective cell (big drops, loud) to the
 *            stratiform rain after it (small drops, the 13-20 kHz shimmer): L21's time course (note 10).
 *   wind     a gusting broadband roar (NOISE roar path, ~250 Hz, Q 0.5, tilted), level ~ u^3 (u 3 .. 20 m/s with
 *            INTENSITY), gusts as a slow random walk (~0.3 Hz, +-40 %) and a flutter (ours, as #22's bed).
 *   thunder  one flash at a time (THUNDER core: the channel model channel, N-waves by distance); the cell's distance DIST,
 *            each flash at DIST x 2^(+-1) (nearer as INTENSITY rises), energy and height drawn per flash. */
#include "tx_engine.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_thunder.h"

extern const tx_engine_t tx_eng_rainwater;
extern const tx_engine_t tx_eng_rainsurf;

enum { P_INTENS, P_SURFACE, P_AGE, P_DIST, NP };

static const tx_param_t PARAMS[NP] = {
    {"INTENSITY", 127, 64, "storm intensity: rain 0.5 .. 60 mm/h, wind 3 .. 20 m/s, flashes 1 .. 12 / min (nearer)"},
    {"SURFACE", 127, 70, "where the rain falls: water (#2) | leaves, tent, wood, glass, metal roof (#3)"},
    {"AGE", 127, 30, "storm age: young convective cell (big drops, loud) .. stratiform rain after it (fine drops)"},
    {"DIST", 127, 60, "distance of the thunder cell: 0.5 .. 15 km (log)"},
};

#define ARENA 4800u                /* bytes for the rain engine's state (rain on water 4588, on surfaces 4324) */
#define RAIN_P 8u                  /* the rain's PARTICLES inside the scene (the scene's CPU share) */
#define WIND_REF 14000              /* the wind bed's gain at 10 m/s (Q15; calibrated by render) */

typedef struct {
    uint32_t arena[ARENA / 4u];
    tx_cloud_t cloud;
    tx_noise_t wind;
    tx_flash_t fl;
    tx_dcblock_t hp;
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK], tb[TX_BLOCK];
    const tx_engine_t *rain;
    uint32_t seed, next;           /* samples to the next flash while the storm is on */
    int32_t on, gust, flut, u_l2;
    uint8_t p[NP], quiet, rain_on, dirty, lv;
    int32_t level;
    uint16_t dropped;
} storm_t;

static void rain_set(storm_t *s, const char *name, uint8_t v)
{
    int id = tx_param_find(s->rain, name);
    if (id >= 0) s->rain->param(s->arena, (uint8_t)id, v);
}

/* (re)choose the rain engine and push the scene's controls into it */
static void rain_setup(storm_t *s)
{
    const tx_engine_t *e = s->p[P_SURFACE] < 22u ? &tx_eng_rainwater : &tx_eng_rainsurf;
    uint8_t rate = (uint8_t)(30u + s->p[P_INTENS] * 87u / 127u);   /* 0.5 .. 60 mm/h on the rain's 0.1 .. 100 scale */
    uint8_t mix = (uint8_t)(110u - s->p[P_AGE] * 90u / 127u);      /* convective 110 .. stratiform 20 */
    if (e != s->rain) {
        if (e->state_bytes > ARENA) { s->rain = 0; return; }
        s->rain = e;
        e->init(s->arena, s->seed);
        e->param(s->arena, TX_PID_PARTICLES, RAIN_P);
        s->rain_on = 0;
    }
    rain_set(s, "RATE", rate);
    if (e == &tx_eng_rainwater) { rain_set(s, "STORM", mix); rain_set(s, "WIND", (uint8_t)(s->p[P_INTENS] / 2u)); }
    else {
        rain_set(s, "DROPS", mix);
        rain_set(s, "SURFACE", (uint8_t)((s->p[P_SURFACE] - 22u) * 127u / 105u));
    }
    e->param(s->arena, TX_PID_LEVEL, (uint8_t)(s->lv * 7u / 10u));   /* the rain -6 dB under the scene's level */
    s->dirty = 0;
}

static void set_param(storm_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    if (id == P_INTENS) s->u_l2 = 103872 + (int32_t)(v * 1610u);   /* log2(3 m/s) .. log2(20 m/s) */
    s->dirty = 1;
}

static void storm_init(void *st, uint32_t seed)
{
    storm_t *s = (storm_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    s->seed = seed;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    tx_noise_init(&s->wind, seed ^ 0x57041u);
    tx_noise_band(&s->wind, 250, 8, TX_NOISE_BAND);
    tx_noise_tilt(&s->wind, 600);
    s->level = tx_level_q15(100);
    s->lv = 100;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    rain_setup(s);
    s->quiet = 1;
}

static void storm_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    tx_cloud_key(&((storm_t *)st)->cloud, note, vel, on);
}

static void storm_param(void *st, uint8_t id, uint8_t val)
{
    storm_t *s = (storm_t *)st;
    if (id == TX_PID_PARTICLES) return;                      /* one flash, the rain's heroes fixed (RAIN_P) */
    if (id == TX_PID_LEVEL) {
        s->lv = (uint8_t)(val > 127u ? 127u : val);
        s->level = tx_level_q15(s->lv);
        if (s->rain) s->rain->param(s->arena, TX_PID_LEVEL, (uint8_t)(s->lv * 7u / 10u));
    } else
        set_param(s, id, val);
}

/* a flash: at the cell's distance x 2^(+-1), nearer with intensity and velocity; energy and height drawn */
static void strike(storm_t *s, uint32_t vel)
{
    tx_rng_t *r = &s->cloud.rng;
    int32_t d_l2 = 517000 + (int32_t)(s->p[P_DIST] * 2523u)   /* log2(500 m) .. log2(15 km) */
                 + tx_rand_s15(r) * 2 - (int32_t)s->p[P_INTENS] * 258 - ((int32_t)vel - 64) * 1024;
    int32_t el2 = 131072 + (int32_t)(tx_rand(r) >> 16) * 3;  /* E_l 4e4 .. 3e5 J/m */
    uint32_t h = 2000u + tx_rand_below(r, 3000u);
    int32_t lv = (int32_t)(((uint32_t)(vel > 127u ? 127u : vel) * 258u * (uint32_t)s->level) >> 15);   /* ~ velocity */
    d_l2 = d_l2 < 566000 ? 566000 : d_l2 > 910000 ? 910000 : d_l2;   /* 0.4 .. 15 km */
    if (s->fl.active) s->dropped++;
    tx_flash_start(&s->fl, r, tx_exp2(d_l2), h, 256u, el2, lv);
}

TX_AUDIO_CODE static void storm_render(void *st, int32_t *mix, unsigned n)
{
    storm_t *s = (storm_t *)st;
    tx_rng_t *r = &s->cloud.rng;
    unsigned k, i, j, held;
    int32_t *buf = s->buf, z, rq = 1;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    if (s->dirty) rain_setup(s);
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;                              /* silent and nothing new */
    for (j = 0; j < n; j++) buf[j] = 0;
    held = tx_cloud_held(&s->cloud);
    for (i = 0; i < k; i++) strike(s, s->ev[i].vel);         /* a press: a flash now */
    if (s->rain) {                                           /* the rain follows the storm */
        if (held && !s->rain_on) { s->rain->key(s->arena, 60, 100, 1); s->rain_on = 1; }
        if (!held && s->rain_on) { s->rain->key(s->arena, 60, 0, 0); s->rain_on = 0; }
        s->rain->render(s->arena, buf, n);
        {
            tx_stats_t rs;
            s->rain->stats(s->arena, &rs);
            rq = rs.quiet;
        }
    }
    s->on += held ? (32768 - s->on) >> 5 : -(s->on >> 5);    /* ~1 s build / fade */
    if (!held && s->on < 64) s->on = 0;
    if (held) {                                              /* lightning: Poisson, 0.5 .. 8 / min */
        uint32_t per_min_q8 = tx_knob_exp(s->p[P_INTENS], 256u, 234960);   /* 1 .. 12 / min, Q8 */
        if (s->next > n) s->next -= n;
        else {
            if (s->next) strike(s, 64u + (tx_rand(r) >> 26));
            s->next = tx_cloud_exp_q8(&s->cloud, (677376000u / per_min_q8) << 8) >> 8;   /* mean 60 fs / rate */
            if (s->next < 4410u) s->next = 4410u;
        }
    } else
        s->next = 0;
    {   /* the wind: ~u^3, gusting */
        int32_t g;
        uint32_t al;
        s->gust += ((tx_rand_s15(r) * 3) >> 6) - (s->gust >> 7);   /* AR(1), ~0.3 Hz */
        s->gust = s->gust > 13107 ? 13107 : s->gust < -13107 ? -13107 : s->gust;
        s->flut += (tx_rand_s15(r) - s->flut) >> 2;
        al = tx_exp2(3 * (s->u_l2 - 217706 + (int32_t)(((int64_t)s->gust * 32768) >> 15) / 3) + (16 << 16));   /* (u / 10)^3, Q16 */
        g = (int32_t)(((uint64_t)al * WIND_REF) >> 16);
        g = (int32_t)(((int64_t)g * (32768 + (s->flut >> 2))) >> 15);
        g = (int32_t)(((int64_t)g * s->on) >> 15);
        g = (int32_t)(((int64_t)g * s->level) >> 15);
        tx_noise_roar_add(&s->wind, s->on ? g : 0, buf, n);
    }
    if (s->fl.active) {                                      /* thunder over the rain: x 5 (set by render: a flash
                                                              * 2-3 km away stands well over the rain) */
        int32_t *tb = s->tb;
        for (j = 0; j < n; j++) tb[j] = 0;
        tx_flash_add(&s->fl, r, tb, n);
        for (j = 0; j < n; j++) {                            /* x 5, then a soft knee from -6 dBFS: a near crack */
            int32_t v = tb[j] * 5, m = v < 0 ? -v : v;      /* saturates toward full scale, never past it */
            if (m > (1 << 22)) {
                uint32_t x = (uint32_t)(m - (1 << 22)), r;   /* excess over 0.5 FS: x / (1 + x / 0.5 FS) */
                if (x > (1u << 25)) x = 1u << 25;
                r = ((1u << 22) + x) >> 8;                   /* 32-bit: (x 2^6 / r) 2^8 = x 2^22 / (2^22 + x) */
                m = (1 << 22) + (int32_t)(((x << 6) / r) << 8);
                v = v < 0 ? -m : m;
            }
            buf[j] += v;
        }
    }
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    z = s->hp.x1 | s->hp.y1 | s->on | s->wind.gain | s->fl.active | !rq;
    s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
}

static void storm_stats(const void *st, tx_stats_t *o)
{
    const storm_t *s = (const storm_t *)st;
    o->particles = (uint16_t)(s->fl.active ? 1u : 0u);
    o->limit = 1;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: the fiercest storm on a metal roof, a near flash pressed while one rumbles, the rain dense */
static const char SCENE_MAX[] =
    "0 param INTENSITY 127\n0 param SURFACE 127\n0 param AGE 0\n0 param DIST 0\n"
    "0 on 60 127\n500 on 64 127\n1200 on 67 127\n2000 param SURFACE 0\n2500 on 72 127\n"
    "3500 off 60\n3500 off 64\n3500 off 67\n3500 off 72\n"
    "6000 end\n";

const tx_engine_t tx_eng_storm = {
    "storm", "#26 Storm (scene)", NP, PARAMS, 1, 1, sizeof(storm_t), SCENE_MAX,
    storm_init, storm_key, storm_param, storm_render, storm_stats,
};
