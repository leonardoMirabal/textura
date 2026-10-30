/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura reverb TYPEs INFINITE and DARK: Frodhes' infinite tank (rev_inf.c, its LITE build: our own), as one
 * instance. DECAY 0..119: T60 4..60 s; 120..126: SUSTAIN (the tail never decays and what is played piles into it, up
 * to a bounded level); 127: FREEZE (the input closes, the tail at that moment is held for ever).
 *
 *   in (HL_NC samples at 44.1 kHz) -> TONE's input one-pole -> the input's gain (FREEZE's fade x the level guard x a
 *   limiter on the block's peak) -> 3 diffusers (allpass, g 0.5, lifting steps) -> 8 lines (41..84 ms x SIZE, primes),
 *   every one read modulated (allpass interpolation: no high loss, a held tank keeps its colour), the damping one-pole
 *   and the line's gain in one product -> line 0's DC block -> the 8 x 8 Householder mix (lossless) -> back into the
 *   lines (int16). Mid / side from the line ends. It runs every 4th control block for 128 samples (its fixed costs
 *   once), every read moved each time.
 * Textura runs it at the full rate: the design's lengths and swing x 2, its one-poles as at the half rate (rv_pole,
 * rv_weight), its gains from the same times (calibrated: the tails' T30 as at the half rate, within 5 % over DECAY and
 * TONE); every line modulated and every read moved each tank block (the design: 4 modulated, a read every 4th).
 *
 * The loop loses nothing to its rounding (a 60 s T60 holds at any level): the reads round, the line ends and the mix
 * carry their remainders, the allpasses are lifting steps; a quiet line truncates toward zero instead, so a tail that
 * is not held ends in exact zeros. SUSTAIN / FREEZE: every line's gain is 1 + dq, a level controller's.
 *
 * DARK (Textura): the same tank darker and still: no wander (MOD 0: the reads stand, nothing beats), TONE's damping
 * 60 steps darker (the input one-pole near 250 Hz and lower, the top's absolute damping up), so a FREEZE holds a deep,
 * soft bed. The framework sets it (c->dark: damp + 60, mod 0).
 *
 * DRIFT (Textura): the same tank, its wander a slow random walk instead of the slow sines: each wandering read follows
 * two one-poles (0.75 s each) toward a target drawn at random every 1.5 .. 6 s, over twice the swing, so the tail
 * slowly bends in pitch, never in a cycle; the reads stay allpass-interpolated (clean, no high loss). c->drift: the
 * framework's random state (never reseeded: no two tails alike). */
#include "tx_fx_kern.h"

#define RI_LCPT 2u                  /* log2 RI_CPT */
#define RI_MODQ8 (20u * 256u)       /* the swing at MOD 64, Q8 samples (+-10 at 22.05 kHz, +-20 here: a pass; the lines 2.39x
                                    * shorter, as much a second as the +-24 before) */
#define RI_KD 9                     /* TONE's damping ratio R = damp x RI_KD / 256 */
#define RI_KA 76                    /* TONE darker than its middle: an absolute damping on top, Q14 per step and pass */
#define RI_KDD 40                   /* DARK: TONE's damping with the loss, x 2.5 (Q4; the short lines' more passes:
                                    * DARK's tails at DECAY 0 .. 100 as with the long lines) */
#define RI_KI 2400u                 /* TONE's input one-pole: k = 2^(-damp x RI_KI / 65536) */
#define RI_IN 16844                 /* the input's level, Q15 (the tank's level as with 2.39x longer lines) */
#define RI_CMIN 550                 /* every line's least damping (Q14) a pass: the rounding's noise cannot build at
                                    * the top */
#define RI_KMAX 12288               /* the input one-pole's brightest */
#define RI_HOT 2u                   /* the tank runs 4x hotter than its input */
#define RI_XMAX 12000               /* the input limiter */
#define RI_LV 16000                 /* the level guard */
#define RI_DQMAX (64 << 8)
#define RI_DQCUT (256 << 8)
#define RI_SUS 10000
#define RI_KP 16
#define RI_XQ 64
#define RI_LNB 7u                   /* log2 RI_NB */
#define RI_FIN_STEP (475 * (int32_t)RI_CPT)   /* the freeze fade: 50 ms */
#define RI_DCK 13u                  /* the loop's DC block: 0.86 Hz */
#define RI_LOUD 24
#define RI_REL (-31)
#define RI_LOSS 140u
#define RI_WK 8u                    /* DRIFT: the walk's one-poles, 2^-8 a read (a read every tank block: 2.9 ms) */
#define RI_WN0 516u                 /* DRIFT: reads to the next target: 516 .. 2049 (1.5 .. 6 s) */
#define RI_GQ 3929u                 /* the gains' exponent, Q14 (the design's 59 / 256 = 3776: the full rate's tails
                                     * 4 % longer, their damping and swing at the full rate; DARK: 3776) */
static const uint16_t RI_LEN[RI_NL] = {907 * RV_X, 1021 * RV_X, 1129 * RV_X, 1237 * RV_X, 1373 * RV_X, 1511 * RV_X,
                                       1669 * RV_X, 1847 * RV_X};   /* primes (half rate), 41 .. 84 ms at
                                    * SIZE 127: in ROOM's memory (the longest + the swing at MOD 127 < RI_N) */
static const uint16_t RI_AP[RI_NDIF] = {127 * RV_X, 193 * RV_X, 277 * RV_X};
static const uint32_t RI_INC[RI_NL] = {218157u, 645121u, 324119u, 751083u, 433197u, 860162u, 539159u, 966124u};
                                    /* 0.07 .. 0.31 Hz, the LFO step a control block (f x 32 / 44100 x 2^32) */

void tx_inf_start(tx_inf_t *s)
{
    tx_fx_memset(s, 0, offsetof(tx_inf_t, hy));         /* the rest reads as empty until written (O(1)) */
    s->ilim = 32768;
    s->fin = 32768;
    s->gi = RI_IN;
    s->ik = 16384;
    s->gdec = s->gsz = -1;
    s->rd = 119;
}
int tx_inf_active(const tx_inf_t *s) { return s->quiet < RI_N + 2u * 1024u; }

static inline void ri_dif2(int16_t *q, int32_t *x, uint32_t n, int z)   /* the diffusers: g 0.5, two lifting steps */
{
    int16_t *e = q + n;
    if (z)
        for (; q < e; q++, x++) { int32_t v = hl_c16(*x); *q = (int16_t)v; *x = -(v >> 1); }
    else
        for (; q < e; q++, x++) {
            int32_t b = *q, v = hl_c16(*x + (b >> 1));
            *q = (int16_t)v;
            *x = b - (v >> 1);
        }
}
static inline void ri_end(const int16_t *b, int32_t *y, uint32_t n, int32_t a, int32_t b0, int32_t a1, int32_t *yp,
                          int32_t *op, int32_t *oe, int lo)
{
    int32_t p = *b, q, v = *yp, o = *op, e = *oe;
    if (lo)
        do {
            b += RI_NL; q = *b;
            v = p + ((a * (q - v) + 16384) >> 15);
            o = sh_tz(b0 * v + a1 * o, 14);
            *y = o; y += RI_NL / 2u; p = q;
        } while (--n);
    else
        do {
            b += RI_NL; q = *b;
            v = p + ((a * (q - v) + 16384) >> 15);
            o = b0 * v + a1 * o + e;
            e = o & 16383;
            o >>= 14;
            *y = o; y += RI_NL / 2u; p = q;
        } while (--n);
    *yp = v; *op = o; *oe = lo ? 0 : e;
}
#define RI_RUNS(d, len, k, s, s0, body)                                                     \
    for (s = (s0); s < RI_NB; s += k) {                                                     \
        k = (len) - (d) < RI_NB - s ? (len) - (d) : RI_NB - s;                              \
        body;                                                                               \
        d = (d) + k >= (len) ? 0u : (d) + k;                                                \
    }

static void ri_gains(tx_inf_t *s, int32_t d, int dark)      /* DECAY 0..119: T60 4 .. 60 s (4 x 15^(d / 119)) */
{
    uint32_t inv = exp2_neg((uint32_t)(131072 + d * 2151)) - RI_LOSS, k, q = dark ? 3776u : RI_GQ;
    for (k = 0; k < RI_NL; k++)                            /* (a pass's time: the half-rate length) */
        s->g[k] = (int16_t)(exp2_neg((((uint32_t)(s->len[k] / RV_X) * inv) >> 8) * q >> 14) >> 1);
}

static int32_t ri_input(tx_inf_t *s, const int32_t *in, uint32_t o)
{
    uint32_t i;
    int32_t pk = 0, gi = s->gi, xi = 0, il = s->il, ie = s->ie, ik = s->ik;
    for (i = 0; i < HL_NC; i++) pk |= in[i] ^ (in[i] >> 31);
    pk = pk > 65535 ? 65535 : pk;
    if (((pk * (gi >> 4)) >> (11 - RI_HOT)) > RI_XMAX) gi = (RI_XMAX << (15 - RI_HOT)) / pk;
    for (i = 0; i < HL_NC; i++) {
        int32_t x = clampi(in[i], -65535, 65535);
        x = (x - il) * ik + ie;
        ie = x & 16383;
        il += x >> 14;
        x = sh_tz(il * gi, 15 - RI_HOT);
        s->x[o + i] = x;
        xi |= x ^ (x >> 31);
    }
    s->il = il, s->ie = ie;
    return xi;
}

static int32_t ri_mix(const int32_t *y, const int32_t *z, const int32_t *xp, int16_t *o, int32_t *mo, int32_t *so,
                      int32_t *tep, int32_t *abp)
{
    const int32_t *e = y + RI_NB * (RI_NL / 2u);
    int32_t te = *tep, ab = 0, act = 0;
    for (; y < e; y += RI_NL / 2u, z += RI_NL / 2u, o += RI_NL) {   /* (y: lines 0..3, z: 4..7) */
        int32_t y0 = y[0], y1 = y[1], y2 = y[2], y3 = y[3], y4 = z[0], y5 = z[1], y6 = z[2], y7 = z[3], x = *xp++;
        int32_t A = y0 - y3 + y4 + y6, B = y1 + y2 - y5 + y7, u = A + B, S, t, p, q;
        S = u + 2 * (y3 + y5) + te, t = S >> 2;
        te = S & 3;
        p = x - t, q = -x - t;
        o[0] = (int16_t)hl_c16(y1 + p);
        o[1] = (int16_t)hl_c16(y2 + q);
        o[2] = (int16_t)hl_c16(y3 + p);
        o[3] = (int16_t)hl_c16(y4 + p);
        o[4] = (int16_t)hl_c16(y5 + q);
        o[5] = (int16_t)hl_c16(y6 + p);
        o[6] = (int16_t)hl_c16(y7 + q);
        o[7] = (int16_t)hl_c16(y0 + q);
        A -= B;
        *mo++ = u >> (RI_HOT + 1u);
        *so++ = A >> (RI_HOT + 1u);
        ab += u ^ (u >> 31);
        act |= u | A;
    }
    *tep = te;
    *abp += ab;
    return act;
}

/* the ends of lines 4 hf .. 4 hf + 3, a tank block, into hy[hf] (hs[hf] their state: the two halves may run on two
 * cores; every read is at least 835 samples old, older than the block) */
static void ri_half(tx_inf_t *s, uint32_t hf)
{
    uint32_t j, m, s0;
    for (j = 0; j < RI_NL / 2u; j++) {
        uint32_t k = hf * (RI_NL / 2u) + j, f = s->ef[k], d;
        if (f >= RI_NB) continue;
        d = s->ew + f + RI_N - s->mI[k] - 1u;
        d = d >= RI_N ? d - RI_N : d;
        RI_RUNS(d, RI_N, m, s0, f, ri_end(s->ring + d * RI_NL + k, s->hy[hf].y[s0] + j, m, s->ma[k], s->eb0[k],
                                          s->eck[k], &s->hs[hf].yp[j], &s->hs[hf].op[j], &s->hs[hf].oe[j],
                                          (s->mlo >> k) & 1u));
    }
}
static void ri_job(void *a) { ri_half((tx_inf_t *)a, 1u); }   /* (the other core's: lines 4..7) */

static void ri_tank(tx_inf_t *s, int32_t *mo, int32_t *so, const tx_rev_ctl_t *c, int32_t xi)
{
    uint32_t i, k, d, m, w = s->w;
    int32_t act = 0, ab = 0, closed = c->freeze == 2, dec, ct, ch = 0, mod = c->dark ? 0 : c->mod;
    int16_t *ap = s->ap;
    if (!c->freeze && c->decay >= 120) {                   /* a release: the tail fades out in ~2 s */
        s->rd = s->rd > RI_REL + 2 * (int32_t)RI_CPT ? s->rd - 2 * (int32_t)RI_CPT : RI_REL;
        dec = s->rd;
    } else {
        dec = c->decay > 119 ? 119 : c->decay;
        s->rd = 119;
    }
    if (s->gsz != c->size) {
        for (k = 0; k < RI_NL; k++) s->tlen[k] = (uint16_t)((RI_LEN[k] * (uint32_t)(127 + c->size)) / 254u);
        s->gsz = c->size;
    }
    for (k = 0; k < RI_NL; k++)
        if (s->len[k] != s->tlen[k]) {
            s->len[k] = !s->len[k] ? s->tlen[k] : s->len[k] < s->tlen[k] ? s->len[k] + 1u : s->len[k] - 1u;
            ch = 1;
        }
    if (ch || s->gdec != dec || s->gdk != c->dark) { ri_gains(s, dec, c->dark); s->gdec = dec; s->gdk = (uint8_t)c->dark; }
    ct = c->freeze ? 0 : c->damp * RI_KD;
    s->c = s->c < ct ? (ct - s->c > 64 ? s->c + 64 : ct) : (s->c - ct > 64 ? s->c - 64 : ct);
    s->ik = (int32_t)(exp2_neg((uint32_t)c->damp * RI_KI) >> 1);
    s->ik = s->ik < RI_KMAX ? s->ik : RI_KMAX;
    s->ik = rv_weight(s->ik * 2) >> 1;                     /* (Q14) */
    ct = c->freeze || c->damp <= 60 ? 0 : (c->damp - 60) * RI_KA;
    s->ca = s->ca < ct ? (ct - s->ca > 64 ? s->ca + 64 : ct) : (s->ca - ct > 64 ? s->ca - 64 : ct);
    ct = closed ? 0 : 32768;
    s->fin = s->fin < ct ? (ct - s->fin > RI_FIN_STEP ? s->fin + RI_FIN_STEP : ct)
                         : (s->fin - ct > RI_FIN_STEP ? s->fin - RI_FIN_STEP : ct);
    for (k = 0; k < RI_NL; k++) {                          /* the line ends: their reads, gains and poles */
        uint32_t I, f = 0;
        int32_t g = c->freeze ? 16384 + (s->dq >> 8) : s->g[k];
        int32_t ck = clampi((((16384 - g) * s->c * (c->dark ? RI_KDD : 16)) >> 13) + s->ca, 0, 14000) + RI_CMIN;
        {                                                  /* the read (every tank block) */
            int32_t dep = (int32_t)((RI_MODQ8 * (uint32_t)mod) >> 6), lf, u0, u1;
            uint32_t t, r;
            lf = tx_fx_sine(s->ph[k] += RI_CPT * RI_INC[k]) >> 1;   /* (+-16384: the swing) */
            if (c->drift) {                                /* DRIFT: the walk, over twice the swing */
                if (!s->wn[k]) {
                    uint32_t *q = c->drift;
                    *q = *q * 1664525u + 1013904223u;
                    s->wt[k] = (int32_t)(*q >> 16) - 32768;
                    s->wn[k] = (int32_t)(RI_WN0 + ((*q >> 4) & 511u) * 3u);
                }
                s->wn[k]--;
                s->wg |= (uint8_t)(1u << k);
            } else
                s->wt[k] = lf, s->wn[k] = 0;
            if ((s->wg >> k) & 1u) {                       /* the walk (or its glide back onto the sine): no jump */
                s->wu[k] += (s->wt[k] - s->wu[k]) >> RI_WK;
                s->wv[k] += (s->wu[k] - s->wv[k]) >> RI_WK;
                if (!c->drift && s->wv[k] - lf < 64 && lf - s->wv[k] < 64) s->wg &= (uint8_t)~(1u << k);
                else lf = s->wv[k];
            } else
                s->wu[k] = s->wv[k] = lf;                  /* (a DRIFT starts where the sine is) */
            t = (uint32_t)s->len[k] * 256u + (uint32_t)(dep + ((dep * lf) >> 14));
            I = (t - 128u) >> 8;
            r = t - I * 256u;
            s->ma[k] = ((256 - (int32_t)r) * 32768) / (256 + (int32_t)r);
            s->mI[k] = (uint16_t)I;
            d = w + RI_N - I - 1u;
            d = d >= RI_N ? d - RI_N : d;
            r = d + (I >> 1);
            u0 = s->full ? s->ring[d * RI_NL + k] : 0;
            u1 = s->full ? s->ring[(r >= RI_N ? r - RI_N : r) * RI_NL + k] : 0;
            if ((u0 < 0 ? -u0 : u0) + (u1 < 0 ? -u1 : u1) + (s->hs[k >> 2].op[k & 3u] < 0 ? -s->hs[k >> 2].op[k & 3u] : s->hs[k >> 2].op[k & 3u]) < RI_LOUD)
                s->mlo |= (uint8_t)(1u << k);
            else
                s->mlo &= (uint8_t)~(1u << k);
        }
        I = s->mI[k];
        if (!s->full) {
            int32_t v = (int32_t)(I + 1u) - (int32_t)s->age;
            f = v <= 0 ? 0u : v >= (int32_t)RI_NB ? RI_NB : (uint32_t)v;
            for (i = 0; i < f; i++) s->hy[k >> 2].y[i][k & 3u] = 0;
        }
        s->ef[k] = f;
        ck = rv_pole(ck * 2) >> 1;                         /* (Q14) */
        s->eck[k] = ck;
        s->eb0[k] = (g * (16384 - ck)) >> 14;
    }
    s->ew = w;
    {   /* the line ends: 4..7 on the other core (or here, after), 0..3 here, while this one runs the diffusers */
        int taken = rv_fork(c->par, ri_job, s);
        for (k = 0; k < RI_NDIF; k++) {
            uint32_t s0;
            d = s->apd[k];
            RI_RUNS(d, RI_AP[k], m, s0, 0, ri_dif2(ap + d, s->x + s0, m, !((s->apfull >> k) & 1u));
                    if (d + m >= RI_AP[k]) s->apfull |= 1u << k);
            s->apd[k] = (uint16_t)d;
            ap += RI_AP[k];
        }
        ri_half(s, 0u);
        rv_join(c->par, ri_job, s, taken);
    }
    {   /* the loop's DC block, on line 0 */
        int32_t hp = s->dh, he = s->de, *y = &s->hy[0].y[0][0];
        for (i = 0; i < RI_NB; i++, y += RI_NL / 2u) {
            int32_t v = *y - hp + he;
            he = v & ((1 << RI_DCK) - 1);
            hp += v >> RI_DCK;
            *y -= hp;
        }
        s->dh = hp, s->de = he;
    }
    act = ri_mix(&s->hy[0].y[0][0], &s->hy[1].y[0][0], s->x, s->ring + w * RI_NL, mo, so, &s->te, &ab);
    if (!w) { uint32_t i; for (i = 0; i < RI_NL; i++) s->ring[RI_N * RI_NL + i] = s->ring[i]; }   /* (not memcpy: bytes) */
    w += RI_NB;
    s->w = w >= RI_N ? 0u : w;
    if (!s->full && (s->age += RI_NB) > RI_N + RI_NB) s->full = 1;
    s->quiet = act ? 0u : s->quiet < 0x40000000u ? s->quiet + RI_NB : s->quiet;
    act |= xi;
    ab >>= RI_LNB;
    s->lv += (ab - s->lv) >> (9u - RI_LCPT);
    if (ab > RI_LV) s->ilim -= s->ilim >> 4;
    else if (s->ilim < 32768) s->ilim += ((32768 - s->ilim) >> 8) + 1;
    if (!c->freeze) s->tgt = s->dq = s->dqi = 0, s->fm = 0;   /* the held level (SUSTAIN / FREEZE) */
    else {
        if (s->fm != 1 + closed) { s->fm = (uint8_t)(1 + closed); s->tgt = closed ? 0 : s->lv > 0 ? s->lv : 1; }
        if (closed && !s->tgt) {
            if (s->fin) s->hw = 0;
            else if ((s->hw += HL_NC * RI_CPT) >= 132300u) s->tgt = s->lv > 0 ? s->lv : 1;
        } else {
            int32_t t = s->tgt, e, dd;
            if (!closed && xi > RI_XQ && s->lv > t) t = s->lv < RI_SUS ? s->lv : RI_SUS;
            t = t > 0 ? t : 1;
            e = clampi((s->lv - t) * 1024 / t, -1024, 1024);
            s->dqi = clampi(s->dqi - e * (int32_t)RI_CPT, closed ? -RI_DQMAX : -RI_DQCUT, RI_DQMAX);
            dd = s->dqi - e * (e < 0 ? RI_KP : 8 * RI_KP);
            s->dq = clampi(dd, closed ? -RI_DQMAX : -RI_DQCUT, RI_DQMAX);
            s->tgt = t;
        }
    }
    s->gi = (((s->ilim * s->fin) >> 15) * RI_IN) >> 15;
}

/* one control block: HL_NC samples in, HL_NC out (RI_CPT control blocks a tank block: the output 2.9 ms late) */
TX_AUDIO_CODE void tx_inf_run(tx_inf_t *s, const int32_t *in, int32_t *mid, int32_t *side, const tx_rev_ctl_t *c)
{
    uint32_t i, h = s->half, o = h * (RI_NB / RI_CPT);
    const int32_t *m = s->m + o, *q = s->s + o;
    s->xi |= ri_input(s, in, o);
    for (i = 0; i < HL_NC; i++) mid[i] = m[i], side[i] = q[i];
    if (++h < RI_CPT) s->half = (uint8_t)h;
    else s->half = 0, ri_tank(s, s->m, s->s, c, s->xi), s->xi = 0;
}
