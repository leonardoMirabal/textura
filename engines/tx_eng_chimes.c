/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #24 Wind chimes: tubes hanging in the wind, struck by a clapper.
 *
 * Each held key hangs a tube tuned to its note (a key press also strikes it once, velocity = force); while tubes hang,
 * the wind strikes them at random. Physics:
 *   tube     a free-free bar / tube: modes 1 : 2.7565 : 5.4039 : 8.9330 x f1 (Euler-Bernoulli; note 04), the first four
 *            (those under fs/6 kept), one shared SVF bank per tube. Loss by material, constant Q: aluminium ~1500,
 *            glass ~600 (ours, from the note's metal / glass ranges), bamboo ~100 (M25: tan delta ~0.01 dry, bending).
 *   strike   the clapper sets each mode's amplitude (the bank's state directly: the struck tube's ring does not
 *            depend on Q); a soft clapper (long contact) excites the high modes less: gains n^-tilt (ours).
 *   wind     PhISEM timing (note 04: chimes = PhISEM timing into tubes): a wind energy fed by the WIND level and gusts
 *            (a slow random walk), strikes at a Poisson rate ~ energy x the number of tubes (up to ~20 / s), each on a
 *            random hanging tube with a force ~ energy x a random draw (ours: no chime measurement).
 * A tube rings out to exact zero (its modes stop under -72 dBFS; a time bound of 3 T60 after its last strike).
 * CPU pass (wave 4): the modes are PLATE core oscillators (lossless magic circle, the decay e^(-pi f t / Q) applied per
 * block, four modes interleaved), no longer SVF bank resonators on the error-feedback path: 2.5x cheaper, the same
 * mode frequencies and Q; a strike adds energy with a random phase instead of adding to the band state.
 * Variation pass (the user's session 5: "the same every time"): a round tube's bending modes come in pairs, one per
 * plane, and no real tube is perfectly round: each pair is split by 0.1-0.8 % (this tube's own), so the two beat
 * slowly; the clapper's angle sets how the strike shares out between the planes, so every strike beats differently
 * (ours, the standard degenerate-pair picture of bells and bars). And a struck tube swings away on its string and
 * twists: what reaches the listener swells and fades at the pendulum's ~0.6-1.2 Hz, by 15-50 % from the blow, dying
 * in a few seconds (ours).
 * Six modes now (1 : 2.76 : 5.40 : 8.93 : 13.34 : 18.64, those under fs / 6), the first two in both planes, the upper
 * ones single (the same oscillator count as four in pairs): the inharmonic shimmer a struck tube has above its hum.
 * Spectrogram review (session 5: "pure sine lines, no strike transient"): each strike now also clicks: the clapper
 * meets the tube for ~0.1-0.4 ms (a Hertz pulse, harder = shorter and brighter), ~-14 dB under the ring's peak (ours),
 * so the strike has the broadband body a struck tube has before its modes take over.
 * Alike pass (txaudit -e: 10.7 % of strike pairs alike at 8 seeds): the point weights were a sine guess that put the
 * clapper near f1's antinode and mode 2's node over much of its range, so most rings were a near-pure f1. They are now
 * the free-free bar's own mode shapes |phi_n(x)| (every mode large at the free end, f1's node at 0.224 L): the mode
 * mix moves with where the clapper lands, 0.15 .. 0.42 of the length (not the middle: a node of every even mode, a
 * near-pure f1 there). And the clapper's speed sets its contact time (Hertz: a slower blow stays longer on the tube and
 * rings darker): the tilt falls 0.625 per octave of force, so the dull blows are the soft ones. The modes +6.7 dB keep
 * the level (8 seeds x 90 s held: the same total); the mean mix ~2 dB brighter above f1 (centroid 338 vs 313 Hz, the
 * user's pick of three). Alike: 5.9 % at 8 seeds (5.2 % over 132 s; was 10.7 / 10.5). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_plate.h"
#include "tx_core_pulse.h"
#include "tx_core_modal.h"

#define OUT_TRIM 11412               /* output gain, Q12 (+4.5 dB): integrated (a key held 10 s): -30.5 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +4.4 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 5

enum { P_WIND, P_GUST, P_MATERIAL, P_CLAPPER, NP };

static const tx_param_t PARAMS[NP] = {
    {"WIND", 127, 50, TX_TEXT("wind: calm .. strong: strikes ~0.3 .. 20 / s across the tubes, harder")},
    {"GUST", 127, 50, TX_TEXT("gustiness: steady .. gusty (bursts of strikes)")},
    {"MATERIAL", 127, 0, TX_TEXT("aluminium (Q ~1500, long ring) | glass (Q ~600) | bamboo (Q ~100, M25)")},
    {"CLAPPER", 127, 64, TX_TEXT("clapper: soft (felt / wood: dark) .. hard (metal: bright, all modes)")},
};

#define NT 8u
#define NM 6u                       /* modes (the first two in two planes: NM + 2 oscillators) */
#define NB 2u
#define A_STRIKE 5000              /* a mode's amplitude for a full strike (Q15 band state; calibrated by render) */
#define A_MODE 10816               /* the modes' share of it: A_STRIKE +6.7 dB (the mode shapes' mean weight; the click
                                    * keeps A_STRIKE) */
#define TILT_MEAN 24576            /* +0.375 on the clapper's tilt (Q16), at a blow of force 2^14 */
#define HERTZ 160                  /* the tilt's fall per octave of force, Q8 (0.625; ours, from the mean mix) */

TX_AUDIO_DATA static const uint16_t RATIO[NM] = { 1024, 2823, 5534, 9147, 13664, 19085 };   /* Q10 */
TX_AUDIO_DATA static const uint16_t MAT_Q[3] = { 1500, 600, 100 };
/* a free-free bar's mode shapes |phi_n(x)| / 2 at x = 0, 1/32 .. 1/2 of its length (Q15; symmetric about the middle) */
TX_AUDIO_DATA static const uint16_t SHAPE[NM][17] = {
    {32767, 28010, 23260, 18540, 13880, 9322, 4913, 704, 3250, 6896, 10180, 13052, 15469, 17391, 18786, 19633, 19917},
    {32767, 24725, 16744, 8966, 1596, 5133, 10978, 15715, 19161, 21187, 21733, 20816, 18531, 15045, 10596, 5471, 0},
    {32767, 21527, 10512, 215, 8727, 15669, 20085, 21661, 20354, 16418, 10387, 3014, 4802, 12124, 18078, 21958, 23304},
    {32767, 18339, 4500, 7559, 16467, 21121, 21016, 16425, 8389, 1444, 11119, 18736, 22805, 22525, 17939, 9919, 0},
    {32767, 15178, 1162, 13925, 20835, 20715, 14057, 3026, 9085, 18738, 23133, 20989, 12898, 1152, 10913, 19867, 23165},
    {32767, 12055, 6350, 18530, 21449, 14886, 1915, 12100, 21506, 22529, 14727, 1152, 12865, 21812, 22171, 13802, 0},
};
static const int32_t L2_RATIO[NM] = { 0, 96004, 161761, 207036, 244980, 276575 };   /* log2 of the ratios, Q16 */

typedef struct {
    tx_pmode_t m[NM + NB];         /* the free modes (m[i]); the first NB also in the other plane (m[nm + i]) */
    int32_t ge[NM + NB];           /* their envelopes with 8 more bits (g << 8): the decay stays exponential to the end */
    uint32_t f1;                   /* Hz */
    uint32_t silent, tmax;         /* samples since the last strike; 3 T60 */
    uint32_t bdue[2];              /* the clapper's bounces: samples to go (0 = none) */
    uint32_t sw_ph, sw_inc;        /* the swing: phase, step per block */
    int32_t sw_d;                  /* its depth (Q15), dying */
    int32_t bforce[2];             /* their force (Q15), signed */
    int32_t qf[4];                 /* this block's blows, each at its own sample: force (Q15) and sample (qo), qn of */
    uint8_t qo[4], qn, pad[3];     /* them (the tube renders up to each, then it is struck: never on the block grid) */
    uint8_t em, note, alive, nm, ak, pad2[3];
} tube_t;

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    tx_pulses_t clicks;            /* the clapper's contact clicks */
    tube_t t[NT];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK];
    uint8_t p[NP], limit, quiet;
    int32_t level, energy, gust, tilt;
    uint32_t q;
    uint16_t dropped;
    tx_vary_t vy;
} chimes_t;

static void set_param(chimes_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    if (id == P_MATERIAL) s->q = MAT_Q[(v * 3u) >> 7];
    if (id == P_CLAPPER) s->tilt = (int32_t)(127u - v) * 1032;   /* n^-tilt, tilt 0 .. 2 (Q16) */
}

static void chimes_init(void *st, uint32_t seed)
{
    chimes_t *s = (chimes_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    for (i = 0; i < NT; i++) s->t[i].em = 0xFF;
    tx_pulses_init(&s->clicks);
    s->limit = 6;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((chimes_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void chimes_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((chimes_t *)st)->vy);
    tx_cloud_key(&((chimes_t *)st)->cloud, note, vel, on);
}

static void quiet_tube(tube_t *t)
{
    unsigned i;
    for (i = 0; i < NM + NB; i++) t->m[i].g = t->m[i].gl = 0;
}

static int tube_quiet(const tube_t *t)
{
    unsigned i;
    for (i = 0; i < NM + NB; i++) if (t->m[i].g | t->m[i].gl) return 0;
    return 1;
}

/* 2 sin(pi f / fs), Q16, from f in Hz Q8 (series to x^5: < 3e-6 below fs / 6) */
static int32_t coef_q16(uint32_t f_q8)
{
    int64_t x = (int64_t)f_q8 * 299, x2 = (x * x) >> 30, s;
    s = x - ((((x * x2) >> 30) * 178956971) >> 30) + ((((((x * x2) >> 30) * x2) >> 30) * 8947849) >> 30);
    return (int32_t)(s >> 13);
}

static void chimes_param(void *st, uint8_t id, uint8_t val)
{
    chimes_t *s = (chimes_t *)st;
    if (tx_cloud_keyat(&s->cloud, id, val)) return;          /* (a press's own sample: TX_PID_KEYAT) */
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NT ? NT : val);
        for (i = s->limit; i < NT; i++) { s->t[i].alive = 0; s->t[i].em = 0xFF; quiet_tube(&s->t[i]); }
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* a phase-correct kick into a ringing mode: the mode's (x, y) at amplitude g plus a velocity kick a, renormalised
 * (event rate, 64-bit); from rest it starts at zero displacement */
static void kick_mode(tx_pmode_t *m, int32_t a)
{
    int32_t X, Y;
    uint64_t g2;
    uint32_t g;
    if (!m->g && !m->gl) { m->x = 16000; m->y = 0; }
    a = a > 32767 ? 32767 : a < -32767 ? -32767 : a;
    X = m->g * m->x / 16000 + a;                             /* 32-bit: g < 2^15, |x| < 2^15 */
    Y = m->g * m->y / 16000;
    g2 = (uint64_t)((int64_t)X * X) + (uint64_t)((int64_t)Y * Y);
    if (g2 > 0x3FFFFFFFull) g2 = 0x3FFFFFFFull;
    g = tx_isqrt32((uint32_t)g2);
    if (!g) { m->g = 0; return; }
    m->x = X * 16000 / (int32_t)g;                           /* |X| <= g: < 2^30 */
    m->y = Y * 16000 / (int32_t)g;
    m->g = (int32_t)(g > 32767u ? 32767u : g);
}

/* strike a tube: each mode's amplitude from the force, the clapper (n^-tilt) and where it hits. No two strikes alike
 * (the user's session 3): the force +-0.4 oct, the clapper's hardness +-0.5 of the tilt (and the press's tone), the point along the tube
 * (each mode weighted 0.15 .. 1 by its shape there: SHAPE), and 1-2 bounces 3-40 ms later at -6 .. -18 dB that add to the
 * ring in phase or against it (ours) */
static void strike(chimes_t *s, tube_t *t, int32_t force_q15, int bounce, unsigned o)
{
    tx_rng_t *r = &s->cloud.rng;
    unsigned i;
    int32_t tilt = s->tilt + TILT_MEAN + ((tx_rand_s15(r) * 33) >> 5) - (tx_vary_tone(&s->vy) >> 1), pt = 4915 + (int32_t)tx_rand_below(r, 8848u);   /* 0.15 .. 0.42 */
    int32_t sg = bounce ? (force_q15 < 0 ? -1 : 1) : ((tx_rand(r) & 1u) ? 1 : -1);
    uint32_t ang;                                            /* the clapper's angle to the planes: the next blow lands */
    t->ak = (uint8_t)(t->ak + 3u + tx_rand_below(r, 3u));   /* 3-5 eighths of a turn round from the last (the clapper */
    ang = ((uint32_t)(t->ak & 7u) << 29) + (tx_rand(r) >> 3);   /* swings back from the other side; ours) */
    int32_t ca = tx_sin_q15(ang + 0x40000000u), sa = tx_sin_q15(ang);
    if (force_q15 < 0) force_q15 = -force_q15;
    if (!bounce) force_q15 = (int32_t)(((int64_t)force_q15 * tx_exp2((tx_rand_s15(r) * 26) / 32 + (16 << 16))) >> 16);   /* +-0.4 oct */
    tilt -= ((tx_log2_q16((uint32_t)force_q15 + 1u) - (14 << 16)) * HERTZ) >> 8;   /* faster: shorter contact, brighter */
    tilt = tilt < 0 ? 0 : tilt;
    for (i = 0; i < t->nm; i++) {
        int32_t g = (int32_t)tx_exp2((15 << 16) - (int32_t)(((int64_t)tilt * L2_RATIO[i]) >> 16));   /* (f_n / f1)^-tilt */
        int32_t sh = SHAPE[i][pt >> 10] + (((SHAPE[i][(pt >> 10) + 1] - SHAPE[i][pt >> 10]) * (pt & 1023)) >> 10);   /* the mode's shape at the point */
        int32_t a = (int32_t)(((int64_t)force_q15 * g >> 15) * A_MODE >> 15);
        a = (int32_t)(((int64_t)a * (4915 + (((sh < 0 ? -sh : sh) * 27853) >> 15))) >> 15);
        a = (int32_t)(((int64_t)a * s->level) >> 15);
        if (i < NB) {                                        /* the low modes: shared between the planes */
            kick_mode(&t->m[i], (int32_t)(((int64_t)sg * a * ca) >> 15));
            kick_mode(&t->m[t->nm + i], (int32_t)(((int64_t)sg * a * sa) >> 15));
        } else
            kick_mode(&t->m[i], sg * a);
    }
    t->silent = 0;
    {   /* the clapper's click: a Hertz pulse of 4-18 samples (harder clapper = shorter), ~ the force (ours) */
        int32_t ca = (int32_t)(((int64_t)force_q15 * A_STRIKE >> 15) * s->level >> 15) >> 1;
        uint32_t dur = 4u + (uint32_t)(tilt >> 13) + tx_rand_below(r, 4u);
        tx_pulse_start(&s->clicks, TX_PULSE_SHAPE_HERTZ, dur, ca > 32767 ? 32767 : ca, o);
    }
    if (!bounce) {                                           /* the blow sets the tube swinging */
        t->sw_inc = 14984956u + (tx_rand(r) >> 16) * 228u;   /* 0.6 .. 1.2 Hz at 172.3 blocks / s */
        t->sw_ph = tx_rand(r);
        t->sw_d = 4915 + (int32_t)tx_rand_below(r, 11469u); /* 15 .. 50 % */
        unsigned nb = 1u + tx_rand_below(r, 2u), k;           /* a clapper on a string rebounds */
        for (k = 0; k < 2u; k++) {
            t->bdue[k] = k < nb ? o + 132u + tx_rand_below(r, 1632u) * (k + 1u) : 0u;   /* (from the blow's sample) */
            t->bforce[k] = (int32_t)(((int64_t)force_q15 * (4129 + (int32_t)tx_rand_below(r, 12288u))) >> 15) * ((tx_rand(r) & 1u) ? 1 : -1);
        }
    }
}

/* a blow at sample o of this block: queued on the tube, struck in time order as the tube renders (chimes_render) */
static void blow(tube_t *t, int32_t force_q15, unsigned o)
{
    if (t->qn >= 4u) return;                                 /* (a fifth in one block on one tube: too close to hear) */
    t->qf[t->qn] = force_q15; t->qo[t->qn] = (uint8_t)(o < TX_BLOCK ? o : TX_BLOCK - 1u); t->qn++;
}

static void hang(chimes_t *s, const tx_event_t *e)
{
    tube_t *t = 0;
    unsigned i;
    for (i = 0; i < s->limit && !t; i++) if (s->t[i].alive && s->t[i].note == e->note) t = &s->t[i];
    for (i = 0; i < s->limit && !t; i++) if (!s->t[i].alive && tube_quiet(&s->t[i])) t = &s->t[i];
    for (i = 0; i < s->limit && !t; i++) if (!s->t[i].alive) t = &s->t[i];
    if (!t) { s->dropped++; return; }
    if (!t->alive || t->note != e->note) {
        uint32_t f1 = tx_note_inc((int32_t)e->note << 16) / 97391u;
        uint32_t split = 66u + tx_rand_below(&s->cloud.rng, 459u);   /* the planes 0.1 .. 0.8 % apart, Q16 */
        quiet_tube(t);
        t->f1 = f1; t->nm = 0;
        for (i = 0; i < NM; i++) {
            uint32_t hz = (f1 * RATIO[i]) >> 10;
            if (hz > 7300u) break;
            t->nm = (uint8_t)(i + 1u);
        }
        for (i = 0; i < t->nm; i++) {
            uint32_t hz = (f1 * RATIO[i]) >> 10, f8 = (f1 * RATIO[i]) >> 2, k;
            for (k = 0; k < (i < NB ? 2u : 1u); k++) {
                tx_pmode_t *m = &t->m[k * t->nm + i];
                m->e0 = m->e = coef_q16(k ? f8 + (uint32_t)(((uint64_t)f8 * split) >> 16) : f8);   /* Hz Q8 */
                m->hz = (uint16_t)hz;
                m->dec = tx_plate_decay((uint32_t)(((uint64_t)hz * 804u) / s->q));   /* pi f / Q per s, Q8 */
            }
        }
    }
    t->alive = 1; t->em = e->emitter; t->note = e->note;
    t->tmax = s->q * 291060u / (t->f1 ? t->f1 : 1u);         /* 3 T60 = 6.6 Q / f1 s, samples */
    blow(t, (int32_t)e->vel * e->vel * 2, e->offset);
}

TX_AUDIO_CODE static void chimes_render(void *st, int32_t *mix, unsigned n)
{
    chimes_t *s = (chimes_t *)st;
    unsigned k, i, j, hanging = 0;
    int32_t *buf = s->buf;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) buf[j] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    for (i = 0; i < k; i++) hang(s, &s->ev[i]);
    for (i = 0; i < NT; i++) {                               /* released keys: the tube stops hanging (rings out) */
        tube_t *t = &s->t[i];
        if (t->alive && !(t->em < TX_CLOUD_EMITTERS && s->cloud.em[t->em].held && s->cloud.em[t->em].note == t->note)) {
            t->alive = 0; t->em = 0xFF;
        }
        hanging += t->alive;
    }
    /* the wind: energy toward WIND x (1 + gust), strikes at a Poisson rate ~ energy x tubes */
    {
        tx_rng_t *r = &s->cloud.rng;
        int32_t target;
        s->gust += ((tx_rand_s15(r) * (s->p[P_GUST] * 258)) >> 17) - (s->gust >> 6);
        s->gust = s->gust > 32767 ? 32767 : s->gust < -32767 ? -32767 : s->gust;
        target = (int32_t)s->p[P_WIND] * 258;
        target = target + ((target >> 1) * s->gust >> 15) * 2;
        target = target < 0 ? 0 : target > 32767 ? 32767 : target;
        s->energy += (target - s->energy) >> 4;
        if (hanging && s->energy > 64) {
            /* rate per block = 20 / s x (E / 1)^2 x tubes / 6 / 172 blocks per s; Q16 probability */
            uint32_t e2 = (uint32_t)((s->energy >> 7) * (s->energy >> 7));   /* E^2, Q16 */
            uint32_t p = e2 * hanging * 20u / 6u / 172u + 26u;  /* + a calm floor ~0.07 / s per tube */
            if ((tx_rand(r) >> 16) < p) {
                unsigned pick = tx_rand_below(r, hanging), c = 0;
                for (i = 0; i < NT; i++)
                    if (s->t[i].alive && c++ == pick) {
                        blow(&s->t[i], (int32_t)(((s->energy >> 1) + (int32_t)(tx_rand(r) >> 18)) >> 0), tx_rand(r) >> 24);   /* (a
                                                     * gust's blow: anywhere in the block, as likely) */
                        break;
                    }
            }
        }
    }
    for (i = 0; i < NT; i++) {
        tube_t *t = &s->t[i];
        unsigned q;
        unsigned done = 0, nmo = t->nm + (t->nm < NB ? t->nm : NB);
        int32_t lvl;
        if (!t->nm || (tube_quiet(t) && !t->qn && !((t->bdue[0] && t->bdue[0] <= n) || (t->bdue[1] && t->bdue[1] <= n)))) { t->qn = 0; for (q = 0; q < 2u; q++) t->bdue[q] = t->bdue[q] > n ? t->bdue[q] - n : 0u; continue; }
        t->sw_ph += t->sw_inc;
        t->sw_d -= t->sw_d >> 8;                             /* the swing dies (~1.5 s) */
        lvl = 32767 - t->sw_d + ((t->sw_d * tx_sin_q15(t->sw_ph)) >> 15);
        for (;;) {   /* the blows and the clapper's bounces due in this block, in time order, each at its own sample:
                      * the tube renders up to it (the envelope ramps to g x the swing), then it is struck */
            unsigned o = TX_BLOCK, pick = 8u, k;
            for (k = 0; k < t->qn; k++) if (t->qo[k] < o) { o = t->qo[k]; pick = k; }
            for (k = 0; k < 2u; k++) if (t->bdue[k] && t->bdue[k] <= n && t->bdue[k] - 1u < o) { o = t->bdue[k] - 1u; pick = 4u + k; }
            if (pick == 8u) break;
            if (o > done) { tx_pmode_add(t->m, nmo, lvl, buf + done, o - done); done = o; }
            if (pick < 4u) {
                int32_t f = t->qf[pick];
                t->qn--; t->qf[pick] = t->qf[t->qn]; t->qo[pick] = t->qo[t->qn];
                strike(s, t, f, 0, o);
            } else { t->bdue[pick - 4u] = 0; strike(s, t, t->bforce[pick - 4u], 1, o); }
        }
        for (q = 0; q < 2u; q++) t->bdue[q] = t->bdue[q] > n ? t->bdue[q] - n : 0u;
        if (n > done) tx_pmode_add(t->m, nmo, lvl, buf + done, n - done);
        for (q = 0; q < t->nm + (t->nm < NB ? t->nm : NB); q++) {   /* then decays one block */
            int32_t e = t->ge[q];
            if ((e >> 8) != t->m[q].g) e = t->m[q].g << 8;   /* (struck since: from the mode's amplitude) */
            e = (int32_t)(((int64_t)e * t->m[q].dec) >> 16);   /* (per block: 64-bit is fine) */
            if (e < 256) e = 0;                              /* (g: rounding made the decay linear at the end, */
            t->ge[q] = e;                                    /* a visible stop at ~-55 dB; session 5) */
            t->m[q].g = e >> 8;
        }
        t->silent += n;
        if (t->silent > t->tmax) quiet_tube(t);              /* 3 T60: a bound */
    }
    tx_pulses_add(&s->clicks, buf, n);
    tx_vary_block(&s->vy, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = s->hp.x1 | s->hp.y1 | (int32_t)tx_pulses_active(&s->clicks);
        for (i = 0; i < NT; i++) z |= s->t[i].alive | !tube_quiet(&s->t[i]);
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void chimes_stats(const void *st, tx_stats_t *o)
{
    const chimes_t *s = (const chimes_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < NT; i++) n += s->t[i].alive;
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: eight tubes (every mode under fs/6), strong gusty wind, a long-ringing material */
static const char SCENE_MAX[] =
    TX_TEXT("0 particles 8\n0 param WIND 127\n0 param GUST 127\n0 param MATERIAL 0\n0 param CLAPPER 127\n"
    "0 on 53 127\n0 on 55 127\n0 on 57 127\n0 on 60 127\n0 on 62 127\n0 on 64 127\n0 on 67 127\n0 on 69 127\n"
    "3000 off 53\n3000 off 55\n3000 off 57\n3000 off 60\n3000 off 62\n3000 off 64\n3000 off 67\n3000 off 69\n"
    "4000 end\n");

const tx_engine_t tx_eng_chimes = {
    "chimes", TX_TEXT("#24 Wind chimes"), NP, PARAMS, NT, 6, TX_STATE_SIZE(chimes_t), SCENE_MAX,
    chimes_init, chimes_key, chimes_param, chimes_render, chimes_stats,
};
