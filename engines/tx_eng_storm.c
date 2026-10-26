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
 *            each flash at DIST x 2^(+-1) (nearer as INTENSITY rises), energy and height drawn per flash.
 * The end (the user, session 5: "ends super sharp, not natural"): released, the rain thins out over ~3-4 s (its
 * output faded, tau 0.74 s, its drops let go only at -54 dB) and the wind dies away over ~4-5 s; a thunder tail
 * is never cut: a new flash waits for the one still rolling (a press then strikes at once only after ~4 s).
 * PASSING (0: the storm stays as INTENSITY sets it): a squall passing over, 1 .. 10 min from the press. Ours, after a
 * squall line's time course: a calm, then the gust front (the wind first, sudden), the heavy convective rain a minute
 * behind it with the flashes, the peak, then the trailing rain thinning (its drops smaller: AGE moves toward the
 * stratiform) and the wind easing; light rain and a breeze stay after it. INTENSITY is the peak; each squall draws its
 * own length (x/ 1.25) and stage times (+-0.04 of the length). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_thunder.h"

#define OUT_TRIM 3072               /* output gain, Q12 (-0.5 dB with the spectrogram fixes, session 5; -0.6 dB; -1.4 dB with the thunder's rolling tail, session 5): integrated (a key held 10 s): -25.4 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

extern const tx_engine_t tx_eng_rainwater;
extern const tx_engine_t tx_eng_rainsurf;
void tx_rainsurf_lite(void *st, unsigned on);

enum { P_INTENS, P_SURFACE, P_AGE, P_DIST, P_PASSING, NP };

static const tx_param_t PARAMS[NP] = {
    {"INTENSITY", 127, 64, "storm intensity: rain 0.5 .. 60 mm/h, wind 3 .. 20 m/s, flashes 1 .. 12 / min (nearer)"},
    {"SURFACE", 127, 70, "where the rain falls: water (#2) | leaves, tent, wood, glass, metal roof (#3)"},
    {"AGE", 127, 30, "storm age: young convective cell (big drops, loud) .. stratiform rain after it (fine drops)"},
    {"DIST", 127, 60, "distance of the thunder cell: 0.5 .. 15 km (log)"},
    {"PASSING", 127, 0, "0: a steady storm | a squall passing over from the press: 1 .. 10 min (log; INTENSITY its peak)"},
};

#define ARENA 5920u                /* bytes for the rain engine's state (rain on water 4708, on surfaces 5920 with hail):
                                    * a rain engine that does not fit is silently left out, so txtest checks it */
#define TB 128u                    /* the thunder's scratch, samples (rendered in pieces: the state fits its slot) */
#define RAIN_P 8u                  /* the rain's PARTICLES inside the scene (the scene's CPU share) */
#ifndef SHEET_IN
#define SHEET_IN 24                /* the sheets' drive */
#endif
#ifndef SHEET_AMT
#define SHEET_AMT 16               /* RATE steps at a full sheet (42 a decade) */
#endif
#ifndef SHEET_GUST
#define SHEET_GUST 40              /* RATE steps at a full gust (+-40 %: 13107) */
#endif
#define WIND_REF 14000              /* the wind bed's gain at 10 m/s (Q15; calibrated by render) */

typedef struct {
    uint32_t arena[ARENA / 4u];
    tx_cloud_t cloud;
    tx_noise_t wind;
    tx_flash_t fl;
    tx_dcblock_t hp;
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK], tb[TB];
    const tx_engine_t *rain;
    uint32_t seed, next;           /* samples to the next flash while the storm is on */
    int32_t on, gust, flut, u_l2;
    int32_t sheet;                 /* rain sheets: the rate surging with the gusts, faster (Q15, AR(1) ~0.25 s) */
    int8_t rid;                    /* the rain's RATE (its param id) */
    uint8_t rate0, rtick, rsent;   /* the rain's mean RATE, blocks to its next update, the RATE last sent */
    int32_t rf;                    /* the rain's fade (Q15): 1 while held, falling after */
    uint32_t fl_age;               /* samples since the flash started */
    uint8_t pend;                  /* a pressed flash waiting for the rolling one (its velocity; 0 = none) */
    uint8_t p[NP], quiet, rain_on, dirty, lv;
    int32_t level;
    uint16_t dropped;
    uint32_t sq_t, sq_len;         /* the squall (PASSING): samples since the press, its length in samples */
    int16_t sq_d[4];               /* its stage times' offsets (Q15 of the length) */
    uint8_t sq_held, sq_age, sq_tick, pad;   /* held last block; the AGE last sent; blocks to the next AGE update */
    int32_t sq_r, sq_w;            /* the rain's and the wind's share now (Q15) */
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
        if (e == &tx_eng_rainsurf) tx_rainsurf_lite(s->arena, 1);   /* (CPU: the lite surface under the wind) */
        s->rain_on = 0;
    }
    s->rid = (int8_t)tx_param_find(e, "RATE"); s->rate0 = rate; s->rsent = 0xFF;
    rain_set(s, "RATE", rate);
    if (e == &tx_eng_rainwater) { rain_set(s, "STORM", mix); rain_set(s, "WIND", (uint8_t)(s->p[P_INTENS] / 2u)); }
    else {
        rain_set(s, "DROPS", mix);
        rain_set(s, "SURFACE", (uint8_t)((s->p[P_SURFACE] - 22u) * 127u / 105u));
    }
    e->param(s->arena, TX_PID_LEVEL, (uint8_t)(s->lv * 7u / 10u));   /* the rain -6 dB under the scene's level */
    s->dirty = 0;
    s->sq_age = 0xFF; s->sq_tick = 0;                         /* (a squall's drops: sent again) */
}

/* the squall's stages: x (its time, Q15 of the length) between a and b -> 0 .. 32767 eased (smoothstep) */
static int32_t sq_ease(int32_t x, int32_t a, int32_t b)
{
    uint32_t u, u2;
    if (x <= a) return 0;
    if (x >= b) return 32767;
    u = (uint32_t)(((x - a) << 15) / (b - a));               /* (32-bit: x - a < 2^16) */
    u2 = (u * u) >> 15;
    return (int32_t)((u2 * (98304u - 2u * u)) >> 15);        /* u^2 (3 - 2u) */
}
/* the wind's and the rain's shares at x (Q15 of the length; past 1: the end's): the wind leads, the rain follows */
static void sq_shares(const storm_t *s, int32_t x, int32_t *w, int32_t *r)
{
    int32_t up, dn;
    up = sq_ease(x, 3277 + s->sq_d[0], 9830 + s->sq_d[0]);   /* the gust front: 0.10 .. 0.30 */
    dn = sq_ease(x, 14746 + s->sq_d[1], 32767);              /* easing: 0.45 .. 1 */
    *w = 3932 + (((32767 - 3932) * up) >> 15) - (((32767 - 4915) * dn) >> 15);   /* 0.12, 1, 0.15 after */
    up = sq_ease(x, 5898 + s->sq_d[2], 11796 + s->sq_d[2]);  /* the rain: 0.18 .. 0.36 */
    dn = sq_ease(x, 17039 + s->sq_d[3], 32767);              /* trailing: 0.52 .. 1 */
    *r = 983 + (((32767 - 983) * up) >> 15) - (((32767 - 1311) * dn) >> 15);       /* 0.03, 1, 0.04 after */
}
static void sq_start(storm_t *s)                             /* a squall from now: its length and stages drawn */
{
    tx_rng_t *r = &s->cloud.rng;
    uint32_t i, len_s = tx_exp2((int32_t)((uint32_t)(s->p[P_PASSING] - 1u) * 1725u) + 386897);   /* 60 .. 600 s */
    len_s = (len_s * tx_exp2(((tx_rand_s15(r) * 42) >> 6) + (16 << 16))) >> 16;   /* x/ 1.25 (32-bit: < 2^26) */
    s->sq_len = len_s * 44100u; s->sq_t = 0;
    for (i = 0; i < 4u; i++) s->sq_d[i] = (int16_t)((tx_rand_s15(r) * 1311) >> 15);   /* +-0.04 */
    s->sq_age = 0xFF; s->sq_tick = 0;
}

/* the squall's rain and wind now: its intensities from the shares, the rain's RATE mean (sent with the sheets), its
 * drops moving toward the stratiform after the peak (AGE), each 0.25 s when changed */
static void sq_block(storm_t *s, unsigned n, unsigned held)
{
    int32_t x, w, r, iw, ir;
    if (held && !s->sq_held) sq_start(s);                   /* (a press: a squall from now) */
    s->sq_held = (uint8_t)held;
    if (held && s->sq_t < 2u * s->sq_len) s->sq_t += n;
    x = (int32_t)(s->sq_t / ((s->sq_len >> 15) + 1u));      /* Q15 of the length */
    sq_shares(s, x, &w, &r);
    s->sq_w = w; s->sq_r = r;
    iw = (s->p[P_INTENS] * w) >> 15; ir = (s->p[P_INTENS] * r) >> 15;
    s->u_l2 = 103872 + iw * 1412;
    s->rate0 = (uint8_t)(30 + ((ir * 87 * 258) >> 15));     /* (as rain_setup: x 87 / 127) */
    if (s->rain && !s->sq_tick--) {
        int32_t ae = s->p[P_AGE] + (((127 - s->p[P_AGE]) * sq_ease(x, 14746, 31130)) >> 15);   /* 0.45 .. 0.95 */
        s->sq_tick = 43;
        if ((uint8_t)ae != s->sq_age) {
            uint8_t mix = (uint8_t)(110 - ((ae * 90 * 258) >> 15));
            s->sq_age = (uint8_t)ae;
            if (s->rain == &tx_eng_rainwater) { rain_set(s, "STORM", mix); rain_set(s, "WIND", (uint8_t)(iw >> 1)); }
            else rain_set(s, "DROPS", mix);
        }
    }
}

static void set_param(storm_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    if (id == P_INTENS) s->u_l2 = 103872 + (int32_t)(v * 1412u);   /* log2(3 m/s) .. log2(20 m/s) (was x 1610: 26 m/s at the top) */
    if (id == P_PASSING) { if (v) sq_start(s); s->sq_r = s->sq_w = 32767; }   /* (turned now: a squall from now) */
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
    tx_flash_start(&s->fl, r, tx_exp2(d_l2), h, 256u, el2, lv);
    s->fl_age = 0;
}

TX_AUDIO_CODE static void storm_render(void *st, int32_t *mix, unsigned n)
{
    storm_t *s = (storm_t *)st;
    tx_rng_t *r = &s->cloud.rng;
    unsigned k, i, j, held;
    int32_t *buf = s->buf, z;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    if (s->dirty) rain_setup(s);
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;                              /* silent and nothing new */
    for (j = 0; j < n; j++) buf[j] = 0;
    held = tx_cloud_held(&s->cloud);
    for (i = 0; i < k; i++) {                                /* a press: a flash now, unless one still rolls */
        if (s->fl.active && s->fl_age < 176400u) { s->pend = s->ev[i].vel; s->dropped++; }
        else strike(s, s->ev[i].vel);
    }
    if (s->pend && !s->fl.active) { strike(s, s->pend); s->pend = 0; }
    if (s->p[P_PASSING]) sq_block(s, n, held);
    if (s->rain) {                                           /* the rain follows the storm, thinning out after it */
        int32_t r0 = s->rf, dr;
        if (held) s->rf += (32767 - s->rf + 7) >> 3;            /* (back in ~50 ms) */
        else s->rf -= (s->rf >> 7) + 1;                      /* tau ~0.74 s */
        if (s->rf < 64) s->rf = 0;
        if (held && !s->rain_on) { s->rain->key(s->arena, 60, 100, 1); s->rain_on = 1; }
        if (!held && s->rain_on && !s->rf) { s->rain->key(s->arena, 60, 0, 0); s->rain_on = 0; }
        s->rain->render(s->arena, buf, n);
        dr = (s->rf - r0) / (int32_t)n;
        if (r0 != 32767 || s->rf != 32767)
            for (j = 0; j < n; j++) { r0 += dr; buf[j] = tx_trim1(buf[j], r0 >> 3); }   /* Q12 */
    }
    s->on += held ? (32768 - s->on) >> 5 : -((s->on >> 7) + 1);   /* ~1 s build, ~4-5 s fade */
    if (!held && s->on < 64) s->on = 0;
    if (s->fl.active) s->fl_age += n;
    if (held) {                                              /* lightning: Poisson, 0.5 .. 8 / min */
        uint32_t per_min_q8 = tx_knob_exp(s->p[P_PASSING] ? (uint32_t)((s->p[P_INTENS] * s->sq_r) >> 15) : s->p[P_INTENS],
                                          256u, 234960);    /* 1 .. 12 / min, Q8 (a squall: with its rain) */
        if (s->next > n) s->next -= n;
        else {
            if (s->next && !s->fl.active) strike(s, 64u + (tx_rand(r) >> 26));   /* (not over a rolling one) */
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
    if (s->rain && s->rid >= 0) {                            /* rain sheets: in a gust the rain comes in harder, in
                                                              * waves of a second or so (ours: the references' 0.5-4 Hz) */
        int32_t v;
        s->sheet += ((tx_rand_s15(r) * SHEET_IN) >> 6) - (s->sheet >> 5);
        s->sheet = s->sheet > 32767 ? 32767 : s->sheet < -32767 ? -32767 : s->sheet;
        if (!s->rtick--) {
            s->rtick = 1;
            v = (int32_t)s->rate0 + ((s->gust * SHEET_GUST) >> 15) + ((s->sheet * SHEET_AMT) >> 15);
            v = v < 0 ? 0 : v > 127 ? 127 : v;
            if ((uint8_t)v != s->rsent) { s->rsent = (uint8_t)v; s->rain->param(s->arena, (uint8_t)s->rid, (uint8_t)v); }
        }
    }
    if (s->fl.active) {                                      /* thunder over the rain: x 5 (set by render: a flash
                                                              * 2-3 km away stands well over the rain) */
        int32_t *tb = s->tb;
        unsigned o, m;
        for (o = 0; o < n; o += m) {
            m = n - o < TB ? n - o : TB;
            for (j = 0; j < m; j++) tb[j] = 0;
            tx_flash_add(&s->fl, r, tb, m);
            for (j = 0; j < m; j++) {                        /* x 5, then a soft knee from -6 dBFS: a near crack */
                int32_t v = tb[j] * 5, a = v < 0 ? -v : v;  /* saturates toward full scale, never past it */
                if (a > (1 << 22)) {
                    uint32_t x = (uint32_t)(a - (1 << 22)), q;   /* excess over 0.5 FS: x / (1 + x / 0.5 FS) */
                    if (x > (1u << 25)) x = 1u << 25;
                    q = ((1u << 22) + x) >> 8;               /* 32-bit: (x 2^6 / q) 2^8 = x 2^22 / (2^22 + x) */
                    a = (1 << 22) + (int32_t)(((x << 6) / q) << 8);
                    v = v < 0 ? -a : a;
                }
                buf[o + j] += v;
            }
            if (!s->fl.gen_done) break;                      /* (the channel grows TX_TH_GEN steps a block, not a piece) */
        }
    }
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    z = s->hp.x1 | s->hp.y1 | s->on | s->wind.gain | s->fl.active | s->rf | s->pend;   /* (the rain: silent once faded) */
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

/* the hosted rain engines that do not fit the arena (txtest: a rain left out would be silent, not an error); the
 * first one's name in *bad */
unsigned tx_storm_misfits(const char **bad)
{
    const tx_engine_t *const r[2] = { &tx_eng_rainwater, &tx_eng_rainsurf };
    unsigned i, n = 0;
    for (i = 0; i < 2u; i++) if (r[i]->state_bytes > ARENA) { if (!n) *bad = r[i]->name; n++; }
    return n;
}

const tx_engine_t tx_eng_storm = {
    "storm", "#26 Storm (scene)", NP, PARAMS, 1, 1, sizeof(storm_t), SCENE_MAX,
    storm_init, storm_key, storm_param, storm_render, storm_stats,
};
