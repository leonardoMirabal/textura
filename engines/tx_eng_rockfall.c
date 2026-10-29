/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #34 Rockfall / scree: a rock face lets go (R1 Hertz).
 *
 * A key press triggers a fall (velocity = volume; lower keys = a bigger main block). The time course follows R2's
 * seismic template:
 *   crack      fracture pulses as the block detaches (< 1 s): a few sharp, bright cracks;
 *   impact     the intact mass hits ~1.7 s later (R2), breaking up: one big hit and 2-5 follow-ups within ~1 s;
 *   free fall  silence, sqrt(2 h / g) (R2: 7.5 s for 271 m);
 *   fragments  impacts on the talus with a rate decaying over FRAG (4 .. 13 s; R2: ~40 s), 3 .. 80 per s,
 *              surges of ~1 s, each its own (R5), sizes from a power law V^-beta (R4: 1.43 .. 2.27), and the rattle of the smallest bits
 *              (SHAKER, PhISEM).
 * Every hit: loudness ~ momentum m v (R1), x sqrt(radiated share) (10^-6 soft marl .. 10^-3 hard rock, R1); a Hertz
 * contact of t_c ~ R v^-0.2 (0.25 m at 10 m/s = 1.3 ms; soft ground longer: ours) into the ground's low boom
 * (3 modes 35 / 70 / 140 Hz, Q 3: ours, note 04's 30-150 Hz) plus a bright click on hard ground. Distance: 1 / r on
 * everything (the hits and the talus alike: -40 dB from 30 m to 3 km) and air absorption (a low-pass: below ~150 Hz air
 * takes well under 1 dB per km, so the far fall is its own low band, 1 / r quieter); beyond ~1 km the many ground and air
 * paths smear that low band into the 70-120 Hz rumble (R7): a noise band led by the low-passed fall's own envelope, at
 * most a quarter of it, so the rumble never outgrows the fall and the level only falls with distance. */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_pulse.h"
#include "tx_core_shaker.h"

#define OUT_TRIM 11024               /* output gain, Q12 (+5.4 dB): max momentary (one velocity-100 press): -34.2 LUFS at LEVEL 100 -> target -20 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter;
                                    * lowered 5.8 dB: the default chord <= +12 dBFS, the max scene <= +24 dBFS) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +3.2 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -100
#define VARY_HI 0                  /* (no boost: the max scene stays under +24.1 dBFS) */
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 3

enum { P_VOLUME, P_HEIGHT, P_FRAG, P_GROUND, P_DIST, NP };

static const tx_param_t PARAMS[NP] = {
    {"VOLUME", 127, 64, "rock volume: 0.01 .. 10 m^3 (log): bigger main block, more fragments, louder (m v)"},
    {"HEIGHT", 127, 50, "fall height: 5 .. 100 m (log): silence sqrt(2 h / g) 1 .. 4.5 s, impact speed sqrt(2 g h)"},
    {"FRAG", 127, 64, "fragmentation: a few big blocks (beta 1.4, 4 s) .. many small (beta 2.3, 13 s of tumbling talus)"},
    {"GROUND", 127, 80, "ground: soft (marl: dull, quiet) .. hard rock (bright, loud): radiated share 1e-6 .. 1e-3"},
    {"DIST", 127, 40, "distance: 30 m .. 3 km (log): 1 / r, darker (air); far: the low-passed fall smeared into a rumble"},
};

#define NF 3u
#define DIST_REF_L2 458697         /* log2(127.9 m) Q16: DIST 40, the default: the hits' and the talus' reference */
#define RUM_UNIT 2654              /* the rumble band's mean |x| per Q8 x Q15 of gain (339639 Q23 at 1.0), / 256 */
#define NG 3u
#define A_REF 36000                /* the main hit at the reference (1 m^3, 30 m fall, 150 m, hard): Q15 (calibrated) */

static const uint16_t GROUND_HZ[NG] = { 35, 70, 140 };

typedef struct {
    uint32_t t;                    /* samples since the trigger */
    uint32_t t_imp, t_frag, t_end; /* the impact, the fragment phase, its end */
    uint32_t next;                 /* the next event (samples on t) */
    int32_t l_main;                /* log2 amplitude of the main hit (Q16) */
    int32_t lr_main;               /* log2(R / 1 m) of the main block, Q16 */
    uint8_t phase, alive, hits, pad;
    uint32_t sp, sinc, sel;        /* the talus's surges: their phase, its step per sample, the last event's time */
    int32_t sdep;                  /* this surge's depth, Q15 */
    tx_rng_t srng;                 /* the surges' own draws (seeded from the cloud's state, not drawn from it) */
} fall_t;

typedef struct {
    tx_cloud_t cloud;
    tx_dcblock_t hp;
    tx_pulses_t pul;
    tx_shaker_t talus;
    tx_noise_t rumble;
    tx_svf_t ground[NG];
    int32_t gf[NG], gg[NG], gi[NG];
    fall_t f[NF];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t ex[TX_BLOCK], buf[TX_BLOCK];
    int32_t lpa, lp1, lp2, rum, dhp;  /* rum: the low-passed fall's envelope (mean |x|, Q23) */
    uint8_t p[NP], limit, quiet, pad;
    int32_t level, vol_l2, h_l2, beta_inv, share_l2, dist_l2;
    int32_t dist_g, rum_sh;        /* 1 / r (Q12, 1.0 at the default 128 m); the rumble's share (Q8) */
    uint32_t frag_ms;
    uint16_t dropped;
    tx_vary_t vy;
} rock_t;

static int32_t log2_q16(uint32_t x) { return (32 << 16) - (int32_t)(tx_neglog2_q12(x ? x : 1u) << 4); }

static void set_param(rock_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_VOLUME: s->vol_l2 = -435412 + (int32_t)(v * 5142u); break;     /* log2(0.01 m^3) + v log2(1000) / 127 */
    case P_HEIGHT: s->h_l2 = 152170 + (int32_t)(v * 2224u); break;        /* log2(5 m) + v log2(20) / 127 */
    case P_FRAG:
        s->beta_inv = (int32_t)(4096u * 4096u / (1720u + v * 21u));   /* 1 / (beta - 1), beta 1.42 .. 2.27, Q12 */
        s->frag_ms = tx_knob_exp(v, 4000u, 111400);          /* 4 .. 13 s (log; R2: ~40 s, a tap quiet within 20 s) */
        break;
    case P_GROUND: s->share_l2 = -1306000 + (int32_t)(v * 5142u); break;  /* log2(1e-6) .. log2(1e-3) */
    case P_DIST: {
        uint32_t fc;
        s->dist_l2 = 321577 + (int32_t)(v * 3428u);                         /* log2(30 m) + v log2(100) / 127 */
        fc = tx_exp2(1001000 - (2 * (s->dist_l2 - 321577)) / 3);           /* air: ~15 kHz at 30 m, (r)^-2/3 */
        fc = fc < 120u ? 120u : fc > 16000u ? 16000u : fc;
        s->lpa = 32768 - (int32_t)tx_exp2((15 << 16) - (int32_t)(fc * 862u >> 6));
        s->dist_g = (int32_t)tx_exp2((12 << 16) - (s->dist_l2 - DIST_REF_L2));    /* 1 / r: 4.3 at 30 m .. 0.043 at 3 km */
        {   /* the rumble's share of the fall's envelope: 0 up to ~1 km, then rising (log r) to 1/4 at 3 km */
            int32_t far = s->dist_l2 - 653000;
            s->rum_sh = far <= 0 ? 0 : far >= 104000 ? 64 : (far * 41) >> 16;   /* 64 / 104000 */
        }
        break;
    }
    default: break;
    }
}

static void rock_init(void *st, uint32_t seed)
{
    rock_t *s = (rock_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    tx_pulses_init(&s->pul);
    tx_shaker_init(&s->talus, seed ^ 0x70C4u);
    tx_shaker_res(&s->talus, 0, 1800, 14745);                /* grit on the talus: ~1.8 kHz, r 0.9 (references: 0.6-2.5 kHz) */
    tx_shaker_objects(&s->talus, 400u << 4, 3000);
    tx_shaker_decay(&s->talus, 400, 31457);
    tx_noise_init(&s->rumble, seed ^ 0x4B4Du);
    tx_noise_band(&s->rumble, 90, 16, TX_NOISE_BAND);        /* R7: 70-120 Hz air wave */
    for (i = 0; i < NG; i++) {
        s->gf[i] = tx_svf_coef(GROUND_HZ[i]);
        s->gi[i] = (int32_t)tx_svf_gin_noise(GROUND_HZ[i], 48u);
        s->gg[i] = 2048 >> i;
    }
    s->limit = 2;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((rock_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void rock_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((rock_t *)st)->vy);
    tx_cloud_key(&((rock_t *)st)->cloud, note, vel, on);
}

static void rock_param(void *st, uint8_t id, uint8_t val)
{
    rock_t *s = (rock_t *)st;
    if (id == TX_PID_PARTICLES) {
        unsigned i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NF ? NF : val);
        for (i = s->limit; i < NF; i++) s->f[i].alive = 0;
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else
        set_param(s, id, val);
}

/* one hit: a block of radius 2^lr m at speed v (log2 v, Q16), relative loudness l (log2, Q16): a Hertz pulse into the
 * ground + a bright click on hard ground */
static void hit(rock_t *s, unsigned off, int32_t lr, int32_t lv, int32_t l)
{
    /* t_c = 1.3 ms (R / 0.25 m) (v / 10)^-0.2 x (soft ground: up to x 4), samples */
    int32_t soft = (-955000 - s->share_l2) / 5;              /* 0 at 1e-3 .. ~2 octaves at 1e-6 */
    uint32_t u = tx_rand(&s->cloud.rng);                     /* each knock its own: a face, an edge, a corner */
    int32_t lt = 336500 + (lr + 131072) - (lv - 217706) / 5 + (soft > 0 ? soft : 0)   /* log2(57 samples) = 5.83 */
                 + (int32_t)(u >> 16) - 32768;                 /* contact +-0.5 octave */
    uint32_t dur = tx_exp2(lt < (1 << 16) ? (1 << 16) : lt > (12 << 16) ? (12 << 16) : lt);
    int32_t a = l > (15 << 16) ? 32767 : l < 0 ? 0 : (int32_t)tx_exp2(l);
    a = (int32_t)(((int64_t)a * s->level) >> 15);
    if (a < 4) return;
    tx_pulse_start(&s->pul, TX_PULSE_SHAPE_HERTZ, dur, a, off);
    if (s->share_l2 > -1150000) {                            /* hard ground: the stone's own bright click (ours), its
                                                              * share 1/12 .. 2/3 by the corner that strikes */
        int32_t c = (int32_t)(((int64_t)a * tx_exp2((int32_t)(u & 0xFFFFu) * 3 + (13 << 16))) >> 16);   /* a / 8 .. a */
        tx_pulse_start(&s->pul, TX_PULSE_SHAPE_NWAVE, 4u + (dur >> 6), (u & 0x10000u) ? c : (c * 2) / 3, off);
    }
}

/* the next surge of fragments: 0.4 .. 2.5 s (log-uniform), +-15 .. 50 % */
static void surge_draw(fall_t *f)
{
    uint32_t u = tx_rand(&f->srng) >> 16;
    f->sinc = (uint32_t)(((uint64_t)243481u * tx_exp2((16 << 16) - (int32_t)(((uint64_t)u * 173015u) >> 16))) >> 16);
    f->sdep = 9830 + (int32_t)tx_rand_below(&f->srng, 22938u);
}

static void trigger(rock_t *s, const tx_event_t *e)
{
    fall_t *f = 0;
    unsigned i;
    int32_t vl2, lv, lr;
    for (i = 0; i < s->limit && !f; i++) if (!s->f[i].alive) f = &s->f[i];
    if (!f) { s->dropped++; return; }
    vl2 = s->vol_l2 + ((int32_t)e->vel - 100) * 2458 - ((int32_t)e->note - 60) * 5461;   /* velocity / keys: volume */
    lr = (vl2 - 125000) / 3;                                 /* R = (3 V / 4 pi)^(1/3): log2(4 pi / 3) = 2.07 */
    lv = (s->h_l2 + 285000) / 2;                             /* v = sqrt(2 g h): log2(19.62) = 4.29 */
    f->lr_main = lr;
    /* loudness ~ m v x sqrt(share) / r: against 1 m^3, 30 m fall (24.3 m/s), 150 m, share 1e-3; here at the
     * reference distance (the default 128 m): the rest of 1 / r is the output's dist_g, after any hit saturates */
    f->l_main = (int32_t)(log2_q16(A_REF) + (vl2 - 0) + (lv - 301000) + (s->share_l2 + 653000) / 2
                - (DIST_REF_L2 - 473750));
    f->t = 0;
    f->t_imp = 74970u;                                       /* 1.7 s (R2) */
    {   /* sqrt(2 h / g) s -> samples: log2(sqrt(2 / 9.81)) = -1.147 */
        uint32_t tf = tx_exp2(s->h_l2 / 2 - 75170 + 1011124);
        f->t_frag = f->t_imp + 22050u + tf;
    }
    f->t_end = f->t_frag + s->frag_ms * 44u + s->frag_ms / 10u;
    f->next = tx_rand_below(&s->cloud.rng, 4410u);
    f->phase = 0; f->hits = 0; f->alive = 1;
    tx_rng_seed(&f->srng, s->cloud.rng.s ^ 0x5E6E5u);  /* read, not drawn: the fall's other draws stay as they were */
    f->sp = tx_rand(&f->srng); f->sel = 0;
    surge_draw(f);
    (void)lv;
}

/* per block: each fall's events in this block */
static void fall_block(rock_t *s, fall_t *f, unsigned n, int32_t *talus_e)
{
    tx_rng_t *r = &s->cloud.rng;
    int32_t lv = (s->h_l2 + 285000) / 2;
    uint32_t t1 = f->t + n;
    while (f->next < t1) {
        unsigned off = f->next - f->t;
        if (f->next < f->t_imp) {                            /* crack: small sharp fracture pulses */
            hit(s, off, -262144, 300000, f->l_main - (3 << 16) - (int32_t)(tx_rand(r) >> 15));
            f->next += 2205u + (tx_cloud_exp_q8(&s->cloud, 6615u << 8) >> 8);   /* ~ every 0.2 s */
            if (f->next >= f->t_imp) f->next = f->t_imp;
        } else if (f->next < f->t_imp + 44100u) {            /* the impact and its break-up */
            int32_t l = f->hits ? f->l_main - (1 << 16) - (int32_t)(tx_rand(r) >> 14) : f->l_main;
            hit(s, off, f->hits ? f->lr_main - 65536 : f->lr_main, lv, l);
            *talus_e += f->hits ? 0 : 1 << 22;
            f->hits++;
            f->next += f->hits > 5u ? 44100u : 2205u + (tx_cloud_exp_q8(&s->cloud, 6615u << 8) >> 8);
            if (f->next >= f->t_imp + 44100u && f->next < f->t_frag) f->next = f->t_frag;
        } else if (f->next < f->t_frag) {
            f->next = f->t_frag;                             /* free fall: silence */
        } else if (f->next < f->t_end) {                     /* fragments on the talus: rate decaying, in surges */
            uint32_t el = f->next - f->t_frag, span = f->t_end - f->t_frag;
            int32_t decay = 32768 - (int32_t)(((el >> 5) << 15) / ((span >> 5) ? (span >> 5) : 1u));   /* 1 .. 0 */
            int32_t surge;
            {   /* surges, each its own length (0.4 .. 2.5 s) and depth (+-15 .. 50 %): the debris comes in waves.
                 * (A fixed 1 Hz sine was a beat: a line 15-17 dB over the envelope's floor in every fall) */
                uint64_t d = (uint64_t)(el - f->sel) * f->sinc;
                f->sel = el;
                if ((uint64_t)f->sp + d >= 0x100000000ull) { f->sp = (uint32_t)((uint64_t)f->sp + d); surge_draw(f); }
                else f->sp += (uint32_t)d;
                surge = 32768 + (int32_t)(((int64_t)tx_sin_q15(f->sp) * f->sdep) >> 16);
            }
            int32_t sz = tx_cloud_pareto(&s->cloud, s->beta_inv, 9 << 12);   /* volume: up to 9 octaves below (debris) */
            int32_t lfr = f->lr_main - 131072 - sz * 16 / 3;                  /* R ~ V^(1/3) */
            uint32_t gap;
            hit(s, off, lfr, lv - 65536, f->l_main - (2 << 16) - sz * 16);   /* m v: smaller, slower */
            *talus_e += (decay * (surge >> 4)) >> 9;
            gap = 11289600u / (3u + (uint32_t)(((decay >> 8) * (surge >> 8) * 50) >> 14));   /* 3 .. ~80 /s, Q8 samples:
                                                              * the debris tumbling between the big knocks (references) */
            f->next += 1u + (tx_cloud_exp_q8(&s->cloud, gap) >> 8);
        } else {
            f->alive = 0;
            return;
        }
    }
    f->t = t1;
}

TX_AUDIO_CODE static void rock_render(void *st, int32_t *mix, unsigned n)
{
    rock_t *s = (rock_t *)st;
    unsigned k, i, j;
    int32_t *buf = s->buf, *ex = s->ex, te = 0, any = 0;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) buf[j] = ex[j] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;                              /* silent and nothing new: nothing to compute */
    for (i = 0; i < k; i++) trigger(s, &s->ev[i]);
    for (i = 0; i < NF; i++) if (s->f[i].alive) fall_block(s, &s->f[i], n, &te);
    tx_pulses_add(&s->pul, ex, n);
    for (j = 0; j < n; j++) any |= ex[j];
    /* the direct hits (darker with distance below) + the ground's boom */
    {   /* the direct radiation: the force pulses high-passed at ~60 Hz (a one-sided force has no far-field DC) */
        int32_t h = s->dhp;
        for (j = 0; j < n; j++) { int32_t d = ex[j] - h; h += ((d >> 15) * 280) + (((d & 0x7FFF) * 280) >> 15); buf[j] = (ex[j] - h) >> 1; }
        if (!any && h > -64 && h < 64) h = 0;
        s->dhp = h;
    }
    tx_svf_bank_add_g(s->ground, s->gf, 16384 / 3, s->gi, s->gg, NG, ex, buf, n);
    /* the talus rattle: energy from the hits' activity */
    if (te) tx_shaker_shake(&s->talus, te > (1 << 24) ? (1 << 24) : te);
    tx_shaker_objects(&s->talus, 400u << 4, (int32_t)(((int64_t)12000 * s->level) >> 15));
    tx_shaker_add(&s->talus, buf, n);
    /* distance: air absorption (two one-poles), then the far air wave's rumble (R7) */
    {
        int32_t lp1 = s->lp1, lp2 = s->lp2, a = s->lpa >> 2;
        for (j = 0; j < n; j++) {
            int32_t d1 = buf[j] - lp1, d2;
            lp1 += ((d1 >> 13) * a) + (((d1 & 0x1FFF) * a) >> 13);   /* 32-bit split products */
            d2 = lp1 - lp2;
            lp2 += ((d2 >> 13) * a) + (((d2 & 0x1FFF) * a) >> 13);
            buf[j] = lp2;
        }
        if (!any && lp1 > -64 && lp1 < 64 && lp2 > -64 && lp2 < 64 && tx_shaker_quiet(&s->talus)) lp1 = lp2 = 0;
        s->lp1 = lp1; s->lp2 = lp2;
    }
    tx_trim(buf, n, s->dist_g);                              /* 1 / r on everything: the hits, the ground, the talus */
    {   /* far: the rumble, led by the low-passed fall (its mean |x| per block; held ~50 ms: the paths' spread) */
        int32_t g = 0;
        if (s->rum_sh) {
            uint32_t m = 0;
            for (j = 0; j < n; j++) m += (uint32_t)(buf[j] < 0 ? -buf[j] : buf[j]) >> 8;   /* < 2^19 each */
            m = n ? (m / n) << 8 : 0;
            s->rum = (int32_t)m > s->rum ? (int32_t)m : s->rum - (s->rum >> 3);
            if (s->rum < 256) s->rum = 0;
            g = (((s->rum >> 11) * 197) >> 8) * s->rum_sh;   /* / RUM_UNIT (2^11 x 256 / 197); Q15: mean |x| = rum x share */
            if (g > (1 << 19)) g = 1 << 19;
        } else s->rum = 0;
        tx_noise_level(&s->rumble, g);
        tx_noise_add(&s->rumble, buf, n);
    }
    tx_vary_block(&s->vy, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_headroom(buf, n);                                     /* (the biggest falls, nearest: the ground boom saturated) */
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    {
        int32_t z = s->hp.x1 | s->hp.y1 | s->lp1 | s->lp2 | s->dhp | (int32_t)tx_pulses_active(&s->pul) | !tx_shaker_quiet(&s->talus)
                    | !tx_svf_bank_quiet(s->ground, NG) | !tx_noise_quiet(&s->rumble);
        for (i = 0; i < NF; i++) z |= s->f[i].alive;
        s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
    }
}

static void rock_stats(const void *st, tx_stats_t *o)
{
    const rock_t *s = (const rock_t *)st;
    unsigned i, n = 0;
    for (i = 0; i < NF; i++) n += s->f[i].alive;
    o->particles = (uint16_t)n;
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = 0;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: three falls overlapping (impacts, fragments, talus), big and close on hard rock */
static const char SCENE_MAX[] =
    "0 particles 3\n0 param VOLUME 127\n0 param FRAG 127\n0 param GROUND 127\n0 param DIST 0\n0 param HEIGHT 0\n"
    "0 on 53 127\n100 off 53\n1500 on 60 127\n1600 off 60\n3000 on 67 127\n3100 off 67\n"
    "12000 end\n";

const tx_engine_t tx_eng_rockfall = {
    "rockfall", "#34 Rockfall / scree", NP, PARAMS, NF, 2, sizeof(rock_t), SCENE_MAX,
    rock_init, rock_key, rock_param, rock_render, rock_stats,
};
