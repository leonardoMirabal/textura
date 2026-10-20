/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura player: the engines compiled to WebAssembly; app.js calls these (one live instance, mono, 44.1 kHz). */
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>
#include "tx_engine.h"
#include "tx_host.h"
#include "tx_script.h"
#include "tx_core_chamber.h"

static uint8_t arena[TX_STATE_MAX] __attribute__((aligned(64)));    /* the live instance */
static uint8_t arena2[TX_STATE_MAX] __attribute__((aligned(64)));   /* the reference render (CPU ratio, hash) */
static const tx_engine_t *cur;
static int32_t mix[TX_BLOCK], peak, stats[8];
static float out[TX_BLOCK * 64];
static char hex[65];
static tx_chamber_t chamber, chamber2;                 /* the live CHAMBER (after the engine) and the check's */
static int ch_on;
static uint8_t ch_p[TX_CH_NP];

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

EMSCRIPTEN_KEEPALIVE int txw_select(int i, unsigned seed)
{
    if (!eng(i)) return -1;
    cur = eng(i);
    memset(arena, 0, sizeof arena);
    cur->init(arena, seed);
    peak = 0;
    return 0;
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

/* render blocks x 256 frames -> floats in [-1, 1] (Q23 / 2^23, clamped) */
EMSCRIPTEN_KEEPALIVE float *txw_render(int blocks)
{
    int b, j;
    if (blocks > 64) blocks = 64;
    for (b = 0; b < blocks; b++) {
        float *o = out + b * TX_BLOCK;
        for (j = 0; j < (int)TX_BLOCK; j++) mix[j] = 0;
        if (cur) cur->render(arena, mix, TX_BLOCK);
        if (ch_on) tx_chamber_process(&chamber, mix, TX_BLOCK);
        for (j = 0; j < (int)TX_BLOCK; j++) {
            int32_t v = mix[j], a = v < 0 ? -v : v;
            if (a > peak) peak = a;
            if (v > TX_Q23) v = TX_Q23;
            if (v < -TX_Q23) v = -TX_Q23;
            o[j] = (float)v * (1.0f / 8388608.0f);
        }
    }
    return out;
}

EMSCRIPTEN_KEEPALIVE int txw_peak_take(void) { int p = peak; peak = 0; return p; }

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
