/* SPDX-License-Identifier: GPL-3.0-only */
/* The plugin window's layout size, in logical pixels (plugin/gui_core.c draws it at any scale; the platform views,
 * plugin/gui_mac.m, gui_win.c, gui_x11.c, keep its aspect, 5:3). The window resizes from TXG_MIN_W to TXG_MAX_W wide. */
#ifndef TXG_SIZE_H
#define TXG_SIZE_H
#define TXG_W 1200
#define TXG_H 720
#define TXG_MIN_W 840
#define TXG_MAX_W 2400
#endif
