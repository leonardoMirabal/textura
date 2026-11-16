/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #17 Crumpling: a sheet crushed in the hand (CLOUD draws, NOISE SVF bank, INSECT resonators).
 *
 * A key press squeezes a sheet (a burst of clicks), holding keeps crushing it, release lets it relax (the clicks
 * thin out over seconds, ~1 / (1 + t / 0.15 s): ours). Physics:
 *   clicks    ridges snap through in avalanches: each a click, its energy from a power law p(E) ~ E^-alpha over
 *             four decades (G14: mylar alpha ~1, least squares 1.0-1.1; G13: paper 1.3-1.6), log-uniform at
 *             alpha = 1. A click is an impulse plus a short noise burst (G13: impulsive, short exponential decay)
 *             through the sheet's low-Q resonances (Q 2-8, by material: paper's spectrum peaks near 2 kHz (G13),
 *             mylar's sits above 1 kHz (G14); the rest ours). The loudest clicks also ring a ridge mode of their
 *             own (a hero: a Q 10 two-pole at a random pitch in the material's band, ours).
 *   cluster   G14: click energies correlate over t0 = 30-50 us (stretched exponential, beta 0.35), gone by ~10 ms:
 *             each click brings CLUSTER aftershocks (mean) at Omori times t = c ((1 - u)^-2 - 1), c = 2 samples
 *             (p 1.5: ~94 % within 10 ms), sizes from the same law under the main one.
 *   burst     the slow burstiness of a squeeze is OUR design (no source): the rate x 2^(BURST x the sum
 *             of three random walks with time constants 30 ms / 300 ms / 3 s).
 *   thickness G14 (qualitative): the largest click energy ~ h^2, the click rate ~ h^-2 (total power ~ constant).
 * Keys: the sheet's size (its resonances and the ridge pitches) a semitone per key around SIZE; velocity = the
 * squeeze's energy (rate x (v / 100)^2, click energies x (v / 100)^2). */
#include "tx_engine.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_insect.h"
#include "tx_core_chorus.h"

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_G +1.3 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 2

enum { P_RATE, P_MATERIAL, P_THICK, P_BURST, P_CLUSTER, P_SIZE, NP };

static const tx_param_t PARAMS[NP] = {
    {"RATE", 127, 50, TX_TEXT("crush speed: 3 .. 1000 clicks/s per hand (log)")},
    {"MATERIAL", 127, 64, TX_TEXT("foil | mylar (alpha 1, bright) | paper (alpha 1.45, ~2 kHz) | dry leaves (a bright rustle) | snow (dense, dull)")},
    {"THICK", 127, 64, TX_TEXT("sheet thickness x 0.25 .. 4: the largest clicks ~ h^2, their rate ~ 1 / h^2")},
    {"BURST", 127, 50, TX_TEXT("squeeze burstiness: steady .. bursts (random walks of 30 ms / 0.3 s / 3 s)")},
    {"CLUSTER", 127, 40, TX_TEXT("aftershocks per click: 0 .. 4 (clicks bunch within ~10 ms)")},
    {"SIZE", 127, 64, TX_TEXT("sheet size: resonances x 2 .. x 0.5 (keys: a semitone each)")},
};

#define NS 8u                      /* sheets (one per key) */
#define NH 16u                     /* hero ridge modes */
#define NM 6u                      /* sheet resonances */
#define NPEND 64u
#define CLICK_MAX 32u              /* clicks per block, at most (5.5 k/s; the rest is counted as dropped) */
#define NMAT 5u
#define RANGE_L2 54426             /* four decades of energy: log2(1e4) Q12 */
#define OUT_G 13582                /* output gain, Q12 (x 3.32; x 2.85 before tx_core_vary, calibrated: a held key at the defaults = -26 LUFS,
                                    * the loudness target for sustained textures) */
#define A_REF 40000                /* the largest click's amplitude at THICK 1, v100 (Q15; calibrated by render) */
#define HERO_L2 45056              /* clicks above 11 octaves of energy (the top ~2.3 octaves) ring a hero */

typedef struct {
    uint16_t alpha;                /* Q12 (4096 = 1: log-uniform) */
    uint16_t f[NM];                /* resonances at SIZE 64, Hz */
    uint8_t q_x4;                  /* bank Q x 4 */
    uint8_t dry;                   /* the click itself in the output, Q8 */
    uint16_t burst_us;             /* noise burst time constant */
    uint16_t h_lo, h_hi;           /* hero pitch band, Hz */
    uint8_t rate_x;                /* rate multiplier (snow is dense) */
    uint8_t tilt;                  /* each higher resonance x tilt / 256 (darker materials fall) */
    uint16_t lvl;                  /* click level x lvl / 256 (by ear: soft materials hold their loudness) */
    uint8_t bright;                /* 1: the burst keeps its top octave (foil, mylar, dry leaves: their recordings */
                                   /* stay within ~10 dB up to 16 kHz) */
} mat_t;

static const mat_t MAT[NMAT] = {
    {4300, {1800, 3100, 4700, 6200, 7000, 5400}, 24, 150, 800, 3000, 9000, 1, 255, 256, 1},  /* foil (ours) */
    {4096, {1200, 2300, 3600, 5000, 6500, 7200}, 16, 128, 1200, 2000, 8000, 1, 240, 256, 1},  /* mylar: alpha 1 (G14) */
    {5939, {700, 1200, 1800, 2300, 3000, 4200}, 12, 64, 3000, 1200, 3000, 1, 200, 320, 0},   /* paper: 1.45, ~2 kHz (G13) */
    {5734, {1200, 1900, 2800, 4000, 5500, 7000}, 10, 96, 2500, 1500, 6000, 2, 220, 900, 1},   /* dry leaves (ours; a
                                                                 * recording of shuffled leaves: flat 2.5-10 kHz) */
    {6554, {250, 450, 800, 1300, 2000, 3000}, 8, 12, 4000, 0, 0, 4, 128, 900, 0},            /* snow (ours): no ridges */
};

typedef struct {
    int32_t env;                   /* squeeze, Q15 */
    uint32_t next_q8;              /* samples to the next click */
    uint32_t rel;                  /* blocks since release (0 = held) */
    uint8_t em, note, vel, alive;
} sheet_t;

typedef struct { uint32_t t; int32_t size; uint8_t sh, pad[3]; } pend_t;

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    tx_svf_t bank[NM];
    tx_ireso_t hero[NH];
    sheet_t sh[NS];
    pend_t pend[NPEND];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t exc[TX_BLOCK], buf[TX_BLOCK], bpush[TX_BLOCK];
    int32_t bf[NM], bg[NM];
    uint16_t ho[CLICK_MAX];
    int16_t ha[CLICK_MAX];
    uint8_t hn[CLICK_MAX];
    uint16_t hoff[NH];             /* each hero's strike in this block */
    int16_t hamp[NH];
    uint8_t hnum[NH];
    int32_t nlp1, nlp2;            /* the burst noise's low-pass states */
    int32_t walk[3];               /* the burst random walks, Q12 */
    int32_t benv, bdecay;          /* the noise-burst envelope (Q23-ish) and its decay per sample (Q15) */
    uint32_t now;
    uint8_t p[NP], limit, quiet, npend;
    int32_t level, rate_l2, h_l2, size_l2, bq;
    int32_t pinv;                  /* 1 / (alpha - 1), Q12 (0: log-uniform) */
    uint32_t pm;                   /* (1 - 2^-Lmax) x 2^32: the power law truncated at RANGE */
    uint16_t dropped, ncl;
    tx_vary_t vy;
} crumple_t;

static const mat_t *mat_of(const crumple_t *s) { return &MAT[(s->p[P_MATERIAL] * NMAT) >> 7]; }

static void set_bank(crumple_t *s)
{
    const mat_t *m = mat_of(s);
    unsigned i;
    int32_t g = 2048 / 2;
    for (i = 0; i < NM; i++) {
        uint32_t hz = (uint32_t)(((uint64_t)m->f[i] * tx_exp2(s->size_l2 + (16 << 16))) >> 16);
        s->bf[i] = tx_svf_coef(hz);
        s->bg[i] = g;
        g = g * m->tilt >> 8;
    }
    s->bq = (int32_t)(65536u / m->q_x4);
    if (m->alpha <= 4096u + 41u) s->pinv = 0;
    else {
        int32_t lmax = (int32_t)(((int64_t)RANGE_L2 * (m->alpha - 4096u)) >> 12);   /* Q12 */
        s->pinv = (int32_t)((1u << 24) / (m->alpha - 4096u));
        s->pm = 0xFFFFFFFFu - (lmax >= (31 << 12) ? 1u : tx_exp2((32 << 16) - lmax * 16));   /* 2^32 (1 - 2^-Lmax) */
    }                     /* 1 / Q in Q14 */
    s->bdecay = 32768 - (int32_t)(32768u * 23u / m->burst_us);   /* exp(-1 / (tau fs)): 1 - 22.7 us / tau */
}

static void set_param(crumple_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_RATE: s->rate_l2 = 103872 + (int32_t)(v * 4316u); break;       /* log2(3 /s) + v log2(333) / 127, Q16 */
    case P_THICK: s->h_l2 = ((int32_t)v - 64) * 2064; break;              /* log2 h: -2 .. +2 octaves, Q16 */
    case P_SIZE: s->size_l2 = (64 - (int32_t)v) * 1024; set_bank(s); break;   /* +-1 octave, Q16 */
    case P_MATERIAL: set_bank(s); break;
    default: break;
    }
}

static void crumple_init(void *st, uint32_t seed)
{
    crumple_t *s = (crumple_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    for (i = 0; i < NS; i++) s->sh[i].em = 0xFF;
    s->limit = 8;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((crumple_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void crumple_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((crumple_t *)st)->vy);
    tx_cloud_key(&((crumple_t *)st)->cloud, note, vel, on);
}

static void crumple_param(void *st, uint8_t id, uint8_t val)
{
    crumple_t *s = (crumple_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NH ? NH : val);
        for (i = s->limit; i < NH; i++) s->hero[i].y1 = s->hero[i].y2 = s->hero[i].p1 = s->hero[i].p2 = 0;   /* cut */
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* an energy from the power law, log2(E / E_min) Q12 in 0 .. RANGE_L2: truncated Pareto by inversion,
 * x = -log2(1 - v (1 - 2^-Lmax)) / (alpha - 1) with Lmax = RANGE (alpha - 1); alpha = 1: log-uniform */
static int32_t draw_size(crumple_t *s)
{
    uint32_t v = tx_rand(&s->cloud.rng), u;
    int32_t x;
    if (!s->pinv) return (int32_t)(((v >> 16) * (uint32_t)RANGE_L2) >> 16);
    u = 0xFFFFFFFFu - (uint32_t)(((uint64_t)v * s->pm) >> 32);
    x = (int32_t)(((int64_t)tx_neglog2_q12(u) * s->pinv) >> 12);
    return x > RANGE_L2 ? RANGE_L2 : x;
}

static void pend_push(crumple_t *s, uint32_t t, int32_t size, unsigned sh)
{
    pend_t *p;
    if (s->npend >= NPEND) { s->dropped++; return; }
    p = &s->pend[s->npend++];
    p->t = t; p->size = size; p->sh = (uint8_t)sh;
}

/* a click at offset o of energy size (Q12 log2 over E_min) from sheet sh */
static void click(crumple_t *s, unsigned o, int32_t size, unsigned shi)
{
    const mat_t *m = mat_of(s);
    sheet_t *h = &s->sh[shi];
    int32_t vl2 = ((int32_t)h->vel - 100) * 655;            /* velocity: energy x (v / 100)^2, Q16 log2 (approx) */
    int32_t e_l2 = (size - RANGE_L2) * 16 + 2 * s->h_l2 + vl2;   /* log2(E / E_max at h 1), Q16 */
    int32_t a = (int32_t)(((int64_t)tx_exp2(e_l2 / 2 + (15 << 16)) * A_REF) >> 15);   /* amplitude ~ sqrt(E) */
    a = (int32_t)(((int64_t)a * s->level) >> 15);
    a = (a * (int32_t)m->lvl) >> 8;
    if (a > 32767) a = 32767;
    if (!a) return;
    if (s->ncl >= CLICK_MAX) { s->dropped++; return; }
    {   /* a crinkle: 3-7 micro-clicks over ~1-7 ms (a ridge snaps in steps; the recordings' events rise in 3-8 ms,
         * far less impulsive than single spikes: ours), each ~a / 2 with a random sign */
        uint32_t k = 3u + (tx_rand(&s->cloud.rng) >> 30) + (tx_rand(&s->cloud.rng) >> 31) * 2u, c, t = o;
        int32_t sa = a >> 1;
        for (c = 0; c < k && t < TX_BLOCK; c++) {
            s->exc[t] += ((tx_rand(&s->cloud.rng) & 1u) ? sa : -sa) * 256;   /* Q15 -> Q23 */
            s->bpush[t] += sa << 3;                          /* the noise burst (its envelope) */
            t += 20u + tx_rand_below(&s->cloud.rng, 60u);
            sa = sa * 27000 >> 15;
        }
    }
    s->ho[s->ncl] = (uint16_t)o; s->ha[s->ncl] = (int16_t)a;
    s->hn[s->ncl] = (uint8_t)(m->h_hi && size >= HERO_L2 ? shi : 0xFF);
    s->ncl++;
}

static void hero_start(crumple_t *s, unsigned o, int32_t a, unsigned shi)
{
    const mat_t *m = mat_of(s);
    unsigned i, pick = NH;
    uint32_t f;
    int32_t kl2 = s->size_l2 - ((int32_t)s->sh[shi].note - 60) * 5461;   /* a key up: a smaller sheet, higher */
    for (i = 0; i < s->limit && pick == NH; i++) if (!s->hnum[i] && tx_ireso_quiet(&s->hero[i])) pick = i;
    if (pick == NH) { s->dropped++; return; }
    f = m->h_lo + tx_rand_below(&s->cloud.rng, (uint32_t)(m->h_hi - m->h_lo));
    f = (uint32_t)(((uint64_t)f * tx_exp2(-kl2 + (16 << 16))) >> 16);
    if (f > 20000u) f = 20000u;
    tx_ireso_set(&s->hero[pick], f, 160);                    /* Q 10 (ours) */
    s->hero[pick].g = 128;
    s->hero[pick].zlim = 128;                                /* Q 10 at >= 600 Hz: limit cycles < ~120 LSB */
    s->hoff[pick] = (uint16_t)o; s->hamp[pick] = (int16_t)a; s->hnum[pick] = 1;
}

TX_AUDIO_CODE static void crumple_render(void *st, int32_t *mix, unsigned n)
{
    crumple_t *s = (crumple_t *)st;
    const mat_t *m = mat_of(s);
    tx_rng_t *r = &s->cloud.rng;
    unsigned k, i, j;
    uint16_t d0 = s->dropped;
    int32_t any = 0;
    if (n > TX_BLOCK) n = TX_BLOCK;
    s->ncl = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) { s->now += n; s->cloud.last_events = 0; return; }   /* idle */
    for (j = 0; j < n; j++) s->exc[j] = s->buf[j] = s->bpush[j] = 0;
    for (i = 0; i < k; i++) {                                /* a key: a sheet in the hand */
        const tx_event_t *e = &s->ev[i];
        sheet_t *h = 0;
        for (j = 0; j < NS && !h; j++) if (s->sh[j].alive && s->sh[j].note == e->note) h = &s->sh[j];
        for (j = 0; j < NS && !h; j++) if (!s->sh[j].alive) h = &s->sh[j];
        if (!h) { s->dropped++; continue; }
        h->alive = 1; h->em = e->emitter; h->note = e->note; h->vel = e->vel; h->rel = 0;
        h->env = 32767 * 3; h->next_q8 = (uint32_t)e->offset << 8;   /* the squeeze: x 3 at first, settling */
    }
    {   /* the burst walks (ours): one-poles of normal draws at 30 ms / 300 ms / 3 s, Q12 */
        static const uint8_t SH[3] = {2, 6, 9};              /* 1 - exp(-5.8 ms / tau) ~ 2^-2.4, 2^-5.7, 2^-9 */
        for (i = 0; i < 3; i++) s->walk[i] += ((tx_gauss_q12(r) * (1 << (SH[i] >> 1))) - s->walk[i]) >> SH[i];
    }
    for (i = 0; i < NS; i++) {
        sheet_t *h = &s->sh[i];
        int held;
        int32_t rl2, w;
        uint32_t per_q8;
        if (!h->alive) continue;
        any = 1;
        held = h->em < TX_CLOUD_EMITTERS && s->cloud.em[h->em].held && s->cloud.em[h->em].note == h->note;
        if (held) h->env += (32767 - h->env) >> 4;           /* the first squeeze settles in ~0.1 s */
        else {
            h->rel++;
            h->env = (int32_t)(32767u * 26u / (26u + h->rel));   /* relaxation ~ 1 / (1 + t / 0.15 s) (ours) */
            if (h->rel > 1000u) { h->alive = 0; h->em = 0xFF; continue; }
        }
        w = (s->walk[0] + s->walk[1] + s->walk[2]) * s->p[P_BURST] >> 6;   /* Q12 octaves: up to ~+-4 */
        rl2 = s->rate_l2 - 2 * s->h_l2 + ((int32_t)h->vel - 100) * 1310 + w * 16 + (m->rate_x == 4 ? 131072 : m->rate_x == 2 ? 65536 : 0);
        {   /* x the squeeze envelope: + log2(env / 2^15) */
            uint32_t ev = (uint32_t)(h->env > 131071 ? 131071 : h->env);
            if (ev < 64u) { h->next_q8 = (uint32_t)n << 8; continue; }   /* (too thin to click: wait) */
            rl2 += (2 << 16) - (int32_t)(tx_neglog2_q12(ev << 15) << 4);
        }
        if (rl2 > 14 << 16) rl2 = 14 << 16;                  /* 16 k clicks/s at most */
        per_q8 = tx_exp2(1535412 - rl2);                     /* fs x 256 / rate: log2(44100 x 256) = 23.43 */
        while ((h->next_q8 >> 8) < n) {
            unsigned o = h->next_q8 >> 8, a, cnt = 0;
            int32_t sz = draw_size(s);
            click(s, o, sz, i);
            {   /* the cluster: aftershocks (binomial, mean CLUSTER), Omori c 2 samples, p 1.5 */
                uint32_t bits = tx_rand(r), mean = (s->p[P_CLUSTER] + 16u) >> 5;
                for (a = 0; a < 2u * mean; a++) cnt += bits >> a & 1u;
                for (a = 0; a < cnt; a++) {
                    uint32_t l = tx_neglog2_q12(tx_rand(r));            /* Q12 */
                    uint32_t t = l > (6u << 12) ? 8190u : ((tx_exp2((int32_t)(l * 32u) + (16 << 16)) - 65536u) * 2u) >> 16;
                    int32_t z = draw_size(s);
                    if (z > sz - 4096) z = sz - 4096;            /* under the main click */
                    if (z > 0) pend_push(s, s->now + o + 1u + t, z, i);
                }
            }
            h->next_q8 += tx_cloud_exp_q8(&s->cloud, per_q8);
            if (s->ncl >= CLICK_MAX) { h->next_q8 = (uint32_t)n << 8; break; }
        }
        h->next_q8 -= (uint32_t)n << 8;
    }
    for (i = 0; i < s->npend; ) {                            /* due aftershocks */
        int32_t d = (int32_t)(s->pend[i].t - s->now);
        if (d < (int32_t)n) {
            click(s, (unsigned)(d < 0 ? 0 : d), s->pend[i].size, s->pend[i].sh);
            s->pend[i] = s->pend[--s->npend];
        } else i++;
    }
    {   /* the click bursts (noise x envelope) and the dry clicks */
        int32_t e = s->benv, dk = s->bdecay, dry = m->dry;
        int32_t l1 = s->nlp1, l2 = s->nlp2;
        if (e || s->ncl) for (j = 0; j < n; j++) {
            int32_t x, bn;
            e -= ((e >> 8) * (32768 - dk)) >> 7;             /* the burst decays (tau by material) */
            e += s->bpush[j];
            x = (int32_t)(tx_rand(r) >> 20) - 2048;          /* +-2^11 */
            l1 += (x - l1) >> 1; l2 += (l1 - l2) >> (1 - m->bright);   /* low-passes: paper falls above */
            bn = (l2 * (e >> 6)) >> 1;                       /* ~8-10 kHz; the burst, Q23 */
            s->exc[j] += bn;
            s->buf[j] += (bn >> 8) * dry;                    /* heard dry: the burst only (the 1-sample spikes made */
        }                                                    /* it far too impulsive: kurtosis 650-770 vs 14-25) */
        s->nlp1 = e ? l1 : 0; s->nlp2 = e ? l2 : 0;
        if (e < 256) e = 0;                                  /* (e >> 8 stops decaying below 256) */
        s->benv = e;
    }
    tx_svf_bank_add(s->bank, s->bf, s->bq, s->bg, NM, s->exc, s->buf, n);
    for (i = 0; i < s->ncl; i++) if (s->hn[i] != 0xFF) hero_start(s, s->ho[i], s->ha[i], s->hn[i]);
    for (i = 0; i < NH; i++) { tx_ireso_add(&s->hero[i], &s->hoff[i], &s->hamp[i], s->hnum[i], s->buf, n); s->hnum[i] = 0; }
    for (j = 0; j < n; j++) {                                /* the loudness target (OUT_G) */
        int32_t v = s->buf[j] > (1 << 25) ? (1 << 25) : s->buf[j] < -(1 << 25) ? -(1 << 25) : s->buf[j];
        s->buf[j] = ((v >> 8) * OUT_G) >> 4;
    }
    tx_vary_block(&s->vy, s->buf, n);
    tx_dcblock_add(&s->hp, s->buf, mix, n);
    s->now += n;
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    s->cloud.last_events = s->ncl;
    {
        int32_t z = any | s->npend | s->benv | s->hp.x1 | s->hp.y1 | !tx_svf_bank_quiet(s->bank, NM);
        for (i = 0; i < NH; i++) z |= !tx_ireso_quiet(&s->hero[i]);
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void crumple_stats(const void *st, tx_stats_t *o)
{
    const crumple_t *s = (const crumple_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < NH; i++) n += !tx_ireso_quiet(&s->hero[i]);
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: eight hands crushing thin foil as fast as possible, bursty, clustered */
static const char SCENE_MAX[] =
    TX_TEXT("0 particles 16\n0 param MATERIAL 0\n0 param RATE 127\n0 param THICK 0\n0 param BURST 127\n0 param CLUSTER 127\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "5000 end\n");

const tx_engine_t tx_eng_crumple = {
    "crumple", TX_TEXT("#17 Crumpling"), NP, PARAMS, NH, 8, TX_STATE_SIZE(crumple_t), SCENE_MAX,
    crumple_init, crumple_key, crumple_param, crumple_render, crumple_stats,
};
