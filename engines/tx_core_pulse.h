/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura PULSE core: short force / pressure pulses played from a shape table (contact, click, spark, N-wave).
 *
 * A pulse is a shape (TX_PULSE_N + 1 points, Q15) read at a rate set by its duration, with linear interpolation,
 * times an amplitude (Q15), added to the mix in Q23. 20 bytes per active pulse, ~6 ops per sample while it lasts.
 * A small bank (tx_pulses_t) plays several at once; a new pulse takes a free one, else the oldest. */
#ifndef TX_CORE_PULSE_H
#define TX_CORE_PULSE_H
#include <stdint.h>
#include "tx_core_math.h"

enum { TX_PULSE_SHAPE_HERTZ = 0, TX_PULSE_SHAPE_RCOS = 1, TX_PULSE_SHAPE_NWAVE = 2, TX_PULSE_SHAPE_CLICK = 3 };

typedef struct {
    uint32_t pos;                  /* Q16 index into the shape; >= TX_PULSE_N << 16: done */
    uint32_t step;                 /* Q16 index step per sample */
    int16_t amp;                   /* Q15, signed */
    uint16_t wait;                 /* samples to wait before it starts (its start offset; may pass the block's end:
                                    * it starts in a later block, at its own sample, never clamped onto the grid) */
    uint8_t shape, pad[3];         /* shape id */
    uint32_t age;
} tx_pulse_t;

#define TX_PULSES 8u
typedef struct {
    tx_pulse_t p[TX_PULSES];
    uint32_t clock;
} tx_pulses_t;

void tx_pulses_init(tx_pulses_t *b);
/* start a pulse of dur samples (>= 2) at amplitude amp_q15, offset samples into the next rendered block (up to 65535:
 * later blocks) */
void tx_pulse_start(tx_pulses_t *b, unsigned shape, uint32_t dur, int32_t amp_q15, unsigned offset);
TX_AUDIO_CODE void tx_pulses_add(tx_pulses_t *b, int32_t *out, unsigned n);
unsigned tx_pulses_active(const tx_pulses_t *b);

#endif
