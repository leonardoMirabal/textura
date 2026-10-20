/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #3 Rain on surfaces: rain on leaves, a tent, a wooden deck, a window, a metal roof (02).
 *
 * While a key is held it rains (velocity: heavier), release lets the shower pass (~3-4 s); a press also drops one big
 * drop near you (key row: lower = bigger drop, 2 .. 6 mm). Physics:
 *   drops    four size classes (0.7-1.2 / 1.2-2.4 / 2.4-4.1 / > 4.1 mm, L21) arriving per m^2 per s as note 02's table
 *            (Marshall-Palmer x Gunn-Kinzer, L15 / L16; log-log in R), times the listening AREA; DROPS shifts the size
 *            mix (drizzle .. convective: the small and big classes x 4^-+1, ours as note 02's Lambda knob).
 *   force    a drop pushes the surface for ~2 D / V with Mitchell's shape F = 3.596 t'^0.5 exp(-3.333 t'),
 *            t' = V t / D, peak 0.85 rho D^2 V^2 at 0.15 D / V, impulse = momentum (L29, L31 / L33): big drops are
 *            louder and darker (corner V / (2 pi 0.3 D)). Sparse classes are explicit pulses (PARTICLES per block);
 *            dense classes and the overflow become noise of the same power (Campbell), low-passed at the class corner.
 *   surface  a roof or a window is modally dense: a log grid of 12 bands (150 Hz .. 7 kHz, half-octave) driven at unit
 *            noise power, Q from the material (I6: rubber 9, wood 10, glass 395, steel 1400; the bands capped at
 *            Q 40, ours), plus 6 low panel modes (60-400 Hz) at the material's own Q for glass and metal (note 03).
 *   leaves   a water-repellent leaf gets a second, weaker push from the rebounding jet at 0.44 tau_g,
 *            tau_g = sqrt(rho D^3 / gamma) (gain 0.2, measured < 0.37 when splashing).
 *   inside   heard from under the roof: a low-pass (~600 Hz .. open) (ours).
 * Realism pass (rain on a porch, a shed roof, a tent, a window, a metal roof; references, analysis only): real rain on a
 * surface is an even patter (drop-level std 1-5 dB, crest 10-13 dB, kurtosis 0-5) with its energy up to 1-2 kHz
 * and beyond. Ours was a few loud drops over a dull bed (kurtosis 91, crest 22 dB, centroid 750 Hz). So: the drop's
 * level is the force law's fourth root (ours: D^2 V^2 spread 44 dB between the classes), and the surface bands rise
 * +6 dB / oct (radiation below coincidence, ours). The default SURFACE is wood as documented (50 was the tent). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"

#define OUT_TRIM 7540               /* output gain, Q12 (+0.8 dB): integrated (a key held 10 s): -26.8 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +4.5 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -130
#define VARY_HI 15
#define VARY_TILT 160
#define VARY_DRIFT 60
#define VARY_SPLIT 3

enum { P_RATE, P_SURFACE, P_DROPS, P_AREA, P_INSIDE, NP };

static const tx_param_t PARAMS[NP] = {
    {"RATE", 127, 70, "rain rate R: 0.1 .. 100 mm/h (log): drizzle .. downpour"},
    {"SURFACE", 127, 64, "leaves (Q 8, rebound) | tent (Q 5) | wood (Q 15) | glass (Q 400) | metal roof (Q 1400)"},
    {"DROPS", 127, 64, "drop sizes: drizzle (small drops) .. convective (big drops: louder, darker)"},
    {"AREA", 127, 50, "listening area: 0.1 .. 20 m^2 (log): a few drops near you .. a whole roof"},
    {"INSIDE", 127, 0, "outside .. inside under the roof (low-passed)"},
};

#define NC 4u
#define NB 12u
#define NPANEL 6u
#define TABN 64u
#define CARRY 64u
#define A_REF 3800                 /* the biggest class's pulse peak (Q15 force units; calibrated by render) */

/* per class: arrivals per m^2 per s at R = 1, 5, 25, 100 mm/h (note 02, L15 x L16), x 100 */
static const uint32_t RATE_TAB[NC][4] = {
    {34000, 97700, 211100, 345000}, {7400, 42800, 159800, 378100}, {80, 1900, 19500, 89400}, {1, 20, 670, 7700}
};
static const uint16_t D_UM[NC] = { 950, 1700, 3100, 5000 };   /* representative diameters */
static const uint16_t V_MM[NC] = { 3850, 5800, 8100, 9100 };  /* terminal speeds (L16), mm/s */
static const uint16_t SURF_Q[5] = { 8, 5, 15, 400, 1400 };
static const uint16_t PANEL_HZ[NPANEL] = { 63, 97, 141, 199, 273, 381 };   /* ours: a few low panel modes */

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    tx_svf_t band[NB], panel[NPANEL];
    int32_t bf[NB], bgin[NB], bg[NB], pf[NPANEL], pgin[NPANEL], pg[NPANEL], bq, pq;
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int16_t tab[TABN + 1];         /* Mitchell's pulse shape, Q15 peak */
    int32_t ex[TX_BLOCK + CARRY], buf[TX_BLOCK];
    int32_t nlp[NC];               /* the noise beds' low-passes (Q15) */
    int32_t olp, olpa;             /* the inside low-pass */
    uint32_t lam_q8[NC];           /* arrivals per second over the AREA, Q8 */
    uint32_t step[NC], len[NC];    /* pulse table step per sample (Q16), length (samples) */
    int32_t amp[NC], fca[NC], comp[NC];   /* pulse peak (Q15), the corner as a one-pole (Q15), its sd compensation (Q8) */
    uint32_t tg[NC];               /* leaves: the rebound delay, samples */
    int32_t wet;                   /* rain on (Q15 ramp) */
    uint32_t rng_n;
    uint8_t p[NP], limit, quiet, surf, nrain;
    int32_t level;
    uint16_t dropped;
    tx_vary_t vy;
} rain_t;

#define MUL15(a, b) ((((a) >> 15) * (b)) + ((((a) & 0x7FFF) * (b)) >> 15))

static int32_t log2_q16(uint32_t x) { return (32 << 16) - (int32_t)(tx_neglog2_q12(x ? x : 1u) << 4); }

static void build_table(rain_t *s)
{
    unsigned i;
    int32_t peak = 1, v[TABN + 1];
    for (i = 0; i <= TABN; i++) {                            /* t' = i / 32 (0 .. 2): F ~ sqrt(t') exp(-3.333 t') */
        uint32_t sq = i ? tx_exp2((log2_q16(i) - (5 << 16)) / 2 + (15 << 16)) : 0u;   /* sqrt(i / 32), Q15 */
        uint32_t ex = tx_exp2((15 << 16) - (int32_t)(i * 9849u));   /* exp(-3.333 i / 32) = 2^(-0.1503 i), Q15 */
        v[i] = (int32_t)((sq * ex) >> 15);
        if (v[i] > peak) peak = v[i];
    }
    for (i = 0; i <= TABN; i++) s->tab[i] = (int16_t)(v[i] * 32767 / peak);
    s->tab[TABN] = 0;
}

static void surface(rain_t *s)
{
    unsigned i, m = (s->p[P_SURFACE] * 5u) >> 7;
    uint32_t q = SURF_Q[m], qb = q > 40u ? 40u : q;
    s->surf = (uint8_t)m;
    s->bq = (int32_t)(16384u / qb);
    for (i = 0; i < NB; i++) {
        uint32_t hz = tx_exp2(473750 + (int32_t)i * 32768);  /* 150 Hz x 2^(i/2) .. 6.8 kHz */
        s->bf[i] = tx_svf_coef(hz);
        s->bgin[i] = (int32_t)tx_svf_gin_noise(hz, qb * 16u);
        s->bg[i] = (int32_t)tx_exp2((10 << 16) + ((int32_t)i - 6) * 32768);   /* +6 dB / oct: a plate radiates
                                                              * better as f rises (below coincidence; ours: Q11, 1.0 at 1.2 kHz) */
    }
    s->pq = (int32_t)(16384u / q);
    for (i = 0; i < NPANEL; i++) {
        s->pf[i] = tx_svf_coef(PANEL_HZ[i]);
        s->pgin[i] = (int32_t)tx_svf_gin_noise(PANEL_HZ[i], q * 16u);
        s->pg[i] = 1024;
    }
}

/* a pulse's peak from D and V: A_REF sqrt((D / 5 mm) (V / 9.1 m/s)): the force F_max = 0.85 rho D^2 V^2, compressed to
 * its fourth root (ours: the references' drops vary 1-5 dB, crest 10-13 dB; the D^2 V^2 law gave a 44 dB spread and a
 * kurtosis of 90, D V still 18.7 dB crest) */
static int32_t peak_of(uint32_t d_um, uint32_t v_mm)
{
    int32_t l = ((log2_q16(d_um) - 805306) + (log2_q16(v_mm) - 862000)) / 2;   /* log2 5000, log2 9100 */
    return (int32_t)tx_exp2(log2_q16(A_REF) + (l > (2 << 16) ? (2 << 16) : l));
}

static void rates(rain_t *s)
{
    static const int32_t LR[4] = { 0, 152170, 304340, 435412 };   /* log2 1, 5, 25, 100 */
    int32_t lr = -217706 + (int32_t)(s->p[P_RATE] * 5142u);   /* log2(0.1) + v log2(1000) / 127 */
    int32_t la = -217706 + (int32_t)(s->p[P_AREA] * 3896u);   /* log2(0.1 m^2) + v log2(200) / 127 */
    int32_t shift = ((int32_t)s->p[P_DROPS] - 64) * 2048;     /* +-2 octaves of the size mix */
    unsigned c, k;
    for (c = 0; c < NC; c++) {
        int32_t l0, l1, lv;
        uint32_t L, fc, a2;
        k = lr < LR[1] ? 0u : lr < LR[2] ? 1u : 2u;
        l0 = log2_q16(RATE_TAB[c][k]); l1 = log2_q16(RATE_TAB[c][k + 1]);
        lv = l0 + ((((l1 - l0) >> 4) * ((lr - LR[k]) >> 4)) / ((LR[k + 1] - LR[k]) >> 8));   /* log-log, 32-bit */
        lv += la - 435412 + (8 << 16);                       /* / 100 (the table), x area, Q8 */
        lv += c >= 2u ? shift : -shift / 2;                  /* convective: more big drops, fewer small */
        s->lam_q8[c] = lv > (26 << 16) ? (1u << 26) : lv < 0 ? 0u : tx_exp2(lv);
        L = 882u * D_UM[c] / V_MM[c] / 10u;                  /* 2 D / V in samples */
        s->len[c] = L ? L : 1u;
        s->step[c] = ((uint32_t)TABN << 16) / s->len[c];
        fc = (uint32_t)V_MM[c] * 531u / D_UM[c];             /* V / (2 pi 0.3 D): (mm/s / um) x 1e3 / 1.885 */
        s->fca[c] = 32768 - (int32_t)tx_exp2((15 << 16) - (int32_t)((fc > 20000u ? 20000u : fc) * 862u >> 6));
        a2 = (uint32_t)((65536u * 2u - (uint32_t)s->fca[c] * 2u) / (uint32_t)(s->fca[c] >> 8 ? s->fca[c] >> 8 : 1));   /* (2 - a) / a, Q8 */
        s->comp[c] = (int32_t)tx_exp2((log2_q16(a2 ? a2 : 1u) - (8 << 16)) / 2 + (8 << 16));   /* sqrt, Q8 */
        s->amp[c] = peak_of(D_UM[c], V_MM[c]);
        s->tg[c] = tx_exp2((3 * log2_q16(D_UM[c])) / 2 - 574900);   /* 0.44 tau_g = 0.002287 D_um^1.5 samples */
    }
}

static void set_param(rain_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    if (id == P_SURFACE) surface(s);
    else if (id == P_INSIDE) s->olpa = 32768 - (int32_t)tx_exp2((15 << 16) - (int32_t)(tx_knob_exp(127u - v, 600u, 315000) * 862u >> 6));
    else rates(s);
}

static void rain_init(void *st, uint32_t seed)
{
    rain_t *s = (rain_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    s->rng_n = seed * 2654435761u + 77u;
    if (!s->rng_n) s->rng_n = 1;
    build_table(s);
    s->limit = 16;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((rain_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void rain_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((rain_t *)st)->vy);
    tx_cloud_key(&((rain_t *)st)->cloud, note, vel, on);
}

static void rain_param(void *st, uint8_t id, uint8_t val)
{
    rain_t *s = (rain_t *)st;
    if (id == TX_PID_PARTICLES) s->limit = (uint8_t)(val < 1u ? 1u : val > 32u ? 32u : val);
    else if (id == TX_PID_LEVEL) s->level = tx_level_q15(val > 127u ? 127u : val);
    else set_param(s, id, val);
}

/* one drop: a Mitchell pulse (table step, peak a, Q15) into the excitation at off */
static void pulse(rain_t *s, unsigned off, uint32_t step, int32_t a)
{
    uint32_t pos = 0;
    unsigned j = off;
    while (pos < ((uint32_t)TABN << 16) && j < TX_BLOCK + CARRY) {
        uint32_t k = pos >> 16, fr = (pos >> 8) & 255u;
        int32_t v = s->tab[k] + (((s->tab[k + 1] - s->tab[k]) * (int32_t)fr) >> 8);
        s->ex[j++] += (v * (a >> 4)) >> 3;                   /* Q15 x Q11 >> 3 -> Q23 */
        pos += step;
    }
}

TX_AUDIO_CODE static void rain_render(void *st, int32_t *mix, unsigned n)
{
    rain_t *s = (rain_t *)st;
    tx_rng_t *r = &s->cloud.rng;
    unsigned k, i, j, c, nexp = 0;
    int32_t *ex = s->ex, *buf = s->buf, any = 0;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) buf[j] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    for (i = 0; i < k; i++) {                                /* a press: one big drop near you (2 .. 6 mm by key) */
        const tx_event_t *e = &s->ev[i];
        int32_t dl2 = 718615 + (60 - (int32_t)e->note) * 3641 - (tx_vary_tone(&s->vy) >> 2);   /* brighter = smaller drops */   /* log2(2000 um), 2^(1/18) bigger per key down */
        uint32_t D = tx_exp2(dl2 < 718615 ? 718615 : dl2 > 822500 ? 822500 : dl2), L = 882u * D / 9100u / 10u;
        int32_t a = peak_of(D, 9100u) * e->vel * e->vel / 16129;   /* (<= 2^16 x 2^14: 32-bit) */
        a = (int32_t)(((int64_t)a * s->level) >> 15);
        pulse(s, e->offset, ((uint32_t)TABN << 16) / (L ? L : 1u), a);
    }
    {   /* rain: on while a key is held; released, the shower passes over ~3-4 s (session 5: no hard stop): the drop rate
     * thins out, the last drops sparse */
        unsigned held = tx_cloud_held(&s->cloud);
        s->wet += held ? (32768 - s->wet) >> 4 : -((s->wet >> 7) + 1);   /* (released: tau 0.74 s, the rate thins) */
        if (s->wet < 0) s->wet = 0;                          /* (released: it thins out to nothing, no last step) */
    }
    if (s->wet) {
        uint32_t x32 = s->rng_n;
        for (c = 0; c < NC; c++) {
            uint32_t lam = (uint32_t)(((uint64_t)s->lam_q8[c] * (uint32_t)s->wet) >> 15);
            uint32_t mu_q8 = (lam >> 4) * n / 2756u;           /* drops expected in this block, Q8 (fs / 16 = 2756) */
            int32_t a = (int32_t)(((int64_t)s->amp[c] * s->level) >> 15);
            uint32_t want = 0, bed_q8 = 0;
            if (mu_q8 >= (8u << 8)) bed_q8 = mu_q8;          /* dense: all into the bed */
            else if (mu_q8) {                                /* sparse: a Poisson count (exponential gaps in blocks) */
                uint32_t mean = 65536u / mu_q8, acc = tx_cloud_exp_q8(&s->cloud, mean);
                while (acc < 256u && want < 64u) { want++; acc += tx_cloud_exp_q8(&s->cloud, mean); }
                if (nexp + want > s->limit) {                /* over PARTICLES: the rest into the bed */
                    uint32_t over = nexp + want - s->limit;
                    bed_q8 = over << 8;
                    s->dropped = (uint16_t)(s->dropped + over);
                    want -= over;
                }
            }
            if (bed_q8) {                                    /* noise of the same power: sd^2 = drops/sample x 0.35 L a^2 */
                uint32_t dps_q16 = (bed_q8 << 8) / n;        /* drops per sample, Q16 */
                uint32_t var_q16 = (uint32_t)(((uint64_t)dps_q16 * s->len[c] * 90u) >> 8);   /* x 0.35 L */
                int32_t sd = (int32_t)(((uint64_t)(uint32_t)a * tx_exp2((log2_q16(var_q16 ? var_q16 : 1u) - (16 << 16)) / 2 + (16 << 16))) >> 16);
                int32_t lp = s->nlp[c], fa = s->fca[c] >> 2;
                sd = (int32_t)(((int64_t)sd * s->comp[c] * 443) >> 16);   /* x sqrt((2 - a) / a) (the low-pass) x sqrt 3 */
                if (sd > 32767) sd = 32767;
                for (j = 0; j < n; j++) {
                    int32_t x;
                    x32 ^= x32 << 13; x32 ^= x32 >> 17; x32 ^= x32 << 5;
                    x = (((int32_t)(x32 >> 17) - 16384) * sd) >> 14;   /* uniform, Q15 */
                    lp += ((x - lp) * fa) >> 13;
                    ex[j] += lp * 256;                       /* Q15 -> Q23 */
                }
                s->nlp[c] = lp;
            }
            for (i = 0; i < want; i++) {                     /* explicit pulses, sizes +-30 % within the class */
                int32_t ai = (int32_t)(((int64_t)a * (22938 + (int32_t)(tx_rand(r) >> 17))) >> 15);
                unsigned off = tx_rand_below(r, n);
                pulse(s, off, s->step[c], ai);
                if (s->surf == 0u && off + s->tg[c] + s->len[c] < TX_BLOCK + CARRY)   /* leaves: the rebound, x 0.2 */
                    pulse(s, off + s->tg[c], s->step[c], ai / 5);
            }
            nexp += want;
        }
        s->rng_n = x32 ? x32 : 1u;
    } else
        for (c = 0; c < NC; c++) s->nlp[c] = 0;
    for (j = 0; j < n; j++) any |= ex[j];
    /* the surface: 12 bands (+ the panel modes for glass and metal); a little of the drops' own splash */
    for (j = 0; j < n; j++) buf[j] += ex[j] >> 3;
    tx_svf_bank_add_g(s->band, s->bf, s->bq, s->bgin, s->bg, NB, ex, buf, n);
    if (s->surf >= 3u) tx_svf_bank_add_g(s->panel, s->pf, s->pq, s->pgin, s->pg, NPANEL, ex, buf, n);
    else for (i = 0; i < NPANEL; i++) s->panel[i].low = s->panel[i].band = s->panel[i].ef = 0;
    if (s->p[P_INSIDE]) {                                    /* inside: the low-pass (32-bit split products) */
        int32_t lp = s->olp, a = s->olpa;
        for (j = 0; j < n; j++) { int32_t d = buf[j] - lp; lp += MUL15(d, a); buf[j] = lp; }
        if (lp > -64 && lp < 64 && !any) lp = 0;
        s->olp = lp;
    } else s->olp = 0;
    for (j = 0; j < CARRY; j++) { ex[j] = ex[n + j]; ex[n + j] = 0; }   /* the pulses' tails */
    for (j = CARRY; j < n; j++) ex[j] = 0;
    tx_vary_block(&s->vy, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = s->hp.x1 | s->hp.y1 | s->wet | s->olp | !tx_svf_bank_quiet(s->band, NB) | !tx_svf_bank_quiet(s->panel, NPANEL);
        for (j = 0; j < CARRY; j++) z |= ex[j];
        for (c = 0; c < NC; c++) z |= s->nlp[c];
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
    s->nrain = (uint8_t)(nexp > 255u ? 255u : nexp);
}

static void rain_stats(const void *st, tx_stats_t *o)
{
    const rain_t *s = (const rain_t *)st;
    o->particles = s->nrain;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: a downpour of big drops on a metal roof (bands + panel modes), the explicit pulses at the limit */
static const char SCENE_MAX[] =
    "0 particles 32\n0 param RATE 127\n0 param DROPS 127\n0 param AREA 127\n0 param SURFACE 127\n0 param INSIDE 64\n"
    "0 on 53 127\n0 on 60 127\n0 on 67 127\n0 on 79 127\n1000 on 53 127\n2000 param AREA 40\n"
    "3000 off 53\n3000 off 60\n3000 off 67\n3000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_rainsurf = {
    "rainsurf", "#3 Rain on surfaces", NP, PARAMS, 32, 16, sizeof(rain_t), SCENE_MAX,
    rain_init, rain_key, rain_param, rain_render, rain_stats,
};
