/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura CONTACT core: a mallet meeting a hard body, Hunt-Crossley.
 *
 *   f(x, x') = -k x^alpha (1 + mu x')              (I2 Eq. 1: Hertz alpha 1.5; felt hammers ~2.8)
 *
 * The body (a stone) is far stiffer and heavier than the mallet, so it is treated as immovable during the contact
 * (I2's Marhefka-Orin case). In the mu -> 0 limit the energy balance gives the largest compression
 * x_max = ((alpha + 1) m v^2 / (2 k))^(1 / (alpha + 1)) and the contact time t_c ~ 2.94 x_max / v (the Hertz
 * constant, used for every alpha: ours), i.e. t_c ~ (m / k)^(1/(alpha+1)) v^((1-alpha)/(1+alpha)) (
 * v^-0.2 for alpha 1.5, v^-0.47 for 2.8). Restitution from mu v: e ~ 1 - (2/3) mu v (Hunt and Crossley's small-mu v
 * result; I2 Eq. 7 solved exactly is not needed for a level). The force is shaped as a half-sine of length t_c,
 * whose spectrum |F(f)| / F(0) = |cos(pi f t_c) / (1 - (2 f t_c)^2)| weights each mode of the struck body (flat to
 * ~1 / (2 t_c): a stone hammer is an impulse up to ~3-6 kHz, felt rolls off above ~600 Hz).
 * Everything here runs per strike (control rate), in the log2 domain. */
#ifndef TX_CORE_CONTACT_H
#define TX_CORE_CONTACT_H
#include <stdint.h>
#include "tx_core_math.h"

typedef struct {
    int32_t lk;                    /* log2 k (N / m^alpha), Q16 */
    int32_t alpha;                 /* Q12 */
    int32_t lm;                    /* log2 mallet mass (kg), Q16 */
    int32_t lmu;                   /* log2 mu (s / m), Q16 */
} tx_mallet_t;

enum { TX_MALLET_STONE, TX_MALLET_WOOD, TX_MALLET_RUBBER, TX_MALLET_FELT, TX_MALLETS };
extern const tx_mallet_t TX_MALLET[TX_MALLETS];

/* a mallet between the table rows: pos Q15 over stone .. wood .. rubber .. felt (linear in the log parameters) */
void tx_mallet_mix(tx_mallet_t *out, int32_t pos_q15);
/* a hit at speed 2^(lv / 65536) m/s: the contact time (samples, Q8) and the restitution (Q15) */
void tx_contact_hit(const tx_mallet_t *ml, int32_t lv_q16, uint32_t *tc_q8, int32_t *e_q15);
/* the half-sine force spectrum at f, relative to DC (Q15) */
int32_t tx_contact_weight(uint32_t tc_q8, uint32_t f_hz);

#endif
