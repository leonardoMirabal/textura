/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #27 Crickets: a field of singing crickets (INSECT + CHORUS cores).
 *
 * A field of individuals (POP core, tx_core_pop.h): while a key is held COUNT crickets are out there, the first at the
 * key, the others farther out on an area law, each its own distance, carrier, period, pulse length, glide and H2; each
 * sings in bouts and pauses of its own (ACTIVITY: a few chirps now and then .. runs of chirps, short pauses), never
 * faster than its temperature; now and then one leaves after a bout and a new one comes. COUNT 0: a cricket per key held (released:
 * it finishes its chirp). A key while the field is full: the one nearest the key in a pause sings now. Physics:
 *   song      the clockwork escapement (N1, N2, N10): one file tooth per cycle of the harp resonance, so each pulse
 *             (one wing closing) is a near-pure tone at the carrier, its pitch falling 5-15 % over the pulse, the
 *             envelope peaking ~2/3 through, a weak phase-locked H2 (-20 .. -30 dB). Field cricket 4.83 kHz,
 *             ~200 cycles per pulse, H2 -25 dB (N1, N10); Eugryllodes 3.2 kHz, H2 -17 dB (N1); mole cricket
 *             G. vineae 3.51 kHz, H2 -26, H3 -35 dB (N28), a trill; Dolbear's tree cricket (ours: 2.9 kHz, 7 pulses).
 *             Pulses per chirp and the pulse spacing: ours, inside the note's ranges.
 *   rhythm    Dolbear (N7): chirps per minute N = 40 + 7.2 (T_C - 10) (148 /min at 25 C); the pulses scale with the
 *             same ratio (Q10 ~ 2 for insect rhythms, ours).
 *   chorus    N19's inhibitory resetting (CHORUS core) among the ones singing: callers hearing a neighbour move their next call to
 *             last + s d + T + eps; SYNC = s (0: free, ~0.5: alternation, 1: synchrony); CV(eps) 0.06, t 60 ms.
 *   distance  level 1 / r (each cricket placed on the plane; the propagation delays feed the chorus).
 * Keys: carrier per caller, a quarter tone per key around the species' carrier at C4 (a "tuned field"); velocity =
 * proximity (+-1 octave of distance). FIELD: the distant chorus: 3 far callers (each its own carrier +-3 % and
 * rhythm +-10 %), their pulses pure (the air and the grass take the harmonics), FIELD loud (sounds while a key is held;
 * it was a band of noise: the user, 2026-10-25, no hiss under the engines). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_radiate.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_insect.h"
#include "tx_core_chorus.h"
#include "tx_core_pop.h"

#define OUT_TRIM 11152               /* output gain, Q12 (+1.6 dB with the spectrogram fixes, session 5; +5.8 dB): integrated (a key held 10 s): -31.8 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +1.3 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 160
#define VARY_DRIFT 60
#define VARY_SPLIT 0

enum { P_TEMP, P_SPECIES, P_COUNT, P_SYNC, P_DIST, P_FIELD, P_ACT, P_SIZE, NP };

static const tx_param_t PARAMS[NP] = {
    {"TEMP", 127, 76, TX_TEXT("air temperature 10 .. 35 C: chirp rate 40 .. 220 /min (Dolbear), pulses as fast")},
    {"SPECIES", 127, 0, TX_TEXT("field cricket 4.8 kHz | Eugryllodes 3.2 kHz | tree cricket 2.9 kHz | mole cricket 3.5 kHz trill | ground cricket 7.4 kHz fast trill | low cricket 2 kHz")},
    {"COUNT", 32, 3, TX_TEXT("crickets in the field while a key is held: 0 = one per key held .. 32 (the first at the key, the others farther out on an area law, each its own distance, carrier and period)")},
    {"SYNC", 127, 64, TX_TEXT("reset by neighbours s: 0 free .. 0.5 alternate .. 1 synchrony")},
    {"DIST", 127, 50, TX_TEXT("distance at velocity 100: 0.3 .. 30 m (log): level 1 / r; delays between callers")},
    {"FIELD", 127, 0, TX_TEXT("the distant chorus behind (a band at the carrier): off .. loud")},
    {"ACTIVITY", 127, 100, TX_TEXT("how much each one sings: a few chirps now and then (bouts ~1.5 s, pauses ~15 s) .. busy (runs of ~6 chirps, pauses ~1 s: never a steady beat); never faster: the rhythm is its own")},
    {"SIZE", 127, 64, TX_TEXT("body size: the carrier -0.5 .. +0.5 oct (bigger: lower); the rhythm stays")},
};

#define NV 32u
#define NSP 6u
#define G_REF 4900                 /* gain at 1 m, Q15 (calibrated by render: the chord stays under full scale) */
#define FADE 4u
#define FAR_SH 12                  /* the far callers' pulse level: FIELD x LEVEL x 4.9 >> FAR_SH (Q15; by render: the
                                    * FIELD at its old loudness) */
#define NFAR 6u                    /* far callers (3 until the FM-1's second core and cache layout gave the room; the
                                    * chorus's power as with 3: FAR_G) */
#define FAR_G 28                   /* their gain: 39 x sqrt(3 / NFAR) (the FIELD's level as with 3 callers) */

typedef struct {
    uint16_t f0;                   /* carrier, Hz */
    int16_t glide, h2, h3;         /* Q15 */
    uint8_t pulses;                /* per chirp; 0 = a trill (each call one pulse) */
    uint16_t len, per;             /* pulse length and spacing at 25 C, samples */
} species_t;

static const species_t SPECIES[NSP] = {
    {4830, 1311, 1843, 0, 3, 794, 1588},    /* field cricket: H2 -25 dB (N1, N10); 18 ms pulses 36 ms apart, 4 % fall */
    {3200, 1638, 4626, 0, 4, 794, 1323},    /* Eugryllodes: H2 -17 dB (N1) */
    {2900, 1638, 1843, 0, 7, 529, 970},     /* Dolbear's tree cricket (ours) */
    {3510, 1638, 1642, 583, 0, 617, 882},   /* mole cricket G. vineae: H2 -26, H3 -35 dB (N28), a trill */
    {7400, 1311, 1300, 0, 0, 1500, 3392},   /* a small ground cricket's fast trill (ours, from a close single-cricket
                                             * recording: 7.4 kHz, 13 pulses/s, steady) */
    {2000, 1638, 2600, 0, 2, 4000, 2000},   /* a low cricket's chirps (ours, from a recording: 2 kHz, ~2 chirps/s, each
                                             * chirp one sound: 2 long pulses overlapping, 90 ms each) */
};

typedef struct {
    tx_itone_t tone[2];
    uint32_t inc;                  /* carrier increment */
    int32_t gain;                  /* Q15 */
    int32_t tf;                    /* own period factor, Q15 */
    int32_t lf, gl, h2f;           /* the individual: pulse length, glide and H2 factors (Q15) */
    int8_t pk, pad3[3];            /* and its pulses per chirp re the species' */
    int32_t pc;                    /* samples to the next pulse of the chirp (from the block start) */
    uint8_t pl, slot, em, note, alive, stop;
    uint16_t linger;               /* released: blocks it keeps calling (0 = not yet counted) */
    uint8_t sing, keyed, pad4[2];  /* in a bout; bound to its key (COUNT 0) */
    uint32_t bl;                   /* blocks left in the bout or the pause */
} cricket_t;

typedef struct {
    tx_cloud_t cloud;
    tx_chorus_t ch;
    tx_itone_t far[8];             /* the FIELD's far callers' pulses */
    uint32_t finc[8];              /* their carriers */
    int32_t fpc[8];                /* samples to their next pulse */
    uint8_t fpl[8];                /* pulses left in their chirps */
    uint8_t fsing[8];              /* in a bout */
    uint32_t fbl[8];               /* blocks left in the bout or the pause */
    cricket_t c[NV];
    int16_t env[TX_ITONE_ENV + 1];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    uint8_t who[TX_CHORUS_MAX];
    uint16_t off[TX_CHORUS_MAX];
    uint32_t vph[8], vinc[8];      /* the FIELD's virtual callers */
    uint8_t p[NP], limit, quiet;
    int32_t level, q15;            /* rate ratio to 25 C, Q15 */
    int32_t sz_l2;                 /* SIZE's carrier shift, log2 Q16 */
    int32_t chirp_T;               /* the chirp period at TEMP, samples */
    int32_t dist_l2;
    uint16_t dropped;
    uint8_t knote, kvel;           /* the last key pressed: the field's carrier and distance */
    tx_vary_t vy;
    tx_rad_t rad;                  /* the body radiates its lows poorly (tx_core_radiate.h) */
} crickets_t;

static const species_t *sp_of(const crickets_t *s) { return &SPECIES[(s->p[P_SPECIES] * NSP) >> 7]; }

/* call period, pulse length and spacing at the current temperature for a caller of factor tf */
static int32_t call_T(const crickets_t *s, int32_t tf)
{
    const species_t *sp = sp_of(s);
    int32_t T = sp->pulses ? s->chirp_T : (int32_t)(((uint32_t)sp->per << 15) / (uint32_t)s->q15);
    return (int32_t)(((int64_t)T * tf) >> 15);
}
static uint32_t scaled(const crickets_t *s, uint32_t v) { return (v << 15) / (uint32_t)s->q15; }

static void set_timing(crickets_t *s)
{
    const species_t *sp = sp_of(s);
    int32_t tc10 = 100 + s->p[P_TEMP] * 250 / 127;          /* tenths of a degree C: 10.0 .. 35.0 */
    int32_t n10 = 400 + 72 * (tc10 - 100) / 10;              /* Dolbear, chirps per 10 min */
    uint32_t chirp_len, i;
    s->q15 = n10 * 32768 / 1480;
    s->chirp_T = (int32_t)(26460000u / (uint32_t)n10);       /* 44100 x 600 / (10 N) */
    chirp_len = sp->pulses ? scaled(s, (uint32_t)sp->per * (sp->pulses - 1u) + sp->len) : scaled(s, sp->len);
    tx_chorus_set(&s->ch, s->ch.s_q15, 1966, 2646u, chirp_len);
    if (!sp->pulses) {                                       /* a trill: the effector delay within the pulse period */
        uint32_t T = scaled(s, sp->per);
        tx_chorus_set(&s->ch, s->ch.s_q15, 1966, T >> 2, chirp_len);
    }
    for (i = 0; i < NV; i++)
        if (s->c[i].alive && s->ch.c[i].alive) s->ch.c[i].T = call_T(s, s->c[i].tf);
    for (i = 0; i < 8; i++) s->vinc[i] = (uint32_t)(0xFFFFFFFFu / (uint32_t)(call_T(s, 32768) / 256 + 1));   /* per block */
}

static void set_param(crickets_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_TEMP: set_timing(s); break;
    case P_SPECIES: {
        unsigned i;
        set_timing(s);
        for (i = 0; i < 8; i++)                              /* the far callers' carriers: CV 6 % (N1) */
            s->finc[i] = (uint32_t)(((uint64_t)sp_of(s)->f0 * 97391u * tx_exp2(s->sz_l2 + ((tx_gauss_q12(&s->cloud.rng) * 2789) >> 12) + (16 << 16))) >> 16);
        break;
    }
    case P_SYNC: tx_chorus_set(&s->ch, (int32_t)v * 258, s->ch.cv_q15, (uint32_t)s->ch.t_eff, (uint32_t)s->ch.x_len); break;
    case P_DIST: s->dist_l2 = -113738 + (int32_t)(v * 3482u); break;   /* log2(0.3 m) + v log2(100) / 127, Q16 */
    case P_SIZE: {                                           /* +-0.5 oct; the callers there now (and the far ones)
                                                              * retuned with it */
        int32_t l2 = (64 - (int32_t)v) * 512, d = l2 - s->sz_l2;
        uint32_t k = tx_exp2(d + (16 << 16));
        unsigned i;
        s->sz_l2 = l2;
        if (d) {
            for (i = 0; i < NV; i++) if (s->c[i].alive) s->c[i].inc = (uint32_t)(((uint64_t)s->c[i].inc * k) >> 16);
            for (i = 0; i < 8; i++) s->finc[i] = (uint32_t)(((uint64_t)s->finc[i] * k) >> 16);
        }
        break;
    }
    default: break;
    }
}

static void crickets_init(void *st, uint32_t seed)
{
    crickets_t *s = (crickets_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    tx_chorus_init(&s->ch);
    tx_itone_env(s->env, 18022);                             /* the peak 0.55 through the pulse (recordings 0.41-0.61) */
    for (i = 0; i < NV; i++) s->c[i].em = 0xFF;
    for (i = 0; i < 8; i++) { s->vph[i] = tx_rand(&s->cloud.rng); s->fbl[i] = 1u + tx_rand_below(&s->cloud.rng, 400u); }
    s->limit = 16;
    s->q15 = 32768;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((crickets_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void crickets_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    crickets_t *s = (crickets_t *)st;
    if (on && vel && (!s->p[P_COUNT] || !tx_cloud_held(&s->cloud))) tx_vary_press(&s->vy);   /* (the field: its night, not
                                                                                       * each key) */
    tx_cloud_key(&s->cloud, note, vel, on);
}

static void crickets_param(void *st, uint8_t id, uint8_t val)
{
    crickets_t *s = (crickets_t *)st;
    if (tx_cloud_keyat(&s->cloud, id, val)) return;          /* (a press's own sample: TX_PID_KEYAT) */
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NV ? NV : val);
        for (i = s->limit; i < NV; i++) {                    /* cut: the pulse sounding ends, no new call */
            s->c[i].alive = 0; s->c[i].em = 0xFF; s->c[i].pl = 0;
            tx_chorus_stop(&s->ch, i);
        }
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* the bouts (tx_core_pop.h): rare (1.5 s bouts, 15 s pauses) .. busy (2 s, ~6 chirps, then 1 s); ours, from the
 * references' single callers (a few chirps now and then when cold or unsettled). The busy end was 1 min bouts: a
 * steady beat (call gap CV 0.35); the user's rule is no regular beat, so a busy cricket sings in runs of chirps with
 * short pauses (CV ~1) */
static const tx_pop_law_t LAW = {525170, 552600, 742875, 486834};

static unsigned act(const crickets_t *s) { return s->p[P_ACT]; }

/* a bout begins: the cricket joins the chorus (its delays to the ones singing now), its first call first samples on */
static void bout_start(crickets_t *s, unsigned i, uint32_t first)
{
    cricket_t *c = &s->c[i];
    int32_t T = call_T(s, c->tf);
    c->sing = 1;
    c->bl = tx_pop_bout(&LAW, &s->cloud.rng, act(s), (uint32_t)T / TX_BLOCK + 2u);
    tx_chorus_place(&s->ch, i, s->ch.x[i], s->ch.y[i]);
    tx_chorus_start(&s->ch, i, T, first);
}

static void bout_end(crickets_t *s, unsigned i)
{
    s->c[i].sing = 0;
    s->c[i].bl = tx_pop_pause(&LAW, &s->cloud.rng, act(s));
    tx_chorus_stop(&s->ch, i);
}

static unsigned free_slot(const crickets_t *s)
{
    unsigned i;
    for (i = 0; i < s->limit; i++) if (!s->c[i].alive && !s->c[i].tone[0].on && !s->c[i].tone[1].on) return i;
    for (i = 0; i < s->limit; i++) if (!s->c[i].alive) return i;
    for (i = 0; i < s->limit; i++) if (s->c[i].stop) return i;
    return NV;
}

/* an individual in slot ci: near = at the key (1: the one a press brings, singing now; 2: entering after a pause of
 * its own), else farther out on the area law (1 .. 8 x the distance), entering after a pause of its own */
static void individual(crickets_t *s, unsigned ci, uint8_t note, uint8_t vel, int near, uint8_t em)
{
    const species_t *sp = sp_of(s);
    cricket_t *c = &s->c[ci];
    tx_rng_t *r = &s->cloud.rng;
    int32_t rl2 = s->dist_l2 + ((int32_t)100 - (int32_t)vel) * 1638, rr, ang, f_l2;   /* velocity: +-1 oct / 40 */
    if (!near) rl2 += tx_pop_far_l2(r, 3 << 16);
    if (rl2 > 9 << 16) rl2 = 9 << 16;                        /* 512 m at most */
    rr = (int32_t)tx_exp2(rl2 + (10 << 16));                 /* mm */
    ang = (int32_t)tx_rand(r);
    s->ch.x[ci] = (tx_sin_q15((uint32_t)ang + 0x40000000u) * (rr >> 4)) >> 11;
    s->ch.y[ci] = (tx_sin_q15((uint32_t)ang) * (rr >> 4)) >> 11;
    c->gain = (int32_t)((uint32_t)G_REF * 1000u / (uint32_t)(rr < 300 ? 300 : rr));
    f_l2 = ((int32_t)note - 60) * 2731 + (tx_vary_tone(&s->vy) >> 3) + s->sz_l2;   /* a quarter tone per key (2^(1/24), Q16); the character (warmer = higher, faster); the body (SIZE) */
    f_l2 += (tx_gauss_q12(r) * (near ? 930 : 2789)) >> 12;   /* each its own carrier: the field CV 6 % (N1: 4.8-5.3 kHz),
                                                              * the key's own 2 % (it stays on its key) */
    /* no two crickets alike (note 17): its own pulse length (CV 20 %), glide (CV 60 %), H2 (sd 6 dB), and a
     * pulse more or fewer per chirp (field crickets 3-5) */
    c->lf = 32768 + ((tx_gauss_q12(r) * 3277) >> 12);
    c->gl = 32768 + ((tx_gauss_q12(r) * 9830) >> 12);
    c->h2f = (int32_t)tx_exp2(((tx_gauss_q12(r) * 32660) >> 12) + (15 << 16));
    if (c->gl < 9830) c->gl = 9830;                          /* (a glide always falls) */
    if (c->h2f > 46341) c->h2f = 46341;                      /* (H2 at most +3 dB: the chorus's peaks) */
    if (c->lf > 37683) c->lf = 37683;                        /* (pulses at most 15 % longer: the same) */
    c->pk = (int8_t)((int32_t)tx_rand_below(r, 3u) - 1);
    c->inc = (uint32_t)(((uint64_t)sp->f0 * 97391u * tx_exp2(f_l2 + (16 << 16))) >> 16);
    c->tf = 32768 + ((tx_gauss_q12(r) * 1638) >> 12);        /* own period: CV 10 % (ours) */
    c->alive = 1; c->stop = 0; c->em = em; c->keyed = (uint8_t)(em != 0xFF); c->note = note; c->pl = 0; c->linger = 0;
    if (near == 1) bout_start(s, ci, 0);
    else { c->sing = 0; c->bl = 1u + ((tx_pop_pause(&LAW, r, act(s)) * (1u + tx_rand_below(r, 255u))) >> 8); tx_chorus_stop(&s->ch, ci); }
}

static unsigned singing_pop(const crickets_t *s)           /* the individuals in the field now (not leaving) */
{
    unsigned i, n = 0;
    for (i = 0; i < s->limit; i++) n += s->c[i].alive && !s->c[i].stop && !s->c[i].keyed;
    return n;
}

/* a key: COUNT 0, a cricket of its own (as long as the key is held); else the field's first one at the key, or, the
 * field full, the one nearest the key in a pause sings now */
static void spawn(crickets_t *s, const tx_event_t *e)
{
    unsigned n = s->p[P_COUNT], ci, i, bi = NV;
    uint32_t bd = 0xFFFFFFFFu, ki;
    s->knote = e->note; s->kvel = e->vel;
    if (!n || singing_pop(s) < (n < s->limit ? n : s->limit)) {
        ci = free_slot(s);
        if (ci == NV) { s->dropped++; return; }
        individual(s, ci, e->note, e->vel, n ? 2 : 1, n ? 0xFF : e->emitter);   /* a field: the one at the key enters
                                                             * after a pause of its own, as the others (singing at once,
                                                             * the start was louder than what follows) */
        return;
    }
    ki = (uint32_t)(((uint64_t)sp_of(s)->f0 * 97391u * tx_exp2(((int32_t)e->note - 60) * 2731 + s->sz_l2 + (16 << 16))) >> 16);
    for (i = 0; i < s->limit; i++) {
        const cricket_t *c = &s->c[i];
        uint32_t d;
        if (!c->alive || c->stop || c->sing) continue;
        d = c->inc > ki ? c->inc - ki : ki - c->inc;
        if (d < bd) { bd = d; bi = i; }
    }
    if (bi < NV) bout_start(s, bi, 0);                      /* (all singing: none more; COUNT says how many) */
}

/* per block: the field kept at COUNT (one comes or goes a block), the bouts and pauses running out; after a bout
 * now and then one leaves (a new one comes, drawn afresh) */
static void field_block(crickets_t *s, unsigned n)
{
    unsigned want = s->p[P_COUNT] < s->limit ? s->p[P_COUNT] : s->limit, have = singing_pop(s), i;
    int scene = tx_cloud_held(&s->cloud) > 0;
    if (want && scene && have < want) {
        unsigned ci = free_slot(s);
        if (ci < NV) individual(s, ci, s->knote, s->kvel, 0, 0xFF);
    } else if (have > (want ? want : have)) {
        for (i = s->limit; i-- > 0;) if (s->c[i].alive && !s->c[i].stop && !s->c[i].keyed && !s->c[i].sing) break;
        if (i >= s->limit) for (i = s->limit; i-- > 0;) if (s->c[i].alive && !s->c[i].stop && !s->c[i].keyed) break;
        if (i < s->limit) { s->c[i].stop = 1; s->c[i].sing = 0; tx_chorus_stop(&s->ch, i); }
    }
    for (i = 0; i < s->limit; i++) {
        cricket_t *c = &s->c[i];
        if (!c->alive || c->stop || --c->bl) continue;
        if (!c->sing) { bout_start(s, i, tx_rand_below(&s->cloud.rng, n)); continue; }
        bout_end(s, i);
        if (want && !c->keyed && tx_rand_below(&s->cloud.rng, 5u + act(s) / 6u) == 0u)
            c->stop = 1;                                     /* it leaves (ours: ~5 bouts; ~25 of the short busy ones) */
    }
}

TX_AUDIO_CODE static void crickets_render_raw(void *st, int32_t *mix, unsigned n)
{
    crickets_t *s = (crickets_t *)st;
    const species_t *sp = sp_of(s);
    unsigned k, i, ncall;
    uint16_t d0 = s->dropped;
    uint32_t len = scaled(s, sp->len), per = scaled(s, sp->per);
    int32_t held_any = 0;
    if (n > TX_BLOCK) n = TX_BLOCK;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    for (i = 0; i < k; i++) spawn(s, &s->ev[i]);
    field_block(s, n);
    ncall = tx_chorus_block(&s->ch, &s->cloud.rng, n, s->who, s->off, TX_CHORUS_MAX);
    for (i = 0; i < ncall; i++) {
        cricket_t *c = &s->c[s->who[i]];
        if (!c->alive || c->stop) continue;
        {   /* its own count; now and then a chirp one pulse longer or shorter (ours) */
            int32_t np = sp->pulses ? (int32_t)sp->pulses + c->pk + ((tx_rand(&s->cloud.rng) >> 29) == 0 ? (int32_t)(tx_rand(&s->cloud.rng) >> 31) * 2 - 1 : 0) : 1;
            c->pl = (uint8_t)(np < 2 && sp->pulses ? 2 : np);
        }
        c->pc = s->off[i];
    }
    for (i = 0; i < NV; i++) {
        cricket_t *c = &s->c[i];
        int held;
        if (!c->alive && !c->tone[0].on && !c->tone[1].on) continue;
        held = c->alive && (c->keyed ? c->em < TX_CLOUD_EMITTERS && s->cloud.em[c->em].held && s->cloud.em[c->em].note == c->note
                                     : s->p[P_COUNT] && tx_cloud_held(&s->cloud) > 0);
        if (c->alive && !held && !c->stop) {                 /* released: each caller goes on for 0-3 s, then stops */
            if (!c->linger) c->linger = (uint16_t)(2u + tx_rand_below(&s->cloud.rng, 516u));   /* after its call */
            else if (--c->linger == 1u) { c->stop = 1; tx_chorus_stop(&s->ch, i); }            /* (session 5: no */
        }                                                                                        /* hard stop) */
        held_any |= c->alive && !c->stop;
        while (c->pl && c->pc < (int32_t)n) {
            /* each pulse its own: level x (1 + 0.7 sd: +4.6 / -10 dB), length CV 16 %, spacing 10 % (the recordings:
             * 2.4-4.8 dB, 0.1) */
            int32_t g = (int32_t)(((int64_t)c->gain * s->level) >> 15), jv;
            uint32_t ln;
            g = (int32_t)(((int64_t)g * (32768 + ((tx_gauss_q12(&s->cloud.rng) * 2867) >> 10))) >> 15);
            g = g < 0 ? 0 : g;
            ln = (uint32_t)((int32_t)len + (int32_t)(((int64_t)tx_gauss_q12(&s->cloud.rng) * (int32_t)len * 82) >> 22));
            ln = (uint32_t)(((uint64_t)ln * (uint32_t)c->lf) >> 15);
            c->slot ^= 1u;
            tx_itone_start(&c->tone[c->slot], tx_vary_scale(c->inc, tx_vary_drift(&s->vy) >> 3), (sp->glide * c->gl) >> 15,   /* (the night warms, cools) */
                           ln, g > 32767 ? 32767 : g, (int32_t)(((int64_t)sp->h2 * c->h2f) >> 15), sp->h3,
                           (unsigned)c->pc);
            c->tone[c->slot].rasp = 3300;                    /* the teeth's rasp ~-20 dB under the tone (recordings: */
            if (!c->tone[c->slot].nz) c->tone[c->slot].nz = c->inc | 1u;   /* carrier band 18-24 dB over the rest) */
            c->pl--;
            jv = (int32_t)(((int64_t)tx_gauss_q12(&s->cloud.rng) * (int32_t)per * 51) >> 22);
            c->pc += (int32_t)per + jv;
        }
        if (c->pl) c->pc -= (int32_t)n;
        tx_itone_add(&c->tone[0], s->env, mix, n);
        tx_itone_add(&c->tone[1], s->env, mix, n);
        if (c->stop && !c->pl && !c->tone[0].on && !c->tone[1].on) { c->alive = 0; c->stop = 0; c->em = 0xFF; c->sing = 0; }
    }
    {   /* FIELD: the distant chorus, 8 far callers chirping at their own rates (each call a chirp of the species'
         * pulses, pure: the harmonics lost on the way) */
        int32_t g = held_any ? (int32_t)(((int64_t)s->p[P_FIELD] * s->level * FAR_G) >> (FAR_SH + 3)) : 0;
        for (i = 0; i < NFAR; i++) {
            uint32_t ph = s->vph[i];
            s->vph[i] += s->vinc[i] + (s->vinc[i] >> 4) * (i & 3);   /* spread +-10 % (ours) */
            if (g && !--s->fbl[i]) {                         /* each sings in bouts too */
                s->fsing[i] ^= 1u;
                s->fbl[i] = s->fsing[i] ? tx_pop_bout(&LAW, &s->cloud.rng, act(s), 2u) : tx_pop_pause(&LAW, &s->cloud.rng, act(s));
            }
            if (s->vph[i] < ph && g && s->fsing[i] && !s->fpl[i]) {   /* a call */
                s->fpl[i] = sp->pulses ? sp->pulses : 1u;
                s->fpc[i] = (int32_t)tx_rand_below(&s->cloud.rng, n);
            }
            while (s->fpl[i] && s->fpc[i] < (int32_t)n) {
                int32_t a = (int32_t)(((int64_t)g * (24576 + (int32_t)tx_rand_below(&s->cloud.rng, 16384u))) >> 15);   /* each pulse its own level */
                tx_itone_start(&s->far[i], s->finc[i], sp->glide, len, a > 32767 ? 32767 : a, 0, 0, (unsigned)s->fpc[i]);
                s->fpl[i]--;
                s->fpc[i] += (int32_t)per;
            }
            if (s->fpl[i]) s->fpc[i] -= (int32_t)n;
            tx_itone_add(&s->far[i], s->env, mix, n);
        }
    }
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = 0;
        for (i = 0; i < NFAR; i++) z |= s->far[i].on | s->fpl[i];
        for (i = 0; i < NV; i++) z |= s->c[i].alive | s->c[i].tone[0].on | s->c[i].tone[1].on;
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

/* the engine into its own block, then trimmed into the mix (OUT_TRIM) */
TX_AUDIO_CODE static void crickets_render(void *st, int32_t *mix, unsigned n)
{
    int32_t b[TX_BLOCK];
    unsigned j;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) b[j] = 0;
    crickets_render_raw(st, b, n);
    tx_rad_hp(&((crickets_t *)st)->rad, b, n, 3);   /* a cricket radiates under ~1 kHz poorly: -12 dB / oct */
    if (!tx_rad_quiet(&((crickets_t *)st)->rad)) ((crickets_t *)st)->quiet = 0;
    tx_vary_block(&((crickets_t *)st)->vy, b, n);
    if (((crickets_t *)st)->vy.lp) ((crickets_t *)st)->quiet = 0;          /* the split still settling */
    tx_trim_add(b, mix, n, OUT_TRIM);
}

static void crickets_stats(const void *st, tx_stats_t *o)
{
    const crickets_t *s = (const crickets_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < NV; i++) n += s->c[i].alive;
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: 32 mole crickets trilling (the densest pulses) in synchrony, close, hot, the field loud */
static const char SCENE_MAX[] =
    TX_TEXT("0 particles 32\n0 param SPECIES 70\n0 param COUNT 32\n0 param ACTIVITY 127\n0 param SYNC 127\n0 param TEMP 127\n0 param DIST 0\n"
    "0 param FIELD 127\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4000 end\n");

const tx_engine_t tx_eng_crickets = {
    "crickets", TX_TEXT("#27 Crickets"), NP, PARAMS, NV, 16, TX_STATE_SIZE(crickets_t), SCENE_MAX,
    crickets_init, crickets_key, crickets_param, crickets_render, crickets_stats,
};
