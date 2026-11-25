/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura VARY: no two presses alike, and a held texture that never loops (the user's sessions 3 and 5: "in reality
 * no two drops of water are the same").
 *
 * The engine's own block, before its OUT_TRIM, is split by one pole (corner fs / (2 pi 2^k) x ~1.1: k 1 = 4.9 kHz,
 * 2 = 2.0, 3 = 0.93, 4 = 0.45, 5 = 0.22 kHz; at the engine's own spectral centre; k 0: the two-tap average, which
 * tilts the whole band up to fs / 2, crossing at ~11 kHz: for sounds that live up high) into a low and a high part with gains
 * of their own. Each key press draws a new character: a level (lo .. hi dB, uniform) and a spectral tilt (+-tilt dB
 * between the parts, half on each, power-kept for a balanced sound: a wetter / drier surface, a nearer / farther
 * source, a harder / softer hit). While sounding, both
 * wander (a new drift target every 2 .. 6 s, +-drift dB (the level's on alternate sides), reached in ~1.5 s: the weather, the wind, the flow
 * changing). The press character moves in ~40 ms, the drift slowly. Gains are 32-bit split products (tx_trim1);
 * the targets are worked out once per block. Depths are in tenths of a dB (TX_DB10). The engine's per-event physics
 * (sizes, pitches, positions, counts) stays its own: this is the common layer on top of it. */
#ifndef TX_CORE_VARY_H
#define TX_CORE_VARY_H
#include <stdint.h>
#include "tx_core_math.h"
#include "tx_core_trim.h"

#define TX_DB10(x) ((int32_t)(x) * 1089)       /* tenths of a dB -> log2 Q16 (65536 / 60.206) */

typedef struct {
    tx_rng_t rng;
    int32_t lo, span, tilt, drift;              /* depths, log2 Q16 (level lo, level span, tilt +-, drift +-) */
    int32_t pl, pt;                             /* the press character: level, tilt (log2 Q16) */
    int32_t dl, dt, dl_t, dt_t;                 /* the drift now and its target */
    int32_t cl, ct;                             /* the press character as it moves (log2 Q16) */
    int32_t gl, gh;                             /* the gains reached at the last block's end, Q12 */
    int32_t lp;                                 /* the split's low part */
    uint32_t walk;                              /* blocks to the next drift target */
    uint8_t k;                                  /* the split's pole: 2^-k */
    uint8_t pos, dk_l[8], dk_t[8];              /* the decks of press levels and tilts (strata 0..7), the next card */
} tx_vary_t;

/* lo_db10 .. hi_db10: the press level range; tilt_db10: +- between the parts; drift_db10: +- slow wander (level and
 * tilt); k: the split's corner (1 .. 5, above) */
static inline void tx_vary_init(tx_vary_t *v, uint32_t seed, int32_t lo_db10, int32_t hi_db10, int32_t tilt_db10,
                                int32_t drift_db10, unsigned k)
{
    tx_zero(v, sizeof *v);
    tx_rng_seed(&v->rng, seed ^ 0x5A17C0DEu);
    v->lo = TX_DB10(lo_db10);
    v->span = TX_DB10(hi_db10 - lo_db10);
    v->tilt = TX_DB10(tilt_db10);
    v->drift = TX_DB10(drift_db10);
    v->gl = v->gh = 4096;
    v->k = (uint8_t)(k > 5u ? 5u : k);
}

/* uniform in [-a, a) for a log2 Q16 depth a (< 2^16 x 8) */
static inline int32_t tx_vary_pm(tx_rng_t *r, int32_t a) { return (int32_t)(((int64_t)tx_rand_s15(r) * a) >> 15); }

/* the next card of a shuffled deck of 8 (a new shuffle when it runs out): over any run of presses the draws cover the
 * range evenly, and the same stratum never comes twice in a row */
static inline unsigned tx_vary_card(tx_vary_t *v, uint8_t *deck, unsigned pos)
{
    if (!pos) {                                             /* shuffle (Fisher-Yates), the first card not the last */
        unsigned i, last = deck[7];
        for (i = 0; i < 8u; i++) deck[i] = (uint8_t)i;
        for (i = 7u; i > 0u; i--) {
            unsigned j = tx_rand_below(&v->rng, i + 1u);
            uint8_t t = deck[i]; deck[i] = deck[j]; deck[j] = t;
        }
        if (deck[0] == last) { uint8_t t = deck[0]; deck[0] = deck[4]; deck[4] = t; }
    }
    return deck[pos];
}

/* a key press: a new character; level and tilt each from its own deck of 8 strata, jittered within the stratum */
static inline void tx_vary_press(tx_vary_t *v)
{
    unsigned cl = tx_vary_card(v, v->dk_l, v->pos), ct = tx_vary_card(v, v->dk_t, v->pos);
    uint32_t u = (cl << 13) + (tx_rand(&v->rng) >> 19);    /* 0 .. 65535: (card + a uniform) / 8 */
    uint32_t w = (ct << 13) + (tx_rand(&v->rng) >> 19);
    v->pos = (uint8_t)((v->pos + 1u) & 7u);
    v->pl = v->lo + (int32_t)(((int64_t)u * v->span) >> 16);
    v->pt = (int32_t)(((int64_t)((int32_t)w - 32768) * v->tilt) >> 15);
}

/* the character now (press + drift), log2 Q16, for the engine's own physics to follow (a size, a pitch, a rate): the
 * level offset, and the tone (the tilt: + = brighter, i.e. smaller, harder, nearer); the press part at once */
static inline int32_t tx_vary_level(const tx_vary_t *v) { return v->pl + v->dl; }
static inline int32_t tx_vary_tone(const tx_vary_t *v) { return v->pt + v->dt; }
/* the tone's slow drift alone (for particles that live long: a held insect's carrier follows it) */
static inline int32_t tx_vary_drift(const tx_vary_t *v) { return v->dt; }
/* u x 2^(l2 / 65536), |l2| < 8 octaves (event rate: a 64-bit product) */
static inline uint32_t tx_vary_scale(uint32_t u, int32_t l2) { return (uint32_t)(((uint64_t)u * tx_exp2(l2 + (16 << 16))) >> 16); }

TX_AUDIO_CODE static inline void tx_vary_block(tx_vary_t *v, int32_t *x, unsigned n)
{
    int32_t gl, gh, dgl, dgh, l, tl, th, lp = v->lp, r = (1 << v->k) - 1;
    unsigned j, k = v->k;
    if (!v->walk) {                                         /* the next drift target, 2 .. 6 s away */
        int32_t a = v->drift - (int32_t)(((int64_t)(tx_rand(&v->rng) >> 16) * (v->drift - (v->drift >> 2))) >> 16);
        int32_t b = v->drift - (int32_t)(((int64_t)(tx_rand(&v->rng) >> 16) * (v->drift - (v->drift >> 2))) >> 16);
        v->dl_t = v->dl_t > 0 ? -a : a;                     /* each target on the other side, 0.25 .. 1 x drift: */
        v->dt_t = (tx_rand(&v->rng) & 1u) ? b : -b;         /* the level swings, the tone wanders */
        v->walk = 345u + tx_rand_below(&v->rng, 689u);
    }
    v->walk--;
    v->dl += (v->dl_t - v->dl) >> 8;                        /* ~1.5 s */
    v->dt += (v->dt_t - v->dt) >> 8;
    v->cl += (v->pl - v->cl) >> 3;                          /* ~45 ms */
    v->ct += (v->pt - v->ct) >> 3;
    {   /* the tilt t (log2) shared out +-t/2, less 0.5 log2(cosh-like (2^t + 2^-t) / 2): a balanced sound keeps its
         * power, so the tilt changes the colour and the press level the loudness (and the boost is ~half) */
        int32_t t = v->ct + v->dt, at = t < 0 ? -t : t, c;
        uint32_t x, y;
        if (at > (6 << 16)) at = 6 << 16;
        x = tx_exp2(at + (16 << 16));                       /* 2^|t|, Q16 (<= 2^22) */
        y = 0xFFFFFFFFu / x;                                /* 2^-|t|, Q16 */
        c = ((32 << 16) - (int32_t)(tx_neglog2_q12((x + y) >> 1) << 4) - (16 << 16)) >> 1;
        l = v->cl + v->dl + (12 << 16) - c;
        gl = (int32_t)tx_exp2(l - (t >> 1));
        gh = (int32_t)tx_exp2(l + (t >> 1));
    }
    if (gl > 65535) gl = 65535;
    if (gh > 65535) gh = 65535;
    if (!n) return;
    dgl = ((gl - v->gl) * 256) / (int32_t)n;                 /* a ramp over the block, ending on the target, in Q20 (a */
    dgh = ((gh - v->gh) * 256) / (int32_t)n;                 /* Q12 gain stepped by a unit on the block's grid: the slow
                                                              * drift's change is a few units a block) */
    tl = gl; th = gh;
    if (!lp) {                                              /* silence in, nothing held: the gains move on, the samples */
        for (j = 0; j < n && !x[j]; j++) {}                 /* stay exact zeros (the split of 0 is 0) */
        if (j == n) { v->gl = tl; v->gh = th; return; }
    }
    gl = v->gl << 8; gh = v->gh << 8;
    if (!k)                                                 /* the two-tap split: lp holds the last input */
        for (j = 0; j < n; j++) {
            int32_t a = x[j], m = (a + lp) >> 1;
            lp = a;
            gl += dgl; gh += dgh;
            x[j] = tx_trim1(m, gl >> 8) + tx_trim1(a - m, gh >> 8) + (((m >> 12) * (gl & 255) + ((a - m) >> 12) * (gh & 255)) >> 8);
        }
    else for (j = 0; j < n; j++) {
        int32_t h = x[j] - lp;
        lp += (h + (h > 0 ? r : 0)) >> k;                   /* one pole; rounds away from 0: reaches x, silence is 0 */
        gl += dgl; gh += dgh;
        x[j] = tx_trim1(lp, gl >> 8) + tx_trim1(x[j] - lp, gh >> 8) + (((lp >> 12) * (gl & 255) + ((x[j] - lp) >> 12) * (gh & 255)) >> 8);
    }
    v->lp = lp;
    v->gl = tl; v->gh = th;
}

#endif
