/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura lanes: the sound behind the panel. Four lanes, each an engine instance (engines/: the same code the bench
 * and the audition player run), each through its own DRIVE and FILTER, panned into a stereo pair, then the GRAIN
 * DELAY, the REVERB and the output limiter (tx_core_mix.h, tx_fx.h): the audition player's chain (audition/tx_web.c).
 *
 *   main loop   ln_select (a lane's engine: prepared in the lane's spare arena, here, never in the audio interrupt),
 *               ln_key, ln_param, ln_level, ln_pan, ln_fx: queued (ln_q), the interrupt takes them at its next block
 *   interrupt   ln_block: the queue, then one block of the four lanes and the effects, Q23 stereo
 *
 * A switched-out engine fades over 4 blocks (23 ms) in its own arena while the new one starts in the other, as the
 * player does, so a switch never clicks. This file knows nothing of the FM-1: tests/lanes_test.c runs it beside the
 * player's chain and checks they render the same samples. Integers only (as engines/). */
#include "tx_engine.h"
#include "tx_core_mix.h"
#include "tx_fx.h"

#define LN_N TX_MIX_SLOTS                       /* lanes */
#define LN_QN 128u                              /* queued events (a power of two) */

enum { LN_EV_KEY, LN_EV_PARAM, LN_EV_PAN, LN_EV_SWAP, LN_EV_OFF, LN_EV_FX };
enum { LN_FX_DRIVE, LN_FX_FILTER, LN_FX_REVERB, LN_FX_GRAIN };   /* ln_fx groups (as the player's txw_fx_param) */
typedef struct { uint8_t kind, lane, a, b, c; } ln_ev_t;

static struct {
    tx_mix_t mix;
    tx_fx_t fx;
    uint8_t side[LN_N];                         /* which of the lane's two arenas holds its engine */
    const tx_engine_t *next_e[LN_N];            /* prepared by ln_select, taken by LN_EV_SWAP */
    const tx_engine_t *ret_e[LN_N];             /* the switched-out engine, fading */
    void *ret_st[LN_N];
    int32_t ret_g[LN_N], ret_last[LN_N], dc[LN_N];
    volatile uint8_t busy[LN_N];                /* a switch is under way (main loop: wait before the next) */
    ln_ev_t q[LN_QN];
    volatile uint32_t qw, qr;
    uint32_t dropped;                           /* events lost to a full queue (a test reads it: must stay 0) */
    int32_t tmp[TX_BLOCK];
} ln;
static uint8_t ln_arena[LN_N][2][TX_STATE_MAX] __attribute__((aligned(64)));

static void ln_init(void)
{
    uint8_t *b = (uint8_t *)&ln;
    uint32_t i;
    for (i = 0; i < sizeof ln; i++) b[i] = 0;
    tx_mix_init(&ln.mix);
    tx_fx_init(&ln.fx);
    ln.mix.ins = tx_fx_lane; ln.mix.bus = tx_fx_bus; ln.mix.busq = tx_fx_bus_quiet; ln.mix.fx = &ln.fx;
}

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
/* lane k's engine: e (0: the lane off). The new engine is set up here, in the lane's free arena; the interrupt swaps
 * it in at its next block. Returns -1 while the lane's last switch is still fading (call again later). */
static int ln_select(uint32_t k, const tx_engine_t *e, uint32_t seed)
{
    uint8_t *st;
    uint32_t i;
    if (k >= LN_N) return -1;
    if (ln.busy[k]) return -1;
    if (!e) { ln.busy[k] = 1; if (ln_q(LN_EV_OFF, k, 0, 0, 0)) { ln.busy[k] = 0; return -1; } return 0; }
    if (e->state_bytes > TX_STATE_MAX) return -1;
    st = ln_arena[k][ln.mix.e[k] ? ln.side[k] ^ 1u : ln.side[k]];
    for (i = 0; i < TX_STATE_MAX; i++) st[i] = 0;
    e->init(st, seed);
    ln.next_e[k] = e;
    ln.busy[k] = 1;
    if (ln_q(LN_EV_SWAP, k, 0, 0, 0)) { ln.busy[k] = 0; return -1; }
    return 0;
}
static const tx_engine_t *ln_engine(uint32_t k) { return k < LN_N ? ln.mix.e[k] : 0; }
static int ln_key(uint32_t k, uint32_t note, uint32_t vel, uint32_t on) { return ln_q(LN_EV_KEY, k, note, vel, on != 0); }
static int ln_param(uint32_t k, uint32_t id, uint32_t v) { return ln_q(LN_EV_PARAM, k, id, v, 0); }   /* id: the engine's, or TX_PID_* */
static int ln_level(uint32_t k, uint32_t v) { return ln_param(k, TX_PID_LEVEL, v); }
static int ln_pan(uint32_t k, uint32_t v) { return ln_q(LN_EV_PAN, k, v, 0, 0); }
static int ln_fx(uint32_t g, uint32_t k, uint32_t id, uint32_t v) { return ln_q(LN_EV_FX, k, g, id, v); }

/* ---- the interrupt side */
static void ln_event(const ln_ev_t *v)
{
    uint32_t k = v->lane;
    switch (v->kind) {
    case LN_EV_KEY:
        if (k < LN_N && ln.mix.e[k]) ln.mix.e[k]->key(ln.mix.st[k], v->a, v->b, v->c);
        break;
    case LN_EV_PARAM:
        if (k < LN_N && ln.mix.e[k]) ln.mix.e[k]->param(ln.mix.st[k], v->a, v->b);
        break;
    case LN_EV_PAN:
        if (k < LN_N) tx_mix_pan(&ln.mix, k, v->a);
        break;
    case LN_EV_SWAP:
    case LN_EV_OFF:
        if (k >= LN_N) break;
        if (ln.mix.e[k]) {                      /* the old engine fades out in its arena */
            if (ln.ret_g[k]) ln.dc[k] += ln.ret_last[k];   /* one still fading: its last sample decays instead */
            ln.ret_e[k] = ln.mix.e[k]; ln.ret_st[k] = ln.mix.st[k]; ln.ret_g[k] = 32768;
            if (v->kind == LN_EV_SWAP) ln.side[k] ^= 1u;
        }
        ln.mix.e[k] = v->kind == LN_EV_SWAP ? ln.next_e[k] : 0;   /* (initialised by ln_select: tx_mix_set's part) */
        ln.mix.st[k] = v->kind == LN_EV_SWAP ? ln_arena[k][ln.side[k]] : 0;
        ln.mix.slot_peak[k] = 0;
        if (!ln.ret_g[k]) ln.busy[k] = 0;
        break;
    case LN_EV_FX:
        if (v->a == LN_FX_DRIVE && k < TX_FX_LANES) tx_fx_drive_param(&ln.fx.drive[k], v->b, v->c);
        else if (v->a == LN_FX_FILTER && k < TX_FX_LANES) tx_fx_filter_param(&ln.fx.filter[k], v->b, v->c);
        else if (v->a == LN_FX_REVERB) tx_fx_reverb_param(&ln.fx.rev, v->b, v->c);
        else if (v->a == LN_FX_GRAIN) tx_fx_grain_param(&ln.fx.gd, v->b, v->c);
        break;
    }
}

/* one block (n <= TX_BLOCK frames): L, R Q23, limited to -1 dBFS */
static void ln_block(int32_t *L, int32_t *R, uint32_t n)
{
    uint32_t k, j;
    while (ln.qr != ln.qw) { ln_event(&ln.q[ln.qr % LN_QN]); ln.qr++; }
    tx_mix_begin(&ln.mix, n);
    for (k = 0; k < LN_N; k++) {
        if (!ln.mix.e[k] && !ln.ret_g[k] && !ln.dc[k]) continue;
        tx_mix_slot_render(&ln.mix, k, n);
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
            if (!ln.ret_g[k]) ln.busy[k] = 0;   /* the arena is free again */
        }
    }
    tx_mix_end(&ln.mix, L, R, n);
}
