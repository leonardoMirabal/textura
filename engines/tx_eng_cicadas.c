/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #28 Cicadas: tymbal buckling into the air sac (INSECT core, TYMBAL).
 *
 * A key press starts a cicada calling (the song swells in over ~0.3 s), holding keeps it calling, release lets it
 * fall silent (time constant ~0.37 s: ours). Physics (N4, N5, N6):
 *   tymbal   the ribs buckle in sequence, each an impulse every second cycle of the tymbal resonance (N6: 3-4 ribs;
 *            here 4); the in-click is a short tone burst (~5 cycles build-up, ~5 plateau, ~5 decay: ~3.5 ms at
 *            4.3 kHz, N4) whose pitch falls while the tymbal buckles (5.5 -> 4.3 kHz) and in its tail (3.84 -> 3.2,
 *            -17 %, N5): here the click glides from 1.15 to 0.85 x the carrier over 15 cycles (ours, both together).
 *   sac      the abdominal air sac, a Helmholtz resonator coupled to the tymbal (tymbal Q 9.3, sac Q 3.4, together
 *            ~12.5: N5): a low-Q two-pole at the carrier in series.
 *   rhythm   the two tymbals alternate, each clicking at 100-250 Hz (N4: 100-200; Cyclochila 230 Hz together);
 *            out-clicks (the tymbal snapping back) > 20 dB quieter and higher (~6 kHz): here -22 dB, from 1.4 x.
 *   loud     148.5 dB SPL next to the tympana (N5): velocity = loudness (as the note's controls say).
 * Ours: the swell and the fade, the rib / click jitter of COHERENCE, the companions of CHORUS (rate CV 5 %, carrier
 * CV 4 %, 1.5-4 x farther, free-running: no cicada synchrony data in the note). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_radiate.h"
#include "tx_core_cloud.h"
#include "tx_core_insect.h"
#include "tx_core_chorus.h"

#define OUT_TRIM 6324               /* output gain, Q12 (+1.7 dB with the spectrogram fixes, session 5; +2.7 dB): integrated (a key held 10 s): -29.8 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter;
                                    * lowered 1.1 dB: the default chord <= +12 dBFS, the max scene <= +24 dBFS) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM -0.6 dB with it, held there by the max scene under +24.1 dBFS (its mean
 * loudness below the old one) */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 0

enum { P_RATE, P_CARRIER, P_Q, P_COHERE, P_CHORUS, P_DIST, P_PHRASE, NP };

static const tx_param_t PARAMS[NP] = {
    {"RATE", 127, 20, "clicks per tymbal: 100 .. 250 Hz (log; the two tymbals together twice that: 230 Hz Cyclochila)"},
    {"CARRIER", 127, 98, "tymbal / air-sac resonance at C4: 0.8 .. 7 kHz (log; 4.3 kHz Cyclochila); keys a quarter tone each"},
    {"Q", 127, 45, "tymbal Q: 4 .. 30 (log; 9.3 measured): buzzy .. ringing"},
    {"COHERE", 127, 100, "rib coherence: buzzy (ribs and clicks jitter) .. a clean tone"},
    {"CHORUS", 127, 0, "companions per key: 0 .. 7 cicadas farther out"},
    {"DIST", 127, 40, "distance: 0.3 .. 30 m (log): level 1 / r"},
    {"PHRASE", 127, 70, "pulse groups of the song: steady .. 20 /s (recordings: 7-13 /s)"},
};

#define NV 16u
#define NRIB 4u
#define NPEND 40u
#define G_REF 5000                 /* output gain at 1 m, Q12 (calibrated by render: the chord under full scale) */
#define RIB_AMP 3000               /* rib impulse, Q15 */

typedef struct {
    tx_tymbal_t ty;
    tx_tev_t pend[NPEND];
    uint32_t next_q8;              /* samples to the next click from the block start, Q8 */
    uint32_t per_q8;               /* half the click period of one tymbal (the two alternate), Q8 */
    uint32_t f;                    /* carrier, Hz */
    int32_t a1s, a1o, dur;         /* a click's start a1, an out-click's start a1, the glide length (samples) */
    int32_t gain, env;             /* Q12 at this distance, the swell (Q15) */
    uint32_t pph, pinc;            /* the pulse-group phase and its step per block */
    int32_t pmod;                  /* the pulse-group gain now, Q15 */
    uint8_t npend, side, em, note, alive, vel, pad[2];
} cicada_t;

typedef struct {
    tx_cloud_t cloud;
    cicada_t c[NV];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    uint8_t p[NP], limit, quiet;
    int32_t level, rate_l2, car_l2, q_x16, jit, dist_l2;
    uint16_t dropped;
    tx_vary_t vy;
    tx_rad_t rad;                  /* the body radiates its lows poorly (tx_core_radiate.h) */
} cicadas_t;

static void tune(cicadas_t *s, cicada_t *c)
{
    int32_t a2, b0, a1e, sa1, sa2, sb0;
    uint32_t f = c->f;
    tx_ireso_coefs(f * 1150u / 1000u, (uint32_t)s->q_x16, &c->a1s, &a2, &b0);   /* the glide's start */
    tx_ireso_coefs(f * 1400u / 1000u, (uint32_t)s->q_x16, &c->a1o, &a2, &b0);   /* the out-click's start */
    tx_ireso_coefs(f * 850u / 1000u, (uint32_t)s->q_x16, &a1e, &a2, &b0);       /* the glide's end */
    tx_ireso_coefs(f, (uint32_t)s->q_x16, &c->ty.a1, &c->ty.a2, &c->ty.b0);     /* r and the input gain at f */
    tx_ireso_coefs(f, 54u, &sa1, &sa2, &sb0);                                   /* the sac, Q 3.4 (N5) */
    c->ty.a1_end = a1e;
    c->ty.sa1 = sa1; c->ty.sa2 = sa2; c->ty.sb0 = sb0;
    c->dur = (int32_t)(15u * TX_FS / f);                                        /* 15 cycles */
    c->ty.da1 = (int32_t)((uint32_t)(a1e - c->a1s > 0 ? a1e - c->a1s : 0) * 256u / (uint32_t)(c->dur + 1));
}

static void set_param(cicadas_t *s, unsigned id, unsigned v)
{
    unsigned i;
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_RATE: s->rate_l2 = 435412 + (int32_t)(v * 683u); break;      /* log2(100 Hz) + v log2(2.5) / 127, Q16 */
    case P_CARRIER: s->car_l2 = 630302 + (int32_t)(v * 1614u); break;   /* log2(800 Hz) + v log2(8.75) / 127 */
    case P_Q: s->q_x16 = (int32_t)tx_exp2(262144 + (int32_t)(v * 1504u)); break;   /* 64 .. 480 (Q 4 .. 30) */
    case P_COHERE: s->jit = (127 - (int32_t)v) * 258; break;           /* Q15: 1 .. 0 */
    case P_DIST: s->dist_l2 = -113738 + (int32_t)(v * 3482u); break;  /* log2(0.3 m) + v log2(100) / 127 */
    default: break;
    }
    if (id == P_Q)                                           /* the carrier moves new calls only (a key's pitch) */
        for (i = 0; i < NV; i++) if (s->c[i].alive) tune(s, &s->c[i]);
}

static void cicadas_init(void *st, uint32_t seed)
{
    cicadas_t *s = (cicadas_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    for (i = 0; i < NV; i++) s->c[i].em = 0xFF;
    s->limit = 8;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((cicadas_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void cicadas_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((cicadas_t *)st)->vy);
    tx_cloud_key(&((cicadas_t *)st)->cloud, note, vel, on);
}

static void cicadas_param(void *st, uint8_t id, uint8_t val)
{
    cicadas_t *s = (cicadas_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NV ? NV : val);
        for (i = s->limit; i < NV; i++) { s->c[i].alive = 0; s->c[i].em = 0xFF; s->c[i].npend = 0; }   /* rings out */
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

static int quiet_c(const cicada_t *c) { return !c->alive && !c->npend && tx_tymbal_quiet(&c->ty); }

static void spawn(cicadas_t *s, const tx_event_t *e)
{
    unsigned n = 1u + (s->p[P_CHORUS] >> 4), k, i;
    tx_rng_t *r = &s->cloud.rng;
    for (k = 0; k < n; k++) {
        cicada_t *c = 0;
        int32_t f_l2, rl2 = s->dist_l2, rr;
        for (i = 0; i < s->limit && !c; i++) if (quiet_c(&s->c[i])) c = &s->c[i];
        for (i = 0; i < s->limit && !c; i++) if (!s->c[i].alive) c = &s->c[i];
        if (!c) { s->dropped++; continue; }
        f_l2 = s->car_l2 + ((int32_t)e->note - 60) * 2731 + (tx_vary_tone(&s->vy) >> 3);  /* a quarter tone per key; the character */
        if (k) {
            f_l2 += (tx_gauss_q12(r) * 3718) >> 12;          /* carrier CV 8 % */
            rl2 += 38336 + (int32_t)tx_rand_below(r, 92736u);   /* 1.5 .. 4 x farther */
        }
        c->f = tx_exp2(f_l2);
        if (c->f > 8000u) c->f = 8000u;
        tune(s, c);
        rr = (int32_t)tx_exp2(rl2 + (10 << 16));             /* mm */
        c->gain = (int32_t)((uint32_t)G_REF * 1000u / (uint32_t)(rr < 300 ? 300 : rr));
        {
            int32_t rt = s->rate_l2 + (k ? (tx_gauss_q12(r) * 4648) >> 12 : 0);   /* rate CV 10 % */
            c->per_q8 = (TX_FS << 7) / tx_exp2(rt) ;          /* fs / (2 R), Q8 */
        }
        if (!c->alive) { c->next_q8 = k ? tx_rand_below(r, 65535u) % (c->per_q8 * 2u) : 0; c->env = 0; c->npend = 0; }
        c->pinc = (uint32_t)(((uint64_t)s->p[P_PHRASE] * 1963000u * (60000u + tx_rand_below(r, 11000u))) >> 16);   /* sin^2 at 0 .. 20 Hz x 0.92-1.08 */
        c->alive = 1; c->em = e->emitter; c->note = e->note; c->vel = e->vel;
    }
}

static void push(cicada_t *c, int32_t off, int32_t amp, int32_t a1)
{
    tx_tev_t *t;
    if (c->npend >= NPEND) return;
    t = &c->pend[c->npend++];
    t->off = (uint16_t)(off < 0 ? 0 : off > 65535 ? 65535 : off);
    t->amp = (int16_t)(amp > 32767 ? 32767 : amp < -32767 ? -32767 : amp);
    t->a1 = a1;
}

/* the clicks starting in this block: 4 ribs 2 cycles apart (jittered by 1 - COHERE), and the out-click half way to
 * the next click of the same tymbal */
static void clicks(cicadas_t *s, cicada_t *c, unsigned n)
{
    tx_rng_t *r = &s->cloud.rng;
    int32_t cyc = (int32_t)(TX_FS * 2u / c->f), k;
    while ((c->next_q8 >> 8) < n) {
        int32_t t0 = (int32_t)(c->next_q8 >> 8);
        int32_t a = (int32_t)(((int64_t)RIB_AMP * c->env) >> 15);
        a = (int32_t)(((int64_t)a * c->pmod) >> 15);
        if (c->side) a = (a * 29205) >> 15;                  /* the other tymbal -1 dB (ours) */
        for (k = 0; k < (int32_t)NRIB; k++) {
            int32_t dj = (int32_t)(((int64_t)tx_rand_s15(r) * s->jit) >> 15) * cyc >> 16;   /* +-0.5 cycle x jit */
            int32_t aj = a + (int32_t)(((int64_t)tx_rand_s15(r) * s->jit >> 15) * a >> 16);
            push(c, t0 + k * cyc + (k ? dj : 0), aj, k ? 0 : c->a1s);
        }
        push(c, t0 + (int32_t)(c->per_q8 >> 8), (a * 2600) >> 15, c->a1o);   /* out-click -22 dB */
        c->side ^= 1u;
        {   /* the next click (the other tymbal): +-15 % x (1 - COHERE) */
            int32_t pj = (int32_t)(((int64_t)tx_rand_s15(r) * s->jit) >> 15);
            c->next_q8 += (uint32_t)((int32_t)c->per_q8 + (int32_t)(((int64_t)(int32_t)(c->per_q8 >> 4) * pj * 5) >> 16));
        }
    }
    c->next_q8 -= n << 8;
}

TX_AUDIO_CODE static void cicadas_render_raw(void *st, int32_t *mix, unsigned n)
{
    cicadas_t *s = (cicadas_t *)st;
    unsigned k, i, j;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    for (i = 0; i < k; i++) spawn(s, &s->ev[i]);
    for (i = 0; i < NV; i++) {
        cicada_t *c = &s->c[i];
        int held;
        unsigned m;
        if (quiet_c(c)) continue;
        held = c->alive && c->em < TX_CLOUD_EMITTERS && s->cloud.em[c->em].held && s->cloud.em[c->em].note == c->note;
        if (held) c->env += (32767 - c->env) >> 6;           /* the swell: ~0.37 s */
        else c->env -= (c->env >> 6) + 1;                    /* the fade */
        if (c->env < 0) c->env = 0;
        if (c->alive && !held && c->env < 64) { c->alive = 0; c->em = 0xFF; }
        if (c->alive) {                                      /* the pulse groups: 0.55 + 0.45 sin^2 (ours, from the */
            int32_t sn;                                      /* recordings' 7-13 Hz modulation, depth 0.17-0.38) */
            c->pph += c->pinc;
            sn = tx_sin_q15(c->pph);
            c->pmod = c->pinc ? 18022 + (int32_t)(((int64_t)sn * sn * 14745) >> 30) : 32767;
            clicks(s, c, n);
        }
        for (j = 1; j < c->npend; j++) {                     /* insertion sort by offset (a few events) */
            tx_tev_t t = c->pend[j];
            unsigned q = j;
            while (q && c->pend[q - 1].off > t.off) { c->pend[q] = c->pend[q - 1]; q--; }
            c->pend[q] = t;
        }
        for (m = 0; m < c->npend && c->pend[m].off < n; m++) { }
        {
            int32_t g = (int32_t)(((int64_t)c->gain * s->level) >> 15);
            g = (int32_t)(((int64_t)g * c->vel * c->vel) >> 14);   /* velocity = loudness (v / 127)^2 */
            c->ty.g = g > 6000 ? 6000 : g;                      /* closer than ~0.8 m: no louder (headroom) */
        }
        {   /* the ribs' own clicks, radiated directly (a 2-sample spike each: brighter, as the recordings */
            unsigned q;                                      /* keep 5-12 kHz within ~10 dB of the peak: ours) */
            for (q = 0; q < m; q++) {
                int32_t d = (c->pend[q].amp * c->ty.g) >> 4;
                mix[c->pend[q].off] += d;
                if (c->pend[q].off + 1u < n) mix[c->pend[q].off + 1u] -= d;
            }
        }
        tx_tymbal_add(&c->ty, c->pend, m, mix, n);
        for (j = m; j < c->npend; j++) { c->pend[j - m] = c->pend[j]; c->pend[j - m].off = (uint16_t)(c->pend[j].off - n); }
        c->npend = (uint8_t)(c->npend - m);
        if (!c->alive) c->npend = 0;
    }
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = 0;
        for (i = 0; i < NV; i++) z |= !quiet_c(&s->c[i]);
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

/* the engine into its own block, then trimmed into the mix (OUT_TRIM) */
TX_AUDIO_CODE static void cicadas_render(void *st, int32_t *mix, unsigned n)
{
    int32_t b[TX_BLOCK];
    unsigned j;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) b[j] = 0;
    cicadas_render_raw(st, b, n);
    tx_rad_hp(&((cicadas_t *)st)->rad, b, n, 3);   /* a cicada radiates under ~1 kHz poorly: -12 dB / oct */
    if (!tx_rad_quiet(&((cicadas_t *)st)->rad)) ((cicadas_t *)st)->quiet = 0;
    tx_vary_block(&((cicadas_t *)st)->vy, b, n);
    if (((cicadas_t *)st)->vy.lp) ((cicadas_t *)st)->quiet = 0;          /* the split still settling */
    tx_trim_add(b, mix, n, OUT_TRIM);
}

static void cicadas_stats(const void *st, tx_stats_t *o)
{
    const cicadas_t *s = (const cicadas_t *)st;
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

/* the worst case: 16 cicadas, the fastest clicks, a low carrier (long rib trains), buzzy, close */
static const char SCENE_MAX[] =
    "0 particles 16\n0 param RATE 127\n0 param CARRIER 0\n0 param COHERE 0\n0 param CHORUS 127\n0 param DIST 0\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_cicadas = {
    "cicadas", "#28 Cicadas", NP, PARAMS, NV, 8, sizeof(cicadas_t), SCENE_MAX,
    cicadas_init, cicadas_key, cicadas_param, cicadas_render, cicadas_stats,
};
