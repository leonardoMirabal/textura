/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #3 Rain on surfaces: rain on leaves, a tent, a wooden deck or shingles, a window, a metal roof (research
 * notes 03, 02, 18).
 *
 * While a key is held it rains (velocity: heavier), release lets the shower pass (~3-4 s); a press also drops one big
 * drop near you (key row: lower = bigger drop, 2 .. 6 mm). Physics:
 *   drops    four size classes (0.7-1.2 / 1.2-2.4 / 2.4-4.1 / > 4.1 mm, L21) arriving per m^2 per s as note 02's table
 *            (Marshall-Palmer x Gunn-Kinzer, L15 / L16; log-log in R), times the listening AREA; DROPS shifts the size
 *            mix (drizzle .. convective: the small and big classes x 4^-+1, ours as note 02's Lambda knob).
 *   force    a drop pushes the surface for ~2 D / V with Mitchell's shape F = 3.596 t'^0.5 exp(-3.333 t'),
 *            t' = V t / D, peak 0.85 rho D^2 V^2 at 0.15 D / V, impulse = momentum (L29, L31 / L33): big drops are
 *            louder and darker (corner V / (2 pi 0.3 D)).
 *   every drop is its own event, never a noise bed (the user, 2026-10-25: dense rain must not become a flat hiss):
 *            each lands at its own time, size (+-0.4 octave of D inside its class: pulse length, corner and level) and
 *            place on the surface heard from about 0.5 m: over a disc of the AREA its level falls as 1 / distance,
 *            so the few nearest stand out and the mass behind is softer (p(r) ~ r). The nearest (inside 0.5 m) are full
 *            Mitchell pulses (PARTICLES per block); the others are impulses of the same momentum through their class's
 *            corner (the same low-frequency force, one sample each); past FAR_CAP a block the rest join them with
 *            their power (sqrt of the count), so the cost stays flat.
 *   splash   a drop on a hard wet surface throws a crown of droplets: a bright tick (differentiated, low-passed at the
 *            surface's corner), its share drawn per drop (0.25 .. 2 x: no two drops alike), bigger on wet wood and
 *            metal, little on fabric and leaves (ours, fitted to the references' upper octaves).
 *   surface  a roof or a window is modally dense: a log grid of 12 bands (150 Hz .. 7 kHz, half-octave, Q 2-5) whose
 *            gains are the surface's body (fitted per surface to the references: a tent's 0.3-2 kHz drum, a porch's
 *            boards, a window's coincidence near 2.7 kHz over its low panel, a metal roof's flat ring), plus 4
 *            low panel modes at the surface's own frequencies and Q (tent membrane 70-230 Hz; glass Q 120; steel Q 300).
 *            SURFACE moves between the five continuously (bands, modes, splash interpolated).
 *   leaves   a water-repellent leaf gets a second, weaker push from the rebounding jet at 0.44 tau_g,
 *            tau_g = sqrt(rho D^3 / gamma) (gain 0.2, measured < 0.37 when splashing), on the near drops.
 *   inside   heard from under the roof: a low-pass (~600 Hz .. open) (ours). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"

#define OUT_TRIM 8096               /* output gain, Q12 (x the surface's level): a key held 10 s at the defaults -22.7 LUFS
                                    * raw at LEVEL 100, as before the rework (note 18) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT */
#define VARY_LO -130
#define VARY_HI 15
#define VARY_TILT 160
#define VARY_DRIFT 40
#define VARY_SPLIT 3

enum { P_RATE, P_SURFACE, P_DROPS, P_AREA, P_INSIDE, NP };

static const tx_param_t PARAMS[NP] = {
    {"RATE", 127, 70, "rain rate R: 0.1 .. 100 mm/h (log): drizzle .. downpour"},
    {"SURFACE", 127, 64, "leaves | tent | wood (deck, shingles) | glass (window) | metal roof, continuous between"},
    {"DROPS", 127, 64, "drop sizes: drizzle (small drops) .. convective (big drops: louder, darker); above 64 also drips off eaves and leaves (4-5.5 mm)"},
    {"AREA", 127, 50, "area heard: 0.1 .. 20 m^2 (log): a few drops near you .. a whole roof (the near few stand out)"},
    {"INSIDE", 127, 0, "outside .. inside under the roof (low-passed)"},
};

#define NC 4u
#define NB 12u
#define NPANEL 4u
#define NSURF 5u
#define TABN 64u
#define CARRY 64u
#define NAMP 32u                   /* the distance law's quantiles */
#define FAR_CAP 48u                /* far drops per class per block, beyond: merged (power) */
#define A_REF 3800                 /* the biggest class's pulse peak (Q15 force units; calibrated by render) */

/* per class: arrivals per m^2 per s at R = 1, 5, 25, 100 mm/h (note 02, L15 x L16), x 100 */
static const uint32_t RATE_TAB[NC][4] = {
    {34000, 97700, 211100, 345000}, {7400, 42800, 159800, 378100}, {80, 1900, 19500, 89400}, {1, 20, 670, 7700}
};
static const uint16_t D_UM[NC] = { 950, 1700, 3100, 5000 };   /* representative diameters */
static const uint16_t V_MM[NC] = { 3850, 5800, 8100, 9100 };  /* terminal speeds (L16), mm/s */

/* the surfaces (note 18: fitted to the references' 1/3-octave spectra, dense rain; ours): band gains (dB, bands at
 * 150 Hz x 2^(i/2)), band Q x4, panel modes (Hz), their Q and gain (dB), the splash into the body and in the air
 * above it (dB, the air's corner in 100 Hz), the
 * drop's own push heard directly (dB), the leaf rebound (Q8), the surface's damping of the top (a 2-pole corner, 100 Hz),
 * its brightness over ~2 kHz (a shelf: 1 + k), its level (dB: every surface
 * equally loud at the defaults, by render) */
typedef struct {
    int8_t g[NB];
    uint8_t q4;
    uint16_t pf[NPANEL];
    uint16_t pq;
    int8_t pg, sp, air;
    uint8_t spc;
    int8_t dir;
    uint8_t reb, top, shelf;
    int8_t lvl;
} surf_t;
static const surf_t SURF[NSURF] = {
    /* leaves: soft ticks, little body, a light mid-high patter */
    {{-40, -19, -17, -31, -26, -30, -40, -40, 1, 20, 22, 13}, 10, {180, 260, 390, 560}, 6, -60, 26, -53, 55, -60, 51, 80, 7, 10},
    /* tent: the fabric drums 0.3-2 kHz, falls fast above 3 kHz; its membrane 70-230 Hz */
    {{-40, -26, -6, -20, -17, -18, -29, -20, 2, 12, -1, -35}, 8, {69, 105, 160, 230}, 6, -60, 21, -70, 50, -60, 0, 28, 3, 11},
    /* wood (boards, shingles): distinct drops on wood, its body 1-2 kHz (the porch, darker above: the cleanest sound) */
    {{7, -16, -11, -14, -14, -17, -27, -40, -40, -9, -1, 23}, 12, {120, 175, 250, 360}, 15, -60, 16, -9, 60, -60, 0, 90, 5, 10},
    /* glass (a window): its panel low, the coincidence near 2.7 kHz, dull above */
    {{19, 16, 12, -8, -2, -4, -3, -6, -1, -6, 15, -3}, 16, {64, 118, 172, 662}, 120, -51, 20, -40, 60, -60, 0, 140, 0, -5},
    /* metal roof: flat 0.2-4 kHz, a steel ring (Q 300) at its panel modes */
    {{-7, -3, -12, -12, -18, -24, -40, -40, 7, 8, 12, 0}, 20, {215, 436, 527, 2083}, 300, -60, 26, -59, 50, -60, 0, 60, 7, 7},
};

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    tx_svf_t band[NB], panel[NPANEL];
    int32_t bf[NB], bgin[NB], bg[NB], pf[NPANEL], pgin[NPANEL], pg[NPANEL], bq, pq;
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int16_t tab[TABN + 1];         /* Mitchell's pulse shape, Q15 peak */
    int16_t amp_t[NAMP];           /* the distance law: level at each quantile of the disc, Q15 */
    int32_t ex[TX_BLOCK + CARRY], buf[TX_BLOCK], fb[TX_BLOCK];   /* fb: a class's far impulses, then the splash */
    int32_t flp[NC];               /* the far drops' corner low-passes (Q15) */
    int32_t olp, olp2, olpa;       /* the top's damping and the inside: a 2-pole low-pass */
    int32_t sx1, slp, slp2, spa, spg, airg, dirg, senv, shlp, shk, otrim;
    uint32_t srng;   /* the splash: last input, its low-pass, its coefficient (Q15), gain (Q11); direct (Q11) */
    uint32_t lam_q8[NC];           /* arrivals per second over the AREA, Q8 */
    uint32_t drip_q8;              /* drips off the eaves / leaves per second, Q8 */
    uint32_t len[NC];              /* pulse length (samples) */
    int32_t amp[NC], fca[NC];      /* pulse peak (Q15), the corner as a one-pole (Q15) */
    uint32_t tg[NC];               /* leaves: the rebound delay, samples */
    int32_t wet;                   /* rain on (Q15 ramp) */
    uint32_t mtab;                 /* the pulse's mean over its length, Q15 (a far drop's momentum) */
    uint32_t top;                  /* the surface's top corner, Hz */
    uint8_t p[NP], limit, quiet, reb, near_n, nrain, pad[3];
    int32_t level;
    uint16_t dropped, pad2;
    tx_vary_t vy;
} rain_t;

#define MUL15(a, b) ((((a) >> 15) * (b)) + ((((a) & 0x7FFF) * (b)) >> 15))

static int32_t log2_q16(uint32_t x) { return (32 << 16) - (int32_t)(tx_neglog2_q12(x ? x : 1u) << 4); }
static int32_t db_q11(int32_t db10) { return (int32_t)tx_exp2((11 << 16) + db10 * 1089); }   /* tenths of a dB -> Q11 */
static uint32_t corner_a(uint32_t hz) { return 32768u - tx_exp2((15 << 16) - (int32_t)((hz > 20000u ? 20000u : hz) * 862u >> 6)); }

static void build_table(rain_t *s)
{
    unsigned i;
    int32_t peak = 1, v[TABN + 1];
    uint32_t sum = 0;
    for (i = 0; i <= TABN; i++) {                            /* t' = i / 32 (0 .. 2): F ~ sqrt(t') exp(-3.333 t') */
        uint32_t sq = i ? tx_exp2((log2_q16(i) - (5 << 16)) / 2 + (15 << 16)) : 0u;   /* sqrt(i / 32), Q15 */
        uint32_t ex = tx_exp2((15 << 16) - (int32_t)(i * 9849u));   /* exp(-3.333 i / 32) = 2^(-0.1503 i), Q15 */
        v[i] = (int32_t)((sq * ex) >> 15);
        if (v[i] > peak) peak = v[i];
    }
    for (i = 0; i <= TABN; i++) { s->tab[i] = (int16_t)(v[i] * 32767 / peak); sum += (uint32_t)s->tab[i]; }
    s->tab[TABN] = 0;
    s->mtab = sum / TABN;
}

/* the surface now: rows m and m + 1 mixed by fr (Q7) */
static void surface(rain_t *s)
{
    unsigned i, v = s->p[P_SURFACE] * 4u, m = v >> 7, fr = v & 127u;
    const surf_t *a = &SURF[m], *b = &SURF[m + 1u < NSURF ? m + 1u : m];
    int32_t q4 = (a->q4 * (128 - (int32_t)fr) + b->q4 * (int32_t)fr) >> 7;
    int32_t lq = (log2_q16(a->pq) * (128 - (int32_t)fr) + log2_q16(b->pq) * (int32_t)fr) >> 7;
    uint32_t pq = tx_exp2(lq);
#define MIX(x, y) (((int32_t)(x) * (128 - (int32_t)fr) + (int32_t)(y) * (int32_t)fr) >> 7)
    s->bq = (int32_t)(65536u / (uint32_t)(q4 < 4 ? 4 : q4));          /* 16384 / Q */
    for (i = 0; i < NB; i++) {
        uint32_t hz = tx_exp2(473750 + (int32_t)i * 32768);  /* 150 Hz x 2^(i/2) .. 6.8 kHz */
        s->bf[i] = tx_svf_coef(hz);
        s->bgin[i] = (int32_t)tx_svf_gin_noise(hz, (uint32_t)q4 * 4u);
        s->bg[i] = db_q11(MIX(a->g[i], b->g[i]) * 10 + ((int32_t)i - 9) * 30);   /* + 6 dB / oct: a plate radiates
                                                              * better as f rises (below coincidence), then the surface */
    }
    s->pq = (int32_t)(16384u / (pq < 2u ? 2u : pq));
    for (i = 0; i < NPANEL; i++) {
        uint32_t hz = tx_exp2(MIX(log2_q16(a->pf[i]) >> 4, log2_q16(b->pf[i]) >> 4) << 4);
        s->pf[i] = tx_svf_coef(hz);
        s->pgin[i] = (int32_t)tx_svf_gin_noise(hz, pq * 16u);
        s->pg[i] = db_q11(MIX(a->pg, b->pg) * 10 - (int32_t)i * 15 + 120);
    }
    s->spg = db_q11(MIX(a->sp, b->sp) * 10);
    {   /* the air path runs on the splash already at the bands' scale: its gain re the splash's, Q15 (x 4: the scales) */
        int32_t l = (17 << 16) + (MIX(a->air, b->air) - MIX(a->sp, b->sp)) * 10890;   /* log2 Q16 */
        int32_t r = l >= (15 << 16) ? 32767 : (int32_t)tx_exp2(l);
        s->airg = r > 32767 ? 32767 : r;
    }
    s->spa = (int32_t)corner_a((uint32_t)MIX(a->spc, b->spc) * 100u);
    s->dirg = db_q11(MIX(a->dir, b->dir) * 10) * 64;          /* Q15 (the push heard directly, re the bands' input) */
    if (s->dirg > 32767) s->dirg = 32767;
    s->reb = (uint8_t)MIX(a->reb, b->reb);
    s->top = (uint32_t)MIX(a->top, b->top) * 100u;
    s->shk = MIX(a->shelf, b->shelf);
    s->otrim = (int32_t)(((int64_t)OUT_TRIM * db_q11(MIX(a->lvl, b->lvl) * 10)) >> 11);
    if (s->otrim > 65535) s->otrim = 65535;
#undef MIX
}

/* a pulse's peak from D and V: A_REF sqrt((D / 5 mm) (V / 9.1 m/s)): the force F_max = 0.85 rho D^2 V^2, compressed to
 * its fourth root (ours: the references' drops vary 1-5 dB, crest 10-13 dB; the D^2 V^2 law gave a 44 dB spread) */
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
        uint32_t L, fc;
        k = lr < LR[1] ? 0u : lr < LR[2] ? 1u : 2u;
        l0 = log2_q16(RATE_TAB[c][k]); l1 = log2_q16(RATE_TAB[c][k + 1]);
        lv = l0 + ((((l1 - l0) >> 4) * ((lr - LR[k]) >> 4)) / ((LR[k + 1] - LR[k]) >> 8));   /* log-log, 32-bit */
        lv += la - 435412 + (8 << 16);                       /* / 100 (the table), x area, Q8 */
        lv += c >= 2u ? shift : -shift / 2;                  /* convective: more big drops, fewer small */
        s->lam_q8[c] = lv > (26 << 16) ? (1u << 26) : lv < 0 ? 0u : tx_exp2(lv);
        L = 882u * D_UM[c] / V_MM[c] / 10u;                  /* 2 D / V in samples */
        s->len[c] = L ? L : 1u;
        fc = (uint32_t)V_MM[c] * 531u / D_UM[c];             /* V / (2 pi 0.3 D): (mm/s / um) x 1e3 / 1.885 */
        s->fca[c] = (int32_t)corner_a(fc);
        s->amp[c] = peak_of(D_UM[c], V_MM[c]);
        s->tg[c] = tx_exp2((3 * log2_q16(D_UM[c])) / 2 - 574900);   /* 0.44 tau_g = 0.002287 D_um^1.5 samples */
    }
    {   /* drips: water caught above (eaves, gutters, leaves) falls in big drops (Tate: ~4.5 mm off an edge), 0 .. 3 /s
         * by DROPS over 64 x (0.3 + R / 25 mm/h, at most 1.3): the porch's and the window's single drops (ours) */
        int32_t d = (int32_t)s->p[P_DROPS] - 64;
        uint32_t rr = lr > 304340 ? 256u : tx_exp2(lr - 304340 + (8 << 16));   /* R / 25, Q8 */
        s->drip_q8 = d > 0 ? (uint32_t)(d * d) * 768u / 3969u * (77u + rr) >> 8 : 0u;
    }
    {   /* the distance law over the AREA heard from h = 0.5 m: a(u) = 1 / sqrt(1 + (A / pi h^2) u), u the disc's
         * quantile (p(r) ~ r); the nearest (r < h: u < pi h^2 / A) are the full pulses */
        int32_t lk = la + 22605;                             /* log2(A / (pi 0.25)) = log2 A + 0.345 */
        uint32_t kq8 = lk > (20 << 16) ? (1u << 28) : tx_exp2(lk + (8 << 16));   /* k, Q8 */
        unsigned i;
        for (i = 0; i < NAMP; i++) {
            uint32_t x = 256u + (uint32_t)(((uint64_t)kq8 * (2u * i + 1u)) >> 6);   /* 1 + k u, Q8 (/ 2 NAMP) */
            s->amp_t[i] = (int16_t)tx_exp2((15 << 16) - (log2_q16(x) - (8 << 16)) / 2);
            if (s->amp_t[i] < 0) s->amp_t[i] = 32767;
        }
        i = kq8 > 256u ? (uint32_t)((NAMP << 8) / kq8) : NAMP;   /* quantiles inside r < h */
        s->near_n = (uint8_t)(i < 1u ? 1u : i);
    }
}

static void set_param(rain_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    if (id == P_SURFACE || id == P_INSIDE) {                 /* the top: the surface's damping, or inside the roof */
        uint32_t in = tx_knob_exp(127u - s->p[P_INSIDE], 600u, 315000);   /* 600 Hz .. 16.6 kHz */
        if (id == P_SURFACE) surface(s);
        s->olpa = (int32_t)corner_a(in < s->top ? in : s->top);
    } else rates(s);
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
    build_table(s);
    s->srng = seed * 2654435761u + 77u;
    if (!s->srng) s->srng = 1u;
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

/* one drop: a Mitchell pulse (length len samples, peak a, Q15) into the excitation at off */
static void pulse(rain_t *s, unsigned off, uint32_t len, int32_t a)
{
    uint32_t pos = 0, step = ((uint32_t)TABN << 16) / (len ? len : 1u);
    unsigned j = off;
    while (pos < ((uint32_t)TABN << 16) && j < TX_BLOCK + CARRY) {
        uint32_t k = pos >> 16, fr = (pos >> 8) & 255u;
        int32_t v = s->tab[k] + (((s->tab[k + 1] - s->tab[k]) * (int32_t)fr) >> 8);
        s->ex[j++] += (v * (a >> 4)) >> 5;                   /* Q15 x Q11 >> 5 -> Q21 (the bands' input: Q23 / 4) */
        pos += step;
    }
}

/* a drop's splash: 0.25 .. 2.8 x its push, log-uniform (one drop thuds, the next splashes bright: the references'
 * drops spread 1-2.6 octaves in centroid); its crown's droplets land over ~1 ms (the spray: below) */
static void splash(rain_t *s, uint32_t r, unsigned off, int32_t a)
{
    int32_t sh = (int32_t)tx_exp2((int32_t)((r >> 22) & 1023u) * 224 + (8 << 16));   /* 2^-2 .. 2^1.5, Q10 (log-uniform) */
    int32_t v = (a * sh) >> 2;                               /* (Q15 x Q10 >> 2 -> Q23: as a push's peak) */
    (void)r;
    s->fb[off] += (int32_t)(((int64_t)v * s->spg) >> 13);    /* into the bands' scale (Q21) */
}

TX_AUDIO_CODE static void rain_render(void *st, int32_t *mix, unsigned n)
{
    rain_t *s = (rain_t *)st;
    tx_rng_t *r = &s->cloud.rng;
    unsigned k, i, j, c, nexp = 0;
    int32_t *ex = s->ex, *buf = s->buf, *fb = s->fb, any = 0;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) buf[j] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet && !tx_cloud_held(&s->cloud)) return;   /* silent and nothing new: nothing to compute */
    for (i = 0; i < k; i++) {                                /* a press: one big drop near you (2 .. 6 mm by key) */
        const tx_event_t *e = &s->ev[i];
        int32_t dl2 = 718615 + (60 - (int32_t)e->note) * 3641 - (tx_vary_tone(&s->vy) >> 2);   /* brighter = smaller drops */
        uint32_t D = tx_exp2(dl2 < 718615 ? 718615 : dl2 > 822500 ? 822500 : dl2), L = 882u * D / 9100u / 10u;
        int32_t a = peak_of(D, 9100u) * e->vel * e->vel / 16129;   /* (<= 2^16 x 2^14: 32-bit) */
        a = (int32_t)(((int64_t)a * s->level) >> 15);
        pulse(s, e->offset, L, a);
    }
    {   /* rain: on while a key is held; released, the shower passes over ~3-4 s (session 5: no hard stop) */
        unsigned held = tx_cloud_held(&s->cloud);
        s->wet += held ? (32768 - s->wet) >> 4 : -((s->wet >> 7) + 1);   /* (released: tau 0.74 s, the rate thins) */
        if (s->wet < 0) s->wet = 0;
    }
    for (j = 0; j < n; j++) fb[j] = 0;
    if (s->wet) {
        for (c = 0; c < NC; c++) {
            uint32_t lam = (uint32_t)(((uint64_t)s->lam_q8[c] * (uint32_t)s->wet) >> 15);
            uint32_t mu_q8 = (lam >> 4) * n / 2756u;           /* drops expected in this block, Q8 (fs / 16 = 2756) */
            int32_t a = (int32_t)(((int64_t)s->amp[c] * s->level) >> 15);
            int32_t mom = (int32_t)(((uint64_t)(uint32_t)a * s->len[c] * s->mtab) >> 15);   /* a far drop's momentum (Q15 x samples) */
            int32_t fg = 256, lp, fa = s->fca[c];
            uint32_t want = 0, far = 0;
            if (mu_q8 >= (8u << 8)) want = (mu_q8 + (tx_rand(r) >> 24)) >> 8;   /* dense: the mean (a Poisson's spread is small) */
            else if (mu_q8) {                                /* sparse: a Poisson count (exponential gaps in blocks) */
                uint32_t mean = 65536u / mu_q8, acc = tx_cloud_exp_q8(&s->cloud, mean);
                while (acc < 256u && want < 64u) { want++; acc += tx_cloud_exp_q8(&s->cloud, mean); }
            }
            if (!want && !s->flp[c]) continue;
            if (want > FAR_CAP) {                            /* past the cap: the rest join with their power */
                fg = (int32_t)tx_exp2((log2_q16((want << 8) / FAR_CAP) - (8 << 16)) / 2 + (8 << 16));   /* sqrt, Q8 */
                s->dropped = (uint16_t)(s->dropped + (want - FAR_CAP));
                want = FAR_CAP;
            }
            for (i = 0; i < want; i++) {
                uint32_t x = tx_rand(r), y = tx_rand(r);
                unsigned off = ((x >> 16) * n) >> 16, u = x & (NAMP - 1u);
                int32_t sz = (int32_t)tx_exp2((int32_t)((x >> 5) & 32767u) * 4 + (14 << 16));   /* its own size: 0.5 .. 2 x its class (log) */
                int32_t ai = (int32_t)(((int64_t)a * sz) >> 15), ad = (ai * s->amp_t[u]) >> 15;
                if (u < s->near_n && nexp < s->limit) {      /* near: a full pulse of its own length */
                    uint32_t len = (s->len[c] * (uint32_t)(sz >> 7)) >> 8;
                    if (len > CARRY) len = CARRY;
                    pulse(s, off, len, ad);
                    if (s->reb && off + s->tg[c] + len < TX_BLOCK + CARRY)   /* leaves: the rebound */
                        pulse(s, off + s->tg[c], len, (ad * s->reb) >> 8);
                    nexp++;
                } else {                                     /* far: its momentum at once, through the corner below */
                    int32_t m = (int32_t)(((int64_t)mom * sz) >> 15);
                    m = (int32_t)(((int64_t)m * s->amp_t[u] * fg) >> 23);
                    buf[off] += m;                           /* (buf: this class's impulses, Q15) */
                    far++;
                }
                splash(s, y, off, (ad * fg) >> 8);
            }
            lp = s->flp[c];
            for (j = 0; j < n; j++) {                        /* the far drops through their class's corner */
                int32_t d = buf[j] - lp;
                lp += MUL15(d, fa);
                ex[j] += lp * 64;                            /* Q15 -> Q21 */
                buf[j] = 0;
            }
            if (!far && lp > -16 && lp < 16) lp = 0;
            s->flp[c] = lp;
        }
        if (s->drip_q8) {                                    /* the drips: each its own size, fall and place */
            uint32_t mu_q8 = (uint32_t)(((uint64_t)s->drip_q8 * (uint32_t)s->wet * n) >> 15), d = 0;
            mu_q8 /= 44100u;
            if (mu_q8) {
                uint32_t mean = 65536u / mu_q8, acc = tx_cloud_exp_q8(&s->cloud, mean);
                while (acc < 256u && d < 4u) {
                    uint32_t x = tx_rand(r), y = tx_rand(r);
                    uint32_t D = 4000u + ((x >> 20) & 1023u) * 3u / 2u, V = 3000u + ((x >> 8) & 4095u);   /* 4-5.5 mm, 3-7 m/s */
                    uint32_t L = 882u * D / V / 10u;
                    int32_t a = (int32_t)(((int64_t)peak_of(D, V) * s->level * (int32_t)(V >> 6)) >> 21);   /* x 4 V / 4 m/s: a drip
                                                              * is near and big, its push 20-30 dB over a raindrop's (D^2 V^2) */
                    unsigned off = (x & 255u) * n >> 8;
                    a = (a * s->amp_t[(x >> 3) & 15u]) >> 15;    /* nearer half of the area */
                    pulse(s, off, L > CARRY ? CARRY : L, a);
                    splash(s, y, off, a * 2);                    /* a big crown: a drip cracks on a wet board */
                    nexp++; d++;
                    acc += tx_cloud_exp_q8(&s->cloud, mean);
                }
            }
        }
    } else
        for (c = 0; c < NC; c++) s->flp[c] = 0;
    {   /* the splashes' spray: each crown's droplets land over ~0.7 ms (an envelope of tiny random impacts, decaying
         * 1/32 a sample), never a single-sample click; nothing between the drops */
        int32_t e = s->senv;
        uint32_t x = s->srng;
        for (j = 0; j < n; j++) {
            e += fb[j] - (e >> 5);
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            fb[j] = (((int32_t)(x >> 16) - 32768) * (e >> 13)) >> 2;   /* (32-bit: |e| < 2^28) */
        }
        if (e < 32) e = 0;
        s->senv = e; s->srng = x;
    }
    /* the surface: 12 bands + its panel modes, rung by the pushes and the splashes; the push heard directly; the
     * splash's droplets in the air above the bands */
    for (j = 0; j < n; j++) {
        buf[j] = MUL15(ex[j], s->dirg);
        ex[j] += fb[j];
        any |= ex[j];
    }
    tx_svf_bank_add_g(s->band, s->bf, s->bq, s->bgin, s->bg, NB, ex, buf, n);
    tx_svf_bank_add_g(s->panel, s->pf, s->pq, s->pgin, s->pg, NPANEL, ex, buf, n);
    {   /* a shelf: up to +24 dB over ~2 kHz (x + k (x - lp)), so the bands' own gains stay in their range (the surfaces'
         * upper octaves are 20-50 dB over a raindrop's force spectrum there) */
        int32_t lp = s->shlp, k = s->shk;
        for (j = 0; j < n; j++) { lp += MUL15(buf[j] - lp, 8960); buf[j] += k * (buf[j] - lp); }
        if (!any && lp > -64 && lp < 64) lp = 0;
        s->shlp = lp;
    }
    {
        int32_t x1 = s->sx1, lp = s->slp, lp2 = s->slp2, a = s->spa, g = s->airg;   /* (the air re the splash, Q15) */
        for (j = 0; j < n; j++) {                            /* d/dt (the crown's droplets: bright), then the corner (2 poles) */
            int32_t d = fb[j] - x1;
            x1 = fb[j];
            lp += MUL15(d - lp, a);
            lp2 += MUL15(lp - lp2, a);
            buf[j] += MUL15(lp2, g);
            any |= x1;
        }
        if (!any && lp > -64 && lp < 64 && lp2 > -64 && lp2 < 64) lp = lp2 = 0;
        s->sx1 = x1; s->slp = lp; s->slp2 = lp2;
    }
    {   /* the top's damping / inside: 2 poles (32-bit split products) */
        int32_t lp = s->olp, lp2 = s->olp2, a = s->olpa;
        for (j = 0; j < n; j++) { lp += MUL15(buf[j] - lp, a); lp2 += MUL15(lp - lp2, a); buf[j] = lp2; }
        if (lp > -64 && lp < 64 && lp2 > -64 && lp2 < 64 && !any) lp = lp2 = 0;
        s->olp = lp; s->olp2 = lp2;
    }
    for (j = 0; j < CARRY; j++) { ex[j] = ex[n + j]; ex[n + j] = 0; }   /* the pulses' tails */
    for (j = CARRY; j < n; j++) ex[j] = 0;
    tx_vary_block(&s->vy, buf, n);
    tx_trim(buf, n, s->otrim);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = s->hp.x1 | s->hp.y1 | s->wet | s->olp | s->olp2 | s->slp | s->slp2 | s->sx1 | s->senv | s->shlp | !tx_svf_bank_quiet(s->band, NB)
                    | !tx_svf_bank_quiet(s->panel, NPANEL);
        for (j = 0; j < CARRY; j++) z |= ex[j];
        for (c = 0; c < NC; c++) z |= s->flp[c];
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

/* the worst case: a downpour of big drops on a metal roof (bands + panel modes), the near pulses at the limit */
static const char SCENE_MAX[] =
    "0 particles 32\n0 param RATE 127\n0 param DROPS 127\n0 param AREA 127\n0 param SURFACE 127\n0 param INSIDE 64\n"
    "0 on 53 127\n0 on 60 127\n0 on 67 127\n0 on 79 127\n1000 on 53 127\n2000 param AREA 40\n"
    "3000 off 53\n3000 off 60\n3000 off 67\n3000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_rainsurf = {
    "rainsurf", "#3 Rain on surfaces", NP, PARAMS, 32, 16, sizeof(rain_t), SCENE_MAX,
    rain_init, rain_key, rain_param, rain_render, rain_stats,
};
