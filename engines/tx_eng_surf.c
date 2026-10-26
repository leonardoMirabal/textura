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
 * air's high-cut and 1 / r (ours). FLOOR: the far surf along the shore, a steady roar under the waves while the sea is
 * on (ours: -12 dB; 0 for a beach heard alone). STRIKE 0: a press sends no breaker; the sea it holds starts partway
 * into a wave period (its first breaker 0.1 .. 0.6 periods on). COUNT: wave trains along the shore, each with
 * its own timing (random phase, the same period and jitter) and size (farther: -1 .. -7 dB): more breakers.
 *
 * WATER 1, a lake (ours, against recordings of lake shores and of a moored hull): short wind waves (PERIOD 0.6 .. 4 s
 * on average, no period kept: each train's waves at random; trains farther -14 .. -1 dB), in groups (a slow walk of
 * their size, +-5 dB). No
 * approach, roar, whoomph or backwash: each wave is a burst of small splashes, the 4 bands' noise each its own (its
 * own clumps, heavy-tailed: a few loud), each wave its own tilt and the bands' own lags (the run-up's body first, the
 * foam fizzing on up to 1 s after). FACE: a sloping shore (the wash rises 0.15 .. 0.6 s, decays 0.5 s, flat 250 Hz -
 * 8 kHz, the fizz's bubbles in it) .. a hull or a jetty (a slap: rising in 2 .. 8 ms, its highs gone in ~25 ms, its
 * lows on ~0.2 s, then the water sloshing dark under it, -16 dB, 0.6 s). Air caught at the face rings as a few plops
 * (2 .. 10 mm, 330 .. 1600 Hz), the trough's gurgle after them. The recordings: shore washes 0.13 .. 1 /s, flat; a
 * hull's slaps ~2 /s, -20 dB in 270 ms, centroid ~850 Hz. */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_noise.h"
#include "tx_core_bubble.h"

#define OUT_TRIM 4096               /* output gain, Q12 (-2.5 dB): integrated (a key held 10 s): -23.5 LUFS at LEVEL 100 -> target -26 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM +2.5 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 2

enum { P_PERIOD, P_HEIGHT, P_CHOP, P_BACKWASH, P_DIST, P_FLOOR, P_STRIKE, P_COUNT, P_SHORE, P_WATER, P_FACE, NP };

static const tx_param_t PARAMS[NP] = {
    {"PERIOD", 127, 70, "wave period: 6 .. 15 s (+-20 % wave to wave; a lake: 0.6 .. 4 s on average, at random)"},
    {"HEIGHT", 127, 64, "breaker height 0.3 .. 3 m (log): louder (-5 .. +5 dB), longer, lower whoomph (80 .. 30 Hz), more rumble"},
    {"CHOP", 127, 64, "wave-to-wave variety: regular .. +-10 dB (L37) and +-40 % timing (a lake: its sizes and groups; its timing is always at random)"},
    {"BACKWASH", 127, 64, "the backwash after each wave: none .. -8 dB under the break (64) .. as loud (water draining back through sand or shingle)"},
    {"DIST", 127, 64, "distance: on the shore .. 300 m away (high-cut, quieter)"},
    {"FLOOR", 127, 64, "the far surf along the shore under the waves while the sea is on: none .. -12 dB (64) .. -6 dB"},
    {"STRIKE", 1, 1, "a press: 1 = a breaker now; 0 = presses only hold the sea, which starts partway into a wave period"},
    {"COUNT", 8, 1, "wave trains along the shore, each its own timing and size (farther ones smaller): 1 .. 8, more breakers"},
    {"SHORE", 127, 0, "the beach: sand (the backwash a soft hiss) .. shingle (the stones dragged back rattle and clatter)"},
    {"WATER", 1, 0, "0 = the sea's breakers; 1 = a lake: short wind waves lapping, in groups (no breakers, no backwash)"},
    {"FACE", 127, 0, "a lake's waves meet: a sloping shore (soft washes, plops) .. a hull or a jetty (dark short slaps, gurgles)"},
};

#define NBRK 3u                    /* breakers sounding at once (one wave train) */
#define NBRK_MAX 10u               /* .. with more trains (COUNT): 3 + 1 a train past the first */
#define NTR 8u                     /* wave trains at most (COUNT) */
#define NB 4u                      /* roar bands (CPU: 4 wide bands fitted to the 8 octave bands' sum) */
#define NH 16u                     /* bubble heroes */
#define ROAR_REF 4000              /* roar band gain for a 1 m wave at its peak (Q11 units; calibrated by render) */

/* the roar's spectrum (L38): flat 100-1600 Hz, -5..-6 dB / oct above 2 kHz, the 63 Hz band from HEIGHT (L38); first
 * as 8 octave bands (Q 1.4) of -9 (HEIGHT) -3 0 0 -2 -5 -10 -16 dB at 63 .. 7000 Hz, now as 4 two-octave bands at
 * Q 0.9 whose gains (least squares on the complex sum, the same noise in) give the same response within 2 dB per
 * third-octave (1 dB rms) for half the cost */
static const uint16_t BAND_HZ[NB] = { 63, 250, 1000, 4000 };
static const uint16_t BAND_G[NB] = { 4559, 7160, 5288, 1790 };   /* Q12 (1.113 1.748 1.291 0.437); the 63 Hz one at
                                                                  * HEIGHT 64, +-6 dB over HEIGHT */
#define ROAR_Q 18204                /* 16384 / 0.9 */
#define ROAR_GIN_Q16 14u            /* Q x 16 for the noise gain */

typedef struct {
    uint32_t t;                    /* samples since this breaker started (the approach) */
    uint32_t t_plunge;             /* the plunge (end of the approach) */
    int32_t a;                     /* its size factor (Q15, 1.0 = HEIGHT's wave) */
    uint8_t alive, kicked, pad[2];
    uint16_t wb[NB];               /* a lake's wave: its own spectrum, the roar bands' weights (Q12): each runs up or
                                    * slaps its own way, so the bands do not move as one */
    uint16_t lag[NB];              /* .. and each band's own lag (samples): the body first, the foam's fizz after */
} brk_t;

typedef struct {
    tx_cloud_t cloud;
    tx_pool_t pool;
    tx_declick_t dc;
    tx_dcblock_t hp;
    tx_bubble_t b[TX_POOL_MAX];
    brk_t br[NBRK_MAX];
    tx_svf_t roar[NB], pipe[4];
    int32_t rf[NB], rgin[NB], rg[NB], pf[4], pgin[4], pg[4];
    tx_noise_t back;
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t nz[TX_BLOCK], pin[TX_BLOCK], buf[TX_BLOCK];
    uint32_t next;                 /* samples to the next breaker while the sea is on */
    uint32_t tnext[NTR];           /* the other trains' next breakers (0: not running) */
    uint32_t launched;             /* breakers so far (stats) */
    uint8_t tvel[NTR];             /* the trains' sizes (velocity) */
    uint32_t x32, burst;           /* noise; samples of the whoomph burst left */
    int32_t burst_a, olp, olpa, ga, h_l2, floor;
    int32_t hg;                    /* HEIGHT's level, Q15 (1 m: 1) */
    int32_t rt, rt_x1, rt_y;       /* the shingle's rattle: its click envelope, high-pass and low-pass states */
    uint32_t xb[NB];               /* lake: each band's own noise (its own splashes, not the others') */
    int32_t nlp[NB];               /* .. low-passed (3.2 x the band's centre: the band's skirt 12 dB / oct above, the
                                    * slosh under a hull stays dark) */
    int32_t grp;                   /* lake: the waves' group level (tenths of a dB), a slow walk wave to wave */
    uint32_t fz;                   /* the fizz's rate, Q8 of the sea's */
    uint8_t p[NP], limit, quiet, sea;
    int32_t level;
    uint16_t dropped;
    tx_vary_t vy;
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

/* the roar bands' gains: the sea's (HEIGHT sets the 63 Hz one), or a lake's wash (FACE: a shore's flat 250 Hz - 8 kHz
 * .. a hull's dark slap, 125-500 Hz; a lake's 63 Hz band is not run: the references' lows under 100 Hz are their wind
 * and their place, and it cost a fifth of the lake for 0.02 of texture distance) */
static const uint16_t LAKE_G[2][NB] = { { 0, 4400, 5300, 2800 }, { 0, 20700, 8050, 10350 } };
static const uint16_t LAG[2][NB] = { { 0, 8820, 26460, 44100 }, { 0, 0, 0, 0 } };   /* the bands' lags at most
                                                              * (samples): a shore's foam fizzes on after the run-up, the
                                                              * water drips off a hull after the slap */
static void set_bands(surf_t *s)
{
    unsigned i;
    if (!s->p[P_WATER]) {
        for (i = 1; i < NB; i++) s->rg[i] = (ROAR_REF * (int32_t)BAND_G[i]) >> 12;
        s->rg[0] = (int32_t)(((int64_t)db_q15((int32_t)s->p[P_HEIGHT] * 12 / 127 - 6) * ROAR_REF * BAND_G[0]) >> 27);   /* -6 .. +6 dB */
        s->fz = 256u;
        return;
    }
    for (i = 0; i < NB; i++) {
        int32_t f = s->p[P_FACE], g = ((int32_t)LAKE_G[0][i] * (127 - f) + (int32_t)LAKE_G[1][i] * f) / 127;
        s->rg[i] = (ROAR_REF * g) >> 12;
    }
    s->fz = 480u - (uint32_t)s->p[P_FACE] * 480u / 127u;     /* a shore's wash fizzes; under a hull none */
}

static void set_param(surf_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    if (id == P_HEIGHT) {
        s->h_l2 = -113800 + (int32_t)(v * 1720u);            /* log2(0.3 m) .. log2(3 m) */
        set_pipe(s);
        s->hg = (int32_t)tx_exp2((15 << 16) + ((int32_t)v - 64) * 860);   /* the break's level ~ sqrt(height): +-5 dB (ours:
                                                              * a 10 x higher wave plays 10 dB louder, not 20: playable) */
    }
    if (id == P_HEIGHT || id == P_WATER || id == P_FACE) set_bands(s);
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
    tx_noise_band(&s->back, 2000, 11, TX_NOISE_BAND);   /* (2 kHz: the references fall from 3 kHz up) */
    tx_noise_tilt(&s->back, 5000);
    s->limit = 8;
    tx_pool_init(&s->pool, NH, s->limit);
    for (i = 0; i < NB; i++) {
        s->rf[i] = tx_svf_coef(BAND_HZ[i]);
        s->rgin[i] = (int32_t)tx_svf_gin_noise(BAND_HZ[i], ROAR_GIN_Q16);
    }
    s->x32 = seed * 2654435761u + 5u;
    if (!s->x32) s->x32 = 1;
    for (i = 0; i < NB; i++) s->xb[i] = (s->x32 ^ (0x9E3779B9u * (i + 1u))) | 1u;
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((surf_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void surf_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((surf_t *)st)->vy);
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
    unsigned i, nb = NBRK + (s->p[P_COUNT] > 1u ? s->p[P_COUNT] - 1u : 0u);
    uint32_t oldest = 0;
    int32_t db;
    if (nb > NBRK_MAX) nb = NBRK_MAX;
    s->launched++;
    for (i = 0; i < nb; i++) if (!s->br[i].alive) { b = &s->br[i]; break; }
    if (!b) for (i = 0; i < nb; i++) if (s->br[i].t >= oldest) { oldest = s->br[i].t; b = &s->br[i]; }
    db = (int32_t)(((int64_t)tx_rand_s15(r) * s->p[P_CHOP]) >> 15) * 10 / 127;   /* +-10 dB */
    if (s->p[P_WATER]) {                                     /* a lake: the waves come in groups and lulls (a slow walk,
                                                              * ~+-5 dB at CHOP 64, the group ~4 waves) */
        s->grp = s->grp * 3 / 4 + (int32_t)(((int64_t)tx_rand_s15(r) * (20 + s->p[P_CHOP])) >> 15);
        s->grp = s->grp > 120 ? 120 : s->grp < -120 ? -120 : s->grp;   /* (+-12 dB at most) */
        db += s->grp / 10;
    }
    b->a = db_q15(db) * (int32_t)vel / 100;                  /* <= 2^17 x 127: 32-bit */
    b->a = (int32_t)(((int64_t)b->a * s->hg) >> 15);         /* (HEIGHT: <= x 1.8) */
    b->t = 0;
    if (s->p[P_WATER]) {                                     /* a lake's wave: its tilt +-6 dB a band (two octaves), at
                                                              * random; it runs up 0.15 .. 0.6 s on a shore, a hull's */
        int32_t tl = (int32_t)(((int64_t)tx_rand_s15(r) * 60) >> 15);
        for (i = 0; i < NB; i++) {
            int32_t d = tl * (2 * (int32_t)i - 3) / 3 + (int32_t)(((int64_t)tx_rand_s15(r) * 120) >> 15);
            b->wb[i] = (uint16_t)(db_q15(d / 10) >> 3);
            b->lag[i] = (uint16_t)tx_rand_below(r, LAG[s->p[P_FACE] > 63u][i] + 1u);
        }
    }
    if (s->p[P_WATER]) {
        uint32_t f = s->p[P_FACE];                           /* slap 2 .. 8 ms */
        b->t_plunge = ((6600u + tx_rand_below(r, 19800u)) * (127u - f) + (90u + tx_rand_below(r, 260u)) * f) / 127u;
    } else
        b->t_plunge = 30000u + tx_rand_below(r, 22000u);     /* the approach: 0.7 .. 1.2 s */
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
    {   /* backwash: from 1.5 s after the plunge, rising 0.8 s, decaying tau 1.5 s (sand) .. 3 s (shingle: the
         * stones go on rattling as the water drains down through them; ours, against the references) */
        int32_t w = 0;
        if (t > tp + 66150u) {
            uint32_t dt = t - tp - 66150u;
            if (dt < 35280u) w = (int32_t)(dt * 32768u / 35280u);
            else {
                uint32_t kb = 1463u * 127u / (127u + (uint32_t)s->p[P_SHORE]);   /* / 1.5 .. 3 s x log2 e (3.5 s outlived
                                                             * a tap's 20 s) */
                uint32_t x = dt > 1000000u ? (15u << 16) + 1u : ((dt - 35280u) * kb) >> 10;
                w = x > (15u << 16) ? 0 : (int32_t)tx_exp2((15 << 16) - (int32_t)x);
            }
        }
        *back = (int32_t)(((int64_t)w * b->a) >> 15);
    }
}

/* a lake's wave meeting the face: air caught as it folds or slaps, ringing as a few bubbles (f0 = 3.28 m/s / a), then
 * the trough's gurgle, a short chain ~70 ms apart at random (ours): a shore 2 .. 5 mm (650 .. 1600 Hz), a hull 3 .. 10 mm
 * (330 .. 1100 Hz), more of them under a hull */
static void plops(surf_t *s, const brk_t *b)
{
    tx_rng_t *r = &s->cloud.rng;
    uint32_t f = s->p[P_FACE], k = tx_rand_below(r, 2u + f * 2u / 127u), w = tx_rand_below(r, 2000u), stolen;
    int32_t a = (int32_t)(((int64_t)b->a * s->level) >> 15);
    for (; k; k--) {
        int32_t a_l2 = 65536 + (int32_t)(f * 302u) + (int32_t)(((tx_rand(r) >> 16) * (86500u + f * 216u)) >> 16);
        uint32_t env;
        int slot;
        env = (uint32_t)(((uint64_t)tx_exp2((24 << 16) + a_l2 * 3 / 2 - (int32_t)(tx_rand(r) >> 16) * 3) * (uint32_t)a) >> 15);
        slot = tx_pool_take(&s->pool, env, &stolen);
        if (slot < 0) { s->dropped++; continue; }
        if (stolen) tx_declick_add(&s->dc, s->b[slot].last);
        tx_bubble_start(&s->b[slot], a_l2, 3277u, env, w);
        w += 440u + ((tx_neglog2_q12(tx_rand(r) | 1u) * 1834u) >> 12);   /* 10 ms + exponential, mean 60 ms: the
                                                              * gurgle's pops clustered, not in a row */
    }
}

/* a lake's wave in roar band i at time t (samples): its run-up (t / tp)^2, then its decay: a shore's wash tau 0.5 s;
 * a hull's slap 0.12 s, its highs gone in ~25 ms and its lows ringing on, then the water sloshing under it (-16 dB, tau
 * 0.6 s, in the low bands) */
static const uint16_t KT_HULL[NB] = { 384, 256, 64, 24 };   /* a hull's decay per band, x / 256 */
static const uint16_t WS_HULL[NB] = { 256, 192, 26, 0 };   /* .. its slosh per band, x / 256 */
static int32_t lake_env(const surf_t *s, const brk_t *b, uint32_t t, unsigned i)
{
    uint32_t tp = b->t_plunge, f = s->p[P_FACE], tau, kq, dt;
    int32_t e;
    if (t < tp) {
        int32_t q = (int32_t)((t << 15) / tp);
        return (int32_t)(((int64_t)((q * q) >> 15) * b->a) >> 15);
    }
    tau = (22050u * (127u - f) + 5200u * f) / 127u;
    tau = tau * ((256u * (127u - f) + KT_HULL[i] * f) / 127u) >> 8;
    kq = 94548u * 1024u / tau;                               /* 1 / tau x log2 e, Q26 */
    dt = t - tp;
    e = dt >= 1006632960u / kq ? 0 : (int32_t)tx_exp2((15 << 16) - (int32_t)((dt * kq) >> 10));
    if (f) {
        int32_t ws = (int32_t)((f * 41u * WS_HULL[i]) >> 8);
        uint32_t xs = dt > 200000u ? (15u << 16) + 1u : (dt * 3574u) >> 10;
        e = (int32_t)(((int64_t)e * (32768 - ws)) >> 15) + (xs > (15u << 16) ? 0 : (int32_t)(((int64_t)tx_exp2((15 << 16) - (int32_t)xs) * ws) >> 15));
    }
    return (int32_t)(((int64_t)e * b->a) >> 15);
}

/* the time to a train's next wave: the period +-jit. A lake's short wind waves keep no period: each train's next
 * after T E, E exponential (mean 1, at most 8): Poisson, its trains' arrivals together IOI CV ~1, never a beat (the
 * period +-80 % gave 0.8 .. 0.9 with 8 trains; lulls of the wind, every train put off together, cut silences the
 * references never have). Their groups are in their sizes (launch) */
static uint32_t gap(surf_t *s, uint32_t T, int32_t jit)
{
    if (s->p[P_WATER]) {
        uint32_t e = (tx_neglog2_q12(tx_rand(&s->cloud.rng) | 1u) * 2839u) >> 12;   /* E, Q12 (x ln 2) */
        if (e > 8u * 4096u) e = 8u * 4096u;
        return (uint32_t)(((uint64_t)T * e) >> 12);
    }
    return (uint32_t)((int64_t)T * (32768 + jit) >> 15);
}

TX_AUDIO_CODE static void surf_render(void *st, int32_t *mix, unsigned n)
{
    surf_t *s = (surf_t *)st;
    tx_rng_t *r = &s->cloud.rng;
    unsigned k, i, j, live = 0;
    int32_t *nz = s->nz, *pin = s->pin, *buf = s->buf, z;
    uint64_t pr = 0, pb = 0, pq[NB] = { 0, 0, 0, 0 };
    uint32_t x32 = s->x32;
    uint16_t d0 = s->dropped;
    if (n > TX_BLOCK) n = TX_BLOCK;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    if (!k && s->quiet) return;                              /* silent and nothing new */
    for (j = 0; j < n; j++) buf[j] = pin[j] = 0;
    for (i = 0; i < k; i++) {                                /* a press: a breaker now */
        if (s->p[P_STRIKE]) launch(s, s->ev[i].vel);         /* (STRIKE 0: presses hold the sea only) */
    }
    {   /* the sea: breakers every PERIOD while a key is held */
        unsigned held = tx_cloud_held(&s->cloud);
        uint32_t T = s->p[P_WATER] ? 26460u + s->p[P_PERIOD] * 1181u   /* a lake: 0.6 .. 4 s */
                                   : 6u * 44100u + s->p[P_PERIOD] * 3125u;   /* 6 .. 15 s */
        if (held && !s->sea)                                 /* the press already sent one; or (STRIKE 0) the sea */
            s->next = s->p[P_STRIKE] ? 6u * 44100u : T / 10u + tx_rand_below(r, T / 2u);   /* is partway into a period */
        s->sea = (uint8_t)(held != 0);
        if (s->sea) {
            if (s->next > n) s->next -= n;
            else {
                int32_t jit = (int32_t)(((int64_t)tx_rand_s15(r) * (6554 + s->p[P_CHOP] * 52)) >> 15);   /* +-20 .. 40 % */
                launch(s, 100u);
                s->next = gap(s, T, jit);
            }
            for (i = 1; i < NTR; i++) {                      /* the other trains (COUNT): along the shore, each its */
                if (i >= s->p[P_COUNT]) { s->tnext[i] = 0; continue; }   /* own timing, farther ones smaller */
                if (!s->tnext[i]) {                          /* joining: partway into its period */
                    s->tnext[i] = 1u + tx_rand_below(r, T);
                    s->tvel[i] = (uint8_t)(s->p[P_WATER] ? 20u + tx_rand_below(r, 71u)   /* (a lake: 20 .. 90, -14 .. -1 dB) */
                                                  : 45u + tx_rand_below(r, 46u));   /* 45 .. 90: -7 .. -1 dB */
                }
                if (s->tnext[i] > n) s->tnext[i] -= n;
                else {
                    int32_t jit = (int32_t)(((int64_t)tx_rand_s15(r) * (6554 + s->p[P_CHOP] * 52)) >> 15);
                    launch(s, s->tvel[i]);
                    s->tnext[i] = gap(s, T, jit);
                    if (!s->tnext[i]) s->tnext[i] = 1u;
                }
            }
        } else for (i = 1; i < NTR; i++) s->tnext[i] = 0;
    }
    for (i = 0; i < NBRK_MAX; i++) {
        brk_t *b = &s->br[i];
        int32_t er, eb;
        if (!b->alive) continue;
        if (s->p[P_WATER]) { er = lake_env(s, b, b->t, 1); eb = 0; }
        else envs(s, b, &er, &eb);
        if (!b->kicked && b->t + n > b->t_plunge) {          /* the plunge: the whoomph and the jet */
            b->kicked = 1;
            if (!s->p[P_WATER]) { s->burst = 11025u; s->burst_a = b->a; }
            else plops(s, b);                                /* a lake's wave: air caught at the face, plops and gurgles */
        }
        pr += (uint64_t)((int64_t)er * er);
        if (s->p[P_WATER]) for (j = 1; j < NB; j++) {
            int32_t ej = lake_env(s, b, b->t > b->lag[j] ? b->t - b->lag[j] : 0u, j);
            int64_t w = ((int64_t)ej * b->wb[j]) >> 17;         /* (/ 32: the sum stays in 32 bits) */
            pq[j] += (uint64_t)(w * w);
            if (ej > er) er = ej;                            /* (it ends when its last band has: the lagged foam) */
        }
        pb += (uint64_t)((int64_t)eb * eb);
        b->t += n;
        if (b->kicked && b->t > b->t_plunge + (s->p[P_WATER] ? 35280u : 2u * 44100u) && er < (b->a >> 7) && eb < (b->a >> 7)) b->alive = 0;   /* -42 dB */
        live = 1;
    }
    {   /* the roar: white noise in ~1.5 ms clumps of random gain -> 8 octave bands (L38's shape) x the envelope */
        uint32_t e = (uint32_t)tx_exp2((int32_t)((32 << 16) - (int32_t)(tx_neglog2_q12((uint32_t)(pr > 0xFFFFFFFFull ? 0xFFFFFFFFull : pr)) << 4)) / 2);
        int32_t g = (int32_t)(((uint64_t)e * (uint32_t)s->level) >> 15), any = 0;
        int32_t gb[NB];
        if (!s->p[P_WATER] && (s->sea || s->floor)) {       /* the far surf along the shore: a floor at -12 dB (ours; a
                                                              * lake: none) */
            s->floor += s->sea ? ((int32_t)s->p[P_FLOOR] * 128 - s->floor) >> 6 : -(s->floor >> 6);   /* (FLOOR 64: 8192) */
            if (!s->sea && s->floor < 64) s->floor = 0;
            pr += (uint64_t)((int64_t)s->floor * s->floor);
            e = (uint32_t)tx_exp2((int32_t)((32 << 16) - (int32_t)(tx_neglog2_q12((uint32_t)(pr > 0xFFFFFFFFull ? 0xFFFFFFFFull : pr)) << 4)) / 2);
            g = (int32_t)(((uint64_t)e * (uint32_t)s->level) >> 15);
        }
        if (!pr) g = 0;
        for (i = 0; i < NB; i++) {
            int32_t gi = g;
            if (s->p[P_WATER]) {                             /* a lake: each band its waves' own sum */
                uint32_t ei = pq[i] ? (uint32_t)tx_exp2((int32_t)((32 << 16) - (int32_t)(tx_neglog2_q12((uint32_t)(pq[i] > 0xFFFFFFFFull ? 0xFFFFFFFFull : pq[i])) << 4)) / 2) : 0u;
                gi = (int32_t)(((uint64_t)ei * (uint32_t)s->level) >> 10);
            }
            gb[i] = (int32_t)(((int64_t)s->rg[i] * gi) >> 15); any |= gb[i];
        }
        if (s->p[P_WATER] && (any || !tx_svf_bank_quiet(s->roar, NB))) {
            /* a lake: each band its own noise in clumps of its own (the lower band's longer: 5.8 .. 0.7 ms), their gains
             * heavy-tailed, 2^(3 u^3 - 1.75): mostly faint, a few loud. The wash is many small splashes, each band's its
             * own, not one roar */
            static const uint16_t CL[NB] = { 512, 256, 64, 32 };
            static const int32_t CD[NB] = { 196608, 196608, 196608, 196608 };   /* the clumps' spread, octaves (Q16) at u^3 */
            static const int32_t LPA[NB] = { 0, 3730, 14000, 0 };   /* the noise's low-pass: 2 pi fc / fs, Q15 (800,
                                                              * 3200 Hz; the top band none) */
            for (i = 1; i < NB; i++) {                     /* (the 63 Hz band not run: LAKE_G) */
                uint32_t xb = s->xb[i];
                int32_t y = s->nlp[i], la = LPA[i];
                for (j = 0; j < n; j += CL[i]) {
                    unsigned q, end = j + CL[i] < n ? j + CL[i] : n;
                    int32_t cg;
                    xb ^= xb << 13; xb ^= xb >> 17; xb ^= xb << 5;
                    {   uint32_t u = xb >> 16, u3 = ((u * u) >> 16) * u >> 16;   /* (u^3: mostly faint, a few loud) */
                        int32_t l2 = (int32_t)(((int64_t)u3 * CD[i]) >> 16) - CD[i] / 4 - 65536;
                        cg = gb[i] ? (int32_t)tx_exp2(l2 + (12 << 16)) : 0; }   /* Q12 */
                    if (la) for (q = j; q < end; q++) {          /* (low-passed) */
                        xb ^= xb << 13; xb ^= xb >> 17; xb ^= xb << 5;
                        y += ((((((int32_t)(xb >> 16) - 32768) * cg) >> 7) - y) >> 8) * la >> 7;
                        nz[q] = y;
                    } else for (q = j; q < end; q++) {
                        xb ^= xb << 13; xb ^= xb >> 17; xb ^= xb << 5;
                        nz[q] = (((int32_t)(xb >> 16) - 32768) * cg) >> 7;
                    }
                }
                s->xb[i] = xb;
                s->nlp[i] = gb[i] || y <= -256 || y >= 256 ? y : 0;   /* (the shifts' floor would hold a small offset
                                                              * for ever: the band never quiet) */
                tx_svf_bank_add_g(&s->roar[i], &s->rf[i], ROAR_Q, &s->rgin[i], &gb[i], 1, nz, buf, n);
            }
        } else if (any || !tx_svf_bank_quiet(s->roar, NB)) {
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
            tx_svf_bank_add_g(s->roar, s->rf, ROAR_Q, s->rgin, gb, NB, nz, buf, n);
        }
        if (!any && tx_svf_bank_quiet(s->roar, NB)) for (i = 0; i < NB; i++) s->roar[i].low = s->roar[i].band = s->roar[i].ef = 0;
        {   /* the cloud's fizz: bubble heroes at ~150 / s at a full wave's peak, 0.5 .. 4 mm, mostly deep */
            uint32_t mu_q8 = (uint32_t)(((uint64_t)e * 225u) >> 15) * n / 256u * s->fz >> 8;   /* expected in this block, Q8 */
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
    if (!s->p[P_WATER] || !tx_svf_bank_quiet(s->pipe, 4)) tx_svf_bank_add_g(s->pipe, s->pf, 2048, s->pgin, s->pg, 4, pin, buf, n);
    {   /* the backwash hiss */
        uint32_t e = pb ? (uint32_t)tx_exp2((int32_t)((32 << 16) - (int32_t)(tx_neglog2_q12((uint32_t)(pb > 0xFFFFFFFFull ? 0xFFFFFFFFull : pb)) << 4)) / 2) : 0u;
        int32_t g = (int32_t)(((uint64_t)e * s->p[P_BACKWASH] * 13u) >> 9);   /* (x 8: the backwash was 20 dB under the
                                                              * break, inaudible; now -8 dB at 64, by render) */
        int32_t sh = s->p[P_SHORE];
        g = (int32_t)(((int64_t)g * s->level) >> 15);
        tx_noise_roar_add(&s->back, (int32_t)(((int64_t)g * (160 - sh)) >> 7), buf, n);   /* (shingle: less hiss, */
        if (sh && (e || s->rt)) {                            /* the rattle instead) */
            /* the shingle: stones dragged down the beach knock each other, a dense clatter of short bright clicks
             * (~4000 /s at a full backwash, sizes u^2: most faint, a few loud; ours, against the references) */
            int32_t rt = s->rt, x1 = s->rt_x1, y = s->rt_y;
            uint32_t pc = (uint32_t)(((uint64_t)e * (uint32_t)sh) >> 9);   /* the chance per sample, Q16 (<= ~10 %) */
            int32_t ca = g * sh * 2;                     /* (the loudest knock ~-6 dBFS at BACKWASH 127) */
            for (j = 0; j < n; j++) {
                int32_t x;
                x32 ^= x32 << 13; x32 ^= x32 >> 17; x32 ^= x32 << 5;
                if ((x32 & 0xFFFFu) < pc) {                  /* a knock: its size u^2 (most faint) */
                    uint32_t u = x32 >> 22;                  /* 0 .. 1023 */
                    rt += (int32_t)(((int64_t)ca * (int32_t)(u * u >> 10)) >> 10);
                }
                if (!rt) continue;
                x = (int32_t)((x32 >> 16) & 0x7FFFu) - 16384;   /* (the same draw's upper half: one a sample) */
                x1 += (x - x1) >> 4;                         /* a stone's knock: noise high-passed (~0.4 kHz) and */
                y += (x - x1 - y) >> 2;                      /* low-passed (~1.8 kHz): the references' shingle peaks at
                                                              * 0.5-1.3 kHz and falls above 3 kHz */
                buf[j] += (int32_t)(((int64_t)y * rt) >> 15);
                rt -= (rt >> 6) + 1;                         /* tau ~1.4 ms */
                if (rt < 0) rt = 0;
            }
            s->rt = rt; s->rt_x1 = rt ? x1 : 0; s->rt_y = rt ? y : 0;
        } else s->rt = s->rt_x1 = s->rt_y = 0;
    }
    s->x32 = x32 ? x32 : 1u;
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
    tx_vary_block(&s->vy, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->cloud.last_dropped = (uint16_t)(s->cloud.last_dropped + (uint16_t)(s->dropped - d0));
    z = s->hp.x1 | s->hp.y1 | s->dc.v | (int32_t)s->pool.alive | s->olp | (int32_t)live | (int32_t)s->burst | s->back.gain
        | !tx_svf_bank_quiet(s->roar, NB) | !tx_svf_bank_quiet(s->pipe, 4) | s->sea | s->floor | s->rt;
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
    o->total = s->launched;                                  /* (the breakers: the presses' and the sea's) */
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
