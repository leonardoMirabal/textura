/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura LIMIT core: the output peak limiter (tx_core_limit.h). */
#include "tx_core_limit.h"

#define T_Q23 7476717u             /* -1 dBFS */
#define G_Q23 (T_Q23 * 3u / 7u)    /* the estimate's gate: 0.43 T (< T / 2.13 / 1.0625, the taps' sum, the +0.5 dB) */
#define PCLAMP (1 << 27)           /* (a pair's half sum, +24 dBFS: the products below stay in 32 bits) */


void tx_limit_init(tx_limit_t *l) { tx_zero(l, sizeof *l); l->env = 65536u; l->min_env = 65536u; l->ch = 1; }

/* |the half-sample point| between w[k - 8] and w[k - 7] (16 taps, Kaiser-windowed sinc, Q11: DC gain 2048 / 2048) */
static uint32_t half(const int32_t *w, unsigned k)
{
    static const int16_t H[8] = {1296, -406, 215, -126, 74, -41, 20, -8};
    int32_t acc = 0, p;
    unsigned i;
    for (i = 0; i < 8u; i++) {
        p = (w[(k - 8u - i) & 15u] >> 1) + (w[(k - 7u + i) & 15u] >> 1);
        p = p > PCLAMP ? PCLAMP : p < -PCLAMP ? -PCLAMP : p;
        acc += (p >> 9) * H[i];                              /* 2^18 x (sum |H| 2186 < 2^12) < 2^31 */
    }
    acc >>= 1;                                               /* Q23: the point = sum H p / 1024, acc = sum H p / 512 */
    return (uint32_t)(acc < 0 ? -acc : acc);
}


/* |x| above which the current gain env passes T: T 2^16 / env (32-bit: T << 4 / (env >> 12), rounded down) */
static uint32_t thr_of(uint32_t env) { return env >= 65536u ? T_Q23 : (T_Q23 << 4) / ((env >> 12) + 1u); }   /* rounded down */

TX_AUDIO_CODE void tx_limit_process(tx_limit_t *l, int32_t *x, unsigned n)
{
    uint32_t env = l->env, mn = l->min_env, thr = thr_of(env);
    unsigned j, c = 0, cm = l->ch > 1u ? 1u : 0u, k = l->k;
    for (j = 0; j < n; j++) {
        int32_t *w = l->w[c], v = x[j], s;
        uint32_t a, ma = 0;
        w[k] = v;
        if ((uint32_t)(v < 0 ? -v : v) > G_Q23) l->hot[c] = 16;
        if (l->hot[c]) { l->hot[c]--; ma = half(w, k); ma += ma >> 4; }   /* the point after the sample sent now (+0.5 dB:
                                                                * the true peak may lie off the half point) */
        s = w[(k - 8u) & 15u];                               /* the sample sent now: 8 frames back */
        a = (uint32_t)(s < 0 ? -s : s);
        if (l->mb[c] > a) a = l->mb[c];
        if (ma > a) a = ma;
        l->mb[c] = ma;
        if (a > thr) {                                       /* over: the gain drops at once to T / |x| */
            env = (T_Q23 << 4) / ((a >> 12) + 1u);           /* Q16, rounded down: |y| <= T */
            thr = a;
            if (env < mn) mn = env;
        }
        x[j] = env < 65536u ? ((s >> 16) * (int32_t)env) + ((int32_t)((uint32_t)(s & 0xFFFF) * env >> 16)) : s;
        if ((j & 15u) == 15u && env < 65536u) {              /* release, every 16 samples */
            env += ((65536u - env) >> 6) + 1u;              /* tau ~ 64 x 16 / fs = 23 ms per e-fold */
            if (env > 65536u) env = 65536u;
            thr = thr_of(env);
        }
        if (c == cm) { c = 0; k = (k + 1u) & 15u; } else c++;
    }
    l->env = env; l->min_env = mn; l->k = (uint8_t)k;
}

uint32_t tx_limit_take(tx_limit_t *l)
{
    uint32_t m = l->min_env;
    l->min_env = 65536u;
    if (m >= 65536u) return 0;                               /* -20 log10(m) x 10 = 60.206 x (-log2 m) */
    return (tx_neglog2_q12(m << 16) >> 2) * 60206u / 1024000u;
}
