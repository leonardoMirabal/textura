/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #29 Katydids / grasshoppers: stridulation, tooth by tooth (INSECT + CHORUS cores).
 *
 * A key press puts a singer (and CHORUS companions farther out) in the grass; holding keeps it calling; release lets
 * it finish its call. Two families:
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
#include "tx_core_cloud.h"
#include "tx_core_insect.h"
#include "tx_core_chorus.h"

#define OUT_TRIM 14202               /* output gain, Q12 (+8.5 dB): integrated (a key held 10 s): -34.5 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +2.3 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -130
#define VARY_HI 15
#define VARY_TILT 140
#define VARY_DRIFT 60
#define VARY_SPLIT 0

enum { P_SPECIES, P_JITTER, P_PEGS, P_TEMP, P_CHORUS, P_SYNC, P_DIST, NP };

static const tx_param_t PARAMS[NP] = {
    {"SPECIES", 127, 0, "true katydid rasp 4.5 kHz | katydid Panacanthus 5 kHz (tonal) | grasshopper C. biguttulus 6.6 kHz | C. brunneus 9.7 kHz"},
    {"JITTER", 127, 12, "strike timing SD: 0 .. 50 % of an interval: a pure tone .. a broadband rasp"},
    {"PEGS", 127, 50, "grasshoppers: peg impacts within a pulse 1.5 .. 6 kHz (log; N30 >= 1.5 kHz)"},
    {"TEMP", 127, 76, "temperature 10 .. 35 C: call and syllable rhythm (Q10 2); the spectrum stays"},
    {"CHORUS", 127, 0, "companions per key: 0 .. 7 singers farther out"},
    {"SYNC", 127, 64, "reset by neighbours s: 0 free .. 0.5 alternate .. 1 synchrony"},
    {"DIST", 127, 40, "distance at velocity 100: 0.3 .. 30 m (log): level 1 / r"},
};

#define NV 16u
#define NSP 4u
#define NEV 96u                    /* strikes per voice per block, at most */
#define G_REF 1600                 /* strike amplitude at 1 m (Q15; calibrated by render) */

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
    {4500, 9000, 64, 80, 7000, 2, 3, 200, 0, 1500, 0, 0, 650},    /* a true katydid's rasp: 3 syllables ("ka-ty-did") of
                                                                    * ~45 ms, 150 ms apart, a call per 0.65 s; wing Q 4,
                                                                    * uneven teeth at half the carrier rate (ours, fitted
                                                                    * to two katydid recordings: peak 5 kHz, broad 2.5-5
                                                                    * kHz, 5-6 onsets /s, soft 25-30 ms attacks) */
    {5000, 14700, 264, 160, 11024, 1, 0, 226, 0, 0, 0, 0, 600},    /* Panacanthus (N8c); 14.7 kHz -9.4 dB */
    {6570, 18000, 96, 96, 26000, 0, 3, 0, 80, 200, 730, 24, 3500}, /* C. biguttulus (N25, N30); upper -2 dB (ours) */
    {9720, 19500, 96, 96, 20000, 0, 1, 0, 75, 100, 100, 20, 2000}, /* C. brunneus (N25: 7-8 ms pulses at ~100 /s) */
};

typedef struct {
    tx_ireso_t r1, r2;
    int32_t next_q8[2];            /* the next strike of each leg (katydid: one wing), samples from the block start */
    int32_t t;                     /* samples since the call started (at the block start) */
    int32_t gain;                  /* strike amplitude, Q15 */
    int32_t tf;                    /* own period factor, Q15 */
    uint32_t f1;                   /* carrier / low peak at this key, Hz */
    uint8_t in_call, em, note, alive, stop, pad[3];
} singer_t;

typedef struct {
    tx_cloud_t cloud;
    tx_chorus_t ch;
    singer_t v[NV];
    int16_t env[TX_ITONE_ENV + 1];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    uint8_t who[TX_CHORUS_MAX];
    uint16_t off[TX_CHORUS_MAX];
    uint16_t eo[2][NEV], mo[NEV];  /* per-leg strike offsets and amplitudes, then merged */
    int16_t ea[2][NEV], ma[NEV];
    uint8_t p[NP], limit, quiet;
    int32_t level, q15, jit, peg_l2, dist_l2;
    uint16_t dropped;
    tx_vary_t vy;
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
static int32_t call_len(const katy_t *s, uint32_t f1)
{
    const species_t *sp = sp_of(s);
    if (sp->katydid == 2u) return ms10(s, (uint32_t)sp->pspc * (sp->pulses - 1u)) + (int32_t)(sp->teeth * TX_FS / f1);
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
    if (on && vel) tx_vary_press(&((katy_t *)st)->vy);
    tx_cloud_key(&((katy_t *)st)->cloud, note, vel, on);
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

static int quiet_v(const singer_t *v) { return !v->alive && tx_ireso_quiet(&v->r1) && tx_ireso_quiet(&v->r2); }

/* the two resonances for a carrier f1 (Hz) */
static void tune(const species_t *sp, singer_t *v, uint32_t f1)
{
    if (f1 > 16000u) f1 = 16000u;
    tx_ireso_set(&v->r1, f1, sp->q1);
    tx_ireso_set(&v->r2, sp->katydid == 1u ? (f1 * 2940u / 1000u > 20000u ? 20000u : f1 * 2940u / 1000u)
                         : sp->katydid ? f1 * 2u : sp->f2, sp->q2);
}

static void spawn(katy_t *s, const tx_event_t *e)
{
    const species_t *sp = sp_of(s);
    unsigned n = 1u + (s->p[P_CHORUS] >> 4), k, i;
    tx_rng_t *r = &s->cloud.rng;
    int32_t r0_l2 = s->dist_l2 + ((int32_t)100 - (int32_t)e->vel) * 1638;
    for (k = 0; k < n; k++) {
        singer_t *v;
        unsigned vi = NV;
        int32_t rl2 = r0_l2, rr, ang, f_l2;
        for (i = 0; i < s->limit && vi == NV; i++) if (quiet_v(&s->v[i])) vi = i;
        for (i = 0; i < s->limit && vi == NV; i++) if (!s->v[i].alive) vi = i;
        for (i = 0; i < s->limit && vi == NV; i++) if (s->v[i].stop) vi = i;
        if (vi == NV) { s->dropped++; continue; }
        v = &s->v[vi];
        if (k) rl2 += 38336 + (int32_t)tx_rand_below(r, 92736u);
        if (rl2 > 9 << 16) rl2 = 9 << 16;
        rr = (int32_t)tx_exp2(rl2 + (10 << 16));
        ang = (int32_t)tx_rand(r);
        tx_chorus_place(&s->ch, vi, (tx_sin_q15((uint32_t)ang + 0x40000000u) * (rr >> 4)) >> 11,
                        (tx_sin_q15((uint32_t)ang) * (rr >> 4)) >> 11);
        v->gain = (int32_t)((uint32_t)G_REF * 1000u / (uint32_t)(rr < 300 ? 300 : rr));
        f_l2 = ((int32_t)e->note - 60) * 2731 + (tx_vary_tone(&s->vy) >> 3);   /* the character (warmer = higher, faster) */
        if (k) f_l2 += (tx_gauss_q12(r) * 2789) >> 12;       /* individuals: CV 3 % (ours) */
        v->f1 = (uint32_t)(((uint64_t)sp->f1 * tx_exp2(f_l2 + (16 << 16))) >> 16);
        if (v->f1 > 16000u) v->f1 = 16000u;
        tune(sp, v, v->f1);
        v->r1.g = sp->katydid == 1u ? 256 : sp->katydid ? 900 : 640;   /* clicks / a rasp do not build up: x 2.5-3.5 */
        v->r2.g = sp->katydid == 1u ? (sp->l2 >> 7) * 10 : (sp->l2 * 5) >> 8;   /* the upper level; katydid: x 10 for */
                                                                          /* the low mode's build-up (by render) */
        v->tf = 32768 + ((tx_gauss_q12(r) * 1638) >> 12);   /* own period CV 5 % (ours) */
        v->alive = 1; v->stop = 0; v->em = e->emitter; v->note = e->note; v->in_call = 0;
        tx_chorus_start(&s->ch, vi, call_T(s, v->tf), k ? tx_rand_below(r, 65535u) % (uint32_t)call_T(s, v->tf) : 0);
    }
}

/* the strike interval: the period x (1 + JITTER x a normal draw), at least 1/4 of it */
static int32_t interval_q8(katy_t *s, int32_t per_q8)
{
    int32_t j = sp_of(s)->katydid == 2u && s->jit < 11469 ? 11469 : s->jit;   /* a rasp: uneven teeth (SD >= 35 %) */
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
    int32_t len = call_len(s, v->f1);
    unsigned legs = sp->katydid ? 1u : 2u, L, cnt[2] = {0, 0}, a, b, m;
    int32_t per_q8 = sp->katydid ? (int32_t)((TX_FS << 8) / (sp->katydid == 2u ? v->f1 >> 1 : v->f1)) : (int32_t)((TX_FS << 8) / tx_exp2(s->peg_l2));
    for (L = 0; L < legs; L++) {
        while (v->in_call && (v->next_q8[L] >> 8) < (int32_t)n && cnt[L] < NEV) {
            int32_t o = v->next_q8[L] >> 8, ts = v->t + o, e, amp;
            if (ts >= len) { if (L + 1 == legs) v->in_call = 0; break; }
            if (sp->katydid == 2u) {                         /* syllables: each its own sweep of the file */
                int32_t spc = ms10(s, sp->pspc), sl = (int32_t)(sp->teeth * TX_FS / v->f1), u = ts % spc;
                uint32_t pos = u < sl ? (uint32_t)(((uint32_t)u << 6) / (uint32_t)sl) : 64u;
                e = s->env[pos > 64u ? 64u : pos];
            } else if (sp->katydid) {
                uint32_t pos = (uint32_t)(((uint32_t)ts << 6) / (uint32_t)len);   /* along the call: the file */
                e = s->env[pos > 64u ? 64u : pos];
            } else
                e = leg_env(s, sp, L ? ts + (ms10(s, sp->syl) >> 1) + (ts >> 6) : ts);   /* leg 2 drifts (ours) */
            amp = (int32_t)(((int64_t)g * e) >> 15);
            if (amp) { s->eo[L][cnt[L]] = (uint16_t)o; s->ea[L][cnt[L]] = (int16_t)(amp > 32767 ? 32767 : amp); cnt[L]++; }
            v->next_q8[L] += interval_q8(s, per_q8);
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
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    for (i = 0; i < k; i++) spawn(s, &s->ev[i]);
    ncall = tx_chorus_block(&s->ch, &s->cloud.rng, n, s->who, s->off, TX_CHORUS_MAX);
    for (i = 0; i < ncall; i++) {
        singer_t *v = &s->v[s->who[i]];
        if (!v->alive || v->stop) continue;
        v->in_call = 1;
        tune(sp_of(s), v, tx_vary_scale(v->f1, tx_vary_drift(&s->vy) >> 3));   /* the night warms, cools */
        v->t = -(int32_t)s->off[i];
        v->next_q8[0] = v->next_q8[1] = (int32_t)s->off[i] << 8;
    }
    for (i = 0; i < NV; i++) {
        singer_t *v = &s->v[i];
        unsigned m = 0;
        int held;
        if (quiet_v(v)) continue;
        held = v->alive && v->em < TX_CLOUD_EMITTERS && s->cloud.em[v->em].held && s->cloud.em[v->em].note == v->note;
        if (v->alive && !held && !v->stop) { v->stop = 1; tx_chorus_stop(&s->ch, i); }
        if (v->in_call) m = strikes(s, v, n);
        if (v->in_call) v->t += (int32_t)n;
        tx_ireso_add(&v->r1, s->mo, s->ma, m, mix, n);
        tx_ireso_add(&v->r2, s->mo, s->ma, m, mix, n);
        if (v->stop && !v->in_call) { v->alive = 0; v->stop = 0; v->em = 0xFF; }
    }
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = 0;
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
    for (j = 0; j < n; j++) b[j] = 0;
    katy_render_raw(st, b, n);
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

/* the worst case: 16 katydids calling in synchrony (every resonator rings at once), jittered, close, hot */
static const char SCENE_MAX[] =
    "0 particles 16\n0 param SPECIES 40\n0 param PEGS 127\n0 param JITTER 127\n0 param CHORUS 127\n0 param SYNC 127\n"
    "0 param DIST 0\n0 param TEMP 127\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_katydids = {
    "katydids", "#29 Katydids / grasshoppers", NP, PARAMS, NV, 8, sizeof(katy_t), SCENE_MAX,
    katy_init, katy_key, katy_param, katy_render, katy_stats,
};
