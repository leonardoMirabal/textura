/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura lanes: the sound behind the panel. Four lanes, each an engine instance (engines/: the same code the bench
 * and the audition player run), each through its own DRIVE and FILTER, panned into a stereo pair, then the GRAIN
 * DELAY, the REVERB and the output limiter (tx_core_mix.h, tx_fx.h): the audition player's chain (audition/tx_web.c).
 *
 *   main loop   ln_select (a lane's engine: prepared in the spare arena, here, never in the audio interrupt),
 *               ln_key, ln_param, ln_level, ln_pan, ln_fx, ln_gen_on, ln_gen_param, ln_gen_mute: queued (ln_q), the
 *               interrupt takes them at its next block
 *   interrupt   ln_block: the queue, then per lane its generator (tx_core_gen: the lane playing on its own, its
 *               activity and notes, as the player's) and one block of it, the effects, Q23 stereo; then the CPU
 *               governor (tx_core_gov.h) when the app gives it the lanes' costs (LN_CLOCK or LN_COST_EST, below)
 *
 * A switched-out engine fades over 4 blocks (23 ms) in its own arena while the new one starts in the spare, as the
 * player does, so a switch never clicks. One arena a lane and one spare for all of them (the player keeps two a lane):
 * the faded-out engine's arena becomes the spare, so while one lane's switch fades another sounding lane's waits
 * (ln_select: -1, call again; a lane silent of its engine needs no spare). This file knows nothing of the FM-1:
 * tests/lanes_test.c runs it beside the player's chain and checks they render the same samples. Integers only (as
 * engines/). */
#include "tx_engine.h"
#include "tx_core_mix.h"
#include "tx_fx.h"
#include "tx_core_gen.h"
#include "tx_core_gov.h"

#define LN_N TX_MIX_SLOTS                       /* lanes */
#define LN_QN 128u                              /* queued events (a power of two) */
#define LN_GOV_POOL_US TX_GOV_POOL_DEVICE       /* the sources' CPU pool on the FM-1 (CLAUDE.md: ~2500 us a block) */
/* The governor's costs, FM-1 us a block, from the app:
 *   LN_CLOCK(), LN_CLOCK_US(d)   the device: its free-running clock (wrapping) and a difference of it in us; each
 *                                lane's block is timed (less its inserts, which the governor is given apart) and so
 *                                is the modulation
 *   LN_COST_EST                  the host and the web (no FM-1 clock): a stand-in from the heroes sounding, the same
 *                                every run, so the tests can rely on it
 * Neither (tests/lanes_test.c): the governor never acts, as the player's before its calibration.
 * With LN_CLOCK and LN_ISR_US() (the last block's whole interrupt, us) the grain delay's cap is measured too (ln_bus):
 * its room is TX_GOV_ISR_TARGET less this block's interrupt so far (the lanes, timed as they ran) and the rest as
 * measured in the blocks before (the reverb, the limiter, the platform's part), each held at its peak, falling 1/16 a
 * block (a burst in the lanes holds the grains down for a while instead of stopping and starting them). Elsewhere
 * the cap stays the model's (tx_fx_gd_cap: the effects reserve less the reverb's TYPE). */
#if defined(LN_CLOCK) || defined(LN_COST_EST)
#define LN_GOV
#endif
#define LN_EST_BASE 100u                        /* the stand-in: a lane's block, us, and each hero sounding more */
#define LN_EST_HERO 20u                         /* (make tx-budget: 120..250 us typical, 5..60 a hero) */
#define LN_EST_MOD 8u                           /* the modulation's stand-in, us (make tx-budget section 5: 4..30) */

/* LN_POOL: where the lanes' big state goes. On the FM-1 (textura.c) the platform's .pool (zeroed at start), as it is
 * far over the 96 KiB of .data + .bss; on the host (Mach-O has no such section) and in the tests, plain statics */
#ifndef LN_POOL
#define LN_POOL
#endif

enum { LN_EV_KEY, LN_EV_PARAM, LN_EV_PAN, LN_EV_SWAP, LN_EV_OFF, LN_EV_FX, LN_EV_GEN_ON, LN_EV_GEN_PARAM,
       LN_EV_GEN_MUTE, LN_EV_HOST, LN_EV_GSYNC, LN_EV_TRIM };

/* the source's hosted engine (tx_eng_source.c): its own parameters, set over the source's curves until the sound
 * (PRESET or TYPE) changes */
const tx_engine_t *tx_source_hosted(const void *st);
int tx_source_host_value(const void *st, unsigned id);
void tx_source_host_param(void *st, unsigned id, unsigned val);
enum { LN_FX_DRIVE, LN_FX_FILTER, LN_FX_REVERB, LN_FX_GRAIN, LN_FX_RELATCH };   /* ln_fx groups (as the player's
                                                 * txw_fx_param; RELATCH: a whole sound loaded, tx_fx_reverb_relatch) */
typedef struct { uint8_t kind, lane, a, b, c; } ln_ev_t;

static struct {
    tx_mix_t mix;
    tx_fx_t fx;
    uint8_t ar[LN_N];                           /* the arena (ln_arena) holding the lane's engine */
    uint8_t spare;                              /* the free arena (taken by a switch, given back by its fade) */
    volatile uint8_t spare_busy;                /* a switch holds the spare (main loop sets it, its fade clears it) */
    const tx_engine_t *next_e[LN_N];            /* prepared by ln_select, taken by LN_EV_SWAP */
    const tx_engine_t *ret_e[LN_N];             /* the switched-out engine, fading */
    void *ret_st[LN_N];
    int32_t ret_g[LN_N], ret_last[LN_N], dc[LN_N];
    volatile uint8_t busy[LN_N];                /* a switch is under way (main loop: wait before the next) */
    ln_ev_t q[LN_QN];
    volatile uint32_t qw, qr;
    uint32_t dropped;                           /* events lost to a full queue (a test reads it: must stay 0) */
    int32_t tmp[TX_BLOCK];
    tx_gen_t gen[LN_N];                         /* the lanes' generators */
    uint8_t gen_on[LN_N], gen_mute[LN_N];       /* mute: no notes (a classic engine: its activity map plays it) */
    int8_t int_id[LN_N];                        /* the lane engine's ACTIVITY (the source's), -1: none */
    uint32_t gen_seed[LN_N];                    /* set by ln_gen_on before its event */
    tx_gov_t gov;
    uint32_t gov_cost[LN_N], gov_lvl[LN_N];
    int32_t lpk[LN_N];                          /* each lane's peak in its last block (Q23: the followers, the governor) */
    uint16_t lev[LN_N];                         /* its engine's events in its last block (the envelopes, the followers) */
    uint16_t lhero[LN_N];                       /* its heroes sounding after its last block */
    uint32_t t_blk, t_bus, blk_us;              /* (ln_bus) ln_block's start and the bus's end (clock); its last us */
    uint32_t pre_pk, rev_pk, post_pk;           /* held (ln_bus): ln_block before the bus, the reverb, the rest after
                                                 * the bus and outside ln_block (the limiter, the platform's), us */
    uint32_t use_max, cap_lo;                   /* (the debug reply, textura.c) since its last read: the most used
                                                 * before the grains, the most the cap was under TX_GD_MAXG */
} ln LN_POOL;
static uint8_t ln_arena[LN_N + 1u][TX_STATE_MAX] __attribute__((aligned(64))) LN_POOL;

#if defined(LN_CLOCK) && defined(LN_ISR_US)
#define LN_BUS_TIMED
#define LN_HOLD(h, x) ((x) > (h) - ((h) >> 4) ? (x) : (h) - ((h) >> 4))
/* the bus on the FM-1: the grain delay's room measured (above), the grains and the reverb timed apart */
static void ln_bus(void *u, int32_t *L, int32_t *R, unsigned n)
{
    tx_fx_t *fx = (tx_fx_t *)u;
    uint32_t t0 = LN_CLOCK(), t1, use;
    ln.pre_pk = LN_HOLD(ln.pre_pk, LN_CLOCK_US(t0 - ln.t_blk));
    use = ln.pre_pk + ln.rev_pk + ln.post_pk;
    fx->room_us = use < TX_GOV_ISR_TARGET ? (int32_t)(TX_GOV_ISR_TARGET - use) : 0;
    tx_fx_gd_cap(fx);
    if (use > ln.use_max) ln.use_max = use;
    if (TX_GD_MAXG - fx->gd.cap > ln.cap_lo) ln.cap_lo = TX_GD_MAXG - fx->gd.cap;
    TX_FAR(tx_fx_grain_process)(&fx->gd, L, R, n);
    t1 = LN_CLOCK();
    TX_FAR(tx_fx_reverb_process)(&fx->rev, L, R, n);
    ln.t_bus = LN_CLOCK();
    ln.rev_pk = LN_HOLD(ln.rev_pk, LN_CLOCK_US(ln.t_bus - t1));
}
#endif

static void ln_init(void)
{
    uint8_t *b = (uint8_t *)&ln;
    uint32_t i;
    for (i = 0; i < sizeof ln; i++) b[i] = 0;
    tx_mix_init(&ln.mix);
    tx_fx_init(&ln.fx);
    ln.mix.ins = tx_fx_lane; ln.mix.bus = tx_fx_bus; ln.mix.busq = tx_fx_bus_quiet; ln.mix.fx = &ln.fx;
#ifdef LN_BUS_TIMED
    ln.mix.bus = ln_bus;
#endif
    for (i = 0; i < LN_N; i++) { tx_gen_init(&ln.gen[i], 1u + i); ln.int_id[i] = -1; ln.ar[i] = (uint8_t)i; }
    ln.spare = LN_N;   /* (the player's first
                                                         * generator call: its parameter, seeded 1 + lane) */
    tx_gov_init(&ln.gov, LN_N, LN_GOV_POOL_US);
}

/* the source's activity knob, which the generator drives (as the player's) */
static int ln_act_id(const tx_engine_t *e) { return tx_param_find(e, "ACTIVITY"); }

/* ---- main loop side */
static int ln_q(uint32_t kind, uint32_t lane, uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t w = ln.qw;
    if (w - ln.qr >= LN_QN) { ln.dropped++; return -1; }
    ln.q[w % LN_QN] = (ln_ev_t){(uint8_t)kind, (uint8_t)lane, (uint8_t)a, (uint8_t)b, (uint8_t)c};
    __asm__ volatile("" ::: "memory");          /* the event, then the index (one writer, one reader) */
    ln.qw = w + 1u;
    return 0;
}
/* lane k's engine: e (0: the lane off). The new engine is set up here, in a free arena (the lane's own when its engine
 * is off, else the spare); the interrupt swaps it in at its next block. Returns -1 while the lane's last switch is
 * still fading, or (a sounding lane) while another lane's switch holds the spare: call again later. */
static int ln_select(uint32_t k, const tx_engine_t *e, uint32_t seed)
{
    uint8_t *st;
    uint32_t i;
    if (k >= LN_N) return -1;
    if (ln.busy[k]) return -1;
    if (!e) { ln.busy[k] = 1; if (ln_q(LN_EV_OFF, k, 0, 0, 0)) { ln.busy[k] = 0; return -1; } return 0; }
    if (e->state_bytes > TX_STATE_MAX) return -1;
    if (ln.mix.e[k] && ln.spare_busy) return -1;
    st = ln_arena[ln.mix.e[k] ? ln.spare : ln.ar[k]];
    for (i = 0; i < TX_STATE_MAX; i++) st[i] = 0;
    e->init(st, seed);
    ln.next_e[k] = e;
    ln.busy[k] = 1;
    if (ln.mix.e[k]) ln.spare_busy = 1;
    if (ln_q(LN_EV_SWAP, k, 0, 0, 0)) { ln.busy[k] = 0; if (ln.mix.e[k]) ln.spare_busy = 0; return -1; }
    return 0;
}
static const tx_engine_t *ln_engine(uint32_t k) { return k < LN_N ? ln.mix.e[k] : 0; }
static int ln_key(uint32_t k, uint32_t note, uint32_t vel, uint32_t on) { return ln_q(LN_EV_KEY, k, note, vel, on != 0); }
static int ln_param(uint32_t k, uint32_t id, uint32_t v) { return ln_q(LN_EV_PARAM, k, id, v, 0); }   /* id: the engine's, or TX_PID_* */
static int ln_level(uint32_t k, uint32_t v) { return ln_param(k, TX_PID_LEVEL, v); }
static int ln_pan(uint32_t k, uint32_t v) { return ln_q(LN_EV_PAN, k, v, 0, 0); }
static int ln_trim(uint32_t k, uint32_t q12) { return ln_q(LN_EV_TRIM, k, q12 & 255u, q12 >> 8, 0); }   /* the sound's level */
static int ln_fx(uint32_t g, uint32_t k, uint32_t id, uint32_t v) { return ln_q(LN_EV_FX, k, g, id, v); }
/* the lane's generator on (a new walk from seed) or off (its notes released) */
static int ln_gen_on(uint32_t k, uint32_t on, uint32_t seed)
{
    if (k >= LN_N) return -1;
    ln.gen_seed[k] = seed;
    return ln_q(LN_EV_GEN_ON, k, on != 0, 0, 0);
}
static int ln_gen_param(uint32_t k, uint32_t id, uint32_t v) { return ln_q(LN_EV_GEN_PARAM, k, id, v, 0); }
static int ln_gen_mute(uint32_t k, uint32_t m) { return ln_q(LN_EV_GEN_MUTE, k, m != 0, 0, 0); }
/* the lane's source: its hosted engine's parameter id set to v (rel: released, back to the source's own) */
static int ln_host(uint32_t k, uint32_t id, uint32_t v, uint32_t rel) { return ln_q(LN_EV_HOST, k, id, v, rel != 0); }
static int ln_gsync(uint32_t smp) { return ln_q(LN_EV_GSYNC, 0, 0, smp & 0xFFu, smp >> 8 & 0xFFu); }   /* the grain delay's
                                                         * echo on the tempo, samples (< 65536; 0: its TIME) */
/* the hosted engine's value now (main loop: a read of the interrupt's state, for the screen only), or -1 */
/* the engine the lane's source hosts now (0: none, or not a source): a read of the interrupt's state, one pointer */
static const tx_engine_t *ln_hosted(uint32_t k)
{
    if (k >= LN_N || !ln.mix.e[k] || ln.int_id[k] < 0) return 0;
    return tx_source_hosted(ln.mix.st[k]);
}
static int ln_host_value(uint32_t k, const tx_engine_t *src, const tx_engine_t *hosted, uint32_t id)
{
    if (k >= LN_N || !src || ln.mix.e[k] != src || tx_source_hosted(ln.mix.st[k]) != hosted) return -1;
    return tx_source_host_value(ln.mix.st[k], id);
}
static int32_t ln_gen_activity(uint32_t k) { return k < LN_N ? ln.gen[k].a >> 8 : 0; }   /* 0..127, the walk now */
static uint32_t ln_gov_cur(uint32_t k) { return k < LN_N ? ln.gov.cur[k] : 0u; }
static uint32_t ln_gov_user(uint32_t k) { return k < LN_N ? ln.gov.user[k] : 0u; }
static uint32_t ln_gov_us(void) { return ln.gov.avg >> 3; }   /* the pool's cost now, us (its ~8-block mean) */

#ifdef LN_COST_EST
/* the host's stand-in for a lane's block on the FM-1 (us): a base and its heroes sounding (lhero: 0xFFFF quiet);
 * nothing when quiet */
static uint32_t ln_cost_est(uint32_t k)
{
    if (!ln.mix.e[k] || ln.lhero[k] == 0xFFFFu) return 0;   /* (quiet: it adds exact zeros, next to free) */
    return LN_EST_BASE + LN_EST_HERO * ln.lhero[k];
}
#endif

/* ---- the interrupt side */
static void ln_event(const ln_ev_t *v)
{
    uint32_t k = v->lane;
    switch (v->kind) {
    case LN_EV_KEY:
        if (k < LN_N && ln.mix.e[k]) ln.mix.e[k]->key(ln.mix.st[k], v->a, v->b, v->c);
#ifdef LN_MOD
        if (k < LN_N && v->c) mod_key_isr(k, v->b);   /* (the LFOs' triggers, VELOCITY, RANDOM) */
        else if (k < LN_N) mod_key_off_isr(k);
#endif
        break;
    case LN_EV_PARAM:
        if (k < LN_N && ln.mix.e[k]) ln.mix.e[k]->param(ln.mix.st[k], v->a, v->b);
        if (k < LN_N && v->a == TX_PID_PARTICLES) { ln.gov.cur[k] = 0; tx_gov_user(&ln.gov, k, v->b); }
        break;
    case LN_EV_PAN:
        if (k < LN_N) tx_mix_pan(&ln.mix, k, v->a);
        break;
    case LN_EV_TRIM:
        if (k < LN_N) tx_mix_trim(&ln.mix, k, (uint32_t)v->a | (uint32_t)v->b << 8);
        break;
    case LN_EV_SWAP:
    case LN_EV_OFF:
        if (k >= LN_N) break;
        if (ln.mix.e[k]) {                      /* the old engine fades out in its arena */
            if (ln.ret_g[k]) ln.dc[k] += ln.ret_last[k];   /* one still fading: its last sample decays instead */
            ln.ret_e[k] = ln.mix.e[k]; ln.ret_st[k] = ln.mix.st[k]; ln.ret_g[k] = 32768;
            if (v->kind == LN_EV_SWAP) {        /* the lane takes the spare; its old arena is the spare once faded */
                uint8_t a = ln.ar[k];
                ln.ar[k] = ln.spare; ln.spare = a;
            }
        }
        ln.mix.e[k] = v->kind == LN_EV_SWAP ? ln.next_e[k] : 0;   /* (initialised by ln_select: tx_mix_set's part) */
        ln.mix.st[k] = v->kind == LN_EV_SWAP ? ln_arena[ln.ar[k]] : 0;
        ln.mix.slot_peak[k] = 0;
        ln.int_id[k] = -1;
#ifdef LN_MOD
        mod_lane_reset_isr(k);                  /* (its notes gone with it) */
#endif
        if (ln.mix.e[k]) {
            const tx_engine_t *e = ln.mix.e[k];
            const char *a = e->name, *b = "source";
            while (*a && *a == *b) { a++; b++; }
            if (*a == *b) ln.int_id[k] = (int8_t)ln_act_id(e);   /* (the player's: the source only) */
            ln.gov.cur[k] = 0; tx_gov_user(&ln.gov, k, e->particles_def);
        }
        if (!ln.ret_g[k]) ln.busy[k] = 0;
        break;
    case LN_EV_GEN_ON:
        if (k >= LN_N) break;
        if (v->a && !ln.gen_on[k] && ln.gen_seed[k]) tx_rng_seed(&ln.gen[k].rng, ln.gen_seed[k] ^ 0x6E4E4E21u);   /* each
                                                         * start a new walk (as the player's txw_gen_on) */
        if (!v->a && ln.gen_on[k] && ln.mix.e[k]) {
            tx_gen_ev_t ev[TX_GEN_OFFS];
            unsigned ne = tx_gen_stop(&ln.gen[k], ev, TX_GEN_OFFS), q;
            for (q = 0; q < ne; q++) {
                ln.mix.e[k]->key(ln.mix.st[k], ev[q].note, 0, 0);
#ifdef LN_MOD
                mod_key_off_isr(k);
#endif
            }
        }
        ln.gen_on[k] = v->a;
        break;
    case LN_EV_GEN_PARAM:
        if (k < LN_N) tx_gen_param(&ln.gen[k], v->a, v->b);
        break;
    case LN_EV_GEN_MUTE:
        if (k < LN_N) ln.gen_mute[k] = v->a;
        break;
    case LN_EV_GSYNC: tx_fx_grain_sync(&ln.fx.gd, (uint32_t)v->b | (uint32_t)v->c << 8); break;
    case LN_EV_HOST:
        if (k < LN_N && ln.mix.e[k] && ln.int_id[k] >= 0) tx_source_host_param(ln.mix.st[k], v->a, v->c ? 256u : v->b);
        break;
    case LN_EV_FX:
        if (v->a == LN_FX_DRIVE && k < TX_FX_LANES) tx_fx_drive_param(&ln.fx.drive[k], v->b, v->c);
        else if (v->a == LN_FX_FILTER && k < TX_FX_LANES) tx_fx_filter_param(&ln.fx.filter[k], v->b, v->c);
        else if (v->a == LN_FX_REVERB) tx_fx_reverb_param(&ln.fx.rev, v->b, v->c);
        else if (v->a == LN_FX_GRAIN) tx_fx_grain_param(&ln.fx.gd, v->b, v->c);
        else if (v->a == LN_FX_RELATCH) tx_fx_reverb_relatch(&ln.fx.rev);
        break;
    }
}

/* one block (n <= TX_BLOCK frames): L, R Q23, limited to -1 dBFS */
static void ln_block(int32_t *L, int32_t *R, uint32_t n)
{
    uint32_t k, j;
#ifdef LN_BUS_TIMED
    {                                           /* the last block's rest: after its bus, and the interrupt outside */
        uint32_t isr = LN_ISR_US(), bus = LN_CLOCK_US(ln.t_bus - ln.t_blk);
        uint32_t post = (ln.blk_us > bus ? ln.blk_us - bus : 0u) + (isr > ln.blk_us ? isr - ln.blk_us : 0u);
        if (ln.blk_us) ln.post_pk = LN_HOLD(ln.post_pk, post);
        ln.t_blk = LN_CLOCK();
        ln.t_bus = ln.t_blk;                    /* (a block with no bus call: its whole time counts as the rest) */
    }
#endif
    while (ln.qr != ln.qw) { ln_event(&ln.q[ln.qr % LN_QN]); ln.qr++; }
#ifdef LN_MOD
    {
#ifdef LN_CLOCK
        uint32_t t0 = LN_CLOCK();
#endif
        mod_block();                            /* the modulation: the LFOs, the matrix, its targets set (mod.c) */
#ifdef LN_CLOCK
        tx_gov_extra(&ln.gov, LN_CLOCK_US(LN_CLOCK() - t0));   /* (the pool carries it, no lane does) */
#elif defined(LN_COST_EST)
        tx_gov_extra(&ln.gov, LN_EST_MOD);
#endif
    }
#endif
    tx_mix_begin(&ln.mix, n);
    for (k = 0; k < LN_N; k++) {
#ifdef LN_CLOCK
        uint32_t t0 = LN_CLOCK();
#endif
        ln.lpk[k] = 0; ln.lev[k] = 0;
        if (!ln.mix.e[k] && !ln.ret_g[k] && !ln.dc[k]) continue;
        ln.mix.slot_peak[k] = 0;                /* (a peak a block: the lane's own, before its old engine's fade) */
        if (ln.gen_on[k] && ln.mix.e[k]) {      /* the lane's generator: its activity and notes */
            tx_gen_ev_t ev[TX_GEN_EV_MAX];
            uint8_t in;
            unsigned ne = tx_gen_block(&ln.gen[k], n, ev, TX_GEN_EV_MAX, &in), q;
            if (ln.int_id[k] >= 0) ln.mix.e[k]->param(ln.mix.st[k], (uint8_t)ln.int_id[k], in);
            for (q = 0; q < ne && !ln.gen_mute[k]; q++) {
                ln.mix.e[k]->key(ln.mix.st[k], ev[q].note, ev[q].vel, ev[q].vel != 0);
#ifdef LN_MOD
                if (ev[q].vel) mod_key_isr(k, ev[q].vel);
                else mod_key_off_isr(k);
#endif
            }
        }
        tx_mix_slot_render(&ln.mix, k, n);
        ln.lpk[k] = ln.mix.slot_peak[k];
        if (ln.mix.e[k] && ln.mix.e[k]->stats) {
            tx_stats_t st;
            ln.mix.e[k]->stats(ln.mix.st[k], &st);
            ln.lev[k] = st.events; ln.lhero[k] = st.quiet ? 0xFFFFu : st.particles;
        } else ln.lhero[k] = 0xFFFFu;
#ifdef LN_GOV
        {                                       /* (the fade below is not the lane's: as the player) */
            uint32_t f = tx_fx_lane_us(&ln.fx, k);   /* its inserts (DRIVE, FILTER): apart, no hero buys them */
#ifdef LN_CLOCK
            uint32_t c = LN_CLOCK_US(LN_CLOCK() - t0);   /* (timed with the inserts: they are taken off) */
            ln.gov_cost[k] = c > f ? c - f : 0u;
#else
            ln.gov_cost[k] = ln_cost_est(k);
#endif
            tx_gov_fixed(&ln.gov, k, ln.mix.e[k] ? f : 0u);
        }
#endif
        ln.gov_lvl[k] = (uint32_t)ln.mix.slot_peak[k];
        if (ln.ret_g[k] || ln.dc[k]) {          /* the switched-out engine's fade, the decaying step (as the player) */
            int32_t g = ln.ret_g[k], d = ln.dc[k], last = 0;
            for (j = 0; j < n; j++) ln.tmp[j] = 0;
            if (g) ln.ret_e[k]->render(ln.ret_st[k], ln.tmp, n);
            for (j = 0; j < n; j++) {
                int32_t gj = g - (int32_t)(j * 8192u / TX_BLOCK);   /* 4 blocks from 1 to 0 */
                last = (ln.tmp[j] >> 15) * gj + (((ln.tmp[j] & 0x7FFF) * gj) >> 15);   /* = (x * g) >> 15, 32-bit */
                ln.tmp[j] = last + d;
                d -= d >> 6;
                if (d > -64 && d < 64) d = 0;
            }
            ln.ret_last[k] = g > 8192 ? last : 0;
            ln.ret_g[k] = g > 8192 ? g - 8192 : 0;
            ln.dc[k] = d;
            tx_mix_add(&ln.mix, k, ln.tmp, n);
            if (!ln.ret_g[k]) {                 /* the arena is free again (a switch's: the spare) */
                if (ln.ret_st[k] == ln_arena[ln.spare]) ln.spare_busy = 0;
                ln.ret_st[k] = 0;
                ln.busy[k] = 0;
            }
        }
    }
#ifdef LN_GOV
    {                                           /* the governor: over the pool, the quietest lane gives a hero */
        uint8_t on[LN_N];
        int pk;
        for (k = 0; k < LN_N; k++) on[k] = ln.mix.e[k] != 0;
        pk = tx_gov_block(&ln.gov, ln.gov_cost, ln.gov_lvl, on);
        if (pk >= 0 && ln.mix.e[pk]) ln.mix.e[pk]->param(ln.mix.st[pk], TX_PID_PARTICLES, ln.gov.cur[pk]);
    }
#endif
    tx_mix_end(&ln.mix, L, R, n);
#ifdef LN_BUS_TIMED
    ln.blk_us = LN_CLOCK_US(LN_CLOCK() - ln.t_blk);
#endif
}
