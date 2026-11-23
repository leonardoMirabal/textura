/* SPDX-License-Identifier: GPL-3.0-only */
/* The plugin window's core: everything but the operating system (the platform views, plugin/gui_platform.h, own the
 * native view and its pixels and pass the input). It reads what the instance plays through plugin/src/txp_ui.h,
 * draws the whole window into an RGBA canvas with frui (plugin/ui: anti-aliased shapes, the font) and turns the mouse
 * into the app's parameters, mutes and solos. Main thread only.
 *
 * The window (at least TXG_W x TXG_H logical pixels, gui_size.h; any size and shape: one scale both ways, the
 * smaller of width / TXG_W and height / TXG_H, and the rest to the lanes, so the MAP stays round):
 *   top bar     TEXTURA, HELP (?: the tour, the manual), the scene loaded last with its previous / next (PRESET; a click on
 *               it: the presets browser),
 *               SAVE (into a user scene, named), UNDO, PLAY ALL / STOP ALL (the FM-1's REC), LANES, the output's
 *               peak, SETTINGS
 *   place       (left) MAP: the place seen from above, you at the centre of a half circle: each lane a dot, its angle
 *               the lane's PAN, its distance the card named DIST of its engine (an engine without one: the middle ring,
 *               a hollow dot); a dot glows with its lane's events; drag one to move it (PAN, DIST); lanes on one spot
 *               fan out and share a label. SPACE: the same place in perspective. At its head the selected lane. M/S:
 *               the mix's stereo picture (a half-circle scope, L R MID SIDE in dB, the correlation); it only shows.
 *   lanes       (right, scrolling) one row a lane in use: its number, the engine and family with ENGINE (the engine
 *               browser), its event line, HOW MANY (six blocks) and HOW ACTIVE (nine), PAN, its MIDI channel, VOL, M
 *               and S and a square ENGINE button; ADD LANE under the last. A click anywhere on a row selects its lane (its controls too).
 *   drawer      the selected lane: its engine (CHANGE ENGINE), every card (name, value, hint; DIST among them), PAN,
 *               LEVEL, GROOVE PART, CYCLE
 *   strip       COMP, DRIVE, FILTER, DELAY, REVERB, M/S (each a picture and its main values) and the movement (two sources);
 *               a section's header opens its drawer, which rises from the strip over the lane's drawer (the place
 *               and the rows stay, the rows scroll): a large live picture and every parameter of the effect in
 *               groups; MOVEMENT's has LFO 1 .. 8, ENV and FOLLOW, the matrix and MOD CC
 * Overlays: the engine browser, the presets browser, SETTINGS (MIDI IN with KEYS, SCALE, TIMING, LOADING, WINDOW,
 * ENGINE COLOURS), SAVE's panel, the colour picker, HELP's menu; the tour (the window dimmed but the part a step is about,
 * a card: SKIP, BACK, NEXT; the keys too). A value (TXG_W_PARAM): drag, wheel, double-click for its default, a
 * click steps a choice. Red only marks the selected lane, what is on and what an LFO moves.
 * The movement: a source's tile (LFO, ENV, FOLLOW) clicked: its mode (a red bar over the content): a drag on a control sets
 * how far the source moves it (a matrix slot: SOURCE, TARGET, DEPTH), its value untouched; outside the mode a moved
 * control is red, with its reach, its live place and a tag per link; a source's grip dropped on a control, the
 * right-click menu, CLEAR, CLEAR ALL MOVEMENT and UNDO do the same through the matrix's host parameters. Repaints: the whole window
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
       TXG_W_SELDOT, TXG_W_STEP, TXG_W_CMETER, TXG_W_TIP,   /* (TIP: a card whose hint was cut: hovered, all of it) */
       /* step 5c: the engine browser's PREVIEW (play / stop), its HOW MANY (lane 0) and HOW ACTIVE (1) steppers (v -1 or
        * +1); the effect drawer's area (never hit: for the tests) */
       TXG_W_PREVIEW, TXG_W_PVSTEP, TXG_W_FXD,
       /* step 5d: a row's square ENGINE button beside M and S (opens the engine browser, as TXG_W_SND does) */
       TXG_W_SWAP,    /* (no longer drawn: the row's one icon button is TXG_W_SND) */
       /* step 5f: a lane's STOP / PLAY, in its row (lane k) and in the drawer (the selected lane) */
       TXG_W_LRUN, TXG_W_DRUN,
       /* step 5g, the movement: a source's tile (lane: its matrix SOURCE, mod.c MS_*: LFO 1 .. 8 = 1 .. 8, ENV 1 .. 2 = 16,
        * 17, FOLLOW 1 .. 2 = 18, 19), its grip (dragged onto a control: a link), a control's tag (lane: the control's id,
        * v the source), the source mode's bar (never hit; lane: the source), its CLEAR and DONE, the right-click menu
        * (lane: its item, 0 .. 11 the sources in tile order, 12 Remove movement, -1 its panel), CLEAR ALL MOVEMENT, its
        * CLEAR and KEEP, the message line (never hit), a control the mode's source cannot move (never hit; lane: its id) */
       TXG_W_SRC, TXG_W_GRIP, TXG_W_TAG, TXG_W_MBAR, TXG_W_MCLEAR, TXG_W_MDONE, TXG_W_MENU, TXG_W_CLRALL, TXG_W_CLRYES,
       TXG_W_CLRNO, TXG_W_MSG, TXG_W_NOMOVE, TXG_W_MAMT,
       TXG_W_MSM,     /* (never hit: M/S's meters, lane 0, and its goniometer, lane 1, for the tests) */
       /* the dice: a row's (lane k: a random engine on it), the top bar's (RANDOM's list), the list's ticks (lane: the
        * group, 0 .. 3 ENGINES SOUND EFFECTS MOVEMENT) and ROLL; its CLOSE is TXG_W_CLOSE, its panel TXG_W_PICKER, -3;
        * the top bar's LEVEL (never hit: its label, value and bar, for the tests) */
       TXG_W_DICE, TXG_W_RND, TXG_W_RNDTICK, TXG_W_RNDGO, TXG_W_LVL,
       TXG_W_NAMEDICE,     /* (SAVE's dice beside the name: another name drawn) */
       /* HELP: the top bar's ? (its menu), the menu's items (lane 0 TAKE THE TOUR, 1 OPEN THE MANUAL; its panel
        * TXG_W_PICKER, -4); the tour: its card (lane: the step, 0 .. 7), the part of the window the step lights (never
        * hit; lane: the step), the card's SKIP, BACK, NEXT (lane 0 1 2); the logo (never hit: the tests' top bar) */
       TXG_W_HELP, TXG_W_RNDMENU, TXG_W_TOUR, TXG_W_TOURLIT, TXG_W_TOURBTN, TXG_W_LOGO,
       TXG_W_VMS };   /* (the place's third tab, M/S: the mix's stereo picture) */

typedef struct txg txg_t;
txg_t *txg_new(txp_core *c);             /* NULL: out of memory or the font did not load */
void txg_free(txg_t *g);
int txg_resize(txg_t *g, int pw, int ph);   /* its own canvas (headless): pw x ph pixels, any aspect; 0 ok */
float txg_scale_of(int pw, int ph);      /* canvas pixels a logical pixel for a pw x ph canvas (the views' mouse) */
int txg_tick(txg_t *g);                  /* reads what plays; 1 when a repaint is due */
int txg_paint(txg_t *g);                 /* its own canvas brought up to date; 1 when pixels changed */
const uint32_t *txg_pixels(const txg_t *g, int *w, int *h);   /* 0xFFRRGGBB */
int txg_input(txg_t *g, const frg_input *e);   /* bit 0: what is drawn may have changed; bit 1: the event was used */
int txg_find(const txg_t *g, int kind, int lane, float r[4]);
/* SAVE's panel as drawn: its user preset (0 .. 15), its name, the line under it (n bytes), every text inside its room
 * (fit 1); 0: not open */
int txg_save_info(txg_t *g, int *slot, char name[12], char *line, int n, int *fit);   /* a control of the last draw (logical x y w h): 1 */
const frg_client *txg_client(txg_t *g);  /* for a platform view (gui_platform.h) */
void txg_fit_size(uint32_t *w, uint32_t *h);   /* the nearest size the window takes (each side in its range) */
/* the tour: eight steps over the window, each lighting a part of it with a card (first: the window's first open ever,
 * gui_clap.c; its card says so); its flag's folder (SKIP or DONE writes the flag there; "": none, nothing written);
 * whether it starts by itself (1: no flag in dir and not a test run) */
void txg_tour_start(txg_t *g, int first);
void txg_tour_dir(txg_t *g, const char *dir);
int txg_first_run(const char *dir, int test);
/* the texts as drawn last, each inside its room (1): what 0 the top bar's (the preset box at its widest name and lane
 * count, the buttons' labels), 1 the tour's card, 3 the place's M/S tab (its half circle round inside the place, its
 * names, levels and correlation in their room), 4 the place's head (the selected lane's name, and the widest sound name
 * at its smallest size, beside the tabs) */
int txg_fits(const txg_t *g, int what);
#endif
