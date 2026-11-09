/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura MODAL core: decaying-sinusoid modes, phase-correct strikes (tx_core_modal.h). */
#include "tx_core_modal.h"
#include "tx_fx_tables.h"

/* sin(2 pi i / 512) x 32767 = TX_FX_SINE[2 i] (the effects' 1024-point sine, every other point: the same values) */
#define SIN512(i) TX_FX_SINE[2u * (i)]

static const uint32_t ATAN_PH[16] = {                        /* atan(2^-i) as a 32-bit phase */
    536870912u, 316933406u, 167458907u, 85004756u, 42667331u, 21354465u, 10679838u, 5340245u,
    2670163u, 1335087u, 667544u, 333772u, 166886u, 83443u, 41722u, 20861u,
};

int32_t tx_log2_q16(uint32_t x) { return (int32_t)(32u << 16) - (int32_t)(tx_neglog2_q12(x) << 4); }

uint32_t tx_mode_decay(uint32_t f_hz, uint32_t q_x16, uint32_t sigma0_q8)
{
    uint32_t l;
    if (q_x16 < 8u) q_x16 = 8u;
    if (f_hz > 22050u) f_hz = 22050u;
    l = f_hz * 1724u / q_x16 + ((sigma0_q8 * 275u) >> 11);   /* 16 (pi f / (Q fs) + sigma0 / fs) / ln 2, Q16 */
    if (l >= (30u << 16)) return 0;
    return tx_exp2((int32_t)(30u << 16) - (int32_t)l);
}

void tx_mode_strike(tx_mode_t *m, int32_t a)
{
    int32_t c = tx_sin_q15(m->ph + 0x40000000u), s = tx_sin_q15(m->ph), x, y, t;
    uint32_t ang = 0;
    unsigned i;
    x = (m->amp >> 1) + (((a >> 15) * c) >> 1);             /* the phasor A + a e^(-i phi), halved */
    y = -(((a >> 15) * s) >> 1);
    if (x < 0) { x = -x; y = -y; ang = 0x80000000u; }      /* CORDIC vectoring works in the right half-plane */
    for (i = 0; i < 16; i++) {
        if (y > 0) { t = x + (y >> i); y -= x >> i; x = t; ang += ATAN_PH[i]; }
        else { t = x - (y >> i); y += x >> i; x = t; ang -= ATAN_PH[i]; }
    }
    t = (int32_t)(((int64_t)x * 19898) >> 14);              /* x 1 / K (0.60725), x 2 (the halving) */
    m->amp = t > (1 << 30) ? (1 << 30) : t;
    m->ph += ang;
}

TX_AUDIO_CODE void tx_modal_run(tx_mode_t *m, unsigned nm, int32_t *out, unsigned j0, unsigned j1)
{
    unsigned i;
    for (i = 0; i < nm; i++) {
        uint32_t ph = m[i].ph, inc = m[i].inc, m16 = m[i].m16;
        int32_t a = m[i].amp;
        unsigned j = j0;
        if (!a) { m[i].ph = ph + inc * (j1 - j0); continue; }
        while (j < j1) {
            unsigned L = j1 - j < 16u ? j1 - j : 16u, k;
            int32_t a1 = (int32_t)(((uint64_t)(uint32_t)a * m16) >> 30), st, aa;
            if (L < 16u) a1 = a - (int32_t)((uint32_t)((a - a1) >> 4) * L);
            st = (a1 - a) / (int32_t)L;
            aa = a;
            for (k = 0; k < L; k++) {
                uint32_t ix = ph >> 23;
                int32_t sv = SIN512(ix) + (((SIN512(ix + 1) - SIN512(ix)) * (int32_t)((ph >> 15) & 0xFFu)) >> 8);
                out[j + k] += ((sv * (aa >> 15)) >> 7) + ((sv * ((aa >> 7) & 255)) >> 15);   /* Q15 x Q23 -> Q23: the
                                                              * amplitude's next 8 bits too. With Q15 alone a quiet ring
                                                              * stepped through a few dozen levels, a periodic staircase whose
                                                              * sidebands swept as it decayed: the "arcs" under the stone drum
                                                              * (session 5's spectrogram review) */
                ph += inc;
                aa += st;
            }
            a = a1;
            j += L;
            if (a < TX_MODAL_QUIET) { a = 0; ph += inc * (j1 - j); break; }
        }
        m[i].ph = ph; m[i].amp = a;
    }
}
