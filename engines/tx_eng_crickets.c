/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #27 Crickets: a field of singing crickets (INSECT + CHORUS cores).
 *
 * A key press puts a cricket (and CHORUS companions farther out) in the field; holding keeps it singing; release lets
 * it finish its chirp. Physics:
 *   song      the clockwork escapement (N1, N2, N10): one file tooth per cycle of the harp resonance, so each pulse
 *             (one wing closing) is a near-pure tone at the carrier, its pitch falling 5-15 % over the pulse, the
 *             envelope peaking ~2/3 through, a weak phase-locked H2 (-20 .. -30 dB). Field cricket 4.83 kHz,
 *             ~200 cycles per pulse, H2 -25 dB (N1, N10); Eugryllodes 3.2 kHz, H2 -17 dB (N1); mole cricket
 *             G. vineae 3.51 kHz, H2 -26, H3 -35 dB (N28), a trill; Dolbear's tree cricket (ours: 2.9 kHz, 7 pulses).
 *             Pulses per chirp and the pulse spacing: ours, inside the note's ranges.
 *   rhythm    Dolbear (N7): chirps per minute N = 40 + 7.2 (T_C - 10) (148 /min at 25 C); the pulses scale with the
 *             same ratio (Q10 ~ 2 for insect rhythms, ours).
 *   chorus    N19's inhibitory resetting (CHORUS core): callers hearing a neighbour move their next call to
 *             last + s d + T + eps; SYNC = s (0: free, ~0.5: alternation, 1: synchrony); CV(eps) 0.06, t 60 ms.
 *   distance  level 1 / r (each cricket placed on the plane; the propagation delays feed the chorus).
 * Keys: carrier per caller, a quarter tone per key around the species' carrier at C4 (a "tuned field"); velocity =
 * proximity (+-1 octave of distance). FIELD: the distant chorus as a band of noise at the carrier, its envelope from
 * 8 virtual callers at the chirp rate (sounds while a key is held). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_insect.h"
#include "tx_core_chorus.h"

#define OUT_TRIM 9276               /* output gain, Q12 (+5.8 dB): integrated (a key held 10 s): -31.8 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +1.3 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 160
#define VARY_DRIFT 60
#define VARY_SPLIT 0

enum { P_TEMP, P_SPECIES, P_CHORUS, P_SYNC, P_DIST, P_FIELD, NP };

static const tx_param_t PARAMS[NP] = {
    {"TEMP", 127, 76, "air temperature 10 .. 35 C: chirp rate 40 .. 220 /min (Dolbear), pulses as fast"},
    {"SPECIES", 127, 0, "field cricket 4.8 kHz | Eugryllodes 3.2 kHz | tree cricket 2.9 kHz | mole cricket 3.5 kHz trill"},
    {"CHORUS", 127, 16, "companions per key: 0 .. 15 crickets farther out (carriers +-3 %)"},
    {"SYNC", 127, 64, "reset by neighbours s: 0 free .. 0.5 alternate .. 1 synchrony"},
    {"DIST", 127, 50, "distance at velocity 100: 0.3 .. 30 m (log): level 1 / r; delays between callers"},
    {"FIELD", 127, 0, "the distant chorus behind (a band at the carrier): off .. loud"},
};

#define NV 32u
#define NSP 4u
#define G_REF 4900                 /* gain at 1 m, Q15 (calibrated by render: the chord stays under full scale) */
#define FADE 4u

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
};

typedef struct {
    tx_itone_t tone[2];
    uint32_t inc;                  /* carrier increment */
    int32_t gain;                  /* Q15 */
    int32_t tf;                    /* own period factor, Q15 */
    int32_t pc;                    /* samples to the next pulse of the chirp (from the block start) */
    uint8_t pl, slot, em, note, alive, stop, pad[2];
} cricket_t;

typedef struct {
    tx_cloud_t cloud;
    tx_chorus_t ch;
    tx_noise_t bed;
    cricket_t c[NV];
    int16_t env[TX_ITONE_ENV + 1];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    uint8_t who[TX_CHORUS_MAX];
    uint16_t off[TX_CHORUS_MAX];
    uint32_t vph[8], vinc[8];      /* the FIELD's virtual callers */
    uint8_t p[NP], limit, quiet;
    int32_t level, q15;            /* rate ratio to 25 C, Q15 */
    int32_t chirp_T;               /* the chirp period at TEMP, samples */
    int32_t dist_l2;
    uint16_t dropped;
    tx_vary_t vy;
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
    case P_SPECIES: set_timing(s); tx_noise_band(&s->bed, sp_of(s)->f0, 128, TX_NOISE_BAND); break;
    case P_SYNC: tx_chorus_set(&s->ch, (int32_t)v * 258, s->ch.cv_q15, (uint32_t)s->ch.t_eff, (uint32_t)s->ch.x_len); break;
    case P_DIST: s->dist_l2 = -113738 + (int32_t)(v * 3482u); break;   /* log2(0.3 m) + v log2(100) / 127, Q16 */
    default: break;
    }
}

static void crickets_init(void *st, uint32_t seed)
{
    crickets_t *s = (crickets_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    tx_chorus_init(&s->ch);
    tx_noise_init(&s->bed, seed ^ 0xC41C7E75u);
    tx_noise_flicker(&s->bed, 6000);
    tx_itone_env(s->env, 18022);                             /* the peak 0.55 through the pulse (recordings 0.41-0.61) */
    for (i = 0; i < NV; i++) s->c[i].em = 0xFF;
    for (i = 0; i < 8; i++) s->vph[i] = tx_rand(&s->cloud.rng);
    s->limit = 16;
    s->q15 = 32768;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((crickets_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void crickets_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((crickets_t *)st)->vy);
    tx_cloud_key(&((crickets_t *)st)->cloud, note, vel, on);
}

static void crickets_param(void *st, uint8_t id, uint8_t val)
{
    crickets_t *s = (crickets_t *)st;
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

/* a key: a cricket at the key's carrier + companions farther out */
static void spawn(crickets_t *s, const tx_event_t *e)
{
    const species_t *sp = sp_of(s);
    unsigned n = 1u + (s->p[P_CHORUS] >> 3), k, i;
    tx_rng_t *r = &s->cloud.rng;
    int32_t r0_l2 = s->dist_l2 + ((int32_t)100 - (int32_t)e->vel) * 1638;   /* velocity: +-1 oct of distance / 40 */
    for (k = 0; k < n; k++) {
        cricket_t *c = 0;
        int32_t rl2 = r0_l2, rr, ang, f_l2;
        uint32_t ci = NV;
        for (i = 0; i < s->limit && ci == NV; i++) if (!s->c[i].alive && !s->c[i].tone[0].on && !s->c[i].tone[1].on) ci = i;
        for (i = 0; i < s->limit && ci == NV; i++) if (!s->c[i].alive) ci = i;
        for (i = 0; i < s->limit && ci == NV; i++) if (s->c[i].stop) ci = i;
        if (ci == NV) { s->dropped++; continue; }
        c = &s->c[ci];
        if (k) rl2 += 38336 + (int32_t)tx_rand_below(r, 92736u);   /* companions x 1.5 .. 4 farther (ours) */
        if (rl2 > 9 << 16) rl2 = 9 << 16;                  /* 512 m at most */
        rr = (int32_t)tx_exp2(rl2 + (10 << 16));             /* mm */
        ang = (int32_t)tx_rand(r);
        tx_chorus_place(&s->ch, ci, (tx_sin_q15((uint32_t)ang + 0x40000000u) * (rr >> 4)) >> 11,
                        (tx_sin_q15((uint32_t)ang) * (rr >> 4)) >> 11);
        c->gain = (int32_t)((uint32_t)G_REF * 1000u / (uint32_t)(rr < 300 ? 300 : rr));
        f_l2 = ((int32_t)e->note - 60) * 2731 + (tx_vary_tone(&s->vy) >> 3);   /* a quarter tone per key (2^(1/24), Q16); the character (warmer = higher, faster) */
        if (k) f_l2 += (tx_gauss_q12(r) * 2789) >> 12;       /* companions: carrier CV 3 % (N1: 4.8-5.3 kHz) */
        c->inc = (uint32_t)(((uint64_t)sp->f0 * 97391u * tx_exp2(f_l2 + (16 << 16))) >> 16);
        c->tf = 32768 + ((tx_gauss_q12(r) * 1638) >> 12);    /* own period: CV 5 % (ours) */
        c->alive = 1; c->stop = 0; c->em = e->emitter; c->note = e->note; c->pl = 0;
        tx_chorus_start(&s->ch, ci, call_T(s, c->tf), k ? tx_rand_below(r, 65535u) % (uint32_t)call_T(s, c->tf) : 0);
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
    ncall = tx_chorus_block(&s->ch, &s->cloud.rng, n, s->who, s->off, TX_CHORUS_MAX);
    for (i = 0; i < ncall; i++) {
        cricket_t *c = &s->c[s->who[i]];
        if (!c->alive || c->stop) continue;
        c->pl = sp->pulses ? sp->pulses : 1u;
        c->pc = s->off[i];
    }
    for (i = 0; i < NV; i++) {
        cricket_t *c = &s->c[i];
        int held;
        if (!c->alive && !c->tone[0].on && !c->tone[1].on) continue;
        held = c->alive && c->em < TX_CLOUD_EMITTERS && s->cloud.em[c->em].held && s->cloud.em[c->em].note == c->note;
        if (c->alive && !held && !c->stop) { c->stop = 1; tx_chorus_stop(&s->ch, i); }
        held_any |= held;
        while (c->pl && c->pc < (int32_t)n) {
            /* each pulse its own: level SD ~3 dB, length CV 8 %, spacing 5 % (the recordings: 2.4-4.8 dB, 0.1) */
            int32_t g = (int32_t)(((int64_t)c->gain * s->level) >> 15), jv;
            uint32_t ln;
            g = (int32_t)(((int64_t)g * (32768 + ((tx_gauss_q12(&s->cloud.rng) * 2867) >> 10))) >> 15);
            g = g < 0 ? 0 : g;
            ln = (uint32_t)((int32_t)len + (int32_t)(((int64_t)tx_gauss_q12(&s->cloud.rng) * (int32_t)len * 82) >> 22));
            c->slot ^= 1u;
            tx_itone_start(&c->tone[c->slot], tx_vary_scale(c->inc, tx_vary_drift(&s->vy) >> 3), sp->glide,   /* (the night warms, cools) */
                           ln, g > 32767 ? 32767 : g, sp->h2, sp->h3,
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
        if (c->stop && !c->pl && !c->tone[0].on && !c->tone[1].on) { c->alive = 0; c->stop = 0; c->em = 0xFF; }
    }
    {   /* FIELD: the distant chorus, a band at the carrier with the envelope of 8 virtual callers */
        uint32_t cl = sp->pulses ? per * (sp->pulses - 1u) + len : len;
        uint32_t duty = ((cl << 8) / ((uint32_t)call_T(s, 32768) / 256u + 1u)) << 16;   /* fraction of a call, 0.32 */
        int32_t on = 0;
        for (i = 0; i < 8; i++) {
            s->vph[i] += s->vinc[i] + (s->vinc[i] >> 4) * (i & 3);   /* spread +-10 % (ours) */
            on += s->vph[i] < duty;
        }
        tx_noise_level(&s->bed, held_any ? (int32_t)(((int64_t)s->p[P_FIELD] * s->level * (2 + on)) >> 13) : 0);
        tx_noise_add(&s->bed, mix, n);
    }
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = !tx_noise_quiet(&s->bed);
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
    "0 particles 32\n0 param SPECIES 127\n0 param CHORUS 127\n0 param SYNC 127\n0 param TEMP 127\n0 param DIST 0\n"
    "0 param FIELD 127\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_crickets = {
    "crickets", "#27 Crickets", NP, PARAMS, NV, 16, sizeof(crickets_t), SCENE_MAX,
    crickets_init, crickets_key, crickets_param, crickets_render, crickets_stats,
};
