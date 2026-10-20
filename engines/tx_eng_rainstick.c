/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #16 Rain stick: seeds falling through a pinned tube.
 *
 * A key press turns the stick over, holding keeps it tilted, release lays it flat. Physics:
 *   flow     the seeds above run down at a constant rate until the reservoir is empty (a granular flow does not
 *            depend on the column height: Beverloo, G16), so the sound holds, then stops. Turning it over again
 *            puts the seeds that already fell back on top (reservoir 1 - R). Laid flat, the seeds in transit settle
 *            (energy T60 250 ms, ours).
 *   clicks   PhISEM (SHAKER core): the shake energy is held at the flow level; collisions per second =
 *            seeds x pins hit / fall time (each seed meets ~20 pins on the way: ours); a seed resonance from the
 *            seed kind (GRAIN), re-tuned per collision (STK varyFactor 0.25); sound decay 0.96 (STK cabasa).
 *   tube     the tube's air column, closed at both ends: modes f_n = n c / 2L (n = 1..3, gains 1, 0.7, 0.5), driven
 *            by the raw pin impacts (broadband force impulses) (granular flow in a pipe couples to the tube's acoustic modes: G24 / G25 silo music).
 *            Each key is a stick of its own length: a semitone up = 2^(1/12) shorter.
 *   pulse    narrow-tube grain flow is intermittent (~4 Hz, G21): an optional flow pulsation, jittered.
 * Velocity = tilt: faster flow and harder clicks. No rain-stick measurement exists: the pin count, tube Q
 * and gains are ours. */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_cloud.h"
#include "tx_core_shaker.h"
#include "tx_core_noise.h"

#define OUT_TRIM 9492               /* output gain, Q12 (+7.3 dB): integrated (a key held 10 s): -33.3 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

enum { P_SEEDS, P_TIME, P_GRAIN, P_LENGTH, P_BODY, P_PULSE, NP };

static const tx_param_t PARAMS[NP] = {
    {"SEEDS", 127, 64, "seeds inside: 50 .. 5000 (log): density of the patter"},
    {"TIME", 127, 61, "fall time of a full stick (tilt, pin density): 2 .. 20 s (log)"},
    {"GRAIN", 127, 50, "seed kind: small hard seeds / grit (9 kHz, dry) .. big beans (2 kHz, ringing)"},
    {"LENGTH", 127, 56, "tube length at C4: 0.3 .. 2 m (log) -> air modes c/2L = 570 .. 86 Hz (keys: shorter going up)"},
    {"BODY", 127, 60, "tube resonance: none .. strong (mode Q 6 .. 25)"},
    {"PULSE", 127, 20, "intermittent flow: steady .. pulsing (~4 Hz)"},
};

#define NV 6u
#define NT 3u
#define E_L2 (23 << 16)            /* flow energy at velocity 127: 0.5 (Q24) */
#define BASE_Q11 6144              /* collision gain base: 3.0 (calibrated by render) */

TX_AUDIO_DATA static const uint8_t TUBE_G[NT] = { 128, 90, 64 };   /* mode gains, Q7 (ours) */

typedef struct {
    tx_shaker_t sh;
    tx_svf_t tube[NT];
    int32_t f[NT];
    int32_t res;                   /* reservoir, Q16 (65536 = all seeds at the top) */
    uint32_t pph;                  /* pulse phase */
    uint8_t em, note, vel, alive;
} voice_t;

typedef struct {
    tx_cloud_t cloud;
    tx_declick_t dc;
    tx_dcblock_t hp;
    voice_t v[NV];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t tmp[TX_BLOCK], exc[TX_BLOCK], buf[TX_BLOCK];
    uint8_t p[NP], limit, quiet;
    int32_t level;                 /* Q15 */
    uint32_t seeds, t_ms;
    int32_t gf, gr;                /* seed resonance Hz, r Q14 */
    int32_t len_l2;                /* log2(f1 at C4 / 1 Hz), Q16 */
    int32_t q, wet, pulse;
    uint16_t dropped;
} stick_t;

static void set_param(stick_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_SEEDS: s->seeds = tx_knob_exp(v, 50u, 435412); break;          /* 50 .. 5000 */
    case P_TIME: s->t_ms = tx_knob_exp(v, 2000u, 217706); break;          /* 2 .. 20 s */
    case P_GRAIN:
        s->gf = (int32_t)tx_exp2((int32_t)(860880 - (int32_t)v * 1120));  /* 9000 Hz x 2^(-2.17 v / 127) */
        s->gr = 11469 + (int32_t)v * 28;                                  /* r 0.70 .. 0.92 */
        break;
    case P_LENGTH: s->len_l2 = 600137 - (int32_t)(v * 1414u); break;     /* log2(343 / 0.6 m) - v log2(6.67) / 127 */
    case P_BODY: s->q = (int32_t)((16u << 14) / (96u + v * 304u / 127u)); s->wet = (int32_t)v * 258; break;
    case P_PULSE: s->pulse = (int32_t)v * 206; break;                     /* depth 0 .. 0.8, Q15 */
    default: break;
    }
}

static void stick_init(void *st, uint32_t seed)
{
    stick_t *s = (stick_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);                         /* a press = turn it over (one event) */
    tx_cloud_rate(&s->cloud, 0);                             /* no stream: the flow is the voice's */
    for (i = 0; i < NV; i++) { tx_shaker_init(&s->v[i].sh, seed * 17u + i * 4099u + 3u); s->v[i].em = 0xFF; }
    s->limit = 4;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
}

static void stick_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    tx_cloud_key(&((stick_t *)st)->cloud, note, vel, on);
}

static void cut(stick_t *s, voice_t *v)
{
    unsigned i;
    tx_declick_add(&s->dc, v->sh.last * 512);
    tx_shaker_init(&v->sh, v->sh.rng.s);
    for (i = 0; i < NT; i++) v->tube[i].low = v->tube[i].band = 0;
    v->alive = 0; v->em = 0xFF; v->res = 0;
}

static void stick_param(void *st, uint8_t id, uint8_t val)
{
    stick_t *s = (stick_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NV ? NV : val);
        for (i = s->limit; i < NV; i++) if (s->v[i].alive) cut(s, &s->v[i]);
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* a key press: turn the stick over */
static void turn(stick_t *s, const tx_event_t *e)
{
    voice_t *v = 0;
    unsigned i;
    for (i = 0; i < s->limit && !v; i++)
        if (s->v[i].alive && s->v[i].note == e->note) v = &s->v[i];
    if (v) { v->res = 65536 - v->res; v->em = e->emitter; v->vel = e->vel; return; }   /* the fallen seeds are on top */
    for (i = 0; i < s->limit && !v; i++) if (!s->v[i].alive) v = &s->v[i];
    if (!v) {
        int32_t lo = 0x7FFFFFFF;
        for (i = 0; i < s->limit; i++) if (s->v[i].res < lo) { lo = s->v[i].res; v = &s->v[i]; }
        s->dropped++;
        cut(s, v);
    }
    v->alive = 1; v->em = e->emitter; v->note = e->note; v->vel = e->vel; v->res = 65536;
    v->pph = tx_rand(&s->cloud.rng);
    {
        int32_t f1 = s->len_l2 + ((int32_t)e->note - 60) * 5461;
        for (i = 0; i < NT; i++) {
            uint32_t hz = (tx_exp2(f1) * (i + 1u));
            v->f[i] = tx_svf_coef(hz);
        }
    }
}

/* per block: the flow of one stick -> its shaker's energy and collision rate */
static void flow(stick_t *s, voice_t *v)
{
    int held = v->em < TX_CLOUD_EMITTERS && s->cloud.em[v->em].held && s->cloud.em[v->em].note == v->note;
    uint32_t vel = v->vel;
    tx_shaker_res(&v->sh, 0, (uint32_t)s->gf, s->gr);
    tx_shaker_vary(&v->sh, 8192);
    tx_shaker_eq(&v->sh, 0, -1);
    if (held && v->res > 0) {
        uint32_t n_q4 = s->seeds * 7430u / s->t_ms * vel / 100u;   /* N = seeds x 20 pins / T / 43.07, Q4 */
        uint32_t dr = 380430u * vel / 100u / s->t_ms;          /* reservoir per block: 1 / (T blocks) */
        int32_t e = (int32_t)tx_exp2(E_L2) / 16129 * (int32_t)(vel * vel), m;
        tx_shaker_objects(&v->sh, n_q4, (int32_t)(((uint32_t)BASE_Q11 * (uint32_t)s->level) >> 15));
        tx_shaker_decay(&v->sh, 250, 31457);                 /* flat: the seeds in transit settle; decay 0.96 */
        v->pph += 24932159u * 4u + (uint32_t)((int32_t)tx_rand_s15(&s->cloud.rng) * 900);   /* ~4 Hz +-30 % */
        m = 32768 + ((tx_sin_q15(v->pph) * s->pulse) >> 15);
        tx_shaker_energy(&v->sh, (int32_t)(((int64_t)e * m) >> 15));
        v->res -= (int32_t)(dr ? dr : 1u);
        if (v->res < 0) v->res = 0;
    }
}

TX_AUDIO_CODE static void stick_render(void *st, int32_t *mix, unsigned n)
{
    stick_t *s = (stick_t *)st;
    unsigned k, i, j, t;
    int32_t *buf = s->buf, *tmp = s->tmp, *exc = s->exc;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) buf[j] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    for (i = 0; i < k; i++) turn(s, &s->ev[i]);
    for (i = 0; i < NV; i++) {
        voice_t *v = &s->v[i];
        int32_t dry = 32768 - (s->wet >> 1), g[NT];
        if (!v->alive) continue;
        flow(s, v);
        for (j = 0; j < n; j++) tmp[j] = exc[j] = 0;
        tx_shaker_add_exc(&v->sh, tmp, exc, n);
        for (j = 0; j < n; j++) buf[j] += (tmp[j] >> 15) * dry + (((tmp[j] & 0x7FFF) * dry) >> 15);
        for (t = 0; t < NT; t++) g[t] = ((s->wet >> 4) * TUBE_G[t]) >> 7;   /* Q11 */
        if (s->wet) tx_svf_bank_add(v->tube, v->f, s->q, g, NT, exc, buf, n);   /* the pin impacts drive the tube */
        else for (t = 0; t < NT; t++) v->tube[t].low = v->tube[t].band = 0;
        if (tx_svf_bank_quiet(v->tube, NT) && tx_shaker_quiet(&v->sh) && (v->res == 0 || !(v->em < TX_CLOUD_EMITTERS
            && s->cloud.em[v->em].held && s->cloud.em[v->em].note == v->note))) { v->alive = 0; v->em = 0xFF; }
    }
    tx_declick_run(&s->dc, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = s->dc.v | s->hp.x1 | s->hp.y1;
        for (i = 0; i < NV; i++) z |= s->v[i].alive;
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void stick_stats(const void *st, tx_stats_t *o)
{
    const stick_t *s = (const stick_t *)st;
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

/* the worst case: 6 sticks, the densest flow (a collision every sample), strong tubes, pulsing, turned again */
static const char SCENE_MAX[] =
    "0 particles 6\n0 param SEEDS 127\n0 param TIME 0\n0 param BODY 127\n0 param PULSE 127\n0 param GRAIN 127\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "1500 on 53 127\n1500 on 60 127\n1500 on 67 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_rainstick = {
    "rainstick", "#16 Rain stick", NP, PARAMS, NV, 4, sizeof(stick_t), SCENE_MAX,
    stick_init, stick_key, stick_param, stick_render, stick_stats,
};
