/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura engine API: every texture engine is an instance (all state in one struct) that renders into a shared mix.
 *
 * Rules for every file in engines/ (bench/txtest checks them):
 *   - integers only: no float / double, no 64-bit '/' or '%' (no libgcc on the device). 64-bit products only at
 *     event or control rate; inner (per-sample) loops use 32-bit products.
 *   - no mutable globals: an engine's state lives in the memory its caller hands to init(); const tables are shared.
 *     So up to TX_SLOTS instances (of the same or different engines) can render one after the other into one mix.
 *   - audio-rate functions are TX_AUDIO_CODE and the tables they read TX_AUDIO_DATA (internal SRAM on the FM-1).
 *
 * Audio: mono, Q23 in int32 (1 << 23 = full scale), 256-frame blocks at 44.1 kHz (5805 us), Frodhes' platform. The
 * mix buffer has 8 bits of headroom over full scale; the output stage (later: the firmware's) saturates.
 *
 * Keys: key() takes a MIDI note number 0..127. The FM-1's 27 keys F3..G5 are notes 53..79 (TX_KEY_F3 + k). Velocity
 * 1..127; on = 0 is a release. A key pressed = a burst of particles; held = continuous emission; released = the
 * emission stops and the particles ring out (engines may differ, as their physics says).
 *
 * Parameters: each engine has its OWN list, taken from its physics (tx_param_t, values 0..max, max <= 127). Only two
 * things are shared by all engines and set through the same param() call with the reserved ids below:
 *   TX_PID_PARTICLES  the hero-pool limit, 1..particles_max (density vs CPU)
 *   TX_PID_LEVEL      output level 0..127, 100 = unity (tx_level_q15) */
#ifndef TX_ENGINE_H
#define TX_ENGINE_H
#include <stdint.h>

#ifndef TX_AUDIO_CODE
#define TX_AUDIO_CODE              /* firmware: the internal-SRAM code section */
#endif
#ifndef TX_AUDIO_DATA
#define TX_AUDIO_DATA              /* firmware: the internal-SRAM data section */
#endif

#define TX_FS 44100u
#define TX_BLOCK 256u              /* frames per render call (at most) */
#define TX_BLOCK_US 5805u          /* 256 / 44100 s */
#define TX_Q23 (1 << 23)           /* full scale in the mix */
#define TX_KEY_F3 53u              /* the FM-1's first key; G5 = 79 */
#define TX_NKEYS 27u
#define TX_SLOTS 4u                /* instances that may play at once (stage S3) */
#define TX_STATE_MAX 16384u        /* bytes of state an instance may use (a slot's arena) */

#define TX_PID_PARTICLES 0x80u     /* shared parameter ids (param()) */
#define TX_PID_LEVEL 0x81u

typedef struct {
    const char *name;              /* short label, upper case: "RATE" */
    uint8_t max;                   /* values 0..max (max <= 127) */
    uint8_t def;                   /* default */
    const char *help;              /* one line for the player: what it shapes, and its range in physical units */
} tx_param_t;

typedef struct {                   /* a snapshot after the last render (for tests, the bench and the player) */
    uint16_t particles;            /* hero particles sounding now */
    uint16_t limit;                /* the PARTICLES limit in force */
    uint16_t events;               /* events emitted in the last block */
    uint16_t dropped;              /* events of the last block that found no hero (quieter than every hero) or */
                                   /* were over the per-block cap */
    uint32_t steals;               /* heroes stolen since init (quietest first) */
    uint32_t total;                /* events since init */
    uint8_t quiet;                 /* 1: nothing sounds; render() adds exact zeros until the next key */
} tx_stats_t;

typedef struct tx_engine_s {
    const char *name;              /* id: "test" (file tx_eng_<name>.c, WAVs build/wav/<name>-<scene>.wav) */
    const char *title;             /* shown in the player */
    uint8_t n_params;
    const tx_param_t *params;
    uint8_t particles_max;         /* the hero pool's size (the PARTICLES range is 1..particles_max) */
    uint8_t particles_def;         /* default limit: the engine's worst case must fit its CPU share at this value */
    uint32_t state_bytes;          /* sizeof the instance struct (<= TX_STATE_MAX) */
    const char *scene_max;         /* the worst-case scene, in txrender's script language (bench/tx_script.h) */
    void (*init)(void *st, uint32_t seed);                       /* zero state, defaults applied, PRNG seeded */
    void (*key)(void *st, uint8_t note, uint8_t vel, uint8_t on);
    void (*param)(void *st, uint8_t id, uint8_t val);            /* id < n_params, or TX_PID_* */
    void (*render)(void *st, int32_t *mix, unsigned n);          /* ADDS n <= TX_BLOCK mono Q23 samples into mix */
    void (*stats)(const void *st, tx_stats_t *s);
} tx_engine_t;

/* the registry (tx_engines.c): every engine in the build, then a null */
extern const tx_engine_t *const tx_engines[];
const tx_engine_t *tx_engine_find(const char *name);   /* by name, or 0 */
int tx_param_find(const tx_engine_t *e, const char *name);   /* by name (case-insensitive), or -1 */

/* LEVEL 0..127 -> Q15 gain: (v / 100)^2, so 100 = unity, 127 = +4.2 dB, 0 = silent */
static inline int32_t tx_level_q15(uint32_t v) { return (int32_t)((v * v * 26844u) >> 13); }

#endif
