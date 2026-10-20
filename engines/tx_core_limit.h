/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura LIMIT core: the output stage's peak limiter (after the mix and the CHAMBER; never inside an engine).
 *
 * gain = min(env, T / |x|) per sample: an instant attack, so the output never passes the threshold T (-1 dBFS); the
 * envelope env recovers toward 1 with a ~25 ms time constant (release), so a loud crack ducks the mix briefly instead
 * of clipping it. Under the threshold and with env at 1 it costs one compare per sample; a 32-bit division runs only
 * on samples over it. Integer: x Q23 in int32 (the mix has 8 bits of headroom), gain Q16. */
#ifndef TX_CORE_LIMIT_H
#define TX_CORE_LIMIT_H
#include <stdint.h>
#include "tx_core_math.h"

typedef struct {
    uint32_t env;                  /* the release envelope, Q16 (65536 = no reduction) */
    uint32_t min_env;              /* the deepest gain since the last tx_limit_take (Q16), for meters and tests */
} tx_limit_t;

void tx_limit_init(tx_limit_t *l);
/* in place over n samples of a Q23 mix: afterwards |x| <= T (7476717, -1 dBFS) */
TX_AUDIO_CODE void tx_limit_process(tx_limit_t *l, int32_t *x, unsigned n);
/* the deepest gain reduction since the last call, dB x 10 (0 = none), and reset */
uint32_t tx_limit_take(tx_limit_t *l);

#endif
