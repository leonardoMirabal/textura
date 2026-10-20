/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura CREAK core: jittered stick-slip trains (tx_core_creak.h). */
#include "tx_core_creak.h"

static int32_t gauss12(tx_rng_t *r)
{
    uint32_t a = tx_rand(r), b = tx_rand(r);
    int32_t s = (int32_t)((a >> 16) + (a & 0xFFFFu) + (b >> 16) + (b & 0xFFFFu)) - 131072;
    return (s * 7094) >> 15;
}

unsigned tx_creak_block(tx_creak_t *c, tx_rng_t *r, uint32_t f_hz, int32_t jit_q15, int32_t amp, unsigned n,
                        uint16_t *off, int16_t *imp, unsigned cap)
{
    uint32_t inc, ph = c->ph, j = 0;
    int32_t m;
    unsigned k = 0;
    c->e += (gauss12(r) * 3 - c->e) >> 3;                    /* ~20 Hz one-pole at the block rate (x 3: SD ~1 after) */
    m = 32768 + (int32_t)(((int64_t)c->e * jit_q15) >> 12);
    if (m < 8192) m = 8192;
    if (f_hz > 4000u) f_hz = 4000u;
    inc = (uint32_t)(((uint64_t)f_hz * 97391u * (uint32_t)m) >> 15);
    if (!inc) return 0;
    for (;;) {
        uint32_t left = 0xFFFFFFFFu - ph, steps = left / inc + 1u;   /* samples to the next wrap */
        if (j + steps > n) { ph += inc * (n - j); break; }
        j += steps;
        ph += inc * steps;
        if (k < cap) {
            int32_t a = amp + (int32_t)(((int64_t)tx_rand_s15(r) * amp * 39) >> 22);   /* +-30 % */
            off[k] = (uint16_t)(j - 1u); imp[k] = (int16_t)(a > 32767 ? 32767 : a < -32767 ? -32767 : a); k++;
        }
    }
    c->ph = ph;
    return k;
}
