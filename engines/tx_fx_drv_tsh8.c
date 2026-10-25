/* SPDX-License-Identifier: GPL-3.0-only */
/* DRIVE TYPE 2, TSH8: the mid-hump overdrive: two diodes in the clipping amplifier's feedback, so the output is the
 * clean input plus a high-passed, clipped branch (the lows stay at unity gain), then a low-pass and an active tone.
 * The circuit and its tables: tools/drive_gen/tsh8.py. 1x, no oversampling, no divide.
 * Frodhes' drv_tsh8.c (TYPE 3 there, its anti-aliased kernel) as an instance (included by tx_fx_drive.c): the same
 * integers out (checked against Frodhes' file, bit for bit, on moving knobs). Its filters are the lane's
 * (tx_drv_ts_t); its two diode tables (12.2 KB) are shared (tx_drv_shared_t): TSH8 runs on one lane at a time.
 *
 * Once a block (n <= TX_DRV_BLK, even), four passes, two samples per iteration where it pays:
 *   1  the input biquad (the coupling high-pass x P; x TSH8_IN_K: full scale = 0.301 V)          -> vi (volts Q24)
 *   2  the branch biquad (per AMOUNT step: 720 Hz high-pass x the 51 pF low-pass x the gain)    -> u (volts Q14);
 *      the two-sample average of vi written back
 *   3  V = g(u): the diode table of this AMOUNT step (RAM, built from the 8 keys), with first-order antiderivative
 *      anti-aliasing (no divide: the reciprocal of the step from DRV_AA_RCP by count-leading-zeros, one product)
 *   4  vi + V -> the tone biquad (per TONE step, x the level x 2^TSH8_OSH, x Q) -> >> TSH8_OSH -> out
 * Knobs: the AMOUNT and TONE steps that run glide toward the knobs, TSH8_GSTEP steps a block (the whole range in 64
 * blocks, 0.37 s: no click); the branch and tone rows (and the level) are copied only when a step moves.
 * The diode table follows AMOUNT behind, at most TSH8_TSTEP steps a swap: the next one is built in the second
 * buffer, 32 nodes a block, then swapped in at the next block's start, crossfaded from the old one over its first 16
 * samples (a block builds or swaps, never both). The first table for a lane that takes TSH8 (or after a jump of
 * AMOUNT while it was off) is built whole in the block it starts (~98 us on the FM-1, the kernel not run: the lane
 * stays dry that block and fades in from the next).
 * Cost (Frodhes, pi32v2 -Os): input 14.5 + branch 18.5 + the clipper 47 + tone 18 = 98 instructions a sample, 156.8
 * us at 1.6, the worst block's build or swap on top: 165 us. */
#ifdef TX_FX_DRIVE_UNIT
#define DRV_US_TSH8 165u
_Static_assert(TSH8_AA, "TSH8 runs its anti-aliased kernel only");
_Static_assert(TSH8_NODES == TX_DRV_TS_NODES, "tx_fx_drv.h TX_DRV_TS_NODES: the generated table's size");
_Static_assert((TX_DRV_BLK & 1u) == 0, "TSH8 runs two samples a pass: the block must be even");
#define TSH8_FINE_SH 7
#define TSH8_FINE_END (2 << 14)
#define TSH8_COARSE_SH 12
#define TSH8_NFINE (TSH8_FINE_END >> TSH8_FINE_SH)
#define TSH8_SLICE 32u                                   /* table nodes built a block */
#define TSH8_GSTEP 2                                     /* AMOUNT / TONE steps the rows move at most a block */
#define TSH8_OSH 12                                      /* the tone stage's output bits below the sample's */
#define TSH8_XF 16u                                      /* samples a table swap is crossfaded over */
#define TSH8_TSTEP 16                                    /* AMOUNT steps a table moves at most per swap */
#define TSH8_AA_SF 5
#define TSM(a, b) ((int64_t)(a) * (b))

/* nodes [i, e) of AMOUNT step r's table into t: {g, F} per node, g the keys below and above the step, linearly
 * (tools/drive_gen/tsh8.py), F the exact antiderivative of the interpolated g >> TSH8_AA_SF; built in order from
 * node 0 (the running values in sh). F's running sum needs 35 bits: int64. */
static void ts_build_gf(tx_drv_shared_t *sh, int32_t *t, int32_t r, uint32_t i, uint32_t e)
{
    uint32_t kp = TSH8_KPOS[r];
    const uint8_t *d0 = TSH8_KD[kp >> 9], *d1 = TSH8_KD[(kp >> 9) + (kp & 511u ? 1u : 0u)];
    int32_t w = (int32_t)(kp & 511u), v0 = i ? sh->ts_v0 : 0, v1 = i ? sh->ts_v1 : 0, gp = sh->ts_gp;
    int64_t s = i ? sh->ts_s : 0;
    for (; i < e; i++) {
        int32_t g = v0 + (((v1 - v0) * w) >> 8);
        if (i) s += ((int64_t)(gp + g) * ((int64_t)1 << (i <= (uint32_t)TSH8_NFINE ? TSH8_FINE_SH : TSH8_COARSE_SH))) >> 1;
        t[2 * i] = g; t[2 * i + 1] = (int32_t)(s >> TSH8_AA_SF);
        gp = g; v0 += d0[i]; v1 += d1[i];
    }
    sh->ts_v0 = v0; sh->ts_v1 = v1; sh->ts_gp = gp; sh->ts_s = s;
}

static void drv_tsh8_reset(tx_drv_ts_t *s)
{
    tx_fx_memset(s, 0, sizeof *s);
    s->ra = -1;                                            /* (the shared table is kept: it stays right for its step) */
}

/* pass 4: s = vi + V, the tone biquad (Q28), 2 samples per iteration, the output over V */
static __attribute__((noinline)) void ts_tone2qi(const int32_t *c, int32_t *st, const int32_t *vi, int32_t *v,
                                                 uint32_t n)
{
    int32_t x1 = st[0], x2 = st[1], y1 = st[2], y2 = st[3];
    uint32_t i;
    for (i = n >> 1; i; i--) {
        int32_t xa = vi[0] + v[0] * 512, xb = vi[1] + v[1] * 512, ya, yb, k;
        int64_t pa, pb;
        k = c[2]; pa = TSM(k, x2); pb = TSM(k, x1);
        k = c[1]; pa += TSM(k, x1); pb += TSM(k, xa);
        k = c[0]; pa += TSM(k, xa); pb += TSM(k, xb);
        k = c[4]; pa += TSM(k, y2); pb += TSM(k, y1);
        k = c[3]; pa += TSM(k, y1);
        ya = (int32_t)(pa >> 28);
        pb += TSM(k, ya);
        yb = (int32_t)(pb >> 28);
        x2 = xa; x1 = xb; y2 = ya; y1 = yb;
        v[0] = ya >> TSH8_OSH; v[1] = yb >> TSH8_OSH;      /* (its state keeps them: the recursion's rounding */
        vi += 2; v += 2;                                   /* stays 2^-12 of a sample) */
    }
    st[0] = x1; st[1] = x2; st[2] = y1; st[3] = y2;
}

/* |u| is masked to 21 bits (the table runs to 128 V, |u| <= 67 V at full scale): a far hotter input folds back into
 * the table, never a read outside it */
#define TSH8_AEND ((128 << 14) - 1)
#define TSH8_AT (1 << TSH8_AA_SF)                         /* |du| below this (2 mV): g at the midpoint, no quotient */

/* pass 1: the input biquad (HP x P, Q28), x TSH8_IN_K on the way in, 2 samples per iteration */
static __attribute__((noinline)) void ts_in2q(const int32_t *c, int32_t *st, const int32_t *in, int32_t *out,
                                              uint32_t n)
{
    int32_t x1 = st[0], x2 = st[1], y1 = st[2], y2 = st[3];
    uint32_t i;
    for (i = n >> 1; i; i--) {
        int32_t xa = in[0] * TSH8_IN_K, xb = in[1] * TSH8_IN_K, ya, yb, k;
        int64_t pa, pb;
        k = c[2]; pa = TSM(k, x2); pb = TSM(k, x1);
        k = c[1]; pa += TSM(k, x1); pb += TSM(k, xa);
        k = c[0]; pa += TSM(k, xa); pb += TSM(k, xb);
        k = c[4]; pa += TSM(k, y2); pb += TSM(k, y1);
        k = c[3]; pa += TSM(k, y1);
        ya = (int32_t)(pa >> 28);
        pb += TSM(k, ya);
        yb = (int32_t)(pb >> 28);
        x2 = xa; x1 = xb; y2 = ya; y1 = yb;
        out[0] = ya; out[1] = yb;
        in += 2; out += 2;
    }
    st[0] = x1; st[1] = x2; st[2] = y1; st[3] = y2;
}

/* pass 2: the branch biquad, writing the two-sample average of Vi back over vi (x1 is the previous Vi) */
static __attribute__((noinline)) void ts_br2qa(const int32_t *c, int32_t *st, int32_t *in, int32_t *out, uint32_t n)
{
    int32_t x1 = st[0], x2 = st[1], y1 = st[2], y2 = st[3];
    uint32_t i;
    for (i = n >> 1; i; i--) {
        int32_t xa = in[0], xb = in[1], ya, yb, k;
        int64_t pa, pb;
        k = c[2]; pa = TSM(k, x2); pb = TSM(k, x1);
        k = c[1]; pa += TSM(k, x1); pb += TSM(k, xa);
        k = c[0]; pa += TSM(k, xa); pb += TSM(k, xb);
        k = c[4]; pa += TSM(k, y2); pb += TSM(k, y1);
        k = c[3]; pa += TSM(k, y1);
        ya = (int32_t)(pa >> 28);
        pb += TSM(k, ya);
        yb = (int32_t)(pb >> 28);
        in[0] = (xa + x1) >> 1; in[1] = (xb + xa) >> 1;
        x2 = xa; x1 = xb; y2 = ya; y1 = yb;
        out[0] = ya >> 4; out[1] = yb >> 4;
        in += 2; out += 2;
    }
    st[0] = x1; st[1] = x2; st[2] = y1; st[3] = y2;
}

/* one sample of the clipper: x (u Q14) -> V (Q15); X1, F1 the previous input and its F (updated); gf / gc the {g, F}
 * table's fine and coarse bases, rcp the reciprocal table's base */
#define TS_SEGA(BASE, SH)                                                                                   \
    do {                                                                                                    \
        e = (BASE) + 2 * (a >> (SH)); fr = a & ((1 << (SH)) - 1);                                           \
        g0 = e[0]; dg = e[2] - g0; p = dg * fr;                                                             \
        F = e[1] + ((g0 * fr + (int32_t)(((uint32_t)p * (uint32_t)fr) >> ((SH) + 1))) >> TSH8_AA_SF);      \
        hd = (SH) + 1;                                                                                      \
    } while (0)
#define TS_AA_BODY(gf, gc, xin, vout, X1, F1)                                                               \
    do {                                                                                                    \
        int32_t x = (xin), a, fr, g0, dg, p, F, d, hd;                                                      \
        uint32_t ad;                                                                                        \
        const int32_t *e;                                                                                   \
        a = x < 0 ? -x : x;                                                                                 \
        a &= TSH8_AEND;                                                                                     \
        if (a < TSH8_FINE_END) TS_SEGA(gf, TSH8_FINE_SH); else TS_SEGA(gc, TSH8_COARSE_SH);                 \
        d = x - (X1);                                                                                       \
        ad = (uint32_t)(d < 0 ? -d : d);                                                                    \
        if (ad < TSH8_AT) {                                                                                 \
            int32_t v_ = g0 + (p >> (hd - 1));                                                              \
            v_ = x < 0 ? -v_ : v_;                                                                          \
            (vout) = v_ - ((dg * d) >> hd);                                                                 \
        } else {                                                                                            \
            int nz = __builtin_clz(ad);                                                                     \
            int32_t r = rcp[(ad << nz) >> (32 - DRV_AA_RB)];                                                \
            int32_t df_ = F - (F1);                                                                         \
            if (d < 0) df_ = -df_;                                                                          \
            (vout) = (int32_t)(TSM(df_, r) >> (47 - TSH8_AA_SF - nz));                                      \
        }                                                                                                   \
        (X1) = x; (F1) = F;                                                                                 \
    } while (0)
#define TS_GC(gf) ((gf) + 2 * (TSH8_NFINE - (TSH8_FINE_END >> TSH8_COARSE_SH)))
#define TS_RCP ((const uint16_t *)((uintptr_t)DRV_AA_RCP - (2u << (DRV_AA_RB - 1))))   /* (indexed from 2^(RB-1)) */

/* pass 3: the clipper, in place, one sample per iteration */
static __attribute__((noinline)) void ts_aa1(const int32_t *gf, int32_t *st, int32_t *u, uint32_t n)
{
    const int32_t *gc = TS_GC(gf);
    const uint16_t *rcp = TS_RCP;
    int32_t x1 = st[0], F1 = st[1];
    uint32_t i;
    for (i = n; i; i--) {
        int32_t v;
        TS_AA_BODY(gf, gc, u[0], v, x1, F1);
        *u++ = v;
    }
    st[0] = x1; st[1] = F1;
}

/* F of the table g at x (the clipper's segment, no output): after a table swap the previous F must be the new
 * table's, or the first quotient jumps */
static int32_t ts_F(const int32_t *gf, int32_t x)
{
    const int32_t *gc = TS_GC(gf), *e;
    int32_t a = x < 0 ? -x : x, fr, g0, dg, p, F, hd;
    a &= TSH8_AEND;
    if (a < TSH8_FINE_END) TS_SEGA(gf, TSH8_FINE_SH); else TS_SEGA(gc, TSH8_COARSE_SH);
    (void)hd;
    return F;
}

/* pass 3 after a swap, its first TSH8_XF samples: both tables' outputs (each its own F chain), crossfaded */
static __attribute__((noinline)) void ts_aa1x(const int32_t *g0f, const int32_t *gf, int32_t *st, int32_t *u)
{
    const int32_t *gc0 = TS_GC(g0f), *gc = TS_GC(gf);
    const uint16_t *rcp = TS_RCP;
    int32_t x1 = st[0], F0 = st[1], F1 = ts_F(gf, st[0]), xo;
    uint32_t i;
    for (i = 0; i < TSH8_XF; i++) {
        int32_t va, vb;
        xo = x1;
        TS_AA_BODY(g0f, gc0, u[i], va, xo, F0);
        TS_AA_BODY(gf, gc, u[i], vb, x1, F1);
        u[i] = va + (int32_t)(((int64_t)(vb - va) * (int32_t)(i + 1u)) >> 4);
    }
    st[0] = x1; st[1] = F1;
}

/* the four passes, in to out (out may be in); g0f: the table before a swap (crossfaded into gf), or 0 */
static __attribute__((noinline)) void ts_kernel_aa(tx_drv_ts_t *s, tx_drv_shared_t *sh, const int32_t *g0f,
                                                   const int32_t *gf, const int32_t *in, int32_t *out, uint32_t n)
{
    int32_t *vi = sh->t1;
    ts_in2q(sh->ts_in, s->is, in, vi, n);
    ts_br2qa(s->br, s->bs, vi, out, n);
    if (g0f) { ts_aa1x(g0f, gf, s->as, out); ts_aa1(gf, s->as, out + TSH8_XF, n - TSH8_XF); }
    else ts_aa1(gf, s->as, out, n);
    ts_tone2qi(s->tone, s->ts, vi, out, n);
}

/* a table word stored as two int16 (high, low: no word in the chip-ROM window in the image) */
static inline int32_t ts_w(const int16_t *p) { return (int32_t)(((uint32_t)(uint16_t)p[0] << 16) | (uint16_t)p[1]); }
/* r0 + ((r1 - r0) w >> 8), 64-bit (tools/drive_gen/tsh8.py lerp) */
static int32_t ts_lerp(int32_t r0, int32_t r1, int32_t w) { return r0 + (int32_t)((((int64_t)r1 - r0) * w) >> 8); }
/* row word i at position pos (row << 8 | w) of a split16 table: the row, or between it and the next */
static int32_t ts_row(const int16_t (*R)[10], uint32_t pos, int i)
{
    const int16_t *p = R[pos >> 8];
    return pos & 255u ? ts_lerp(ts_w(p + 2 * i), ts_w(p + 10 + 2 * i), (int32_t)(pos & 255u)) : ts_w(p + 2 * i);
}
static int32_t ts_lv(const int32_t *L, uint32_t pos)
{
    return pos & 255u ? ts_lerp(L[pos >> 8], L[(pos >> 8) + 1u], (int32_t)(pos & 255u)) : L[pos >> 8];
}
/* the rows of steps (a, t); the tone row x the level: b' = b L >> 24, L = LVA[a] LVT[t] >> 15 (Q24) */
static void ts_set(tx_drv_ts_t *s, int32_t a, int32_t t)
{
    uint32_t pa = TSH8_PA[a], pt = TSH8_PT[t];
    int32_t L = (int32_t)(((int64_t)ts_lv(TSH8_LVA, pa) * ts_lv(TSH8_LVT, pt)) >> 15), i;
    for (i = 0; i < 5; i++) s->br[i] = ts_row(TSH8_BR, pa, i);
    for (i = 0; i < 3; i++) s->tone[i] = (int32_t)(((int64_t)ts_row(TSH8_TONE, pt, i) * L) >> (24 - TSH8_OSH));
    s->tone[3] = ts_row(TSH8_TONE, pt, 3); s->tone[4] = ts_row(TSH8_TONE, pt, 4);
}
static int32_t ts_step(int32_t v, int32_t tgt)
{
    return v < tgt - TSH8_GSTEP ? v + TSH8_GSTEP : v > tgt + TSH8_GSTEP ? v - TSH8_GSTEP : tgt;
}
/* the whole table of AMOUNT step a into the buffer that runs (the shared input biquad with it) */
static void ts_first(tx_drv_shared_t *sh, int32_t a)
{
    int i;
    a = a < 1 ? 1 : a > 127 ? 127 : a;
    ts_build_gf(sh, sh->ts_tab[0], a, 0u, TSH8_NODES); sh->ts_cur = 0; sh->ts_trow = a; sh->ts_bpos = -1;
    for (i = 0; i < 5; i++) sh->ts_in[i] = ts_w(&TSH8_IN[0][2 * i]);
}

/* the wet signal of the block, in -> out (out may be in), n even; returns 1 when it only built the first table (the
 * kernel not run, out untouched: the lane stays dry this block) */
static int drv_tsh8_run(tx_drv_ts_t *s, tx_drv_shared_t *sh, const int32_t *in, int32_t *out, uint32_t n,
                        int32_t a, int32_t t)
{
    const int32_t *g0 = 0;
    if (s->ra < 0) {                                       /* (after a reset: the knobs) */
        if (sh->ts_trow != a) { ts_first(sh, a); return 1; }
        s->ra = a; s->rt = t; ts_set(s, a, t);
    } else if (s->ra != a || s->rt != t) { s->ra = ts_step(s->ra, a); s->rt = ts_step(s->rt, t); ts_set(s, s->ra, s->rt); }
    if (sh->ts_bpos < 0) {                                 /* (a block builds, or swaps, or neither: never both) */
        if (sh->ts_trow != s->ra) {                                                 /* the next table: from here */
            int32_t d = s->ra - sh->ts_trow;
            sh->ts_brow = sh->ts_trow + (d > TSH8_TSTEP ? TSH8_TSTEP : d < -TSH8_TSTEP ? -TSH8_TSTEP : d);
            sh->ts_bpos = 0;
        }
    } else if (sh->ts_bpos < (int32_t)TSH8_NODES) {
        uint32_t e = (uint32_t)sh->ts_bpos + TSH8_SLICE;
        if (e > TSH8_NODES) e = TSH8_NODES;
        ts_build_gf(sh, sh->ts_tab[sh->ts_cur ^ 1u], sh->ts_brow, (uint32_t)sh->ts_bpos, e);
        sh->ts_bpos = (int32_t)e;
    } else {                                                                        /* built: it runs from here, */
        sh->ts_cur ^= 1u; sh->ts_trow = sh->ts_brow; sh->ts_bpos = -1;              /* crossfaded in */
        if (n >= TSH8_XF) g0 = sh->ts_tab[sh->ts_cur ^ 1u];
    }
    ts_kernel_aa(s, sh, g0, sh->ts_tab[sh->ts_cur], in, out, n & ~1u);
    return 0;
}
#endif
