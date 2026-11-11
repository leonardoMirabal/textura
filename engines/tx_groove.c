/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura GROOVE: the TYPES and the density (tx_groove.h) */
#include "tx_groove.h"

typedef struct { uint8_t span, n, sw; uint32_t hit, acc; } gr_type_t;   /* a TYPE: n 0, a polyrhythm (its cycles
                                                                        * in hit), else a grid of n steps */
#define TX_GR_NAMED 73u
#define GR_ONCE __attribute__((noinline))   /* (helpers kept out of line: the image's flash, not their speed) */
#define GR_SMALL __attribute__((minsize))   /* (the UI's and the per-block bookkeeping: compiled for size) */
/* the polyrhythms (TYPES 1 .. 32): the cycles over the span for PULSE, OFFBEAT, FILL (5 bits each), the span (beats)
 * at bit 15, bit 19: the FILL's cycle named too ("3:4:5"; else "3:4") */
#define GR_NP 32u
static const uint32_t GR_P[GR_NP] = {
    0x23083u, 0x23064u, 0x21843u, 0x21862u, 0xA1483u, 0xA1462u, 0xA1CA3u, 0xA18A4u, 0xA1062u, 0xA1C83u, 0x22885u,
    0x228A4u, 0x23C65u, 0x23CA3u, 0x23887u, 0x238E4u, 0x23CC5u, 0x23CA6u, 0x238E5u, 0x23CA7u, 0x43907u, 0x438E8u,
    0x43109u, 0x22845u, 0x43103u, 0xA0C86u, 0xC24E5u, 0xC24C4u, 0x238E3u, 0x228A2u, 0x44105u, 0x4410Bu,
};
/* the grids (TYPE 0, then 33 .. 72): the form (bits 0 .. 2: span and steps, GR_FM) and the swing (bits 3 .. 5,
 * GR_SW); the steps: bit i step i, acc strong, hit medium, the rest weak; their names in 5-bit codes
 * (tools/gen_groove_names.py: 0 a name's end, 1 .. 26 A .. Z, 27 .. 30 " -+/", 31 a digit, its value next; the SWINGs
 * of 8 steps named by their swing) */
#define GR_NG 41u
static const uint8_t GR_FM[8][2] = {{4, 16}, {4, 8}, {4, 12}, {8, 32}, {4, 14}, {4, 10}, {3, 6}, {6, 12}};
static const uint8_t GR_SW[8] = {50, 54, 58, 62, 66, 70, 75, 60};
static const uint8_t GR_G[GR_NG] = {
    0, 1, 9, 17, 25, 33, 41, 49, 56, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0, 0, 0, 3, 3, 2, 0, 2,
    4, 5, 6, 7, 0,
};
static const uint16_t GR_GB[GR_NG - 6u][2] = {   /* (hit, acc: their first 16 steps; GR_GB32 the rest of a 32; the
                                                  * SWINGs of 8 steps 2 .. 7 STRAIGHT 8's, 1) */
    {0x5555, 0x1111}, {0xFF, 0x55}, {0x5555, 0x1111}, {0xB6D, 0x249}, {0xFFF, 0x249}, {0x1249, 0x1041}, {0x1111, 0x1}, {0x1449, 0x41},
    {0x4914, 0x4100}, {0x1489, 0x81}, {0x8914, 0x8100}, {0x2449, 0x1}, {0x4949, 0x101}, {0x6D6D, 0x101},
    {0x5D5D, 0x101}, {0x4949, 0x4141}, {0xAB5, 0x1}, {0x5AD, 0x1}, {0x110, 0x100}, {0x4949, 0x101}, {0x5555, 0x4444},
    {0x1441, 0x401}, {0x1449, 0x1}, {0x1451, 0x1}, {0xC49, 0x1}, {0x1449, 0x41}, {0x2449, 0x1}, {0xB6D, 0x41},
    {0x4C49, 0x1}, {0x295, 0x1}, {0xA95, 0x81}, {0x129, 0x21}, {0x15, 0x1}, {0x6DB, 0x41}, {0x1111, 0x101},
};
static const uint16_t GR_GB32[2][2] = {{0x1489, 0x80}, {0x2449, 0x0}};   /* (SON MONTUNO, PARTIDO: steps 16 .. 31) */
static const uint8_t GR_GN[197] = {
    0xec, 0xbd, 0x59, 0xf6, 0x0f, 0xdf, 0x80, 0x49, 0x65, 0x48, 0x07, 0xd1, 0xfd, 0x11, 0x98, 0x37, 0xb9, 0xb3, 0x7f,
    0xf8, 0x06, 0x4c, 0x54, 0x8d, 0x61, 0x05, 0x50, 0x99, 0x20, 0x2b, 0x74, 0x02, 0xf2, 0x28, 0x2d, 0x04, 0xa0, 0xc0,
    0x0c, 0xa7, 0xa9, 0x15, 0x30, 0x58, 0xb0, 0x65, 0xff, 0xc1, 0xbf, 0x00, 0x83, 0x05, 0x5b, 0xf6, 0x17, 0xfc, 0x0f,
    0x20, 0x6b, 0x13, 0x61, 0xff, 0xc1, 0xbf, 0x00, 0xb2, 0x36, 0x11, 0xf6, 0x17, 0xfc, 0x0f, 0x20, 0xde, 0x9c, 0x01,
    0x50, 0x59, 0x66, 0x62, 0xec, 0x81, 0x91, 0x5c, 0xac, 0x89, 0xb1, 0x07, 0x66, 0x68, 0x22, 0x00, 0x11, 0x52, 0x78,
    0xe0, 0x87, 0x2f, 0xfc, 0x47, 0x5b, 0x14, 0xc6, 0x80, 0x28, 0x4d, 0x14, 0xf0, 0x5c, 0xd9, 0x44, 0x3e, 0x08, 0x48,
    0x69, 0xe2, 0x5d, 0xf0, 0x8c, 0x11, 0x25, 0x50, 0x20, 0x64, 0x09, 0x6b, 0x82, 0x13, 0x50, 0x05, 0x13, 0xa5, 0xf5,
    0xc0, 0x7c, 0x75, 0xbd, 0x3a, 0xc1, 0x7c, 0x6e, 0xb7, 0xe7, 0x68, 0x75, 0x0f, 0xc0, 0x20, 0x69, 0x22, 0x0f, 0xb8,
    0xe0, 0xd2, 0x79, 0x60, 0xc1, 0xe0, 0xd8, 0x3b, 0x0f, 0x98, 0xda, 0xca, 0x36, 0xb5, 0x15, 0x30, 0x8b, 0x2d, 0x6e,
    0x7f, 0xd1, 0xbf, 0xe8, 0x7f, 0x00, 0x93, 0x6c, 0xd9, 0x7f, 0xf4, 0x2f, 0xc0, 0x0d, 0x8c, 0x6a, 0xa0, 0xd2, 0x01,
    0x93, 0xbd, 0xbb, 0x3f, 0x01, 0x00, 0x00,
};
#define GR_F 8                      /* the floor (Q5 of a slot's full weight): events between slots too */
static const uint8_t GR_LV[3][3] = {{0, 8, 32}, {6, 32, 4}, {28, 8, 0}};   /* PULSE, OFFBEAT, FILL: weak, medium,
                                                                            * strong steps (Q5) */
static const uint8_t GR_W[4] = {12, 12, 9, 6};      /* the bump's half width by the sound's weight (ticks): loose ..
                                                     * tight */
static const uint16_t GR_WF[4] = {0, 13107, 22938, 32768};   /* the depth by the sound's weight (Q15) */

unsigned tx_groove_ntypes(void) { return TX_GR_NTYPES; }
static GR_SMALL void gr_get(unsigned t, gr_type_t *o)        /* TYPE t (an E(k, n) made here) */
{
    unsigned n = TX_GR_EU_N0, k, i, r;
    if (t && t <= GR_NP) {                          /* a polyrhythm */
        uint32_t v = GR_P[t - 1u];
        o->span = (uint8_t)(v >> 15 & 15u); o->n = 0; o->sw = (uint8_t)(v >> 19); o->acc = 0;
        o->hit = (v & 31u) | (v >> 5 & 31u) << 8 | (v >> 10 & 31u) << 16;
        return;
    }
    if (t < TX_GR_NAMED) {                          /* a grid */
        t -= t ? GR_NP : 0u;
        o->span = GR_FM[GR_G[t] & 7u][0]; o->n = GR_FM[GR_G[t] & 7u][1]; o->sw = GR_SW[GR_G[t] >> 3];
        k = t < 2u ? t : t < 8u ? 1u : t - 6u;
        o->hit = GR_GB[k][0]; o->acc = GR_GB[k][1];
        if (o->n > 16u) { o->hit |= (uint32_t)GR_GB32[t - 31u][0] << 16; o->acc |= (uint32_t)GR_GB32[t - 31u][1] << 16; }
        return;
    }
    t -= TX_GR_NAMED;
    while (n < TX_GR_EU_N1 && t >= n - 1u) { t -= n - 1u; n++; }
    k = t + 1u;
    o->span = 4; o->n = (uint8_t)n; o->sw = 50; o->hit = 0; o->acc = 1;
    for (i = 0, r = 0; i < n; i++, r = r + k >= n ? r + k - n : r + k) if (r < k) o->hit |= 1u << i;   /* (the even
                                                     * spread of k over n: step i where i k mod n < k) */
}
static GR_ONCE unsigned gr_pop(uint32_t v)                  /* the bits set */
{
    unsigned c = 0;
    for (; v; v &= v - 1u) c++;
    return c;
}
static GR_ONCE char *gr_num(char *s, unsigned n)
{
    if (n >= 10u) *s++ = (char)('0' + n / 10u);
    *s++ = (char)('0' + n % 10u);
    return s;
}
static GR_ONCE unsigned gr_c5(unsigned i)          /* GR_GN's code i */
{
    unsigned b = i * 5u;
    return ((unsigned)GR_GN[b >> 3] | (unsigned)GR_GN[(b >> 3) + 1u] << 8) >> (b & 7u) & 31u;
}
GR_SMALL void tx_groove_name(unsigned t, char *s)
{
    gr_type_t g;
    unsigned k, i, c;
    const char *p;
    if (t >= TX_GR_NTYPES) t = 0;
    gr_get(t, &g);
    if (!g.n)                                       /* a polyrhythm: its cycles, "3:4", "3:4:5" */
        for (k = 0; k < 2u + g.sw; k++) { if (k) *s++ = ':'; s = gr_num(s, g.hit >> (8u * k) & 255u); }
    else if (t >= TX_GR_NAMED) {                    /* E(k, n) */
        *s++ = 'E'; *s++ = '('; s = gr_num(s, gr_pop(g.hit)); *s++ = ','; s = gr_num(s, g.n); *s++ = ')';
    } else if (g.n == 8u && g.sw != 50u) {          /* SWING 54 .. 75 */
        for (p = "SWING "; *p; ) *s++ = *p++;
        s = gr_num(s, g.sw);
    } else {                                        /* a grid: its name in GR_GN */
        for (k = t ? t - GR_NP : 0u, k -= k > 7u ? 6u : 0u, i = 0; k; i++) if (!(c = gr_c5(i))) k--; else i += c == 31u;
        while ((c = gr_c5(i++)) != 0) *s++ = c == 31u ? (char)('0' + gr_c5(i++)) : c < 27u ? (char)('@' + c) : " -+/"[c - 27u];
    }
    *s = 0;
}
GR_SMALL void tx_groove_lane_init(tx_groove_lane_t *l, uint32_t seed)
{
    tx_zero(l, sizeof *l);
    tx_rng_seed(&l->rng, seed);
    l->b = l->bt = 22938;
    l->bar = 0xFFFFFFFFu;
}

/* the lane's slots: n of them over the span S (ticks); a grid's (c 0) or c even pulses; slot j's place and weight */
typedef struct { uint32_t S, c, n, part, hit, acc, sw; } gr_ctx_t;
static GR_SMALL uint32_t gr_ctx(const tx_groove_lane_t *l, unsigned type, gr_ctx_t *x)   /* the mean density (Q8; 0: no pulse) */
{
    gr_type_t g;
    unsigned p = l->part;
    uint32_t sum, m;
    if (!p || p >= TX_GRP_FREE) return 0;
    gr_get(type < TX_GR_NTYPES ? type : 0u, &g);
    x->part = p - 1u;
    x->S = g.span * 96u;
    x->hit = g.hit; x->acc = g.acc; x->sw = g.sw;
    x->c = l->cycle >= 2u ? l->cycle : g.n ? 0u : g.hit >> (8u * x->part) & 255u;
    x->n = x->c ? x->c : g.n;
    if (x->c) sum = 32u * x->c + 8u;                /* (the slots' weights: the first 40, the rest 32) */
    else {                                          /* (strong, medium, weak steps) */
        m = x->n < 32u ? (1u << x->n) - 1u : 0xFFFFFFFFu;
        p = gr_pop(x->acc & m);
        m = gr_pop(x->hit & ~x->acc & m);
        sum = p * GR_LV[x->part][2] + m * GR_LV[x->part][1] + (x->n - p - m) * GR_LV[x->part][0];
    }
    return (GR_F << 8) + ((sum * GR_W[l->wl & 3u]) << 8) / x->S;
}
static uint32_t gr_pos(const gr_ctx_t *x, uint32_t j)
{
    uint32_t p = j * x->S / x->n;
    if (!x->c && (j & 1u)) p += (x->sw - 50u) * x->S / (50u * x->n);   /* (swing: the off steps later) */
    return p;
}
static uint32_t gr_w(const gr_ctx_t *x, uint32_t j)
{
    if (x->c) return j ? 32u : 40u;
    return GR_LV[x->part][(x->acc >> j & 1u) ? 2 : x->hit >> j & 1u];
}
static uint32_t gr_dist(const gr_ctx_t *x, uint32_t p, uint32_t j)   /* the place p's distance from slot j (ticks) */
{
    uint32_t ps = gr_pos(x, j), d = p > ps ? p - ps : ps - p;
    return d > x->S / 2u ? x->S - d : d;
}
static uint32_t gr_place(const tx_groove_lane_t *l, const gr_ctx_t *x, unsigned shift, uint32_t tick)
{
    uint32_t st = x->c ? x->S / 32u : x->S / x->n;  /* (SHIFT: a grid's steps, a cycle's 32ths of the span) */
    return (tick + shift * st + (uint32_t)((l->ph >> 16) + 4096 * 96)) % x->S;
}

int32_t tx_groove_mul(tx_groove_lane_t *l, unsigned type, unsigned amount, unsigned shift, uint32_t pos_q8)
{
    gr_ctx_t x;
    uint32_t tick = pos_q8 >> 8, bar = tick / 384u, n, W, gq, p, mq = gr_ctx(l, type, &x);
    int32_t dt, dd;
    int on = mq && l->wl;
    dt = on ? (int32_t)(((amount * 258u * GR_WF[l->wl & 3u]) >> 15) * (uint32_t)l->b >> 15) : 0;
    dd = dt - l->d;
    l->d += dd > 64 ? 64 : dd < -64 ? -64 : dd;
    if (!l->d || !on) { if (!on) l->d = 0; return 4096; }
    if (bar != l->bar) {                            /* a bar: a new breath to drift to, the phase's drift turns */
        l->bar = bar;
        l->bt = 11469 + (int32_t)((tx_rand(&l->rng) >> 17) * 21299u >> 15);
        l->dph += tx_rand_s15(&l->rng) >> 6;
        if (l->ph > (16 << 16)) l->dph -= 256; else if (l->ph < -(16 << 16)) l->dph += 256;
        l->dph = l->dph > 768 ? 768 : l->dph < -768 ? -768 : l->dph;
    }
    l->b += (l->bt - l->b) >> 9;
    l->ph += l->dph;
    n = x.n;
    W = GR_W[l->wl & 3u];
    p = gr_place(l, &x, shift, tick);
    gq = GR_F << 8;
    {                                               /* the near slots' bumps: a bump is narrower than half a step and a
                                                     * swing moves a step half a step at most, so the step at or before
                                                     * the place and its two neighbours are all that can be near */
        uint32_t j0 = p * n / x.S, m = n < 4u ? n : 3u, i, j, d;
        for (i = 0; i < m; i++) {
            j = n < 4u ? i : (j0 + n - 1u + i) % n;
            if ((d = gr_dist(&x, p, j)) < W) gq += (gr_w(&x, j) * (W - d) << 8) / W;
        }
    }
    return 4096 + (int32_t)(((int32_t)((gq << 12) / mq) - 4096) * l->d >> 15);
}
int32_t tx_groove_dist(const tx_groove_lane_t *l, unsigned type, unsigned shift, uint32_t tick)
{
    gr_ctx_t x;
    uint32_t j, p, w, wm = 1, best = 0x7FFFFFFFu;
    if (!gr_ctx(l, type, &x)) return 0x7FFFFFFF;
    p = gr_place(l, &x, shift, tick);
    for (j = 0; j < x.n; j++) if ((w = gr_w(&x, j)) > wm) wm = w;   /* (its pulse: the part's heaviest slots) */
    for (j = 0; j < x.n; j++) if (gr_w(&x, j) * 4u >= wm * 3u && gr_dist(&x, p, j) < best) best = gr_dist(&x, p, j);
    return (int32_t)best;
}
GR_SMALL unsigned tx_groove_slots(const tx_groove_lane_t *l, unsigned type, uint8_t *pos, uint8_t *w, unsigned max, unsigned *beats)
{
    gr_ctx_t x;
    gr_type_t g;
    unsigned j;
    gr_get(type < TX_GR_NTYPES ? type : 0u, &g);
    *beats = g.span;
    if (!gr_ctx(l, type, &x)) return 0;
    for (j = 0; j < x.n && j < max; j++) { pos[j] = (uint8_t)(gr_pos(&x, j) * 256u / x.S); w[j] = (uint8_t)gr_w(&x, j); }
    return j;
}
