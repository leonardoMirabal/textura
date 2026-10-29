/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura LIMIT core: the output stage's peak limiter (after the mix and the CHAMBER; never inside an engine).
 *
 * gain = min(env, T / |x|) per sample: an instant attack, so the output never passes the threshold T (-1 dBFS); the
 * envelope env recovers toward 1 with a ~25 ms time constant (release), so a loud crack ducks the mix briefly instead
 * of clipping it. Under the threshold and with env at 1 it costs one compare per sample; a 32-bit division runs only
 * on samples over it. Integer: x Q23 in int32 (the mix has 8 bits of headroom), gain Q16.
 *
 * The peaks between the samples too: a crack's edge can peak ~1.5 .. 3 dB above its samples once rebuilt (a converter,
 * the USB audio's 48 kHz resampler), so -1 dBFS samples came out at 0 dBFS, clipped. The limiter looks 8 frames ahead
 * (0.18 ms of latency) and estimates the half-sample point on each side of the sample it sends (a 16-tap
 * interpolator, weighted +0.5 dB: the true peak may lie off the half point); the gain keeps the larger of the three
 * within T. The estimate runs only near a sample over 0.43 T (below that it cannot reach T: its taps' sum is 2.13),
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
    uint32_t mb[2];                /* each channel's half-sample point before the sample sent next (its |estimate|) */
    uint8_t hot[2];                /* frames left in which the window holds a sample over the estimate's gate */
    uint8_t big[2];                /* frames left in which it holds one over +24 dBFS (the estimate clamps its
                                    * pairs) */
    uint8_t k, ch;                 /* the ring's newest; the channels interleaved: 1 (tx_limit_init) or 2 (the mix's) */
} tx_limit_t;

void tx_limit_init(tx_limit_t *l);
/* in place over n samples of a Q23 mix (ch interleaved), 8 frames late: afterwards |x| <= T (7476717, -1 dBFS) and so
 * are the estimated half-sample points between */
TX_AUDIO_CODE void tx_limit_process(tx_limit_t *l, int32_t *x, unsigned n);
/* the deepest gain reduction since the last call, dB x 10 (0 = none), and reset */
uint32_t tx_limit_take(tx_limit_t *l);

#endif
