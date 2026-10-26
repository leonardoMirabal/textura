/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura BIRD core: a bird's voice and its songs (15), cheap enough for a chorus on the FM-1.
 *
 *   source   the syrinx's labia oscillate; how they start sets the timbre: born at a Hopf bifurcation the wave is a
 *            sine (whistles, hoots), born on an invariant circle (SNIC) it is pulse-like and rich (caws, the gull's
 *            break). A phase oscillator reads the wave from TX_SYRINX (8 shapes between the two, tx_tables: the
 *            theta model's cycle; its harmonics fall as K^n), the shape capped by the pitch so no harmonic folds
 *            back over fs / 2 (the higher the note, the purer: as the cavity does in the bird), with roughness in
 *            the tension (a slow random walk on the pitch: the muscles' twitches). Songbirds have two sources (the
 *            left and right labia): a second, independent voice joins some notes (the thrushes' two-voice chords).
 *   tract    the trachea, a tube with a delay and a reflection (p = s - r p(t - T), r 0.7 here for headroom), then the
 *            oro-oesophageal cavity, a resonance that tracks the pitch in songbirds (a near-pure tone) or sits fixed
 *            (the crow's formant), then the beak's radiation (+6 dB / oct; none with a closed beak: the owl).
 *   songs    every bird is an individual: its seed gives its repertoire (syllable types and phrase types, never
 *            stored: re-derived from the seed when sung), its own pitch, tempo, tract and roughness. A song is a
 *            bout of phrases built on the fly in the species' style (a thrush repeats each phrase 2-4 times, a
 *            nightingale swells a whistle or rattles a trill, a robin never sings the same strophe twice...); every
 *            rendition and every syllable varies (pitch, length, level, glide), so no two phrases are alike.
 * ~30 ops per sample while a voice sings (+8 with the second voice). */
#ifndef TX_CORE_BIRD_H
#define TX_CORE_BIRD_H
#include <stdint.h>
#include "tx_core_math.h"

/* the types (presets pick one): 0 a woodland mix (each bird drawn from blackbird, song thrush, robin, wren, chaffinch),
 * 1 chirper (chaffinch, sparrow), 2 crow, 3 owl, 4 gull, and the melodic species 5 blackbird, 6 song thrush,
 * 7 nightingale, 8 robin, 9 wren, 10 hermit thrush, 11 skylark, 12 wood thrush, 13 reed warbler (a wetland), 14 curlew
 * (an estuary), 15 oystercatcher (a beach), 16 cuckoo (spring), 17 woodpecker (drumming on its tree), 18 wood pigeon,
 * 19 nightjar (churring, a heath at night), 20 bittern (booming, a reedbed) */
#define TX_BIRD_TYPES 21u
#define TX_BIRD_DL 48u              /* trachea delay line (samples, the longest tube) */
#define TX_BIRD_SEG 16u             /* segments in a phrase */

typedef struct {                    /* a syllable: the gestures */
    uint16_t len;                   /* samples */
    int16_t df, arch;               /* the pitch path, Q12 octaves: end - start, an arch at the middle */
    int16_t two_l2;                 /* the second voice: its pitch re the first, Q12 octaves */
    int32_t f0_l2;                  /* start pitch, log2 Hz Q16 */
    uint8_t k0, k1, kb;             /* shape at the start and end (0 .. 224: 8 shapes Q5), the break position (Q8; 0 none) */
    uint8_t amp;                    /* level, Q8 */
    uint8_t am_hz, am_depth;        /* tremolo: rate (Hz) and depth (Q8) */
    uint8_t two_amp;                /* the second voice's level (Q8; 0 none) */
    int8_t spot;                    /* a woodpecker's strike: where the bill lands, its ring's shift (1/256 oct) */
} tx_bird_syl_t;

typedef struct { uint16_t id; uint8_t n, gap, amp, cres; int8_t lev, pad; } tx_bird_seg_t;   /* n x syllable id, gap (2 ms),
                                    * level, swell (the first's share), pitch step (1/32 oct) */

typedef struct {
    tx_bird_syl_t s;                /* the syllable now */
    tx_bird_seg_t seg[TX_BIRD_SEG]; /* the phrase now */
    uint8_t nseg, si, sn, mrep;     /* its segments, which one, its repetition, the phrase's repetitions left */
    uint8_t nph, type, sp, closed;  /* phrases left in the song, the type, the species, a closed beak */
    uint8_t dlen, dp, track, sub;
    uint8_t singing, song, recent[2];
    uint8_t pace, verb, pamp, breath;   /* the bird's pace (gaps x pace / 128) and verbosity (+- notes), the phrase's level
                                     * (Q8), its breath noise (Q5; 0: none) */
    int8_t ctl[4];                  /* the player's TEMPO, LENGTH, VARIETY, EXCITE (tx_bird_ctl_t) */
    uint16_t pgap;                  /* the gap after this phrase (2 ms) */
    int16_t plev;                   /* the phrase's pitch level, Q12 octaves */
    uint32_t seed;                  /* the bird: its repertoire */
    int32_t f_off;                  /* the bird's pitch + this song's + the key's, log2 Q16 */
    int32_t tempo;                  /* Q12: the bird's x this song's */
    uint32_t pos, wait, phase, phase2;
    int32_t jw;                     /* the roughness walk, Q16 log2 */
    int32_t jsd;                    /* its depth */
    int32_t oec_l2, oec_q;          /* the cavity: fixed (log2 Hz Q16) or, tracking, an offset re f0 (track 1) */
    int32_t a1, a2, b0;             /* the cavity's coefficients (Q14, Q14, Q15) */
    int32_t y1, y2, x1;             /* its state, the radiation's last input */
    int32_t la1, la2, lb0, ly1, ly2;   /* a woodpecker's trunk: its low body, a second resonance under the ring (lb0 0:
                                     * none, every other bird) */
    int32_t gain;                   /* Q12: level x distance x the type's trim */
    int32_t last;                   /* the last output (a declick when cut) */
    int32_t bz, bh;                 /* the breath: its noise low-passed, and the slower low-pass taken off (a band) */
    uint32_t nz;                    /* its generator */
    tx_rng_t rng;                   /* the rendition's own draws */
    int32_t dl[TX_BIRD_DL];
} tx_bird_voice_t;

extern const uint16_t TX_BIRD_PAUSE[TX_BIRD_TYPES];   /* mean pause between bouts at the middle, 0.1 s */
extern const int32_t TX_BIRD_F0[TX_BIRD_TYPES];       /* the type's base pitch, log2 Hz Q16 */

/* how the player has the birds sing, each -64 .. 63 (0: the kind's own): TEMPO (faster up: +-0.5 oct of time; a
 * woodpecker's drum rate, a nightjar's churr rate), LENGTH (longer songs up: x/ 2 the phrases in a bout, +-2 notes in a
 * strophe; a woodpecker's roll, the bittern's booms, the cuckoo's calls in a series), VARIETY (a bigger repertoire up,
 * fewer repeats, more new phrases; the nightjar's churr switches, the cuckoo's interval, the waders' and gull's songs
 * over calls), EXCITE (an excited bird up: shorter pauses between phrases, quicker, louder phrases) */
typedef struct { int8_t tempo, length, variety, excite; } tx_bird_ctl_t;

/* start a song (a bout) of a bird: its seed is the individual (repertoire, voice), `song` a song type for the
 * species that keep a few (wren, chaffinch), var_seed this rendition's draws; pitch offset f_off (log2 Q16), of it
 * size_l2 the bird's size (its throat and trachea scale with it, tx_bird_size), ctl the player's (0: none), gain (Q12) */
void tx_bird_song(tx_bird_voice_t *v, unsigned type, uint32_t bird_seed, unsigned song, uint32_t var_seed, int32_t f_off,
                  int32_t size_l2, const tx_bird_ctl_t *ctl, int32_t gain);
/* SIZE (the knob's offset -64 .. 64) as the type's birds' pitch offset, log2 Q16: each kind its own range */
int32_t tx_bird_size(unsigned type, int32_t k);
/* the gain (Q12) that keeps a bird of that size as loud as its kind at SIZE 64 (its tract and beak scale with it) */
int32_t tx_bird_size_gain(unsigned type, int32_t size_l2);
/* ---- the flock: how a type's birds share time and space (the engine keeps the members and asks these; act is the
 * activity 0 .. 127 << 8). A few birds near and distinct over many farther, quieter ones; neighbours in one band take
 * turns (countersinging); crows answer crows, gulls set each other off (bursts and lulls); more birds as it rises. */
unsigned tx_bird_want(unsigned type, int32_t act);           /* birds present */
unsigned tx_bird_overlap(unsigned type, int32_t act);        /* how many may sing at once */
int32_t tx_bird_band(unsigned type, uint32_t seed, int32_t f_off);   /* a bird's pitch centre, log2 Hz Q16 */
int32_t tx_bird_dist(tx_rng_t *r, unsigned type, int32_t near_l2, int32_t depth_l2, int32_t act);   /* log2 m Q16 */
/* the bird close by (the key's): how much farther it is (log2 m Q16) at this activity: a lone bird is somewhere around,
 * the nearest come close only as the place fills (so the loudness grows with the birds) */
int32_t tx_bird_near(unsigned type, int32_t act);
/* 1: a bird (band, distance) waits while this other one sings (a neighbour in its band, near enough to matter) */
int tx_bird_yield(unsigned type, int32_t band, int32_t r_l2, int32_t band_o, int32_t r_o);
/* another bird has just started: an idle one's new wait (samples): it answers sooner, or keeps its own; nidle = the idle
 * birds that might (a call sets off n of them on average, n < 1: bursts that die out, never a runaway) */
uint32_t tx_bird_answer(tx_rng_t *r, unsigned type, int32_t act, uint32_t wait, unsigned nidle);

/* end the song after the phrase it is singing (the bird is leaving: the lane released or fewer birds wanted) */
void tx_bird_stop(tx_bird_voice_t *v);
/* sing n samples into out (added); returns 1 while the song goes on */
TX_AUDIO_CODE int tx_bird_render(tx_bird_voice_t *v, int32_t *out, unsigned n);

#endif
