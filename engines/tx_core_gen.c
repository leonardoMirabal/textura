/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura GEN: a lane's generator (tx_core_gen.h). */
#include "tx_core_gen.h"

const tx_param_t tx_gen_params[TX_GEN_NP] = {
    {"ACTIVITY", 127, 64, "the lane's activity (the fader): how much happens; EVOLVE wanders around it (+-20)"},
    {"ROOT", 127, 60, "the lowest note of the lane (MIDI; 60 = C4): its pitch sets the source's size, as a key"},
    {"SPAN", 36, 12, "the lane's range above ROOT: 0 .. 36 semitones (chromatic)"},
    {"WANDER", 127, 64, "how the pitch moves: anywhere in the span .. a walk of 1-3 semitones (64) .. close to a drifting centre"},
    {"RANDOMNESS", 127, 64, "the timing: steady .. random (64, Poisson) .. bursty and clumped (flurries and silences)"},
    {"EVOLVE", 127, 64, "the horizon of the slow change: 10 s .. 2 h (log; 64 ~ 4 min): a 1/f wander up to it"},
    {"RECALL", 127, 50, "memory: how often the lane returns to a stretch it has played, and replays its motifs (0 .. 1)"},
    {"MUTATE", 127, 40, "how much a return varies: an exact return .. a loose one (notes moved, the walk kept)"},
};

/* 1 / sqrt(k) for k = 1 .. 14, Q15 */
static const uint16_t INV_SQRT[TX_GEN_K + 1] = { 0, 32767, 23170, 18919, 16384, 14654, 13377, 12385, 11585, 10923, 10362,
                                                  9880, 9459, 9088, 8757 };

static int32_t clamp32(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }

static int32_t gauss12(tx_rng_t *r)                        /* N(0, 1), Q12 */
{
    uint32_t a = tx_rand(r), b = tx_rand(r);
    int32_t s = (int32_t)(a >> 16) + (int32_t)(a & 0xFFFFu) + (int32_t)(b >> 16) + (int32_t)(b & 0xFFFFu) - 131072;
    return (s * 3547) >> 15;
}

static uint32_t exp16(tx_rng_t *r)                          /* Exp(1), Q16 */
{
    return (tx_neglog2_q12(tx_rand(r)) * 2839u) >> 8;
}

/* an OU step with a per-step constant of 172 (each walk is stepped every 2^k blocks: its time constant 2^k s) */
static int32_t ou(tx_rng_t *r, int32_t w)
{
    w += -((w * 381) >> 16) + ((gauss12(r) * 3533) >> 12);
    return clamp32(w, -98304, 98304);
}

/* the horizon: seconds, 10 .. 7200 (log) */
static uint32_t horizon_s(const tx_gen_t *g) { return tx_exp2(217706 + (int32_t)g->p[TX_GEN_EVOLVE] * 4898); }
/* walks within it: 1 .. 14 */
static unsigned kact(const tx_gen_t *g)
{
    int32_t l = 217706 + (int32_t)g->p[TX_GEN_EVOLVE] * 4898;   /* log2 10 + v log2 720 / 127, Q16 */
    unsigned k = (unsigned)(l >> 16) + 1u;
    return k > TX_GEN_K ? TX_GEN_K : k;
}

void tx_gen_init(tx_gen_t *g, uint32_t seed)
{
    uint8_t *b = (uint8_t *)g;
    unsigned i;
    for (i = 0; i < sizeof *g; i++) b[i] = 0;
    tx_rng_seed(&g->rng, seed ^ 0x6E4E4E21u);
    for (i = 0; i < TX_GEN_NP; i++) g->p[i] = tx_gen_params[i].def;
    for (i = 0; i < TX_GEN_K; i++) { g->wa[i] = gauss12(&g->rng) * 6; g->wp[i] = gauss12(&g->rng) * 6; }
    g->a = 64 << 8;
    g->pc = (int32_t)g->p[TX_GEN_SPAN] << 7;
    g->last = g->p[TX_GEN_SPAN] / 2;
    g->thr = 65536;
    g->play = 0xFF;
    g->dec = 10;
}

void tx_gen_param(tx_gen_t *g, unsigned id, unsigned v)
{
    if (id >= TX_GEN_NP) return;
    if (v > tx_gen_params[id].max) v = tx_gen_params[id].max;
    g->p[id] = (uint8_t)v;
}

/* the next note: its pitch (semitones above ROOT) */
static int32_t next_pitch(tx_gen_t *g)
{
    tx_rng_t *r = &g->rng;
    int32_t span = g->p[TX_GEN_SPAN], w = g->p[TX_GEN_WANDER], c = g->pc >> 8, p;
    if (g->play != 0xFF) {                                   /* a motif replayed (moved by MUTATE) */
        const tx_gen_motif_t *m = &g->mot[g->play];
        p = c + m->d[g->pi];
        if ((tx_rand(r) >> 17) < (uint32_t)g->p[TX_GEN_MUTATE] * 129u)   /* probability MUTATE / 2 */
            p += (tx_rand(r) & 1u) ? 1 + (int32_t)(tx_rand(r) & 1u) : -1 - (int32_t)(tx_rand(r) & 1u);
        if (++g->pi >= m->len) g->play = 0xFF;
    } else if (w < 64 && (tx_rand(r) >> 26) < (uint32_t)(64 - w)) {   /* anywhere in the span */
        p = (int32_t)tx_rand_below(r, (uint32_t)span + 1u);
    } else {                                                 /* a walk, pulled toward the centre as WANDER rises */
        int32_t maxs = w <= 64 ? 3 : 3 - (2 * (w - 64)) / 63, st = 1 + (int32_t)tx_rand_below(r, (uint32_t)maxs);
        int32_t pull = w <= 64 ? 0 : ((c - g->last) * (w - 64)) / 126;
        p = g->last + ((tx_rand(r) & 1u) ? st : -st) + pull;
    }
    while (p < 0 || p > span) p = p < 0 ? -p : 2 * span - p;   /* reflect at the edges */
    if (span == 0) p = 0;
    return p;
}

unsigned tx_gen_block(tx_gen_t *g, unsigned n, tx_gen_ev_t *ev, unsigned max, uint8_t *intensity)
{
    tx_rng_t *r = &g->rng;
    unsigned k, ne = 0, ka = kact(g);
    int32_t ea = 0, ep = 0, base = (int32_t)g->p[TX_GEN_ACTIVITY] << 8, span = g->p[TX_GEN_SPAN];
    int32_t aw, pw, ar, pr;
    uint32_t hz = horizon_s(g);
    if (max > TX_GEN_EV_MAX) max = TX_GEN_EV_MAX;
    /* the walks (walk k every 2^k blocks), the 1/f sums over the horizon's walks */
    for (k = 0; k < TX_GEN_K; k++) {
        if (!(g->blk & ((1u << k) - 1u))) { g->wa[k] = ou(r, g->wa[k]); g->wp[k] = ou(r, g->wp[k]); }
        if (k < ka) { ea += g->wa[k]; ep += g->wp[k]; }
    }
    if (!(g->blk & 63u)) g->wr = ou(r, g->wr);
    g->blk++;
    ea = (int32_t)(((int64_t)ea * INV_SQRT[ka]) >> 15);      /* sd ~1 (Q15) whatever the horizon */
    ep = (int32_t)(((int64_t)ep * INV_SQRT[ka]) >> 15);
    aw = base + ((ea * 5120) >> 15);                         /* +-20 knob units */
    pw = (span << 7) + ((ep * (span << 6)) >> 15);           /* the centre +-SPAN / 4 (1 sd) */
    /* the memory: the recalled stretch, glided in and out (~3 s) */
    g->g += ((g->r_len ? 32767 : 0) - g->g) >> 9;
    if (g->g > 64 || g->r_len) {
        int32_t m = g->p[TX_GEN_MUTATE];
        ar = (int32_t)g->hist[(g->r_at + g->r_el) % TX_GEN_HIST] << 8;
        pr = (int32_t)g->phist[((g->r_at / 10u) + g->r_el / 10u) % TX_GEN_PHIST] << 8;
        ar += ((aw - base) * m) / 127;                       /* the walk keeps MUTATE of its motion */
        pr += ((pw - (span << 7)) * m) / 127;
        aw += (int32_t)(((int64_t)(ar - aw) * g->g) >> 15);
        pw += (int32_t)(((int64_t)(pr - pw) * g->g) >> 15);
    }
    g->a = clamp32(aw, 0, 127 << 8);
    if (pw < 0) pw = -pw;                                    /* a soft edge: the centre reflects, never sticks */
    if (pw > (span << 8)) pw = 2 * (span << 8) - pw;
    g->pc = clamp32(pw, 0, span << 8);
    *intensity = g->intensity = (uint8_t)(g->a >> 8);
    /* once a second: record; at decision times, perhaps recall */
    g->sec += n;
    if (g->sec >= TX_FS) {
        g->sec -= TX_FS;
        g->secs++;
        g->hist[g->hpos] = (uint8_t)(g->a >> 8);
        if (!(g->secs % 10u)) { g->phist[g->ppos] = (uint8_t)(g->pc >> 8); g->ppos = (uint16_t)((g->ppos + 1u) % TX_GEN_PHIST); if (g->plen < TX_GEN_PHIST) g->plen++; }
        g->hpos = (uint16_t)((g->hpos + 1u) % TX_GEN_HIST);
        if (g->hlen < TX_GEN_HIST) g->hlen++;
        if (g->r_len && ++g->r_el >= g->r_len) g->r_len = 0;
        if (!g->dec || !--g->dec) {
            uint32_t mean = hz / 32u < 2u ? 2u : hz / 32u, avail = g->hlen < hz ? g->hlen : hz;
            g->dec = 1u + (uint32_t)(((uint64_t)mean * exp16(r)) >> 16);
            if (!g->r_len && avail > 8u && (tx_rand(r) >> 25) < g->p[TX_GEN_RECALL]) {
                uint32_t lo = hz / 16u, hi = hz / 4u, len, back, u1 = tx_rand(r) >> 16, u2 = tx_rand(r) >> 16;
                len = lo + (uint32_t)(((uint64_t)(hi - lo) * (tx_rand(r) >> 16)) >> 16);
                if (len > avail / 2u) len = avail / 2u;
                if (len < 2u) len = 2u;
                back = len + (uint32_t)(((uint64_t)(avail - len) * (u1 > u2 ? u1 : u2)) >> 16);   /* older likelier */
                g->r_at = (g->hpos + TX_GEN_HIST - back) % TX_GEN_HIST;
                g->r_len = len; g->r_el = 0;
            }
        }
    }
    /* releases due in this block */
    for (k = 0; k < g->noff; ) {
        if (g->off[k].wait < n) {
            if (ne < max) { ev[ne].off = (uint16_t)g->off[k].wait; ev[ne].note = g->off[k].note; ev[ne].vel = 0; ne++; }
            g->off[k] = g->off[--g->noff];
        } else { g->off[k].wait -= n; k++; }
    }
    /* the notes: the rate from the activity, steady .. Poisson .. self-exciting */
    {
        int32_t rand_ = clamp32((int32_t)g->p[TX_GEN_RANDOM] + ((g->wr * 16) >> 15), 0, 127);
        uint32_t j16 = rand_ >= 64 ? 65536u : (uint32_t)rand_ * 1024u;
        uint32_t n16 = rand_ > 64 ? (uint32_t)(rand_ - 64) * 832u : 0u;   /* branching 0 .. 0.8 */
        uint32_t lam = tx_exp2(-283234 + ((g->a * 4460) >> 8) + (8 << 16));   /* notes/s Q8: 0.05 .. 20 */
        uint32_t lam0 = (uint32_t)(((uint64_t)lam * (65536u - n16)) >> 16) + (uint32_t)g->exc;
        uint32_t inc = (lam0 * n * 380u) >> 16, prev = g->acc, gap = (TX_FS << 8) / (lam ? lam : 1u);
        g->acc += inc;
        g->exc -= (int32_t)(((uint32_t)g->exc * 950u) >> 16) + (g->exc > 0);   /* tau 0.4 s */
        if (g->exc < 0) g->exc = 0;
        while (g->acc >= g->thr && ne < max) {
            unsigned off = inc ? (unsigned)(((g->thr > prev ? g->thr - prev : 0u) * n) / inc) : 0u;   /* (< 2^30) */
            int32_t p, vel;
            uint32_t dur;
            if (off >= n) off = n - 1u;
            g->acc -= g->thr; prev = 0;
            g->thr = (65536u - j16) + (uint32_t)(((uint64_t)j16 * exp16(r)) >> 16);
            if (g->thr < 64u) g->thr = 64u;
            if (g->thr > (6u << 16)) g->thr = 6u << 16;       /* (an Exp over 6: once in 400; the integral cap above it) */
            g->exc += (int32_t)(n16 / 102u);                 /* each note: alpha = n / tau */
            if (g->exc > 16384) g->exc = 16384;
            /* a phrase: notes closer than twice the mean gap; a long gap ends it (kept as a motif) */
            if (g->since > 2u * gap) {
                if (g->mrec_len >= 2u) {
                    tx_gen_motif_t *m = &g->mot[g->nmot < TX_GEN_MOT ? g->nmot++ : tx_rand_below(r, TX_GEN_MOT)];
                    for (k = 0; k < g->mrec_len; k++) m->d[k] = g->mrec[k];
                    m->len = g->mrec_len;
                }
                g->mrec_len = 0;
                g->play = 0xFF;
                if (g->nmot && (tx_rand(r) >> 25) < g->p[TX_GEN_RECALL]) { g->play = (uint8_t)tx_rand_below(r, g->nmot); g->pi = 0; }
            }
            g->since = 0;
            p = next_pitch(g);
            g->last = p;
            if (g->mrec_len < TX_GEN_MLEN) g->mrec[g->mrec_len++] = (int8_t)clamp32(p - (g->pc >> 8), -36, 36);
            vel = clamp32(64 + ((g->a - (64 << 8)) >> 9) + (int32_t)tx_rand_below(r, 41u) - 20, 1, 127);
            dur = (gap * 4u) / 5u;
            dur = dur < 1323u ? 1323u : dur > 66150u ? 66150u : dur;
            if (g->noff >= TX_GEN_OFFS) {                    /* too many sounding: the oldest released now */
                if (ne < max) { ev[ne].off = (uint16_t)off; ev[ne].note = g->off[0].note; ev[ne].vel = 0; ne++; }
                g->off[0] = g->off[--g->noff];
            }
            if (ne < max) {
                ev[ne].off = (uint16_t)off; ev[ne].note = (uint8_t)clamp32((int32_t)g->p[TX_GEN_ROOT] + p, 0, 127);
                ev[ne].vel = (uint8_t)vel; ne++;
                g->off[g->noff].wait = off + dur; g->off[g->noff].note = ev[ne - 1].note; g->noff++;
                g->notes++;
            }
        }
        if (g->acc > (16u << 16)) g->acc = 16u << 16;
        g->since += n;
    }
    return ne;
}

unsigned tx_gen_stop(tx_gen_t *g, tx_gen_ev_t *ev, unsigned max)
{
    unsigned ne = 0;
    while (g->noff && ne < max) { g->noff--; ev[ne].off = 0; ev[ne].note = g->off[g->noff].note; ev[ne].vel = 0; ne++; }
    return ne;
}
