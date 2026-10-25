/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #29 Katydids / grasshoppers: stridulation, tooth by tooth (INSECT + CHORUS cores).
 *
 * A population in the grass (POP core, tx_core_pop.h): while a key is held COUNT singers are out there, the first at
 * the key, the others farther out on an area law, each its own distance, carrier, period, syllables and file; each
 * sings in bouts and pauses of its own (ACTIVITY: a call or two now and then .. runs of calls, short pauses), never
 * faster than its temperature; now and then one leaves after a bout and a new one comes. COUNT 0: a singer per key
 * held (released: it finishes its call). A key while all are there: the one nearest the key in a pause sings now.
 * Past the heroes (PARTICLES) the rest are FIELD's far callers: up to 6 farther out (2 .. 8 x the distance), each
 * its own call period, syllables and bouts, their calls cheap: a katydid's tooth groups or a grasshopper's pulses as
 * single strikes into one shared pair of wing resonances (the air has blurred the strikes within), Panacanthus' calls
 * as tones. Two families:
 *   katydid      (N8, N8c) one decaying oscillation of the wing per tooth strike: regular strikes give a tone,
 *                uneven ones a broadband rasp. The escapement sets one strike per carrier cycle; JITTER is the SD of
 *                the strike interval. Panacanthus: 5.0 kHz, wing Q 16-17, 226 cycles per call (N8c); its upper
 *                components: 14.7 kHz at -9.4 dB kept (20,25 kHz dropped); here a Q 10 two-pole
 *                at 14.7 kHz on the same strikes (its Q: ours). Calls ~1.7 /s at 25 C (ours).
 *   grasshopper  (N23-N25, N30) both hind legs scrape a peg file across a wing vein, the legs out of phase (their
 *                rhythms drift freely: C. biguttulus). No tuned resonator: a click (250-450 us) per peg impact, a
 *                bimodal spectrum (a low peak, 6.57 kHz C. biguttulus / 9.72 kHz C. brunneus; a band at 20-40 kHz,
 *                here its bottom at 18-19.5 kHz, ours), each a Q 6 two-pole (ours: 0.2-0.3 ms clicks).
 *                PEGS = the impact rate within a pulse (N30: >= 1.5 kHz; ~100 pegs, derived 1.5-6 kHz). Pulses
 *                ~8 ms; C. biguttulus syllables of 3 strokes + a 10-15 ms pause (~73 ms, N30); C. brunneus pulses
 *                7-8 ms at ~100 /s (N25), in echemes of 20 (ours).
 *   rhythm       TEMP scales the call and syllable rhythm with Q10 = 2 (ours); the spectrum does not move (N25).
 *   chorus       N19 resetting between callers (CHORUS core): SYNC = s.
 * Keys: carrier / peak a quarter tone per key around the species' (body size); velocity = proximity. */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_radiate.h"
#include "tx_core_cloud.h"
#include "tx_core_insect.h"
#include "tx_core_chorus.h"
#include "tx_core_pop.h"

#define OUT_TRIM 6420                /* output gain, Q12 (-6 dB with note 17's steadier presses and longer syllables; -2.5 dB step 6, longer syllables; +1.6 dB with the spectrogram fixes, session 5; +8.5 dB): integrated (a key held 10 s): -34.5 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +2.3 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -60                 /* (was -130: a singer's level steadier, the chorus references' level sd 0.4-6 dB) */
#define VARY_HI 15
#define VARY_TILT 140
#define VARY_DRIFT 60
#define VARY_SPLIT 0

enum { P_SPECIES, P_JITTER, P_PEGS, P_TEMP, P_COUNT, P_SYNC, P_DIST, P_ACT, P_FIELD, NP };

static const tx_param_t PARAMS[NP] = {
    {"SPECIES", 127, 0, "true katydid rasp 4.5 kHz | katydid Panacanthus 5 kHz (tonal) | grasshopper C. biguttulus 6.6 kHz | C. brunneus 9.7 kHz"},
    {"JITTER", 127, 12, "strike timing SD: 0 .. 50 % of an interval: a pure tone .. a broadband rasp"},
    {"PEGS", 127, 50, "grasshoppers: peg impacts within a pulse 1.5 .. 6 kHz (log; N30 >= 1.5 kHz)"},
    {"TEMP", 127, 76, "temperature 10 .. 35 C: call and syllable rhythm (Q10 2); the spectrum stays"},
    {"COUNT", 64, 0, "singers in the grass while a key is held: 0 = one per key held .. 64 (the first at the key, the others farther out on an area law, each its own distance, carrier and period; past PARTICLES heroes the rest are FIELD's)"},
    {"SYNC", 127, 64, "reset by neighbours s: 0 free .. 0.5 alternate .. 1 synchrony"},
    {"DIST", 127, 40, "distance at velocity 100: 0.3 .. 30 m (log): level 1 / r"},
    {"ACTIVITY", 127, 100, "how much each one sings: a call or two now and then (bouts ~3 s, pauses ~15 s) .. busy (runs of ~5 calls, pauses ~4 s: never a steady beat); never faster: the rhythm is its own"},
    {"FIELD", 127, 0, "the far callers behind: none .. 6 (FIELD^2 / 576 of them, 24 = one, 59 = six; above, louder: more stand for each)"},
};

#define NV 16u
#define NSP 4u
#define NEV 96u                    /* strikes per voice per block, at most */
#define G_REF 1600                 /* strike amplitude at 1 m (Q15; calibrated by render) */
#define TOOTH_HZ 400u              /* the true katydid's tooth groups at 25 C (the references: 390-400 /s) */
#define NFK 6u                     /* far callers (FIELD) */
static const uint16_t FAR_G[4] = {1460, 5950, 6900, 10600};   /* a far caller's tone at 1 m by species (Q15 x 1000 / mm;
                                    * by render: 6 far ones as loud as 6 heroes on the same distance law, -1 dB) */

typedef struct {
    uint16_t f1, f2;               /* the two resonances, Hz */
    uint16_t q1, q2;               /* their Q x 16 */
    int16_t l2;                    /* the upper one's level, Q15 */
    uint8_t katydid, pulses;       /* family; pulses per syllable */
    uint16_t teeth;                /* katydid: strikes per call */
    uint16_t plen, pspc, syl;      /* grasshopper pulse length, pulse spacing, syllable period (ms x 10, at 25 C) */
    uint16_t nsyl;                 /* syllables per call */
    uint16_t call;                 /* call period at 25 C, ms */
} species_t;

static const species_t SPECIES[NSP] = {
    {3700, 5550, 128, 128, 0, 2, 4, 36, 0, 1000, 0, 0, 1600},       /* a true katydid's rasp: 2-5 syllables ("ka-ty-did",
                                                                    * "ka-ty-did-n't") of ~70 ms, 100 ms apart, a call per
                                                                    * 1.6 s; uneven teeth at 3/4 of the carrier, in groups
                                                                    * of ~400 /s (TOOTH_HZ), 30 groups a syllable; the
                                                                    * wing's two resonances 3.3 and 5 kHz (Q 5) at the same
                                                                    * level (the references: syllables 50-70 ms at ~10 /s,
                                                                    * rising in 10-20 ms, the envelope pulsed at 390-400
                                                                    * Hz, peaks at 3.2 and 5 kHz, a dip at 4; agent D,
                                                                    * 2026-10-25: the old one, 280 ms smooth bursts of one
                                                                    * band at 4.5 kHz, was a hiss) */
    {5000, 14700, 264, 160, 11024, 1, 0, 226, 0, 0, 0, 0, 600},    /* Panacanthus (N8c); 14.7 kHz -9.4 dB */
    {6570, 18000, 96, 96, 26000, 0, 3, 0, 80, 200, 730, 24, 3500}, /* C. biguttulus (N25, N30); upper -2 dB (ours) */
    {9720, 19500, 96, 96, 20000, 0, 1, 0, 75, 100, 100, 20, 2000}, /* C. brunneus (N25: 7-8 ms pulses at ~100 /s) */
};

typedef struct {
    tx_ireso_t r1, r2, r3;         /* r3: the true katydid's high band (after the low-pass) */
    int32_t next_q8[2];            /* the next strike of each leg (katydid: one wing), samples from the block start */
    int32_t t;                     /* samples since the call started (at the block start) */
    int32_t gain;                  /* strike amplitude, Q15 */
    int32_t tf;                    /* own period factor, Q15 */
    uint32_t f1;                   /* carrier / low peak at this key, Hz */
    uint8_t in_call, em, note, alive, stop, npul;   /* npul: its syllables per call (katydids) */
    uint16_t tq;                   /* its syllables' length (teeth) re the species', Q8 */
    uint16_t jq;                   /* its teeth's unevenness re JITTER, Q8 */
    uint16_t tr;                   /* its tooth rate (true katydid), Hz at 25 C */
    uint16_t linger;               /* released: blocks it keeps calling (0 = not yet counted) */
    uint8_t sing, keyed, pad4[2];  /* in a bout; bound to its key (COUNT 0) */
    uint32_t bl;                   /* blocks left in the bout or the pause */
} singer_t;

typedef struct {                   /* a far caller (FIELD) */
    tx_itone_t t;                  /* (Panacanthus: its call, a tone) */
    uint32_t inc;                  /* its carrier, phase step (the tone) */
    int32_t amp;                   /* its level at its distance, Q15 */
    int32_t tf;                    /* own period factor, Q15 */
    int32_t nx, cn;                /* samples to its next syllable, to its next call (from the block start) */
    int32_t s0, ni, sln;           /* the syllable now: its start, the next strike (from the block start), its length
                                    * (0: none) */
    uint32_t bl;                   /* blocks left in the bout or the pause */
    uint16_t len, tr;              /* its syllables' length re the species', Q8; its strike interval, samples */
    uint8_t sing, syl, nsyl, on;   /* in a bout; syllables left in the call; per call; drawn (in the field) */
} far_t;

typedef struct {
    tx_cloud_t cloud;
    tx_chorus_t ch;
    singer_t v[NV];
    far_t f[NFK];
    tx_ireso_t fr1, fr2;           /* the far callers' shared wing resonances */
    uint16_t fo[NEV];              /* their strikes this block */
    int16_t fa[NEV];
    int16_t env[TX_ITONE_ENV + 1];
    int16_t env2[TX_ITONE_ENV + 1];   /* a syllable's: a quick rise, a longer fall */
    int32_t hb[TX_BLOCK];          /* the high band, added after the low-pass */
    tx_event_t ev[TX_CLOUD_EV_MAX];
    uint8_t who[TX_CHORUS_MAX];
    uint16_t off[TX_CHORUS_MAX];
    uint16_t eo[2][NEV], mo[NEV];  /* per-leg strike offsets and amplitudes, then merged */
    int16_t ea[2][NEV], ma[NEV];
    uint8_t p[NP], limit, quiet;
    int32_t level, q15, jit, peg_l2, dist_l2;
    uint16_t dropped;
    uint8_t knote, kvel;           /* the last key pressed: the grass's carrier and distance */
    tx_vary_t vy;
    tx_rad_t rad;                  /* the body radiates its lows poorly (tx_core_radiate.h) */
    int32_t lp1, lp2;              /* and its highs: the 7 kHz low-pass */
} katy_t;

static const species_t *sp_of(const katy_t *s) { return &SPECIES[(s->p[P_SPECIES] * NSP) >> 7]; }
static int32_t ms10(const katy_t *s, uint32_t v)        /* ms x 10 -> samples at the temperature */
{
    return (int32_t)(((v * 4410u / 1000u) << 7) / (uint32_t)(s->q15 >> 8));
}
static int32_t call_T(const katy_t *s, int32_t tf)
{
    int32_t T = ms10(s, sp_of(s)->call * 10u);
    return (int32_t)(((int64_t)T * tf) >> 15);
}
/* a syllable of n teeth at a tooth rate (Hz at 25 C), samples at the temperature */
static int32_t syl_len(const katy_t *s, uint32_t n, uint32_t hz)
{
    return ms10(s, n * 10000u / (hz ? hz : 1u));
}
/* a singer's call length (v 0: the species' own) */
static int32_t call_len_v(const katy_t *s, uint32_t f1, const singer_t *v)
{
    const species_t *sp = sp_of(s);
    uint32_t tq = v ? v->tq : 256u, np = v && v->npul ? v->npul : sp->pulses;
    uint32_t teeth = (sp->teeth * tq) >> 8;
    if (sp->katydid == 2u) return ms10(s, (((uint32_t)sp->pspc * tq) >> 8) * (np - 1u)) + syl_len(s, teeth, v ? v->tr : TOOTH_HZ);
    if (sp->katydid) return (int32_t)(teeth * TX_FS / f1);
    return ms10(s, (uint32_t)sp->syl * sp->nsyl);
}
static int32_t call_len(const katy_t *s, uint32_t f1)
{
    const species_t *sp = sp_of(s);
    if (sp->katydid == 2u) return ms10(s, (uint32_t)sp->pspc * (sp->pulses - 1u)) + syl_len(s, sp->teeth, TOOTH_HZ);
    if (sp->katydid) return (int32_t)(sp->teeth * TX_FS / f1);
    return ms10(s, (uint32_t)sp->syl * sp->nsyl);
}

static void set_rhythm(katy_t *s)
{
    int32_t tc10 = 100 + s->p[P_TEMP] * 250 / 127, T, x;
    unsigned i;
    s->q15 = (int32_t)tx_exp2((tc10 - 250) * 655 + (15 << 16));   /* 2^((T - 25) / 10), Q15 */
    T = call_T(s, 32768);
    x = call_len(s, sp_of(s)->f1);
    tx_chorus_set(&s->ch, s->ch.s_q15, 1966, (uint32_t)(T / 4 < 2646 ? T / 4 : 2646), (uint32_t)x);
    for (i = 0; i < NV; i++) if (s->v[i].alive && s->ch.c[i].alive) s->ch.c[i].T = call_T(s, s->v[i].tf);
}

static void set_param(katy_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_SPECIES: case P_TEMP: set_rhythm(s); break;
    case P_JITTER: s->jit = (int32_t)v * 129; break;                     /* SD, Q15: 0 .. 0.5 */
    case P_PEGS: s->peg_l2 = 691208 + (int32_t)(v * 1032u); break;       /* log2(1500 Hz) + v x 2 oct / 127 */
    case P_SYNC: tx_chorus_set(&s->ch, (int32_t)v * 258, s->ch.cv_q15, (uint32_t)s->ch.t_eff, (uint32_t)s->ch.x_len); break;
    case P_DIST: s->dist_l2 = -113738 + (int32_t)(v * 3482u); break;     /* log2(0.3 m) + v log2(100) / 127 */
    default: break;
    }
}

static void katy_init(void *st, uint32_t seed)
{
    katy_t *s = (katy_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    tx_chorus_init(&s->ch);
    tx_itone_env(s->env, 16384);                             /* the strike level across a katydid's call (ours) */
    for (i = 0; i <= TX_ITONE_ENV; i++) {                    /* a syllable: up in its first fifth, held, down in */
        uint32_t q = i < TX_ITONE_ENV / 5u ? (0x40000000u / (TX_ITONE_ENV / 5u)) * i   /* its last third (the */
                   : i < (TX_ITONE_ENV * 2u) / 3u ? 0x40000000u                        /* references: up in */
                   : 0x40000000u + (0x40000000u / (TX_ITONE_ENV - (TX_ITONE_ENV * 2u) / 3u)) * (i - (TX_ITONE_ENV * 2u) / 3u);
        int32_t v = tx_sin_q15(q);                           /* 10-20 ms, the dips between ~-12 dB) */
        s->env2[i] = (int16_t)((v * v) >> 15);
    }
    s->env2[TX_ITONE_ENV] = 0;
    for (i = 0; i < NV; i++) s->v[i].em = 0xFF;
    s->limit = 8;
    s->q15 = 32768;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((katy_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void katy_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    katy_t *s = (katy_t *)st;
    if (on && vel && (!s->p[P_COUNT] || !tx_cloud_held(&s->cloud))) tx_vary_press(&s->vy);   /* (the grass: its night, not
                                                                                   * each key) */
    tx_cloud_key(&s->cloud, note, vel, on);
}

static void katy_param(void *st, uint8_t id, uint8_t val)
{
    katy_t *s = (katy_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NV ? NV : val);
        for (i = s->limit; i < NV; i++) { s->v[i].alive = 0; s->v[i].em = 0xFF; s->v[i].in_call = 0; tx_chorus_stop(&s->ch, i); }
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

static int quiet_v(const singer_t *v) { return !v->alive && tx_ireso_quiet(&v->r1) && tx_ireso_quiet(&v->r2) && tx_ireso_quiet(&v->r3); }

/* the two resonances for a carrier f1 (Hz) */
static void tune(const species_t *sp, singer_t *v, uint32_t f1)
{
    if (f1 > 16000u) f1 = 16000u;
    tx_ireso_set(&v->r1, f1, sp->q1);
    tx_ireso_set(&v->r2, sp->katydid == 1u ? (f1 * 2940u / 1000u > 20000u ? 20000u : f1 * 2940u / 1000u)
                         : sp->katydid ? (f1 * 3u) >> 1 : sp->f2, sp->q2);
    if (sp->katydid == 2u) tx_ireso_set(&v->r3, f1 * 42u / 10u > 18000u ? 18000u : f1 * 42u / 10u, 48u);   /* ~15.5 kHz, Q 3 */
}

/* the bouts (tx_core_pop.h): rare (3 s bouts, 15 s pauses) .. busy (runs of ~5 calls, ~6 s, pauses ~4 s) (ours). The
 * busy end was 1 min bouts: a steady beat (call gap CV 0.17); the user's rule is no regular beat (CV now ~1) */
static const tx_pop_law_t LAW = {590706, 655360, 742875, 622000};

static unsigned act(const katy_t *s) { return s->p[P_ACT]; }

static void bout_start(katy_t *s, unsigned i, uint32_t first)
{
    singer_t *v = &s->v[i];
    int32_t T = call_T(s, v->tf);
    v->sing = 1;
    v->bl = tx_pop_bout(&LAW, &s->cloud.rng, act(s), (uint32_t)T / TX_BLOCK + 2u);
    tx_chorus_place(&s->ch, i, s->ch.x[i], s->ch.y[i]);
    tx_chorus_start(&s->ch, i, T, first);
}

static void bout_end(katy_t *s, unsigned i)
{
    s->v[i].sing = 0;
    s->v[i].bl = tx_pop_pause(&LAW, &s->cloud.rng, act(s));
    tx_chorus_stop(&s->ch, i);
}

static unsigned free_slot(const katy_t *s)
{
    unsigned i;
    for (i = 0; i < s->limit; i++) if (quiet_v(&s->v[i])) return i;
    for (i = 0; i < s->limit; i++) if (!s->v[i].alive) return i;
    for (i = 0; i < s->limit; i++) if (s->v[i].stop) return i;
    return NV;
}

/* a singer in slot vi: near = at the key (the one a press brings, singing now), else farther out on the area law
 * (1 .. 8 x the distance), entering after a pause of its own */
static void individual(katy_t *s, unsigned vi, uint8_t note, uint8_t vel, int near, uint8_t em)
{
    const species_t *sp = sp_of(s);
    singer_t *v = &s->v[vi];
    tx_rng_t *r = &s->cloud.rng;
    int32_t rl2 = s->dist_l2 + ((int32_t)100 - (int32_t)vel) * 1638, rr, ang, f_l2;
    if (!near) rl2 += tx_pop_far_l2(r, 3 << 16);
    if (rl2 > 9 << 16) rl2 = 9 << 16;
    rr = (int32_t)tx_exp2(rl2 + (10 << 16));
    ang = (int32_t)tx_rand(r);
    s->ch.x[vi] = (tx_sin_q15((uint32_t)ang + 0x40000000u) * (rr >> 4)) >> 11;
    s->ch.y[vi] = (tx_sin_q15((uint32_t)ang) * (rr >> 4)) >> 11;
    v->gain = (int32_t)((uint32_t)G_REF * 1000u / (uint32_t)(rr < 300 ? 300 : rr));
    f_l2 = ((int32_t)note - 60) * 2731 + (tx_vary_tone(&s->vy) >> 3);   /* the character (warmer = higher, faster) */
    f_l2 += (tx_gauss_q12(r) * (near ? 930 : 2789)) >> 12;   /* individuals: CV 3 % (ours); the key's own 1 % */
    /* no two singers alike (note 17): its syllables' length (x 1.5, x/ 1.35: the references' katydid syllables 140-320 ms),
     * 2-4 syllables per call ("katy-did", "katy-didn't"), its teeth's unevenness (x/ 1.4: a cleaner or raspier
     * file) */
    v->tq = (uint16_t)(tx_exp2(((tx_gauss_q12(r) * (sp->katydid == 2u ? 6554 : 28000)) >> 12) + (8 << 16) + (sp->katydid == 2u ? 0 : 39000)));
    if (v->tq < 128u) v->tq = 128u;
    if (v->tq > 640u) v->tq = 640u;
    v->npul = sp->katydid == 2u ? (uint8_t)(2u + tx_rand_below(r, 4u)) : 0u;
    v->tr = (uint16_t)((TOOTH_HZ * tx_exp2(((tx_gauss_q12(r) * 8192) >> 12) + (16 << 16))) >> 16);   /* sd 0.25 oct (the
                                                              * draw's SD is 2.0: tx_gauss_q12 reads 8192, not 4096) */
    v->jq = (uint16_t)(tx_exp2(((tx_gauss_q12(r) * 32000) >> 12) + (8 << 16)));
    if (v->jq > 768u) v->jq = 768u;
    v->f1 = (uint32_t)(((uint64_t)sp->f1 * tx_exp2(f_l2 + (16 << 16))) >> 16);
    if (v->f1 > 16000u) v->f1 = 16000u;
    tune(sp, v, v->f1);
    v->r1.g = sp->katydid == 1u ? 256 : sp->katydid ? 450 : 640;   /* clicks / a rasp do not build up: x 2.5-3.5 */
    v->r2.g = sp->katydid == 1u ? (sp->l2 >> 7) * 10 : sp->katydid ? 550 : (sp->l2 * 5) >> 8;   /* the upper level; katydid: x 10 for */
    v->r3.g = sp->katydid == 2u ? 625 : 0;              /* the references' 11-18 kHz band, -10 dB, with the syllables */
                                                                      /* the low mode's build-up (by render) */
    v->tf = 32768 + ((tx_gauss_q12(r) * 1638) >> 12);       /* own period CV 5 % (ours) */
    v->alive = 1; v->stop = 0; v->em = em; v->keyed = (uint8_t)(em != 0xFF); v->note = note; v->in_call = 0; v->linger = 0;
    if (near) bout_start(s, vi, 0);
    else { v->sing = 0; v->bl = 1u + ((tx_pop_pause(&LAW, r, act(s)) * (1u + tx_rand_below(r, 255u))) >> 8); tx_chorus_stop(&s->ch, vi); }
}

static unsigned singing_pop(const katy_t *s)
{
    unsigned i, n = 0;
    for (i = 0; i < s->limit; i++) n += s->v[i].alive && !s->v[i].stop && !s->v[i].keyed;
    return n;
}

/* a key: COUNT 0, a singer of its own (as long as the key is held); else the grass's first one at the key, or, all
 * there, the one nearest the key in a pause sings now */
static void spawn(katy_t *s, const tx_event_t *e)
{
    unsigned n = s->p[P_COUNT], vi, i, bi = NV;
    uint32_t bd = 0xFFFFFFFFu, kf;
    s->knote = e->note; s->kvel = e->vel;
    if (!n || singing_pop(s) < (n < s->limit ? n : s->limit)) {
        vi = free_slot(s);
        if (vi == NV) { s->dropped++; return; }
        {
            unsigned first = singing_pop(s) > 0u;            /* (the field's later ones: each at its own phase) */
            individual(s, vi, e->note, e->vel, 1, n ? 0xFF : e->emitter);
            if (n && first) tx_chorus_start(&s->ch, vi, call_T(s, s->v[vi].tf), tx_rand_below(&s->cloud.rng, 65535u) % (uint32_t)call_T(s, s->v[vi].tf));
        }
        return;
    }
    kf = (uint32_t)(((uint64_t)sp_of(s)->f1 * tx_exp2(((int32_t)e->note - 60) * 2731 + (16 << 16))) >> 16);
    for (i = 0; i < s->limit; i++) {
        const singer_t *v = &s->v[i];
        uint32_t d;
        if (!v->alive || v->stop || v->sing) continue;
        d = v->f1 > kf ? v->f1 - kf : kf - v->f1;
        if (d < bd) { bd = d; bi = i; }
    }
    if (bi < NV) bout_start(s, bi, 0);
    else if ((vi = free_slot(s)) < NV) individual(s, vi, e->note, e->vel, 1, e->emitter);   /* all singing: one more of its own while the key is held */
}

/* per block: the grass kept at COUNT (one comes or goes a block), the bouts and pauses running out; after a bout now
 * and then one leaves (a new one comes, drawn afresh) */
static void field_block(katy_t *s, unsigned n)
{
    unsigned want = s->p[P_COUNT] < s->limit ? s->p[P_COUNT] : s->limit, have = singing_pop(s), i;
    int scene = tx_cloud_held(&s->cloud) > 0;
    if (want && scene && have < want) {
        unsigned vi = free_slot(s);
        if (vi < NV) individual(s, vi, s->knote, s->kvel, 0, 0xFF);
    } else if (have > (want ? want : have)) {
        for (i = s->limit; i-- > 0;) if (s->v[i].alive && !s->v[i].stop && !s->v[i].keyed && !s->v[i].sing) break;
        if (i >= s->limit) for (i = s->limit; i-- > 0;) if (s->v[i].alive && !s->v[i].stop && !s->v[i].keyed) break;
        if (i < s->limit) { s->v[i].stop = 1; s->v[i].sing = 0; tx_chorus_stop(&s->ch, i); }
    }
    for (i = 0; i < s->limit; i++) {
        singer_t *v = &s->v[i];
        if (!v->alive || v->stop || --v->bl) continue;
        if (!v->sing) { bout_start(s, i, tx_rand_below(&s->cloud.rng, n)); continue; }
        bout_end(s, i);
        if (want && !v->keyed && tx_rand_below(&s->cloud.rng, 5u) == 0u) v->stop = 1;   /* it leaves (ours: ~5 bouts) */
    }
}

/* FIELD's far callers: one drawn afresh as it comes (farther out than the heroes: 2 .. 8 x the distance) */
static void far_draw(katy_t *s, far_t *f)
{
    const species_t *sp = sp_of(s);
    tx_rng_t *r = &s->cloud.rng;
    int32_t rl2 = s->dist_l2 + ((int32_t)100 - (int32_t)s->kvel) * 1638 + 65536 + tx_pop_far_l2(r, 2 << 16), rr, f_l2;
    uint32_t hz;
    if (rl2 > 9 << 16) rl2 = 9 << 16;
    rr = (int32_t)tx_exp2(rl2 + (10 << 16));
    f->amp = (int32_t)((uint32_t)FAR_G[(s->p[P_SPECIES] * NSP) >> 7] * 1000u / (uint32_t)(rr < 300 ? 300 : rr));
    f_l2 = ((int32_t)s->knote - 60) * 2731 + ((tx_gauss_q12(r) * 2789) >> 12);   /* the field: CV 3 % */
    hz = (uint32_t)(((uint64_t)sp->f1 * tx_exp2(f_l2 + (16 << 16))) >> 16);
    if (hz > 16000u) hz = 16000u;
    f->inc = hz * 97391u;                                    /* 2^32 / fs per Hz */
    {   /* the shared resonances at the key's carrier (this one's, the last drawn) */
        singer_t t;
        tune(sp, &t, hz);
        s->fr1.a1 = t.r1.a1; s->fr1.a2 = t.r1.a2; s->fr2.a1 = t.r2.a1; s->fr2.a2 = t.r2.a2;
        s->fr1.g = sp->katydid ? 450 : 640;
        s->fr2.g = sp->katydid ? 550 : (sp->l2 * 5) >> 8;
    }
    f->tf = 32768 + ((tx_gauss_q12(r) * 1638) >> 12);
    f->len = (uint16_t)(tx_exp2(((tx_gauss_q12(r) * 14000) >> 12) + (8 << 16)));
    if (f->len < 160u) f->len = 160u;
    if (f->len > 420u) f->len = 420u;
    f->nsyl = sp->katydid == 2u ? (uint8_t)(2u + tx_rand_below(r, 4u)) : sp->katydid ? 1u : (uint8_t)sp->nsyl;
    hz = sp->katydid == 2u ? (TOOTH_HZ * tx_exp2(((tx_gauss_q12(r) * 4096) >> 12) + (16 << 16))) >> 16 : 0u;   /* +-1/4 oct */
    f->tr = (uint16_t)(sp->katydid == 2u ? ms10(s, 10000u / (hz ? hz : 1u)) : ms10(s, sp->pspc));   /* tooth groups | pulses */
    if (f->tr < 8u) f->tr = 8u;
    f->sing = 0; f->syl = 0; f->sln = 0; f->on = 1;
    f->bl = 1u + ((tx_pop_pause(&LAW, r, act(s)) * (1u + tx_rand_below(r, 255u))) >> 8);   /* entering after a pause */
}

/* FIELD's far callers this block: FIELD^2 / 576 of them (at most NFK; above, each stands for more: louder), their
 * bouts on the law of the heroes; each syllable a train of single strikes (a katydid's tooth groups, a grasshopper's
 * pulses, +-20 % apart, under the syllable's envelope) into the shared resonances, or a tone (Panacanthus) */
TX_AUDIO_CODE static void far_block(katy_t *s, int32_t *mix, unsigned n, int held)
{
    const species_t *sp = sp_of(s);
    unsigned fv = s->p[P_FIELD], want = fv ? (fv * fv) / 576u : 0u, i, m = 0, a, b;
    int32_t g = (int32_t)(((int64_t)s->level * (fv > 59u ? (int32_t)fv * 555 : 32768)) >> 15);   /* (59 x 555 = 2^15) */
    int32_t spc, sl, tone = sp->katydid == 1u, nb, bp;
    const int16_t *env = sp->katydid == 2u ? s->env2 : s->env;
    if (fv && !want) want = 1u;
    if (want > NFK) want = NFK;
    if (sp->katydid) { bp = (int32_t)(TX_FS / sp->f1); nb = 6; }   /* (a burst's tail past the block is dropped) */
    else { bp = (int32_t)(TX_FS / tx_exp2(s->peg_l2)); nb = ms10(s, sp->plen) / (bp ? bp : 1); }
    if (bp < 4) bp = 4;
    if (nb < 1) nb = 1;
    if (nb > 8) nb = 8;
    spc = sp->katydid == 2u ? ms10(s, sp->pspc) : ms10(s, sp->syl);
    sl = sp->katydid == 2u ? syl_len(s, sp->teeth, TOOTH_HZ) : tone ? (int32_t)(sp->teeth * TX_FS / sp->f1)
                                                          : (ms10(s, sp->syl) * 3) >> 2;
    for (i = 0; i < NFK; i++) {
        far_t *f = &s->f[i];
        int32_t amp;
        if (!f->on && i < want && held) far_draw(s, f);
        if (f->on && (i >= want || !held)) { f->on = 0; f->sing = 0; }   /* it goes (its call finishes) */
        if (f->on && !--f->bl) {                             /* each sings in bouts */
            f->sing ^= 1u;
            f->bl = f->sing ? tx_pop_bout(&LAW, &s->cloud.rng, act(s), 2u) : tx_pop_pause(&LAW, &s->cloud.rng, act(s));
            if (f->sing) f->cn = (int32_t)tx_rand_below(&s->cloud.rng, n);
        }
        amp = (int32_t)(((int64_t)f->amp * g) >> 15);
        for (;;) {
            if (f->sln) {                                    /* in a syllable: its strikes */
                int32_t pos;
                if (f->ni >= (int32_t)n) break;
                pos = f->ni - f->s0;
                if (pos >= f->sln) { f->sln = 0; continue; }
                {   /* a tooth group or a pulse: a short burst of strikes at the carrier (katydid) or the peg rate
                     * (grasshopper), struck hard first, softer after (as the heroes' groups) */
                    int32_t e = env[(uint32_t)(((uint32_t)pos << 6) / (uint32_t)f->sln)], o = f->ni < 0 ? 0 : f->ni, k;
                    int32_t x = (int32_t)(((int64_t)amp * e) >> 15);
                    x = (int32_t)(((int64_t)x * (24576 + (int32_t)tx_rand_below(&s->cloud.rng, 16384u))) >> 15);
                    for (k = 0; k < nb && o < (int32_t)n && m < NEV; k++) {
                        int32_t y = (x * (nb - k)) / nb;
                        s->fo[m] = (uint16_t)o; s->fa[m] = (int16_t)(y > 32767 ? 32767 : y); m++;
                        o += bp + (int32_t)(((int64_t)tx_gauss_q12(&s->cloud.rng) * bp) >> 16);   /* +-10 % */
                    }
                }
                f->ni += (int32_t)f->tr + (int32_t)(((int64_t)tx_gauss_q12(&s->cloud.rng) * f->tr) >> 15);   /* +-20 % */
                continue;
            }
            if (f->syl && f->nx < (int32_t)n) {              /* a syllable */
                int32_t ln = (sl * f->len) >> 8;
                if (ln < 64) ln = 64;
                if (tone) {
                    int32_t x = (int32_t)(((int64_t)amp * (24576 + (int32_t)tx_rand_below(&s->cloud.rng, 16384u))) >> 15);
                    tx_itone_start(&f->t, f->inc, 0, (uint32_t)ln, x > 32767 ? 32767 : x, 0, 0, (unsigned)(f->nx < 0 ? 0 : f->nx));
                } else { f->s0 = f->nx; f->ni = f->nx; f->sln = ln; }
                f->syl--;
                f->nx += (spc * f->len) >> 8;
                continue;
            }
            if (!f->syl && f->on && f->sing && f->cn < (int32_t)n) {   /* a call: the period +-5 % */
                int32_t T = call_T(s, f->tf);
                f->syl = f->nsyl; f->nx = f->cn;
                f->cn += T + (int32_t)(((int64_t)tx_gauss_q12(&s->cloud.rng) * T) >> 17);
                continue;
            }
            break;
        }
        if (f->sln) { f->s0 -= (int32_t)n; f->ni -= (int32_t)n; }
        if (f->syl) f->nx -= (int32_t)n;
        if (f->on && f->sing) f->cn -= (int32_t)n;
        if (tone) tx_itone_add(&f->t, env, mix, n);
    }
    for (a = 1; a < m; a++) {                                /* the strikes in order (a few dozen) */
        uint16_t o = s->fo[a]; int16_t x = s->fa[a];
        for (b = a; b && s->fo[b - 1] > o; b--) { s->fo[b] = s->fo[b - 1]; s->fa[b] = s->fa[b - 1]; }
        s->fo[b] = o; s->fa[b] = x;
    }
    if (m || !tx_ireso_quiet(&s->fr1) || !tx_ireso_quiet(&s->fr2)) tx_ireso_add2(&s->fr1, &s->fr2, s->fo, s->fa, m, mix, n);
}

/* the strike interval: the period x (1 + JITTER x a normal draw), at least 1/4 of it */
static int32_t interval_q8(katy_t *s, int32_t per_q8, uint32_t jq)
{
    int32_t j = sp_of(s)->katydid == 2u && s->jit < 6554 ? 6554 : s->jit;   /* a rasp: uneven teeth (SD >= 20 %) */
    j = (int32_t)(((int64_t)j * jq) >> 8);
    int32_t d = (int32_t)(((int64_t)tx_gauss_q12(&s->cloud.rng) * j) >> 15);   /* Q12 */
    int32_t q = per_q8 + (int32_t)(((int64_t)per_q8 * d) >> 12);
    return q < (per_q8 >> 2) ? (per_q8 >> 2) : q;
}

/* grasshopper leg gate: the pulse envelope (Q15) at time t (samples since the call start) for a leg shifted by sh */
static int32_t leg_env(const katy_t *s, const species_t *sp, int32_t t)
{
    int32_t S = ms10(s, sp->syl), sp_ = ms10(s, sp->pspc), pl = ms10(s, sp->plen), ts, k, u;
    if (t < 0) return 0;
    ts = t % S;
    k = ts / sp_;
    if (k >= sp->pulses) return 0;
    u = ts - k * sp_;
    if (u >= pl) return 0;
    return tx_sin_q15((uint32_t)(((uint32_t)u << 15) / (uint32_t)pl) << 16) ;   /* a half-sine pulse */
}

static unsigned strikes(katy_t *s, singer_t *v, unsigned n)
{
    const species_t *sp = sp_of(s);
    int32_t g = (int32_t)(((int64_t)v->gain * s->level) >> 15);
    int32_t len = call_len_v(s, v->f1, v);
    unsigned legs = sp->katydid ? 1u : 2u, L, cnt[2] = {0, 0}, a, b, m;
    int32_t per_q8 = sp->katydid ? (int32_t)((TX_FS << 8) / v->f1) : (int32_t)((TX_FS << 8) / tx_exp2(s->peg_l2));
    int32_t pg = sp->katydid == 2u ? ms10(s, (10000u << 8) / v->tr) >> 8 : 1;   /* the tooth groups' period (the
                                                              * strokes as fast as the rhythm) */
    for (L = 0; L < legs; L++) {
        while (v->in_call && (v->next_q8[L] >> 8) < (int32_t)n && cnt[L] < NEV) {
            int32_t o = v->next_q8[L] >> 8, ts = v->t + o, e, amp;
            if (ts >= len) { if (L + 1 == legs) v->in_call = 0; break; }
            if (sp->katydid == 2u) {                         /* syllables: each its own sweep of the file */
                int32_t spc = ms10(s, ((uint32_t)sp->pspc * v->tq) >> 8), sl = syl_len(s, (sp->teeth * v->tq) >> 8, v->tr), u = ts % spc;
                uint32_t pos = u < sl ? (uint32_t)(((uint32_t)u << 6) / (uint32_t)sl) : 64u;
                e = s->env2[pos > 64u ? 64u : pos];
                if (ts < len - sl && e < 8192) e = 8192;   /* between syllables the file still sounds, -12 dB (the
                                                            * references' dips: the wing's opening stroke; ours) */
                {   /* the teeth come in groups ~400 /s (the wing's own beat: the references' envelope pulses at
                     * 390-400 Hz, 10-15 dB deep): each group struck hard at its start, softer after */
                    int32_t w = pg - ts % pg, q = (w << 8) / pg;   /* 256 .. 0 through the group */
                    e = (int32_t)(((int64_t)e * (6554 + ((q * q * 26214) >> 16))) >> 15);
                }
            } else if (sp->katydid) {
                uint32_t pos = (uint32_t)(((uint32_t)ts << 6) / (uint32_t)len);   /* along the call: the file */
                e = s->env[pos > 64u ? 64u : pos];
            } else
                e = leg_env(s, sp, L ? ts + (ms10(s, sp->syl) >> 1) + (ts >> 6) : ts);   /* leg 2 drifts (ours) */
            amp = (int32_t)(((int64_t)g * e) >> 15);
            if (amp) { s->eo[L][cnt[L]] = (uint16_t)o; s->ea[L][cnt[L]] = (int16_t)(amp > 32767 ? 32767 : amp); cnt[L]++; }
            v->next_q8[L] += interval_q8(s, per_q8, v->jq);
        }
        v->next_q8[L] -= (int32_t)(n << 8);
        if (v->next_q8[L] < -(1 << 20)) v->next_q8[L] = 0;
    }
    for (a = b = m = 0; a < cnt[0] || b < cnt[1]; m++) {       /* merge the legs by offset */
        if (b >= cnt[1] || (a < cnt[0] && s->eo[0][a] <= s->eo[1][b])) { s->mo[m] = s->eo[0][a]; s->ma[m] = s->ea[0][a]; a++; }
        else { s->mo[m] = s->eo[1][b]; s->ma[m] = s->ea[1][b]; b++; }
    }
    return m;
}

TX_AUDIO_CODE static void katy_render_raw(void *st, int32_t *mix, unsigned n)
{
    katy_t *s = (katy_t *)st;
    unsigned k, i, ncall;
    int held_any = 0;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    for (i = 0; i < k; i++) spawn(s, &s->ev[i]);
    field_block(s, n);
    ncall = tx_chorus_block(&s->ch, &s->cloud.rng, n, s->who, s->off, TX_CHORUS_MAX);
    for (i = 0; i < ncall; i++) {
        singer_t *v = &s->v[s->who[i]];
        if (!v->alive || v->stop) continue;
        v->in_call = 1;
        if (v->npul && (tx_rand(&s->cloud.rng) >> 30) == 0u) {   /* now and then a syllable more or less ("katy-did", */
            int32_t np = (int32_t)v->npul + (int32_t)(tx_rand(&s->cloud.rng) >> 31) * 2 - 1;   /* "katy-didn't") */
            v->npul = (uint8_t)(np < 2 ? 3 : np > 5 ? 4 : np);
        }
        tune(sp_of(s), v, tx_vary_scale(v->f1, tx_vary_drift(&s->vy) >> 3));   /* the night warms, cools */
        v->t = -(int32_t)s->off[i];
        v->next_q8[0] = v->next_q8[1] = (int32_t)s->off[i] << 8;
    }
    for (i = 0; i < NV; i++) {
        singer_t *v = &s->v[i];
        unsigned m = 0;
        int held;
        if (quiet_v(v)) continue;
        held = v->alive && (v->keyed ? v->em < TX_CLOUD_EMITTERS && s->cloud.em[v->em].held && s->cloud.em[v->em].note == v->note
                                     : s->p[P_COUNT] && tx_cloud_held(&s->cloud) > 0);
        if (v->alive && !held && !v->stop) {                 /* released: each singer goes on for 0-3 s, then stops */
            if (!v->linger) v->linger = (uint16_t)(2u + tx_rand_below(&s->cloud.rng, 516u));   /* after its call */
            else if (--v->linger == 1u) { v->stop = 1; tx_chorus_stop(&s->ch, i); }            /* (session 5) */
        }
        held_any |= v->alive && !v->stop;
        if (v->in_call) m = strikes(s, v, n);
        if (v->in_call) v->t += (int32_t)n;
        tx_ireso_add2(&v->r1, &v->r2, s->mo, s->ma, m, mix, n);
        if (v->r3.g) tx_ireso_add(&v->r3, s->mo, s->ma, m, s->hb, n);
        if (v->stop && !v->in_call) { v->alive = 0; v->stop = 0; v->em = 0xFF; v->sing = 0; }
    }
    far_block(s, mix, n, held_any);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = 0;
        for (i = 0; i < NFK; i++) z |= s->f[i].t.on | s->f[i].syl | s->f[i].on | (s->f[i].sln != 0);
        z |= !tx_ireso_quiet(&s->fr1) || !tx_ireso_quiet(&s->fr2);
        for (i = 0; i < NV; i++) z |= !quiet_v(&s->v[i]);
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

/* the engine into its own block, then trimmed into the mix (OUT_TRIM) */
TX_AUDIO_CODE static void katy_render(void *st, int32_t *mix, unsigned n)
{
    int32_t b[TX_BLOCK];
    unsigned j;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) b[j] = ((katy_t *)st)->hb[j] = 0;
    katy_render_raw(st, b, n);
    tx_rad_hp(&((katy_t *)st)->rad, b, n, 3);
    {   /* above ~7 kHz the wings radiate less and the grass and the air take the rest: two one-poles (step 6: the
         * references' katydid band stops near 7-8 kHz, ours ran flat to 20 kHz) */
        katy_t *k = (katy_t *)st;
        int32_t a = k->lp1, c = k->lp2;
        unsigned j;
        for (j = 0; j < n; j++) {
            int32_t h = b[j] - a; a += (h * 5 + (h > 0 ? 7 : 0)) >> 3;
            h = a - c; c += (h * 5 + (h > 0 ? 7 : 0)) >> 3;
            b[j] = c + k->hb[j];
        }
        k->lp1 = a; k->lp2 = c;
        if (a | c) k->quiet = 0;
    }   /* a katydid or grasshopper radiates under ~1 kHz poorly: -12 dB / oct */
    if (!tx_rad_quiet(&((katy_t *)st)->rad)) ((katy_t *)st)->quiet = 0;
    tx_vary_block(&((katy_t *)st)->vy, b, n);
    if (((katy_t *)st)->vy.lp) ((katy_t *)st)->quiet = 0;          /* the split still settling */
    tx_trim_add(b, mix, n, OUT_TRIM);
}

static void katy_stats(const void *st, tx_stats_t *o)
{
    const katy_t *s = (const katy_t *)st;
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

/* the worst case: 16 katydids calling in synchrony (every resonator rings at once), jittered, close, hot, the far
 * callers all singing */
static const char SCENE_MAX[] =
    "0 particles 16\n0 param SPECIES 40\n0 param PEGS 127\n0 param JITTER 127\n0 param COUNT 16\n0 param ACTIVITY 127\n0 param SYNC 127\n"
    "0 param DIST 0\n0 param TEMP 127\n0 param FIELD 127\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_katydids = {
    "katydids", "#29 Katydids / grasshoppers", NP, PARAMS, NV, 8, sizeof(katy_t), SCENE_MAX,
    katy_init, katy_key, katy_param, katy_render, katy_stats,
};
