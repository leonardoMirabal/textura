/* SPDX-License-Identifier: GPL-3.0-only */
/* The plugin window's core: everything but the operating system (the platform views, plugin/gui_platform.h, own the
 * native view and its pixels and pass the input). It reads what the instance plays through plugin/src/txp_ui.h,
 * draws the whole window into an RGBA canvas with frui (plugin/ui: anti-aliased shapes, the font) and turns the mouse
 * into the app's parameters, mutes and solos. Main thread only.
 *
 * The window (at least TXG_W x TXG_H logical pixels, gui_size.h; any size and shape: one scale both ways, the
 * smaller of width / TXG_W and height / TXG_H, and the rest to the lanes, so the MAP stays round):
 *   top bar     TEXTURA, the scene loaded last with its previous / next (PRESET; a click on it: the presets browser),
 *               SAVE (into a user scene, named), UNDO, PLAY ALL / STOP ALL (the FM-1's REC), LANES, the output's
 *               peak, SETTINGS
 *   place       (left) MAP: the place seen from above, you at the centre of a half circle: each lane a dot, its angle
 *               the lane's PAN, its distance the card named DIST of its sound (a sound without one: the middle ring,
 *               a hollow dot); a dot glows with its lane's events; drag one to move it (PAN, DIST); lanes on one spot
 *               fan out and share a label. SPACE: the same place in perspective. At its head the selected lane.
 *   lanes       (right, scrolling) one row a lane in use: its number, the sound and family, its event line, HOW MANY
 *               (six blocks) and HOW ACTIVE (nine), PAN, its MIDI channel, VOL, M and S; ADD LANE under the last.
 *   drawer      the selected lane: its sound, every card (name, value, hint; DIST among them), PAN, LEVEL, GROOVE PART, CYCLE
 *   strip       COMP, DRIVE, FILTER, DELAY, REVERB (each a picture and its main values) and the movement (two LFOs);
 *               a section's header opens the expanded view: every parameter of each effect, LFO 1 .. 8, ENV and
 *               FOLLOW, the matrix and MOD CC
 * Overlays: the sound browser, the presets browser, SETTINGS (MIDI IN with KEYS, SCALE, TIMING, LOADING, WINDOW,
 * ENGINE COLOURS), SAVE's panel, the colour picker. A value (TXG_W_PARAM): drag, wheel, double-click for its default, a
 * click steps a choice. Red only marks the selected lane, what is on and what an LFO moves. Repaints: the whole window
 * when anything drawn but the activity changed, else only the place, the event lines, the drawer and strip and the
 * meter (frui's base clip: the same pixels as a whole repaint). */
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
       TXG_W_SEARCH, TXG_W_NAV, TXG_W_LIST, TXG_W_KEEP, TXG_W_LOAD, TXG_W_SEG, TXG_W_FIELD, TXG_W_WSIZE, TXG_W_VIEWSEL,
       /* step 5: a value (lane: its id), HOW MANY / HOW ACTIVE blocks, PLAY ALL, UNDO, SAVE, the strip's sections
        * (lane: the section), the expanded view's tabs, SAVE's slots, its name, its SAVE, the tempo (lane: its step),
        * the rows' area (the wheel scrolls it) */
       TXG_W_PARAM, TXG_W_MANY, TXG_W_ACT, TXG_W_REC, TXG_W_UNDO, TXG_W_SAVE, TXG_W_FXOPEN, TXG_W_FXTAB, TXG_W_SLOT,
       TXG_W_SAVEGO, TXG_W_NAME, TXG_W_TEMPO, TXG_W_ROWS,
       TXG_W_PLACE, TXG_W_MAPR,    /* (never hit: the place's area and the map's half circle, for the tests) */
       /* step 5b: a row's dot and number (selects its lane), a choice's steppers (lane: its id; v -1 or +1), the
        * compressor's meter (never hit: for the tests) */
       TXG_W_SELDOT, TXG_W_STEP, TXG_W_CMETER, TXG_W_TIP };   /* (TIP: a card whose hint was cut: hovered, all of it) */

typedef struct txg txg_t;
txg_t *txg_new(txp_core *c);             /* NULL: out of memory or the font did not load */
void txg_free(txg_t *g);
int txg_resize(txg_t *g, int pw, int ph);   /* its own canvas (headless): pw x ph pixels, any aspect; 0 ok */
float txg_scale_of(int pw, int ph);      /* canvas pixels a logical pixel for a pw x ph canvas (the views' mouse) */
int txg_tick(txg_t *g);                  /* reads what plays; 1 when a repaint is due */
int txg_paint(txg_t *g);                 /* its own canvas brought up to date; 1 when pixels changed */
const uint32_t *txg_pixels(const txg_t *g, int *w, int *h);   /* 0xFFRRGGBB */
int txg_input(txg_t *g, const frg_input *e);   /* bit 0: what is drawn may have changed; bit 1: the event was used */
int txg_find(const txg_t *g, int kind, int lane, float r[4]);   /* a control of the last draw (logical x y w h): 1 */
const frg_client *txg_client(txg_t *g);  /* for a platform view (gui_platform.h) */
void txg_fit_size(uint32_t *w, uint32_t *h);   /* the nearest size the window takes (each side in its range) */
#endif
