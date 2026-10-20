/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #33 Mineral sounds: hand-held mineral specimens struck, clinked together or rubbed (
 * MODAL + CONTACT cores).
 *
 * A key picks a specimen (its own shape: a fixed mode set per key) and plays it with the ACTION:
 *   strike   one knock with a hard tool: the specimen rings, damped by the hand holding it (HOLD);
 *   clink    two specimens knocked together: both ring (the second ~0.8-1.25 x the size), and the pair rattles:
 *            1-3 micro-collisions within ~15 ms at -6 .. -15 dB (I1: "a few within ~15 ms"; their numbers ours);
 *   rub      held: the pair slides, stick-slip as a stream of micro-impacts (~300-1500 /s by velocity, energies
 *            from a power law alpha 1.5: ours; note 11's friction route stays in #11), until release.
 * Physics:
 *   body     constant-Q modes (note 05 table: granite ~180, marble ~300, sandstone ~100, slate / limestone flagged
 *            guesses, basalt ~600 (between the audio and ultrasonic values: ours), flint / chert ~160, obsidian
 *            ~175 (M22), quartz capped 2000); 5 inharmonic modes per specimen (1.12-1.7 apart: compact irregular
 *            bodies, ours). Size: f1 = 0.025 c / L with c = sqrt(E / rho) from the table: fitted to a recording of
 *            two hand-held stones knocked together (peaks 1.3-4.2 kHz for ~8 cm stones; ours).
 *   hand     the holding hand damps the stone: sigma0 by HOLD (fingertips .. a firm grip; the recording rings
 *            -30 dB in ~64 ms at ~1.8 kHz, i.e. sigma0 ~ 22 /s over the granite Q), and a low thud of the hand-arm
 *            body (~90-110 Hz, Q 4: the recording's strongest single peak is ~94 Hz; ours).
 *   humidity water lowers Q (M9 marble, M6): Q x (1 - 0.6 HUMID) for the porous (sandstone, limestone, marble),
 *            x (1 - 0.25 HUMID) for the rest (ours).
 * Keys: a different specimen per key, a semitone smaller per key up; velocity = how hard (level, contact time). */
#include "tx_engine.h"
#include "tx_core_cloud.h"
#include "tx_core_modal.h"
#include "tx_core_contact.h"

enum { P_MINERAL, P_ACTION, P_SIZE, P_HOLD, P_HUMID, NP };

static const tx_param_t PARAMS[NP] = {
    {"MINERAL", 127, 0, "granite | marble | sandstone | slate | limestone | basalt | flint | obsidian | quartz"},
    {"ACTION", 127, 64, "strike (a tool) | clink (two stones knocked together) | rub (held: stick-slip)"},
    {"SIZE", 127, 64, "specimen size at C4: 25 .. 2 cm (log): f1 = 0.025 c / L (~1.3 kHz for 8 cm granite)"},
    {"HOLD", 127, 76, "the hand: hung on a string (free ring) .. fingertips .. a firm grip (damping 1 .. 80 /s)"},
    {"HUMID", 127, 0, "dry .. wet (Q lower, the porous most)"},
};

#define NV 8u
#define NMS 4u                     /* modes per specimen */
#define NMV (2u * NMS + 1u)        /* a pair + the hand thud */
#define NMIN 9u
#define A_REF 42000000             /* a mode's amplitude for a v100 knock (Q30; calibrated by render) */

static const uint16_t MIN_Q[NMIN] = {180, 300, 100, 185, 150, 600, 160, 175, 2000};
static const uint16_t MIN_C[NMIN] = {4300, 4700, 3500, 4500, 4200, 5100, 5400, 5400, 6000};
static const uint8_t POROUS[NMIN] = {0, 1, 1, 0, 1, 0, 0, 0, 0};

typedef struct {
    tx_mode_t m[NMV];
    uint16_t f[NMV];
    int16_t gw[NMV];
    uint16_t moff[3];              /* rattles: samples from the block start (0xFFFF none) */
    int16_t mamp[3];               /* Q15 of the knock */
    int32_t a0;
    uint32_t next_q8;              /* rub: samples to the next micro-impact */
    uint8_t note, vel, alive, em, rub, pad[3];
} pair_t;

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    pair_t v[NV];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK];
    tx_mallet_t tool, stone;
    uint8_t p[NP], limit, quiet;
    int32_t level, size_l2;
    uint32_t q_x16, c, s0_q8;
    uint16_t dropped;
} min_t;

static unsigned action_of(const min_t *s) { return (unsigned)(s->p[P_ACTION] * 3u) >> 7; }

static void set_q(min_t *s)
{
    unsigned r = (s->p[P_MINERAL] * NMIN) >> 7;
    uint32_t h = s->p[P_HUMID];
    uint32_t loss = POROUS[r] ? h * 155u : h * 64u;          /* Q15 x 1/2: 0.6 or 0.25 at 127 */
    s->q_x16 = (uint32_t)MIN_Q[r] * 16u * (32768u - loss) >> 15;
    s->c = MIN_C[r];
}

static void set_param(min_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_MINERAL: case P_HUMID: set_q(s); break;
    case P_SIZE: s->size_l2 = -131072 - (int32_t)(v * 1880u); break;   /* log2(0.25 m) - v log2(12.5) / 127, Q16 */
    case P_HOLD: s->s0_q8 = tx_knob_exp(v, 256u, 414000); break;   /* 1 .. 80 /s (6.3 oct), Q8 */
    default: break;
    }
}

static void min_init(void *st, uint32_t seed)
{
    min_t *s = (min_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    tx_mallet_mix(&s->tool, 0);                              /* a hard tool (the stone hammer row) */
    tx_mallet_mix(&s->stone, 0);
    s->stone.lm -= 217706;                                   /* a hand-held stone: ~10x lighter than the hammer row */
    s->limit = 4;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
}

static void min_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    tx_cloud_key(&((min_t *)st)->cloud, note, vel, on);
}

static void min_param(void *st, uint8_t id, uint8_t val)
{
    min_t *s = (min_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i, k;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NV ? NV : val);
        for (i = s->limit; i < NV; i++) { for (k = 0; k < NMV; k++) s->v[i].m[k].amp = 0; s->v[i].alive = 0; }
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* the specimen(s) of a key: mode frequencies and gains (fixed per key), the thud; the force spectrum applied */
static void build(min_t *s, pair_t *v, uint32_t tc)
{
    tx_rng_t r;
    unsigned k, sp, two = action_of(s) > 0;
    tx_rng_seed(&r, v->note * 2654435761u ^ 0x5701E5u);
    for (sp = 0; sp < 2; sp++) {
        int32_t f_l2 = tx_log2_q16(s->c) - 348773 - s->size_l2 - ((int32_t)v->note - 60) * 5461;   /* 0.025 c / L */
        if (sp) f_l2 += (int32_t)tx_rand_below(&r, 42000u) - 21000;   /* the second: x 0.8 .. 1.25 */
        for (k = 0; k < NMS; k++) {
            unsigned i = sp * NMS + k;
            uint32_t f;
            int32_t g;
            if (k) f_l2 += 10715 + (int32_t)tx_rand_below(&r, 39450u);   /* x 1.12 .. 1.7 */
            f = tx_exp2(f_l2);
            g = k ? 12000 + (int32_t)tx_rand_below(&r, 20767u) : 32767;
            v->f[i] = (uint16_t)(f > 20000u || (sp && !two) ? 0u : f);
            v->m[i].inc = f * 97391u;
            v->m[i].m16 = tx_mode_decay(f, s->q_x16, s->s0_q8);
            v->gw[i] = (int16_t)(v->f[i] ? ((int64_t)g * tx_contact_weight(tc, f)) >> 15 : 0);
        }
    }
    k = 2u * NMS;                                            /* the hand thud: ~90-110 Hz, Q 4 (ours) */
    v->f[k] = (uint16_t)(90u + tx_rand_below(&r, 20u));
    v->m[k].inc = v->f[k] * 97391u;
    v->m[k].m16 = tx_mode_decay(v->f[k], 64u, 0u);
    v->gw[k] = 28000;
}

static void strike(pair_t *v, int32_t a)
{
    unsigned k;
    for (k = 0; k < NMV; k++) if (v->f[k] && v->gw[k]) tx_mode_strike(&v->m[k], (int32_t)(((int64_t)a * v->gw[k]) >> 15));
}

static void knock(min_t *s, const tx_event_t *e)
{
    pair_t *v = 0;
    unsigned i, act = action_of(s);
    int32_t lv = 65536 + ((int32_t)e->vel - 100) * 1638, ev, a;
    uint32_t tc;
    for (i = 0; i < s->limit && !v; i++) if (s->v[i].alive && s->v[i].note == e->note) v = &s->v[i];
    for (i = 0; i < s->limit && !v; i++) if (!s->v[i].alive) v = &s->v[i];
    if (!v) {
        int32_t lo = 0x7FFFFFFF;
        for (i = 0; i < s->limit; i++) { int32_t l = s->v[i].m[0].amp + s->v[i].m[NMS].amp; if (l < lo) { lo = l; v = &s->v[i]; } }
        s->dropped++;
        if (v) { unsigned k; for (k = 0; k < NMV; k++) v->m[k].amp = 0; }
    }
    if (!v) return;
    tx_contact_hit(act ? &s->stone : &s->tool, lv, &tc, &ev);
    a = (int32_t)(((int64_t)A_REF * (int32_t)tx_exp2(lv + (15 << 16))) >> 16);
    a = (int32_t)(((int64_t)a * (32768 + ev)) >> 16);
    a = (int32_t)(((int64_t)a * s->level) >> 15);
    if (!v->alive || v->note != e->note) { unsigned k; for (k = 0; k < NMV; k++) v->m[k].amp = 0; }
    v->note = e->note; v->vel = e->vel; v->em = e->emitter; v->a0 = a;
    build(s, v, tc);
    v->rub = (uint8_t)(act == 2u);
    v->next_q8 = 0;
    if (!v->rub) strike(v, a);
    v->moff[0] = v->moff[1] = v->moff[2] = 0xFFFF;
    if (act == 1u) {                                         /* clink: the pair rattles */
        uint32_t n = 1u + tx_rand_below(&s->cloud.rng, 3u), k, t = e->offset;
        for (k = 0; k < n; k++) {
            t += 40u + tx_rand_below(&s->cloud.rng, 220u);   /* within ~15 ms */
            v->moff[k] = (uint16_t)t;
            v->mamp[k] = (int16_t)(5800 + tx_rand_below(&s->cloud.rng, 10600u));   /* -15 .. -6 dB */
        }
    }
    v->alive = 1;
}

TX_AUDIO_CODE static void min_render(void *st, int32_t *mix, unsigned n)
{
    min_t *s = (min_t *)st;
    unsigned k, i, j;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;
    for (j = 0; j < n; j++) s->buf[j] = 0;
    for (i = 0; i < k; i++) knock(s, &s->ev[i]);
    for (i = 0; i < NV; i++) {
        pair_t *v = &s->v[i];
        unsigned j0 = 0, m, live = 0;
        if (!v->alive) continue;
        if (v->rub) {                                        /* stick-slip: micro-impacts while the key is held */
            int held = v->em < TX_CLOUD_EMITTERS && s->cloud.em[v->em].held && s->cloud.em[v->em].note == v->note;
            uint32_t per_q8 = (TX_FS << 8) / (300u + (uint32_t)v->vel * 10u);   /* ~300 .. 1570 /s */
            if (!held) v->rub = 0;
            unsigned sb;
            for (sb = 0; v->rub && sb < n; sb += 64u) {     /* the impacts of each 64-sample slice, summed into */
                int32_t sum = 0;                             /* one strike at its start (CPU: 4 per block) */
                unsigned e1 = sb + 64u < n ? sb + 64u : n;
                while ((v->next_q8 >> 8) < e1) {
                    int32_t x = (int32_t)(((int64_t)tx_neglog2_q12(tx_rand(&s->cloud.rng)) * 8192) >> 12);   /* alpha 1.5 */
                    int32_t a = (int32_t)(((int64_t)(v->a0 >> 3) * (int32_t)tx_exp2(-(x > 65536 * 6 ? 65536 * 6 : x) + (15 << 16))) >> 15);
                    sum += (tx_rand(&s->cloud.rng) & 1u) ? a : -a;
                    v->next_q8 += tx_cloud_exp_q8(&s->cloud, per_q8);
                }
                if (sum) {
                    tx_modal_run(v->m, NMV, s->buf, j0, sb);
                    j0 = sb;
                    strike(v, sum);
                }
            }
            if (v->rub) v->next_q8 -= (uint32_t)n << 8;
        }
        for (m = 0; m < 3; m++) {
            if (v->moff[m] < n && v->moff[m] >= j0) {
                tx_modal_run(v->m, NMV, s->buf, j0, v->moff[m]);
                j0 = v->moff[m];
                strike(v, (int32_t)(((int64_t)v->a0 * v->mamp[m]) >> 15));
                v->moff[m] = 0xFFFF;
            } else if (v->moff[m] != 0xFFFF)
                v->moff[m] = (uint16_t)(v->moff[m] >= n ? v->moff[m] - n : 0);
        }
        tx_modal_run(v->m, NMV, s->buf, j0, n);
        for (m = 0; m < NMV; m++) live |= v->m[m].amp != 0;
        if (!live && !v->rub && v->moff[0] == 0xFFFF && v->moff[1] == 0xFFFF && v->moff[2] == 0xFFFF) v->alive = 0;
    }
    tx_dcblock_add(&s->hp, s->buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = s->hp.x1 | s->hp.y1;
        for (i = 0; i < NV; i++) z |= s->v[i].alive;
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void min_stats(const void *st, tx_stats_t *o)
{
    const min_t *s = (const min_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < NV; i++) n += s->v[i].alive;
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: eight quartz pairs rubbed hard, hung free (long rings), every mode alive */
static const char SCENE_MAX[] =
    "0 particles 8\n0 param MINERAL 127\n0 param ACTION 127\n0 param HOLD 0\n0 param SIZE 127\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_minerals = {
    "minerals", "#33 Mineral sounds", NP, PARAMS, NV, 4, sizeof(min_t), SCENE_MAX,
    min_init, min_key, min_param, min_render, min_stats,
};
