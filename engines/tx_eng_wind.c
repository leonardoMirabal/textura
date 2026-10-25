/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura Wind: turbulent air outdoors (WIND core, tx_core_wind.h): gusts and lulls on several time scales, the rush
 * over the ground and through the canopy (brighter and wider as it blows harder), leaves and grass rustling,
 * buffeting at the listener, gusts whooshing past, and now and then a faint edge tone. No steady whistle: the singing
 * wires are the aeolian engine.
 *
 * Playing: a held key keeps the wind blowing (it rises over ~0.4 s and, released, dies over ~4 s); a press is a gust
 * (velocity: how strong) with a whoosh passing. WIND 0 is a calm: silence. */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_cloud.h"
#include "tx_core_wind.h"

#define OUT_TRIM 3250               /* output gain, Q12: a key held at the defaults is -26 LUFS */

enum { P_WIND, P_GUST, P_FOLIAGE, P_BUFFET, P_WHOOSH, P_EDGE, P_SCALE, NP };

static const tx_param_t PARAMS[NP] = {
    {"WIND", 127, 64, "mean wind speed: 0 calm (silence) .. 28 m/s (^1.5: 0.5 m/s at 9, 10 at 64: a breath now and then .. a storm)"},
    {"GUST", 127, 64, "gustiness: turbulence 5 .. 40 %, gusts and lulls 0.04 .. 0.34 /s (more in light air)"},
    {"FOLIAGE", 127, 64, "what it blows through: open ground and grass .. leafy trees (rustle)"},
    {"BUFFET", 127, 48, "exposure: sheltered .. in the open (low pressure bursts at the listener)"},
    {"WHOOSH", 127, 64, "gusts passing by: none .. often (a band sweeping down as it passes)"},
    {"EDGE", 127, 16, "edge tones: none .. now and then (faint, short, in strong gusts only)"},
    {"SCALE", 127, 64, "the obstacles' size: big trees (lower) .. fine grass (higher), +-1 octave"},
};

typedef struct {
    tx_wind_t w;
    tx_dcblock_t hp;
    int32_t buf[TX_BLOCK];
    uint8_t p[NP], held, quiet;
    uint16_t dropped;
} wind_t;

static void apply(wind_t *s)
{
    tx_wind_t *w = &s->w;
    int32_t v = s->p[P_WIND], g = s->p[P_GUST];
    if (!v) w->U = 0;
    else {   /* 28 (v / 127)^1.5 m/s: 0.5 m/s at 9 (calm air, a breath now and then), 10 at 64, 28 at 127 */
        int32_t l = (32 << 16) - (int32_t)(tx_neglog2_q12((uint32_t)v) << 4) - 458007;   /* log2(v / 127) */
        uint32_t p = tx_exp2((l * 3) / 2 + (16 << 16));
        w->U = (int32_t)(((uint64_t)114688u * p) >> 16);
    }
    w->ti = 1638 + (g * g * 711) / 1000;                     /* 0.05 + 0.35 (GUST / 127)^2: 0.05 .. 0.4 */
    w->grate = ((15 + g * 115 / 127) * (19661 + v * 206)) >> 15;   /* (0.04 .. 0.34 /s) (0.6 .. 1.4): /65536 per block */
    w->gusty = g;
    w->foliage = s->p[P_FOLIAGE];
    w->buffet = s->p[P_BUFFET];
    w->whoosh = s->p[P_WHOOSH];
    w->edge = s->p[P_EDGE];
    w->scale = ((int32_t)s->p[P_SCALE] - 64) * 1024;          /* +-1 octave */
}

static void wind_init(void *st, uint32_t seed)
{
    wind_t *s = (wind_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_wind_init(&s->w, seed);
    for (i = 0; i < NP; i++) s->p[i] = PARAMS[i].def;
    apply(s);
    s->quiet = 1;
}

static void wind_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    wind_t *s = (wind_t *)st;
    (void)note;
    if (on && vel) {
        if (s->held < 255u) s->held++;
        s->w.held = 1;
        tx_wind_press(&s->w, vel);
        s->quiet = 0;
    } else if (s->held) {
        s->held--;
        if (!s->held) s->w.held = 0;
    }
}

static void wind_param(void *st, uint8_t id, uint8_t val)
{
    wind_t *s = (wind_t *)st;
    if (id == TX_PID_PARTICLES) s->w.limit = (uint8_t)(val < 1u ? 1u : val > TX_WIND_NW ? TX_WIND_NW : val);
    else if (id == TX_PID_LEVEL) s->w.level = tx_level_q15(val > 127u ? 127u : val);
    else if (id < NP) { s->p[id] = (uint8_t)(val > PARAMS[id].max ? PARAMS[id].max : val); apply(s); }
}

TX_AUDIO_CODE static void wind_render(void *st, int32_t *mix, unsigned n)
{
    wind_t *s = (wind_t *)st;
    unsigned j;
    if (n > TX_BLOCK) n = TX_BLOCK;
    if (s->quiet) return;
    for (j = 0; j < n; j++) s->buf[j] = 0;
    tx_wind_block(&s->w, s->buf, n);
    tx_trim(s->buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, s->buf, mix, n);
    s->quiet = (uint8_t)(!s->held && tx_wind_quiet(&s->w) && !(s->hp.x1 | s->hp.y1));
}

static void wind_stats(const void *st, tx_stats_t *o)
{
    const wind_t *s = (const wind_t *)st;
    o->particles = s->w.nwh;
    o->limit = s->w.limit;
    o->events = s->w.events;
    o->dropped = 0;
    o->steals = 0;
    o->total = s->w.total;
    o->quiet = s->quiet;
}

/* the worst case: a storm, gusty, leafy, exposed, every whoosh and the edge tones */
static const char SCENE_MAX[] =
    "0 particles 4\n0 param WIND 127\n0 param GUST 127\n0 param FOLIAGE 127\n0 param BUFFET 127\n0 param WHOOSH 127\n"
    "0 param EDGE 127\n0 on 60 127\n500 on 64 127\n1000 on 67 127\n1500 on 72 127\n"
    "3000 off 60\n3000 off 64\n3000 off 67\n3000 off 72\n4000 end\n";

const tx_engine_t tx_eng_wind = {
    "wind", "Wind", NP, PARAMS, TX_WIND_NW, 3, sizeof(wind_t), SCENE_MAX,
    wind_init, wind_key, wind_param, wind_render, wind_stats,
};
