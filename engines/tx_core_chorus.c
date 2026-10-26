/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura CHORUS core: inhibitory resetting between callers (tx_core_chorus.h). */
#include "tx_core_chorus.h"

int32_t tx_gauss_q12(tx_rng_t *r)
{
    uint32_t a = tx_rand(r), b = tx_rand(r);
    int32_t s = (int32_t)((a >> 16) + (a & 0xFFFFu) + (b >> 16) + (b & 0xFFFFu)) - 131072;
    return (s * 7094) >> 15;                                 /* the sum's SD is 37837: x 8192 / 37837 (SD 2.0 in Q12;
                                                              * every use is tuned to it, so it stays) */
}

static uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) { if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return r;
}

void tx_chorus_init(tx_chorus_t *ch)
{
    uint8_t *p = (uint8_t *)ch;
    unsigned i;
    for (i = 0; i < sizeof *ch; i++) p[i] = 0;
    tx_chorus_set(ch, 16384, 1966, 2646, 2205);              /* s 0.5, CV 0.06, t 60 ms, x 50 ms (N19) */
}

void tx_chorus_set(tx_chorus_t *ch, int32_t s_q15, int32_t cv_q15, uint32_t t_eff, uint32_t x_len)
{
    ch->s_q15 = s_q15 < 0 ? 0 : s_q15 > 32768 ? 32768 : s_q15;
    ch->cv_q15 = cv_q15 < 0 ? 0 : cv_q15 > 8192 ? 8192 : cv_q15;
    ch->t_eff = (int32_t)t_eff;
    ch->x_len = (int32_t)x_len;
}

void tx_chorus_place(tx_chorus_t *ch, unsigned i, int32_t x_mm, int32_t y_mm)
{
    unsigned j;
    if (i >= TX_CHORUS_MAX) return;
    ch->x[i] = x_mm; ch->y[i] = y_mm;
    for (j = 0; j < TX_CHORUS_MAX; j++) {                    /* to the singing ones (a later caller adds its own) */
        int32_t dx, dy;
        uint32_t d, s;
        if (!ch->c[j].alive) continue;
        dx = (ch->x[j] - x_mm) >> 6; dy = (ch->y[j] - y_mm) >> 6;   /* 64 mm units: +-2^15 = 2 km */
        dx = dx > 32767 ? 32767 : dx < -32767 ? -32767 : dx;
        dy = dy > 32767 ? 32767 : dy < -32767 ? -32767 : dy;
        d = isqrt32((uint32_t)(dx * dx) + (uint32_t)(dy * dy));
        s = d * 33712u >> 12;                                /* fs / 343 m/s = 0.1286 samples per mm: 8.23 per unit */
        ch->dly[i][j] = ch->dly[j][i] = (uint16_t)(s > 65535u ? 65535u : s);
    }
    ch->dly[i][i] = 0;
}

static int32_t draw_eps(tx_chorus_t *ch, tx_rng_t *rng, int32_t T)
{
    int32_t sd = (int32_t)(((int64_t)T * ch->cv_q15) >> 15);
    int32_t e = (int32_t)(((int64_t)tx_gauss_q12(rng) * sd) >> 12);
    return e < -(T >> 1) ? -(T >> 1) : e > (T >> 1) ? (T >> 1) : e;   /* within +-T / 2 */
}

void tx_chorus_start(tx_chorus_t *ch, unsigned i, int32_t T, uint32_t first)
{
    tx_caller_t *c;
    if (i >= TX_CHORUS_MAX) return;
    c = &ch->c[i];
    c->T = T < 64 ? 64 : T;
    c->eps = 0;
    c->next = ch->now + first;
    c->last = c->next - (uint32_t)c->T;
    c->alive = 1;
}

unsigned tx_chorus_block(tx_chorus_t *ch, tx_rng_t *rng, unsigned n, uint8_t *who, uint16_t *off, unsigned cap)
{
    unsigned k = 0, i, j;
    for (;;) {
        int32_t best = (int32_t)n;
        unsigned bi = TX_CHORUS_MAX;
        uint32_t t;
        for (i = 0; i < TX_CHORUS_MAX; i++) {                /* the earliest due call */
            int32_t d;
            if (!ch->c[i].alive) continue;
            d = (int32_t)(ch->c[i].next - ch->now);
            if (d < 0) { ch->c[i].next = ch->now; d = 0; }
            if (d < best) { best = d; bi = i; }
        }
        if (bi == TX_CHORUS_MAX || k >= cap) break;
        t = ch->c[bi].next;
        who[k] = (uint8_t)bi; off[k] = (uint16_t)best; k++;
        ch->c[bi].last = t;
        ch->c[bi].eps = draw_eps(ch, rng, ch->c[bi].T);
        ch->c[bi].next = t + (uint32_t)(ch->c[bi].T + ch->c[bi].eps);
        for (j = 0; j < TX_CHORUS_MAX; j++) {                /* the call reaches the others: reset (N19 Eq. 1) */
            tx_caller_t *c = &ch->c[j];
            uint32_t a;
            int32_t d, nx;
            if (j == bi || !c->alive || !ch->s_q15) continue;
            a = t + ch->dly[bi][j];
            d = (int32_t)(a - c->last);
            if (d < ch->x_len) continue;                     /* masked by its own call */
            if ((int32_t)(c->next - a) <= ch->t_eff) continue;   /* its next call is already committed */
            nx = (int32_t)(((int64_t)d * ch->s_q15) >> 15) + c->T + c->eps;
            if (nx < d + 1) nx = d + 1;
            c->next = c->last + (uint32_t)nx;
        }
    }
    ch->now += n;
    return k;
}
