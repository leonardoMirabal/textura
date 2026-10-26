/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura modulation sources: the LFOs and the matrix's sum (tx_mod.h). Integers only, 32-bit products. */
#include "tx_mod.h"
#include "tx_core_math.h"

const tx_param_t tx_lfo_params[TX_LP_N] = {
    {"SHAPE", TX_LS_N - 1, TX_LS_SMOOTH, "sine | triangle | saw up | saw down | square | pulse | S&H | smooth (random, "
                                         "eased) | drift (a random walk) | gusts (random swells) | chaos | steps"},
    {"RATE", 127, 64, "free: 0.0005 Hz (33 min) .. 20 Hz, exponential"},
    {"SYNC", TX_LFO_NSYNC - 1, 0, "FREE, or a division of the beat: 1/32T .. 64 BAR"},
    {"MODE", TX_LM_N - 1, TX_LM_FREE, "free-running | a key restarts it | lane 1..4's start or notes | one-shot"},
    {"PHASE", 127, 0, "the start phase: 0 .. 360 degrees"},
    {"WIDTH", 127, 64, "the shape's skew: pulse duty, triangle symmetry, SMOOTH's easing, GUSTS' rise, STEPS' chance"},
    {"JITTER", 127, 38, "free: each cycle's length varies at random by up to +-50 % (38 = 15 %)"},
    {"FADE", 127, 0, "after a restart the depth rises over 0 .. 10 s"},
    {"RESTART", 1, 0, "off | on START (the transport's)"},
    {"LEVEL", 127, 127, "the LFO's depth, scaling all its routes"},
};
/* 1/32T 1/32 1/16T 1/16 1/16D 1/8T 1/8 1/8D 1/4T 1/4 1/4D 1/2T 1/2 1/2D 1/1T 1 BAR 1D 2 4 8 16 32 64 BAR */
const uint16_t TX_LFO_SYNC_UNITS[TX_LFO_NSYNC] = {0, 8, 12, 16, 24, 36, 32, 48, 72, 64, 96, 144, 128, 192, 288, 256, 384,
                                                  576, 768, 1536, 3072, 6144, 12288, 24576};

static uint32_t rnd(tx_lfo_t *l) { uint32_t r = l->rng; r ^= r << 13; r ^= r >> 17; r ^= r << 5; return l->rng = r; }
static int32_t rnd_q15(tx_lfo_t *l) { return (int32_t)(rnd(l) >> 16) - 32768; }

uint32_t tx_lfo_inc(unsigned rate) { return tx_exp2(891668 + (int32_t)(rate > 127u ? 127u : rate) * 7889); }   /* 12466 x 2^(rate x 15.29 / 127) */
uint32_t tx_lfo_period_ms(unsigned rate) { return tx_exp2(1371666 - (int32_t)(rate > 127u ? 127u : rate) * 7889); }

void tx_lfo_init(tx_lfo_t *l, unsigned n, uint32_t seed)
{
    static const uint8_t SH[TX_LFO_N] = {TX_LS_SMOOTH, TX_LS_DRIFT, TX_LS_GUSTS, TX_LS_SH, TX_LS_SMOOTH, TX_LS_SMOOTH,
                                         TX_LS_SMOOTH, TX_LS_SMOOTH};
    static const uint8_t RT[TX_LFO_N] = {64, 49, 55, 72, 64, 64, 64, 64};   /* 0.1, 0.03, 0.05, 0.2 Hz, then 0.1 */
    unsigned i;
    uint8_t *b = (uint8_t *)l;
    for (i = 0; i < sizeof *l; i++) b[i] = 0;
    for (i = 0; i < TX_LP_N; i++) l->p[i] = tx_lfo_params[i].def;
    l->p[TX_LP_SHAPE] = SH[n % TX_LFO_N]; l->p[TX_LP_RATE] = RT[n % TX_LFO_N];
    l->rng = seed | 1u;
    l->jf = 32768; l->fade = 65536;
    l->a = rnd_q15(l); l->b = rnd_q15(l);
    l->x = 1024 + (int32_t)(rnd(l) >> 22); l->y = 1024; l->z = 1024 << 4;   /* (CHAOS: x y z; DRIFT uses x) */
    if (l->p[TX_LP_SHAPE] == TX_LS_DRIFT) l->x = 0;
}
void tx_lfo_param(tx_lfo_t *l, unsigned id, unsigned v)
{
    if (id >= TX_LP_N) return;
    if (v > tx_lfo_params[id].max) v = tx_lfo_params[id].max;
    if (id == TX_LP_SHAPE && v != l->p[id]) {   /* a new shape: from the middle, the walks at rest */
        l->x = v == TX_LS_CHAOS ? 1024 : 0; l->y = v == TX_LS_CHAOS ? 1024 : 0; l->z = v == TX_LS_CHAOS ? 1024 << 4 : 0;
        l->stage = 0; l->acc = 0;
    }
    if (id == TX_LP_MODE && v == TX_LM_ONESHOT && l->p[id] != v) l->held = 1;   /* (one-shot: waits for its trigger) */
    if (id == TX_LP_MODE && v != TX_LM_ONESHOT) l->held = 0;
    l->p[id] = (uint8_t)v;
}
void tx_lfo_restart(tx_lfo_t *l)
{
    l->ph = (uint32_t)l->p[TX_LP_PHASE] << 25;
    l->held = 0;
    l->fresh = 1;
    if (l->p[TX_LP_FADE]) l->fade = 0;
}

static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}

/* the shape's value at the phase (Q15); wrap: a cycle began this block */
TX_AUDIO_CODE static int32_t lfo_shape(tx_lfo_t *l, uint32_t inc, int wrap)
{
    uint32_t ph = l->ph, u = ph >> 16, w = l->p[TX_LP_WIDTH];
    switch (l->p[TX_LP_SHAPE]) {
    case TX_LS_SINE: return tx_sin_q15(ph);
    case TX_LS_TRI: {
        uint32_t pk = w * 516u + 1u;
        int32_t v = u < pk ? (int32_t)((u << 15) / pk) : (int32_t)(((65535u - u) << 15) / (65536u - pk));
        return 2 * v - 32768;
    }
    case TX_LS_SAWUP: return (int32_t)u - 32768;
    case TX_LS_SAWDN: return 32767 - (int32_t)u;
    case TX_LS_SQUARE: return ph >> 31 ? -32767 : 32767;
    case TX_LS_PULSE: return (ph >> 25) < w ? 32767 : -32767;
    case TX_LS_SH: if (wrap) l->a = rnd_q15(l); return l->a;
    case TX_LS_STEPS: if (wrap && (rnd(l) >> 25) < w) l->a = rnd_q15(l); return l->a;
    case TX_LS_SMOOTH: {                        /* random points, eased between (WIDTH: linear .. a cosine) */
        int32_t lin = (int32_t)(u >> 1), e = (32768 - tx_sin_q15((ph >> 1) + 0x40000000u)) >> 1;
        if (wrap) { l->a = l->b; l->b = rnd_q15(l); }
        e = lin + (((e - lin) * (int32_t)(w * 258u)) >> 15);   /* (w / 127: 32-bit products only, no 64-bit) */
        return l->a + ((((l->b - l->a) >> 1) * e) >> 14);
    }
    case TX_LS_DRIFT: {                         /* an Ornstein-Uhlenbeck walk, its time constant the rate, sd 0.4 */
        uint32_t a = ((inc >> 12) * 201u) >> 10, sq;
        int32_t n;
        if (a < 1u) a = 1u;
        if (a > 8192u) a = 8192u;
        sq = isqrt(2u * a << 15);               /* sqrt(2a), Q15 */
        n = (int32_t)(rnd(l) >> 20) + (int32_t)(l->rng >> 8 & 4095u) + (int32_t)(rnd(l) >> 20) + (int32_t)(l->rng >> 8 & 4095u) - 8190;
        n *= 14;                                /* ~N(0, 1), Q15 */
        l->x += -((l->x * (int32_t)a) >> 15) + ((((13107 * (int32_t)sq) >> 15) * n) >> 15);
        if (l->x > 32767) l->x = 32767;
        if (l->x < -32767) l->x = -32767;
        return l->x;
    }
    case TX_LS_GUSTS: {                         /* swells at random (Poisson at the rate): a quick rise, a slow fall */
        if (rnd(l) < inc) { l->stage = 1; l->y = 8192 + (int32_t)(rnd(l) >> 17); }
        if (l->stage == 1) {
            uint32_t st = ((uint32_t)l->y * (inc >> 16)) >> 16;
            st = st * 512u / (w + 1u);
            l->x += (int32_t)(st ? st : 1u);
            if (l->x >= l->y) { l->x = l->y; l->stage = 2; }
        } else if (l->stage == 2) {
            uint32_t fa = inc >> 16;
            l->x -= (l->x * (int32_t)(fa ? fa : 1u)) >> 15;
            if (l->x < 64) { l->x = 0; l->stage = 0; }
        }
        return l->x;
    }
    case TX_LS_CHAOS: {                         /* a Lorenz system (10, 28, 8/3), Q10, steps of 0.005 at the rate */
        unsigned k = 0;
        l->acc += inc;
        while (l->acc >= 30678337u && k < 16u) {
            int32_t x = l->x, y = l->y, z = l->z;
            l->x = x + ((10 * (y - x) * 328) >> 16);
            l->y = y + (((((x * (28672 - z)) >> 10) - y) * 328) >> 16);
            l->z = z + (((((x * y) >> 10) - ((z * 2731) >> 10)) * 328) >> 16);   /* (z 8/3) */
            l->acc -= 30678337u; k++;
        }
        if (k == 16u) l->acc = 0;
        {
            int32_t v = (l->x * 13107) >> 13;     /* (x 8/5) */
            return v > 32767 ? 32767 : v < -32767 ? -32767 : v;
        }
    }
    }
    return 0;
}

TX_AUDIO_CODE void tx_lfo_block(tx_lfo_t *l, unsigned rate, unsigned level, uint32_t pos)
{
    uint32_t inc = tx_lfo_inc(rate), sy = l->p[TX_LP_SYNC], old = l->ph;
    int wrap = 0;
    int32_t s;
    if (l->fresh) { l->fresh = 0; wrap = 1; }
    if (sy) {                                   /* synced: the phase from the song position (no drift, START re-phases) */
        uint32_t span = (uint32_t)TX_LFO_SYNC_UNITS[sy] << 8, b = (pos % span) * (0xFFFFFFFFu / span);
        if (b < l->pb) wrap = 1;
        inc = b - l->pb;                        /* (the walks' time: this block's step; a wrap's is right too) */
        if (inc > 0x40000000u) inc = 0;         /* (a jump back: START, a snap) */
        l->pb = b;
        l->ph = b + ((uint32_t)l->p[TX_LP_PHASE] << 25);
    } else if (!l->held) {                      /* free: the rate, each cycle's length varied (JITTER) */
        uint32_t e = (inc >> 15) * l->jf + (((inc & 0x7FFFu) * l->jf) >> 15);
        l->ph += e;
        if (l->ph < old) {
            int32_t d = (rnd_q15(l) * (int32_t)l->p[TX_LP_JITTER] * 129) >> 15;   /* (/ 254) */
            wrap = 1;
            l->jf = (uint32_t)(32768 + d);
            if (l->p[TX_LP_MODE] == TX_LM_ONESHOT) { l->held = 1; l->ph = 0xFFFFFFFFu; }
        }
    }
    s = lfo_shape(l, inc, wrap);
    if (l->fade < 65536u) {                     /* FADE: 10 s at 127 (1723 blocks) */
        uint32_t f = l->p[TX_LP_FADE], st = f ? 4830u / f : 65536u;
        l->fade += st ? st : 1u;
        if (l->fade > 65536u) l->fade = 65536u;
    }
    if (level > 127u) level = 127u;
    s = (s * (int32_t)(level * 258u + (level >> 6))) >> 15;   /* (level / 127) */
    l->out = (s * (int32_t)(l->fade >> 1)) >> 15;
    {                                           /* the ring: a value each 1/32 cycle */
        uint32_t pt = l->ph >> 27;
        if (pt != l->rpt) { l->ring[l->rw] = (int16_t)l->out; l->rw = (uint8_t)((l->rw + 1u) & (TX_LFO_RING - 1u)); l->rpt = pt; }
    }
}

TX_AUDIO_CODE int32_t tx_lfo_lag(const tx_lfo_t *l, uint32_t lag_q8)
{
    uint32_t i = lag_q8 >> 8, f = lag_q8 & 255u;
    int32_t v0, v1;
    if (!lag_q8) return l->out;
    if (i > TX_LFO_RING - 2u) i = TX_LFO_RING - 2u;
    v0 = i ? l->ring[(l->rw - i) & (TX_LFO_RING - 1u)] : l->out;
    v1 = l->ring[(l->rw - 1u - i) & (TX_LFO_RING - 1u)];
    return v0 + (((v1 - v0) * (int32_t)f) >> 8);
}

TX_AUDIO_CODE void tx_mod_sum(const tx_mod_rt_t *r, unsigned n, const tx_lfo_t *lfo, const int16_t *ext, int32_t *sum)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        int32_t v = r[i].src < TX_LFO_N ? tx_lfo_lag(&lfo[r[i].src], (uint32_t)r[i].lag << 5) : ext[r[i].src - TX_LFO_N];
        int32_t c = ((int32_t)r[i].depth * v) >> 15;
        sum[r[i].t] += (c * (int32_t)r[i].hi) >> 7;
    }
}
