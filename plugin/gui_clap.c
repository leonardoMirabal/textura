/* SPDX-License-Identifier: GPL-3.0-only */
/* The plugin window as CLAP's gui extension; clap-wrapper shows the same window as the VST3 editor and the Audio Unit's
 * Cocoa view. Embedded only (no floating window). Sizes: logical points on macOS, physical pixels on Windows and X11
 * (CLAP's rule: there the host's set_scale gives the factor). (From Frodhes' plugin/gui_clap.c: Textura's window core,
 * plugin/gui_core.c, over plugin/src/txp_ui.h; no settings table, no update checker.) */
#include "gui_clap.h"
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "gui_core.h"
#include "gui_platform.h"
#include "gui_shot.h"

struct txg_clap {
    const clap_host_t *host;
    txp_core *pc;                    /* the plugin core (txp_ui.h) */
    txg_t *core;
    frg_view *view;
    uint32_t w, h;                   /* logical points */
    double scale;                    /* the host's (Windows, X11) */
    int px_api;                      /* sizes in physical pixels (win32, x11) */
    /* X11: the host's event loop */
    const clap_host_posix_fd_support_t *hfd;
    const clap_host_timer_support_t *htm;
    int fd;
    clap_id timer;
};

txg_clap_t *txg_clap_new(const clap_host_t *host, txp_core *pc)
{
    txg_clap_t *c = calloc(1, sizeof *c);
    if (!c) return 0;
    c->host = host;
    c->pc = pc;
    c->w = TXG_W; c->h = TXG_H;
    c->scale = 0.0;
    c->fd = -1;
    c->timer = CLAP_INVALID_ID;
    *txp_ui_gui_data(pc) = c;
    return c;
}
static void loop_release(txg_clap_t *c)
{
    if (c->fd >= 0 && c->hfd) c->hfd->unregister_fd(c->host, c->fd);
    if (c->timer != CLAP_INVALID_ID && c->htm) c->htm->unregister_timer(c->host, c->timer);
    c->fd = -1;
    c->timer = CLAP_INVALID_ID;
}
static void close_window(txg_clap_t *c)
{
    loop_release(c);
    if (c->view) { frg_view_destroy(c->view); c->view = 0; }
    if (c->core) { txg_free(c->core); c->core = 0; }
}
void txg_clap_free(txg_clap_t *c)
{
    if (!c) return;
    close_window(c);
    free(c);
}
static double px_scale(const txg_clap_t *c)
{
    if (!c->px_api) return 1.0;
    if (c->scale > 0.0) return c->scale;
    return c->view ? (double)frg_view_scale(c->view) : 1.0;
}
static void saved_size(txg_clap_t *c)       /* the size last set (saved with the project) */
{
    int w = txp_ui_win_w(c->pc), h = txp_ui_win_h(c->pc);
    uint32_t ww = (uint32_t)(w > 0 ? w : TXG_W), hh = (uint32_t)(h > 0 ? h : (int)ww * TXG_H / TXG_W);
    txg_fit_size(&ww, &hh);
    c->w = ww; c->h = hh;
}
static int phys(const txg_clap_t *c, uint32_t v) { return (int)lround(v * px_scale(c)); }
void txg_clap_size_changed(txg_clap_t *c)    /* a loaded state's size: the window asks its host for it */
{
    const clap_host_gui_t *hg;
    if (!c) return;
    saved_size(c);
    if (!c->view) return;
    hg = c->host->get_extension(c->host, CLAP_EXT_GUI);
    if (hg && hg->request_resize && hg->request_resize(c->host, (uint32_t)phys(c, c->w), (uint32_t)phys(c, c->h)))
        frg_view_resize(c->view, phys(c, c->w), phys(c, c->h));
}

/* ---- clap.gui */
#define GUI(plugin) ((txg_clap_t *)*txp_ui_gui_data(txp_core_of(plugin)))
static bool g_api_ok(const char *api, bool floating) { return !floating && api && !strcmp(api, FRG_VIEW_API); }
static bool g_is_api_supported(const clap_plugin_t *plugin, const char *api, bool is_floating)
{
    (void)plugin;
    return g_api_ok(api, is_floating);
}
static bool g_get_preferred_api(const clap_plugin_t *plugin, const char **api, bool *is_floating)
{
    (void)plugin;
    *api = FRG_VIEW_API;
    *is_floating = false;
    return true;
}
static bool g_create(const clap_plugin_t *plugin, const char *api, bool is_floating)
{
    txg_clap_t *c = GUI(plugin);
    if (!c || !g_api_ok(api, is_floating) || c->core) return false;
    c->px_api = strcmp(api, CLAP_WINDOW_API_COCOA) != 0;
    saved_size(c);
    c->core = txg_new(c->pc);
    if (!c->core) return false;
    c->view = frg_view_create(txg_client(c->core), 1, 1);
    if (!c->view) { close_window(c); return false; }
    if (c->scale > 0.0) frg_view_set_scale(c->view, (float)c->scale);
    frg_view_resize(c->view, phys(c, c->w), phys(c, c->h));
    return true;
}
static void g_destroy(const clap_plugin_t *plugin)
{
    txg_clap_t *c = GUI(plugin);
    if (c) close_window(c);
}
static bool g_set_scale(const clap_plugin_t *plugin, double scale)
{
    txg_clap_t *c = GUI(plugin);
    if (!c || !(scale > 0.0)) return false;
    if (!c->px_api) return false;    /* macOS: the window's backing scale, read by the view itself */
    c->scale = scale;
    if (c->view) frg_view_set_scale(c->view, (float)scale);
    return true;
}
static bool g_get_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height)
{
    txg_clap_t *c = GUI(plugin);
    if (!c) return false;
    *width = (uint32_t)phys(c, c->w);
    *height = (uint32_t)phys(c, c->h);
    return true;
}
static bool g_can_resize(const clap_plugin_t *plugin) { (void)plugin; return true; }
static bool g_get_resize_hints(const clap_plugin_t *plugin, clap_gui_resize_hints_t *hints)
{
    (void)plugin;
    hints->can_resize_horizontally = true;
    hints->can_resize_vertically = true;
    hints->preserve_aspect_ratio = false;   /* (any aspect: the place stays round, the rows take the rest) */
    hints->aspect_ratio_width = TXG_W;
    hints->aspect_ratio_height = TXG_H;
    return true;
}
static bool g_adjust_size(const clap_plugin_t *plugin, uint32_t *width, uint32_t *height)
{
    txg_clap_t *c = GUI(plugin);
    double k = c ? px_scale(c) : 1.0;
    uint32_t w = (uint32_t)lround(*width / k), h = (uint32_t)lround(*height / k);
    txg_fit_size(&w, &h);
    *width = (uint32_t)lround(w * k);
    *height = (uint32_t)lround(h * k);
    return true;
}
static bool g_set_size(const clap_plugin_t *plugin, uint32_t width, uint32_t height)
{
    txg_clap_t *c = GUI(plugin);
    double k;
    uint32_t w, h;
    if (!c) return false;
    k = px_scale(c);
    w = (uint32_t)lround(width / k); h = (uint32_t)lround(height / k);
    txg_fit_size(&w, &h);
    c->w = w; c->h = h;
    txp_ui_win_w_set(c->pc, w == TXG_W && h == TXG_H ? 0 : (int)w);
    txp_ui_win_h_set(c->pc, w == TXG_W && h == TXG_H ? 0 : (int)h);
    if (c->view) frg_view_resize(c->view, (int)width, (int)height);
    return true;
}
static bool g_set_parent(const clap_plugin_t *plugin, const clap_window_t *window)
{
    txg_clap_t *c = GUI(plugin);
    void *parent;
    if (!c || !c->view || !window) return false;
    parent = !strcmp(FRG_VIEW_API, CLAP_WINDOW_API_X11) ? (void *)(uintptr_t)window->x11
           : !strcmp(FRG_VIEW_API, CLAP_WINDOW_API_WIN32) ? window->win32 : window->cocoa;
    if (frg_view_set_parent(c->view, parent)) return false;
    if (frg_view_poll_fd(c->view) >= 0 && c->fd < 0) {   /* X11: the host's loop watches the display */
        c->hfd = c->host->get_extension(c->host, CLAP_EXT_POSIX_FD_SUPPORT);
        c->htm = c->host->get_extension(c->host, CLAP_EXT_TIMER_SUPPORT);
        if (c->hfd && c->hfd->register_fd(c->host, frg_view_poll_fd(c->view), CLAP_POSIX_FD_READ))
            c->fd = frg_view_poll_fd(c->view);
        if (c->htm && !c->htm->register_timer(c->host, 33, &c->timer)) c->timer = CLAP_INVALID_ID;
    }
    return true;
}
static bool g_set_transient(const clap_plugin_t *plugin, const clap_window_t *window)
{
    (void)plugin; (void)window;
    return false;
}
static void g_suggest_title(const clap_plugin_t *plugin, const char *title) { (void)plugin; (void)title; }
static bool g_show(const clap_plugin_t *plugin)
{
    txg_clap_t *c = GUI(plugin);
    if (!c || !c->view) return false;
    frg_view_show(c->view, 1);
    return true;
}
static bool g_hide(const clap_plugin_t *plugin)
{
    txg_clap_t *c = GUI(plugin);
    if (!c || !c->view) return false;
    frg_view_show(c->view, 0);
    return true;
}
static const clap_plugin_gui_t EXT_GUI = {
    g_is_api_supported, g_get_preferred_api, g_create, g_destroy, g_set_scale, g_get_size, g_can_resize,
    g_get_resize_hints, g_adjust_size, g_set_size, g_set_parent, g_set_transient, g_suggest_title, g_show, g_hide,
};
const clap_plugin_gui_t *txg_clap_ext(void) { return &EXT_GUI; }

/* ---- X11: the host's loop */
static void on_fd(const clap_plugin_t *plugin, int fd, clap_posix_fd_flags_t flags)
{
    txg_clap_t *c = GUI(plugin);
    (void)fd; (void)flags;
    if (c && c->view) frg_view_poll(c->view);
}
static void on_timer(const clap_plugin_t *plugin, clap_id id)
{
    txg_clap_t *c = GUI(plugin);
    if (c && c->view && id == c->timer) frg_view_poll(c->view);
}
static const clap_plugin_posix_fd_support_t EXT_FD = { on_fd };
static const clap_plugin_timer_support_t EXT_TIMER = { on_timer };
const clap_plugin_posix_fd_support_t *txg_clap_fd_ext(void)
{
    return strcmp(FRG_VIEW_API, CLAP_WINDOW_API_X11) ? 0 : &EXT_FD;
}
const clap_plugin_timer_support_t *txg_clap_timer_ext(void)
{
    return strcmp(FRG_VIEW_API, CLAP_WINDOW_API_X11) ? 0 : &EXT_TIMER;
}

/* ---- the tests' extension (gui_shot.h) */
static bool s_tick(const clap_plugin_t *plugin)
{
    txg_clap_t *c = GUI(plugin);
    if (!c || !c->core) return false;
    if (c->view) {                   /* (the view paints the core) */
        frg_view_repaint(c->view);
        frg_view_poll(c->view);
        return true;
    }
    (void)txg_tick(c->core);
    (void)txg_paint(c->core);
    return true;
}
static bool s_grab(const clap_plugin_t *plugin, int view, const uint32_t **px, int *w, int *h)
{
    txg_clap_t *c = GUI(plugin);
    if (!c || !c->core) return false;
    *px = view && c->view ? frg_view_grab(c->view, w, h) : txg_pixels(c->core, w, h);
    return *px != 0;
}
static int s_input(const clap_plugin_t *plugin, const frg_input *e)
{
    txg_clap_t *c = GUI(plugin);
    if (!c || !c->core) return -1;
    return txg_input(c->core, e);
}
static bool s_headless(const clap_plugin_t *plugin, int pw, int ph)
{
    txg_clap_t *c = GUI(plugin);
    if (!c || c->core) return false;
    c->core = txg_new(c->pc);
    return c->core && txg_resize(c->core, pw, ph) == 0;
}
static bool s_find(const clap_plugin_t *plugin, int kind, int lane, float r[4])
{
    txg_clap_t *c = GUI(plugin);
    return c && c->core && txg_find(c->core, kind, lane, r);
}
static bool s_masks(const clap_plugin_t *plugin, uint32_t *mute, uint32_t *solo)
{
    txg_clap_t *c = GUI(plugin);
    if (!c) return false;
    txp_ui_mutes(c->pc, mute, solo);
    return true;
}
static bool s_lane(const clap_plugin_t *plugin, int k, int *fam, int *snd)
{
    txg_clap_t *c = GUI(plugin);
    txp_snap_t sn;
    if (!c || k < 0 || k >= (int)TXP_LANES || txp_ui_snap(c->pc, &sn) || !sn.ui.lane[k].used) return false;
    *fam = sn.ui.lane[k].fi; *snd = sn.ui.lane[k].si;
    return true;
}
static int s_sel(const clap_plugin_t *plugin)
{
    txg_clap_t *c = GUI(plugin);
    txp_snap_t sn;
    return !c || txp_ui_snap(c->pc, &sn) ? -1 : (int)sn.ui.sel;
}
static bool s_comp(const clap_plugin_t *plugin, int *in10, int *out10, int *gr10)
{
    txg_clap_t *c = GUI(plugin);
    txp_snap_t sn;
    if (!c || txp_ui_snap(c->pc, &sn)) return false;
    *in10 = sn.ui.comp_in10; *out10 = sn.ui.comp_out10; *gr10 = sn.ui.comp_gr10;
    return true;
}
static int s_run(const clap_plugin_t *plugin, int k)
{
    txg_clap_t *c = GUI(plugin);
    txp_snap_t sn;
    return !c || k < 0 || k >= (int)TXP_LANES || txp_ui_snap(c->pc, &sn) ? -1 : sn.ui.lane[k].used ? sn.ui.lane[k].cnt : 0;
}
static bool s_ms(const clap_plugin_t *plugin, int db10[4], int *corr, uint32_t *gon_blk)
{
    txg_clap_t *c = GUI(plugin);
    txp_snap_t sn;
    if (!c || txp_ui_snap(c->pc, &sn)) return false;
    for (int i = 0; i < 4; i++) db10[i] = sn.ui.ms_db10[i];
    *corr = sn.ui.ms_corr; *gon_blk = sn.ui.gon_blk;
    return true;
}
static const txg_gui_shot_t EXT_SHOT = { s_tick, s_grab, s_input, s_headless, s_find, s_masks, s_lane, s_sel, s_comp, s_run, s_ms };
const void *txg_clap_shot_ext(void) { return &EXT_SHOT; }
