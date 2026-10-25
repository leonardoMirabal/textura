/* SPDX-License-Identifier: GPL-3.0-only */
/* The firmware's lanes (firmware/src/lanes.c) against the audition player's chain (audition/tx_web.c's render, written
 * out here the same way): the same engines, keys, parameters, effects and engine switches, block by block. Every
 * sample must match; so must the queue's count of lost events (0). Exit code = the failures.
 *
 *   make fw-test (or: cc -Iengines -Ifirmware/src tests/lanes_test.c ENGINES -lm) */
#include <stdio.h>
#include <string.h>
#include "../firmware/src/lanes.c"

/* ---- the reference: the player's chain, inline (txw_select, txw_key_slot, txw_param, txw_fx_param, txw_render) */
static tx_mix_t rm;
static tx_fx_t rfx;
static uint8_t ra[TX_MIX_SLOTS][2][TX_STATE_MAX] __attribute__((aligned(64)));
static int rside[TX_MIX_SLOTS];
static const tx_engine_t *rret_e[TX_MIX_SLOTS];
static void *rret_st[TX_MIX_SLOTS];
static int32_t rret_g[TX_MIX_SLOTS], rret_last[TX_MIX_SLOTS], rdc[TX_MIX_SLOTS], rtmp[TX_BLOCK];

static void r_init(void)
{
    tx_mix_init(&rm);
    tx_fx_init(&rfx); rm.ins = tx_fx_lane; rm.bus = tx_fx_bus; rm.busq = tx_fx_bus_quiet; rm.fx = &rfx;
}
static void r_select(int k, const tx_engine_t *e, uint32_t seed)
{
    if (rm.e[k]) {
        if (rret_g[k]) rdc[k] += rret_last[k];
        rret_e[k] = rm.e[k]; rret_st[k] = rm.st[k]; rret_g[k] = 32768;
        rside[k] ^= 1;
    }
    if (!e) { tx_mix_set(&rm, (unsigned)k, 0, 0, 0); return; }
    memset(ra[k][rside[k]], 0, TX_STATE_MAX);
    tx_mix_set(&rm, (unsigned)k, e, ra[k][rside[k]], seed);
}
static void r_block(int32_t *L, int32_t *R)
{
    unsigned k;
    int j;
    tx_mix_begin(&rm, TX_BLOCK);
    for (k = 0; k < TX_MIX_SLOTS; k++) {
        if (!rm.e[k] && !rret_g[k] && !rdc[k]) continue;
        tx_mix_slot_render(&rm, k, TX_BLOCK);
        if (rret_g[k] || rdc[k]) {
            int32_t g = rret_g[k], d = rdc[k], last = 0;
            for (j = 0; j < (int)TX_BLOCK; j++) rtmp[j] = 0;
            if (g) rret_e[k]->render(rret_st[k], rtmp, TX_BLOCK);
            for (j = 0; j < (int)TX_BLOCK; j++) {
                int32_t gj = g - (j * 8192) / (int)TX_BLOCK;
                last = (int32_t)(((int64_t)rtmp[j] * gj) >> 15);
                rtmp[j] = last + d;
                d -= d >> 6;
                if (d > -64 && d < 64) d = 0;
            }
            rret_last[k] = g > 8192 ? last : 0;
            rret_g[k] = g > 8192 ? g - 8192 : 0;
            rdc[k] = d;
            tx_mix_add(&rm, k, rtmp, TX_BLOCK);
        }
    }
    tx_mix_end(&rm, L, R, TX_BLOCK);
}

/* ---- the scene: at block b, do this (to both) */
typedef struct { uint32_t b; char op; int k; const char *eng; int a, v, c; } step_t;
static const step_t SCENE[] = {
    {0, 'S', 0, "source", 0, 0, 0},             /* lane 1: the source */
    {0, 'K', 0, 0, 60, 100, 1},
    {20, 'S', 1, "surf", 0, 0, 0},              /* lane 2: a classic engine */
    {20, 'K', 1, 0, 64, 90, 1},
    {40, 'P', 0, 0, 16, 106, 0},                /* the source's COUNT (id 16): 32 */
    {60, 'F', 2, 0, 0, 2, 0},                   /* REVERB TYPE hall */
    {60, 'F', 2, 0, 1, 60, 0},                  /* its MIX */
    {80, 'F', 0, 0, 0, 60, 1},                  /* lane 2's DRIVE AMOUNT */
    {90, 'F', 1, 0, 0, 1, 0},                   /* lane 1's FILTER TYPE low-pass */
    {100, 'F', 3, 0, 0, 80, 0},                 /* the GRAIN DELAY's MIX */
    {120, 'L', 0, 0, 0, 70, 0},                 /* lane 1's LEVEL */
    {130, 'N', 1, 0, 20, 0, 0},                 /* lane 2's pan */
    {150, 'S', 0, "rainwater", 0, 0, 0},        /* a switch on lane 1: the source fades out over 4 blocks */
    {151, 'K', 0, 0, 62, 110, 1},
    {200, 'K', 0, 0, 62, 0, 0},
    {220, 'S', 1, 0, 0, 0, 0},                  /* lane 2 off: its tail fades */
    {300, 'S', 1, "fire", 0, 0, 0},
    {300, 'K', 1, 0, 60, 100, 1},
    {400, 0, 0, 0, 0, 0, 0},
};

int main(void)
{
    static int32_t L[TX_BLOCK], R[TX_BLOCK], rL[TX_BLOCK], rR[TX_BLOCK];
    uint32_t b, s = 0, seed = 12345u, fails = 0, j, loud = 0;
    ln_init();
    r_init();
    for (b = 0; SCENE[s].op; b++) {
        while (SCENE[s].op && SCENE[s].b == b) {
            const step_t *t = &SCENE[s++];
            const tx_engine_t *e = t->eng ? tx_engine_find(t->eng) : 0;
            switch (t->op) {
            case 'S':
                if (t->eng && !e) { printf("FAIL no engine %s\n", t->eng); return 1; }
                seed = seed * 1664525u + 1013904223u;
                if (ln_select((uint32_t)t->k, e, seed)) { printf("FAIL ln_select busy at block %u\n", b); fails++; }
                r_select(t->k, e, seed);
                break;
            case 'K':
                ln_key((uint32_t)t->k, (uint32_t)t->a, (uint32_t)t->v, (uint32_t)t->c);
                rm.e[t->k]->key(rm.st[t->k], (uint8_t)t->a, (uint8_t)t->v, (uint8_t)t->c);
                break;
            case 'P':
                ln_param((uint32_t)t->k, (uint32_t)t->a, (uint32_t)t->v);
                rm.e[t->k]->param(rm.st[t->k], (uint8_t)t->a, (uint8_t)t->v);
                break;
            case 'L':
                ln_level((uint32_t)t->k, (uint32_t)t->v);
                rm.e[t->k]->param(rm.st[t->k], TX_PID_LEVEL, (uint8_t)t->v);
                break;
            case 'N':
                ln_pan((uint32_t)t->k, (uint32_t)t->a);
                tx_mix_pan(&rm, (unsigned)t->k, (unsigned)t->a);
                break;
            case 'F':                           /* group k: 0 drive, 1 filter (lane c), 2 reverb, 3 grain */
                ln_fx((uint32_t)t->k, (uint32_t)t->c, (uint32_t)t->a, (uint32_t)t->v);
                if (t->k == 0) tx_fx_drive_param(&rfx.drive[t->c], (unsigned)t->a, (unsigned)t->v);
                else if (t->k == 1) tx_fx_filter_param(&rfx.filter[t->c], (unsigned)t->a, (unsigned)t->v);
                else if (t->k == 2) tx_fx_reverb_param(&rfx.rev, (unsigned)t->a, (unsigned)t->v);
                else tx_fx_grain_param(&rfx.gd, (unsigned)t->a, (unsigned)t->v);
                break;
            }
        }
        ln_block(L, R, TX_BLOCK);
        r_block(rL, rR);
        for (j = 0; j < TX_BLOCK; j++) {
            if (L[j] != rL[j] || R[j] != rR[j]) {
                if (fails < 5u) printf("FAIL block %u frame %u: %d %d vs the player's %d %d\n", b, j, L[j], R[j], rL[j], rR[j]);
                fails++;
                break;
            }
            if (L[j] > 1000 || L[j] < -1000) loud = 1;
        }
    }
    if (ln.dropped) { printf("FAIL %u events dropped\n", ln.dropped); fails++; }
    if (!loud) { printf("FAIL the scene was silent\n"); fails++; }
    printf("%s lanes: %u blocks, the firmware's lanes = the player's chain\n", fails ? "FAIL" : "ok  ", b);
    return fails ? 1 : 0;
}
