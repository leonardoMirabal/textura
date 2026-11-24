/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura LIMIT core: the output peak limiter (tx_core_limit.h). */
#include "tx_core_limit.h"

#define T_Q23 6663308u             /* -2 dBFS (the FM-1's headphone jack clipped under -1 dBFS: Frodhes' LIM_T) */
#define G_Q23 (T_Q23 * 2u / 5u)    /* the estimate's gate: 0.4 T (< T / 2.13 / 1.125, the half taps' sum, the +1 dB;
                                    * the quarter taps' sum is 1.82) */
#define PCLAMP (1 << 27)           /* (a pair's half sum, +24 dBFS: the products below stay in 32 bits) */


void tx_limit_init(tx_limit_t *l) { tx_zero(l, sizeof *l); l->env = 65536u; l->min_env = 65536u; l->ch = 1; }

/* |the half-sample point| between q[7] and q[8] (16 taps, Kaiser-windowed sinc, Q11: DC gain 2048 / 2048), q the
 * window oldest first. With every input within +-PCLAMP a pair's half sum is too (HP); else each pair clamps (HC) */
#define HP(i, h) ((((q[7 - (i)] >> 1) + (q[8 + (i)] >> 1)) >> 9) * (h))
#define HC(i, h) ((PC((q[7 - (i)] >> 1) + (q[8 + (i)] >> 1)) >> 9) * (h))
#define PC(p) ((p) > PCLAMP ? PCLAMP : (p) < -PCLAMP ? -PCLAMP : (p))    /* (a macro: inline in its caller) */
static uint32_t half(const int32_t *q, unsigned big)
{
    int32_t acc = big ? HC(0, 1296) + HC(1, -406) + HC(2, 215) + HC(3, -126)
                        + HC(4, 74) + HC(5, -41) + HC(6, 20) + HC(7, -8)
                      : HP(0, 1296) + HP(1, -406) + HP(2, 215) + HP(3, -126)
                        + HP(4, 74) + HP(5, -41) + HP(6, 20) + HP(7, -8);
    acc >>= 1;                                               /* 2^18 x (sum |H| 2186 < 2^12) < 2^31; Q23: the point =
                                                              * sum H p / 1024, acc = sum H p / 512 */
    return (uint32_t)(acc < 0 ? -acc : acc);
}

/* the larger |quarter-sample point| between q[7] and q[8] (at 7.25 and 7.75; the same Kaiser-windowed sinc, Q11): the
 * two are mirror images, so their sum runs on the pairs' half sums (HP) and their difference on the pairs' half
 * differences (DP), 16 products for both. Without them a crack's peak a quarter sample off the half point passed T
 * by up to 0.9 dB once rebuilt (the plugin's 48 kHz: -1.41 dBFS) */
#define DP(i, h) ((((q[7 - (i)] >> 1) - (q[8 + (i)] >> 1)) >> 9) * (h))
#define DC(i, h) ((PC((q[7 - (i)] >> 1) - (q[8 + (i)] >> 1)) >> 9) * (h))
static uint32_t quarter(const int32_t *q, unsigned big)
{
    int32_t sm, df, a, b;
    if (big) {
        sm = HC(0, 2450) + HC(1, -592) + HC(2, 307) + HC(3, -180) + HC(4, 106) + HC(5, -60) + HC(6, 31) + HC(7, -14);
        df = DC(0, 1238) + DC(1, -112) + DC(2, 43) + DC(3, -23) + DC(4, 14) + DC(5, -9) + DC(6, 6) + DC(7, -3);
    } else {
        sm = HP(0, 2450) + HP(1, -592) + HP(2, 307) + HP(3, -180) + HP(4, 106) + HP(5, -60) + HP(6, 31) + HP(7, -14);
        df = DP(0, 1238) + DP(1, -112) + DP(2, 43) + DP(3, -23) + DP(4, 14) + DP(5, -9) + DP(6, 6) + DP(7, -3);
    }                                                        /* 2^18 x 3740 and x 1448: in 32 bits; the points =
                                                              * (sm +- df) / 4 (Q23) */
    sm >>= 1; df >>= 1;
    a = (sm + df) >> 1; b = (sm - df) >> 1;
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    return (uint32_t)(a > b ? a : b);
}


/* |x| above which a gain env under 1 passes T: T 2^16 / env, as T << 4 / ((env >> 12) + 1) (rounded down): env >> 12
 * takes 16 values, so a table (no division on the release) */
static const uint32_t THR[16] = {
    106612928u, 53306464u, 35537642u, 26653232u, 21322585u, 17768821u, 15230418u, 13326616u,
    11845880u, 10661292u, 9692084u, 8884410u, 8200994u, 7615209u, 7107528u, 6663308u};
#define THR_OF(e) ((e) >= 65536u ? T_Q23 : THR[(e) >> 12])

/* the estimate's gate for samples entering now: an estimate counts only over thr, and thr can only fall by the
 * release, at most 3 steps while a sample stays in the window (16 frames + the next sample's mb: 34 samples). So
 * below 3/8 of thr 3 steps on (and below 0.4 T) the estimate cannot reach thr: it is skipped, the output the same */
static uint32_t gate_of(uint32_t env)
{
    unsigned i;
    uint32_t t;
    for (i = 0; i < 3u && env < 65536u; i++) env += ((65536u - env) >> 6) + 1u;
    t = THR_OF(env);
    t = (t >> 2) + (t >> 3);
    return t > G_Q23 ? t : G_Q23;
}

TX_AUDIO_CODE void tx_limit_process(tx_limit_t *l, int32_t *x, unsigned n)
{
    uint32_t env = l->env, mn = l->min_env, thr = THR_OF(env), g = gate_of(env);
    uint32_t mb0 = l->mb[0], mb1 = l->mb[1];
    unsigned j, hot0 = l->hot[0], hot1 = l->hot[1], big0 = l->big[0], big1 = l->big[1], k = l->k, two = l->ch > 1u;
    for (j = 0; j < n; j++) {                                /* (a pass per sample; the channels alternate if two) */
        unsigned c = two & j;
        int32_t *w = l->w[c], *q, v = x[j], s;
        uint32_t a, ma = 0, *mb = c ? &mb1 : &mb0;
        unsigned *hot = c ? &hot1 : &hot0, *big = c ? &big1 : &big0;
        w[k] = v; w[k + 16u] = v;
        q = w + k + 1u;                                      /* the window, oldest first: q[15] the newest */
        a = (uint32_t)(v < 0 ? -v : v);
        if (a > g) { *hot = 16; if (a > (uint32_t)PCLAMP) *big = 16; }
        if (*hot) {                                          /* the points after the sample sent now (+1 dB: the
                                                              * true peak may lie between the quarter points) */
            (*hot)--;
            uint32_t m4 = quarter(q, *big);
            ma = half(q, *big);
            if (m4 > ma) ma = m4;
            ma += ma >> 3;
            if (*big) (*big)--;                              /* (big <= hot: it counts down with it) */
        }
        s = q[7];                                            /* the sample sent now: 8 frames back */
        a = (uint32_t)(s < 0 ? -s : s);
        if (*mb > a) a = *mb;
        if (ma > a) a = ma;
        *mb = ma;
        if (a > thr) {                                       /* over: the gain drops at once to T / |x| */
            env = (T_Q23 << 4) / ((a >> 12) + 1u);           /* Q16, rounded down: |y| <= T */
            thr = a;
            if (env < mn) mn = env;
        }
        if (env < 65536u) {
            x[j] = ((s >> 16) * (int32_t)env) + ((int32_t)((uint32_t)(s & 0xFFFF) * env >> 16));
            if ((j & 15u) == 15u) {                          /* release, every 16 samples */
                env += ((65536u - env) >> 6) + 1u;          /* tau ~ 64 x 16 / fs = 23 ms per e-fold */
                if (env > 65536u) env = 65536u;
                thr = THR_OF(env);
                g = gate_of(env);
            }
        } else x[j] = s;
        if (c == two) k = (k + 1u) & 15u;
    }
    l->env = env; l->min_env = mn; l->k = (uint8_t)k; l->mb[0] = mb0; l->mb[1] = mb1;
    l->hot[0] = (uint8_t)hot0; l->hot[1] = (uint8_t)hot1; l->big[0] = (uint8_t)big0; l->big[1] = (uint8_t)big1;
}

uint32_t tx_limit_take(tx_limit_t *l)
{
    uint32_t m = l->min_env;
    l->min_env = 65536u;
    if (m >= 65536u) return 0;                               /* -20 log10(m) x 10 = 60.206 x (-log2 m) */
    return (tx_neglog2_q12(m << 16) >> 2) * 60206u / 1024000u;
}
