/* SPDX-License-Identifier: GPL-3.0-only */
/* The desktop plugin's bridge (from Frodhes' plugin/src/bridge.h).
 *
 * Two layers:
 *   the slot     one copy of the app (firmware/src/textura.c over web/web_hal.c) compiled into its own object
 *                (plugin/src/engine_unit.c with TXP_SLOT = 0 .. TXP_SLOTS - 1, plugin/cmake/slots.cmake). Everything
 *                in it is static except its table, txp_slot_<N> (txp_slot_api_t below): independent apps in one binary,
 *                no runtime file copies. Every slot is the HQ build (lanes.c LN_HQ: no CPU limit, nothing thinned).
 *                The per-slot glue (time, queue, FIFO, state, the host's parameters) is slot_bridge.h.
 *   the instance a plugin instance (plugin/src/bridge.c): it holds one slot, maps the host's frames and events onto the
 *                app's frames, resamples for other rates and hands out float.
 *
 * Time. The app runs on host milliseconds: each step is one, and every 256 frames owed app_audio(256) renders a block
 * into the slot's FIFO. Two ways to place the events:
 *   exact  (the plugin's own) each event carries the app frame it is for, in 1/256 frame (putx). A step hands the
 *          app only the events for the next block to render, runs one pass of app_main() (a coroutine) up to its next
 *          platform_poll(), then renders what is owed: an event acts from the start of the block that holds its frame
 *          (as every event does on the FM-1: the lanes take their queue at each block's start).
 *   ms     (the host build's, tests/host.c: the test mode) each event carries the host millisecond it is due at
 *          (put); a step: ms + 1, the events due into the app's MIDI ring / applied, the audio owed, then the
 *          app_main() pass. A 44.1 kHz render of a MIDI script is the host build's render of the same script, sample
 *          for sample (tools/plugin_gate.py).
 *
 * Threads (CLAP's): boot / reuse / state reads on the main thread; put / step / read on the audio thread; drain and
 * settle on the main thread only while the instance is not processing. Never both at once for one slot (the instance
 * owns its slot). */
#ifndef TEXTURA_PLUGIN_BRIDGE_H
#define TEXTURA_PLUGIN_BRIDGE_H
#include <stdint.h>

#define TXP_SLOTS 16u                    /* the slots: the instances that can play at once */
#define TXP_FS 44100u                    /* the app's rate: always */
#define TXP_NPARAM_MAX 1024u             /* the host's parameters (slot_bridge.h sb_params: about 480 with 16 lanes) */
#define TXP_STATE_MAX 8192               /* the state's JSON text (slot_bridge.h) */
#define TXP_ID_OWN0 0x10000u             /* the plugin's own parameters: not the app's ids (those are < 0x1C00) */
#define TXP_ID_PRESET (TXP_ID_OWN0 + 0u) /* PRESET: F001 .. F100 (a program change on channel 1) */
#define TXP_LAT_MS 10u                   /* the ms test mode's pre-roll (bridge.c) */

enum { TXP_EV_MIDI = 1, TXP_EV_PARAM, TXP_EV_RESET, TXP_EV_RELEASE };

/* a host parameter: the app's id, its range, how its value reads */
typedef struct {
    uint32_t id;                         /* params.c: scope << 8 | index (stable), or TXP_ID_OWN0 + k */
    uint16_t hi;                         /* 0 .. hi, stepped */
    uint8_t scaled;                      /* 1: a lane's card: 0 .. 127 onto the card's range now (as its CC) */
    uint8_t is_enum;                     /* a choice (the host may show a list) */
    char name[32];
    char module[24];
} txp_param_t;

/* what the main thread may read of a slot: published by the audio side (a sequence lock) */
typedef struct {
    uint32_t rev;                        /* bumps on every change the app made or took */
    int16_t pv[TXP_NPARAM_MAX];          /* the host parameters' values (the table's order) */
    uint8_t scene;                       /* the scene loaded last (scenes.c scn_cur; 0xFF none) */
    uint8_t settled;                     /* no load, glide or engine switch under way */
    uint16_t tempo10;                    /* the app's tempo (BPM x 10) */
} txp_snap_t;

typedef struct {
    uint32_t steps, blocks, q_full, ring_wait, midi, params, state_ok, state_bad, ms;
} txp_stats_t;

typedef struct {
    int (*boot)(void);                   /* main thread: the app started (0 ok) */
    int (*booted)(void);
    void (*reuse)(void);                 /* main thread: as booted again (the last instance's events dropped) */
    int (*put)(int kind, uint32_t at_ms, uint32_t a, int32_t v);    /* ms: due at that millisecond (0: at once) */
    int (*putx)(int kind, uint32_t at_q8, uint32_t a, int32_t v);   /* exact: for that app frame x 256 */
    void (*step)(void);                  /* one millisecond */
    void (*drain)(void);                 /* the queue applied now (no block running) */
    uint32_t (*avail)(void);             /* frames in the FIFO */
    void (*read)(int32_t *lr, uint32_t n);
    uint32_t (*now)(void);               /* the slot's millisecond */
    uint32_t (*room)(void);              /* events that still fit the queue */
    void (*set_exact)(int on);
    uint32_t (*efr)(void), (*efw)(void); /* the FIFO's read and write frames (the app's frames) */
    void (*snapshot)(txp_snap_t *out);
    int (*state_get)(char *buf, int n, int fresh);   /* fresh: the audio side captures it first (processing) */
    int (*state_set)(const char *json);  /* into the mailbox (the next step or drain applies it); -1 unreadable */
    void (*set_tempo)(uint32_t bpm10);   /* the host's tempo (0: none) */
    void (*stats)(txp_stats_t *out);
    uint32_t (*params)(txp_param_t *tab, uint32_t max);   /* the host parameters (constant tables only) */
    int (*text)(uint32_t id, int32_t v, char *buf, uint32_t n);   /* v's text (constant tables only); -1: a number */
    void (*settle)(void);                /* main thread, not processing: run until a load is done (audio dropped) */
} txp_slot_api_t;

#define TXP_SLOT_DECL(n) extern const txp_slot_api_t txp_slot_##n;
TXP_SLOT_DECL(0) TXP_SLOT_DECL(1) TXP_SLOT_DECL(2) TXP_SLOT_DECL(3) TXP_SLOT_DECL(4) TXP_SLOT_DECL(5)
TXP_SLOT_DECL(6) TXP_SLOT_DECL(7) TXP_SLOT_DECL(8) TXP_SLOT_DECL(9) TXP_SLOT_DECL(10) TXP_SLOT_DECL(11)
TXP_SLOT_DECL(12) TXP_SLOT_DECL(13) TXP_SLOT_DECL(14) TXP_SLOT_DECL(15)

/* ---- the instance (bridge.c) */
typedef struct txp_inst txp_inst_t;
const txp_param_t *txp_params(uint32_t *n);   /* the host parameters (the same for every instance) */
int txp_param_index(uint32_t id);             /* its place in the table, -1 none */
int txp_param_text(uint32_t idx, double v, char *out, uint32_t n);
int txp_param_parse(uint32_t idx, const char *text, double *out);
txp_inst_t *txp_inst_new(void);          /* main thread; a slot claimed (none free: the instance stays silent) */
void txp_inst_free(txp_inst_t *in);
int txp_inst_slot(const txp_inst_t *in);
int txp_inst_activate(txp_inst_t *in, double rate);   /* 0 ok; -1 a rate it cannot play */
void txp_inst_deactivate(txp_inst_t *in);
uint32_t txp_inst_latency(const txp_inst_t *in);
void txp_inst_reset(txp_inst_t *in);
void txp_inst_midi(txp_inst_t *in, uint32_t time, uint8_t st, uint8_t d1, uint8_t d2);   /* time: frame in the block */
void txp_inst_param(txp_inst_t *in, uint32_t time, uint32_t id, double value);
void txp_inst_param_now(txp_inst_t *in, uint32_t id, double value);   /* outside processing (a flush): queued */
void txp_inst_drain(txp_inst_t *in);     /* the queue applied (not active: and settled) */
int txp_inst_get(const txp_inst_t *in, uint32_t id, double *value);
int txp_inst_snap(const txp_inst_t *in, txp_snap_t *out);
int txp_inst_state_get(const txp_inst_t *in, char *buf, int n);
int txp_inst_state_set(txp_inst_t *in, const char *json);
uint32_t txp_inst_rev(const txp_inst_t *in);
void txp_inst_stats(const txp_inst_t *in, txp_stats_t *st, uint32_t *late);
void txp_inst_render(txp_inst_t *in, float *l, float *r, uint32_t n, uint32_t tempo_bpm10);
#endif
