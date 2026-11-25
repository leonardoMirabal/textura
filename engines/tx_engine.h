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
 *   TX_PID_LEVEL      output level 0..127, 100 = unity (tx_level_q15)
 *   TX_PID_KEYAT      the next key() lands this many samples (0..255) into the coming render block, not at its start
 *                     (a caller that knows an event's sample: the source's clock, a lane's generator). One-shot: any
 *                     key() call takes it back to 0. An engine without it ignores it (its key stays at the start) */
#ifndef TX_ENGINE_H
#define TX_ENGINE_H
#include <stdint.h>

#ifndef TX_AUDIO_CODE
#define TX_AUDIO_CODE              /* firmware: the internal-SRAM code section */
#endif
#ifndef TX_AUDIO_DATA
#define TX_AUDIO_DATA              /* firmware: the internal-SRAM data section */
#endif
/* TX_RAM: a hot loop that runs from RAM on the FM-1 (.ram_text, copied there at boot) instead of XIP flash through its
 * cache. In a full mix (four lanes and the effects) the code no longer fits the cache and each block fetches it again:
 * ~28 us a KB a block on the device, ~3 % of a four-lane block for the grain delay's 4 KB. RAM and XIP are farther
 * apart than a call reaches (23 bits): a TX_RAM function calls only TX_RAM code or inlines (TX_INLINE), and XIP calls
 * it through TX_FAR (its address in a register: one instruction more). On the host both are nothing. */
#ifdef __PI32V2__
#ifdef TX_TWIN_BUILD
#define TX_RAM                     /* (core 1's twin: the same code in XIP, see TX_C1) */
#else
#define TX_RAM __attribute__((section(".ram_text.tx")))
#endif
#define TX_FAR(fn) (__extension__({ __typeof__(&(fn)) p_ = &(fn); __asm__("" : "+r"(p_)); p_; }))
#define TX_SHARE                   /* (a flash function a TX_RAM one calls: global, so its twin calls this one copy) */
#else
#define TX_RAM
#define TX_FAR(fn) fn
#define TX_SHARE static
#endif
/* TX_C1(fn): core 1's copy of a TX_RAM function. On the FM-1 both cores fetching code from RAM at once slow each other,
 * so core 1 runs twins in XIP: tools/fm1_bench.py builds every engine file with TX_RAM code a second time
 * (TX_TWIN_BUILD: TX_RAM empty, each TX_RAM function fn renamed fn_c1, the file's other globals weak, so the one copy
 * of those is kept) and core 1's jobs call fn_c1; core 0 keeps the RAM original. The same source, the same samples.
 * TX_TWIN(fn) declares the twin (after fn). On the host, and with TX_NO_TWINS (fm1_bench.py --no-twins), TX_C1 is
 * TX_FAR. */
#if defined __PI32V2__ && !defined TX_TWIN_BUILD && !defined TX_NO_TWINS
#define TX_C1(fn) fn##_c1
#define TX_TWIN(fn) extern __typeof__(fn) fn##_c1
#else
#define TX_C1(fn) TX_FAR(fn)       /* (TX_NO_TWINS: the RAM code itself, out of a call's reach) */
#define TX_TWIN(fn) extern __typeof__(fn) fn
#endif
#define TX_INLINE static inline __attribute__((always_inline))   /* (a helper a TX_RAM function may use) */

#define TX_FS 44100u
#define TX_BLOCK 256u              /* frames per render call (at most) */
#define TX_BLOCK_US 5805u          /* 256 / 44100 s */
#define TX_Q23 (1 << 23)           /* full scale in the mix */
#define TX_KEY_F3 53u              /* the FM-1's first key; G5 = 79 */
#define TX_NKEYS 27u
#define TX_SLOTS 4u                /* instances that may play at once (stage S3) */
#define TX_STATE_MAX 16384u        /* bytes of state an instance may use (a slot's arena) */
/* an engine's state_bytes, checked at compile time: an instance over TX_STATE_MAX fails the build (a lane could never
 * take it: lanes.c ln_select refuses it, and a scene load waited for it forever) */
#define TX_STATE_SIZE(T) ((uint32_t)sizeof(T) + 0u * (uint32_t)sizeof(char[sizeof(T) <= TX_STATE_MAX ? 1 : -1]))

/* clear n bytes (tx_engines.c): 32-bit stores where p is word aligned (an engine's state, a voice), else byte by byte.
 * The same zeros as a byte loop, which the FM-1's compiler keeps as one store a byte (no builtin memset there): an
 * engine's init, a hosted engine's switch, cleared ~12 KB at a time inside an audio block */
void tx_zero(void *p, uint32_t n);

#define TX_PID_PARTICLES 0x80u     /* shared parameter ids (param()) */
#define TX_PID_LEVEL 0x81u
#define TX_PID_KEYAT 0x82u         /* (see above: a key's sample offset in the coming block) */

typedef struct {
    const char *name;              /* short label, upper case: "RATE" */
    uint8_t max;                   /* values 0..max (max <= 127) */
    uint8_t def;                   /* default */
    const char *help;              /* one line for the player: what it shapes, and its range in physical units */
} tx_param_t;
/* a parameter's help line: the player's and the bench's text. The FM-1 app (TX_APP, tools/fm1_bench.py --app) reads
 * none of them (only the source's PRESET line, which is left plain), so it keeps none in its flash */
#ifdef TX_APP
#define TX_TEXT(s) ""
#else
#define TX_TEXT(s) s
#endif

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

/* the mix's effects in the order the sound goes through them (tx_core_mix.c, tx_fx.c): COMPRESSOR, DRIVE, FILTER,
 * GRAIN DELAY, REVERB, as Frodhes, then the M/S (the last: the whole mix's middle and sides). The FX pages and the EFFECTS front page follow this order left to right (ui.c); on
 * the host each call marks its stage (TX_STAGE), and the host's state "chain" / "chainok" (tests/scripts/chain.txt)
 * fails when the audio and the pages ever part. Nothing on the FM-1 */
enum { TX_ST_COMP, TX_ST_DRIVE, TX_ST_FILTER, TX_ST_GRAIN, TX_ST_REVERB, TX_ST_MS, TX_ST_N };
#ifdef DINGHY_HOST
extern uint8_t tx_st_seen[8], tx_st_n;          /* (tx_core_mix.c: the stages of the last block, in their order) */
#define TX_STAGE(s) do { if (tx_st_n < 8u) tx_st_seen[tx_st_n++] = (uint8_t)(s); } while (0)
#else
#define TX_STAGE(s) do { } while (0)
#endif

#endif
