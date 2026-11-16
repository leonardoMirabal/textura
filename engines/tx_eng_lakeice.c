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
 *   the boom             a thermal crack in thick ice also thumps the sheet as a whole: a low boom (45-130 Hz, Q 16, its
 *                        pitch drawn per crack: ours, from the references' 60-140 Hz peaks), louder the bigger the crack.
 * Ice loss Q: near 0 C 40-100, -20 C ~290-650 (natural ice, C10): COLD sets it (Q 60 .. 500). Crack sizes: Pareto
 * alpha 1.4 over 4 octaves of energy (references: crack levels spread 3.4-12 dB; ours), each crack somewhere else on
 * the sheet (distance +-1 octave: the near sharp shots and the far long whistles of one lake) through ice of its own
 * thickness (+-0.5 octave), so the mass of small far cracks fills the night between the big ones (note 18). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_pulse.h"
#include "tx_core_chirp.h"

#define OUT_TRIM 16686               /* output gain, Q12 (-2.8 dB with the branching cracks, session 5; +1.3 dB with the spectrogram fixes, session 5; +10.8 dB): max momentary (one velocity-100 press): -37.8 LUFS at LEVEL 100 -> target -20 LUFS
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
    {"THICK", 127, 64, TX_TEXT("ice thickness h: 2 .. 30 cm (log): faster sweeps, singing tone (f h) / h = 1550 .. 100 Hz")},
    {"DIST", 127, 64, TX_TEXT("distance of the crack at C4: 5 .. 500 m (log): longer sweeps, later air wave, quieter")},
    {"RATE", 127, 40, TX_TEXT("cracks per second while a key is held: 0.1 .. 10 (log)")},
    {"ICE", 127, 0, TX_TEXT("fresh lake ice (f h 31 Hz m) | sea ice (48 Hz m) | a frozen lake booming far off | thin ice "
                     "cracking underfoot (a puddle, a pond's edge) (32 each)")},
    {"COLD", 127, 80, TX_TEXT("temperature: near 0 C (wet, Q ~60, dull) .. -20 C (Q ~500, ringing)")},
};

#define NV 8u
#define NAIR 16u
#define CHIRP_L2 (13 << 16)        /* chirp start amplitude at 50 m, median crack: 2^13 Q15 = -12 dBFS (calibrated) */

/* the kinds (ICE; ours, set against the references, analysis only) */
typedef struct {
    int32_t dx, dh;                /* distance and thickness re DIST's and THICK's, log2 Q16 */
    uint16_t fh;                   /* f h, Hz mm (0: fresh, 31 .. 35 by ICE) */
    uint8_t boom;                  /* the boom re a crack's, Q4 */
    uint8_t burst;                 /* cracks per press (a step on thin ice breaks several) */
    uint16_t spread;               /* over ms */
    uint8_t near, pad;             /* near: at the feet (1 .. 3 m), DIST unused */
    uint16_t rate, out;            /* the cracking rate re RATE's, its level: Q8 */
} lkind_t;
static const lkind_t LK[4] = {
    {0, 0, 0, 16, 1, 0, 0, 0, 256, 256},                    /* fresh lake ice */
    {0, 0, 48000, 16, 1, 0, 0, 0, 256, 256},                /* sea ice */
    /* a lake booming far off: 6.4 x farther (long whistling sweeps, the air wave late and dull; second pass: 4 x was
     * nearer than the references, texd 2.28 -> 2.16; 10 x lost the busy end's fused cloud), 2 x thicker, its booms
     * (the sheet's thermal cracks thumping it) 4 x louder, half as often (the references: rumbling ice); +4.3 dB for
     * the farther cracks (its loudness kept) */
    {175672, 65536, 0, 64, 1, 0, 0, 0, 128, 834},
    /* thin ice underfoot: 8 x thinner (2.5 mm .. 4 cm: the sweeps fast and high, the tone high), at the feet; a step
     * breaks 4 or so within ~0.25 s, no boom (the references: walking on thin ice, 1.5-10 kHz, a step a second) */
    {0, -196608, 20000, 0, 4, 250, 1, 0, 256, 80},
};

typedef struct { uint32_t due; int32_t amp, hl2; uint16_t dur, boom; } air_t;   /* hl2: the crack's local thickness; boom: its pitch, Hz */

typedef struct {
    tx_cloud_t cloud;
    tx_pool_t pool;
    tx_dcblock_t hp;
    tx_chirp_t ch[TX_POOL_MAX];
    tx_pulses_t clicks;
    air_t air[NAIR];
    tx_svf_t tone, boom;
    int32_t hpl;                    /* near: the lows taken out (underfoot: no sheet-wide sweep below ~700 Hz) */
    int32_t bf, bgin;               /* the boom's coefficient, input gain */
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t ex[TX_BLOCK], buf[TX_BLOCK], bx[TX_BLOCK];
    uint32_t now, tsilent, tmax;      /* samples since the tone was last rung; its 3 T60 bound */
    uint8_t p[NP], quiet, pend;     /* pend: the first key's crack still to come (pnote, pvel) */
    uint8_t pnote, pvel, pad2[2];
    const lkind_t *k;
    int32_t level, h_l2, x_l2, q, tf, tg, tq;
    uint16_t dropped;
    tx_vary_t vy;
} ice_t;

/* the singing tone for ice of thickness 2^hl2 mm: f = (f h) / h, f h 31 .. 48 Hz m. Its Q: the ice's, at most 48
 * (20 until note 18: the references' cracks ring 0.1-0.3 s; each crack's own pitch keeps it from one line):
 * the sheet radiates the coupled wave into the air (that is the tone), which damps it (session 5: the former Q 60-500
 * held one pure ~400 Hz line through every event; ours). Set again by each crack's air wave, through ice of that
 * crack's own local thickness: no two cracks ring the same note, and a ringing tone glides to the next */
static void tone_set(ice_t *s, int32_t hl2)
{
    uint32_t fh = s->k->fh ? s->k->fh : 31000u + (uint32_t)s->p[P_ICE] * 134u;     /* Hz mm */
    uint32_t h = tx_exp2(hl2), f = fh / (h ? h : 1u);
    int32_t q = s->q > (s->k->near ? 4 : 48) ? (s->k->near ? 4 : 48) : s->q;   /* (near: a broad knock of the broken plate) */
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
    case P_RATE: case P_ICE:
        s->k = &LK[s->p[P_ICE] >> 5];
        tx_cloud_rate(&s->cloud, (tx_knob_exp(s->p[P_RATE], 26u, 435383) * s->k->rate) >> 8);   /* 0.1 .. 10 /s */
        tx_cloud_burst(&s->cloud, s->k->burst, s->k->spread);
        break;
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
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_sizes(&s->cloud, 5734, 4u << 12);
    tx_cloud_omori(&s->cloud, 2, 6963, 120, 3u << 12);      /* a crack branches: ~2 smaller ones follow within ~0.1-1 s
                                                              * (Omori p 1.7, c 120 ms, at least 3 octaves of energy
                                                              * under it; ours): no two presses alike (session 5) */
    tx_pool_init(&s->pool, NV, 6);
    tx_pulses_init(&s->clicks);
    s->q = 60;
    s->bf = tx_svf_coef(80u);
    s->bgin = (int32_t)tx_svf_gin_noise(80u, 256u);
    s->k = &LK[0];
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((ice_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

/* the first key (none held) starts the ice cracking at RATE, its first crack after a gap of its own (a start striking
 * at the press was louder than what follows); let go before that (a tap), it cracks then: a tap is still a crack. A
 * key while others are held cracks at once. */
static void ice_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    ice_t *s = (ice_t *)st;
    tx_cloud_t *c = &s->cloud;
    if (on && vel) tx_vary_press(&s->vy);
    if (on && vel && !tx_cloud_held(c)) {
        uint8_t b = c->burst_n;
        c->burst_n = 0;
        tx_cloud_key(c, note, vel, on);
        c->burst_n = b;
        s->pend = 1; s->pnote = note; s->pvel = vel;
        return;
    }
    if ((!on || !vel) && s->pend && note == s->pnote) {
        s->pend = 0;
        tx_cloud_key(c, note, s->pvel, 1);                   /* (its crack now: the burst on the held key) */
    }
    tx_cloud_key(c, note, vel, on);
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
    /* no two cracks alike (the user's session 3): each one somewhere else on the sheet (distance +-0.8 oct), through
     * ice of its own local thickness (+-0.3 oct: sweep and tone), with +-3 dB of energy (ours) */
    int32_t jx = tx_rand_s15(r) * 2;                         /* (its level change halved below: playable peaks) */
    int32_t xl2 = (s->k->near ? 473800 : s->x_l2 + s->k->dx) - ((int32_t)e->note - 60) * 5461 + jx;   /* (near: 1.5 m) */
    int32_t size = e->size_l2 * 8 + ((int32_t)e->vel - 100) * 819 + (tx_rand_s15(r) >> 1);
    int32_t hl2 = s->h_l2 + s->k->dh + tx_rand_s15(r);
    uint32_t x, stolen, i;
    int32_t a, lx;
    xl2 = xl2 < 435412 ? 435412 : xl2 > 1175000 ? 1175000 : xl2;   /* 1 .. 2500 m */
    x = tx_exp2(xl2);
    lx = xl2 - 805306;                                       /* log2(x / 50 m) */
    int slot;
    a = (int32_t)tx_exp2(CHIRP_L2 + size - lx / 2 + jx / 4); /* 2-D spreading ~ 1 / sqrt(x) */
    a = (int32_t)(((int64_t)(a > 32767 ? 32767 : a) * s->level) >> 15);
    slot = tx_pool_take(&s->pool, (uint32_t)a, &stolen);
    if (slot >= 0) {
        tx_chirp_start(&s->ch[slot], x, tx_exp2(hl2), (uint32_t)s->q, a);
        s->ch[slot].wait += e->offset;
        if (!s->ch[slot].on) tx_pool_free(&s->pool, (unsigned)slot);
    }
    for (i = 0; i < NAIR; i++) if (!s->air[i].amp) break;
    if (i < NAIR) {                                          /* the air wave: ~ 1 / x (ours: -6 dB at 50 m vs the chirp) */
        int32_t aa = (int32_t)tx_exp2(CHIRP_L2 - (1 << 16) + size - lx + jx / 2);
        s->air[i].amp = (int32_t)(((int64_t)(aa > 32767 ? 32767 : aa) * s->level) >> 15);
        s->air[i].due = s->now + e->offset + tx_chirp_air(x);
        s->air[i].hl2 = hl2;
        s->air[i].dur = (uint16_t)(3u + (x >> 9));           /* duller far away: ~ x (ours) */
        s->air[i].boom = (uint16_t)tx_exp2(359930 + (int32_t)(tx_rand(r) >> 16) * 3 / 2);   /* 45 .. 130 Hz */
        if (s->air[i].dur > 200u) s->air[i].dur = 200u;
    } else
        s->dropped++;
}

TX_AUDIO_CODE static void ice_render(void *st, int32_t *mix, unsigned n)
{
    ice_t *s = (ice_t *)st;
    unsigned k, i, j;
    int32_t *buf = s->buf, *ex = s->ex, *bx = s->bx, any = 0, bany = 0;
    uint32_t rej0 = s->pool.rejected;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) buf[j] = ex[j] = bx[j] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    for (i = 0; i < k && s->pend; i++) if (s->ev[i].note == s->pnote) s->pend = 0;   /* (the first key has cracked) */
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
            s->bf = tx_svf_coef(w->boom);                    /* and its boom, at this crack's own pitch */
            s->bgin = (int32_t)tx_svf_gin_noise(w->boom, 256u);
            bx[off] += (w->amp * 64) * s->k->boom;            /* (x 1024 at boom 16) */
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
    for (j = 0; j < n; j++) bany |= bx[j];
    {   /* the boom (Q 16: it rings ~0.1-0.3 s) */
        int32_t g = 2048, gi = s->bgin;
        tx_svf_bank_add_g(&s->boom, &s->bf, 16384 / 16, &gi, &g, 1, bx, buf, n);
        (void)bany;
    }
    s->now += n;
    tx_vary_block(&s->vy, buf, n);
    if (s->k->near) {   /* a one-pole high-pass at ~700 Hz (c = 1/8 - 1/64 ~ 2 pi 700 / fs): the references are flat 0.5-12 kHz */
        int32_t l = s->hpl, nz = 0;
        for (j = 0; j < n; j++) {
            int32_t x = buf[j] - l;
            nz |= buf[j];
            l += (x >> 3) - (x >> 6);
            buf[j] = x;
        }
        if (!nz && l > -4096 && l < 4096) {                  /* (silent in: its last residue let go, not held) */
            for (j = 0; j < n; j++) buf[j] = 0;
            l = 0;
        }
        s->hpl = l;
    }
    tx_trim(buf, n, (OUT_TRIM * s->k->out) >> 8);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (s->pool.rejected - rej0) + s->dropped);
    s->dropped = 0;
    {
        int32_t z = s->hp.x1 | s->hp.y1 | (int32_t)s->pool.alive | (int32_t)tx_pulses_active(&s->clicks) | !tx_svf_bank_quiet(&s->tone, 1) | !tx_svf_bank_quiet(&s->boom, 1);
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
    TX_TEXT("0 particles 8\n0 param RATE 127\n0 param THICK 0\n0 param DIST 127\n0 param COLD 127\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "6000 end\n");

const tx_engine_t tx_eng_lakeice = {
    "lakeice", TX_TEXT("#18 Lake ice"), NP, PARAMS, NV, 6, TX_STATE_SIZE(ice_t), SCENE_MAX,
    ice_init, ice_key, ice_param, ice_render, ice_stats,
};
