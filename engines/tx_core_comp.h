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
 * Clean on low notes and chords (after Frodhes' compressor, 2026-11-09/10): the fast envelope is held 16 control
 * blocks (11.6 ms, a cycle down to 86 Hz) after the last block that raised it, so it does not fall between a note's
 * waveform peaks; the reduction applied rises at once (ATK is the detector's) but falls through a one-pole of ~93 ms,
 * at most half REL's time constant (a fast REL stays fast). The release adapts to the playing: a released envelope
 * caught again by less than +3.5 dB (a chord's beats, a low note's cycle) halves its release speed, down to REL's / 8;
 * a new strike (+3.5 dB) gives REL back at once, else it drifts back over ~93 ms; with REL 59 and under (fast enough
 * to fall between beats) a rise under +3.5 dB is caught at ATK's speed / 16. A 41 Hz sine 10 dB over THR (4:1): H2..H5
 * -58.3 -> -87.8 dB re H1; held chords: the intermodulation re the tones at the defaults -39.4 -> -44.3 dB, the median
 * over the settings -30.1 -> -32.5 (make tx-pristine). The cost: the envelope sits nearer the tops, 0.6 .. 0.7 dB more
 * reduction on held chords and low notes.
 *
 * THR brings its own make-up and body (after Frodhes' compressor stage 2, 2026-11-10), above THR 64 (-20 dBFS, the
 * factory one: every factory scene renders as before). The auto make-up: the reduction per unit of slope over THR, times
 * the slope (fitted on the factory presets: 20 of them x THR 64 .. 127 x RAT 2:1 4:1 20:1 x ATK 40 100, BS.1770
 * loudness re COMP off at MKUP 0); MKUP a trim on top, the sum at most +24 dB. It gives back at most the reduction of
 * the moment, so the gain never passes MKUP's (no peak over COMP off), and follows THR down at the reduction's own
 * smoothing (a step of THR no dip). The body lift: under a steady reduction (followed up over ~186 ms, down at once)
 * 0.35 of it given back, faded in over THR 64 .. 88: a strike gets the full ratio, the body after it a gentler one.
 * The loudness lost re COMP off, the presets' mean over those settings: THR 80 / 96 / 112 / 127 1.25 / 2.55 / 4.44 /
 * 6.48 -> 0.45 / 0.58 / 0.66 / 0.69 dB (64: 0.44); the fatness (body - strike, on less off; THR -40 dB 20:1) -7.4 ->
 * +5.2 dB at ATK 100, +7.8 at ATK 40 (make tx-pristine).
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
    int32_t gs;                    /* the reduction applied: up at once, its release smoothed (power log2 Q12 x 16) */
    int32_t kq;                    /* the fast envelope's release in use (Q16; adapted to the playing, from kr) */
    int32_t amk, amg;              /* the auto make-up (power log2 Q12, from THR RAT MKUP), as the gain uses it (x 16) */
    int32_t bs, bq;                /* the steady reduction the body lift gives back from (power log2 Q12 x 16); the
                                    * lift as the gain uses it (Q8 x 16) */
    uint8_t hd, gsh, beta;         /* the fast envelope's hold left (control blocks); the smoothing's shift (from REL);
                                    * the body lift (Q8, from THR) */
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
