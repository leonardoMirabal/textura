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
} txg_gui_shot_t;
#endif
