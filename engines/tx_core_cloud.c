/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura CLOUD / EVENTS core: emitters (bursts, Poisson streams), power-law sizes, Omori aftershocks, the hero
 * pool with quietest-first stealing (tx_core_cloud.h explains the design). */
#include "tx_core_cloud.h"

#define TX_STREAM_GUARD 64u        /* stream events per emitter per block, at most (a guard; rates stay far below) */
#define TX_OMORI_MAX_L2 (13 << 12) /* aftershock times: log2 factor cap (2^13 c ~ 9 min at c = 70 ms) */
#define TX_OMORI_MAX_T 441000u     /* ... and at most 10 s after the main event */

/* ---- draws */
uint32_t tx_cloud_exp_q8(tx_cloud_t *c, uint32_t mean_q8)
{
    uint32_t ln = (tx_neglog2_q12(tx_rand(&c->rng)) * TX_LN2_Q12) >> 12;   /* -ln u, Q12 (<= 22.2) */
    uint64_t d = ((uint64_t)ln * mean_q8) >> 12;                            /* per event: 64-bit product */
    return d > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)d;
}

/* a truncated power law by inversion: u is drawn in (2^(-max (a-1)), 1], so log2(E/E_min) = -log2(u)/(a-1) falls in
 * [0, max] by itself. (It used to be clamped: the share over the top, ~2-10 %, piled up at exactly E_max, every one
 * the same size: a stream's largest bubble rang the same note again and again, a stationary line on the spectrogram;
 * session 5.) One draw, as before. */
int32_t tx_cloud_pareto(tx_cloud_t *c, int32_t inv_q12, int32_t max_l2)
{
    uint32_t u = tx_rand(&c->rng), cut;
    int64_t x;
    if (inv_q12 <= 0 || max_l2 <= 0) return 0;
    {   /* the share kept: 1 - 2^(-max / inv), Q16 */
        uint32_t r = ((uint32_t)max_l2 << 4) / (uint32_t)inv_q12;   /* max / inv, Q4 (max_l2 < 2^27) */
        cut = r >= (16u << 4) ? 65535u : 65536u - tx_exp2((16 << 16) - (int32_t)(r << 12));
    }
    u = 0xFFFFFFFFu - (u >> 16) * cut;                       /* uniform in (2^32 (1 - share), 2^32) */
    x = ((int64_t)tx_neglog2_q12(u) * inv_q12) >> 12;        /* log2(E/E_min) = -log2(u)/(a-1) */
    return x > max_l2 ? max_l2 : (int32_t)x;
}

static uint32_t omori_t(tx_cloud_t *c)                       /* t = c (2^(-log2(u) / (p-1)) - 1), samples */
{
    int64_t x = ((int64_t)tx_neglog2_q12(tx_rand(&c->rng)) * c->after_inv_q12) >> 12;
    uint32_t g;
    uint64_t t;
    if (x > TX_OMORI_MAX_L2) x = TX_OMORI_MAX_L2;
    g = tx_exp2((int32_t)x * 16 + (16 << 16));               /* 2^x, Q16 */
    t = ((uint64_t)(g - 65536u) * c->after_c) >> 16;
    return t > TX_OMORI_MAX_T ? TX_OMORI_MAX_T : (uint32_t)t;
}

/* the next stream gap, Q8 samples: Poisson by default, partly periodic (tx_cloud_regular), or clumped
 * (tx_cloud_clump: the emitter's quick runs and quiet spells) */
static uint32_t stream_gap(tx_cloud_t *c, tx_emitter_t *e)
{
    uint32_t x;
    if (c->clump) {
        uint32_t m = 256u + (((uint32_t)c->clump * 3u) >> 2), p = c->period_q8;   /* (1 +- 3a/4), Q8 */
        if (!e->lull) m = 512u - m;
        if (tx_rand_below(&c->rng, 3u) == 0u) e->lull ^= 1u;                   /* a run lasts ~3 events */
        if (p <= 0x7FFFFFu) return tx_cloud_exp_q8(c, (p * m) >> 8);
        p = (p >> 9) * m;                                    /* (slow streams: 32-bit products) */
        return tx_cloud_exp_q8(c, p > 0x7FFFFFFFu ? 0xFFFFFFFEu : p << 1);
    }
    x = tx_cloud_exp_q8(c, c->period_q8);
    if (c->jitter_q8 >= 256u) return x;
    return (uint32_t)(((uint64_t)c->period_q8 * (256u - c->jitter_q8) + (uint64_t)x * c->jitter_q8) >> 8);
}

/* ---- configuration */
void tx_cloud_init(tx_cloud_t *c, uint32_t seed)
{
    tx_zero(c, sizeof *c);
    tx_rng_seed(&c->rng, seed);
    tx_cloud_rate(c, 10u << 8);
    c->jitter_q8 = 256;
    tx_cloud_burst(c, 4, 30);
    tx_cloud_sizes(c, 6144, 8u << 12);                       /* alpha 1.5 over 8 octaves of energy */
    tx_cloud_omori(c, 0, 6881, 70, 7168);                    /* off; p 1.68, c 70 ms, gap 1.75 decades (5.8 oct) */
}

void tx_cloud_rate(tx_cloud_t *c, uint32_t rate_q8)
{
    c->period_q8 = rate_q8 ? (TX_FS << 16) / rate_q8 : 0;   /* fs / rate, Q8 */
}

void tx_cloud_regular(tx_cloud_t *c, uint32_t jitter_q8) { c->jitter_q8 = (uint16_t)(jitter_q8 > 256u ? 256u : jitter_q8); }
void tx_cloud_clump(tx_cloud_t *c, uint32_t amount) { c->clump = (uint8_t)(amount > 255u ? 255u : amount); }

void tx_cloud_burst(tx_cloud_t *c, uint32_t n, uint32_t spread_ms)
{
    if (n > 255u) n = 255u;
    if (spread_ms > 2000u) spread_ms = 2000u;
    c->burst_n = (uint8_t)n;
    c->burst_q8 = n ? spread_ms * 11290u / n : 0;            /* 44100 * 256 / 1000 = 11289.6 */
}

void tx_cloud_sizes(tx_cloud_t *c, uint32_t alpha_q12, uint32_t range_l2_q12)
{
    uint32_t a1 = alpha_q12 > 4096u + 41u ? alpha_q12 - 4096u : 41u;   /* alpha >= 1.01 */
    c->size_inv_q12 = (int32_t)((1u << 24) / a1);
    c->size_max_l2 = (int32_t)range_l2_q12;
}

void tx_cloud_omori(tx_cloud_t *c, uint32_t n, uint32_t p_q12, uint32_t c_ms, uint32_t gap_l2_q12)
{
    uint32_t p1 = p_q12 > 4096u + 41u ? p_q12 - 4096u : 41u;
    c->after_n = (uint8_t)(n > 16u ? 16u : n);
    c->after_inv_q12 = (int32_t)((1u << 24) / p1);
    c->after_c = c_ms * 441u / 10u;
    c->after_gap_l2 = (int32_t)gap_l2_q12;
}

/* ---- the pending store */
static void pend_push(tx_cloud_t *c, uint32_t t, int32_t size, uint8_t note, uint8_t vel, uint8_t kind, uint8_t em)
{
    tx_pend_t *p;
    if (c->npend >= TX_CLOUD_PENDING) { c->pend_full++; return; }
    p = &c->pend[c->npend++];
    p->t = t; p->size_l2 = size; p->note = note; p->vel = vel; p->kind = kind; p->emitter = em;
}

static void aftershocks(tx_cloud_t *c, uint32_t t0, int32_t main_size, uint8_t note, uint8_t vel, uint8_t em)
{
    uint32_t bits = tx_rand(&c->rng), k, cnt = 0;
    int32_t cap = main_size - c->after_gap_l2;
    for (k = 0; k < 2u * c->after_n; k++) cnt += bits >> k & 1u;   /* binomial(2n, 1/2): mean n */
    if (cap < 0) cap = 0;
    for (k = 0; k < cnt; k++) {
        int32_t s = tx_cloud_pareto(c, c->size_inv_q12, c->size_max_l2);
        pend_push(c, t0 + omori_t(c), s > cap ? cap : s, note, vel, TX_EV_AFTER, em);
    }
}

/* ---- keys */
void tx_cloud_key(tx_cloud_t *c, uint8_t note, uint8_t vel, uint8_t on)
{
    unsigned i, pick = TX_CLOUD_EMITTERS, ko = c->koff;   /* (ko: TX_PID_KEYAT, one-shot) */
    tx_emitter_t *e;
    c->koff = 0;
    if (!on) {
        for (i = 0; i < TX_CLOUD_EMITTERS; i++)
            if (c->em[i].held && c->em[i].note == note) {
                c->em[i].held = 0;
                if (!c->rel_k) c->em[i].fade = 0;            /* (RELEASE: the stream fades out instead) */
            }
        return;
    }
    for (i = 0; i < TX_CLOUD_EMITTERS && pick == TX_CLOUD_EMITTERS; i++)   /* the same key again, else a free one */
        if (c->em[i].held && c->em[i].note == note) pick = i;
    for (i = 0; i < TX_CLOUD_EMITTERS && pick == TX_CLOUD_EMITTERS; i++)
        if (!c->em[i].held && !c->em[i].fade) pick = i;
    for (i = 0; i < TX_CLOUD_EMITTERS && pick == TX_CLOUD_EMITTERS; i++)   /* else a fading one */
        if (!c->em[i].held) pick = i;
    if (pick == TX_CLOUD_EMITTERS) pick = c->em_rr++ % TX_CLOUD_EMITTERS;   /* all held: take the next in turn */
    e = &c->em[pick];
    e->note = note; e->vel = vel ? vel : 1u; e->held = 1; e->fade = 32767;
    e->next_q8 = c->period_q8 ? stream_gap(c, e) + (ko << 8) : 0;   /* (from the press's own sample) */
    {
        uint32_t acc = ko << 8;                              /* the burst: MAIN at once, then Poisson spaced */
        for (i = 0; i < c->burst_n; i++) {
            int32_t s = tx_cloud_pareto(c, c->size_inv_q12, c->size_max_l2);
            pend_push(c, c->now + (acc >> 8), s, note, e->vel, i ? TX_EV_BURST : TX_EV_MAIN, (uint8_t)pick);
            acc += tx_cloud_exp_q8(c, c->burst_q8);
        }
    }
}

unsigned tx_cloud_held(const tx_cloud_t *c)
{
    unsigned i, n = 0;
    for (i = 0; i < TX_CLOUD_EMITTERS; i++) n += c->em[i].held;
    return n;
}

uint32_t tx_cloud_level(const tx_cloud_t *c)
{
    unsigned i;
    uint32_t n = 0;
    for (i = 0; i < TX_CLOUD_EMITTERS; i++) n += c->em[i].held ? 32767u : c->em[i].fade;
    return n;
}

int tx_cloud_idle(const tx_cloud_t *c) { return !c->npend && !tx_cloud_level(c); }

/* ---- one block: the due events, sorted by offset, at most TX_CLOUD_EV_MAX */
static void emit(tx_cloud_t *c, tx_event_t *ev, unsigned *k, unsigned *drop, uint32_t off, int32_t size,
                 uint8_t note, uint8_t vel, uint8_t kind, uint8_t em)
{
    if (c->after_n && (kind == TX_EV_MAIN || (kind == TX_EV_STREAM && c->after_stream)))
        aftershocks(c, c->now + off, size, note, vel, em);
    if (*k >= TX_CLOUD_EV_MAX) { (*drop)++; return; }
    ev[*k].offset = (uint16_t)off; ev[*k].note = note; ev[*k].vel = vel;
    ev[*k].kind = kind; ev[*k].emitter = em; ev[*k].size_l2 = size;
    (*k)++;
}

TX_AUDIO_CODE unsigned tx_cloud_block(tx_cloud_t *c, tx_event_t *ev, unsigned n)
{
    unsigned i, k = 0, drop = 0;
    for (i = 0; i < TX_CLOUD_EMITTERS; i++) {               /* streams of the held keys */
        tx_emitter_t *e = &c->em[i];
        unsigned guard = 0;
        if (!e->held) {                                      /* released: fading (RELEASE), else stopped */
            if (!e->fade) continue;
            e->fade = (uint16_t)(e->fade - (e->fade >> c->rel_k) - 1u);
            if (e->fade < 256u) { e->fade = 0; continue; }   /* -42 dB: stopped */
        }
        if (!c->period_q8) continue;
        while ((e->next_q8 >> 8) < n && guard++ < TX_STREAM_GUARD) {
            if (e->held || (tx_rand(&c->rng) >> 17) < e->fade)   /* thinned by the fade (a Poisson stream stays one) */
                emit(c, ev, &k, &drop, e->next_q8 >> 8, tx_cloud_pareto(c, c->size_inv_q12, c->size_max_l2),
                     e->note, e->vel, TX_EV_STREAM, (uint8_t)i);
            e->next_q8 += stream_gap(c, e) + 1u;
        }
        e->next_q8 = (e->next_q8 >> 8) >= n ? e->next_q8 - (n << 8) : 0;
    }
    for (i = 0; i < c->npend;) {                             /* bursts and aftershocks due in this block */
        tx_pend_t p = c->pend[i];
        int32_t d = (int32_t)(p.t - c->now);
        if (d < (int32_t)n) {
            c->pend[i] = c->pend[--c->npend];                /* (aftershocks it spawns are appended: seen below) */
            emit(c, ev, &k, &drop, d < 0 ? 0u : (uint32_t)d, p.size_l2, p.note, p.vel, p.kind, p.emitter);
        } else
            i++;
    }
    for (i = 1; i < k; i++) {                                /* insertion sort by offset (k <= 32) */
        tx_event_t t = ev[i];
        unsigned j = i;
        while (j && ev[j - 1].offset > t.offset) { ev[j] = ev[j - 1]; j--; }
        ev[j] = t;
    }
    c->now += n;
    c->total += k;
    c->last_events = (uint16_t)k;
    c->last_dropped = (uint16_t)drop;
    return k;
}

/* ---- the hero pool */
void tx_pool_init(tx_pool_t *p, uint32_t max, uint32_t limit)
{
    unsigned i;
    if (max > TX_POOL_MAX) max = TX_POOL_MAX;
    if (max < 1u) max = 1u;
    for (i = 0; i < TX_POOL_MAX; i++) p->level[i] = 0;
    p->alive = p->young = p->young2 = 0; p->steals = p->rejected = 0;
    p->max = (uint8_t)max;
    p->limit = (uint8_t)(limit < 1u ? 1u : limit > max ? max : limit);
}

int tx_pool_take(tx_pool_t *p, uint32_t level, uint32_t *stolen_level)
{
    unsigned i, q = TX_POOL_MAX;
    uint32_t lo = 0xFFFFFFFFu;
    *stolen_level = 0;
    for (i = 0; i < p->limit; i++)
        if (!(p->alive >> i & 1u)) { p->alive |= 1u << i; p->young |= 1u << i; p->level[i] = level; return (int)i; }
    for (i = 0; i < p->limit; i++)                           /* full: the quietest settled hero, if quieter */
        if (!((p->young | p->young2) >> i & 1u) && p->level[i] < lo) { lo = p->level[i]; q = i; }
    if (q == TX_POOL_MAX || level <= lo) { p->rejected++; return -1; }
    p->steals++;
    p->young |= 1u << q;
    *stolen_level = lo;
    p->level[q] = level;
    return (int)q;
}

uint32_t tx_pool_set_limit(tx_pool_t *p, uint32_t limit)
{
    uint32_t keep, cut;
    unsigned i;
    if (limit < 1u) limit = 1u;
    if (limit > p->max) limit = p->max;
    p->limit = (uint8_t)limit;
    keep = limit >= 32u ? 0xFFFFFFFFu : (1u << limit) - 1u;
    cut = p->alive & ~keep;
    for (i = limit; i < TX_POOL_MAX; i++) p->level[i] = 0;
    p->alive &= keep;
    p->young &= keep; p->young2 &= keep;
    return cut;
}

unsigned tx_pool_count(const tx_pool_t *p)
{
    uint32_t a = p->alive;
    unsigned n = 0;
    while (a) { n += a & 1u; a >>= 1; }
    return n;
}
