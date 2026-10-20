/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #5 Surf: waves breaking on a beach .
 *
 * A held key = the sea: breakers arrive every PERIOD (+-20 %); a press sends one breaker now (velocity = its size).
 * Each breaker (ours: the time course; the spectra and the whoomph from L37 / L38):
 *   approach  the wave's roar rises over ~1 s before it plunges;
 *   whoomph   the plunge traps an air tube, a pipe closed at one end: f0 = c / (4 (L + 0.6 R)) (L37: peaks at 34, 48,
 *             78 Hz and harmonics; L 1 m, R 0.25 m -> 75 Hz). L and R grow with the wave: 80 .. 30 Hz over HEIGHT; the
 *             odd harmonics 3 f0, 5 f0 (closed-open); excited by the collapse (a 0.25 s noise burst), plus the jet
 *             hitting the water, a smaller rise near 100 Hz (L37);
 *   roar      the bubble cloud: broadband, flat +-3 dB from 100 to 1600 Hz, falling 5-6 dB / oct above 2 kHz (L38),
 *             every band 40-2500 Hz rising 5-12 dB during a break (L37); the cloud's bubbles burst in clumps (noise in
 *             ~1.5 ms clumps of random gain, plus bubble heroes for the fizz); decays over ~1-3 s with the wave's size;
 *   backwash  the water draining back: a high hiss (2-5 kHz) rising ~1.5 s after the plunge, decaying over ~3 s.
 * Waves vary +-10 dB in level (L37: CHOP). Larger waves have relatively more energy under 100 Hz (L38). DIST: the
 * air's high-cut and 1 / r (ours). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_bubble.h"

#define OUT_TRIM 3072               /* output gain, Q12 (-2.5 dB): integrated (a key held 10 s): -23.5 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

enum { P_PERIOD, P_HEIGHT, P_CHOP, P_BACKWASH, P_DIST, NP };

static const tx_param_t PARAMS[NP] = {
    {"PERIOD", 127, 70, "wave period: 6 .. 15 s (+-20 % wave to wave)"},
    {"HEIGHT", 127, 64, "breaker height 0.3 .. 3 m (log): louder, longer, lower whoomph (80 .. 30 Hz), more rumble"},
    {"CHOP", 127, 64, "wave-to-wave variety: regular .. +-10 dB (L37) and +-40 % timing"},
    {"BACKWASH", 127, 64, "backwash hiss after each wave: none .. strong (water draining through sand or shingle)"},
    {"DIST", 127, 64, "distance: on the shore .. 300 m away (high-cut, quieter)"},
};

#define NBRK 3u                    /* breakers sounding at once */
#define NB 8u                      /* roar bands */
#define NH 16u                     /* bubble heroes */
#define ROAR_REF 4000              /* roar band gain for a 1 m wave at its peak (Q11 units; calibrated by render) */

static const uint16_t BAND_HZ[NB] = { 63, 125, 250, 500, 1000, 2000, 4000, 7000 };
static const int16_t BAND_DB[NB] = { -9, -3, 0, 0, -2, -5, -10, -16 };   /* L38: flat 100-1600 Hz, -5..-6 dB / oct above
                                                                       * 2 kHz; 63 Hz from HEIGHT (L38) */

typedef struct {
    uint32_t t;                    /* samples since this breaker started (the approach) */
    uint32_t t_plunge;             /* the plunge (end of the approach) */
    int32_t a;                     /* its size factor (Q15, 1.0 = HEIGHT's wave) */
    uint8_t alive, kicked, pad[2];
} brk_t;

typedef struct {
    tx_cloud_t cloud;
    tx_pool_t pool;
    tx_declick_t dc;
    tx_dcblock_t hp;
    tx_bubble_t b[TX_POOL_MAX];
    brk_t br[NBRK];
    tx_svf_t roar[NB], pipe[4];
    int32_t rf[NB], rgin[NB], rg[NB], pf[4], pgin[4], pg[4];
    tx_noise_t back;
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t nz[TX_BLOCK], pin[TX_BLOCK], buf[TX_BLOCK];
    uint32_t next;                 /* samples to the next breaker while the sea is on */
    uint32_t x32, burst;           /* noise; samples of the whoomph burst left */
    int32_t burst_a, olp, olpa, ga, h_l2, floor;
    uint8_t p[NP], limit, quiet, sea;
    int32_t level;
    uint16_t dropped;
} surf_t;

static int32_t db_q15(int32_t db) { return (int32_t)tx_exp2((15 << 16) + db * 10885); }   /* 10^(db/20), Q15 */

static void set_pipe(surf_t *s)
{
    /* f0 = c / (4 (L + 0.6 R)): L 1 m, R 0.25 m at a 1 m wave -> 75 Hz; ~ 1 / height^0.4 (ours): 80 .. 30 Hz */
    int32_t f_l2 = 408300 - (int32_t)(((int64_t)s->h_l2 * 26214) >> 16);   /* log2 75 = 6.23, x height^-0.4 */
    uint32_t f0 = tx_exp2(f_l2), i;
    static const uint8_t HARM[3] = { 1, 3, 5 };
    f0 = f0 < 30u ? 30u : f0 > 80u ? 80u : f0;
    for (i = 0; i < 3; i++) {
        uint32_t hz = f0 * HARM[i];
        s->pf[i] = tx_svf_coef(hz);
        s->pgin[i] = (int32_t)tx_svf_gin_noise(hz, 8u * 16u);   /* Q 8 */
        s->pg[i] = i == 0 ? 160 : i == 1 ? 90 : 50;          /* set against the references (their 30-80 Hz
                                                              * peaks sit ~5-10 dB under the roar's band) */
    }
    s->pf[3] = tx_svf_coef(100);                             /* the jet's impact near 100 Hz (Q 8 too: one bank) */
    s->pgin[3] = (int32_t)tx_svf_gin_noise(100, 8u * 16u);
    s->pg[3] = 70;
}

static void set_param(surf_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    if (id == P_HEIGHT) {
        s->h_l2 = -113800 + (int32_t)(v * 1720u);            /* log2(0.3 m) .. log2(3 m) */
        set_pipe(s);
        s->rg[0] = (int32_t)(((int64_t)db_q15(BAND_DB[0] + (int32_t)v * 12 / 127) * ROAR_REF) >> 15);   /* -9 .. +3 dB */
    }
    if (id == P_DIST) {
        uint32_t fc = tx_knob_exp(127u - v, 1500u, 196608);   /* 1.5 .. 12 kHz */
        s->olpa = 32768 - (int32_t)tx_exp2((15 << 16) - (int32_t)(fc * 862u >> 6));
        s->ga = (int32_t)tx_exp2((15 << 16) - (int32_t)v * 1548);   /* -18 dB over the range */
    }
}

static void surf_init(void *st, uint32_t seed)
{
    surf_t *s = (surf_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);
    tx_cloud_rate(&s->cloud, 0);
    tx_noise_init(&s->back, seed ^ 0xBAC4u);
    tx_noise_band(&s->back, 3000, 11, TX_NOISE_BAND);
    tx_noise_tilt(&s->back, 5000);
    s->limit = 8;
    tx_pool_init(&s->pool, NH, s->limit);
    for (i = 0; i < NB; i++) {
        s->rf[i] = tx_svf_coef(BAND_HZ[i]);
        s->rgin[i] = (int32_t)tx_svf_gin_noise(BAND_HZ[i], 22u);   /* Q 1.4: octave bands */
        s->rg[i] = (int32_t)(((int64_t)db_q15(BAND_DB[i]) * ROAR_REF) >> 15);
    }
    s->x32 = seed * 2654435761u + 5u;
    if (!s->x32) s->x32 = 1;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
}

static void surf_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    tx_cloud_key(&((surf_t *)st)->cloud, note, vel, on);
}

static void surf_param(void *st, uint8_t id, uint8_t val)
{
    surf_t *s = (surf_t *)st;
    if (id == TX_PID_PARTICLES) {
        uint32_t cut, i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NH ? NH : val);
        cut = tx_pool_set_limit(&s->pool, s->limit);
        for (i = 0; i < NH; i++) if (cut & (1u << i)) { tx_declick_add(&s->dc, s->b[i].last); s->b[i].env = 0; }
    } else if (id == TX_PID_LEVEL) {
        int32_t nl = tx_level_q15(val > 127u ? 127u : val);
        tx_bubbles_rescale(s->b, &s->pool, s->level, nl);
        s->level = nl;
    } else
        set_param(s, id, val);
}

/* a new breaker: its size from the velocity and CHOP (+-10 dB at most, L37) */
static void launch(surf_t *s, uint32_t vel)
{
    tx_rng_t *r = &s->cloud.rng;
    brk_t *b = 0;
    unsigned i;
    uint32_t oldest = 0;
    int32_t db;
    for (i = 0; i < NBRK; i++) if (!s->br[i].alive) { b = &s->br[i]; break; }
    if (!b) for (i = 0; i < NBRK; i++) if (s->br[i].t >= oldest) { oldest = s->br[i].t; b = &s->br[i]; }
    db = (int32_t)(((int64_t)tx_rand_s15(r) * s->p[P_CHOP]) >> 15) * 10 / 127;   /* +-10 dB */
    b->a = db_q15(db) * (int32_t)vel / 100;                  /* <= 2^17 x 127: 32-bit */
    b->t = 0;
    b->t_plunge = 30000u + tx_rand_below(r, 22000u);         /* the approach: 0.7 .. 1.2 s */
    b->alive = 1; b->kicked = 0;
}

/* a breaker's envelopes at time t (samples): the roar (Q15) and the backwash (Q15) */
static void envs(const surf_t *s, const brk_t *b, int32_t *roar, int32_t *back)
{
    uint32_t t = b->t, tp = b->t_plunge;
    int32_t e;
    uint32_t tau = 26460u + (uint32_t)(((uint64_t)tx_exp2(s->h_l2 + (16 << 16)) * 22050u) >> 16);   /* 0.6 + 0.5 h s: -40 dB
                                                              * ~5 s after a 1 m wave's plunge (note 02: decay ~3-5 s) */
    if (t < tp) {                                            /* the approach: rising (t / tp)^2 to 0.8 */
        int32_t f = (int32_t)((t << 15) / tp);
        e = (int32_t)(((f * f) >> 15) * 26000 >> 15);
    } else {
        uint32_t dt = t - tp, kq = 94548u * 1024u / tau, x = dt > 1000000u ? (15u << 16) + 1u : (dt * kq) >> 10;   /* dt / tau x log2 e, Q16 */
        e = x > (15u << 16) ? 0 : (int32_t)tx_exp2((15 << 16) - (int32_t)x);
    }
    *roar = (int32_t)(((int64_t)e * b->a) >> 15);
    {   /* backwash: from 1.5 s after the plunge, rising 0.8 s, decaying tau 1.5 s */
        int32_t w = 0;
        if (t > tp + 66150u) {
            uint32_t dt = t - tp - 66150u;
            if (dt < 35280u) w = (int32_t)(dt * 32768u / 35280u);
            else {
                uint32_t x = dt > 1000000u ? (15u << 16) + 1u : ((dt - 35280u) * 1463u) >> 10;   /* / 1.5 s x log2 e */
                w = x > (15u << 16) ? 0 : (int32_t)tx_exp2((15 << 16) - (int32_t)x);
            }
        }
        *back = (int32_t)(((int64_t)w * b->a) >> 15);
    }
}

TX_AUDIO_CODE static void surf_render(void *st, int32_t *mix, unsigned n)
{
    surf_t *s = (surf_t *)st;
    tx_rng_t *r = &s->cloud.rng;
    unsigned k, i, j, live = 0;
    int32_t *nz = s->nz, *pin = s->pin, *buf = s->buf, z;
    uint64_t pr = 0, pb = 0;
    uint32_t x32 = s->x32;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;                              /* silent and nothing new */
    for (j = 0; j < n; j++) buf[j] = pin[j] = 0;
    for (i = 0; i < k; i++) launch(s, s->ev[i].vel);         /* a press: a breaker now */
    {   /* the sea: breakers every PERIOD while a key is held */
        unsigned held = tx_cloud_held(&s->cloud);
        if (held && !s->sea) s->next = 6u * 44100u;          /* the press already sent one */
        s->sea = (uint8_t)(held != 0);
        if (s->sea) {
            if (s->next > n) s->next -= n;
            else {
                uint32_t T = 6u * 44100u + s->p[P_PERIOD] * 3125u;   /* 6 .. 15 s */
                int32_t jit = (int32_t)(((int64_t)tx_rand_s15(r) * (6554 + s->p[P_CHOP] * 52)) >> 15);   /* +-20 .. 40 % */
                launch(s, 100u);
                s->next = (uint32_t)((int64_t)T * (32768 + jit) >> 15);
            }
        }
    }
    for (i = 0; i < NBRK; i++) {
        brk_t *b = &s->br[i];
        int32_t er, eb;
        if (!b->alive) continue;
        envs(s, b, &er, &eb);
        if (!b->kicked && b->t + n > b->t_plunge) {          /* the plunge: the whoomph and the jet */
            b->kicked = 1;
            s->burst = 11025u;
            s->burst_a = b->a;
        }
        pr += (uint64_t)((int64_t)er * er);
        pb += (uint64_t)((int64_t)eb * eb);
        b->t += n;
        if (b->kicked && b->t > b->t_plunge + 2u * 44100u && er < (b->a >> 7) && eb < (b->a >> 7)) b->alive = 0;   /* -42 dB */
        live = 1;
    }
    {   /* the roar: white noise in ~1.5 ms clumps of random gain -> 8 octave bands (L38's shape) x the envelope */
        uint32_t e = (uint32_t)tx_exp2((int32_t)((32 << 16) - (int32_t)(tx_neglog2_q12((uint32_t)(pr > 0xFFFFFFFFull ? 0xFFFFFFFFull : pr)) << 4)) / 2);
        int32_t g = (int32_t)(((uint64_t)e * (uint32_t)s->level) >> 15), any = 0;
        int32_t gb[NB];
        if (s->sea || s->floor) {                             /* the far surf along the shore: a floor at -12 dB (ours) */
            s->floor += s->sea ? (8192 - s->floor) >> 6 : -(s->floor >> 6);
            if (!s->sea && s->floor < 64) s->floor = 0;
            pr += (uint64_t)((int64_t)s->floor * s->floor);
            e = (uint32_t)tx_exp2((int32_t)((32 << 16) - (int32_t)(tx_neglog2_q12((uint32_t)(pr > 0xFFFFFFFFull ? 0xFFFFFFFFull : pr)) << 4)) / 2);
            g = (int32_t)(((uint64_t)e * (uint32_t)s->level) >> 15);
        }
        if (!pr) g = 0;
        for (i = 0; i < NB; i++) { gb[i] = (int32_t)(((int64_t)s->rg[i] * g) >> 15); any |= gb[i]; }
        if (any || !tx_svf_bank_quiet(s->roar, NB)) {
            for (j = 0; j < n; j += 64) {
                unsigned q, end = j + 64u < n ? j + 64u : n;
                int32_t cg;
                x32 ^= x32 << 13; x32 ^= x32 >> 17; x32 ^= x32 << 5;
                cg = any ? (int32_t)tx_exp2((int32_t)((x32 >> 16) & 0xFFFFu) * 3 / 2 - 65536 + (12 << 16)) : 0;   /* Q12, 2^(-1 .. 0.5) */
                for (q = j; q < end; q++) {
                    x32 ^= x32 << 13; x32 ^= x32 >> 17; x32 ^= x32 << 5;
                    nz[q] = (((int32_t)(x32 >> 16) - 32768) * cg) >> 7;   /* ~ +-2^20 x the clump's gain: the bank's
                                                              * low bands take ~x 17 inside (unit noise power at Q 1.4) */
                }
            }
            tx_svf_bank_add_g(s->roar, s->rf, 11916, s->rgin, gb, NB, nz, buf, n);   /* q 1 / 1.4 */
        }
        if (!any && tx_svf_bank_quiet(s->roar, NB)) for (i = 0; i < NB; i++) s->roar[i].low = s->roar[i].band = s->roar[i].ef = 0;
        {   /* the cloud's fizz: bubble heroes at ~150 / s at a full wave's peak, 0.5 .. 4 mm, mostly deep */
            uint32_t mu_q8 = (uint32_t)(((uint64_t)e * 225u) >> 15) * n / 256u;   /* expected in this block, Q8 */
            uint32_t want = 0, acc, mean, stolen;
            if (mu_q8) {
                mean = 65536u / mu_q8;
                acc = tx_cloud_exp_q8(&s->cloud, mean);
                while (acc < 256u && want < 8u) { want++; acc += tx_cloud_exp_q8(&s->cloud, mean); }
            }
            for (i = 0; i < want; i++) {
                int32_t a_l2 = -65536 + (int32_t)((tx_rand(r) >> 16) * 3), d_l2 = -(int32_t)(tx_neglog2_q12(tx_rand(r)) * 8 * 16);
                int32_t l = (25 << 16) + a_l2 + d_l2;
                uint32_t env;
                int slot;
                if (l < (17 << 16)) continue;
                env = (uint32_t)(((uint64_t)tx_exp2(l) * (uint32_t)g) >> 15);
                slot = tx_pool_take(&s->pool, env, &stolen);
                if (slot < 0) { s->dropped++; continue; }
                if (stolen) tx_declick_add(&s->dc, s->b[slot].last);
                tx_bubble_start(&s->b[slot], a_l2, d_l2 > -9963 ? 6554u : 0u, env, tx_rand_below(r, n));
            }
        }
    }
    if (s->burst) {                                          /* the whoomph: a decaying noise burst into the pipe */
        int32_t a = (int32_t)(((int64_t)s->burst_a * s->level) >> 15);
        for (j = 0; j < n && s->burst; j++, s->burst--) {
            int32_t env = (int32_t)(((uint64_t)s->burst * s->burst) >> 12) * 2;   /* (t_left / 11025)^2, ~Q15 */
            x32 ^= x32 << 13; x32 ^= x32 >> 17; x32 ^= x32 << 5;
            pin[j] = (int32_t)(((int64_t)(((int32_t)(x32 >> 16) - 32768) * (env >> 3)) * a) >> 23) * 16;   /* -24 dB:
                                                              * the references' 30-80 Hz bands sit under the roar */
        }
    }
    s->x32 = x32 ? x32 : 1u;
    tx_svf_bank_add_g(s->pipe, s->pf, 2048, s->pgin, s->pg, 4, pin, buf, n);
    {   /* the backwash hiss */
        uint32_t e = pb ? (uint32_t)tx_exp2((int32_t)((32 << 16) - (int32_t)(tx_neglog2_q12((uint32_t)(pb > 0xFFFFFFFFull ? 0xFFFFFFFFull : pb)) << 4)) / 2) : 0u;
        int32_t g = (int32_t)(((uint64_t)e * s->p[P_BACKWASH] * 13u) >> 12);
        g = (int32_t)(((int64_t)g * s->level) >> 15);
        tx_noise_roar_add(&s->back, g, buf, n);
    }
    tx_bubbles_render(s->b, &s->pool, &s->dc, buf, n);
    tx_pool_tick(&s->pool);
    tx_declick_run(&s->dc, buf, n);
    {   /* distance: the air's high-cut and 1 / r */
        int32_t lp = s->olp, a = s->olpa, g = s->ga, any = 0;
        for (j = 0; j < n; j++) {
            int32_t d = buf[j] - lp;
            any |= buf[j];
            lp += ((d >> 15) * a) + (((d & 0x7FFF) * a) >> 15);
            buf[j] = ((lp >> 15) * g) + (((lp & 0x7FFF) * g) >> 15);
        }
        if (lp > -64 && lp < 64 && !any) { lp = 0; for (j = 0; j < n; j++) buf[j] = 0; }
        s->olp = lp;
    }
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    z = s->hp.x1 | s->hp.y1 | s->dc.v | (int32_t)s->pool.alive | s->olp | (int32_t)live | (int32_t)s->burst | s->back.gain
        | !tx_svf_bank_quiet(s->roar, NB) | !tx_svf_bank_quiet(s->pipe, 4) | s->sea | s->floor;
    s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !z);
}

static void surf_stats(const void *st, tx_stats_t *o)
{
    const surf_t *s = (const surf_t *)st;
    o->particles = (uint16_t)tx_pool_count(&s->pool);
    o->limit = s->limit;
    o->events = s->cloud.last_events;
    o->dropped = s->cloud.last_dropped;
    o->steals = s->pool.steals;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: three big breakers overlapping, strong backwash, every hero busy, near */
static const char SCENE_MAX[] =
    "0 particles 16\n0 param HEIGHT 127\n0 param BACKWASH 127\n0 param CHOP 127\n0 param DIST 0\n0 param PERIOD 0\n"
    "0 on 53 127\n300 on 60 127\n600 on 67 127\n1500 on 72 127\n2500 on 79 127\n"
    "4000 off 53\n4000 off 60\n4000 off 67\n4000 off 72\n4000 off 79\n"
    "5000 end\n";

const tx_engine_t tx_eng_surf = {
    "surf", "#5 Surf / breaking waves", NP, PARAMS, NH, 8, sizeof(surf_t), SCENE_MAX,
    surf_init, surf_key, surf_param, surf_render, surf_stats,
};
