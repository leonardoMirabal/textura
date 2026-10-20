/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #30 Bees / flies: insects flying around the listener (WINGBEAT core). No mosquitoes.
 *
 * A key press lets an insect (and SWARM companions) fly; holding keeps it around; release lets it fly off (and fade
 * over ~1.5 s, ours). Physics:
 *   buzz     wingbeat f0 and harmonic levels per species (honeybee 230 Hz, H2 -2.5 / H3 -8 / H4 -19.5 dB: N13 / N29;
 *            hornet 107 Hz, H2 +0.5 .. H7 -23 dB: N29; bumblebee ~175 Hz, H2 -5 / H3 -10: N17 / N26; blowfly ~180 Hz,
 *            H2 -8 .. -12: N27; hoverfly 150 Hz and carpenter bee 118 Hz: N11 / N16 (their harmonics: ours)).
 *            Keys: C4 = the species' f0, 2^(1/24) per semitone (a "pitched bee" without reaching mosquito pitch).
 *   angle    H1 loudest in front, H2 at the sides (N22, N27): the side table has H1 -10 dB, H2 +2 dB (ours: N22's dB
 *            are paywalled); the mix follows the angle between the insect's heading and the listener.
 *   flight   each insect flies a 2-D path at SPEED around a home distance (a spring to DIST plus a random wander,
 *            ours): level ~ 1 / r, pitch x c / (c + v_r) (Doppler: +-0.3-1.5 % at 1-5 m/s). Manoeuvres change the
 *            stroke amplitude, not f0 (N13): an amplitude wander +-3 dB, f0 wander ~1 %.
 *   swarm    companions with f0 spread SPREAD (CV ~5 % across a swarm, note 08). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_wingbeat.h"

#define OUT_TRIM 8859               /* output gain, Q12 (+4.1 dB): integrated (a key held 10 s): -30.1 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +2.6 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 60
#define VARY_SPLIT 4

enum { P_SPECIES, P_SWARM, P_SPREAD, P_SPEED, P_DIST, NP };

static const tx_param_t PARAMS[NP] = {
    {"SPECIES", 127, 0, "honeybee 230 Hz | blowfly 180 | bumblebee 175 | hoverfly 150 | carpenter bee 118 | hornet 107"},
    {"SWARM", 127, 0, "companions per key: 0 .. 7 (a swarm)"},
    {"SPREAD", 127, 64, "wingbeat spread across the swarm: CV 0 .. 10 % (5 % typical)"},
    {"SPEED", 127, 64, "flight speed: 0.2 .. 5 m/s (log): wander, Doppler +-0.06 .. 1.5 %"},
    {"DIST", 127, 50, "hovering distance: 0.1 .. 5 m (log): level ~ 1 / r (velocity: closer / farther)"},
};

#define NV 16u
#define NSP 6u
#define C_MM 343000                /* speed of sound, mm/s */
#define R_REF 300                  /* the level reference distance, mm */
#define G_REF 4000                 /* gain at R_REF (Q15; calibrated by render) */
#define R_MIN 60                   /* the level law stops at 6 cm (the insect's own size) */
#define FADE_BLK 430u              /* fly-off fade, blocks (2.5 s; the level as (1 - t / T)^3) */

typedef struct { uint16_t f0; int8_t h[TX_WING_H]; } species_t;
static const species_t SPECIES[NSP] = {           /* harmonic levels in dB x 2 (-128: absent) */
    {230, {0, -5, -16, -39, -128, -128, -128, -128}},   /* honeybee (N29) */
    {180, {0, -20, -36, -50, -60, -128, -128, -128}},   /* blowfly (N27: H2 -8 .. -12; higher ours) */
    {175, {0, -10, -20, -30, -40, -128, -128, -128}},   /* bumblebee (N26 B. sylvarum; H4, H5 ours) */
    {150, {0, -16, -30, -44, -128, -128, -128, -128}},  /* hoverfly (ours) */
    {118, {0, -8, -20, -32, -44, -128, -128, -128}},    /* carpenter bee (ours) */
    {107, {0, 1, -16, -30, -42, -48, -46, -128}},       /* hornet V. simillima (N29) */
};

typedef struct {
    tx_buzz_t bz;
    int32_t px, py;                /* position, mm (the listener at 0, 0) */
    int32_t vx, vy;                /* velocity, mm/s */
    uint32_t inc0;                 /* own f0 as a phase increment (before Doppler) */
    int32_t speed, home;           /* mm/s, mm */
    int32_t wob;                   /* amplitude wander, Q15 around 1 */
    uint16_t fade;                 /* blocks into the fly-off (0 = still around) */
    uint8_t em, note, alive, pad;
} bug_t;

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    bug_t b[NV];
    int16_t front[TX_WING_N + 1], side[TX_WING_N + 1];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK];
    uint8_t p[NP], limit, quiet;
    int32_t level, speed_l2, dist_l2, cv;
    uint32_t f0;
    uint16_t dropped;
    tx_vary_t vy;
} bees_t;

static uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) { if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return r;
}
static int32_t gauss_q12(tx_rng_t *r)
{
    uint32_t a = tx_rand(r), b = tx_rand(r);
    int32_t s = (int32_t)((a >> 16) + (a & 0xFFFFu) + (b >> 16) + (b & 0xFFFFu)) - 131072;
    return (s * 7094) >> 15;
}

static void set_species(bees_t *s)
{
    const species_t *sp = &SPECIES[(s->p[P_SPECIES] * NSP) >> 7];
    int8_t side[TX_WING_H];
    unsigned k;
    for (k = 0; k < TX_WING_H; k++) side[k] = sp->h[k];
    side[0] = (int8_t)(side[0] - 20);                        /* sides: H1 -10 dB, H2 +2 dB (ours) */
    if (side[1] > -120) side[1] = (int8_t)(side[1] + 4);
    tx_wing_table(s->front, sp->h, TX_WING_H);
    tx_wing_table(s->side, side, TX_WING_H);
    s->f0 = sp->f0;
}

static void set_param(bees_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_SPECIES: set_species(s); break;
    case P_SPREAD: s->cv = (int32_t)v * 26; break;                         /* Q15: 0 .. 0.1 */
    case P_SPEED: s->speed_l2 = 500953 + (int32_t)(v * 2397u); break;      /* log2(200 mm/s) + v log2(25) / 127 */
    case P_DIST: s->dist_l2 = 435412 + (int32_t)(v * 2914u); break;        /* log2(100 mm) + v log2(50) / 127 */
    default: break;
    }
}

static void bees_init(void *st, uint32_t seed)
{
    bees_t *s = (bees_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    for (i = 0; i < NV; i++) s->b[i].em = 0xFF;
    s->limit = 8;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((bees_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void bees_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((bees_t *)st)->vy);
    tx_cloud_key(&((bees_t *)st)->cloud, note, vel, on);
}

static void bees_param(void *st, uint8_t id, uint8_t val)
{
    bees_t *s = (bees_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NV ? NV : val);
        for (i = s->limit; i < NV; i++) { s->b[i].alive = 0; s->b[i].em = 0xFF; }   /* its gain ramps to 0 in the block */
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* a key: one insect + SWARM companions, each from a free slot (else the farthest-gone fly-off makes way) */
static void launch(bees_t *s, const tx_event_t *e)
{
    unsigned n = 1u + (s->p[P_SWARM] >> 4), c, i;
    tx_rng_t *r = &s->cloud.rng;
    for (c = 0; c < n; c++) {
        bug_t *b = 0;
        int32_t home, ang, f_l2;
        for (i = 0; i < s->limit && !b; i++) if (!s->b[i].alive) b = &s->b[i];
        if (!b) {
            uint16_t most = 0;
            for (i = 0; i < s->limit; i++) if (s->b[i].fade >= most && s->b[i].fade) { most = s->b[i].fade; b = &s->b[i]; }
            if (!b) { s->dropped++; return; }
        }
        home = (int32_t)tx_exp2(s->dist_l2 - ((int32_t)e->vel - 100) * 1638);   /* velocity: closer */
        ang = (int32_t)tx_rand(r);
        b->px = (tx_sin_q15((uint32_t)ang + 0x40000000u) * home) >> 15;
        b->py = (tx_sin_q15((uint32_t)ang) * home) >> 15;
        b->speed = (int32_t)tx_exp2(s->speed_l2) * (28672 + (int32_t)(tx_rand(r) >> 19)) >> 15;   /* x 0.875 .. 1.125 */
        b->vx = (tx_sin_q15((uint32_t)ang) * b->speed) >> 15;     /* off tangentially */
        b->vy = -(tx_sin_q15((uint32_t)ang + 0x40000000u) * b->speed) >> 15;
        b->home = home;
        f_l2 = ((int32_t)e->note - 60) * 2731 + (tx_vary_tone(&s->vy) >> 3) + ((gauss_q12(r) * s->cv * 3) >> 12);   /* CV -> log2: x 1.44, Q16 */
        b->inc0 = (uint32_t)(((uint64_t)s->f0 * 97391u * tx_exp2(f_l2 + (16 << 16))) >> 16);
        b->wob = 32768;
        b->fade = 0;
        b->alive = 1; b->em = e->emitter; b->note = e->note;
        if (!b->bz.g) b->bz.ph = tx_rand(r);
    }
}

/* per block: steer (spring to home + wander, or fly off), move, Doppler, angle, level */
static void fly(bees_t *s, bug_t *b, int32_t *g1, int32_t *mix1)
{
    tx_rng_t *r = &s->cloud.rng;
    int held = b->em < TX_CLOUD_EMITTERS && s->cloud.em[b->em].held && s->cloud.em[b->em].note == b->note;
    int32_t rr, ax, ay, sp, vr, ratio, cross, gm;
    uint32_t v2;
    rr = (int32_t)isqrt32((uint32_t)((b->px >> 4) * (b->px >> 4) + (b->py >> 4) * (b->py >> 4))) * 16;
    if (rr < 30) rr = 30;
    if (held && !b->fade) {                                  /* spring toward the home circle + random wander */
        int32_t k = ((rr - b->home) * 256) / (b->home > 0 ? b->home : 1);   /* (r - R) / R, Q8 */
        k = k > 2048 ? 2048 : k < -2048 ? -2048 : k;
        ax = -(((b->px / (rr >> 4 ? rr >> 4 : 1)) * k * b->speed) >> 9);   /* ~ 8 x speed x (r - R) / R, mm/s^2 */
        ay = -(((b->py / (rr >> 4 ? rr >> 4 : 1)) * k * b->speed) >> 9);
    } else {                                                 /* fly off: away from the listener */
        ax = (b->px / (rr >> 4 ? rr >> 4 : 1)) * (b->speed >> 2);
        ay = (b->py / (rr >> 4 ? rr >> 4 : 1)) * (b->speed >> 2);
        if (!b->fade) b->fade = 1;
    }
    ax += (gauss_q12(r) * b->speed) >> 10;                   /* wander (~ 4 x speed per s, ours) */
    ay += (gauss_q12(r) * b->speed) >> 10;
    b->vx += (ax * 380) >> 16;                               /* dt = 5.8 ms: x 380 / 65536 */
    b->vy += (ay * 380) >> 16;
    v2 = (uint32_t)((b->vx >> 4) * (b->vx >> 4) + (b->vy >> 4) * (b->vy >> 4));
    sp = (int32_t)isqrt32(v2) * 16;                          /* hold the flight speed */
    if (sp < 16) sp = 16;
    b->vx = b->vx * (b->speed >> 4) / (sp >> 4 ? sp >> 4 : 1);
    b->vy = b->vy * (b->speed >> 4) / (sp >> 4 ? sp >> 4 : 1);
    b->px += (b->vx * 380) >> 16;
    b->py += (b->vy * 380) >> 16;
    /* Doppler: v_r = p . v / r (mm/s), f' = f c / (c + v_r) */
    vr = (((b->px >> 4) * (b->vx >> 4) + (b->py >> 4) * (b->vy >> 4)) * 16) / (rr >> 4 ? rr >> 4 : 1);   /* p . v / r */
    ratio = (int32_t)((C_MM * 4096u) / (uint32_t)(C_MM + vr));        /* Q12 */
    b->bz.inc = (b->inc0 >> 12) * (uint32_t)ratio;
    /* angle: |sin| between heading and the line of sight -> side mix */
    cross = (((b->px >> 4) * (b->vy >> 4) - (b->py >> 4) * (b->vx >> 4)) * 16) / (rr >> 4 ? rr >> 4 : 1);   /* |v| sin */
    cross = cross < 0 ? -cross : cross;
    *mix1 = (cross > b->speed ? b->speed : cross) * 32767 / (b->speed > 0 ? b->speed : 1);
    if (*mix1 > 32767) *mix1 = 32767;
    /* level: 1 / r, the amplitude wander, the fly-off fade */
    b->wob += ((32768 + ((gauss_q12(r) * 11) >> 1)) - b->wob) >> 4;   /* +-3 dB-ish slow wander */
    gm = (int32_t)(((int64_t)(G_REF * R_REF / (rr > R_MIN ? rr : R_MIN)) * b->wob) >> 15);
    gm = (int32_t)(((int64_t)gm * s->level) >> 15);
    gm = gm > 32767 ? 32767 : gm;
    if (b->fade) {
        int32_t q = (int32_t)((FADE_BLK - (b->fade < FADE_BLK ? b->fade : FADE_BLK)) * 32767u / FADE_BLK);   /* 1 .. 0 */
        q = (int32_t)(((int64_t)q * q >> 15) * q >> 15);     /* cubed: it fades away, no audible end (session 5) */
        gm = (int32_t)(((int64_t)gm * q) >> 15);
        if (b->fade < 65535u) b->fade++;
    }
    *g1 = gm > 32767 ? 32767 : gm;
}

TX_AUDIO_CODE static void bees_render(void *st, int32_t *mix, unsigned n)
{
    bees_t *s = (bees_t *)st;
    unsigned k, i, j;
    int32_t *buf = s->buf;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) buf[j] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    for (i = 0; i < k; i++) launch(s, &s->ev[i]);
    for (i = 0; i < NV; i++) {
        bug_t *b = &s->b[i];
        int32_t g1 = 0, m1 = b->bz.mix;
        if (b->alive) fly(s, b, &g1, &m1);
        if (!b->alive && !b->bz.g) continue;
        tx_buzz_add(&b->bz, s->front, s->side, g1, m1, buf, n);
        if (b->alive && b->fade >= FADE_BLK && !b->bz.g) { b->alive = 0; b->em = 0xFF; }
    }
    tx_vary_block(&s->vy, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = s->hp.x1 | s->hp.y1;
        for (i = 0; i < NV; i++) z |= s->b[i].alive | s->b[i].bz.g;
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void bees_stats(const void *st, tx_stats_t *o)
{
    const bees_t *s = (const bees_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < NV; i++) n += s->b[i].alive;
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: every slot flying (8 keys x swarms), hornets close and fast */
static const char SCENE_MAX[] =
    "0 particles 16\n0 param SPECIES 127\n0 param SWARM 127\n0 param SPEED 127\n0 param DIST 0\n0 param SPREAD 127\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "5000 end\n";

const tx_engine_t tx_eng_bees = {
    "bees", "#30 Bees / flies", NP, PARAMS, NV, 8, sizeof(bees_t), SCENE_MAX,
    bees_init, bees_key, bees_param, bees_render, bees_stats,
};
