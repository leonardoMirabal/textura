/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura GEN: a lane's generator (SOURCE-DESIGN 4f). It plays its lane's source on its own: it sets the source's
 * activity (INTENSITY) along a trajectory that wanders around the lane's fader, and it emits pitched NOTES (a pitch
 * and a velocity: key presses, so pitch sets the source's size as a key does), how many from the activity and the
 * randomness. The landscape evolves over a long horizon and remembers where it has been.
 *
 *   evolution   activity = the fader + 20 knob units x a 1/f wander: up to 14 Ornstein-Uhlenbeck walks with time
 *               constants 1 s x 2^k up to EVOLVE (10 s .. 2 h), equal power per octave; walk k is stepped every 2^k
 *               blocks with the same per-step constant (172), so a 2 h walk keeps its precision and the cost is flat.
 *               The pitch centre wanders on a second bank (+-SPAN / 4); the randomness on a third walk (+-16).
 *   notes       a rate 0.05 .. 20 /s (log) from the activity; RANDOMNESS 0 .. 64: steady .. Poisson (the next note
 *               when the integrated rate reaches a threshold (1 - j) + j Exp(1)); above 64 a self-exciting
 *               (Hawkes) part: each note raises the rate for ~0.4 s, branching up to 0.8, the base rate lowered so the
 *               mean stays: flurries and silences. Pitch (chromatic, ROOT .. ROOT + SPAN): WANDER 0 = anywhere in the
 *               span .. 64 = a walk of 1-3 semitones .. 127 = close to the wandering centre. Velocity with the
 *               activity, +-20. Each note lasts ~0.8 of the mean gap (30 ms .. 1.5 s).
 *   memory      the activity once a second and the pitch centre every 10 s, as long as the horizon (2 h: 7.2 KB);
 *               phrases (notes closer than twice the mean gap, up to 8) into 16 motifs. At decision times (Poisson,
 *               mean horizon / 32) with probability RECALL the activity and centre glide (3 s) toward a recorded
 *               stretch (older ones likelier), held for horizon / 16 .. / 4, the walk keeping MUTATE of its own
 *               motion under it; a new phrase replays a stored motif with probability RECALL, each note moved
 *               +-1-2 semitones with probability MUTATE / 2.
 * Integers only; per block a few walks and a draw per note. ~8.5 KB per lane (the memory: it may live in slow RAM). */
#ifndef TX_CORE_GEN_H
#define TX_CORE_GEN_H
#include <stdint.h>
#include "tx_engine.h"
#include "tx_core_math.h"

#define TX_GEN_K 14u                /* walks: 1 s .. 2^13 s */
#define TX_GEN_HIST 7200u           /* activity, one a second: 2 h */
#define TX_GEN_PHIST 720u           /* pitch centre, one per 10 s */
#define TX_GEN_MOT 16u
#define TX_GEN_MLEN 8u
#define TX_GEN_OFFS 8u              /* notes sounding (pending releases) */
#define TX_GEN_EV_MAX 16u           /* events per block at most */

enum { TX_GEN_ACTIVITY, TX_GEN_ROOT, TX_GEN_SPAN, TX_GEN_WANDER, TX_GEN_RANDOM, TX_GEN_EVOLVE, TX_GEN_RECALL,
       TX_GEN_MUTATE, TX_GEN_NP };
extern const tx_param_t tx_gen_params[TX_GEN_NP];

typedef struct { uint16_t off; uint8_t note, vel; } tx_gen_ev_t;   /* vel 0 = a release */
typedef struct { int8_t d[TX_GEN_MLEN]; uint8_t len, pad; } tx_gen_motif_t;

typedef struct {
    tx_rng_t rng;
    uint8_t p[TX_GEN_NP];
    uint8_t nmot, mrec_len, play, pi;          /* motifs stored, the phrase being recorded, the motif playing (0xFF none) */
    int32_t wa[TX_GEN_K], wp[TX_GEN_K], wr;    /* walks: activity, pitch centre, randomness (Q15) */
    uint32_t blk;                              /* blocks since init */
    int32_t a;                                 /* activity now, Q8 knob units */
    int32_t pc;                                /* pitch centre, Q8 semitones above ROOT */
    int32_t last;                              /* last note, semitones above ROOT */
    uint32_t acc, thr;                         /* the rate integral and the next threshold, Q16 notes */
    int32_t exc;                               /* the self-excited rate, notes/s Q8 */
    uint32_t since;                            /* samples since the last note */
    uint32_t sec;                              /* samples into the current second */
    uint32_t secs;                             /* seconds since init */
    uint16_t hpos, hlen, ppos, plen;           /* history ring positions and fill */
    uint32_t dec;                              /* seconds to the next decision */
    int32_t g;                                 /* the recall's weight, Q15 */
    uint32_t r_at, r_len, r_el;                /* the recalled stretch: start (seconds back), length, elapsed; r_len 0 = none */
    int8_t mrec[TX_GEN_MLEN];                  /* the phrase being recorded (offsets from the centre) */
    tx_gen_motif_t mot[TX_GEN_MOT];
    struct { uint32_t wait; uint8_t note, pad[3]; } off[TX_GEN_OFFS];
    uint8_t noff, intensity, pad[2];
    uint32_t notes;                            /* notes since init */
    uint8_t hist[TX_GEN_HIST], phist[TX_GEN_PHIST];
} tx_gen_t;

void tx_gen_init(tx_gen_t *g, uint32_t seed);
void tx_gen_param(tx_gen_t *g, unsigned id, unsigned v);
/* one block of n samples: the events (note-ons and releases, offsets within the block) into ev (at most max), the
 * source's INTENSITY now into *intensity; returns the number of events */
unsigned tx_gen_block(tx_gen_t *g, unsigned n, tx_gen_ev_t *ev, unsigned max, uint8_t *intensity);
/* release every sounding note (a lane stopped): returns the releases */
unsigned tx_gen_stop(tx_gen_t *g, tx_gen_ev_t *ev, unsigned max);

#endif
