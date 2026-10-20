/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #13 Small stones / gravel: a handful of stones shaken, or gravel under a step.
 *
 * PhISEM (SHAKER core, the STK port): grains are too small to ring in the audio band, so each collision is a click
 * filtered by one resonance whose frequency and width come from the grain size ( measured two gravels by
 * LPC: 1.5 cm -> 6460 Hz, r 0.932; 0.3 cm -> 12670 Hz, r 0.843). Between them, and beyond: f = 6460 Hz x
 * (d / 15 mm)^-0.418 (the power law through Cook's two points; G12's rigid-body law would be d^-1, see the note's
 * open question 3) and r linear in log d. A shake = shake energy decaying at SHAKE; collisions at N / 1024 per
 * sample; each collision bumps a sound level (decay 0.98 per sample, STK rocks) and re-tunes the resonance by
 * +-SPREAD (STK varyFactor 0.11-0.18). STK's rocks equaliser 1, 0, -1.
 *
 * Keys: a press = one shake / step (energy ~ velocity^2), held = steps at RATE. Each key is a grain size class
 * (d at C4 = SIZE, 2^(1/24) smaller per semitone up); each sounding key is one SHAKER voice (PARTICLES voices).
 * Ours: per-collision level ~ d (heavier stones click louder); GROUND = a shared pair of low box modes (230 / 610 Hz,
 * Q 4-12) for a container or a wooden floor, 0 = open ground. */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_shaker.h"
#include "tx_core_noise.h"

#define OUT_TRIM 14702               /* output gain, Q12 (+9.8 dB): integrated (a key held 10 s): -39.5 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +1.3 dB with it: the mean of 8 presses back on its
 * loudness (held by the peak bound: raw peaks at most 1 dB over the old ones) */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 60
#define VARY_DRIFT 40
#define VARY_SPLIT 0

enum { P_SIZE, P_AMOUNT, P_SHAKE, P_RATE, P_SPREAD, P_GROUND, NP };

static const tx_param_t PARAMS[NP] = {
    {"SIZE", 127, 72, "stone diameter at C4: 1 .. 40 mm (log) -> click resonance ~20 .. 4.3 kHz (Cook: 15 mm 6.5 kHz)"},
    {"AMOUNT", 127, 64, "number of stones N: 2 .. 2048 (log): collisions 43 N per second during a shake"},
    {"SHAKE", 127, 30, "how long a shake / step lasts (energy T60): 20 ms (crunch) .. 2 s (long rattle)"},
    {"RATE", 127, 59, "shakes / steps per second while a key is held: 0.5 .. 10 (log)"},
    {"SPREAD", 127, 35, "grain shape spread: resonance re-tuned per collision by +-0 .. 40 % (STK 11-18 %)"},
    {"GROUND", 127, 0, "what they rattle in: open ground .. a box / wooden floor (low modes 230 / 610 Hz)"},
};

#define NV 8u
#define NG 2u
#define L2_F15 829494              /* log2(6460 Hz), Q16 */
#define L2_D15 256034              /* log2(15 mm), Q16 */
#define BASE_L2 152000             /* collision gain at 15 mm: 2^2.32 = 5.0 (calibrated by render) */

TX_AUDIO_DATA static const uint16_t GROUND_HZ[NG] = { 230, 610 };

typedef struct {
    tx_shaker_t sh;
    uint8_t em, note, alive, pad;
} voice_t;

typedef struct {
    tx_cloud_t cloud;
    tx_declick_t dc;
    tx_dcblock_t hp;
    voice_t v[NV];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK];
    tx_svf_t ground[NG];
    int32_t gf[NG], gq, gwet;
    uint8_t p[NP], limit, quiet;
    int32_t level;                 /* Q15 */
    int32_t size_l2;               /* log2(d at C4 / 1 mm), Q16 */
    uint32_t n_q4, t60;
    int32_t vary;
    uint16_t dropped;
    tx_vary_t vy;
} gravel_t;

static void set_param(gravel_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_SIZE: s->size_l2 = (int32_t)(v * 2745u); break;               /* 0 .. log2(40) = 5.32 */
    case P_AMOUNT: s->n_q4 = tx_knob_exp(v, 32u, 10 << 16); break;      /* 2 .. 2048, Q4 */
    case P_SHAKE: s->t60 = tx_knob_exp(v, 20u, 435412); break;          /* 20 .. 2000 ms */
    case P_RATE: tx_cloud_rate(&s->cloud, tx_knob_exp(v, 128u, 283264)); break;   /* 0.5 .. 10 /s */
    case P_SPREAD: s->vary = (int32_t)v * 103; break;
    case P_GROUND: {
        unsigned i;
        s->gq = (int32_t)((16u << 14) / (64u + v * 128u / 127u));   /* 1 / Q, Q 4 .. 12 */
        s->gwet = (int32_t)v * 258;
        for (i = 0; i < NG; i++) s->gf[i] = tx_sin_q15(GROUND_HZ[i] * 97391u >> 1);
        break;
    }
    default: break;
    }
}

static void gravel_init(void *st, uint32_t seed)
{
    gravel_t *s = (gravel_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);                         /* a press: one shake now */
    tx_cloud_sizes(&s->cloud, 6144, 0);
    tx_cloud_regular(&s->cloud, 80);                         /* steps: fairly regular (ours) */
    for (i = 0; i < NV; i++) { tx_shaker_init(&s->v[i].sh, seed * 31u + i * 977u + 1u); s->v[i].em = 0xFF; }
    s->limit = 6;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((gravel_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void gravel_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((gravel_t *)st)->vy);
    tx_cloud_key(&((gravel_t *)st)->cloud, note, vel, on);
}

static void cut(gravel_t *s, voice_t *v)
{
    tx_declick_add(&s->dc, v->sh.last * 512);
    tx_shaker_init(&v->sh, v->sh.rng.s);
    v->alive = 0; v->em = 0xFF;
}

static void gravel_param(void *st, uint8_t id, uint8_t val)
{
    gravel_t *s = (gravel_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NV ? NV : val);
        for (i = s->limit; i < NV; i++) if (s->v[i].alive) cut(s, &s->v[i]);
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* one shake / step: the key's voice (or a new one) gets energy ~ velocity^2 */
static void shake(gravel_t *s, const tx_event_t *e)
{
    voice_t *v = 0;
    unsigned i;
    int32_t d_l2, f_l2, r, amp_l2;
    for (i = 0; i < s->limit && !v; i++)
        if (s->v[i].alive && s->v[i].em == e->emitter && s->v[i].note == e->note) v = &s->v[i];
    if (!v) {
        for (i = 0; i < s->limit && !v; i++) if (!s->v[i].alive) v = &s->v[i];
        if (!v) {                                            /* all busy: the one with the least energy */
            int32_t lo = 0x7FFFFFFF;
            for (i = 0; i < s->limit; i++) if (s->v[i].sh.energy < lo) { lo = s->v[i].sh.energy; v = &s->v[i]; }
            s->dropped++;
            cut(s, v);
        }
        v->alive = 1; v->em = e->emitter; v->note = e->note;
    }
    {                                                        /* (re)configure: the knobs apply at every shake */
        int32_t tn = tx_vary_tone(&s->vy);                   /* the press's character: brighter = finer, */
        d_l2 = s->size_l2 - ((int32_t)e->note - 60) * 2731 - tn;   /* at the same level (playable) */
        if (d_l2 < -65536) d_l2 = -65536;                    /* 0.5 .. 64 mm */
        if (d_l2 > 393216) d_l2 = 393216;
        f_l2 = L2_F15 - (int32_t)(((int64_t)(d_l2 - L2_D15) * 27394) >> 16);   /* f ~ d^-0.418 */
        r = 15270 + (int32_t)(((int64_t)(d_l2 - L2_D15) * 627) >> 16);          /* r: 0.843 @3 mm, 0.932 @15 mm */
        r = r < 9830 ? 9830 : r > 16000 ? 16000 : r;
        tx_shaker_res(&v->sh, 0, tx_exp2(f_l2 > 935000 ? 935000 : f_l2), r);
        amp_l2 = d_l2 - L2_D15 + tn;                         /* level ~ d (ours), +-3 octaves */
        amp_l2 = amp_l2 > (3 << 16) ? (3 << 16) : amp_l2 < -(3 << 16) ? -(3 << 16) : amp_l2;
        tx_shaker_objects(&v->sh, s->n_q4,
                          (int32_t)(((uint64_t)tx_exp2(BASE_L2 + amp_l2 + (11 << 16)) * (uint32_t)s->level) >> 15));
        tx_shaker_decay(&v->sh, s->t60, 32113);
        tx_shaker_vary(&v->sh, s->vary);
        tx_shaker_eq(&v->sh, 0, -1);
    }
    tx_shaker_shake(&v->sh, (int32_t)((uint32_t)e->vel * e->vel * 1040u));
}

TX_AUDIO_CODE static void gravel_render(void *st, int32_t *mix, unsigned n)
{
    gravel_t *s = (gravel_t *)st;
    unsigned k, i, j;
    int32_t *buf = s->buf;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) buf[j] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    for (i = 0; i < k; i++) shake(s, &s->ev[i]);             /* (block-accurate: the shake starts with the block) */
    for (i = 0; i < NV; i++) {
        voice_t *v = &s->v[i];
        if (!v->alive) continue;
        tx_shaker_add(&v->sh, buf, n);
        if (tx_shaker_quiet(&v->sh)) { v->alive = 0; v->em = 0xFF; }
    }
    /* ground / box modes on the sum (shared bank helper, unit-peak modes) */
    if (s->gwet) {
        int32_t g[NG];
        for (i = 0; i < NG; i++) g[i] = s->gwet >> 4;        /* Q11 */
        tx_svf_bank_add(s->ground, s->gf, s->gq, g, NG, buf, buf, n);
    } else
        for (i = 0; i < NG; i++) s->ground[i].low = s->ground[i].band = 0;
    tx_declick_run(&s->dc, buf, n);
    tx_vary_block(&s->vy, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = s->dc.v | s->hp.x1 | s->hp.y1;
        for (i = 0; i < NV; i++) z |= s->v[i].alive;
        z |= !tx_svf_bank_quiet(s->ground, NG);
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void gravel_stats(const void *st, tx_stats_t *o)
{
    const gravel_t *s = (const gravel_t *)st;
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

/* the worst case: 8 voices, a collision every sample, long shakes re-fired, re-tuning, the box modes */
static const char SCENE_MAX[] =
    "0 particles 8\n0 param AMOUNT 127\n0 param SHAKE 127\n0 param RATE 127\n0 param SPREAD 127\n0 param GROUND 127\n"
    "0 param SIZE 0\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "1500 on 53 127\n1500 on 60 127\n2000 param SIZE 127\n2000 on 64 127\n2000 on 79 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_gravel = {
    "gravel", "#13 Small stones / gravel", NP, PARAMS, NV, 6, sizeof(gravel_t), SCENE_MAX,
    gravel_init, gravel_key, gravel_param, gravel_render, gravel_stats,
};
