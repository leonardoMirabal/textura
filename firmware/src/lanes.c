/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura lanes: the sound behind the panel. Four lanes, each an engine instance (engines/: the same code the bench
 * and the audition player run), each panned into a stereo pair, then the DRIVE, the FILTER, the
 * COMPRESSOR, the GRAIN DELAY, the REVERB and the output limiter (tx_core_mix.h, tx_fx.h): the audition player's chain (audition/tx_web.c).
 *
 *   main loop   ln_select (a lane's engine: prepared in the spare arena, here, never in the audio interrupt),
 *               ln_key, ln_param, ln_level, ln_pan, ln_fx, ln_gen_on, ln_gen_param, ln_gen_mute: queued (ln_q), the
 *               interrupt takes them at its next block
 *   interrupt   ln_block: the queue, then every lane's generator (tx_core_gen: the lane playing on its own, its
 *               activity and notes, as the player's), every lane's engine for one block (some on the second core:
 *               LN_PAR, below) over its ring, then lane by lane its pan into the pair; the effects, Q23
 *               stereo; then the CPU governor (tx_core_gov.h) when the app gives it the lanes' costs (LN_CLOCK or
 *               LN_COST_EST, below)
 *
 * A switched-out engine rings out in its own arena while the new one starts in the spare, as the player does
 * (tx_core_ring.h: its tail at full level up to 302 ms, then a 151 ms fade; done at once when quiet), so a switch is
 * never a cut. It renders under the new engine, through the lane's gate, trim and pan, so a switch changes
 * none of those under it. One arena a lane and one spare for all of them (the player keeps two a lane): the rung-out
 * engine's arena becomes the spare, so while one lane's switch rings another sounding lane's waits (ln_can_select,
 * ln_select: -1, call again; a lane silent of its engine needs no spare) and ln_hurry has the ringing ones skip the
 * rest of their hold: they fade at once (151 ms, never cut), so a scene that changes several lanes morphs them one
 * by one, ~150 ms apart. A still playing engine handed over (the caller kept its key down) fades the same way: a
 * crossfade. The rings' cost is the pool's (the governor's extra), so no lane's heroes pay for it. This file knows
 * nothing of the FM-1:
 * tests/lanes_test.c runs it beside the player's chain and checks they render the same samples. Integers only (as
 * engines/). */
#include "tx_engine.h"
#include "tx_core_mix.h"
#include "tx_fx.h"
#include "tx_core_gen.h"
#include "tx_core_gov.h"
#include "tx_core_ring.h"

#define LN_N TX_MIX_SLOTS                       /* lanes */
#define LN_QN 128u                              /* queued events (a power of two) */
#define LN_GOV_POOL_US TX_GOV_POOL_DEVICE       /* the sources' CPU pool on the FM-1 (CLAUDE.md: ~2500 us a block) */
/* The governor's costs, FM-1 us a block, from the app:
 *   LN_CLOCK(), LN_CLOCK_US(d)   the device: its free-running clock (wrapping) and a difference of it in us; each
 *                                lane's block is timed (its generator and engine) and so
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
#if defined(LN_PIPE) && !defined(LN_PAR)
#error "LN_PIPE needs LN_PAR (the second core's hooks)"
#endif
#define LN_EST_BASE 100u                        /* the stand-in: a lane's block, us, and each hero sounding more */
#define LN_EST_HERO 20u                         /* (make tx-budget: 120..250 us typical, 5..60 a hero) */
#define LN_EST_MOD 8u                           /* the modulation's stand-in, us (make tx-budget section 5: 4..30) */
/* LN_PAR: a second core renders some of the lanes' engines while this one renders the rest. The app gives
 *   LN_PAR_ON()            1 while the second core runs (it may stop for good at any block: then 0)
 *   LN_PAR_RUN(fn, arg)    hands it fn(arg): 1 taken, 0 not (this core does the work)
 *   LN_PAR_WAIT()          waits for it: 0 done, else the work is this core's: it refused the job (a bad read it
 *                          could not clear: nothing done, it stays on), or it was stopped mid-job (its lanes' engines
 *                          are left as it left them: they render here again, once; a click at most, one core then)
 * A lane's engine renders into its own block (ln_out: its own cache lines, nothing else written on them while the
 * other core works); everything else (the generators before, the pan, the fades, the governor after)
 * stays on this core in lane order, and the pair is a sum of int32 terms: the samples are the same whichever core
 * renders which lane, and the same as with one core (tests/lanes_test.c checks it with a thread as the second core).
 * Which lanes go there: the split of the sounding lanes whose larger half costs least (the lanes' last costs: the
 * governor's), ln.par_force a fixed one (the tests). While the second core runs, the governor's pool is twice
 * LN_GOV_POOL_US (a lane alone still at most half of it: one core's pool).
 * LN_PIPE (with LN_PAR): the bus one block behind. A block's mix end (the DRIVE, the COMPRESSOR, the GRAIN DELAY's end,
 * the REVERB, the limiter: tx_mix_run) is handed to the second core as ln_block returns, and runs there between the
 * interrupts (its XIP twins, nothing handed on from there); the next ln_block waits for it first, gives its result
 * out, then goes on as before. The work is done in the same order as without, only the output is one block later
 * (5.8 ms): the same samples (tests/lanes_test.c LANES_TEST_PIPE). The interrupt is left with the lanes. Second core
 * off, or the job refused: the mix end runs here at the block's end (the same delay); lost mid-job: here again, once
 * (a click at most). */

/* LN_POOL: where the lanes' big state goes. On the FM-1 (textura.c) the platform's .pool (zeroed at start), as it is
 * far over the 96 KiB of .data + .bss; on the host (Mach-O has no such section) and in the tests, plain statics */
#ifndef LN_POOL
#define LN_POOL
#endif

enum { LN_EV_KEY, LN_EV_PARAM, LN_EV_PAN, LN_EV_SWAP, LN_EV_OFF, LN_EV_FX, LN_EV_GEN_ON, LN_EV_GEN_PARAM,
       LN_EV_GEN_MUTE, LN_EV_HOST, LN_EV_GSYNC, LN_EV_TRIM, LN_EV_GATE };

/* the source's hosted engine (tx_eng_source.c): its own parameters, set over the source's curves until the sound
 * (PRESET or TYPE) changes */
const tx_engine_t *tx_source_hosted(const void *st);
int tx_source_host_value(const void *st, unsigned id);
void tx_source_host_param(void *st, unsigned id, unsigned val);
enum { LN_FX_DRIVE, LN_FX_REVERB = 2, LN_FX_GRAIN, LN_FX_RELATCH, LN_FX_COMP };   /* ln_fx groups (1: the lanes' FILTER, retired; as the player's
                                                 * txw_fx_param; RELATCH: a whole sound loaded, tx_fx_reverb_relatch) */
typedef struct { uint8_t kind, lane, a, b, c; } ln_ev_t;

static struct {
    tx_mix_t mix;
    tx_fx_t fx;
    uint8_t ar[LN_N];                           /* the arena (ln_arena) holding the lane's engine */
    uint8_t spare;                              /* the free arena (taken by a switch, given back by its fade) */
    volatile uint8_t spare_busy;                /* a switch holds the spare (main loop sets it, its fade clears it) */
    const tx_engine_t *next_e[LN_N];            /* prepared by ln_select, taken by LN_EV_SWAP */
    uint32_t ring_us;                           /* (the governor) the rings' cost this block: the pool carries it */
    volatile uint8_t hurry;                     /* ln_hurry: the rings skip the rest of their hold (ln_block) */
    volatile uint8_t busy[LN_N];                /* a switch is under way (main loop: wait before the next) */
    ln_ev_t q[LN_QN];
    volatile uint32_t qw, qr;
    uint32_t dropped;                           /* events lost to a full queue (a test reads it: must stay 0) */
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
    uint32_t gus[LN_N];                         /* (LN_CLOCK) the lane's generator this block, us */
    uint32_t par_n;                             /* (LN_PAR) the block's frames, for the second core's job */
    uint8_t par_mask, par_force;                /* the lanes it rendered this block; a fixed split (0: by cost) */
    uint32_t par_blocks, par_lost;              /* blocks it rendered lanes in; jobs lost (refused, held mid-job) */
    uint32_t gv_us[2], gv_pk;                   /* (LN_CLOCK) the grain voices' halves' last block, us; held, those on
                                                 * this core (ln_bus) */
    uint32_t isr_pk;                            /* (LN_BUS_TIMED) the whole interrupt, held (two cores: the grains' cap) */
    uint32_t c0_us, c1_us, c0_max, c1_max;      /* (LN_CLOCK) the render phase's busy time per core, last and the
                                                 * most since the debug reply */
    uint8_t pipe;                               /* (LN_PIPE) the last block's mix end: 0 none, 1 done here, 2 on the
                                                 * second core */
    uint32_t pipe_n, pipe_lost;                 /* its frames; the jobs lost (run here again) */
} ln LN_POOL;
static uint8_t ln_arena[LN_N + 1u][TX_STATE_MAX] __attribute__((aligned(64))) LN_POOL;
typedef struct {                                /* a lane's engine block, written by the core that renders it */
    int32_t x[TX_BLOCK];
    uint32_t us;                                /* (LN_CLOCK) its render, us */
    tx_ring_t ring;                             /* the switched-out engine, ringing out (tx_core_ring.h): rendered
                                                 * with the lane, so on the lane's core */
    uint32_t rus, rhp;                          /* the ring's last block: (LN_CLOCK) us; its heroes sounding */
    int32_t rpk;                                /* its peak (0: silent or done; the tests) */
} __attribute__((aligned(64))) ln_out_t;
static ln_out_t ln_out[LN_N] LN_POOL;
static struct { uint32_t gv_us[2], busy_us, pipe_us; volatile uint8_t pipe_on; } __attribute__((aligned(64))) ln_c1t LN_POOL;
                                                /* (the second core's own line: its job's grain voices and busy time;
                                                 * LN_PIPE: the mix end's us, and on while it runs it there) */
#define LN_GV 4u                                /* the split's items past the lanes: the grain delay's voices, the echo
                                                 * and the first half (LN_GV), the second half (LN_GV + 1) */

#if defined(LN_CLOCK) && defined(LN_ISR_US)
#define LN_BUS_TIMED
#define LN_HOLD(h, x) ((x) > (h) - ((h) >> 4) ? (x) : (h) - ((h) >> 4))
/* the bus on the FM-1: the grain delay's room measured (above), the grains and the reverb timed apart */
static int ln_bus_begin(tx_fx_t *fx, unsigned n)   /* at the block's start: the cap from the blocks before */
{
    uint32_t pre = ln.pre_pk > ln.gv_pk ? ln.pre_pk - ln.gv_pk : 0u, use = pre + ln.rev_pk + ln.post_pk;
    fx->room_us = use < TX_GOV_ISR_TARGET ? (int32_t)(TX_GOV_ISR_TARGET - use) : 0;
#ifdef LN_PAR
    if (LN_PAR_ON()) {                          /* two cores: the grains as set while the whole interrupt (held) fits
                                                 * its target; over it, the room is the grains' cost less the excess */
        uint32_t g = tx_fx_gd_dev_us(fx, fx->gd.cap), isr = ln.isr_pk;
        use = isr;
        fx->room_us = (int32_t)(isr <= TX_GOV_ISR_TARGET ? g + (TX_GOV_ISR_TARGET - isr) : g > isr - TX_GOV_ISR_TARGET ? g - (isr - TX_GOV_ISR_TARGET) : 0u);
        if (use > ln.use_max) ln.use_max = use;
        use = (uint32_t)tx_fx_bus_begin(fx, n);
        if (TX_GD_MAXG - fx->gd.cap > ln.cap_lo) ln.cap_lo = TX_GD_MAXG - fx->gd.cap;
        return (int)use;
    }
#endif
    if (use > ln.use_max) ln.use_max = use;
    use = (uint32_t)tx_fx_bus_begin(fx, n);
    if (TX_GD_MAXG - fx->gd.cap > ln.cap_lo) ln.cap_lo = TX_GD_MAXG - fx->gd.cap;
    return (int)use;
}
static void ln_bus(void *u, int32_t *L, int32_t *R, unsigned n)
{
    tx_fx_t *fx = (tx_fx_t *)u;
    uint32_t t0 = LN_CLOCK(), t1;
    ln.pre_pk = LN_HOLD(ln.pre_pk, LN_CLOCK_US(t0 - ln.t_blk));
    TX_FAR(tx_fx_grain_end)(&fx->gd, L, R, n);
    t1 = LN_CLOCK();
    TX_FAR(tx_fx_reverb_process)(&fx->rev, L, R, n);
    ln.t_bus = LN_CLOCK();
    ln.rev_pk = LN_HOLD(ln.rev_pk, LN_CLOCK_US(ln.t_bus - t1));
}
#endif

#ifdef LN_PAR
/* the reverb's ROOM on two cores (tx_fx_reverb_t.body_run): its body (the CHAMBER) on the second core, the tail here */
static void ln_rev_job(uint32_t a) { (void)a; TX_FAR(tx_fx_reverb_body)(&ln.fx.rev); }   /* (in XIP: either core) */
static int ln_rev_run(void) { return !ln_c1t.pipe_on && LN_PAR_ON() && LN_PAR_RUN(ln_rev_job, 0u); }   /* (on the
                                                 * second core itself, LN_PIPE: nothing handed on) */
static int ln_rev_wait(void) { int l = LN_PAR_WAIT(); if (l) ln.par_lost++; return l; }
/* the reverb kernels' halves on the second core (tx_fx_reverb_t.par): one job, its function and argument here */
static void (*ln_kfn)(void *);
static void *ln_karg;
static void ln_kjob(uint32_t a) { (void)a; ln_kfn(ln_karg); }
static int ln_krun(void (*fn)(void *), void *a) { ln_kfn = fn; ln_karg = a; return !ln_c1t.pipe_on && LN_PAR_ON() && LN_PAR_RUN(ln_kjob, 0u); }
#ifdef LN_PIPE
static void ln_pipe_job(uint32_t a)             /* the second core's: the last block's mix end (its twins) */
{
#ifdef LN_CLOCK
    uint32_t t0 = LN_CLOCK();
#endif
    (void)a;
    ln_c1t.pipe_on = 1;
    tx_mix_run(&ln.mix, ln.pipe_n, 1u);
    ln_c1t.pipe_on = 0;
#ifdef LN_CLOCK
    ln_c1t.pipe_us = LN_CLOCK_US(LN_CLOCK() - t0);
#endif
}
#endif
#endif
typedef char ln_scr1_fits[sizeof(tx_drv_scr_t) <= TX_MIX_SCR_BYTES ? 1 : -1];
static void ln_init(void)
{
    uint8_t *b = (uint8_t *)&ln;
    uint32_t i;
    for (i = 0; i < sizeof ln; i++) b[i] = 0;
    tx_mix_init(&ln.mix);
    tx_fx_init(&ln.fx);
#ifdef LN_PAR
    ln.fx.rev.body_run = ln_rev_run; ln.fx.rev.body_wait = ln_rev_wait;
    ln.fx.rev.par.run = ln_krun; ln.fx.rev.par.wait = ln_rev_wait;   /* (and the mix's DRIVE: R there) */
    ln.fx.scr1 = (tx_drv_scr_t *)TX_MIX_SCR(&ln.mix);   /* (the DRIVE TYPEs' scratch there: the mix's, free then) */
#endif
#ifdef LN_PIPE
    ln.mix.pre1 = tx_fx_drive_mix_c1; ln.mix.bus1 = tx_fx_bus_end_c1;   /* (the mix end on the second core) */
#endif
    ln.mix.pre = tx_fx_drive_mix; ln.mix.bus = tx_fx_bus_end; ln.mix.busq = tx_fx_bus_quiet; ln.mix.fx = &ln.fx;   /* (the
                                                 * bus's begin and the grains' voices: ln_block, ln_render) */
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
/* ln_select(k, e) would take the switch now (0: it would answer -1, the last switch still ringing) */
static int ln_can_select(uint32_t k, const tx_engine_t *e)
{
    if (k >= LN_N || ln.busy[k]) return 0;
    return !e || !ln.mix.e[k] || !ln.spare_busy;
}
/* a switch waits: the rings holding an arena skip the rest of their tail's hold and fade now (TX_RING_FADE, 151 ms:
 * shortened, never cut), so the next lane's switch comes sooner */
static void ln_hurry(void) { ln.hurry = 1; }
static const tx_engine_t *ln_engine(uint32_t k) { return k < LN_N ? ln.mix.e[k] : 0; }
static int ln_key(uint32_t k, uint32_t note, uint32_t vel, uint32_t on) { return ln_q(LN_EV_KEY, k, note, vel, on != 0); }
static int ln_param(uint32_t k, uint32_t id, uint32_t v) { return ln_q(LN_EV_PARAM, k, id, v, 0); }   /* id: the engine's, or TX_PID_* */
static int ln_level(uint32_t k, uint32_t v) { return ln_param(k, TX_PID_LEVEL, v); }   /* the lane's output (tx_mix_level) */
static int ln_pan(uint32_t k, uint32_t v) { return ln_q(LN_EV_PAN, k, v, 0, 0); }
static int ln_trim(uint32_t k, uint32_t q12) { return ln_q(LN_EV_TRIM, k, q12 & 255u, q12 >> 8, 0); }   /* the sound's level */
static int ln_gate(uint32_t k, uint32_t open) { return ln_q(LN_EV_GATE, k, open != 0, 0, 0); }   /* a mute: the output only */
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
static int ln_gsync(uint32_t smp) { return ln_q(LN_EV_GSYNC, 0, smp >> 16 & 0xFFu, smp & 0xFFu, smp >> 8 & 0xFFu); }   /* the grain delay's
                                                         * echo on the tempo, samples (< 2^24; 0: its TIME) */
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
        if (k < LN_N && v->a == TX_PID_LEVEL) { tx_mix_level(&ln.mix, k, v->b); break; }   /* (the lane's LEVEL: its
                                                 * output, at once on any engine and its ring; the engine stays at
                                                 * unity, its own voices' levels as they are) */
        if (k < LN_N && ln.mix.e[k]) ln.mix.e[k]->param(ln.mix.st[k], v->a, v->b);
        if (k < LN_N && v->a == TX_PID_PARTICLES) { ln.gov.cur[k] = 0; tx_gov_user(&ln.gov, k, v->b); }
        break;
    case LN_EV_PAN:
        if (k < LN_N) tx_mix_pan(&ln.mix, k, v->a);
        break;
    case LN_EV_TRIM:
        if (k < LN_N) tx_mix_trim(&ln.mix, k, (uint32_t)v->a | (uint32_t)v->b << 8);
        break;
    case LN_EV_GATE:
        if (k < LN_N) tx_mix_gate(&ln.mix, k, v->a);
        break;
    case LN_EV_SWAP:
    case LN_EV_OFF:
        if (k >= LN_N) break;
        if (ln.mix.e[k]) {                      /* the old engine fades out in its arena */
            tx_ring_start(&ln_out[k].ring, ln.mix.e[k], ln.mix.st[k]);   /* (one still ringing: its last sample decays) */
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
        if (!tx_ring_holds(&ln_out[k].ring)) ln.busy[k] = 0;
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
    case LN_EV_GSYNC: tx_fx_grain_sync(&ln.fx.gd, (uint32_t)v->b | (uint32_t)v->c << 8 | (uint32_t)v->a << 16); break;
    case LN_EV_HOST:
        if (k < LN_N && ln.mix.e[k] && ln.int_id[k] >= 0) tx_source_host_param(ln.mix.st[k], v->a, v->c ? 256u : v->b);
        break;
    case LN_EV_FX:
        if (v->a == LN_FX_DRIVE) tx_fx_drive_set(&ln.fx, v->b, v->c);
        else if (v->a == LN_FX_REVERB) tx_fx_reverb_param(&ln.fx.rev, v->b, v->c);
        else if (v->a == LN_FX_GRAIN) tx_fx_grain_param(&ln.fx.gd, v->b, v->c);
        else if (v->a == LN_FX_RELATCH) tx_fx_reverb_relatch(&ln.fx.rev);
        else if (v->a == LN_FX_COMP) tx_comp_param(&ln.mix.comp, v->b, v->c);
        break;
    }
}

/* lane k's ring (its switched-out engine), one block, into o->x (written whole: zeros when none) */
static void ln_ring_lane(ln_out_t *o, uint32_t n)
{
    uint32_t j;
    int32_t pk = 0;
#ifdef LN_CLOCK
    uint32_t t0 = LN_CLOCK();
#endif
    o->rhp = 0;
    if (!tx_ring_on(&o->ring)) { for (j = 0; j < n; j++) o->x[j] = 0; o->rus = 0; o->rpk = 0; return; }
    tx_ring_block(&o->ring, o->x, n, &o->rhp);
#ifdef LN_CLOCK
    o->rus = LN_CLOCK_US(LN_CLOCK() - t0);
#endif
    for (j = 0; j < n; j++) { int32_t a = o->x[j] < 0 ? -o->x[j] : o->x[j]; if (a > pk) pk = a; }
    o->rpk = pk;
}
/* lane k's engine, one block, into ln_out[k] over its ring (the old engine: the same gate, trim and pan, so a switch
 * changes none of them under it), on either core; it writes nothing else */
static void ln_render_lane(uint32_t k, uint32_t n)
{
    ln_out_t *o = &ln_out[k];
#ifdef LN_CLOCK
    uint32_t t0;
#endif
    ln_ring_lane(o, n);
#ifdef LN_CLOCK
    t0 = LN_CLOCK();
#endif
    ln.mix.e[k]->render(ln.mix.st[k], o->x, n);   /* (render adds) */
#ifdef LN_CLOCK
    o->us = LN_CLOCK_US(LN_CLOCK() - t0);
#endif
}
static void ln_gv(uint32_t h, uint32_t *us, uint32_t c)   /* the grain delay's voices, half h, on core c */
{
#ifdef LN_CLOCK
    uint32_t t0 = LN_CLOCK();
#endif
    if (c) {                                    /* core 1: the twins in XIP (TX_C1) */
        if (h) TX_C1(tx_fx_grain_voices_b)(&ln.fx.gd, ln.par_n);
        else TX_C1(tx_fx_grain_voices_a)(&ln.fx.gd, ln.par_n);
    } else if (h) TX_FAR(tx_fx_grain_voices_b)(&ln.fx.gd, ln.par_n);
    else TX_FAR(tx_fx_grain_voices_a)(&ln.fx.gd, ln.par_n);
#ifdef LN_CLOCK
    *us = LN_CLOCK_US(LN_CLOCK() - t0);
#else
    (void)us;
#endif
}
#ifdef LN_PAR
static void ln_par_job(uint32_t mask)           /* the second core's job: its lanes, and maybe the grains' voices */
{
    uint32_t k;
#ifdef LN_CLOCK
    uint32_t t0 = LN_CLOCK();
#endif
    for (k = 0; k < 2u; k++) if (mask >> (LN_GV + k) & 1u) ln_gv(k, &ln_c1t.gv_us[k], 1u);
    for (k = 0; k < LN_N; k++) if (mask >> k & 1u) ln_render_lane(k, ln.par_n);
#ifdef LN_CLOCK
    ln_c1t.busy_us = LN_CLOCK_US(LN_CLOCK() - t0);
#endif
}
/* the second core's items among on (the lanes, LN_GV the grains' voices; two or more): the split whose larger half
 * costs least, the last one on a tie */
static uint32_t ln_par_split(uint32_t on)
{
    uint32_t m, k, best = 0, bc = 0xFFFFFFFFu;
    if (ln.par_force) return ln.par_force & on;
    for (m = 1; m < (4u << LN_GV) - 1u; m++) {
        uint32_t c0 = 0, c1 = 0, c;
        if ((m & on) != m || m == on) continue;
        for (k = 0; k <= LN_GV + 1u; k++) if (on >> k & 1u) {
            uint32_t ck = (k >= LN_GV ? ln.gv_us[k - LN_GV] : ln.gov_cost[k]) + 1u;   /* (not timed yet: 1) */
            if (m >> k & 1u) c1 += ck; else c0 += ck;
        }
        c = c0 > c1 ? c0 : c1;
        if (c < bc || (c == bc && m == ln.par_mask)) { bc = c; best = m; }
    }
    return best;
}
#endif
/* every lane's engine, and (gv) the grain delay's voices: some on the second core (LN_PAR) */
static void ln_render(uint32_t n, int gv)
{
    uint32_t k, on = 0, m1 = 0;
#ifdef LN_CLOCK
    uint32_t t0 = LN_CLOCK();
#endif
    for (k = 0; k < LN_N; k++) if (ln.mix.e[k]) on |= 1u << k;
    ln.par_n = n;
    if (gv) { tx_fx_grain_split(&ln.fx.gd, n); on |= 3u << LN_GV; }   /* (the voices' two halves: either core) */
#ifdef LN_PAR
    if ((on & (on - 1u)) && LN_PAR_ON()) {      /* two items or more */
        m1 = ln_par_split(on);
        if (m1 && !LN_PAR_RUN(ln_par_job, m1)) m1 = 0;
    }
#endif
    for (k = 0; k < 2u; k++) if ((on & ~m1) >> (LN_GV + k) & 1u) ln_gv(k, &ln.gv_us[k], 0u);
    for (k = 0; k < LN_N; k++) if ((on & ~m1) >> k & 1u) ln_render_lane(k, n);
#ifdef LN_CLOCK
    ln.c0_us = LN_CLOCK_US(LN_CLOCK() - t0);
    if (ln.c0_us > ln.c0_max) ln.c0_max = ln.c0_us;
#endif
#ifdef LN_PAR
    if (m1 && LN_PAR_WAIT()) {                  /* stopped mid-job: its items here, from where it left them */
        for (k = 0; k < 2u; k++) if (m1 >> (LN_GV + k) & 1u) ln_gv(k, &ln.gv_us[k], 0u);
        for (k = 0; k < LN_N; k++) if (m1 >> k & 1u) ln_render_lane(k, n);
        ln.par_lost++;
        m1 = 0;
    }
    if (m1) {
        ln.par_blocks++;
        for (k = 0; k < 2u; k++) if (m1 >> (LN_GV + k) & 1u) ln.gv_us[k] = ln_c1t.gv_us[k];
        ln.c1_us = ln_c1t.busy_us;
        if (ln.c1_us > ln.c1_max) ln.c1_max = ln.c1_us;
    } else ln.c1_us = 0;
    ln.par_mask = (uint8_t)m1;
#endif
#ifdef LN_BUS_TIMED
    ln.gv_pk = gv ? LN_HOLD(ln.gv_pk, ((m1 >> LN_GV & 1u) ? 0u : ln.gv_us[0]) + ((m1 >> (LN_GV + 1u) & 1u) ? 0u : ln.gv_us[1]))
                  : 0u;                         /* (those on this core: inside pre_pk) */
#endif
}

/* one block (n <= TX_BLOCK frames): L, R Q23, limited to -2 dBFS */
static void ln_block(int32_t *L, int32_t *R, uint32_t n)
{
    uint32_t k;
#ifdef LN_BUS_TIMED
    {                                           /* the last block's rest: after its bus, and the interrupt outside */
        uint32_t isr = LN_ISR_US(), bus = LN_CLOCK_US(ln.t_bus - ln.t_blk);
        uint32_t post = (ln.blk_us > bus ? ln.blk_us - bus : 0u) + (isr > ln.blk_us ? isr - ln.blk_us : 0u);
        if (ln.blk_us) ln.post_pk = LN_HOLD(ln.post_pk, post);
        ln.isr_pk = LN_HOLD(ln.isr_pk, isr);
        ln.t_blk = LN_CLOCK();
        ln.t_bus = ln.t_blk;                    /* (a block with no bus call: its whole time counts as the rest) */
    }
#endif
#ifdef LN_PIPE
    {                                           /* the last block's mix end: done (or here), then out */
        uint32_t m = n < ln.pipe_n ? n : ln.pipe_n;
        if (ln.pipe == 2u && LN_PAR_WAIT()) {   /* refused, or lost mid-job: here (again) */
            ln_c1t.pipe_on = 0;
            ln.pipe_lost++;
            tx_mix_run(&ln.mix, ln.pipe_n, 0u);
        }
        if (!ln.pipe) m = 0;
        tx_mix_out(&ln.mix, L, R, m);
        for (k = m; k < n; k++) L[k] = R[k] = 0;
        ln.pipe = 0;
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
        tx_gov_extra(&ln.gov, LN_CLOCK_US(LN_CLOCK() - t0) + tx_fx_drive_mix_us(&ln.fx));   /* (the pool carries
                                                 * it, no lane does; and the mix's DRIVE, as it did on the lanes) */
#elif defined(LN_COST_EST)
        tx_gov_extra(&ln.gov, LN_EST_MOD + tx_fx_drive_mix_us(&ln.fx));
#endif
    }
#endif
    tx_mix_begin(&ln.mix, n);
    ln.ring_us = 0;
    if (ln.hurry) {                             /* (ln_hurry: a switch waits for the spare) */
        for (k = 0; k < LN_N; k++) if (tx_ring_holds(&ln_out[k].ring)) ln_out[k].ring.hold = 0;
        ln.hurry = 0;
    }
    for (k = 0; k < LN_N; k++) {                /* the generators, in lane order (their notes reach the modulation) */
#ifdef LN_CLOCK
        uint32_t t0 = LN_CLOCK();
#endif
        ln.lpk[k] = 0; ln.lev[k] = 0;
        if (ln.gen_on[k] && ln.mix.e[k]) {      /* the lane's generator: its activity and notes */
            tx_gen_ev_t ev[TX_GEN_EV_MAX];
            uint8_t in;
            unsigned ne, q;
#ifdef LN_GROOVE
            int32_t gm = gr_isr(k);             /* GROOVE (groove.c): the notes' rate leans to its pulse, ACTIVITY the
                                                 * same way; 4096: untouched */
            ln.gen[k].rlean = (int16_t)(gm - 4096);
#endif
            ne = tx_gen_block(&ln.gen[k], n, ev, TX_GEN_EV_MAX, &in);
#ifdef LN_GROOVE
            if (gm != 4096) in = gr_act(gm, in);
#endif
            if (ln.int_id[k] >= 0) ln.mix.e[k]->param(ln.mix.st[k], (uint8_t)ln.int_id[k], in);
            for (q = 0; q < ne && !ln.gen_mute[k]; q++) {
                ln.mix.e[k]->key(ln.mix.st[k], ev[q].note, ev[q].vel, ev[q].vel != 0);
#ifdef LN_MOD
                if (ev[q].vel) mod_key_isr(k, ev[q].vel);
                else mod_key_off_isr(k);
#endif
            }
        }
#ifdef LN_CLOCK
        ln.gus[k] = LN_CLOCK_US(LN_CLOCK() - t0);
#endif
    }
    {
        int gv = 0;                             /* the bus's begin: the grains' cap, and may their voices run now? */
#ifdef LN_BUS_TIMED
        gv = ln_bus_begin(&ln.fx, n);
#else
        gv = tx_fx_bus_begin(&ln.fx, n);
#endif
        ln_render(n, gv);                       /* every lane's engine, the grains' voices (some on the
                                                 * second core: LN_PAR) */
    }
    for (k = 0; k < LN_N; k++) {                /* lane by lane: its pan (tx_mix_slot_render's), its ring */
        ln_out_t *o = &ln_out[k];
        if (!ln.mix.e[k]) {                     /* an emptied lane: its ring alone, here */
            if (!tx_ring_on(&o->ring)) { o->rpk = 0; continue; }
            ln_ring_lane(o, n);
        }
        ln.mix.slot_peak[k] = 0;                /* (a peak a block: the lane's, its ring's under it) */
        tx_mix_add(&ln.mix, k, o->x, n);
        ln.lpk[k] = ln.mix.slot_peak[k];
        if (ln.mix.e[k] && ln.mix.e[k]->stats) {
            tx_stats_t st;
            ln.mix.e[k]->stats(ln.mix.st[k], &st);
            ln.lev[k] = st.events; ln.lhero[k] = st.quiet ? 0xFFFFu : st.particles;
        } else ln.lhero[k] = 0xFFFFu;
#ifdef LN_GOV
        {                                       /* (the fade below is not the lane's: as the player) */
#ifdef LN_CLOCK
            ln.gov_cost[k] = ln.mix.e[k] ? ln.gus[k] + ln_out[k].us : 0u;   /* (its generator and engine) */
#else
            ln.gov_cost[k] = ln_cost_est(k);
#endif
        }
#endif
        ln.gov_lvl[k] = (uint32_t)ln.mix.slot_peak[k];
#ifdef LN_CLOCK
        ln.ring_us += o->rus;                   /* (the ring's cost: the pool's, below) */
#elif defined(LN_COST_EST)
        if (o->rhp) ln.ring_us += LN_EST_BASE + LN_EST_HERO * o->rhp;
#endif
        if (o->ring.st && !tx_ring_holds(&o->ring)) {   /* the ring done: its arena free again (a switch's: the spare) */
            if (o->ring.st == ln_arena[ln.spare]) ln.spare_busy = 0;
            o->ring.st = 0;
            ln.busy[k] = 0;
        }
    }
#ifdef LN_GOV
    {                                           /* the governor: over the pool, the quietest lane gives a hero */
        uint8_t on[LN_N];
        int pk;
        uint32_t ex;
        for (k = 0; k < LN_N; k++) on[k] = ln.mix.e[k] != 0;
#ifdef LN_PAR
        ln.gov.budget = LN_PAR_ON() ? 2u * LN_GOV_POOL_US : LN_GOV_POOL_US;
#endif
        ex = ln.gov.extra;
        ln.gov.extra = ex + ln.ring_us;         /* (a ringing engine: the pool carries it, no lane's heroes pay) */
        pk = tx_gov_block(&ln.gov, ln.gov_cost, ln.gov_lvl, on);
        ln.gov.extra = ex;
        if (pk >= 0 && ln.mix.e[pk]) ln.mix.e[pk]->param(ln.mix.st[pk], TX_PID_PARTICLES, ln.gov.cur[pk]);
    }
#endif
#ifdef LN_PIPE
    ln.pipe_n = n;                              /* the mix end: the second core's, between the interrupts */
    if (LN_PAR_ON() && LN_PAR_RUN(ln_pipe_job, 0u)) ln.pipe = 2u;
    else { tx_mix_run(&ln.mix, n, 0u); ln.pipe = 1u; }
#else
    tx_mix_end(&ln.mix, L, R, n);
#endif
#ifdef LN_BUS_TIMED
    ln.blk_us = LN_CLOCK_US(LN_CLOCK() - ln.t_blk);
#endif
}
