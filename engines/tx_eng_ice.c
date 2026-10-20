/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #32 Ice crashing: icicles falling and shattering, or ice cubes in a glass (MODAL,
 * CONTACT, INSECT resonators, CLOUD).
 *
 * ACTION picks the scene:
 *   icicles  a key drops an icicle (its length by the key): it hits the frozen ground, rings for a moment (a free-free
 *            ice rod: f1 = 3.56 (d / 4) c / L^2, c = sqrt(E / rho) = 3140 m/s (E 9.04 GPa, rho 917: C3), d = L / 15
 *            (ours) -> f1 ~ 0.059 c / L: a 30 cm icicle ~620 Hz, note 09's example), damped by the ground it lies on
 *            (sigma0 ~60 /s, ours), cracks into chunks (cracks run at up to 1320 m/s, C7: each a 1-7 sample click; a
 *            few in ~20 ms, ours) that bounce with little restitution (ice on ice / snow: 0.1-0.3, ours) and thud
 *            on the ground (icicle recordings: thumps at 60-300 Hz, -30 dB in 6-65 ms, high crackle above).
 *   cubes    ice cubes in a glass: cubes ring ultrasonically (a 3 cm cube ~100 kHz: note 09), so a clink is the
 *            contact click and the glass: 4 modes 1 : 2.67 : 5.05 : 7.95 (a recording of cubes in a glass: 519 /
 *            1386 / 2620 / 4126 Hz; the ratios ours from it), Q ~250 with the drink (-30 dB after 0.14-0.6 s in it).
 *            A press drops cubes in (a burst), holding shakes the glass (Poisson clinks, AMOUNT x velocity).
 *   both     TEMP sets the ice's Q (C10, natural ice: ~290-650 near -20 C, 40-100 above about -5 C) and how wet it is;
 *            FIZZ: melting ice releases bubbles, a fizz of many small events at 1-3 kHz (C8 / C11): sparse
 *            impulses into a broad 2 kHz resonance, louder near 0 C.
 * Keys: the icicle length / the glass size, a semitone per key. Sheet break-up (chirps) is #18 Lake ice. */
#include "tx_engine.h"
#include "tx_core_cloud.h"
#include "tx_core_modal.h"
#include "tx_core_contact.h"
#include "tx_core_insect.h"

enum { P_ACTION, P_TEMP, P_AMOUNT, P_SIZE, P_FIZZ, NP };

static const tx_param_t PARAMS[NP] = {
    {"ACTION", 127, 0, "icicles falling and shattering | ice cubes in a glass"},
    {"TEMP", 127, 64, "ice temperature -40 .. 0 C: ringing (Q ~500) .. wet and dull (Q ~50), more fizz"},
    {"AMOUNT", 127, 64, "icicles: chunks per break 3 .. 24 | cubes: shaking 1 .. 15 clinks/s"},
    {"SIZE", 127, 64, "icicle length 10 .. 80 cm | glass: lowest mode 300 .. 1200 Hz (keys a semitone)"},
    {"FIZZ", 127, 20, "melt fizz: off .. strong (bubbles at 1-3 kHz)"},
};

#define NV 8u                      /* ringing bodies (icicles or glasses) */
#define NMB 4u
#define NF 32u                     /* chunks */
#define A_REF 100000000            /* a mode's amplitude for a v100 impact (Q30; calibrated by render) */

static const uint16_t ROD_R[NMB] = {1024, 2823, 5534, 9147};      /* free-free, Q10 */
static const uint16_t GLS_R[NMB] = {1024, 2734, 5171, 8141};      /* the glass recording's ratios, Q10 */
static const uint16_t Q_T[9] = {400, 450, 500, 480, 420, 300, 150, 90, 50};   /* -40 .. 0 C by 5 C (C10 natural) */

typedef struct {
    tx_mode_t m[NMB];
    uint8_t used, note, pad[2];
} body_t;

typedef struct {
    uint32_t t;                    /* samples to its next impact */
    int32_t v;                     /* impact speed, mm/s */
    int32_t a;                     /* amplitude scale */
    uint8_t alive, k, pad[2];
} chunk_t;

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    body_t b[NV];
    chunk_t ch[NF];
    tx_mode_t thud[2];
    tx_ireso_t fz;
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK], bpush[TX_BLOCK];
    uint16_t fo[64];
    int16_t fa[64];
    int32_t res, res_x1;
    tx_mallet_t mal;
    uint8_t p[NP], limit, quiet;
    int32_t level, size_l2;
    uint32_t q_x16, fizz;
    uint16_t dropped;
} ice_t;

static unsigned cubes(const ice_t *s) { return s->p[P_ACTION] >= 64u; }

static void set_param(ice_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    {
        uint32_t x = (uint32_t)s->p[P_TEMP] * 8u, i = x >> 7, f = x & 127u;   /* -40 .. 0 C over the table */
        uint32_t q = i >= 8u ? Q_T[8] : (Q_T[i] * (128u - f) + Q_T[i + 1] * f) >> 7;
        s->q_x16 = q * 16u;
        s->fizz = (uint32_t)s->p[P_FIZZ] * (64u + (uint32_t)s->p[P_TEMP]) >> 6;   /* warmer: more fizz (ours) */
    }
    if (cubes(s)) tx_cloud_rate(&s->cloud, 256u + (uint32_t)s->p[P_AMOUNT] * 28u);   /* 1 .. 15 /s */
    else tx_cloud_rate(&s->cloud, 0);
    if (id == P_SIZE) s->size_l2 = (int32_t)v * 1714;        /* 0 .. +log2(8) (icicle 10-80 cm) / 0 .. 2 oct (glass) */
}

static void ice_init(void *st, uint32_t seed)
{
    ice_t *s = (ice_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_mallet_mix(&s->mal, 0);
    s->mal.lm -= 217706;
    for (i = 0; i < 2; i++) { s->thud[i].inc = (70u + i * 75u) * 97391u; s->thud[i].m16 = tx_mode_decay(70u + i * 75u, 40u, 0u); }
    tx_ireso_set(&s->fz, 2000, 24);                          /* the fizz: 2 kHz, Q 1.5 (C8: 1-3 kHz) */
    s->fz.g = 256;
    s->limit = 6;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
}

static void ice_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    tx_cloud_key(&((ice_t *)st)->cloud, note, vel, on);
}

static void ice_param(void *st, uint8_t id, uint8_t val)
{
    ice_t *s = (ice_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i, k;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NV ? NV : val);
        for (i = s->limit; i < NV; i++) { for (k = 0; k < NMB; k++) s->b[i].m[k].amp = 0; s->b[i].used = 0; }
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* a body (icicle or glass) for this key, struck with a (Q30) */
static void body_strike(ice_t *s, unsigned note, int32_t a, uint32_t tc)
{
    body_t *b = 0;
    unsigned i, k, cu = cubes(s);
    uint32_t f1;
    for (i = 0; i < s->limit && !b; i++) if (s->b[i].used && s->b[i].note == note) b = &s->b[i];
    for (i = 0; i < s->limit && !b; i++) if (!s->b[i].used) b = &s->b[i];
    if (!b) {
        int32_t lo = 0x7FFFFFFF;
        for (i = 0; i < s->limit; i++) if (s->b[i].m[0].amp < lo) { lo = s->b[i].m[0].amp; b = &s->b[i]; }
        s->dropped++;
    }
    if (!b) return;
    if (!b->used || b->note != note) {
        if (cu) f1 = tx_exp2(539293 + s->size_l2 * 2 / 3 + ((int32_t)note - 60) * 5461);   /* 300 Hz x 2^(0..2) */
        else f1 = tx_exp2(tx_log2_q16(3140u) - 267150 + 217706 - s->size_l2 + ((int32_t)note - 60) * 5461);   /* 0.059 c / L */
        for (k = 0; k < NMB; k++) {
            uint32_t f = (f1 * (cu ? GLS_R[k] : ROD_R[k])) >> 10;
            b->m[k].amp = 0; b->m[k].ph = 0;
            b->m[k].inc = f < 20000u ? f * 97391u : 0u;
            /* the glass rings with the drink (Q ~250, ours from the recording); the icicle with the ice's Q, damped by
             * the ground it lies on (sigma0 60 /s, ours) */
            b->m[k].m16 = f < 20000u ? (cu ? tx_mode_decay(f, 4000u, 256u) : tx_mode_decay(f, s->q_x16, 60u * 256u)) : 0u;
        }
        b->note = (uint8_t)note;
    }
    for (k = 0; k < NMB; k++)
        if (b->m[k].inc) tx_mode_strike(&b->m[k], (int32_t)(((int64_t)(k ? a - (a >> 2) : a) * tx_contact_weight(tc, b->m[k].inc / 97391u)) >> 15) * ((k & 1u) ? -1 : 1));
    b->used = 1;
}

static void icicle(ice_t *s, const tx_event_t *e)
{
    tx_rng_t *r = &s->cloud.rng;
    int32_t lv = 65536 + ((int32_t)e->vel - 100) * 1638, er, a;
    uint32_t tc, n = 3u + ((uint32_t)s->p[P_AMOUNT] * 21u >> 7), j, i, c;
    tx_contact_hit(&s->mal, lv, &tc, &er);
    a = (int32_t)(((int64_t)A_REF * (int32_t)tx_exp2(lv - 65536 + (15 << 16))) >> 15);
    a = (int32_t)(((int64_t)a * s->level) >> 15);
    body_strike(s, e->note, a, tc);
    tx_mode_strike(&s->thud[0], a / 3);                      /* the ground */
    tx_mode_strike(&s->thud[1], -a / 6);
    s->bpush[e->offset] += a >> 8;                           /* the first crack; 1-4 more within ~20 ms follow as */
    for (c = 0, j = e->offset; c < 1u + (tx_rand(r) >> 30); c++) {   /* one-impact chunks (k = 4: no bounce) */
        chunk_t *h = 0;
        j += 40u + tx_rand_below(r, 300u);
        for (i = 0; i < NF && !h; i++) if (!s->ch[i].alive) h = &s->ch[i];
        if (!h) break;
        h->t = j; h->v = 2048; h->a = a >> (1 + c); h->k = 4; h->alive = 1;
    }
    for (j = 0; j < n; j++) {                                /* the chunks fly and bounce */
        chunk_t *h = 0;
        for (i = 0; i < NF && !h; i++) if (!s->ch[i].alive) h = &s->ch[i];
        if (!h) { s->dropped++; break; }
        h->t = e->offset + 1500u + tx_rand_below(r, 12000u);  /* 30-300 ms aloft (ours) */
        h->v = 300 + (int32_t)tx_rand_below(r, 1500u);
        h->a = (int32_t)(((int64_t)a * (int32_t)(4000u + tx_rand_below(r, 28000u))) >> 15);
        h->k = 0; h->alive = 1;
    }
}

TX_AUDIO_CODE static void ice_render(void *st, int32_t *mix, unsigned n)
{
    ice_t *s = (ice_t *)st;
    tx_rng_t *r = &s->cloud.rng;
    unsigned k, i, j, nf = 0;
    uint16_t d0 = s->dropped;
    int32_t any = 0;
    if (n > TX_BLOCK) n = TX_BLOCK;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;
    for (j = 0; j < n; j++) s->buf[j] = s->bpush[j] = 0;
    for (i = 0; i < k; i++) {
        const tx_event_t *e = &s->ev[i];
        if (!cubes(s)) { if (e->kind == TX_EV_MAIN) icicle(s, e); continue; }
        {   /* a clink: a cube hits the glass (sizes from a power law, alpha 1.5 over 1.5 decades: ours) */
            int32_t x = (int32_t)(((int64_t)tx_neglog2_q12(tx_rand(r)) * 8192) >> 12), lv, er, a;
            uint32_t tc, nb = e->kind == TX_EV_MAIN ? 3u + (tx_rand(r) >> 30) : 1u, c;
            if (x > 20400) x = 20400;
            lv = 65536 + ((int32_t)e->vel - 100) * 1638 - x * 8;
            tx_contact_hit(&s->mal, lv, &tc, &er);
            a = (int32_t)(((int64_t)A_REF / 5 * (int32_t)tx_exp2(lv - 65536 + (15 << 16))) >> 15);
            a = (int32_t)(((int64_t)a * s->level) >> 15);
            for (c = 0; c < nb; c++) {                       /* dropped in: a few clinks at once (ours) */
                body_strike(s, e->note, c ? a >> c : a, tc);
                s->bpush[e->offset] += a >> 5;
            }
        }
    }
    for (i = 0; i < NF; i++) {                               /* chunk impacts: a click and a small thud each */
        chunk_t *h = &s->ch[i];
        if (!h->alive) continue;
        any = 1;
        while (h->alive && h->t < n) {
            int32_t a = (int32_t)(((int64_t)h->a * h->v) >> 11);
            s->bpush[h->t] += a >> 10;
            tx_mode_strike(&s->thud[h->k & 1u], a >> 5);
            h->k++;
            h->v = (h->v * 6554) >> 15;                      /* restitution 0.2 (ice on frozen ground: ours) */
            if (h->v < 60 || h->k > 4) { h->alive = 0; break; }
            h->t += ((uint32_t)h->v * 9207u) >> 10;          /* flight 2 v / g */
        }
        if (h->alive) h->t -= n;
    }
    for (i = 0; i < NV; i++) {
        body_t *b = &s->b[i];
        unsigned m, live = 0;
        if (!b->used) continue;
        tx_modal_run(b->m, NMB, s->buf, 0, n);
        for (m = 0; m < NMB; m++) live |= b->m[m].amp != 0;
        b->used = (uint8_t)(live != 0);
        any |= b->used;
    }
    tx_modal_run(s->thud, 2, s->buf, 0, n);
    any |= s->thud[0].amp | s->thud[1].amp;
    if (s->fizz && (cubes(s) ? tx_cloud_held(&s->cloud) : any)) {   /* the melt fizz: sparse tiny pops */
        uint32_t t = tx_rand_below(r, 200u);
        int32_t lvl = (int32_t)(((int64_t)s->fizz * s->level) >> 13);
        while (t < n && nf < 64u) {
            int32_t x = (int32_t)(((int64_t)tx_neglog2_q12(tx_rand(r)) * 8192) >> 12);   /* alpha 1.5 */
            s->fo[nf] = (uint16_t)t;
            s->fa[nf] = (int16_t)(((int64_t)lvl * (int32_t)tx_exp2(-(x > 6 << 16 ? 6 << 16 : x) + (15 << 16))) >> 15) * ((tx_rand(r) & 1u) ? 1 : -1);
            nf++;
            t += 4u + tx_rand_below(r, 60u);                 /* ~1300 /s (ours) */
        }
    }
    tx_ireso_add(&s->fz, s->fo, s->fa, nf, s->buf, n);
    any |= !tx_ireso_quiet(&s->fz);
    {   /* the clicks: a sharp spike and a short bright burst */
        int32_t e = s->res, x1 = s->res_x1;
        for (j = 0; j < n; j++) {
            int32_t x;
            e += s->bpush[j];
            if (!e) continue;
            x = (int32_t)(tx_rand(r) >> 17) - 16384;
            x1 += (x - x1) >> 2;
            s->buf[j] += ((x - x1) * (e >> 12)) >> 2;
            e -= (e >> 5) + 1;
            if (e < 0) e = 0;
        }
        s->res = e; s->res_x1 = e ? x1 : 0;
        any |= e;
    }
    tx_dcblock_add(&s->hp, s->buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !any && !(s->hp.x1 | s->hp.y1));
}

static void ice_stats(const void *st, tx_stats_t *o)
{
    const ice_t *s = (const ice_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < NV; i++) n += s->b[i].used;
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: eight glasses shaken hard with cubes, cold (long rings), loud fizz */
static const char SCENE_MAX[] =
    "0 particles 8\n0 param ACTION 127\n0 param TEMP 20\n0 param AMOUNT 127\n0 param FIZZ 127\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4500 end\n";

const tx_engine_t tx_eng_ice = {
    "ice", "#32 Ice crashing", NP, PARAMS, NV, 6, sizeof(ice_t), SCENE_MAX,
    ice_init, ice_key, ice_param, ice_render, ice_stats,
};
