/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #24 Wind chimes: tubes hanging in the wind, struck by a clapper.
 *
 * Each held key hangs a tube tuned to its note (a key press also strikes it once, velocity = force); while tubes hang,
 * the wind strikes them at random. Physics:
 *   tube     a free-free bar / tube: modes 1 : 2.7565 : 5.4039 : 8.9330 x f1 (Euler-Bernoulli; note 04), the first four
 *            (those under fs/6 kept), one shared SVF bank per tube. Loss by material, constant Q: aluminium ~1500,
 *            glass ~600 (ours, from the note's metal / glass ranges), bamboo ~100 (M25: tan delta ~0.01 dry, bending).
 *   strike   the clapper sets each mode's amplitude (the bank's state directly: the struck tube's ring does not
 *            depend on Q); a soft clapper (long contact) excites the high modes less: gains n^-tilt (ours).
 *   wind     PhISEM timing (note 04: chimes = PhISEM timing into tubes): a wind energy fed by the WIND level and gusts
 *            (a slow random walk), strikes at a Poisson rate ~ energy x the number of tubes (up to ~20 / s), each on a
 *            random hanging tube with a force ~ energy x a random draw (ours: no chime measurement).
 * A tube rings out to exact zero (a time bound of 3 T60 after its last strike guards against rounding cycles). */
#include "tx_engine.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"

enum { P_WIND, P_GUST, P_MATERIAL, P_CLAPPER, NP };

static const tx_param_t PARAMS[NP] = {
    {"WIND", 127, 50, "wind: calm .. strong: strikes ~0.3 .. 20 / s across the tubes, harder"},
    {"GUST", 127, 50, "gustiness: steady .. gusty (bursts of strikes)"},
    {"MATERIAL", 127, 0, "aluminium (Q ~1500, long ring) | glass (Q ~600) | bamboo (Q ~100, M25)"},
    {"CLAPPER", 127, 64, "clapper: soft (felt / wood: dark) .. hard (metal: bright, all modes)"},
};

#define NT 8u
#define NM 4u
#define A_STRIKE 5000              /* a mode's amplitude for a full strike (Q15 band state; calibrated by render) */

TX_AUDIO_DATA static const uint16_t RATIO[NM] = { 1024, 2823, 5534, 9147 };   /* Q10 */
TX_AUDIO_DATA static const uint16_t MAT_Q[3] = { 1500, 600, 100 };
static const int32_t L2_RATIO[NM] = { 0, 96004, 161761, 207036 };   /* log2 of the mode ratios, Q16 */

typedef struct {
    tx_svf_t m[NM];
    int32_t f[NM];
    uint32_t f1;                   /* Hz */
    uint32_t silent, tmax;         /* samples since the last strike; 3 T60 */
    uint8_t em, note, alive, nm;
} tube_t;

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    tube_t t[NT];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t zero[TX_BLOCK], buf[TX_BLOCK];
    uint8_t p[NP], limit, quiet;
    int32_t level, energy, gust, tilt;
    uint32_t q;
    uint16_t dropped;
} chimes_t;

static void set_param(chimes_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    if (id == P_MATERIAL) s->q = MAT_Q[(v * 3u) >> 7];
    if (id == P_CLAPPER) s->tilt = (int32_t)(127u - v) * 1032;   /* n^-tilt, tilt 0 .. 2 (Q16) */
}

static void chimes_init(void *st, uint32_t seed)
{
    chimes_t *s = (chimes_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    for (i = 0; i < NT; i++) s->t[i].em = 0xFF;
    s->limit = 6;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
}

static void chimes_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    tx_cloud_key(&((chimes_t *)st)->cloud, note, vel, on);
}

static void quiet_tube(tube_t *t)
{
    unsigned i;
    for (i = 0; i < NM; i++) t->m[i].low = t->m[i].band = t->m[i].ef = 0;
}

static void chimes_param(void *st, uint8_t id, uint8_t val)
{
    chimes_t *s = (chimes_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NT ? NT : val);
        for (i = s->limit; i < NT; i++) { s->t[i].alive = 0; s->t[i].em = 0xFF; quiet_tube(&s->t[i]); }
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* strike a tube: each mode's amplitude set from the force and the clapper (n^-tilt) */
static void strike(chimes_t *s, tube_t *t, int32_t force_q15)
{
    unsigned i;
    for (i = 0; i < t->nm; i++) {
        int32_t g = (int32_t)tx_exp2((15 << 16) - (int32_t)(((int64_t)s->tilt * L2_RATIO[i]) >> 16));   /* (f_n / f1)^-tilt */
        int32_t a = (int32_t)(((int64_t)force_q15 * g >> 15) * A_STRIKE >> 15);
        a = (int32_t)(((int64_t)a * s->level) >> 15);
        t->m[i].band += (tx_rand(&s->cloud.rng) & 1u) ? a : -a;   /* the phase of each mode's kick (ours) */
        if (t->m[i].band > 65000) t->m[i].band = 65000;
        if (t->m[i].band < -65000) t->m[i].band = -65000;
    }
    t->silent = 0;
}

static void hang(chimes_t *s, const tx_event_t *e)
{
    tube_t *t = 0;
    unsigned i;
    for (i = 0; i < s->limit && !t; i++) if (s->t[i].alive && s->t[i].note == e->note) t = &s->t[i];
    for (i = 0; i < s->limit && !t; i++) if (!s->t[i].alive && tx_svf_bank_quiet(s->t[i].m, NM)) t = &s->t[i];
    for (i = 0; i < s->limit && !t; i++) if (!s->t[i].alive) t = &s->t[i];
    if (!t) { s->dropped++; return; }
    if (!t->alive || t->note != e->note) {
        uint32_t f1 = tx_note_inc((int32_t)e->note << 16) / 97391u;
        quiet_tube(t);
        t->f1 = f1; t->nm = 0;
        for (i = 0; i < NM; i++) {
            uint32_t hz = (f1 * RATIO[i]) >> 10;
            if (hz > 7300u) break;
            t->f[i] = tx_svf_coef(hz);
            t->nm = (uint8_t)(i + 1u);
        }
    }
    t->alive = 1; t->em = e->emitter; t->note = e->note;
    t->tmax = s->q * 291060u / (t->f1 ? t->f1 : 1u);         /* 3 T60 = 6.6 Q / f1 s, samples */
    strike(s, t, (int32_t)e->vel * e->vel * 2);
}

TX_AUDIO_CODE static void chimes_render(void *st, int32_t *mix, unsigned n)
{
    chimes_t *s = (chimes_t *)st;
    unsigned k, i, j, hanging = 0;
    int32_t *buf = s->buf;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) buf[j] = s->zero[j] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    for (i = 0; i < k; i++) hang(s, &s->ev[i]);
    for (i = 0; i < NT; i++) {                               /* released keys: the tube stops hanging (rings out) */
        tube_t *t = &s->t[i];
        if (t->alive && !(t->em < TX_CLOUD_EMITTERS && s->cloud.em[t->em].held && s->cloud.em[t->em].note == t->note)) {
            t->alive = 0; t->em = 0xFF;
        }
        hanging += t->alive;
    }
    /* the wind: energy toward WIND x (1 + gust), strikes at a Poisson rate ~ energy x tubes */
    {
        tx_rng_t *r = &s->cloud.rng;
        int32_t target;
        s->gust += ((tx_rand_s15(r) * (s->p[P_GUST] * 258)) >> 17) - (s->gust >> 6);
        s->gust = s->gust > 32767 ? 32767 : s->gust < -32767 ? -32767 : s->gust;
        target = (int32_t)s->p[P_WIND] * 258;
        target = target + ((target >> 1) * s->gust >> 15) * 2;
        target = target < 0 ? 0 : target > 32767 ? 32767 : target;
        s->energy += (target - s->energy) >> 4;
        if (hanging && s->energy > 64) {
            /* rate per block = 20 / s x (E / 1)^2 x tubes / 6 / 172 blocks per s; Q16 probability */
            uint32_t e2 = (uint32_t)((s->energy >> 7) * (s->energy >> 7));   /* E^2, Q16 */
            uint32_t p = e2 * hanging * 20u / 6u / 172u + 26u;  /* + a calm floor ~0.07 / s per tube */
            if ((tx_rand(r) >> 16) < p) {
                unsigned pick = tx_rand_below(r, hanging), c = 0;
                for (i = 0; i < NT; i++)
                    if (s->t[i].alive && c++ == pick) {
                        strike(s, &s->t[i], (int32_t)(((s->energy >> 1) + (int32_t)(tx_rand(r) >> 18)) >> 0));
                        break;
                    }
            }
        }
    }
    for (i = 0; i < NT; i++) {
        tube_t *t = &s->t[i];
        int32_t g[NM] = { 2048, 2048, 2048, 2048 }, gi[NM] = { 256, 256, 256, 256 };
        if (!t->nm || tx_svf_bank_quiet(t->m, NM)) continue;
        tx_svf_bank_add_g(t->m, t->f, (int32_t)(16384u / s->q), gi, g, t->nm, s->zero, buf, n);
        t->silent += n;
        if (t->silent > t->tmax) quiet_tube(t);              /* 3 T60: a rounding cycle must not hold it */
    }
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = s->hp.x1 | s->hp.y1;
        for (i = 0; i < NT; i++) z |= s->t[i].alive | !tx_svf_bank_quiet(s->t[i].m, NM);
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void chimes_stats(const void *st, tx_stats_t *o)
{
    const chimes_t *s = (const chimes_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < NT; i++) n += s->t[i].alive;
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: eight tubes (every mode under fs/6), strong gusty wind, a long-ringing material */
static const char SCENE_MAX[] =
    "0 particles 8\n0 param WIND 127\n0 param GUST 127\n0 param MATERIAL 0\n0 param CLAPPER 127\n"
    "0 on 53 127\n0 on 55 127\n0 on 57 127\n0 on 60 127\n0 on 62 127\n0 on 64 127\n0 on 67 127\n0 on 69 127\n"
    "3000 off 53\n3000 off 55\n3000 off 57\n3000 off 60\n3000 off 62\n3000 off 64\n3000 off 67\n3000 off 69\n"
    "4000 end\n";

const tx_engine_t tx_eng_chimes = {
    "chimes", "#24 Wind chimes", NP, PARAMS, NT, 6, sizeof(chimes_t), SCENE_MAX,
    chimes_init, chimes_key, chimes_param, chimes_render, chimes_stats,
};
