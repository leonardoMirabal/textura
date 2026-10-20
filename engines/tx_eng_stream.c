/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #4 Stream / brook: water running over stones (L2).
 *
 * Each held key is a little plunge in the stream (a riffle over a stone), its bubbles' size centred on the key; a press
 * also tips a stone in (a burst). Physics (note 02):
 *   bubbles    turbulence and small falls entrain bubbles with a broad power-law size distribution: a log-bin rate
 *              a^-gamma (gamma 0.5-2.3 from L7; no measured brook value). Each rings at its Minnaert
 *              frequency 3.28 m/s / a with the L2 damping (BUBBLE heroes); its amplitude ~ a^1.5 x D, D = u^beta: most
 *              bubbles are deep and quiet, a few near the surface loud, and only those (D > 0.9, L2's riseCutoff) rise
 *              (xi 0.1): the babble. NEAR moves beta (20 far .. 3 near, ours).
 *   bed        the bubbles that are too quiet or too many for the heroes become a noise band at the size centre of the
 *              same power (Campbell: sum A^2 / 4d).
 *   roar       a dense bubble cloud has collective low modes, ~ f0 / N^(1/3) (L8): a low band (100-600 Hz, Q 1.5, ours)
 *              ~ sqrt(rate) (L32: the flow raises mainly 125 Hz-2 kHz); ROAR, off by default.
 *   eddies     the flow is not steady: the bubble rate wanders with the eddies (two random processes, ~0.5 and ~4 Hz,
 *              depth EDDY; ours, set against the references' 1-2.4 dB rms swings).
 * Listening session 2 ("good water, but a wind sound mixed in"): the wind was the roar band, so ROAR defaults to 0
 * (water only; up for a big rushing stream). The water's own continuity now comes from the bubbles: FLOW 105 (~5 k / s),
 * sizes over 5 octaves (centre / 4 .. x 8) and amplitude ~ a^1/4 (the brook references: flat 0.5-5 kHz). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_bubble.h"
#include "tx_core_noise.h"

#define OUT_TRIM 8757               /* output gain, Q12 (+2.4 dB): integrated (a key held 10 s) at -26 LUFS, LEVEL 100
                                    * (LOUDNESS.md; re-set with the water-only default, session 2) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +4.2 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -130
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 2

enum { P_FLOW, P_SIZE, P_SPREAD, P_NEAR, P_EDDY, P_ROAR, NP };

static const tx_param_t PARAMS[NP] = {
    {"FLOW", 127, 105, "bubbles per second per plunge: 30 .. 30 000 (log): a trickle .. a rushing riffle"},
    {"SIZE", 127, 30, "bubble size at C4: radius 0.6 .. 8 mm (log): pitch centre 5.5 kHz .. 400 Hz (keys: a semitone)"},
    {"SPREAD", 127, 64, "size law gamma: 0.5 (many big bubbles: gulping) .. 2.3 (mostly small: fizzing)"},
    {"NEAR", 127, 64, "how near the surface they burst: deep (quiet, steady) .. near (loud, rising plinks: babble)"},
    {"EDDY", 127, 30, "eddies: a steady flow .. gurgling (the rate wanders, ~0.5 and ~4 Hz)"},
    {"ROAR", 127, 0, "the bubble cloud's low roar (100-600 Hz): off (water only) .. a big rushing stream"},
};

#define NH TX_POOL_MAX
#define A_REF_L2 ((26 << 16) + 22000)       /* a near bubble at the size centre: 2^27 Q30 = -18 dBFS (calibrated by render) */
#define HERO_MIN_L2 (-6 * 65536)    /* quieter than A_REF / 64: into the bed */
#define L2_FC(a_l2) (765600 - (a_l2))   /* log2 of the Minnaert frequency 3280 Hz / a_mm, Q16 */

typedef struct {
    tx_cloud_t cloud;
    tx_pool_t pool;
    tx_declick_t dc;
    tx_dcblock_t hp;
    tx_noise_t bed, roar;
    tx_bubble_t b[NH];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK];
    uint32_t bed_e;                /* this block's bed energy (sum A^2 fs / 4d, Q?) */
    int32_t size_l2, beta_q8, eddy_d, e1, e2, flow_on;
    uint32_t rate_q8;
    uint8_t p[NP], limit, quiet, pad;
    int32_t level;
    uint16_t dropped;
    tx_vary_t vy;
} stream_t;

static uint32_t isqrt_s(uint32_t x)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) { if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return r;
}

static void set_param(stream_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_FLOW: s->rate_q8 = tx_knob_exp(v, 30u << 8, 653057); break;     /* 30 .. 30 000 /s (9.97 oct) */
    case P_SIZE: s->size_l2 = -48300 + (int32_t)(v * 1934u); break;        /* log2(0.6 mm) + v log2(13.3) / 127 */
    case P_SPREAD:                                            /* alpha = gamma + 1, gamma 0.5 .. 2.3, over 4 oct */
        tx_cloud_sizes(&s->cloud, 6144u + v * 58u, 5u << 12);   /* over 5 octaves */
        break;
    case P_NEAR: s->beta_q8 = (int32_t)(20 * 256) - (int32_t)v * 35; break;   /* beta 20 .. 2.6 */
    case P_EDDY: s->eddy_d = (int32_t)v * 180; break;                      /* depth 0 .. 0.7 (Q15) */
    default: break;
    }
}

static void stream_init(void *st, uint32_t seed)
{
    stream_t *s = (stream_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 12, 60);                       /* a stone tipped in: a gulp of bubbles */
    tx_noise_init(&s->bed, seed ^ 0xB0BB1Eu);
    tx_noise_init(&s->roar, seed ^ 0x2041u);
    s->limit = 16;
    tx_pool_init(&s->pool, NH, s->limit);
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((stream_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void stream_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((stream_t *)st)->vy);
    tx_cloud_key(&((stream_t *)st)->cloud, note, vel, on);
}

static void stream_param(void *st, uint8_t id, uint8_t val)
{
    stream_t *s = (stream_t *)st;
    if (id == TX_PID_PARTICLES) {
        uint32_t cut, i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NH ? NH : val);
        cut = tx_pool_set_limit(&s->pool, s->limit);
        for (i = 0; i < NH; i++) if (cut & (1u << i)) { tx_declick_add(&s->dc, s->b[i].last); s->b[i].env = 0; }
    } else if (id == TX_PID_LEVEL) {
        int32_t nl = tx_level_q15(val > 127u ? 127u : val);
        tx_bubbles_rescale(s->b, &s->pool, s->level, nl);
        s->level = nl;
    } else
        set_param(s, id, val);
}

/* one bubble: radius from the key's centre and the power-law draw, depth factor D = u^beta */
static void bubble(stream_t *s, const tx_event_t *e)
{
    tx_rng_t *r = &s->cloud.rng;
    int32_t c_l2 = s->size_l2 - ((int32_t)e->note - 60) * 5461;   /* the key's centre radius (log2 mm, Q16) */
    int32_t a_l2 = c_l2 - (2 << 16) + e->size_l2 * 16;       /* from centre / 4 upward (power law: big ones rarer) */
    int32_t d_l2 = -(int32_t)(((int64_t)tx_neglog2_q12(tx_rand(r)) * s->beta_q8) >> 4);   /* log2 D = beta log2 u, Q16 */
    int32_t l = (a_l2 - c_l2) / 4 + d_l2 + ((int32_t)e->vel - 100) * 1000;   /* x (a / a_c)^1/4 x D, velocity: with the
                                                              * small ones the common ones and a big one ringing longer, this
                                                              * keeps the brook references' flat 0.5-5 kHz (ours: L2's a^1.5,
                                                              * then a^1, boomed; a^-1/2 hissed) */
    uint32_t env, stolen;
    int slot;
    if (e->kind != TX_EV_STREAM) l += 65536;                 /* a stone tipped in: louder */
    if (l < HERO_MIN_L2) {                                   /* quiet: into the bed (A^2 fs / 4d, in 2^-20 FS^2 s) */
        uint32_t d = tx_bubble_damp_q8(a_l2) >> 8;            /* 1/s */
        int32_t a2 = 2 * (A_REF_L2 - (30 << 16) + l) + (20 << 16);   /* log2(A^2 / 2^-20) */
        uint32_t e2 = a2 < 0 ? 0u : tx_exp2(a2 > (30 << 16) ? (30 << 16) : a2);
        s->bed_e += (e2 / (d ? d : 1u)) * 11u;                /* x fs / 4 = 11025: x 11 (k samples) */
        return;
    }
    l += A_REF_L2;
    l = l > (29 << 16) ? (29 << 16) : l;
    env = (uint32_t)(((uint64_t)tx_exp2(l) * (uint32_t)s->level) >> 15);
    slot = tx_pool_take(&s->pool, env, &stolen);
    if (slot < 0) { s->dropped++; return; }
    if (stolen) tx_declick_add(&s->dc, s->b[slot].last);
    tx_bubble_start(&s->b[slot], a_l2, d_l2 > -9963 ? 6554u : 0u, env, e->offset);   /* D > 0.9: it rises */
}

TX_AUDIO_CODE static void stream_render(void *st, int32_t *mix, unsigned n)
{
    stream_t *s = (stream_t *)st;
    tx_rng_t *r = &s->cloud.rng;
    unsigned k, i, j, held;
    int32_t *buf = s->buf, z;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    held = tx_cloud_held(&s->cloud);
    {   /* eddies: the rate wanders (two low-passed random processes) */
        int32_t m;
        s->e1 += (tx_rand_s15(r) - s->e1) >> 6;              /* ~0.4 Hz */
        s->e2 += (tx_rand_s15(r) - s->e2) >> 3;              /* ~3.5 Hz */
        m = (int32_t)(((int64_t)(s->e1 * 4 + s->e2 * 2) * s->eddy_d) >> 15);   /* the sd's ~ equal */
        m = m > 30000 ? 30000 : m < -30000 ? -30000 : m;
        tx_cloud_rate(&s->cloud, (uint32_t)(((uint64_t)s->rate_q8 * (uint32_t)(32768 + m)) >> 15));
    }
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;                              /* silent and nothing new */
    for (j = 0; j < n; j++) buf[j] = 0;
    s->bed_e = 0;
    for (i = 0; i < k; i++) bubble(s, &s->ev[i]);
    s->flow_on += held ? (32768 - s->flow_on) >> 4 : -(s->flow_on >> 4);
    if (!held && s->flow_on < 64) s->flow_on = 0;
    {   /* the events over the per-block cap: as average bubbles into the bed */
        uint32_t over = s->cloud.last_dropped;
        if (over) {
            uint32_t d = tx_bubble_damp_q8(s->size_l2) >> 8;
            uint32_t e2 = tx_exp2(2 * (A_REF_L2 - (30 << 16) - (4 << 16)) + (20 << 16));   /* a typical deep one */
            s->bed_e += over * (e2 / (d ? d : 1u)) * 11u;
        }
    }
    {   /* the bed: a band at the size centre, gain sqrt(energy per sample) (calibrated by render) */
        uint32_t fc = tx_exp2(L2_FC(s->size_l2));
        uint32_t g = isqrt_s(s->bed_e / n * 64u);            /* sqrt(sum / n): in 2^-10 FS (x 8) */
        g = (uint32_t)(((uint64_t)g * 28000u * (uint32_t)s->level) >> 18);
        tx_noise_band(&s->bed, fc, 16, TX_NOISE_BAND);
        tx_noise_roar_add(&s->bed, (int32_t)(g > 500000u ? 500000u : g), buf, n);
    }
    {   /* the roar: the cloud's low modes, ~ sqrt(rate) while the stream flows */
        uint32_t fl = tx_exp2(L2_FC(s->size_l2)) / 8u;
        uint32_t g = isqrt_s(s->rate_q8 >> 8) * (uint32_t)held;
        fl = fl < 100u ? 100u : fl > 600u ? 600u : fl;
        g = (uint32_t)(((uint64_t)g * 220u * (uint32_t)s->flow_on) >> 15);
        g = (uint32_t)(((uint64_t)g * s->p[P_ROAR]) >> 6);    /* ROAR: 0 .. 2 (64 = the former default level) */
        g = (uint32_t)(((uint64_t)g * (uint32_t)s->level) >> 15);
        tx_noise_band(&s->roar, fl, 24, TX_NOISE_BAND);
        tx_noise_roar_add(&s->roar, (int32_t)(g > 500000u ? 500000u : g), buf, n);
    }
    tx_bubbles_render(s->b, &s->pool, &s->dc, buf, n);
    tx_pool_tick(&s->pool);
    tx_declick_run(&s->dc, buf, n);
    tx_vary_block(&s->vy, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    z = s->hp.x1 | s->hp.y1 | s->dc.v | (int32_t)s->pool.alive | s->bed.gain | s->roar.gain | s->flow_on;
    s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
}

static void stream_stats(const void *st, tx_stats_t *o)
{
    const stream_t *s = (const stream_t *)st;
    o->particles = (uint16_t)tx_pool_count(&s->pool);
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = s->pool.steals;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: four plunges at the full rate, near the surface, every hero busy, stones tipped in */
static const char SCENE_MAX[] =
    "0 particles 32\n0 param FLOW 127\n0 param NEAR 127\n0 param EDDY 127\n0 param SIZE 0\n"
    "0 on 53 127\n0 on 60 127\n0 on 67 127\n0 on 79 127\n1000 on 53 127\n1500 param SIZE 127\n2000 param SPREAD 0\n"
    "3000 off 53\n3000 off 60\n3000 off 67\n3000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_stream = {
    "stream", "#4 Stream / brook", NP, PARAMS, NH, 16, sizeof(stream_t), SCENE_MAX,
    stream_init, stream_key, stream_param, stream_render, stream_stats,
};
