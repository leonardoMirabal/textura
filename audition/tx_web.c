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
#include "tx_core_gen.h"
#include "tx_core_gov.h"
#include "tx_fx.h"

static uint8_t arenas[TX_MIX_SLOTS][2][TX_STATE_MAX] __attribute__((aligned(64)));   /* the four layers, two each: a
                                                                * new engine starts in one while the old fades in the other */
static uint8_t arena2[TX_STATE_MAX] __attribute__((aligned(64)));   /* the reference render (CPU ratio, hash) */
static int ref_dirty;                                   /* arena2 was used for a hash: the CPU reference restarts */
static tx_mix_t mixer;                                  /* S3: four layers -> pan -> stereo -> CHAMBER -> limiter */
static int ed;                                          /* the layer the txw_* parameter / key calls act on */
static const tx_engine_t *cur;                          /* = mixer.e[ed] */
static uint8_t *arena = arenas[0][0];                   /* = mixer.st[ed] */
static int side[TX_MIX_SLOTS];                          /* which of a layer's two arenas holds its engine */
/* a switched-out engine fades out over 4 blocks (23 ms) in its arena, so a switch is click-free; if it is switched
 * again before that, the fading one's last sample decays to zero instead (dc) */
static const tx_engine_t *ret_e[TX_MIX_SLOTS];
static void *ret_st[TX_MIX_SLOTS];
static int32_t ret_g[TX_MIX_SLOTS], ret_last[TX_MIX_SLOTS], dc[TX_MIX_SLOTS], tmp[TX_BLOCK];
static int32_t mix[TX_BLOCK], peak, stats[8], outL[TX_BLOCK], outR[TX_BLOCK];
static float out[TX_BLOCK * 64 * 2];                    /* stereo, interleaved */
static double slot_ms[TX_MIX_SLOTS];                    /* render time per layer since the last txw_slot_time */
static char hex[65];
static tx_chamber_t chamber, chamber2;                 /* the live CHAMBER (after the mix) and the check's */
static int ch_on;
static uint8_t ch_p[TX_CH_NP];
static int lim_db10;                                    /* the output limiter's deepest reduction since the last take */
/* the lanes' generators (tx_core_gen: SOURCE-DESIGN 4f), one per layer: when on, it sets the source's INTENSITY (or,
 * for an old engine, the player reads its activity) and presses its notes into the layer's engine */
static tx_gen_t gens[TX_MIX_SLOTS];
static int gen_on[TX_MIX_SLOTS], gen_ready[TX_MIX_SLOTS], gen_mute[TX_MIX_SLOTS];   /* (mute: no notes, a one-shot lane) */
static int32_t gen_stat[8];
/* the CPU governor across the layers (tx_core_gov): each layer's render time per block, scaled to the FM-1 by the
 * player's calibration (gov_factor: FM-1 us per host us, x 1000), against the sources' budget (55 %: 3193 us) */
static tx_gov_t gov;
static int gov_on = 1, gov_ready;
static uint32_t gov_factor = 0u, gov_cost[TX_MIX_SLOTS], gov_lvl[TX_MIX_SLOTS];
static void gov_init(void) { if (!gov_ready) { tx_gov_init(&gov, TX_MIX_SLOTS, 3193u); gov_ready = 1; } }

static tx_fx_t fxc;                                     /* the effects: each layer's DRIVE + FILTER, the REVERB */
static void ready(void)                                 /* the mixer, once, before any call that uses it */
{
    static int r;
    if (!r) {
        tx_mix_init(&mixer); mixer.ch = &chamber;
        tx_fx_init(&fxc); mixer.ins = tx_fx_lane; mixer.bus = tx_fx_bus; mixer.busq = tx_fx_bus_quiet; mixer.fx = &fxc;
        r = 1;
    }
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
    ed = k; cur = mixer.e[k]; arena = arenas[k][side[k]];
}
/* the engine of the edited layer: i (an engine index), or -1 = the layer off */
EMSCRIPTEN_KEEPALIVE int txw_select(int i, unsigned seed)
{
    ready();
    if (i >= 0 && !eng(i)) return -1;
    if (mixer.e[ed]) {                                       /* the old engine fades out in its arena */
        if (ret_g[ed]) dc[ed] += ret_last[ed];               /* one still fading: its last sample decays instead */
        ret_e[ed] = mixer.e[ed]; ret_st[ed] = mixer.st[ed]; ret_g[ed] = 32768;
        side[ed] ^= 1;
    }
    if (i < 0) { tx_mix_set(&mixer, (unsigned)ed, 0, 0, 0); cur = 0; return 0; }
    memset(arenas[ed][side[ed]], 0, TX_STATE_MAX);
    tx_mix_set(&mixer, (unsigned)ed, eng(i), arenas[ed][side[ed]], seed);
    cur = eng(i); arena = arenas[ed][side[ed]];
    gov_init(); gov.cur[ed] = 0; tx_gov_user(&gov, (unsigned)ed, eng(i)->particles_def);
    return 0;
}
EMSCRIPTEN_KEEPALIVE void txw_pan(int k, int v) { ready(); tx_mix_pan(&mixer, (unsigned)k, (unsigned)(v < 0 ? 0 : v)); }
EMSCRIPTEN_KEEPALIVE void txw_key_slot(int k, int note, int vel, int on)
{
    if (k < 0 || k >= (int)TX_MIX_SLOTS || !mixer.e[k] || note < 0 || note > 127) return;
    mixer.e[k]->key(mixer.st[k], (uint8_t)note, (uint8_t)vel, (uint8_t)(on != 0));
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

/* ---- the dev view's hosted block: the edited layer's source, the engine it hosts now and that engine's own parameters,
 * set directly (an override: it wins over the source's mapping until the lane's sound changes; v < 0 releases it) */
extern const tx_engine_t tx_eng_source;
const tx_engine_t *tx_source_hosted(const void *st);
int tx_source_host_value(const void *st, unsigned id);
void tx_source_host_param(void *st, unsigned id, unsigned val);
int tx_source_host_over(const void *st, unsigned id);
static const tx_engine_t *hosted(void) { return cur == &tx_eng_source ? tx_source_hosted(arena) : 0; }
EMSCRIPTEN_KEEPALIVE const char *txw_host_name(void) { return hosted() ? hosted()->name : ""; }
EMSCRIPTEN_KEEPALIVE int txw_host_n(void) { return hosted() ? hosted()->n_params : 0; }
EMSCRIPTEN_KEEPALIVE const char *txw_host_pname(int id) { return hosted() && id >= 0 && id < hosted()->n_params ? hosted()->params[id].name : ""; }
EMSCRIPTEN_KEEPALIVE const char *txw_host_phelp(int id) { return hosted() && id >= 0 && id < hosted()->n_params ? hosted()->params[id].help : ""; }
EMSCRIPTEN_KEEPALIVE int txw_host_pmax(int id) { return hosted() && id >= 0 && id < hosted()->n_params ? hosted()->params[id].max : 0; }
EMSCRIPTEN_KEEPALIVE int txw_host_value(int id)          /* the value now (the source's, or the override) */
{
    int v;
    if (!hosted() || id < 0 || id >= hosted()->n_params) return -1;
    v = tx_source_host_value(arena, (unsigned)id);
    return v >= 0 ? v : hosted()->params[id].def;
}
EMSCRIPTEN_KEEPALIVE int txw_host_over(int id) { return hosted() && id >= 0 ? tx_source_host_over(arena, (unsigned)id) : 0; }
EMSCRIPTEN_KEEPALIVE void txw_host_param(int id, int v)
{
    if (hosted() && id >= 0) tx_source_host_param(arena, (unsigned)id, v < 0 ? 256u : (unsigned)v);
}

/* id >= 0: the engine's own parameter; -1 PARTICLES, -2 LEVEL */
EMSCRIPTEN_KEEPALIVE void txw_param(int id, int v)
{
    if (!cur) return;
    cur->param(arena, (uint8_t)(id == -1 ? TX_PID_PARTICLES : id == -2 ? TX_PID_LEVEL : id), (uint8_t)v);
    if (id == -1) { gov_init(); gov.cur[ed] = 0; tx_gov_user(&gov, (unsigned)ed, (unsigned)v); }
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
            if (!mixer.e[k] && !ret_g[k] && !dc[k]) continue;
            t0 = emscripten_get_now();
            if (gen_on[k] && mixer.e[k]) {                   /* the lane's generator: its activity and notes */
                tx_gen_ev_t ev[TX_GEN_EV_MAX];
                uint8_t in;
                unsigned ne = tx_gen_block(&gens[k], TX_BLOCK, ev, TX_GEN_EV_MAX, &in), q;
                int ip = tx_param_find(mixer.e[k], "ACTIVITY");
                if (ip >= 0 && !strcmp(mixer.e[k]->name, "source")) mixer.e[k]->param(mixer.st[k], (uint8_t)ip, in);
                for (q = 0; q < ne && !gen_mute[k]; q++) mixer.e[k]->key(mixer.st[k], ev[q].note, ev[q].vel, ev[q].vel != 0);
            }
            tx_mix_slot_render(&mixer, k, TX_BLOCK);
            {   /* the governor's cost: this layer's block on the FM-1 (us), less its inserts (DRIVE, FILTER: given
                 * apart, as the firmware's lanes.c: no hero buys them back) */
                double us = (emscripten_get_now() - t0) * 1000.0 * gov_factor / 1000.0;
                uint32_t f = tx_fx_lane_us(&fxc, (unsigned)k);
                gov_cost[k] = us > 1e6 ? 1000000u : (uint32_t)us;
                gov_cost[k] = gov_cost[k] > f ? gov_cost[k] - f : 0u;
                gov_lvl[k] = (uint32_t)mixer.slot_peak[k];
                gov_init();
                tx_gov_fixed(&gov, (unsigned)k, mixer.e[k] ? f : 0u);
            }
            if (ret_g[k] || dc[k]) {                         /* the switched-out engine's fade, the decaying step */
                int32_t g = ret_g[k], d = dc[k], last = 0;
                for (j = 0; j < (int)TX_BLOCK; j++) tmp[j] = 0;
                if (g) ret_e[k]->render(ret_st[k], tmp, TX_BLOCK);
                for (j = 0; j < (int)TX_BLOCK; j++) {
                    int32_t gj = g - (j * 8192) / (int)TX_BLOCK;   /* 4 blocks from 1 to 0 */
                    last = (int32_t)(((int64_t)tmp[j] * gj) >> 15);
                    tmp[j] = last + d;
                    d -= d >> 6;
                    if (d > -64 && d < 64) d = 0;
                }
                ret_last[k] = g > 8192 ? last : 0;
                ret_g[k] = g > 8192 ? g - 8192 : 0;
                dc[k] = d;
                tx_mix_add(&mixer, k, tmp, TX_BLOCK);
            }
            slot_ms[k] += emscripten_get_now() - t0;
        }
        if (gov_on && gov_factor) {                          /* the governor (once calibrated): the quietest lane first */
            uint8_t on[TX_MIX_SLOTS];
            int pk;
            gov_init();
            for (k = 0; k < TX_MIX_SLOTS; k++) on[k] = mixer.e[k] != 0;
            pk = tx_gov_block(&gov, gov_cost, gov_lvl, on);
            if (pk >= 0 && mixer.e[pk]) mixer.e[pk]->param(mixer.st[pk], TX_PID_PARTICLES, gov.cur[pk]);
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
    if (ref_dirty) { blk = 0; ref_dirty = 0; }               /* (a hash render used arena2 in between) */
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
    ref_dirty = 1;
    tx_sha256_init(&h);
    e->init(arena2, s.seed);
    for (b = 0; b < s.blocks; b++) { tx_script_step(e, arena2, &s, b, &next, mix); tx_sha256_q23(&h, mix, TX_BLOCK); }
    tx_sha256_hex(&h, hex);
    return hex;
}

/* ---- EFFECTS (tx_fx.h): group 0 DRIVE and 1 FILTER (each layer's own, on its signal before the pan), 2 REVERB (on the
 * mix; its TYPE ROOM is the CHAMBER's body + a short hall), 3 GRAIN DELAY (on the mix, before the reverb) */
static const tx_param_t *fx_list(int g, int *n)
{
    if (g == 0) { *n = TX_DR_NP; return tx_fx_drive_params; }
    if (g == 1) { *n = TX_FL_NP; return tx_fx_filter_params; }
    if (g == 2) { *n = TX_RV_NP; return tx_fx_reverb_params; }
    if (g == 3) { *n = TX_GD_NP; return tx_fx_grain_params; }
    *n = 0; return 0;
}
EMSCRIPTEN_KEEPALIVE int txw_fx_n(int g) { int n; fx_list(g, &n); return n; }
EMSCRIPTEN_KEEPALIVE const char *txw_fx_pname(int g, int i) { int n; const tx_param_t *p = fx_list(g, &n); return i >= 0 && i < n ? p[i].name : ""; }
EMSCRIPTEN_KEEPALIVE const char *txw_fx_phelp(int g, int i) { int n; const tx_param_t *p = fx_list(g, &n); return i >= 0 && i < n ? p[i].help : ""; }
EMSCRIPTEN_KEEPALIVE int txw_fx_pmax(int g, int i) { int n; const tx_param_t *p = fx_list(g, &n); return i >= 0 && i < n ? p[i].max : 0; }
EMSCRIPTEN_KEEPALIVE int txw_fx_pdef(int g, int i) { int n; const tx_param_t *p = fx_list(g, &n); return i >= 0 && i < n ? p[i].def : 0; }
/* group g's parameter i to v (k: the layer, for DRIVE / FILTER) */
EMSCRIPTEN_KEEPALIVE void txw_fx_param(int g, int k, int i, int v)
{
    ready();
    if (v < 0) v = 0;
    if (g == 0 && k >= 0 && k < (int)TX_FX_LANES) tx_fx_drive_param(&fxc.drive[k], (unsigned)i, (unsigned)v);
    else if (g == 1 && k >= 0 && k < (int)TX_FX_LANES) tx_fx_filter_param(&fxc.filter[k], (unsigned)i, (unsigned)v);
    else if (g == 2) tx_fx_reverb_param(&fxc.rev, (unsigned)i, (unsigned)v);
    else if (g == 3) tx_fx_grain_param(&fxc.gd, (unsigned)i, (unsigned)v);
}
EMSCRIPTEN_KEEPALIVE int txw_fx_frozen(void) { ready(); return tx_fx_reverb_frozen(&fxc.rev); }
EMSCRIPTEN_KEEPALIVE int txw_fx_running(void) { ready(); return !tx_fx_reverb_quiet(&fxc.rev); }
EMSCRIPTEN_KEEPALIVE int txw_fx_grains(void) { ready(); return fxc.gd.on ? fxc.gd.act : -1; }   /* sounding (-1: off) */
EMSCRIPTEN_KEEPALIVE int txw_fx_cap(void) { ready(); return fxc.gd.cap; }

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
    ref_dirty = 1;
    tx_sha256_init(&h);
    e->init(arena2, s.seed);
    tx_chamber_init(&chamber2);
    for (b = 0; b < s.blocks; b++) { tx_script_step_ch(e, arena2, &chamber2, &s, b, &next, mix); tx_sha256_q23(&h, mix, TX_BLOCK); }
    tx_sha256_hex(&h, hex);
    return hex;
}

/* ---- the lanes' generators: on / off (off releases its notes), parameters, a live readout */
EMSCRIPTEN_KEEPALIVE void txw_gen_on(int k, int on, unsigned seed)
{
    if (k < 0 || k >= (int)TX_MIX_SLOTS) return;
    if (!gen_ready[k]) { tx_gen_init(&gens[k], seed ? seed : 1u); gen_ready[k] = 1; }
    else if (on && !gen_on[k] && seed) tx_rng_seed(&gens[k].rng, seed ^ 0x6E4E4E21u);   /* each start a new walk */
    if (!on && gen_on[k] && mixer.e[k]) {
        tx_gen_ev_t ev[TX_GEN_OFFS];
        unsigned ne = tx_gen_stop(&gens[k], ev, TX_GEN_OFFS), q;
        for (q = 0; q < ne; q++) mixer.e[k]->key(mixer.st[k], ev[q].note, 0, 0);
    }
    gen_on[k] = on != 0;
}
EMSCRIPTEN_KEEPALIVE void txw_gen_mute(int k, int m) { if (k >= 0 && k < (int)TX_MIX_SLOTS) gen_mute[k] = m != 0; }
EMSCRIPTEN_KEEPALIVE int txw_gen_n(void) { return TX_GEN_NP; }
EMSCRIPTEN_KEEPALIVE const char *txw_gen_pname(int i) { return i >= 0 && i < (int)TX_GEN_NP ? tx_gen_params[i].name : ""; }
EMSCRIPTEN_KEEPALIVE const char *txw_gen_phelp(int i) { return i >= 0 && i < (int)TX_GEN_NP ? tx_gen_params[i].help : ""; }
EMSCRIPTEN_KEEPALIVE int txw_gen_pmax(int i) { return i >= 0 && i < (int)TX_GEN_NP ? tx_gen_params[i].max : 0; }
EMSCRIPTEN_KEEPALIVE int txw_gen_pdef(int i) { return i >= 0 && i < (int)TX_GEN_NP ? tx_gen_params[i].def : 0; }
EMSCRIPTEN_KEEPALIVE void txw_gen_param(int k, int i, int v)
{
    if (k < 0 || k >= (int)TX_MIX_SLOTS) return;
    if (!gen_ready[k]) { tx_gen_init(&gens[k], 1u + (unsigned)k); gen_ready[k] = 1; }
    tx_gen_param(&gens[k], (unsigned)i, (unsigned)(v < 0 ? 0 : v));
}
/* activity now (knob), pitch centre (semitones x 10), notes since start, recall weight (x 1000), on */
EMSCRIPTEN_KEEPALIVE int32_t *txw_gen_state(int k)
{
    if (k < 0 || k >= (int)TX_MIX_SLOTS) return gen_stat;
    gen_stat[0] = gens[k].a >> 8; gen_stat[1] = (gens[k].pc * 10) >> 8; gen_stat[2] = (int32_t)gens[k].notes;
    gen_stat[3] = (gens[k].g * 1000) >> 15; gen_stat[4] = gen_on[k];
    return gen_stat;
}

/* ---- the CPU governor: on / off, its budget (us per block), the calibration (FM-1 us per host us x 1000), a layer's
 * heroes now (PARTICLES as the governor allows) */
EMSCRIPTEN_KEEPALIVE void txw_gov_on(int on)
{
    unsigned k;
    gov_init();
    gov_on = on != 0;
    if (!gov_on) for (k = 0; k < TX_MIX_SLOTS; k++)          /* off: every layer back to its own setting */
        if (mixer.e[k] && gov.user[k] && gov.cur[k] != gov.user[k]) { gov.cur[k] = gov.user[k]; mixer.e[k]->param(mixer.st[k], TX_PID_PARTICLES, gov.user[k]); }
}
EMSCRIPTEN_KEEPALIVE void txw_gov_budget(int us) { gov_init(); gov.budget = (uint32_t)(us < 1 ? 1 : us); }
EMSCRIPTEN_KEEPALIVE void txw_gov_factor(int x1000) { if (x1000 > 0) gov_factor = (uint32_t)x1000; }
EMSCRIPTEN_KEEPALIVE int txw_gov_cur(int k) { gov_init(); return k >= 0 && k < (int)TX_MIX_SLOTS ? gov.cur[k] : 0; }
EMSCRIPTEN_KEEPALIVE int txw_gov_user(int k) { gov_init(); return k >= 0 && k < (int)TX_MIX_SLOTS ? gov.user[k] : 0; }
