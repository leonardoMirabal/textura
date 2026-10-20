/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura player: the engines compiled to WebAssembly; app.js calls these (one live instance, mono, 44.1 kHz). */
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>
#include "tx_engine.h"
#include "tx_host.h"
#include "tx_script.h"
#include <math.h>
#include "tx_core_chamber.h"
#include "tx_core_limit.h"
#include "tx_core_mix.h"

static uint8_t arenas[TX_MIX_SLOTS][TX_STATE_MAX] __attribute__((aligned(64)));   /* the four layers */
static uint8_t arena2[TX_STATE_MAX] __attribute__((aligned(64)));   /* the reference render (CPU ratio, hash) */
static tx_mix_t mixer;                                  /* S3: four layers -> pan -> stereo -> CHAMBER -> limiter */
static int ed;                                          /* the layer the txw_* parameter / key calls act on */
static const tx_engine_t *cur;                          /* = mixer.e[ed] */
static uint8_t *arena = arenas[0];                      /* = arenas[ed] */
static int32_t mix[TX_BLOCK], peak, stats[8], outL[TX_BLOCK], outR[TX_BLOCK];
static float out[TX_BLOCK * 64 * 2];                    /* stereo, interleaved */
static double slot_ms[TX_MIX_SLOTS];                    /* render time per layer since the last txw_slot_time */
static char hex[65];
static tx_chamber_t chamber, chamber2;                 /* the live CHAMBER (after the mix) and the check's */
static int ch_on;
static uint8_t ch_p[TX_CH_NP];
static int lim_db10;                                    /* the output limiter's deepest reduction since the last take */

static void ready(void)                                 /* the mixer, once, before any call that uses it */
{
    static int r;
    if (!r) { tx_mix_init(&mixer); mixer.ch = &chamber; r = 1; }
}

static const tx_engine_t *eng(int i)
{
    int n = 0;
    while (tx_engines[n]) n++;
    return i >= 0 && i < n ? tx_engines[i] : 0;
}

EMSCRIPTEN_KEEPALIVE int txw_count(void) { int n = 0; while (tx_engines[n]) n++; return n; }
EMSCRIPTEN_KEEPALIVE const char *txw_name(int i) { return eng(i) ? eng(i)->name : ""; }
EMSCRIPTEN_KEEPALIVE const char *txw_title(int i) { return eng(i) ? eng(i)->title : ""; }
EMSCRIPTEN_KEEPALIVE int txw_state_bytes(int i) { return eng(i) ? (int)eng(i)->state_bytes : 0; }

/* ---- layers: which one the parameter / key calls act on; its engine; its pan; keys to any layer */
EMSCRIPTEN_KEEPALIVE int txw_slots(void) { return (int)TX_MIX_SLOTS; }
EMSCRIPTEN_KEEPALIVE void txw_slot(int k)
{
    if (k < 0 || k >= (int)TX_MIX_SLOTS) return;
    ready();
    ed = k; cur = mixer.e[k]; arena = arenas[k];
}
/* the engine of the edited layer: i (an engine index), or -1 = the layer off */
EMSCRIPTEN_KEEPALIVE int txw_select(int i, unsigned seed)
{
    ready();
    if (i < 0) { tx_mix_set(&mixer, (unsigned)ed, 0, 0, 0); cur = 0; return 0; }
    if (!eng(i)) return -1;
    memset(arenas[ed], 0, TX_STATE_MAX);
    tx_mix_set(&mixer, (unsigned)ed, eng(i), arenas[ed], seed);
    cur = eng(i); arena = arenas[ed];
    peak = 0;
    tx_limit_init(&mixer.lim);
    return 0;
}
EMSCRIPTEN_KEEPALIVE void txw_pan(int k, int v) { ready(); tx_mix_pan(&mixer, (unsigned)k, (unsigned)(v < 0 ? 0 : v)); }
EMSCRIPTEN_KEEPALIVE void txw_key_slot(int k, int note, int vel, int on)
{
    if (k < 0 || k >= (int)TX_MIX_SLOTS || !mixer.e[k] || note < 0 || note > 127) return;
    mixer.e[k]->key(arenas[k], (uint8_t)note, (uint8_t)vel, (uint8_t)(on != 0));
}
/* a layer's render time since the last call, us x 10 (and reset): the CPU meter's split */
EMSCRIPTEN_KEEPALIVE int txw_slot_time(int k)
{
    int t;
    if (k < 0 || k >= (int)TX_MIX_SLOTS) return 0;
    t = (int)(slot_ms[k] * 10000.0);
    slot_ms[k] = 0;
    return t;
}
/* a layer's peak since the last call, dBFS x 10 (-1200 = silent) */
EMSCRIPTEN_KEEPALIVE int txw_slot_peak(int k)
{
    int32_t p;
    if (k < 0 || k >= (int)TX_MIX_SLOTS) return -1200;
    p = mixer.slot_peak[k]; mixer.slot_peak[k] = 0;
    if (!p) return -1200;
    return (int)(200.0 * log10((double)p / 8388608.0));
}

EMSCRIPTEN_KEEPALIVE int txw_nparams(void) { return cur ? cur->n_params : 0; }
EMSCRIPTEN_KEEPALIVE const char *txw_pname(int id) { return cur && id < cur->n_params ? cur->params[id].name : ""; }
EMSCRIPTEN_KEEPALIVE const char *txw_phelp(int id) { return cur && id < cur->n_params ? cur->params[id].help : ""; }
EMSCRIPTEN_KEEPALIVE int txw_pmax(int id) { return cur && id < cur->n_params ? cur->params[id].max : 0; }
EMSCRIPTEN_KEEPALIVE int txw_pdef(int id) { return cur && id < cur->n_params ? cur->params[id].def : 0; }
EMSCRIPTEN_KEEPALIVE int txw_particles_max(void) { return cur ? cur->particles_max : 0; }
EMSCRIPTEN_KEEPALIVE int txw_particles_def(void) { return cur ? cur->particles_def : 0; }

/* id >= 0: the engine's own parameter; -1 PARTICLES, -2 LEVEL */
EMSCRIPTEN_KEEPALIVE void txw_param(int id, int v)
{
    if (!cur) return;
    cur->param(arena, (uint8_t)(id == -1 ? TX_PID_PARTICLES : id == -2 ? TX_PID_LEVEL : id), (uint8_t)v);
}

EMSCRIPTEN_KEEPALIVE void txw_key(int note, int vel, int on)
{
    if (cur && note >= 0 && note < 128) cur->key(arena, (uint8_t)note, (uint8_t)vel, (uint8_t)(on != 0));
}

/* render blocks x 256 frames of the four layers -> stereo floats in [-1, 1], interleaved L R (Q23 / 2^23) */
EMSCRIPTEN_KEEPALIVE float *txw_render(int blocks)
{
    int b, j;
    unsigned k;
    ready();
    if (blocks > 64) blocks = 64;
    for (b = 0; b < blocks; b++) {
        float *o = out + 2 * b * TX_BLOCK;
        tx_mix_begin(&mixer, TX_BLOCK);
        for (k = 0; k < TX_MIX_SLOTS; k++) {
            double t0;
            if (!mixer.e[k]) continue;
            t0 = emscripten_get_now();
            tx_mix_slot_render(&mixer, k, TX_BLOCK);
            slot_ms[k] += emscripten_get_now() - t0;
        }
        mixer.ch_on = ch_on;
        tx_mix_end(&mixer, outL, outR, TX_BLOCK);
        if (mixer.raw_peak > peak) peak = mixer.raw_peak;    /* the meter reads the mix before the limiter */
        {
            int g = (int)tx_limit_take(&mixer.lim);
            if (g > lim_db10) lim_db10 = g;
        }
        for (j = 0; j < (int)TX_BLOCK; j++) {
            o[2 * j] = (float)outL[j] * (1.0f / 8388608.0f);
            o[2 * j + 1] = (float)outR[j] * (1.0f / 8388608.0f);
        }
    }
    return out;
}

EMSCRIPTEN_KEEPALIVE int txw_peak_take(void) { int p = peak; peak = 0; return p; }

/* the output limiter's deepest gain reduction since the last call, dB x 10 (0 = it did not act) */
EMSCRIPTEN_KEEPALIVE int txw_lim_take(void) { int g = lim_db10; lim_db10 = 0; return g; }

/* particles, limit, events (last block), dropped (last block), steals, total events, quiet */
EMSCRIPTEN_KEEPALIVE int32_t *txw_stats(void)
{
    tx_stats_t s;
    memset(&s, 0, sizeof s);
    if (cur) cur->stats(arena, &s);
    stats[0] = s.particles; stats[1] = s.limit; stats[2] = s.events; stats[3] = s.dropped;
    stats[4] = (int32_t)s.steals; stats[5] = (int32_t)s.total; stats[6] = s.quiet;
    return stats;
}

/* the reference render for the CPU estimate: engine 0's worst scene at its default PARTICLES, as txbench's
 * calib.json reference (app.js times it). Returns the blocks rendered. */
static tx_script_t ref;
EMSCRIPTEN_KEEPALIVE int txw_ref(void)
{
    const tx_engine_t *e = tx_engines[0];
    unsigned next = 0, i;
    uint32_t b;
    static int parsed;
    if (!parsed) {
        if (tx_script_parse(&ref, e, e->scene_max)) return 0;
        for (i = 0; i < ref.n; i++)
            if (ref.ev[i].kind == TX_SEV_PARAM && ref.ev[i].id == TX_PID_PARTICLES) ref.ev[i].a = e->particles_def;
        parsed = 1;
    }
    e->init(arena2, ref.seed);
    e->param(arena2, TX_PID_PARTICLES, e->particles_def);
    for (b = 0; b < ref.blocks; b++) tx_script_step(e, arena2, &ref, b, &next, mix);
    return (int)ref.blocks;
}

/* one block of the same reference scene, cycling: app.js times it next to the live engine in every audio callback,
 * so both are measured at the same clock (a computer slows down between callbacks) */
EMSCRIPTEN_KEEPALIVE void txw_ref_step(void)
{
    static uint32_t blk;
    static unsigned next;
    const tx_engine_t *e = tx_engines[0];
    if (!ref.blocks) { txw_ref(); blk = 0; }
    if (blk == 0) { e->init(arena2, ref.seed); e->param(arena2, TX_PID_PARTICLES, e->particles_def); next = 0; }
    tx_script_step(e, arena2, &ref, blk, &next, mix);
    if (++blk >= ref.blocks) blk = 0;
}

/* sha256 of an engine's worst scene ("max"), to compare with the native txrender / txtest baseline */
EMSCRIPTEN_KEEPALIVE const char *txw_max_sha(int i)
{
    const tx_engine_t *e = eng(i);
    static tx_script_t s;
    tx_sha256_t h;
    unsigned next = 0;
    uint32_t b;
    if (!e || tx_script_parse(&s, e, e->scene_max)) return "";
    tx_sha256_init(&h);
    e->init(arena2, s.seed);
    for (b = 0; b < s.blocks; b++) { tx_script_step(e, arena2, &s, b, &next, mix); tx_sha256_q23(&h, mix, TX_BLOCK); }
    tx_sha256_hex(&h, hex);
    return hex;
}

/* ---- CHAMBER: its own parameters, on / off (off: not computed; on: starts silent with the current knobs) */
EMSCRIPTEN_KEEPALIVE int txw_ch_n(void) { return TX_CH_NP; }
EMSCRIPTEN_KEEPALIVE const char *txw_ch_pname(int i) { return i >= 0 && i < (int)TX_CH_NP ? tx_chamber_params[i].name : ""; }
EMSCRIPTEN_KEEPALIVE const char *txw_ch_phelp(int i) { return i >= 0 && i < (int)TX_CH_NP ? tx_chamber_params[i].help : ""; }
EMSCRIPTEN_KEEPALIVE int txw_ch_pmax(int i) { return i >= 0 && i < (int)TX_CH_NP ? tx_chamber_params[i].max : 0; }
EMSCRIPTEN_KEEPALIVE int txw_ch_pdef(int i) { return i >= 0 && i < (int)TX_CH_NP ? tx_chamber_params[i].def : 0; }
EMSCRIPTEN_KEEPALIVE void txw_ch_param(int i, int v)
{
    if (i < 0 || i >= (int)TX_CH_NP) return;
    ch_p[i] = (uint8_t)v;
    tx_chamber_param(&chamber, (unsigned)i, (unsigned)v);
}
EMSCRIPTEN_KEEPALIVE void txw_ch_on(int on)
{
    unsigned i;
    if (on && !ch_on) {
        ready();
        tx_chamber_init(&chamber);
        for (i = 0; i < TX_CH_NP; i++) tx_chamber_param(&chamber, i, ch_p[i]);
    }
    ch_on = on != 0;
}
/* sha256 of an engine's max scene through the CHAMBER at its defaults (txtest: chamber+<engine>-max) */
EMSCRIPTEN_KEEPALIVE const char *txw_ch_max_sha(int i)
{
    const tx_engine_t *e = eng(i);
    static tx_script_t s;
    tx_sha256_t h;
    unsigned next = 0;
    uint32_t b;
    if (!e || tx_script_parse(&s, e, e->scene_max)) return "";
    tx_sha256_init(&h);
    e->init(arena2, s.seed);
    tx_chamber_init(&chamber2);
    for (b = 0; b < s.blocks; b++) { tx_script_step_ch(e, arena2, &chamber2, &s, b, &next, mix); tx_sha256_q23(&h, mix, TX_BLOCK); }
    tx_sha256_hex(&h, hex);
    return hex;
}
