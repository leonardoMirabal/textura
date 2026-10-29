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
 *            Each puff its own period (+-0.75 oct) and depth: a broad peak around f_p, no steady line.
 *   bundle   close flames synchronise in phase, farther ones in counter-phase, far ones not at all (E12, a
 *            discontinuous transition): Kuramoto coupling per block, the lag 0 or pi set by SPACING (ours).
 *   sum      the flames' noises are independent Gaussian sources with the same spectrum, so their sum is one noise
 *            whose variance is the sum of the squared gains: one band, gain sqrt(sum g_i^2) per block. The cost does
 *            not grow with the number of flames.
 *   crackle  wick / wax micro-bursts: Poisson events per flame (~6 / s at the default, as the wood-wick references'
 *            7-12 onsets / s), power-law sizes (b 1.4, as fracture E33), each a burst of 2-6 sharp clicks over 1-6 ms;
 *            a bigger one when a flame is lit.
 *   jet      a gas flame's fuel jet is a turbulent jet of its own: a hiss at St U / d (St ~0.2; ~1 mm orifice, 15-45 m/s:
 *            2.5-7 kHz), its power rising steeply with the flow (Lighthill: U^8; ours, from FLOW 64 up, x the brightness
 *            of SLOPE): the gas burner's and the torch's 2-8 kHz hiss over the roar (references, note 18).
 *   gusts    in a draught the flame is blown: gusts arrive as events (Poisson, (DRAUGHT / 127)^2 x 2 per s), each its own
 *            strength (log-uniform up to +24 dB of roar, deeper with the draught), rise (25-90 ms) and fall (0.1-0.6 s),
 *            the flicker deepening while it lasts: a candle in the wind's 15-23 dB level range, its whoomps (references).
 * Realism pass (reference recordings, analysis only): a candle is quiet, broadband (-2 dB / oct) and crackly, its
 * level wandering slowly (0.5-1.3 Hz, 1.5-3 dB): so the still-air flicker no longer modulates the roar (a 12 Hz
 * tremolo read as synthetic), the roar is broader (Q 0.5) and lower (from 120 Hz), and quieter at the default. */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_pulse.h"

#define OUT_TRIM 10170               /* output gain, Q12 (+7.3 dB; -1 dB with the louder pops, note 18): integrated (a key held 10 s): -39.9 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter;
                                    * lowered 1.9 dB: the default chord <= +12 dBFS, the max scene <= +24 dBFS) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +0.6 dB with it, held there by the max scene under +24.1 dBFS (its mean
 * loudness below the old one) */
#define VARY_LO -100
#define VARY_HI 0                  /* (no boost: the max scene stays under +24.1 dBFS) */
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 3

enum { P_SIZE, P_FLOW, P_SLOPE, P_SPACING, P_DRAUGHT, P_SPUTTER, NP };

static const tx_param_t PARAMS[NP] = {
    {"SIZE", 127, 33, "flame size D at C4: 5 mm .. 1 m (log) -> flicker 0.52 sqrt(g/D) = 23 .. 1.6 Hz (keys: lower = bigger)"},
    {"FLOW", 127, 15, "gas flow / burn rate: roar level -30 .. 0 dB and peak 120 .. 700 Hz"},
    {"SLOPE", 127, 89, "roar fall-off above the peak: alpha 2.1 (bright) .. 3.4 (dark), -6.3 .. -10 dB/oct"},
    {"SPACING", 127, 30, "distance between flames: touching = flicker in phase, apart = counter-phase, far = independent"},
    {"DRAUGHT", 127, 10, "air movement: still (steady flame) .. windy (deep, irregular flicker and gusts)"},
    {"SPUTTER", 127, 100, "wick crackle per flame: off .. 20 /s (log); each a cluster of 1-3 sharp clicks"},
};

#define NFL 8u                     /* flames: one per cloud emitter */
#define L2_WBLK 1656437            /* log2(1.6287 Hz x 256 / fs x 2^32), Q16: f_p of a 1 m flame, phase per block */
#define ROAR_G 327680              /* roar gain for one flame at full flow (Q15, 10.0): one flame at the defaults
                                    * ~-28 dBFS rms (calibrated by render) */

typedef struct {
    uint32_t th, w;                /* flicker phase and its step per block */
    int32_t amp;                   /* Q15, ramps to the target (lit) or 0 */
    int32_t det;                   /* detune, Q15 fraction */
    int32_t fmul, fdg;             /* this puff's period (the step x fmul) and depth, Q15 */
    uint8_t lit, note;
} flame_t;

typedef struct {
    tx_cloud_t cloud;
    tx_noise_t roar, jet;
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
    int32_t jet_g;                 /* the jet's hiss gain (Q15, 0: none) */
    int32_t ge, gpk;               /* a gust: its gain now and its peak (log2 Q16); */
    uint8_t gup, gdec, gatt, gpad; /* rising, its fall and rise (shifts per block) */
    uint16_t dropped;
    tx_vary_t vy;
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
    uint32_t fv = s->p[P_FLOW], fpk = tx_knob_exp(fv, 120u, 166730);   /* 120 .. 700 Hz (2.54 octaves): a candle's
                                                              * roar peaks 100-250 Hz (references), burners 300-600 (E11) */
    uint32_t k = (uint32_t)(127u - s->p[P_SLOPE]) * 13u / 8u + 98u;    /* octaves above f_pk, Q6: 2.5 (4 - alpha) */
    uint32_t fc = (uint32_t)(((uint64_t)fpk * tx_exp2((int32_t)(k << 10) + (16 << 16))) >> 16);
    tx_noise_band(&s->roar, fpk, 8, TX_NOISE_BAND);      /* Q 0.5: broad */
    tx_noise_tilt(&s->roar, fc);
    s->flow_g = (int32_t)tx_exp2((15 << 16) - (int32_t)((127u - fv) * 2580u));   /* 2^(-5 (1 - v/127)): -30 dB */
    if (fv > 64u) {                                          /* the jet: 2.5 .. 7 kHz, (U / U_max)^8 over FLOW 64 .. 127 */
        uint32_t u = fv - 64u;
        tx_noise_band(&s->jet, 2500u + u * 71u, 11, TX_NOISE_BAND);   /* Q 0.7 */
        s->jet_g = (int32_t)tx_exp2((8 << 16) - (int32_t)((63u - u) * 4161u)   /* 8 log2(U): -72 dB at FLOW 64, -42 dB re the roar gain at 127 (its band is not tilted) */
                                    - (int32_t)s->p[P_SLOPE] * 516);              /* darker SLOPE: -6 dB at 89 */
    } else s->jet_g = 0;
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
    case P_SPUTTER: tx_cloud_rate(&s->cloud, v ? tx_knob_exp(v - 1u, 26u, 500700) : 0u); break;   /* 0.1 .. 20 /s */
    default: break;
    }
}

static void candle_init(void *st, uint32_t seed)
{
    candle_t *s = (candle_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);                         /* lighting a flame: one tick */
    tx_cloud_sizes(&s->cloud, 5734, 8u << 12);               /* b = 1.4 over 8 octaves (2.4 decades) of energy */
    tx_noise_init(&s->roar, seed ^ 0x5EEDF1A3u);
    tx_noise_init(&s->jet, seed ^ 0x1E7A11CEu);
    tx_pulses_init(&s->ticks);
    s->limit = NFL;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((candle_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void candle_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((candle_t *)st)->vy);
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
            if (!f->lit) { f->th = tx_rand(r); f->det = tx_rand_s15(r) / 8; f->fmul = f->fdg = 32768; }   /* +-4 % */
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
    {   /* gust events: (d / 127)^2 x 2 per s, each its own strength, rise and fall */
        uint32_t d = (uint32_t)s->draught >> 8, p = d * d * 2u * 256u / 16129u;   /* per s, x 256 per block / 44100 .. */
        if (!s->gup && (tx_rand(r) >> 16) < (p * 65536u / 44100u)) {   /* (p: per s x 256 / fs, a block's chance) */
            uint32_t u = tx_rand(r);
            s->gpk = (int32_t)(((u >> 16) * (4u * 65536u >> 4)) >> 12) * (int32_t)(38u + d * 90u / 127u) / 128   /* 0 .. 4 oct, */
                     * (int32_t)(136u - s->p[P_FLOW]) / 136;     /* less over a roaring burner (0.3 oct at full flow) */
            s->gatt = (uint8_t)(1u + ((u >> 8) & 1u) + ((u >> 9) & 1u));   /* rise ~25-90 ms */
            s->gdec = (uint8_t)(4u + ((u >> 4) % 3u));                      /* fall ~0.1-0.6 s */
            s->gup = 1;
        }
        if (s->gup) { s->ge += (s->gpk - s->ge) >> s->gatt; if (s->ge >= s->gpk - 2048) s->gup = 0; }
        else s->ge -= s->ge >> s->gdec;
        if (s->ge < 64) s->ge = 0;
    }
    depth = (int32_t)(((uint32_t)s->draught * 19661u) >> 15) + (int32_t)(n - 1u) * 2621 + (s->ge >> 3);   /* still air: steady; a gust flutters */
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
        {   /* each puff its own period (within +-0.75 oct of f_p's, log-uniform) and depth (0.3 .. 1.3): six flames
             * flickering together were a steady 10-12 Hz tremolo, a line 15-19 dB over the envelope's floor (the
             * candle recordings: none) */
            uint32_t th0 = f->th, w = (uint32_t)(((uint64_t)f->w * (uint32_t)f->fmul) >> 15);
            f->th += w + (uint32_t)((int32_t)(w >> 15) * jit) + (uint32_t)c;
            if (f->th < th0 && (th0 - f->th) > 0x80000000u) {   /* a new puff (the phase wrapped, not pulled back) */
                f->fmul = (int32_t)tx_exp2((15 << 16) + (int32_t)tx_rand_s15(r) * 3 / 2);
                f->fdg = 9830 + (int32_t)tx_rand_below(r, 32768u);
            }
        }
        m = 32768 + ((tx_sin_q15(f->th) * (int32_t)(((int64_t)depth * f->fdg) >> 15)) >> 15);
        {
            int32_t gm = (s->gust * ((s->draught >> 2) + 2400)) >> 10;   /* gust x 8 x draught, + a slow wander in still
                                                              * air (references: rms std 1.5-3 dB at 0.5-1.3 Hz) */
            gm = gm > 24576 ? 24576 : gm < -27853 ? -27853 : gm;
            m = (m >> 1) * ((32768 + gm) >> 1) >> 13;
        }
        if (m < 0) m = 0;
        g[i] = (f->amp >> 1) * (m >> 1) >> 13;
        sum += (uint32_t)((g[i] >> 3) * (g[i] >> 3));
    }
    {
        uint32_t gg = isqrt32(sum) << 3;
        return (int32_t)(((uint64_t)gg * tx_exp2(s->ge + (12 << 16))) >> 12);   /* x the gust */
    }
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
        int32_t l2 = (14 << 16) + e->size_l2 * 8 + (e->kind == TX_EV_MAIN ? (1 << 16) : 0);   /* -6 dBFS .. */
        uint32_t a = tx_exp2(l2 > (16 << 16) ? (16 << 16) : l2);                         /* .. +6 dBFS (the near pops
                                                              * stand 12-20 dB over the roar: references, note 18) */
        a = (a * e->vel) >> 7;
        a = (uint32_t)(((uint64_t)a * (uint32_t)s->level) >> 15);
        {   /* a crackle: 2-6 sharp clicks within ~1-6 ms (a burst), random signs, each weaker (references: attack 1.5-6 ms,
             * crest 13-19 dB, broadband to 16 kHz; a single wide N-wave read as a soft synthetic tick) */
            tx_rng_t *r = &s->cloud.rng;
            unsigned c, nc = 2u + tx_rand_below(r, 5u), off = e->offset;
            int32_t amp = (int32_t)a;
            for (c = 0; c < nc; c++) {
                tx_pulse_start(&s->ticks, TX_PULSE_SHAPE_CLICK, 3u + tx_rand_below(r, 10u), (tx_rand(r) & 1u) ? amp : -amp,
                               off > 255u ? 255u : off);
                off += 4u + tx_rand_below(r, 56u);
                amp = (amp >> 8) * (int32_t)(110u + tx_rand_below(r, 130u));
            }
        }
    }
    g = flames(s);
    g = (int32_t)(((uint32_t)g * (uint32_t)(s->flow_g >> 3)) >> 12);        /* Q15 x Q12 */
    g = (int32_t)(((uint64_t)(uint32_t)g * ROAR_G) >> 15);
    g = (int32_t)(((uint64_t)(uint32_t)g * (uint32_t)s->level) >> 15);
    tx_noise_roar_add(&s->roar, g, buf, n);
    tx_noise_roar_add(&s->jet, (int32_t)(((int64_t)g * s->jet_g) >> 15), buf, n);   /* the jet follows the flames' gains */
    tx_pulses_add(&s->ticks, buf, n);
    tx_vary_block(&s->vy, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !g && !s->roar.gain && !s->jet.gain && !tx_pulses_active(&s->ticks)
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
