/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #32 Ice crashing: icicles falling and shattering, or ice cubes in a glass (MODAL,
 * CONTACT, INSECT resonators, CLOUD).
 *
 * ACTION picks the scene:
 *   icicles  a key drops an icicle (its length by the key): it hits the frozen ground, rings for a moment (a free-free
 *            ice rod: f1 = 3.56 (d / 4) c / L^2, c = sqrt(E / rho) = 3140 m/s (E 9.04 GPa, rho 917: C3), d = L / 15
 *            (ours) -> f1 ~ 0.059 c / L: a 30 cm icicle ~620 Hz, note 09's example), damped by the ground it lies on
 *            (sigma0 ~60 /s, ours), cracks into chunks (cracks run at up to 1320 m/s, C7: each a 1-7 sample click; a
 *            few in ~20 ms, ours) that bounce with little restitution (ice on ice / snow: 0.1-0.3, ours) and thud
 *            on the ground (icicle recordings: thumps at 60-300 Hz, -30 dB in 6-65 ms, high crackle above).
 *   cubes    ice cubes in a glass: cubes ring ultrasonically (a 3 cm cube ~100 kHz: note 09), so a clink is the
 *            contact click and the glass: 4 modes 1 : 2.67 : 5.05 : 7.95 (a recording of cubes in a glass: 519 /
 *            1386 / 2620 / 4126 Hz; the ratios ours from it), Q ~250 with the drink (-30 dB after 0.14-0.6 s in it).
 *            A press drops cubes in (a burst), holding shakes the glass (Poisson clinks, AMOUNT x velocity).
 *   both     TEMP sets the ice's Q (C10, natural ice: ~290-650 near -20 C, 40-100 above about -5 C) and how wet it is;
 *            FIZZ: melting ice releases bubbles, a fizz of many small events at 1-3 kHz (C8 / C11): sparse
 *            impulses into a broad 2 kHz resonance, louder near 0 C.
 * Keys: the icicle length / the glass size, a semitone per key. Sheet break-up (chirps) is #18 Lake ice.
 * Realism (icicle and cubes-in-glass references, analysis only): breaking icicles are a bright shatter (bands 5-13 kHz
 * on top, 0.5-0.8 kHz 15-25 dB under, little under 120 Hz, kurtosis 900-2200), so the rod rings -20 dB under the
 * crack, the ground thud is -20 dB, the cracks' and chunks' clicks +12 dB and brighter (a ~5 kHz high-pass); the
 * cube's clink adds a few-sample spike (ice on glass is hard and light) and its burst is brighter too (ours). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_modal.h"
#include "tx_core_contact.h"
#include "tx_core_insect.h"

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM -1.5 dB with it, held there by the chord's limiter under ~8 dB (its mean
 * loudness below the old one) */
#define VARY_LO -130
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 1

enum { P_ACTION, P_TEMP, P_AMOUNT, P_SIZE, P_FIZZ, NP };

static const tx_param_t PARAMS[NP] = {
    {"ACTION", 127, 0, "icicles falling and shattering | ice cubes in a glass | ice crystals tinkling (frost falling from "
                        "twigs) | snow crunching underfoot (32 each)"},
    {"TEMP", 127, 64, "ice temperature -40 .. 0 C: ringing (Q ~500) .. wet and dull (Q ~50), more fizz"},
    {"AMOUNT", 127, 64, "icicles: chunks per break 3 .. 24 | cubes: shaking 1 .. 15 clinks/s | crystals: 1 .. 15 /s | "
                        "snow: steps 0.5 .. 2.5 /s while held"},
    {"SIZE", 127, 64, "icicle length 10 .. 80 cm | glass: lowest mode 300 .. 1200 Hz (keys a semitone)"},
    {"FIZZ", 127, 20, "melt fizz: off .. strong (bubbles at 1-3 kHz)"},
};

#define NV 8u                      /* ringing bodies (icicles or glasses) */
#define NMB 4u
#define NF 48u                     /* chunks (48: at the top 3-5 breaks/s, each scattering for up to 0.8 s) */
#define NPC 6u                     /* chunk rings at once */
#define OUT_TRIM 21700             /* output gain, Q12 (+15.9 dB; LOUDNESS.md: a v100 icicle toward -20 LUFS max
                                    * momentary, held at -33.5 by its crack's crest: the default chord <= +12 dBFS and
                                    * the output limiter under ~8 dB) */
#define A_REF 100000000            /* a mode's amplitude for a v100 impact (Q30; calibrated by render) */
#define NSTEP 4u                   /* footsteps crunching at once */
enum { K_ICICLES, K_CUBES, K_CRYSTALS, K_SNOW };
/* each kind's level re the icicles', Q8 (by render: on the icicles' loudness) */
static const uint16_t K_OUT[4] = {256, 256, 256, 192};   /* (snow 91 -> 192: its crunch in segments, the same loudness) */

static const uint16_t ROD_R[NMB] = {1024, 2823, 5534, 9147};      /* free-free, Q10 */
static const uint16_t GLS_R[NMB] = {1024, 2734, 5171, 8141};      /* the glass recording's ratios, Q10 */
static const uint16_t Q_T[9] = {400, 450, 500, 480, 420, 300, 150, 90, 50};   /* -40 .. 0 C by 5 C (C10 natural) */

typedef struct {
    tx_mode_t m[NMB];
    uint8_t used, note, pad[2];
} body_t;

typedef struct {
    uint32_t t;                    /* samples to its next impact */
    int32_t v;                     /* impact speed, mm/s */
    int32_t a;                     /* amplitude scale */
    uint32_t inc;                  /* its own piece's ring (a shorter rod: higher), 0 = none */
    uint8_t alive, k, pad[2];
} chunk_t;

typedef struct {                   /* a step into snow: the bonds between the grains break as the foot sinks (ours) */
    uint32_t t, len;               /* samples into it, its length */
    uint32_t next;                 /* samples to its next grain bond breaking */
    int32_t a;                     /* its loudness */
    uint32_t pk;                   /* the foot's peak (heel down), samples */
    uint32_t segend;               /* the crunch's present segment ends here (samples into the step) */
    int32_t sg;                    /* and its gain, Q15 */
    uint8_t alive, pad[3];
} step_t;

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    body_t b[NV];
    chunk_t ch[NF];
    tx_mode_t thud[2];
    tx_mode_t piece[NPC];          /* the chunks' own short rings (a few at a time) */
    tx_ireso_t fz;
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK], bpush[TX_BLOCK], spk[TX_BLOCK];
    uint16_t fo[64];
    int16_t fa[64];
    int32_t res, res_x1;
    tx_mallet_t mal;
    uint8_t p[NP], limit, quiet;
    int32_t level, size_l2, fe;    /* fe: the fizz's envelope (Q15) */
    uint32_t q_x16, fizz;
    uint16_t dropped, steals;
    tx_vary_t vy;
    int32_t f1_l2;                 /* the last icicle's rod (log2 Hz Q16): its chunks ring above it */
    uint8_t rk, rd, pad2[2];       /* the clicks' colour now: high-pass shift, decay shift (per crack) */
    uint16_t fzt;                  /* blocks of fizz left after a break */
    uint32_t fnext;                /* samples to the next fizz pop */
    step_t st[NSTEP];
    uint8_t lp, pad3[3];           /* lp: the clicks low-passed (snow: the grains' crunch is dull) */
    int32_t res_x2;                /* (its second pole) */
} ice_t;

static unsigned kind(const ice_t *s) { return s->p[P_ACTION] >> 5; }
static unsigned cubes(const ice_t *s) { return kind(s) == K_CUBES; }

static void set_param(ice_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    {
        uint32_t x = (uint32_t)s->p[P_TEMP] * 8u, i = x >> 7, f = x & 127u;   /* -40 .. 0 C over the table */
        uint32_t q = i >= 8u ? Q_T[8] : (Q_T[i] * (128u - f) + Q_T[i + 1] * f) >> 7;
        s->q_x16 = q * 16u;
        s->fizz = (uint32_t)s->p[P_FIZZ] * (64u + (uint32_t)s->p[P_TEMP]) >> 6;   /* warmer: more fizz (ours) */
    }
    if (cubes(s) || kind(s) == K_CRYSTALS) tx_cloud_rate(&s->cloud, 256u + (uint32_t)s->p[P_AMOUNT] * 28u);   /* 1 .. 15 /s */
    else if (kind(s) == K_SNOW) tx_cloud_rate(&s->cloud, 128u + (uint32_t)s->p[P_AMOUNT] * 4u);   /* 0.5 .. 2.5 steps /s */
    else tx_cloud_rate(&s->cloud, 0);
    tx_cloud_release(&s->cloud, kind(s) == K_ICICLES ? 0u : 9u);   /* cubes, crystals, snow: a press starts a shake, a
                                                              * shower, a walk that thins out after the key (tau ~6 s); a
                                                              * source's strike (its key up after 150 ms) played one clink
                                                              * or one step, then silence */
    if (id == P_SIZE) s->size_l2 = (int32_t)v * 1714;        /* 0 .. +log2(8) (icicle 10-80 cm) / 0 .. 2 oct (glass) */
}

static void ice_init(void *st, uint32_t seed)
{
    ice_t *s = (ice_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_mallet_mix(&s->mal, 0);
    s->mal.lm -= 217706;
    for (i = 0; i < 2; i++) { s->thud[i].inc = (70u + i * 75u) * 97391u; s->thud[i].m16 = tx_mode_decay(70u + i * 75u, 40u, 0u); }
    s->rk = 1; s->rd = 5;
    tx_ireso_set(&s->fz, 2000, 24);                          /* the fizz: 2 kHz, Q 1.5 (C8: 1-3 kHz) */
    s->fz.g = 256;
    s->limit = 6;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((ice_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void ice_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((ice_t *)st)->vy);
    tx_cloud_key(&((ice_t *)st)->cloud, note, vel, on);
}

static void ice_param(void *st, uint8_t id, uint8_t val)
{
    ice_t *s = (ice_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i, k;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NV ? NV : val);
        for (i = s->limit; i < NV; i++) { for (k = 0; k < NMB; k++) s->b[i].m[k].amp = 0; s->b[i].used = 0; }
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* a body (icicle or glass) for this key, struck with a (Q30); fresh: a new icicle (its own length: +-0.3 oct, and
 * the press's tone), not the one still ringing on this key */
static void body_strike(ice_t *s, unsigned note, int32_t a, uint32_t tc, int fresh)
{
    body_t *b = 0;
    unsigned i, k, cu = cubes(s);
    uint32_t f1, sig = 256u;
    for (i = 0; i < s->limit && !b && !fresh; i++) if (s->b[i].used && s->b[i].note == note) b = &s->b[i];
    for (i = 0; i < s->limit && !b; i++) if (!s->b[i].used) b = &s->b[i];
    if (!b) {
        int32_t lo = 0x7FFFFFFF;
        for (i = 0; i < s->limit; i++) if (s->b[i].m[0].amp < lo) { lo = s->b[i].m[0].amp; b = &s->b[i]; }
        s->steals++;                                         /* (the quietest ring taken over: a steal, not a drop) */
    }
    if (!b) return;
    if (!b->used || b->note != note || fresh) {
        int32_t own = fresh ? ((tx_rand_s15(&s->cloud.rng) * 19661) >> 15) + (tx_vary_tone(&s->vy) >> 1) : 0;
        if (cu) f1 = tx_exp2(539293 + s->size_l2 * 2 / 3 + ((int32_t)note - 60) * 5461);   /* 300 Hz x 2^(0..2) */
        else {
            s->f1_l2 = tx_log2_q16(3140u) - 267150 + 217706 - s->size_l2 + ((int32_t)note - 60) * 5461 + own;   /* 0.059 c / L */
            f1 = tx_exp2(s->f1_l2);
        }
        sig = cu ? 256u : (40u + tx_rand_below(&s->cloud.rng, 210u)) * 256u;   /* how it lies: on ice .. in snow (40 .. 250 /s, ours) */
        for (k = 0; k < NMB; k++) {
            uint32_t f = (f1 * (cu ? GLS_R[k] : ROD_R[k])) >> 10;
            b->m[k].amp = 0; b->m[k].ph = 0;
            b->m[k].inc = f < 20000u ? f * 97391u : 0u;
            /* the glass rings with the drink (Q ~250, ours from the recording); the icicle with the ice's Q, damped by
             * the ground it lies on (sigma0 60 /s, ours) */
            b->m[k].m16 = f < 20000u ? (cu ? tx_mode_decay(f, 4000u, sig) : tx_mode_decay(f, s->q_x16, sig)) : 0u;
        }
        b->note = (uint8_t)note;
    }
    for (k = 0; k < NMB; k++)
        if (b->m[k].inc) tx_mode_strike(&b->m[k], (int32_t)(((int64_t)(k ? a - (a >> 2) : a) * tx_contact_weight(tc, b->m[k].inc / 97391u)) >> 15) * ((k & 1u) ? -1 : 1));
    b->used = 1;
}

/* a draw in octaves: sd x (the sum of three uniforms: sd 1, within +-3), log2 Q16; up at most 1.5 sd (headroom) */
static int32_t oct(ice_t *s, int32_t sd_q16)
{
    int32_t g = (int32_t)(((int64_t)tx_rand_s15(&s->cloud.rng) + tx_rand_s15(&s->cloud.rng) + tx_rand_s15(&s->cloud.rng)) * sd_q16 >> 15);
    return g > sd_q16 + (sd_q16 >> 1) ? sd_q16 + (sd_q16 >> 1) : g;
}

/* the crack's own colour: how sharp (the burst's high-pass) and how short (no two cracks alike: note 17) */
static void crack_colour(ice_t *s)
{
    s->rk = (uint8_t)(1u + tx_rand_below(&s->cloud.rng, 2u));   /* corner ~9 / 5 kHz */
    s->rd = (uint8_t)(4u + tx_rand_below(&s->cloud.rng, 2u));   /* tau ~0.4 / 0.7 ms */
}

static void icicle(ice_t *s, const tx_event_t *e)
{
    tx_rng_t *r = &s->cloud.rng;
    int32_t lv = 65536 + ((int32_t)e->vel - 100) * 1638, er, a, nl2;
    uint32_t tc, n, j, i, c, fly;
    tx_contact_hit(&s->mal, lv, &tc, &er);
    tc = tx_vary_scale(tc, -(tx_vary_tone(&s->vy) >> 1)); if (tc < 1u) tc = 1u;   /* the press's tone: harder / softer */
    a = (int32_t)(((int64_t)A_REF * (int32_t)tx_exp2(lv - 65536 + (15 << 16))) >> 15);
    a = (int32_t)(((int64_t)a * s->level) >> 15);
    {   /* its own weight: sd 0.7 octave, the mean power kept, at most +0.5 octave (the peaks' headroom) */
        int32_t w = oct(s, 45000) - 21600;
        a = (int32_t)(((int64_t)a * (int32_t)tx_exp2((w > 32768 ? 32768 : w) + (15 << 16))) >> 15);
    }
    /* no two icicles alike (note 17: the references' per-break level spread 10-12 dB, ours 4 at the top): how loud its
     * rod rings under the crack (-20 dB, sd 6 dB), its thud (sd 1 oct), how many chunks (x/ 1.6), how far they fly */
    body_strike(s, e->note, (int32_t)(((int64_t)a * (int32_t)tx_exp2(oct(s, 65536) + (15 << 16) - 217706 - 45426)) >> 15), tc, 1);
    {
        int32_t th = (int32_t)tx_exp2(oct(s, 65536) + (15 << 16) - 45426);   /* Q15 (the mean power kept) */
        tx_mode_strike(&s->thud[0], (int32_t)(((int64_t)(a / 30) * th) >> 15));   /* the ground (-20 dB: 30-120 Hz) */
        tx_mode_strike(&s->thud[1], (int32_t)(((int64_t)(-a / 60) * th) >> 15));
    }
    crack_colour(s);
    s->fzt = 172u;                                           /* ~1 s of fizz from the fresh break */
    s->bpush[e->offset] += a >> 6;                           /* the first crack; 1-4 more within ~20 ms follow as */
    for (c = 0, j = e->offset; c < 1u + (tx_rand(r) >> 30); c++) {   /* one-impact chunks (k = 4: no bounce) */
        chunk_t *h = 0;
        j += 40u + tx_rand_below(r, 300u);
        for (i = 0; i < NF && !h; i++) if (!s->ch[i].alive) h = &s->ch[i];
        if (!h) break;
        h->t = j; h->v = 2048; h->a = a >> (1 + c); h->k = 4; h->inc = 0; h->alive = 1;
    }
    nl2 = oct(s, 45000);
    n = (uint32_t)(((uint64_t)(3u + ((uint32_t)s->p[P_AMOUNT] * 21u >> 7)) * tx_exp2(nl2 + (16 << 16))) >> 16);
    n = n < 2u ? 2u : n > 28u ? 28u : n;
    {   /* a pile shattering fast: as many chunks as the air still has room for (the breaks themselves are never
         * dropped: B's faster presses at the top) */
        uint32_t fr = 0;
        for (i = 0; i < NF; i++) fr += !s->ch[i].alive;
        if (n > fr) n = fr;
    }
    fly = 12000u + tx_rand_below(r, 24000u);                 /* this break's scatter: 0.3 .. 0.8 s (ours) */
    for (j = 0; j < n; j++) {                                /* the chunks fly and bounce */
        chunk_t *h = 0;
        uint32_t u;
        for (i = 0; i < NF && !h; i++) if (!s->ch[i].alive) h = &s->ch[i];
        if (!h) break;
        u = tx_rand(r) >> 16;
        h->t = e->offset + 1300u + (uint32_t)(((uint64_t)u * u * fly) >> 32);   /* 30 ms .. the scatter, most early */
        h->v = 300 + (int32_t)tx_rand_below(r, 1500u);
        h->a = (int32_t)(((int64_t)a * (int32_t)(4000u + tx_rand_below(r, 28000u))) >> 15);
        /* a chunk is a shorter piece of the rod: it rings higher (1.5 .. 6 x: L / l), briefly, at its first impact */
        h->inc = s->f1_l2 ? tx_exp2(s->f1_l2 + 38336 + (int32_t)tx_rand_below(r, 131072u)) : 0u;
        h->inc = h->inc < 18000u ? h->inc * 97391u : 0u;
        h->k = 0; h->alive = 1;
    }
}

/* ice crystals (hoar frost, rime) shaken from twigs onto the crust: a few tiny pieces each, falling a short way; a piece
 * of a few mm rings ultrasonically, so what rings is its splinter's bending (3 .. 10 kHz, log-uniform: ours, the
 * references' tinkle 2.7-6.6 kHz), briefly, with a bright tick (ours) */
static void crystals(ice_t *s, const tx_event_t *e)
{
    tx_rng_t *r = &s->cloud.rng;
    int32_t a = (int32_t)(((int64_t)(A_REF / 6) * (int32_t)tx_exp2(((int32_t)e->vel - 100) * 1638 + (15 << 16))) >> 15);
    uint32_t n = e->kind == TX_EV_MAIN ? 3u + tx_rand_below(r, 6u) : 1u + (tx_rand(r) >> 31), j, i;
    a = (int32_t)(((int64_t)a * s->level) >> 15);
    for (j = 0; j < n; j++) {
        chunk_t *h = 0;
        int32_t x = (int32_t)(((int64_t)tx_neglog2_q12(tx_rand(r)) * 8192) >> 12);   /* sizes alpha 1.5 */
        for (i = 0; i < NF && !h; i++) if (!s->ch[i].alive) h = &s->ch[i];
        if (!h) { s->dropped++; break; }
        h->t = e->offset + (j ? tx_rand_below(r, 9000u) : 0u);   /* a shower over ~0.2 s */
        h->v = 150 + (int32_t)tx_rand_below(r, 500u);         /* a short fall: 1 .. 3 cm */
        h->a = (int32_t)(((int64_t)a * (int32_t)tx_exp2(-(x > 4 << 16 ? 4 << 16 : x) + (15 << 16))) >> 15);
        h->inc = tx_exp2(757000 + (int32_t)tx_rand_below(r, 113800u));   /* 3 .. 10 kHz (log2 Q16) */
        h->inc = h->inc < 18000u ? h->inc * 97391u : 0u;
        h->k = 0; h->alive = 1;
    }
}

/* a step into snow: the foot compacts it, breaking the bonds between grains (a dense crunch of small fractures, its
 * rate rising as the heel goes down and fading as the foot settles: 0.15 .. 0.35 s; the references: a step a second or
 * two, 15-16 crunch segments /s, 125 Hz .. 2 kHz, the squeak of cold snow brighter); the ground under it thuds */
static void step(ice_t *s, const tx_event_t *e)
{
    tx_rng_t *r = &s->cloud.rng;
    step_t *f = 0;
    unsigned i;
    int32_t a = (int32_t)(((int64_t)A_REF * (int32_t)tx_exp2(((int32_t)e->vel - 100) * 1638 + (15 << 16))) >> 15);
    a = (int32_t)(((int64_t)a * s->level) >> 15);
    for (i = 0; i < NSTEP && !f; i++) if (!s->st[i].alive) f = &s->st[i];
    if (!f) { s->dropped++; return; }
    f->len = 6615u + tx_rand_below(r, 8820u);                /* 0.15 .. 0.35 s */
    f->pk = f->len / 5u + tx_rand_below(r, f->len / 5u);     /* the heel down at 20 .. 40 % */
    f->t = 0; f->next = e->offset; f->a = a; f->alive = 1; f->segend = 0; f->sg = 32767;
    tx_mode_strike(&s->thud[0], a / 24);
    tx_mode_strike(&s->thud[1], -a / 48);
}

/* the steps' crunch this block: grain bonds breaking, Poisson at up to ~1500 /s at the heel (ours), each a click of a
 * power-law size into the low-passed burst */
static void crunch(ice_t *s, unsigned n)
{
    tx_rng_t *r = &s->cloud.rng;
    unsigned i;
    for (i = 0; i < NSTEP; i++) {
        step_t *f = &s->st[i];
        if (!f->alive) continue;
        while (f->next < n) {
            uint32_t tt = f->t + f->next, env, gap;
            int32_t x, c;
            if (tt >= f->len) { f->alive = 0; break; }
            env = tt < f->pk ? (tt << 15) / f->pk : (((f->len - tt) << 10) / ((f->len - f->pk) >> 5));   /* Q15 */
            x = (int32_t)(((int64_t)tx_neglog2_q12(tx_rand(r)) * 8192) >> 12);   /* alpha 1.5 */
            c = (int32_t)(((int64_t)(f->a >> 7) * (int32_t)tx_exp2(-(x > 5 << 16 ? 5 << 16 : x) + (15 << 16))) >> 15);
            c = (int32_t)(((int64_t)c * (int32_t)env) >> 15);
            if (tt >= f->segend) {                           /* the snowpack gives way in segments, not grain by grain:
                                                              * each 20 .. 80 ms, its own strength, a third of them
                                                              * near silent (the references: 15-16 crunch segments /s;
                                                              * ours lacked 2-3 dB of their 10-40 Hz modulation) */
                f->segend = tt + 900u + tx_rand_below(r, 2600u);
                f->sg = (tx_rand(r) >> 16) < 21000u ? 1000 : 8192 + (int32_t)tx_rand_below(r, 24576u);
            }
            c = (int32_t)(((int64_t)c * f->sg) >> 15);
            if ((tx_rand(r) >> 16) < (tt << 16) / f->len) {   /* a crisp bond: a bare 2-sample click, more of them as
                                                              * the foot rolls (the heel compacts, dull; the bonds snap
                                                              * later; ours). The references (snow steps / walk) have
                                                              * more top: 4-8 kHz was 2-3 dB short without */
                int32_t k = (tx_rand(r) & 1u) ? (c >> 2) : -(c >> 2);
                s->spk[f->next] += k;
                if (f->next + 1u < n) s->spk[f->next + 1u] -= k;
            } else s->bpush[f->next] += c;
            gap = 8u + (uint32_t)(((uint64_t)tx_neglog2_q12(tx_rand(r) | 1u) * ((29u << 15) / (env + 1024u)) * 2839u) >> 24);
            f->next += gap;
        }
        if (f->alive) { f->next -= n; f->t += n; }
    }
}

TX_AUDIO_CODE static void ice_render(void *st, int32_t *mix, unsigned n)
{
    ice_t *s = (ice_t *)st;
    tx_rng_t *r = &s->cloud.rng;
    unsigned k, i, j, nf = 0;
    uint16_t d0 = s->dropped;
    int32_t any = 0;
    if (n > TX_BLOCK) n = TX_BLOCK;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;
    for (j = 0; j < n; j++) s->buf[j] = s->bpush[j] = s->spk[j] = 0;
    for (i = 0; i < k; i++) {
        const tx_event_t *e = &s->ev[i];
        if (kind(s) == K_CRYSTALS) { crystals(s, e); continue; }
        if (kind(s) == K_SNOW) { step(s, e); continue; }
        if (!cubes(s)) { if (e->kind == TX_EV_MAIN) icicle(s, e); continue; }
        {   /* a clink: a cube hits the glass (sizes from a power law, alpha 1.5 over 1.5 decades: ours) */
            int32_t x = (int32_t)(((int64_t)tx_neglog2_q12(tx_rand(r)) * 8192) >> 12), lv, er, a;
            uint32_t tc, nb = e->kind == TX_EV_MAIN ? 3u + (tx_rand(r) >> 30) : 1u, c;
            if (x > 20400) x = 20400;
            lv = 65536 + ((int32_t)e->vel - 100) * 1638 - x * 8;
            tx_contact_hit(&s->mal, lv, &tc, &er);
            tc = tx_vary_scale(tc, -(tx_vary_tone(&s->vy) >> 1)); if (tc < 1u) tc = 1u;   /* the press's tone: harder / softer */
            a = (int32_t)(((int64_t)A_REF / 5 * (int32_t)tx_exp2(lv - 65536 + (15 << 16))) >> 15);
            a = (int32_t)(((int64_t)a * s->level) >> 15);
            crack_colour(s);
            for (c = 0; c < nb; c++) {                       /* dropped in: a few clinks at once (ours) */
                body_strike(s, e->note, c ? a >> c : a, tc, 0);
                s->bpush[e->offset] += a >> 5;
            }
            {   /* the cube meets the glass: ice on glass is hard and light, a contact of a few us, so the clink's own
                 * click is a pulse of a few samples, bright (a cubes-in-glass recording keeps 5-8 kHz within
                 * 6-8 dB of its peak: ours) */
                static const int16_t SH[5] = {256, 180, -154, -154, -128};   /* a zero-mean ~5-7 kHz pulse (ours) */
                int32_t sp = a >> 2;
                unsigned q;
                for (q = 0; q < 5u && e->offset + q < n; q++) s->spk[e->offset + q] += (sp >> 8) * SH[q];
            }
        }
    }
    {
        unsigned sn = kind(s) == K_SNOW;
        int any_st = 0;
        for (i = 0; i < NSTEP; i++) any_st |= s->st[i].alive;
        if (any_st) { crunch(s, n); any = 1; }
        if (sn) { s->rk = (uint8_t)(4u - (s->p[P_TEMP] < 64u)); s->rd = 6; }   /* cold snow squeaks brighter (ours); a
                                                              * grain's crunch ~1.5 ms */
        s->lp = (uint8_t)sn;
    }
    for (i = 0; i < NF; i++) {                               /* chunk impacts: a click and a small thud each */
        chunk_t *h = &s->ch[i];
        if (!h->alive) continue;
        any = 1;
        while (h->alive && h->t < n) {
            int32_t a = (int32_t)(((int64_t)h->a * h->v) >> 11);
            unsigned cry = kind(s) == K_CRYSTALS;
            if (cry) a <<= 2;                                /* (a crystal: its ring over a light tick) */
            s->bpush[h->t] += cry ? a >> 12 : a >> 8;        /* the chunk's click, +12 dB (references: shatter) */
            if (!h->k) crack_colour(s);                      /* (its own colour as it lands) */
            if (!h->k && h->inc) {                           /* its piece rings, damped by the ground (~10-30 ms; the */
                tx_mode_t *m = &s->piece[0];                 /* quietest ring; it lands at the block start) */
                unsigned q, cr = kind(s) == K_CRYSTALS;      /* (a crystal on the crust: lightly, ~30-100 ms) */
                for (q = 1; q < NPC; q++) if (s->piece[q].amp < m->amp) m = &s->piece[q];
                m->inc = h->inc;
                m->m16 = tx_mode_decay(h->inc / 97391u, s->q_x16, (cr ? 15u + tx_rand_below(&s->cloud.rng, 40u) : 60u + tx_rand_below(&s->cloud.rng, 200u)) << 8);
                m->amp = 0;
                tx_mode_strike(m, cr ? a : a >> 3);
            }
            if (kind(s) != K_CRYSTALS) tx_mode_strike(&s->thud[h->k & 1u], a >> 9);
            h->k++;
            h->v = (h->v * 6554) >> 15;                      /* restitution 0.2 (ice on frozen ground: ours) */
            if (h->v < 60 || h->k > 4) { h->alive = 0; break; }
            h->t += ((uint32_t)h->v * 9207u) >> 10;          /* flight 2 v / g */
        }
        if (h->alive) h->t -= n;
    }
    for (i = 0; i < NV; i++) {
        body_t *b = &s->b[i];
        unsigned m, live = 0;
        if (!b->used) continue;
        tx_modal_run(b->m, NMB, s->buf, 0, n);
        for (m = 0; m < NMB; m++) live |= b->m[m].amp != 0;
        b->used = (uint8_t)(live != 0);
        any |= b->used;
    }
    tx_modal_run(s->thud, 2, s->buf, 0, n);
    any |= s->thud[0].amp | s->thud[1].amp;
    tx_modal_run(s->piece, NPC, s->buf, 0, n);
    for (i = 0; i < NPC; i++) any |= s->piece[i].amp;
    /* the melt fizz: fresh breaks and wet chunks release their air for ~1 s, then the ice is quiet (it ran under every
     * ring's tail before: a -90 dB bed between the breaks; the user, 2026-10-25: silence between events) */
    if (s->fzt) s->fzt--;
    if (kind(s) >= K_CRYSTALS) s->fzt = 0;                   /* (no melt fizz: frost and snow) */
    s->fe += (((cubes(s) ? tx_cloud_held(&s->cloud) != 0 : s->fzt != 0) ? 32767 : 0) - s->fe) >> 5;   /* dies away over
                                                              * ~0.5 s (session 5: no dead stop) */
    if (s->fe < 32) s->fe = 0;
    if (s->fizz && s->fe) {                                  /* separate pops (bubbles bursting at the melting surface:
                                                              * 30 .. 300 /s by FIZZ, sizes alpha 1.5, each bubble its own
                                                              * pitch 1-3 kHz (C8); was ~1300 /s: a hiss) */
        uint32_t t = s->fnext;
        int32_t lvl = (int32_t)(((((int64_t)s->fizz * s->level) >> 12) * s->fe) >> 15);   /* (+6 dB a pop: fewer) */
        uint32_t gap = 44100u / (30u + s->fizz * 2u);
        if (t < n) tx_ireso_set(&s->fz, 1000u + tx_rand_below(r, 2000u), 24);   /* this block's bubbles */
        while (t < n && nf < 64u) {
            int32_t x = (int32_t)(((int64_t)tx_neglog2_q12(tx_rand(r)) * 8192) >> 12);   /* alpha 1.5 */
            s->fo[nf] = (uint16_t)t;
            s->fa[nf] = (int16_t)(((int64_t)lvl * (int32_t)tx_exp2(-(x > 6 << 16 ? 6 << 16 : x) + (15 << 16))) >> 15) * ((tx_rand(r) & 1u) ? 1 : -1);
            nf++;
            t += 1u + (uint32_t)(((uint64_t)tx_neglog2_q12(tx_rand(r) | 1u) * gap * 2839u) >> 24);   /* exponential gaps */
        }
        s->fnext = t - n;
    } else s->fnext = tx_rand_below(r, 2000u);
    tx_ireso_add(&s->fz, s->fo, s->fa, nf, s->buf, n);
    any |= !tx_ireso_quiet(&s->fz);
    {   /* the clicks: a sharp spike and a short bright burst */
        int32_t e = s->res, x1 = s->res_x1;
        for (j = 0; j < n; j++) {
            int32_t x;
            e += s->bpush[j];
            if (!e) continue;
            x = (int32_t)(tx_rand(r) >> 17) - 16384;
            x1 += (x - x1) >> s->rk;                         /* minus a low-pass (each crack its own: 5 .. 9 kHz; the */
                                                             /* references 5-8 kHz) */
            if (s->lp) {                                     /* (snow: two poles of the low-pass, ~0.4 .. 0.9 kHz) */
                s->res_x2 += (x1 - s->res_x2) >> s->rk;
                s->buf[j] += (s->res_x2 * (e >> 12)) >> 0;
            }
            else s->buf[j] += ((x - x1) * (e >> 12)) >> (s->rk + 1);   /* (a duller crack is softer) */
            e -= (e >> s->rd) + 1;
            if (e < 0) e = 0;
        }
        s->res = e; s->res_x1 = e ? x1 : 0; if (!e) s->res_x2 = 0;
        any |= e;
    }
    for (j = 0; j < n; j++) {                                /* + the clinks' spikes; at most full scale before the
                                                              * trim (only the max scene gets there: <= +16 dBFS after) */
        int32_t v = s->buf[j] + s->spk[j];
        s->buf[j] = v > TX_Q23 ? TX_Q23 : v < -TX_Q23 ? -TX_Q23 : v;
    }
    tx_vary_block(&s->vy, s->buf, n);
    tx_trim(s->buf, n, (OUT_TRIM * K_OUT[kind(s)]) >> 8);    /* the loudness target */
    tx_dcblock_add(&s->hp, s->buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !any && !s->fe && !(s->hp.x1 | s->hp.y1));
}

static void ice_stats(const void *st, tx_stats_t *o)
{
    const ice_t *s = (const ice_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < NV; i++) n += s->b[i].used;
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = s->steals;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: eight glasses shaken hard with cubes, cold (long rings), loud fizz */
static const char SCENE_MAX[] =
    "0 particles 8\n0 param ACTION 48\n0 param TEMP 20\n0 param AMOUNT 127\n0 param FIZZ 127\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4500 end\n";

const tx_engine_t tx_eng_ice = {
    "ice", "#32 Ice crashing", NP, PARAMS, NV, 6, sizeof(ice_t), SCENE_MAX,
    ice_init, ice_key, ice_param, ice_render, ice_stats,
};
