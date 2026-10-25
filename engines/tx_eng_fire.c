/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #8 Wood fire: roar, crackle and pops (CLOUD with Omori, NOISE roar, INSECT resonators).
 *
 * A key throws a log on the fire: a burst of crackle with an Omori tail, the roar swelling as it catches (~1.5 s);
 * holding keeps it burning (crackles at the MOIST rate); release lets it burn down (~3 s). Physics:
 *   roar     turbulent combustion, a monopole noise: broadband, peaking at f_pk 300-600 Hz and falling above (E11;
 *            alpha ~3 by ear, E1; big fires lower: small surface fires ~200 Hz, E35). Here noise -> a broad band-pass
 *            (Q 0.5) at f_pk 400 Hz (fire 0.1 m) .. 120 Hz (3 m), -6 dB/oct above, plus a faint hiss band at 2.5 kHz:
 *            campfire recordings are broad (100 Hz-16 kHz within ~10 dB) and their bed sits 7-14 dB under the
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

enum { P_SIZE, P_MOIST, P_WOOD, P_DRAUGHT, P_BURSTY, P_ROAR, NP };

static const tx_param_t PARAMS[NP] = {
    {"SIZE", 127, 50, "fire diameter D: 0.1 .. 3 m (log): flicker 0.52 sqrt(g/D) 5.2 .. 0.9 Hz, roar peak 400 .. 120 Hz"},
    {"MOIST", 127, 72, "moisture / resin: crackles 1 .. 30 /s per log (log; campfire recordings: 8-15 onsets/s in all)"},
    {"WOOD", 127, 40, "green / needles (3.2 + 7.2 kHz, notch at 6) .. dry (brighter, 4.5 + 9.5 kHz)"},
    {"DRAUGHT", 127, 30, "wind on the fire: steady .. deep flicker and gusts"},
    {"BURSTY", 127, 24, "aftershocks per crackle: 0 .. 8 (Omori p 1.68, c 70 ms)"},
    {"ROAR", 127, 80, "roar level: off .. loud"},
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
#define ROAR_G 2500                /* roar gain of one log fully burning at ROAR 127 (Q15, 0.27; by the recordings: */
                                   /* the bed sits 7-14 dB under the total) */

typedef struct {
    int32_t heat;                  /* Q15: catches ~1.5 s, burns down ~3 s */
    uint8_t em, note, alive, pad;
} log_t;

typedef struct {
    tx_cloud_t cloud;
    tx_noise_t roar, hiss, rumble; /* the roar band, the flame's hiss (2.5 kHz, Q 0.5), the plume's rumble (50 Hz) */
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
    int32_t level, d_l2, pinv;
    uint32_t pm;
    uint16_t dropped;
    tx_vary_t vy;
} fire_t;

static void set_wood(fire_t *s)
{
    int32_t w = s->p[P_WOOD] * 258;                         /* Q15: 0 green .. 1 dry */
    tx_ireso_set(&s->bank[0], 3200, 80);                     /* Q 5: ~1 ms (E32 peaks; Q ours) */
    tx_ireso_set(&s->bank[1], 7200, 144);                    /* Q 9 */
    tx_ireso_set(&s->bank[2], 4500, 80);
    tx_ireso_set(&s->bank[3], 9500, 144);
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
        fpk = tx_exp2(566323 - (int32_t)v * 900);         /* 400 Hz -> 120 Hz (1.74 oct down): broad and low, as the */
        tx_noise_band(&s->roar, fpk, 8, TX_NOISE_BAND);      /* campfire recordings (Q 0.5, no tilt: -6 dB/oct above) */
        tx_noise_tilt(&s->roar, 0);
        /* f_p = 0.52 sqrt(g / D) = 1.6287 / sqrt(D) Hz; the phase step per block: f_p x 256 / fs x 2^32 */
        s->fw = tx_exp2(1656437 - s->d_l2 / 2);
        break;
    }
    case P_MOIST: tx_cloud_rate(&s->cloud, tx_knob_exp(v, 256u, 321580)); break;  /* 1 .. 30 /s, Q8 */
    case P_WOOD: set_wood(s); break;
    case P_BURSTY: tx_cloud_omori(&s->cloud, (v + 8u) >> 4, 6881, 70, 23101); break;   /* p 1.68, c 70 ms, 1.7 dec */
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
    tx_noise_init(&s->hiss, seed ^ 0x415541u);
    tx_noise_band(&s->hiss, 2500, 8, TX_NOISE_BAND);
    s->hiss.glide = 0;
    tx_noise_init(&s->rumble, seed ^ 0x2B3C4Du);
    tx_noise_band(&s->rumble, 50, 8, TX_NOISE_BAND);      /* the buoyant plume's slow turbulence, below 100 Hz */
    s->rumble.glide = 0;                                     /* (the recordings: -13..-25 dB re total at 40-80 Hz; ours) */
    for (i = 0; i < NLOG; i++) s->lg[i].em = 0xFF;
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
    l2 = 619512 + (int32_t)tx_rand_below(&s->cloud.rng, 283116u)   /* log2(700) + 0 .. 4.32 oct */
         - ((sz * 1316) >> 10)                                     /* the biggest 0.8 oct lower (sz / RANGE x 0.8 oct) */
         + ((int32_t)note - 60) * 2731;
    f = tx_exp2(l2);
    if (f > 16000u) f = 16000u;
    if (f < 900u) f = 900u;
    tx_ireso_set(&s->hero[pick], f, tx_exp2((6 << 16) + (int32_t)tx_rand_below(&s->cloud.rng, 140000u)));   /* Q x 16: 64 .. 282 */
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
            if (lg) { if (!lg->alive) lg->heat = 0; lg->alive = 1; lg->em = e->emitter; lg->note = e->note; }
        }
        sz = e->kind == TX_EV_AFTER ? e->size_l2 : draw_size(s);
        if (e->kind == TX_EV_MAIN) sz = RANGE_L2 - 8192 - (int32_t)tx_rand_below(r, 8192u);   /* the throw's big one */
        vl2 = ((int32_t)e->vel - 100) * 655 + ((60 - (int32_t)e->note) * 1365);   /* velocity; bigger logs +3 dB/oct */
        a = (int32_t)(((int64_t)tx_exp2(((sz - RANGE_L2) * 16 + vl2) / 2 + (15 << 16)) * A_REF) >> 15);
        a = (int32_t)(((int64_t)a * s->level) >> 15);
        if (a > 32767) a = 32767;
        if (a < 8) continue;
        {   /* a crackle = a burst of 1-4 sharp clicks over ~0.3-4 ms, random signs, each ~0.6 x the last (the */
            uint32_t nc = 1u + tx_rand_below(r, 4u), c, o = e->offset;   /* recordings: attack 2-5 ms, very */
            int32_t aa = a;                                  /* impulsive: kurtosis 200-2600; ours) */
            /* no two crackles alike (note 14: the recordings' per-crackle centroid spreads 0.8-2 octaves, ours 0.35): the
             * fracture's own front (a click 1 .. 10 samples wide: flat to 16 kHz .. dull above ~4 kHz; bigger cracks
             * blunter) and how much of it the wood rings (the bank's share +-1 octave, sd) */
            int32_t gb = tx_gauss_q12(r) * 16, bw, dr;
            uint32_t w;
            int own = sz >= HERO_L2 && nh < HERO_BLK && pop(s, e->offset, a, e->note, sz);   /* its own ring: the bank a */
            nh += (unsigned)own;                             /* little (at most HERO_BLK new per block: CPU) */
            gb -= 45426 + (own ? 2 << 16 : 0);                                     /* (the power's mean kept: ln2 sd^2 octaves under) */
            gb = gb > (2 << 16) ? (2 << 16) : gb < -(3 << 16) ? -(3 << 16) : gb;
            bw = (int32_t)tx_exp2(gb + (16 << 16));   /* Q16 */
            uint32_t q;
            dr = tx_gauss_q12(r) * 16 - 22713;               /* and how dry its snap: +-1 octave (sd), power kept */
            dr = dr > (2 << 16) ? (2 << 16) : dr < -(3 << 16) ? -(3 << 16) : dr;
            dr = (int32_t)tx_exp2(dr + (12 << 16));          /* Q12 */
            w = 1u + tx_rand_below(r, 6u) + (sz > (RANGE_L2 * 3) / 4 ? tx_rand_below(r, 4u) : 0u);
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
        if (held) lg->heat += (32767 - lg->heat) >> 6;       /* catches: tau ~0.37 s (~1.5 s to full) */
        else lg->heat -= (lg->heat >> 7) + 1;                /* burns down: tau ~0.75 s (~3-4 s) */
        if (lg->heat <= 0) { lg->heat = 0; lg->alive = 0; lg->em = 0xFF; continue; }
        heat += lg->heat;
        any = 1;
    }
    {   /* flicker (E14) + wander + gusts (DRAUGHT) -> the roar gain, sqrt of the summed heat */
        int32_t d = s->p[P_DRAUGHT] * 258, am, sq;
        uint32_t x = (uint32_t)heat;                         /* <= 8 x 2^15 */
        s->fph += s->fw + (uint32_t)(((int64_t)tx_rand_s15(r) * (int32_t)(s->fw >> 3)) >> 15);   /* phase jitter */
        s->wander += (tx_gauss_q12(r) * 4 - s->wander) >> 5;   /* ~1 Hz */
        s->gust += ((s->p[P_DRAUGHT] > 0 && (tx_rand(r) >> 24) < (uint32_t)s->p[P_DRAUGHT] >> 4 ? 8192 : 0) - s->gust) >> 4;
        am = 32768 + (int32_t)(((int64_t)tx_sin_q15(s->fph) * (6554 + (d >> 1))) >> 15)
                   + ((s->wander * (2048 + (d >> 3))) >> 12) + (int32_t)(((int64_t)s->gust * d) >> 15);
        if (am < 0) am = 0;
        sq = 0;
        if (x) {                                             /* isqrt of the heat, Q15 -> Q15 */
            uint32_t rr = 0, bit = 1u << 30, xx = x << 12;
            while (bit > xx) bit >>= 2;
            while (bit) { if (xx >= rr + bit) { xx -= rr + bit; rr = (rr >> 1) + bit; } else rr >>= 1; bit >>= 2; }
            sq = (int32_t)((rr * 181u) >> 6);                /* sqrt(x 2^12) = 64 sqrt(x); sqrt(x / 2^15) in Q15 */
        }
        g = (int32_t)(((int64_t)ROAR_G * s->p[P_ROAR] >> 7) * sq >> 15);
        g = (int32_t)(((int64_t)g * am) >> 15);
        g = (int32_t)(((int64_t)g * s->level) >> 15);
        if (!any) g = 0;
        tx_noise_roar_add(&s->roar, g, s->buf, n);
        tx_noise_level(&s->hiss, g >> 6);                    /* the hiss well under the roar, same flicker (ours) */
        tx_noise_add(&s->hiss, s->buf, n);
        tx_noise_level(&s->rumble, g * 2);
        tx_noise_add(&s->rumble, s->buf, n);
    }
    for (i = 0; i < NB; i++) tx_ireso_add(&s->bank[i], s->off, s->amp, ne, s->buf, n);
    for (i = 0; i < NH; i++) { tx_ireso_add(&s->hero[i], &s->hoff[i], &s->hamp[i], s->hnum[i], s->buf, n); s->hnum[i] = 0; }
    tx_vary_block(&s->vy, s->buf, n);
    tx_trim(s->buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, s->buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = any | s->roar.gain | !tx_noise_quiet(&s->hiss) | !tx_noise_quiet(&s->rumble) | s->hp.x1 | s->hp.y1;
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
