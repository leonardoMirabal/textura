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

#define OUT_TRIM 11570              /* output gain, Q12 (+9.0 dB; +3.5 with note 17's quieter creak and thud, the
                                    * preset's loudness kept): max momentary (one velocity-100 press): -23.3 LUFS at LEVEL 100 -> target -20 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +2.2 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 2

enum { P_LOAD, P_WOOD, P_MOIST, P_CREAK, P_PITCH, P_SIZE, P_WEIGHT, P_LITTER, NP };

static const tx_param_t PARAMS[NP] = {
    {"LOAD", 127, 40, "time to the snap while held: 0.3 .. 8 s (log); 127 = never (creaks and crackles only)"},
    {"WOOD", 127, 27, "spruce (bright, Q 160) | pine (Q 120) | maple (Q 110) | oak (dark, Q 80) | green branch (Q 40) | "
                      "tree trunk (creaking in the wind, no snap) | twigs underfoot (a step snaps a few) (18 each)"},
    {"MOIST", 127, 20, "dry .. wet: duller, fewer cracks, more damping"},
    {"CREAK", 127, 25, "stick-slip creak: off .. strong"},
    {"PITCH", 127, 70, "creak slip rate: 30 .. 800 Hz (log): ratchety .. singing (W7)"},
    {"SIZE", 127, 64, "piece length at C4: 10 .. 100 cm (log): f1 ~ 0.06 c / L"},
    {"WEIGHT", 127, 80, "twigs underfoot: the step's weight, light .. heavy: 1 .. 4 twigs a step"},
    {"LITTER", 127, 0, "twigs underfoot: the litter's depth, bare .. deep: it takes the highs and holds the twigs"},
};

#define NP_ 3u                     /* pieces (one per key; 3 at most: 4 snapping at once with creaks reached ~1140 us on the FM-1) */
#define NMW 5u
#define NR 3u                      /* creak resonators */
#define NAF 12u                    /* pending aftershocks per piece */
#define NEVB 48u
#define NWOOD 7u
#define A_REF 540000000            /* a mode's amplitude for the snap at v100 (Q30; calibrated by render) */

/* the kinds (WOOD): a species of wood bent until it snaps, or a source of its own (ours, set against the kinds'
 * reference recordings, analysis only) */
typedef struct {
    uint16_t q, c;                 /* loss along the grain (Q), sqrt(E' / rho) m/s (typical) */
    int32_t size;                  /* the piece's length re SIZE's, log2 Q16 */
    int32_t crf;                   /* the creak's body re the piece's f1, log2 Q16 */
    int32_t pitch;                 /* the slip rate re PITCH's, log2 Q16 */
    uint8_t snap;                  /* 0: never snaps (creaks only), 1: at LOAD, 2: a step: 20 .. 100 ms */
    uint8_t creak;                 /* creak level re CREAK's, Q7 */
    uint8_t slow;                  /* the creak's swells: 2^slow x longer */
    uint8_t prec;                  /* precursor cracks re the bend's, Q7 */
    uint8_t twigs;                 /* pieces per press (a step on twigs: each its own) */
    uint8_t thud;                  /* 1: the hands jerk at the snap (a low thud; none for a foot on twigs) */
    uint16_t out;                  /* its level, Q8 */
} wkind_t;
static const wkind_t WK[NWOOD] = {
    {160, 5500, -65536, 0, 0, 1, 24, 0, 32, 1, 1, 384},   /* spruce (the four species: dry wood fails brittle, a */
    {120, 4500, -65536, 0, 0, 1, 24, 0, 32, 1, 1, 384},   /* faint creak before the snap; their references are */
    {110, 4300, -65536, 0, 0, 1, 24, 0, 32, 1, 1, 384},   /* breaks: creak 128 was 2.13 from them, 1.45 now with */
    {80, 4000, -65536, 0, 0, 1, 24, 0, 32, 1, 1, 384},    /* the next branch below. Second pass: a dry branch bends
                                                              * nearly silent, so a quarter of the precursor cracks
                                                              * (the references: silent 88 % of the time between the
                                                              * snaps, ours 40 %), and the hand snaps pieces half as
                                                              * long as SIZE's (thin branches: their snaps 5-6 kHz,
                                                              * ours 3.7 kHz before) */
    {40, 3000, 0, 0, 0, 1, 128, 0, 128, 1, 0, 287},        /* green branch (+1 dB: its level as before the next
                                                              * branch came sooner) */
    /* tree trunk: a live trunk or a bough rubbing in the wind: long slow groans of stick-slip (the references: 1-3 kHz,
     * a creak every ~2-4 s) on a big wet body, a fibre ticking now and then, never a snap */
    {40, 3000, 131072, 114688, -65536, 0, 255, 2, 8, 1, 1, 256},
    /* twigs underfoot: a step snaps a few short dry twigs within ~0.1 s (the references: 5-12 kHz, a step a second,
     * ~6 snaps a second, nothing between the steps) */
    {120, 5000, -98304, 0, 0, 2, 0, 0, 64, 3, 0, 256},
};
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
    uint8_t em, note, vel, alive, held, snapped, rel, rk;    /* rel: how fast the hands let go (the creak's fade, 2^-rel per block) */
    uint8_t rd, pad2[3];                                     /* rk, rd: its cracks' burst corner and decay shifts */
} piece_t;

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    piece_t pc[NP_];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK], bpush[TX_BLOCK];
    int32_t lz;                    /* twigs: the litter's low-pass */
    uint16_t eo[NEVB], co[NEVB];
    int16_t ea[NEVB], ca[NEVB];
    int32_t res, res_x1;
    uint8_t rk, rd, pad3[2];       /* the burst's colour now */
    uint8_t p[NP], limit, quiet;
    int32_t level, size_l2;
    uint32_t q_x16, c, tau_s;      /* tau in samples (0 = never) */
    const wkind_t *k;
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
    s->k = &WK[r];
    s->c = s->k->c;
    s->q_x16 = (uint32_t)s->k->q * 16u * (32768u - (uint32_t)s->p[P_MOIST] * 155u) >> 15;   /* wet: Q x 0.4 */
    tx_cloud_burst(&s->cloud, s->k->snap == 2u ? 1u + (s->p[P_WEIGHT] * 3u + 64u) / 127u : s->k->twigs,
                   s->k->twigs > 1u ? 100u : 0u);   /* (twigs: WEIGHT, 1 .. 4 twigs a step;
                                                     * the default 80: 3) */
    switch (id) {
    case P_LOAD: s->tau_s = v >= 127u ? 0u : (uint32_t)(((uint64_t)tx_knob_exp(v, 77u, 314000) * TX_FS) >> 8); break;   /* 0.3 .. 8 s */
    case P_SIZE: s->size_l2 = -217706 + (int32_t)(v * 1714u); break;   /* log2(0.1 m) + v log2(10) / 127 */
    default: break;
    }
    tx_cloud_rate(&s->cloud, s->k->snap == 1u && s->tau_s ? (TX_FS * 512u) / (s->tau_s + TX_FS) : 0u);   /* a hand
                                                              * that snaps branches takes the next as it goes: a new
                                                              * one while held, at random, ~2 per bend + 1 s (the
                                                              * references: 0.56 big snaps /s; ours 0.07-0.27 before) */
}

static void wood_init(void *st, uint32_t seed)
{
    wood_t *s = (wood_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    s->limit = 3;
    s->rk = 2; s->rd = 5;
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
    if (s->k->twigs < 2u)                                    /* (twigs: each its own piece) */
        for (i = 0; i < s->limit && !p; i++) if (s->pc[i].alive && s->pc[i].note == e->note) p = &s->pc[i];
    for (i = 0; i < s->limit && !p; i++) if (piece_quiet(&s->pc[i])) p = &s->pc[i];
    for (i = 0; i < s->limit && !p; i++) if (!s->pc[i].alive) p = &s->pc[i];
    if (!p) { s->dropped++; return; }
    {   /* no two pieces alike (note 17: a branch-snapping recording's per-snap peak spreads 2.9 octaves, ours 0.2-0.9): each
         * its own length and thickness (f1 ~ d / L^2: sd 0.8 octave; twigs, the most, +0.4 octave: ours) */
        int32_t g = (int32_t)(((int64_t)tx_rand_s15(&s->cloud.rng) + tx_rand_s15(&s->cloud.rng) + tx_rand_s15(&s->cloud.rng)) * 52429 >> 15);
        g = g > (2 << 16) ? (2 << 16) : g < -(2 << 16) ? -(2 << 16) : g;
        p->f1 = tx_exp2(tx_log2_q16(s->c) - 267150 - s->size_l2 - s->k->size + ((int32_t)e->note - 60) * 5461 + (tx_vary_tone(&s->vy) >> 2)
                        + g + 26214);   /* 0.059 c / L */
        if (p->f1 > 9000u) p->f1 = 9000u;
        p->rk = (uint8_t)(1u + tx_rand_below(&s->cloud.rng, 3u));   /* its fracture's edge: 9 / 3.5 / 1.7 kHz corner */
        p->rd = (uint8_t)(4u + tx_rand_below(&s->cloud.rng, 3u));   /* and length: 0.4 / 0.7 / 1.5 ms */
    }
    {   /* no two pieces alike: the grain's loss (Q x 2^+-1) and the grip (sigma0 5 .. 80 /s) are this piece's */
        uint32_t q = tx_vary_scale(s->q_x16, tx_rand_s15(&s->cloud.rng) * 2);
        uint32_t grip = (5u + tx_rand_below(&s->cloud.rng, 76u) + (s->k->snap == 2u ? s->p[P_LITTER] * 3u / 2u : 0u)) * 256u;   /* (twigs:
                                                              * the litter holds them: up to +190 /s) */
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
            uint32_t f = (uint32_t)(((uint64_t)p->f1 * CR[k] * tx_exp2(s->k->crf - ((int32_t)s->p[P_MOIST] - 20) * 129 + (16 << 16))) >> 26);
            tx_ireso_set(&p->cr[k], f > 18000u ? 18000u : f, (uint32_t)(207 - (int32_t)s->p[P_MOIST] * 3 / 4));   /* Q 12 (ours); wet: the
                                                              * wood softer and lossier (moisture lowers the modulus and
                                                              * raises the loss): -0.2 oct, Q 6.6 at MOIST 127; as HEAD at 20 */
            p->cr[k].g = k ? 140 : 200;
            p->cr[k].zlim = 32;
        }
    }
    p->thud.inc = (60u + tx_rand_below(&s->cloud.rng, 30u)) * 97391u;
    p->thud.m16 = tx_mode_decay(p->thud.inc / 97391u, 48u, 0u);
    p->t = 0;
    p->tau = s->k->snap == 2u ? 882u + tx_rand_below(&s->cloud.rng, 3529u)   /* a step: 20 .. 100 ms (ours) */
           : s->k->snap && s->tau_s ? s->tau_s - (s->tau_s >> 2) + (uint32_t)(((uint64_t)s->tau_s * (tx_rand(&s->cloud.rng) >> 16)) >> 17) : 0u;   /* +-25 % (ours) */ p->next_q8 = 0; p->snapped = 0;
    for (k = 0; k < NAF; k++) p->af[k] = 0;
    p->cenv = 0; p->cgoal = 32767;
    p->rel = (uint8_t)(2u + tx_rand_below(&s->cloud.rng, 5u));   /* let go in ~20 .. 300 ms (ours) */
    p->em = e->emitter; p->note = e->note; p->vel = e->vel; p->alive = 1; p->held = 1;
}

/* the creak's three resonators on the same slips in one pass (CPU: as tx_ireso_add on each, bit for bit) */
TX_AUDIO_CODE static void creak_res(tx_ireso_t *cr, const uint16_t *off, const int16_t *imp, unsigned nimp, int32_t *out,
                                    unsigned n)
{
    int32_t a1 = cr[0].y1, a2 = cr[0].y2, b1 = cr[1].y1, b2 = cr[1].y2, c1 = cr[2].y1, c2 = cr[2].y2;
    int32_t p1 = cr[0].p1, p2 = cr[0].p2;
    const int32_t ka1 = cr[0].a1, ka2 = cr[0].a2, kb1 = cr[1].a1, kb2 = cr[1].a2, kc1 = cr[2].a1, kc2 = cr[2].a2;
    const int32_t ga = cr[0].g, gb = cr[1].g, gc = cr[2].g;
    int32_t a01 = a1, a02 = a2, b01 = b1, b02 = b2, c01 = c1, c02 = c2;
    unsigned j, k = 0, q;
    if (!nimp && !(a1 | a2 | b1 | b2 | c1 | c2 | p1 | p2)) return;
    for (j = 0; j < n; j++) {
        int32_t x = 0, d, y, z, w;
        while (k < nimp && off[k] == j) { x += imp[k]; k++; }
        d = (x - p2) >> 1;                                   /* (1 - z^-2) / 2 */
        p2 = p1; p1 = x;
        y = ((ka1 * a1) >> 14) - ((ka2 * a2) >> 14) + d;
        y = y > 65535 ? 65535 : y < -65535 ? -65535 : y;
        a2 = a1; a1 = y;
        z = ((kb1 * b1) >> 14) - ((kb2 * b2) >> 14) + d;
        z = z > 65535 ? 65535 : z < -65535 ? -65535 : z;
        b2 = b1; b1 = z;
        w = ((kc1 * c1) >> 14) - ((kc2 * c2) >> 14) + d;
        w = w > 65535 ? 65535 : w < -65535 ? -65535 : w;
        c2 = c1; c1 = w;
        out[j] += y * ga + z * gb + w * gc;
    }
    if (!nimp && !(p1 | p2)) {                               /* silent input: small or frozen -> exact zero */
        int32_t *v[3][2] = {{&a1, &a2}, {&b1, &b2}, {&c1, &c2}}, o[3][2] = {{a01, a02}, {b01, b02}, {c01, c02}};
        for (q = 0; q < 3; q++) {
            int32_t zl = cr[q].zlim ? cr[q].zlim : 8, u1 = *v[q][0], u2 = *v[q][1];
            if ((u1 > -zl && u1 < zl && u2 > -zl && u2 < zl) || (u1 == o[q][0] && u2 == o[q][1])) *v[q][0] = *v[q][1] = 0;
        }
    }
    cr[0].y1 = a1; cr[0].y2 = a2; cr[1].y1 = b1; cr[1].y2 = b2; cr[2].y1 = c1; cr[2].y2 = c2;
    for (q = 0; q < 3; q++) { cr[q].p1 = p1; cr[q].p2 = p2; }
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
    s->bpush[o] += (int32_t)(((int64_t)(a >> 9) * dull) >> 15);   /* (+6 dB: twigs snap mostly as crack, note 17) */
    s->rk = p->rk; s->rd = p->rd;                            /* (the burst takes the latest crack's colour) */
    {   /* the fracture front itself: a 2-sample spike (fibres break in microseconds: a sharp crack, ours) */
        int32_t sp = (int32_t)(((int64_t)(a >> 7) * dull) >> 15);
        s->buf[o] += sp;
        if (o + 1u < TX_BLOCK) s->buf[o + 1u] -= sp >> 1;
    }
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
                uint32_t per_q8, rate_q8 = ((24u * (256u - s->p[P_MOIST])) * s->k->prec) >> 7;  /* ~24 /s dry at the start, ~12 wet (ours) */
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
                    if (s->k->thud) {
                        int32_t g = (int32_t)(((int64_t)tx_rand_s15(r) + tx_rand_s15(r) + tx_rand_s15(r)) * 65536 >> 15);
                        g = g > (1 << 16) ? (1 << 16) : g;
                        tx_mode_strike(&p->thud, (int32_t)(((int64_t)(A_REF / 3) * (int32_t)tx_exp2(g - 98304 + (15 << 16)) >> 15) * s->level >> 15));
                    }
                    for (a = 0; a < 16; a++) c2 += bits >> a & 1u;   /* binomial(16, 1/2): mean 8 */
                    if (s->k->snap == 2u) c2 >>= 2;                  /* (a thin twig: ~2, within 0.5 s: ours) */
                    for (a = 0; a < c2 && a < NAF; a++) {
                        uint32_t l = tx_neglog2_q12(tx_rand(r)), t;
                        int64_t xx = ((int64_t)l * 6019) >> 12;      /* / (p - 1) = / 0.68, Q12 */
                        if (xx > 13 << 12) xx = 13 << 12;
                        t = (uint32_t)((((uint64_t)tx_exp2((int32_t)xx * 16 + (16 << 16)) - 65536u) * 3087u) >> 16);   /* c 70 ms */
                        p->af[a] = 1u + (t > (s->k->snap == 2u ? 22050u : 441000u) ? (s->k->snap == 2u ? 22050u : 441000u) : t);
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
                if (p->held && !p->snapped && (tx_rand(r) >> (26u + s->k->slow)) == 0) p->cgoal = (int32_t)(tx_rand(r) >> 17);
                p->cenv += (p->cgoal - p->cenv) >> (p->held ? 4u + s->k->slow : p->rel);
                lvl = (int32_t)(((int64_t)p->cenv * ((s->p[P_CREAK] * s->k->creak) >> 7) * (40 + p->vel)) >> 15);   /* Q15-ish slip size */
                lvl = (int32_t)(((int64_t)lvl * s->level) >> 15);
                lvl = (int32_t)(((int64_t)lvl * (35348 - (int32_t)s->p[P_MOIST] * 129)) >> 15);   /* wet: water lubricates
                                                              * the slip, less stick-slip (ours): -5.4 dB at MOIST 127, 0 at
                                                              * the default 20 */
                if (p->tau && !p->snapped) {                 /* the fibres slip more as the load builds: the creak grows
                                                              * with t / tau (a quick snap creaks briefly; at the top the
                                                              * creak hid the snaps: note 17) */
                    uint32_t fr = p->t >= p->tau ? 32768u : (p->t << 8) / ((p->tau >> 7) | 1u);   /* (32-bit: tau < 2^19) */
                    lvl = (int32_t)(((int64_t)lvl * (int32_t)fr) >> 15);
                }
                if (lvl > 64) {
                    uint32_t f0 = (uint32_t)(((uint64_t)tx_knob_exp(s->p[P_PITCH], 30u, 310000) * tx_exp2(s->k->pitch + (16 << 16))) >> 16);   /* 30 .. 800 Hz */
                    unsigned nc = tx_creak_block(&p->ck, r, f0, 4915, lvl > 20000 ? 20000 : lvl, n, s->co, s->ca, NEVB);
                    creak_res(p->cr, s->co, s->ca, nc, s->buf, n);
                } else
                    creak_res(p->cr, 0, 0, 0, s->buf, n);
            }
            {   /* done: released (or snapped), no aftershock left, the creak faded */
                unsigned left = 0;
                for (a = 0; a < NAF; a++) left |= p->af[a] != 0;
                if ((!p->held || p->snapped) && !left && p->cenv < 64) { p->alive = 0; p->em = 0xFF; }
            }
        } else
            creak_res(p->cr, 0, 0, 0, s->buf, n);
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
        int32_t e = s->res, x1 = s->res_x1, dry = 32767 - (int32_t)s->p[P_MOIST] * 258;
        for (j = 0; j < n; j++) {
            int32_t x;
            e += s->bpush[j];
            if (!e) continue;
            x = (int32_t)(tx_rand(r) >> 17) - 16384;
            x1 += (x - x1) >> s->rk;                         /* a dry fracture is a bright crack (a snapping reference */
            s->buf[j] += ((x1 + ((x - x1 - x1) * dry >> 15)) * (e >> 12)) >> 1;   /* centres ~7.5 kHz): the high part */
            e -= (e >> s->rd) + 1;                           /* by dryness, the low-passed part when wet (ours) */
            if (e < 0) e = 0;
        }
        s->res = e; s->res_x1 = e ? x1 : 0;
        any |= e;
    }
    if (s->k->snap == 2u && s->p[P_LITTER]) {                 /* twigs: LITTER, its pores take the
                                                              * highs (one pole: off at 0 .. a ~1.5 kHz corner at 127) */
        int32_t y = s->lz, a = 32767 - (int32_t)s->p[P_LITTER] * 200;
        for (j = 0; j < n; j++) { y += (int32_t)(((int64_t)(s->buf[j] - y) * a) >> 15); s->buf[j] = y; }
        s->lz = y;
    }
    tx_vary_block(&s->vy, s->buf, n);
    tx_trim(s->buf, n, (OUT_TRIM * s->k->out) >> 8);
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

/* the worst case: three pieces bent hard (a fourth key: dropped) to a quick snap, strong creaks at the highest slip rate */
static const char SCENE_MAX[] =
    "0 particles 3\n0 param LOAD 0\n0 param CREAK 127\n0 param PITCH 127\n0 param SIZE 127\n"
    "0 on 53 127\n0 on 60 127\n0 on 67 127\n0 on 74 127\n"
    "2500 off 53\n2500 off 60\n2500 off 67\n2500 off 74\n"
    "4500 end\n";

const tx_engine_t tx_eng_wood = {
    "wood", "#31 Wood cracks / creak", NP, PARAMS, NP_, 3, sizeof(wood_t), SCENE_MAX,
    wood_init, wood_key, wood_param, wood_render, wood_stats,
};
