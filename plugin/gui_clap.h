/* SPDX-License-Identifier: GPL-3.0-only */
/* The plugin window as CLAP's gui extension (gui_clap.c). The plugin (plugin/src/textura_clap.c) makes one txg_clap_t
 * per instance and keeps it in txp_ui_gui_data; the window reaches the plugin core only through plugin/src/txp_ui.h.
 * (From Frodhes' plugin/gui_clap.h.) */
#ifndef TXG_GUI_CLAP_H
#define TXG_GUI_CLAP_H
#include <clap/clap.h>
#include "src/txp_ui.h"

typedef struct txg_clap txg_clap_t;
txg_clap_t *txg_clap_new(const clap_host_t *host, txp_core *pc);   /* main thread, in init */
void txg_clap_free(txg_clap_t *c);                                  /* main thread, in destroy */
const clap_plugin_gui_t *txg_clap_ext(void);   /* CLAP_EXT_GUI */
const void *txg_clap_shot_ext(void);           /* TXG_EXT_GUI_SHOT (gui_shot.h) */
/* the instance's saved window size changed (a state loaded): the next window opens at it; an open one asks the host to
 * resize (CLAP gui request_resize) */
void txg_clap_size_changed(txg_clap_t *c);
/* Linux (X11): the host's event loop drives the view: CLAP_EXT_POSIX_FD_SUPPORT on the display's socket and
 * CLAP_EXT_TIMER_SUPPORT (~33 ms); NULL on the other platforms (their views run their own) */
const clap_plugin_posix_fd_support_t *txg_clap_fd_ext(void);
const clap_plugin_timer_support_t *txg_clap_timer_ext(void);
#endif
