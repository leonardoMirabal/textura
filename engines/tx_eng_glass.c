/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #19 Glass / crystal shards: a pane breaks, its shards ring, fall and bounce (MODAL + CONTACT cores;
 *: I5 fracture scaling, I6 / M7 glass Q).
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
 *            that restrikes the shard's modes phase-correctly (MODAL) and clicks; stops under 5 cm/s or 6 bounces.
 *   floor    every impact of a big shard also knocks the floor: two low modes (70-150 Hz, Q 4; a shattering
 *            recording shows 65-180 Hz peaks at its impacts: ours).
 * Keys: SIZE shifts with the key (a semitone smaller shards per key up). */
#include "tx_engine.h"
#include "tx_core_cloud.h"
#include "tx_core_modal.h"
#include "tx_core_contact.h"

enum { P_ENERGY, P_SIZE, P_RING, P_BOUNCE, P_HEIGHT, NP };

static const tx_param_t PARAMS[NP] = {
    {"ENERGY", 127, 100, "the blow: a crack .. a smash (8 .. 48 shards, louder)"},
    {"SIZE", 127, 84, "largest shards at C4: 3 .. 20 cm (log): f1 = 1.03 h c / L^2 (8 cm ~3 kHz)"},
    {"RING", 127, 30, "glass Q: 200 (on a soft floor) .. 6000 (crystal, hung free) (log)"},
    {"BOUNCE", 127, 50, "restitution on the floor: 0.1 (carpet) .. 0.6 (stone tiles)"},
    {"HEIGHT", 127, 64, "the pane's height above the floor: 0.2 .. 2 m (log): first impacts after 0.2 .. 0.64 s"},
};

#define NF 48u                     /* shards */
#define NMS 3u                     /* modes per ringing shard */
#define NRING 16u                  /* ringing shards (the PARTICLES pool) */
#define A_REF 60000000             /* a mode's amplitude for the largest shard at the crack (Q30; by render) */
#define C_GLASS 4980u

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
    tx_mallet_t mal;
    uint8_t p[NP], limit, quiet;
    int32_t level, size_l2, e_q15, h_l2;
    uint32_t q_x16;
    uint16_t dropped;
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
    default: break;
    }
}

static void glass_init(void *st, uint32_t seed)
{
    glass_t *s = (glass_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    tx_mallet_mix(&s->mal, 0);
    s->mal.lm -= 326000;                                     /* a shard: ~30x lighter than the hammer row */
    s->limit = 12;
    for (i = 0; i < 2; i++) { s->floor[i].inc = (80u + i * 55u) * 97391u; s->floor[i].m16 = tx_mode_decay(80u + i * 55u, 64u, 0u); }
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
}

static void glass_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
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
        unsigned k;
        for (k = 0; k < NMS; k++) if (r->m[k].inc) tx_mode_strike(&r->m[k], (k & 1u) ? -(a >> (k + 0)) : a >> k);
        tx_mode_strike(&s->floor[o & 1u], a >> 3);          /* the floor: big shards only (ours) */
    }
    s->bpush[o] += (int32_t)(((int64_t)h->c * v_mm) >> 10);   /* the click (every shard: ~ L, ours) */
}

static void glass_break(glass_t *s, const tx_event_t *e)
{
    tx_rng_t *r = &s->cloud.rng;
    unsigned n = 8u + ((uint32_t)s->p[P_ENERGY] * 40u >> 7), i, j, ringn = 0;
    int32_t lmax_l2 = s->size_l2 - ((int32_t)e->note - 60) * 5461;
    int32_t ve = 65536 + ((int32_t)e->vel - 100) * 1638;   /* log2 of the blow speed (2 m/s at v100) */
    uint32_t tc;
    int32_t er;
    tx_contact_hit(&s->mal, 65536, &tc, &er);
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
                    v->m[k].inc = f < 20000u ? f * 97391u : 0u;
                    v->m[k].m16 = f < 20000u ? tx_mode_decay(f, s->q_x16, 179u) : 0u;
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
    }
    {   /* the blow knocks the frame / floor: the low body of the break (a recording's loudest moment, 65-180 Hz) */
        int32_t b = (int32_t)(((int64_t)A_REF * 3 * s->level) >> 15);
        tx_mode_strike(&s->floor[0], b);
        tx_mode_strike(&s->floor[1], -b / 2);
    }
    (void)ringn;
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
    for (i = 0; i < k; i++) glass_break(s, &s->ev[i]);
    /* impacts of this block, shard by shard; a ringing shard renders up to each of its impacts */
    for (i = 0; i < NF; i++) {
        shard_t *h = &s->sh[i];
        if (!h->alive) continue;
        any = 1;
        while (h->alive && h->t < n) {
            unsigned o = h->t;
            impact(s, h, o, h->v);
            h->k++;
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
    if (s->shn | s->shn_in) {                                /* the shattering: bright noise rising over ~25 ms, */
        int32_t e = s->shn, x1 = s->shn_x1, in = s->shn_in;  /* falling with tau ~90 ms (ours) */
        for (j = 0; j < n; j++) {
            int32_t x = (int32_t)(tx_rand(&s->cloud.rng) >> 17) - 16384;
            x1 += (x - x1) >> 1;                             /* white, gently low-passed (~7 kHz) */
            s->buf[j] += (x1 * (e >> 14)) >> 1;
            e += (in >> 10) - (e >> 12) - 1;
            in -= in >> 10;
            if (in < 1024) in = 0;
            if (e < 0) e = 0;
            if (!e && !in) break;
        }
        s->shn = e; s->shn_in = in; s->shn_x1 = e ? x1 : 0;
        any |= e | in;
    }
    {   /* the clicks: noise bursts, tau ~0.4 ms (glass) */
        int32_t e = s->res, x1 = s->res_x1;
        for (j = 0; j < n; j++) {
            int32_t x;
            e += s->bpush[j];
            if (!e) continue;
            x = (int32_t)(tx_rand(&s->cloud.rng) >> 17) - 16384;
            x1 += (x - x1) >> 2;                             /* (a slight low-pass: ~3.5 kHz corner, then +6 dB/oct */
            s->buf[j] += ((x - x1) * (e >> 12)) >> 3;        /* below it: bright, as the recordings' 16 kHz) */
            e -= (e >> 4) + 1;
            if (e < 0) e = 0;
        }
        s->res = e; s->res_x1 = e ? x1 : 0;
        any |= e;
    }
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
    "0 particles 16\n0 param ENERGY 127\n0 param RING 127\n0 param SIZE 127\n0 param BOUNCE 127\n0 param HEIGHT 127\n"
    "0 on 53 127\n100 off 53\n300 on 60 127\n400 off 60\n600 on 67 127\n700 off 67\n900 on 79 127\n1000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_glass = {
    "glass", "#19 Glass / crystal shards", NP, PARAMS, NRING, 12, sizeof(glass_t), SCENE_MAX,
    glass_init, glass_key, glass_param, glass_render, glass_stats,
};
