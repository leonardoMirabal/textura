/* SPDX-License-Identifier: GPL-3.0-only */
/* txfm1's runner (txfm1_core.h): the same code times the table on the FM-1 and on the host. */
#include "txfm1_core.h"
#include "tx_fx.h"
#include "tx_core_mix.h"
#include "tx_tables.h"

extern const tx_engine_t *const tx_engines[];

struct fm_run_s {
    uint8_t arena[4][TX_STATE_MAX] __attribute__((aligned(8)));   /* one per lane (a scene uses the first) */
    tx_fx_t fx;
    tx_mix_t mix;
    const fm_cfg_t *tab, *c;
    const fm_ev_t *ev;
    uint32_t blk, nblk, warm, rng;
    uint16_t next[4];
    fm_stats_t s;
    int32_t buf[4][TX_BLOCK];
    const fm_par_t *par;
    struct { const tx_engine_t *e; void *st; int32_t *out; uint8_t ping; } job;   /* C1: core 1's render */
    struct { int32_t x[TX_BLOCK]; uint32_t t; } __attribute__((aligned(64))) lb[4];   /* LANES: each lane's engine block
                                    * and its time (ticks), written by the core that renders it (its own lines) */
    uint8_t pmask;                                           /* LANES FM_PAR: core 1's items this block (the lanes,
                                                              * FM_GV, FM_GV + 1: the grain delay's voices' halves) */
    struct { uint32_t t[2], busy; } __attribute__((aligned(64))) gv1;   /* core 1's own line: its voices', its job's
                                                              * ticks */
    uint32_t gvt[2];                                         /* the voices' halves' last times, ticks */
    uint8_t c1_lost;                                         /* C1: core 1's copy lost (held mid-job): core 0's only */
};

uint32_t fm_run_bytes(void) { return (uint32_t)sizeof(fm_run_t); }
typedef char fm_run_fits[sizeof(fm_run_t) <= FM_RUN_MAX ? 1 : -1];   /* (FM_RUN_MAX: the pool's share) */
const fm_stats_t *fm_stats(const fm_run_t *r) { return &r->s; }
void fm_set_par(fm_run_t *r, const fm_par_t *p) { r->par = p; }

/* the ping (C1, TYPE 1): no engine, a pattern from the block number (core 1 runs any code at all?) */
static void c1_fill(int32_t *out, uint32_t blk, unsigned kind, const int32_t *in, void *st)
{
    uint32_t j;
    if (kind == 5u) {                  /* 5: load: a pointer chase with read-modify-writes over its own 16 KB arena,
                                        * 16384 steps a block (both cores busy on the bus at once, no engine code) */
        uint32_t *w = (uint32_t *)st, i = blk & 4095u, a = blk, k;
        for (k = 0; k < 16384u; k++) {
            uint32_t v = w[i];
            w[i] = v * 1664525u + 1013904223u + k;
            a = (a ^ v) * 0x9E3779B1u + (a >> 7);
            i = (v ^ a ^ k) & 4095u;
            if ((k & 63u) == 63u) out[k >> 6] = (int32_t)a >> 9;
        }
        return;
    }
    if (kind == 3u) {                  /* 3: flash data (a constant table read through the XIP) */
        for (j = 0; j < TX_BLOCK; j++) out[j] = (int32_t)TX_LOG2_FRAC[(blk * 7u + j * 13u) % 257u] << 4;
        return;
    }
    if (kind == 4u) {                  /* 4: data core 0 wrote just before the job (in: buf[2]) */
        for (j = 0; j < TX_BLOCK; j++) out[j] = in[j] ^ (int32_t)j;
        return;
    }
    if (kind == 1u) {
        for (j = 0; j < TX_BLOCK; j++) out[j] = (int32_t)((blk * 0x9E3779B1u) ^ (j * 0x85EBCA6Bu)) >> 9;
        return;
    }
    for (j = 0; j < TX_BLOCK; j++) {   /* 2: the math ping: 64-bit products and 32-bit divisions, as the engines' */
        uint32_t a = blk * 0x9E3779B1u + j, b = j * 0x85EBCA6Bu + 12345u;
        int64_t p = (int64_t)(int32_t)a * (int32_t)b;
        uint64_t q = (uint64_t)a * b;
        int32_t d = (int32_t)a / (int32_t)((b >> 8) | 1u);
        uint32_t u = a / ((b >> 12) | 1u);
        out[j] = (int32_t)((uint32_t)(p >> 31) ^ (uint32_t)(q >> 32) ^ (uint32_t)d ^ u) >> 9;
    }
}

/* core 1's work (C1): its copy of the scene (or the ping), one block; c1_job is the same on core 1 (its argument the
 * run: a 32-bit address, the device's only) */
static void c1_work(fm_run_t *r)
{
    if (r->job.ping) c1_fill(r->job.out, r->blk, r->job.ping, r->buf[2], r->job.st); else r->job.e->render(r->job.st, r->job.out, TX_BLOCK);
}
static void c1_job(uint32_t a) { c1_work((fm_run_t *)(uintptr_t)a); }

/* LANES: lane k's engine, one block, into lb[k] (either core; ticks from now(), core 0 less its nested interrupts).
 * The run and the clock for core 1's job: set by core 0 before it hands the job over (its argument: the block) */
static uint32_t (*lane_now)(void);
static fm_run_t *lane_run;
#define FM_GV 4u                                             /* the split's items past the lanes: the grain voices'
                                                              * halves (the echo and the first, the second) */
static void lane_render(fm_run_t *r, unsigned k, unsigned core)   /* its engine, then its inserts (prep done) */
{
    uint32_t j, a = lane_now();
    for (j = 0; j < TX_BLOCK; j++) r->lb[k].x[j] = 0;
    tx_engines[r->tab[r->c->lane[k]].eng]->render(r->arena[k], r->lb[k].x, TX_BLOCK);
    tx_fx_lane_run(&r->fx, k, r->lb[k].x, TX_BLOCK, core);
    r->lb[k].t = lane_now() - a;
}
static uint32_t gv_run(fm_run_t *r, unsigned h)             /* the grain delay's voices, half h, ticks */
{
    uint32_t a = lane_now();
    if (h) TX_FAR(tx_fx_grain_voices_b)(&r->fx.gd, TX_BLOCK);
    else TX_FAR(tx_fx_grain_voices_a)(&r->fx.gd, TX_BLOCK);
    return lane_now() - a;
}
static void lanes_job(uint32_t a)                            /* core 1's: its items (pmask) */
{
    fm_run_t *r = lane_run;
    unsigned k;
    uint32_t t0 = lane_now();
    (void)a;
    for (k = 0; k < 2u; k++) if (r->pmask >> (FM_GV + k) & 1u) r->gv1.t[k] = gv_run(r, k);
    for (k = 0; k < r->c->eng; k++) if (r->pmask >> k & 1u) lane_render(r, k, 1u);
    r->gv1.busy = lane_now() - t0;
}
/* the reverb's ROOM on two cores (FM_PAR, as firmware/src/lanes.c): its body (the CHAMBER) on core 1, the tail here */
static void rev_job(uint32_t a) { (void)a; TX_FAR(tx_fx_reverb_body)(&lane_run->fx.rev); }
static int rev_run(void) { fm_run_t *r = lane_run; return r->par && !r->c1_lost && r->par->run(rev_job, r->blk); }
static int rev_wait(void)
{
    fm_run_t *r = lane_run;
    if (!r->par->wait()) return 0;
    r->c1_lost = 1;                                          /* (held mid-job: the body runs here, the run stays on one core) */
    return 1;
}
/* the reverb kernels' halves on core 1 (tx_fx_reverb_t.par, as firmware/src/lanes.c): one job, its function and argument */
static void (*rev_kfn)(void *);
static void *rev_karg;
static void rev_kjob(uint32_t a) { (void)a; rev_kfn(rev_karg); }
static int rev_krun(void (*fn)(void *), void *a)
{
    fm_run_t *r = lane_run;
    rev_kfn = fn; rev_karg = a;
    return r->par && !r->c1_lost && r->par->run(rev_kjob, r->blk);
}
/* core 1's items among on: the split whose larger half took least in the last block (the last split on a tie) */
static uint32_t lanes_split(const fm_run_t *r, uint32_t on)
{
    uint32_t m, k, best = 0, bc = 0xFFFFFFFFu;
    for (m = 1; m < (4u << FM_GV) - 1u; m++) {
        uint32_t c0 = 0, c1 = 0, c;
        if ((m & on) != m || m == on) continue;
        for (k = 0; k <= FM_GV + 1u; k++) if (on >> k & 1u) {
            uint32_t ck = (k >= FM_GV ? r->gvt[k - FM_GV] : r->lb[k].t) + 1u;
            if (m >> k & 1u) c1 += ck; else c0 += ck;
        }
        c = c0 > c1 ? c0 : c1;
        if (c < bc || (c == bc && m == r->pmask)) { bc = c; best = m; }
    }
    return best;
}

static void fx_setup(tx_fx_t *fx, unsigned kind, unsigned type, unsigned decay)
{
    tx_fx_init(fx);
    if (kind == FM_FX_DRIVE) {
        tx_fx_drive_param(&fx->drive[0], TX_DR_AMOUNT, 90); tx_fx_drive_param(&fx->drive[0], TX_DR_TONE, 100);
        tx_fx_drive_param(&fx->drive[0], TX_DR_MIX, type ? 127 : 100); tx_fx_drive_param(&fx->drive[0], TX_DR_TYPE, type);
    }
    if (kind == FM_FX_FILTER) {
        tx_fx_filter_param(&fx->filter[0], TX_FL_TYPE, 2); tx_fx_filter_param(&fx->filter[0], TX_FL_RESO, 90);
        tx_fx_filter_param(&fx->filter[0], TX_FL_LFO, 90); tx_fx_filter_param(&fx->filter[0], TX_FL_FREQ, 70);
    }
    if (kind == FM_FX_REVERB) {
        tx_fx_reverb_param(&fx->rev, TX_RV_TYPE, type); tx_fx_reverb_param(&fx->rev, TX_RV_MIX, 90);
        tx_fx_reverb_param(&fx->rev, TX_RV_DECAY, decay < 127 ? decay : 100);
        tx_fx_reverb_param(&fx->rev, TX_RV_PREDELAY, 40); tx_fx_reverb_param(&fx->rev, TX_RV_LOWCUT, 30);
        if (decay == 1) { tx_fx_reverb_param(&fx->rev, TX_RV_DECAY, 64); tx_fx_reverb_param(&fx->rev, TX_RV_SHIMMER, 127); }
    }
    if (kind == FM_FX_GRAIN) {
        fx->reserve_us = 100000u;                            /* (no cap: the cost of every grain asked, as tx-budget) */
        tx_fx_grain_param(&fx->gd, TX_GD_MIX, 100); tx_fx_grain_param(&fx->gd, TX_GD_GRAINS, type - 1u);
        tx_fx_grain_param(&fx->gd, TX_GD_PITCH, decay ? 71 : 64); tx_fx_grain_param(&fx->gd, TX_GD_FEEDBACK, 70);
        tx_fx_grain_param(&fx->gd, TX_GD_SPRAY, 30);
    }
}

/* the lanes' bus: a ROOM reverb, the grain delay (16 grains, pitched) and a drive + filter on every lane; off: the
 * effects left out (FM_NO_*) */
static void lanes_fx(tx_fx_t *fx, unsigned off)
{
    unsigned k;
    tx_fx_init(fx);
    tx_fx_reverb_param(&fx->rev, TX_RV_TYPE, TX_RVT_ROOM); tx_fx_reverb_param(&fx->rev, TX_RV_MIX, 60);
    tx_fx_reverb_param(&fx->rev, TX_RV_DECAY, 100);
    tx_fx_grain_param(&fx->gd, TX_GD_MIX, 80); tx_fx_grain_param(&fx->gd, TX_GD_GRAINS, 15);
    tx_fx_grain_param(&fx->gd, TX_GD_PITCH, 71); tx_fx_grain_param(&fx->gd, TX_GD_FEEDBACK, 70);
    for (k = 0; k < TX_FX_LANES; k++) {
        tx_fx_drive_param(&fx->drive[k], TX_DR_AMOUNT, 70); tx_fx_drive_param(&fx->drive[k], TX_DR_MIX, 100);
        tx_fx_filter_param(&fx->filter[k], TX_FL_TYPE, 2); tx_fx_filter_param(&fx->filter[k], TX_FL_FREQ, 90);
        if (off & FM_NO_DRIVE) tx_fx_drive_param(&fx->drive[k], TX_DR_AMOUNT, 0);
        if (off & FM_NO_FILTER) tx_fx_filter_param(&fx->filter[k], TX_FL_TYPE, 0);
    }
    if (off & FM_NO_GRAIN) tx_fx_grain_param(&fx->gd, TX_GD_MIX, 0);
    if (off & FM_NO_REVERB) {                                /* (the grains' cap as with the ROOM: its 390 us less) */
        tx_fx_reverb_param(&fx->rev, TX_RV_TYPE, TX_RVT_OFF); tx_fx_reverb_param(&fx->rev, TX_RV_MIX, 0);
        fx->reserve_us -= 390u;
    }
}

static void scene_init(fm_run_t *r, unsigned k, const fm_cfg_t *c)
{
    const tx_engine_t *e = tx_engines[c->eng];
    uint32_t i, *w = (uint32_t *)r->arena[k];
    for (i = 0; i < TX_STATE_MAX / 4u; i++) w[i] = 0;
    e->init(r->arena[k], c->seed);
    r->next[k] = 0;
}

static void scene_events(fm_run_t *r, unsigned k, const fm_cfg_t *c, uint32_t blk)
{
    const tx_engine_t *e = tx_engines[c->eng];
    while (r->next[k] < c->nev && r->ev[c->ev + r->next[k]].blk <= blk) {
        const fm_ev_t *v = &r->ev[c->ev + r->next[k]++];
        if (v->kind == FM_EV_ON) e->key(r->arena[k], v->a, v->b, 1);
        else if (v->kind == FM_EV_OFF) e->key(r->arena[k], v->a, 0, 0);
        else e->param(r->arena[k], v->id, v->a);
    }
}

static void start(fm_run_t *r)
{
    const fm_cfg_t *c = r->c;
    unsigned k;
    r->blk = 0;
    r->rng = 99u;
    if (c->kind == FM_K_SCENE) scene_init(r, 0, c);
    else if (c->kind == FM_K_C1) { scene_init(r, 0, c); scene_init(r, 1, c); r->c1_lost = 0; }
    else if (c->kind == FM_K_FX) {
        fx_setup(&r->fx, c->eng, c->type, c->decay);
        tx_comp_init(&r->mix.comp);
        if (c->eng == FM_FX_COMP) {                          /* TYPE 0: its defaults; 1: THR -40 dBFS, 20:1, ATK 0 */
            tx_comp_param(&r->mix.comp, TX_CP_ON, 1);
            if (c->type) { tx_comp_param(&r->mix.comp, TX_CP_THR, 127); tx_comp_param(&r->mix.comp, TX_CP_RATIO, 7); tx_comp_param(&r->mix.comp, TX_CP_ATK, 0); }
        }
    }
    else {
        lanes_fx(&r->fx, c->type);
        if (c->type & (FM_G15 | FM_G30)) r->fx.reserve_us = 100000u;   /* (the grains' cap never under those asked) */
        if (c->type & FM_G30) tx_fx_grain_param(&r->fx.gd, TX_GD_GRAINS, TX_GD_MAXG - 1u);   /* (30, pitched) */
        r->c1_lost = 0; r->pmask = 0; r->gvt[0] = r->gvt[1] = 0;
        for (k = 0; k < 4u; k++) r->lb[k].t = 0;
        tx_mix_init(&r->mix);
        r->mix.ins = tx_fx_lane; r->mix.busq = tx_fx_bus_quiet; r->mix.fx = &r->fx;
        r->mix.bus = tx_fx_bus_end;                         /* (its begin and the grain voices: fm_step) */
        if (c->type & FM_PAR) {
            r->fx.rev.body_run = rev_run; r->fx.rev.body_wait = rev_wait;
            r->fx.rev.par.run = rev_krun; r->fx.rev.par.wait = rev_wait;
        }
        for (k = 0; k < c->eng; k++) { scene_init(r, k, &r->tab[c->lane[k]]); tx_mix_pan(&r->mix, k, 20u + 30u * k); }
    }
}

void fm_begin(fm_run_t *r, const fm_cfg_t *tab, const fm_ev_t *ev, unsigned cfg, unsigned warm)
{
    unsigned i, k;
    r->tab = tab; r->ev = ev; r->c = &tab[cfg];
    r->par = 0;                                              /* (none until fm_set_par: the host's) */
    r->nblk = r->c->nblk;
    r->warm = warm ? (FM_WARM < r->nblk ? FM_WARM : r->nblk) : 0u;
    for (i = 0; i < 8u; i++) {
        r->s.ring[i] = r->s.rraw[i] = 0;
        for (k = 0; k < 4u; k++) r->s.lane_ring[k][i] = 0;
    }
    r->s.worst = r->s.w8 = r->s.sum = r->s.raw_worst = r->s.raw_w8 = r->s.n = 0;
    for (k = 0; k < 4u; k++) r->s.lane_worst[k] = r->s.lane_w8[k] = 0;
    r->s.c1_jobs = r->s.c1_mis = r->s.c1_first = r->s.c1_solo = 0;
    r->s.c0_worst = r->s.c1_worst = r->s.c0_sum = r->s.c1_sum = 0;
    r->s.hash = 2166136261u;
    start(r);
}

static uint32_t ring8(uint32_t *ring, uint32_t n, uint32_t v, uint32_t *w8)
{
    uint32_t i, a = 0;
    ring[n & 7u] = v;
    if (n >= 7u) {
        for (i = 0; i < 8u; i++) a += ring[i];
        a >>= 3;
        if (a > *w8) *w8 = a;
    }
    return a;
}

/* the clock probe: FM_CLK_OPS adds in one dependent chain (each waits for the last: one a cycle, none paired), 64 to
 * a pass (the loop's own steps under 2 %); the empty asm keeps the compiler from folding them */
#define CK1 x += 0x9E3779B9u; __asm__ volatile("" : "+r"(x));
#define CK8 CK1 CK1 CK1 CK1 CK1 CK1 CK1 CK1
static uint32_t clk_probe(uint32_t x)
{
    unsigned i;
    for (i = 0; i < FM_CLK_OPS / 64u; i++) { CK8 CK8 CK8 CK8 CK8 CK8 CK8 CK8 }
    return x;
}

int fm_step(fm_run_t *r, const fm_clock_t *ck, int32_t *L, int32_t *R)
{
    const fm_cfg_t *c = r->c;
    uint32_t t0, n0, d, raw, j, k;
    int measuring = r->warm == 0u;
    if (c->kind == FM_K_FX) {                                  /* a busy input: noise at -12 dBFS (tx-budget's) */
        for (j = 0; j < TX_BLOCK; j++) { r->rng = r->rng * 1664525u + 1013904223u; L[j] = R[j] = (int32_t)(r->rng >> 10) - (1 << 21); }
    }
    n0 = ck->nested(); t0 = ck->now();
    if (c->kind == FM_K_SCENE) {
        scene_events(r, 0, c, r->blk);
        for (j = 0; j < TX_BLOCK; j++) L[j] = 0;
        tx_engines[c->eng]->render(r->arena[0], L, TX_BLOCK);
    } else if (c->kind == FM_K_C1) {                           /* the same scene on both cores at once, compared */
        const tx_engine_t *e = tx_engines[c->eng];
        int on1 = 0;
        scene_events(r, 0, c, r->blk);
        if (!r->c1_lost) {
            scene_events(r, 1, c, r->blk);
            for (j = 0; j < TX_BLOCK; j++) r->buf[1][j] = 0;
            r->job.e = e; r->job.st = r->arena[1]; r->job.out = r->buf[1]; r->job.ping = c->type;
            if (c->type == 4u)                                   /* (the shared ping's fresh input) */
                for (j = 0; j < TX_BLOCK; j++) r->buf[2][j] = (int32_t)(r->blk * 0x9E3779B1u + j * 0x85EBCA6Bu) >> 9;
            on1 = r->par && r->par->run(c1_job, (uint32_t)(uintptr_t)r);
        }
        for (j = 0; j < TX_BLOCK; j++) L[j] = 0;
        if (c->type == 4u) for (j = 0; j < TX_BLOCK; j++) L[j] = ((int32_t)(r->blk * 0x9E3779B1u + j * 0x85EBCA6Bu) >> 9) ^ (int32_t)j;
        else if (c->type) c1_fill(L, r->blk, c->type, 0, r->arena[0]); else e->render(r->arena[0], L, TX_BLOCK);
        if (on1 && r->par->wait()) { on1 = 0; r->c1_lost = 1; }   /* (held mid-job: its copy is gone) */
        if (!r->c1_lost) {
            if (!on1) c1_work(r);
            for (j = 0; j < TX_BLOCK && r->buf[1][j] == L[j]; j++) ;
            if (measuring) {
                if (on1) r->s.c1_jobs++; else r->s.c1_solo++;
                if (j < TX_BLOCK && !r->s.c1_mis++) r->s.c1_first = r->s.n + 1u;
            }
        } else if (measuring) r->s.c1_solo++;
        for (j = 0; j < TX_BLOCK; j++) R[j] = L[j];
    } else if (c->kind == FM_K_FX) {
        if (c->eng == FM_FX_CLOCK) L[0] = R[0] = (int32_t)(clk_probe(r->blk) >> 9);   /* (its result: kept, hashed) */
        else if (c->eng == FM_FX_COMP) TX_FAR(tx_comp_process)(&r->mix.comp, L, R, TX_BLOCK); /* the mix's own */
        else if (c->eng >= FM_FX_REVERB) tx_fx_bus(&r->fx, L, R, TX_BLOCK); else tx_fx_lane(&r->fx, 0, L, TX_BLOCK);
    } else {                                                 /* the lanes: every lane's events, the bus's begin, the
                                                              * inserts' prep (lane order), then the engines with
                                                              * their inserts and the grain voices (FM_PAR: some on
                                                              * core 1), then lane by lane the pan, the bus's end:
                                                              * the same samples either way */
        uint32_t m1 = 0, on = (1u << c->eng) - 1u, t0, c0;
        tx_mix_begin(&r->mix, TX_BLOCK);
        lane_now = ck->now; lane_run = r;
        for (k = 0; k < c->eng; k++) scene_events(r, k, &r->tab[c->lane[k]], r->blk);
        if (tx_fx_bus_begin(&r->fx, TX_BLOCK)) {            /* (the grain voices may run now: in two halves) */
            tx_fx_grain_split(&r->fx.gd, TX_BLOCK); on |= 3u << FM_GV;
        }
        for (k = 0; k < c->eng; k++) tx_fx_lane_prep(&r->fx, k);
        if ((c->type & FM_PAR) && r->par && !r->c1_lost && (on & (on - 1u))) {
            m1 = lanes_split(r, on);
            r->pmask = (uint8_t)m1;
            if (!r->par->run(lanes_job, r->blk)) m1 = 0;
        }
        t0 = ck->now(); c0 = ck->nested();
        for (k = 0; k < 2u; k++) if ((on & ~m1) >> (FM_GV + k) & 1u) r->gvt[k] = gv_run(r, k);
        for (k = 0; k < c->eng; k++) if (!(m1 >> k & 1u)) {
            uint32_t n0 = ck->nested();
            lane_render(r, k, 0u);
            r->lb[k].t -= ck->nested() - n0;                 /* (core 0's: less its nested interrupts) */
        }
        c0 = (ck->now() - t0) - (ck->nested() - c0);         /* core 0's busy ticks in the render phase */
        if (m1 && r->par->wait()) {                          /* held mid-job: its items here, as it left them */
            if (measuring) { r->s.c1_mis++; if (!r->s.c1_first) r->s.c1_first = r->s.n + 1u; }
            r->c1_lost = 1;
            for (k = 0; k < 2u; k++) if (m1 >> (FM_GV + k) & 1u) r->gvt[k] = gv_run(r, k);
            for (k = 0; k < c->eng; k++) if (m1 >> k & 1u) lane_render(r, k, 0u);
            m1 = 0;
        }
        for (k = 0; k < 2u; k++) if (m1 >> (FM_GV + k) & 1u) r->gvt[k] = r->gv1.t[k];
        if (c->type & FM_PAR) {
            if (!m1) r->pmask = 0;
            if (measuring) {
                uint32_t c1 = m1 ? r->gv1.busy : 0u;
                if (m1) r->s.c1_jobs++; else r->s.c1_solo++;
                if (c0 > r->s.c0_worst) r->s.c0_worst = c0;
                if (c1 > r->s.c1_worst) r->s.c1_worst = c1;
                r->s.c0_sum += c0 >> 4; r->s.c1_sum += c1 >> 4;
            }
        }
        for (k = 0; k < c->eng; k++) {
            uint32_t ld = r->lb[k].t;
            if (measuring) {
                if (ld > r->s.lane_worst[k]) r->s.lane_worst[k] = ld;
                ring8(r->s.lane_ring[k], r->s.n, ld, &r->s.lane_w8[k]);
            }
            tx_mix_add(&r->mix, k, r->lb[k].x, TX_BLOCK);         /* (its inserts: lane_render) */
        }
        tx_mix_end(&r->mix, L, R, TX_BLOCK);
    }
    raw = ck->now() - t0;
    d = raw - (ck->nested() - n0);
    if (d > raw) d = raw;
    r->blk++;
    if (!measuring) {
        if (--r->warm == 0u) start(r);                         /* the warm-up's over: the measured run from the top */
        return 1;
    }
    if (d > r->s.worst) r->s.worst = d;
    if (raw > r->s.raw_worst) r->s.raw_worst = raw;
    ring8(r->s.ring, r->s.n, d, &r->s.w8);
    ring8(r->s.rraw, r->s.n, raw, &r->s.raw_w8);
    r->s.sum += d >> 4;                                      /* (ticks / 16: 32 bits hold a 300 s row) */
    {
        uint32_t h = r->s.hash;
        for (j = 0; j < TX_BLOCK; j++) {
            uint32_t v = (uint32_t)L[j];
            if (c->kind != FM_K_SCENE && c->kind != FM_K_C1) v ^= (uint32_t)R[j] * 16777619u;
            for (k = 0; k < 4u; k++) { h ^= (v >> (8u * k)) & 0xFFu; h *= 16777619u; }
        }
        r->s.hash = h;
    }
    r->s.n++;
    return r->s.n < r->nblk;
}
