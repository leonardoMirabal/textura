/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #18 Lake ice: cracks running through a frozen lake, heard from the shore (CHIRP core).
 *
 * A key press is one crack (power-law size); holding the key lets the ice keep cracking at RATE. Lower keys are
 * farther (the distance at C4 = DIST, 2^(1/12) closer per semitone up). Each crack sends:
 *   the flexural chirp   through the ice: f(t) = x^2 / (8 pi kappa t^2) falling from 16 kHz, kappa = 951 h m^2/s,
 *                        amplitude ~ 1 / t and the ice's loss along the path (CHIRP core); 2-D spreading ~ 1 / sqrt(x);
 *   the air wave         the crack's click, x / 343 m/s later, ~ 1 / x, duller with distance (ours);
 *   the singing tone     the air-coupled flexural wave whose phase speed equals c_air rings at f = (f h) / h: 31 Hz m
 *                        for fresh lake ice, up to 48 for sea ice (D3, D7; 33 derived for E 9 GPa), on one shared
 *                        resonance of the sheet (Q of the ice), rung when each air wave arrives (note 09's delay).
 * Ice loss Q: near 0 C 40-100, -20 C ~290-650 (natural ice, C10): COLD sets it (Q 60 .. 500). Crack sizes: Pareto
 * alpha 1.5 over 3 octaves of energy (ours; note 09 gives crack rates as a knob). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_pulse.h"
#include "tx_core_chirp.h"

#define OUT_TRIM 23033               /* output gain, Q12 (+1.3 dB with the spectrogram fixes, session 5; +10.8 dB): max momentary (one velocity-100 press): -37.8 LUFS at LEVEL 100 -> target -20 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter;
                                    * lowered 7.0 dB: the default chord <= +12 dBFS, the max scene <= +24 dBFS,
                                    * the limiter under ~8 dB in the chord) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +2.9 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -130
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 3

enum { P_THICK, P_DIST, P_RATE, P_ICE, P_COLD, NP };

static const tx_param_t PARAMS[NP] = {
    {"THICK", 127, 64, "ice thickness h: 2 .. 30 cm (log): faster sweeps, singing tone (f h) / h = 1550 .. 100 Hz"},
    {"DIST", 127, 64, "distance of the crack at C4: 5 .. 500 m (log): longer sweeps, later air wave, quieter"},
    {"RATE", 127, 40, "cracks per second while a key is held: 0.1 .. 5 (log)"},
    {"ICE", 127, 0, "ice type: fresh lake ice (f h 31 Hz m) .. sea ice (48 Hz m)"},
    {"COLD", 127, 80, "temperature: near 0 C (wet, Q ~60, dull) .. -20 C (Q ~500, ringing)"},
};

#define NV 8u
#define NAIR 16u
#define CHIRP_L2 (13 << 16)        /* chirp start amplitude at 50 m, median crack: 2^13 Q15 = -12 dBFS (calibrated) */

typedef struct { uint32_t due; int32_t amp, hl2; uint16_t dur; } air_t;   /* hl2: the crack's local thickness */

typedef struct {
    tx_cloud_t cloud;
    tx_pool_t pool;
    tx_dcblock_t hp;
    tx_chirp_t ch[TX_POOL_MAX];
    tx_pulses_t clicks;
    air_t air[NAIR];
    tx_svf_t tone;
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t ex[TX_BLOCK], buf[TX_BLOCK];
    uint32_t now, tsilent, tmax;      /* samples since the tone was last rung; its 3 T60 bound */
    uint8_t p[NP], quiet, pad;
    int32_t level, h_l2, x_l2, q, tf, tg, tq;
    uint16_t dropped;
    tx_vary_t vy;
} ice_t;

/* the singing tone for ice of thickness 2^hl2 mm: f = (f h) / h, f h 31 .. 48 Hz m. Its Q: the ice's, at most 20:
 * the sheet radiates the coupled wave into the air (that is the tone), which damps it (session 5: the former Q 60-500
 * held one pure ~400 Hz line through every event; ours). Set again by each crack's air wave, through ice of that
 * crack's own local thickness: no two cracks ring the same note, and a ringing tone glides to the next */
static void tone_set(ice_t *s, int32_t hl2)
{
    uint32_t fh = 31000u + (uint32_t)s->p[P_ICE] * 134u;     /* Hz mm */
    uint32_t h = tx_exp2(hl2), f = fh / (h ? h : 1u);
    int32_t q = s->q > 20 ? 20 : s->q;
    s->tf = tx_svf_coef(f);
    s->tq = (int32_t)(16384u / (uint32_t)(q > 2 ? q : 2));
    s->tg = (int32_t)tx_svf_gin_noise(f, (uint32_t)q * 16u);
    s->tmax = (uint32_t)q * 291060u / (f ? f : 1u);          /* 3 T60 = 6.6 Q / f s, in samples */
}

static void set_param(ice_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_THICK: s->h_l2 = 283241 + (int32_t)(v * 2016u); break;         /* log2(20 mm) + v log2(15) / 127 */
    case P_DIST: s->x_l2 = 587592 + (int32_t)(v * 3428u); break;          /* log2(500 cm) + v log2(100) / 127 */
    case P_RATE: tx_cloud_rate(&s->cloud, tx_knob_exp(v, 26u, 369849)); break;   /* 0.1 .. 5 /s */
    case P_COLD: s->q = (int32_t)tx_knob_exp(v, 60u, 200540); break;      /* Q 60 .. 500 (3.06 oct) */
    default: break;
    }
    tone_set(s, s->h_l2);
}

static void ice_init(void *st, uint32_t seed)
{
    ice_t *s = (ice_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_sizes(&s->cloud, 6144, 3u << 12);
    tx_pool_init(&s->pool, NV, 6);
    tx_pulses_init(&s->clicks);
    s->q = 60;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((ice_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void ice_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((ice_t *)st)->vy);
    tx_cloud_key(&((ice_t *)st)->cloud, note, vel, on);
}

static void ice_param(void *st, uint8_t id, uint8_t val)
{
    ice_t *s = (ice_t *)st;
    if (id == TX_PID_PARTICLES) {
        uint32_t cut = tx_pool_set_limit(&s->pool, val), i;
        for (i = 0; i < TX_POOL_MAX; i++) if (cut >> i & 1u) s->ch[i].on = 0;   /* (a cut chirp stops at once) */
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* one crack: a chirp hero + a delayed air wave (click + tone) */
static void crack(ice_t *s, const tx_event_t *e)
{
    tx_rng_t *r = &s->cloud.rng;
    /* no two cracks alike (the user's session 3): each one somewhere else on the sheet (distance +-0.5 oct), through
     * ice of its own local thickness (+-0.2 oct: sweep and tone), with +-3 dB of energy (ours) */
    int32_t xl2 = s->x_l2 - ((int32_t)e->note - 60) * 5461 + tx_rand_s15(r);
    int32_t size = e->size_l2 * 8 + ((int32_t)e->vel - 100) * 819 + (tx_rand_s15(r) >> 1);
    int32_t hl2 = s->h_l2 + (tx_rand_s15(r) * 13) / 32;
    uint32_t x, stolen, i;
    int32_t a, lx;
    xl2 = xl2 < 435412 ? 435412 : xl2 > 1175000 ? 1175000 : xl2;   /* 1 .. 2500 m */
    x = tx_exp2(xl2);
    lx = xl2 - 805306;                                       /* log2(x / 50 m) */
    int slot;
    a = (int32_t)tx_exp2(CHIRP_L2 + size - lx / 2);          /* 2-D spreading ~ 1 / sqrt(x) */
    a = (int32_t)(((int64_t)(a > 32767 ? 32767 : a) * s->level) >> 15);
    slot = tx_pool_take(&s->pool, (uint32_t)a, &stolen);
    if (slot >= 0) {
        tx_chirp_start(&s->ch[slot], x, tx_exp2(hl2), (uint32_t)s->q, a);
        s->ch[slot].wait += e->offset;
        if (!s->ch[slot].on) tx_pool_free(&s->pool, (unsigned)slot);
    }
    for (i = 0; i < NAIR; i++) if (!s->air[i].amp) break;
    if (i < NAIR) {                                          /* the air wave: ~ 1 / x (ours: -6 dB at 50 m vs the chirp) */
        int32_t aa = (int32_t)tx_exp2(CHIRP_L2 - (1 << 16) + size - lx);
        s->air[i].amp = (int32_t)(((int64_t)(aa > 32767 ? 32767 : aa) * s->level) >> 15);
        s->air[i].due = s->now + e->offset + tx_chirp_air(x);
        s->air[i].hl2 = hl2;
        s->air[i].dur = (uint16_t)(3u + (x >> 9));           /* duller far away: ~ x (ours) */
        if (s->air[i].dur > 200u) s->air[i].dur = 200u;
    } else
        s->dropped++;
}

TX_AUDIO_CODE static void ice_render(void *st, int32_t *mix, unsigned n)
{
    ice_t *s = (ice_t *)st;
    unsigned k, i, j;
    int32_t *buf = s->buf, *ex = s->ex, any = 0;
    uint32_t rej0 = s->pool.rejected;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) buf[j] = ex[j] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    tx_pool_tick(&s->pool);
    for (i = 0; i < k; i++) crack(s, &s->ev[i]);
    for (i = 0; i < TX_POOL_MAX; i++) {
        if (!(s->pool.alive >> i & 1u)) continue;
        tx_chirp_add(&s->ch[i], buf, n);
        if (!s->ch[i].on) tx_pool_free(&s->pool, i);
        else s->pool.level[i] = (uint32_t)s->ch[i].g + 1u;
    }
    for (i = 0; i < NAIR; i++) {                             /* air waves arriving in this block */
        air_t *w = &s->air[i];
        int32_t d;
        if (!w->amp) continue;
        d = (int32_t)(w->due - s->now);
        if (d < (int32_t)n) {
            unsigned off = d < 0 ? 0u : (unsigned)d;
            tx_pulse_start(&s->clicks, TX_PULSE_SHAPE_NWAVE, w->dur, w->amp, off);
            tone_set(s, w->hl2);
            ex[off] += w->amp * 2048;                        /* rings the sheet's tone (calibrated by render) */
            w->amp = 0;
        }
    }
    tx_pulses_add(&s->clicks, buf, n);
    for (j = 0; j < n; j++) any |= ex[j];
    {
        int32_t g = 1024, gi = s->tg;
        tx_svf_bank_add_g(&s->tone, &s->tf, s->tq, &gi, &g, 1, ex, buf, n);
        s->tsilent = any ? 0u : s->tsilent + n;              /* a high-Q band can hold a tiny rounding cycle: */
        if (s->tsilent > s->tmax) s->tone.low = s->tone.band = s->tone.ef = 0;   /* 3 T60 after its last ring */
    }
    s->now += n;
    tx_vary_block(&s->vy, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (s->pool.rejected - rej0) + s->dropped);
    s->dropped = 0;
    {
        int32_t z = s->hp.x1 | s->hp.y1 | (int32_t)s->pool.alive | (int32_t)tx_pulses_active(&s->clicks) | !tx_svf_bank_quiet(&s->tone, 1);
        for (i = 0; i < NAIR; i++) z |= s->air[i].amp;
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
    (void)any;
}

static void ice_stats(const void *st, tx_stats_t *o)
{
    const ice_t *s = (const ice_t *)st;
    o->particles = (uint16_t)tx_pool_count(&s->pool);
    o->limit = s->pool.limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = s->pool.steals;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: every chirp slot busy (fast cracking on every key), thin ice far away (long sweeps), cold */
static const char SCENE_MAX[] =
    "0 particles 8\n0 param RATE 127\n0 param THICK 0\n0 param DIST 127\n0 param COLD 127\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "6000 end\n";

const tx_engine_t tx_eng_lakeice = {
    "lakeice", "#18 Lake ice", NP, PARAMS, NV, 6, sizeof(ice_t), SCENE_MAX,
    ice_init, ice_key, ice_param, ice_render, ice_stats,
};
