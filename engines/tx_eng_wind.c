/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura Wind: turbulent air outdoors (WIND core, tx_core_wind.h): gusts and lulls on several time scales, the rush
 * over the ground and through the canopy (brighter and wider as it blows harder), leaves and grass rustling,
 * buffeting at the listener, gusts whooshing past, and now and then a faint edge tone. No steady whistle: the singing
 * wires are the aeolian engine.
 *
 * Playing: a held key keeps the wind blowing (it rises over ~0.4 s and, released, dies over ~4 s); a press is a gust
 * (velocity: how strong) with a whoosh passing; more keys held (up to 4), more gust sources and more canopy around
 * (more whooshes and rustle). WIND 0 is a calm: silence. KIND picks the place: each its own spectrum and motion. */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_cloud.h"
#include "tx_core_wind.h"

#define OUT_TRIM 3250               /* output gain, Q12: a key held at the defaults is -26 LUFS */

enum { P_WIND, P_GUST, P_FOLIAGE, P_BUFFET, P_WHOOSH, P_EDGE, P_SCALE, P_KIND, P_DRIFT, P_SKITTER, NP };

static const tx_param_t PARAMS[NP] = {
    {"WIND", 127, 64, "mean wind speed: 0 calm (silence) .. 28 m/s (^1.5: 0.5 m/s at 9, 10 at 64: a breath now and then .. a storm)"},
    {"GUST", 127, 64, "gustiness: turbulence 5 .. 40 %, gusts and lulls 0.04 .. 0.34 /s (more in light air)"},
    {"FOLIAGE", 127, 64, "what it blows through: open ground and grass .. leafy trees (rustle)"},
    {"BUFFET", 127, 48, "exposure: sheltered .. in the open (low pressure bursts at the listener)"},
    {"WHOOSH", 127, 64, "gusts passing by: none .. often (a band sweeping down as it passes)"},
    {"EDGE", 127, 16, "edge tones: none .. now and then (faint, short, in strong gusts only)"},
    {"SCALE", 127, 64, "the obstacles' size: big trees (lower) .. fine grass (higher), +-1 octave"},
    {"KIND", 127, 0, "the place, 13 steps each: wind | a breeze in tall grass | pines | leafy trees | an open plain or moor | "
                     "a sea wind on the coast | a gale on a ridge | a gusty autumn day | a winter storm | a desert wind"},
    {"DRIFT", 127, 64, "what the wind lifts, where it does (a winter storm's snow, a desert's sand): none .. +-3 octaves of the place's own "
                      "(a fine hiss rising with the gusts over its threshold speed)"},
    {"SKITTER", 127, 64, "dry leaves tumbling over the ground, where there are (a gusty autumn day): none, then x 1/4 .. "
                        "x 4 the place's own (runs of clicks over 4 m/s, louder as it blows harder)"},
};

/* the kinds (a place each: what the wind blows through and how it moves there; ours, set against the references):
 * the speed's top (x, Q8), the turbulence (x, Q8), the level (Q12: -26 LUFS at the defaults; grass and pines a little
 * under, the storms a little over), then the core's paths (tx_wkind_t; r2 0 = the rush's top at 6 dB / octave). The
 * gusts, the turbulence and the flutter are set against the references at the defaults: most places blow steadier
 * than a gale, their bands not all swelling together (the references' 100 ms level spans 3 .. 10 dB; the ridge's and
 * the desert's 20) */
#define O(t) ((t) * 6554)                 /* tenths of an octave -> Q16 */
typedef struct { int16_t us, tim, trim; tx_wkind_t k; uint8_t lpc; } kind_t;   /* (the offsets need int32: see tx_wkind_t;
                                                              * lpc: a low-pass on the whole, 1 - e^(-2 pi fc / fs) Q8, 0: none) */
#define NK 10u
static const kind_t KINDS[NK] = {
    /*            gsz  grt   kr lull |  rg       ro      rbw |  lg       lr lsh  lhp       lo lut   lp |  bg |  wg   wr wq0 wq1       wo   wt |  em   fl |    r2 | sk sut sr */
    /* wind: ground and trees, the all-round one */
    {256, 153, 5530, {153, 64, 256, 3, 256, O(0), O(20), 256, O(0), 7, 400, O(0), 0, 512, 256, 256, 256, 19, 40, O(0), 256, 256, 128, 0, 0, 0, 0, 0}},
    /* a breeze in tall grass: blades swishing (fewer, longer flaps, darker), waves of wind across the field (long, broad
     * whooshes, higher), little at the ground */
    {180, 64, 26433, {75, 45, 200, 3, 140, O(-5), O(20), 900, O(-10), 8, 500, O(-12), 0, 512, 90, 200, 400, 12, 19, O(10), 420, 0, 0, O(10), 0, 0, 0, 0}},
    /* pines: the needles' smooth hiss (dense, tiny flaps, high), soughing in slow swells, sheltered under the canopy:
     * the needles take the highs (the references fall 11 dB from 1 to 4 kHz; ours rose 2 dB): a 1 kHz low-pass */
    {230, 120, 29700, {132, 50, 150, 2, 110, O(-4), O(20), 520, O(30), 5, 1500, O(-5), 0, 512, 50, 140, 150, 16, 24, O(15), 420, 100, 75, O(10), 0, 0, 0, 0}, 34},
    /* leafy trees: leaves rattling and flapping (bigger, mid-bright), gusts sweeping through the canopy */
    {230, 96, 9500, {96, 64, 256, 3, 170, O(0), O(20), 460, O(0), 8, 600, O(2), 0, 512, 110, 300, 300, 16, 30, O(7), 300, 80, 0, 0, 0, 0, 0, 0}},
    /* an open plain or moor: no trees, the low broad roar of the air over open ground, buffeting, gusts moaning past */
    {256, 153, 6195, {153, 128, 256, 3, 300, O(-4), O(30), 70, O(-10), 6, 800, O(-5), 0, 512, 430, 215, 200, 30, 50, O(-10), 300, 150, 0, O(10), 0, 0, 0, 0}},
    /* a sea wind on the coast: strong and steady (little turbulence, long slow swells), a broad bright roar, spray hiss */
    {280, 76, 6803, {72, 37, 150, 1, 300, O(3), O(35), 110, O(40), 4, 2000, O(5), 0, 512, 120, 200, 60, 16, 24, O(5), 500, 20, 50, O(10), 0, 0, 0, 0}},
    /* a gale on a ridge: violent sudden gusts and deep lulls, a deep roar over the rock, howling gusts, heavy buffeting
     * (gusts twice as often and rising twice as fast: the reference's 1-6 Hz modulation; a brighter roar) */
    {330, 150, 1763, {200, 600, 1000, 9, 500, O(-2), O(25), 40, O(0), 6, 1000, O(0), 0, 512, 1500, 400, 300, 48, 96, O(-5), 300, 300, 1000, O(10), 0, 0, 0, 0}},
    /* a gusty autumn day: calm, then a gust and dry leaves crackling (sparse, big, bright, ~u^3) and skittering over the
     * ground above 4 m/s, in runs */
    {230, 228, 6582, {270, 80, 350, 6, 100, O(0), O(20), 400, O(-40), 9, 1200, O(6), 0, 768, 250, 250, 250, 19, 40, O(8), 256, 80, 150, O(10), 128, 64, 1024, 0}},
    /* a winter storm: bare branches, a low roar (the references: 400-500 Hz centroid), blowing snow a faint hiss over
     * 7 m/s (~u^3), howling gusts, buffeting */
    {320, 82, 7380, {112, 75, 400, 2, 260, O(-12), O(20), 100, O(20), 6, 3000, O(10), 112, 768, 320, 160, 350, 40, 80, O(-18), 300, 250, 0, O(5), 0, 0, 0, 0}},
    /* a desert wind: sand lifting over 5 m/s, its grains ticking and hissing (~u^3, bright), no plants */
    {256, 250, 4775, {300, 256, 256, 1, 120, O(0), O(20), 1600, O(30), 4, 4000, O(30), 80, 768, 100, 200, 100, 19, 30, O(5), 256, 60, 100, O(10), 0, 0, 0, 0}},
};

typedef struct {
    tx_wind_t w;
    tx_dcblock_t hp;
    int32_t buf[TX_BLOCK];
    uint8_t p[NP], held, quiet, kind, pad;
    uint16_t dropped;
    int32_t lz;                    /* the canopy's low-pass */
} wind_t;

static void apply(wind_t *s)
{
    tx_wind_t *w = &s->w;
    int32_t v = s->p[P_WIND], g = s->p[P_GUST];
    const kind_t *k = &KINDS[(s->p[P_KIND] * NK) >> 7];
    w->k = k->k;
    if (k->k.lut && s->p[P_DRIFT] != 64u)                   /* DRIFT: the lifted snow or sand, x 2^(+-3) (0: none) */
        w->k.lg = s->p[P_DRIFT] ? (int16_t)(((int32_t)k->k.lg * (int32_t)tx_exp2(((int32_t)s->p[P_DRIFT] - 64) * 3072 + (12 << 16))) >> 12) : 0;
    if (k->k.sk && s->p[P_SKITTER] != 64u)                  /* SKITTER: the leaves on the ground, x 2^(+-2) (0: none) */
        w->k.sk = s->p[P_SKITTER] ? (int16_t)(((int32_t)k->k.sk * (int32_t)tx_exp2(((int32_t)s->p[P_SKITTER] - 64) * 2048 + (12 << 16))) >> 12) : 0;
    s->kind = (uint8_t)((s->p[P_KIND] * NK) >> 7);
    if (!v) w->U = 0;
    else {   /* 28 (v / 127)^1.5 m/s: 0.5 m/s at 9 (calm air, a breath now and then), 10 at 64, 28 at 127 */
        int32_t l = (32 << 16) - (int32_t)(tx_neglog2_q12((uint32_t)v) << 4) - 458007;   /* log2(v / 127) */
        uint32_t p = tx_exp2((l * 3) / 2 + (16 << 16));
        w->U = (int32_t)(((uint64_t)114688u * p * (uint32_t)k->us) >> 24);
    }
    w->ti = ((1638 + (g * g * 711) / 1000) * k->tim) >> 8;  /* 0.05 + 0.35 (GUST / 127)^2: 0.05 .. 0.4 (x kind) */
    w->grate = ((((15 + g * 115 / 127) * (19661 + v * 206)) >> 15) * w->k.grt) >> 8;   /* (0.04 .. 0.34 /s) (0.6 .. 1.4): /65536 per block */
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
    tx_zero(b, sizeof *s);
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
        s->w.nsrc = (uint8_t)(s->held > 4u ? 4u : s->held);
        tx_wind_press(&s->w, vel);
        s->quiet = 0;
    } else if (s->held) {
        s->held--;
        if (!s->held) s->w.held = 0;
        else s->w.nsrc = (uint8_t)(s->held > 4u ? 4u : s->held);
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
    tx_trim(s->buf, n, (OUT_TRIM * KINDS[s->kind].trim) >> 12);
    if (KINDS[s->kind].lpc) {                                /* under a canopy: the needles scatter the highs */
        int32_t y = s->lz, c = KINDS[s->kind].lpc;
        for (j = 0; j < n; j++) { y += ((s->buf[j] - y) >> 8) * c; s->buf[j] = y; }
        s->lz = y;
    }
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
