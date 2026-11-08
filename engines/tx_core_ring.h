/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura RING core: a switched-out engine rings out instead of being cut. When a lane (a player's layer) changes
 * its engine or its sound, the old instance is handed here, released by the caller (its keys up, its generator off)
 * or still playing: it renders on in its own arena at full level for TX_RING_HOLD blocks, so its tail (drops still falling,
 * embers still ticking) sounds as it would, then fades to silence over TX_RING_FADE blocks. It ends at once when the
 * engine is quiet (it adds exact zeros: nothing is lost). The caller may end the hold early (hold = 0: another
 * switch waits for the arena; or the engine was handed over still playing, its key down): the fade starts at once,
 * shortened but never cut, so a playing engine crossfades into the new one. If the lane switches again while one
 * still rings, the ringing one's last sample decays to zero instead (dc), so even that is click-free (the firmware's
 * lanes never do: a lane waits for its own ring).
 *
 * The same code in the firmware's lanes (firmware/src/lanes.c), the audition player (audition/tx_web.c) and the test
 * that holds them equal (tests/lanes_test.c). Integers only, 32-bit products. */
#ifndef TX_CORE_RING_H
#define TX_CORE_RING_H
#include <stdint.h>
#include "tx_engine.h"

#define TX_RING_HOLD 52u                /* blocks at full level: 302 ms at 256 frames, 44.1 kHz */
#define TX_RING_FADE 26u                /* blocks fading to 0: 151 ms */
#define TX_RING_N (TX_RING_HOLD + TX_RING_FADE)   /* the longest a ring holds its arena, blocks */

typedef struct {
    const tx_engine_t *e;               /* the ringing engine (0: none) */
    void *st;                           /* its state, in its arena */
    int32_t g, last, dc;                /* its level (Q15, 0: done); its last sample; a cut ring's decaying step */
    uint32_t hold;                      /* blocks still at full level */
} tx_ring_t;

/* e (in st) starts ringing out; one still ringing is given up (its last sample decays: dc) */
static inline void tx_ring_start(tx_ring_t *r, const tx_engine_t *e, void *st)
{
    if (r->g) r->dc += r->last;
    r->e = e; r->st = st; r->g = 32768; r->hold = TX_RING_HOLD; r->last = 0;
}
static inline int tx_ring_on(const tx_ring_t *r) { return r->g != 0 || r->dc != 0; }   /* anything to add */
static inline int tx_ring_holds(const tx_ring_t *r) { return r->g != 0; }              /* its arena still in use */

/* one block of the ring into x (written whole, zeros when none: n <= TX_BLOCK frames; the new engine then adds its
 * own over it, so both go through the lane's insert); *particles: its heroes sounding (the cost estimate), 0 when
 * quiet or done */
static inline void tx_ring_block(tx_ring_t *r, int32_t *x, unsigned n, uint32_t *particles)
{
    int32_t g = r->g, d = r->dc, last = 0, step = r->hold ? 0 : (int32_t)((32768u + TX_RING_FADE - 1u) / TX_RING_FADE);
    int quiet = 0;
    unsigned j;
    for (j = 0; j < n; j++) x[j] = 0;
    if (particles) *particles = 0;
    if (!g && !d) return;               /* (none: zeros) */
    if (g) {
        r->e->render(r->st, x, n);
        if (r->e->stats) {
            tx_stats_t s;
            r->e->stats(r->st, &s);
            quiet = s.quiet;
            if (particles && !quiet) *particles = s.particles;
        }
    }
    for (j = 0; j < n; j++) {
        int32_t gj = g - (int32_t)((uint32_t)step * j / TX_BLOCK);
        if (gj < 0) gj = 0;
        last = (x[j] >> 15) * gj + (((x[j] & 0x7FFF) * gj) >> 15);   /* = (x * g) >> 15, 32-bit */
        x[j] = last + d;
        d -= d >> 6;
        if (d > -64 && d < 64) d = 0;
    }
    if (r->hold) r->hold--;
    else g = g > step ? g - step : 0;
    if (quiet) g = 0;                   /* (exact zeros from here: done) */
    r->g = g; r->last = g ? last : 0; r->dc = d;
    if (!g) r->e = 0;
}
#endif
