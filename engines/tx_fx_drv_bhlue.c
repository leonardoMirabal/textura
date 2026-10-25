/* SPDX-License-Identifier: GPL-3.0-only */
/* DRIVE TYPE 4, BHLUE: the two-stage soft overdrive: a treble-boosting clean stage, then a stage whose feedback holds
 * two + two diodes in series with a resistor: a soft symmetric clip that never goes flat; a treble-cut tone. The
 * circuit and its tables: tools/drive_gen/bhlue.py (the stock values). 1x, no oversampling, no divide.
 * Frodhes' drv_bhlue.c (TYPE 5 there, its anti-aliased kernel) as an instance (included by tx_fx_drive.c): the same
 * integers out (checked against Frodhes' file, bit for bit, on moving knobs); the clipper's {g, F} table is a
 * constant here (BHLUE_GF, generated: no RAM, nothing built at boot).
 *
 * Once a block (n <= TX_DRV_BLK, even), four passes, two samples per iteration where it pays:
 *   1  the two coupling high-passes (input, stage 2), x 2^12 (Q27; x clamped to +-2^18 first: 8 x full scale), and
 *      the P shelf (sinc / cos: the clipper's two-sample average compensated)
 *   2  PRE: stage 1 x stage 2's linear gain, one biquad per AMOUNT step (b Q15, a Q28), held to +-64 V   -> u, in m
 *   3  the clipper with first-order antiderivative anti-aliasing (no divide: count-leading-zeros, the shared
 *      reciprocal table, one product): {g, F} per 1/16 V node, odd
 *   4  POST: the tone x the volume load x the level x Q, one biquad per TONE step (c Q25, d Q28)  -> m (Q15)
 * Rows: 64 per knob plus a few of their own (tools/drive_gen/bhlue.py), a step between two interpolated when it moves
 * (BHLUE_PA / BHLUE_PT); the levels the same way.
 * Knobs: the steps that run glide toward the knobs, BHLUE_GSTEP steps a block (the whole range in 64 blocks, 0.37 s).
 * Cost (Frodhes, pi32v2 -Os): 24.5 + 19.5 + 38 + 15.5 = 97.5 instructions a sample, 156 us at 1.6, + set-up: 157 us. */
#ifdef TX_FX_DRIVE_UNIT
#define DRV_US_BHLUE 157u
_Static_assert(BHLUE_AA, "BHLUE runs its anti-aliased kernel only");
#define BHLUE_GSTEP 2                                    /* AMOUNT / TONE steps the rows move at most a block */
#define BH_CLAMP (1 << 18)
#define BHM(a, b) ((int64_t)(a) * (b))
#define BH_XMAX ((64 << 16) - 1)                          /* |u| held to the table's 64 V (PRE's own clamp) */
/* the same hold before the >> 4: (u >> 4) in [-BH_XMAX, BH_XMAX] <=> u in [-16 BH_XMAX, 16 BH_XMAX + 15] */
#define BH_UHI (16 * BH_XMAX + 15)
#define BH_ULO (-16 * BH_XMAX)
#define BH_AT (1 << 9)                                    /* |du| below this (2^-7 V): g at the midpoint */

static void drv_bhlue_reset(tx_drv_bh_t *s)
{
    tx_fx_memset(s, 0, sizeof *s);
    s->ra = -1;
}

/* pass 1: the two high-passes and the P shelf, 2 samples per iteration (x clamped first) */
static __attribute__((noinline)) void bh_hps2qs(const tx_drv_bhk_t *k, tx_drv_bhs_t *st, const int32_t *in,
                                                int32_t *m, uint32_t n)
{
    int32_t xa1 = st->xa1, x1 = st->x1, h1 = st->xo, y1 = st->up;
    uint32_t j;
    for (j = n >> 1; j; j--) {
        int32_t ma = in[0], mb = in[1], xa, xb, ga, gb, ha, hb, ya, c;
        c = k->lo; ma = ma > c ? ma : c; mb = mb > c ? mb : c;
        c = k->hi; ma = ma < c ? ma : c; mb = mb < c ? mb : c;
        xa = ma * 4096; xb = mb * 4096;
        c = k->pa;
        ga = (xa - xa1) + (int32_t)(BHM(c, x1) >> 30);
        gb = (xb - xa) + (int32_t)(BHM(c, ga) >> 30);
        c = k->ph;
        ha = (ga - x1) + (int32_t)(BHM(c, h1) >> 30);
        hb = (gb - ga) + (int32_t)(BHM(c, ha) >> 30);
        ya = (int32_t)((BHM(k->s0, ha) + BHM(k->s1, h1) + BHM(k->sp, y1)) >> 28);
        y1 = (int32_t)((BHM(k->s0, hb) + BHM(k->s1, ha) + BHM(k->sp, ya)) >> 28);
        xa1 = xb; x1 = gb; h1 = hb;
        m[0] = ya; m[1] = y1;
        in += 2; m += 2;
    }
    st->xa1 = xa1; st->x1 = x1; st->xo = h1; st->up = y1;
}
/* pass 2: PRE, its output held to +-64 V */
static __attribute__((noinline)) void bh_pre2qc(const tx_drv_bhk_t *k, tx_drv_bhs_t *st, int32_t *m, uint32_t n)
{
    int32_t h1 = st->h1, h2 = st->h2, u1 = st->u1, u2 = st->u2;
    uint32_t j;
    for (j = n >> 1; j; j--) {
        int32_t ha = m[0], hb = m[1], ua, ub, c, sa, sb;
        int64_t pa, pb;
        c = k->b2; pa = BHM(c, h2); pb = BHM(c, h1);
        c = k->b1; pa += BHM(c, h1); pb += BHM(c, ha);
        c = k->b0; pa += BHM(c, ha); pb += BHM(c, hb);
        pa *= 64; pb *= 64;
        c = k->a2; pa -= BHM(c, u2); pb -= BHM(c, u1);
        c = k->a1; pa -= BHM(c, u1);
        ua = (int32_t)(pa >> 28);
        pb -= BHM(c, ua);
        ub = (int32_t)(pb >> 28);
        h2 = ha; h1 = hb; u2 = ua; u1 = ub;
        sa = ua > BH_UHI ? BH_UHI : ua; sa = sa < BH_ULO ? BH_ULO : sa; sa >>= 4;
        sb = ub > BH_UHI ? BH_UHI : ub; sb = sb < BH_ULO ? BH_ULO : sb; sb >>= 4;
        m[0] = sa; m[1] = sb;
        m += 2;
    }
    st->h1 = h1; st->h2 = h2; st->u1 = u1; st->u2 = u2;
}
/* pass 3: the clipper with its antiderivative: {g, F} per 1/16 V node (g Q16 V, F >> 9) */
static __attribute__((noinline)) void bh_aa1(tx_drv_bhs_t *st, int32_t *m, uint32_t n)
{
    const uint16_t *rcp = (const uint16_t *)((uintptr_t)DRV_AA_RCP - (2u << (DRV_AA_RB - 1)));   /* (from 2^(RB-1)) */
    int32_t x1 = st->ax1, F1 = st->aF1;
    uint32_t j;
    for (j = n; j; j--) {
        int32_t x = m[0], a, fr, g0, dg, p, F, d, v;
        uint32_t ad;
        const int32_t *e;
        a = x < 0 ? -x : x;
        e = &BHLUE_GF[0][0] + 2 * (a >> 12); fr = a & 4095;
        g0 = e[0]; dg = e[2] - g0; p = dg * fr;
        F = e[1] + (int32_t)(BHM(fr, g0 * 8192 + p) >> (13 + 9));
        d = x - x1;
        ad = (uint32_t)(d < 0 ? -d : d);
        if (ad < BH_AT) {
            int32_t v_ = g0 + (p >> 12);
            v_ = x < 0 ? -v_ : v_;
            v = v_ - ((dg * d) >> 13);
        } else {
            int nz = __builtin_clz(ad);
            int32_t r = (int32_t)rcp[(ad << nz) >> (32 - DRV_AA_RB)];
            int32_t df = F - F1;
            if (d < 0) df = -df;
            v = (int32_t)(BHM(df, r) >> (47 - 9 - nz));
        }
        x1 = x; F1 = F;
        *m++ = v;
    }
    st->ax1 = x1; st->aF1 = F1;
}
/* pass 4: POST */
static __attribute__((noinline)) void bh_post2q(const tx_drv_bhk_t *k, tx_drv_bhs_t *st, int32_t *m, uint32_t n)
{
    int32_t w1 = st->w1, w2 = st->w2, z1 = st->z1, z2 = st->z2;
    uint32_t j;
    for (j = n >> 1; j; j--) {
        int32_t va = m[0], vb = m[1], za, zb, c;
        int64_t pa, pb;
        c = k->c2; pa = BHM(c, w2); pb = BHM(c, w1);
        c = k->c1; pa += BHM(c, w1); pb += BHM(c, va);
        c = k->c0; pa += BHM(c, va); pb += BHM(c, vb);
        pa *= 1024; pb *= 1024;
        c = k->d2; pa -= BHM(c, z2); pb -= BHM(c, z1);
        c = k->d1; pa -= BHM(c, z1);
        za = (int32_t)(pa >> 28);
        pb -= BHM(c, za);
        zb = (int32_t)(pb >> 28);
        w2 = va; w1 = vb; z2 = za; z1 = zb;
        m[0] = za >> 8; m[1] = zb >> 8;
        m += 2;
    }
    st->w1 = w1; st->w2 = w2; st->z1 = z1; st->z2 = z2;
}

/* a table word stored as two int16 (high, low: no word in the chip-ROM window in the image) */
static inline int32_t bh_w(const int16_t *p, int i) { return (int32_t)(((uint32_t)(uint16_t)p[2 * i] << 16) | (uint16_t)p[2 * i + 1]); }
/* r0 + ((r1 - r0) w >> 8), 64-bit (tools/drive_gen/bhlue.py lerp) */
static int32_t bh_lerp(int32_t r0, int32_t r1, int32_t w) { return r0 + (int32_t)((((int64_t)r1 - r0) * w) >> 8); }
/* word i of a split16 row table at position pos (row << 8 | w): the row, or between it and the next (k words a row) */
static int32_t bh_row(const int16_t *R, int k, uint32_t pos, int i)
{
    const int16_t *p = R + 2 * k * (int)(pos >> 8);
    return pos & 255u ? bh_lerp(bh_w(p, i), bh_w(p + 2 * k, i), (int32_t)(pos & 255u)) : bh_w(p, i);
}
static int32_t bh_lv(const int32_t *L, uint32_t pos)
{
    return pos & 255u ? bh_lerp(L[pos >> 8], L[(pos >> 8) + 1u], (int32_t)(pos & 255u)) : L[pos >> 8];
}
/* the rows of steps (a, t); POST's c x the level: c' = c L >> 24, L = LVA[a] LVT[t] >> 15 (exactly c at L = 2^24) */
static void bh_set(tx_drv_bh_t *s, int32_t a, int32_t t)
{
    uint32_t pa = BHLUE_PA[a], pt = BHLUE_PT[t];
    int32_t L = (int32_t)(((int64_t)bh_lv(BHLUE_LVA, pa) * bh_lv(BHLUE_LVT, pt)) >> 15), r[7], c[3], i;
    tx_drv_bhk_t *k = &s->k;
    for (i = 0; i < 7; i++) r[i] = bh_row(&BHLUE_PRE[0][0], 7, pa, i);          /* pa ph b0 b1 b2 a1 a2 */
    for (i = 0; i < 3; i++) c[i] = (int32_t)(((int64_t)bh_row(&BHLUE_POST[0][0], 5, pt, i) * L) >> 24);
    k->pa = r[0]; k->ph = r[1]; k->b0 = r[2]; k->b1 = r[3]; k->b2 = r[4]; k->a1 = r[5]; k->a2 = r[6];
    k->c0 = c[0]; k->c1 = c[1]; k->c2 = c[2];
    k->d1 = bh_row(&BHLUE_POST[0][0], 5, pt, 3); k->d2 = bh_row(&BHLUE_POST[0][0], 5, pt, 4);
    k->lo = -BH_CLAMP; k->hi = BH_CLAMP;
    k->s0 = BHLUE_SH[0]; k->s1 = BHLUE_SH[1]; k->sp = BHLUE_SH[2];
}
static int32_t bh_step(int32_t v, int32_t tgt)
{
    return v < tgt - BHLUE_GSTEP ? v + BHLUE_GSTEP : v > tgt + BHLUE_GSTEP ? v - BHLUE_GSTEP : tgt;
}

/* the wet signal of the block, in -> out (out may be in), n even */
static void drv_bhlue_run(tx_drv_bh_t *s, const int32_t *in, int32_t *out, uint32_t n, int32_t a, int32_t t)
{
    if (s->ra < 0) { s->ra = a; s->rt = t; bh_set(s, a, t); }                       /* (after a reset: the knobs) */
    else if (s->ra != a || s->rt != t) { s->ra = bh_step(s->ra, a); s->rt = bh_step(s->rt, t); bh_set(s, s->ra, s->rt); }
    n &= ~1u;
    bh_hps2qs(&s->k, &s->s, in, out, n);
    bh_pre2qc(&s->k, &s->s, out, n);
    bh_aa1(&s->s, out, n);
    bh_post2q(&s->k, &s->s, out, n);
}
#endif
