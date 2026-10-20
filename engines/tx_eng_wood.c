/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #31 Wood cracks / creak: a branch or a board bent until it snaps (MODAL, INSECT
 * resonators, CREAK core).
 *
 * A key takes a piece of wood and bends it: it creaks and crackles while held, the crackles accelerating, and snaps
 * after the time LOAD (if still held), with a tail of aftershocks; release before that lets it relax. Physics:
 *   cracks   micro-fractures with power-law energies (precursors E^-1.7: W4 1.5-1.9), their rate rising towards
 *            failure as the energy release ((tau - t) / tau)^-gamma, gamma 0.27 (W4): rate ~ ((tau - t) / tau)^-1.27;
 *            the snap = the main shock, 1.75 decades above its largest aftershock, ~8 aftershocks at Omori times
 *            (p 1.68, c 70 ms: W8). Each crack: a sharp click (1-3 samples + a 1 ms burst) and a strike on the
 *            piece's modes.
 *   body     a free-free bar (a branch, a slat): f1 = 3.56 (d / 4) c / L^2 with d = L / 15 (ours) -> f1 ~ 0.059 c / L
 *            (a 30 cm pine branch ~890 Hz); 5 modes 1 : 2.76 : 5.40 : 8.93 : 13.3; loss tan delta
 *            0.003-0.02 along the grain (M4b: Q 50-330 by wood) plus the hands holding it (sigma0 25 /s, ours);
 *            moisture lowers Q and the crack rate and dulls the clicks (ours).
 *   creak    stick-slip (W7, CREAK core): slips at PITCH (+-15 % jitter below 20 Hz) into 3 resonators on the
 *            piece's first modes (Q 12, ours), in bursts (a slow random envelope, ours). Two creak recordings slip at
 *            ~260 and ~490 Hz with body peaks 0.5-1.6 / 2.8-3.2 kHz.
 * Keys: the piece's length (a semitone per key); velocity = how hard it is bent (crack sizes, creak level). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_modal.h"
#include "tx_core_insect.h"
#include "tx_core_creak.h"

#define OUT_TRIM 7716               /* output gain, Q12 (+3.3 dB): max momentary (one velocity-100 press): -23.3 LUFS at LEVEL 100 -> target -20 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +2.2 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 2

enum { P_LOAD, P_WOOD, P_MOIST, P_CREAK, P_PITCH, P_SIZE, NP };

static const tx_param_t PARAMS[NP] = {
    {"LOAD", 127, 40, "time to the snap while held: 0.3 .. 8 s (log); 127 = never (creaks and crackles only)"},
    {"WOOD", 127, 40, "spruce (bright, Q 160) | pine (Q 120) | maple (Q 110) | oak (dark, Q 80) | green branch (Q 40)"},
    {"MOIST", 127, 20, "dry .. wet: duller, fewer cracks, more damping"},
    {"CREAK", 127, 25, "stick-slip creak: off .. strong"},
    {"PITCH", 127, 70, "creak slip rate: 30 .. 800 Hz (log): ratchety .. singing (W7)"},
    {"SIZE", 127, 64, "piece length at C4: 10 .. 100 cm (log): f1 ~ 0.06 c / L"},
};

#define NP_ 4u                     /* pieces (one per key) */
#define NMW 5u
#define NR 3u                      /* creak resonators */
#define NAF 12u                    /* pending aftershocks per piece */
#define NEVB 48u
#define NWOOD 5u
#define A_REF 540000000            /* a mode's amplitude for the snap at v100 (Q30; calibrated by render) */

static const uint16_t W_Q[NWOOD] = {160, 120, 110, 80, 40};
static const uint16_t W_C[NWOOD] = {5500, 4500, 4300, 4000, 3000};   /* sqrt(E' / rho), m/s (ours, typical) */
static const uint16_t BAR_R[NMW] = {1024, 2823, 5534, 9147, 13665};   /* free-free, Q10 */

typedef struct {
    tx_mode_t m[NMW];
    tx_mode_t thud;                /* the hands / arm jerking at the snap: ~60-90 Hz, Q 3 (a snap recording peaks at 62 Hz: ours) */
    tx_ireso_t cr[NR];
    tx_creak_t ck;
    uint32_t t, tau;               /* samples since the press, to the snap (0 = never) */
    uint32_t next_q8;              /* samples to the next precursor */
    uint32_t af[NAF];              /* aftershock times, samples from now (0 = none) */
    int32_t afs[NAF];              /* their sizes (Q12 log2 under the main) */
    int32_t cenv, cgoal;           /* creak burst envelope, Q15 */
    uint32_t f1;
    uint8_t em, note, vel, alive, held, snapped, rel, pad;   /* rel: how fast the hands let go (the creak's fade, 2^-rel per block) */
} piece_t;

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    piece_t pc[NP_];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK], bpush[TX_BLOCK];
    uint16_t eo[NEVB], co[NEVB];
    int16_t ea[NEVB], ca[NEVB];
    int32_t res, res_x1;
    uint8_t p[NP], limit, quiet;
    int32_t level, size_l2;
    uint32_t q_x16, c, tau_s;      /* tau in samples (0 = never) */
    uint16_t dropped;
    tx_vary_t vy;
} wood_t;

static void set_param(wood_t *s, unsigned id, unsigned v)
{
    unsigned r;
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    r = (s->p[P_WOOD] * NWOOD) >> 7;
    s->c = W_C[r];
    s->q_x16 = (uint32_t)W_Q[r] * 16u * (32768u - (uint32_t)s->p[P_MOIST] * 155u) >> 15;   /* wet: Q x 0.4 */
    switch (id) {
    case P_LOAD: s->tau_s = v >= 127u ? 0u : (uint32_t)(((uint64_t)tx_knob_exp(v, 77u, 314000) * TX_FS) >> 8); break;   /* 0.3 .. 8 s */
    case P_SIZE: s->size_l2 = -217706 + (int32_t)(v * 1714u); break;   /* log2(0.1 m) + v log2(10) / 127 */
    default: break;
    }
}

static void wood_init(void *st, uint32_t seed)
{
    wood_t *s = (wood_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    s->limit = 3;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((wood_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void wood_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((wood_t *)st)->vy);
    tx_cloud_key(&((wood_t *)st)->cloud, note, vel, on);
}

static void wood_param(void *st, uint8_t id, uint8_t val)
{
    wood_t *s = (wood_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i, k;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NP_ ? NP_ : val);
        for (i = s->limit; i < NP_; i++) {
            for (k = 0; k < NMW; k++) s->pc[i].m[k].amp = 0;
            for (k = 0; k < NR; k++) s->pc[i].cr[k].y1 = s->pc[i].cr[k].y2 = s->pc[i].cr[k].p1 = s->pc[i].cr[k].p2 = 0;
            s->pc[i].alive = 0;
        }
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

static int piece_quiet(const piece_t *p)
{
    unsigned k;
    if (p->alive) return 0;
    for (k = 0; k < NMW; k++) if (p->m[k].amp) return 0;
    if (p->thud.amp) return 0;
    for (k = 0; k < NR; k++) if (!tx_ireso_quiet(&p->cr[k])) return 0;
    return 1;
}

static void take(wood_t *s, const tx_event_t *e)
{
    piece_t *p = 0;
    unsigned i, k;
    for (i = 0; i < s->limit && !p; i++) if (s->pc[i].alive && s->pc[i].note == e->note) p = &s->pc[i];
    for (i = 0; i < s->limit && !p; i++) if (piece_quiet(&s->pc[i])) p = &s->pc[i];
    for (i = 0; i < s->limit && !p; i++) if (!s->pc[i].alive) p = &s->pc[i];
    if (!p) { s->dropped++; return; }
    p->f1 = tx_exp2(tx_log2_q16(s->c) - 267150 - s->size_l2 + ((int32_t)e->note - 60) * 5461 + (tx_vary_tone(&s->vy) >> 2));   /* 0.059 c / L */
    {   /* no two pieces alike: the grain's loss (Q x 2^+-1) and the grip (sigma0 5 .. 80 /s) are this piece's */
        uint32_t q = tx_vary_scale(s->q_x16, tx_rand_s15(&s->cloud.rng) * 2);
        uint32_t grip = (5u + tx_rand_below(&s->cloud.rng, 76u)) * 256u;
    for (k = 0; k < NMW; k++) {
        uint32_t f = (p->f1 * BAR_R[k]) >> 10;
        p->m[k].inc = f < 20000u ? f * 97391u : 0u;
        p->m[k].m16 = f < 20000u ? tx_mode_decay(f, q < 16u ? 16u : q, grip) : 0u;
        if (!p->m[k].inc) p->m[k].amp = 0;
    }
    }
    {
        static const uint16_t CR[NR] = {1024, 1536, 2823};   /* creak body: f1, 1.5 f1 (ours), f2 */
        for (k = 0; k < NR; k++) {
            uint32_t f = (p->f1 * CR[k]) >> 10;
            tx_ireso_set(&p->cr[k], f > 18000u ? 18000u : f, 192);   /* Q 12 (ours) */
            p->cr[k].g = k ? 140 : 200;
            p->cr[k].zlim = 32;
        }
    }
    p->thud.inc = (60u + tx_rand_below(&s->cloud.rng, 30u)) * 97391u;
    p->thud.m16 = tx_mode_decay(p->thud.inc / 97391u, 48u, 0u);
    p->t = 0;
    p->tau = s->tau_s ? s->tau_s - (s->tau_s >> 2) + (uint32_t)(((uint64_t)s->tau_s * (tx_rand(&s->cloud.rng) >> 16)) >> 17) : 0u;   /* +-25 % (ours) */ p->next_q8 = 0; p->snapped = 0;
    for (k = 0; k < NAF; k++) p->af[k] = 0;
    p->cenv = 0; p->cgoal = 32767;
    p->rel = (uint8_t)(2u + tx_rand_below(&s->cloud.rng, 5u));   /* let go in ~20 .. 300 ms (ours) */
    p->em = e->emitter; p->note = e->note; p->vel = e->vel; p->alive = 1; p->held = 1;
}

/* a crack of piece p at offset o, energy x (Q12 log2 below the snap's): strike the modes, click */
static void crack(wood_t *s, piece_t *p, unsigned o, int32_t x, unsigned *ne)
{
    int32_t vl2 = ((int32_t)p->vel - 100) * 655;
    int32_t a = (int32_t)(((int64_t)A_REF * (int32_t)tx_exp2(((-x * 16 + vl2) >> 1) + (15 << 16))) >> 15);   /* ~ sqrt(E) */
    int32_t dull = 32768 - (int32_t)s->p[P_MOIST] * 160;    /* wet: duller clicks (ours) */
    a = (int32_t)(((int64_t)a * s->level) >> 15);
    if (a < 4096) return;
    if (*ne < 6u) {                                          /* <= 6 mode strikes per piece per block (CPU); the rest click */
        s->eo[*ne] = (uint16_t)o; s->ea[*ne] = (int16_t)(a >> 15); (*ne)++;   /* (strikes of this block, in order) */
    }
    s->bpush[o] += (int32_t)(((int64_t)(a >> 10) * dull) >> 15);
    {   /* the fracture front itself: a 2-sample spike (fibres break in microseconds: a sharp crack, ours) */
        int32_t sp = (int32_t)(((int64_t)(a >> 7) * dull) >> 15);
        s->buf[o] += sp;
        if (o + 1u < TX_BLOCK) s->buf[o + 1u] -= sp >> 1;
    }
    (void)p;
}

TX_AUDIO_CODE static void wood_render(void *st, int32_t *mix, unsigned n)
{
    wood_t *s = (wood_t *)st;
    tx_rng_t *r = &s->cloud.rng;
    unsigned k, i, j;
    uint16_t d0 = s->dropped;
    int32_t any = 0;
    if (n > TX_BLOCK) n = TX_BLOCK;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;
    for (j = 0; j < n; j++) s->buf[j] = s->bpush[j] = 0;
    for (i = 0; i < k; i++) take(s, &s->ev[i]);
    for (i = 0; i < NP_; i++) {
        piece_t *p = &s->pc[i];
        unsigned ne = 0, a, j0 = 0;
        if (piece_quiet(p)) continue;
        any = 1;
        if (p->alive) {
            p->held = (uint8_t)(p->em < TX_CLOUD_EMITTERS && s->cloud.em[p->em].held && s->cloud.em[p->em].note == p->note);
            if (p->held && !p->snapped) {                    /* loading: precursors at a rate rising to the snap */
                uint32_t per_q8, rate_q8 = 24u * (256u - s->p[P_MOIST]);  /* ~24 /s dry at the start, ~12 wet (ours) */
                int32_t lr = 0;
                if (p->tau) {                                /* x ((tau - t) / tau)^-1.27 (W4) */
                    uint32_t left = p->tau > p->t ? p->tau - p->t : 1u;
                    int32_t l2 = tx_log2_q16(p->tau) - tx_log2_q16(left);
                    lr = (l2 * 1270) / 1000;
                    if (lr > 4 << 16) lr = 4 << 16;          /* x 16 at most (~190 /s per piece) */
                }
                per_q8 = (TX_FS << 16) / rate_q8;           /* fs / rate, Q8 */
                per_q8 = (uint32_t)(((uint64_t)per_q8 * tx_exp2((16 << 16) - lr)) >> 16);
                while ((p->next_q8 >> 8) < n) {
                    int32_t x = 21000 + (int32_t)(((int64_t)tx_neglog2_q12(tx_rand(r)) * 5851) >> 12);   /* E^-1.7, from */
                    crack(s, p, p->next_q8 >> 8, x > 40000 ? 40000 : x, &ne);                        /* 1 dec down */
                    p->next_q8 += tx_cloud_exp_q8(&s->cloud, per_q8 ? per_q8 : 1u);
                }
                p->next_q8 -= (uint32_t)n << 8;
                if (p->tau && p->t + n >= p->tau) {          /* the snap: main shock + ~8 aftershocks (W8) */
                    unsigned o = p->tau > p->t ? p->tau - p->t : 0u, c2 = 0;
                    uint32_t bits = tx_rand(r);
                    crack(s, p, o < n ? o : n - 1u, 0, &ne);
                    tx_mode_strike(&p->thud, (int32_t)(((int64_t)A_REF / 3 * s->level) >> 15));
                    for (a = 0; a < 16; a++) c2 += bits >> a & 1u;   /* binomial(16, 1/2): mean 8 */
                    for (a = 0; a < c2 && a < NAF; a++) {
                        uint32_t l = tx_neglog2_q12(tx_rand(r)), t;
                        int64_t xx = ((int64_t)l * 6019) >> 12;      /* / (p - 1) = / 0.68, Q12 */
                        if (xx > 13 << 12) xx = 13 << 12;
                        t = (uint32_t)((((uint64_t)tx_exp2((int32_t)xx * 16 + (16 << 16)) - 65536u) * 3087u) >> 16);   /* c 70 ms */
                        p->af[a] = 1u + (t > 441000u ? 441000u : t);
                        p->afs[a] = 23250 + (int32_t)(((int64_t)tx_neglog2_q12(tx_rand(r)) * 5851) >> 12);   /* >= 1.75 dec */
                    }
                    p->snapped = 1;
                    p->cgoal = 0;
                }
            }
            if (!p->held) p->cgoal = 0;
            for (a = 0; a < NAF; a++)                        /* aftershocks due */
                if (p->af[a]) {
                    if (p->af[a] <= n) { crack(s, p, p->af[a] - 1u, p->afs[a], &ne); p->af[a] = 0; }
                    else p->af[a] -= n;
                }
            p->t += n;
            {   /* the creak: bursts while bent (a slow envelope toward a random goal, ours) */
                int32_t lvl;
                if (p->held && !p->snapped && (tx_rand(r) >> 26) == 0) p->cgoal = (int32_t)(tx_rand(r) >> 17);
                p->cenv += (p->cgoal - p->cenv) >> (p->held ? 4u : p->rel);
                lvl = (int32_t)(((int64_t)p->cenv * s->p[P_CREAK] * (40 + p->vel)) >> 15);   /* Q15-ish slip size */
                lvl = (int32_t)(((int64_t)lvl * s->level) >> 15);
                if (lvl > 64) {
                    uint32_t f0 = tx_knob_exp(s->p[P_PITCH], 30u, 310000);   /* 30 .. 800 Hz */
                    unsigned nc = tx_creak_block(&p->ck, r, f0, 4915, lvl > 20000 ? 20000 : lvl, n, s->co, s->ca, NEVB), q;
                    for (q = 0; q < NR; q++) tx_ireso_add(&p->cr[q], s->co, s->ca, nc, s->buf, n);
                } else
                    for (a = 0; a < NR; a++) tx_ireso_add(&p->cr[a], 0, 0, 0, s->buf, n);
            }
            {   /* done: released (or snapped), no aftershock left, the creak faded */
                unsigned left = 0;
                for (a = 0; a < NAF; a++) left |= p->af[a] != 0;
                if ((!p->held || p->snapped) && !left && p->cenv < 64) { p->alive = 0; p->em = 0xFF; }
            }
        } else
            for (a = 0; a < NR; a++) tx_ireso_add(&p->cr[a], 0, 0, 0, s->buf, n);
        /* the body: strikes in time order (insertion-sorted), each rendering up to its sample */
        for (a = 1; a < ne; a++) {
            uint16_t o = s->eo[a]; int16_t v = s->ea[a]; unsigned q = a;
            while (q && s->eo[q - 1] > o) { s->eo[q] = s->eo[q - 1]; s->ea[q] = s->ea[q - 1]; q--; }
            s->eo[q] = o; s->ea[q] = v;
        }
        for (a = 0; a < ne; a++) {
            unsigned m;
            tx_modal_run(p->m, NMW, s->buf, j0, s->eo[a]);
            j0 = s->eo[a];
            for (m = 0; m < NMW; m++) if (p->m[m].inc) tx_mode_strike(&p->m[m], (int32_t)(s->ea[a] * ((m & 1u) ? -12000 : 16000)) >> 0);
        }
        tx_modal_run(p->m, NMW, s->buf, j0, n);
        tx_modal_run(&p->thud, 1, s->buf, 0, n);
    }
    {   /* the clicks: a 1-3 sample spike and a ~1 ms burst */
        int32_t e = s->res, x1 = s->res_x1;
        for (j = 0; j < n; j++) {
            int32_t x;
            e += s->bpush[j];
            if (!e) continue;
            x = (int32_t)(tx_rand(r) >> 17) - 16384;
            x1 += (x - x1) >> 2;
            s->buf[j] += (x1 * (e >> 12)) >> 1;
            e -= (e >> 5) + 1;
            if (e < 0) e = 0;
        }
        s->res = e; s->res_x1 = e ? x1 : 0;
        any |= e;
    }
    tx_vary_block(&s->vy, s->buf, n);
    tx_trim(s->buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, s->buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !any && !(s->hp.x1 | s->hp.y1));
}

static void wood_stats(const void *st, tx_stats_t *o)
{
    const wood_t *s = (const wood_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < NP_; i++) n += s->pc[i].alive;
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: four pieces bent hard to a quick snap, strong creaks at the highest slip rate */
static const char SCENE_MAX[] =
    "0 particles 4\n0 param LOAD 0\n0 param CREAK 127\n0 param PITCH 127\n0 param SIZE 127\n"
    "0 on 53 127\n0 on 60 127\n0 on 67 127\n0 on 74 127\n"
    "2500 off 53\n2500 off 60\n2500 off 67\n2500 off 74\n"
    "4500 end\n";

const tx_engine_t tx_eng_wood = {
    "wood", "#31 Wood cracks / creak", NP, PARAMS, NP_, 3, sizeof(wood_t), SCENE_MAX,
    wood_init, wood_key, wood_param, wood_render, wood_stats,
};
