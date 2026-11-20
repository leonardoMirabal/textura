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
#define TXP_STATE_MAX 81920              /* the state's JSON text (slot_bridge.h): the scene, the settings and the user
                                          * scenes U01 .. U16 (each one's record in hex, 4 kB) */
#define TXP_ID_OWN0 0x10000u             /* the plugin's own parameters: not the app's ids (those are < 0x1C00) */
#define TXP_ID_PRESET (TXP_ID_OWN0 + 0u) /* PRESET: F001 .. F100 (a program change on channel 1) */
#define TXP_ID_KEYS (TXP_ID_OWN0 + 1u)   /* KEYS: what SELECTED CH's notes F3 .. G5 do: 0 PLAY (they play the selected
                                          * lane, as sent: the default), 1 FM-1 KEYS (the FM-1's keys in LANDSCAPE mode)
                                          * (slot_bridge.h sb_keys) */
#define TXP_LAT_MS 10u                   /* the ms test mode's pre-roll (bridge.c) */

enum { TXP_EV_MIDI = 1, TXP_EV_PARAM, TXP_EV_RESET, TXP_EV_RELEASE,
       TXP_EV_MUTE,                      /* the window's M: lane a muted (v 1) or not (the app's own mute: scape.c) */
       TXP_EV_SOLO,                      /* the window's S: the lanes soloed, v a mask (the plugin's own: slot_bridge.h) */
       TXP_EV_SOUND,                     /* the sound browser: lane a gets family v >> 4's sound v & 15 (the FM-1
                                          * browser's sc_sound_set); v | TXP_SND_KEEP: kept (the scene is edited) */
       TXP_EV_LOAD,                      /* the presets browser: v a scene (0 .. 115: F001 .. U16) or TXP_LOAD_LS +
                                          * a landscape; a 1: with its effects (the FM-1's EDIT again on the one loaded) */
       TXP_EV_SEL,                       /* the selected lane: a (the app's own: SELECTED CH's notes, the keys) */
       TXP_EV_REC,                       /* every lane and the transport stopped / started (the FM-1's REC) */
       TXP_EV_UNDO,                      /* the last load or sound kept undone; again: redone (the FM-1's SAVE held) */
       TXP_EV_NAME,                      /* a user scene's name for TXP_EV_SAVE: its characters a .. a + 3 in v, low
                                          * byte first */
       TXP_EV_SAVE,                      /* the scene that plays saved into user scene a (0 .. 15: U01 .. U16) */
       TXP_EV_TEMPO };                   /* the app's own tempo: v BPM x 10 (the host's leads when it gives one) */
#define TXP_SND_KEEP 0x1000
#define TXP_LOAD_LS 0x100
#define TXP_LANES 16u                    /* the lanes a slot has (TX_MIX_SLOTS: plugin/cmake/slots.cmake) */
#define TXP_ACT_N 2048u                  /* the activity ring: app blocks (11.9 s), a power of two */

/* a host parameter: the app's id, its range, how its value reads */
typedef struct {
    uint32_t id;                         /* params.c: scope << 8 | index (stable), or TXP_ID_OWN0 + k */
    uint16_t hi;                         /* 0 .. hi, stepped */
    uint8_t scaled;                      /* 1: a lane's card: 0 .. 127 onto the card's range now (as its CC) */
    uint8_t is_enum;                     /* a choice (the host may show a list) */
    char name[32];
    char module[24];
} txp_param_t;

/* what the window shows of a lane and of the slot (published with the values, txp_snap_t) */
typedef struct {
    uint8_t used;                        /* a sound is set */
    uint8_t mute;                        /* the app's mute (a scene keeps it) */
    uint8_t ch;                          /* its MIDI channel: 0 OFF, 1 .. 16 */
    uint8_t dist;                        /* the card named DIST of its sound (0 .. 7), 0xFF none */
    uint8_t fi, si;                      /* its family and sound (txp_catalog_t), 0xFF none */
    uint8_t eng;                         /* its engine (txp_catalog_t eng), 0xFF none */
    uint8_t run;                         /* it plays (HOW MANY not OFF) */
    uint8_t modm;                        /* what the modulation moves now (TXP_MOD_*) */
    uint8_t lpan, ldist;                 /* PAN and DIST as the modulation has them now (0 .. 127) */
    uint8_t mcnt, mact;                  /* stopped by STOP ALL: the HOW MANY and HOW ACTIVE PLAY ALL brings back
                                          * (0: none remembered) */
    char snd[24];                        /* the sound's name ("campfire"), "" none */
    char fam[16];                        /* its family ("Fire") */
} txp_ui_lane_t;
enum { TXP_MOD_LEVEL = 1, TXP_MOD_PAN = 2, TXP_MOD_COUNT = 4, TXP_MOD_ACTIVE = 8, TXP_MOD_DIST = 16 };
#define TXP_CARD_N 8u                    /* a sound's cards at most (params.c SCD_MAX) */
typedef struct {                         /* a lane's cards (its sound's), for the window's drawer */
    uint8_t n;                           /* how many */
    uint8_t mod;                         /* bit i: card i is moved by the modulation now */
    uint8_t live[TXP_CARD_N];            /* each one as the modulation has it now (0 .. 127, as its host value) */
    uint8_t def[TXP_CARD_N];             /* its sound's own value (0 .. 127: a double-click) */
    char lab[TXP_CARD_N][9];             /* their names ("DIST") */
} txp_ui_cards_t;
#define TXP_LFO_N 8u
#define TXP_USER_N 16u
typedef struct {
    uint8_t nlane;                       /* LANES: 1 .. 16 */
    uint8_t edited;                      /* the scene was changed since its load or save */
    uint16_t solo;                       /* the lanes soloed (bit k: lane k + 1; 0 none) */
    char preset[24];                     /* the scene loaded last ("F003 GARDEN RAIN"), "" none */
    uint8_t scene;                       /* its number (0 .. 115), 0xFF none (a landscape: land) */
    uint8_t land;                        /* the landscape (its family) */
    uint8_t fx_yours;                    /* EFFECTS ON LOAD is SMART and the effects were changed since the load */
    uint8_t lfo_on;                      /* the LFOs routed (bit n: LFO n + 1 has a route with a depth) */
    int16_t lfo[TXP_LFO_N];              /* each LFO's value now (Q15, bipolar) */
    char lfo_to[TXP_LFO_N][24];          /* its first route's target ("L1 PAN") */
    uint16_t user;                       /* the user scenes saved (bit n: U01 + n) */
    char uname[TXP_USER_N][13];          /* their names */
    uint8_t sel;                         /* the app's selected lane (scape.c sc.sel) */
    uint8_t any_run;                     /* a lane plays (REC: all stopped) */
    uint8_t saving;                      /* a user scene being saved */
    uint8_t undo;                        /* an undo is there (a load, or a sound kept) */
    int16_t comp_gr10;                   /* the compressor's gain reduction now, dB x 10 (0: none, or off): its most
                                          * since the last publication */
    int16_t comp_in10, comp_out10;       /* its input (its detector's level) and output levels, their most since the
                                          * last publication, dBFS x 10 (-900: silence, or COMP off) */
    int16_t out_db10;                    /* the output's peak since the last publication, dBFS x 10 (-900: silence) */
    uint32_t gmod[2];                    /* the globals (scope 4, index i < 64) the modulation moves now: bit i */
    uint8_t lfo_mod;                     /* the LFOs whose RATE or LEVEL the modulation moves now (bit n) */
    txp_ui_lane_t lane[TXP_LANES];
    txp_ui_cards_t card[TXP_LANES];
} txp_ui_snap_t;

/* the app's constant tables the window shows (families and sounds, the engines, the factory presets): the same for every
 * slot, from constant tables only (any thread) */
#define TXP_FAM_MAX 10u
#define TXP_SND_MAX 12u
#define TXP_ENG_MAX 64u
#define TXP_PR_MAX 100u
#define TXP_CAT_MAX 10u
typedef struct {
    uint8_t nfam;
    struct { char name[16]; uint8_t n; struct { char name[24]; uint8_t eng; } s[TXP_SND_MAX]; } fam[TXP_FAM_MAX];
    uint8_t neng;
    char eng[TXP_ENG_MAX][16];           /* an engine's name ("fire", "stone cloud") */
    uint8_t npr, ncat;
    char pr[TXP_PR_MAX][12];             /* F001 .. : the factory presets' names */
    char cat[TXP_CAT_MAX][16];           /* their categories, cat_at[c] .. cat_at[c + 1] - 1 */
    uint8_t cat_at[TXP_CAT_MAX + 1u];
    struct { uint8_t fi, si, cnt, act, vol, pan, mute; } prl[TXP_PR_MAX][4];   /* a preset's four lanes (fi 0xFF: empty;
                                          * cnt HOW MANY 0 .. 6, act HOW ACTIVE 0 .. 8) */
} txp_catalog_t;

/* what the main thread may read of a slot: published by the audio side (a sequence lock) */
typedef struct {
    uint32_t rev;                        /* bumps on every change the app made or took */
    int16_t pv[TXP_NPARAM_MAX];          /* the host parameters' values (the table's order) */
    uint8_t scene;                       /* the scene loaded last (scenes.c scn_cur; 0xFF none) */
    uint8_t settled;                     /* no load, glide or engine switch under way */
    uint16_t tempo10;                    /* the app's tempo (BPM x 10) */
    txp_ui_snap_t ui;
    /* each host parameter's value as the app shows it now (its page's text: "4.2 kHz", "L12", "1/8", "LFO 2"; a card
     * in its sound's own unit), the table's order; refreshed as the values change (all of them every 32nd time) */
    char ptx[TXP_NPARAM_MAX][16];
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
    /* the lanes' activity, one entry an app block (any thread): the last n blocks (n <= TXP_ACT_N / 2), oldest first,
     * into ev (each lane's engine events in the block, 255 at most) and pk (its peak: 0 silent, 1 .. 255 = -72 .. 0
     * dBFS, before its mute); returns the blocks rendered so far (the newest is that count - 1) */
    uint32_t (*act)(uint8_t (*ev)[TXP_LANES], uint8_t (*pk)[TXP_LANES], uint32_t n);
    void (*catalog)(txp_catalog_t *out); /* the constant tables (any thread) */
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
const txp_catalog_t *txp_catalog(void);       /* the app's constant tables (the same for every instance) */
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
void txp_inst_event(txp_inst_t *in, uint32_t time, int kind, uint32_t a, int32_t v);   /* TXP_EV_MUTE / SOLO, timed */
void txp_inst_event_now(txp_inst_t *in, int kind, uint32_t a, int32_t v);              /* (outside processing) */
uint32_t txp_inst_act(const txp_inst_t *in, uint8_t (*ev)[TXP_LANES], uint8_t (*pk)[TXP_LANES], uint32_t n);
void txp_inst_drain(txp_inst_t *in);     /* the queue applied (not active: and settled) */
int txp_inst_get(const txp_inst_t *in, uint32_t id, double *value);
int txp_inst_snap(const txp_inst_t *in, txp_snap_t *out);
int txp_inst_state_get(const txp_inst_t *in, char *buf, int n);
int txp_inst_state_set(txp_inst_t *in, const char *json);
uint32_t txp_inst_rev(const txp_inst_t *in);
void txp_inst_stats(const txp_inst_t *in, txp_stats_t *st, uint32_t *late);
void txp_inst_render(txp_inst_t *in, float *l, float *r, uint32_t n, uint32_t tempo_bpm10);
#endif
