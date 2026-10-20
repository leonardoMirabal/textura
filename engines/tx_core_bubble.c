/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura BUBBLE core: Minnaert bubbles with damping and a capped rise (tx_core_bubble.h). */
#include "tx_core_bubble.h"

#define L2_INC_1MM 1851457         /* log2(3280 Hz / fs x 2^32), Q16: f0 of a 1 mm bubble as a phase increment */
#define L2_D1 984506               /* log2(130 x 256): 0.13 / a at a = 1 mm, 1/s Q8 */
#define L2_D2 1037500              /* log2(227.7 x 256): 0.0072 a^-1.5 at a = 1 mm, 1/s Q8 */
#define INC_MAX 1932735283u        /* 0.45 fs */
#define SUB 32u

static int32_t clamp_a(int32_t a_l2)
{
    return a_l2 < TX_BUBBLE_A_MIN_L2 ? TX_BUBBLE_A_MIN_L2 : a_l2 > TX_BUBBLE_A_MAX_L2 ? TX_BUBBLE_A_MAX_L2 : a_l2;
}

uint32_t tx_bubble_inc(int32_t a_l2)
{
    uint32_t inc = tx_exp2(L2_INC_1MM - clamp_a(a_l2));     /* f0 = 3.28 / a */
    return inc > INC_MAX ? INC_MAX : inc;
}

uint32_t tx_bubble_damp_q8(int32_t a_l2)
{
    int32_t l = clamp_a(a_l2);
    return tx_exp2(L2_D1 - l) + tx_exp2(L2_D2 - l - l / 2);   /* 130 / a_mm + 227.7 a_mm^-1.5 */
}

void tx_bubble_start(tx_bubble_t *b, int32_t a_l2, uint32_t xi_q16, uint32_t env_q30, uint32_t wait)
{
    uint32_t inc = tx_bubble_inc(a_l2), d = tx_bubble_damp_q8(a_l2), cap;
    uint64_t r;
    /* per 32 samples: e^(-32 d / fs) = 2^(-d x 68.607), d Q8 -> x 17563 >> 16 */
    uint32_t x = (uint32_t)(((uint64_t)d * 17563u) >> 16);
    b->d32 = x >= (16u << 16) ? 0u : tx_exp2((16 << 16) - (int32_t)x);
    /* rise: inc0 xi d / fs per sample; xi d / fs in Q32 = xi_q16 d_q8 (2^32 / fs) >> 24 */
    r = ((uint64_t)xi_q16 * d * 97391u) >> 24;
    if (r > 0xFFFFFFFFu) r = 0xFFFFFFFFu;
    b->dinc = (uint32_t)(((uint64_t)inc * r) >> 32);
    cap = inc > INC_MAX / 2u ? INC_MAX : inc * 2u;          /* min(2 f0, 0.45 fs) */
    b->cap = cap;
    b->ph = 0;
    b->inc = inc;
    b->env = env_q30 > (1u << 30) ? (1u << 30) : env_q30;
    b->last = 0;
    b->wait = wait;
}

TX_AUDIO_CODE void tx_bubble_render(tx_bubble_t *b, int32_t *out, unsigned n)
{
    uint32_t ph = b->ph, inc = b->inc, dinc = b->dinc, env = b->env, d32 = b->d32;
    unsigned j;
    int32_t y = b->last;
    if (b->wait >= n) { b->wait -= n; return; }
    j = b->wait;
    b->wait = 0;
    while (j < n) {
        unsigned end = j + SUB < n ? j + SUB : n, len = end - j;
        uint32_t e1 = (env >> 16) * d32 + (((env & 0xFFFFu) * d32) >> 16);   /* env x d32 (Q16), 32-bit products */
        int32_t e = (int32_t)env, step = ((int32_t)e1 - e) / (int32_t)len;
        uint32_t di = dinc;
        if (di && inc + di * len >= b->cap) {                /* the rise reaches its cap in this sub-block: */
            di = (b->cap - inc) / len;                       /* land on it, then hold */
            dinc = 0;
        }
        for (; j < end; j++) {
            ph += inc;
            inc += di;
            y = (tx_sin_q15(ph) * (e >> 15)) >> 7;            /* Q15 x Q15 -> Q23 */
            out[j] += y;
            e += step;
        }
        env = e1;
    }
    b->ph = ph; b->inc = inc; b->dinc = dinc; b->env = env; b->last = y;
}

TX_AUDIO_CODE void tx_bubbles_render(tx_bubble_t *b, tx_pool_t *p, tx_declick_t *dc, int32_t *out, unsigned n)
{
    unsigned i;
    for (i = 0; i < TX_POOL_MAX; i++) {
        if (!(p->alive >> i & 1u)) continue;
        tx_bubble_render(&b[i], out, n);
        if (b[i].env < TX_BUBBLE_FLOOR) { tx_declick_add(dc, b[i].last); b[i].last = 0; tx_pool_free(p, i); }
        else p->level[i] = b[i].env;
    }
}

void tx_bubbles_rescale(tx_bubble_t *b, tx_pool_t *p, int32_t old_q15, int32_t new_q15)
{
    uint32_t q12 = old_q15 > 0 ? ((uint32_t)new_q15 << 12) / (uint32_t)old_q15 : 0;   /* ratio, Q12 */
    unsigned i;
    if (new_q15 == old_q15) return;
    for (i = 0; i < TX_POOL_MAX; i++)
        if (p->alive >> i & 1u) {
            uint64_t e = ((uint64_t)b[i].env * q12) >> 12;
            b[i].env = e > (1u << 30) ? (1u << 30) : (uint32_t)e;
        }
}
