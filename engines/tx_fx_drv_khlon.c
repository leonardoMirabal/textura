/* SPDX-License-Identifier: GPL-3.0-only */
/* DRIVE TYPE 1, KHLON: the clean-blend overdrive. A gain stage (its feedback per AMOUNT) drives a pair of germanium
 * diodes to ground through a coupling capacitor; a clean path (two clean feeds, one working against the gain pot) is
 * summed with it in an inverting amp with an active treble control (TONE: cut .. boost). AMOUNT 1..127 is the GAIN
 * knob from its minimum (a clean boost) to its maximum; TONE is the TREBLE knob.
 * Frodhes' drv_khlon.c (TYPE 2 there) as an instance (included by tx_fx_drive.c): the same integers out (checked
 * against Frodhes' file, bit for bit, on moving knobs; with its 0.9.6 dip at the wet's level and 0.9.10 POST in Q8
 * since), its state in tx_drv_kh_t, its two scratch blocks shared.
 *
 * Per sample, loops over the block, signals in volts Q20 (the bus x 32: 1.0 V for the engine's full scale):
 *   O1   the gain stage, a biquad (transposed form II, 64-bit states)                 in -> t1
 *   CL   the clean paths, first order                                                 in -> out
 *   then, per AMOUNT row ("post", AMOUNT <= 125): the clipper (the +-4.5 V rail, the coupling capacitor's state, the
 *        diodes' curve KH_GF) into t1, and the slot (its own path into the sum, first order) added into out;
 *        ("pre", 126 and 127): the slot as the gain stage's third order, then the clipper into t1, added into out
 *   POST the summing amp's pole x the treble control, a biquad; its b carry the level (per AMOUNT x per TONE: about
 *        the loudness of the drive switched off) and the volts -> bus scale
 * The clipper is the plain curve (KH_AA 0: Frodhes' build; its folding stays under -40 dB there); the anti-aliased
 * one (first-order antiderivative, no divide: count-leading-zeros, a reciprocal table, one 32 x 32 -> 64 product,
 * then a fixed droop compensation w = v + 0.148 (v - v1)) is kept for the rows from KH_AA_FROM, none while KH_AA is 0.
 * |e| stays below 9 V (the capacitor's state is a running mix of the clamped input): the table holds 0 .. 9.06 V.
 *
 * Knobs: the rows glide one step a block. The post / pre change (between AMOUNT 125 and 126) is not a row step: the
 * two modes' states do not map onto each other, so it dips through the dry signal, a block each: the wet fades out
 * (5.8 ms), the new mode starts from rest and runs a block under the dry signal, then fades in.
 * Cost (Frodhes, pi32v2 -Os, ~1.6 us per instruction per sample): post 69, pre 70, a dip block 80.5 instructions a
 * sample: 129 us a block at the worst; here +~5 a sample for the mirrored node table (KH_GP): 137 us. */
#ifdef TX_FX_DRIVE_UNIT
#ifndef KH_AA
#define KH_AA 0
#endif
#if !KH_AA
#define DRV_US_KHLON 137u              /* (129 + the mirrored node table: ~5 instructions a sample, estimated) */
#define KH_AA_FROM 127u
#else
#define DRV_US_KHLON 177u
#define KH_AA_FROM KH_AA_A
#endif
#define KH_M64(a, b) ((int64_t)(a) * (int64_t)(b))
#define KH_FN static __attribute__((noinline))

static void kh_tdf2(int64_t *st, const int32_t *c, const int32_t *h)   /* biquad: b0 b1 b2 a1 a2 */
{
    st[0] = KH_M64(c[1], h[0]) + KH_M64(c[3], h[2]) + KH_M64(c[2], h[1]) + KH_M64(c[4], h[3]);
    st[1] = KH_M64(c[2], h[0]) + KH_M64(c[4], h[2]);
}
static int64_t kh_tdf1(const int32_t *c, const int32_t *h)              /* first order: b0 b1 a1 */
{
    return KH_M64(c[1], h[0]) + KH_M64(c[2], h[2]);
}
static int32_t kh_unf(const uint32_t *r0, uint32_t i, uint32_t h)   /* a stored value (or the mean of two rows) */
{
    int32_t v = (int32_t)(r0[i] ^ 0x80000000u);    /* (stored with the sign bit flipped: tools/drive_gen/khlon.py) */
    return h ? (v + (int32_t)(r0[11u + i] ^ 0x80000000u)) >> 1 : v;
}
static uint32_t kh_mode_of(uint32_t ra) { return (uint32_t)(KH_AMAP[ra] >> 1) >= KH_PRE_ROW; }
#if KH_AA                                        /* (the anti-aliased clipper and its {g, F} table: KH_AA builds) */
static int32_t kh_F(int32_t e)                   /* F at the carried e1 (once, when the AA rows start) */
{
    int32_t a = e < 0 ? -e : e, fr = a & ((1 << 14) - 1), g0, dg;
    const int32_t *p = KH_GF + ((a >> 14) << 1);
    g0 = p[0]; dg = p[2] - g0;
    return p[1] + (int32_t)(KH_M64(fr, g0 + ((dg * fr) >> 15)) >> 13);
}
#endif
static void kh_load(tx_drv_kh_t *s)              /* the rows -> the coefficients (only when one changes) */
{
    uint32_t ga = KH_AMAP[s->ra], gr = ga >> 1, gh = ga & 1u, tr = s->rt >> 1, th = s->rt & 1u, i;
    const uint32_t *g = KH_G[gr], *t = KH_T[tr];
    int32_t gs = gh ? (KH_GS[gr] + KH_GS[gr + 1]) >> 1 : KH_GS[gr];
    int32_t lt = th ? (KH_LT[tr] + KH_LT[tr + 1]) >> 1 : KH_LT[tr];
    int32_t sc = (int32_t)(((int64_t)gs * lt + (1 << 13)) >> 14);   /* Q24 */
    for (i = 0; i < 5; i++) s->o1[i] = kh_unf(g, i, gh);
    for (i = 0; i < 3; i++) { s->cl[i] = kh_unf(g, 5 + i, gh); s->sl[i] = kh_unf(g, 8 + i, gh); }
    for (i = 0; i < 5; i++) {
        int32_t v = (int32_t)(t[i] ^ 0x80000000u);
        if (th) v = (v + (int32_t)(t[5 + i] ^ 0x80000000u)) >> 1;
        s->po[i] = i < 3 ? (int32_t)((KH_M64(v, sc) + (1 << 15)) >> 16) : v;   /* (b x 2^8: POST's output Q8) */
    }
    s->pre = (uint8_t)(gr >= KH_PRE_ROW);
#if KH_AA
    if (s->ra >= KH_AA_FROM && !s->aa) s->F1 = kh_F(s->e1);   /* (the AA rows start: F1 from the carried e1) */
    s->aa = s->ra >= KH_AA_FROM;
#endif
    s->gf = KH_GP; s->kc4 = KH_KC4; s->rail = KH_RAIL;
}
static void kh_rest(tx_drv_kh_t *s)              /* every state to rest */
{
    s->so1[0] = s->so1[1] = s->spo[0] = s->spo[1] = s->scl = s->ssl = 0;
    s->sc = s->e1 = s->F1 = s->v1 = 0;              /* (F(0) = 0) */
    tx_fx_memset(s->ho1, 0, sizeof s->ho1); tx_fx_memset(s->hcl, 0, sizeof s->hcl);
    tx_fx_memset(s->hsl, 0, sizeof s->hsl); tx_fx_memset(s->hpo, 0, sizeof s->hpo);
}
static void kh_hist(int32_t *h, int32_t x1, int32_t x2, int32_t y1, int32_t y2)
{
    h[0] = x1; h[1] = x2; h[2] = y1; h[3] = y2;
}
static void drv_khlon_reset(tx_drv_kh_t *s)
{
    kh_rest(s);
    s->ra = 0xFFu; s->dip = s->held = 0; s->aa = 0; s->dg = 4096;      /* the next run starts at the knobs' rows, no glide */
}

#if KH_AA
/* the clipper's input: e = rail(x) - sc; its node pointer, fraction, g0, dg for |e| (the curve is odd) */
#define KH_E(x)                                                                                 \
            e = (x); e = e < nL ? nL : e; e = e > L ? L : e; e -= sc;                           \
            a = e < 0 ? -e : e;                                                                 \
            p = gf + ((a >> 14) << 1);                                                          \
            fr = a & ((1 << 14) - 1);                                                           \
            g0 = p[0]; dg = p[2] - g0;
#define KH_AA_SF 12                                     /* F in Q28 = Q40 >> 12 */
#define KH_BETA_Q8 38                                   /* the droop compensation: 0.148 */
#endif
/* O1 and CL in one pass (the input read and scaled once): in -> t (O1), u (CL); in may be u */
KH_FN void kh_o1cl_pass(tx_drv_kh_t *s, int32_t *t, const int32_t *in, int32_t *u, uint32_t n)
{
    int32_t b0 = s->o1[0], b1 = s->o1[1], b2 = s->o1[2], a1 = s->o1[3], a2 = s->o1[4];
    int32_t c0 = s->cl[0], c1 = s->cl[1], ca = s->cl[2];
    int64_t s1 = s->so1[0], s2 = s->so1[1], sc = s->scl;
    const int32_t *e = in + n;
    while (in < e) {
        int32_t x = *in++ * 32, y, z;
        y = (int32_t)((s1 + KH_M64(b0, x)) >> 24);
        s1 = s2 + KH_M64(b1, x) + KH_M64(a1, y);
        s2 = KH_M64(b2, x) + KH_M64(a2, y);
        *t++ = y;
        z = (int32_t)((sc + KH_M64(c0, x)) >> 24);
        sc = KH_M64(c1, x) + KH_M64(ca, z);
        *u++ = z;
    }
    s->so1[0] = s1; s->so1[1] = s2; s->scl = sc;
}
/* the plain clipper on the curve's nodes for e >= 0 (KH_GP: g, Q21, even), mirrored at run time, the same integers
 * as a table over the whole range with its negative half mirrored (Frodhes' KH_GFS, 9288 B; KH_GP 2324 B): e's cell
 * and fraction come from e itself (two's complement: e = (e >> 14) 2^14 + (e & 16383)); a cell left of 0 reads its
 * nodes -(cell) and -(cell) - 1, negated (x ^ sg - sg: no branch). Keeps e1 current for a switch to the AA rows. KH_PV:
 * one sample, e and v. */
#define KH_PV(x)                                                                                   \
            e = (x); e = e < nL ? nL : e; e = e > L ? L : e; e -= sc;                              \
            sg = e >> 31; m = ((e >> 14) ^ sg) - sg;                                               \
            g0 = (gs[m] ^ sg) - sg; d = ((gs[m + 1 + 2 * sg] ^ sg) - sg) - g0;                     \
            v = (g0 + ((d * (e & ((1 << 14) - 1))) >> 14)) >> 1;                                   \
            sc += (int32_t)(KH_M64(e - v, kc4) >> 32);
/* post: the clipper in place on t */
KH_FN void kh_clipv_pass(tx_drv_kh_t *s, int32_t *t, uint32_t n)
{
    const int32_t *gs = s->gf;
    int32_t sc = s->sc, kc4 = s->kc4, L = s->rail, nL = -s->rail, e = s->e1, *te = t + n;
    while (t < te) {
        int32_t g0, v, sg, m, d;
        KH_PV(*t)
        *t++ = v;
    }
    s->sc = sc; s->e1 = e;
}
/* pre: the clipper, its V_S added into u (t keeps the input: the last two for the history) */
KH_FN void kh_clipvadd_pass(tx_drv_kh_t *s, const int32_t *t, int32_t *u, uint32_t n)
{
    const int32_t *gs = s->gf;
    int32_t sc = s->sc, kc4 = s->kc4, L = s->rail, nL = -s->rail, e = s->e1, *ue = u + n;
    while (u < ue) {
        int32_t g0, v, sg, m, d;
        KH_PV(*t++)
        *u++ += v;
    }
    s->sc = sc; s->e1 = e;
}
#if KH_AA
/* the anti-aliased clipper (rows from KH_AA_FROM), V_S into t: F(e) = F[k] + (fr (g0 + (dg fr) >> 15)) >> 13;
 * |d| = |e - e1| >= 2^12: v = (F - F1) 2^12 / d by clz, KH_RCP and one 64-bit product; else g at the midpoint */
KH_FN void kh_clipaa_pass(tx_drv_kh_t *s, int32_t *t, uint32_t n)
{
    const int32_t *gf = KH_GF;
    const uint16_t *rc = (const uint16_t *)((uintptr_t)KH_RCP - (2u << 10));   /* (indexed from 2^10) */
    int32_t sc = s->sc, kc4 = s->kc4, L = s->rail, nL = -s->rail, e1 = s->e1, F1 = s->F1;
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t e, a, fr, g0, dg, pr, F, d, v;
        uint32_t ad;
        const int32_t *p;
        KH_E(t[i])
        pr = dg * fr;
        F = p[1] + (int32_t)(KH_M64(fr, g0 + (pr >> 15)) >> 13);
        d = e - e1;
        ad = (uint32_t)(d < 0 ? -d : d);
        if (__builtin_expect(ad >= (1u << KH_AA_SF), 1)) {
            int nz = __builtin_clz(ad);
            uint32_t r = rc[(ad << nz) >> (32 - 11)];
            int32_t q = (int32_t)(KH_M64(F - F1, r) >> (47 - KH_AA_SF - nz));
            v = d < 0 ? -q : q;
        } else {
            int32_t gm = g0 + (pr >> 14);
            v = ((e < 0 ? -gm : gm) - ((dg * d) >> 15)) >> 1;
        }
        e1 = e; F1 = F;
        sc += (int32_t)(KH_M64(e - v, kc4) >> 32);
        t[i] = v;
    }
    s->sc = sc; s->e1 = e1; s->F1 = F1;
}
#endif
KH_FN void kh_sladd_pass(tx_drv_kh_t *s, const int32_t *t, int32_t *u, uint32_t n)   /* post: u += the slot (V_S) */
{
    int32_t b0 = s->sl[0], b1 = s->sl[1], a1 = s->sl[2];
    int64_t st = s->ssl;
    const int32_t *te = t + n;
    while (t < te) {
        int32_t x = *t++, y;
        y = (int32_t)((st + KH_M64(b0, x)) >> 24);
        st = KH_M64(b1, x) + KH_M64(a1, y);
        *u++ += y;
    }
    s->ssl = st;
}
KH_FN void kh_sl_pass(tx_drv_kh_t *s, int32_t *t, uint32_t n)   /* pre: the third order, in place */
{
    int32_t b0 = s->sl[0], b1 = s->sl[1], a1 = s->sl[2];
    int64_t st = s->ssl;
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t x = t[i], y;
        y = (int32_t)((st + KH_M64(b0, x)) >> 24);
        st = KH_M64(b1, x) + KH_M64(a1, y);
        t[i] = y;
    }
    s->ssl = st;
}
#if KH_AA
/* the AA rows' droop compensation w = v + beta (v - v1), fused where V_S enters the sum: post mode u += slot(w) */
KH_FN void kh_slfadd_pass(tx_drv_kh_t *s, const int32_t *t, int32_t *u, uint32_t n)
{
    int32_t b0 = s->sl[0], b1 = s->sl[1], a1 = s->sl[2], v1 = s->v1;
    int64_t st = s->ssl;
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t v = t[i], x = v + (((v - v1) * KH_BETA_Q8) >> 8), y;
        v1 = v;
        y = (int32_t)((st + KH_M64(b0, x)) >> 24);
        st = KH_M64(b1, x) + KH_M64(a1, y);
        u[i] += y;
    }
    s->ssl = st; s->v1 = v1;
}
KH_FN void kh_firadd_pass(tx_drv_kh_t *s, const int32_t *t, int32_t *u, uint32_t n)   /* pre mode: u += w */
{
    int32_t v1 = s->v1;
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t v = t[i];
        u[i] += v + (((v - v1) * KH_BETA_Q8) >> 8);
        v1 = v;
    }
    s->v1 = v1;
}
#endif
/* POST, in place, transposed form II. Its output carries 8 bits below the bus's LSB through the recursion (b x 2^8:
 * kh_load), cut to the bus on the way out (Frodhes 83fa7520): with the output floored to the bus in the loop, its
 * rounding error went round the poles (a DC gain of ~95 at GAIN 20) and a quiet note (below ~-60 dBFS in) sat in a
 * dead band or came out at H3 -5..-15 dB, over a standing DC of ~-40 LSB. The last two outputs (Q8) are POST's history
 * (hpo[2], hpo[3]). */
KH_FN void kh_post_pass(tx_drv_kh_t *s, int32_t *u, uint32_t n)
{
    int32_t b0 = s->po[0], b1 = s->po[1], b2 = s->po[2], a1 = s->po[3], a2 = s->po[4], y = 0, y1 = 0;
    int64_t s1 = s->spo[0], s2 = s->spo[1];
    uint32_t i;
    for (i = 0; i < n; i++) {
        int32_t x = u[i];
        int64_t t = s1 + KH_M64(b0, x);
        y1 = y;
        y = (int32_t)(t >> 24);
        u[i] = (int32_t)(t >> 32);                      /* (y >> 8: the sum's high word) */
        s1 = s2 + KH_M64(b1, x) + KH_M64(a1, y);
        s2 = KH_M64(b2, x) + KH_M64(a2, y);
    }
    s->spo[0] = s1; s->spo[1] = s2; s->hpo[2] = y; s->hpo[3] = y1;
}
/* the dip's dry at the wet's level (Frodhes a316e7da: the wet and the dry differ by a few dB, so the 17 ms dip through
 * the plain dry was a level jump of ~3.5 dB): the dry x dg (Q12, 1/4 .. 4), the RMS ratio of the wet to the dry over
 * the dip's first block (every 4th sample), taken there (a square root of each, then a 32-bit divide) */
static uint32_t kh_isqrt(uint64_t v)              /* floor(sqrt(v)), bit by bit (no divide) */
{
    uint64_t r = 0, b = (uint64_t)1 << 62;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return (uint32_t)r;
}
static void kh_dip_gain(tx_drv_kh_t *s, const int32_t *dry, const int32_t *y, uint32_t n)
{
    uint64_t ew = 0, ed = 0;
    uint32_t i, rw, rd;
    for (i = 0; i < n; i += 4u) { ew += (uint64_t)KH_M64(y[i], y[i]); ed += (uint64_t)KH_M64(dry[i], dry[i]); }
    rw = kh_isqrt(ew); rd = kh_isqrt(ed);
    while (rw >= (1u << 18) || rd >= (1u << 18)) { rw >>= 1; rd >>= 1; }
    s->dg = !rd ? 4096 : (int32_t)((rw << 12) / rd);
    s->dg = s->dg < 1024 ? 1024 : s->dg > 16384 ? 16384 : s->dg;
}
#define KH_DRY(d) ((int32_t)(KH_M64(d, dg) >> 12))
/* a dip's fade block: POST into u, then the blend over the dry signal (x dg), out = dry + (POST(u) - dry) w / 256,
 * w = 1..n (up: fading in) or n - 1..0 (fading out) a sample, the product 32-bit (|POST(u) - dry| < 2^23) */
KH_FN void kh_fade_pass(const int32_t *dry, const int32_t *y, int32_t *out, uint32_t n, int32_t w, int32_t dw,
                        int32_t dg)
{
    const int32_t *e = dry + (n & ~1u);
    while (dry < e) {
        int32_t d0 = KH_DRY(dry[0]), d1 = KH_DRY(dry[1]);
        out[0] = d0 + (((y[0] - d0) * w) >> 8);
        out[1] = d1 + (((y[1] - d1) * (w + dw)) >> 8);
        dry += 2; y += 2; out += 2; w += 2 * dw;
    }
    if (n & 1u) { int32_t d0 = KH_DRY(dry[0]); out[0] = d0 + (((y[0] - d0) * w) >> 8); }
}
static void kh_fade(tx_drv_kh_t *s, const int32_t *in, int32_t *u, int32_t *out, uint32_t n, uint32_t up)
{
    kh_post_pass(s, u, n);
    if (up) kh_fade_pass(in, u, out, n, 1, 1, s->dg);
    else { kh_dip_gain(s, in, u, n); kh_fade_pass(in, u, out, n, (int32_t)n - 1, -1, s->dg); }
}

/* the wet signal of the block, in -> out (out may be in); 3 <= n <= TX_DRV_BLK */
static void drv_khlon_run(tx_drv_kh_t *s, tx_drv_scr_t *sh, const int32_t *in, int32_t *out, uint32_t n,
                          int32_t amount, int32_t tone)
{
    uint32_t ta = (uint32_t)(amount - 1), tt = (uint32_t)(tone + ((tone + 1) >> 7));   /* 0..126, 0..128 */
    uint32_t tpre = kh_mode_of(ta), ph = s->dip;
    int32_t *u = out, *t = sh->t1, cl1, cl2, x1, x2;
    if (s->ra == 0xFFu) { s->ra = (uint8_t)ta; s->rt = (uint8_t)tt; kh_load(s); ph = 0; }
    else {
        uint32_t ra = s->ra, rt = s->rt;
        if (!ph && tpre != s->pre) ph = 1;       /* the other mode: the dip (1 fade out, 2 under the dry, 3 fade in) */
        if (ph == 2) {                           /* the dip's floor: the mode the knob is in, from rest */
            ra = ta; rt = tt; kh_rest(s);
            s->ra = (uint8_t)ra; s->rt = (uint8_t)rt; kh_load(s);
        } else if (!ph && !s->held) {            /* one row a block, inside the mode that runs (held while it dips, and
                                                  * the block after: POST's history is whole again) */
            if (ra != ta) {
                uint32_t nr = ra < ta ? ra + 1u : ra - 1u;
                if (kh_mode_of(nr) == s->pre) ra = nr;
            }
            rt = rt < tt ? rt + 1u : rt > tt ? rt - 1u : rt;
            if (ra != s->ra || rt != s->rt) {    /* new coefficients; the states from the history */
                s->ra = (uint8_t)ra; s->rt = (uint8_t)rt; kh_load(s);
                kh_tdf2(s->so1, s->o1, s->ho1); s->scl = kh_tdf1(s->cl, s->hcl);
                s->ssl = kh_tdf1(s->sl, s->hsl); kh_tdf2(s->spo, s->po, s->hpo);
            }
        }
    }
    if (ph) u = sh->t2;                          /* (the dry signal stays in in) */
    x1 = in[n - 1] * 32; x2 = in[n - 2] * 32;    /* (in may be u: overwritten) */
    kh_o1cl_pass(s, t, in, u, n);
    kh_hist(s->ho1, x1, x2, t[n - 1], t[n - 2]);
    kh_hist(s->hcl, x1, x2, cl1 = u[n - 1], cl2 = u[n - 2]);
    if (!s->pre) {
        int32_t w1, w2;
#if KH_AA
        if (s->aa) {
            kh_clipaa_pass(s, t, n); kh_slfadd_pass(s, t, u, n);
            w1 = t[n - 1] + (((t[n - 1] - t[n - 2]) * KH_BETA_Q8) >> 8);
            w2 = t[n - 2] + (((t[n - 2] - t[n - 3]) * KH_BETA_Q8) >> 8);
        } else
#endif
        {
            kh_clipv_pass(s, t, n); kh_sladd_pass(s, t, u, n);
            s->v1 = w1 = t[n - 1]; w2 = t[n - 2];
        }
        kh_hist(s->hsl, w1, w2, u[n - 1] - cl1, u[n - 2] - cl2);
    } else {
        kh_sl_pass(s, t, n);
        kh_hist(s->hsl, s->ho1[2], s->ho1[3], t[n - 1], t[n - 2]);
#if KH_AA
        if (s->aa) { kh_clipaa_pass(s, t, n); kh_firadd_pass(s, t, u, n); }
        else
#endif
        kh_clipvadd_pass(s, t, u, n);
    }
    s->hpo[0] = u[n - 1]; s->hpo[1] = u[n - 2];
    if (!ph) kh_post_pass(s, u, n);
    else {
        if (ph != 2) kh_fade(s, in, u, out, n, ph == 3);
        else {
            int32_t dg = s->dg;
            uint32_t i;
            kh_post_pass(s, u, n);
            for (i = 0; i < n; i++) out[i] = KH_DRY(in[i]);   /* (the dry at the wet's level) */
        }
        ph = ph < 3u ? ph + 1u : 0u;
    }
    s->held = u != out;
    s->dip = (uint8_t)ph;
}
#endif
