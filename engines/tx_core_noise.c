/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura NOISE core: filtered noise band with flicker (tx_core_noise.h). */
#include "tx_core_noise.h"

void tx_noise_init(tx_noise_t *z, uint32_t seed)
{
    tx_rng_seed(&z->rng, seed);
    z->low = z->band = 0;
    z->gain = z->target = 0;
    z->glide = 2;                                            /* ~4 blocks (23 ms) to settle */
    z->flick = 32768; z->flick_depth = 0;
    z->lp = 0; z->tilt = 0;
    tx_noise_band(z, 1000, 16, TX_NOISE_BAND);
}

void tx_noise_band(tx_noise_t *z, uint32_t fc_hz, uint32_t q_x16, uint8_t mode)
{
    uint32_t ph;
    if (fc_hz < 20u) fc_hz = 20u;
    if (fc_hz > TX_FS / 6u) fc_hz = TX_FS / 6u;              /* Chamberlin stays stable and close below fs/6 */
    if (q_x16 < 8u) q_x16 = 8u;
    if (q_x16 > 128u) q_x16 = 128u;
    ph = fc_hz * 97391u >> 1;                                /* (fc / fs) 2^32 / 2 = pi fc / fs as a phase: */
                                                             /* 2^32 / 44100 = 97391.5 */
    z->f = tx_sin_q15(ph);                                   /* sin in Q15 = 2 sin in Q14 */
    z->q = (int32_t)((16u << 14) / q_x16);
    {   /* Chamberlin stability: f^2 + 2 q f < 4, i.e. f < sqrt(q^2 + 4) - q (Q14); keep it at 85 % of that, for every
         * centre and Q (at Q 0.7 near fs/6 the filter was on the edge: Aeolian's wake reached +48 dBFS) */
        uint32_t qq = (uint32_t)z->q, x = qq * qq + (1u << 30), r = 0, bit = 1u << 30;   /* q^2 + 4 in Q28 */
        int32_t fmax;
        while (bit > x) bit >>= 2;
        while (bit) { if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
        fmax = (int32_t)((((int32_t)r - (int32_t)qq) * 27853) >> 15);
        if (z->f > fmax) z->f = fmax;
    }
    z->mode = mode;
}

TX_AUDIO_CODE void tx_noise_add(tx_noise_t *z, int32_t *out, unsigned n)
{
    int32_t low = z->low, band = z->band, f = z->f, q = z->q, g0, g1, gstep, g, x, y;
    unsigned j;
    if (!z->gain && !z->target) { z->low = z->band = 0; return; }
    if (z->flick_depth) {                                    /* flicker: a new random gain each block, smoothed */
        int32_t t = 32768 - (int32_t)(((tx_rand(&z->rng) >> 17) * (uint32_t)z->flick_depth) >> 15);
        z->flick += (t - z->flick) >> 1;
    } else
        z->flick = 32768;
    g0 = z->gain;
    g1 = z->gain + ((z->target - z->gain) >> z->glide);
    if (g1 == g0 && g1 != z->target) g1 = z->target;         /* the last step */
    if (z->target == 0 && g1 < 64) g1 = 0;                   /* end on exact zero */
    z->gain = g1;
    g0 = (g0 >> 1) * (z->flick >> 1) >> 13;                  /* Q15 x Q15 -> Q15 (32-bit) */
    g1 = (g1 >> 1) * (z->flick >> 1) >> 13;
    gstep = (int32_t)((g1 - g0) / (int32_t)(n ? n : 1u));    /* 32-bit division, once per block */
    g = g0;
    for (j = 0; j < n; j++) {
        x = ((int32_t)(tx_rand(&z->rng) >> 21) - 1024) * 2;   /* white, +-2^11 = +-1.0 in Q11 */
        low += (f * band) >> 14;
        y = x - low - ((q * band) >> 14);                     /* high */
        band += (f * y) >> 14;
        switch (z->mode) {
        case TX_NOISE_LOW: y = low; break;
        case TX_NOISE_HIGH: break;
        case TX_NOISE_WHITE: y = x; break;
        default: y = band; break;
        }
        out[j] += y * (g >> 3);                              /* Q11 x Q12 -> Q23 */
        g += gstep;
    }
    if (!z->gain) low = band = 0;
    z->low = low; z->band = band;
}

void tx_noise_tilt(tx_noise_t *z, uint32_t fc_hz)
{
    uint32_t e;
    if (!fc_hz) { z->tilt = 0; return; }
    if (fc_hz > 20000u) fc_hz = 20000u;
    e = fc_hz * 862u >> 6;                                   /* 2 pi fc / (fs ln 2), Q16 (13.47 fc) */
    z->tilt = 32768 - (int32_t)tx_exp2((15 << 16) - (int32_t)e);   /* 1 - exp(-2 pi fc / fs), Q15 */
    if (z->tilt < 1) z->tilt = 1;
}

TX_AUDIO_CODE void tx_noise_roar_add(tx_noise_t *z, int32_t g_q15, int32_t *out, unsigned n)
{
    int32_t low = z->low, band = z->band, lp = z->lp, f = z->f, q = z->q, a = z->tilt ? z->tilt : 32768, g, gstep, x, y;
    unsigned j;
    if (!g_q15 && !z->gain) { z->low = z->band = z->lp = 0; return; }
    if (g_q15 > 524288) g_q15 = 524288;
    if (g_q15 < 0) g_q15 = 0;
    g = z->gain;
    gstep = (g_q15 - g) / (int32_t)(n ? n : 1u);             /* 32-bit division, once per block */
    for (j = 0; j < n; j++) {
        x = (int32_t)(tx_rand(&z->rng) >> 18) - 8192;          /* white, +-2^13 */
        low += (f * band) >> 14;
        y = x - low - ((q * band) >> 14);
        band += (f * y) >> 14;
        lp += ((band - lp) * a) >> 15;
        out[j] += lp * (g >> 5);                             /* Q13 x Q10 -> Q23 */
        g += gstep;
    }
    z->gain = g_q15;
    if (!g_q15) low = band = lp = 0;
    z->low = low; z->band = band; z->lp = lp;
}

uint32_t tx_svf_gin_noise(uint32_t fc_hz, uint32_t q_x16)
{
    /* fs Q / (pi f) in Q16 = 44100 / pi x 65536 / 16 x q_x16 / f: 57.5e6 x q_x16 / f, then sqrt -> Q8 */
    uint32_t r, x, bit;
    if (fc_hz < 20u) fc_hz = 20u;
    x = (57496000u / fc_hz) * q_x16;                         /* <= 2.87e6 x 1600 overflows: clamp first */
    if (57496000u / fc_hz > 0xFFFFFFFFu / (q_x16 ? q_x16 : 1u)) x = 0xFFFFFFFFu;
    r = 0; bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) { if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return r > 65535u ? 65535u : r;                          /* sqrt of Q16 = Q8 */
}

/* (f * h) >> 14 for f <= 2^14 and |h| up to 2^21 in 32-bit products, exactly (floor): h = 16 hh + hl; a mode driven far
 * past its band's clamp can carry an h whose product with f overflows 32 bits */
TX_INLINE int32_t svf_mul14(int32_t f, int32_t h)
{
    int32_t hh, hl, a;
    h = h > 2097151 ? 2097151 : h < -2097151 ? -2097151 : h;
    hh = h >> 4; hl = h & 15;
    a = f * hh;
    return (a >> 10) + ((16 * (a & 1023) + f * hl) >> 14);
}

TX_INLINE void svf_bank_add_g(tx_svf_t *r, const int32_t *f, int32_t q, const int32_t *gin_q8,
                               const int32_t *g_q11, unsigned nr, const int32_t *in, int32_t *out, unsigned n)
{
    int32_t lo[TX_SVF_MAX], bd[TX_SVF_MAX], fi[TX_SVF_MAX], g[TX_SVF_MAX], gi[TX_SVF_MAX], lo0[TX_SVF_MAX], bd0[TX_SVF_MAX];
    int32_t ef[TX_SVF_MAX];
    unsigned i, j;
    int32_t silent = 0;
    if (nr > TX_SVF_MAX) nr = TX_SVF_MAX;
    for (j = 0; j < n; j++) silent |= in[j];
    silent = !silent;
    if (silent && tx_svf_bank_quiet(r, nr)) return;
    for (i = 0; i < nr; i++) {
        lo[i] = lo0[i] = r[i].low; bd[i] = bd0[i] = r[i].band; fi[i] = f[i]; g[i] = g_q11[i];
        gi[i] = gin_q8 ? gin_q8[i] : 256;
        ef[i] = r[i].ef;
    }
    if (q >= 512)                                            /* Q <= 32 */
        for (j = 0; j < n; j++) {                            /* the modes side by side: independent chains overlap */
            int32_t v = in[j] > (1 << 24) ? (1 << 24) : in[j] < -(1 << 24) ? -(1 << 24) : in[j];
            int32_t x = ((v >> 8) * q) >> 14, y = 0;         /* Q15, x 1 / Q (|x| <= 2^16 / 4 for Q >= 4) */
            for (i = 0; i < nr; i++) {
                int32_t h;
                lo[i] += (fi[i] * bd[i]) >> 14;
                h = ((x * gi[i]) >> 8) - lo[i] - ((q * bd[i]) >> 14);
                bd[i] += (fi[i] * h) >> 14;
                bd[i] = bd[i] > (1 << 17) ? (1 << 17) : bd[i] < -(1 << 17) ? -(1 << 17) : bd[i];
                y += (bd[i] * g[i]) >> 3;                    /* Q15 x Q11 >> 3 -> Q23 */
            }
            out[j] += y;
        }
    else                                                     /* high Q: the damping q band / 2^14 is under 1 LSB */
        for (j = 0; j < n; j++) {                            /* for |band| < 2^14 / q: error feedback keeps it exact */
            int32_t v = in[j] > (1 << 24) ? (1 << 24) : in[j] < -(1 << 24) ? -(1 << 24) : in[j];
            int32_t x = ((v >> 8) * q) >> 14, y = 0;
            for (i = 0; i < nr; i++) {
                int32_t h, d;
                lo[i] += (fi[i] * bd[i]) >> 14;
                ef[i] += q * bd[i];                          /* the damping with its carried remainder */
                d = ef[i] >> 14;
                ef[i] -= d * 16384;
                h = ((x * gi[i]) >> 8) - lo[i] - d;
                if (h > 131071 || h < -131071) bd[i] += svf_mul14(fi[i], h);   /* (past 2^17: fi * h split, exact) */
                else bd[i] += (fi[i] * h) >> 14;
                bd[i] = bd[i] > (1 << 17) ? (1 << 17) : bd[i] < -(1 << 17) ? -(1 << 17) : bd[i];
                y += (bd[i] * g[i]) >> 3;
            }
            out[j] += y;
        }
    for (i = 0; i < nr; i++) {
        if (silent && ((bd[i] > -16 && bd[i] < 16 && lo[i] > -16 && lo[i] < 16) || (bd[i] == bd0[i] && lo[i] == lo0[i])))
            lo[i] = bd[i] = 0;
        if (!lo[i] && !bd[i]) ef[i] = 0;
        r[i].low = lo[i]; r[i].band = bd[i]; r[i].ef = ef[i];
    }
}
TX_AUDIO_CODE void tx_svf_bank_add_g(tx_svf_t *r, const int32_t *f, int32_t q, const int32_t *gin_q8,
                                     const int32_t *g_q11, unsigned nr, const int32_t *in, int32_t *out, unsigned n)
{
    svf_bank_add_g(r, f, q, gin_q8, g_q11, nr, in, out, n);
}
/* svf_bank_add_g for 4 modes, each mode's state in its own variables (no arrays, no mode loop): the same steps in
 * the same order, sample for sample (the CHAMBER's body) */
#define B4_LO(k) lo##k += (f##k * bd##k) >> 14
#define B4_CL(k) bd##k = bd##k > (1 << 17) ? (1 << 17) : bd##k < -(1 << 17) ? -(1 << 17) : bd##k
#define B4_LOW(k) do {                                                                                    \
        int32_t h;                                                                                        \
        B4_LO(k);                                                                                         \
        h = ((x * gi##k) >> 8) - lo##k - ((q * bd##k) >> 14);                                             \
        bd##k += (f##k * h) >> 14;                                                                        \
        B4_CL(k);                                                                                         \
        y += (bd##k * g##k) >> 3;                                                                         \
    } while (0)
#define B4_HIGH(k) do {                                                                                   \
        int32_t h, d;                                                                                     \
        B4_LO(k);                                                                                         \
        ef##k += q * bd##k;                                                                               \
        d = ef##k >> 14;                                                                                  \
        ef##k -= d * 16384;                                                                               \
        h = ((x * gi##k) >> 8) - lo##k - d;                                                               \
        if (h > 131071 || h < -131071) bd##k += svf_mul14(f##k, h);                                       \
        else bd##k += (f##k * h) >> 14;                                                                   \
        B4_CL(k);                                                                                         \
        y += (bd##k * g##k) >> 3;                                                                         \
    } while (0)
#define B4_END(m, k) do {                                                                                 \
        if (silent && ((bd##k > -16 && bd##k < 16 && lo##k > -16 && lo##k < 16)                            \
                       || (bd##k == r[m].band && lo##k == r[m].low)))                                     \
            lo##k = bd##k = 0;                                                                            \
        if (!lo##k && !bd##k) ef##k = 0;                                                                  \
        r[m].low = lo##k; r[m].band = bd##k; r[m].ef = ef##k;                                             \
    } while (0)
TX_RAM static void svf_bank4(tx_svf_t *r, const int32_t *f, int32_t q, const int32_t *gin_q8, const int32_t *g_q11,
                             const int32_t *in, int32_t *out, unsigned n)
{
    int32_t silent = 0;
    unsigned j, k;
    for (j = 0; j < n; j++) silent |= in[j];
    silent = !silent;
    if (silent && tx_svf_bank_quiet(r, 4u)) return;
    for (k = 0; k < 4u; k += 2u) {                           /* two modes at a time over the block (their sum into
                                                              * out: the same total, the modes' order aside) */
        int32_t lo0 = r[k].low, bd0 = r[k].band, ef0 = r[k].ef, f0 = f[k], g0 = g_q11[k];
        int32_t lo1 = r[k + 1u].low, bd1 = r[k + 1u].band, ef1 = r[k + 1u].ef, f1 = f[k + 1u], g1 = g_q11[k + 1u];
        int32_t gi0 = gin_q8 ? gin_q8[k] : 256, gi1 = gin_q8 ? gin_q8[k + 1u] : 256;
        if (q >= 512)                                        /* Q <= 32 */
            for (j = 0; j < n; j++) {
                int32_t v = in[j] > (1 << 24) ? (1 << 24) : in[j] < -(1 << 24) ? -(1 << 24) : in[j];
                int32_t x = ((v >> 8) * q) >> 14, y = 0;
                B4_LOW(0); B4_LOW(1);
                out[j] += y;
            }
        else                                                 /* high Q: the damping's remainder carried */
            for (j = 0; j < n; j++) {
                int32_t v = in[j] > (1 << 24) ? (1 << 24) : in[j] < -(1 << 24) ? -(1 << 24) : in[j];
                int32_t x = ((v >> 8) * q) >> 14, y = 0;
                B4_HIGH(0); B4_HIGH(1);
                out[j] += y;
            }
        B4_END(k, 0); B4_END(k + 1u, 1);
    }
}
/* the same code in RAM on the FM-1, for the CHAMBER's body (ROOM's run path is all TX_RAM): the engines keep theirs;
 * 4 modes (the CHAMBER's) unrolled */
TX_RAM void tx_svf_bank_add_g_ram(tx_svf_t *r, const int32_t *f, int32_t q, const int32_t *gin_q8,
                                  const int32_t *g_q11, unsigned nr, const int32_t *in, int32_t *out, unsigned n)
{
    if (nr == 4u) svf_bank4(r, f, q, gin_q8, g_q11, in, out, n);
    else svf_bank_add_g(r, f, q, gin_q8, g_q11, nr, in, out, n);
}

TX_AUDIO_CODE void tx_svf_bank_add(tx_svf_t *r, const int32_t *f, int32_t q, const int32_t *g_q11, unsigned nr,
                                   const int32_t *in, int32_t *out, unsigned n)
{
    tx_svf_bank_add_g(r, f, q, 0, g_q11, nr, in, out, n);
}
