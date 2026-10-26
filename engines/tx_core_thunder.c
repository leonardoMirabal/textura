/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura THUNDER core: channel, segment boxes, recursive N-wave, air (tx_core_thunder.h). */
#include "tx_core_thunder.h"

#define STEP_CM 1600               /* 16 m (the channel model 8 m; the deflections do not depend on the step: 640 x 16 m reach the ground from 5 km) */
#define XMAX 8191                  /* the excitation's bound (13 bits: S2 <= (2L)^2 / 2 x 2^13 < 2^31) */
#define AREA_L2 10                 /* a segment's box area at 1 km, E_l 1e4 J/m: 2^10 (calibrated by render) */
#define TH_KCAP 40u                /* the far roll: x r / 3.6 km up to 2.5 (+8 dB, tau 3.75 s) */
#define TH_FSH 6                   /* the roll fades out (23 ms) 6 x 6 dB under its peak: a tap ends within 16 s */
#define OUT_G 512                  /* output multiplier (|y| <= L x XMAX = 2.6e6: x 512 < 2^31) */

static uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) { if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return r;
}
static uint32_t isqrt64(uint64_t x)                          /* shifts, adds, compares only (no libcall) */
{
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > x) bit >>= 2;
    while (bit) { if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return (uint32_t)r;
}
static int32_t gauss_q12(tx_rng_t *r)                        /* ~N(0, 1), Q12: 4 uniforms, x sqrt(3) */
{
    uint32_t a = tx_rand(r), b = tx_rand(r);
    int32_t s = (int32_t)((a >> 16) + (a & 0xFFFFu) + (b >> 16) + (b & 0xFFFFu)) - 131072;   /* var 4 / 12 x 2^32 */
    return (s * 7094) >> 15;                                 /* x sqrt(3) / 16: Q16 -> Q12 */
}
static int32_t log2_q16(uint32_t x) { return (32 << 16) - (int32_t)(tx_neglog2_q12(x) << 4); }

/* one channel step (the channel model): the step's orientation is the memory (Theta = the mean of the last 4 deflections
 * theta', Psi = a slow mean of the azimuths phi'), deflected by theta' ~ N(-Theta / 20, sd) at phi' ~ N(+-Psi / 20, 45 deg)
 * around it. The channel thus wiggles about the vertical instead of random-walking. */
static void grow(tx_flash_t *f, tx_rng_t *r)
{
    int32_t k, Th, cT, sT, cP, sP, e1x, e1y, e1z, e2x, e2y, bx, by, bz;
    int32_t dth, dph, cs, sn, cp, sp, nx, ny, nz, nn, mx, my, mz, vx, vy, vz;
    uint32_t rr, t, m, a;
    Th = (f->th[0] + f->th[1] + f->th[2] + f->th[3]) >> 2;   /* phase units >> 8 */
    /* angles are phases (2^32 = 2 pi): they wrap by design, so the products are unsigned */
    dth = (int32_t)((uint32_t)(int32_t)(((int64_t)gauss_q12(r) * f->sd_q16) >> 12) * 65536u - (uint32_t)(Th / 20) * 256u);
    dph = (int32_t)((uint32_t)gauss_q12(r) * 131072u + (uint32_t)(((tx_rand(r) & 1u) ? 1 : -1) * (f->psi / 20)) * 256u);
    for (k = 3; k > 0; k--) f->th[k] = f->th[k - 1];
    f->th[0] = dth >> 8;
    f->psi += ((dph >> 8) - f->psi) >> 6;                    /* the azimuth memory (~64 steps; the channel model k_phi 100) */
    /* the memory orientation (Theta, Psi) and its frame */
    cT = tx_sin_q15((uint32_t)(Th * 256) + 0x40000000u) >> 1; sT = tx_sin_q15((uint32_t)(Th * 256)) >> 1;   /* Q14 */
    cP = tx_sin_q15((uint32_t)(f->psi * 256) + 0x40000000u) >> 1; sP = tx_sin_q15((uint32_t)(f->psi * 256)) >> 1;
    bx = (sT * cP) >> 14; by = (sT * sP) >> 14; bz = -cT;  /* u(Theta, Psi), z up: the channel goes down */
    e1x = (cT * cP) >> 14; e1y = (cT * sP) >> 14; e1z = sT;
    e2x = -sP; e2y = cP;
    cs = tx_sin_q15((uint32_t)dth + 0x40000000u); sn = tx_sin_q15((uint32_t)dth);
    cp = tx_sin_q15((uint32_t)dph + 0x40000000u); sp = tx_sin_q15((uint32_t)dph);
    nx = (cs * bx + sn * ((cp * e1x + sp * e2x) >> 15)) >> 15;
    ny = (cs * by + sn * ((cp * e1y + sp * e2y) >> 15)) >> 15;
    nz = (cs * bz + sn * ((cp * e1z) >> 15)) >> 15;
    nn = (int32_t)isqrt32((uint32_t)(nx * nx + ny * ny + nz * nz));   /* renormalise to Q14 */
    if (nn < 1024) { nx = 0; ny = 0; nz = -16384; nn = 16384; }
    f->ux = nx * 16384 / nn; f->uy = ny * 16384 / nn; f->uz = nz * 16384 / nn;
    /* the segment: midpoint, distance, arrival, box width (end-on smear), area */
    mx = f->px + ((f->ux * STEP_CM) >> 15); my = f->py + ((f->uy * STEP_CM) >> 15); mz = f->pz + ((f->uz * STEP_CM) >> 15);
    f->px += (f->ux * STEP_CM) >> 14; f->py += (f->uy * STEP_CM) >> 14; f->pz += (f->uz * STEP_CM) >> 14;
    vx = mx - f->lx; vy = my; vz = mz - 150;
    rr = isqrt64((uint64_t)((int64_t)vx * vx + (int64_t)vy * vy + (int64_t)vz * vz));   /* cm */
    if (rr < 1000u) rr = 1000u;
    t = rr * 329u >> 8;                                      /* samples: fs / 343 m/s = 1.2857 per cm */
    {   /* |cos a| = |u . v| / r, in 32 bits (v in 128 cm units) */
        int32_t dot = (f->ux * (vx >> 7) + f->uy * (vy >> 7) + f->uz * (vz >> 7)) >> 7;   /* Q7 x 128 cm */
        uint32_t ad = (uint32_t)(dot < 0 ? -dot : dot), rq = (rr >> 7) ? (rr >> 7) : 1u;
        m = ((ad >> 2) * 2057u) / (rq * 32u);                /* 2057 samples x |cos a| (16 m at 343 m/s); 32-bit to 30 km */
    }
    /* a segment is no straight rod: its own kinks and width spread the arrival +-25 % (ours); and the long end-on
     * smears, capped at TX_TH_SMEAR, take a random width under the cap (0.6 .. 1): a run of equal boxes made a comb of
     * stationary lines at fs / SMEAR and its harmonics (~170 / 340 / 520 Hz; session 5's spectrogram review) */
    m = m + (uint32_t)(((int32_t)m * (tx_rand_s15(r) >> 2)) >> 15);
    m = m < 1u ? 1u : m > TX_TH_SMEAR ? (TX_TH_SMEAR * 3u) / 5u + tx_rand_below(r, (TX_TH_SMEAR * 2u) / 5u) : m;
    {   /* area: 1 / r to 3.6 km, 1 / r^2 beyond; scaled to the flash's distance (aref), its loss from there taken at
         * the output (oatt): a far flash's excitation was a few units, and its whole steps a broadband hiss */
        uint32_t rm = rr / 100u;
        a = (uint32_t)f->area0 * f->aref / (rm ? rm : 1u);
        if (rm > 3600u) a = a * f->knee / rm;
        a = a > 32767u ? 32767u : a;
    }
    if (f->nseg < TX_TH_SEGS) {
        f->seg_a[f->nseg] = (int16_t)a;
        f->seg_t[f->nseg] = (t << 8) | m;                    /* absolute for now */
        f->nseg++;
        if (t < f->tmin) f->tmin = t;
        if (t > f->tmax) f->tmax = t;
    }
    if (f->pz <= 150 || f->nseg >= TX_TH_SEGS) f->gen_done = 1;   /* the ground (or the buffer) */
}

void tx_flash_start(tx_flash_t *f, tx_rng_t *r, uint32_t dist_m, uint32_t height_m, uint32_t tort_q8, int32_t el_l2,
                    int32_t level_q15)
{
    uint8_t *b = (uint8_t *)f;
    unsigned i;
    int32_t lf, ls, l2;
    uint32_t T_us;
    for (i = 0; i < sizeof *f; i++) b[i] = 0;
    if (dist_m < 50u) dist_m = 50u;
    if (dist_m > 30000u) dist_m = 30000u;
    f->pz = (int32_t)(height_m * 100u);
    f->aref = dist_m > 1000u ? dist_m : 1000u;
    f->knee = dist_m > 3600u ? dist_m : 3600u;
    {   /* the loss from 1 km to aref: 1 / r, then 1 / r^2 past 3.6 km (Q15) */
        uint32_t at = 32768000u / f->aref;
        if (f->aref > 3600u) at = at * 3600u / f->aref;
        f->oatt = (int32_t)at;
    }
    f->uz = -16384;
    f->lx = (int32_t)(dist_m * 100u);
    f->tmin = 0xFFFFFFFFu;
    f->sd_q16 = (int32_t)((323100000u >> 8) * tort_q8 >> 16);   /* 27.08 deg = 323.1e6 phase units, >> 16 */
    for (i = 0; i < 4; i++) f->th[i] = 0;
    /* level: sqrt(E_l) (ours), the area of one segment at 1 km */
    f->area0 = (int32_t)(((uint64_t)tx_exp2((el_l2 >> 1) + (AREA_L2 << 16)) * (uint32_t)level_q15) >> 15);   /* x sqrt(E_l / 1e4) */
    /* N-wave: f_m = 683.5 Hz x (E_l / 1e4)^-1/2; T(1 km) = 0.33 / f_m; x sqrt((1 + 0.524 ln(r / 10)) / 3.413) */
    lf = 617690 - el_l2 / 2;                                 /* log2(683.5 Hz) = 9.417 */
    {
        int32_t lr = log2_q16(dist_m * 10u) - log2_q16(100u);    /* log2(r / 10 m), Q16 */
        uint32_t x = 65536u + (uint32_t)(((int64_t)(lr > 0 ? lr : 0) * 23808) >> 16);   /* 1 + 0.524 ln(r / 10) */
        ls = (log2_q16(x) - (16 << 16) - 116104) / 2;        /* log2 sqrt(x / 3.413): log2 3.413 = 1.7714 */
    }
    T_us = tx_exp2(1201413 - lf + ls);                       /* 0.33e6 / f_m us (log2 330000 = 18.332) x stretch */
    l2 = (int32_t)(T_us * 2u * 441u / 10000u);               /* 2L = 2 T fs */
    l2 = l2 < 8 ? 8 : l2 > (int32_t)TX_TH_LMAX ? (int32_t)TX_TH_LMAX : l2;
    f->l2 = (uint16_t)l2;
    f->recip = (int32_t)(65536u / (uint32_t)(l2 - 1));       /* 2 / (2L - 1), Q15 */
    /* air: f_c = 1000 Hz (600 m / r)^(2/3), 150 .. 12000 Hz */
    {
        int32_t lc = (2 * (log2_q16(600u) - log2_q16(dist_m))) / 3;
        uint32_t fc = tx_exp2(lc + 653125);                  /* 1000 x 2^lc: log2(1000) = 9.966 */
        fc = fc < 60u ? 60u : fc > 12000u ? 12000u : fc;
        f->lpa = 32768 - (int32_t)tx_exp2((15 << 16) - (int32_t)(fc * 862u >> 6));
    }
    /* the roll beyond the shadow (ours): past ~3.6 km the direct sound falls as 1 / r^2 (the air bends it up over
     * you) while the roll scattered by the air and the ground falls as 1 / r: relatively louder (x r / 3.6 km, +8 dB
     * at most), slower (tau 1.5 s x the same, 3.75 s at most) and through the same air (its noise under f_c, 220 Hz at
     * most). Nearer, the tail is as it was. */
    {
        uint32_t k = dist_m > 3600u ? (dist_m * 16u) / 3600u : 16u;   /* r / 3.6 km, Q4 */
        k = k > TH_KCAP ? TH_KCAP : k;
        f->tgain = (int32_t)(k * 16u);                       /* Q8: 256 .. 640 */
        f->trel = (int32_t)(4096u / k);                      /* 256 (tau 1.5 s) .. 102 (3.75 s) */
        f->ta = f->lpa < 1024 ? f->lpa : 1024;               /* 1024: 1 / 32, ~220 Hz */
    }
    f->out_g = OUT_G;
    f->st_t[0] = 0; f->st_g[0] = 32767; f->nst = 1;
    f->active = 1;
    (void)r;
}

void tx_flash_restrike(tx_flash_t *f, int32_t gain_q15)
{
    if (!f->active || f->nst >= TX_TH_STROKES) return;
    f->st_t[f->nst] = f->gen_done ? f->clock : 0;
    f->st_g[f->nst] = (int16_t)(gain_q15 > 32767 ? 32767 : gain_q15);
    f->nst++;
}

#define MUL15(a, b) ((((a) >> 15) * (b)) + ((((a) & 0x7FFF) * (b)) >> 15))

TX_AUDIO_CODE void tx_flash_add(tx_flash_t *f, tx_rng_t *r, int32_t *out, unsigned n)
{
    unsigned i, j, s;
    int32_t *d = f->d, x, s1, s2, lp1, lp2, any = 0;
    uint32_t c0;
    if (!f->active) return;
    if (!f->gen_done) {                                      /* grow the channel first */
        for (i = 0; i < TX_TH_GEN && !f->gen_done; i++) grow(f, r);
        if (f->gen_done) for (i = 0; i < f->nseg; i++) f->seg_t[i] -= (f->tmin << 8);   /* relative to the first */
        if (!f->gen_done) return;
        f->tmax -= f->tmin;
    }
    /* the boxes of every stroke arriving in this block: +h at the start, -h at the end (derivative) */
    c0 = f->clock;
    for (s = 0; s < f->nst; s++) {
        uint32_t st = f->st_t[s];
        int32_t g = f->st_g[s];
        if (c0 + n <= st || c0 > st + f->tmax) continue;
        for (i = 0; i < f->nseg; i++) {
            uint32_t ta = st + (f->seg_t[i] >> 8), m = f->seg_t[i] & 255u;
            if (ta >= c0 && ta < c0 + n) {
                int32_t h = ((f->seg_a[i] * g) >> 15) / (int32_t)m;
                d[ta - c0] += h;
                d[ta - c0 + m] -= h;
            }
        }
    }
    /* integrate, N-wave window, air, out */
    x = f->x; s1 = f->s1; s2 = f->s2; lp1 = f->lp1; lp2 = f->lp2;
    {   /* the rolling tail (session 5: "a thunder tail never cuts"): the arrivals of the channel's segments end at
         * once, real thunder rolls on, its sound scattered and reflected by the ground and the air: a low noise
         * (~200 Hz, two one-poles) following the output's envelope, which falls with tau 1.5 s (ours) */
        int32_t env = f->env, n1 = f->n1, n2 = f->n2;
        uint32_t nr = f->nr ? f->nr : 0x2545F491u;
        int32_t L2 = f->l2, rc = f->recip, a = f->lpa, g = f->out_g, trel = f->trel, tgn = f->tgain, ta = f->ta, oa = f->oatt, epk = f->epk;
        int16_t *dl = f->dl;
        unsigned p = f->dlpos;
        for (j = 0; j < n; j++) {
            int32_t xo, y, xc;
            x += d[j];
            xc = x > XMAX ? XMAX : x < -XMAX ? -XMAX : x;
            xo = dl[p];                                      /* x[n - 2L] */
            dl[p] = (int16_t)xc;
            p = p + 1u >= (unsigned)L2 ? 0u : p + 1u;
            s2 += s1 - L2 * xo;                              /* sum k x[n-k], k = 0 .. 2L-1 (uses the old S1) */
            s1 += xc - xo;
            y = s1 - MUL15(s2, rc);                          /* the odd ramp +1 .. -1 */
            lp1 += MUL15(y * 64 - lp1, a);                   /* (the air's states x 64: a far flash is quiet and its
                                                              * corner low, and whole steps were a broadband hiss) */
            lp2 += MUL15(lp1 - lp2, a);
            {
                int32_t yo = lp2 * (g >> 6), ay = (yo < 0 ? -yo : yo), u;
                ay = (ay > (1 << 26) ? (1 << 26) : ay) << 4;   /* (x 16: the fall's last steps are under -70 dBFS) */
                env = ay > env ? env + ((ay - env) >> 6) : env - (((env >> 12) * trel) >> 12) - (env > 0);
                if (env > epk) epk = env;
                else if (!ay && env > 0 && env < (epk >> TH_FSH)) env -= (env >> 10) + 1;   /* (36 dB under its peak: a quick
                                                              * fade, never a cut) */
                nr ^= nr << 13; nr ^= nr >> 17; nr ^= nr << 5;
                u = (int32_t)(nr >> 17) - 16384;
                n1 += MUL15(u * 64 - n1, ta);                /* (x 64, as the air's) */
                n2 += MUL15(n1 - n2, ta);
                out[j] += MUL15(yo + (((((n2 >> 6) * (env >> 16)) >> 1) >> 8) * tgn), oa);   /* (calibrated by render;
                                                              * x 32768: exact) */
            }
            any |= xc | s1 | s2;
        }
        f->dlpos = (uint16_t)p;
        if (!env) n1 = n2 = 0;
        f->env = env; f->epk = env ? epk : 0; f->n1 = n1; f->n2 = n2; f->nr = nr;
    }
    for (j = 0; j < TX_TH_SMEAR + 1u; j++) d[j] = d[n + j];  /* carry the boxes' far edges */
    for (; j < n + TX_TH_SMEAR + 1u; j++) d[j] = 0;
    f->x = x; f->s1 = s1; f->s2 = s2;
    f->clock = c0 + n;
    if (!any && ((lp1 > -1024 && lp1 < 1024 && lp2 > -1024 && lp2 < 1024) || (lp1 == f->lp1 && lp2 == f->lp2)))
        lp1 = lp2 = 0;                                       /* small, or frozen (the step rounds to zero) */
    f->lp1 = lp1; f->lp2 = lp2;
    if (f->clock > f->st_t[f->nst - 1] + f->tmax + TX_TH_SMEAR && !any && !lp1 && !lp2 && !x && !f->env) f->active = 0;
}
