/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura stone cloud: stones of mixed sizes clinking in a space, a self-playing cloud (
 * MODAL + CONTACT cores). The feel is inspired by a reference instrument (krint: a Poisson cloud of short, bright
 * modal stone hits with a room tail); the parameters are this engine's own, from the contact and modal physics.
 *
 * Holding a key keeps a cloud of hits going (a press starts it at once); release lets the last ones ring out.
 *   timing   a stream at DENSITY hits/s per held key; CLUMP from Poisson (CV 1, the reference instrument) to runs of
 *            quick hits and quiet spells (CV ~1.5); never a regular beat (was IRREG, down to periodic).
 *   stones   the audible ring of a hand-sized stone comes from thin pieces (chips, flakes, slabs): bending modes of
 *            a free-free piece, f1 = 1.03 h c / L^2 (Euler-Bernoulli, note 05) with h = L / 8 (ours), so
 *            f1 ~ 0.128 c / L: a 15 cm granite flake (c = sqrt(E / rho) ~ 4.3 km/s) rings at ~3.7 kHz. Each hit draws
 *            its own piece: L log-normal around SIZE (SPREAD: +-0 .. 1.5 octaves); 4 modes, each 1.15-1.9 x the last
 *            (sparse and inharmonic: the reference hits show 2-6 peaks, e.g. 773 / 1570 / 1751 / 5273 / 8320 Hz;
 *            ours), random gains; constant Q by MINERAL (note 05 table).
 *   contact  the pieces hit each other or the ground: HARD from stone on stone (an impulse) to soft ground (rubber /
 *            felt-like contacts: darker), Hunt-Crossley via CONTACT; energies from a power law (alpha 1.5 over two
 *            decades: hits vary by ~20 dB, as in the recordings); a 2 ms attack residue for the modes not kept.
 *   room     the space around them: a 4-line feedback delay network (Hadamard, lines 431-1013 samples, a one-pole
 *            high-frequency loss in each), T60 by ROOM (ours; the reference instrument shows such a decorrelated
 *            tail). Bounded: the tail is cleared 3 T60 after the last input (no rounding cycles).
 * Keys: a semitone smaller pieces per key up (C4 = SIZE); velocity = how hard they are thrown (energy). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_modal.h"
#include "tx_core_contact.h"
#include "tx_core_chorus.h"

#define OUT_TRIM 9939               /* output gain, Q12 (+4.9 dB): integrated (a key held 10 s): -30.9 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +2.8 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 2

enum { P_DENSITY, P_SIZE, P_SPREAD, P_MINERAL, P_HARD, P_CLUMP, P_ROOM, NP };

static const tx_param_t PARAMS[NP] = {
    {"DENSITY", 127, 70, "hits per second per key: 0.3 .. 40 (log)"},
    {"SIZE", 127, 64, "piece length at C4: 3 .. 60 cm (log): f1 ~ 0.13 c / L (a 15 cm granite flake ~3.7 kHz)"},
    {"SPREAD", 127, 40, "mixed sizes: all alike .. +-3 octaves (sd)"},
    {"MINERAL", 127, 30, "granite (Q 180) | marble 300 | slate 185 | flint 160 | obsidian 175 | quartz 2000"},
    {"HARD", 127, 0, "stone on stone (bright clinks) .. on wood .. on soft ground (dull knocks)"},
    {"CLUMP", 127, 16, "timing: random (Poisson) .. in runs and quiet spells"},
    {"ROOM", 127, 50, "the space: dry .. a long tail (T60 0.2 .. 2.5 s)"},
};

#define NV 16u
#define NMO 4u
#define NMIN 6u
#define NL 4u
#define LTOT (431u + 593u + 769u + 1013u)
#define A_HIT 300000000            /* a mode's amplitude for the largest hit at v100 (Q30; calibrated by render) */
#define RANGE_L2 27213             /* two decades of energy, Q12 */

static const uint16_t MIN_Q[NMIN] = {180, 300, 185, 160, 175, 2000};
static const uint16_t MIN_C[NMIN] = {4300, 4700, 4500, 5400, 5400, 6000};   /* sqrt(E / rho), m/s (note 05 table) */
static const uint16_t LLEN[NL] = {431, 593, 769, 1013};

typedef struct {
    tx_mode_t m[NMO];
    uint8_t alive, pad[3];
} piece_t;

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    piece_t v[NV];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK];
    int16_t line[LTOT];
    uint16_t lpos[NL];
    int32_t lp[NL];                /* the lines' high-frequency loss states */
    int32_t fb[NL];                /* feedback gains, Q15 */
    int32_t res, res_x1;           /* attack residue */
    uint32_t idle_cnt, tail_max;   /* samples since the last input to the room; 3 T60 */
    tx_mallet_t mallet;
    uint8_t p[NP], limit, quiet;
    int32_t level, size_l2, spread, wet;
    uint32_t q_x16, c;
    int32_t pinv;
    uint32_t pm;
    uint16_t dropped;
    tx_vary_t vy;
} cloud_t;

static void set_room(cloud_t *s)
{
    unsigned i;
    int32_t t60_l2 = -152170 + (int32_t)(s->p[P_ROOM] * 1882u);   /* log2(0.2 s) + v log2(12.5) / 127, Q16 */
    for (i = 0; i < NL; i++) {
        /* g = 10^(-3 L / (T60 fs)) = 2^(-9.966 L / (T60 fs)): log2 g = -9.966 L / fs / T60 */
        int32_t lg = (int32_t)((uint32_t)LLEN[i] * 14810u / 1000u);   /* 9.966 L / fs, Q16 (65536 x 9.966 / 44100) */
        lg = (lg << 16) / (int32_t)tx_exp2(t60_l2 + (16 << 16));      /* / T60 (Q16 s) */
        s->fb[i] = (int32_t)tx_exp2((15 << 16) - lg);
    }
    s->wet = s->p[P_ROOM] ? 6000 + s->p[P_ROOM] * 100 : 0;   /* Q15 send (ours) */
    s->tail_max = (uint32_t)(((uint64_t)tx_exp2(t60_l2 + (16 << 16)) * 3u * TX_FS) >> 16);
}

static void set_param(cloud_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_DENSITY: tx_cloud_rate(&s->cloud, tx_knob_exp(v, 77u, 452312)); break;   /* 0.3 .. 40 /s (7.06 oct), Q8 */
    case P_SIZE: s->size_l2 = -332212 + (int32_t)(v * 2224u); break;    /* log2(0.03 m) + v log2(20) / 127, Q16 */
    case P_SPREAD: s->spread = (int32_t)v * 774; break;                 /* SD / 2 in octaves, Q16: 0 .. 1.5 */
    case P_MINERAL: { unsigned r = (v * NMIN) >> 7; s->q_x16 = MIN_Q[r] * 16u; s->c = MIN_C[r]; break; }
    case P_HARD:                                         /* the pieces themselves: ~10x lighter than a hammer */
        tx_mallet_mix(&s->mallet, (int32_t)v * 258);      /* (t_c x 0.4: a 2 cm-edge flake meeting stone ~80 us) */
        s->mallet.lm -= 217706;
        break;
    case P_CLUMP: tx_cloud_clump(&s->cloud, v * 2u); break;
    case P_ROOM: set_room(s); break;
    default: break;
    }
}

static void sc_init(void *st, uint32_t seed)
{
    cloud_t *s = (cloud_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    {   /* the power law alpha 1.5 truncated at two decades */
        int32_t lmax = (RANGE_L2 * 2048) >> 12;
        s->pinv = (int32_t)((1u << 24) / 2048u);
        s->pm = 0xFFFFFFFFu - tx_exp2((32 << 16) - lmax * 16);
    }
    s->limit = 10;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((cloud_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void sc_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((cloud_t *)st)->vy);
    tx_cloud_key(&((cloud_t *)st)->cloud, note, vel, on);
}

static void sc_param(void *st, uint8_t id, uint8_t val)
{
    cloud_t *s = (cloud_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i, k;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NV ? NV : val);
        for (i = s->limit; i < NV; i++) { for (k = 0; k < NMO; k++) s->v[i].m[k].amp = 0; s->v[i].alive = 0; }
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

static int32_t draw_energy(cloud_t *s)
{
    uint32_t u = 0xFFFFFFFFu - (uint32_t)(((uint64_t)tx_rand(&s->cloud.rng) * s->pm) >> 32);
    int32_t x = (int32_t)(((int64_t)tx_neglog2_q12(u) * s->pinv) >> 12);
    return x > RANGE_L2 ? RANGE_L2 : x;
}

/* a hit: a new piece (a free voice, else the quietest is replaced) struck at once */
static void hit(cloud_t *s, const tx_event_t *e)
{
    tx_rng_t *r = &s->cloud.rng;
    piece_t *v = 0;
    unsigned i, k;
    int32_t l_l2, f_l2, lv, ev, a, lo = 0x7FFFFFFF;
    uint32_t tc;
    for (i = 0; i < s->limit && !v; i++) if (!s->v[i].alive) v = &s->v[i];
    if (!v) {
        for (i = 0; i < s->limit; i++) { int32_t l = s->v[i].m[0].amp + s->v[i].m[1].amp; if (l < lo) { lo = l; v = &s->v[i]; } }
        s->dropped++;
    }
    if (!v) return;
    l_l2 = s->size_l2 - ((int32_t)e->note - 60) * 5461 + (int32_t)(((int64_t)tx_gauss_q12(r) * s->spread) >> 12);
    f_l2 = tx_log2_q16(s->c) - 194372 - l_l2;                /* f1 = 0.128 c / L: log2 0.128 = -2.966 */
    lv = 65536 + ((int32_t)e->vel - 100) * 1638 + (((draw_energy(s) - RANGE_L2) * 16) >> 1);   /* v ~ sqrt(E) */
    tx_contact_hit(&s->mallet, lv, &tc, &ev);
    a = (int32_t)(((int64_t)A_HIT * (int32_t)tx_exp2(lv - 65536 + (15 << 16))) >> 15);
    a = (int32_t)(((int64_t)a * s->level) >> 15);
    for (k = 0; k < NMO; k++) {
        uint32_t f;
        int32_t g;
        if (k) f_l2 += 13215 + (int32_t)tx_rand_below(r, 47480u);   /* x 1.15 .. 1.9 */
        f = tx_exp2(f_l2);
        v->m[k].amp = 0;
        if (f < 60u || f > 20000u) continue;
        v->m[k].inc = f * 97391u;
        v->m[k].m16 = tx_mode_decay(f, s->q_x16, 179u);
        v->m[k].ph = 0;
        g = k ? 16384 + (int32_t)tx_rand_below(r, 16383u) : 32767;   /* -6 .. 0 dB (ours: a random strike point) */
        g = (int32_t)(((int64_t)g * tx_contact_weight(tc, f)) >> 15);
        tx_mode_strike(&v->m[k], (int32_t)(((int64_t)a * g) >> 15));
    }
    s->res += (int32_t)(((int64_t)(a >> 5) * tx_contact_weight(tc, 6000u)) >> 15);
    v->alive = 1;
}

TX_AUDIO_CODE static void sc_render(void *st, int32_t *mix, unsigned n)
{
    cloud_t *s = (cloud_t *)st;
    unsigned k, i, j, e = 0;
    uint16_t d0 = s->dropped;
    int32_t any = 0;
    if (n > TX_BLOCK) n = TX_BLOCK;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;
    for (j = 0; j < n; j++) s->buf[j] = 0;
    /* hits in time order: each piece renders up to the next hit's offset (a hit lands at its sample) */
    {
        unsigned j0 = 0;
        for (e = 0; e <= k; e++) {
            unsigned j1 = e < k ? s->ev[e].offset : n;
            if (j1 > j0) for (i = 0; i < NV; i++) if (s->v[i].alive) tx_modal_run(s->v[i].m, NMO, s->buf, j0, j1);
            j0 = j1;
            if (e < k) hit(s, &s->ev[e]);
        }
    }
    for (i = 0; i < NV; i++) {
        piece_t *v = &s->v[i];
        unsigned m, live = 0;
        if (!v->alive) continue;
        for (m = 0; m < NMO; m++) live |= v->m[m].amp != 0;
        v->alive = (uint8_t)(live != 0);
        any |= v->alive;
    }
    if (s->res) {                                            /* the attack residue: low-passed noise, tau 2 ms */
        int32_t en = s->res, x1 = s->res_x1;
        for (j = 0; j < n; j++) {
            int32_t x = (int32_t)(tx_rand(&s->cloud.rng) >> 17) - 16384;
            x1 += (x - x1) >> 1;
            s->buf[j] += (x1 * (en >> 14)) >> 3;
            en -= (en >> 6) + 1;
            if (en <= 0) { en = 0; break; }
        }
        s->res = en; s->res_x1 = en ? x1 : 0;
    }
    {   /* the room: 4 lines, Hadamard feedback, a one-pole HF loss per line; input = the dry sum x wet */
        int32_t in_any = 0;
        for (j = 0; j < n && !in_any; j++) in_any = s->buf[j];
        if (in_any && s->wet) s->idle_cnt = 0;
        if (s->wet && s->idle_cnt < s->tail_max) {
            int16_t *L0 = s->line, *L1 = L0 + LLEN[0], *L2 = L1 + LLEN[1], *L3 = L2 + LLEN[2];
            uint32_t p0 = s->lpos[0], p1 = s->lpos[1], p2 = s->lpos[2], p3 = s->lpos[3];
            int32_t g0 = s->fb[0], g1 = s->fb[1], g2 = s->fb[2], g3 = s->fb[3], w = s->wet;
            int32_t lp0 = s->lp[0], lp1 = s->lp[1], lp2 = s->lp[2], lp3 = s->lp[3];
            for (j = 0; j < n; j++) {
                int32_t a0 = L0[p0], a1 = L1[p1], a2 = L2[p2], a3 = L3[p3], x, h0, h1, h2, h3;
                x = (int32_t)(((s->buf[j] >> 8) * (w >> 3)) >> 12);   /* Q15 line units */
                lp0 += (a0 - lp0) >> 1; lp1 += (a1 - lp1) >> 1; lp2 += (a2 - lp2) >> 1; lp3 += (a3 - lp3) >> 1;
                h0 = (lp0 + lp1 + lp2 + lp3) >> 1; h1 = (lp0 - lp1 + lp2 - lp3) >> 1;
                h2 = (lp0 + lp1 - lp2 - lp3) >> 1; h3 = (lp0 - lp1 - lp2 + lp3) >> 1;
                h0 = ((h0 * g0) >> 15) + x; h1 = ((h1 * g1) >> 15) + x; h2 = ((h2 * g2) >> 15) - x; h3 = ((h3 * g3) >> 15) - x;
                L0[p0] = (int16_t)(h0 > 32767 ? 32767 : h0 < -32767 ? -32767 : h0);
                L1[p1] = (int16_t)(h1 > 32767 ? 32767 : h1 < -32767 ? -32767 : h1);
                L2[p2] = (int16_t)(h2 > 32767 ? 32767 : h2 < -32767 ? -32767 : h2);
                L3[p3] = (int16_t)(h3 > 32767 ? 32767 : h3 < -32767 ? -32767 : h3);
                s->buf[j] += (a0 + a1 - a2 - a3) * 64;       /* Q15 x 2^8 / 4 -> Q23 */
                if (++p0 >= LLEN[0]) p0 = 0;
                if (++p1 >= LLEN[1]) p1 = 0;
                if (++p2 >= LLEN[2]) p2 = 0;
                if (++p3 >= LLEN[3]) p3 = 0;
            }
            s->lpos[0] = (uint16_t)p0; s->lpos[1] = (uint16_t)p1; s->lpos[2] = (uint16_t)p2; s->lpos[3] = (uint16_t)p3;
            s->lp[0] = lp0; s->lp[1] = lp1; s->lp[2] = lp2; s->lp[3] = lp3;
            s->idle_cnt += n;
            if (s->idle_cnt >= s->tail_max) {               /* 3 T60 after the last input: clear */
                for (j = 0; j < LTOT; j++) s->line[j] = 0;
                for (j = 0; j < NL; j++) s->lp[j] = 0;
            }
        }
    }
    tx_vary_block(&s->vy, s->buf, n);
    tx_trim(s->buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, s->buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = any | s->res | s->hp.x1 | s->hp.y1 | (s->wet && s->idle_cnt < s->tail_max);
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void sc_stats(const void *st, tx_stats_t *o)
{
    const cloud_t *s = (const cloud_t *)st;
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

/* the worst case: eight keys at the densest, big quartz pieces ringing long, a long room */
static const char SCENE_MAX[] =
    "0 particles 16\n0 param DENSITY 127\n0 param MINERAL 127\n0 param SIZE 127\n0 param ROOM 127\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_stonecloud = {
    "stonecloud", "Stone cloud", NP, PARAMS, NV, 10, sizeof(cloud_t), SCENE_MAX,
    sc_init, sc_key, sc_param, sc_render, sc_stats,
};
