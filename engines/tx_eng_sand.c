/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #14 Sand: sand pouring onto a hand, a pile, a tray, a glass bulb, into a tube (
 *).
 *
 * Holding a key pours (velocity = how much), release stops the flow; the grains still in the air land after
 * sqrt(2 h / g). Physics:
 *   flow     Beverloo: W = C rho_b sqrt(g) (D0 - k d)^(5/2), C 0.58, rho_b 1500, k 2.9 (sand) (G16), so the grain rate
 *            is lambda = W / m_grain = 1.963 (D0 - 2.9 d)^2.5 / d^3 per second (SI): 0.2 mm sand through a 3 mm neck
 *            ~7e4 /s (note 04). The orifice jams below D0 = 2.9 d (silence).
 *   impacts  every grain's impact is a click (grains ring in the MHz: their sound is the click's low-frequency tail);
 *            amplitude ~ d^1.5 v (sqrt of the impact energy), so the power lambda a^2 ~ the kinetic energy flux W v^2:
 *            fine sand hisses, coarse sand crackles, at the same loudness. A shot train: Bernoulli clicks per sample
 *            while lambda < fs / 4, white noise of the same variance above (Campbell's theorem).
 *   surface  ordinary sand has no tone of its own (G22: Ottawa sand and glass beads show no peak): the colour comes
 *            from what it lands on: a hand (soft, 3 kHz low-pass), a pile (1.2 kHz), a metal tray (4 modes, Q 300),
 *            a glass bulb (4 modes, Q 500) (ours: no measured spectra), or a tube: a quarter-wave air column
 *            (closed-open: odd harmonics of c / 4L; an inverting comb with a damped loop) driven by the flow
 *            (granular flow couples to the tube's modes: G24 / G25).
 *   pulse    narrow-tube flow is intermittent at ~4 Hz (G21): an optional flow modulation.
 * Keys: the container size (tray / bulb modes, tube length) at C4 = SIZE, a semitone per key.
 * Realism pass (sand-pouring references, analysis only): a pour is lumpy (rms std 3-17 dB, 0.5-1 Hz swings), clumped
 * (kurtosis 1-25) and bright (energy at 3-10 kHz, little under 1 kHz). Ours was a steady white hiss (rms std 0.2 dB,
 * kurtosis -0.5, flat to 30 Hz). So: the flow rate wanders log-normally (+-0.6 oct slow, +-0.4 oct fast), the dense
 * noise comes in 0.7 ms clumps of random gain (2^(-1.5 .. 0.5)), and the dry path is a band (hand 1.5-6 kHz, pile
 * 0.8-4 kHz: one-pole high-pass, two-pole low-pass) instead of a 3 kHz low-pass (ours, set against the references). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"

#define OUT_TRIM 4100               /* output gain, Q12 (-2.4 dB; -1.5 with note 17's swings past the cap): integrated (a key held 10 s): -25.1 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +2.4 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 120
#define VARY_DRIFT 40
#define VARY_SPLIT 0

enum { P_GRAIN, P_ORIFICE, P_SURFACE, P_SIZE, P_HEIGHT, P_PULSE, NP };

static const tx_param_t PARAMS[NP] = {
    {"GRAIN", 127, 40, TX_TEXT("grain diameter d: 0.1 .. 1 mm (log): fine hiss .. coarse crackle (same loudness)")},
    {"ORIFICE", 127, 50, TX_TEXT("orifice / neck D0: 1 .. 20 mm (log): Beverloo flow (D0 - 2.9 d)^2.5, jams below 2.9 d")},
    {"SURFACE", 127, 0, TX_TEXT("landing on: hand | sand pile | metal tray | glass bulb | tube (quarter-wave)")},
    {"SIZE", 127, 64, TX_TEXT("container size at C4: tray / bulb x 0.5 .. 2, tube length 0.1 .. 1 m (keys: a semitone each)")},
    {"HEIGHT", 127, 50, TX_TEXT("fall height: 1 cm .. 1 m (log): impact speed sqrt(2 g h): louder, longer to stop")},
    {"PULSE", 127, 0, TX_TEXT("intermittent flow: steady .. pulsing (~4 Hz, narrow tubes)")},
};

#define NV 4u
#define NM 6u
#define DMAX 512u                  /* tube delay line (2L / c: 1 m = 257 samples) */
#define A_REF 2700                 /* noise amplitude at the reference flow (Q15; calibrated by render) */

typedef struct { uint16_t hz[NM]; uint16_t q; uint8_t nm, dry; } surface_t;
static const surface_t SURF[5] = {
    {{0}, 0, 0, 255},                                       /* hand */
    {{0}, 0, 0, 255},                                       /* pile */
    {{400, 690, 1060, 1450, 0, 0}, 300, 4, 80},            /* metal tray (ours; 4 modes: the CPU rule) */
    {{1100, 1870, 2650, 3480, 0, 0}, 500, 4, 80},          /* glass bulb (ours) */
    {{0}, 0, 0, 60},                                        /* tube */
};

typedef struct {
    tx_svf_t body[NM];
    int32_t f[NM], gin[NM], g[NM];
    int32_t line[DMAX];
    uint32_t wpos, dly_q8;         /* tube: write position, delay (samples Q8) */
    int32_t tlp, lp, lp2, hp;      /* tube loop low-pass, dry low-pass (two poles), dry high-pass state */
    int32_t lump, flick;           /* flow irregularity: slow (~1 Hz) and fast (~7 Hz) random log-rate (Q15) */
    int32_t flow;                  /* Q15 ramp */
    int32_t sg, sgt;               /* the slug now: gain (Q15, ramped per block) and its target */
    uint16_t sleft, phold;         /* blocks left in this slug (or gap); blocks the flow runs unpulsed */
    uint32_t pph;                  /* pulse phase */
    uint16_t pmul, pdg;            /* this pulse's period (the step x pmul) and depth, Q15 (0.71 .. 1.41, 0.3 .. 1.3) */
    int32_t stop;                  /* blocks the grains in the air still take to land */
    uint8_t em, note, vel, alive;
    uint8_t c_surf, c_size, c_note, c_ok;   /* the surface computed for (SURFACE, SIZE, note) */
} pour_t;

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    pour_t v[NV];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t x[TX_BLOCK], buf[TX_BLOCK];
    uint8_t p[NP], limit, quiet;
    int32_t level, d_l2, d0_l2, size_l2, h_l2, pulse;
    uint16_t dropped;
    tx_vary_t vy;
} sand_t;

static int32_t log2_q16(uint32_t x) { return (32 << 16) - (int32_t)(tx_neglog2_q12(x) << 4); }

static void set_param(sand_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_GRAIN: s->d_l2 = 435412 + (int32_t)(v * 1714u); break;         /* log2(100 um) + v log2(10) / 127 */
    case P_ORIFICE: s->d0_l2 = 653124 + (int32_t)(v * 2230u); break;      /* log2(1000 um) + v log2(20) / 127 */
    case P_SIZE: s->size_l2 = -65536 + (int32_t)(v * 1032u); break;        /* x 0.5 .. 2 */
    case P_HEIGHT: s->h_l2 = -435412 + (int32_t)(v * 3428u); break;        /* log2(h / 1 m): 0.01 .. 1 m */
    case P_PULSE: s->pulse = (int32_t)v * 206; break;                      /* depth 0 .. 0.8 */
    default: break;
    }
}

static void sand_init(void *st, uint32_t seed)
{
    sand_t *s = (sand_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    for (i = 0; i < NV; i++) s->v[i].em = 0xFF;
    s->limit = 3;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((sand_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void sand_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((sand_t *)st)->vy);
    tx_cloud_key(&((sand_t *)st)->cloud, note, vel, on);
}

static void clear(pour_t *v)
{
    unsigned i;
    for (i = 0; i < NM; i++) v->body[i].low = v->body[i].band = v->body[i].ef = 0;
    for (i = 0; i < DMAX; i++) v->line[i] = 0;
    v->tlp = v->lp = v->lp2 = v->hp = 0; v->flow = 0; v->alive = 0; v->em = 0xFF; v->c_ok = 0;
}

static void sand_param(void *st, uint8_t id, uint8_t val)
{
    sand_t *s = (sand_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NV ? NV : val);
        for (i = s->limit; i < NV; i++) clear(&s->v[i]);
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

static void pour(sand_t *s, const tx_event_t *e)
{
    pour_t *v = 0;
    unsigned i;
    for (i = 0; i < s->limit && !v; i++) if (s->v[i].alive && s->v[i].note == e->note) v = &s->v[i];
    for (i = 0; i < s->limit && !v; i++) if (!s->v[i].alive) v = &s->v[i];
    if (!v) { s->dropped++; return; }
    if (!v->alive) { clear(v); v->pph = tx_rand(&s->cloud.rng); v->pmul = v->pdg = 32768; v->phold = 0; }
    v->alive = 1; v->em = e->emitter; v->note = e->note; v->vel = e->vel;
}

/* per block: the surface's modes or the tube for this key */
static void surface(sand_t *s, pour_t *v)
{
    if (v->c_ok && v->c_surf == s->p[P_SURFACE] && v->c_size == s->p[P_SIZE] && v->c_note == v->note) return;
    v->c_ok = 1; v->c_surf = s->p[P_SURFACE]; v->c_size = s->p[P_SIZE]; v->c_note = v->note;
    {
    const surface_t *sf = &SURF[(s->p[P_SURFACE] * 5u) >> 7];
    int32_t k_l2 = s->size_l2 - ((int32_t)v->note - 60) * 5461;   /* bigger = lower */
    unsigned i;
    for (i = 0; i < NM; i++) {
        uint32_t hz = i < sf->nm ? (uint32_t)(((uint64_t)sf->hz[i] * tx_exp2((16 << 16) - k_l2)) >> 16) : 0;
        v->f[i] = hz ? tx_svf_coef(hz) : 0;
        v->gin[i] = hz ? (int32_t)tx_svf_gin_noise(hz, sf->q * 16u) : 0;
        v->g[i] = hz ? 1024 : 0;
    }
    /* tube: L = 0.316 m x 2^k (0.1 .. 1 m over SIZE), delay 2L / c samples */
    {
        uint32_t L_mm = tx_exp2(k_l2 * 5 / 3 + 544200);      /* 316 mm x 2^(1.66 k): 0.1 .. 1 m over SIZE */
        uint32_t d = L_mm * 66u;                             /* 2 L / c x fs x 256 = L_mm x 65.8 (Q8 samples) */
        v->dly_q8 = d < (8u << 8) ? (8u << 8) : d > ((DMAX - 2u) << 8) ? ((DMAX - 2u) << 8) : d;
    }
    }
}

/* the slugs' power compensation (log2, Q16) at intermittency I (Q15): 0.5 log2(1 + 8 I ln 2) for the gains, and
 * 0.5 log2(1 / (1 - share of jams)) for the gaps (their share ~ I / 2 x ~1.5 / (1 + I / 2), ours: ~ I / 3) */
static int32_t slug_comp(int32_t I)
{
    uint32_t x = 65536u + (uint32_t)(((int64_t)I * 363409) >> 15);   /* 1 + 5.545 I, Q16 */
    uint32_t y = 65536u - (uint32_t)(I * 2 / 3);                     /* 1 - I / 3, Q16 */
    int32_t a = (int32_t)(16u << 16) - (int32_t)(tx_neglog2_q12(x) << 4);   /* log2 x */
    int32_t b = (int32_t)(16u << 16) - (int32_t)(tx_neglog2_q12(y) << 4);   /* log2 y (< 0) */
    return (a - b) / 2;
}

/* a clump's gain (Q15 amplitude a scaled): grains fall in clusters of every size (a power law: most clumps a few grains,
 * now and then a lump), so the pour ticks and rustles instead of hissing (the user, 2026-10-25: no flat hiss; the
 * references' level spread 8-10 dB, ours was 1-2). 2^(-3 E), E exponential (capped at 16 / 3), +1.2 octaves: the mean
 * power kept (E[2^(-6 E)] = 1 / (1 + 6 ln 2)); the loudest +1.2 octaves, the median -0.9 */
static int32_t clump(uint32_t *x, int32_t a)
{
    uint32_t u, e;
    *x ^= *x << 13; *x ^= *x >> 17; *x ^= *x << 5;
    u = *x | 1u;
    e = tx_neglog2_q12(u);                                   /* -log2 u, Q12: E / ln 2 */
    e = (e * 2839u) >> 12;                                   /* x ln 2: E, Q12 */
    if (e > 21845u) e = 21845u;
    return (int32_t)(((int64_t)a * (int32_t)tx_exp2(77300 - (int32_t)(e * 48u) + (16 << 16))) >> 16);   /* 3 E x 16 = Q16 */
}

TX_AUDIO_CODE static void sand_render(void *st, int32_t *mix, unsigned n)
{
    sand_t *s = (sand_t *)st;
    unsigned k, i, j;
    int32_t *x = s->x, *buf = s->buf;
    uint16_t d0 = s->dropped;
    const surface_t *sf = &SURF[(s->p[P_SURFACE] * 5u) >> 7];
    int tube = (s->p[P_SURFACE] * 5u) >> 7 == 4u;
    /* the dry path: grain clicks are bright (references: energy at 3-10 kHz, little under 1 kHz): a one-pole high-pass
     * (pile 0.8 kHz, hand 1.5 kHz) and a two-pole low-pass (pile 4 kHz, hand 6 kHz) */
    int32_t lpa = (s->p[P_SURFACE] * 5u) >> 7 == 1u ? 14250 : 18850;   /* Q15: 1 - exp(-2 pi fc / fs) */
    int32_t hpa = (s->p[P_SURFACE] * 5u) >> 7 == 1u ? 3700 : 6300;
    {   /* the grains' click is shorter, so brighter, for finer grains and faster impacts (Hertz: contact time ~ R v^-1/5;
         * ours: the band's corners x (d / 200 um)^-0.8 x (h / 0.1 m)^0.1, +-1.5 octaves: a fine sandfall from high ~12 kHz,
         * coarse grit poured low ~3 kHz; the references 4.4 .. 11.8 kHz) */
        int32_t k = -(((s->d_l2 - 500953) * 4) / 5) + (s->h_l2 + 217706) / 10;
        k = k > 98304 ? 98304 : k < -98304 ? -98304 : k;
        lpa = (int32_t)(((int64_t)lpa * (int32_t)tx_exp2(k + (16 << 16))) >> 16);
        hpa = (int32_t)(((int64_t)hpa * (int32_t)tx_exp2(k + (16 << 16))) >> 16);
        lpa = lpa > 30000 ? 30000 : lpa;                     /* (a one-pole tops out near fs / 4) */
        hpa = hpa > 24000 ? 24000 : hpa;
    }
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) buf[j] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    for (i = 0; i < k; i++) pour(s, &s->ev[i]);
    for (i = 0; i < NV; i++) {
        pour_t *v = &s->v[i];
        int held = v->alive && v->em < TX_CLOUD_EMITTERS && s->cloud.em[v->em].held && s->cloud.em[v->em].note == v->note;
        int32_t lx, lam_l2, amp, p16, mpul, mod;
        uint32_t x32;
        if (!v->alive && tx_svf_bank_quiet(v->body, NM) && !v->tlp && !v->lp && !v->lp2 && !v->hp) continue;
        if (held) v->stop = (int32_t)tx_exp2((s->h_l2 >> 1) + 411640);   /* sqrt(2 h / g) = 0.45 s sqrt(h): 77.8 sqrt(h) blocks */
        else if (v->stop > 0) v->stop--;
        v->flow += (held || v->stop > 0) && v->alive ? (32768 - v->flow) >> 3 : -((v->flow >> 5) + 1);   /* the last grains
                                                              * trickle out over ~1 s (tau 0.19 s; session 5: no hard stop) */
        if (!held && v->stop <= 0 && v->flow < 64) { v->flow = 0; v->alive = 0; v->em = 0xFF; }
        surface(s, v);
        /* Beverloo: lambda = 1.963 (D0 - 2.9 d)^2.5 / d^3 (m), lengths in um: log2 = 10.94 + 2.5 log2(D0 - 2.9 d) - 3 log2 d */
        {
            uint32_t D0 = tx_exp2(s->d0_l2), d = tx_exp2(s->d_l2), kd = d * 29u / 10u;
            if (D0 <= kd + 5u) { lx = 0; lam_l2 = -(30 << 16); }
            else { lx = log2_q16(D0 - kd); lam_l2 = 716964 + (5 * lx) / 2 - 3 * s->d_l2; }
            lam_l2 += ((int32_t)v->vel - 100) * 1638;        /* velocity: x 2 per 40 (how much is poured) */
            /* p = lambda / fs per sample, Q16: log2 p = lam_l2 - log2 44100 (15.43) */
            p16 = lam_l2 - 1011143 > (10 << 16) ? 0x7FFFFFFF : (int32_t)tx_exp2(lam_l2 - 1011143 + (16 << 16));
            /* amplitude per grain ~ d^1.5 v (power ~ lambda d^3 v^2 = W v^2), against d 200 um and v 1.4 m/s (h 0.1 m):
             * log2 v_mm = (log2 h + log2(2 x 9810 x 1000)) / 2 = (h_l2 + 24.226) / 2; log2(1400) = 10.451 */
            amp = (int32_t)tx_exp2((3 * (s->d_l2 - 500953)) / 2 + (s->h_l2 + 1587660) / 2 - 684900 + (12 << 16));   /* Q12 */
        }
        amp = (int32_t)(((int64_t)amp * A_REF >> 12) * v->flow >> 15);
        amp = (int32_t)(((int64_t)amp * s->level) >> 15);
        /* the pulse (~4 Hz, jittered): the flow pumps. Each pulse its own period (within +-0.5 oct, log-uniform)
         * and depth (0.3 .. 1.3): a clogging flow, never a steady 4 Hz line (it was one: 12-14 dB over the
         * envelope's floor at PULSE 127) */
        {
            uint32_t ph0 = v->pph, st = (uint32_t)(((uint64_t)99728636u * (uint32_t)v->pmul) >> 15);
            if (v->phold) v->phold--;                        /* flowing freely a while: no pulse */
            else {
                v->pph += st + (uint32_t)(((int64_t)tx_rand_s15(&s->cloud.rng) * (int32_t)(st >> 2)) >> 15);   /* +-25 % */
                if (v->pph < ph0 && s->pulse) {      /* (PULSE 0: no draws, the flow as it was) */
                    v->pmul = (uint16_t)tx_exp2((15 << 16) + (int32_t)tx_rand_s15(&s->cloud.rng));
                    v->pdg = (uint16_t)(9830u + tx_rand_below(&s->cloud.rng, 32768u));
                    if (tx_rand_below(&s->cloud.rng, 5u) < 2u) {   /* 2 in 5: the jam clears, a free run of 30 ms +
                                                              * exp(150 ms), at most 1 s (the pulses come in bouts) */
                        uint32_t h = 5u + (tx_cloud_exp_q8(&s->cloud, 26u << 8) >> 8);
                        v->phold = (uint16_t)(h > 172u ? 172u : h);
                        v->pph = 0;                          /* (at the wrap: the mean flow, no step in the gain) */
                    }
                }
            }
        }
        mpul = 32768 - (int32_t)(((int64_t)((32768 + tx_sin_q15(v->pph)) >> 1) * s->pulse * (int32_t)v->pdg) >> 30);
        if (mpul < 0) mpul = 0;
        amp = (int32_t)(((int64_t)amp * mpul) >> 15);
        {   /* a real pour is lumpy (references: rms std 3-17 dB, slow 0.5-1 Hz swings and fast flutter): the rate wanders
             * log-normally, +-0.6 oct slow and +-0.4 oct fast (ours, set against the references) */
            tx_rng_t *r = &s->cloud.rng;
            v->lump += (tx_rand_s15(r) - v->lump) >> 5;
            v->flick += (tx_rand_s15(r) - v->flick) >> 2;
            mod = (int32_t)tx_exp2(v->lump * 32 - 21800 + v->flick * 4 + (12 << 16));   /* Q12 (slow
                                                              * swings +-1.2 oct, the mean power kept: a sandfall's
                                                              * level swings ~20 dB, note 17) */
        }
        {   /* the stream breaks into slugs (granular flow near jamming is intermittent: arches form and collapse, the
             * bursts power-law sized; a wide neck flows on, still lumpy). Intermittency I from the neck's margin over
             * 2.9 d (D0 / 2.9 d: 2 .. 8 -> I 1 .. 0.5): slugs of 17-200 ms, each 2^(-4 I E) loud (E exponential: most
             * slugs a little under the mean, a few lumps well over), the mean power kept, and between slugs (chance I / 2)
             * a jam of ~20-300 ms x I: silence (ours; the pour references: level sd 3-17 dB, lumps at 0.5-1 Hz; the
             * user: no flat hiss) */
            tx_rng_t *r = &s->cloud.rng;
            uint32_t D0 = tx_exp2(s->d0_l2), kd = (tx_exp2(s->d_l2) * 29u / 10u) | 1u, rt = (D0 << 4) / kd;   /* Q4 */
            int32_t I = rt <= 32u ? 32767 : rt >= 160u ? 6554 : 32767 - (int32_t)((rt - 32u) * 205u);   /* Q15: 1 .. 0.2 */
            if (!v->sleft) {
                uint32_t e = (tx_neglog2_q12(tx_rand(r) | 1u) * 2839u) >> 12;   /* E, Q12 */
                if (e > 16384u) e = 16384u;
                if (v->sgt && (int32_t)(tx_rand(r) >> 17) < I - 13107) {   /* a jam: silence for a while (none once the
                                                              * neck is wide: a sandfall pours on, note 17) */
                    v->sgt = 0;
                    v->sleft = (uint16_t)(3u + (((e * (uint32_t)(I >> 7)) >> 12) * 17u >> 8) * 3u);
                } else {
                    int32_t k = (int32_t)(((e * (uint32_t)I) >> 15) * 64u);       /* 4 I E octaves, Q16 */
                    v->sgt = (int32_t)tx_exp2(slug_comp(I) - k + (15 << 16));
                    if (v->sgt > 65535) v->sgt = 65535;
                    v->sleft = (uint16_t)(3u + tx_rand_below(r, 32u));
                }
            }
            v->sleft--;
            v->sg += (v->sgt - v->sg) >> 1;
            mod = (int32_t)(((int64_t)mod * v->sg) >> 15);
            if (mod > 32767) mod = 32767;                    /* (x 8 at most) */
        }
        if (amp > 16383) amp = 16383;
        if (p16 < 16384) amp = (int32_t)(((int64_t)amp * mod) >> 12);   /* (the swings after the cap: a heavy pour swings
                                                              * too instead of sitting on the cap, note 17) */
        if (amp > 24000) amp = 24000;                        /* (the filters' headroom: |x - lp| x a < 2^31) */
        /* the shot train: clicks (rare) or noise of the same variance (dense) */
        x32 = s->cloud.rng.s;
        if (p16 < 16384) {
            for (j = 0; j < n; j += 32) {                    /* the grains arrive in clumps, each 0.7 ms its own size */
                unsigned e = j + 32u < n ? j + 32u : n, q;
                int32_t g = clump(&x32, amp);
                for (q = j; q < e; q++) {
                    x32 ^= x32 << 13; x32 ^= x32 >> 17; x32 ^= x32 << 5;
                    x[q] = (int32_t)(x32 & 0xFFFFu) < p16 ? ((int32_t)(x32 >> 17) * g) >> 14 : 0;   /* |a| x U(0, 2) */
                }
            }
        } else {
            int32_t sd = (int32_t)(((int64_t)amp * (int32_t)tx_exp2((int32_t)((log2_q16((uint32_t)p16) - (16 << 16)) / 2) + (16 << 16))) >> 16);
            if (sd > 16383) sd = 16383;                      /* the force at most +-1 full scale */
            sd = (int32_t)(((int64_t)sd * mod) >> 12);
            if (sd > 24000) sd = 24000;
            for (j = 0; j < n; j += 32) {                    /* clumps: each 0.7 ms its own gain (+-1 oct, ours: */
                unsigned e = j + 32u < n ? j + 32u : n, q;   /* grains arrive in clusters; references kurtosis 1-25) */
                int32_t g = clump(&x32, sd);
                for (q = j; q < e; q++) {
                    x32 ^= x32 << 13; x32 ^= x32 >> 17; x32 ^= x32 << 5;
                    x[q] = (((int32_t)(x32 >> 17) - 16384) * g) >> 13;   /* uniform, sd ~ amp sqrt(p) x 2 / sqrt 3 */
                }
            }
        }
        s->cloud.rng.s = x32 ? x32 : 1u;
        /* x is Q15 force: the dry share (low-passed), the modes, or the tube */
        {
            int32_t lp = v->lp, lp2 = v->lp2, hp = v->hp, dry = sf->dry;
            for (j = 0; j < n; j++) {
                lp += ((x[j] - lp) * (lpa >> 2)) >> 13;
                lp2 += ((lp - lp2) * (lpa >> 2)) >> 13;
                hp += ((lp2 - hp) * (hpa >> 2)) >> 13;
                buf[j] += (lp2 - hp) * dry;                  /* Q15 x Q8 -> Q23 */
                x[j] = x[j] * 256;                           /* -> Q23 for the bank / tube */
            }
            if (lp > -4 && lp < 4) { int32_t any = 0; for (j = 0; j < n; j++) any |= x[j]; if (!any) lp = 0; }
            if (lp2 > -16 && lp2 < 16 && !lp) lp2 = 0;            /* floor rounding sticks a one-pole at a few */
            if (hp > -16 && hp < 16 && !lp2) hp = 0;              /* LSB: under -66 dBFS, cleared */
            v->lp = lp; v->lp2 = lp2; v->hp = hp;
        }
        if (sf->nm) tx_svf_bank_add_g(v->body, v->f, (int32_t)(16384u / sf->q), v->gin, v->g, sf->nm, x, buf, n);
        if (tube) {                                          /* quarter-wave: y = x - g lp(y[n - D]) (odd harmonics) */
            uint32_t w = v->wpos, dq = v->dly_q8, di = dq >> 8, fr = dq & 255u;
            int32_t tlp = v->tlp, any = 0;
            for (j = 0; j < n; j++) {
                uint32_t r0 = (w - di) & (DMAX - 1u), r1 = (w - di - 1u) & (DMAX - 1u);
                int32_t yd = v->line[r0] + (((v->line[r1] - v->line[r0]) * (int32_t)fr) >> 8);
                int32_t y;
                tlp += ((yd - tlp) * 3) >> 2;                  /* loop loss: low-pass ~ 9 kHz-ish (ours) */
                y = (x[j] >> 8) - ((tlp * 29491) >> 15);     /* g 0.9, inverting: a closed-open tube */
                y = y > (1 << 16) ? (1 << 16) : y < -(1 << 16) ? -(1 << 16) : y;   /* +-2 FS: tlp x g < 2^31 */
                v->line[w] = y;
                w = (w + 1u) & (DMAX - 1u);
                buf[j] += y * 128;
                any |= y;
            }
            if (!any && tlp > -4 && tlp < 4) { unsigned m; for (m = 0; m < DMAX; m++) v->line[m] = 0; tlp = 0; }
            v->tlp = tlp; v->wpos = w;
        }
    }
    tx_vary_block(&s->vy, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = s->hp.x1 | s->hp.y1;
        for (i = 0; i < NV; i++) z |= s->v[i].alive | !tx_svf_bank_quiet(s->v[i].body, NM) | s->v[i].tlp | s->v[i].lp | s->v[i].lp2 | s->v[i].hp;
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void sand_stats(const void *st, tx_stats_t *o)
{
    const sand_t *s = (const sand_t *)st;
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

/* the worst case: four pours onto a metal tray (6 modes each), dense flow, then into tubes */
static const char SCENE_MAX[] =
    TX_TEXT("0 particles 4\n0 param SURFACE 70\n0 param ORIFICE 127\n0 param GRAIN 0\n0 param HEIGHT 127\n0 param PULSE 127\n"
    "0 on 53 127\n0 on 60 127\n0 on 67 127\n0 on 79 127\n"
    "1500 param SURFACE 127\n2500 param SURFACE 90\n"
    "3000 off 53\n3000 off 60\n3000 off 67\n3000 off 79\n"
    "5000 end\n");

const tx_engine_t tx_eng_sand = {
    "sand", TX_TEXT("#14 Sand"), NP, PARAMS, NV, 3, sizeof(sand_t), SCENE_MAX,
    sand_init, sand_key, sand_param, sand_render, sand_stats,
};
