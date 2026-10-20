/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #25 Thunder: a lightning flash heard from a distance (THUNDER core).
 *
 * A key press is a flash: a new tortuous channel from the start height down to the ground (THUNDER core: the channel model's
 * generator, one N-wave per 10 m segment, heard in order of distance). Holding the key restrikes the same channel
 * (return strokes 40-50 ms apart, E2: here 30 ms + an exponential draw of mean 20 ms, at most 4; later strokes at
 * 0.6 of the first, ours). Lower keys are farther: the distance at C4 is DIST, 2^(1/12) closer per semitone up.
 * Velocity scales the energy per length (+-1 octave of E_l per 40 velocity) and the level. The sound starts at the
 * first arrival: the flash-to-bang delay d / c (2.9 s per km) is not waited for. */
#include "tx_engine.h"
#include "tx_core_cloud.h"
#include "tx_core_thunder.h"

enum { P_DIST, P_ENERGY, P_TORT, P_HEIGHT, NP };

static const tx_param_t PARAMS[NP] = {
    {"DIST", 127, 50, "distance to the flash at C4: 0.2 .. 20 km (log): level 1/r then 1/r^2, duller (air), longer N-waves"},
    {"ENERGY", 127, 64, "channel energy per length E_l: 1e4 .. 1e6 J/m (log): N-wave peak f_m 680 .. 68 Hz (Few), louder"},
    {"TORT", 127, 64, "tortuosity: deflection spread x 0.25 .. 4 of the natural spread (27 deg): smooth roll .. cracking claps"},
    {"HEIGHT", 127, 100, "channel start height: 0.5 .. 5 km (log): how long it rumbles (up to ~15 s)"},
};

#define NF 2u

typedef struct {
    tx_cloud_t cloud;
    tx_declick_t dc;
    tx_dcblock_t hp;
    tx_flash_t fl[NF];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    uint32_t next[NF];             /* next restrike, samples on the flash clock (0 = none) */
    uint8_t em[NF], note[NF];
    uint8_t p[NP], limit, quiet;
    int32_t level;
    int32_t dist_l2, el_l2, height_l2;
    uint32_t tort_q8;
    uint16_t dropped;
} thunder_t;

static void set_param(thunder_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_DIST: s->dist_l2 = 501039 + (int32_t)(v * 3428u); break;       /* log2(200 m) + v log2(100) / 127 */
    case P_ENERGY: s->el_l2 = (int32_t)(v * 3428u); break;                /* log2(E_l / 1e4): 0 .. 6.64 */
    case P_TORT: s->tort_q8 = tx_knob_exp(v, 64u, 4 << 16); break;        /* 64 .. 1024 (x 0.25 .. 4) */
    case P_HEIGHT: s->height_l2 = 587592 + (int32_t)(v * 1714u); break;    /* log2(500 m) + v log2(10) / 127 */
    default: break;
    }
}

static void thunder_init(void *st, uint32_t seed)
{
    thunder_t *s = (thunder_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    s->limit = NF;
    for (i = 0; i < NF; i++) s->em[i] = 0xFF;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
}

static void thunder_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    tx_cloud_key(&((thunder_t *)st)->cloud, note, vel, on);
}

static void cut(thunder_t *s, unsigned i)
{
    tx_declick_add(&s->dc, s->fl[i].lp2 * s->fl[i].out_g);
    s->fl[i].active = 0;
    s->em[i] = 0xFF;
    s->next[i] = 0;
}

static void thunder_param(void *st, uint8_t id, uint8_t val)
{
    thunder_t *s = (thunder_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NF ? NF : val);
        for (i = s->limit; i < NF; i++) if (s->fl[i].active) cut(s, i);
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* a key press: a flash in a free slot (else the oldest makes way) */
static void flash(thunder_t *s, const tx_event_t *e)
{
    unsigned i, k = NF;
    uint32_t old = 0;
    int32_t dl2, el2, lv;
    for (i = 0; i < s->limit && k == NF; i++) if (!s->fl[i].active) k = i;
    if (k == NF) {
        for (i = 0; i < s->limit; i++) if (s->fl[i].clock >= old) { old = s->fl[i].clock; k = i; }
        s->dropped++;
        cut(s, k);
    }
    dl2 = s->dist_l2 - ((int32_t)e->note - 60) * 5461;
    el2 = s->el_l2 + ((int32_t)e->vel - 100) * 1638;
    el2 = el2 < 0 ? 0 : el2 > 435412 ? 435412 : el2;
    lv = (int32_t)(((uint32_t)e->vel * e->vel * 2u * (uint32_t)s->level) >> 15);
    tx_flash_start(&s->fl[k], &s->cloud.rng, tx_exp2(dl2), tx_exp2(s->height_l2), s->tort_q8, el2, lv);
    s->em[k] = e->emitter; s->note[k] = e->note;
    s->next[k] = 1323u + (tx_cloud_exp_q8(&s->cloud, 882u << 8) >> 8);   /* 30 ms + exp(20 ms) */
}

TX_AUDIO_CODE static void thunder_render(void *st, int32_t *mix, unsigned n)
{
    thunder_t *s = (thunder_t *)st;
    int32_t buf[TX_BLOCK];
    unsigned k, i, j;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) buf[j] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    for (i = 0; i < k; i++) flash(s, &s->ev[i]);
    for (i = 0; i < NF; i++) {
        tx_flash_t *f = &s->fl[i];
        if (!f->active) continue;
        if (s->next[i] && f->gen_done && f->clock >= s->next[i]) {   /* held: another return stroke */
            const tx_emitter_t *e = s->em[i] < TX_CLOUD_EMITTERS ? &s->cloud.em[s->em[i]] : 0;
            if (e && e->held && e->note == s->note[i] && f->nst < TX_TH_STROKES) {
                tx_flash_restrike(f, 19661);
                s->next[i] = f->clock + 1323u + (tx_cloud_exp_q8(&s->cloud, 882u << 8) >> 8);
            } else
                s->next[i] = 0;
        }
        tx_flash_add(f, &s->cloud.rng, buf, n);
        if (!f->active) { s->em[i] = 0xFF; s->next[i] = 0; }
    }
    tx_declick_run(&s->dc, buf, n);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = s->dc.v | s->hp.x1 | s->hp.y1;
        for (i = 0; i < NF; i++) z |= s->fl[i].active;
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void thunder_stats(const void *st, tx_stats_t *o)
{
    const thunder_t *s = (const thunder_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < NF; i++) n += s->fl[i].active;
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: two flashes started in the same block (both channels grow at once), close and energetic, held (all
 * strokes), then two more on top (stealing) */
static const char SCENE_MAX[] =
    "0 particles 2\n0 param DIST 0\n0 param ENERGY 127\n0 param TORT 127\n0 param HEIGHT 127\n"
    "0 on 79 127\n0 on 53 127\n2000 on 67 127\n2000 on 60 127\n"
    "3000 off 53\n3000 off 60\n3000 off 67\n3000 off 79\n"
    "6000 end\n";

const tx_engine_t tx_eng_thunder = {
    "thunder", "#25 Thunder", NP, PARAMS, NF, NF, sizeof(thunder_t), SCENE_MAX,
    thunder_init, thunder_key, thunder_param, thunder_render, thunder_stats,
};
