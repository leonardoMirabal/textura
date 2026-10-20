/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #15 Singing / booming sand: the song of dunes (SYNC core).
 *
 * A key press starts an avalanche on the slip face (the flow ramps up over ~0.1 s), holding keeps it flowing,
 * release lets it stop (~0.3 s). Physics (G5): grains in the avalanche collide at the shear rate
 * Gamma = 0.4 sqrt(g / d) (100 Hz for 160-180 um, 66 Hz for 380 um: measured in four deserts) and excite elastic
 * waves at the surface; the waves pull the collisions into step (mode-locking, a phase-locked loop: G5 Fig. 5) so the
 * avalanche "sings" a loud tone at f ~ Gamma (100-105 dB, coherence ~10 periods when the avalanche is steady, a few
 * when it is intermittent), saturating when the vibration lifts the grains off the flow. Below a threshold speed
 * (0.23 m/s for avalanches, 0.47 m/s pushed) there is no song, only the grains' rumble; poorly
 * sorted sand does not sing (G11: sorted -> tone, mixed -> noise). Near the threshold two patches may sing a doublet
 * (beating). Pushing fast raises f (inertia: G5); here velocity raises f up to +10 % (ours).
 * Keys: grain size per key (C4 = GRAIN, 2^(-2/12) per semitone up: f a semitone up). Ours: the surface resonance Q 10,
 * the coupling strength, the hiss band (0.9 kHz, Q 0.7, -42 dB).
 * Realism pass (singing-dune references, analysis only): the song is a nearly pure tone (28-43 dB over the rest of
 * 40-400 Hz) falling 6-8 dB / oct above, with little hiss. So: the surface resonance Q 10 -> 25, an output one-pole
 * low-pass at ~500 Hz (the bed absorbs the highs), the hiss 12 dB lower and at 0.9 kHz (was 1.6). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_sync.h"

#define OUT_TRIM 1850               /* output gain, Q12 (-9.0 dB): integrated (a key held 10 s): -17.0 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +2.1 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 60
#define VARY_SPLIT 5

enum { P_GRAIN, P_FLOW, P_SORTING, P_STEADY, P_DETUNE, NP };

static const tx_param_t PARAMS[NP] = {
    {"GRAIN", 127, 40, "grain diameter at C4: 100 .. 400 um (log): f = 0.4 sqrt(g/d) = 125 .. 63 Hz (keys: a semitone each)"},
    {"FLOW", 127, 100, "avalanche speed: 0.05 .. 1 m/s (log): no song below ~0.23 m/s, louder and locked above"},
    {"SORTING", 127, 20, "grain sorting: well sorted (one size: a tone) .. mixed (+-20 %: noise, no song)"},
    {"STEADY", 127, 100, "avalanche: intermittent (coherence ~20 ms) .. steady slide (~100 ms)"},
    {"DETUNE", 127, 10, "two patches singing apart: 0 .. 6 % (near threshold: a beating doublet)"},
};

#define NA 4u
#define VC_L2 (-138790)            /* log2(0.23 m/s): the singing threshold (avalanches) */

typedef struct {
    tx_sync_t sy;
    int32_t flow;                  /* Q15 of the target flow (ramps) */
    uint8_t em, note, alive, vel;
} aval_t;

typedef struct {
    tx_cloud_t cloud;
    tx_noise_t hiss;
    tx_dcblock_t hp;
    aval_t a[NA];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK];
    uint8_t p[NP], limit, quiet;
    int32_t level, grain_l2, flow_l2, spread, jit, detune;
    int32_t lp;                    /* the output low-pass (the sand bed absorbs the highs) */
    uint16_t dropped;
    tx_vary_t vy;
} song_t;

static void set_param(song_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_GRAIN: s->grain_l2 = 435412 + (int32_t)(v * 1032u); break;     /* log2(100 um) + v x 2 oct / 127 */
    case P_FLOW: s->flow_l2 = -283241 + (int32_t)(v * 2230u); break;       /* log2(0.05) + v log2(20) / 127 */
    case P_SORTING: s->spread = 164 + (int32_t)v * 51; break;              /* Q15: 0.5 % .. 20 % */
    case P_STEADY: s->jit = 5243 - (int32_t)v * 37; break;                 /* phase >> 16: 0.08 .. 0.01 cycle / block */
    case P_DETUNE: s->detune = (int32_t)v * 15; break;                     /* Q15: 0 .. 6 % */
    default: break;
    }
}

static void song_init(void *st, uint32_t seed)
{
    song_t *s = (song_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    tx_noise_init(&s->hiss, seed ^ 0x51A9D00Du);
    tx_noise_band(&s->hiss, 900, 11, TX_NOISE_BAND);
    for (i = 0; i < NA; i++) { tx_sync_init(&s->a[i].sy); s->a[i].em = 0xFF; }
    s->limit = 3;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((song_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void song_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((song_t *)st)->vy);
    tx_cloud_key(&((song_t *)st)->cloud, note, vel, on);
}

static void song_param(void *st, uint8_t id, uint8_t val)
{
    song_t *s = (song_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NA ? NA : val);
        for (i = s->limit; i < NA; i++) { s->a[i].alive = 0; s->a[i].em = 0xFF; s->a[i].flow = 0; }   /* rings out */
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* the frequency of a key: f = 0.4 sqrt(g / d) = 1253 Hz / sqrt(d / 1 um), x velocity push (ours, up to +10 %) */
static uint32_t key_hz(song_t *s, unsigned note, unsigned vel)
{
    int32_t d_l2 = s->grain_l2 - ((int32_t)note - 60) * 10923   /* 2^(-2/12) per semitone; */
                 - (tx_vary_tone(&s->vy) >> 2);              /* the press's sand: brighter = finer */
    int32_t f_l2 = 674457 - d_l2 / 2 + (int32_t)vel * 70;        /* log2(1253) = 10.291; vel 127: +0.136 oct */
    return tx_exp2(f_l2);
}

static void avalanche(song_t *s, const tx_event_t *e)
{
    aval_t *a = 0;
    unsigned i;
    uint32_t f;
    for (i = 0; i < s->limit && !a; i++) if (s->a[i].alive && s->a[i].note == e->note) a = &s->a[i];
    for (i = 0; i < s->limit && !a; i++) if (!s->a[i].alive && tx_sync_quiet(&s->a[i].sy)) a = &s->a[i];
    for (i = 0; i < s->limit && !a; i++) if (!s->a[i].alive) a = &s->a[i];
    if (!a) { s->dropped++; return; }
    f = key_hz(s, e->note, e->vel);
    if (!a->alive) {
        tx_sync_emitters(&a->sy, &s->cloud.rng, TX_SYNC_N, f, s->spread);
        tx_sync_res(&a->sy, 0, f, 400);                      /* Q 25 (ours; the references' tone stands 28-43 dB */
        tx_sync_res(&a->sy, 1, (uint32_t)(((uint64_t)f * (uint32_t)(32768 + s->detune)) >> 15), 400);   /* over the rest) */
        {   /* the second patch's grains at the detuned rate */
            unsigned k;
            for (k = 1; k < TX_SYNC_N; k += 2) a->sy.inc[k] = (uint32_t)(((uint64_t)a->sy.inc[k] * (uint32_t)(32768 + s->detune)) >> 15);
        }
    }
    a->alive = 1; a->em = e->emitter; a->note = e->note; a->vel = e->vel;
}

TX_AUDIO_CODE static void song_render(void *st, int32_t *mix, unsigned n)
{
    song_t *s = (song_t *)st;
    unsigned k, i, j;
    int32_t *buf = s->buf, hiss = 0;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) buf[j] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;                              /* silent and nothing new */
    for (i = 0; i < k; i++) avalanche(s, &s->ev[i]);
    for (i = 0; i < NA; i++) {
        aval_t *a = &s->a[i];
        int held;
        if (!a->alive && tx_sync_quiet(&a->sy)) continue;
        held = a->alive && a->em < TX_CLOUD_EMITTERS && s->cloud.em[a->em].held && s->cloud.em[a->em].note == a->note;
        a->flow += held ? (32768 - a->flow) >> 4 : -((a->flow >> 6) + 1);   /* start ~0.1 s; the avalanche slows and
                                                              * stops over ~1.5-2 s (tau 0.37 s; session 5: no hard stop) */
        if (!held && a->flow < 256) { a->flow = 0; a->alive = 0; a->em = 0xFF; }
        {   /* speed v = FLOW x flow ramp: the coupling switches on above the threshold v_c (ours: K ~ (v - v_c) / v_c) */
            int32_t vl2 = s->flow_l2 + ((int32_t)a->vel - 100) * 819;   /* velocity: +-0.5 oct of speed per 40 */
            int32_t over = vl2 - VC_L2, fl = a->flow;
            a->sy.k = over > 0 ? (int32_t)(((int64_t)((over > 131072 ? 131072 : over) >> 8) * fl) >> 15) : 0;   /* <= 512: <= ~0.1 cycle / block */
            a->sy.amp = (int32_t)(((int64_t)tx_exp2(vl2 / 2 + (11 << 16)) * fl) >> 15);    /* ~ sqrt(v): impulses (ours) */
            a->sy.asat = 13107;                              /* 0.8 (Q14): the grains take off */
            a->sy.jit = (uint32_t)s->jit;
            hiss += (int32_t)(((int64_t)tx_exp2(vl2 + (13 << 16)) * fl) >> 15);
        }
        tx_sync_add(&a->sy, &s->cloud.rng, ((s->level * 181) >> 15) + 1, buf, n);   /* <= 1.4 FS per avalanche */
    }
    tx_noise_level(&s->hiss, (int32_t)(((int64_t)(hiss > 65535 ? 65535 : hiss) * s->level) >> 19));   /* -12 dB (references) */
    tx_noise_add(&s->hiss, buf, n);
    {   /* a one-pole low-pass at ~500 Hz: the references fall 6-8 dB / oct above the tone, ours fell 3 */
        int32_t lp = s->lp;
        for (j = 0; j < n; j++) { lp += ((buf[j] - lp) >> 4) * 2254 >> 11; buf[j] = lp; }
        s->lp = (lp > -16 && lp < 16) ? 0 : lp;
    }
    tx_vary_block(&s->vy, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = s->hp.x1 | s->hp.y1 | s->lp | !tx_noise_quiet(&s->hiss);
        for (i = 0; i < NA; i++) z |= s->a[i].alive | !tx_sync_quiet(&s->a[i].sy);
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void song_stats(const void *st, tx_stats_t *o)
{
    const song_t *s = (const song_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < NA; i++) n += s->a[i].alive;
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: four avalanches, fast flow, coupling on, detuned patches */
static const char SCENE_MAX[] =
    "0 particles 4\n0 param FLOW 127\n0 param DETUNE 127\n0 param STEADY 0\n0 param GRAIN 0\n"
    "0 on 53 127\n0 on 60 127\n0 on 67 127\n0 on 79 127\n"
    "3000 off 53\n3000 off 60\n3000 off 67\n3000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_sandsong = {
    "sandsong", "#15 Singing / booming sand", NP, PARAMS, NA, 3, sizeof(song_t), SCENE_MAX,
    song_init, song_key, song_param, song_render, song_stats,
};
