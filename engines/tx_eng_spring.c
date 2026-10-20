/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #23a Spring: kicking a reverb spring (DISPERSE core).
 *
 * A key press kicks a spring (velocity = how hard); it answers with descending chirped echoes every T_D, fading. Each
 * key is a spring of its own length: T_D at C4 = LENGTH, 2^(1/12) shorter per semitone up. Geometry (D6):
 * T_D ~ 4 L R / (r sqrt(E / rho)), F_C ~ sqrt(3) r sqrt(E / rho) / (16 sqrt(5) pi R^2) (e.g. steel, R 2.5 mm, r 0.2 mm,
 * 200 turns: T_D 31 ms, F_C 2.5 kHz): LENGTH sets T_D (wire length / turns), RADIUS F_C (thin wire on a wide helix =
 * low F_C, darker), DECAY the loop gain (0.5-0.95 range), CHIRP the number of allpass sections
 * (how far each echo is smeared). The high chirp chain is dropped (D2b: >= 30 dB down). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_cloud.h"
#include "tx_core_disperse.h"

#define OUT_TRIM 44912               /* output gain, Q12 (+20.8 dB): max momentary (one velocity-100 press): -44.8 LUFS at LEVEL 100 -> target -20 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter;
                                    * lowered 2.0 dB: the limiter under ~8 dB in the default chord) */

enum { P_LENGTH, P_RADIUS, P_DECAY, P_CHIRP, NP };

static const tx_param_t PARAMS[NP] = {
    {"LENGTH", 127, 64, "echo period T_D at C4: 20 .. 80 ms (log) (wire length, turns; keys: a semitone each)"},
    {"RADIUS", 127, 64, "transition frequency F_C: 1.5 .. 5 kHz (log) (thick wire on a narrow helix = higher, brighter)"},
    {"DECAY", 127, 80, "loop gain per echo: 0.5 .. 0.95 (short splash .. long rattle)"},
    {"CHIRP", 127, 64, "dispersion: 10 .. 40 allpass sections at fs/4 (a click .. a long descending chirp per echo)"},
};

#define NV 3u
#define KICK 20000                 /* kick amplitude at velocity 127 (Q15; calibrated by render) */

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    tx_spring_t sp[NV];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK];
    uint8_t note[NV], age[NV];
    uint8_t p[NP], limit, quiet;
    int32_t level, td_l2;
    uint32_t fc, m, g;
    uint16_t dropped;
} spring_t;

static void set_param(spring_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_LENGTH: s->td_l2 = 935968 + (int32_t)(v * 1032u); break;       /* log2(20000 us) + v x 2 oct / 127 */
    case P_RADIUS: s->fc = tx_knob_exp(v, 1500u, 113808); break;          /* 1.5 .. 5 kHz (1.74 oct) */
    case P_DECAY: s->g = 16384u + v * 116u; break;                       /* 0.5 .. 0.95, Q15 */
    case P_CHIRP: s->m = 10u + (v * 30u) / 127u; break;     /* (40 at most: the CPU rule) */
    default: break;
    }
}

static void spring_init(void *st, uint32_t seed)
{
    spring_t *s = (spring_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    for (i = 0; i < NV; i++) tx_spring_init(&s->sp[i]);
    s->limit = NV;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
}

static void spring_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    tx_cloud_key(&((spring_t *)st)->cloud, note, vel, on);
}

static void spring_param(void *st, uint8_t id, uint8_t val)
{
    spring_t *s = (spring_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NV ? NV : val);
        for (i = s->limit; i < NV; i++) tx_spring_init(&s->sp[i]);   /* (cut at once) */
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

static void configure(spring_t *s, unsigned i)
{
    int32_t l = s->td_l2 - ((int32_t)s->note[i] - 60) * 5461;
    tx_spring_set(&s->sp[i], tx_exp2(l), s->fc, s->m, (int32_t)s->g);
}

static void kick(spring_t *s, const tx_event_t *e)
{
    unsigned i, k = NV;
    uint8_t old = 0;
    for (i = 0; i < s->limit && k == NV; i++) if (s->sp[i].active && s->note[i] == e->note) k = i;
    for (i = 0; i < s->limit && k == NV; i++) if (!s->sp[i].active) k = i;
    if (k == NV) {                                           /* all ringing: the oldest */
        for (i = 0; i < s->limit; i++) if (s->age[i] >= old) { old = s->age[i]; k = i; }
        s->dropped++;
        tx_spring_init(&s->sp[k]);
    }
    s->note[k] = e->note; s->age[k] = 0;
    configure(s, k);
    tx_spring_kick(&s->sp[k], (int32_t)(((int64_t)KICK * e->vel * e->vel / 16129 * s->level) >> 15));
}

TX_AUDIO_CODE static void spring_render(void *st, int32_t *mix, unsigned n)
{
    spring_t *s = (spring_t *)st;
    unsigned k, i, j;
    int32_t *buf = s->buf;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) buf[j] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    for (i = 0; i < k; i++) kick(s, &s->ev[i]);
    for (i = 0; i < NV; i++) {
        if (!s->sp[i].active) continue;
        configure(s, i);                                     /* the knobs act on ringing springs too */
        tx_spring_add(&s->sp[i], buf, n);
        if (s->age[i] < 255u) s->age[i]++;
    }
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = s->hp.x1 | s->hp.y1;
        for (i = 0; i < NV; i++) z |= s->sp[i].active;
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void spring_stats(const void *st, tx_stats_t *o)
{
    const spring_t *s = (const spring_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < NV; i++) n += s->sp[i].active;
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: three springs with the most sections, long and ringing, kicked again */
static const char SCENE_MAX[] =
    "0 particles 3\n0 param CHIRP 127\n0 param DECAY 127\n0 param RADIUS 0\n0 param LENGTH 127\n"
    "0 on 53 127\n0 on 60 127\n0 on 67 127\n200 off 53\n200 off 60\n200 off 67\n"
    "500 on 79 127\n700 off 79\n1000 on 53 127\n1200 off 53\n"
    "4000 end\n";

const tx_engine_t tx_eng_spring = {
    "spring", "#23a Spring", NP, PARAMS, NV, NV, sizeof(spring_t), SCENE_MAX,
    spring_init, spring_key, spring_param, spring_render, spring_stats,
};
