/* SPDX-License-Identifier: GPL-3.0-only */
/* What the plugin's window (plugin/gui_core.c) may ask of its instance (plugin/src/textura_clap.c), so the window never
 * touches the bridge's slots or the app. Main thread only (CLAP's gui thread), and never blocking on the audio thread:
 * reads come from what the audio side last published (txp_snap_t), writes go through a queue the audio thread takes
 * at its next process call (or a flush the host is asked for while it is not processing), and every write to a host
 * parameter is told to the host as a CLAP parameter event, with gestures around a drag (its undo, its automation).
 *
 * The window's writes: host parameters (a lane's LEVEL, PAN and cards, LANES, PRESET: the app's own ids, values 0 ..
 * hi as the host sees them), a lane's mute (the app's own: a scene keeps it) and the SOLO (the plugin's own: lanes
 * soloed play, the others are shut; saved with the project), a lane's sound from the sound browser and a scene or
 * landscape from the presets browser (the FM-1 browsers' own calls: slot_bridge.h). The window's own state, saved with
 * the project too: its width, the selected lane, the place's view and the engines' colours. */
#ifndef TXP_UI_H
#define TXP_UI_H
#include <stdint.h>
#include "bridge.h"

/* the ids the window uses (params.c: lane k's scope is k for lanes 1 .. 4, 16 + k - 4 after; the globals' scope 4) */
#define TXP_ID_LANE(k, i) (((uint32_t)(k) < 4u ? (uint32_t)(k) : 12u + (uint32_t)(k)) << 8 | (uint32_t)(i))
enum { TXP_PL_SOUND = 0, TXP_PL_COUNT = 1, TXP_PL_ACTIVE = 2, TXP_PL_LEVEL = 3, TXP_PL_PAN = 4, TXP_PL_CARD0 = 8 };
#define TXP_ID_LANES (4u << 8 | 81u)       /* LANES: the lanes in use - 1 (params.c PG_NLANE) */
#define TXP_ID_GLOB(i) (4u << 8 | (uint32_t)(i))   /* a global (params.c PG_*) */
enum { TXP_PG_SCNTEMPO = 37, TXP_PG_SCNFX = 38, TXP_PG_GAMOUNT = 56, TXP_PG_GTYPE, TXP_PG_GSHIFT, TXP_PG_SCALE,
       TXP_PG_ROOT, TXP_PG_SELCH, TXP_PG_LCH0, TXP_PG_SCMODE = TXP_PG_LCH0 + 4, TXP_PG_TRANS, TXP_PG_LCH4,
       TXP_PG_CHG = TXP_PG_LCH4 + 12, TXP_PG_NLANE };
#define TXP_ID_LCH(k) TXP_ID_GLOB((k) < 4 ? TXP_PG_LCH0 + (k) : TXP_PG_LCH4 + (k) - 4)   /* lane k's MIDI channel */
#define TXP_ID_MODCC (7u << 8 | 64u)      /* MOD CC (params.c P_MAT, mod.c MQ_CC) */
#define TXP_COL_N 11                      /* the engines' colours (gui_core.c's palette; 0 bone, the default) */

typedef struct txp_plugin txp_core;      /* one plugin instance */
struct clap_plugin;
txp_core *txp_core_of(const struct clap_plugin *plugin);
void **txp_ui_gui_data(txp_core *c);     /* the window's own pointer for this instance (plugin/gui_clap.c) */
int txp_ui_has_slot(const txp_core *c);  /* 0: one of more than 16 instances in this process (it stays silent) */

int txp_ui_snap(const txp_core *c, txp_snap_t *out);   /* what the app published last: 0 ok (-1: no slot, all zero) */
int txp_ui_get(const txp_core *c, uint32_t id);       /* a host parameter's value (the window's own write until the
                                                       * app shows it); -1 an unknown id */
int txp_ui_hi(uint32_t id);                           /* its range 0 .. hi; -1 unknown */
int txp_ui_default(const txp_core *c, uint32_t id);   /* a fresh instance's value (a double-click) */
void txp_ui_begin(txp_core *c, uint32_t id);          /* a drag: begin, any number of sets, end */
void txp_ui_set(txp_core *c, uint32_t id, int v);     /* (a click alone: a set alone) */
void txp_ui_end(txp_core *c, uint32_t id);
void txp_ui_mute(txp_core *c, uint32_t lane, int on); /* lane 0 .. 15 */
void txp_ui_solo(txp_core *c, uint32_t mask);         /* bit k: lane k + 1 soloed */
void txp_ui_mutes(const txp_core *c, uint32_t *mute, uint32_t *solo);   /* the masks now (the window's writes first) */
uint32_t txp_ui_act(const txp_core *c, uint8_t (*ev)[TXP_LANES], uint8_t (*pk)[TXP_LANES], uint32_t n);   /* bridge.h */
int txp_ui_win_w(const txp_core *c);                  /* the window's width saved (0: the default) */
void txp_ui_win_w_set(txp_core *c, int w);
void txp_ui_win_resize(txp_core *c, int w);           /* the window asks its host for that width (SETTINGS: SIZE) */
/* the sound browser: lane's sound set to family fam's sound snd (an audition; keep: the choice, the scene edited) */
void txp_ui_sound(txp_core *c, int lane, int fam, int snd, int keep);
/* the presets browser: a scene loaded (0 .. 115: F001 .. U16; TXP_LOAD_LS + n: landscape n), with_fx: its effects too */
void txp_ui_load(txp_core *c, int what, int with_fx);
int txp_ui_col(const txp_core *c, int eng);           /* an engine's colour (0 .. TXP_COL_N - 1; 0 bone) */
void txp_ui_col_set(txp_core *c, int eng, int col);
void txp_ui_col_reset(txp_core *c);
int txp_ui_view(const txp_core *c);                   /* the place's view: 0 MAP, 1 SPACE (saved with the project) */
void txp_ui_view_set(txp_core *c, int v);
int txp_ui_sel(const txp_core *c);                    /* the selected lane, 0 .. 15 */
void txp_ui_sel_set(txp_core *c, int k);
#endif
