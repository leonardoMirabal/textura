/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #8 Wood fire: roar, crackle and pops (CLOUD with Omori, NOISE roar, INSECT resonators).
 *
 * A key throws a log on the fire: a burst of crackle with an Omori tail, the roar swelling as it catches (~1.5 s);
 * holding keeps it burning (crackles at the MOIST rate); release lets it burn down (~3 s). Physics:
 *   roar     turbulent combustion, a monopole noise: broadband, peaking at f_pk 300-600 Hz and falling above (E11;
 *            alpha ~3 by ear, E1; big fires lower: small surface fires ~200 Hz, E35). Here noise -> a broad band-pass
 *            (Q 0.5) at f_pk 400 Hz (fire 0.1 m) .. 120 Hz (3 m), -6 dB/oct above (the faint 2.5 kHz hiss band is gone:
 *            36 dB under the roar it changed no measure, and the user wants no hiss under the engines); campfire recordings are broad (100 Hz-16 kHz within ~10 dB) and their bed sits 7-14 dB under the
 *            crackle; and a 50 Hz band for the plume's slow turbulence (the recordings: -12..-26 dB re total at 40-100
 *            Hz; ours). The burning logs add as independent sources:
 *            gain ~ sqrt(sum of their heat).
 *   flicker  the fire puffs at f_p = 0.52 sqrt(g / D) (E14: ~3 Hz for 30 cm, 1.6 Hz for 1 m): an amplitude
 *            modulation of the roar, depth and gusts by DRAUGHT, plus a ~1 Hz wander (E9's idea).
 *   crackle  fracture: energies p(E) ~ E^-1.4 (E33 / E34: 1.37-1.45) over 3 decades, Omori aftershocks after each
 *            crackle (p 1.68, c 70 ms, sizes at most 1.7 decades below: E33; BURSTY = their mean count, E33 ~8 per
 *            main shock). Each crackle: a click into a shared bank (E32: peaks ~3.2 and ~7.2 kHz, a notch near
 *            6 kHz for green wood / needles; dry wood brighter, 4.5 / 9.5 kHz: ours, unmeasured), Q giving a
 *            ~1-1.5 ms ring-down (E32), each a burst of 1-4 sharp clicks over ~0.3-4 ms heard dry too (the recordings:
 *            very impulsive, flat to 16 kHz). Base rate per log: MOIST 1-30 /s (campfires: 8-15 onsets /s in all).
 *   pops     the top ~5 % of the energies also ring a pop of their own (a hero: one two-pole at a random pitch in
 *            2.5-12 kHz, lower for bigger logs: ours).
 * Keys: log size (a semitone per key: lower = bigger: deeper pops, more heat); velocity = the throw's burst size. */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_insect.h"
#include "tx_core_chorus.h"

#define OUT_TRIM 7036               /* output gain, Q12 (+2.7 dB): integrated (a key held 10 s): -41.6 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +2.0 dB with it: the mean of 8 presses back on its
 * loudness (held by the peak bound: raw peaks at most 1 dB over the old ones) */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 1

enum { P_SIZE, P_MOIST, P_WOOD, P_DRAUGHT, P_BURSTY, P_ROAR, P_KIND, NP };

static const tx_param_t PARAMS[NP] = {
    {"SIZE", 127, 50, "fire diameter D: 0.1 .. 3 m (log): flicker 0.52 sqrt(g/D) 5.2 .. 0.9 Hz, roar peak 400 .. 120 Hz"},
    {"MOIST", 127, 72, "moisture / resin: crackles 1 .. 30 /s per log (log; campfire recordings: 8-15 onsets/s in all)"},
    {"WOOD", 127, 40, "green / needles (3.2 + 7.2 kHz, notch at 6) .. dry (brighter, 4.5 + 9.5 kHz)"},
    {"DRAUGHT", 127, 30, "wind on the fire: steady .. deep flicker and gusts"},
    {"BURSTY", 127, 24, "aftershocks per crackle: 0 .. 8 (Omori p 1.68, c 70 ms)"},
    {"ROAR", 127, 80, "roar level: off .. loud"},
    {"KIND", 127, 0, "campfire | bonfire | fireplace | embers | kindling | pine | leaves, paper | torch (16 each)"},
};

/* the kinds (KIND): each a fire of its own, the knobs above moving it as they move the campfire. Ours, set against
 * the kinds' reference recordings (analysis only): */
typedef struct {
    int16_t rate;                  /* crackle rate re MOIST's, 1/256 octave */
    int8_t omori;                  /* aftershocks per crackle added to BURSTY's mean */
    uint8_t range;                 /* the sizes' spread re 3 decades, Q7 (smaller: all near the top) */
    uint8_t a;                     /* crackle level, Q7 */
    uint8_t fsc;                   /* the bank's peaks re the wood's, Q7 */
    uint8_t wmin;                  /* the click's least width, samples (1: flat to 16 kHz; 3: dull above ~10 kHz) */
    uint16_t dry;                  /* the dry click re DRY, Q7 */
    int32_t plo, poct;             /* pops: the lowest pitch and the span, log2 Hz Q16 */
    uint16_t pmin;                 /* their floor, Hz */
    int8_t hero;                   /* the pops' threshold re HERO_L2, 1/16 of the range (less: more pops) */
    uint16_t roar;                 /* roar level, Q7 */
    int8_t fpk;                    /* roar peak re SIZE's, 1/16 octave */
    uint16_t tilt;                 /* the roar's extra low-pass corner, Hz (0: none) */
    uint8_t flick, fspd;           /* flicker depth (Q7) and speed re the plume's f_p (Q4) */
    uint8_t flap;                  /* a flame flapping in the wind: random swings 0.5 .. 4 Hz, depth Q7 */
    uint16_t rumb;                 /* the plume's (a chimney's) rumble: level Q7 */
    uint8_t rhz;                   /* and its band, Hz */
    uint8_t catchk, burnk;         /* heat: catches 2^-catchk per block, burns down 2^-burnk */
    uint8_t fuel;                  /* 1: a piece burns out while held, the next catches after a while (flare-ups) */
    uint8_t hiss;                  /* resin: hiss bursts per log per 10 s (shaped events, no bed) */
    uint16_t out;                  /* its level, Q8 (by render, 3 logs: a bonfire +3 dB over the campfire, embers -6;
                                    * the rest on the campfire's loudness) */
} kind_t;
#define NKIND 8u
static const kind_t KINDS[NKIND] = {
    /* campfire: mixed logs (the engine as it was) */
    {0, 0, 128, 128, 128, 1, 128, 619512, 283116, 900, 0, 128, 0, 0, 128, 16, 0, 128, 50, 6, 7, 0, 0, 256},
    /* bonfire: a big pile, a deep heavy roar (the references: centroid 10.9-13 kHz, the bed 7 dB under the
     * crackle), denser crackle, its pops lower and rarer (the CPU: denser crackle, fewer heroes) */
    {768, -1, 90, 130, 128, 1, 128, 604826, 294912, 600, 6, 320, 0, 0, 150, 16, 30, 200, 40, 7, 8, 0, 0, 90},
    /* fireplace: the chimney's draught a low rumble (the references: 125 Hz dominant), crackle with low pops of
     * settling logs, a quiet roar */
    {0, 0, 128, 128, 115, 2, 100, 539290, 327680, 300, 1, 80, -5, 0, 64, 16, 0, 400, 110, 6, 7, 0, 0, 146},
    /* embers: no flame: sparse bright ticks and tiny pops as they cool (3-8 kHz, the bed 29 dB under the ticks) */
    {-717, -1, 90, 70, 150, 1, 160, 739752, 124518, 2500, -3, 0, 0, 0, 0, 16, 0, 0, 50, 6, 9, 0, 0, 290},
    /* kindling, twigs: fast dense crackle at 3-5 kHz, nothing above ~12 kHz; a twig catches and burns down fast */
    {768, -1, 90, 100, 115, 4, 40, 691454, 131072, 1500, 0, 60, 8, 0, 150, 16, 0, 60, 50, 4, 6, 0, 0, 128},
    /* pine: resin pops, louder and burstier, and short hisses of gas from the resin (events, no bed) */
    {77, 2, 150, 125, 128, 1, 128, 643156, 229376, 900, -3, 128, 0, 0, 128, 16, 0, 128, 50, 6, 7, 0, 4, 256},
    /* leaves, paper: a fast flicker of tiny crackles flat to 12 kHz, the flame fluttering, flaring and dying */
    {563, 0, 80, 100, 128, 1, 256, 670356, 196608, 1200, 4, 180, 16, 0, 160, 64, 0, 64, 50, 3, 4, 1, 0, 147},
    /* torch: a roar flat to ~800 Hz and steeply down above, flapping in the wind (0.5-4 Hz), a sputter now and
     * then */
    {-640, 0, 110, 50, 128, 3, 40, 653118, 131072, 1000, 0, 450, 8, 800, 128, 16, 128, 160, 60, 5, 7, 0, 0, 387},
};

#define NLOG 8u
#define NH 12u
#define NB 4u                      /* the crackle bank: two pairs (green / dry) */
#define NEV 64u
#define RANGE_L2 40795             /* 3 decades of energy: log2(10^3) Q12 (E33: 2-3; the recordings spread 5-7.6 dB) */
#define HERO_BLK 3u                /* new heroes per block at most (their set-up is the cost) */
#define HERO_L2 12000              /* the top ~44 % of the truncated law ring on their own (2.9 octaves of 10) */
#define A_REF 21000                /* the largest crackle's amplitude, Q15 (calibrated by render) */
#define DRY 96                     /* the dry click: Q15 x 96 -> Q23 (0.37 of the impulse; by the recordings' flat HF) */
#define HISS_G 2000                /* a resin hiss at its loudest, Q15 (by render: ~12 dB under the loud pops) */
#define ROAR_G 2500                /* roar gain of one log fully burning at ROAR 127 (Q15, 0.27; by the recordings: */
                                   /* the bed sits 7-14 dB under the total) */

typedef struct {
    int32_t heat;                  /* Q15: catches ~1.5 s, burns down ~3 s */
    int32_t fuel;                  /* (fuel kinds) what is left of the piece burning, Q15 */
    uint16_t relight;              /* (fuel kinds) blocks until the next piece catches */
    uint8_t em, note, alive, pad;
} log_t;

typedef struct {
    tx_cloud_t cloud;
    tx_noise_t roar, rumble;       /* the roar band, the plume's rumble (50 Hz) */
    tx_noise_t hiss;               /* (pine) a resin hiss burst */
    int32_t hg, hpk;               /* its gain now and its peak, Q15 */
    uint16_t hbl;                  /* blocks left in it (0: decaying) */
    uint16_t flapt;                /* (torch) blocks to the flap's next swing */
    int32_t flapv, flapg;          /* the flap now and where it swings to, Q15 */
    tx_dcblock_t hp;
    tx_ireso_t bank[NB];
    tx_ireso_t hero[NH];
    log_t lg[NLOG];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK];
    uint16_t off[NEV], hoff[NH];
    int16_t amp[NEV], hamp[NH];
    uint8_t hnum[NH];
    uint32_t fph, fw;              /* the flicker phase and its step per block */
    int32_t wander, gust;          /* ~1 Hz wander and gusts, Q12 */
    uint8_t p[NP], limit, quiet;
    const kind_t *k;
    int32_t level, d_l2, pinv;
    uint32_t pm;
    uint16_t dropped;
    tx_vary_t vy;
} fire_t;

static void set_wood(fire_t *s)
{
    int32_t w = s->p[P_WOOD] * 258;                         /* Q15: 0 green .. 1 dry */
    uint32_t c = s->k->fsc;                                  /* (the kind's peaks) */
    tx_ireso_set(&s->bank[0], 3200u * c >> 7, 80);           /* Q 5: ~1 ms (E32 peaks; Q ours) */
    tx_ireso_set(&s->bank[1], 7200u * c >> 7, 144);          /* Q 9 */
    tx_ireso_set(&s->bank[2], 4500u * c >> 7, 80);
    tx_ireso_set(&s->bank[3], 9500u * c >> 7, 144);
    s->bank[0].g = s->bank[1].g = (int32_t)((32768 - w) >> 7);
    s->bank[2].g = s->bank[3].g = (int32_t)(w >> 7);
    s->bank[1].g = s->bank[1].g * 3 >> 2;                    /* the upper peak a little lower (E32: ~-2 dB) */
    s->bank[3].g = s->bank[3].g * 3 >> 2;
}

static void set_param(fire_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_SIZE: {
        uint32_t fpk;
        s->d_l2 = -217706 + (int32_t)(v * 2531u);           /* log2(0.1 m) + v log2(30) / 127, Q16 */
        fpk = tx_exp2(566323 - (int32_t)v * 900 + s->k->fpk * 4096);   /* 400 Hz -> 120 Hz (1.74 oct down): broad and */
        tx_noise_band(&s->roar, fpk, 8, TX_NOISE_BAND);      /* low, as the campfire recordings (Q 0.5, no tilt: */
        tx_noise_tilt(&s->roar, s->k->tilt);                 /* -6 dB/oct above; a torch: steeper) */
        /* f_p = 0.52 sqrt(g / D) = 1.6287 / sqrt(D) Hz; the phase step per block: f_p x 256 / fs x 2^32 */
        s->fw = (uint32_t)(((uint64_t)tx_exp2(1656437 - s->d_l2 / 2) * s->k->fspd) >> 4);
        break;
    }
    case P_MOIST: {                                          /* 1 .. 30 /s, Q8, re the kind */
        uint32_t r = tx_knob_exp(v, 256u, 321580);
        if (s->k->rate) {
            r = (uint32_t)(((uint64_t)r * tx_exp2(s->k->rate * 256 + (16 << 16))) >> 16);
            if (r > (64u << 8)) r = 64u << 8;                /* at most 64 /s a log (CPU: the densest kinds) */
        }
        tx_cloud_rate(&s->cloud, r);
        break;
    }
    case P_WOOD: set_wood(s); break;
    case P_BURSTY: {                                         /* p 1.68, c 70 ms, 1.7 dec */
        int32_t o = (int32_t)((v + 8u) >> 4) + s->k->omori;
        tx_cloud_omori(&s->cloud, (uint32_t)(o < 0 ? 0 : o), 6881, 70, 23101);
        break;
    }
    case P_KIND: {                                           /* a kind: everything it shapes set afresh */
        unsigned i;
        s->k = &KINDS[(v * NKIND) >> 7];
        for (i = 0; i < P_KIND; i++) set_param(s, i, s->p[i]);
        tx_noise_band(&s->rumble, s->k->rhz, 8, TX_NOISE_BAND);
        break;
    }
    default: break;
    }
}

static void fire_init(void *st, uint32_t seed)
{
    fire_t *s = (fire_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 6, 400);                       /* the throw: 6 crackles over ~0.4 s (ours) */
    tx_cloud_sizes(&s->cloud, 5734, RANGE_L2);               /* (aftershock sizes; the rest are redrawn) */
    s->cloud.after_stream = 1;
    tx_noise_init(&s->roar, seed ^ 0xF12EF12Eu);
    tx_noise_init(&s->rumble, seed ^ 0x2B3C4Du);
    tx_noise_band(&s->rumble, 50, 8, TX_NOISE_BAND);      /* the buoyant plume's slow turbulence, below 100 Hz */
    s->rumble.glide = 0;                                     /* (the recordings: -13..-25 dB re total at 40-80 Hz; ours) */
    tx_noise_init(&s->hiss, seed ^ 0x51C0FFEEu);
    for (i = 0; i < NLOG; i++) s->lg[i].em = 0xFF;
    s->k = &KINDS[0];
    {   /* the truncated power law b = 1.4: pinv = 1 / 0.4, Lmax = RANGE x 0.4 */
        int32_t lmax = (int32_t)((RANGE_L2 * 1638) >> 12);
        s->pinv = (int32_t)((1u << 24) / 1638u);
        s->pm = 0xFFFFFFFFu - tx_exp2((32 << 16) - lmax * 16);
    }
    s->limit = 8;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((fire_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void fire_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((fire_t *)st)->vy);
    tx_cloud_key(&((fire_t *)st)->cloud, note, vel, on);
}

static void fire_param(void *st, uint8_t id, uint8_t val)
{
    fire_t *s = (fire_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NH ? NH : val);
        for (i = s->limit; i < NH; i++) s->hero[i].y1 = s->hero[i].y2 = s->hero[i].p1 = s->hero[i].p2 = 0;
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

static int32_t draw_size(fire_t *s)
{
    uint32_t u = 0xFFFFFFFFu - (uint32_t)(((uint64_t)tx_rand(&s->cloud.rng) * s->pm) >> 32);
    int32_t x = (int32_t)(((int64_t)tx_neglog2_q12(u) * s->pinv) >> 12);
    return x > RANGE_L2 ? RANGE_L2 : x;
}

/* a crackle's own ring (a hero): the piece of wood that cracks rings at its own pitch and damping. Pitch log-uniform
 * over 0.7 .. 14 kHz (4.3 octaves; bigger cracks up to 0.8 octave lower, a key a quarter tone: a bigger log deeper),
 * Q 4 .. 18 (log-uniform: a dull thock .. a ringing tick). Ours, from the campfire recordings' per-crackle centroid
 * spread (0.8-2 octaves, note 17). Returns 0 when no hero is free (the crackle stays in the shared bank). */
static int pop(fire_t *s, unsigned o, int32_t a, unsigned note, int32_t sz)
{
    unsigned i, pick = NH;
    int32_t l2;
    uint32_t f;
    for (i = 0; i < s->limit && pick == NH; i++) if (!s->hnum[i] && tx_ireso_quiet(&s->hero[i])) pick = i;
    if (pick == NH) { s->dropped++; return 0; }
    l2 = s->k->plo + (int32_t)tx_rand_below(&s->cloud.rng, (uint32_t)s->k->poct)   /* log2(700) + 0 .. 4.32 oct */
         - ((sz * 1316) >> 10)                                     /* the biggest 0.8 oct lower (sz / RANGE x 0.8 oct) */
         + ((int32_t)note - 60) * 2731;
    f = tx_exp2(l2);
    if (f > 16000u) f = 16000u;
    if (f < s->k->pmin) f = s->k->pmin;
    {
        uint32_t q = tx_exp2((6 << 16) + (int32_t)tx_rand_below(&s->cloud.rng, 140000u));   /* Q x 16: 64 .. 282 */
        if (f < 600u && q > 64u) q = 64u;                    /* a low thump: Q 4 at most (it rings short; CPU) */
        tx_ireso_set(&s->hero[pick], f, q);
    }
    s->hero[pick].g = 128;
    s->hero[pick].zlim = 64;
    s->hoff[pick] = (uint16_t)o; s->hamp[pick] = (int16_t)a; s->hnum[pick] = 1;
    return 1;
}

TX_AUDIO_CODE static void fire_render(void *st, int32_t *mix, unsigned n)
{
    fire_t *s = (fire_t *)st;
    tx_rng_t *r = &s->cloud.rng;
    unsigned k, i, j, ne = 0, nh = 0;
    uint16_t d0 = s->dropped;
    int32_t heat = 0, any = 0, g;
    if (n > TX_BLOCK) n = TX_BLOCK;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;
    for (j = 0; j < n; j++) s->buf[j] = 0;
    for (i = 0; i < k; i++) {
        const tx_event_t *e = &s->ev[i];
        int32_t sz, a, vl2;
        if (e->kind == TX_EV_MAIN) {                         /* a log thrown on */
            log_t *lg = 0;
            for (j = 0; j < NLOG && !lg; j++) if (s->lg[j].alive && s->lg[j].note == e->note) lg = &s->lg[j];
            for (j = 0; j < NLOG && !lg; j++) if (!s->lg[j].alive) lg = &s->lg[j];
            if (lg) {
                if (!lg->alive) lg->heat = 0;
                lg->alive = 1; lg->em = e->emitter; lg->note = e->note;
                if (s->k->fuel) { lg->fuel = 32767; lg->relight = 0; }   /* (a fresh piece) */
            }
        }
        if (s->k->fuel && e->kind == TX_EV_STREAM) {         /* a piece burning out: its crackles thin with its heat */
            const log_t *lp = 0;
            for (j = 0; j < NLOG && !lp; j++) if (s->lg[j].alive && s->lg[j].note == e->note) lp = &s->lg[j];
            if (!lp || (int32_t)(tx_rand(r) >> 17) >= lp->heat) continue;
        }
        sz = e->kind == TX_EV_AFTER ? e->size_l2 : draw_size(s);
        if (e->kind == TX_EV_MAIN) sz = RANGE_L2 - 8192 - (int32_t)tx_rand_below(r, 8192u);   /* the throw's big one */
        if (s->k->range != 128u) sz = RANGE_L2 - (((RANGE_L2 - sz) * s->k->range) >> 7);   /* the kind's spread */
        vl2 = ((int32_t)e->vel - 100) * 655 + ((60 - (int32_t)e->note) * 1365);   /* velocity; bigger logs +3 dB/oct */
        a = (int32_t)(((int64_t)tx_exp2(((sz - RANGE_L2) * 16 + vl2) / 2 + (15 << 16)) * A_REF) >> 15);
        a = (int32_t)(((int64_t)a * s->level) >> 15);
        if (s->k->a != 128u) a = (a * s->k->a) >> 7;
        if (a > 32767) a = 32767;
        if (a < 8) continue;
        {   /* a crackle = a burst of 1-4 sharp clicks over ~0.3-4 ms, random signs, each ~0.6 x the last (the */
            uint32_t nc = 1u + tx_rand_below(r, 4u), c, o = e->offset;   /* recordings: attack 2-5 ms, very */
            int32_t aa = a;                                  /* impulsive: kurtosis 200-2600; ours) */
            /* no two crackles alike (note 14: the recordings' per-crackle centroid spreads 0.8-2 octaves, ours 0.35): the
             * fracture's own front (a click 1 .. 10 samples wide: flat to 16 kHz .. dull above ~4 kHz; bigger cracks
             * blunter) and how much of it the wood rings (the bank's share +-2 octaves, sd) */
            int32_t gb = tx_gauss_q12(r) * 16, bw, dr;
            uint32_t w;
            int own = sz >= HERO_L2 + s->k->hero * (RANGE_L2 >> 4) && nh < HERO_BLK && pop(s, e->offset, a, e->note, sz);   /* its own ring: the bank a */
            nh += (unsigned)own;                             /* little (at most HERO_BLK new per block: CPU) */
            gb -= 45426 + (own ? 2 << 16 : 0);                                     /* (the power's mean kept: ln2 sd^2 octaves under) */
            gb = gb > (2 << 16) ? (2 << 16) : gb < -(3 << 16) ? -(3 << 16) : gb;
            bw = (int32_t)tx_exp2(gb + (16 << 16));   /* Q16 */
            uint32_t q;
            dr = tx_gauss_q12(r) * 16 - 22713;               /* and how dry its snap: +-2 octaves (sd), power kept */
            dr = dr > (2 << 16) ? (2 << 16) : dr < -(3 << 16) ? -(3 << 16) : dr;
            dr = (int32_t)tx_exp2(dr + (12 << 16));          /* Q12 */
            w = s->k->wmin + tx_rand_below(r, 6u) + (sz > (RANGE_L2 * 3) / 4 ? tx_rand_below(r, 4u) : 0u);
            if (s->k->dry != 128u) dr = (dr * s->k->dry) >> 7;
            for (c = 0; c < nc && o < n && ne < NEV; c++) {
                int32_t sa = (tx_rand(r) & 1u) ? aa : -aa, sb = (int32_t)(((int64_t)sa * bw) >> 16), d = (((sa * DRY) >> 6) * dr >> 6) / (int32_t)w;
                s->off[ne] = (uint16_t)o; s->amp[ne] = (int16_t)(sb > 32767 ? 32767 : sb < -32767 ? -32767 : sb); ne++;
                for (q = 0; q < w && o + q < n; q++) s->buf[o + q] += d;   /* the click itself (to 16 kHz in the refs) */
                o += 13u + tx_rand_below(r, 40u);
                aa = aa * 19661 >> 15;
            }
        }
    }
    for (i = 0; i < NLOG; i++) {                             /* the logs' heat -> the roar */
        log_t *lg = &s->lg[i];
        int held;
        if (!lg->alive) continue;
        held = lg->em < TX_CLOUD_EMITTERS && s->cloud.em[lg->em].held && s->cloud.em[lg->em].note == lg->note;
        if (s->k->fuel && held) {                            /* a piece flares, burns out; the next catches after a
                                                              * pause (exponential, ~1.5 s): flare-ups, never a beat */
            if (lg->relight && !--lg->relight) lg->fuel = 24576 + (int32_t)tx_rand_below(r, 8192u);
            lg->heat += (lg->fuel - lg->heat) >> s->k->catchk;
            lg->fuel -= (lg->heat >> 8) + 1;
            if (lg->fuel <= 0) {
                lg->fuel = 0;
                if (!lg->relight) lg->relight = (uint16_t)(43u + (tx_cloud_exp_q8(&s->cloud, 258u << 8) >> 8));
            }
            if (lg->heat < 1) lg->heat = 1;                  /* (still there: smouldering) */
        } else if (held) lg->heat += (32767 - lg->heat) >> s->k->catchk;   /* catches: tau ~0.37 s (~1.5 s to full) */
        else lg->heat -= (lg->heat >> s->k->burnk) + 1;     /* burns down: tau ~0.75 s (~3-4 s) */
        if (lg->heat <= 0) { lg->heat = 0; lg->alive = 0; lg->em = 0xFF; continue; }
        if (s->k->hiss && lg->heat > 8192 && tx_rand_below(r, 1720u) < (uint32_t)s->k->hiss * (uint32_t)lg->heat >> 15
            && !s->hbl && s->hg < 256) {                     /* resin: a hiss of gas, 60-350 ms, at 2.5-6 kHz */
            s->hbl = (uint16_t)(10u + (tx_cloud_exp_q8(&s->cloud, 20u << 8) >> 8));
            if (s->hbl > 60u) s->hbl = 60u;
            s->hpk = (int32_t)tx_exp2((13 << 16) + (int32_t)tx_rand_below(r, 131072u));   /* 0.25 .. 1, Q15 */
            tx_noise_band(&s->hiss, 2500u + tx_rand_below(r, 3500u), 16, TX_NOISE_BAND);
        }
        heat += lg->heat;
        any = 1;
    }
    {   /* flicker (E14) + wander + gusts (DRAUGHT) -> the roar gain, sqrt of the summed heat */
        int32_t d = s->p[P_DRAUGHT] * 258, am, sq;
        uint32_t x = (uint32_t)heat;                         /* <= 8 x 2^15 */
        s->fph += s->fw + (uint32_t)(((int64_t)tx_rand_s15(r) * (int32_t)(s->fw >> 3)) >> 15);   /* phase jitter */
        s->wander += (tx_gauss_q12(r) * 4 - s->wander) >> 5;   /* ~1 Hz */
        s->gust += ((s->p[P_DRAUGHT] > 0 && (tx_rand(r) >> 24) < (uint32_t)s->p[P_DRAUGHT] >> 4 ? 8192 : 0) - s->gust) >> 4;
        {
            int32_t fd = 6554 + (d >> 1);
            if (s->k->flick != 128u) fd = (fd * s->k->flick) >> 7;
            am = 32768 + (int32_t)(((int64_t)tx_sin_q15(s->fph) * fd) >> 15)
                       + ((s->wander * (2048 + (d >> 3))) >> 12) + (int32_t)(((int64_t)s->gust * d) >> 15);
        }
        if (am < 0) am = 0;
        if (s->k->flap) {                                    /* a torch in the wind: the flame swings to a new place at
                                                              * random moments (~3 /s, Poisson), ~50 ms to get there */
            if (!s->flapt) {
                s->flapg = (int32_t)(tx_rand(r) >> 17);
                s->flapt = (uint16_t)(1u + (tx_cloud_exp_q8(&s->cloud, 57u << 8) >> 8));
            } else s->flapt--;
            s->flapv += (s->flapg - s->flapv) >> 3;
            am = (int32_t)(((int64_t)am * (32768 - ((s->flapv * s->k->flap) >> 7))) >> 15);
        }
        sq = 0;
        if (x) {                                             /* isqrt of the heat, Q15 -> Q15 */
            uint32_t rr = 0, bit = 1u << 30, xx = x << 12;
            while (bit > xx) bit >>= 2;
            while (bit) { if (xx >= rr + bit) { xx -= rr + bit; rr = (rr >> 1) + bit; } else rr >>= 1; bit >>= 2; }
            sq = (int32_t)((rr * 181u) >> 6);                /* sqrt(x 2^12) = 64 sqrt(x); sqrt(x / 2^15) in Q15 */
        }
        g = (int32_t)(((int64_t)ROAR_G * s->p[P_ROAR] >> 7) * sq >> 15);
        if (s->k->roar != 128u) g = (g * s->k->roar) >> 7;
        g = (int32_t)(((int64_t)g * am) >> 15);
        g = (int32_t)(((int64_t)g * s->level) >> 15);
        if (!any) g = 0;
        tx_noise_roar_add(&s->roar, g, s->buf, n);
        {   /* the plume's (a chimney's) rumble: with the roar's own level, before the kind's roar share */
            int32_t gr = (int32_t)(((int64_t)ROAR_G * s->p[P_ROAR] >> 7) * sq >> 15);
            gr = (int32_t)(((int64_t)gr * am) >> 15);
            gr = (int32_t)(((int64_t)gr * s->level) >> 15);
            if (!any) gr = 0;
            tx_noise_level(&s->rumble, s->k->rumb == 128u ? g * 2 : (gr * s->k->rumb) >> 6);
        }
        tx_noise_add(&s->rumble, s->buf, n);
        if (s->k->hiss || s->hg) {                           /* resin hiss: ~10 ms up, held, ~40 ms down */
            int32_t t = s->hbl ? (int32_t)(((int64_t)s->hpk * s->level) >> 15) * HISS_G >> 15 : 0;
            if (s->hbl) s->hbl--;
            s->hg += t > s->hg ? (t - s->hg) >> 1 : -((s->hg - t) >> 3) - (s->hg > 0);
            if (s->hg < 0) s->hg = 0;
            tx_noise_roar_add(&s->hiss, s->hg, s->buf, n);
        }
    }
    for (i = 0; i < NB; i++) tx_ireso_add(&s->bank[i], s->off, s->amp, ne, s->buf, n);
    for (i = 0; i < NH; i++) { tx_ireso_add(&s->hero[i], &s->hoff[i], &s->hamp[i], s->hnum[i], s->buf, n); s->hnum[i] = 0; }
    tx_vary_block(&s->vy, s->buf, n);
    tx_trim(s->buf, n, s->k->out == 256u ? OUT_TRIM : (OUT_TRIM * s->k->out) >> 8);
    tx_dcblock_add(&s->hp, s->buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = any | s->roar.gain | !tx_noise_quiet(&s->rumble) | s->hp.x1 | s->hp.y1 | s->hg | s->hiss.gain;
        for (i = 0; i < NB; i++) z |= !tx_ireso_quiet(&s->bank[i]);
        for (i = 0; i < NH; i++) z |= !tx_ireso_quiet(&s->hero[i]);
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void fire_stats(const void *st, tx_stats_t *o)
{
    const fire_t *s = (const fire_t *)st;
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

/* the worst case: eight logs, wet and bursty (aftershock storms), every pop ringing, full roar, windy */
static const char SCENE_MAX[] =
    "0 particles 12\n0 param MOIST 127\n0 param BURSTY 127\n0 param ROAR 127\n0 param DRAUGHT 127\n0 param SIZE 127\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "500 on 53 127\n500 on 60 127\n500 on 67 127\n500 on 74 127\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "6000 end\n";

const tx_engine_t tx_eng_fire = {
    "fire", "#8 Wood fire", NP, PARAMS, NH, 8, sizeof(fire_t), SCENE_MAX,
    fire_init, fire_key, fire_param, fire_render, fire_stats,
};
