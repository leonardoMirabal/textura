/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura modulation sources: the LFOs and the matrix's sum (tx_mod.h). Integers only, 32-bit products. */
#include "tx_mod.h"
#include "tx_core_math.h"

const tx_param_t tx_lfo_params[TX_LP_N] = {
    {"SHAPE", TX_LS_N - 1, TX_LS_SMOOTH, TX_TEXT("sine | triangle | saw up | saw down | square | pulse | S&H | smooth (random, "
                                         "eased) | drift (a random walk) | gusts (random swells) | chaos | steps")},
    {"RATE", 127, 64, TX_TEXT("free: 0.0005 Hz (33 min) .. 20 Hz, exponential")},
    {"SYNC", TX_LFO_NSYNC - 1, 0, TX_TEXT("FREE, or a division of the beat: 1/32T .. 64 BAR")},
    {"MODE", TX_LM_N - 1, TX_LM_FREE, TX_TEXT("free-running | a key restarts it | lane 1..4's start or notes | one-shot")},
    {"PHASE", 127, 0, TX_TEXT("the start phase: 0 .. 360 degrees")},
    {"WIDTH", 127, 64, TX_TEXT("the shape's skew: pulse duty, triangle symmetry, SMOOTH's easing, GUSTS' rise, STEPS' chance")},
    {"JITTER", 127, 38, TX_TEXT("free: each cycle's length varies at random by up to +-50 % (38 = 15 %)")},
    {"FADE", 127, 0, TX_TEXT("after a restart the depth rises over 0 .. 10 s")},
    {"RESTART", 1, 0, TX_TEXT("off | on START (the transport's)")},
    {"LEVEL", 127, 127, TX_TEXT("the LFO's depth, scaling all its routes")},
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
    tx_zero(b, sizeof *l);
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

/* ---- ENV */
const tx_param_t tx_env_params[TX_EP_N] = {
    {"TRIGGER", TX_ET_N - 1, TX_ET_KEY, TX_TEXT("key (any lane's note) | lane 1..4's notes | lane 1..4's events | random | "
                                        "LFO 1..8 rising through 0 | the transport's START")},
    {"ATTACK", 127, 40, TX_TEXT("1 ms .. 60 s, exponential")},
    {"DECAY", 127, 70, TX_TEXT("1 ms .. 60 s, exponential")},
    {"RELEASE", 127, 80, TX_TEXT("1 ms .. 60 s, exponential")},
    {"SUSTAIN", 127, 64, TX_TEXT("the level held while the notes are held (key and lane triggers)")},
    {"CURVE", 127, 64, TX_TEXT("linear .. eased (a fast start, as an exponential)")},
    {"VARY", 127, 32, TX_TEXT("each trigger: its times +-50 % and its peak down to half at most, at random")},
    {"LOOP", 1, 0, TX_TEXT("off | on: it starts again on its own after a random rest")},
};
static uint32_t ernd(uint32_t *r) { uint32_t x = *r; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *r = x; }
uint32_t tx_env_ms(unsigned v) { return tx_exp2((int32_t)(v > 127u ? 127u : v) * 8191); }   /* 60000^(v / 127) */
static uint32_t env_blocks_q8(const tx_env_t *e, unsigned id)    /* a stage's length in blocks (Q8), VARY applied */
{
    uint32_t msq8 = tx_exp2((int32_t)e->p[id] * 8191 + (8 << 16));   /* ms, Q8 */
    uint32_t b = ((msq8 >> 4) * 2822u) >> 10;                         /* x 44100 / 256 / 1000 (2.756), Q8 */
    return ((b >> 4) * (e->vf >> 4)) >> 4;                           /* x vf (Q12) */
}
static uint32_t env_inc(const tx_env_t *e, unsigned id)
{
    uint32_t b = env_blocks_q8(e, id);
    return b <= 256u ? (1u << 24) : 0xFFFFFFFFu / b;                 /* Q24 a block (Q32 / Q8) */
}
void tx_env_init(tx_env_t *e, uint32_t seed)
{
    unsigned i;
    uint8_t *b = (uint8_t *)e;
    tx_zero(b, sizeof *e);
    for (i = 0; i < TX_EP_N; i++) e->p[i] = tx_env_params[i].def;
    e->rng = seed | 1u; e->vf = 4096; e->peak = 32767;
}
void tx_env_param(tx_env_t *e, unsigned id, unsigned v)
{
    if (id >= TX_EP_N) return;
    e->p[id] = (uint8_t)(v > tx_env_params[id].max ? tx_env_params[id].max : v);
}
static void env_start(tx_env_t *e)                                    /* a trigger: the attack, from where it is */
{
    int32_t r = (int32_t)(ernd(&e->rng) >> 16) - 32768, q = (int32_t)(ernd(&e->rng) >> 17);
    int32_t vy = e->p[TX_EP_VARY];
    e->vf = (uint32_t)(4096 + ((r * vy * 258) >> 19));                /* +-50 % (x 258 >> 15: / 127, no division) */
    e->peak = 32767 - ((q * vy * 258) >> 16);                         /* down to half */
    e->stage = TX_ES_ATT; e->ph = 0; e->a = e->out; e->b = e->peak;
}
static void env_next(tx_env_t *e, unsigned stage, int32_t to)
{
    e->stage = (uint8_t)stage; e->ph = 0; e->a = e->out; e->b = to;
}
TX_AUDIO_CODE void tx_env_block(tx_env_t *e, unsigned trig, unsigned gate)
{
    unsigned t = e->p[TX_EP_TRIGGER], id;
    uint32_t p, c = e->p[TX_EP_CURVE];
    if (t == TX_ET_RANDOM && (e->stage == TX_ES_IDLE || e->stage == TX_ES_REL || e->stage == TX_ES_REST)) {
        uint32_t gap = (env_blocks_q8(e, TX_EP_DECAY) >> 6) + 1u;    /* 4 x DECAY, blocks */
        trig = ernd(&e->rng) % gap == 0u;
    }
    if (t >= TX_ET_EV1 && t < TX_ET_EV1 + 4u && (e->stage == TX_ES_ATT || e->stage == TX_ES_DEC)) trig = 0;
    if (t != TX_ET_KEY && (t < TX_ET_LANE1 || t >= TX_ET_LANE1 + 4u)) gate = 0;
    if (trig || (e->stage == TX_ES_IDLE && e->p[TX_EP_LOOP])) env_start(e);
    switch (e->stage) {
    case TX_ES_IDLE: e->out = 0; return;
    case TX_ES_SUS:
        e->out = e->b;
        if (!gate) env_next(e, TX_ES_REL, 0);
        return;
    case TX_ES_REST:
        if (e->rest) e->rest--;
        else env_start(e);
        if (e->stage == TX_ES_REST) return;
        break;
    }
    id = e->stage == TX_ES_ATT ? TX_EP_ATTACK : e->stage == TX_ES_DEC ? TX_EP_DECAY : TX_EP_RELEASE;
    e->ph += env_inc(e, id);
    if (e->ph >= (1u << 24)) e->ph = 1u << 24;
    p = e->ph >> 9;                                                    /* Q15 */
    p += ((int32_t)(((p * (65536u - p)) >> 15) - p) * (int32_t)c * 258) >> 15;   /* CURVE: towards p (2 - p), eased */
    e->out = e->a + (((e->b - e->a) * (int32_t)p) >> 15);
    if (e->ph < (1u << 24)) return;
    e->out = e->b;
    if (e->stage == TX_ES_ATT) env_next(e, TX_ES_DEC, (e->peak * e->p[TX_EP_SUSTAIN] * 258) >> 15);
    else if (e->stage == TX_ES_DEC) { if (gate) e->stage = TX_ES_SUS; else env_next(e, TX_ES_REL, 0); }
    else if (e->p[TX_EP_LOOP]) {                                       /* LOOP: a random rest, then again */
        e->stage = TX_ES_REST;
        e->rest = (uint32_t)(((ernd(&e->rng) >> 16) * ((env_blocks_q8(e, TX_EP_DECAY) >> 7) + 1u)) >> 16);
    } else e->stage = TX_ES_IDLE;
}

/* ---- FOLLOW */
const tx_param_t tx_fol_params[TX_FP_N] = {
    {"INPUT", TX_FI_N - 1, TX_FI_LEVEL1, TX_TEXT("lane 1..4's level | lane 1..4's event rate | the mix's level | the grain "
                                         "delay's grains sounding")},
    {"ATTACK", 127, 30, TX_TEXT("the rise: 5 ms .. 10 s, exponential")},
    {"RELEASE", 127, 70, TX_TEXT("the fall: 5 ms .. 10 s, exponential")},
    {"GAIN", 127, 64, TX_TEXT("x0 .. x2 (64: x1)")},
};
uint32_t tx_fol_ms(unsigned v) { return (5u * tx_exp2((int32_t)(v > 127u ? 127u : v) * 5660 + (8 << 16))) >> 8; }
void tx_fol_init(tx_fol_t *f)
{
    unsigned i;
    for (i = 0; i < TX_FP_N; i++) f->p[i] = tx_fol_params[i].def;
    f->env = 0; f->out = 0;
}
void tx_fol_param(tx_fol_t *f, unsigned id, unsigned v)
{
    if (id >= TX_FP_N) return;
    f->p[id] = (uint8_t)(v > tx_fol_params[id].max ? tx_fol_params[id].max : v);
}
TX_AUDIO_CODE void tx_fol_block(tx_fol_t *f, int32_t x)
{
    int32_t d, o;
    uint32_t b, c;
    x = x < 0 ? 0 : x > 32767 ? 32767 : x;
    d = (x << 8) - f->env;                                             /* Q23 */
    b = (tx_fol_ms(f->p[d > 0 ? TX_FP_ATTACK : TX_FP_RELEASE]) * 2822u) >> 6;   /* blocks, Q8 (ms x 0.1723) */
    c = b <= 256u ? 65535u : (65536u * 256u) / b;                    /* the step a block, Q16 */
    f->env += ((d >> 9) * (int32_t)c) >> 7;
    o = ((f->env >> 8) * (int32_t)f->p[TX_FP_GAIN]) >> 6;
    f->out = o > 32767 ? 32767 : o;
}
int32_t tx_fol_level_q15(uint32_t pk)             /* 20 log10(pk / 2^23) over -60 .. 0: log2 / 9.97 */
{
    int32_t l2;
    if (pk >= (1u << 23)) return 32767;
    if (!pk) return 0;
    l2 = (int32_t)(32u << 12) - (int32_t)tx_neglog2_q12(pk) - (23 << 12);   /* log2(pk / 2^23), Q12, <= 0 */
    l2 += 40820;                                                       /* + 9.966 (60 dB) */
    return l2 <= 0 ? 0 : (l2 * 26303) >> 15;                          /* (x 32767 / 40820, no division) */
}
int32_t tx_fol_rate_q15(uint32_t ev_q8)           /* events a block (Q8) -> /s (x 172.3), log over 0.1 .. 1000 */
{
    uint32_t r10 = ev_q8 * 1723u / 256u;                               /* tenths of an event a second (x 10) */
    int32_t l2;
    if (r10 <= 1u) return 0;
    l2 = (int32_t)(32u << 12) - (int32_t)tx_neglog2_q12(r10);         /* log2(rate x 10), Q12: 0 .. 13.29 */
    return l2 >= 54426 ? 32767 : (l2 * 19728) >> 15;                 /* (x 32767 / 54426) */
}
