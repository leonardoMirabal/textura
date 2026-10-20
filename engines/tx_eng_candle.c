/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #9 Candle / gas flame: the soft roar of a flame, breathing at its flicker frequency.
 *
 * Each held key lights a flame (up to 8: the PARTICLES limit); release snuffs it. Physics:
 *   roar     turbulent combustion is a monopole noise source: broadband, peaking at f_pk 300-600 Hz (E11) and
 *            falling as f^-alpha above (alpha 2.1-3.4, 3 by ear: E1). Here: noise -> band-pass at f_pk (Q 0.75) ->
 *            a one-pole low-pass k octaves higher, k = 2.5 (4 - alpha): -6 dB/oct up to it, -12 above, so about
 *            -3 alpha dB/oct over the audible band (NOISE core, roar path). Gas FLOW raises the level and f_pk.
 *   flicker  a buoyant flame puffs at f_p = 0.52 sqrt(g / D) (E14; a 2 cm candle 11.5 Hz, E13's 10-12 Hz): each
 *            flame's roar is amplitude-modulated at its own f_p. Lower keys are bigger flames (D x 2^(1/12) per
 *            semitone down), each flame detuned +-4 %. Still air flickers little, a draught much more.
 *   bundle   close flames synchronise in phase, farther ones in counter-phase, far ones not at all (E12, a
 *            discontinuous transition): Kuramoto coupling per block, the lag 0 or pi set by SPACING (ours).
 *   sum      the flames' noises are independent Gaussian sources with the same spectrum, so their sum is one noise
 *            whose variance is the sum of the squared gains: one band, gain sqrt(sum g_i^2) per block. The cost does
 *            not grow with the number of flames.
 *   sputter  wick / wax micro-bursts (ours, no measurement): Poisson ticks per flame, power-law sizes (b 1.4, as
 *            fracture E33), a bigger one when a flame is lit. */
#include "tx_engine.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_pulse.h"

enum { P_SIZE, P_FLOW, P_SLOPE, P_SPACING, P_DRAUGHT, P_SPUTTER, NP };

static const tx_param_t PARAMS[NP] = {
    {"SIZE", 127, 33, "flame size D at C4: 5 mm .. 1 m (log) -> flicker 0.52 sqrt(g/D) = 23 .. 1.6 Hz (keys: lower = bigger)"},
    {"FLOW", 127, 40, "gas flow / burn rate: roar level -30 .. 0 dB and peak 250 .. 700 Hz"},
    {"SLOPE", 127, 89, "roar fall-off above the peak: alpha 2.1 (bright) .. 3.4 (dark), -6.3 .. -10 dB/oct"},
    {"SPACING", 127, 30, "distance between flames: touching = flicker in phase, apart = counter-phase, far = independent"},
    {"DRAUGHT", 127, 20, "air movement: still (steady flame) .. windy (deep, irregular flicker and gusts)"},
    {"SPUTTER", 127, 40, "wick sputter ticks per flame: off .. 6 /s (log)"},
};

#define NFL 8u                     /* flames: one per cloud emitter */
#define L2_WBLK 1656437            /* log2(1.6287 Hz x 256 / fs x 2^32), Q16: f_p of a 1 m flame, phase per block */
#define ROAR_G 327680              /* roar gain for one flame at full flow (Q15, 10.0): one flame at the defaults
                                    * ~-28 dBFS rms (calibrated by render) */

typedef struct {
    uint32_t th, w;                /* flicker phase and its step per block */
    int32_t amp;                   /* Q15, ramps to the target (lit) or 0 */
    int32_t det;                   /* detune, Q15 fraction */
    uint8_t lit, note;
} flame_t;

typedef struct {
    tx_cloud_t cloud;
    tx_noise_t roar;
    tx_pulses_t ticks;
    tx_dcblock_t hp;
    flame_t fl[NFL];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK];
    uint8_t p[NP], limit, quiet;
    int32_t level;                 /* Q15 */
    int32_t size_l2;               /* log2(D at C4 / 1 m), Q16 */
    int32_t flow_g;                /* roar gain from FLOW, Q15 */
    int32_t kappa, lag;            /* coupling (Q15 of the flicker step) and lag (0 or pi as a phase) */
    int32_t draught;               /* Q15 */
    int32_t gust;                  /* slow random gain, Q15 signed */
    uint16_t dropped;
} candle_t;

static uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return r;
}

static void set_roar(candle_t *s)
{
    uint32_t fv = s->p[P_FLOW], fpk = tx_knob_exp(fv, 250u, 97355);   /* 250 .. 700 Hz (1.49 octaves) */
    uint32_t k = (uint32_t)(127u - s->p[P_SLOPE]) * 13u / 8u + 98u;    /* octaves above f_pk, Q6: 2.5 (4 - alpha) */
    uint32_t fc = (uint32_t)(((uint64_t)fpk * tx_exp2((int32_t)(k << 10) + (16 << 16))) >> 16);
    tx_noise_band(&s->roar, fpk, 12, TX_NOISE_BAND);
    tx_noise_tilt(&s->roar, fc);
    s->flow_g = (int32_t)tx_exp2((15 << 16) - (int32_t)((127u - fv) * 2580u));   /* 2^(-5 (1 - v/127)): -30 dB */
}

static void set_param(candle_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_SIZE: s->size_l2 = -500955 + (int32_t)(v * 3944u); break;   /* log2(0.005) + v log2(200) / 127 */
    case P_FLOW: case P_SLOPE: set_roar(s); break;
    case P_SPACING:                                           /* E12: in phase, counter-phase, then independent */
        if (v < 45u) { s->lag = 0; s->kappa = 8192 - (int32_t)v * 100; }
        else if (v < 90u) { s->lag = (int32_t)0x80000000u; s->kappa = 4915; }
        else { s->lag = 0; s->kappa = 0; }
        break;
    case P_DRAUGHT: s->draught = (int32_t)v * 258; break;
    case P_SPUTTER: tx_cloud_rate(&s->cloud, v ? tx_knob_exp(v - 1u, 13u, 451540) : 0u); break;   /* 0.05 .. 6 /s */
    default: break;
    }
}

static void candle_init(void *st, uint32_t seed)
{
    candle_t *s = (candle_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);                         /* lighting a flame: one tick */
    tx_cloud_sizes(&s->cloud, 5734, 8u << 12);               /* b = 1.4 over 8 octaves (2.4 decades) of energy */
    tx_noise_init(&s->roar, seed ^ 0x5EEDF1A3u);
    tx_pulses_init(&s->ticks);
    s->limit = NFL;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
}

static void candle_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    tx_cloud_key(&((candle_t *)st)->cloud, note, vel, on);
}

static unsigned lit_count(const candle_t *s)
{
    unsigned i, n = 0;
    for (i = 0; i < NFL; i++) n += s->fl[i].amp > 0 || s->fl[i].lit;
    return n;
}

static void candle_param(void *st, uint8_t id, uint8_t val)
{
    candle_t *s = (candle_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i, n;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NFL ? NFL : val);
        for (i = NFL, n = lit_count(s); i-- > 0 && n > s->limit;)   /* snuff the extra flames at once */
            if (s->fl[i].amp > 0 || s->fl[i].lit) { s->fl[i].amp = 0; s->fl[i].lit = 0; n--; }
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* flames: light / snuff from the held keys, ramp, couple, flicker -> the summed roar gain (Q15) */
static int32_t flames(candle_t *s)
{
    tx_rng_t *r = &s->cloud.rng;
    int32_t g[NFL], depth;
    uint32_t sum = 0;
    unsigned i, j, n = 0;
    for (i = 0; i < NFL; i++) {
        flame_t *f = &s->fl[i];
        const tx_emitter_t *e = &s->cloud.em[i];
        if (e->held && (!f->lit || f->note != e->note)) {
            if (!f->lit && f->amp == 0 && lit_count(s) >= s->limit) { s->dropped++; continue; }
            if (!f->lit) { f->th = tx_rand(r); f->det = tx_rand_s15(r) / 8; }   /* +-4 % */
            f->lit = 1; f->note = e->note;
            {
                int32_t d = s->size_l2 - ((int32_t)e->note - 60) * 5461;
                uint32_t w = tx_exp2(L2_WBLK - d / 2);            /* f_p = 0.52 sqrt(g / D) */
                f->w = w + (uint32_t)(((int64_t)w * f->det) >> 15);
            }
        } else if (!e->held)
            f->lit = 0;
        {
            int32_t tgt = f->lit ? (int32_t)e->vel * e->vel * 2 : 0;   /* velocity^2, Q15 */
            f->amp += (tgt - f->amp) >> (f->lit ? 3 : 4);      /* ~46 ms to light, ~93 ms to snuff */
            if (!f->lit && f->amp < 64) f->amp = 0;
        }
        n += f->amp > 0;
    }
    if (!n) return 0;
    s->gust += (tx_rand_s15(r) - s->gust) >> 4;              /* slow random gain (~1 Hz) */
    depth = 6554 + (int32_t)(((uint32_t)s->draught * 19661u) >> 15) + (int32_t)(n - 1u) * 2621;
    if (depth > 31130) depth = 31130;
    for (i = 0; i < NFL; i++) {
        flame_t *f = &s->fl[i];
        int32_t c = 0, jit, m;
        if (!f->amp) continue;
        if (s->kappa && n > 1u) {                            /* Kuramoto: pull toward (or away from) the others */
            for (j = 0; j < NFL; j++)
                if (j != i && s->fl[j].amp) c += tx_sin_q15(s->fl[j].th - f->th - (uint32_t)s->lag);
            c /= (int32_t)(n - 1u);
            c = (int32_t)(((f->w >> 15) * (uint32_t)s->kappa) >> 15) * c;   /* kappa w x mean sin */
        }
        jit = (int32_t)(((int64_t)tx_rand_s15(r) * s->draught) >> 16);   /* draught: +-50 % flicker jitter */
        f->th += f->w + (uint32_t)((int32_t)(f->w >> 15) * jit) + (uint32_t)c;
        m = 32768 + ((tx_sin_q15(f->th) * depth) >> 15);
        {
            int32_t gm = (s->gust * (s->draught >> 3)) >> 10;  /* gust x 4 x draught */
            gm = gm > 24576 ? 24576 : gm < -24576 ? -24576 : gm;
            m = (m >> 1) * ((32768 + gm) >> 1) >> 13;
        }
        if (m < 0) m = 0;
        g[i] = (f->amp >> 1) * (m >> 1) >> 13;
        sum += (uint32_t)((g[i] >> 3) * (g[i] >> 3));
    }
    return (int32_t)(isqrt32(sum) << 3);
}

TX_AUDIO_CODE static void candle_render(void *st, int32_t *mix, unsigned n)
{
    candle_t *s = (candle_t *)st;
    unsigned k, i;
    int32_t *buf = s->buf, g;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (i = 0; i < n; i++) buf[i] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    for (i = 0; i < k; i++) {                                /* sputter ticks (a bigger one at lighting) */
        const tx_event_t *e = &s->ev[i];
        int32_t l2 = (8 << 16) + e->size_l2 * 8 + (e->kind == TX_EV_MAIN ? (1 << 16) : 0);   /* -42 dBFS .. */
        uint32_t a = tx_exp2(l2 > (13 << 16) ? (13 << 16) : l2);                            /* .. -12 dBFS */
        a = (a * e->vel) >> 7;
        a = (uint32_t)(((uint64_t)a * (uint32_t)s->level) >> 15);
        tx_pulse_start(&s->ticks, TX_PULSE_SHAPE_NWAVE, 6u + tx_rand_below(&s->cloud.rng, 9u), (int32_t)a, e->offset);
    }
    g = flames(s);
    g = (int32_t)(((uint32_t)g * (uint32_t)(s->flow_g >> 3)) >> 12);        /* Q15 x Q12 */
    g = (int32_t)(((uint64_t)(uint32_t)g * ROAR_G) >> 15);
    g = (int32_t)(((uint64_t)(uint32_t)g * (uint32_t)s->level) >> 15);
    tx_noise_roar_add(&s->roar, g, buf, n);
    tx_pulses_add(&s->ticks, buf, n);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !g && !s->roar.gain && !tx_pulses_active(&s->ticks)
                         && !s->hp.x1 && !s->hp.y1);
}

static void candle_stats(const void *st, tx_stats_t *o)
{
    const candle_t *s = (const candle_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < NFL; i++) n += s->fl[i].amp > 0;
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: 8 flames, full flow, every coupling and draught path, the densest sputter */
static const char SCENE_MAX[] =
    "0 particles 8\n0 param FLOW 127\n0 param SLOPE 0\n0 param SPACING 60\n0 param DRAUGHT 127\n0 param SPUTTER 127\n"
    "0 param SIZE 0\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "1000 param SPACING 10\n1500 on 53 127\n1500 on 60 127\n2000 param SPACING 120\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_candle = {
    "candle", "#9 Candle / gas flame", NP, PARAMS, NFL, NFL, sizeof(candle_t), SCENE_MAX,
    candle_init, candle_key, candle_param, candle_render, candle_stats,
};
