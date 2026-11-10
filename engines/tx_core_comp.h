/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura COMP core: the master bus compressor, the first effect on the mix (tx_core_mix.c: the lanes' mix -> COMP ->
 * GRAIN DELAY -> REVERB -> the output limiter). Its controls are Frodhes' COMPRESSOR's (COMP, THR, ATK, REL, RAT,
 * MKUP: the same ranges), on this detector:
 *
 * Stereo-linked: one gain for both sides, from the louder side's power (max |L| |R|, squared), averaged over each
 * control block of 32 frames. Two power envelopes: a fast one (ATK 0.1 .. 50 ms, REL 20 ms .. 1.5 s) and a slow one
 * (attack 400 ms, release 1.5 s); the detector is the larger, so a lone crack recovers at REL while a dense, steady
 * sound holds its gain (the release follows the programme, no pumping). A soft knee (12 dB wide) around THR (0 ..
 * -40 dBFS); RAT 1.5:1 .. 20:1; MKUP 0 .. +24 dB. The gain glides to its target over ~6 ms and ramps across each
 * control block (no zipper). COMP off: the gain glides back to 1, then bypassed, bit-exact.
 * MKUP glides (1/8 of the way a control block, then exactly there; after Frodhes' 8354082b): a jump 0 -> 127 was a
 * click of 3.2x the held render's largest second difference, 1.3 now (make tx-pristine).
 *
 * Integers only, 32-bit products; the levels in "power log2" units (log2 of the power, Q12: 1 dB = 1360.6). */
#ifndef TX_CORE_COMP_H
#define TX_CORE_COMP_H
#include <stdint.h>
#include "tx_engine.h"

#define TX_CP_ON 0u
#define TX_CP_THR 1u
#define TX_CP_ATK 2u
#define TX_CP_REL 3u
#define TX_CP_RATIO 4u
#define TX_CP_MKUP 5u
#define TX_CP_NP 6u
extern const tx_param_t tx_comp_params[TX_CP_NP];       /* the app's knobs and their defaults */

typedef struct {
    int32_t ef, es;                /* the fast and slow power envelopes (Q26: full scale = 2^26) */
    int32_t g;                     /* the gain now, Q14 (16384 = 1) */
    int32_t s, t, mk;              /* the slope 1 - 1/ratio (Q12), the threshold and the make-up (power log2 Q12) */
    int32_t ka, kr;                /* the fast envelope's attack and release a control block (Q16) */
    int32_t gr;                    /* the gain reduction at the last control block (power log2 Q12, >= 0) */
    int32_t mg;                    /* the make-up the gain uses: glides to mk (power log2 Q12) */
    uint8_t p[TX_CP_NP];           /* the controls */
    uint8_t on;                    /* the detector running (0: its envelopes restart from the next block) */
} tx_comp_t;

void tx_comp_init(tx_comp_t *c);                         /* the defaults, but off (COMP 0: the app switches it on) */
void tx_comp_param(tx_comp_t *c, unsigned id, unsigned v);   /* TX_CP_*: main loop (a few divisions) */
/* in place over n frames of the Q23 pair (|x| above 32 full scale is clamped) */
TX_AUDIO_CODE void tx_comp_process(tx_comp_t *c, int32_t *L, int32_t *R, unsigned n);
/* the gain reduction now, dB x 10 (the make-up not counted) */
static inline int32_t tx_comp_gr_db10(const tx_comp_t *c) { return (c->gr * 10 + 680) / 1361; }

#endif
