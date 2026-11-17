/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #20 Static / sparks: electric discharges as N-wave ticks, a corona fizz, an optional resonant body.
 *
 * A key press fires one discharge (with Omori aftershocks when BURST is up); a held key keeps discharging at RATE.
 *   spark    a spark heats a thin channel in microseconds and launches an N-wave of half-duration T (2 us for a
 *            piezo igniter, E27; ~30-40 us for big gaps, E25 / E2). Its spectrum is 2 P_s T j1(2 pi f T): rising
 *            +6 dB/oct up to its peak 0.33 / T, falling above. In the audio band: a doublet [+A, -A] (2 sin(pi f/fs),
 *            ~f) through two one-pole low-passes at 0.33 / T. Written as a 24-sample kernel per event (no voice).
 *            Level ~ P_s T^2; P_s ~ d^0.2 (gap d < 4 mm) or d^1.2 (above) (E25); T = 2 us (d / 0.5 mm)^0.8 (ours,
 *            spanning E27 to E25 / E2). Distance (E2, sigma0 0.524, r0 0.1 m): T x sqrt(1 + sigma0 ln(r/r0)),
 *            p_s x (r0 / r) / sqrt(1 + sigma0 ln(r/r0)). Lower keys = wider gaps (2^(1/24) per semitone), velocity
 *            scales the gap (energy).
 *   corona   sparse bipolar microsecond pulses, positive / negative peaks 1.4 : 1 (E4: 1.3-1.6), spectrum peaking near
 *            10 kHz: a Poisson shot train through a one-pole high-pass (6 kHz) and low-pass (14 kHz), while keys are
 *            held.
 *   body     what the spark jumps to (ours): a shared bank of 4 band-pass resonators (1.2 / 2.7 / 4.4 / 6.5 kHz,
 *            Q 3-20), each fed with input gain 1 / Q so its peak gain is 1 (bank headroom).
 *   no two alike (ours): each spark breaks down its own way: its channel's length +-SPK_GAP (so its N-wave's width),
 *            where it lands within the sample (a sub-sample onset, its energy kept), the N-wave's tail SPK_TAIL .. 1
 *            of its head (the channel's angle to the listener), its re-strikes (one discharge in three a single
 *            stroke, else one or two more as the charge recovers, 0.05 .. 0.5 ms apart, each 0.15 .. 0.7 of the
 *            first), and where it strikes the body: each mode's drive -SPK_MODE .. +SPK_MODE dB, of either sign (the
 *            mode's shape at that spot). Drawn from a stream of its own: the discharges' times stay the cloud's.
 * Level law (a DELIBERATE choice, approved 2026-10-20): the physical P_s T^2 spans ~70 dB over the GAP range, which
 * would make a small tick inaudible next to a big crack; the gap's part is applied at half its dB, distance at its
 * full law. */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_lin.h"

#define OUT_TRIM 6100               /* output gain, Q12 (-1.7 dB with the session-5 fixes: the chord's limiter under ~8 dB; +11.7 dB): max momentary (one velocity-100 press): -54.2 LUFS at LEVEL 100 -> target -20 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter;
                                    * lowered 12.4 dB: the default chord <= +12 dBFS, the max scene <= +24 dBFS,
                                    * the limiter under ~8 dB in the chord) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM -6.5 dB with it, held there by the chord's limiter under ~8 dB (its mean
 * loudness below the old one) */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 160
#define VARY_DRIFT 40
#define VARY_SPLIT 0

enum { P_GAP, P_RATE, P_BURST, P_DIST, P_CORONA, P_BODY, NP };

static const tx_param_t PARAMS[NP] = {
    {"GAP", 127, 57, TX_TEXT("spark gap at C4: 0.3 .. 20 mm (log): N-wave 1.3 .. 38 us, tick (bright) .. crack (fuller, louder)")},
    {"RATE", 127, 16, TX_TEXT("discharges per second while a key is held: 0.1 .. 508, perceptually even (0.1 + 508 x^2.5; 16: 3 /s)")},
    {"BURST", 127, 40, TX_TEXT("crackle bursts: Omori aftershocks per discharge, 0 .. 12 on average (p 1.68, c 30 ms)")},
    {"DIST", 127, 30, TX_TEXT("distance to the spark: 0.1 .. 10 m (log): level ~1/r, the N-wave lengthens (duller)")},
    {"CORONA", 127, 0, TX_TEXT("corona fizz while keys are held: off .. loud (bipolar us pulses, ~10 kHz)")},
    {"BODY", 127, 30, TX_TEXT("what the spark jumps to: free air (dry tick) .. a small metal object (ringing modes)")},
};

#define NB 4u                      /* body modes */
#define KLEN 24u                   /* spark kernel length, samples */
#define TICK_REF_L2 (14 << 16)     /* doublet amplitude of the reference spark (d 4 mm, r0, v100): 2^14 Q15 */
#define SPARK_MAX 32u
#define SPK_GAP 22938              /* the channel's length per spark: +-0.35 oct, Q16 (log2 d) */
#define SPK_TAIL 24576             /* the N-wave's tail re its head: 0.75 .. 1, Q15 */
#define SPK_MODE 6                 /* the body's modes driven -6 .. +6 dB by where the spark strikes */
#define SPK_DT 226000u             /* a re-strike after 2 .. ~22 samples (0.05 .. 0.5 ms; log2 span, Q16) */
#define SPK_RE_LO 4915             /* a re-strike at 0.15 .. 0.7 of the first stroke, Q15 */
#define SPK_RE_HI 22938

TX_AUDIO_DATA static const uint16_t BODY_HZ[NB] = { 1200, 2700, 4400, 6500 };

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t tick[TX_BLOCK + KLEN];  /* the sparks of this block (+ the tail carried into the next) */
    int32_t buf[TX_BLOCK];
    tx_svf_t body[NB];             /* body bank */
    int32_t f[NB], q;              /* Q14 SVF coefficients (q = 1 / Q, also the input gain) */
    int32_t wet;                   /* Q15 */
    int32_t corona, cor_g;         /* corona target and current gain, Q15 */
    int32_t c_lp1, c_lp2, c_pend;  /* corona filter state, pending negative half */
    uint32_t rng_c;                /* corona shot PRNG (separate from the cloud's) */
    uint32_t rng_s;                /* each spark's own breakdown (separate from the cloud's) */
    int32_t gin[NB];               /* the body's modes' drive by where the last spark struck, Q8 */
    uint8_t p[NP], limit, quiet, sparks;
    int32_t level;                 /* Q15 */
    int32_t gap_l2;                /* log2(d at C4 / 1 mm), Q16 */
    int32_t len_l2;                /* distance: log2 of the lengthening sqrt(1 + sigma0 ln(r/r0)), Q16 */
    int32_t att_l2;                /* distance: log2 of the pressure factor (r0 / r) / sqrt(...), Q16 (<= 0) */
    uint16_t dropped;
    tx_vary_t vy;
} sparks_t;

static int32_t log2_q16(uint32_t x) { return (32 << 16) - (int32_t)(tx_neglog2_q12(x) << 4); }

static void set_param(sparks_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_GAP: s->gap_l2 = -113835 + (int32_t)(v * 3127u); break;   /* log2(0.3) + v log2(66.7) / 127 */
    case P_RATE: tx_cloud_rate(&s->cloud, 26u + ((129999u * (tx_lin_frac((int32_t)v << 8) >> 4)) >> 12)); break;   /* 0.1 ..
                                                             * 508 /s, perceptually even, Q8 (the ends as on the old log scale) */
    case P_BURST: tx_cloud_omori(&s->cloud, (v * 12u + 63u) / 127u, 6881, 30, 8192); s->cloud.after_stream = 1; break;
    case P_DIST: {
        int32_t rl2 = (int32_t)(v * 3428u);                  /* log2(r / r0), Q16: 0 .. 6.64 (0.1 .. 10 m) */
        uint32_t x = 65536u + (uint32_t)(((uint64_t)(uint32_t)rl2 * 23808u) >> 16);   /* 1 + 0.524 ln 2 log2: Q16 */
        int32_t l = log2_q16(x) - (16 << 16);                /* log2(1 + sigma0 ln(r/r0)), Q16 */
        s->len_l2 = l / 2;
        s->att_l2 = -rl2 - l / 2;
        break;
    }
    case P_CORONA: s->corona = v ? (int32_t)tx_exp2((15 << 16) - (int32_t)((127u - v) * 3612u)) : 0; break;   /* -42 .. 0 dB */
    case P_BODY: {
        unsigned i;
        uint32_t qx16 = 48u + v * 272u / 127u;               /* Q x 16: 3 .. 20 */
        s->q = (int32_t)((16u << 14) / qx16);
        s->wet = (int32_t)v * 258;
        for (i = 0; i < NB; i++) s->f[i] = tx_sin_q15(BODY_HZ[i] * 97391u >> 1);   /* 2 sin(pi f / fs), Q14 */
        break;
    }
    default: break;
    }
}

static void sparks_init(void *st, uint32_t seed)
{
    sparks_t *s = (sparks_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);                         /* a key press: one discharge */
    tx_cloud_sizes(&s->cloud, 6144, 6u << 12);               /* discharge energies: alpha 1.5 over 6 octaves */
    s->rng_c = seed * 2654435761u + 0x1234567u;
    if (!s->rng_c) s->rng_c = 1;
    s->rng_s = seed * 2246822519u + 0x5A4B3C2Du;
    if (!s->rng_s) s->rng_s = 1;
    for (i = 0; i < NB; i++) s->gin[i] = 256;
    s->limit = 16;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((sparks_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void sparks_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((sparks_t *)st)->vy);
    tx_cloud_key(&((sparks_t *)st)->cloud, note, vel, on);
}

static void sparks_param(void *st, uint8_t id, uint8_t val)
{
    sparks_t *s = (sparks_t *)st;
    if (id == TX_PID_PARTICLES) s->limit = (uint8_t)(val < 1u ? 1u : val > SPARK_MAX ? SPARK_MAX : val);
    else if (id == TX_PID_LEVEL) s->level = tx_level_q15(val > 127u ? 127u : val);
    else set_param(s, id, val);
}

static uint32_t xs(uint32_t *r) { uint32_t x = *r; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *r = x; }

/* one discharge -> its kernel in tick[] */
static void spark(sparks_t *s, const tx_event_t *e)
{
    int32_t d_l2 = s->gap_l2 - ((int32_t)e->note - 60) * 2731 + ((int32_t)e->vel - 100) * 1638   /* 2^(1/24) / key */
                 - tx_vary_tone(&s->vy);                  /* the character: brighter = a narrower gap */
    int32_t t_l2, p_l2, a_l2, x, y1 = 0, y2 = 0, c, a, b, h0, h1, h2;
    uint32_t fc, k, u = xs(&s->rng_s), v = xs(&s->rng_s), ph, i, j, m, off;
    int32_t a0;
    d_l2 += (int32_t)((u >> 16) * (2u * SPK_GAP) >> 16) - SPK_GAP;   /* this spark's channel */
    for (i = 0; i < NB; i++)                                 /* where it strikes the body: each mode's drive */
        s->gin[i] = (int32_t)(tx_exp2((int32_t)((((v >> (8u * i)) & 255u) * (2u * SPK_MODE * 10885u)) >> 8)
                                      + (8 << 16) - SPK_MODE * 10885))   /* (10885: 1 dB in log2 Q16) */
                    * ((u >> (12u + i)) & 1u ? -1 : 1);       /* the mode's shape at that spot: either sign */
    if (d_l2 < -131072) d_l2 = -131072;                      /* 0.25 .. 32 mm */
    if (d_l2 > 327680) d_l2 = 327680;
    t_l2 = 65536 + (4 * (d_l2 + 65536)) / 5 + s->len_l2;     /* log2(T / 1 us) = 1 + 0.8 log2(d / 0.5 mm) + len */
    p_l2 = d_l2 < 131072 ? d_l2 / 5 : 26214 + (6 * (d_l2 - 131072)) / 5;   /* P_s: d^0.2 below 4 mm, d^1.2 above */
    /* P_s T^2 against the reference spark (d 4 mm: T 10.6 us, log2 3.4), the gap's part at half its dB */
    a_l2 = TICK_REF_L2 + (p_l2 - 26214 + 2 * (t_l2 - 65536 - 157286 - s->len_l2)) / 2 + 2 * s->len_l2 + s->att_l2
         + (e->size_l2 - (2 << 12)) * 8;                     /* energy draw: amplitude ~ sqrt(E), the median = 0 */
    if (a_l2 > (15 << 16)) a_l2 = 15 << 16;
    a = (int32_t)(((uint64_t)tx_exp2(a_l2) * (uint32_t)s->level) >> 15);
    if (a > 32767) a = 32767;
    if (a < 2) return;
    a0 = a;
    /* the N-wave's peak 0.33 / T as two one-pole low-passes: f = 0.33e6 / T_us Hz, c = 1 - exp(-2 pi f / fs) */
    fc = tx_exp2((int32_t)(1201413 - t_l2));                 /* log2(330000) = 18.33, Q16 -> Hz */
    if (fc > 19000u) fc = 19000u;
    c = 16384 - (int32_t)tx_exp2((14 << 16) - (int32_t)(fc * 862u >> 6));   /* Q14 */
    /* the strokes: the gap breaks down, and as the charge recovers it may re-strike once or twice */
    for (m = 1u + xs(&s->rng_s) % 3u, j = 0, off = e->offset; j < m; j++) {
        if (j) {
            uint32_t w = xs(&s->rng_s);
            off += tx_exp2((1 << 16) + (int32_t)(((w >> 16) * SPK_DT) >> 16));   /* 2 .. ~22 samples, log-uniform */
            if (off >= TX_BLOCK) break;                      /* (within the carry) */
            a = (int32_t)(((uint32_t)a0 * (SPK_RE_LO + (((w & 0xFFFFu) * (SPK_RE_HI - SPK_RE_LO)) >> 16))) >> 15);
            u = w;
        }
        /* the doublet [+a, -b] at a sub-sample onset (linear between samples), its tail b = a x SPK_TAIL .. 1 */
        ph = (u >> 4) & 255u;
        {   /* the split keeps the doublet's energy: x sqrt(2 / (6 p^2 - 6 p + 2)), p the onset's fraction */
            uint32_t en = 6u * ph * ph + 131072u - 1536u * ph;   /* Q16: 0.5 .. 2 */
            int32_t sc = (int32_t)tx_exp2((((17 << 16) - log2_q16(en)) >> 1) + (12 << 16));   /* Q12: 1 .. 2 */
            a = (a * sc) >> 12;
        }
        b = (int32_t)(((uint32_t)a * (SPK_TAIL + (((u & 15u) * (32768u - SPK_TAIL)) >> 4))) >> 15);
        h0 = (a * (int32_t)(256u - ph)) >> 8;
        h1 = ((a * (int32_t)ph) >> 8) - ((b * (int32_t)(256u - ph)) >> 8);
        h2 = -((b * (int32_t)ph) >> 8);
        y1 = y2 = 0;
        for (k = 0; k < KLEN; k++) {
            x = k == 0 ? h0 : k == 1 ? h1 : k == 2 ? h2 : 0;
            y1 += ((x - y1) * c) >> 14;
            y2 += ((y1 - y2) * c) >> 14;
            s->tick[off + k] += y2 * 256;                    /* Q15 -> Q23 */
        }
    }
}

TX_AUDIO_CODE static void sparks_render(void *st, int32_t *mix, unsigned n)
{
    sparks_t *s = (sparks_t *)st;
    unsigned k, i, j, held;
    int32_t *t = s->tick, *buf = s->buf, any = 0;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    s->sparks = 0;
    for (i = 0; i < k; i++) {
        if (s->sparks >= s->limit) { s->dropped++; continue; }
        s->sparks++;
        spark(s, &s->ev[i]);
    }
    for (j = 0; j < n; j++) any |= t[j];
    /* body: the ticks through the shared bank helper (unit-peak modes), mixed with the dry ticks */
    if (any || !tx_svf_bank_quiet(s->body, NB)) {
        uint32_t wv = tx_vary_scale((uint32_t)s->wet, -tx_vary_tone(&s->vy));   /* the character: duller = */
        int32_t wet = wv > 32767u ? 32767 : (int32_t)wv, dry = 32768 - (wet >> 1), g[NB];   /* more of the body */
        for (j = 0; j < n; j++) buf[j] = (t[j] >> 15) * dry + (((t[j] & 0x7FFF) * dry) >> 15);
        for (i = 0; i < NB; i++) g[i] = wet >> 4;            /* Q11 */
        if (wet) tx_svf_bank_add_g(s->body, s->f, s->q, s->gin, g, NB, t, buf, n);
        else for (i = 0; i < NB; i++) s->body[i].low = s->body[i].band = 0;
    } else
        for (j = 0; j < n; j++) buf[j] = 0;
    for (j = 0; j < KLEN; j++) { t[j] = t[n + j]; t[n + j] = 0; }   /* carry the kernels' tails */
    for (j = KLEN; j < n; j++) t[j] = 0;
    /* corona: while keys are held */
    held = tx_cloud_held(&s->cloud);
    {
        int32_t tgt = held ? (int32_t)(((uint32_t)s->corona * (uint32_t)s->level) >> 15) : 0;
        int32_t g0 = s->cor_g, g1 = g0 + ((tgt - g0) >> 2), gs, g;
        if (g1 == g0) g1 = tgt;
        if (!tgt && g1 < 64) g1 = 0;
        if (g0 || g1) {
            int32_t lp1 = s->c_lp1, lp2 = s->c_lp2, pend = s->c_pend;
            uint32_t r = s->rng_c;
            gs = (g1 - g0) / (int32_t)n;
            g = g0;
            for (j = 0; j < n; j++) {
                int32_t x = pend, h;
                pend = 0;
                if (xs(&r) < 0x20000000u) { x += 8192; pend = -5851; }   /* 1 in 8 samples; + then - / 1.4 */
                lp1 += ((x - lp1) * 18842) >> 15;            /* 6 kHz one-pole: high-pass = x - lp1 */
                h = x - lp1;
                lp2 += ((h - lp2) * 28304) >> 15;            /* 14 kHz one-pole low-pass */
                buf[j] += lp2 * (g >> 5);                    /* Q13 x Q10 -> Q23 */
                g += gs;
            }
            if (!g1) lp1 = lp2 = pend = 0;
            s->c_lp1 = lp1; s->c_lp2 = lp2; s->c_pend = pend; s->rng_c = r;
        }
        s->cor_g = g1;
    }
    tx_vary_block(&s->vy, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = 0;
        for (j = 0; j < KLEN; j++) z |= t[j];
        z |= !tx_svf_bank_quiet(s->body, NB);
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z && !s->cor_g && !s->hp.x1 && !s->hp.y1);
    }
}

static void sparks_stats(const void *st, tx_stats_t *o)
{
    const sparks_t *s = (const sparks_t *)st;
    o->particles = s->sparks;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: every key slot at 500 /s with aftershocks, the body ringing, the corona on */
static const char SCENE_MAX[] =
    TX_TEXT("0 particles 32\n0 param RATE 127\n0 param BURST 127\n0 param BODY 127\n0 param CORONA 127\n0 param GAP 127\n"
    "0 param DIST 0\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "500 on 53 127\n500 on 60 127\n500 on 67 127\n500 on 74 127\n"
    "1000 on 57 127\n1000 on 64 127\n1000 on 71 127\n1000 on 79 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4000 end\n");

const tx_engine_t tx_eng_sparks = {
    "sparks", TX_TEXT("#20 Static / sparks"), NP, PARAMS, SPARK_MAX, 16, TX_STATE_SIZE(sparks_t), SCENE_MAX,
    sparks_init, sparks_key, sparks_param, sparks_render, sparks_stats,
};
