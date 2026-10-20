/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura scene scripts: timed key / parameter events for an engine, run block by block (txrender, txtest,
 * txbench and the browser player's reference render all use this).
 *
 * One event per line, times in ms (in order), '#' starts a comment:
 *   T on NOTE VEL        key down (NOTE: MIDI number or a name: C4 = 60, F#3, Bb4; VEL 1..127)
 *   T off NOTE           key up
 *   T param NAME V       the engine's own parameter, by name (or index)
 *   T particles V        the shared PARTICLES limit
 *   T level V            the shared LEVEL (100 = unity)
 *   T end                the render stops here (default: 1 s after the last event)
 *   seed N               the engine's PRNG seed (default 1)
 *   engine NAME          the engine (a command-line choice overrides it)
 * Events apply at the start of the 256-frame block that contains their time (5.8 ms resolution, as the device's
 * block-rate event ring). */
#ifndef TX_SCRIPT_H
#define TX_SCRIPT_H
#include <stdint.h>
#include "tx_engine.h"

enum { TX_SEV_ON, TX_SEV_OFF, TX_SEV_PARAM };

typedef struct { uint32_t block; uint8_t kind, id, a, b; } tx_sev_t;   /* PARAM: id = param id, a = value */

typedef struct {
    tx_sev_t ev[1024];
    unsigned n;
    uint32_t blocks;               /* blocks to render */
    uint32_t seed;
    char engine[32];
    char err[160];
} tx_script_t;

/* the engine named by an "engine" line, or "" */
void tx_script_engine(const char *text, char *name, unsigned cap);
/* parse text for engine e: 0 ok, -1 error (s->err says where) */
int tx_script_parse(tx_script_t *s, const tx_engine_t *e, const char *text);
/* ms -> block index */
static inline uint32_t tx_ms_block(uint32_t ms) { return (uint32_t)(((uint64_t)ms * TX_FS) / (1000u * TX_BLOCK)); }

/* run a parsed script on an instance (init'ed here with s->seed); cb sees every block after it is rendered */
typedef void (*tx_block_cb)(void *user, uint32_t blk, const int32_t *mix, const tx_stats_t *st);
void tx_script_run(const tx_engine_t *e, void *st, const tx_script_t *s, tx_block_cb cb, void *user);
/* step API (for interleaving instances): apply the events of block blk (from index *next), render one block */
void tx_script_step(const tx_engine_t *e, void *st, const tx_script_t *s, uint32_t blk, unsigned *next, int32_t *mix);

#endif
