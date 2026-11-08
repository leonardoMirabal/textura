/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #10 Stone drum / lithophone: struck stones (MODAL + CONTACT cores).
 *
 * Each key strikes a stone tuned to it (velocity = the mallet's speed); the stone rings out, a restrike adds to the
 * ring phase-correctly (MODAL). Physics:
 *   body     a linear modal body, constant-Q damping d_n = pi f_n / Q + sigma0 (I6 / I7; T60 = 2.2 Q / f). Q by
 *            mineral (note 05 table: granite ~180 (M9 / M10), marble ~300 (M9), sandstone ~100 (M3 / M8), slate and
 *            limestone flagged guesses, flint / chert ~160 and obsidian ~175 (M22, derived), quartz capped 2000-5000
 *            (M11 / M12): 4000 here); "ringing rock" basalt / diabase Q 2000 (measured on a struck ringing-rock
 *            recording: T60 4-7 s at 0.7-1.2 kHz; M3 / M7 give ~1000 for diabase, ultrasonic). A floor sigma0 >= 0.7 /s
 *            (air and support, ours) bounds the longest rings.
 *   shape    mode ratios: free-free bar 1 : 2.7565 : 5.4039 : 8.9330 : 13.3443 (Euler-Bernoulli); a Korean
 *            pyeon-gyoung slab 1 : 1.5 : 2.3 : 3 (M16; the 5th and 6th ours); a boulder: 6 dense inharmonic modes,
 *            each 1.08-1.6 x the last (ours, from the reference's peaks: e.g. 668 / 980 / 1491 / 1978 / 2239 Hz),
 *            fixed per key. Strike position: the bar's mode shapes (asymptotic free-free form), slab / boulder a
 *            per-key pattern (ours).
 *   mallet   Hunt-Crossley contact (CONTACT core): stone hammer .. wood .. rubber .. felt; the contact time weights
 *            each mode by the half-sine force spectrum; the impulse ~ v (1 + e). Hard mallets bounce: 0-2
 *            micro-collisions within ~15 ms at -10 .. -20 dB (I1: "a few within ~15 ms"; their numbers ours).
 *   nonlinear above strain ~1e-7 rock loses more and its pitch drops (M9): for 50 ms after a hard hit Q_eff =
 *            Q / (1 + (v / 3 m/s)^2), the pitch down 5 % x min(1, v / 3 m/s), relaxing over ~100 ms (note 05's rule).
 * Keys: a semitone each from TUNE at C4. VARY (no two strikes alike, the user's session 3): at 0 each key is one fixed
 * stone; above, every strike is a new stone (shape, tuning, Q, strike point and speed drawn; ours). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_modal.h"
#include "tx_core_contact.h"

#define OUT_TRIM 13879               /* output gain, Q12 (+7.5 dB): max momentary (one velocity-100 press): -29.5 LUFS at LEVEL 100 -> target -20 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter;
                                    * lowered 2.0 dB: the limiter under ~8 dB in the default chord) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +3.1 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 140
#define VARY_DRIFT 40
#define VARY_SPLIT 3

enum { P_MATERIAL, P_SHAPE, P_MALLET, P_DAMP, P_POS, P_TUNE, P_VARY, NP };

static const tx_param_t PARAMS[NP] = {
    {"MATERIAL", 127, 72, TX_TEXT("granite Q180 | marble 300 | sandstone 100 | slate 185 | limestone 150 | ringing rock 2000 | flint 160 | obsidian 175 | quartz 4000")},
    {"SHAPE", 127, 127, TX_TEXT("bar (1 : 2.76 : 5.40 : 8.93) | slab (pyeon-gyoung 1 : 1.5 : 2.3 : 3) | boulder (dense, inharmonic)")},
    {"MALLET", 127, 0, TX_TEXT("stone hammer (an impulse) .. wood .. rubber .. felt (contact 0.1 .. 3 ms: darker)")},
    {"DAMP", 127, 0, TX_TEXT("a hand on the stone: extra damping 0.7 .. 60 /s (log)")},
    {"POS", 127, 40, TX_TEXT("strike position: the end / edge .. the centre (which modes sound)")},
    {"TUNE", 127, 75, TX_TEXT("the lowest mode at C4: 100 Hz .. 2 kHz (log); keys a semitone each")},
    {"VARY", 127, 100, TX_TEXT("each strike: the same stone per key (0: a lithophone) .. a new stone every time (shape, +-3 % tuning, Q +-30 %, strike point, hardness)")},
};

#define NV 12u
#define NM 5u                     /* (6 until note 18: the 6th mode of slab / boulder, -14 dB and above most
                                    * mallets' force spectrum, dropped for CPU) */
#define NMAT 9u
#define CULL_SH 10u                /* a mode stops 60 dB under its strike (2^-10): a ringing rock (Q 2000) rang to the
                                    * modal floor, -81 dB, ~5.4 s a strike; the last 21 dB, inaudible under the next
                                    * strikes, were a quarter of its cost */
#define A_REF 48000000             /* a mode's amplitude for a 2 m/s stone-hammer hit (Q30: -27 dBFS; by render) */

static const uint16_t MAT_Q[NMAT] = {180, 300, 100, 185, 150, 2000, 160, 175, 4000};
static const uint16_t BAR_R[5] = {1024, 2823, 5534, 9147, 13665};   /* Q10 */
static const uint16_t SLAB_R[6] = {1024, 1536, 2355, 3072, 3789, 4710};

typedef struct {
    tx_mode_t m[NM];
    uint32_t inc0[NM];
    uint16_t f[NM];                /* Hz */
    int32_t flat;                  /* pitch drop now, Q15 */
    uint16_t q_eff;                /* Q x 16 while young */
    uint8_t nm, note, young, alive;
    uint16_t moff[2];              /* micro-collisions: samples from the block start */
    int32_t mamp[2];               /* their impulse (Q15 of the main) */
    int32_t a0;                    /* the main hit's amplitude (Q30) */
    int16_t gw[NM];                /* each mode's gain x the force spectrum at its frequency (Q15), of the last hit */
} stone_t;

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    stone_t v[NV];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK];
    tx_mallet_t mallet;
    int32_t res, res_x1;           /* the attack residue: envelope (Q23-ish), the last noise (high-pass) */
    uint8_t p[NP], limit, quiet;
    int32_t level, f1_l2;
    uint32_t q_x16, s0_q8;
    uint16_t dropped;
    tx_vary_t vy;
} drum_t;

static unsigned shape_of(const drum_t *s) { return (unsigned)(s->p[P_SHAPE] * 3u) >> 7; }

static void set_param(drum_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_MATERIAL: s->q_x16 = (uint32_t)MAT_Q[(v * NMAT) >> 7] * 16u; break;
    case P_MALLET: tx_mallet_mix(&s->mallet, (int32_t)v * 258); break;
    case P_DAMP: s->s0_q8 = tx_knob_exp(v, 179u, 386437); break;           /* 0.7 .. 60 /s (6.4 oct), Q8 */
    case P_TUNE: s->f1_l2 = 435412 + (int32_t)(v * 2232u); break;         /* log2(100 Hz) + v log2(20) / 127 */
    default: break;
    }
}

static void drum_init(void *st, uint32_t seed)
{
    drum_t *s = (drum_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    s->limit = 6;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((drum_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void drum_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((drum_t *)st)->vy);
    tx_cloud_key(&((drum_t *)st)->cloud, note, vel, on);
}

static void drum_param(void *st, uint8_t id, uint8_t val)
{
    drum_t *s = (drum_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i, k;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NV ? NV : val);
        for (i = s->limit; i < NV; i++) { for (k = 0; k < NM; k++) s->v[i].m[k].amp = 0; s->v[i].alive = 0; }
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

static int32_t voice_level(const stone_t *v)
{
    int32_t a = 0;
    unsigned k;
    for (k = 0; k < v->nm; k++) a += v->m[k].amp >> 4;
    return a;
}

/* the modes of a stone at this key: ratios (Q10) and gains (Q15) by shape and strike position */
static unsigned modes(drum_t *s, unsigned note, uint32_t *ratio, int32_t *gain)
{
    unsigned sh = shape_of(s), k, n;
    int32_t p = s->p[P_POS] * 129, vy = s->p[P_VARY];        /* 0 .. 0.5, Q15 */
    tx_rng_t r;
    /* VARY 0: each key one fixed stone (the shape drawn from the note); above: every strike a new stone from the
     * running random draws, and the strike point moves (+-0.08 of the length at 127) */
    tx_rng_seed(&r, vy ? tx_rand(&s->cloud.rng) | 1u : note * 2654435761u ^ 0xB01DE5u);
    if (vy) { p += (tx_rand_s15(&s->cloud.rng) * vy) >> 12; p = p < 0 ? -p : p > 16384 ? 32768 - p : p; }
    if (sh == 0) {                                           /* free-free bar: shape ~ sqrt 2 cos((n + 1.5) pi p - pi/4) */
        for (k = 0; k < 5; k++) {
            uint32_t ph = (uint32_t)((int64_t)((2 * (int32_t)k + 3) * p) << 15) - 0x20000000u;
            int32_t c = tx_sin_q15(ph + 0x40000000u);
            ratio[k] = BAR_R[k];
            gain[k] = (c < 0 ? -c : c) * 46341 >> 15;
        }
        return 5;
    }
    n = NM;
    for (k = 0; k < n; k++) {
        int32_t c = tx_sin_q15((k + 1u) * (uint32_t)p * 111411u);   /* a position pattern: sin(1.7 pi (k + 1) p), ours */
        int32_t g;
        if (sh == 1) ratio[k] = SLAB_R[k];
        else ratio[k] = k ? (uint32_t)(((uint64_t)ratio[k - 1] * tx_exp2(7209 + (int32_t)tx_rand_below(&r, 37355u) + (16 << 16))) >> 16) : 1024u;
        g = k < 2 ? 32767 - (int32_t)k * 3000 : (int32_t)tx_exp2((15 << 16) - (int32_t)tx_rand_below(&r, 152000u));   /* 0 .. -14 dB */
        gain[k] = (int32_t)(((int64_t)g * (16384 + ((c < 0 ? -c : c) >> 1))) >> 15);
    }
    return n;
}

/* a strike on stone v: each mode gets a (Q30) x its gain x the force spectrum at its frequency (v->gw) */
static void strike(stone_t *v, int32_t a)
{
    unsigned k;
    for (k = 0; k < v->nm; k++) if (v->f[k] && v->gw[k]) tx_mode_strike(&v->m[k], (int32_t)(((int64_t)a * v->gw[k]) >> 15));
}

static void tune(drum_t *s, stone_t *v, const uint32_t *ratio, unsigned n, unsigned note, uint32_t qx16)
{
    int32_t vy = s->p[P_VARY];
    uint32_t f1 = tx_exp2(s->f1_l2 + ((int32_t)note - 60) * 5461
                          + (vy ? (tx_rand_s15(&s->cloud.rng) * vy) / 160 : 0));   /* a new stone: its size +-0.4 oct
                                                              * at VARY 127 (a field of rocks: the references' strikes spread
                                                              * 0.4-1.9 octaves in centroid; note 18) */
    unsigned k;
    v->nm = (uint8_t)n;
    if (vy) qx16 = (uint32_t)(((uint64_t)qx16 * (uint32_t)(32768 + ((tx_rand_s15(&s->cloud.rng) * vy * 77) >> 15))) >> 15);   /* Q +-30 % */
    for (k = 0; k < n; k++) {
        uint32_t f = (f1 * ratio[k]) >> 10;
        if (vy) f = (uint32_t)(((uint64_t)f * (uint32_t)(65536 + ((tx_rand_s15(&s->cloud.rng) * vy * 31) >> 15))) >> 16);   /* +-3 % */
        v->f[k] = (uint16_t)(f > 20000u ? 0u : f);
        v->inc0[k] = f * 97391u;
        v->m[k].inc = v->inc0[k];
        v->m[k].m16 = tx_mode_decay(f, qx16, s->s0_q8);
    }
}

static void hit(drum_t *s, const tx_event_t *e, unsigned *which)
{
    stone_t *v = 0;
    unsigned i;
    uint32_t ratio[NM], tc;
    int32_t gain[NM], lv, ev, a, x;
    int stolen = 0;
    if (!s->p[P_VARY])                                       /* the same stone: a restrike adds to its ring */
        for (i = 0; i < s->limit && !v; i++) if (s->v[i].alive && s->v[i].note == e->note) v = &s->v[i];
    for (i = 0; i < s->limit && !v; i++) if (!s->v[i].alive) v = &s->v[i];
    if (!v) {                                                /* steal the quietest: its ring goes on in the new stone's
                                                              * modes, phase-continuous (the strike adds to it), never cut:
                                                              * a ringing rock's quietest, at the top, is only ~9 dB under
                                                              * the new strike */
        int32_t lo = 0x7FFFFFFF;
        for (i = 0; i < s->limit; i++) { int32_t l = voice_level(&s->v[i]); if (l < lo) { lo = l; v = &s->v[i]; } }
        s->dropped++;
        stolen = 1;
    }
    if (!v) return;
    *which = (unsigned)(v - s->v);
    lv = 65536 + ((int32_t)e->vel - 100) * 1638;            /* 2 m/s at v100, +-1 octave per 40 */
    lv += (tx_rand_s15(&s->cloud.rng) * s->p[P_VARY]) >> 6;  /* no two hits alike: +-0.8 oct of speed at VARY 100
                                                              * (references: strike levels spread 6-13 dB; note 18) */
    tx_contact_hit(&s->mallet, lv, &tc, &ev);
    tc = tx_vary_scale(tc, -(tx_vary_tone(&s->vy) >> 1)      /* the press's tone: a harder / softer blow; */
                       + ((tx_rand_s15(&s->cloud.rng) * s->p[P_VARY]) >> 7));   /* each its own spot: +-0.4 oct of contact */
    if (tc < 1u) tc = 1u;
    a = (int32_t)(((int64_t)A_REF * (int32_t)tx_exp2(lv + (15 << 16))) >> 16);   /* ~ v: 2 m/s -> A_REF */
    a = (int32_t)(((int64_t)a * (32768 + ev)) >> 16);         /* x (1 + e) / 2 */
    a = (int32_t)(((int64_t)a * s->level) >> 15);
    {   /* M9: hard hits lose more and ring flat for a moment */
        int32_t x2 = (int32_t)tx_exp2(2 * (lv - 103872) + (15 << 16));   /* (v / 3 m/s)^2, Q15 */
        x = (int32_t)tx_exp2(lv - 103872 + (15 << 16));
        v->q_eff = (uint16_t)((uint32_t)s->q_x16 * 32768u / (uint32_t)(32768 + (x2 > 1 << 20 ? 1 << 20 : x2)));
        v->flat = (x > 32768 ? 32768 : x) * 1638 >> 15;    /* 5 % x min(1, v / v_ref) */
    }
    {
        unsigned n = modes(s, e->note, ratio, gain);
        if (stolen) { unsigned k; for (k = n; k < NM; k++) v->m[k].amp = 0; }
        else if (!v->alive || v->note != e->note) { unsigned k; for (k = 0; k < NM; k++) v->m[k].amp = 0; }
        tune(s, v, ratio, n, e->note, v->q_eff);
        v->young = 1;
        {   /* each mode's weight; one under 1/128 of the strongest (-42 dB) is not run (CPU, note 18) */
            int32_t mx = 0;
            for (i = 0; i < n; i++) { v->gw[i] = (int16_t)(((int64_t)gain[i] * tx_contact_weight(tc, v->f[i])) >> 15); if (v->gw[i] > mx) mx = v->gw[i]; }
            for (i = 0; i < n; i++) if (v->gw[i] < (mx >> 7) && !v->m[i].amp) v->gw[i] = 0;
        }
        strike(v, a);
        /* the attack residue: the many short-lived high modes a real stone has beyond the NM kept (and the contact's
         * own noise), as a ~3 ms low-passed noise burst weighted by the force spectrum at 4 kHz (ours; by the reference: the
         * first 10 ms of a hammer on a ringing rock is ~2.4 kHz centroid, > 4 kHz at -15 dB) */
        s->res += (int32_t)(((int64_t)(int32_t)tx_vary_scale((uint32_t)(a >> 1), tx_vary_tone(&s->vy) >> 1)
                             * tx_contact_weight(tc, 4000u)) >> 15);   /* (brighter: more of it) */
    }
    v->alive = 1; v->note = e->note; v->a0 = a;
    {   /* micro-collisions: hard mallets bounce (stone / wood), 0-2 within 15 ms */
        uint32_t nmc = s->p[P_MALLET] < 64u ? tx_rand_below(&s->cloud.rng, 3u) : 0u, k;
        v->moff[0] = v->moff[1] = 0xFFFF;
        for (k = 0; k < nmc; k++) {
            v->moff[k] = (uint16_t)(e->offset + 44u + tx_rand_below(&s->cloud.rng, 600u) * (k + 1u));
            v->mamp[k] = 3277 + (int32_t)tx_rand_below(&s->cloud.rng, 7000u);   /* -20 .. -10 dB */
        }
    }
}

static void render_voice(drum_t *s, stone_t *v, unsigned n)
{
    unsigned j = 0, k;
    for (k = 0; k < 2; k++) {
        if (v->moff[k] < n) {                                /* a bounce: the same contact, smaller */
            tx_modal_run(v->m, v->nm, s->buf, j, v->moff[k]);
            j = v->moff[k];
            strike(v, (int32_t)(((int64_t)v->a0 * v->mamp[k]) >> 15));
            v->moff[k] = 0xFFFF;
        } else if (v->moff[k] != 0xFFFF)
            v->moff[k] = (uint16_t)(v->moff[k] - n);
    }
    tx_modal_run(v->m, v->nm, s->buf, j, n);
}

TX_AUDIO_CODE static void drum_render(void *st, int32_t *mix, unsigned n)
{
    drum_t *s = (drum_t *)st;
    unsigned k, i, j;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;
    for (j = 0; j < n; j++) s->buf[j] = 0;
    {
        unsigned e = 0;
        for (i = 0; i < NV; i++) {
            stone_t *v = &s->v[i];
            unsigned m;
            if (v->alive) {                                  /* the M9 moment: Q back, pitch back */
                if (v->young && ++v->young == 10) for (m = 0; m < v->nm; m++) v->m[m].m16 = tx_mode_decay(v->f[m], s->q_x16, s->s0_q8);
                v->flat -= v->flat >> 4;
                for (m = 0; m < v->nm; m++) v->m[m].inc = v->inc0[m] - (uint32_t)(((uint64_t)v->inc0[m] * (uint32_t)v->flat) >> 15);
            }
        }
        for (e = 0; e < k; e++) {                            /* key strikes land at the block start (keys are */
            unsigned w = NV;                                 /* block-rate events); micro-collisions at their sample */
            hit(s, &s->ev[e], &w);
            (void)w;
        }
    }
    for (i = 0; i < NV; i++) {
        stone_t *v = &s->v[i];
        unsigned m, live = 0;
        if (!v->alive) continue;
        render_voice(s, v, n);
        for (m = 0; m < v->nm; m++) {
            if (v->m[m].amp < (v->a0 >> CULL_SH)) v->m[m].amp = 0;
            live |= v->m[m].amp != 0;
        }
        if (!live && v->moff[0] == 0xFFFF && v->moff[1] == 0xFFFF) v->alive = 0;
    }
    if (s->res) {                                            /* residue: noise x e^(-t / 3 ms) */
        int32_t e = s->res, x1 = s->res_x1;
        for (j = 0; j < n; j++) {
            int32_t x = (int32_t)(tx_rand(&s->cloud.rng) >> 17) - 16384;   /* +-2^14 */
            x1 += (x - x1) >> 1;                             /* a gentle low-pass (~7 kHz) */
            s->buf[j] += (x1 * (e >> 14)) >> 3;
            e -= (e >> 7) + 1;                               /* tau 128 samples = 2.9 ms */
            if (e <= 0) { e = 0; break; }
        }
        s->res = e; s->res_x1 = e ? x1 : 0;
    }
    tx_vary_block(&s->vy, s->buf, n);
    tx_trim(s->buf, n, OUT_TRIM);
    tx_headroom(s->buf, n);                                  /* (a dozen strikes on one sample) */
    tx_dcblock_add(&s->hp, s->buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = s->hp.x1 | s->hp.y1 | s->res;
        for (i = 0; i < NV; i++) z |= s->v[i].alive;
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void drum_stats(const void *st, tx_stats_t *o)
{
    const drum_t *s = (const drum_t *)st;
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

/* the worst case: twelve quartz boulders ringing (every mode alive), struck again and again, hard */
static const char SCENE_MAX[] =
    TX_TEXT("0 particles 12\n0 param MATERIAL 127\n0 param SHAPE 127\n0 param TUNE 127\n"
    "0 on 53 127\n0 on 55 127\n0 on 57 127\n0 on 59 127\n0 on 60 127\n0 on 62 127\n0 on 64 127\n0 on 65 127\n"
    "0 on 67 127\n0 on 69 127\n0 on 71 127\n0 on 72 127\n"
    "100 off 53\n100 off 55\n100 off 57\n100 off 59\n100 off 60\n100 off 62\n100 off 64\n100 off 65\n"
    "100 off 67\n100 off 69\n100 off 71\n100 off 72\n"
    "500 on 53 127\n500 on 57 127\n500 on 60 127\n500 on 64 127\n500 on 67 127\n500 on 71 127\n"
    "600 off 53\n600 off 57\n600 off 60\n600 off 64\n600 off 67\n600 off 71\n"
    "3000 end\n");

const tx_engine_t tx_eng_stonedrum = {
    "stonedrum", TX_TEXT("#10 Stone drum / lithophone"), NP, PARAMS, NV, 6, sizeof(drum_t), SCENE_MAX,
    drum_init, drum_key, drum_param, drum_render, drum_stats,
};
