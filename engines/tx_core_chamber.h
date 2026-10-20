/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura CHAMBER: a small resonant space after any engine (or after the mix of the layers, stage S3): "stones in a
 * space". Two parts, both from the room's physics:
 *
 *   body   the enclosure's lowest axial modes: a box L x L/1.26 x L/1.59 has f = c / 2L x {1, 1.26, 1.59, 2}
 *          (c 343 m/s; 2: the second mode along L). Each a band-pass (shared SVF bank) with Q = pi f T60 / 6.91 (the
 *          room's decay), driven at unit noise power.
 *   tail   a short diffuse reverberation: an 8-line feedback delay network (Hadamard matrix, int16 lines), line lengths
 *          ~ 4.5 ms per metre of room (the mean free path scale), per-line gains for Sabine's
 *          T60 = 0.161 V / (S a) = 0.0268 L / a for a cube-like room of side L and mean absorption a (clamped 30 ms ..
 *          4 s), a one-pole low-pass in each line for the walls' and air's high-frequency loss (softer walls = duller),
 *          one input allpass for diffusion. Lines are int32 at Q20 (1.0 = 2^20, saturated at +-4.0: a tail sits
 *          60 dB under its source, so int16 lines rounded it away), products split into 32-bit halves.
 *
 * Mono, in place on a Q23 buffer (the dry signal is kept; body and tail are added, together at most 4 full scale). State ~22 KB (the lines: 8 x 640
 * int32): one instance per output, outside the engines' 16 KB arenas. After 2 T60 of silent input it clears and idles
 * (exact zero, nothing computed). */
#ifndef TX_CORE_CHAMBER_H
#define TX_CORE_CHAMBER_H
#include <stdint.h>
#include "tx_core_noise.h"

enum { TX_CH_SIZE, TX_CH_ABSORB, TX_CH_BODY, TX_CH_TAIL, TX_CH_NP };
extern const tx_param_t tx_chamber_params[TX_CH_NP];

#define TX_CH_LINES 8u
#define TX_CH_LMAX 640u            /* samples per line: 14.5 ms (a 3 m room) */
#define TX_CH_AP 97u               /* input allpass length */
#define TX_CH_MODES 4u

typedef struct {
    int32_t line[TX_CH_LINES][TX_CH_LMAX];
    int32_t ap[TX_CH_AP];
    uint16_t len[TX_CH_LINES], pos[TX_CH_LINES], appos;
    int32_t lp[TX_CH_LINES];       /* in-loop low-passes (Q20 signal) */
    int32_t g[TX_CH_LINES];        /* per-line decay x 1 / sqrt(8), Q15 */
    int32_t damp;                  /* low-pass coefficient, Q15 */
    tx_svf_t body[TX_CH_MODES];
    int32_t bf[TX_CH_MODES], bgin[TX_CH_MODES], bg[TX_CH_MODES], bq;
    int32_t body_mix, tail_mix;    /* Q15 */
    int32_t tmp[TX_BLOCK];
    uint32_t t60_ms, quiet_blocks;      /* quiet_blocks: silent input samples so far */
    uint8_t p[TX_CH_NP], active, pad[3];
} tx_chamber_t;

void tx_chamber_init(tx_chamber_t *c);
void tx_chamber_param(tx_chamber_t *c, unsigned id, unsigned v);
TX_AUDIO_CODE void tx_chamber_process(tx_chamber_t *c, int32_t *buf, unsigned n);   /* in place, Q23 */
static inline int tx_chamber_quiet(const tx_chamber_t *c) { return !c->active; }

#endif
