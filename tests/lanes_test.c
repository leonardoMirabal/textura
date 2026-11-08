/* SPDX-License-Identifier: GPL-3.0-only */
/* The firmware's lanes (firmware/src/lanes.c) against the audition player's chain (audition/tx_web.c's render, written
 * out here the same way): the same engines, keys, parameters, effects and engine switches, block by block. Every
 * sample must match; so must the queue's count of lost events (0). Exit code = the failures.
 *
 * LANES_TEST_PAR: the same with a thread as the second core (lanes.c LN_PAR), its split changed every block (every
 * split of the lanes in turn, and the cost's), so the samples must not depend on which core renders which lane;
 * built with the thread sanitizer it also finds what the two would race on.
 *
 *   make fw-test (or: cc -Iengines -Ifirmware/src tests/lanes_test.c ENGINES -lm) */
#include <stdio.h>
#include <string.h>
#ifdef LANES_TEST_PAR
#include <pthread.h>
#include <stdint.h>
/* the second core: a thread that runs one job at a time (as firmware/src/core1.h's mailbox) */
static pthread_mutex_t par_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t par_cv = PTHREAD_COND_INITIALIZER;
static void (*par_fn)(uint32_t);
static uint32_t par_arg, par_seq, par_done, par_jobs, par_kt;
static void *par_main(void *u)
{
    (void)u;
    pthread_mutex_lock(&par_mx);
    for (;;) {
        while (par_done == par_seq) pthread_cond_wait(&par_cv, &par_mx);
        {
            void (*fn)(uint32_t) = par_fn;
            uint32_t a = par_arg;
            pthread_mutex_unlock(&par_mx);
            fn(a);
            pthread_mutex_lock(&par_mx);
        }
        par_done = par_seq;
        pthread_cond_broadcast(&par_cv);
    }
    return 0;
}
static void ln_kjob(uint32_t a);
static uint32_t par_rvt(void);
static uint32_t par_drv(void);
static void ln_rev_job(uint32_t a);
static uint32_t par_dt, par_rj;
static int par_run(void (*fn)(uint32_t), uint32_t a)
{
    if (fn == ln_rev_job) par_rj++;                       /* the reverb's ROOM body */
    uint32_t dt = fn == ln_kjob ? par_drv() : 0u;         /* the mix's DRIVE's R: the TYPEs it ran for */
    if (dt) par_dt |= dt;
    else if (fn == ln_kjob) par_kt |= 1u << par_rvt();   /* a reverb kernel's half: the TYPEs it ran for */
    pthread_mutex_lock(&par_mx);
    par_fn = fn; par_arg = a; par_seq++; par_jobs++;
    pthread_cond_broadcast(&par_cv);
    pthread_mutex_unlock(&par_mx);
    return 1;
}
static int par_wait(void)
{
    pthread_mutex_lock(&par_mx);
    while (par_done != par_seq) pthread_cond_wait(&par_cv, &par_mx);
    pthread_mutex_unlock(&par_mx);
    return 0;
}
#define LN_PAR
#define LN_PAR_ON() 1
#define LN_PAR_RUN(fn, a) par_run(fn, a)
#define LN_PAR_WAIT() par_wait()
#endif
#include "../firmware/src/lanes.c"
#ifdef LANES_TEST_PAR
static uint32_t par_rvt(void) { return ln.fx.rev.cur; }
static uint32_t par_drv(void) { return ln_karg == (void *)&ln.fx ? 1u << ln.fx.drive[1].cur : 0u; }
#endif

/* ---- the reference: the player's chain, inline (txw_select, txw_key_slot, txw_param, txw_fx_param, txw_render) */
static tx_mix_t rm;
static tx_fx_t rfx;
static uint8_t ra[TX_MIX_SLOTS][2][TX_STATE_MAX] __attribute__((aligned(64)));
static int rside[TX_MIX_SLOTS];
static const tx_engine_t *rret_e[TX_MIX_SLOTS];
static void *rret_st[TX_MIX_SLOTS];
static int32_t rret_g[TX_MIX_SLOTS], rret_last[TX_MIX_SLOTS], rdc[TX_MIX_SLOTS], rtmp[TX_BLOCK];
static tx_gen_t rgen[TX_MIX_SLOTS];                 /* the lanes' generators (txw_gen_on, txw_gen_param, txw_gen_mute) */
static int rgen_on[TX_MIX_SLOTS], rgen_mute[TX_MIX_SLOTS];

static void r_init(void)
{
    tx_mix_init(&rm);
    tx_fx_init(&rfx); rm.ins = tx_fx_lane; rm.pre = tx_fx_drive_mix; rm.bus = tx_fx_bus; rm.busq = tx_fx_bus_quiet; rm.fx = &rfx;
    for (unsigned k = 0; k < TX_MIX_SLOTS; k++) tx_gen_init(&rgen[k], 1u + k);
}
static void r_gen_on(int k, int on, unsigned seed)
{
    if (on && !rgen_on[k] && seed) tx_rng_seed(&rgen[k].rng, seed ^ 0x6E4E4E21u);
    if (!on && rgen_on[k] && rm.e[k]) {
        tx_gen_ev_t ev[TX_GEN_OFFS];
        unsigned ne = tx_gen_stop(&rgen[k], ev, TX_GEN_OFFS), q;
        for (q = 0; q < ne; q++) rm.e[k]->key(rm.st[k], ev[q].note, 0, 0);
    }
    rgen_on[k] = on != 0;
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
        if (rgen_on[k] && rm.e[k]) {
            tx_gen_ev_t ev[TX_GEN_EV_MAX];
            uint8_t in;
            unsigned ne = tx_gen_block(&rgen[k], TX_BLOCK, ev, TX_GEN_EV_MAX, &in), q;
            int ip = tx_param_find(rm.e[k], "ACTIVITY");
            if (ip >= 0 && !strcmp(rm.e[k]->name, "source")) rm.e[k]->param(rm.st[k], (uint8_t)ip, in);
            for (q = 0; q < ne && !rgen_mute[k]; q++) rm.e[k]->key(rm.st[k], ev[q].note, ev[q].vel, ev[q].vel != 0);
        }
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
    {40, 'P', 0, 0, 16, 96, 0},                 /* the source's COUNT (id 16): 32 */
    {60, 'F', 2, 0, 0, 2, 0},                   /* REVERB TYPE hall */
    {60, 'F', 2, 0, 1, 60, 0},                  /* its MIX */
    {80, 'F', 0, 0, 0, 60, 0},                  /* the mix's DRIVE AMOUNT */
    {90, 'F', 1, 0, 0, 1, 0},                   /* lane 1's FILTER TYPE low-pass */
    {100, 'F', 3, 0, 0, 80, 0},                 /* the GRAIN DELAY's MIX */
    {110, 'F', 0, 0, 3, 2, 0},                  /* the DRIVE's TYPE TSH8 (its left side owns the tables) */
    {115, 'F', 3, 0, 3, 15, 0},                 /* the GRAIN DELAY's GRAINS: 16 */
    {120, 'L', 0, 0, 0, 70, 0},                 /* lane 1's LEVEL */
    {130, 'N', 1, 0, 20, 0, 0},                 /* lane 2's pan */
    {150, 'S', 0, "rainwater", 0, 0, 0},        /* a switch on lane 1: the source fades out over 4 blocks */
    {151, 'K', 0, 0, 62, 110, 1},
    {160, 'F', 2, 0, 0, 3, 0},                  /* REVERB TYPE infinite: its tank's halves on the second core */
    {200, 'K', 0, 0, 62, 0, 0},
    {220, 'S', 1, 0, 0, 0, 0},                  /* lane 2 off: its tail fades */
    {230, 'S', 2, "source", 0, 0, 0},           /* lane 3: the source played by its generator */
    {231, 'A', 2, 0, TX_GEN_ACTIVITY, 90, 0},
    {231, 'G', 2, 0, 1, 777, 0},
    {231, 'K', 2, 0, 60, 100, 1},
    {232, 'F', 0, 0, 0, 90, 0},                 /* the DRIVE's AMOUNT up */
    {260, 'F', 2, 0, 0, 7, 0},                  /* REVERB TYPE plate: the same */
    {270, 'F', 0, 0, 3, 3, 0},                  /* the DRIVE's TYPE OHCD (two cores: R on the second, its scratch
                                                 * the mix's), then BHLUE */
    {290, 'F', 0, 0, 3, 4, 0},
    {300, 'S', 1, "fire", 0, 0, 0},
    {300, 'K', 1, 0, 60, 100, 1},
    {301, 'M', 1, 0, 1, 0, 0},                  /* lane 2's generator, muted (a classic engine: no notes) */
    {301, 'G', 1, 0, 1, 4242, 0},
    {305, 'F', 3, 0, 3, 29, 0},                 /* GRAINS 30: over the cap (the model's 19), both halves */
    {310, 'F', 0, 0, 3, 1, 0},                  /* the DRIVE's TYPE KHLON: TSH8's tables free */
    {330, 'F', 3, 0, 6, 76, 0},                 /* PITCH up: pitched, the cap down to 6 (the second half silent) */
    {340, 'F', 0, 0, 3, 2, 0},                  /* and TSH8 again: the tables built anew */
    {350, 'G', 2, 0, 0, 0, 0},                  /* lane 3's generator off: its notes released */
    {360, 'G', 2, 0, 1, 99, 0},                 /* and on again: a new walk */
    {380, 'F', 2, 0, 0, 1, 0},                  /* REVERB TYPE room: its body on the second core (two cores) */
    {440, 'K', 2, 0, 60, 0, 0},
    {450, 0, 0, 0, 0, 0, 0},
};

int main(void)
{
    static int32_t L[TX_BLOCK], R[TX_BLOCK], rL[TX_BLOCK], rR[TX_BLOCK];
    uint32_t b, s = 0, seed = 12345u, fails = 0, j, loud = 0;
    ln_init();
    r_init();
#ifdef LANES_TEST_PAR
    {
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 1u << 20);
        if (pthread_create(&th, &at, par_main, 0)) { printf("FAIL no thread\n"); return 1; }
        pthread_detach(th);
    }
#endif
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
            case 'G':
                ln_gen_on((uint32_t)t->k, (uint32_t)t->a, (uint32_t)t->v);
                r_gen_on(t->k, t->a, (unsigned)t->v);
                break;
            case 'A':
                ln_gen_param((uint32_t)t->k, (uint32_t)t->a, (uint32_t)t->v);
                tx_gen_param(&rgen[t->k], (unsigned)t->a, (unsigned)t->v);
                break;
            case 'M':
                ln_gen_mute((uint32_t)t->k, (uint32_t)t->a);
                rgen_mute[t->k] = t->a != 0;
                break;
            case 'F':                           /* group k: 0 drive (the mix's), 1 filter (lane c), 2 reverb, 3 grain */
                ln_fx((uint32_t)t->k, (uint32_t)t->c, (uint32_t)t->a, (uint32_t)t->v);
                if (t->k == 0) tx_fx_drive_set(&rfx, (unsigned)t->a, (unsigned)t->v);
                else if (t->k == 1) tx_fx_filter_param(&rfx.filter[t->c], (unsigned)t->a, (unsigned)t->v);
                else if (t->k == 2) tx_fx_reverb_param(&rfx.rev, (unsigned)t->a, (unsigned)t->v);
                else tx_fx_grain_param(&rfx.gd, (unsigned)t->a, (unsigned)t->v);
                break;
            }
        }
#ifdef LANES_TEST_PAR
        ln.par_force = (uint8_t)(b % 64u);      /* every split of the lanes and the grains' voices' halves in turn
                                                 * (0: by cost) */
#endif
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
    {   /* the one spare arena: two sounding lanes switched at once, the second waits for the first's fade (4 blocks) */
        const tx_engine_t *e = tx_engine_find("fire");
        uint32_t w;
        if (!ln.mix.e[0] || !ln.mix.e[1]) { printf("FAIL the spare check needs lanes 1 and 2 sounding\n"); fails++; }
        if (ln_select(0, e, 1u)) { printf("FAIL the spare: the first switch refused\n"); fails++; }
        if (ln_select(1, e, 2u) >= 0) { printf("FAIL the spare: a second switch while it is held\n"); fails++; }
        for (w = 0; w < 8u && ln_select(1, e, 2u) < 0; w++) ln_block(L, R, TX_BLOCK);
        if (w != 4u) { printf("FAIL the spare: free after %u blocks, not 4\n", w); fails++; }
        for (w = 0; w < 4u; w++) ln_block(L, R, TX_BLOCK);
        if (ln.spare_busy || ln.mix.e[0] != e || ln.mix.e[1] != e) { printf("FAIL the spare: not given back\n"); fails++; }
        {   /* every arena in use once: the lanes' and the spare, no two the same */
            uint32_t seen = 1u << ln.spare, k;
            for (k = 0; k < LN_N; k++) { if (seen >> ln.ar[k] & 1u) { printf("FAIL the arenas: %u twice\n", ln.ar[k]); fails++; } seen |= 1u << ln.ar[k]; }
        }
    }
#ifdef LANES_TEST_PAR
    if (!ln.par_blocks) { printf("FAIL the second core rendered nothing\n"); fails++; }
    if (!par_rj) { printf("FAIL the reverb's ROOM body never ran on the second core\n"); fails++; }
    if ((par_kt & (1u << TX_RVT_HALL | 1u << TX_RVT_INF | 1u << TX_RVT_PLATE)) != (1u << TX_RVT_HALL | 1u << TX_RVT_INF | 1u << TX_RVT_PLATE)) {
        printf("FAIL the reverb kernels' halves: not on the second core for every TYPE (0x%x)\n", par_kt); fails++;
    }
    if (par_dt != (1u << TX_DRT_FDRV | 1u << TX_DRT_KHLON | 1u << TX_DRT_OHCD | 1u << TX_DRT_BHLUE)) {
        printf("FAIL the mix's DRIVE: R not on the second core for every TYPE but TSH8 (0x%x)\n", par_dt); fails++;
    }
    printf("     lanes (two cores): %u blocks with lanes on the second core, %u jobs, reverb TYPEs split 0x%x, DRIVE TYPEs 0x%x\n",
           ln.par_blocks, par_jobs, par_kt, par_dt);
#endif
    if (ln.fx.drv.ts_own != &ln.fx.drive[0] || ln.fx.drive[0].cur != TX_DRT_TSH8 || ln.fx.drive[1].cur != TX_DRT_TSH8) {
        printf("FAIL the DRIVE TYPEs: TSH8 not on both sides of the mix's DRIVE (the left owning it)\n"); fails++;
    }
    if (ln.dropped) { printf("FAIL %u events dropped\n", ln.dropped); fails++; }
    if (!loud) { printf("FAIL the scene was silent\n"); fails++; }
    printf("%s lanes: %u blocks, the firmware's lanes = the player's chain\n", fails ? "FAIL" : "ok  ", b);
    return fails ? 1 : 0;
}
