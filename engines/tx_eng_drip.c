/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura #1 Drip: water drops falling into a pool, each one a weak impact click and then the "plink" of the air
 * bubble it traps.
 *
 * A key press lets one drop fall; a held key keeps dripping at RATE, at random (a cave drip, Poisson) or in runs
 * and quiet spells (CLUMP; never a regular beat, the user's rule). Each key is a bubble size: the radius at C4 is
 * SIZE, one semitone higher = a 2^(1/12) smaller bubble
 * (pitch f0 = 3.28 m/s / a), SCATTER varies it drop to drop. Per drop:
 *   impact   a zero-mean force pulse (PULSE, N-wave), length ~0.3 D / U, level ~ U^2 (U = impact speed);
 *   bubble   after the crater collapses (25 ms at 1.4 m/s, 10 ms at 2.36 m/s: t ~ U^-1.75), with probability ~0.9
 *            in the regular-entrainment range (ours: falling off below 1 m/s); a BUBBLE hero, amplitude ~ a^1.5 x U;
 *            near the surface it rises (xi 0.05-0.15, cap 2 f0), deep ones (DEPTH) hold their pitch and are quieter;
 *   second   fast drops sometimes pinch off a second, bigger bubble (ours: irregular entrainment, P up to 0.4);
 *   splash   the crown and the droplets it throws falling back: a burst of high noise (3 .. 10 kHz, 6 .. 12 ms), ~U^2 like
 *            the impact (the references: 5 .. 12 kHz within 10 dB of the plink; ours had none, 20 .. 30 dB down).
 * CAVE: the space around the pool, a diffuse tail (a 4-line feedback delay network at half rate, 20 .. 37 ms lines:
 * a chamber of ~10 m, T60 0.4 .. 4 s; the walls' loss a low-pass in each line, the rock's scatter an allpass).
 * 0 = dry, the open air.
 * U from the fall height (HEIGHT) for a 4 mm drop: U = V_T sqrt(1 - exp(-2 g h / V_T^2)), V_T = 8.83 m/s; the key
 * velocity scales U (100 = the HEIGHT's speed). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_bubble.h"
#include "tx_core_pulse.h"
#include "tx_core_grains.h"

#define OUT_TRIM 6720               /* output gain, Q12 (+6.3 dB): max momentary (one velocity-100 press): -37.6 LUFS at LEVEL 100 -> target -20 LUFS
                                    * (LOUDNESS.md; raw peaks kept <= +2 dBFS for the output limiter;
                                    * lowered 8.7 dB: the default chord <= +12 dBFS, the max scene <= +24 dBFS) */

/* no two presses alike (tx_core_vary.h): press level VARY_LO .. VARY_HI, tilt +-VARY_TILT, drift +-VARY_DRIFT
 * (tenths of a dB), the tilt's corner VARY_SPLIT. OUT_TRIM -2.0 dB with it: the mean of 8 presses back on its
 * loudness */
#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 120
#define VARY_DRIFT 40
#define VARY_SPLIT 0

enum { P_SIZE, P_SCATTER, P_RATE, P_CLUMP, P_DEPTH, P_HEIGHT, P_CAVE, P_DRIBBLE, NP };

static const tx_param_t PARAMS[NP] = {
    {"SIZE", 127, 55, "bubble radius at C4: 0.25 .. 6 mm (pitch 13 kHz .. 550 Hz; keys: a semitone smaller each)"},
    {"SCATTER", 127, 16, "drop-to-drop bubble size spread: 0 .. +-1 octave"},
    {"RATE", 127, 72, "drops per second while a key is held: 0.1 .. 20 (log)"},
    {"CLUMP", 127, 40, "drip timing: 0 = random (cave, Poisson) .. 127 = in runs of quick drops and quiet spells"},
    {"DEPTH", 127, 40, "how deep bubbles pinch off: 0 = at the surface (all rise, plink) .. 127 = deep (no glide, softer)"},
    {"HEIGHT", 127, 48, "fall height: 1 cm .. 3 m (log) -> impact speed 0.4 .. 8.6 m/s: louder, harder click, faster bubble"},
    {"CAVE", 127, 0, "the space around: 0 = dry (open air) .. a cave's diffuse tail, T60 0.4 .. 4 s (log)"},
    {"DRIBBLE", 127, 0, "run-off after rain: while a key is held, small drops along the whole edge, each its own: none .. 4 .. 1500 /s (log)"},
};

#define U_REF_Q8 330               /* 1.29 m/s (a 4 mm drop from 86 mm), m/s Q8: the reference level */
#define AMP_L2 (28 << 16)          /* a bubble of the SIZE radius at U_REF: 2^28 Q30 = -12 dBFS */
#define CLICK_L2 (10 << 16)        /* the impact at U_REF: 2^10 Q15 = -30 dBFS */
#define SPLASH_L2 (13 << 16)       /* the splash at U_REF: 2^11 Q15 peak envelope (set against the references' 5-12 kHz) */
#define SPLASH_C 18840             /* its two low-passes after the first difference, Q15: ~6 kHz each (a band 3 .. 10 kHz,
                                    * falling -6 dB/oct above: the references' 12 .. 20 kHz) */
#define BB0L 2500u                 /* the dribble's grain bands, Hz: bubbles 2.5 .. 10 kHz (Q 14), splashes 400 .. 1500 ..
                                    * 5000 .. 14000 Hz (Q 2.5; a grain's pitch log-uniform over them) */
#define BB0H 10000u
#define SPL_L2 565000              /* log2(400 Hz), Q16 */
#define SPL_SPAN 336000            /* log2(14000 / 400), Q16 */
#define BUBP 24u                   /* a plink with 1 drop in 4 */
#define DR_L2 (13 << 16)            /* a faint far drop's grain, log2 Q15 */
#define DR_SPAN (4 << 16)          /* .. the loudest 4 octaves (24 dB) over it */
#define WALK 20                    /* the surges' walk step at CLUMP 127, log2 Q16 a block */
#define CV_N 4u                    /* the cave: lines (half rate, 22.05 kHz) */
static const uint16_t CV_LEN[CV_N] = { 439, 557, 673, 811 };   /* 20 .. 37 ms: mutually prime, ~10 m of rock */
#define CV_TOT 2480u               /* their sum */
static const uint8_t AP_LEN[CV_N] = { 37, 53, 71, 97 };   /* 1.7 .. 4.4 ms: the rock's scatter, an allpass per line */
#define AP_TOT 258u                /* their sum */
#define AP_G 19661                 /* its gain, Q15 (0.6) */

typedef struct {
    tx_cloud_t cloud;
    tx_pool_t pool;
    tx_declick_t dc;
    tx_dcblock_t hp;
    tx_pulses_t clicks;
    tx_bubble_t b[TX_POOL_MAX];
    tx_event_t ev[TX_CLOUD_EV_MAX];
    int32_t buf[TX_BLOCK];
    uint8_t p[NP], quiet, pad;
    int32_t level;                 /* Q15 */
    int32_t size_l2;               /* log2(a_C4 / 1 mm), Q16 */
    int32_t scatter_q16;           /* +- spread, log2 units Q16 */
    uint32_t u_q8;                 /* impact speed at velocity 100, m/s Q8 */
    uint32_t rise_p16;             /* probability that a bubble rises, Q16 */
    uint16_t last_dropped;
    tx_vary_t vy;
    tx_rng_t sp_rng;               /* the splash's noise (its own: the drops' draws stay as they were) */
    int32_t sp_env, sp_amp, sp_x1, sp_lp, sp_lp2;   /* the splash: envelope now (Q15), the next one's peak, its filter */
    uint32_t sp_wait;              /* samples to the next splash (0: none pending) */
    uint8_t sp_sh, cv_on;          /* the splash's decay shift; the cave running */
    uint16_t cv_pos[CV_N];
    int32_t cv_g[CV_N], cv_lp[CV_N], cv_prev;   /* per-line decay (Q15), wall low-passes, the last output (for the
                                                * upsampling) */
    uint32_t cv_idle, cv_hold;     /* silent input samples so far; when to stop (2 T60) */
    int16_t cv[CV_TOT];            /* the lines, one after another (Q14: +-2 full scale) */
    int16_t ap[AP_TOT];            /* the lines' allpasses, one after another */
    uint8_t ap_pos[CV_N];
    tx_grains_t gr;                /* the dribble: its small far drops as grains (tx_core_grains.h: never a noise bed) */
    tx_rng_t dr_rng;               /* .. its own draws (the drops' stay as they were) */
    uint32_t dr_rate;              /* .. its rate, drops /s Q8 (0: none) */
    int32_t dr_w;                  /* .. its slow walk, log2 Q16 (the run-off comes and goes in surges) */
} drip_t;

static uint32_t isqrt32(uint32_t x)                          /* floor(sqrt(x)), control rate */
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return r;
}

static int32_t log2_q16(uint32_t x)                         /* log2(x) in Q16, x >= 1 */
{
    return (32 << 16) - (int32_t)(tx_neglog2_q12(x) << 4);
}

static void set_param(drip_t *s, unsigned id, unsigned v)
{
    if (id >= NP) return;
    if (v > PARAMS[id].max) v = PARAMS[id].max;
    s->p[id] = (uint8_t)v;
    switch (id) {
    case P_SIZE: s->size_l2 = -131072 + (int32_t)(v * 300480u / 127u); break;   /* log2(0.25 mm) + v log2(24) */
    case P_SCATTER: s->scatter_q16 = (int32_t)(v * 516u); break;              /* 0 .. 1 octave */
    case P_RATE: tx_cloud_rate(&s->cloud, tx_knob_exp(v, 26u, 500695)); break;  /* 0.1 * 2^(7.64 v / 127) /s, Q8 */
    case P_CLUMP: tx_cloud_clump(&s->cloud, v * 2u); break;
    case P_DEPTH: s->rise_p16 = 65535u - v * 516u; break;
    case P_HEIGHT: {
        uint32_t h = tx_knob_exp(v, 655u, 539301);           /* m Q16: 0.01 * 2^(8.23 v / 127) = 1 cm .. 3 m */
        uint32_t x = (uint32_t)(((uint64_t)h * 23789u) >> 16); /* 2 g h / V_T^2 / ln 2, Q16 */
        uint32_t e = x >= (16u << 16) ? 0u : tx_exp2((16 << 16) - (int32_t)x);   /* exp(-2gh/V_T^2), Q16 */
        s->u_q8 = (2260u * isqrt32((65536u - e) << 16)) >> 16;                     /* 8.83 m/s Q8 x sqrt, Q16 */
        if (s->u_q8 < 8u) s->u_q8 = 8u;
        break;
    }
    case P_DRIBBLE: s->dr_rate = v ? tx_knob_exp(v, 1024u, 560000) : 0u; break;   /* 4 x 2^(8.55 v / 127) /s: 4 .. 1500, Q8 */
    case P_CAVE:
        if (v) {   /* T60 = 0.4 s x 2^(3.32 v / 127); a line of L samples (22.05 kHz) loses 60 dB x L / (T60 fs) */
            uint32_t t60 = tx_knob_exp(v, 410u, 217900);     /* ms: 400 .. 4000 (2^3.32: v x 1716 Q16) */
            unsigned i;
            for (i = 0; i < CV_N; i++) {
                /* log2 g = -9.966 L / (T60 fs), Q16 (653100 = 9.966 x 65536; L / 22.05 kHz in ms; L with its
                 * allpass's mean delay) */
                int32_t l2 = -(int32_t)((653100u * (CV_LEN[i] + AP_LEN[i]) / 22u) / t60);
                s->cv_g[i] = (int32_t)tx_exp2(l2 + (15 << 16));
            }
            s->cv_hold = t60 * 88u;                          /* 2 T60 in samples (44.1 per ms) */
        }
        break;
    default: break;
    }
}

static void drip_init(void *st, uint32_t seed)
{
    drip_t *s = (drip_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_cloud_init(&s->cloud, seed);
    tx_cloud_burst(&s->cloud, 1, 0);                         /* a key press: one drop at once */
    tx_cloud_sizes(&s->cloud, 6144, 0);                      /* every drop the same "size": the key sets it */
    tx_pool_init(&s->pool, TX_POOL_MAX, 12);
    tx_pulses_init(&s->clicks);
    tx_rng_seed(&s->sp_rng, seed ^ 0x5B1A5Bu);
    tx_rng_seed(&s->dr_rng, seed ^ 0xD1BB1Eu);
    tx_gr_init(&s->gr);
    tx_gr_band(&s->gr, 0, BB0L, BB0H, 224, 256);              /* the small drops' bubbles (Q 14: plinks, never a hiss) */
    tx_gr_band(&s->gr, 1, 400, 1500, 40, 24);               /* their splashes on the ground (Q 2.5) */
    tx_gr_band(&s->gr, 2, 1500, 5000, 40, 400);
    tx_gr_band(&s->gr, 3, 5000, 14000, 40, 1800);
    for (i = 0; i < NP; i++) set_param(s, i, PARAMS[i].def);
    s->level = tx_level_q15(100);
    s->quiet = 1;
    tx_vary_init(&((drip_t *)st)->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

static void drip_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    if (on && vel) tx_vary_press(&((drip_t *)st)->vy);
    tx_cloud_key(&((drip_t *)st)->cloud, note, vel, on);
}

static void drip_param(void *st, uint8_t id, uint8_t val)
{
    drip_t *s = (drip_t *)st;
    if (id == TX_PID_PARTICLES) {
        uint32_t cut = tx_pool_set_limit(&s->pool, val), i;
        for (i = 0; i < TX_POOL_MAX; i++)
            if (cut >> i & 1u) tx_declick_add(&s->dc, s->b[i].last);
    } else if (id == TX_PID_LEVEL) {
        int32_t nl = tx_level_q15(val > 127u ? 127u : val);
        tx_bubbles_rescale(s->b, &s->pool, s->level, nl);
        s->level = nl;
    } else
        set_param(s, id, val);
}

/* one bubble -> a hero (or rejected when every hero is louder) */
static void bubble(drip_t *s, int32_t a_l2, int32_t amp_l2, uint32_t xi_q16, uint32_t wait)
{
    uint32_t env, stolen;
    int slot;
    if (amp_l2 > (30 << 16)) amp_l2 = 30 << 16;
    env = tx_exp2(amp_l2);
    env = (uint32_t)(((uint64_t)env * (uint32_t)s->level) >> 15);
    if (env <= TX_BUBBLE_FLOOR) { s->pool.rejected++; return; }
    slot = tx_pool_take(&s->pool, env, &stolen);
    if (slot < 0) return;
    if (stolen) tx_declick_add(&s->dc, s->b[slot].last);
    tx_bubble_start(&s->b[slot], a_l2, xi_q16, env, wait);
}

static uint32_t chance(drip_t *s, uint32_t p16) { return (tx_rand(&s->cloud.rng) >> 16) < p16; }

/* one drop: the impact click now, the bubble(s) after the crater collapses */
static void drop(drip_t *s, const tx_event_t *e)
{
    tx_rng_t *r = &s->cloud.rng;
    uint32_t u = (s->u_q8 * e->vel) / 100u, p_bub, wait, dur;
    int32_t lu = log2_q16(u) - log2_q16(U_REF_Q8);           /* log2(U / U_ref), Q16 */
    int32_t a_l2, rel, amp, cl;
    if (u < 4u) u = 4u;
    /* impact: level ~ U^2, length ~ 0.3 D / U (D = 4 mm): 52.9 / U samples */
    cl = CLICK_L2 + 2 * lu;
    if (cl > (15 << 16)) cl = 15 << 16;
    dur = 13547u / u;
    dur = dur < 4u ? 4u : dur > 200u ? 200u : dur;
    tx_pulse_start(&s->clicks, TX_PULSE_SHAPE_NWAVE, dur,
                   (int32_t)(((uint64_t)tx_exp2(cl) * (uint32_t)s->level) >> 15), e->offset);
    {   /* the splash: ~U^2 as the impact, a little after it (the crown rising), decaying 6 or 12 ms (the droplets falling back) */
        int32_t sl = SPLASH_L2 + 2 * lu;
        uint32_t a = tx_exp2(sl > (15 << 16) ? (15 << 16) : sl);
        a = (uint32_t)(((uint64_t)a * (uint32_t)s->level) >> 15);
        if ((int32_t)a > s->sp_amp || !s->sp_wait) { s->sp_amp = (int32_t)a; s->sp_wait = e->offset + 20u + tx_rand_below(&s->sp_rng, 60u); }
        s->sp_sh = (uint8_t)(8u + (tx_rand(&s->sp_rng) >> 31));
    }
    /* the bubble: P ~0.9 above 1 m/s, down to 0.2 at 0.3 m/s (ours) */
    p_bub = u >= 256u ? 58982u : u <= 77u ? 13107u : 13107u + (u - 77u) * 256u;
    if (!chance(s, p_bub)) return;
    a_l2 = s->size_l2 - ((int32_t)e->note - 60) * 5461 - (tx_vary_tone(&s->vy) >> 2)   /* the character: brighter = smaller */
         + (int32_t)(((int64_t)s->scatter_q16 * tx_rand_s15(r)) >> 15);
    rel = a_l2 - s->size_l2;                                 /* amplitude ~ a^1.5 against the SIZE radius, +-18 dB */
    rel = rel + rel / 2;
    rel = rel > (3 << 16) ? (3 << 16) : rel < -(3 << 16) ? -(3 << 16) : rel;
    amp = AMP_L2 + rel + (lu > (5 << 15) ? (5 << 15) : lu < -(6 << 16) ? -(6 << 16) : lu);
    /* crater collapse delay: 25 ms (1102 samples) x (1.4 / U)^1.75, +-25 %, 4 .. 45 ms */
    {
        int32_t l = 662300 + (7 * (log2_q16(358u) - log2_q16(u))) / 4;   /* log2(1102.5) = 10.106 Q16 */
        wait = tx_exp2(l);
        wait = wait > 4096u ? 4096u : wait;                  /* beyond, the clamp below gives 1984 anyway (no overflow) */
        wait = wait +(uint32_t)(((int32_t)wait * (tx_rand_s15(r) / 4)) >> 15);
        wait = wait < 176u ? 176u : wait > 1984u ? 1984u : wait;
        wait += e->offset;
    }
    if (chance(s, s->rise_p16))                              /* near the surface: it rises, xi 0.05 .. 0.15 */
        bubble(s, a_l2, amp, 3277u + ((tx_rand(r) >> 16) * 6554u >> 16), wait);
    else                                                     /* deep: no rise, half the amplitude */
        bubble(s, a_l2, amp - 65536, 0, wait);
    /* a fast drop may pinch off a second, bigger bubble (ours): P = (U - 2 m/s) / 8, at most 0.4 */
    if (u > 512u) {
        uint32_t p2 = (u - 512u) * 32u;
        if (chance(s, p2 > 26214u ? 26214u : p2)) {
            int32_t big = 39322 + (int32_t)((tx_rand(r) >> 16) * 65536u >> 16);   /* 0.6 .. 1.6 octaves larger */
            bubble(s, a_l2 + big, amp - 65536 + (big + big / 2), 0, wait + 220u + tx_rand_below(r, 441u));
        }
    }
}

/* the splash into buf: first-differenced noise through a low-pass, times its envelope */
TX_AUDIO_CODE static void splash(drip_t *s, int32_t *buf, unsigned n)
{
    int32_t env = s->sp_env, x1 = s->sp_x1, lp = s->sp_lp, lp2 = s->sp_lp2, x;
    unsigned j, sh = s->sp_sh;
    if (!env && (!s->sp_wait || s->sp_wait > n)) { if (s->sp_wait) s->sp_wait -= n; return; }   /* (none in this block) */
    for (j = 0; j < n; j++) {
        if (s->sp_wait && !--s->sp_wait) { if (!env) lp = lp2 = x1 = 0; if (s->sp_amp > env) env = s->sp_amp; s->sp_amp = 0; }
        if (!env) continue;                                  /* (the filter restarts from rest: a stale state
                                                             * under the envelope was a thump) */
        x = tx_rand_s15(&s->sp_rng);
        lp += ((x - x1) - lp) * SPLASH_C >> 15;
        lp2 += (lp - lp2) * SPLASH_C >> 15;
        x1 = x;
        buf[j] += (lp2 * env) >> 6;                          /* Q15 x Q15 -> Q23, x 2 (|lp2| < 2^16: < 2^31) */
        env -= (env >> sh) + 1;
        if (env < 0) env = 0;
    }
    s->sp_env = env; s->sp_x1 = x1; s->sp_lp = lp; s->sp_lp2 = lp2;
}

/* the cave: buf at half rate into a 4-line FDN (Hadamard / 2, a decay and a wall low-pass per line), the tail added
 * back (linear interpolation up); it stops 2 T60 after the input fell silent */
TX_AUDIO_CODE static void cave(drip_t *s, int32_t *buf, unsigned n)
{
    unsigned j, i, any = 0, base[CV_N], abase[CV_N];
    int32_t prev = s->cv_prev;
    if (s->p[P_CAVE]) for (j = 0; j < n; j++) any |= (unsigned)buf[j];
    if (!s->p[P_CAVE]) { if (s->cv_on) { for (j = 0; j < CV_TOT; j++) s->cv[j] = 0;
                                          for (j = 0; j < AP_TOT; j++) s->ap[j] = 0; s->cv_on = 0; s->cv_prev = 0;
                                          for (i = 0; i < CV_N; i++) s->cv_lp[i] = 0; } return; }
    if (any) { s->cv_idle = 0; s->cv_on = 1; }
    else if (!s->cv_on) return;
    else if ((s->cv_idle += n) > s->cv_hold) {
        for (j = 0; j < CV_TOT; j++) s->cv[j] = 0; for (j = 0; j < AP_TOT; j++) s->ap[j] = 0;
        for (i = 0; i < CV_N; i++) s->cv_lp[i] = 0;
        s->cv_on = 0; s->cv_prev = 0;
        return;
    }
    for (i = 0, j = 0; i < CV_N; j += CV_LEN[i], i++) base[i] = j;
    for (i = 0, j = 0; i < CV_N; j += AP_LEN[i], i++) abase[i] = j;
    for (j = 0; j + 1 < n; j += 2) {
        int32_t in = (buf[j] + buf[j + 1]) >> 10, y[CV_N], h0, h1, h2, h3, o;   /* Q23 / 2 -> Q14 */
        for (i = 0; i < CV_N; i++) {
            int32_t v = s->cv[base[i] + s->cv_pos[i]];
            int16_t *ap = &s->ap[abase[i] + s->ap_pos[i]];
            int32_t d = *ap, u;
            s->cv_lp[i] += ((v - s->cv_lp[i]) * 20000) >> 15;   /* the walls: ~5 kHz at 22 kHz */
            u = s->cv_lp[i] + ((d * AP_G) >> 15);            /* the rock's scatter: each pass smeared (a fixed */
            u = u > 32767 ? 32767 : u < -32768 ? -32768 : u;  /* path through the lines no longer recurs as a */
            *ap = (int16_t)u;                                /* flutter: 439 + 557 + 673 at 13.2 Hz did) */
            if (++s->ap_pos[i] >= AP_LEN[i]) s->ap_pos[i] = 0;
            u = d - ((u * AP_G) >> 15);
            y[i] = u > 32767 ? 32767 : u < -32768 ? -32768 : u;   /* (the lines' range: the mix stays in 32 bits) */
        }
        h0 = (y[0] + y[1] + y[2] + y[3]) >> 1; h1 = (y[0] - y[1] + y[2] - y[3]) >> 1;
        h2 = (y[0] + y[1] - y[2] - y[3]) >> 1; h3 = (y[0] - y[1] - y[2] + y[3]) >> 1;
        {
            int32_t w[CV_N];
            w[0] = ((h0 * s->cv_g[0]) >> 15) + in; w[1] = ((h1 * s->cv_g[1]) >> 15) + in;
            w[2] = ((h2 * s->cv_g[2]) >> 15) - in; w[3] = ((h3 * s->cv_g[3]) >> 15) - in;
            for (i = 0; i < CV_N; i++) {
                int32_t v = w[i] > 32767 ? 32767 : w[i] < -32768 ? -32768 : w[i];
                s->cv[base[i] + s->cv_pos[i]] = (int16_t)v;
                if (++s->cv_pos[i] >= CV_LEN[i]) s->cv_pos[i] = 0;
            }
        }
        o = (y[0] - y[1] + y[2] - y[3]) >> 1;                /* (the lines' sum / 2: the tail's level; Q14 -> Q23 below) */
        buf[j] += ((prev + o) >> 1) * 512;
        buf[j + 1] += o * 512;
        prev = o;
    }
    s->cv_prev = prev;
}

/* the dribble: while a key is held, small drops at DRIBBLE's rate (Poisson, in surges: a slow walk of +-CLUMP), each a
 * splash grain on the ground and, now and then, a small bubble's plink; their sizes and distances spread their levels
 * (u^3: most faint, a few near and loud) */
static uint32_t dr_exp(tx_rng_t *r, uint32_t mean)        /* an exponential draw of mean `mean` (its own draws) */
{
    uint64_t e = (uint64_t)((tx_neglog2_q12(tx_rand(r) | 1u) * 2839u) >> 12) * mean >> 12;
    return e > 0xFFFFu ? 0xFFFFu : (uint32_t)e;
}
static void dribble(drip_t *s, unsigned n)
{
    tx_rng_t *r = &s->dr_rng;
    uint32_t mu, mean, acc, k, i;
    int32_t lu;
    if (!s->dr_rate || !tx_cloud_held(&s->cloud)) return;
    s->dr_w += (int32_t)(((int64_t)tx_rand_s15(r) * (WALK * (int32_t)s->p[P_CLUMP])) >> 15) - s->dr_w / 172;
    mu = (uint32_t)(((uint64_t)s->dr_rate * tx_exp2(s->dr_w + (16 << 16))) >> 16) * n / 44100u;   /* this block, Q8 */
    if (!mu) return;
    mean = 65536u / mu;
    lu = log2_q16(s->u_q8 < 8u ? 8u : s->u_q8) - log2_q16(U_REF_Q8);
    for (k = 0, acc = dr_exp(r, mean); acc < 256u && k < 64u; k++, acc += dr_exp(r, mean)) {
        uint32_t u = tx_rand(r) >> 16, u3 = ((u * u) >> 16) * u >> 16, off = tx_rand_below(r, n);
        int32_t l2 = DR_L2 + (int32_t)(((int64_t)u3 * DR_SPAN) >> 16) + 2 * lu, a;
        l2 = l2 > (15 << 16) ? (15 << 16) : l2;
        a = (int32_t)(((int64_t)tx_exp2(l2) * s->level) >> 15);
        i = tx_rand(r);
        tx_gr_add(&s->gr, r, off, tx_exp2(SPL_L2 + (int32_t)(((i >> 16) * SPL_SPAN) >> 16)), (i & 1u) ? a : -a);   /* the splash */
        if ((i >> 8 & 0xFFu) < BUBP) {                       /* a plink */
            uint32_t f = tx_exp2(745000 + (int32_t)(((tx_rand(r) >> 16) * 131072u) >> 16));   /* 2.5 .. 10 kHz */
            tx_gr_add(&s->gr, r, off + tx_rand_below(r, 64u), f, (i & 2u) ? a / 2 : -a / 2);
        }
    }
}

TX_AUDIO_CODE static void drip_render(void *st, int32_t *mix, unsigned n)
{
    drip_t *s = (drip_t *)st;
    unsigned k, i;
    uint32_t rej0 = s->pool.rejected;
    int32_t *buf = s->buf;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (i = 0; i < n; i++) buf[i] = 0;
    k = tx_cloud_block(&s->cloud, s->ev, n);
    tx_pool_tick(&s->pool);
    for (i = 0; i < k; i++) drop(s, &s->ev[i]);
    dribble(s, n);
    tx_gr_render(&s->gr, &s->dr_rng, buf, n);
    tx_bubbles_render(s->b, &s->pool, &s->dc, buf, n);
    tx_pulses_add(&s->clicks, buf, n);
    splash(s, buf, n);
    tx_declick_run(&s->dc, buf, n);
    tx_vary_block(&s->vy, buf, n);
    cave(s, buf, n);
    tx_trim(buf, n, OUT_TRIM);
    tx_headroom(buf, n);                                     /* (every key, the biggest bubbles at once) */
    tx_dcblock_add(&s->hp, buf, mix, n);
    s->last_dropped = (uint16_t)(s->cloud.last_dropped + (s->pool.rejected - rej0));
    s->quiet = (uint8_t)(tx_cloud_idle(&s->cloud) && !s->pool.alive && !tx_pulses_active(&s->clicks)
                         && !s->dc.v && !s->hp.x1 && !s->hp.y1 && !s->sp_env && !s->sp_wait && !s->cv_on
                         && tx_gr_quiet(&s->gr));
}

static void drip_stats(const void *st, tx_stats_t *o)
{
    const drip_t *s = (const drip_t *)st;
    o->particles = (uint16_t)tx_pool_count(&s->pool);
    o->limit = s->pool.limit;
    o->events = s->cloud.last_events;
    o->dropped = s->last_dropped;
    o->steals = s->pool.steals;
    o->total = s->cloud.total;
    o->quiet = s->quiet;
}

/* the worst case: every hero, every key slot at the fastest rate, the biggest (longest) bubbles, the hardest drops */
static const char SCENE_MAX[] =
    "0 particles 32\n0 param RATE 127\n0 param SIZE 127\n0 param HEIGHT 127\n0 param SCATTER 127\n"
    "0 param DEPTH 0\n0 param CLUMP 127\n0 param CAVE 127\n"
    "0 on 53 127\n0 on 57 127\n0 on 60 127\n0 on 64 127\n0 on 67 127\n0 on 71 127\n0 on 74 127\n0 on 79 127\n"
    "500 on 53 127\n500 on 60 127\n500 on 67 127\n500 on 74 127\n"
    "1000 on 57 127\n1000 on 64 127\n1000 on 71 127\n1000 on 79 127\n"
    "2000 param SIZE 0\n"
    "3000 off 53\n3000 off 57\n3000 off 60\n3000 off 64\n3000 off 67\n3000 off 71\n3000 off 74\n3000 off 79\n"
    "4000 end\n";

const tx_engine_t tx_eng_drip = {
    "drip", "#1 Drip (bubbles in a pool)", NP, PARAMS, TX_POOL_MAX, 12, sizeof(drip_t), SCENE_MAX,
    drip_init, drip_key, drip_param, drip_render, drip_stats,
};
