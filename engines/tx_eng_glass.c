/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #19 Glass / crystal shards: a pane breaks, its shards ring, fall and bounce (MODAL + CONTACT cores;
 * fracture scaling, glass Q).
 *
 * A key breaks a pane (velocity = the blow). Physics:
 *   crack    the fracture: a sharp broadband click (a crack runs through glass at km/s: a few samples) and the
 *            shards set ringing at once (I5: fracture excites every fragment).
 *   shards   fragment sizes log-uniform between 1 cm and L_max (p(L) ~ 1 / L: ours);
 *            each a thin plate piece whose bending modes follow f1 = 1.03 h c / L^2 (Euler-Bernoulli, h = 4 mm
 *            pane, c = sqrt(E / rho) = 4980 m/s for E 62 GPa, rho 2500) and I5's scaling (a shard
 *            x gamma bigger rings 1 / gamma^2 lower here: a plate); 3 modes 1.4-2.4 apart (the "pancake" class,
 *            ratios ours); shards whose f1 is above 18 kHz only click. Constant Q by RING (I6: "glass" >= ~400;
 *            M7 / optical glass ~6000; a shard on the floor is damped by it: lower).
 *   fall     each shard falls from its height (HEIGHT x 0.5 .. 1) to the floor: t = sqrt(2 h / g); then bounces with
 *            restitution e (BOUNCE): flight times 2 e^k v0 / g, impact speeds e^k v0, each impact a hard contact
 *            that restrikes the shard's modes phase-correctly (MODAL), each mode as driven at the spot it lands on,
 *            and clicks; stops under 5 cm/s or 6 bounces.
 *   floor    every impact of a big shard also knocks the floor: two low modes (70-150 Hz, Q 4; a shattering
 *            recording shows 65-180 Hz peaks at its impacts: ours).
 * KIND: the pane above; or
 *   bottles  two bottles knocked together (a crate, a toast): each a glass shell, its ring mode (n = 2, ~2.3 kHz for
 *            a wine bottle: the references peak there, narrow) and the next (x 2.7), and the whole bottle's free
 *            bending (~0.24 x: the references' 0.5 kHz peak); they knock, rattle 1 .. 4 times (restitution) and ring.
 *   shards   broken glass shifting and settling (swept up, walked on, a pile slipping): small shards (3.5 .. 6 cm,
 *            their f1 5 .. 15 kHz, most only clicking: the references peak at 6 .. 14 kHz) tipping over and dropping
 *            a few mm onto the others, one after another (a held key keeps them settling), bright ticks between.
 * Keys: SIZE shifts with the key (a semitone smaller shards per key up). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_modal.h"
#include "tx_core_contact.h"

#define OUT_TRIM 4981               /* output gain, Q12 (-1.4 dB): max momentary (one velocity-100 press): -18.6 LUFS at LEVEL 100 -> target -20 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +3.1 dB with it: the mean of 8 presses back on its
 * loudness (held by the peak bound: raw peaks at most 1 dB over the old ones) */
#define VARY_LO -130
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 2

enum { P_ENERGY, P_SIZE, P_RING, P_BOUNCE, P_HEIGHT, P_KIND, NP };
enum { K_PANE, K_BOTTLES, K_SHARDS };

static const tx_param_t PARAMS[NP] = {
    {"ENERGY", 127, 100, TX_TEXT("the blow: a crack .. a smash (8 .. 48 shards, louder)")},
    {"SIZE", 127, 84, TX_TEXT("largest shards at C4: 3 .. 20 cm (log): f1 = 1.03 h c / L^2 (8 cm ~3 kHz)")},
    {"RING", 127, 30, TX_TEXT("glass Q: 200 (on a soft floor) .. 6000 (crystal, hung free) (log)")},
    {"BOUNCE", 127, 50, TX_TEXT("restitution on the floor: 0.1 (carpet) .. 0.6 (stone tiles)")},
    {"HEIGHT", 127, 64, TX_TEXT("the pane's height above the floor: 0.2 .. 2 m (log): first impacts after 0.2 .. 0.64 s")},
    {"KIND", 127, 0, TX_TEXT("a pane smashing | bottles clinking | shards shifting and settling (43 each)")},
};

#define NF 48u                     /* shards */
#define NMS 3u                     /* modes per ringing shard */
#define NRING 16u                  /* ringing shards (the PARTICLES pool) */
#define A_REF 60000000             /* a mode's amplitude for the largest shard at the crack (Q30; by render) */
#define C_GLASS 4980u
/* a tumbling shard lands on another spot each time: each impact drives each of its modes SPOT_LO_L2 .. + 12 dB (log2
 * Q16; the mode's shape at that spot: a shard's knocks were one shape, scaled); the range sits 1.3 dB low so the mean
 * power stays (E g^2 = 1); the signs stay the modes' own, so a restrike still adds to the ring in phase */
#define SPOT_RANGE_L2 130624       /* 12 dB */
#define SPOT_LO_L2 (-79463)        /* -7.3 dB */

typedef struct {
    uint32_t t;                    /* samples to the next impact (from the block start) */
    int32_t v;                     /* impact speed, mm/s */
    int32_t a;                     /* this shard's amplitude scale (Q30 per m/s) */
    int32_t c;                     /* its click scale (per m/s) */
    uint8_t ring;                  /* index of its mode voice, or 0xFF (a click-only shard) */
    uint8_t k, alive, pad;
} shard_t;

typedef struct {
    tx_mode_t m[NMS];
    uint8_t used, pad[3];
} ringv_t;

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    shard_t sh[NF];
    ringv_t rv[NRING];
    tx_mode_t floor[2];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK], bpush[TX_BLOCK];
    int32_t res, res_x1;
    int32_t shn, shn_x1, shn_in;   /* the shattering's noise: envelope, high-pass state, energy still to come */
    int32_t shcap;                 /* its ticks' size at most: the loudest break's own (breaks on top of each other add
                                    * cracks, not bigger ones) */
    tx_mallet_t mal;
    uint8_t p[NP], limit, quiet;
    int32_t level, size_l2, e_q15, h_l2;
    uint32_t q_x16;
    uint16_t dropped;
    tx_vary_t vy;
    uint8_t rk, rd, kind, pad2;    /* this break's clicks: high-pass and decay shifts; KIND */
    tx_rng_t srng;                 /* where each impact lands on its shard (its own: the times stay) */
} glass_t;

static void set_param(glass_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_SIZE: s->size_l2 = -331546 + (int32_t)(v * 1412u); break;   /* log2(0.03 m) + v log2(6.67) / 127, Q16 */
    case P_RING: s->q_x16 = tx_knob_exp(v, 3200u, 321580); break;      /* Q 200 .. 6000 (x 30), Q x 16 */
    case P_BOUNCE: s->e_q15 = 3277 + (int32_t)v * 129; break;          /* 0.1 .. 0.6 */
    case P_HEIGHT: s->h_l2 = -152170 + (int32_t)(v * 1709u); break;    /* log2(0.2 m) + v log2(10) / 127 */
    case P_KIND: s->kind = (uint8_t)(v * 3u >> 7); /* fall through: the stream */
    case P_ENERGY:                                                     /* shards: a held key keeps them settling */
        tx_cloud_rate(&s->cloud, s->kind == K_SHARDS ? 512u + (uint32_t)s->p[P_ENERGY] * 12u   /* 2 .. 8 /s */
                                 : s->kind == K_BOTTLES ? 256u + (uint32_t)s->p[P_ENERGY] * 10u : 0u);   /* bottles: a
                                                              * crate jostled, 1 .. 6 knocks /s */
        tx_cloud_release(&s->cloud, s->kind == K_PANE ? 0u : 9u);   /* bottles, shards: a press starts a bout that thins
                                                              * out after the key (tau ~6 s); a source's strike (its key up
                                                              * after 150 ms) played one knock, then silence */
        break;
    default: break;
    }
}

static void glass_init(void *st, uint32_t seed)
{
    glass_t *s = (glass_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_mallet_mix(&s->mal, 0);
    s->mal.lm -= 326000;
    s->rk = 2; s->rd = 4;                                     /* a shard: ~30x lighter than the hammer row */
    s->limit = 12;
    for (i = 0; i < 2; i++) { s->floor[i].inc = (80u + i * 55u) * 97391u; s->floor[i].m16 = tx_mode_decay(80u + i * 55u, 64u, 0u); }
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((glass_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
    tx_rng_seed(&s->srng, seed ^ 0x6A55D0C5u);
}

static void glass_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((glass_t *)st)->vy);
    tx_cloud_key(&((glass_t *)st)->cloud, note, vel, on);
}

static void glass_param(void *st, uint8_t id, uint8_t val)
{
    glass_t *s = (glass_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i, k;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NRING ? NRING : val);
        for (i = s->limit; i < NRING; i++) { for (k = 0; k < NMS; k++) s->rv[i].m[k].amp = 0; s->rv[i].used = 0; }
        for (i = 0; i < NF; i++) if (s->sh[i].ring != 0xFF && s->sh[i].ring >= s->limit) s->sh[i].ring = 0xFF;
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

static int32_t isqrt_l2(int32_t l2) { return l2 / 2; }

/* an impact of shard i at offset o with speed v (mm/s): its modes restruck, a click, the floor knocked */
static void impact(glass_t *s, shard_t *h, unsigned o, int32_t v_mm)
{
    int32_t a = (int32_t)(((int64_t)h->a * v_mm) >> 10);    /* ~ v (per m/s ~ 1024 mm/s) */
    if (a > (1 << 29)) a = 1 << 29;
    if (h->ring != 0xFF) {
        ringv_t *r = &s->rv[h->ring];
        uint32_t u = s->kind == K_BOTTLES ? 0u : tx_rand(&s->srng);   /* (a bottle: knocked shell to shell, its
                                                              * weights below) */
        unsigned k;
        for (k = 0; k < NMS; k++) if (r->m[k].inc) {
            int32_t ak = s->kind == K_BOTTLES ? a >> (k == 1u ? 0 : k == 0u ? 2 : 1) : a >> k;   /* (a bottle: its shell ring
                                                              * first; the whole bottle's bending -12 dB: the references'
                                                              * 0.5 kHz octave 5-12 dB under ours) */
            if (s->kind != K_BOTTLES) {                      /* where it lands: the mode's drive there */
                int32_t g = (int32_t)tx_exp2((int32_t)((((u >> (8u * k)) & 255u) * (uint32_t)SPOT_RANGE_L2) >> 8)
                                             + SPOT_LO_L2 + (8 << 16));   /* Q8: 0.43 .. 1.72 */
                ak = (int32_t)(((int64_t)ak * g) >> 8);
            }
            tx_mode_strike(&r->m[k], (k & 1u) ? -ak : ak);
        }
        if (h->a > A_REF / 3 && s->kind == K_PANE)                                /* the floor: the big shards only, softly (ours; every */
            tx_mode_strike(&s->floor[o & 1u], a >> 5);      /* ringing shard at -18 dB made a pulsing low bed between the
                                                              * breaks: session 5's spectrogram review) */
    }
    s->bpush[o] += (int32_t)(((int64_t)h->c * v_mm) >> (h->ring == 0xFF ? 12 : 10));   /* the click (every shard: ~ L,
                                                              * ours; the chips that do not ring -12 dB: together they
                                                              * were a hiss under the shards, session 5) */
}

static void glass_break(glass_t *s, const tx_event_t *e)
{
    tx_rng_t *r = &s->cloud.rng;
    unsigned n = 8u + ((uint32_t)s->p[P_ENERGY] * 40u >> 7), i, j, ringn = 0;
    /* no two breaks alike (note 17: the references' per-event peak spread 2.3 octaves, ours 1.2): each pane breaks
     * into its own largest pieces (sd 0.45 octave of size: 0.9 of pitch), and its cracks have their own edge */
    int32_t lmax_l2 = s->size_l2 - ((int32_t)e->note - 60) * 5461
                    + (int32_t)(((int64_t)tx_rand_s15(&s->cloud.rng) + tx_rand_s15(&s->cloud.rng) + tx_rand_s15(&s->cloud.rng)) * 29491 >> 15);
    int32_t ve = 65536 + ((int32_t)e->vel - 100) * 1638;   /* log2 of the blow speed (2 m/s at v100) */
    uint32_t tc;
    int32_t er;
    tx_contact_hit(&s->mal, 65536, &tc, &er);
    s->rk = (uint8_t)(1u + tx_rand_below(r, 3u));           /* the clicks' corner ~9 / 3.5 / 1.7 kHz (was 3.5) */
    s->rd = (uint8_t)(3u + tx_rand_below(r, 3u));           /* their tau ~0.2 / 0.4 / 0.7 ms (was 0.4) */
    for (j = 0; j < n; j++) {
        shard_t *h = 0;
        int32_t l2, f_l2, hl2;
        uint32_t f1;
        for (i = 0; i < NF && !h; i++) if (!s->sh[i].alive) h = &s->sh[i];
        if (!h) { s->dropped++; break; }
        /* size: log-uniform (p(L) ~ 1 / L) from 1 cm to L_max (ours: ~40 % of the shards ring audibly) */
        l2 = -435412 + (int32_t)(((uint32_t)(lmax_l2 + 435412 > 0 ? lmax_l2 + 435412 : 0) >> 8) * (tx_rand(r) >> 24));
        f_l2 = 285700 - 2 * l2;                              /* f1 = 1.03 x 0.004 x 4980 / L^2 = 20.5 / L^2 */
        f1 = f_l2 > (15 << 16) ? 0u : tx_exp2(f_l2);
        h->a = (int32_t)(((int64_t)A_REF * (int32_t)tx_exp2((l2 - lmax_l2) * 3 / 2 + (15 << 16))) >> 15);   /* ~ L^1.5 */
        h->a = (int32_t)(((int64_t)h->a * s->level) >> 15);
        h->c = (int32_t)((((int64_t)A_REF * s->level) >> 15) * (int32_t)tx_exp2(l2 - lmax_l2 + (15 << 16)) >> 26);   /* ~ L */
        h->ring = 0xFF;
        if (f1 && f1 < 18000u) {                             /* audible: a mode voice */
            unsigned rv = s->limit;
            for (i = 0; i < s->limit && rv == s->limit; i++) if (!s->rv[i].used) rv = i;
            if (rv < s->limit) {
                ringv_t *v = &s->rv[rv];
                uint32_t f = f1, k;
                for (k = 0; k < NMS; k++) {
                    if (k) f = (uint32_t)(((uint64_t)f * tx_exp2(31785 + (int32_t)tx_rand_below(r, 51000u) + (16 << 16))) >> 16);   /* x 1.4 .. 2.4 */
                    v->m[k].amp = 0; v->m[k].ph = 0;
                    v->m[k].inc = f < 15000u ? f * 97391u : 0u;   /* (modes over 15 kHz left out: the air */
                    v->m[k].m16 = f < 15000u ? tx_mode_decay(f, s->q_x16, 179u) : 0u;   /* takes them; CPU) */
                }
                v->used = 1;
                h->ring = (uint8_t)rv;
                ringn++;
            } else s->dropped++;
        }
        /* the fall: from HEIGHT x 0.5 .. 1: t = sqrt(2 h / g), v0 = sqrt(2 g h) */
        hl2 = s->h_l2 - (int32_t)tx_rand_below(r, 65536u);
        h->t = e->offset + (uint32_t)(((uint64_t)tx_exp2(isqrt_l2(hl2 + 65536 - 215900) + (16 << 16)) * TX_FS) >> 16);   /* sqrt(2h/9.81) s */
        h->v = (int32_t)tx_exp2(isqrt_l2(hl2 + 65536 + 215900) + (10 << 16) - 2228);   /* sqrt(2 g h) m/s -> mm/s */
        h->k = 0; h->alive = 1;
        {   /* the crack sets it ringing (I5) */
            int32_t a = (int32_t)(((int64_t)h->a * 3 * (int32_t)tx_exp2(ve - 65536 + (15 << 16))) >> 15);   /* the blow: ~3 m/s at the crack */
            if (h->ring != 0xFF) {
                ringv_t *v = &s->rv[h->ring];
                unsigned k;
                for (k = 0; k < NMS; k++) if (v->m[k].inc) tx_mode_strike(&v->m[k], (int32_t)(((int64_t)(a >> k) * tx_contact_weight(tc, v->m[k].inc / 97391u)) >> 15));
            }
        }
    }
    s->bpush[e->offset] += (int32_t)((((int64_t)A_REF * s->level) >> 15) >> 7);   /* the crack itself */
    {   /* the shattering: the pane breaks in many cracks within a few ms, each a sharp spike (a recording's first
         * 100 ms is its loudest: -4 dB re its peak; ours) */
        int32_t sp = (int32_t)((((int64_t)A_REF * s->level) >> 15) >> 7), v2 = ((int32_t)e->vel * 258);
        uint32_t o = e->offset, c;
        sp = (int32_t)(((int64_t)sp * v2) >> 15);
        for (c = 0; c < 14u && o < TX_BLOCK; c++) {
            s->buf[o] += (c & 1u) ? -sp : sp;
            s->bpush[o] += sp >> 4;
            o += 2u + tx_rand_below(r, 20u);
            sp -= sp >> 3;
        }
        int32_t add = (int32_t)(((int64_t)(((int64_t)A_REF * s->level) >> 15) * v2) >> 15) * 3 >> 4;   /* ~0.1 s of breaking */
        s->shn_in = s->shn_in > add ? s->shn_in + (add >> 2) : add + (s->shn_in >> 2);   /* noise; breaks add ~ in power */
        if (add - (add >> 2) > s->shcap) s->shcap = add - (add >> 2);   /* (one break's envelope peaks at 0.63 x add) */
    }
    {   /* the blow knocks the frame / floor: the low body of the break (a recording's loudest moment, 65-180 Hz) */
        int32_t b = (int32_t)(((int64_t)A_REF * 3 * s->level) >> 16);   /* (-6 dB: the 62-125 Hz octave was -9 .. -15 dB
                                                              * of the whole, the references' -14 .. -29) */
        tx_mode_strike(&s->floor[0], b);
        tx_mode_strike(&s->floor[1], -b / 2);
    }
    (void)ringn;
}

/* a free mode voice (or NRING: none) */
static unsigned ring_take(glass_t *s, unsigned lim)
{
    unsigned i;
    for (i = 0; i < s->limit && i < lim; i++) if (!s->rv[i].used) return i;
    return NRING;
}

static shard_t *shard_take(glass_t *s)
{
    unsigned i;
    for (i = 0; i < NF; i++) if (!s->sh[i].alive) return &s->sh[i];
    s->dropped++;
    return 0;
}

/* two bottles knocked together: each its own pitch (+-0.35 oct: other bottles, fills), knocking 1 .. 4 times */
static void bottles(glass_t *s, const tx_event_t *e)
{
    tx_rng_t *r = &s->cloud.rng;
    int32_t f_l2 = 732200 - (s->size_l2 + 216000) + ((int32_t)e->note - 60) * 5461;   /* 2.3 kHz at SIZE 84, C4 */
    int32_t v0 = 120 + (int32_t)e->vel * 3 + (int32_t)tx_rand_below(r, 200u), b;   /* mm/s: a knock .. a clash */
    s->rk = (uint8_t)(2u + tx_rand_below(r, 2u));
    s->rd = (uint8_t)(2u + tx_rand_below(r, 2u));            /* (a glass-on-glass tick: ~0.1 .. 0.2 ms) */
    for (b = 0; b < 2; b++) {
        shard_t *h = shard_take(s);
        unsigned rv = ring_take(s, NRING), k;
        int32_t fl = f_l2 + (int32_t)(((int64_t)tx_rand_s15(r) * 23000) >> 15);
        if (!h) return;
        h->ring = 0xFF;
        if (rv < NRING) {
            ringv_t *v = &s->rv[rv];
            static const int32_t RAT[NMS] = {-127000, 0, 93900};   /* log2: x 0.26 (the bottle), 1 (n = 2), 2.7 (n = 3) */
            for (k = 0; k < NMS; k++) {
                uint32_t f = tx_exp2(fl + RAT[k] + (int32_t)(((int64_t)tx_rand_s15(r) * 4000) >> 15));
                v->m[k].amp = 0; v->m[k].ph = 0;
                v->m[k].inc = f < 15000u ? f * 97391u : 0u;
                v->m[k].m16 = f < 15000u ? tx_mode_decay(f, s->q_x16, 179u) : 0u;
            }
            v->used = 1;
            h->ring = (uint8_t)rv;
        } else s->dropped++;
        h->a = (int32_t)(((int64_t)(A_REF * 3) * s->level) >> 15);   /* (by render: the pane's loudness) */
        h->a = (int32_t)(((int64_t)h->a * 28200) >> 15);    /* (-1.3 dB: more knocks, the level kept) */
        h->c = (int32_t)(((((int64_t)A_REF * s->level) >> 15) >> 4) * 28200 >> 15);   /* (glass on glass: a bright tick; the references
                                                              * keep 8-16 kHz ~22 dB under the peak octave: was 9, -40) */
        h->t = e->offset;
        h->v = v0;
        h->k = (uint8_t)(6u - tx_rand_below(r, 4u));         /* 1 .. 4 knocks before it stops */
        h->alive = 1;
    }
}

/* shards shifting: 2 .. 6 small ones tip over and drop a few mm, one after another over ~0.3 s */
static void shards(glass_t *s, const tx_event_t *e)
{
    tx_rng_t *r = &s->cloud.rng;
    unsigned n = 2u + ((uint32_t)s->p[P_ENERGY] * 5u >> 7), j;
    int32_t lmax_l2 = s->size_l2 - 59000 - ((int32_t)e->note - 60) * 5461;   /* 0.9 oct under the pane's largest */
    s->rk = (uint8_t)(1u + tx_rand_below(r, 3u)); s->rd = (uint8_t)(2u + tx_rand_below(r, 2u));   /* (bright ticks) */
    for (j = 0; j < n; j++) {
        shard_t *h = shard_take(s);
        int32_t l2 = lmax_l2 - (int32_t)tx_rand_below(r, 49152u), f_l2 = 285700 - 2 * l2;   /* over 0.75 oct */
        uint32_t f1 = f_l2 > (15 << 16) ? 0u : tx_exp2(f_l2), rv;
        if (!h) return;
        h->ring = 0xFF;
        if (f1 && f1 < 15000u && (rv = ring_take(s, NRING / 2u)) < NRING) {   /* (at most 8 ring: CPU) */
            ringv_t *v = &s->rv[rv];
            uint32_t f = f1, k;
            for (k = 0; k < NMS; k++) {
                if (k) f = (uint32_t)(((uint64_t)f * tx_exp2(31785 + (int32_t)tx_rand_below(r, 51000u) + (16 << 16))) >> 16);
                v->m[k].amp = 0; v->m[k].ph = 0;
                v->m[k].inc = f < 15000u && k < 2u ? f * 97391u : 0u;   /* (two modes: the third over ~12 kHz) */
                v->m[k].m16 = f < 15000u && k < 2u ? tx_mode_decay(f, s->q_x16, 179u) : 0u;
            }
            v->used = 1;
            h->ring = (uint8_t)rv;
        }
        h->a = (int32_t)(((int64_t)A_REF * 4 * (int32_t)tx_exp2((l2 - lmax_l2) * 3 / 2 + (15 << 16))) >> 15);   /* (x 4: */
        h->a = (int32_t)(((int64_t)h->a * s->level) >> 15);                                                 /* by render) */
        h->c = (int32_t)((((int64_t)A_REF * s->level) >> 15) * (int32_t)tx_exp2(l2 - lmax_l2 + (15 << 16)) >> 17);
        h->t = e->offset + tx_rand_below(r, 13230u);         /* within 0.3 s */
        h->v = 150 + (int32_t)tx_rand_below(r, 250u) + ((int32_t)e->vel - 100);   /* a drop of 1 .. 8 mm */
        h->k = 4;                                            /* (a bounce or two, then perhaps a late slip) */
        h->alive = 1;
    }
}

TX_AUDIO_CODE static void glass_render(void *st, int32_t *mix, unsigned n)
{
    glass_t *s = (glass_t *)st;
    unsigned k, i, j;
    uint16_t d0 = s->dropped;
    int32_t any = 0;
    if (n > TX_BLOCK) n = TX_BLOCK;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;
    for (j = 0; j < n; j++) s->buf[j] = s->bpush[j] = 0;
    for (i = 0; i < k; i++)
        if (s->kind == K_BOTTLES) bottles(s, &s->ev[i]);
        else if (s->kind == K_SHARDS) shards(s, &s->ev[i]);
        else glass_break(s, &s->ev[i]);
    /* impacts of this block, shard by shard; a ringing shard renders up to each of its impacts */
    for (i = 0; i < NF; i++) {
        shard_t *h = &s->sh[i];
        if (!h->alive) continue;
        any = 1;
        while (h->alive && h->t < n) {
            unsigned o = h->t;
            impact(s, h, o, h->v);
            h->k++;
            if (s->kind == K_BOTTLES) {                      /* held in a hand or a crate: knocked again as it jiggles,
                                                              * 60 .. 225 ms later (log-uniform), each 0.4 .. 0.9 as fast,
                                                              * no tumble (second pass: the references' gaps median
                                                              * 230 ms, a third under 150 ms; ours bounced off at BOUNCE's
                                                              * e ~0.3: one knock and a tick 30 ms later, gaps 370-420 ms) */
                h->v = (int32_t)(((int64_t)h->v * (13107 + (int32_t)tx_rand_below(&s->cloud.rng, 16384u))) >> 15);
                if (h->v < 50 || h->k > 6) { h->alive = 0; break; }
                h->t += (uint32_t)((2646u * (uint64_t)tx_exp2((int32_t)tx_rand_below(&s->cloud.rng, 124518u) + (16 << 16))) >> 16);
                continue;
            }
            h->v = (int32_t)(((int64_t)h->v * s->e_q15) >> 15);
            if (h->v < 50 || (h->k > 6 && h->k < 16)) {     /* bounced out: it settles, tumbles, slides into others: */
                uint32_t l;                                  /* 0-6 late knocks, Omori-like (c 0.2 s, p 1.5) over */
                if (h->k >= 16 + 6 || (tx_rand(&s->cloud.rng) >> 28) == 0) { h->alive = 0; break; }   /* ~3 s (ours) */
                if (h->k < 16) h->k = 16;
                l = tx_neglog2_q12(tx_rand(&s->cloud.rng));
                if (l > 6144u) l = 6144u;                    /* <= 1.4 s after (u >= 2^-1.5) */
                h->t += (uint32_t)((((uint64_t)tx_exp2((int32_t)(l * 32u) + (16 << 16)) - 65536u) * 8820u) >> 16);
                h->v = 100 + (int32_t)tx_rand_below(&s->cloud.rng, 500u);
                continue;
            }
            h->t += ((uint32_t)h->v * 9207u) >> 10;          /* flight 2 v / g: 2 / 9.81 x fs / 1000 = 8.99 per mm/s */
        }
        if (h->alive) h->t -= n;
    }
    for (i = 0; i < NRING; i++) {
        ringv_t *v = &s->rv[i];
        unsigned m, live = 0, held = 0;
        if (!v->used) continue;
        tx_modal_run(v->m, NMS, s->buf, 0, n);              /* (strikes land at the block start: <= 5.8 ms) */
        for (m = 0; m < NMS; m++) live |= v->m[m].amp != 0;
        for (m = 0; m < NF; m++) held |= s->sh[m].alive && s->sh[m].ring == i;
        if (!live && !held) v->used = 0;
        any |= (int32_t)v->used;
    }
    tx_modal_run(s->floor, 2, s->buf, 0, n);
    any |= s->floor[0].amp | s->floor[1].amp;
    if (s->shn | s->shn_in) {                                /* the shattering: rising over ~25 ms, falling with */
        int32_t e = s->shn, x1 = s->shn_x1, in = s->shn_in;  /* tau ~90 ms (ours). Not a noise (the user, session 5: */
        for (j = 0; j < n; j++) {                            /* "an unpleasant white-noise background"): the pane */
            uint32_t u = tx_rand(&s->cloud.rng);             /* breaks in many separate cracks and chips, each a */
            if ((u >> 16) < (uint32_t)(e >> 12)) {           /* sharp tick of its own size (~3 k/s at the peak, */
                int32_t a = ((e < s->shcap ? e : s->shcap) >> 14) * (int32_t)((u & 0xFFFFu) * (u & 0xFFFFu) >> 18);   /* a u^2 level: a few */
                s->buf[j] += (u & 0x10000u) ? a : -a;        /* loud, most faint; ours) */
                s->bpush[j] += a >> 10;
            }
            (void)x1;
            e += (in >> 10) - (e >> 12) - 1;
            in -= in >> 10;
            if (in < 1024) in = 0;
            if (e < 0) e = 0;
            if (!e && !in) break;
        }
        s->shn = e; s->shn_in = in; s->shn_x1 = e ? x1 : 0;
        if (!e && !in) s->shcap = 0;
        any |= e | in;
    }
    {   /* the clicks: noise bursts, tau ~0.4 ms (glass) */
        int32_t e = s->res, x1 = s->res_x1;
        for (j = 0; j < n; j++) {
            int32_t x;
            e += s->bpush[j];
            if (!e) continue;
            x = (int32_t)(tx_rand(&s->cloud.rng) >> 17) - 16384;
            x1 += (x - x1) >> s->rk;                         /* (a slight low-pass: ~3.5 kHz corner, then +6 dB/oct */
            s->buf[j] += ((x - x1) * (e >> 12)) >> 3;        /* below it: bright, as the recordings' 16 kHz) */
            e -= (e >> s->rd) + 1;
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

static void glass_stats(const void *st, tx_stats_t *o)
{
    const glass_t *s = (const glass_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < NRING; i++) n += s->rv[i].used;
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: crystal (long rings) smashed again and again, big shards, high and bouncy */
static const char SCENE_MAX[] =
    TX_TEXT("0 particles 16\n0 param ENERGY 127\n0 param RING 127\n0 param SIZE 127\n0 param BOUNCE 127\n0 param HEIGHT 127\n"
    "0 on 53 127\n100 off 53\n300 on 60 127\n400 off 60\n600 on 67 127\n700 off 67\n900 on 79 127\n1000 off 79\n"
    "4000 end\n");

const tx_engine_t tx_eng_glass = {
    "glass", TX_TEXT("#19 Glass / crystal shards"), NP, PARAMS, NRING, 12, TX_STATE_SIZE(glass_t), SCENE_MAX,
    glass_init, glass_key, glass_param, glass_render, glass_stats,
};
