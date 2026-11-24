/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura LIMIT core: the output stage's peak limiter (after the mix and the CHAMBER; never inside an engine).
 *
 * gain = min(env, T / |x|) per sample: an instant attack, so the output never passes the threshold T (-2 dBFS); the
 * envelope env recovers toward 1 with a ~25 ms time constant (release), so a loud crack ducks the mix briefly instead
 * of clipping it. Under the threshold and with env at 1 it costs one compare per sample; a 32-bit division runs only
 * on samples over it. Integer: x Q23 in int32 (the mix has 8 bits of headroom), gain Q16. T was -1 dBFS; -2 since
 * 2026-11-11, as Frodhes' (its LIM_T): the FM-1's headphone jack clipped loud material just under -1 dBFS.
 *
 * The peaks between the samples too: a crack's edge can peak ~1.5 .. 3 dB above its samples once rebuilt (a converter,
 * the USB audio's 48 kHz resampler), so -1 dBFS samples came out at 0 dBFS, clipped. The limiter looks 8 frames ahead
 * (0.18 ms of latency) and estimates the quarter, half and three-quarter points on each side of the sample it sends
 * (16-tap interpolators, weighted +1 dB: the true peak may lie between them, and the gain's own step reshapes the
 * edge); the gain keeps the sample and the larger estimate on each side within T. Half points alone let a crack's
 * peak a quarter sample off pass T by 0.7 dB in the plugin's 48 kHz (-1.31 dBFS). The estimates run only near a
 * sample over 0.4 T (below that they cannot reach T: the largest taps' sum is 2.13, x 1.125),
 * and while the gain is down only near one over 3/8 of the threshold the release can reach within the window. */
#ifndef TX_CORE_LIMIT_H
#define TX_CORE_LIMIT_H
#include <stdint.h>
#include "tx_core_math.h"

typedef struct {
    uint32_t env;                  /* the release envelope, Q16 (65536 = no reduction) */
    uint32_t min_env;              /* the deepest gain since the last tx_limit_take (Q16), for meters and tests */
    int32_t w[2][32];              /* each channel's last 16 inputs (a ring at k, held twice: k and k + 16, so
                                    * the window k + 1 .. k + 16 reads without wrapping): the lookahead, the
                                    * interpolator's */
    uint32_t mb[2];                /* each channel's largest point before the sample sent next (its |estimate|) */
    uint8_t hot[2];                /* frames left in which the window holds a sample over the estimate's gate */
    uint8_t big[2];                /* frames left in which it holds one over +24 dBFS (the estimate clamps its
                                    * pairs) */
    uint8_t k, ch;                 /* the ring's newest; the channels interleaved: 1 (tx_limit_init) or 2 (the mix's) */
} tx_limit_t;

void tx_limit_init(tx_limit_t *l);
/* in place over n samples of a Q23 mix (ch interleaved), 8 frames late: afterwards |x| <= T (6663308, -2 dBFS) and so
 * are the estimated points between */
TX_AUDIO_CODE void tx_limit_process(tx_limit_t *l, int32_t *x, unsigned n);
/* the deepest gain reduction since the last call, dB x 10 (0 = none), and reset */
uint32_t tx_limit_take(tx_limit_t *l);

#endif
