/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #12 Rolling pebble: pebbles rolling across a table, a slab, a tray (05 for the bodies).
 *
 * A key press pushes a pebble (velocity = push speed), holding the key keeps it rolling (a slope), release lets it
 * coast to a stop. Physics:
 *   contact  F2's roll prototype on the SCRAPE core: impacts with AR(1)-correlated amplitudes and gaps (0.8), each a
 *            raised cosine of Hertz length: t_c ~ r v^-0.2 (81 us = 3.6 samples for r 10 mm at 1 m/s), the
 *            amplitude dependence K A^-1/5 (F2). Rath's bridging (F3): a ball of radius r only reaches the surface
 *            peaks it can bridge, about w = 2 sqrt(2 r h) apart for roughness depth h: impacts at v / w per second,
 *            bigger pebbles on smoother surfaces = sparser, rounder. Impact level ~ momentum: r^1.5 v (normalised).
 *   shape    out-of-roundness: the impacts swell and fade once per turn, at v / (2 pi r) (F3's rotation term).
 *   motion   rolling resistance: v drops at mu_r g (DRAG) unless the key is held; the pebble stops below 1 cm/s.
 *   surface  what is heard is mostly the surface (pebbles ring ultrasonically, note 05): one shared modal bank fed
 *            by every pebble's force (physically one object), 6 modes of irregular plate-like ratios (ours), Q from
 *            the material (wood 40, stone 150 (granite, 05), glass 400, metal 1000), driven at unit noise power.
 *   bounce   a push drops the ball onto the surface first (from 1 .. 12 cm by velocity: a marble set down, a ball
 *            thrown): it bounces with restitution e (wood 0.55, stone 0.75, glass 0.85, metal 0.8, each bounce +-12 %),
 *            the flights shrinking as e^n (t_n = 2 v_n / g) and each impact's push ~ its speed (momentum), Hertz-short,
 *            until the flights are under 8 ms: then it rolls (the references' bursty, kurtotic first second: a
 *            marble's accelerating patter, a ball's thuds; note 18).
 * Keys: the pebble radius at C4 = SIZE, 2^(1/12) smaller per semitone up.
 * Realism pass (marble-on-wood references, analysis only): real rolling is irregular and bright (energy 0.6-4 kHz,
 * kurtosis 14-370, event-level std 6-7 dB, slow 0.5-0.8 Hz level changes). So: the wood table's modes start at 620 Hz
 * with Q 12 (were 180 Hz, Q 40: a low honk), more of the contact itself, a per-block level jitter (+-6 dB) and bumps
 * (+9 dB, ~3 / s), a slow speed wander (+-0.5 oct) and a jittered turn (a periodic 5.7 Hz swell read as a wheel). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_scrape.h"

#define OUT_TRIM 11813               /* output gain, Q12 (+3.8 dB with the spectrogram fixes, session 5; +5.2 dB): integrated (a key held 10 s): -31.2 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +0.2 dB with it: the mean of 8 presses back on its
 * loudness (held by the peak bound: raw peaks at most 1 dB over the old ones) */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 90
#define VARY_DRIFT 60
#define VARY_SPLIT 3

enum { P_SIZE, P_SPEED, P_ROUGH, P_ROUND, P_SURFACE, P_DRAG, NP };

static const tx_param_t PARAMS[NP] = {
    {"SIZE", 127, 48, TX_TEXT("ball radius at C4: 2 .. 100 mm (log; a marble 8 mm, a ball 50): longer, softer, sparser impacts as it grows")},
    {"SPEED", 127, 64, TX_TEXT("push speed at velocity 100: 5 cm/s .. 2 m/s (log)")},
    {"ROUGH", 127, 50, TX_TEXT("surface roughness depth h: 1 .. 100 um (log): bridging spacing 2 sqrt(2 r h), louder")},
    {"ROUND", 127, 12, TX_TEXT("out-of-roundness: a sphere .. a lumpy pebble (swells once per turn, v / 2 pi r)")},
    {"SURFACE", 127, 38, TX_TEXT("surface: wood floor (200 Hz, Q 15) | wood table (620 Hz, Q 12) | stone slab (150) | glass (400) | metal tray (1000)")},
    {"DRAG", 127, 62, TX_TEXT("rolling resistance mu_r: 0.005 (marble on glass) .. 0.2 (lumpy pebble on wood) (log): coasting")},
};

#define NV 8u
#define NM 6u
#define PATH_SPAN 39322            /* a node-to-node span x 2^(+-0.6), log2 Q16 */
#define G_REF 4000                 /* impact gain: r 10 mm at 0.3 m/s (Q15; calibrated by render) */

TX_AUDIO_DATA static const uint16_t SURF_HZ[5] = { 200, 620, 420, 600, 260 };      /* first surface mode (ours; wood from the
                                                              * marble-on-wood references: 0.6-4 kHz) */
TX_AUDIO_DATA static const uint16_t SURF_Q[5] = { 15, 12, 150, 400, 1000 };
TX_AUDIO_DATA static const uint16_t SURF_E[5] = { 16384, 18022, 24576, 27853, 26214 };   /* restitution, Q15 (ours) */
TX_AUDIO_DATA static const uint16_t SURF_R[NM] = { 1024, 1516, 2222, 2806, 3717, 4823 };   /* ratios, Q10 (ours) */
TX_AUDIO_DATA static const uint16_t SURF_G[NM] = { 2048, 1843, 1638, 1434, 1229, 1024 };   /* gains, Q11 */

typedef struct {
    tx_scrape_t sc;
    uint32_t v;                    /* speed, um/s */
    uint32_t rot;                  /* rotation phase */
    int32_t wander;                /* slow random speed wander (Q15, ~0.8 Hz): an uneven slope / table */
    int32_t r_l2;                  /* log2(r / 1 mm), Q16 */
    int32_t rt;                    /* the press's character in r_l2 (its level change is taken back: playable) */
    uint32_t bt, bfl;              /* bouncing: samples to the next impact, the flight now (samples) */
    int32_t ba;                    /* the next impact's push (Q15; 0: rolling) */
    uint8_t em, note, alive, pad;
} voice_t;

typedef struct {
    tx_cloud_t cloud;
    tx_declick_t dc;
    tx_dcblock_t hp;
    voice_t v[NV];
    tx_svf_t surf[NM];
    int32_t sf[NM], sgin[NM], sg[NM], sq;
    uint32_t kph[NM], mph[NM];     /* each mode's spatial frequency (phase per mm Q8) and its phase at the contact */
    uint16_t kq[NM];               /* this node-to-node span's length factor, Q8 (the path's own) */
    uint32_t prng;                 /* the path's draws (apart from the cloud's) */
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t force[TX_BLOCK], buf[TX_BLOCK];
    uint8_t p[NP], limit, quiet;
    int32_t level;                 /* Q15 */
    int32_t size_l2, speed_l2, h_l2;
    uint32_t decel;                /* um/s lost per block */
    int32_t round;                 /* Q15 */
    uint16_t dropped;
    tx_vary_t vy;
} pebble_t;

static uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) { if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return r;
}

static void set_surface(pebble_t *s)
{
    unsigned i, m = (s->p[P_SURFACE] * 5u) >> 7;             /* 5 surfaces (a wood floor: the ball's thuds, note 18) */
    uint32_t q = SURF_Q[m];
    s->sq = (int32_t)(16384u / q);
    for (i = 0; i < NM; i++) {
        uint32_t hz = (uint32_t)SURF_HZ[m] * SURF_R[i] >> 10;
        s->sf[i] = tx_svf_coef(hz);
        s->sgin[i] = (int32_t)tx_svf_gin_noise(hz, q * 16u);
        s->sg[i] = SURF_G[i];
        s->kph[i] = 83886u * isqrt32(hz * 109u) / 256u;     /* 2^24 / lambda, lambda = 200 mm sqrt(600 Hz / f) */
    }
}

static void set_param(pebble_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_SIZE: s->size_l2 = 65536 + (int32_t)(v * 2911u); break;          /* log2(2 mm) + v log2(50) / 127 */
    case P_SPEED: s->speed_l2 = 1022987 + (int32_t)(v * 2747u); break;     /* log2(50000 um/s) + v log2(40) / 127 */
    case P_ROUGH: s->h_l2 = (int32_t)(v * 3428u); break;                   /* log2(h / 1 um): 0 .. log2(100) */
    case P_ROUND: s->round = (int32_t)v * 206; break;                      /* swell depth 0 .. 0.8 */
    case P_SURFACE: set_surface(s); break;
    case P_DRAG: {                                           /* mu_r g dt, um/s per block: mu 0.005 .. 0.2 */
        uint32_t mu_q16 = tx_knob_exp(v, 328u, 348782);      /* 0.005 x 2^(5.32 v / 127), Q16 */
        s->decel = (uint32_t)(((uint64_t)mu_q16 * 56946u) >> 16);   /* 9.81 m/s^2 x 5.805 ms = 56946 um/s */
        if (!s->decel) s->decel = 1;
        break;
    }
    default: break;
    }
}

static void pebble_init(void *st, uint32_t seed)
{
    pebble_t *s = (pebble_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    for (i = 0; i < NV; i++) { tx_scrape_init(&s->v[i].sc, seed * 11u + i * 6151u + 9u); s->v[i].em = 0xFF; }
    s->limit = 6;
    s->prng = seed * 2654435761u + 0x7E3B1u;
    if (!s->prng) s->prng = 1;
    for (i = 0; i < NM; i++) s->kq[i] = 256;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((pebble_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void pebble_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    pebble_t *s = (pebble_t *)st;
    unsigned i, rolling = 0;
    for (i = 0; i < s->limit; i++) rolling |= s->v[i].alive && s->v[i].note == note;
    if (on && vel && !rolling) tx_vary_press(&s->vy);       /* (a pebble rolling is pushed on: the same pebble, its press
                                                              * level kept; drawn afresh at each push, a pebble pushed
                                                              * often was quieter the more it was pushed) */
    tx_cloud_key(&((pebble_t *)st)->cloud, note, vel, on);
}

static void cut(pebble_t *s, voice_t *v)
{
    tx_declick_add(&s->dc, v->sc.lp * 256);
    tx_scrape_init(&v->sc, v->sc.rng.s);
    v->alive = 0; v->em = 0xFF; v->v = 0;
}

static void pebble_param(void *st, uint8_t id, uint8_t val)
{
    pebble_t *s = (pebble_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NV ? NV : val);
        for (i = s->limit; i < NV; i++) if (s->v[i].alive) cut(s, &s->v[i]);
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* a push: a new pebble (or the same key's pebble pushed again) */
static void push(pebble_t *s, const tx_event_t *e)
{
    voice_t *v = 0;
    unsigned i;
    uint32_t v0;
    int fresh = 0;
    for (i = 0; i < s->limit && !v; i++) if (s->v[i].alive && s->v[i].note == e->note) v = &s->v[i];
    if (!v) {
        fresh = 1;
        for (i = 0; i < s->limit && !v; i++) if (!s->v[i].alive) v = &s->v[i];
        if (!v) {                                            /* all rolling: the slowest makes way */
            uint32_t lo = 0xFFFFFFFFu;
            for (i = 0; i < s->limit; i++) if (s->v[i].v < lo) { lo = s->v[i].v; v = &s->v[i]; }
            s->dropped++;
            cut(s, v);
        }
        v->alive = 1;
        v->rot = tx_rand(&s->cloud.rng);
    }
    v->em = e->emitter; v->note = e->note;
    v->rt = tx_vary_tone(&s->vy);
    v->r_l2 = s->size_l2 - ((int32_t)e->note - 60) * 5461 - v->rt;   /* brighter = smaller */
    if (v->r_l2 < 0) v->r_l2 = 0;                            /* >= 1 mm */
    v0 = tx_exp2(s->speed_l2);                               /* um/s */
    v0 = v0 * e->vel / 100u;                                 /* <= 2e6 x 127: 32-bit */
    if (v0 > v->v) v->v = v0;
    if (fresh) {   /* the drop: h = 1 .. 12 cm by velocity, impact speed sqrt(2 g h), the flight 2 v / g after each bounce
                    * (a new pebble set down; one rolling is just pushed on: dropped afresh at each push, a pebble pushed
                    * often stayed in its sparse bounces, quieter the more it was pushed) */
        uint32_t h_mm = 10u + (uint32_t)e->vel * e->vel * 110u / 16129u;
        uint32_t vi = isqrt32(19620u * h_mm);                /* mm/s: sqrt(2 g h) */
        v->bfl = vi * 9u;                                    /* 2 v / g in samples: 2 x 44100 / 9810 = 9.0 per mm/s */
        v->bt = tx_rand_below(&s->cloud.rng, 64u) + e->offset;
        v->ba = (int32_t)(vi > 1500u ? 32767u : vi * 21u);   /* the push ~ speed */
    }
    if (fresh) v->sc.wait = 0;                               /* the first impact now (not a wait left over from a */
}                                                            /* pebble that was slowing to a stop) */

/* the bounces in this block: each a Hertz-short half sine (its length from the radius), the next after e x the flight */
static void bounce(pebble_t *s, voice_t *v, int32_t *force, unsigned n)
{
    tx_rng_t *r = &s->cloud.rng;
    while (v->ba && v->bt < n) {
        unsigned j, d = 2u + (tx_exp2(v->r_l2 + (16 << 16)) >> 18);   /* ~ r: 2 .. 12 samples */
        int32_t a = (int32_t)(((int64_t)v->ba * s->level) >> 15) * (int32_t)(G_REF >> 9) >> 5;   /* (calibrated: the drop stands over the roll) */
        uint32_t ph = 0, inc = 0x80000000u / d, e;
        a = (int32_t)(((int64_t)a * (24576 + (int32_t)(tx_rand(r) >> 17))) >> 15);   /* +-2.5 dB */
        if (v->bt + d > n) v->bt = n - d;                     /* (whole in this block) */
        for (j = v->bt; j < v->bt + d; j++, ph += inc) force[j] += (tx_sin_q15(ph) * (a >> 4)) >> 3;
        e = SURF_E[(s->p[P_SURFACE] * 5u) >> 7];
        e = (uint32_t)(((int64_t)e * (28836 + (int32_t)(tx_rand(r) >> 19))) >> 15);   /* 0.88 .. 1.12 */
        v->bfl = (uint32_t)(((uint64_t)v->bfl * e) >> 15);
        v->ba = (int32_t)(((int64_t)v->ba * e) >> 15);
        v->bt += v->bfl;
        if (v->bfl < 353u || v->ba < 64) { v->ba = 0; v->bt = 0; }   /* under 8 ms: rolling */
    }
    if (v->ba) v->bt -= n;
}

/* per block: the pebble's motion -> its contact (gap, Hertz length, level, rotation swell, cutoff) */
static void roll(pebble_t *s, voice_t *v)
{
    int held = v->em < TX_CLOUD_EMITTERS && s->cloud.em[v->em].held && s->cloud.em[v->em].note == v->note;
    uint32_t w_um, gap, rot_inc, fc;
    int32_t a_l2, vl2;
    if (!held) v->v = v->v > s->decel ? v->v - s->decel : 0;   /* coasting: mu_r g */
    v->wander += (tx_rand_s15(&s->cloud.rng) - v->wander) >> 5;   /* ~0.85 Hz low-passed random */
    if (v->v < 10000u) {                                     /* under 1 cm/s: it stops */
        v->v = 0;
        v->sc.imp = 0; v->sc.gap_q8 = 0;
        return;
    }
    vl2 = (32 << 16) - (int32_t)(tx_neglog2_q12(v->v) << 4); /* log2(v / 1 um/s) */
    vl2 += v->wander * 3;                                    /* +-0.5 octave of speed wander (std ~0.1) */
    /* bridging spacing w = 2 sqrt(2 r h), um: r (um) = 1000 2^r_l2, h (um) = 2^h_l2 */
    w_um = 2u * isqrt32((uint32_t)(((uint64_t)tx_exp2(v->r_l2 + (16 << 16)) * 2000u * tx_exp2(s->h_l2 + (16 << 16))) >> 32));
    if (w_um < 20u) w_um = 20u;
    gap = (w_um * 44100u) / (v->v >> 8);                   /* fs w / v, Q8 (w <= 6 mm, v >= 1 cm/s: 32-bit) */
    v->sc.gap_q8 = gap < 256u ? 256u : gap;
    v->sc.rub = 0;
    v->sc.corr = 26214;                                      /* F2 roll: correlated series (0.8) */
    /* Hertz length at amplitude 1: 3.6 samples x (r / 10 mm) x (v / 1 m/s)^-0.2, Q8 */
    v->sc.kdur_q8 = tx_exp2((int32_t)(v->r_l2 - 217706 - (vl2 - 1306153) / 5 + (int32_t)(8 << 16) + 121104));
    if (v->sc.kdur_q8 < (2u << 8)) v->sc.kdur_q8 = 2u << 8;
    if (v->sc.kdur_q8 > (64u << 8)) v->sc.kdur_q8 = 64u << 8;
    /* level ~ r^1.5 v (momentum), against r 10 mm at 0.3 m/s, and rougher = louder (h^0.5, ours) */
    a_l2 = (3 * (v->r_l2 - 217706 + v->rt)) / 2 + (vl2 - 1192079) + (s->h_l2 - 217706) / 2;
    {   /* irregularity (references: event-level std 5.6-7.2 dB, kurtosis 14-370): the contact's level jitters per block
         * (+-6 dB, triangular) and a bump or a grain of grit gives a louder burst at ~3 / s (+9 dB, ours) */
        tx_rng_t *r = &s->cloud.rng;
        a_l2 += (tx_rand_s15(r) + tx_rand_s15(r)) * 1;
        if (tx_rand(r) < 73u * 1000000u) a_l2 += 98304;      /* p = 0.017 per block */
    }
    a_l2 = a_l2 > (2 << 16) ? (2 << 16) : a_l2 < -(10 << 16) ? -(10 << 16) : a_l2;
    v->sc.imp = (int32_t)(((uint64_t)tx_exp2(a_l2 + (16 << 16)) * G_REF * (uint32_t)s->level) >> 31);
    if (v->sc.imp > 32767) v->sc.imp = 32767;
    /* out-of-roundness: the impacts swell once per turn, v / (2 pi r) */
    rot_inc = tx_exp2(vl2 + 783429 - v->r_l2);               /* v / (2 pi r) per block: log2(256 2^32 / (fs 2 pi 1000)) */
    rot_inc += (uint32_t)(((int64_t)rot_inc * tx_rand_s15(&s->cloud.rng)) >> 16);   /* +-50 %: a pebble is no wheel */
    v->rot += rot_inc;
    v->sc.amod = 32768 + ((tx_sin_q15(v->rot) * s->round) >> 15);
    /* contact low-pass: 500 Hz + 8 v / w */
    fc = 500u + v->v * 8u / w_um;
    tx_scrape_cutoff(&v->sc, fc);
}

TX_AUDIO_CODE static void pebble_render(void *st, int32_t *mix, unsigned n)
{
    pebble_t *s = (pebble_t *)st;
    unsigned k, i, j;
    int32_t *buf = s->buf, *force = s->force;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) buf[j] = force[j] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    for (i = 0; i < k; i++) push(s, &s->ev[i]);
    for (i = 0; i < NV; i++) {
        voice_t *v = &s->v[i];
        if (!v->alive) continue;
        if (v->ba) bounce(s, v, force, n);
        roll(s, v);
        if (!v->ba) tx_scrape_add(&v->sc, force, n);         /* (it rolls once it stops bouncing) */
        if (!v->v && tx_scrape_quiet(&v->sc)) { v->alive = 0; v->em = 0xFF; }
    }
    {   /* the surface's modes as the pebble rolls across them: a mode is excited ~ its shape at the contact point, so
         * each comes and goes as the contact passes its nodes (bending waves, lambda ~ 200 mm at 600 Hz, ~ f^-1/2;
         * ours). Session 5: the modes were steady lines for the whole hold; the references' come and go. The path is no
         * straight line at one speed: each span from node to node its own length (x 2^+-PATH_SPAN); at one steady
         * speed the modes came and went on a clock, a 3.2 Hz beat in every seed. */
        uint32_t vs = 0, m;
        for (i = 0; i < NV; i++) if (s->v[i].alive) vs += s->v[i].v;
        vs = (vs > 4000000u ? 4000000u : vs) * 3u >> 11;      /* mm per block, Q8 */
        for (m = 0; m < NM; m++) {
            int32_t sh;
            uint32_t ph = s->mph[m];
            s->mph[m] += ((vs * s->kq[m]) >> 8) * s->kph[m];
            if ((ph ^ s->mph[m]) & 0x80000000u) {          /* past a node: the next span its own length */
                uint32_t x = s->prng;
                x ^= x << 13; x ^= x >> 17; x ^= x << 5; s->prng = x;
                s->kq[m] = (uint16_t)(tx_exp2((int32_t)((x >> 16) * (2u * PATH_SPAN) >> 16) - PATH_SPAN + (8 << 16)));
            }
            sh = tx_sin_q15(s->mph[m]);
            sh = (sh * sh) >> 15;                            /* the shape's power at the contact: 0 at a node */
            s->sg[m] = (int32_t)((SURF_G[m] * (uint32_t)(983 + ((sh * 31785) >> 15))) >> 15);   /* 0.03 .. 1 */
        }
    }
    for (j = 0; j < n; j++) buf[j] = force[j] >> 1;         /* the contact itself (the clicks' edge) */
    tx_svf_bank_add_g(s->surf, s->sf, s->sq, s->sgin, s->sg, NM, force, buf, n);   /* the surface */
    tx_declick_run(&s->dc, buf, n);
    tx_vary_block(&s->vy, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = s->dc.v | s->hp.x1 | s->hp.y1 | !tx_svf_bank_quiet(s->surf, NM);
        for (i = 0; i < NV; i++) z |= s->v[i].alive;
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void pebble_stats(const void *st, tx_stats_t *o)
{
    const pebble_t *s = (const pebble_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < NV; i++) n += s->v[i].alive;
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: 8 pebbles rolling fast on a rough metal tray, held, re-pushed */
static const char SCENE_MAX[] =
    TX_TEXT("0 particles 8\n0 param SPEED 127\n0 param ROUGH 127\n0 param ROUND 127\n0 param SURFACE 127\n0 param SIZE 0\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "1500 on 53 127\n1500 on 60 127\n2000 param SIZE 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4000 end\n");

const tx_engine_t tx_eng_pebble = {
    "pebble", TX_TEXT("#12 Rolling pebble"), NP, PARAMS, NV, 6, sizeof(pebble_t), SCENE_MAX,
    pebble_init, pebble_key, pebble_param, pebble_render, pebble_stats,
};
