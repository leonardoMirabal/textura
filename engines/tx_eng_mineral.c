/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #11 Mineral friction (the cheap scrape route): a stone rubbed, scratched or ground against another
 * (05: the brake-noise groan, honk).
 *
 * Each held key is a stone (a bar of the chosen mineral, its first mode at SIZE for C4, a semitone per key) being
 * rubbed; release lifts the contact and the stone rings out. Physics:
 *   contact  F2's impact series (SCRAPE core): rub = dense Gaussian-like impacts, scratch = sparse ones with
 *            exponential gaps (asperities ~1 mm apart), grind = correlated, heavier, longer impacts (~0.3 mm apart,
 *            AR(1) 0.9, Hertz length K A^-1/5); ROUGH moves between them (F2's navigation). The force passes a
 *            low-pass whose cutoff rises with the speed: fc = v / lambda, lambda 0.1 mm (I1: 100 Hz at 1 cm/s,
 *            10 kHz at 1 m/s). Level ~ sqrt(frictional power v F_N) (I1; garbled extract: the root is our reading).
 *   body     a constant-Q mineral (note 05 table: the loss factor 1/Q is the same for every mode, so higher modes
 *            die faster, which is what tells materials apart, I6): 3 free-free bar modes 1 : 2.7565 : 5.4039
 *            (modes over fs/6 dropped; the 4th and 5th, 8.93 and 13.3, faint under the rub: dropped for CPU, note 18), driven at unit noise power (the bank's input gain
 *            sqrt(fs Q / (pi f))), plus a little of the contact noise itself.
 *   stick    stick-slip (F5, brake noise, the same physics): at low speed two regimes coexist, a groan ~70-100 Hz and
 *            a honk ~400-500 Hz; the system jumps between them, a light tap restores the groan, the static-dynamic
 *            friction difference (STICK) sets how strong it is. Here: slip pulses (raised cosine, 3 ms / 1 ms) at
 *            the regime's rate +-5 %, random switches (~0.35 /s, honk more likely at speed), a key press -> groan.
 * Velocity scales the pressure. Ours: lambda, the scratch / grind spacings, the mode gains, the slip shapes.
 * Realism pass (stone-rubbing references, analysis only): rubbing stones are a broadband, irregular, darkish noise
 * (100 Hz-2 kHz, -5..-12 dB / oct, rms std 3-8 dB, kurtosis 2-12) with no ringing tone; ours was the first bar mode
 * 27 dB over the rest (a filter). So: a held stone is damped by the contact (Q <= 25 while rubbing, its own Q when it
 * rings free), the modes 18 dB lower, the contact itself x 4 through a ~2 kHz low-pass, the pressure irregular (a
 * ~1 Hz hand wander of ~0.6 oct, +-6 dB per block, catches +9 dB at ~2 / s), and a key press knocks the stones
 * together first (a Hertz pulse ~0.2 ms: the user expects a percussive start). Ours, set against the references. */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_pulse.h"
#include "tx_core_scrape.h"

#define OUT_TRIM 14872               /* output gain, Q12 (+3.4 dB step 6, hand strokes; +5.0 dB): integrated (a key held 10 s): -31.0 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +2.8 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 3

enum { P_STONE, P_SIZE, P_PRESSURE, P_SPEED, P_ROUGH, P_STICK, NP };

static const tx_param_t PARAMS[NP] = {
    {"STONE", 127, 64, "mineral (Q): flint 55, sandstone 65, slate 150, granite 150, limestone 150, obsidian 175, marble 300, quartz 1000"},
    {"SIZE", 127, 64, "stone pitch at C4: first bar mode 150 Hz .. 2 kHz (log) (keys: a semitone each)"},
    {"PRESSURE", 127, 64, "normal force: 0.5 .. 50 N (log): louder (~sqrt of the frictional power), more stick-slip"},
    {"SPEED", 127, 64, "sliding speed: 1 cm/s .. 1 m/s (log): brighter (contact cutoff v / 0.1 mm), denser, louder"},
    {"ROUGH", 127, 40, "surface: smooth rub (dense) .. scratch (sparse, ~1 mm) .. grind (correlated, heavy, ~0.3 mm)"},
    {"STICK", 127, 0, "stick-slip (static - dynamic friction): off .. strong groan ~80 Hz / honk ~450 Hz"},
};

#define NV 6u
#define NM 3u                     /* (the 4th and 5th bar modes, 8.9 and 13.3 f1 at -21 / -27 dB under the 1st and
                                    * 18 dB under the rub, dropped: note 18, CPU) */
#define G_REF 12000                /* contact force at the reference (5 N, 0.1 m/s, v100), Q15 (calibrated by render) */

TX_AUDIO_DATA static const uint16_t STONE_Q[8] = { 55, 65, 150, 150, 150, 175, 300, 1000 };
TX_AUDIO_DATA static const uint16_t BAR_R[NM] = { 1024, 2823, 5534 };   /* bar ratios, Q10 */
TX_AUDIO_DATA static const uint16_t MODE_G[NM] = { 256, 205, 154 };          /* mode gains, Q11 (ours; -18 dB
                                                              * in the realism pass: the body sat 27 dB over the rub) */

typedef struct {
    tx_scrape_t sc;
    tx_pulses_t slip;
    tx_svf_t body[NM];
    int32_t f[NM], gin[NM], g[NM];
    int32_t gain, target;          /* contact force gain now / wanted (Q15): lift and press ramps */
    uint32_t ph, sinc;             /* stick-slip phase and its step per sample */
    int32_t wander;                /* the hand's slow pressure wander (Q15, ~1 Hz; sd ~0.6 oct) */
    int32_t bq;                    /* the body's damping now: held = damped by the contact, free = the mineral's Q */
    uint32_t sph, sinc2;           /* the hand's stroke: phase, step per block (back and forth, ~0.6-1.1 Hz) */
    uint8_t em, note, vel, alive, honk, nm, pad[2];
} voice_t;

typedef struct {
    tx_cloud_t cloud;
    tx_declick_t dc;
    tx_dcblock_t hp;
    voice_t v[NV];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t tmp[TX_BLOCK], buf[TX_BLOCK], con[TX_BLOCK];
    int32_t dlp;                   /* the contacts' low-pass (~2 kHz: stone on stone is dark, references), shared */
    uint8_t p[NP], limit, quiet;
    int32_t level;                 /* Q15 */
    int32_t q, qx16;               /* body Q */
    int32_t f1_l2;                 /* log2(f1 at C4), Q16 */
    int32_t press_l2, speed_l2;    /* log2(F / 5 N), log2(v / 0.1 m/s), Q16 */
    int32_t stick;                 /* Q15 */
    uint16_t dropped;
    tx_vary_t vy;
} mineral_t;

static void set_param(mineral_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_STONE: {
        uint32_t q = STONE_Q[v >> 4];
        s->qx16 = (int32_t)(q * 16u);
        s->q = (int32_t)(16384u / q);
        break;
    }
    case P_SIZE: s->f1_l2 = 473780 + (int32_t)(v * 1928u); break;       /* log2(150) + v log2(13.3) / 127 */
    case P_PRESSURE: s->press_l2 = -217706 + (int32_t)(v * 3428u); break;   /* log2(0.5 / 5) + v log2(100) / 127 */
    case P_SPEED: s->speed_l2 = -217706 + (int32_t)(v * 3428u); break;      /* log2(0.01 / 0.1) + ... */
    case P_STICK: s->stick = (int32_t)v * 258; break;
    default: break;
    }
}

static void mineral_init(void *st, uint32_t seed)
{
    mineral_t *s = (mineral_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    for (i = 0; i < NV; i++) {
        tx_scrape_init(&s->v[i].sc, seed * 13u + i * 7919u + 5u);
        tx_pulses_init(&s->v[i].slip);
        s->v[i].em = 0xFF;
    }
    s->limit = 3;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((mineral_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void mineral_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((mineral_t *)st)->vy);
    tx_cloud_key(&((mineral_t *)st)->cloud, note, vel, on);
}

static void cut(mineral_t *s, voice_t *v)
{
    unsigned i;
    int32_t last = v->sc.lp * 256;
    tx_declick_add(&s->dc, last);
    tx_scrape_init(&v->sc, v->sc.rng.s);
    tx_pulses_init(&v->slip);
    for (i = 0; i < NM; i++) v->body[i].low = v->body[i].band = 0;
    v->alive = 0; v->em = 0xFF; v->gain = v->target = 0;
}

static void mineral_param(void *st, uint8_t id, uint8_t val)
{
    mineral_t *s = (mineral_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NV ? NV : val);
        for (i = s->limit; i < NV; i++) if (s->v[i].alive) cut(s, &s->v[i]);
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* a key press: a stone at the key's pitch, contact on (or the same stone re-pressed: back to the groan) */
static void press(mineral_t *s, const tx_event_t *e)
{
    voice_t *v = 0;
    unsigned i;
    for (i = 0; i < s->limit && !v; i++) if (s->v[i].alive && s->v[i].note == e->note) v = &s->v[i];
    if (!v) {
        for (i = 0; i < s->limit && !v; i++) if (!s->v[i].alive) v = &s->v[i];
        if (!v) {
            int32_t lo = 0x7FFFFFFF;
            for (i = 0; i < s->limit; i++) if (s->v[i].gain < lo) { lo = s->v[i].gain; v = &s->v[i]; }
            s->dropped++;
            cut(s, v);
        }
        v->alive = 1;
        v->ph = tx_rand(&s->cloud.rng);
        v->sph = 0x40000000u;                                /* (the first stroke from full speed: the knock) */
        v->sinc2 = 14984956u + (tx_rand(&s->cloud.rng) >> 16) * 114u;
    }
    v->em = e->emitter; v->note = e->note; v->vel = e->vel;
    v->honk = 0;                                             /* F5: a light tap restores the groan */
    v->sc.wait = 0;                                          /* the first impact now (no wait left from a stopping rub) */
    {   /* the stones meet: a knock (a Hertz pulse ~0.2 ms, ~ velocity) before the rub (the user expects a percussive
         * start; ours) */
        int32_t a = (int32_t)e->vel * e->vel;                /* Q15 x 1/2 */
        a = (int32_t)(((int64_t)a * s->level) >> 15);
        tx_pulse_start(&v->slip, TX_PULSE_SHAPE_HERTZ, 9u, (tx_rand(&s->cloud.rng) & 1u) ? a : -a, e->offset);
    }
}

/* per block: the stone's modes and the contact from the knobs, the stick-slip regime */
static void control(mineral_t *s, voice_t *v)
{
    tx_rng_t *r = &s->cloud.rng;
    int held = v->em < TX_CLOUD_EMITTERS && s->cloud.em[v->em].held && s->cloud.em[v->em].note == v->note;
    int32_t f1 = s->f1_l2 + ((int32_t)v->note - 60) * 5461 + (tx_vary_tone(&s->vy) >> 2), pl2, vl2, rough = s->p[P_ROUGH] * 258, a_l2, fsp;
    uint32_t i, fc, sp_mm;
    v->nm = 0;
    for (i = 0; i < NM; i++) {                               /* bar modes under fs/6 */
        uint32_t hz = (uint32_t)(((uint64_t)tx_exp2(f1) * BAR_R[i]) >> 10);
        if (hz > 7300u) break;
        v->f[i] = tx_svf_coef(hz);
        v->gin[i] = (int32_t)tx_svf_gin_noise(hz, held && s->qx16 > 400 ? 400u : (uint32_t)s->qx16);   /* at the Q in use */
        v->g[i] = MODE_G[i];
        v->nm = (uint8_t)(i + 1u);
    }
    /* pressure (velocity scales it) and speed */
    pl2 = s->press_l2 + ((int32_t)v->vel - 100) * 1638;      /* +-1 octave of force per 40 velocity */
    vl2 = s->speed_l2;
    {   /* the hand strokes back and forth (step 6: the friction references swell and fade with each stroke; ours was
         * a steady wall): the speed follows |sin| of the stroke (~0.6-1.1 Hz, each stroke its own), never quite 0;
         * at the turns the stone slows, darkens and quiets (ours) */
        int32_t sv = tx_sin_q15(v->sph), lsv;
        uint32_t u;
        sv = sv < 0 ? -sv : sv;
        u = (uint32_t)(sv < 3277 ? 3277 : sv) << 17;         /* 0.1 .. 1, as a fraction of 2^32 */
        lsv = -(int32_t)(tx_neglog2_q12(u) << 4);            /* log2 of it, Q16 (-3.3 .. 0) */
        vl2 += lsv;
        v->sph += v->sinc2;
        if ((v->sph >> 31) != ((v->sph - v->sinc2) >> 31)) {   /* a turn: the next stroke's own rate */
            v->sinc2 = 14984956u + (tx_rand(r) >> 16) * 114u;
        }
    }
    a_l2 = (pl2 + vl2) / 2;                                  /* force ~ sqrt(v F_N) against the reference */
    {   /* a hand rubs unevenly (references: rms std 3-8 dB, kurtosis 2-12): the pressure wanders (+-0.5 oct, ~1 Hz),
         * jitters per block (+-6 dB) and catches (+9 dB, ~2 / s) (ours, set against the references) */
        v->wander += (tx_rand_s15(r) - v->wander) >> 5;
        a_l2 += v->wander * 16 + (tx_rand_s15(r) + tx_rand_s15(r));
        if ((tx_rand(r) >> 16) < 762u) a_l2 += 98304;      /* p = 0.0116 per block */
    }
    a_l2 = a_l2 > (3 << 16) ? (3 << 16) : a_l2 < -(7 << 16) ? -(7 << 16) : a_l2;
    v->target = held ? (int32_t)(((uint64_t)tx_exp2(a_l2 + (16 << 16)) * G_REF) >> 16) : 0;
    v->target = (int32_t)(((uint32_t)(v->target > 32767 ? 32767 : v->target) * (uint32_t)s->level) >> 15);
    v->gain += (v->target - v->gain) >> 1;                   /* ~10 ms press / lift (follows the jitter) */
    v->bq = held && s->q < 655 ? 655 : s->q;                 /* held: the contact damps the stone to Q <= 25 (ours) */
    if (!held && v->gain < 16) v->gain = 0;
    fc = (uint32_t)(((uint64_t)tx_exp2(vl2 + (16 << 16)) * 1000u) >> 16);   /* v / 0.1 mm: 1000 Hz at 0.1 m/s */
    tx_scrape_cutoff(&v->sc, fc);
    /* rub <-> scratch <-> grind (F2's navigation by ROUGH, 0 .. 1) */
    sp_mm = (uint32_t)(((uint64_t)tx_exp2(vl2 + (16 << 16)) * 100u) >> 16);   /* speed, mm/s */
    if (sp_mm < 1u) sp_mm = 1u;
    if (rough < 16384) {                                     /* rub -> scratch: asperities 1 mm apart */
        v->sc.rub = (int32_t)(((uint32_t)(16384 - rough) * 2u * (uint32_t)v->gain) >> 15);
        v->sc.imp = (int32_t)(((uint32_t)rough * 3u * (uint32_t)v->gain) >> 15);
        v->sc.gap_q8 = 44100u * 256u / sp_mm;                /* fs x 1 mm / v */
        v->sc.corr = 0;
        v->sc.kdur_q8 = 2u << 8;
    } else {                                                 /* scratch -> grind: 1 -> 0.3 mm, AR 0 -> 0.9, K 2 -> 12 */
        int32_t t = (rough - 16384) * 2;
        uint32_t sp_q8 = (uint32_t)(256 - ((t * 179) >> 15));   /* spacing mm, Q8: 1 .. 0.3 */
        v->sc.rub = 0;
        v->sc.imp = (int32_t)(((uint32_t)(49152 + t / 2) * (uint32_t)v->gain) >> 15);
        if (v->sc.imp > 32767) v->sc.imp = 32767;
        v->sc.gap_q8 = 44100u * sp_q8 / sp_mm;
        v->sc.corr = (t * 29491) >> 15;
        v->sc.kdur_q8 = (2u << 8) + (uint32_t)((t * 2560) >> 15);
    }
    if (!v->sc.gap_q8) v->sc.gap_q8 = 256;
    /* stick-slip regime: random switches (~0.35 /s), honk likelier at speed; frequency rises a little with force */
    {
        int32_t vn = (s->p[P_SPEED] * 258), pn = s->p[P_PRESSURE] * 258;   /* 0 .. 1, Q15 */
        uint32_t u = tx_rand(r) >> 16, sw = v->honk ? (uint32_t)(98 - (vn * 66 >> 15)) : (uint32_t)(33 + (vn * 66 >> 15));
        if (u < sw) v->honk ^= 1u;                           /* per block: 33 .. 98 / 65536 x 172 /s */
        fsp = v->honk ? 400 + (pn * 100 >> 15) : 70 + (pn * 30 >> 15);
        v->sinc = (uint32_t)fsp * 97391u;
    }
}

TX_AUDIO_CODE static void mineral_render(void *st, int32_t *mix, unsigned n)
{
    mineral_t *s = (mineral_t *)st;
    unsigned k, i, j;
    int32_t *buf = s->buf, *tmp = s->tmp, *con = s->con;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) buf[j] = con[j] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;                              /* silent and nothing new */
    for (i = 0; i < k; i++) press(s, &s->ev[i]);
    for (i = 0; i < NV; i++) {
        voice_t *v = &s->v[i];
        if (!v->alive) continue;
        control(s, v);
        for (j = 0; j < n; j++) tmp[j] = 0;
        tx_scrape_add(&v->sc, tmp, n);
        if (s->stick && v->gain) {                           /* slip pulses at the regime's rate */
            int32_t vn = s->p[P_SPEED] * 258;
            int32_t a = (int32_t)(((uint32_t)s->stick * (uint32_t)v->gain) >> 15);
            uint32_t ph = v->ph, inc = v->sinc;
            a = (a * (32768 - ((vn * 22938) >> 15))) >> 15;  /* strongest at low speed */
            for (j = 0; j < n; j++) {
                uint32_t p0 = ph;
                ph += inc;
                if (ph < p0) {                               /* a slip: the force drops */
                    tx_pulse_start(&v->slip, TX_PULSE_SHAPE_RCOS, v->honk ? 44u : 132u, -a * 2, j);
                    inc = v->sinc + (uint32_t)((int32_t)(v->sinc >> 15) * (tx_rand_s15(&s->cloud.rng) / 20));
                }
            }
            v->ph = ph;
        }
        tx_pulses_add(&v->slip, tmp, n);
        for (j = 0; j < n; j++) con[j] += tmp[j];          /* the contact itself: all stones through one low-pass below */
        if (v->nm) tx_svf_bank_add_g(v->body, v->f, v->bq ? v->bq : s->q, v->gin, v->g, v->nm, tmp, buf, n);
        if (!v->gain && tx_scrape_quiet(&v->sc) && !tx_pulses_active(&v->slip) && tx_svf_bank_quiet(v->body, NM)) {
            v->alive = 0; v->em = 0xFF;
        }
    }
    {   /* the contact noise itself (the rub is mostly this), low-passed at ~2 kHz (references: -5..-12 dB / oct) */
        int32_t d = s->dlp;
        for (j = 0; j < n; j++) { d += ((con[j] - d) >> 3) * 2 + ((con[j] - d) >> 5); buf[j] += d; }   /* a 0.28 */
        s->dlp = (d > -16 && d < 16) ? 0 : d;
    }
    tx_declick_run(&s->dc, buf, n);
    tx_vary_block(&s->vy, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = s->dc.v | s->hp.x1 | s->hp.y1 | s->dlp;
        for (i = 0; i < NV; i++) z |= s->v[i].alive;
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void mineral_stats(const void *st, tx_stats_t *o)
{
    const mineral_t *s = (const mineral_t *)st;
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

/* the worst case: 6 stones, grinding fast and hard with stick-slip, every mode in band, quartz (longest ring) */
static const char SCENE_MAX[] =
    "0 particles 6\n0 param STONE 127\n0 param SIZE 0\n0 param PRESSURE 127\n0 param SPEED 127\n0 param ROUGH 127\n"
    "0 param STICK 127\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "1000 param ROUGH 64\n1500 param SPEED 30\n2000 param ROUGH 0\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_mineral = {
    "mineral", "#11 Mineral friction (scrape)", NP, PARAMS, NV, 3, sizeof(mineral_t), SCENE_MAX,
    mineral_init, mineral_key, mineral_param, mineral_render, mineral_stats,
};
