/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura MIX core: four layers (-> their inserts) -> pan -> stereo -> the compressor -> the DRIVE (and the FILTER)
 * -> CHAMBER (mid send) -> the bus effects (grain delay, reverb) -> M/S -> linked limiter (tx_core_mix.h). */
#include "tx_core_mix.h"
#ifdef DINGHY_HOST
uint8_t tx_st_seen[8], tx_st_n;
#endif

void tx_mix_init(tx_mix_t *m)
{
    unsigned k, i;
    tx_zero(m, sizeof *m);
    for (k = 0; k < TX_MIX_SLOTS; k++) {
        tx_mix_pan(m, k, 64); m->tg[k] = 4096; m->gc[k] = m->gt[k] = 32768; m->lc[k] = m->lv[k] = 32768;
        for (i = 0; i < 2u; i++) {                           /* (the ears at rest where they aim) */
            m->bin[k].e[i].d = m->bin[k].e[i].dt; m->bin[k].e[i].a = m->bin[k].e[i].at;
            m->bin[k].e[i].b = m->bin[k].e[i].bt; m->bin[k].e[i].g = m->bin[k].e[i].gt;
        }
        m->bin[k].c = m->bin[k].ct;
        m->bin[k].run = 0;
    }
    m->amt = TX_MIX_AMT_DEF; m->head = TX_MIX_HEAD_MEDIUM;
    tx_comp_init(&m->comp);
    tx_ms_init(&m->ms);
    tx_limit_init(&m->lim);
    m->lim.ch = 2;                                           /* (interleaved: tx_mix_end) */
}

void tx_mix_set(tx_mix_t *m, unsigned k, const tx_engine_t *e, void *st, uint32_t seed)
{
    if (k >= TX_MIX_SLOTS) return;
    m->e[k] = e; m->st[k] = st; m->slot_peak[k] = 0;
    if (e && st) e->init(st, seed);
}

/* each head size's (tx_core_mix.h TX_MIX_HEAD_*): its time difference's scale, r (1 + pi/2) / c in samples / (pi/2 + 1)
 * << 16 >> 15, x 1024 (MEDIUM, r = 8.8 cm: 29.1 samples at a hard pan, 23182; SMALL 7.6 cm: 25.1; LARGE 10.0 cm: 33.1),
 * its largest whole delay, and its shadow's depth: 1 - the low-pass coefficient at a hard pan, Q15 (MEDIUM: 1 - exp(-2 pi
 * 1800 / 44100) = 0.226 = 7406; SMALL 2084 Hz: 8419; LARGE 1584 Hz: 6620) */
static const uint16_t TX_HEAD_ITD[TX_MIX_HEAD_N] = {20021u, 23182u, 26343u};
static const uint8_t TX_HEAD_DMAX[TX_MIX_HEAD_N] = {25u, 29u, 33u};
static const uint16_t TX_HEAD_SHADOW[TX_MIX_HEAD_N] = {24349u, 25362u, 26148u};
/* the far ear's crossover by its delay D (whole samples, 1 .. 33): the coefficient of a one-pole low-pass at fs / 4 D,
 * 1.5 kHz at most, 1 - exp(-2 pi fc / fs), Q15 (D 29: 380 Hz; D 7 and under: 1.5 kHz), run twice on the slot. Below it
 * the far ear is late (the time difference: a quarter of a cycle at the corner, never the half that cancels in mono),
 * above it the far ear's highs come at once, its level and shadow alone telling where the sound is (as hearing does:
 * above ~1.5 kHz a time difference is ambiguous). The far ear: the sample less its low band, plus its low band late */
static const uint16_t TX_XO[34] = {32768u, 6305u, 6305u, 6305u, 6305u, 6305u, 6305u, 6305u, 5842u, 5248u, 4763u, 4361u,
                                   4020u, 3730u, 3478u, 3258u, 3064u, 2892u, 2738u, 2600u, 2475u, 2362u, 2258u, 2163u,
                                   2076u, 1996u, 1921u, 1852u, 1788u, 1728u, 1672u, 1619u, 1570u, 1523u};

/* the slot's ears' targets: SPEAKERS (or the centre, or AMOUNT 0) the pan gains alone, no delay, no shadow; HEADPHONES
 * the far ear late by a spherical head's time difference (Woodworth: (theta + sin theta) / (pi/2 + 1) of the head's
 * largest (TX_HEAD_ITD) to the whole sample, theta = 0 .. pi/2 from the centre to a hard pan), shadowed (the low-pass
 * coefficient from 1 to the head's corner, the shelf's highs from 1 to 0.4, both with sin theta) and constant power on
 * 61 % of the speakers' arc about the centre (the far ear -10 dB at a hard pan, the centre unchanged); late only below
 * its crossover (TX_XO: the highs on time, so the pair stays whole in mono). AMOUNT scales the delay, both shadow depths
 * and the arc's narrowing alike (100 %: as above, bit for bit) */
static void tx_mix_aim(tx_mix_t *m, unsigned k)
{
    unsigned p = m->bin[k].pan, f = 0;
    int32_t d = 0, a = 32768, b = 32768, gl = m->gl[k], gr = m->gr[k];
    if (m->hp && p != 64u) {
        uint32_t o = p < 64u ? 64u - p : p - 64u, c = 64u * 0x810204u, ph = p == 127u ? 0x40000000u : p * 0x810204u;
        uint32_t am = m->amt > 100u ? 100u : m->amt, hd = m->head < TX_MIX_HEAD_N ? m->head : TX_MIX_HEAD_MEDIUM;
        uint32_t ki = TX_HEAD_ITD[hd] * am / 100u, sa = TX_HEAD_SHADOW[hd] * am / 100u, sb = 19661u * am / 100u;
        int32_t arc = 32768 - (int32_t)(12768u * am / 100u);   /* (100 %: 20000) */
        int32_t u = (int32_t)((o << 15) / (p < 64u ? 64u : 63u)), s;   /* Q15: 32768 at a hard pan */
        s = tx_sin_q15((uint32_t)u << 15);                   /* sin theta */
        d = (int32_t)(((uint32_t)((u * 51472) >> 15) + (uint32_t)s) * ki >> 10);   /* (pi/2 Q15; MEDIUM: 29.1 / (pi/2 + 1)
                                                              * << 16 >> 15 = 22.64 = 23182 / 1024; under 84240 x
                                                              * 26343 < 2^32) */
        d = (d + 32768) & ~0xFFFF;                           /* (a whole sample at rest: read between two samples, a
                                                              * linear interpolation dulls the highs, -3 dB at 11 kHz
                                                              * halfway; the glides between two are fractional) */
        if (d > ((int32_t)TX_HEAD_DMAX[hd] << 16)) d = (int32_t)TX_HEAD_DMAX[hd] << 16;
        a = 32768 - ((s * (int32_t)sa) >> 15);
        b = 32768 - ((s * (int32_t)sb) >> 15);               /* 0.4 = 13107 */
        ph = c + (uint32_t)(((int32_t)(ph - c) >> 15) * arc);   /* 20000 / 32768 = 0.61 of the arc */
        gr = (tx_sin_q15(ph) * 23170) >> 15;
        gl = (tx_sin_q15(ph + 0x40000000u) * 23170) >> 15;
        f = p < 64u;                                         /* the far ear: right when panned left */
    }
    m->bin[k].e[f].dt = d; m->bin[k].e[f].at = a << 8; m->bin[k].e[f].bt = b << 8;
    m->bin[k].e[f ^ 1u].dt = 0; m->bin[k].e[f ^ 1u].at = m->bin[k].e[f ^ 1u].bt = 32768 << 8;
    m->bin[k].ct = (int32_t)TX_XO[(uint32_t)d >> 16 < 34u ? (uint32_t)d >> 16 : 33u] << 8;   /* (the centre: 1, the low
                                                              * band the sample itself) */
    m->bin[k].e[0].gt = gl << 8; m->bin[k].e[1].gt = gr << 8;
    if (m->hp) m->bin[k].run = 1;
    else if (!m->bin[k].run)                                 /* SPEAKERS at rest: its own path, the pan a step as ever
                                                              * (the ears follow, for the next glide) */
        for (f = 0; f < 2u; f++) m->bin[k].e[f].g = m->bin[k].e[f].gt;
}

void tx_mix_pan(tx_mix_t *m, unsigned k, unsigned pan)
{
    uint32_t ph;
    if (k >= TX_MIX_SLOTS) return;
    if (pan > 127u) pan = 127u;
    ph = pan == 127u ? 0x40000000u : pan * 0x810204u;       /* 0 .. pi/2 as a phase (2^30 / 127) */
    m->gr[k] = (tx_sin_q15(ph) * 23170) >> 15;               /* sqrt(2) sin, Q14: 16384 at the centre */
    m->gl[k] = (tx_sin_q15(ph + 0x40000000u) * 23170) >> 15; /* sqrt(2) cos */
    m->bin[k].pan = (uint8_t)pan;
    tx_mix_aim(m, k);
}

void tx_mix_phones(tx_mix_t *m, unsigned mode)
{
    unsigned k;
    m->mode = (uint8_t)(mode == TX_MIX_HEADPHONES);
    m->hp = m->mode && m->amt;                               /* (AMOUNT 0: the speakers' path, as SPEAKERS) */
    for (k = 0; k < TX_MIX_SLOTS; k++) tx_mix_aim(m, k);
}
void tx_mix_phones_amt(tx_mix_t *m, unsigned pct)
{
    m->amt = (uint8_t)(pct > 100u ? 100u : pct);
    tx_mix_phones(m, m->mode);
}
void tx_mix_phones_head(tx_mix_t *m, unsigned size)
{
    m->head = (uint8_t)(size < TX_MIX_HEAD_N ? size : TX_MIX_HEAD_N - 1u);
    tx_mix_phones(m, m->mode);
}

void tx_mix_trim(tx_mix_t *m, unsigned k, unsigned q12)
{
    if (k < TX_MIX_SLOTS) m->tg[k] = (int32_t)(q12 > 65535u ? 65535u : q12);
}

void tx_mix_level(tx_mix_t *m, unsigned k, unsigned v)
{
    if (k < TX_MIX_SLOTS) m->lv[k] = tx_level_q15(v > 127u ? 127u : v);
}

void tx_mix_gate(tx_mix_t *m, unsigned k, int open)
{
    if (k < TX_MIX_SLOTS) m->gt[k] = open ? 32768 : 0;
}

TX_AUDIO_CODE void tx_mix_begin(tx_mix_t *m, unsigned n)
{
    unsigned j;
    for (j = 0; j < n; j++) m->L[j] = m->R[j] = 0;
}

/* tx_mix_add with the slot's gate shut or ramping (a muted lane), or its level not unity or ramping: the level (from
 * lc to lv over the block) after the trim; the peak is taken after them, before the gate (the governor still sees the
 * lane's level), the gate steps 32768 / TX_MIX_GATE_N a sample toward its target, and a shut gate adds nothing */
static TX_AUDIO_CODE void tx_mix_add_gated(tx_mix_t *m, unsigned k, const int32_t *x, unsigned n)
{
    const int32_t st = (int32_t)(32768u / TX_MIX_GATE_N);
    unsigned j;
    int32_t gl = m->gl[k], gr = m->gr[k], pk = m->slot_peak[k], tg = m->tg[k], g = m->gc[k], gt = m->gt[k];
    int32_t lc = m->lc[k], lv = m->lv[k], ld = n ? (lv - lc) / (int32_t)n : 0;   /* (|lv - lc| < 2^16) */
    for (j = 0; j < n; j++) {
        int32_t v = x[j], a, hi, l;
        uint32_t lo;
        v = v > (1 << 26) ? (1 << 26) : v < -(1 << 26) ? -(1 << 26) : v;   /* (|v| <= 8 full scale: 32-bit products) */
        if (tg != 4096) v = (v >> 12) * tg + (int32_t)((((uint32_t)v & 4095u) * (uint32_t)tg) >> 12);
        l = j + 1u == n ? lv : lc + ld * (int32_t)(j + 1u);   /* (the block's last sample: lv exactly) */
        if (l != 32768) {                                    /* (|v| <= 8 x 1.62 full scale after: 32-bit products) */
            v = v > (1 << 26) ? (1 << 26) : v < -(1 << 26) ? -(1 << 26) : v;
            v = (v >> 15) * l + (int32_t)((((uint32_t)v & 32767u) * (uint32_t)l) >> 15);
        }
        a = v < 0 ? -v : v;
        if (a > pk) pk = a;
        if (g != gt) g += g < gt ? st : -st;
        if (!g) continue;
        v = (v >> 15) * g + (int32_t)((((uint32_t)v & 32767u) * (uint32_t)g) >> 15);
        hi = v >> 14; lo = (uint32_t)v & 0x3FFFu;
        hi = hi > 131071 ? 131071 : hi < -131072 ? -131072 : hi;
        m->L[j] += hi * gl + (int32_t)((lo * (uint32_t)gl) >> 14);
        m->R[j] += hi * gr + (int32_t)((lo * (uint32_t)gr) >> 14);
    }
    m->slot_peak[k] = pk; m->gc[k] = g;
    if (n) m->lc[k] = lv;
}

/* HEADPHONES' path (tx_mix_add_bin and its parts) runs from RAM on the FM-1 (TX_RAM: four slots' ears a block, clear of
 * XIP's cache misses) and goes over the block one job at a time: the slot's samples (the trim, the level, the gate)
 * into a scratch after its history, then each ear on its own from there (its delay, shadow and gain; the near ear at rest:
 * the sample itself), so each loop holds only its own state. Sample for sample the arithmetic of one loop doing both
 * ears (bench/txbintest.c: the same samples as that loop, kept there as the reference) */
#define TX_MULQ15(e, c) (((e) >> 15) * (c) + (int32_t)((((uint32_t)(e) & 32767u) * (uint32_t)(c)) >> 15))
#define TX_CLIP26(v) ((v) > (1 << 26) ? (1 << 26) : (v) < -(1 << 26) ? -(1 << 26) : (v))
/* y at a gain g (Q14) added to o: |y| clipped to 32 full scale first, 32-bit products */
#define TX_EAR_ADD(o, y, g) do { int32_t hi_ = (y) >> 14; uint32_t lo_ = (uint32_t)(y) & 0x3FFFu;              \
        hi_ = hi_ > 131071 ? 131071 : hi_ < -131072 ? -131072 : hi_;                                          \
        (o) += hi_ * (g) + (int32_t)((lo_ * (uint32_t)(g)) >> 14); } while (0)

/* the slot's history into hb before its block; returns the block's place (h[j - d]: d samples before sample j) */
TX_INLINE int32_t *tx_mix_bin_load(const tx_mix_t *m, unsigned k, int32_t *hb)
{
    const int32_t *r = m->bin[k].r, *rl = m->bin[k].rl;
    unsigned j;
    for (j = 0; j < TX_BIN_N; j++) { hb[j] = r[j]; hb[TX_BIN_HL + j] = rl[j]; }   /* (the low band's: TX_BIN_HL on) */
    return hb + TX_BIN_N;
}
/* the block's last TX_BIN_N samples (a short block: the history's last ones before them) kept as the slot's history */
TX_INLINE void tx_mix_bin_keep(tx_mix_t *m, unsigned k, const int32_t *hb, unsigned n)
{
    int32_t *r = m->bin[k].r, *rl = m->bin[k].rl;
    const int32_t *h = hb + n;
    unsigned j;
    for (j = 0; j < TX_BIN_N; j++) { r[j] = h[j]; rl[j] = h[TX_BIN_HL + j]; }
}

/* the slot's low band, one sample: x through two one-pole low-passes at c (Q15; 1: x itself, exactly) */
#define TX_XO_LO(x, c, s0, s1) do { (s0) += TX_MULQ15((x) - (s0), (c)); (s1) += TX_MULQ15((s0) - (s1), (c)); } while (0)
/* one ear, one sample: the sample now (hj[0]) less its low band now (lj[0]), plus its low band read d (Q16) back between
 * two samples, then the head's shadow: a one-pole low-pass (a) as a high shelf (b: the highs kept). a = b = 1, d = 0:
 * the sample itself, exactly. |r| <= 2^26: 32-bit products */
TX_INLINE int32_t tx_mix_ear(const int32_t *hj, const int32_t *lj, int32_t d, int32_t a, int32_t b, int32_t *lp)
{
    uint32_t i = (uint32_t)d >> 16;
    int32_t f = (int32_t)(((uint32_t)d & 0xFFFFu) >> 1), x0 = lj[-(int32_t)i], y, e;
    y = x0 + TX_MULQ15(lj[-(int32_t)i - 1] - x0, f);
    y = hj[0] - lj[0] + y;
    y = TX_CLIP26(y);
    e = y - *lp; *lp += TX_MULQ15(e, a);
    e = y - *lp;
    return *lp + TX_MULQ15(e, b);
}
/* tx_mix_add_bin at rest (the gate open, the level unity, every ear where it aims): the near ear as SPEAKERS' (the
 * sample at its gain; at the centre both ears, so the centre is SPEAKERS' bit for bit), the far ear from the history a
 * whole number of samples back (tx_mix_aim's), its shelf and gain in one: g b on the sample, g (1 - b) on the low-pass */
static TX_RAM void tx_mix_add_rest(tx_mix_t *m, unsigned k, const int32_t *x, unsigned n, unsigned f, const tx_mix_to_t *to)
{
    unsigned j;
    int32_t pk = m->slot_peak[k], tg = m->tg[k], *h = tx_mix_bin_load(m, k, to->hb), *nr = f ? to->L : to->R;
    int32_t *fr = f ? to->R : to->L;
    int32_t gn = m->bin[k].e[f ^ 1u].g >> 8, gf = m->bin[k].e[f].g >> 8, bq = m->bin[k].e[f].b >> 8;
    int32_t a = m->bin[k].e[f].a >> 8, gb = (gf * bq) >> 15, gc = (gf * (32768 - bq)) >> 15, lp = m->bin[k].e[f].lp;
    int32_t c = m->bin[k].c >> 8, s0 = m->bin[k].s[0], s1 = m->bin[k].s[1], *hl = h + TX_BIN_HL;
    const int32_t *yd = hl - (int32_t)((uint32_t)m->bin[k].e[f].d >> 16);
    for (j = 0; j < n; j++) {
        int32_t v = x[j], av, y;
        if (tg != 4096) {                                    /* (as tx_mix_add) */
            v = TX_CLIP26(v);
            v = (v >> 12) * tg + (int32_t)((((uint32_t)v & 4095u) * (uint32_t)tg) >> 12);
        }
        av = v < 0 ? -v : v;
        if (av > pk) pk = av;
        TX_EAR_ADD(nr[j], v, gn);
        h[j] = TX_CLIP26(v);
        TX_XO_LO(h[j], c, s0, s1);
        hl[j] = s1;
        y = h[j] - s1 + yd[j];                               /* (the highs now, the low band late) */
        y = TX_CLIP26(y);
        lp += TX_MULQ15(y - lp, a);
        fr[j] += (y >> 14) * gb + (int32_t)((((uint32_t)y & 0x3FFFu) * (uint32_t)gb) >> 14)
               + (lp >> 14) * gc + (int32_t)((((uint32_t)lp & 0x3FFFu) * (uint32_t)gc) >> 14);
    }
    m->slot_peak[k] = pk; m->bin[k].e[f].lp = lp; m->bin[k].s[0] = s0; m->bin[k].s[1] = s1;
    tx_mix_bin_keep(m, k, to->hb, n);
}
/* the centre at rest: both ears the sample at their gains (SPEAKERS' sum), the history kept */
static TX_RAM void tx_mix_add_mid(tx_mix_t *m, unsigned k, const int32_t *x, unsigned n, const tx_mix_to_t *to)
{
    unsigned j;
    int32_t pk = m->slot_peak[k], tg = m->tg[k], *h = tx_mix_bin_load(m, k, to->hb), *L = to->L, *R = to->R;
    int32_t gl = m->bin[k].e[0].g >> 8, gr = m->bin[k].e[1].g >> 8;
    for (j = 0; j < n; j++) {
        int32_t v = x[j], av;
        if (tg != 4096) {
            v = TX_CLIP26(v);
            v = (v >> 12) * tg + (int32_t)((((uint32_t)v & 4095u) * (uint32_t)tg) >> 12);
        }
        av = v < 0 ? -v : v;
        if (av > pk) pk = av;
        TX_EAR_ADD(L[j], v, gl);
        TX_EAR_ADD(R[j], v, gr);
        h[j] = h[TX_BIN_HL + j] = TX_CLIP26(v);             /* (the crossover at 1: the low band is the sample) */
    }
    m->slot_peak[k] = pk;
    for (j = 0; j < 2u; j++) m->bin[k].e[j].lp = m->bin[k].s[j] = h[(int32_t)n - 1];   /* (a = 1: the low-pass is the
                                                              * sample) */
    tx_mix_bin_keep(m, k, to->hb, n);
}

/* one ramp's step a sample: (target - now) / n, a whole block to its target */
TX_INLINE int32_t tx_mix_step(int32_t now, int32_t to, unsigned n) { return n ? (to - now) / (int32_t)n : 0; }

/* tx_mix_add_bin's first pass: the slot's block (the trim, the level and the gate as tx_mix_add_gated) after its
 * history in hb (its place returned), its low band after that's (TX_BIN_HL on: the crossover ramped over the block) */
TX_INLINE int32_t *tx_mix_bin_in(tx_mix_t *m, unsigned k, const int32_t *x, unsigned n, int32_t *hb)
{
    const int32_t st = (int32_t)(32768u / TX_MIX_GATE_N);
    unsigned j;
    int32_t *h = tx_mix_bin_load(m, k, hb), pk = m->slot_peak[k], tg = m->tg[k], g = m->gc[k], gt = m->gt[k];
    int32_t lc = m->lc[k], lv = m->lv[k], ld = n ? (lv - lc) / (int32_t)n : 0;
    if (tg == 4096 && lc == 32768 && lv == 32768 && g == 32768 && gt == 32768)   /* (unity: each step below exact) */
        for (j = 0; j < n; j++) {
            int32_t v = TX_CLIP26(x[j]), av = v < 0 ? -v : v;
            if (av > pk) pk = av;
            h[j] = v;
        }
    else
        for (j = 0; j < n; j++) {
            int32_t v = TX_CLIP26(x[j]), av, l;
            if (tg != 4096) v = (v >> 12) * tg + (int32_t)((((uint32_t)v & 4095u) * (uint32_t)tg) >> 12);
            l = j + 1u == n ? lv : lc + ld * (int32_t)(j + 1u);
            if (l != 32768) {
                v = TX_CLIP26(v);
                v = (v >> 15) * l + (int32_t)((((uint32_t)v & 32767u) * (uint32_t)l) >> 15);
            }
            av = v < 0 ? -v : v;
            if (av > pk) pk = av;
            if (g != gt) g += g < gt ? st : -st;
            v = TX_MULQ15(v, g);
            h[j] = TX_CLIP26(v);                             /* (the history: 32-bit products) */
        }
    {
        int32_t c = m->bin[k].c, dc = tx_mix_step(c, m->bin[k].ct, n), s0 = m->bin[k].s[0], s1 = m->bin[k].s[1];
        int32_t *hl = h + TX_BIN_HL;
        if (!dc) { int32_t c8 = c >> 8; for (j = 0; j < n; j++) { TX_XO_LO(h[j], c8, s0, s1); hl[j] = s1; } }
        else for (j = 0; j < n; j++) { TX_XO_LO(h[j], c >> 8, s0, s1); hl[j] = s1; c += dc; }
        m->bin[k].s[0] = s0; m->bin[k].s[1] = s1;
        if (n) m->bin[k].c = m->bin[k].ct;
    }
    m->slot_peak[k] = pk; m->gc[k] = g;
    if (n) m->lc[k] = lv;
    return h;
}
/* tx_mix_add_bin's ear i (0 left, 1 right) over the block from h into o: its delay, shadow and gain ramped from where
 * they are to where they aim (the delay at most 1/16 sample a sample: a pitch +-6 % while it glides, 29 samples in 10.5 ms;
 * its rate's kinks small, and it never reads past the history a block wrote); its state left as the ramps leave it */
static TX_RAM __attribute__((noinline)) void tx_mix_ear_run(tx_mix_t *m, unsigned k, unsigned i, const int32_t *h, unsigned n,
                                                             int32_t *o)
{
    int32_t d = m->bin[k].e[i].d, a = m->bin[k].e[i].a, b = m->bin[k].e[i].b;
    int32_t g = m->bin[k].e[i].g, lp = m->bin[k].e[i].lp, dt = m->bin[k].e[i].dt;
    int32_t dd = tx_mix_step(d, dt, n), da = tx_mix_step(a, m->bin[k].e[i].at, n), db = tx_mix_step(b, m->bin[k].e[i].bt, n);
    int32_t dg = tx_mix_step(g, m->bin[k].e[i].gt, n);
    unsigned j;
    dd = dd > 4096 ? 4096 : dd < -4096 ? -4096 : dd;
    if (!d && !dd && a == (32768 << 8) && !da && b == (32768 << 8) && !db) {   /* the near ear, still: the sample */
        if (!dg) { int32_t g8 = g >> 8; for (j = 0; j < n; j++) TX_EAR_ADD(o[j], h[j], g8); }
        else for (j = 0; j < n; j++) { TX_EAR_ADD(o[j], h[j], g >> 8); g += dg; }
        if (n) lp = h[n - 1u];                               /* (a = 1: the low-pass is the sample) */
    } else if (!dd && !(d & 0xFFFF)) {                       /* a whole delay, still: no reading between two */
        const int32_t *hl = h + TX_BIN_HL, *yd = hl - (int32_t)((uint32_t)d >> 16);
        for (j = 0; j < n; j++) {
            int32_t y = h[j] - hl[j] + yd[j], e;
            y = TX_CLIP26(y); e = y - lp;
            lp += TX_MULQ15(e, a >> 8);
            e = y - lp; y = lp + TX_MULQ15(e, b >> 8);
            TX_EAR_ADD(o[j], y, g >> 8);
            a += da; b += db; g += dg;
        }
    } else
        for (j = 0; j < n; j++) {
            int32_t y = tx_mix_ear(h + j, h + TX_BIN_HL + j, d, a >> 8, b >> 8, &lp);
            TX_EAR_ADD(o[j], y, g >> 8);
            a += da; b += db; g += dg;
            if (dd) {                                        /* (a glide at its 1/16 sample: stops at its target) */
                d += dd;
                if (dd > 0 ? d > dt : d < dt) d = dt, dd = 0;
            }
        }
    m->bin[k].e[i].d = d; m->bin[k].e[i].a = a; m->bin[k].e[i].b = b; m->bin[k].e[i].g = g; m->bin[k].e[i].lp = lp;
}

/* tx_mix_add in HEADPHONES (or gliding back to SPEAKERS: tx_mix_binaural), into to's pair: the trim, the level and the
 * gate as tx_mix_add_gated, the block into the slot's history, then each ear from it (tx_mix_ear_run) at its gain,
 * every ear's delay, shadow and gain ramped over the block */
TX_RAM void tx_mix_add_bin(tx_mix_t *m, unsigned k, const int32_t *x, unsigned n, const tx_mix_to_t *to)
{
    unsigned j, i;
    const int32_t *h;
    if (m->hp && m->gc[k] == 32768 && m->gt[k] == 32768 && m->lc[k] == 32768 && m->lv[k] == 32768
        && m->bin[k].c == m->bin[k].ct) {                    /* at rest? (the gate, the level, the crossover, the
                                                              * ears) */
        unsigned f = 2u;
        for (i = 0; i < 2u; i++) {
            if (m->bin[k].e[i].d != m->bin[k].e[i].dt || m->bin[k].e[i].a != m->bin[k].e[i].at
                || m->bin[k].e[i].b != m->bin[k].e[i].bt || m->bin[k].e[i].g != m->bin[k].e[i].gt) break;
            if (m->bin[k].e[i].dt || m->bin[k].e[i].at != (32768 << 8) || m->bin[k].e[i].bt != (32768 << 8)) f = i;
        }
        if (i == 2u) {
            if (f == 2u) tx_mix_add_mid(m, k, x, n, to);
            else tx_mix_add_rest(m, k, x, n, f, to);
            return;
        }
    }
    h = tx_mix_bin_in(m, k, x, n, to->hb);
    tx_mix_ear_run(m, k, 0u, h, n, to->L);
    tx_mix_ear_run(m, k, 1u, h, n, to->R);
    tx_mix_bin_keep(m, k, to->hb, n);
    i = !m->hp;                                              /* (i: every ear at rest, in SPEAKERS) */
    for (j = 0; j < 2u; j++) {
        int32_t t = m->bin[k].e[j].dt, d = m->bin[k].e[j].d;
        if (n && (t - d < (int32_t)n && d - t < (int32_t)n)) m->bin[k].e[j].d = d = t;   /* (the delay there: exactly) */
        if (n) { m->bin[k].e[j].a = m->bin[k].e[j].at; m->bin[k].e[j].b = m->bin[k].e[j].bt; m->bin[k].e[j].g = m->bin[k].e[j].gt; }
        if (d != t || m->bin[k].e[j].a != m->bin[k].e[j].at || m->bin[k].e[j].b != m->bin[k].e[j].bt
            || m->bin[k].e[j].g != m->bin[k].e[j].gt) i = 0;
    }
    if (i) m->bin[k].run = 0;
}

TX_AUDIO_CODE void tx_mix_add(tx_mix_t *m, unsigned k, const int32_t *x, unsigned n)
{
    unsigned j;
    int32_t gl, gr, pk, tg, gc, gt;
    if (k >= TX_MIX_SLOTS) return;
    if (m->hp | m->bin[k].run) {                             /* HEADPHONES (or gliding back): the slot's ears */
        tx_mix_to_t to;
        to.L = m->L; to.R = m->R; to.hb = m->hb;
        TX_FAR(tx_mix_add_bin)(m, k, x, n, &to);             /* (in RAM on the FM-1) */
        return;
    }
    gl = m->gl[k]; gr = m->gr[k]; pk = m->slot_peak[k]; tg = m->tg[k];
    gc = m->gc[k]; gt = m->gt[k];
    if (gc != 32768 || gt != 32768 || m->lc[k] != 32768 || m->lv[k] != 32768) {   /* the gate shut or moving, the
                                                              * level not unity: the slot's own loop */
        tx_mix_add_gated(m, k, x, n);
        return;
    }
    for (j = 0; j < n; j++) {
        int32_t v = x[j], a, hi;
        uint32_t lo;
        if (tg != 4096) {                                    /* the trim: |v| <= 8 full scale first, 32-bit products */
            v = v > (1 << 26) ? (1 << 26) : v < -(1 << 26) ? -(1 << 26) : v;
            v = (v >> 12) * tg + (int32_t)((((uint32_t)v & 4095u) * (uint32_t)tg) >> 12);
        }
        a = v < 0 ? -v : v; hi = v >> 14;
        lo = (uint32_t)v & 0x3FFFu;
        if (a > pk) pk = a;
        hi = hi > 131071 ? 131071 : hi < -131072 ? -131072 : hi;   /* |v| <= 32 full scale: products < 2^31 */
        m->L[j] += hi * gl + (int32_t)((lo * (uint32_t)gl) >> 14);
        m->R[j] += hi * gr + (int32_t)((lo * (uint32_t)gr) >> 14);
    }
    m->slot_peak[k] = pk;
}

TX_AUDIO_CODE void tx_mix_slot_render(tx_mix_t *m, unsigned k, unsigned n)
{
    unsigned j;
    if (k >= TX_MIX_SLOTS || !m->e[k]) return;
    for (j = 0; j < n; j++) m->blk[j] = 0;
    m->e[k]->render(m->st[k], m->blk, n);
    if (m->ins) m->ins(m->fx, k, m->blk, n);                 /* the slot's insert (the filter) */
    tx_mix_add(m, k, m->blk, n);
}

TX_AUDIO_CODE void tx_mix_end(tx_mix_t *m, int32_t *L, int32_t *R, unsigned n)
{
    tx_mix_run(m, n, 0u);
    tx_mix_out(m, L, R, n);
}
TX_AUDIO_CODE void tx_mix_run(tx_mix_t *m, unsigned n, unsigned c)
{
    unsigned j;
    int32_t pk = 0;
    void (*pre)(void *, int32_t *, int32_t *, unsigned) = c ? m->pre1 : m->pre;
    void (*bus)(void *, int32_t *, int32_t *, unsigned) = c ? m->bus1 : m->bus;
#ifdef DINGHY_HOST
    tx_st_n = 0;
#endif
    TX_STAGE(TX_ST_COMP);
    TX_FAR(tx_comp_process)(&m->comp, m->L, m->R, n);        /* the compressor, the first effect, as Frodhes (off:
                                                              * returns at once; in XIP, as the limiter: the bus runs on
                                                              * core 1, LN_PIPE) */
    if (pre) pre(m->fx, m->L, m->R, n);                      /* the DRIVE and the FILTER after it (off: at once) */
    if (m->ch && m->ch_on) {                                 /* the chamber on the mid: its added part to both sides */
        for (j = 0; j < n; j++) m->mid[j] = m->blk[j] = (m->L[j] >> 1) + (m->R[j] >> 1);
        TX_FAR(tx_chamber_process)(m->ch, m->mid, n);        /* (in XIP: the bus runs on core 1) */
        for (j = 0; j < n; j++) { int32_t w = m->mid[j] - m->blk[j]; m->L[j] += w; m->R[j] += w; }
    }
    if (bus) bus(m->fx, m->L, m->R, n);                      /* the bus effects (the grain delay, the reverb) */
    TX_STAGE(TX_ST_MS);
    TX_FAR(tx_ms_process)(&m->ms, m->L, m->R, n);            /* the M/S, the last effect: the whole mix's middle and
                                                              * sides, the reverb's tail with them; before the limiter,
                                                              * so a boost (up to +6 dB) never passes it (in XIP) */
    for (j = 0; j < n; j++) {                                /* linked: one gain for both, from the louder */
        int32_t a = m->L[j] < 0 ? -m->L[j] : m->L[j], b = m->R[j] < 0 ? -m->R[j] : m->R[j];
        if (a > pk) pk = a;
        if (b > pk) pk = b;
        m->ilv[2 * j] = m->L[j]; m->ilv[2 * j + 1] = m->R[j];
    }
    m->raw_peak = pk;
    TX_FAR(tx_limit_process)(&m->lim, m->ilv, 2u * n);       /* interleaved: the pair shares the gain envelope (in XIP) */
}
TX_AUDIO_CODE void tx_mix_out(const tx_mix_t *m, int32_t *L, int32_t *R, unsigned n)
{
    unsigned j;
    for (j = 0; j < n; j++) { L[j] = m->ilv[2 * j]; R[j] = m->ilv[2 * j + 1]; }
}

int tx_mix_quiet(const tx_mix_t *m)
{
    unsigned k;
    for (k = 0; k < TX_MIX_SLOTS; k++)
        if (m->e[k]) { tx_stats_t s; m->e[k]->stats(m->st[k], &s); if (!s.quiet) return 0; }
    if (m->busq && !m->busq(m->fx)) return 0;
    return !(m->ch && m->ch_on) || tx_chamber_quiet(m->ch);
}
