/* SPDX-License-Identifier: GPL-3.0-only */
/* txfm1's runner (txfm1_core.h): the same code times the table on the FM-1 and on the host. */
#include "txfm1_core.h"
#include "tx_fx.h"
#include "tx_core_mix.h"

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
};

uint32_t fm_run_bytes(void) { return (uint32_t)sizeof(fm_run_t); }
const fm_stats_t *fm_stats(const fm_run_t *r) { return &r->s; }

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

/* the lanes' bus: a ROOM reverb, the grain delay (16 grains, pitched) and a drive + filter on every lane */
static void lanes_fx(tx_fx_t *fx)
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
    else if (c->kind == FM_K_FX) fx_setup(&r->fx, c->eng, c->type, c->decay);
    else {
        lanes_fx(&r->fx);
        tx_mix_init(&r->mix);
        r->mix.ins = tx_fx_lane; r->mix.bus = tx_fx_bus; r->mix.busq = tx_fx_bus_quiet; r->mix.fx = &r->fx;
        for (k = 0; k < c->eng; k++) { scene_init(r, k, &r->tab[c->lane[k]]); tx_mix_pan(&r->mix, k, 20u + 30u * k); }
    }
}

void fm_begin(fm_run_t *r, const fm_cfg_t *tab, const fm_ev_t *ev, unsigned cfg, unsigned warm)
{
    unsigned i, k;
    r->tab = tab; r->ev = ev; r->c = &tab[cfg];
    r->nblk = r->c->nblk;
    r->warm = warm ? (FM_WARM < r->nblk ? FM_WARM : r->nblk) : 0u;
    for (i = 0; i < 8u; i++) {
        r->s.ring[i] = r->s.rraw[i] = 0;
        for (k = 0; k < 4u; k++) r->s.lane_ring[k][i] = 0;
    }
    r->s.worst = r->s.w8 = r->s.sum = r->s.raw_worst = r->s.raw_w8 = r->s.n = 0;
    for (k = 0; k < 4u; k++) r->s.lane_worst[k] = r->s.lane_w8[k] = 0;
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
    } else if (c->kind == FM_K_FX) {
        if (c->eng >= FM_FX_REVERB) tx_fx_bus(&r->fx, L, R, TX_BLOCK); else tx_fx_lane(&r->fx, 0, L, TX_BLOCK);
    } else {
        tx_mix_begin(&r->mix, TX_BLOCK);
        for (k = 0; k < c->eng; k++) {
            const fm_cfg_t *lc = &r->tab[c->lane[k]];
            uint32_t a = ck->now(), an = ck->nested(), ld;
            scene_events(r, k, lc, r->blk);
            for (j = 0; j < TX_BLOCK; j++) r->buf[k][j] = 0;
            tx_engines[lc->eng]->render(r->arena[k], r->buf[k], TX_BLOCK);
            ld = (ck->now() - a) - (ck->nested() - an);
            if (measuring) {
                if (ld > r->s.lane_worst[k]) r->s.lane_worst[k] = ld;
                ring8(r->s.lane_ring[k], r->s.n, ld, &r->s.lane_w8[k]);
            }
            tx_fx_lane(&r->fx, k, r->buf[k], TX_BLOCK);          /* the lane's inserts (tx_mix_slot_render's ins) */
            tx_mix_add(&r->mix, k, r->buf[k], TX_BLOCK);
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
    r->s.sum += d;
    {
        uint32_t h = r->s.hash;
        for (j = 0; j < TX_BLOCK; j++) {
            uint32_t v = (uint32_t)L[j];
            if (c->kind != FM_K_SCENE) v ^= (uint32_t)R[j] * 16777619u;
            for (k = 0; k < 4u; k++) { h ^= (v >> (8u * k)) & 0xFFu; h *= 16777619u; }
        }
        r->s.hash = h;
    }
    r->s.n++;
    return r->s.n < r->nblk;
}
