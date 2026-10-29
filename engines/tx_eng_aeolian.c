/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #22 Wind / Aeolian harp: wind singing in wires and harp strings (AEOLIAN core).
 *
 * Each held key is a string tuned to its note, exposed to the wind; release: the gust dies over ~1.5 s. Physics:
 *   shedding  vortices leave the wire at f_l = St(Re) u / d (A2b Table 2), a narrow tone of Q(Re) (A2b Eq. 11):
 *             lift at f_l (and 3 f_l, by ear), drag at 2 f_l (-10 dB intensity: x 0.32), lift intensity ~ u^6
 *             (amplitude u^3), wake noise intensity ~ u^8 (amplitude u^4) falling -12 dB/oct above f_l.
 *             The listener's angle from the flow: lift ~ sin, drag ~ cos (dipoles).
 *   lock-in   (A2) a string mode n f1 with reduced velocity V = u / (2 pi d n f1) in 0.8 .. 1.27 captures the shedding
 *             (vortex-induced vibration): the tone moves to n f1, narrower (Q x (1 + 4 LOCK)) and louder (up to x 4,
 *             ours); the harp sings the partial nearest V ~ 1 and jumps to another as the wind changes (glide ~46 ms).
 *             Outside the window the bare wire's own tone at f_l remains.
 *   gusts     a slow random walk of u (~0.3 Hz, sd GUST x 40 %, ours: gust spectra were not researched); a key
 *             press adds a gust (velocity). And gusts as events (note 17: the singing-wire references' onsets come in
 *             bursts, interval CV ~0.9): Poisson at 0.15 .. 0.65 /s by GUST, three times as likely for ~2 s after one
 *             (gusts come in groups), each +12 .. +80 % of u (heavy-tailed), rising in ~0.25 s, dying in ~1.5 s; and
 *             the turbulence's fast buffeting, u +-12 % at ~5-30 Hz (the references' 4-32 Hz modulation -3 dB re the
 *             total, ours was -7 .. -12): the tones' level ~ u^3 flutters with it (ours).
 * Realism pass (aeolian-harp and singing-wire references, analysis only): a harp is heard inside the wind: every
 * reference carries a broadband low roar (50-250 Hz) at about the tones' level, the tones 10-20 dB over it in their own
 * band, crest ~10 dB, weak upper partials. Ours was bare tones (crest 3.8 dB, nothing under 600 Hz). So: a wind bed
 * (NOISE roar path, 110 Hz, Q 0.5, tilted above 250 Hz, ~u^3, gusting with u plus a ~10 Hz flutter: ours, set
 * against the references), and the 3 f lift partial 0.25 -> 0.08 (by ear before; the references show it weaker). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_aeolian.h"

#define OUT_TRIM 3700               /* output gain, Q12 (-3.7 dB; -2 with note 17's gusts and buffeting: ~ u^3 of a
                                    * fluctuating u is louder on the mean): integrated (a key held 10 s): -24.3 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +2.8 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 4

enum { P_WIND, P_GUST, P_WIRE, P_LOCK, P_ANGLE, NP };

static const tx_param_t PARAMS[NP] = {
    {"WIND", 127, 88, "wind speed u: 1 .. 20 m/s (log): f_l = St u / d, level ~ u^3 (tone) and u^4 (wake)"},
    {"GUST", 127, 40, "gustiness: steady .. gusty (u wanders +-40 %, ~0.3 Hz)"},
    {"WIRE", 127, 52, "wire / string diameter d: 0.2 .. 10 mm (log): thinner = higher f_l"},
    {"LOCK", 127, 90, "string lock-in: bare wire (no lock) .. harp string (locks to its partials: narrow, loud)"},
    {"ANGLE", 127, 127, "listener angle from the wind: 0 (downwind: drag 2 f) .. 90 deg (side: lift f)"},
};

#define NS 8u
#define BED_REF 180000            /* the wind bed's gain at 10 m/s (Q15; calibrated against the references) */
#define A_REF 1500                 /* lift amplitude at 10 m/s, d 1 mm, unlocked (Q15; calibrated by render) */

typedef struct {
    tx_aeol_t v;
    uint32_t f1;                   /* the string's fundamental, Hz Q8 */
    int32_t env;                   /* Q15: 1 while held, decays after release */
    uint8_t em, note, alive, pad;
} string_t;

typedef struct {
    tx_cloud_t cloud;
    tx_noise_t wake, bed;          /* the wire's wake; the wind itself (the bed the harp sings in) */
    tx_dcblock_t hp;
    string_t s[NS];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK];
    uint8_t p[NP], limit, quiet;
    int32_t level, wind_l2, gust, gsd, d_l2, lock, sinA, cosA, benv, bfl;
    int32_t gtgt, gcur, gmean, tb, tb2;   /* gust events: target, now, their running mean (Q15 of u); the turbulence */
    uint16_t gclu, gpad;           /* blocks the gusts stay clustered */
    uint16_t dropped;
    tx_vary_t vy;
} aeol_t;

static void set_param(aeol_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_WIND: s->wind_l2 = 653124 + (int32_t)(v * 2230u); break;       /* log2(1000 mm/s) + v log2(20) / 127 */
    case P_GUST: s->gsd = (int32_t)v * 103; break;                        /* Q15: 0 .. 0.4 */
    case P_WIRE: s->d_l2 = 500953 + (int32_t)(v * 2912u); break;          /* log2(200 um) + v log2(50) / 127 */
    case P_LOCK: s->lock = (int32_t)v * 258; break;
    case P_ANGLE: {
        uint32_t ph = (uint32_t)v * 8454894u;                  /* 0 .. 90 deg as a phase */
        s->sinA = tx_sin_q15(ph); s->cosA = tx_sin_q15(ph + 0x40000000u);
        break;
    }
    default: break;
    }
}

static void aeol_init(void *st, uint32_t seed)
{
    aeol_t *s = (aeol_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    tx_noise_init(&s->wake, seed ^ 0xAE01u);
    tx_noise_init(&s->bed, seed ^ 0xBED5u);
    tx_noise_band(&s->bed, 110, 8, TX_NOISE_BAND);          /* Q 0.5 */
    tx_noise_tilt(&s->bed, 250);
    for (i = 0; i < NS; i++) { s->s[i].em = 0xFF; tx_rng_seed(&s->s[i].v.rng, seed * 7u + i * 131u + 1u); }
    s->limit = 4;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((aeol_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void aeol_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((aeol_t *)st)->vy);
    tx_cloud_key(&((aeol_t *)st)->cloud, note, vel, on);
}

static void aeol_param(void *st, uint8_t id, uint8_t val)
{
    aeol_t *s = (aeol_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NS ? NS : val);
        for (i = s->limit; i < NS; i++) { s->s[i].alive = 0; s->s[i].em = 0xFF; s->s[i].env = 0; }   /* ramps out */
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

static void pluck(aeol_t *s, const tx_event_t *e)
{
    string_t *g = 0;
    unsigned i;
    for (i = 0; i < s->limit && !g; i++) if (s->s[i].alive && s->s[i].note == e->note) g = &s->s[i];
    for (i = 0; i < s->limit && !g; i++) if (!s->s[i].alive && !s->s[i].v.a1 && !s->s[i].v.a2) g = &s->s[i];
    for (i = 0; i < s->limit && !g; i++) if (!s->s[i].alive) g = &s->s[i];
    if (!g) { s->dropped++; return; }
    g->f1 = tx_note_inc((int32_t)e->note << 16) / 380u;        /* Hz Q8: inc / (2^32 / fs / 256) */
    if (!g->alive) g->v.inc = 0;
    g->alive = 1; g->em = e->emitter; g->note = e->note;
    s->gust += (int32_t)e->vel * 16;                          /* a gust with the press: up to +6 % (ours) */
}

TX_AUDIO_CODE static void aeol_render(void *st, int32_t *mix, unsigned n)
{
    aeol_t *s = (aeol_t *)st;
    unsigned k, i, j, nlive = 0;
    int32_t *buf = s->buf, wl2, wake = 0;
    uint32_t u, d, fl, q;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) buf[j] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    for (i = 0; i < k; i++) pluck(s, &s->ev[i]);
    /* the wind now: u (1 + gust); the gust a slow random walk back to 0 */
    s->gust += ((tx_rand_s15(&s->cloud.rng) * s->gsd) >> 17) - (s->gust >> 7);   /* AR(1), tau 0.74 s: sd ~ 8 steps = GUST x 40 % */
    s->gust = s->gust > 26214 ? 26214 : s->gust < -26214 ? -26214 : s->gust;
    {   /* gust events and the turbulence's buffeting */
        tx_rng_t *r = &s->cloud.rng;
        uint32_t pr = 57u + (uint32_t)s->p[P_GUST] * 2u;    /* per block, /65536: 0.15 .. 0.65 /s */
        if (s->gclu) { s->gclu--; pr *= 3u; }
        if ((tx_rand(r) >> 16) < pr) {
            uint32_t e = (tx_neglog2_q12(tx_rand(r) | 1u) * 2839u) >> 12;   /* E, Q12 */
            int32_t a = (int32_t)tx_exp2((int32_t)((e * 3u) >> 1) * 16 + (12 << 16) - 3 * 65536 + 31457);   /* 0.12 x 2^(1.5 E) */
            s->gtgt += a > 26214 ? 26214 : a;
            if (s->gtgt > 32767) s->gtgt = 32767;
            s->gclu = 344u;                                  /* ~2 s */
        }
        s->gtgt -= (s->gtgt >> 8) + (s->gtgt > 0);           /* dies ~1.5 s */
        s->gcur += (s->gtgt - s->gcur) >> 5;                 /* rises ~0.25 s */
        s->gmean += (s->gcur - s->gmean) >> 11;              /* (~12 s: WIND stays the mean speed, gusts over lulls) */
        s->tb += ((tx_rand_s15(r) * 3) / 4 - s->tb) >> 1;    /* white at the block rate, two poles: ~5-30 Hz */
        s->tb2 += (s->tb - s->tb2) >> 1;
    }
    u = (uint32_t)(((uint64_t)tx_exp2(s->wind_l2) * (uint32_t)(32768 + s->gust + s->gcur - s->gmean + (s->tb2 * 2) / 5)) >> 15);   /* mm/s
                                                              * (the buffeting ~+-12 %) */
    d = tx_exp2(s->d_l2);                                    /* um */
    fl = tx_aeol_shed(u, d, &q);
    wl2 = (32 << 16) - (int32_t)(tx_neglog2_q12(u) << 4) - 870948;   /* log2(u / 10 m/s): log2(10000) = 13.288 */
    for (i = 0; i < NS; i++) {
        string_t *g = &s->s[i];
        int held = g->alive && g->em < TX_CLOUD_EMITTERS && s->cloud.em[g->em].held && s->cloud.em[g->em].note == g->note;
        int32_t a = 0, boost = 32768, qq = (int32_t)q;
        uint32_t ft = fl;
        if (!g->alive && !g->v.a1 && !g->v.a2 && !g->v.a3) continue;
        g->env += held ? (32768 - g->env) >> 3 : -((g->env >> 6) + 1);   /* ~46 ms on; the gust dies over ~1.5 s
                                                              * (tau 0.37 s; session 5: no hard stop) */
        if (!held && g->env < 64) { g->env = 0; g->alive = 0; g->em = 0xFF; }
        nlive += g->alive;
        if (fl && s->lock) {                                 /* lock-in: a partial n f1 in 0.63 .. 1.0 f_l */
            uint32_t f1 = g->f1 >> 8, nn;
            if (f1) {
                nn = (fl * 4u / 5u + f1 / 2u) / f1;          /* the partial nearest 0.8 f_l (V ~ 1) */
                if (nn < 1u) nn = 1u;
                if (nn * f1 >= fl * 5u / 8u && nn * f1 <= fl) {
                    int32_t off = (int32_t)(nn * f1) - (int32_t)(fl * 4u / 5u);
                    int32_t close = 32768 - (int32_t)(((off < 0 ? -off : off) * 32768) / (int32_t)(fl / 5u + 1u));
                    if (close < 0) close = 0;
                    ft = nn * f1;
                    qq = (int32_t)q + (int32_t)(((int64_t)q * 4 * s->lock) >> 15);
                    boost = 32768 + (int32_t)(((int64_t)3 * 32768 * s->lock >> 15) * close >> 15);
                }
            }
        }
        if (ft) {
            uint32_t inc = ft * 97391u, al;
            g->v.inc = g->v.inc ? g->v.inc + (uint32_t)(((int32_t)(inc - g->v.inc)) >> 6) : inc;   /* glide (~0.37 s: the
                                                              * vortices re-lock to the next partial gradually; at
                                                              * >> 3 it moved in hard steps, a staircase: session 5) */
            tx_aeol_width(&g->v, ft, (uint32_t)qq);
            al = tx_exp2(3 * (wl2 > (1 << 16) ? (1 << 16) : wl2) + (16 << 16));   /* (u / 10)^3, Q16, <= 8 */
            a = (int32_t)(((uint64_t)al * A_REF * (uint32_t)boost >> 31) * (uint32_t)g->env >> 15);
            a = (int32_t)(((int64_t)a * s->level) >> 15);
            {   /* the buffeting on the tone (~u^3: +-36 %), also where its level has topped out (u over 20 m/s) */
                int32_t m = 32768 + (s->tb2 * 6) / 5;
                a = (int32_t)(((int64_t)a * (m < 8192 ? 8192 : m)) >> 15);
            }
            a = a > 32767 ? 32767 : a;
            if (a < 16) a = 0;                               /* (inaudible: not computed, the idle cost) */
        }
        if (a < 512)                                         /* a quiet tone: its 2 f and 3 f (-10, -22 dB under, so
                                                              * under -46 dBFS) are not computed (the idle cost) */
            tx_aeol_add(&g->v, (a * s->sinA) >> 15, 0, 0, buf, n);
        else
            tx_aeol_add(&g->v, (a * s->sinA) >> 15, (((a * 10486) >> 15) * s->cosA) >> 15, (((a * 2600) >> 15) * s->sinA) >> 15,
                        buf, n);
        wake += g->env;
    }
    /* the wake: band at f_l with a -6 dB/oct tilt above (= -12 dB/oct), level ~ u^4 per string */
    if (fl && wake) {
        uint32_t aw = tx_exp2(4 * (wl2 > (1 << 16) ? (1 << 16) : wl2) + (16 << 16));
        uint32_t fw = fl > 3000u ? 3000u : fl;              /* the Chamberlin band at Q 0.7 is stable only well
                                                              * below fs/6 (f < 2 - 1/Q): the wake's centre <= 3 kHz */
        tx_noise_band(&s->wake, fw, 11, TX_NOISE_BAND);
        tx_noise_tilt(&s->wake, fw);
        int32_t wl = (int32_t)(((uint64_t)aw * 2000u * (uint32_t)(wake > 131072 ? 131072 : wake) >> 31) * (uint32_t)s->level >> 15);
        tx_noise_level(&s->wake, wl < 16 ? 0 : wl);
    } else
        tx_noise_level(&s->wake, 0);
    tx_noise_roar_add(&s->wake, s->wake.target, buf, n);
    {   /* the wind bed: broadband low roar (references: 50-250 Hz at about the tone's level, the tones 10-20 dB over
         * it in their own band), ~ u^3, gusting with u and a faster flutter (ours) */
        int32_t tgt = wake > 32768 ? 32768 : wake, g;        /* (the strings' envelopes: the wind dies with them) */
        uint32_t al = tx_exp2(3 * (wl2 > (1 << 16) ? (1 << 16) : wl2) + (16 << 16));
        s->benv += (tgt - s->benv) >> 4;
        if (!nlive && s->benv < 64) s->benv = 0;
        s->bfl += (tx_rand_s15(&s->cloud.rng) - s->bfl) >> 2;   /* ~10 Hz flutter */
        g = (int32_t)(((uint64_t)al * BED_REF) >> 16);
        g = (int32_t)(((int64_t)g * (32768 + (s->bfl >> 1))) >> 15);
        g = (int32_t)(((int64_t)g * s->benv) >> 15);
        g = (int32_t)(((int64_t)g * s->level) >> 15);
        tx_noise_roar_add(&s->bed, s->benv ? g : 0, buf, n);
    }
    tx_vary_block(&s->vy, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    (void)nlive;
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = s->hp.x1 | s->hp.y1 | s->wake.gain | s->bed.gain | s->benv;
        for (i = 0; i < NS; i++) z |= s->s[i].alive | s->s[i].v.a1 | s->s[i].v.a2 | s->s[i].v.a3;
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void aeol_stats(const void *st, tx_stats_t *o)
{
    const aeol_t *s = (const aeol_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < NS; i++) n += s->s[i].alive;
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: 8 strings, strong gusty wind on thin wires, locked */
static const char SCENE_MAX[] =
    "0 particles 8\n0 param WIND 127\n0 param GUST 127\n0 param WIRE 0\n0 param LOCK 127\n0 param ANGLE 64\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "1500 param WIRE 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_aeolian = {
    "aeolian", "#22 Wind / Aeolian harp", NP, PARAMS, NS, 4, sizeof(aeol_t), SCENE_MAX,
    aeol_init, aeol_key, aeol_param, aeol_render, aeol_stats,
};
