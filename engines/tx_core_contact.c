/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura CONTACT core: Hunt-Crossley mallets (tx_core_contact.h). */
#include "tx_core_contact.h"
#include "tx_core_modal.h"

/* stone hammer: Hertz, E* ~30 GPa, R 10 mm (k = 4/3 E* sqrt R), 0.2 kg; wood: E* ~10 GPa, R 15 mm, 0.1 kg;
 * rubber: E ~10 MPa, R 15 mm, 0.1 kg; felt: I2's piano hammer (10 g, k 1.5e11, alpha 2.8, mu 0.6). mu for stone,
 * wood, rubber: ours (0.01, 0.05, 0.3). */
const tx_mallet_t TX_MALLET[TX_MALLETS] = {
    {2090425, 6144, -152170, -435412},
    {2003791, 6144, -217706, -283242},
    {1350673, 6144, -217706, -113834},
    {2433101, 11469, -435412, -48298},
};

void tx_mallet_mix(tx_mallet_t *o, int32_t pos)
{
    int32_t x = (pos < 0 ? 0 : pos > 32767 ? 32767 : pos) * 3, i = x >> 15, f = x & 32767;
    const tx_mallet_t *a = &TX_MALLET[i], *b = &TX_MALLET[i + 1 < TX_MALLETS ? i + 1 : i];
    o->lk = a->lk + (int32_t)(((int64_t)(b->lk - a->lk) * f) >> 15);
    o->alpha = a->alpha + (((b->alpha - a->alpha) * f) >> 15);
    o->lm = a->lm + (int32_t)(((int64_t)(b->lm - a->lm) * f) >> 15);
    o->lmu = a->lmu + (int32_t)(((int64_t)(b->lmu - a->lmu) * f) >> 15);
}

void tx_contact_hit(const tx_mallet_t *ml, int32_t lv, uint32_t *tc_q8, int32_t *e_q15)
{
    int32_t a1 = ml->alpha + 4096;                           /* alpha + 1, Q12 */
    int32_t num = tx_log2_q16((uint32_t)a1) - (13 << 16) + ml->lm + 2 * lv - ml->lk;   /* log2((a+1) m v^2 / 2k) */
    int32_t lx = (num * 256) / (a1 >> 4);                    /* / (alpha + 1): log2 x_max, Q16 */
    int32_t lt = 101962 + lx - lv;                           /* log2 t_c = log2 2.94 + log2 x_max - log2 v */
    int32_t muv = (int32_t)tx_exp2(ml->lmu + lv + (15 << 16));   /* mu v, Q15 */
    int32_t e = 32768 - muv * 2 / 3;
    *tc_q8 = tx_exp2(lt + 1535412);                          /* x fs x 256: log2(44100 x 256) = 23.43 */
    *e_q15 = e < 1638 ? 1638 : e > 32767 ? 32767 : e;
}

int32_t tx_contact_weight(uint32_t tc_q8, uint32_t f_hz)
{
    uint32_t u, u8;
    int32_t c, den;
    if (tc_q8 > 65535u) tc_q8 = 65535u;
    u = (f_hz * tc_q8) / 86u;                                /* 2 f t_c, Q16: f tc_q8 x 2 x 65536 / (256 fs) */
    if (u > (64u << 16)) return 0;
    c = tx_sin_q15((u << 14) + 0x40000000u);                 /* cos(pi u / 2) */
    if (c < 0) c = -c;
    u8 = u >> 8;
    den = 65536 - (int32_t)(u8 * u8);                        /* 1 - u^2, Q16 */
    if (den < 0) den = -den;
    if (den < 3277) return 25736;                            /* the limit at u = 1: pi / 4 */
    c = (int32_t)(((uint32_t)c << 15) / (uint32_t)(den >> 1));
    return c > 32767 ? 32767 : c;
}
