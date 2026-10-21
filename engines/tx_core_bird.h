/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura BIRD core: a bird's voice and its songs, cheap enough for a chorus on the FM-1.
 *
 *   source   the syrinx's labia oscillate; how they start sets the timbre: born at a Hopf bifurcation the wave is a
 *            sine (whistles, hoots), born on an invariant circle (SNIC) it is pulse-like and rich (caws, the gull's
 *            break). A phase oscillator reads the wave from TX_SYRINX (8 shapes between the two, tx_tables: the
 *            theta model's cycle, the SNIC normal form; ours), its pitch and shape driven by the song's gestures,
 *            with roughness in the tension (a slow random walk on the pitch: the muscles' twitches, note 13).
 *   tract    the trachea, a tube with a delay and a reflection (p = s - r p(t - T), r 0.9 in the canary model; here
 *            0.7 for headroom), then the oro-oesophageal cavity, a resonance that tracks the pitch in songbirds (a
 *            near-pure tone) or sits fixed (the crow's formant, the owl's inflated throat), then the beak's
 *            radiation (+6 dB / oct; none with a closed beak: the owl).
 *   songs    gestures make syllables (a pitch path: start, end, an arch; a shape path, with a break; an amplitude
 *            envelope and a tremolo); a phrase repeats a syllable; a song is up to 6 phrases. Each bird has a few
 *            song types drawn once from its seed; every rendition varies (timing +-8 %, repetitions +-1, pitch
 *            +-2 %). The types (note 13 table, numbers from the references where measured, ours otherwise):
 *              0 songbird   whistles 1.8-4.5 kHz, sweeps and arches, a thrush's repeats or a blackbird's twitter
 *              1 chirper    a chaffinch's trills (falling steps, 8-20 /s) and its flourish, ~3.5 kHz
 *              2 crow       harsh caws (SNIC side, rough), f0 ~550 Hz, a fixed formant ~1.5 kHz, in series of 2-6
 *              3 owl        hoots ~0.9 kHz (tawny), pure, closed beak; a hoot, a pause, a tremolo
 *              4 gull       the long call: 6-14 "kyow" notes, each breaking from tonal to harsh, the pitch dropping
 * ~30 ops per sample while a voice sings. */
#ifndef TX_CORE_BIRD_H
#define TX_CORE_BIRD_H
#include <stdint.h>
#include "tx_core_math.h"

#define TX_BIRD_TYPES 5u
#define TX_BIRD_PHR 6u
#define TX_BIRD_DL 48u              /* trachea delay line (samples, the longest tube) */

typedef struct {                    /* a syllable: the gestures */
    uint16_t len;                   /* samples */
    int16_t df, arch;               /* the pitch path, Q12 octaves: end - start, an arch at the middle */
    int32_t f0_l2;                  /* start pitch, log2 Hz Q16 */
    uint8_t k0, k1, kb;             /* shape at the start and end (0 .. 224: 8 shapes Q5), the break position (Q8; 0 none) */
    uint8_t amp;                    /* level, Q8 */
    uint8_t am_hz, am_depth;        /* tremolo: rate (Hz) and depth (Q8) */
} tx_bird_syl_t;

typedef struct { tx_bird_syl_t s; uint8_t reps, pad; uint16_t gap; uint16_t after; } tx_bird_phrase_t;   /* gaps: samples / 4 */

typedef struct {
    tx_bird_phrase_t ph[TX_BIRD_PHR];
    uint8_t nph, pi, rep, singing;  /* the song: phrases, which, repetitions left, 1 while it sings */
    uint8_t type, closed, dlen, dp;
    uint32_t pos, wait;             /* the syllable's sample, or the gap's samples left */
    uint32_t phase;
    int32_t jw;                     /* the roughness walk, Q16 log2 */
    int32_t jsd;                    /* its depth */
    int32_t oec_l2, oec_q;          /* the cavity: fixed (log2 Hz Q16) or, tracking, an offset re f0 (track 1) */
    uint8_t track, sub, pad[2];
    int32_t a1, a2, b0;             /* the cavity's coefficients (Q14, Q14, Q15) */
    int32_t y1, y2, x1;             /* its state, the radiation's last input */
    int32_t gain;                   /* Q12: level x distance x the type's trim */
    int32_t amp;                    /* the amplitude now (Q15) */
    int32_t last;                   /* the last output (a declick when cut) */
    tx_rng_t rng;                   /* the rendition's own draws */
    int32_t dl[TX_BIRD_DL];
} tx_bird_voice_t;

extern const uint16_t TX_BIRD_PAUSE[TX_BIRD_TYPES];   /* mean pause between songs at the middle, 0.1 s */
extern const int32_t TX_BIRD_F0[TX_BIRD_TYPES];       /* the type's base pitch, log2 Hz Q16 */

/* start song `song` (0 .. 3) of a bird (its seed: the same song every time, varied per rendition), pitch offset
 * f_off (log2 Q16), gain (Q12) */
void tx_bird_song(tx_bird_voice_t *v, unsigned type, uint32_t bird_seed, unsigned song, uint32_t var_seed, int32_t f_off,
                  int32_t gain);
/* sing n samples into out (added); returns 1 while the song goes on */
TX_AUDIO_CODE int tx_bird_render(tx_bird_voice_t *v, int32_t *out, unsigned n);

#endif
