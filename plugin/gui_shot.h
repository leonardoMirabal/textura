/* SPDX-License-Identifier: GPL-3.0-only */
/* A private CLAP extension for the window's tests (plugin/tests/clap_render.c --shot): what the open window shows, input
 * into it, and a window without a native view for headless runs. Hosts never ask for it; the plugin offers it only with
 * TEXTURA_PLUGIN_GUI_SHOT set in its environment. (From Frodhes' plugin/gui_shot.h: Textura's id, no model.) */
#ifndef TXG_GUI_SHOT_H
#define TXG_GUI_SHOT_H
#include <stdbool.h>
#include <stdint.h>
#include <clap/clap.h>

#include "gui_platform.h"

#define TXG_EXT_GUI_SHOT "io.github.leonardomirabal.textura.gui-shot/1"
typedef struct {
    /* paints what changed now (as the view's timer does; the native view too, when there is one) */
    bool (*tick)(const clap_plugin_t *plugin);
    /* the canvas as last painted; view: 1 = read back from the native view's surface instead (what the window shows),
     * when there is one */
    bool (*grab)(const clap_plugin_t *plugin, int view, const uint32_t **px, int *w, int *h);
    /* input, as the native view passes it (gui_platform.h, logical coordinates): txg_input's result, -1 no window */
    int (*input)(const clap_plugin_t *plugin, const frg_input *e);
    /* a window core without a native view (no gui create needed), its canvas pw x ph pixels */
    bool (*headless)(const clap_plugin_t *plugin, int pw, int ph);
    /* a control of the last draw (gui_core.h: kind TXG_W_*, lane), its rectangle in logical points */
    bool (*find)(const clap_plugin_t *plugin, int kind, int lane, float r[4]);
    /* the lanes muted and soloed as the window shows them (bit k: lane k + 1) */
    bool (*masks)(const clap_plugin_t *plugin, uint32_t *mute, uint32_t *solo);
    /* lane k's sound as the app shows it: its family and its sound there (false: the lane is empty) */
    bool (*lane)(const clap_plugin_t *plugin, int k, int *fam, int *snd);
    /* the app's selected lane as it published it last (0 .. 15; -1 none) */
    int (*sel)(const clap_plugin_t *plugin);
    /* the compressor's meters as the app published them last: its input and output levels (dBFS x 10, -900 silence)
     * and its gain reduction (dB x 10) */
    bool (*comp)(const clap_plugin_t *plugin, int *in10, int *out10, int *gr10);
    /* lane k's HOW MANY as it plays now (0: stopped or empty; the sound browser's preview too), as the app published
     * it last (-1: no window) */
    int (*run)(const clap_plugin_t *plugin, int k);
    /* the M/S's meters as the app published them last: L R M S peaks (dBFS x 10, -900 silence), the correlation (Q15)
     * and the goniometer's block count (a new block of points: another count) */
    bool (*ms)(const clap_plugin_t *plugin, int db10[4], int *corr, uint32_t *gon_blk);
} txg_gui_shot_t;
#endif
