/* SPDX-License-Identifier: GPL-3.0-only */
/* The plugin window's core: everything but the operating system (the platform views, plugin/gui_platform.h, own the
 * native view and its pixels and pass the input). It reads what the instance plays through plugin/src/txp_ui.h,
 * draws the whole window into an RGBA canvas with frui (plugin/ui: anti-aliased shapes, the font) and turns the mouse
 * into the app's parameters, mutes and solos. Main thread only.
 *
 * The window (TXG_W x TXG_H logical pixels, gui_size.h; any size at its aspect, drawn at the canvas's pixels):
 *   top bar     TEXTURA, the scene loaded last with its previous / next (PRESET; a click on it: the presets browser),
 *               LANES, SETTINGS
 *   MAP         the place seen from above, you at the bottom: each lane a dot, its angle the lane's PAN, its distance
 *               the card named DIST of its sound (a sound without one: the middle ring, a hollow dot, the angle only);
 *               a dot glows with its lane's events; drag one to move it (PAN, DIST), click one to select its lane.
 *               Above the field the selected lane: its family, sound (a click: the sound browser), its engine's
 *               colour (a click: the picker), PAN, DIST, HOW MANY, HOW ACTIVE, channel; the LFOs routed, traced red.
 *   SPACE       (the MAP | SPACE switch) the same place in perspective: the lanes on a ground, far ones small, each
 *               one's events drifting toward the horizon; a drag on the ground moves a lane (PAN, DIST).
 *   lanes       one row a lane in use (LANES, 1 .. 16): its number, the sound and family, its event line (each app
 *               block's engine events as a bar, as high as the lane's peak then; the last 6 s, newest at the right),
 *               its MIDI channel, VOL (the lane's LEVEL), M (the app's mute) and S (the plugin's solo); a muted or
 *               soloed-out lane fades. Below the last, ADD LANE (LANES + 1).
 * Overlays: the sound browser (families, a search, the cursor's sound heard on the lane while it plays, KEEP or
 * CLOSE puts the old one back), the presets browser (USER, the factory's categories, the landscapes; LOAD), SETTINGS
 * (MIDI IN, SCALE, TIMING, LOADING, ENGINE COLOURS, WINDOW). Each engine has a colour (bone at first) for its dots,
 * event lines and row accents. Red only marks the selected lane, what is on (the solo) and what an LFO moves. Repaints: the whole window when anything drawn but the
 * activity changed, else only the map and the event lines (frui's base clip: the same pixels as a whole repaint). */
#ifndef TXG_CORE_H
#define TXG_CORE_H
#include <stdint.h>
#include "gui_platform.h"
#include "gui_size.h"
#include "src/txp_ui.h"

/* the controls (txg_find, the tests) */
enum { TXG_W_NONE, TXG_W_ROW, TXG_W_VOL, TXG_W_MUTE, TXG_W_SOLO, TXG_W_DOT, TXG_W_PREV, TXG_W_NEXT, TXG_W_LDN,
       TXG_W_LUP, TXG_W_SET, TXG_W_CLOSE, TXG_W_ADD,
       /* 3b: the place's view, the browsers, SETTINGS, the colours (txg_find's lane: see gui_core.c's wid_add2 calls) */
       TXG_W_VMAP, TXG_W_VSPACE, TXG_W_PRESET, TXG_W_SND, TXG_W_COLDOT, TXG_W_SWATCH, TXG_W_COLRESET, TXG_W_PICKER,
       TXG_W_SEARCH, TXG_W_NAV, TXG_W_LIST, TXG_W_KEEP, TXG_W_LOAD, TXG_W_SEG, TXG_W_FIELD, TXG_W_WSIZE, TXG_W_VIEWSEL };

typedef struct txg txg_t;
txg_t *txg_new(txp_core *c);             /* NULL: out of memory or the font did not load */
void txg_free(txg_t *g);
int txg_resize(txg_t *g, int pw, int ph);   /* its own canvas (headless): pw x ph pixels at the layout's aspect; 0 ok */
int txg_tick(txg_t *g);                  /* reads what plays; 1 when a repaint is due */
int txg_paint(txg_t *g);                 /* its own canvas brought up to date; 1 when pixels changed */
const uint32_t *txg_pixels(const txg_t *g, int *w, int *h);   /* 0xFFRRGGBB */
int txg_input(txg_t *g, const frg_input *e);   /* bit 0: what is drawn may have changed; bit 1: the event was used */
int txg_find(const txg_t *g, int kind, int lane, float r[4]);   /* a control of the last draw (logical x y w h): 1 */
const frg_client *txg_client(txg_t *g);  /* for a platform view (gui_platform.h) */
void txg_fit_size(uint32_t *w, uint32_t *h);   /* the nearest size the layout takes (logical points) */
#endif
