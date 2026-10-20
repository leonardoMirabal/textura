/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura LIMIT core: the output peak limiter (tx_core_limit.h). */
#include "tx_core_limit.h"

#define T_Q23 7476717u             /* -1 dBFS */


void tx_limit_init(tx_limit_t *l) { l->env = 65536u; l->min_env = 65536u; }

/* |x| above which the current gain env passes T: T 2^16 / env (32-bit: T << 4 / (env >> 12), rounded down) */
static uint32_t thr_of(uint32_t env) { return env >= 65536u ? T_Q23 : (T_Q23 << 4) / ((env >> 12) + 1u); }   /* rounded down */

TX_AUDIO_CODE void tx_limit_process(tx_limit_t *l, int32_t *x, unsigned n)
{
    uint32_t env = l->env, mn = l->min_env, thr = thr_of(env);
    unsigned j;
    for (j = 0; j < n; j++) {
        int32_t v = x[j];
        uint32_t a = (uint32_t)(v < 0 ? -v : v);
        if (a > thr) {                                       /* over: the gain drops at once to T / |x| */
            env = (T_Q23 << 4) / ((a >> 12) + 1u);           /* Q16, rounded down: |y| <= T */
            thr = a;
            if (env < mn) mn = env;
        }
        if (env < 65536u) x[j] = ((v >> 16) * (int32_t)env) + ((int32_t)((uint32_t)(v & 0xFFFF) * env >> 16));
        if ((j & 15u) == 15u && env < 65536u) {              /* release, every 16 samples */
            env += ((65536u - env) >> 6) + 1u;              /* tau ~ 64 x 16 / fs = 23 ms per e-fold */
            if (env > 65536u) env = 65536u;
            thr = thr_of(env);
        }
    }
    l->env = env; l->min_env = mn;
}

uint32_t tx_limit_take(tx_limit_t *l)
{
    uint32_t m = l->min_env;
    l->min_env = 65536u;
    if (m >= 65536u) return 0;                               /* -20 log10(m) x 10 = 60.206 x (-log2 m) */
    return (tx_neglog2_q12(m << 16) >> 2) * 60206u / 1024000u;
}
