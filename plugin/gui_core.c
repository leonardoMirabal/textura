/* SPDX-License-Identifier: GPL-3.0-only */
/* The plugin window's core (gui_core.h). Float is allowed in plugin/ (the app itself stays integer). */
#include "gui_core.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "card_hints.h"
#include "ui/frui_font.h"
#include "ui/frui_gfx.h"

extern const unsigned char txg_font_text[];   /* Inter Tight (SIL OFL 1.1), embedded at build time (cmake/gui.cmake) */
extern const unsigned long txg_font_text_len;

/* the look: dark, nearly monochrome; red for what is selected, on, or moved by an LFO */
#define C_PANEL 0xFF161515u
#define C_WELL 0xFF0A0909u
#define C_LINE 0xFF262424u
#define C_INK 0xFFEBE6E1u
#define C_DIM 0xFF85807Bu
#define C_FAINT 0xFF3A3736u
#define C_ENV 0xFF242221u
#define C_RED 0xFFEF4A3Cu
#define C_BTN 0xFF1F1D1Du
#define C_SEL 0xFF1D1A1Au
#define C_SEG 0xFF262323u
#define C_OVL 0xFF121111u
#define C_HOV 0xFF1A1818u
#define C_NAV 0xFF1B1919u
#define C_DRAW 0xFF131212u

/* the engines' colours (txp_ui.h TXP_COL_N; 0 bone, every engine's at the start). No red: red is the selection's and
 * the LFOs' */
static const uint32_t PAL[TXP_COL_N] = { C_INK, 0xFFE07A4Au, 0xFFD9A650u, 0xFFCBB48Au, 0xFF9AA86Au, 0xFF8FB39Cu,
                                         0xFF5FA3A0u, 0xFF9CC7D6u, 0xFF7F93C4u, 0xFFA993C7u, 0xFFC98A9Cu };
static const char *const PAL_N[TXP_COL_N] = { "BONE", "EMBER", "AMBER", "SAND", "MOSS", "SAGE", "TEAL", "ICE", "SLATE",
                                              "LILAC", "ROSE" };

#define NBLK 1034u                       /* the event lines' span: 6 s of app blocks (256 frames at 44.1 kHz) */
#define GLOW_BLK 26u                     /* a dot's glow: the last 0.15 s */
/* the layout (logical pixels; the window is at least TXG_W x TXG_H of them, any aspect: g->W x g->H) */
#define TOP_H 52.f                       /* the top bar */
#define PH_H 46.f                        /* the place's head: MAP | SPACE, the selected lane */
#define HDR_H 26.f                       /* the rows' column names */
#define ROW_H 40.f                       /* a lane's row */
#define DR_H 136.f                       /* the drawer: the selected lane's cards */
#define FX_H 148.f                       /* the strip: the effects in signal order, the movement */
#define MAP_ANG 1.3962634f               /* PAN at either end: 80 degrees off the middle */
#define MAP_R0 0.12f                     /* DIST 0's distance (the nearest ring: 0.25) */
#define MAP_RNONE 0.5f                   /* a sound without DIST: the middle ring */
#define MAP_CROWD 8                      /* more lanes than this on the map: a label names only the selected or hovered */
#define SP_NEAR 0.36f                    /* SPACE: the ground's half width at the outer ring's side: near (+ far) x the
                                          * place's width */
#define SP_FAR 0.14f
/* the overlays (the browsers, SETTINGS): a header, then a nav, a list, a preview */
#define OV_HDR 46.f
#define OV_Y0 (TOP_H + OV_HDR)
#define OV_NAV 190.f
#define OV_PREV 320.f
#define OV_RH 28.f
#define LFO_H 48                         /* the LFOs' traces: the last 48 ticks */
#define MAX_W 900
#define LIST_MAX 128

enum { OV_NONE, OV_SET, OV_SND, OV_PRE, OV_FX, OV_SAVE };
enum { PB_USER = 100, PB_LAND = 101 };   /* the presets browser's banks past the factory's categories */
/* the strip's sections, in signal order (the expanded view's tabs: these, LFO 1 .. 8, ENV and FOLLOW, the matrix) */
enum { SEC_COMP, SEC_DRIVE, SEC_FILTER, SEC_DELAY, SEC_REVERB, SEC_MOVE, SEC_N };
enum { TAB_LFO0 = SEC_MOVE, TAB_ENV = TAB_LFO0 + 8, TAB_MAT, TAB_N };
static const char *const SEC_MOD[5] = { "Compressor", "Drive", "Filter", "Grain delay", "Reverb" };
static const char *const SEC_NAME[SEC_N] = { "COMP", "DRIVE", "FILTER", "DELAY", "REVERB", "MOVEMENT" };
static const float SEC_WT[SEC_N] = { 1.f, 1.f, 1.6f, 1.f, 1.f, 1.3f };
/* the effects' ids (params.c: the globals' scope 4) */
#define G_ID(i) TXP_ID_GLOB(i)
#define ID_GMIX G_ID(0)
#define ID_GTIME G_ID(1)
#define ID_GFB G_ID(2)
#define ID_RTYPE G_ID(16)
#define ID_RMIX G_ID(17)
#define ID_RDECAY G_ID(18)
#define ID_CON G_ID(26)
#define ID_CTHR G_ID(27)
#define ID_CRATIO G_ID(30)
#define ID_DAMT G_ID(48)
#define ID_DTONE G_ID(49)
#define ID_DMIX G_ID(50)
#define ID_DTYPE G_ID(51)
#define ID_FHP G_ID(52)
#define ID_FLP G_ID(53)
#define ID_FRES G_ID(54)
#define ID_FSLOPE G_ID(55)
#define ID_LFO(n, p) (5u << 8 | (uint32_t)(n) * 16u | (uint32_t)(p))
#define ID_MAT(s, q) (7u << 8 | (uint32_t)(s) * 4u | (uint32_t)(q))
enum { LP_SHAPE, LP_RATE, LP_SYNC, LP_MODE, LP_PHASE, LP_WIDTH, LP_JITTER, LP_FADE, LP_RESTART, LP_LEVEL };

typedef struct { int kind, lane; uint32_t id; int v; float r[4]; } wid_t;

struct txg {
    txp_core *c;
    frui_font *font;
    const txp_catalog_t *cat;
    uint32_t *px;                        /* the headless canvas (txg_resize) */
    int pw, ph;
    const uint32_t *last_px;             /* the buffer painted last: another one, a whole repaint */
    int last_w, last_h;
    float S;                             /* canvas pixels a logical pixel */
    /* the layout now (lay): the logical size, the place's width, the drawer's and the strip's tops, the map, SPACE */
    float W, H, PW, dr_y, fx_y, mcx, mcy, mr, sp_y0, sp_y1, sp_hz;
    int rtop, rvis;                      /* the rows: the first shown, how many fit */
    /* what is drawn */
    txp_snap_t sn;
    int nlane, sel, sel_seen, ov, view, fx_tab;
    uint32_t mute, solo;
    int level[TXP_LANES], pan[TXP_LANES], dist[TXP_LANES], cnt[TXP_LANES], act[TXP_LANES], col[TXP_LANES];
    uint8_t ev[NBLK][TXP_LANES], pk[NBLK][TXP_LANES];
    uint32_t actn, actn_drawn, sig, sig_drawn;
    int16_t lfoh[TXP_LFO_N][LFO_H];      /* the LFOs' values, a tick each (the newest last) */
    int lfo_seen;
    int drawn;                           /* the canvas holds a whole draw */
    /* the colour picker (over anything): the engine it colours, -1 closed; where it opens */
    int pick, pick_x, pick_y;
    /* the sound browser: its lane, the family shown, the cursor (family, sound), the lane's kept sound, the search, an
     * audition waiting (ticks) */
    int sb_lane, sb_fam, sb_cf, sb_ci, sb_kf, sb_ki, sb_pend, sb_top;
    /* the presets browser: the bank shown (a category, PB_USER, PB_LAND), the cursor (a scene, TXP_LOAD_LS + n) */
    int pb_bank, pb_cur, pb_top;
    char q[24];                          /* a browser's search (typed) */
    int items[LIST_MAX], nitems;         /* the list as drawn last */
    /* SAVE: the user scene chosen, its name (typed), the save asked (ticks it waits to show it done) */
    int sv_slot, sv_wait;
    char sv_name[12];
    float sv_x;                          /* (where SAVE is in the top bar: the panel opens under it) */
    /* input */
    int drag, drag_lane, drag_dist;      /* drag: 0 none, TXG_W_DOT, TXG_W_VOL, TXG_W_PARAM, TXG_W_MANY, ... */
    float drag_dx, drag_dy;              /* (SPACE: the pointer off the dot when it was taken) */
    uint32_t drag_id;                    /* TXG_W_PARAM: the parameter; its value at the press, the press, moved */
    int drag_v0, drag_begun, drag_moved, drag_fine;
    float drag_x0, drag_y0, drag_rx, drag_rw;
    int hover_kind, hover_lane;
    wid_t w[MAX_W];
    int nw;
    frg_client cl;
};

/* ---- drawing helpers (logical coordinates) */
static void wid_add2(txg_t *g, int kind, int lane, uint32_t id, int v, float x, float y, float w, float h)
{
    if (g->nw >= MAX_W) return;
    g->w[g->nw].kind = kind; g->w[g->nw].lane = lane; g->w[g->nw].id = id; g->w[g->nw].v = v;
    g->w[g->nw].r[0] = x; g->w[g->nw].r[1] = y; g->w[g->nw].r[2] = w; g->w[g->nw].r[3] = h;
    g->nw++;
}
static void wid_add(txg_t *g, int kind, int lane, float x, float y, float w, float h) { wid_add2(g, kind, lane, 0, 0, x, y, w, h); }
static void R(txg_t *g, frui_canvas *c, float x, float y, float w, float h, uint32_t col)
{
    frui_rect(c, x * g->S, y * g->S, w * g->S, h * g->S, col);
}
static void RR(txg_t *g, frui_canvas *c, float x, float y, float w, float h, float r, uint32_t col)
{
    frui_rrect(c, x * g->S, y * g->S, w * g->S, h * g->S, r * g->S, col);
}
static void RRS(txg_t *g, frui_canvas *c, float x, float y, float w, float h, float r, uint32_t col)
{
    frui_rrect_stroke(c, x * g->S, y * g->S, w * g->S, h * g->S, r * g->S, g->S < 1.5f ? 1.f : g->S * 0.75f, col);
}
static void LN(txg_t *g, frui_canvas *c, float x0, float y0, float x1, float y1, float lw, uint32_t col)
{
    frui_line(c, x0 * g->S, y0 * g->S, x1 * g->S, y1 * g->S, lw * g->S, col);
}
static void CI(txg_t *g, frui_canvas *c, float x, float y, float r, uint32_t col) { frui_circle(c, x * g->S, y * g->S, r * g->S, col); }
static void RI(txg_t *g, frui_canvas *c, float x, float y, float r, float lw, uint32_t col)
{
    frui_ring(c, x * g->S, y * g->S, r * g->S, lw * g->S, col);
}
static frui_style ST(txg_t *g, float size, float wt, float track, int fl)
{
    frui_style s;
    s.size = size * g->S; s.weight = wt; s.track = track * g->S; s.flags = fl;
    return s;
}
static float TX(txg_t *g, frui_canvas *c, float x, float y, float size, float wt, float track, int fl, uint32_t col,
                const char *s)
{
    frui_style st = ST(g, size, wt, track, fl);
    return frui_text(c, g->font, &st, x * g->S, y * g->S, s, col) / g->S;
}
static float TW(txg_t *g, float size, float wt, float track, int fl, const char *s)
{
    frui_style st = ST(g, size, wt, track, fl);
    return frui_text_width(g->font, &st, s) / g->S;
}
static float CAP(txg_t *g, float size) { return frui_font_ascent(g->font, size * g->S) / g->S; }
/* a small label: capitals, tracked (the mockup's display face, here the one font) */
static float LBL(txg_t *g, frui_canvas *c, float x, float y, float size, uint32_t col, const char *s)
{
    return TX(g, c, x, y, size, 620.f, size * 0.14f, 0, col, s);
}
static float LBLW(txg_t *g, float size, const char *s) { return TW(g, size, 620.f, size * 0.14f, 0, s); }
static void clipL(txg_t *g, frui_canvas *c, float x, float y, float w, float h) { frui_clip(c, x * g->S, y * g->S, w * g->S, h * g->S); }
static float lw1(const txg_t *g) { return g->S < 1.5f ? 1.f / g->S : 0.75f; }   /* a hairline */
static void upper(char *d, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n && s[i]; i++) d[i] = s[i] >= 'a' && s[i] <= 'z' ? (char)(s[i] - 32) : s[i];
    d[i] = 0;
}
/* a button: a label centred in a box (pri: the primary look, ink on the panel's dark) */
static void button(txg_t *g, frui_canvas *c, float x, float y, float w, float h, const char *t, int pri)
{
    RR(g, c, x, y, w, h, 3.f, pri ? C_INK : C_BTN);
    RRS(g, c, x, y, w, h, 3.f, pri ? C_INK : C_LINE);
    LBL(g, c, x + (w - LBLW(g, 11.f, t)) * 0.5f, y + h * 0.5f + CAP(g, 11.f) * 0.5f, 11.f, pri ? C_PANEL : C_INK, t);
}
static void dashed(txg_t *g, frui_canvas *c, float x, float y0, float y1, uint32_t col)   /* a dashed upright line */
{
    for (float y = y0; y < y1; y += 5.f) LN(g, c, x, y, x, fminf(y + 2.5f, y1), 1.f, col);
}

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

/* ---- the layout: the place at the left (round at any size), the rows beside it, the drawer and the strip under both */
static void lay(txg_t *g)
{
    float top = TOP_H + PH_H, bot, room;
    g->PW = floorf(g->W * 0.3667f);
    g->PW = g->PW < 400.f ? 400.f : g->PW > 640.f ? 640.f : g->PW;
    g->fx_y = g->H - FX_H;
    g->dr_y = g->fx_y - DR_H;
    bot = g->dr_y;
    room = bot - top - 52.f;             /* (FAR over the outer ring, YOU under you) */
    g->mr = fminf((g->PW - 72.f) * 0.5f, room);
    g->mcx = g->PW * 0.5f;
    g->mcy = top + 20.f + g->mr + (room - g->mr) * 0.5f;
    g->sp_y0 = top + 14.f;
    g->sp_y1 = bot - 18.f;
    g->sp_hz = g->sp_y0 + (g->sp_y1 - g->sp_y0) * 0.1f;
    g->rvis = (int)floorf((g->dr_y - TOP_H - HDR_H) / ROW_H);
    if (g->rvis < 1) g->rvis = 1;
}
/* a row's columns (x from the window's left): the name, the event line, MANY, ACTIVE, PAN, CH, VOL, M S */
typedef struct { float num, dot, name, tl, tlw, many, act, pan, ch, vol, ms; } rcol_t;
#define RW_NAME 140.f
#define RW_STEP 84.f
#define RW_PAN 40.f
#define RW_CH 34.f
#define RW_VOL 92.f
#define RW_MS 48.f
#define R_GAP 10.f
static rcol_t rcols(const txg_t *g)
{
    rcol_t r;
    float x0 = g->PW, right = g->W - 14.f;
    r.num = x0 + 14.f; r.dot = x0 + 36.f; r.name = x0 + 46.f;
    r.tl = x0 + 16.f + RW_NAME + R_GAP;
    r.ms = right - RW_MS;
    r.vol = r.ms - R_GAP - RW_VOL;
    r.ch = r.vol - R_GAP - RW_CH;
    r.pan = r.ch - R_GAP - RW_PAN;
    r.act = r.pan - R_GAP - RW_STEP;
    r.many = r.act - R_GAP - RW_STEP;
    r.tlw = r.many - R_GAP - r.tl;
    if (r.tlw < 40.f) r.tlw = 40.f;
    return r;
}
static int used(const txg_t *g, int k) { return k < g->nlane && g->sn.ui.lane[k].used; }
static int faded(const txg_t *g, int k)   /* muted, or another lane soloed */
{
    uint32_t live = g->solo & ((1u << g->nlane) - 1u);
    return (g->mute >> k & 1u) || (live && !(live >> k & 1u));
}
static uint32_t eng_col(const txg_t *g, int eng) { int i = eng >= 0 && eng < (int)TXP_ENG_MAX ? txp_ui_col(g->c, eng) : 0; return PAL[i >= 0 && i < TXP_COL_N ? i : 0]; }
static uint32_t lane_col(const txg_t *g, int k) { return PAL[g->col[k] >= 0 && g->col[k] < TXP_COL_N ? g->col[k] : 0]; }
static void map_pos(const txg_t *g, int pan, int dist, float *x, float *y)
{
    float a = (float)(pan - 64) / 64.f * MAP_ANG, r = dist < 0 ? MAP_RNONE : MAP_R0 + (1.f - MAP_R0) * (float)dist / 127.f;
    *x = g->mcx + sinf(a) * r * g->mr;
    *y = g->mcy - cosf(a) * r * g->mr;
}
static void map_inv(const txg_t *g, float x, float y, int *pan, int *dist)
{
    float dx = (x - g->mcx) / g->mr, dy = (g->mcy - y) / g->mr, a, r;
    if (dy < 0.001f) dy = 0.001f;
    a = atan2f(dx, dy);
    r = sqrtf(dx * dx + dy * dy);
    *pan = (int)lroundf(64.f + a / MAP_ANG * 64.f);
    *pan = *pan < 0 ? 0 : *pan > 127 ? 127 : *pan;
    *dist = (int)lroundf((r - MAP_R0) / (1.f - MAP_R0) * 127.f);
    *dist = *dist < 0 ? 0 : *dist > 127 ? 127 : *dist;
}
/* SPACE: PAN and DIST onto the ground (sc: the size there, 0.3 far .. 1.3 near), and back (any point of the panel onto
 * the ground: a drag follows the pointer, the far edge at the horizon, the near at the bottom) */
static void sp_pos(const txg_t *g, int pan, int dist, float *x, float *y, float *sc)
{
    float a = (float)(pan - 64) / 64.f * MAP_ANG, r = dist < 0 ? MAP_RNONE : MAP_R0 + (1.f - MAP_R0) * (float)dist / 127.f,
          depth = 1.f - cosf(a) * r;
    depth = depth < 0.f ? 0.f : depth;
    *y = g->sp_hz + (g->sp_y1 - g->sp_hz) * powf(0.06f + depth * 0.9f, 1.7f);
    *x = g->mcx + sinf(a) * r * g->PW * (SP_FAR + SP_NEAR * depth);
    if (sc) *sc = 0.3f + depth;
}
static void sp_inv(const txg_t *g, float x, float y, int fixed, int *pan, int *dist)   /* fixed: no DIST (PAN only) */
{
    float t = (y - g->sp_hz) / (g->sp_y1 - g->sp_hz), depth, ahead, side, a, r;
    t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
    depth = (powf(t, 1.f / 1.7f) - 0.06f) / 0.9f;
    depth = depth < 0.f ? 0.f : depth > 1.f ? 1.f : depth;
    ahead = 1.f - depth;
    side = (x - g->mcx) / (g->PW * (SP_FAR + SP_NEAR * depth));
    if (ahead < 0.001f) ahead = 0.001f;
    a = atan2f(side, ahead);
    r = sqrtf(side * side + ahead * ahead);
    *pan = (int)lroundf(64.f + a / MAP_ANG * 64.f);
    *pan = *pan < 0 ? 0 : *pan > 127 ? 127 : *pan;
    *dist = fixed ? -1 : (int)lroundf((r - MAP_R0) / (1.f - MAP_R0) * 127.f);
    if (!fixed) *dist = *dist < 0 ? 0 : *dist > 127 ? 127 : *dist;
}
static float glow(const txg_t *g, int k)  /* the lane's recent events, 0 .. 1 */
{
    float s = 0.f;
    for (uint32_t b = NBLK - GLOW_BLK; b < NBLK; b++) if (g->ev[b][k]) s += (float)g->pk[b][k] / 255.f;
    s /= 5.f;
    return s > 1.f ? 1.f : s;
}
static void cap_name(char *d, const char *s, size_t n)   /* "campfire" -> "Campfire" */
{
    snprintf(d, n, "%s", s);
    if (d[0] >= 'a' && d[0] <= 'z') d[0] = (char)(d[0] - 32);
}
static void ptext(uint32_t id, int v, char *out, uint32_t n)   /* a host parameter's text (the host's own) */
{
    int i = txp_param_index(id);
    if (id == TXP_ID_MODCC) { snprintf(out, n, "CC %d", v); return; }
    if (i < 0 || txp_param_text((uint32_t)i, v, out, n)) snprintf(out, n, "%d", v);
}
static const txp_param_t *pinfo(uint32_t id)
{
    uint32_t n;
    const txp_param_t *t = txp_params(&n);
    int i = txp_param_index(id);
    return i < 0 ? 0 : &t[i];
}
/* a value's text as the window shows it: the app's own (its page's: "4.2 kHz", a card in its unit), or, while the
 * window's own write waits for the app, the host's text of it */
static void vtext(const txg_t *g, uint32_t id, char *out, uint32_t n)
{
    int i = txp_param_index(id), v = txp_ui_get(g->c, id);
    if (i < 0) { out[0] = 0; return; }
    if (v == g->sn.pv[i] && g->sn.ptx[i][0]) { snprintf(out, n, "%s", g->sn.ptx[i]); return; }
    ptext(id, v, out, n);
}
static const char *snd_name(const txg_t *g, int f, int i)
{
    return f >= 0 && f < g->cat->nfam && i >= 0 && i < g->cat->fam[f].n ? g->cat->fam[f].s[i].name : "";
}
static int snd_eng(const txg_t *g, int f, int i)
{
    return f >= 0 && f < g->cat->nfam && i >= 0 && i < g->cat->fam[f].n ? g->cat->fam[f].s[i].eng : -1;
}
static int has_q(const char *s, const char *q)   /* q in s, any case */
{
    size_t n = strlen(q);
    if (!n) return 1;
    for (; *s; s++) {
        size_t i;
        for (i = 0; i < n && s[i]; i++) {
            char a = s[i], b = q[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (i == n) return 1;
    }
    return 0;
}
static int lane_of_id(uint32_t id)       /* the lane of a lane's parameter, -1 none (txp_ui.h TXP_ID_LANE) */
{
    uint32_t s = id >> 8;
    if (id >= TXP_ID_OWN0) return -1;
    return s < 4u ? (int)s : s >= 16u && s < 28u ? (int)s - 12 : -1;
}
/* a value the modulation moves now (red, "~"), and where it has it (0 .. hi; -1: not moved) */
static int pmod(const txg_t *g, uint32_t id, int *live)
{
    int k = lane_of_id(id), i = (int)(id & 0xFFu);
    const txp_ui_snap_t *u = &g->sn.ui;
    *live = -1;
    if (k >= 0) {
        const txp_ui_lane_t *l = &u->lane[k];
        if (i == TXP_PL_PAN && (l->modm & TXP_MOD_PAN)) { *live = l->lpan; return 1; }
        if (i == TXP_PL_LEVEL) return (l->modm & TXP_MOD_LEVEL) != 0;
        if (i == TXP_PL_COUNT) return (l->modm & TXP_MOD_COUNT) != 0;
        if (i == TXP_PL_ACTIVE) return (l->modm & TXP_MOD_ACTIVE) != 0;
        if (i >= TXP_PL_CARD0 && i < TXP_PL_CARD0 + (int)TXP_CARD_N && (u->card[k].mod >> (i - TXP_PL_CARD0) & 1u)) {
            *live = u->card[k].live[i - TXP_PL_CARD0];
            return 1;
        }
        return 0;
    }
    if ((id >> 8) == 4u && i < 64) return (u->gmod[i / 32] >> (i % 32) & 1u) != 0;
    if ((id >> 8) == 5u && ((i & 15) == LP_RATE || (i & 15) == LP_LEVEL)) return (u->lfo_mod >> (i / 16) & 1u) != 0;
    return 0;
}
/* a card's hint (docs/MANUAL.md section 5, card_hints.h): the lane's sound's engine, the card's name */
static const char *card_hint(const char *snd, const char *card)
{
    const char *eng = 0;
    size_t i;
    for (i = 0; i < sizeof TXH_SND / sizeof TXH_SND[0] && !eng; i++) if (!strcmp(TXH_SND[i].snd, snd)) eng = TXH_SND[i].eng;
    if (!eng) return "";
    for (i = 0; i < sizeof TXH_CARD / sizeof TXH_CARD[0]; i++)
        if (!strcmp(TXH_CARD[i].eng, eng) && !strcmp(TXH_CARD[i].card, card)) return TXH_CARD[i].hint;
    return "";
}
/* a parameter's name in its section ("GRAIN FEEDBACK" in DELAY: "FEEDBACK"; "COMP COMP": "ON") */
static void short_name(const txp_param_t *p, char *o, size_t n)
{
    const char *s = strchr(p->name, ' ');
    if (!strcmp(p->name, "MOD CC") || !s || !strcmp(p->module, "Lanes") || !strcmp(p->module, "Scenes")) { snprintf(o, n, "%s", p->name); return; }
    snprintf(o, n, "%s", s + 1);
    if (!strcmp(o, "COMP")) snprintf(o, n, "ON");
}

/* a hint in two lines at most (words wrapped; what does not fit ends in "...") */
static void hint2(txg_t *g, frui_canvas *c, float x, float y, float w, const char *s)
{
    char ln[2][160];
    const char *p = s;
    for (int l = 0; l < 2; l++) {
        size_t n = 0, cut = 0;
        ln[l][0] = 0;
        while (*p == ' ') p++;
        while (p[n]) {
            char t[160];
            size_t e = n;
            while (p[e] && p[e] != ' ') e++;
            if (e >= sizeof t - 4) break;
            memcpy(t, p, e); t[e] = 0;
            if (TW(g, 9.5f, 450.f, 0.f, 0, t) > w) break;
            cut = e;
            n = e;
            while (p[n] == ' ') n++;
        }
        if (!cut && p[0]) for (cut = 0; p[cut] && p[cut] != ' ' && cut < 40; cut++) {}   /* (a word longer than the line) */
        memcpy(ln[l], p, cut); ln[l][cut] = 0;
        p += cut;
        while (*p == ' ') p++;
        if (l == 1 && *p) {              /* (more: the last line ends in "...") */
            size_t m = strlen(ln[1]);
            while (m > 0 && TW(g, 9.5f, 450.f, 0.f, 0, ln[1]) + TW(g, 9.5f, 450.f, 0.f, 0, "...") > w) ln[1][--m] = 0;
            while (m > 0 && ln[1][m - 1] == ' ') ln[1][--m] = 0;
            strcat(ln[1], "...");
        }
        TX(g, c, x, y + 11.f * (float)l, 9.5f, 450.f, 0.f, 0, C_DIM, ln[l]);
        if (!*p) break;
    }
}
/* ---- a value: its name, its text and (an amount) its fill; drag, wheel, double-click (its default), a click steps a
 * choice. hint: a line under it (the drawer's cards) */
static int is_bip(uint32_t id)           /* drawn from the middle: PAN, a route's DEPTH, TRANSPOSE */
{
    int k = lane_of_id(id);
    return (k >= 0 && (id & 0xFFu) == TXP_PL_PAN) || ((id >> 8) == 7u && (id & 0xFFu) < 64u && (id & 3u) == 2u) ||
           id == TXP_ID_GLOB(TXP_PG_TRANS);
}
static void pcell(txg_t *g, frui_canvas *c, float x, float y, float w, float h, uint32_t id, const char *lab, const char *hint)
{
    const txp_param_t *p = pinfo(id);
    char t[40], nm[40];
    int v, hi, live, m, amount;
    float f, f0, ty, nw;
    if (!p) return;
    v = txp_ui_get(g->c, id); hi = p->hi;
    m = pmod(g, id, &live);
    amount = !p->is_enum && hi >= 20;
    RR(g, c, x, y, w, h, 2.f, C_WELL);
    if (amount && hi > 0) {
        f = (float)(live >= 0 ? live : v) / (float)hi;
        f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
        f0 = is_bip(id) ? 0.5f : 0.f;
        R(g, c, x + w * fminf(f0, f), y, w * fabsf(f - f0), h, 0x0EEBE6E1u);
        R(g, c, x + w * fminf(f0, f), y + h - 2.f, fmaxf(w * fabsf(f - f0), 2.f), 2.f, m ? C_RED : frui_alpha(C_INK, 0.75f));
    } else if (hi >= 1 && hi <= 16) {    /* a choice: its place among its options, a tick under it */
        float sw = w / (float)(hi + 1);
        R(g, c, x + sw * (float)v, y + h - 2.f, sw, 2.f, m ? C_RED : frui_alpha(C_INK, 0.6f));
    }
    if (!lab) { short_name(p, nm, sizeof nm); lab = nm; }
    ty = hint ? y + 15.f : y + h * 0.5f + CAP(g, 10.5f) * 0.5f;
    clipL(g, c, x, y, w, h);
    nw = 9.f;
    if (m) nw += TX(g, c, x + nw, ty, 10.5f, 700.f, 0.f, 0, C_RED, "~ ");
    LBL(g, c, x + nw, ty, 10.5f, C_INK, lab);
    vtext(g, id, t, sizeof t);
    TX(g, c, x + w - 8.f - TW(g, 11.f, 500.f, 0.f, FRUI_TNUM, t), ty, 11.f, 500.f, 0.f, FRUI_TNUM, m ? C_RED : C_INK, t);
    if (hint && hint[0]) hint2(g, c, x + 9.f, y + 30.f, w - 18.f, hint);
    frui_clip_reset(c);
    wid_add2(g, TXG_W_PARAM, (int)id, id, 0, x, y, w, h);
}

/* ---- the parts */
static void caret(txg_t *g, frui_canvas *c, float x, float y, uint32_t col)   /* a small down triangle */
{
    frui_path p;
    frui_path_init(&p);
    frui_path_move(&p, (x - 4.f) * g->S, (y - 2.f) * g->S);
    frui_path_line(&p, (x + 4.f) * g->S, (y - 2.f) * g->S);
    frui_path_line(&p, x * g->S, (y + 2.5f) * g->S);
    frui_path_fill(c, &p, col);
    frui_path_free(&p);
}
static int any_used(const txg_t *g) { for (int k = 0; k < g->nlane; k++) if (used(g, k)) return 1; return 0; }
static void draw_meter(txg_t *g, frui_canvas *c, float x, float yc)   /* the output's level: LEVEL -6.0 dB, a bar */
{
    char t[24];
    int d = g->sn.ui.out_db10;
    float f = d <= -600 ? 0.f : (float)(d + 600) / 600.f;
    f = f > 1.f ? 1.f : f;
    LBL(g, c, x, yc - 2.f, 9.5f, C_DIM, "LEVEL");
    if (d <= -900) snprintf(t, sizeof t, "-inf dB"); else snprintf(t, sizeof t, "%.1f dB", d / 10.0);
    TX(g, c, x + 100.f - TW(g, 11.f, 500.f, 0.f, FRUI_TNUM, t), yc - 2.f, 11.f, 500.f, 0.f, FRUI_TNUM, d > -10 ? C_RED : C_INK, t);
    RR(g, c, x, yc + 5.f, 100.f, 4.f, 2.f, C_FAINT);
    if (f > 0.f) RR(g, c, x, yc + 5.f, 100.f * f, 4.f, 2.f, d > -10 ? C_RED : C_DIM);
}
static float meter_x(const txg_t *g) { return g->W - 140.f - 118.f; }
static void draw_top(txg_t *g, frui_canvas *c)
{
    const float y = 12.f, h = 28.f, yc = y + h * 0.5f;
    char tag[8] = "", name[24] = "", t[32];
    const char *pr = g->sn.ui.preset, *sp = strchr(pr, ' ');
    float x, bx0, xl = meter_x(g) - 146.f, xr = xl - 102.f, xu = xr - 66.f, xs = xu - 66.f, bx1 = xs - 40.f;
    R(g, c, 0, TOP_H - 1.f, g->W, 1.f, C_LINE);
    x = 18.f + TX(g, c, 18.f, yc + CAP(g, 19.f) * 0.5f, 19.f, 700.f, 5.f, 0, C_INK, "TEXTUR") + 5.f;
    x += TX(g, c, x, yc + CAP(g, 19.f) * 0.5f, 19.f, 700.f, 0.f, 0, C_RED, "A");
    bx0 = x + 24.f + 34.f;               /* (the previous scene's button after the name) */
    /* the previous / next scene, the scene (a click: the presets browser) */
    RR(g, c, bx0 - 34.f, y, 28.f, h, 3.f, C_BTN); RRS(g, c, bx0 - 34.f, y, 28.f, h, 3.f, C_LINE);
    LN(g, c, bx0 - 17.f, yc - 5.f, bx0 - 22.f, yc, 1.4f, C_INK); LN(g, c, bx0 - 22.f, yc, bx0 - 17.f, yc + 5.f, 1.4f, C_INK);
    wid_add(g, TXG_W_PREV, -1, bx0 - 34.f, y, 28.f, h);
    RR(g, c, bx1 + 6.f, y, 28.f, h, 3.f, C_BTN); RRS(g, c, bx1 + 6.f, y, 28.f, h, 3.f, C_LINE);
    LN(g, c, bx1 + 17.f, yc - 5.f, bx1 + 22.f, yc, 1.4f, C_INK); LN(g, c, bx1 + 22.f, yc, bx1 + 17.f, yc + 5.f, 1.4f, C_INK);
    wid_add(g, TXG_W_NEXT, -1, bx1 + 6.f, y, 28.f, h);
    RR(g, c, bx0, y, bx1 - bx0, h, 3.f, g->ov == OV_PRE ? C_SEG : C_WELL);
    RRS(g, c, bx0, y, bx1 - bx0, h, 3.f, g->ov == OV_PRE ? C_DIM : C_LINE);
    wid_add(g, TXG_W_PRESET, -1, bx0, y, bx1 - bx0, h);
    if (sp && sp - pr < 8) { snprintf(tag, sizeof tag, "%.*s", (int)(sp - pr), pr); snprintf(name, sizeof name, "%s", sp + 1); }
    else if (pr[0]) snprintf(name, sizeof name, "%s", pr);
    else if (g->sn.ui.land < g->cat->nfam) { snprintf(tag, sizeof tag, "L%02d", g->sn.ui.land + 1); upper(name, g->cat->fam[g->sn.ui.land].name, sizeof name); }
    else snprintf(name, sizeof name, "NO SCENE");
    clipL(g, c, bx0, y, bx1 - bx0 - 90.f, h);
    x = bx0 + 12.f;
    if (tag[0]) x += TX(g, c, x, yc + CAP(g, 13.f) * 0.5f, 13.f, 500.f, 0.4f, FRUI_TNUM, C_RED, tag) + 12.f;
    x += TX(g, c, x, yc + CAP(g, 13.f) * 0.5f, 13.f, 500.f, 0.6f, 0, C_INK, name);
    if (g->sn.ui.edited) TX(g, c, x + 6.f, yc + CAP(g, 13.f) * 0.5f, 13.f, 500.f, 0.f, 0, C_DIM, "*");
    frui_clip_reset(c);
    snprintf(t, sizeof t, "%d LANE%s", g->nlane, g->nlane == 1 ? "" : "S");
    LBL(g, c, bx1 - 26.f - LBLW(g, 10.5f, t), yc + CAP(g, 10.5f) * 0.5f, 10.5f, C_DIM, t);
    caret(g, c, bx1 - 14.f, yc, C_DIM);
    /* SAVE, UNDO, PLAY ALL / STOP ALL (the FM-1's REC: all stopped is plain to see) */
    g->sv_x = xs;
    RR(g, c, xs, y, 60.f, h, 3.f, g->ov == OV_SAVE ? C_SEG : C_BTN); RRS(g, c, xs, y, 60.f, h, 3.f, g->ov == OV_SAVE ? C_DIM : C_LINE);
    LBL(g, c, xs + (60.f - LBLW(g, 11.f, "SAVE")) * 0.5f, yc + CAP(g, 11.f) * 0.5f, 11.f, C_INK, "SAVE");
    wid_add(g, TXG_W_SAVE, -1, xs, y, 60.f, h);
    RR(g, c, xu, y, 60.f, h, 3.f, C_BTN); RRS(g, c, xu, y, 60.f, h, 3.f, C_LINE);
    LBL(g, c, xu + (60.f - LBLW(g, 11.f, "UNDO")) * 0.5f, yc + CAP(g, 11.f) * 0.5f, 11.f, g->sn.ui.undo ? C_INK : C_FAINT, "UNDO");
    wid_add(g, TXG_W_UNDO, -1, xu, y, 60.f, h);
    {
        int run = g->sn.ui.any_run, stopped = !run && any_used(g);
        const char *rt = run ? "STOP ALL" : "PLAY ALL";
        RR(g, c, xr, y, 92.f, h, 3.f, stopped ? C_INK : C_BTN); RRS(g, c, xr, y, 92.f, h, 3.f, stopped ? C_INK : C_LINE);
        if (run) R(g, c, xr + 12.f, yc - 4.f, 8.f, 8.f, C_INK);
        else {
            frui_path p;
            frui_path_init(&p);
            frui_path_move(&p, (xr + 12.f) * g->S, (yc - 5.f) * g->S);
            frui_path_line(&p, (xr + 20.f) * g->S, yc * g->S);
            frui_path_line(&p, (xr + 12.f) * g->S, (yc + 5.f) * g->S);
            frui_path_fill(c, &p, stopped ? C_PANEL : C_INK);
            frui_path_free(&p);
        }
        LBL(g, c, xr + 28.f, yc + CAP(g, 11.f) * 0.5f, 11.f, stopped ? C_PANEL : C_INK, rt);
        wid_add(g, TXG_W_REC, -1, xr, y, 92.f, h);
    }
    /* LANES */
    x = xl;
    LBL(g, c, x, yc + CAP(g, 11.f) * 0.5f, 11.f, C_DIM, "LANES");
    x += 54.f;
    RR(g, c, x, y + 2.f, 24.f, h - 4.f, 3.f, C_BTN); RRS(g, c, x, y + 2.f, 24.f, h - 4.f, 3.f, C_LINE);
    LN(g, c, x + 8.f, yc, x + 16.f, yc, 1.4f, g->nlane > 1 ? C_INK : C_FAINT);
    wid_add(g, TXG_W_LDN, -1, x, y + 2.f, 24.f, h - 4.f);
    snprintf(t, sizeof t, "%d", g->nlane);
    TX(g, c, x + 24.f + (32.f - TW(g, 14.f, 500.f, 0.f, FRUI_TNUM, t)) * 0.5f, yc + CAP(g, 14.f) * 0.5f, 14.f, 500.f, 0.f,
       FRUI_TNUM, C_INK, t);
    x += 56.f;
    RR(g, c, x, y + 2.f, 24.f, h - 4.f, 3.f, C_BTN); RRS(g, c, x, y + 2.f, 24.f, h - 4.f, 3.f, C_LINE);
    LN(g, c, x + 8.f, yc, x + 16.f, yc, 1.4f, g->nlane < (int)TXP_LANES ? C_INK : C_FAINT);
    LN(g, c, x + 12.f, yc - 4.f, x + 12.f, yc + 4.f, 1.4f, g->nlane < (int)TXP_LANES ? C_INK : C_FAINT);
    wid_add(g, TXG_W_LUP, -1, x, y + 2.f, 24.f, h - 4.f);
    draw_meter(g, c, meter_x(g), yc);
    /* SETTINGS */
    x = g->W - 140.f;
    RR(g, c, x, y, 124.f, h, 3.f, g->ov == OV_SET ? C_SEG : C_BTN); RRS(g, c, x, y, 124.f, h, 3.f, C_LINE);
    RI(g, c, x + 20.f, yc, 3.6f, 1.5f, C_INK);
    for (int i = 0; i < 8; i++) {
        float a = (float)i * 0.785398f, ca = cosf(a), sa = sinf(a);
        LN(g, c, x + 20.f + ca * 5.6f, yc + sa * 5.6f, x + 20.f + ca * 7.4f, yc + sa * 7.4f, 1.6f, C_INK);
    }
    LBL(g, c, x + 34.f, yc + CAP(g, 12.f) * 0.5f, 12.f, C_INK, "SETTINGS");
    wid_add(g, TXG_W_SET, -1, x, y, 124.f, h);
}

/* ---- the place */
static void map_arc(txg_t *g, frui_canvas *c, float r, uint32_t col)   /* the upper half of a ring */
{
    float lw = lw1(g), px = g->mcx - r * g->mr, py = g->mcy;
    for (int i = 1; i <= 72; i++) {
        float a = 3.14159265f * (1.f - (float)i / 72.f), x = g->mcx + cosf(a) * r * g->mr, y = g->mcy - sinf(a) * r * g->mr;
        LN(g, c, px, py, x, y, lw, col);
        px = x; py = y;
    }
}
/* a lane's label on a place: "3 Campfire", "3" alone when the place is crowded and the lane is neither selected nor
 * hovered; lanes standing on one spot share one label ("3 Campfire · 1 · 4") */
static int n_used(const txg_t *g) { int n = 0; for (int k = 0; k < g->nlane; k++) n += used(g, k); return n; }
static void draw_view_switch(txg_t *g, frui_canvas *c)
{
    float x = 16.f, y = TOP_H + 12.f, wm = 58.f, ws = 66.f, h = 24.f, xo = g->view ? x + wm : x, wo = g->view ? ws : wm;
    RR(g, c, x, y, wm + ws, h, 3.f, C_PANEL);
    R(g, c, xo, y, wo, h, C_SEG);
    R(g, c, xo, y + h - 2.f, wo, 2.f, C_RED);
    RRS(g, c, x, y, wm + ws, h, 3.f, C_LINE);
    R(g, c, x + wm, y, 1.f, h, C_LINE);
    LBL(g, c, x + (wm - LBLW(g, 11.f, "MAP")) * 0.5f, y + h * 0.5f + CAP(g, 11.f) * 0.5f, 11.f, g->view ? C_DIM : C_INK, "MAP");
    LBL(g, c, x + wm + (ws - LBLW(g, 11.f, "SPACE")) * 0.5f, y + h * 0.5f + CAP(g, 11.f) * 0.5f, 11.f, g->view ? C_INK : C_DIM,
        "SPACE");
    wid_add(g, TXG_W_VMAP, -1, x, y, wm, h);
    wid_add(g, TXG_W_VSPACE, -1, x + wm, y, ws, h);
}
/* the selected lane at the place's head: its number and family, its sound (a click: the sound browser), its engine's
 * colour (a click: the picker) */
static void draw_place_head(txg_t *g, frui_canvas *c)
{
    int k = g->sel;
    const txp_ui_lane_t *l = &g->sn.ui.lane[k];
    float x = 158.f, y = TOP_H + 8.f, w;
    char t[48], nm[32];
    snprintf(t, sizeof t, "LANE %d%s%s", k + 1, used(g, k) ? "  \xC2\xB7  " : "", used(g, k) ? l->fam : "");
    upper(t, t, sizeof t);
    LBL(g, c, x, y + 10.f, 10.f, C_RED, t);
    clipL(g, c, x, y, g->PW - x - 40.f, 40.f);
    if (used(g, k)) { cap_name(nm, l->snd, sizeof nm); w = TX(g, c, x, y + 29.f, 15.f, 650.f, 0.2f, 0, (g->mute >> k & 1u) ? C_DIM : C_INK, nm); }
    else w = TX(g, c, x, y + 29.f, 15.f, 600.f, 0.2f, 0, C_FAINT, "Empty: choose a sound");
    frui_clip_reset(c);
    if (w > g->PW - x - 60.f) w = g->PW - x - 60.f;
    caret(g, c, x + w + 10.f, y + 24.f, C_DIM);
    wid_add(g, TXG_W_SND, k, x - 4.f, y + 14.f, w + 22.f, 22.f);
    if (used(g, k)) {
        float xx = x + w + 28.f;
        CI(g, c, xx, y + 24.f, 5.f, lane_col(g, k));
        RI(g, c, xx, y + 24.f, 7.5f, 1.f, g->pick >= 0 && g->pick == l->eng ? C_INK : C_FAINT);
        wid_add2(g, TXG_W_COLDOT, k, (uint32_t)l->eng, 0, xx - 10.f, y + 14.f, 20.f, 20.f);
    }
}
/* the lanes' labels on a place (pts: the dots, on: drawn): right, left, above, below the dot ... at two distances: the
 * first place clear of the dots, the other labels and the edges (y0 .. y1), the selected lane's first. Lanes on one
 * spot (within 8 px) share a label: the selected (or hovered) one named, the others by number */
static void place_labels(txg_t *g, frui_canvas *c, float (*pts)[2], const int *on, float y0, float y1)
{
    float boxes[TXP_LANES * 2][4];
    int nb = 0, k, order[TXP_LANES], no = 0, crowd = n_used(g) > MAP_CROWD, done[TXP_LANES] = {0};
    char t[96];
    for (k = 0; k < g->nlane; k++) {
        if (!on[k]) continue;
        boxes[nb][0] = pts[k][0] - 10.f; boxes[nb][1] = pts[k][1] - 10.f; boxes[nb][2] = pts[k][0] + 10.f; boxes[nb][3] = pts[k][1] + 10.f;
        nb++;
    }
    if (g->sel < g->nlane && on[g->sel]) order[no++] = g->sel;
    if (g->hover_kind == TXG_W_DOT && g->hover_lane >= 0 && g->hover_lane < g->nlane && on[g->hover_lane] && g->hover_lane != g->sel)
        order[no++] = g->hover_lane;
    for (k = 0; k < g->nlane; k++) if (on[k] && k != g->sel && !(g->hover_kind == TXG_W_DOT && k == g->hover_lane)) order[no++] = k;
    for (int o = 0; o < no; o++) {
        float x, y, w, tries[16][2], *at = 0;
        int named;
        char nm[32];
        k = order[o];
        if (done[k]) continue;
        x = pts[k][0]; y = pts[k][1];
        named = !crowd || k == g->sel || (g->hover_kind == TXG_W_DOT && g->hover_lane == k);
        cap_name(nm, g->sn.ui.lane[k].snd, sizeof nm);
        if (named) snprintf(t, sizeof t, "%d %s", k + 1, nm); else snprintf(t, sizeof t, "%d", k + 1);
        for (int j = 0; j < g->nlane; j++) {   /* (the lanes on the same spot: by number) */
            float dx = pts[j][0] - x, dy = pts[j][1] - y;
            size_t n = strlen(t);
            if (j == k || !on[j] || done[j] || dx * dx + dy * dy > 64.f) continue;
            done[j] = 1;
            snprintf(t + n, sizeof t - n, "  \xC2\xB7  %d", j + 1);
        }
        done[k] = 1;
        w = TW(g, 11.f, 600.f, 0.f, 0, t);
        for (int ring = 0; ring < 2; ring++) {
            float d = 13.f + 10.f * (float)ring, u = 8.f + 8.f * (float)ring;
            float *tr = tries[ring * 8];
            tr[0] = x + d; tr[1] = y - 6.f; tr[2] = x - d - w; tr[3] = y - 6.f;
            tr[4] = x - w * 0.5f; tr[5] = y - 16.f - u; tr[6] = x - w * 0.5f; tr[7] = y + 4.f + u;
            tr[8] = x + d; tr[9] = y - 12.f - u; tr[10] = x - d - w; tr[11] = y + u;
            tr[12] = x + d; tr[13] = y + u; tr[14] = x - d - w; tr[15] = y - 12.f - u;
        }
        for (int i = 0; i < 16 && !at; i++) {
            float b[4] = { tries[i][0] - 2.f, tries[i][1] - 1.f, tries[i][0] + w + 2.f, tries[i][1] + 13.f };
            int hit = b[0] < 2.f || b[2] > g->PW - 2.f || b[1] < y0 || b[3] > y1;
            for (int j = 0; j < nb && !hit; j++)
                hit = b[0] < boxes[j][2] && b[2] > boxes[j][0] && b[1] < boxes[j][3] && b[3] > boxes[j][1];
            if (!hit) at = tries[i];
        }
        if (!at) {                       /* no room: the selected lane's label anyway (inside the edges), the others none */
            if (k != g->sel) continue;
            at = tries[0];
            if (at[0] + w > g->PW - 4.f) at = tries[1];
            if (at[0] < 4.f) at[0] = 4.f;
        }
        if (nb < (int)(TXP_LANES * 2)) {
            boxes[nb][0] = at[0] - 2.f; boxes[nb][1] = at[1] - 1.f; boxes[nb][2] = at[0] + w + 2.f; boxes[nb][3] = at[1] + 13.f;
            nb++;
        }
        TX(g, c, at[0], at[1] + 10.f, 11.f, 600.f, 0.f, 0, k == g->sel ? C_INK : faded(g, k) ? C_FAINT : C_DIM, t);
    }
}
/* the dots: lanes on one spot fanned a few pixels apart, so each one shows and can be taken */
static void fan(float (*pts)[2], const int *on, int n)
{
    for (int k = 0; k < n; k++) {
        int same = 0, idx = 0;
        if (!on[k]) continue;
        for (int j = 0; j < n; j++) {
            float dx, dy;
            if (!on[j]) continue;
            dx = pts[j][0] - pts[k][0]; dy = pts[j][1] - pts[k][1];
            if (dx * dx + dy * dy < 4.f) { if (j < k) idx++; same++; }
        }
        if (same > 1) {
            float a = 6.2831853f * (float)idx / (float)same;
            pts[k][0] += cosf(a) * 9.f; pts[k][1] += sinf(a) * 9.f;
        }
    }
}
static void draw_map_field(txg_t *g, frui_canvas *c)
{
    static const float RINGS[4] = {0.25f, 0.5f, 0.75f, 1.f};
    float lw = lw1(g), pts[TXP_LANES][2];
    int on[TXP_LANES], k;
    for (int i = 0; i < 4; i++) map_arc(g, c, RINGS[i], C_FAINT);
    for (int a = -60; a <= 60; a += 30) {
        float r = (float)a * 3.14159265f / 180.f;
        LN(g, c, g->mcx, g->mcy, g->mcx + sinf(r) * g->mr, g->mcy - cosf(r) * g->mr, lw, C_FAINT);
    }
    LN(g, c, g->mcx - g->mr - 6.f, g->mcy, g->mcx + g->mr + 6.f, g->mcy, lw, C_FAINT);
    {
        frui_path p;
        frui_path_init(&p);
        frui_path_move(&p, g->mcx * g->S, (g->mcy - 7.f) * g->S);
        frui_path_line(&p, (g->mcx + 6.f) * g->S, (g->mcy + 4.f) * g->S);
        frui_path_line(&p, (g->mcx - 6.f) * g->S, (g->mcy + 4.f) * g->S);
        frui_path_fill(c, &p, C_INK);
        frui_path_free(&p);
    }
    TX(g, c, g->mcx - TW(g, 10.f, 500.f, 1.f, 0, "YOU") * 0.5f, g->mcy + 22.f, 10.f, 500.f, 1.f, 0, C_DIM, "YOU");
    TX(g, c, g->mcx - g->mr - 10.f - TW(g, 10.f, 500.f, 0.f, 0, "L"), g->mcy + 4.f, 10.f, 500.f, 0.f, 0, C_DIM, "L");
    TX(g, c, g->mcx + g->mr + 10.f, g->mcy + 4.f, 10.f, 500.f, 0.f, 0, C_DIM, "R");
    TX(g, c, g->mcx - TW(g, 10.f, 500.f, 1.f, 0, "FAR") * 0.5f, g->mcy - g->mr - 8.f, 10.f, 500.f, 1.f, 0, C_DIM, "FAR");
    /* the lanes */
    for (k = 0; k < g->nlane; k++) {
        on[k] = used(g, k);
        if (!on[k]) continue;
        map_pos(g, g->pan[k], g->dist[k], &pts[k][0], &pts[k][1]);
    }
    fan(pts, on, g->nlane);
    for (k = 0; k < g->nlane; k++) {
        const txp_ui_lane_t *l = &g->sn.ui.lane[k];
        float a, mf;
        uint32_t col = lane_col(g, k);
        if (!on[k]) continue;
        a = glow(g, k); mf = faded(g, k) ? 0.25f : 1.f;
        if (l->modm & (TXP_MOD_PAN | TXP_MOD_DIST)) {   /* where the LFO has it now: a red trace off the dot */
            float lx, ly;
            map_pos(g, (l->modm & TXP_MOD_PAN) ? l->lpan : g->pan[k], g->dist[k] < 0 ? -1 : (l->modm & TXP_MOD_DIST) ? l->ldist : g->dist[k], &lx, &ly);
            LN(g, c, pts[k][0], pts[k][1], lx, ly, 1.f, frui_alpha(C_RED, 0.6f * mf));
            CI(g, c, lx, ly, 2.6f, frui_alpha(C_RED, mf));
        }
        CI(g, c, pts[k][0], pts[k][1], 6.f + 6.f * a, frui_alpha(col, (0.08f + 0.22f * a) * mf));
        if (g->dist[k] >= 0) CI(g, c, pts[k][0], pts[k][1], 4.f, frui_alpha(col, mf < 1.f ? 0.35f : 1.f));
        else RI(g, c, pts[k][0], pts[k][1], 3.4f, 1.4f, frui_alpha(col, mf < 1.f ? 0.35f : 1.f));
        if (k == g->sel) RI(g, c, pts[k][0], pts[k][1], 9.f, 1.4f, C_RED);
        wid_add(g, TXG_W_DOT, k, pts[k][0] - 11.f, pts[k][1] - 11.f, 22.f, 22.f);
    }
    place_labels(g, c, pts, on, g->mcy - g->mr - 24.f, g->mcy - 2.f);
}
/* SPACE: the same lanes on the ground in perspective, far ones first; each one's events of the last 6 s drift from it
 * toward the horizon (its trail is its event line), its glow its newest; a drag moves it on the ground */
static void draw_space(txg_t *g, frui_canvas *c)
{
    float lw = lw1(g), vx = g->mcx, W = g->PW;
    float pts[TXP_LANES][2];
    int order[TXP_LANES], on[TXP_LANES] = {0}, no = 0;
    char t[64];
    clipL(g, c, 0, g->sp_y0 - 8.f, W - 1.f, g->sp_y1 - g->sp_y0 + 22.f);
    for (int i = 0; i <= 7; i++) {        /* the ground: depth lines, rays to the vanishing point */
        float y = g->sp_hz + (g->sp_y1 - g->sp_hz) * powf((float)i / 7.f, 1.7f);
        LN(g, c, 0.f, y, W, y, lw, frui_alpha(C_FAINT, 0.35f + 0.5f * (float)i / 7.f));
    }
    for (int i = -6; i <= 6; i++) LN(g, c, vx + (float)i * W * 0.025f, g->sp_hz, vx + (float)i * W * 0.15f, g->sp_y1, lw, frui_alpha(C_FAINT, 0.45f));
    TX(g, c, 12.f, g->sp_hz - 6.f, 10.f, 500.f, 1.f, 0, C_DIM, "FAR");
    TX(g, c, 12.f, g->sp_y1 + 4.f, 10.f, 500.f, 1.f, 0, C_DIM, "L");
    TX(g, c, W - 20.f, g->sp_y1 + 4.f, 10.f, 500.f, 1.f, 0, C_DIM, "R");
    TX(g, c, g->mcx - TW(g, 10.f, 500.f, 1.f, 0, "YOU") * 0.5f, g->sp_y1 + 4.f, 10.f, 500.f, 1.f, 0, C_DIM, "YOU");
    for (int k = 0; k < g->nlane; k++)
        if (used(g, k)) { order[no++] = k; on[k] = 1; sp_pos(g, g->pan[k], g->dist[k], &pts[k][0], &pts[k][1], 0); }
    fan(pts, on, g->nlane);
    for (int i = 1; i < no; i++)         /* far first */
        for (int j = i; j > 0 && pts[order[j - 1]][1] > pts[order[j]][1]; j--) { int a = order[j - 1]; order[j - 1] = order[j]; order[j] = a; }
    for (int o = 0; o < no; o++) {
        int k = order[o];
        const txp_ui_lane_t *l = &g->sn.ui.lane[k];
        float x = pts[k][0], y, sc, mf = faded(g, k) ? 0.25f : 1.f, a = glow(g, k), ex = 0.f, ey = 0.f, x0;
        uint32_t col = lane_col(g, k);
        sp_pos(g, g->pan[k], g->dist[k], &x0, &y, &sc);
        y = pts[k][1];
        ex = vx + (x - vx) * 0.25f; ey = g->sp_hz;
        LN(g, c, x, y, ex, ey, lw, frui_alpha(k == g->sel ? C_RED : C_FAINT, 0.55f));
        for (uint32_t b = 0; b + 1u < NBLK; b += 2u) {   /* the trail: its events, older further away */
            uint32_t e = (uint32_t)g->ev[b][k] + g->ev[b + 1u][k], p = g->pk[b][k] > g->pk[b + 1u][k] ? g->pk[b][k] : g->pk[b + 1u][k];
            float age, f, s, px, py, al;
            if (!e) continue;
            age = 1.f - (float)b / (float)NBLK;
            f = powf(age, 0.7f);
            s = sc * (1.f - f * 0.85f);
            px = x + (ex - x) * f; py = y + (ey - y) * f;
            al = (1.f - age) * (0.35f + 0.65f * (float)p / 255.f) * mf;
            CI(g, c, px, py, fmaxf(0.6f, (0.8f + 2.4f * (float)p / 255.f) * s), frui_alpha(col, al));
        }
        if (l->modm & (TXP_MOD_PAN | TXP_MOD_DIST)) {
            float lx, ly;
            sp_pos(g, (l->modm & TXP_MOD_PAN) ? l->lpan : g->pan[k], g->dist[k] < 0 ? -1 : (l->modm & TXP_MOD_DIST) ? l->ldist : g->dist[k], &lx, &ly, 0);
            LN(g, c, x, y, lx, ly, 1.f, frui_alpha(C_RED, 0.6f * mf));
            CI(g, c, lx, ly, 2.4f, frui_alpha(C_RED, mf));
        }
        /* the source: a soft shadow on the ground, its glow, the dot (hollow: no DIST card) */
        frui_circle(c, x * g->S, (y + 1.f) * g->S, (5.f + 9.f * a) * sc * g->S, frui_alpha(col, (0.08f + 0.25f * a) * mf));
        if (g->dist[k] >= 0) CI(g, c, x, y, 2.5f + 2.f * sc, frui_alpha(col, mf < 1.f ? 0.35f : 1.f));
        else RI(g, c, x, y, 2.2f + 2.f * sc, 1.4f, frui_alpha(col, mf < 1.f ? 0.35f : 1.f));
        if (k == g->sel) RI(g, c, x, y, 6.f + 4.f * sc, 1.3f, C_RED);
        wid_add(g, TXG_W_DOT, k, x - 11.f, y - 11.f, 22.f, 22.f);
    }
    place_labels(g, c, pts, on, g->sp_hz - 4.f, g->sp_y1 - 2.f);
    if (g->drag == TXG_W_DOT && g->drag_lane < g->nlane && used(g, g->drag_lane)) {   /* the drag: where it is, in numbers */
        int k = g->drag_lane;
        float x, y;
        char a[16];
        sp_pos(g, g->pan[k], g->dist[k], &x, &y, 0);
        ptext(TXP_ID_LANE(k, TXP_PL_PAN), g->pan[k], a, sizeof a);
        if (g->dist[k] >= 0) snprintf(t, sizeof t, "PAN %s  \xC2\xB7  DIST %d", a, g->dist[k]);
        else snprintf(t, sizeof t, "PAN %s", a);
        {
            float w = TW(g, 10.5f, 500.f, 0.3f, FRUI_TNUM, t) + 14.f, bx = x - w * 0.5f;
            bx = bx < 4.f ? 4.f : bx + w > W - 4.f ? W - 4.f - w : bx;
            RR(g, c, bx, y - 34.f, w, 18.f, 3.f, C_BTN);
            TX(g, c, bx + 7.f, y - 21.f, 10.5f, 500.f, 0.3f, FRUI_TNUM, C_INK, t);
        }
    }
    frui_clip_reset(c);
}
static void draw_place(txg_t *g, frui_canvas *c)
{
    R(g, c, 0, TOP_H, g->PW, g->dr_y - TOP_H, C_WELL);
    R(g, c, g->PW - 1.f, TOP_H, 1.f, g->dr_y - TOP_H, C_LINE);
    wid_add(g, TXG_W_PLACE, -1, 0, TOP_H, g->PW, g->dr_y - TOP_H);   /* (the tests: the place, the map's half circle) */
    wid_add(g, TXG_W_MAPR, -1, g->mcx - g->mr, g->mcy - g->mr, 2.f * g->mr, g->mr);
    draw_view_switch(g, c);
    draw_place_head(g, c);
    if (g->view) draw_space(g, c); else draw_map_field(g, c);
}

/* ---- the rows */
static void draw_line(txg_t *g, frui_canvas *c, int k, float x, float y, float w, float h, float mf)
{
    int X0 = (int)lroundf(x * g->S), X1 = (int)lroundf((x + w) * g->S), Y1 = (int)lroundf((y + h) * g->S);
    float H = h * g->S;
    uint32_t col = lane_col(g, k);
    RR(g, c, x, y, w, h, 2.f, C_WELL);
    if (X1 <= X0) return;
    for (int i = X0; i < X1; i++) {
        uint32_t b0 = (uint32_t)((uint64_t)(i - X0) * NBLK / (uint32_t)(X1 - X0)), b1 = (uint32_t)((uint64_t)(i + 1 - X0) * NBLK / (uint32_t)(X1 - X0)), e = 0, p = 0;
        float age = 1.f - (float)(i - X0) / (float)(X1 - X0), hh;
        if (b1 <= b0) b1 = b0 + 1u;
        for (uint32_t b = b0; b < b1 && b < NBLK; b++) {
            e += g->ev[b][k];
            if (g->pk[b][k] > p) p = g->pk[b][k];
        }
        if (!p) continue;
        hh = (float)p / 255.f * (H - 2.f) * 0.85f;   /* the lane's level: a faint body */
        frui_rect(c, (float)i, (float)Y1 - 1.f - hh, 1.f, hh, frui_alpha(C_ENV, mf));
        if (!e) continue;                   /* its events: a bar each block that has one, as high as the peak */
        hh = (0.18f + 0.82f * (float)p / 255.f) * (H - 4.f);
        frui_rect(c, (float)i, (float)Y1 - 2.f - hh, 1.f, hh, frui_alpha(col, (0.22f + 0.62f * (1.f - age)) * mf));
    }
}
/* HOW MANY (6 blocks: 1 2 4 10 19 32; none lit: OFF) or HOW ACTIVE (9 blocks: rare .. busy), its name and value */
static void steps(txg_t *g, frui_canvas *c, int k, int act, float x, float yc, int sel, uint32_t ink)
{
    uint32_t id = TXP_ID_LANE(k, act ? TXP_PL_ACTIVE : TXP_PL_COUNT);
    int n = act ? 9 : 6, v = act ? g->act[k] : g->cnt[k], live, m = pmod(g, id, &live);
    float bw = (RW_STEP - 2.f * (float)(n - 1)) / (float)n;
    char t[16], lab[24];
    ptext(id, v, t, sizeof t);
    snprintf(lab, sizeof lab, "%s%s", m ? "~ " : "", act ? "ACTIVE" : "MANY");
    LBL(g, c, x, yc - 4.f, 9.f, m ? C_RED : C_DIM, lab);
    TX(g, c, x + RW_STEP - TW(g, 10.5f, 500.f, 0.f, FRUI_TNUM, t), yc - 4.f, 10.5f, 500.f, 0.f, FRUI_TNUM, v ? ink : C_FAINT, t);
    for (int i = 0; i < n; i++)
        R(g, c, x + (bw + 2.f) * (float)i, yc + 3.f, bw, 6.f, i < v ? (m ? C_RED : sel ? C_RED : ink) : C_FAINT);
    wid_add2(g, act ? TXG_W_ACT : TXG_W_MANY, k, id, 0, x - 2.f, yc - 14.f, RW_STEP + 4.f, 28.f);
}
static int rows_total(const txg_t *g) { return g->nlane + (g->nlane < (int)TXP_LANES ? 1 : 0); }
static void rows_clamp(txg_t *g)
{
    int mx = rows_total(g) - g->rvis;
    if (g->rtop > mx) g->rtop = mx;
    if (g->rtop < 0) g->rtop = 0;
}
static void draw_rows(txg_t *g, frui_canvas *c)
{
    const rcol_t rc = rcols(g);
    const float x0 = g->PW, y0 = TOP_H + HDR_H, x1 = g->W;
    char t[48], nm[32];
    rows_clamp(g);
    {   /* the columns' names */
        float yb = TOP_H + HDR_H * 0.5f + CAP(g, 10.f) * 0.5f;
        LBL(g, c, rc.num, yb, 10.f, C_DIM, "#");
        LBL(g, c, rc.name, yb, 10.f, C_DIM, "SOUND");
        LBL(g, c, rc.tl, yb, 10.f, C_DIM, "EVENTS");
        LBL(g, c, rc.tl + rc.tlw - TW(g, 10.f, 620.f, 1.4f, 0, "6 S"), yb, 10.f, C_FAINT, "6 S");
        LBL(g, c, rc.many, yb, 10.f, C_DIM, "HOW MANY");
        LBL(g, c, rc.act, yb, 10.f, C_DIM, "HOW ACTIVE");
        LBL(g, c, rc.pan, yb, 10.f, C_DIM, "PAN");
        LBL(g, c, rc.ch, yb, 10.f, C_DIM, "CH");
        LBL(g, c, rc.vol, yb, 10.f, C_DIM, "VOL");
        LBL(g, c, rc.ms + 7.f, yb, 10.f, C_DIM, "M");
        LBL(g, c, rc.ms + 33.f, yb, 10.f, C_DIM, "S");
        R(g, c, x0, y0 - 1.f, x1 - x0, 1.f, C_LINE);
    }
    wid_add(g, TXG_W_ROWS, -1, x0, y0, x1 - x0, g->dr_y - y0);
    clipL(g, c, x0, y0, x1 - x0, g->dr_y - y0);
    for (int k = g->rtop; k < g->nlane && k < g->rtop + g->rvis; k++) {
        const txp_ui_lane_t *l = &g->sn.ui.lane[k];
        float y = y0 + ROW_H * (float)(k - g->rtop), yc = y + ROW_H * 0.5f, mf = faded(g, k) ? 0.25f : 1.f, th = ROW_H - 12.f;
        int u = used(g, k), sel = k == g->sel, stopped = u && !l->run;
        uint32_t ink = u ? (mf < 1.f ? C_DIM : C_INK) : C_FAINT;
        if (sel) { R(g, c, x0, y, x1 - x0, ROW_H, C_SEL); R(g, c, x0, y, 2.f, ROW_H, C_RED); }
        R(g, c, x0, y + ROW_H - 1.f, x1 - x0, 1.f, C_LINE);
        wid_add(g, TXG_W_ROW, k, x0, y, x1 - x0, ROW_H);
        snprintf(t, sizeof t, "%d", k + 1);
        TX(g, c, rc.num, yc + CAP(g, 11.f) * 0.5f, 11.f, 500.f, 0.f, FRUI_TNUM, sel ? C_RED : C_DIM, t);
        if (u) {
            float a = glow(g, k), w;
            CI(g, c, rc.dot, yc, 3.5f, frui_alpha(lane_col(g, k), (0.35f + 0.65f * a) * mf));
            cap_name(nm, l->snd, sizeof nm);
            clipL(g, c, rc.name, y, rc.tl - rc.name - 8.f, ROW_H);
            w = TX(g, c, rc.name, yc - 1.f, 12.5f, 600.f, 0.f, 0, ink, nm);
            TX(g, c, rc.name, yc + 12.f, 10.f, 450.f, 0.3f, 0, C_DIM, l->fam);
            frui_clip_reset(c);
            clipL(g, c, x0, y0, x1 - x0, g->dr_y - y0);
            if (w > rc.tl - rc.name - 8.f) w = rc.tl - rc.name - 8.f;
            wid_add(g, TXG_W_SND, k, rc.name - 2.f, yc - 13.f, w + 6.f, 17.f);
        } else {
            TX(g, c, rc.name, yc + CAP(g, 12.f) * 0.5f, 12.f, 450.f, 0.3f, 0, C_FAINT, "empty");
            wid_add(g, TXG_W_SND, k, rc.name - 2.f, yc - 9.f, 44.f, 18.f);
        }
        draw_line(g, c, k, rc.tl, yc - th * 0.5f, rc.tlw, th, mf);
        if (stopped) {                   /* (a stopped lane: plain to see; HOW MANY or HOW ACTIVE starts it) */
            const char *s = "STOPPED  \xC2\xB7  SET HOW MANY TO PLAY";
            if (TW(g, 9.5f, 620.f, 1.3f, 0, s) > rc.tlw - 16.f) s = "STOPPED";
            LBL(g, c, rc.tl + 8.f, yc + CAP(g, 9.5f) * 0.5f, 9.5f, C_DIM, s);
        }
        if (u) {
            steps(g, c, k, 0, rc.many, yc, sel, ink);
            steps(g, c, k, 1, rc.act, yc, sel, ink);
            {   /* PAN (a drag moves it; red while an LFO does) */
                uint32_t id = TXP_ID_LANE(k, TXP_PL_PAN);
                int live, m = pmod(g, id, &live);
                char a[16];
                ptext(id, m && live >= 0 ? live : g->pan[k], a, sizeof a);
                if (m) { snprintf(t, sizeof t, "~%s", a); snprintf(a, sizeof a, "%s", t); }
                TX(g, c, rc.pan, yc + CAP(g, 12.f) * 0.5f, 12.f, 500.f, 0.f, FRUI_TNUM, m ? C_RED : ink, a);
                wid_add2(g, TXG_W_PARAM, (int)id, id, 0, rc.pan - 4.f, yc - 12.f, RW_PAN + 4.f, 24.f);
            }
        }
        {   /* the channel (a click: the next; the lane's own, SETTINGS: MIDI IN too) */
            uint32_t id = TXP_ID_LCH(k);
            if (l->ch) snprintf(t, sizeof t, "%d", l->ch); else snprintf(t, sizeof t, "OFF");
            TX(g, c, rc.ch, yc + CAP(g, 12.f) * 0.5f, 12.f, 500.f, 0.f, FRUI_TNUM, u ? C_INK : C_FAINT, t);
            wid_add2(g, TXG_W_PARAM, (int)id, id, 0, rc.ch - 4.f, yc - 12.f, RW_CH + 4.f, 24.f);
        }
        if (u) {   /* VOL (red while an LFO moves it) */
            float v = (float)g->level[k] / 127.f, ty = yc + 3.f;
            uint32_t kc = (l->modm & TXP_MOD_LEVEL) ? C_RED : mf < 1.f ? C_DIM : C_INK;
            snprintf(t, sizeof t, "%s%d", (l->modm & TXP_MOD_LEVEL) ? "~ " : "", g->level[k]);
            TX(g, c, rc.vol + RW_VOL - TW(g, 11.f, 500.f, 0.f, FRUI_TNUM, t), yc - 5.f, 11.f, 500.f, 0.f, FRUI_TNUM,
               (l->modm & TXP_MOD_LEVEL) ? C_RED : ink, t);
            RR(g, c, rc.vol, ty, RW_VOL, 4.f, 2.f, C_FAINT);
            RR(g, c, rc.vol, ty, RW_VOL * v, 4.f, 2.f, mf < 1.f ? C_DIM : C_INK);
            RR(g, c, rc.vol + RW_VOL * v - 4.f, ty - 4.f, 8.f, 12.f, 2.f, kc);
            wid_add(g, TXG_W_VOL, k, rc.vol - 4.f, yc - 14.f, RW_VOL + 8.f, 28.f);
        }
        {   /* M, S */
            float by = yc - 11.f;
            int m = (int)(g->mute >> k & 1u), s = (int)(g->solo >> k & 1u);
            if (m) RR(g, c, rc.ms, by, 22.f, 22.f, 3.f, C_INK); else RRS(g, c, rc.ms, by, 22.f, 22.f, 3.f, u ? C_LINE : 0x80262424u);
            TX(g, c, rc.ms + (22.f - TW(g, 11.f, 700.f, 0.f, 0, "M")) * 0.5f, yc + CAP(g, 11.f) * 0.5f, 11.f, 700.f, 0.f, 0,
               m ? C_PANEL : u ? C_DIM : C_FAINT, "M");
            if (s) RR(g, c, rc.ms + 26.f, by, 22.f, 22.f, 3.f, C_RED); else RRS(g, c, rc.ms + 26.f, by, 22.f, 22.f, 3.f, u ? C_LINE : 0x80262424u);
            TX(g, c, rc.ms + 26.f + (22.f - TW(g, 11.f, 700.f, 0.f, 0, "S")) * 0.5f, yc + CAP(g, 11.f) * 0.5f, 11.f, 700.f, 0.f, 0,
               s ? 0xFFFFFFFFu : u ? C_DIM : C_FAINT, "S");
            if (u) { wid_add(g, TXG_W_MUTE, k, rc.ms, by, 22.f, 22.f); wid_add(g, TXG_W_SOLO, k, rc.ms + 26.f, by, 22.f, 22.f); }
        }
    }
    if (g->nlane < (int)TXP_LANES && g->nlane >= g->rtop && g->nlane < g->rtop + g->rvis) {   /* ADD LANE */
        float y = y0 + ROW_H * (float)(g->nlane - g->rtop), yc = y + ROW_H * 0.5f;
        int hv = g->hover_kind == TXG_W_ADD;
        LN(g, c, rc.num + 1.f, yc, rc.num + 9.f, yc, 1.3f, hv ? C_INK : C_DIM);
        LN(g, c, rc.num + 5.f, yc - 4.f, rc.num + 5.f, yc + 4.f, 1.3f, hv ? C_INK : C_DIM);
        LBL(g, c, rc.name, yc + CAP(g, 10.5f) * 0.5f, 10.5f, hv ? C_INK : C_DIM, "ADD LANE");
        wid_add(g, TXG_W_ADD, -1, x0, y, rc.tl - x0 - 8.f, ROW_H);
    }
    frui_clip_reset(c);
    if (rows_total(g) > g->rvis) {       /* more rows than room: a scroll bar (the wheel scrolls) */
        float hh = g->dr_y - y0 - 8.f, th = hh * (float)g->rvis / (float)rows_total(g),
              ty = y0 + 4.f + (hh - th) * (float)g->rtop / (float)(rows_total(g) - g->rvis);
        RR(g, c, x1 - 5.f, y0 + 4.f, 3.f, hh, 1.5f, C_LINE);
        RR(g, c, x1 - 5.f, ty, 3.f, th, 1.5f, C_DIM);
    }
    if (any_used(g) && !g->sn.ui.any_run) {   /* all stopped (a landscape's start, REC): said, with the way out */
        const char *s = "ALL LANES STOPPED  \xC2\xB7  PLAY ALL, OR SET A LANE'S HOW MANY";
        float w = LBLW(g, 10.f, s) + 24.f, bx = rc.tl, by = g->dr_y - 30.f;
        if (bx + w > x1 - 12.f) { s = "ALL LANES STOPPED"; w = LBLW(g, 10.f, s) + 24.f; }
        RR(g, c, bx, by, w, 22.f, 3.f, C_BTN); RRS(g, c, bx, by, w, 22.f, 3.f, C_DIM);
        LBL(g, c, bx + 12.f, by + 11.f + CAP(g, 10.f) * 0.5f, 10.f, C_INK, s);
    }
}

/* ---- the drawer: the selected lane, its cards (name, value, what the card does), PAN, DIST, LEVEL, GROOVE */
static void draw_drawer(txg_t *g, frui_canvas *c)
{
    const float y0 = g->dr_y, x1 = g->W;
    int k = g->sel;
    const txp_ui_lane_t *l = &g->sn.ui.lane[k];
    const txp_ui_cards_t *cd = &g->sn.ui.card[k];
    char t[64], nm[32], a[16], b[16];
    float x = 16.f, cx0 = 236.f, cx1 = x1 - 196.f;
    R(g, c, 0, y0, x1, DR_H, C_DRAW);
    R(g, c, 0, y0, x1, 1.f, C_LINE);
    snprintf(t, sizeof t, "LANE %d%s%s", k + 1, used(g, k) ? "  \xC2\xB7  " : "", used(g, k) ? l->fam : "");
    upper(t, t, sizeof t);
    LBL(g, c, x, y0 + 21.f, 10.5f, C_RED, t);
    if (!used(g, k)) {
        float w = TX(g, c, x, y0 + 48.f, 22.f, 700.f, 1.1f, 0, C_FAINT, "EMPTY");
        caret(g, c, x + w + 10.f, y0 + 40.f, C_FAINT);
        wid_add(g, TXG_W_SND, k, x - 4.f, y0 + 26.f, w + 22.f, 28.f);
        TX(g, c, cx0, y0 + 40.f, 12.5f, 450.f, 0.f, 0, C_DIM, "No sound on this lane: click EMPTY (or its row's name) to choose one.");
        return;
    }
    upper(nm, l->snd, sizeof nm);
    clipL(g, c, x, y0, cx0 - x - 16.f, DR_H);
    {
        float w = TX(g, c, x, y0 + 48.f, 22.f, 700.f, 1.1f, 0, (g->mute >> k & 1u) ? C_DIM : C_INK, nm);
        frui_clip_reset(c);
        if (w > cx0 - x - 40.f) w = cx0 - x - 40.f;
        caret(g, c, x + w + 10.f, y0 + 40.f, C_DIM);
        wid_add(g, TXG_W_SND, k, x - 4.f, y0 + 26.f, w + 22.f, 28.f);
    }
    if (l->eng < g->cat->neng) {
        char en[24];
        upper(en, g->cat->eng[l->eng], sizeof en);
        snprintf(t, sizeof t, "%s COLOUR", en);
        CI(g, c, x + 5.f, y0 + 66.f, 4.5f, lane_col(g, k));
        RI(g, c, x + 5.f, y0 + 66.f, 7.f, 1.f, g->pick >= 0 && g->pick == l->eng ? C_INK : C_FAINT);
        clipL(g, c, x, y0 + 56.f, cx0 - x - 16.f, 20.f);
        LBL(g, c, x + 17.f, y0 + 66.f + CAP(g, 9.5f) * 0.5f, 9.5f, C_DIM, t);
        frui_clip_reset(c);
        wid_add2(g, TXG_W_COLDOT, k, (uint32_t)l->eng, 0, x - 4.f, y0 + 56.f, 24.f + LBLW(g, 9.5f, t), 20.f);
    }
    ptext(TXP_ID_LANE(k, TXP_PL_COUNT), g->cnt[k], a, sizeof a);
    ptext(TXP_ID_LANE(k, TXP_PL_ACTIVE), g->act[k], b, sizeof b);
    snprintf(t, sizeof t, "MANY %s  \xC2\xB7  ACTIVE %s  \xC2\xB7  VOL %d", a, b, g->level[k]);
    TX(g, c, x, y0 + 90.f, 10.5f, 450.f, 0.2f, FRUI_TNUM, l->run ? C_DIM : C_FAINT, t);
    if (l->ch) snprintf(t, sizeof t, "MIDI CH %d%s%s", l->ch, l->run ? "" : "  \xC2\xB7  STOPPED", (g->mute >> k & 1u) ? "  \xC2\xB7  MUTED" : "");
    else snprintf(t, sizeof t, "MIDI OFF%s%s", l->run ? "" : "  \xC2\xB7  STOPPED", (g->mute >> k & 1u) ? "  \xC2\xB7  MUTED" : "");
    TX(g, c, x, y0 + 106.f, 10.5f, 450.f, 0.2f, FRUI_TNUM, C_DIM, t);
    /* the cards: four a row */
    {
        float gap = 6.f, cw = (cx1 - cx0 - 3.f * gap) / 4.f, ch = (DR_H - 24.f - gap) / 2.f;
        if (!cd->n) TX(g, c, cx0, y0 + 40.f, 12.5f, 450.f, 0.f, 0, C_DIM, "This sound has no cards.");
        for (int i = 0; i < cd->n && i < (int)TXP_CARD_N; i++) {
            float xx = cx0 + (cw + gap) * (float)(i % 4), yy = y0 + 12.f + (ch + gap) * (float)(i / 4);
            pcell(g, c, xx, yy, cw, ch, TXP_ID_LANE(k, TXP_PL_CARD0 + i), cd->lab[i], card_hint(l->snd, cd->lab[i]));
        }
    }
    /* the lane's own: PAN, DIST, LEVEL, GROOVE's PART and CYCLE */
    {
        float xx = x1 - 182.f, yy = y0 + 10.f, w = 166.f, h = 17.f;
        pcell(g, c, xx, yy, w, h, TXP_ID_LANE(k, TXP_PL_PAN), "PAN", 0); yy += 20.f;
        if (l->dist != 0xFF) { pcell(g, c, xx, yy, w, h, TXP_ID_LANE(k, TXP_PL_CARD0 + l->dist), "DIST", 0); yy += 20.f; }
        pcell(g, c, xx, yy, w, h, TXP_ID_LANE(k, TXP_PL_LEVEL), "LEVEL", 0); yy += 20.f;
        pcell(g, c, xx, yy, w, h, TXP_ID_LANE(k, 48), "GROOVE PART", 0); yy += 20.f;
        pcell(g, c, xx, yy, w, h, TXP_ID_LANE(k, 49), "CYCLE", 0);
    }
}

/* ---- the strip: COMP, DRIVE, FILTER, DELAY, REVERB (the mix's chain, in its order), the movement */
static float hz_x(float v127) { return v127 / 127.f; }   /* the FILTER's steps are log Hz already (params.c F_HZ_*) */
static void viz(txg_t *g, frui_canvas *c, int sec, float x, float y, float w, float h, int on)
{
    uint32_t ink = on ? C_INK : C_FAINT;
    float lw2 = g->S < 1.5f ? 1.2f / g->S : 1.f, px = 0.f, py = 0.f;
    R(g, c, x, y + h - 1.f, w, 1.f, C_LINE);
    if (sec == SEC_COMP) {                /* the gain reduction now, under the threshold's mark */
        float gr = (float)g->sn.ui.comp_gr10 / 10.f, f = gr / 18.f, thr = (float)txp_ui_get(g->c, ID_CTHR) / 127.f;
        f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
        LBL(g, c, x, y + 12.f, 9.f, C_DIM, "GAIN REDUCTION");
        RR(g, c, x, y + 20.f, w, 6.f, 2.f, C_WELL);
        if (f > 0.f) RR(g, c, x + w * (1.f - f), y + 20.f, w * f, 6.f, 2.f, ink);
        {
            char t[16];
            snprintf(t, sizeof t, "-%.1f dB", gr);
            TX(g, c, x + w - TW(g, 10.5f, 500.f, 0.f, FRUI_TNUM, t), y + 12.f, 10.5f, 500.f, 0.f, FRUI_TNUM, ink, t);
        }
        for (int i = 0; i <= 24; i++) {  /* the curve: flat, then bent at THR */
            float a = (float)i / 24.f, b = a < 1.f - thr ? a : (1.f - thr) + (a - (1.f - thr)) * 0.3f, xx = x + a * w, yy = y + h - 3.f - b * (h - 34.f);
            if (i) LN(g, c, px, py, xx, yy, lw2, ink);
            px = xx; py = yy;
        }
    } else if (sec == SEC_DRIVE) {        /* its curve: the more AMOUNT, the harder the bend */
        float amt = (float)txp_ui_get(g->c, ID_DAMT) / 127.f, k = 1.f + amt * 8.f;
        for (int i = 0; i <= 32; i++) {
            float a = (float)i / 16.f - 1.f, b = tanhf(a * k) / tanhf(k), xx = x + (a + 1.f) * 0.5f * w, yy = y + h * 0.5f - b * (h * 0.42f);
            if (i) LN(g, c, px, py, xx, yy, lw2, ink);
            px = xx; py = yy;
        }
        LN(g, c, x, y + h * 0.5f, x + w, y + h * 0.5f, lw1(g), C_FAINT);
    } else if (sec == SEC_FILTER) {       /* the response: HIGH PASS then LOW PASS (their marks dashed; red: moved) */
        int hp = txp_ui_get(g->c, ID_FHP), lp = txp_ui_get(g->c, ID_FLP), sl = txp_ui_get(g->c, ID_FSLOPE), live;
        int mh = pmod(g, ID_FHP, &live), ml = pmod(g, ID_FLP, &live);
        float xh = x + w * hz_x((float)hp), xl = x + w * hz_x((float)lp), st = 0.10f + 0.06f * (float)sl;
        for (int i = 0; i <= 64; i++) {
            float a = (float)i / 64.f, xx = x + a * w, att = 0.f, yy;
            if (hp > 0 && a < hz_x((float)hp)) att += (hz_x((float)hp) - a) / st;
            if (lp < 127 && a > hz_x((float)lp)) att += (a - hz_x((float)lp)) / st;
            att = att > 1.f ? 1.f : att;
            yy = y + 8.f + att * (h - 12.f);
            if (i) LN(g, c, px, py, xx, yy, lw2, C_INK);
            px = xx; py = yy;
        }
        if (hp > 0) dashed(g, c, xh, y + 2.f, y + h - 2.f, mh ? C_RED : C_DIM);
        if (lp < 127) dashed(g, c, xl, y + 2.f, y + h - 2.f, ml ? C_RED : C_DIM);
        TX(g, c, x, y + h - 4.f, 8.5f, 450.f, 0.f, 0, C_FAINT, "20");
        TX(g, c, x + w - TW(g, 8.5f, 450.f, 0.f, 0, "20k"), y + h - 4.f, 8.5f, 450.f, 0.f, 0, C_FAINT, "20k");
    } else if (sec == SEC_DELAY) {        /* the echoes: spaced by TIME, fading by FEEDBACK */
        float tm = (float)txp_ui_get(g->c, ID_GTIME) / 133.f, fb = (float)txp_ui_get(g->c, ID_GFB) / 127.f,
              mix = (float)txp_ui_get(g->c, ID_GMIX) / 127.f, sp = 6.f + tm * 26.f, a = 1.f;
        for (float xx = x + 4.f; xx < x + w && a > 0.04f; xx += sp) {
            float hh = (h - 10.f) * a;
            R(g, c, xx, y + h - 2.f - hh, 2.f, hh, frui_alpha(ink, 0.25f + 0.75f * (xx == x + 4.f ? 1.f : mix)));
            a *= 0.35f + 0.6f * fb;
        }
    } else if (sec == SEC_REVERB) {       /* its tail: as long as DECAY */
        float dc = (float)txp_ui_get(g->c, ID_RDECAY) / 127.f;
        px = x; py = y + 6.f;
        for (int i = 1; i <= 40; i++) {
            float a = (float)i / 40.f, e = expf(-a * (7.f - 6.f * dc)), xx = x + a * w, yy = y + h - 2.f - e * (h - 8.f);
            LN(g, c, px, py, xx, yy, lw2, ink);
            px = xx; py = yy;
        }
    }
}
static int sec_on(const txg_t *g, int s)  /* the section does something (its dot lit) */
{
    switch (s) {
    case SEC_COMP: return txp_ui_get(g->c, ID_CON) > 0;
    case SEC_DRIVE: return txp_ui_get(g->c, ID_DAMT) > 0 && txp_ui_get(g->c, ID_DMIX) > 0;
    case SEC_FILTER: return txp_ui_get(g->c, ID_FHP) > 0 || txp_ui_get(g->c, ID_FLP) < 127;
    case SEC_DELAY: return txp_ui_get(g->c, ID_GMIX) > 0;
    case SEC_REVERB: return txp_ui_get(g->c, ID_RMIX) > 0;
    }
    return g->sn.ui.lfo_on != 0;
}
static void lfo_trace(txg_t *g, frui_canvas *c, int n, float x, float y, float w, float h, uint32_t col)
{
    float px = 0.f, py = 0.f, lw = g->S < 1.5f ? 1.f / g->S : 0.9f;
    R(g, c, x, y + h - 1.f, w, 1.f, C_FAINT);
    for (int i = 0; i < LFO_H; i++) {
        float xx = x + w * (float)i / (float)(LFO_H - 1), yy = y + h - 2.f - ((float)g->lfoh[n][i] / 32767.f * 0.5f + 0.5f) * (h - 4.f);
        if (i) LN(g, c, px, py, xx, yy, lw * 1.2f, col);
        px = xx; py = yy;
    }
    CI(g, c, px, py, 2.2f, col);
}
static void lfo_route(const txg_t *g, int n, char *o, size_t on)   /* LFO n's routes: "L3 PAN +60 %" ("+2": more) */
{
    int first = -1, more = 0;
    o[0] = 0;
    for (int s = 0; s < 16; s++) {
        int is = txp_param_index(ID_MAT(s, 0)), it = txp_param_index(ID_MAT(s, 1));
        if (is < 0 || it < 0 || g->sn.pv[is] != n + 1 || !g->sn.pv[it]) continue;
        if (first < 0) first = s; else more++;
    }
    if (first >= 0) {
        int it = txp_param_index(ID_MAT(first, 1)), id = txp_param_index(ID_MAT(first, 2));
        size_t l;
        snprintf(o, on, "%s  %s", g->sn.ptx[it], id >= 0 ? g->sn.ptx[id] : "");
        l = strlen(o);
        if (more) snprintf(o + l, on - l, "  +%d", more);
    } else snprintf(o, on, "%s", g->sn.ui.lfo_to[n]);
}
static void draw_strip(txg_t *g, frui_canvas *c)
{
    const float y0 = g->fx_y;
    float x = 0.f, tot = 0.f;
    for (int s = 0; s < SEC_N; s++) tot += SEC_WT[s];
    R(g, c, 0, y0, g->W, FX_H, C_PANEL);
    R(g, c, 0, y0, g->W, 1.f, C_LINE);
    for (int s = 0; s < SEC_N; s++) {
        float w = g->W * SEC_WT[s] / tot, ix = x + 12.f, iw = w - 24.f, hx;
        int on = sec_on(g, s), open = g->ov == OV_FX && (s < SEC_MOVE ? g->fx_tab == s : g->fx_tab >= TAB_LFO0);
        if (s) R(g, c, x, y0, 1.f, FX_H, C_LINE);
        if (open) { R(g, c, x + 1.f, y0 + 1.f, w - 1.f, FX_H - 1.f, C_SEL); R(g, c, x + 1.f, y0 + 1.f, w - 1.f, 2.f, C_RED); }
        /* the head: its dot, its name (a click: every parameter of it), "ALL" */
        CI(g, c, ix + 2.5f, y0 + 17.f, 2.6f, on ? (s == SEC_MOVE ? C_RED : C_INK) : C_FAINT);
        hx = ix + 11.f + LBL(g, c, ix + 11.f, y0 + 17.f + CAP(g, 12.f) * 0.5f, 12.f, on ? C_INK : C_DIM, SEC_NAME[s]);
        LBL(g, c, x + w - 12.f - LBLW(g, 9.f, "ALL \xE2\x80\xBA"), y0 + 17.f + CAP(g, 9.f) * 0.5f, 9.f, C_DIM, "ALL \xE2\x80\xBA");
        wid_add(g, TXG_W_FXOPEN, s, x + 1.f, y0 + 4.f, w - 2.f, 26.f);
        if (s == SEC_COMP) {             /* (COMP's own switch: its ON) */
            const char *t = on ? "ON" : "OFF";
            float tw = LBLW(g, 9.f, t) + 12.f, tx = hx + 10.f;
            RR(g, c, tx, y0 + 9.f, tw, 16.f, 2.f, on ? C_SEG : C_WELL); RRS(g, c, tx, y0 + 9.f, tw, 16.f, 2.f, C_LINE);
            LBL(g, c, tx + 6.f, y0 + 17.f + CAP(g, 9.f) * 0.5f, 9.f, on ? C_INK : C_DIM, t);
            wid_add2(g, TXG_W_PARAM, (int)ID_CON, ID_CON, 0, tx, y0 + 9.f, tw, 16.f);
        }
        if (s < SEC_MOVE) {
            static const uint32_t K[5][4] = { { ID_CTHR, ID_CRATIO, 0, 0 }, { ID_DAMT, ID_DTYPE, 0, 0 },
                                              { ID_FHP, ID_FLP, ID_FRES, ID_FSLOPE }, { ID_GMIX, ID_GTIME, 0, 0 },
                                              { ID_RTYPE, ID_RMIX, 0, 0 } };
            viz(g, c, s, ix, y0 + 32.f, iw, 42.f, on);
            if (s == SEC_FILTER) {
                float hw = (iw - 6.f) * 0.5f;
                pcell(g, c, ix, y0 + 82.f, hw, 20.f, K[s][0], "HIGH PASS", 0);
                pcell(g, c, ix + hw + 6.f, y0 + 82.f, hw, 20.f, K[s][1], "LOW PASS", 0);
                pcell(g, c, ix, y0 + 108.f, hw, 20.f, K[s][2], 0, 0);
                pcell(g, c, ix + hw + 6.f, y0 + 108.f, hw, 20.f, K[s][3], 0, 0);
            } else {
                pcell(g, c, ix, y0 + 82.f, iw, 20.f, K[s][0], 0, 0);
                pcell(g, c, ix, y0 + 108.f, iw, 20.f, K[s][1], 0, 0);
            }
        } else {                         /* the movement: the LFOs routed (else LFO 1, 2), each traced, its RATE */
            int shown = 0, more = 0, pick[2] = { -1, -1 };
            for (int n = 0; n < (int)TXP_LFO_N; n++)
                if (g->sn.ui.lfo_on >> n & 1u) { if (shown < 2) pick[shown++] = n; else more++; }
            for (int n = 0; shown < 2 && n < (int)TXP_LFO_N; n++) if (n != pick[0]) pick[shown++] = n;
            if (more) {
                char t[8];
                snprintf(t, sizeof t, "+%d", more);
                LBL(g, c, hx + 8.f, y0 + 17.f + CAP(g, 10.f) * 0.5f, 10.f, C_RED, t);
            }
            for (int i = 0; i < 2; i++) {
                int n = pick[i], routed = (int)(g->sn.ui.lfo_on >> n & 1u);
                float yy = y0 + 36.f + 52.f * (float)i;
                char t[64], sh[24], rt[48];
                int si = txp_param_index(ID_LFO(n, LP_SHAPE));
                snprintf(sh, sizeof sh, "%s", si >= 0 ? g->sn.ptx[si] : "");
                snprintf(t, sizeof t, "LFO %d  \xC2\xB7  %s", n + 1, sh);   /* (its number and shape, as the mockup) */
                clipL(g, c, ix, yy, iw, 14.f);
                LBL(g, c, ix, yy + 9.f, 9.5f, routed ? C_RED : C_DIM, t);
                frui_clip_reset(c);
                lfo_trace(g, c, n, ix, yy + 13.f, iw * 0.5f - 6.f, 20.f, routed ? C_RED : C_DIM);
                pcell(g, c, ix + iw * 0.5f, yy + 14.f, iw * 0.5f, 18.f, ID_LFO(n, LP_RATE), "RATE", 0);
                lfo_route(g, n, rt, sizeof rt);   /* its route: the target, the depth */
                clipL(g, c, ix, yy + 34.f, iw, 14.f);
                TX(g, c, ix, yy + 44.f, 10.f, 600.f, 0.2f, FRUI_TNUM, routed ? C_RED : C_FAINT, routed ? rt : "not routed");
                frui_clip_reset(c);
                wid_add2(g, TXG_W_FXTAB, TAB_LFO0 + n, 0, 0, ix, yy, iw * 0.5f - 6.f, 46.f);
            }
        }
        x += w;
    }
}

/* ---- the overlays: a header (title, the search, CLOSE), then their bodies */
static void ov_header(txg_t *g, frui_canvas *c, const char *title, const char *red, int search, const char *ph)
{
    float y = TOP_H, x;
    R(g, c, 0, y, g->W, g->H - y, C_OVL);
    R(g, c, 0, y + OV_HDR - 1.f, g->W, 1.f, C_LINE);
    x = 18.f + TX(g, c, 18.f, y + 23.f + CAP(g, 17.f) * 0.5f, 17.f, 700.f, 2.4f, 0, C_INK, title);
    if (red) TX(g, c, x + 8.f, y + 23.f + CAP(g, 17.f) * 0.5f, 17.f, 700.f, 2.4f, 0, C_RED, red);
    if (search) {                        /* the search: what is typed (the keys go to it while the browser is open) */
        float sw = 280.f, sx = g->W - 98.f - 14.f - sw;
        RR(g, c, sx, y + 10.f, sw, 26.f, 3.f, C_WELL); RRS(g, c, sx, y + 10.f, sw, 26.f, 3.f, g->q[0] ? C_DIM : C_LINE);
        if (g->q[0]) {
            float w = TX(g, c, sx + 10.f, y + 23.f + CAP(g, 12.f) * 0.5f, 12.f, 500.f, 0.3f, 0, C_INK, g->q);
            R(g, c, sx + 11.f + w, y + 16.f, 1.2f, 14.f, C_RED);
        } else TX(g, c, sx + 10.f, y + 23.f + CAP(g, 12.f) * 0.5f, 12.f, 450.f, 0.3f, 0, C_DIM, ph);
        wid_add(g, TXG_W_SEARCH, -1, sx, y + 10.f, sw, 26.f);
    }
    x = g->W - 98.f;
    button(g, c, x, y + 10.f, 80.f, 26.f, "CLOSE", 0);
    wid_add(g, TXG_W_CLOSE, -1, x, y + 10.f, 80.f, 26.f);
}
static void nav_item(txg_t *g, frui_canvas *c, float y, const char *t, int n, int on, int lane)
{
    char b[8];
    if (on) { R(g, c, 0, y, OV_NAV - 1.f, 28.f, C_NAV); R(g, c, 0, y, 2.f, 28.f, C_RED); }
    TX(g, c, 16.f, y + 14.f + CAP(g, 12.5f) * 0.5f, 12.5f, 600.f, 0.f, 0, on ? C_INK : C_DIM, t);
    snprintf(b, sizeof b, "%d", n);
    TX(g, c, OV_NAV - 16.f - TW(g, 11.f, 450.f, 0.f, FRUI_TNUM, b), y + 14.f + CAP(g, 11.f) * 0.5f, 11.f, 450.f, 0.f, FRUI_TNUM, C_DIM, b);
    wid_add(g, TXG_W_NAV, lane, 0, y, OV_NAV - 1.f, 28.f);
}
static void nav_group(txg_t *g, frui_canvas *c, float y, const char *t) { LBL(g, c, 16.f, y + 18.f, 10.5f, frui_alpha(C_DIM, 0.8f), t); }
static void swatches(txg_t *g, frui_canvas *c, float x, float y, int eng)   /* an engine's colour, in a row */
{
    int cur = txp_ui_col(g->c, eng);
    for (int i = 0; i < TXP_COL_N; i++) {
        float cx = x + 7.f + (float)i * 21.f, cy = y + 7.f;
        if (i == cur) RI(g, c, cx, cy, 10.f, 1.2f, C_INK);
        CI(g, c, cx, cy, 7.f, PAL[i]);
        RI(g, c, cx, cy, 7.f, 0.8f, 0x14FFFFFFu);
        wid_add2(g, TXG_W_SWATCH, i, (uint32_t)eng, i, cx - 10.f, cy - 10.f, 20.f, 20.f);
    }
}
static int lane_of_snd(const txg_t *g, int f, int i)   /* the lanes playing family f's sound i: a mask */
{
    int m = 0;
    for (int k = 0; k < g->nlane; k++) if (used(g, k) && g->sn.ui.lane[k].fi == f && g->sn.ui.lane[k].si == i) m |= 1 << k;
    return m;
}
static void list_view(txg_t *g, int cur, float rows)   /* the window over the list kept round the cursor */
{
    int vis = (int)rows;
    if (cur < 0) return;
    if (cur < g->sb_top) g->sb_top = cur;
    if (cur >= g->sb_top + vis) g->sb_top = cur - vis + 1;
}
static void sb_items(txg_t *g)           /* the sound browser's list: the family's sounds, or the search's (f << 4 | i) */
{
    g->nitems = 0;
    if (g->q[0]) {
        for (int f = 0; f < g->cat->nfam; f++)
            for (int i = 0; i < g->cat->fam[f].n && g->nitems < LIST_MAX; i++)
                if (has_q(g->cat->fam[f].s[i].name, g->q) || has_q(g->cat->fam[f].name, g->q) ||
                    has_q(g->cat->eng[g->cat->fam[f].s[i].eng < g->cat->neng ? g->cat->fam[f].s[i].eng : 0], g->q))
                    g->items[g->nitems++] = f << 4 | i;
    } else
        for (int i = 0; i < g->cat->fam[g->sb_fam].n && g->nitems < LIST_MAX; i++) g->items[g->nitems++] = g->sb_fam << 4 | i;
}
static int sb_auditioning(const txg_t *g)   /* the lane plays the cursor's sound, not its kept one */
{
    const txp_ui_lane_t *l = &g->sn.ui.lane[g->sb_lane];
    return l->used && l->fi == g->sb_cf && l->si == g->sb_ci && !(g->sb_cf == g->sb_kf && g->sb_ci == g->sb_ki);
}
static void draw_sounds(txg_t *g, frui_canvas *c)
{
    char t[64], nm[32];
    int k = g->sb_lane, tot = 0, cur = -1;
    float y, lx = OV_NAV, lw = g->W - OV_NAV - OV_PREV, rows = (g->H - OV_Y0 - 12.f) / OV_RH;
    for (int f = 0; f < g->cat->nfam; f++) tot += g->cat->fam[f].n;
    snprintf(t, sizeof t, "LANE %d", k + 1);
    snprintf(nm, sizeof nm, "Type to search %d sounds", tot);
    ov_header(g, c, "SOUND FOR", t, 1, nm);
    if (sb_auditioning(g)) {
        char a[32];
        float x = 18.f + TW(g, 17.f, 700.f, 2.4f, 0, "SOUND FOR") + 8.f + TW(g, 17.f, 700.f, 2.4f, 0, t) + 28.f;
        upper(a, snd_name(g, g->sb_cf, g->sb_ci), sizeof a);
        snprintf(nm, sizeof nm, "HEARING ON LANE %d: %s", k + 1, a);
        CI(g, c, x, TOP_H + 23.f, 3.f, C_RED);
        TX(g, c, x + 9.f, TOP_H + 23.f + CAP(g, 11.f) * 0.5f, 11.f, 500.f, 0.3f, 0, C_INK, nm);
    }
    /* the families */
    R(g, c, OV_NAV - 1.f, OV_Y0, 1.f, g->H - OV_Y0, C_LINE);
    for (int f = 0; f < g->cat->nfam; f++) nav_item(g, c, OV_Y0 + 6.f + 28.f * (float)f, g->cat->fam[f].name, g->cat->fam[f].n, !g->q[0] && f == g->sb_fam, f);
    /* the sounds */
    sb_items(g);
    for (int j = 0; j < g->nitems; j++) if (g->items[j] == (g->sb_cf << 4 | g->sb_ci)) cur = j;
    list_view(g, cur, rows);
    if (g->sb_top > g->nitems - (int)rows) g->sb_top = g->nitems - (int)rows;
    if (g->sb_top < 0) g->sb_top = 0;
    clipL(g, c, lx, OV_Y0, lw, g->H - OV_Y0);
    if (!g->nitems) TX(g, c, lx + 16.f, OV_Y0 + 30.f, 12.5f, 450.f, 0.f, 0, C_DIM, "No sound has that name.");
    for (int j = g->sb_top; j < g->nitems && (float)(j - g->sb_top) < rows; j++) {
        int f = g->items[j] >> 4, i = g->items[j] & 15, m = lane_of_snd(g, f, i), e = snd_eng(g, f, i);
        int kept = f == g->sb_kf && i == g->sb_ki, cu = j == cur;
        float ry = OV_Y0 + 4.f + OV_RH * (float)(j - g->sb_top), yc = ry + OV_RH * 0.5f, x;
        if (g->hover_kind == TXG_W_LIST && g->hover_lane == j) R(g, c, lx, ry, lw, OV_RH, C_HOV);
        if (kept) { R(g, c, lx, ry, lw, OV_RH, C_NAV); R(g, c, lx, ry, 2.f, OV_RH, C_RED); }
        if (cu) RRS(g, c, lx + 1.f, ry + 1.f, lw - 2.f, OV_RH - 2.f, 2.f, C_DIM);
        snprintf(t, sizeof t, "%02d", i + 1);
        TX(g, c, lx + 16.f, yc + CAP(g, 11.f) * 0.5f, 11.f, 450.f, 0.f, FRUI_TNUM, kept ? C_RED : C_DIM, t);
        CI(g, c, lx + 52.f, yc, 3.5f, eng_col(g, e));
        cap_name(nm, snd_name(g, f, i), sizeof nm);
        TX(g, c, lx + 64.f, yc + CAP(g, 13.f) * 0.5f, 13.f, 500.f, 0.f, 0, C_INK, nm);
        if (g->q[0]) TX(g, c, lx + 250.f, yc + CAP(g, 11.f) * 0.5f, 11.f, 450.f, 0.3f, 0, C_DIM, g->cat->fam[f].name);
        x = lx + lw - 16.f;
        for (int kk = g->nlane - 1; kk >= 0; kk--) {   /* the lanes playing it: tags */
            float tw;
            if (!(m >> kk & 1)) continue;
            snprintf(t, sizeof t, "L%d", kk + 1);
            tw = TW(g, 10.f, 500.f, 0.f, FRUI_TNUM, t) + 8.f;
            x -= tw;
            RRS(g, c, x, yc - 8.f, tw, 16.f, 2.f, kk == k ? C_RED : C_LINE);
            TX(g, c, x + 4.f, yc + CAP(g, 10.f) * 0.5f, 10.f, 500.f, 0.f, FRUI_TNUM, kk == k ? C_INK : C_DIM, t);
            x -= 4.f;
        }
        if (e >= 0 && e < g->cat->neng) TX(g, c, x - 8.f - TW(g, 11.f, 450.f, 0.2f, 0, g->cat->eng[e]), yc + CAP(g, 11.f) * 0.5f, 11.f, 450.f, 0.2f, 0, C_DIM, g->cat->eng[e]);
        wid_add(g, TXG_W_LIST, j, lx, ry, lw, OV_RH);
    }
    frui_clip_reset(c);
    /* the preview: the cursor's sound, its engine's colour, KEEP / CLOSE */
    {
        float px = g->W - OV_PREV + 16.f, pw = OV_PREV - 32.f;
        int e = snd_eng(g, g->sb_cf, g->sb_ci);
        R(g, c, g->W - OV_PREV, OV_Y0, 1.f, g->H - OV_Y0, C_LINE);
        y = OV_Y0 + 24.f;
        snprintf(t, sizeof t, "%s  \xC2\xB7  ENGINE %s", g->sb_cf >= 0 ? g->cat->fam[g->sb_cf].name : "", e >= 0 && e < g->cat->neng ? g->cat->eng[e] : "");
        upper(t, t, sizeof t);
        LBL(g, c, px, y, 10.5f, C_DIM, t);
        upper(nm, snd_name(g, g->sb_cf, g->sb_ci), sizeof nm);
        clipL(g, c, px, y, pw, 50.f);
        TX(g, c, px, y + 36.f, 28.f, 700.f, 1.1f, 0, C_INK, nm);
        frui_clip_reset(c);
        y += 62.f;
        snprintf(t, sizeof t, "Moving through the list plays each sound on lane %d", k + 1);
        TX(g, c, px, y, 12.f, 450.f, 0.f, 0, C_DIM, t);
        TX(g, c, px, y + 17.f, 12.f, 450.f, 0.f, 0, C_DIM, "while it plays, as on the FM-1. KEEP to stay;");
        TX(g, c, px, y + 34.f, 12.f, 450.f, 0.f, 0, C_DIM, "CLOSE puts the old sound back.");
        y += 70.f;
        if (e >= 0) {
            char en[24];
            upper(en, g->cat->eng[e], sizeof en);
            snprintf(t, sizeof t, "%s COLOUR, IN EVERY PRESET", en);
            LBL(g, c, px, y, 10.5f, C_DIM, t);
            swatches(g, c, px, y + 10.f, e);
        }
        y = g->H - 56.f;
        snprintf(t, sizeof t, "KEEP ON LANE %d", k + 1);
        button(g, c, px, y, 150.f, 30.f, t, 1);
        wid_add(g, TXG_W_KEEP, -1, px, y, 150.f, 30.f);
        button(g, c, px + 160.f, y, 90.f, 30.f, "CLOSE", 0);
        wid_add(g, TXG_W_CLOSE, -1, px + 160.f, y, 90.f, 30.f);
    }
}
/* ---- the presets browser: USER (when any), the factory's categories, the landscapes; the cursor's preset previewed
 * (its lanes and where they stand), LOAD loads it (the FM-1's EDIT; again on the one loaded, with its effects when
 * EFFECTS ON LOAD is SMART and they were changed) */
static void pb_items(txg_t *g)
{
    const txp_catalog_t *ct = g->cat;
    g->nitems = 0;
    if (g->q[0]) {
        for (int i = 0; i < ct->npr && g->nitems < LIST_MAX; i++) if (has_q(ct->pr[i], g->q)) g->items[g->nitems++] = i;
        for (int u = 0; u < (int)TXP_USER_N && g->nitems < LIST_MAX; u++)
            if ((g->sn.ui.user >> u & 1u) && has_q(g->sn.ui.uname[u], g->q)) g->items[g->nitems++] = ct->npr + u;
        for (int f = 0; f < ct->nfam && g->nitems < LIST_MAX; f++) if (has_q(ct->fam[f].name, g->q)) g->items[g->nitems++] = TXP_LOAD_LS + f;
    } else if (g->pb_bank == PB_USER) {
        for (int u = 0; u < (int)TXP_USER_N; u++) if (g->sn.ui.user >> u & 1u) g->items[g->nitems++] = ct->npr + u;
    } else if (g->pb_bank == PB_LAND) {
        for (int f = 0; f < ct->nfam; f++) g->items[g->nitems++] = TXP_LOAD_LS + f;
    } else if (g->pb_bank >= 0 && g->pb_bank < ct->ncat)
        for (int i = ct->cat_at[g->pb_bank]; i < ct->cat_at[g->pb_bank + 1] && g->nitems < LIST_MAX; i++) g->items[g->nitems++] = i;
}
static void pb_code(const txg_t *g, int v, char *code, size_t nc, char *name, size_t nn, int *lanes)
{
    if (v >= TXP_LOAD_LS) {
        snprintf(code, nc, "L%02d", v - TXP_LOAD_LS + 1);
        upper(name, g->cat->fam[v - TXP_LOAD_LS].name, nn);
        *lanes = 4;
    } else if (v >= g->cat->npr) {
        snprintf(code, nc, "U%02d", v - g->cat->npr + 1);
        snprintf(name, nn, "%s", g->sn.ui.uname[(v - g->cat->npr) & 15]);
        *lanes = 0;
    } else {
        snprintf(code, nc, "F%03d", v + 1);
        snprintf(name, nn, "%s", g->cat->pr[v]);
        *lanes = 0;
        for (int k = 0; k < 4; k++) if (g->cat->prl[v][k].fi != 0xFF) (*lanes)++;
    }
}
static void seg_param(txg_t *g, frui_canvas *c, float x, float y, float wmax, uint32_t id, float *yo)   /* a choice's
                                          * segments (wrapped to wmax) */
{
    int hi = txp_ui_hi(id), v = txp_ui_get(g->c, id);
    float xx = x, yy = y, h = 22.f;
    for (int i = 0; i <= hi; i++) {
        char t[24];
        float w;
        ptext(id, i, t, sizeof t);
        w = LBLW(g, 10.5f, t) + 20.f;
        if (xx + w > x + wmax && xx > x) { xx = x; yy += h + 4.f; }
        RR(g, c, xx, yy, w, h, 2.f, i == v ? C_SEG : C_PANEL);
        RRS(g, c, xx, yy, w, h, 2.f, C_LINE);
        if (i == v) R(g, c, xx + 1.f, yy + h - 2.f, w - 2.f, 2.f, C_RED);
        LBL(g, c, xx + 10.f, yy + h * 0.5f + CAP(g, 10.5f) * 0.5f, 10.5f, i == v ? C_INK : C_DIM, t);
        wid_add2(g, TXG_W_SEG, (int)(id & 0xFFFFu) << 8 | i, id, i, xx, yy, w, h);
        xx += w + 4.f;
    }
    if (yo) *yo = yy + h;
}
static void draw_presets(txg_t *g, frui_canvas *c)
{
    const txp_catalog_t *ct = g->cat;
    char t[64], code[8], name[24];
    float y, lx = OV_NAV, lw = g->W - OV_NAV - OV_PREV, rows = (g->H - OV_Y0 - 12.f) / OV_RH;
    int cur = -1, nu = 0, loaded = g->sn.ui.scene != 0xFF ? g->sn.ui.scene : TXP_LOAD_LS + g->sn.ui.land;
    for (int u = 0; u < (int)TXP_USER_N; u++) nu += (int)(g->sn.ui.user >> u & 1u);
    ov_header(g, c, "PRESETS", 0, 1, "Type to search presets");
    R(g, c, OV_NAV - 1.f, OV_Y0, 1.f, g->H - OV_Y0, C_LINE);
    y = OV_Y0 + 2.f;
    if (nu) { nav_group(g, c, y, "MINE"); y += 26.f; nav_item(g, c, y, "User", nu, !g->q[0] && g->pb_bank == PB_USER, PB_USER); y += 28.f; }
    nav_group(g, c, y, "FACTORY"); y += 26.f;
    for (int i = 0; i < ct->ncat; i++) {
        char nm[16];
        cap_name(nm, ct->cat[i], sizeof nm);
        for (char *q = nm + 1; *q; q++) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32);
        nav_item(g, c, y, nm, ct->cat_at[i + 1] - ct->cat_at[i], !g->q[0] && g->pb_bank == i, i);
        y += 28.f;
    }
    nav_group(g, c, y, "START FROM"); y += 26.f;
    nav_item(g, c, y, "Landscapes", ct->nfam, !g->q[0] && g->pb_bank == PB_LAND, PB_LAND);
    /* the list */
    pb_items(g);
    for (int j = 0; j < g->nitems; j++) if (g->items[j] == g->pb_cur) cur = j;
    {
        int top = g->pb_top, vis = (int)rows;
        if (cur >= 0 && cur < top) top = cur;
        if (cur >= top + vis) top = cur - vis + 1;
        if (top > g->nitems - vis) top = g->nitems - vis;
        g->pb_top = top < 0 ? 0 : top;
    }
    clipL(g, c, lx, OV_Y0, lw, g->H - OV_Y0);
    if (!g->nitems) TX(g, c, lx + 16.f, OV_Y0 + 30.f, 12.5f, 450.f, 0.f, 0, C_DIM, g->q[0] ? "No preset has that name." : "Nothing here yet.");
    for (int j = g->pb_top; j < g->nitems && (float)(j - g->pb_top) < rows; j++) {
        int v = g->items[j], lanes, isl = v == loaded;
        float ry = OV_Y0 + 4.f + OV_RH * (float)(j - g->pb_top), yc = ry + OV_RH * 0.5f;
        pb_code(g, v, code, sizeof code, name, sizeof name, &lanes);
        if (g->hover_kind == TXG_W_LIST && g->hover_lane == j) R(g, c, lx, ry, lw, OV_RH, C_HOV);
        if (isl) { R(g, c, lx, ry, lw, OV_RH, C_NAV); R(g, c, lx, ry, 2.f, OV_RH, C_RED); }
        if (j == cur) RRS(g, c, lx + 1.f, ry + 1.f, lw - 2.f, OV_RH - 2.f, 2.f, C_DIM);
        TX(g, c, lx + 16.f, yc + CAP(g, 11.f) * 0.5f, 11.f, 450.f, 0.f, FRUI_TNUM, isl ? C_RED : C_DIM, code);
        TX(g, c, lx + 68.f, yc + CAP(g, 13.f) * 0.5f, 13.f, 500.f, 0.3f, 0, C_INK, name);
        if (lanes) {
            snprintf(t, sizeof t, "%d LANE%s", lanes, lanes == 1 ? "" : "S");
            TX(g, c, lx + lw - 16.f - TW(g, 11.f, 450.f, 0.3f, FRUI_TNUM, t), yc + CAP(g, 11.f) * 0.5f, 11.f, 450.f, 0.3f, FRUI_TNUM, C_DIM, t);
        }
        wid_add(g, TXG_W_LIST, j, lx, ry, lw, OV_RH);
    }
    frui_clip_reset(c);
    /* the preview */
    {
        float px = g->W - OV_PREV + 16.f, pw = OV_PREV - 32.f, mh = 120.f;
        int v = g->pb_cur, lanes = 0;
        R(g, c, g->W - OV_PREV, OV_Y0, 1.f, g->H - OV_Y0, C_LINE);
        y = OV_Y0 + 24.f;
        if (v >= 0) {
            int fi[4], si[4], cn[4], ac[4], pn[4];
            pb_code(g, v, code, sizeof code, name, sizeof name, &lanes);
            snprintf(t, sizeof t, "%s  \xC2\xB7  %s", code, v >= TXP_LOAD_LS ? "A LANDSCAPE'S START (STOPPED, AS ON THE FM-1)" : v >= ct->npr ? "YOURS" : "");
            if (v < ct->npr) for (int i = 0; i < ct->ncat; i++) if (v >= ct->cat_at[i] && v < ct->cat_at[i + 1]) snprintf(t, sizeof t, "%s  \xC2\xB7  %s", code, ct->cat[i]);
            clipL(g, c, px, y - 12.f, pw, 16.f);
            LBL(g, c, px, y, 10.5f, C_DIM, t);
            frui_clip_reset(c);
            clipL(g, c, px, y, pw, 50.f);
            TX(g, c, px, y + 36.f, 28.f, 700.f, 1.1f, 0, C_INK, name);
            frui_clip_reset(c);
            y += 52.f;
            for (int k = 0; k < 4; k++) {   /* its lanes */
                fi[k] = si[k] = -1; cn[k] = ac[k] = 0; pn[k] = 64;
                if (v < ct->npr && ct->prl[v][k].fi != 0xFF) { fi[k] = ct->prl[v][k].fi; si[k] = ct->prl[v][k].si; cn[k] = ct->prl[v][k].cnt; ac[k] = ct->prl[v][k].act; pn[k] = ct->prl[v][k].pan; }
                else if (v >= TXP_LOAD_LS && k < ct->fam[v - TXP_LOAD_LS].n) { fi[k] = v - TXP_LOAD_LS; si[k] = k; pn[k] = 64; }
            }
            if (v >= ct->npr && v < TXP_LOAD_LS) {
                TX(g, c, px, y + 12.f, 12.f, 450.f, 0.f, 0, C_DIM, "A preset you saved: its lanes as you left them.");
                y += 30.f;
            } else {
                /* a small map: you at the bottom, each lane at its PAN (the distance its sound's own) */
                float cx = px + pw * 0.5f, cy = y + mh - 10.f, rr = mh - 24.f;
                RR(g, c, px, y, pw, mh, 2.f, C_WELL);
                for (int r = 1; r <= 3; r++) {
                    float ppx = cx - rr * (float)r / 3.f, ppy = cy;
                    for (int i = 1; i <= 32; i++) {
                        float a = 3.14159265f * (1.f - (float)i / 32.f), xx = cx + cosf(a) * rr * (float)r / 3.f, yy = cy - sinf(a) * rr * (float)r / 3.f;
                        LN(g, c, ppx, ppy, xx, yy, 0.75f, C_FAINT);
                        ppx = xx; ppy = yy;
                    }
                }
                CI(g, c, cx, cy, 2.5f, C_INK);
                for (int k = 0; k < 4; k++) {
                    float a, xx, yy;
                    if (fi[k] < 0) continue;
                    a = (float)(pn[k] - 64) / 64.f * MAP_ANG;
                    xx = cx + sinf(a) * rr * 0.62f; yy = cy - cosf(a) * rr * 0.62f;
                    CI(g, c, xx, yy, 3.5f, eng_col(g, snd_eng(g, fi[k], si[k])));
                    snprintf(t, sizeof t, "%d", k + 1);
                    TX(g, c, xx + 6.f, yy + 4.f, 10.f, 600.f, 0.f, 0, C_DIM, t);
                }
                y += mh + 12.f;
                for (int k = 0; k < 4; k++) {   /* the plan: a lane a line */
                    char nm[32];
                    float yc = y + 9.f;
                    snprintf(t, sizeof t, "%d", k + 1);
                    TX(g, c, px, yc + CAP(g, 11.f) * 0.5f, 11.f, 450.f, 0.f, FRUI_TNUM, C_DIM, t);
                    if (fi[k] >= 0) {
                        CI(g, c, px + 18.f, yc, 3.5f, eng_col(g, snd_eng(g, fi[k], si[k])));
                        cap_name(nm, snd_name(g, fi[k], si[k]), sizeof nm);
                        TX(g, c, px + 28.f, yc + CAP(g, 12.f) * 0.5f, 12.f, 500.f, 0.f, 0, C_INK, nm);
                        {
                            char hm[16], ha[16];
                            ptext(TXP_ID_LANE(0, TXP_PL_COUNT), cn[k], hm, sizeof hm);
                            ptext(TXP_ID_LANE(0, TXP_PL_ACTIVE), cn[k] ? ac[k] + 1 : 0, ha, sizeof ha);
                            snprintf(t, sizeof t, "MANY %s \xC2\xB7 ACTIVE %s", hm, ha);
                            TX(g, c, px + pw - TW(g, 10.5f, 450.f, 0.f, FRUI_TNUM, t), yc + CAP(g, 10.5f) * 0.5f, 10.5f, 450.f, 0.f, FRUI_TNUM, C_DIM, t);
                        }
                    } else TX(g, c, px + 28.f, yc + CAP(g, 12.f) * 0.5f, 12.f, 450.f, 0.f, 0, C_FAINT, "empty");
                    y += 19.f;
                }
                y += 8.f;
            }
        }
        LBL(g, c, px, y + 10.f, 10.5f, C_DIM, "EFFECTS ON LOAD");
        TX(g, c, px + pw - TW(g, 10.5f, 450.f, 0.f, 0, "yours stay once you turn one"), y + 10.f, 10.5f, 450.f, 0.f, 0, C_FAINT, "yours stay once you turn one");
        seg_param(g, c, px, y + 18.f, pw, TXP_ID_GLOB(TXP_PG_SCNFX), &y);
        LBL(g, c, px, y + 22.f, 10.5f, C_DIM, "PRESETS CHANGE TEMPO");
        seg_param(g, c, px, y + 30.f, pw, TXP_ID_GLOB(TXP_PG_SCNTEMPO), &y);
        y = g->H - 56.f;
        button(g, c, px, y, 110.f, 30.f, "LOAD", 1);
        wid_add(g, TXG_W_LOAD, -1, px, y, 110.f, 30.f);
        button(g, c, px + 120.f, y, 90.f, 30.f, "CLOSE", 0);
        wid_add(g, TXG_W_CLOSE, -1, px + 120.f, y, 90.f, 30.f);
    }
}
/* ---- SETTINGS: the plugin's own and the app's global settings (host parameters), by function */
static float field(txg_t *g, frui_canvas *c, float x, float y, float w, const char *lab, uint32_t id)
{
    char t[24];
    float bw;
    int v = txp_ui_get(g->c, id);
    ptext(id, v, t, sizeof t);
    TX(g, c, x, y + 11.f + CAP(g, 12.5f) * 0.5f, 12.5f, 450.f, 0.f, 0, C_INK, lab);
    bw = TW(g, 12.f, 500.f, 0.f, FRUI_TNUM, t) + 16.f;
    bw = bw < 80.f ? 80.f : bw;
    RR(g, c, x + w - bw, y, bw, 22.f, 2.f, C_WELL);
    TX(g, c, x + w - 8.f - TW(g, 12.f, 500.f, 0.f, FRUI_TNUM, t), y + 11.f + CAP(g, 12.f) * 0.5f, 12.f, 500.f, 0.f, FRUI_TNUM, C_INK, t);
    wid_add2(g, TXG_W_FIELD, (int)(id & 0xFFFFu), id, 0, x + w - bw, y, bw, 22.f);
    return y + 28.f;
}
static float tempo_field(txg_t *g, frui_canvas *c, float x, float y, float w, const char *lab, int step)
{
    char t[24];
    float bw = 100.f;
    int b = g->sn.tempo10;
    if (step == 10) snprintf(t, sizeof t, "%d.%d BPM", b / 10, b % 10); else snprintf(t, sizeof t, "+0.%d", b % 10);
    TX(g, c, x, y + 11.f + CAP(g, 12.5f) * 0.5f, 12.5f, 450.f, 0.f, 0, C_INK, lab);
    RR(g, c, x + w - bw, y, bw, 22.f, 2.f, C_WELL);
    TX(g, c, x + w - 8.f - TW(g, 12.f, 500.f, 0.f, FRUI_TNUM, t), y + 11.f + CAP(g, 12.f) * 0.5f, 12.f, 500.f, 0.f, FRUI_TNUM, C_INK, t);
    wid_add2(g, TXG_W_TEMPO, step, 0, step, x + w - bw, y, bw, 22.f);
    return y + 28.f;
}
static void group(txg_t *g, frui_canvas *c, float x, float y, const char *h, const char *why)
{
    TX(g, c, x, y + 20.f, 15.f, 700.f, 2.4f, 0, C_INK, h);
    TX(g, c, x, y + 40.f, 12.f, 450.f, 0.f, 0, C_DIM, why);
}
static void draw_settings(txg_t *g, frui_canvas *c)
{
    const float gw = g->W / 4.f, gh = (g->H - OV_Y0) / 2.f, pad = 18.f, iw = gw - 2.f * pad;
    char t[64];
    float x, y;
    ov_header(g, c, "SETTINGS", 0, 0, 0);
    TX(g, c, 18.f + TW(g, 17.f, 700.f, 2.4f, 0, "SETTINGS") + 24.f, TOP_H + 23.f + CAP(g, 11.f) * 0.5f, 11.f, 450.f, 0.3f, 0, C_DIM,
       "Saved with the project; MIDI, scale and loading are host parameters too");
    for (int i = 1; i < 4; i++) R(g, c, gw * (float)i, OV_Y0, 1.f, g->H - OV_Y0, C_LINE);
    R(g, c, gw, OV_Y0 + gh, 2.f * gw, 1.f, C_LINE);
    /* MIDI IN (the whole first column) */
    x = pad; y = OV_Y0 + 8.f;
    group(g, c, x, y, "MIDI IN", "Each lane listens on its own channel.");
    y += 54.f;
    for (int k = 0; k < (int)TXP_LANES; k++) {
        float cw = (iw - 18.f) / 4.f, cx = x + (cw + 6.f) * (float)(k % 4), cy = y + 34.f * (float)(k / 4);
        int v = txp_ui_get(g->c, TXP_ID_LCH(k)), on = k < g->nlane;
        RR(g, c, cx, cy, cw, 29.f, 2.f, C_WELL);
        snprintf(t, sizeof t, "LANE %d", k + 1);
        LBL(g, c, cx + 7.f, cy + 11.f, 9.f, on ? C_DIM : C_FAINT, t);
        if (v > 0) snprintf(t, sizeof t, "CH %d", v); else snprintf(t, sizeof t, "OFF");
        TX(g, c, cx + 7.f, cy + 24.f, 11.5f, 500.f, 0.f, FRUI_TNUM, on ? C_INK : C_DIM, t);
        wid_add2(g, TXG_W_FIELD, (int)(TXP_ID_LCH(k) & 0xFFFFu), TXP_ID_LCH(k), 0, cx, cy, cw, 29.f);
    }
    y += 34.f * 4.f + 6.f;
    y = field(g, c, x, y, iw, "FX channel", TXP_ID_GLOB(TXP_PG_CHG));
    y = field(g, c, x, y, iw, "Selected lane channel", TXP_ID_GLOB(TXP_PG_SELCH));
    y = field(g, c, x, y, iw, "MOD CC (matrix source)", TXP_ID_MODCC);
    LBL(g, c, x, y + 10.f, 10.5f, C_DIM, "KEYS ON SELECTED CH");
    seg_param(g, c, x, y + 18.f, iw, TXP_ID_KEYS, &y);
    TX(g, c, x, y + 18.f, 11.f, 450.f, 0.f, 0, C_DIM, "PLAY: notes play the selected lane, as sent.");
    TX(g, c, x, y + 34.f, 11.f, 450.f, 0.f, 0, C_DIM, "FM-1 KEYS: F3..G5 are the FM-1's keys: the black");
    TX(g, c, x, y + 50.f, 11.f, 450.f, 0.f, 0, C_DIM, "groups pick lanes 1-4 (G#5..A#7: 5-16), F3..E4");
    TX(g, c, x, y + 66.f, 11.f, 450.f, 0.f, 0, C_DIM, "HOW MANY, F4..G5 HOW ACTIVE.");
    /* SCALE */
    x = gw + pad; y = OV_Y0 + 8.f;
    group(g, c, x, y, "SCALE", "The FM-1's own keys; MIDI notes play as sent.");
    y += 58.f;
    LBL(g, c, x, y + 4.f, 10.5f, C_DIM, "MODE");
    seg_param(g, c, x, y + 12.f, iw, TXP_ID_GLOB(TXP_PG_SCMODE), &y);
    LBL(g, c, x, y + 18.f, 10.5f, C_DIM, "SCALE");
    seg_param(g, c, x, y + 26.f, iw, TXP_ID_GLOB(TXP_PG_SCALE), &y);
    y += 8.f;
    y = field(g, c, x, y, iw, "Root", TXP_ID_GLOB(TXP_PG_ROOT));
    (void)field(g, c, x, y, iw, "Transpose", TXP_ID_GLOB(TXP_PG_TRANS));
    /* TIMING */
    x = 2.f * gw + pad; y = OV_Y0 + 8.f;
    group(g, c, x, y, "TIMING", "The host's tempo leads when it gives one.");
    y += 58.f;
    y = tempo_field(g, c, x, y, iw, "Tempo", 10);
    y = tempo_field(g, c, x, y, iw, "Fine (0.1 BPM)", 1);
    y = field(g, c, x, y, iw, "Groove", TXP_ID_GLOB(TXP_PG_GTYPE));
    y = field(g, c, x, y, iw, "Groove amount", TXP_ID_GLOB(TXP_PG_GAMOUNT));
    (void)field(g, c, x, y, iw, "Groove shift", TXP_ID_GLOB(TXP_PG_GSHIFT));
    /* LOADING */
    x = gw + pad; y = OV_Y0 + gh + 8.f;
    group(g, c, x, y, "LOADING", "What a preset brings with it.");
    y += 58.f;
    LBL(g, c, x, y + 4.f, 10.5f, C_DIM, "EFFECTS ON LOAD");
    seg_param(g, c, x, y + 12.f, iw, TXP_ID_GLOB(TXP_PG_SCNFX), &y);
    LBL(g, c, x, y + 18.f, 10.5f, C_DIM, "PRESETS CHANGE TEMPO");
    seg_param(g, c, x, y + 26.f, iw, TXP_ID_GLOB(TXP_PG_SCNTEMPO), &y);
    y += 10.f;
    (void)field(g, c, x, y, iw, "Lanes in use", TXP_ID_LANES);
    /* WINDOW */
    x = 2.f * gw + pad; y = OV_Y0 + gh + 8.f;
    group(g, c, x, y, "WINDOW", "How the plugin looks.");
    y += 58.f;
    LBL(g, c, x, y + 4.f, 10.5f, C_DIM, "SIZE");
    {
        static const int WS[4] = { 1200, 1500, 1800, 2400 };
        static const char *const WN[4] = { "100%", "125%", "150%", "200%" };
        int cur = txp_ui_win_w(g->c) ? txp_ui_win_w(g->c) : TXG_W;
        float xx = x;
        for (int i = 0; i < 4; i++) {
            float w = LBLW(g, 10.5f, WN[i]) + 20.f;
            int on = cur == WS[i];
            RR(g, c, xx, y + 12.f, w, 22.f, 2.f, on ? C_SEG : C_PANEL); RRS(g, c, xx, y + 12.f, w, 22.f, 2.f, C_LINE);
            if (on) R(g, c, xx + 1.f, y + 32.f, w - 2.f, 2.f, C_RED);
            LBL(g, c, xx + 10.f, y + 23.f + CAP(g, 10.5f) * 0.5f, 10.5f, on ? C_INK : C_DIM, WN[i]);
            wid_add2(g, TXG_W_WSIZE, i, 0, WS[i], xx, y + 12.f, w, 22.f);
            xx += w + 4.f;
        }
        y += 50.f;
        LBL(g, c, x, y + 4.f, 10.5f, C_DIM, "PLACE VIEW");
        xx = x;
        for (int i = 0; i < 2; i++) {
            const char *n = i ? "SPACE" : "MAP";
            float w = LBLW(g, 10.5f, n) + 20.f;
            int on = g->view == i;
            RR(g, c, xx, y + 12.f, w, 22.f, 2.f, on ? C_SEG : C_PANEL); RRS(g, c, xx, y + 12.f, w, 22.f, 2.f, C_LINE);
            if (on) R(g, c, xx + 1.f, y + 32.f, w - 2.f, 2.f, C_RED);
            LBL(g, c, xx + 10.f, y + 23.f + CAP(g, 10.5f) * 0.5f, 10.5f, on ? C_INK : C_DIM, n);
            wid_add2(g, TXG_W_VIEWSEL, i, 0, i, xx, y + 12.f, w, 22.f);
            xx += w + 4.f;
        }
        y += 50.f;
        snprintf(t, sizeof t, "HQ build  \xC2\xB7  %d lanes  \xC2\xB7  any size: drag the corner", (int)TXP_LANES);
        TX(g, c, x, y + 11.f, 12.f, 450.f, 0.f, 0, C_DIM, t);
    }
    /* ENGINE COLOURS: the engines the lanes play (the whole last column) */
    x = 3.f * gw + pad; y = OV_Y0 + 8.f;
    group(g, c, x, y, "ENGINE COLOURS", "Bone at first; a colour follows its engine.");
    y += 56.f;
    {
        int seen[TXP_ENG_MAX] = {0}, n = 0;
        for (int k = 0; k < g->nlane && n < 16; k++) {
            int e = g->sn.ui.lane[k].eng;
            float cy;
            char en[24];
            if (!used(g, k) || e >= (int)TXP_ENG_MAX || seen[e]) continue;
            seen[e] = 1;
            cy = y + 24.f * (float)n;
            cap_name(en, g->cat->eng[e], sizeof en);
            TX(g, c, x, cy + 10.f + CAP(g, 12.5f) * 0.5f, 12.5f, 450.f, 0.f, 0, C_INK, en);
            CI(g, c, x + iw - 12.f, cy + 10.f, 6.f, eng_col(g, e));
            RI(g, c, x + iw - 12.f, cy + 10.f, 8.5f, 1.f, g->pick == e ? C_INK : C_FAINT);
            wid_add2(g, TXG_W_COLDOT, -1, (uint32_t)e, 0, x, cy, iw, 20.f);
            n++;
        }
        if (!n) TX(g, c, x, y + 14.f, 12.f, 450.f, 0.f, 0, C_DIM, "No lane plays a sound.");
        y = g->H - 44.f;
        button(g, c, x, y, 170.f, 26.f, "RESET ALL TO BONE", 0);
        wid_add(g, TXG_W_COLRESET, -1, x, y, 170.f, 26.f);
    }
}
/* ---- the expanded strip: every parameter of a section (its tabs: the effects in their order, LFO 1 .. 8, ENV and
 * FOLLOW, the matrix), over the place and the rows; the strip stays under it */
static void fx_cells(txg_t *g, frui_canvas *c, const char *mod, float x, float y, float w, int cols, float ch)
{
    uint32_t n, i;
    const txp_param_t *t = txp_params(&n);
    int j = 0;
    float cw = (w - (float)(cols - 1) * 8.f) / (float)cols;
    for (i = 0; i < n; i++) {
        if (strcmp(t[i].module, mod)) continue;
        pcell(g, c, x + (cw + 8.f) * (float)(j % cols), y + (ch + 8.f) * (float)(j / cols), cw, ch, t[i].id, 0, 0);
        j++;
    }
}
static void draw_fx(txg_t *g, frui_canvas *c)
{
    static const char *const TABN[TAB_N] = { "COMP", "DRIVE", "FILTER", "DELAY", "REVERB", "LFO 1", "LFO 2", "LFO 3", "LFO 4",
                                             "LFO 5", "LFO 6", "LFO 7", "LFO 8", "ENV \xC2\xB7 FOLLOW", "MATRIX" };
    float y0 = TOP_H, y1 = g->fx_y, x = 16.f, by = y0 + 50.f, bh = y1 - by - 12.f;
    char t[64];
    int tab = g->fx_tab;
    R(g, c, 0, y0, g->W, y1 - y0, C_OVL);
    R(g, c, 0, y0 + 40.f, g->W, 1.f, C_LINE);
    for (int i = 0; i < TAB_N; i++) {   /* the tabs */
        float w = LBLW(g, 10.5f, TABN[i]) + 18.f;
        int on = i == tab, lit = i >= TAB_LFO0 && i < TAB_ENV && (g->sn.ui.lfo_on >> (i - TAB_LFO0) & 1u);
        if (i == TAB_LFO0 || i == TAB_ENV) x += 10.f;
        RR(g, c, x, y0 + 9.f, w, 22.f, 2.f, on ? C_SEG : C_PANEL); RRS(g, c, x, y0 + 9.f, w, 22.f, 2.f, C_LINE);
        if (on) R(g, c, x + 1.f, y0 + 29.f, w - 2.f, 2.f, C_RED);
        LBL(g, c, x + 9.f, y0 + 20.f + CAP(g, 10.5f) * 0.5f, 10.5f, on ? C_INK : lit ? C_RED : C_DIM, TABN[i]);
        wid_add(g, TXG_W_FXTAB, i, x, y0 + 9.f, w, 22.f);
        x += w + 4.f;
    }
    button(g, c, g->W - 96.f, y0 + 7.f, 80.f, 26.f, "CLOSE", 0);
    wid_add(g, TXG_W_CLOSE, -1, g->W - 96.f, y0 + 7.f, 80.f, 26.f);
    if (tab < SEC_MOVE) {                /* an effect: its picture, every parameter */
        static const char *const WHY[5] = { "The mix's compressor: the lanes together, before the drive.",
                                            "Saturation on the mix: AMOUNT, its TONE and MIX, five TYPEs.",
                                            "HIGH PASS then LOW PASS on the mix (real cutoffs), RESONANCE, SLOPE.",
                                            "The grain delay: echoes cut into grains, pitched, sprayed, frozen.",
                                            "The reverb: its TYPE, DECAY and SIZE; SHIMMER pitches its tail." };
        float vw = 300.f;
        LBL(g, c, 16.f, by + 12.f, 12.f, C_INK, SEC_NAME[tab]);
        hint2(g, c, 16.f, by + 30.f, vw - 32.f, WHY[tab]);
        RR(g, c, 16.f, by + 56.f, vw - 16.f, fminf(bh - 66.f, 160.f), 2.f, C_WELL);
        viz(g, c, tab, 28.f, by + 68.f, vw - 40.f, fminf(bh - 90.f, 136.f), sec_on(g, tab));
        fx_cells(g, c, SEC_MOD[tab], vw + 16.f, by, g->W - vw - 32.f, 4, 34.f);
    } else if (tab < TAB_ENV) {          /* an LFO: its trace, its routes, every parameter */
        int n = tab - TAB_LFO0, routed = (int)(g->sn.ui.lfo_on >> n & 1u), nr = 0;
        float vw = 300.f, yy;
        snprintf(t, sizeof t, "LFO %d", n + 1);
        LBL(g, c, 16.f, by + 12.f, 12.f, routed ? C_RED : C_INK, t);
        RR(g, c, 16.f, by + 24.f, vw - 16.f, 70.f, 2.f, C_WELL);
        lfo_trace(g, c, n, 26.f, by + 32.f, vw - 36.f, 54.f, routed ? C_RED : C_DIM);
        snprintf(t, sizeof t, "NOW %+d %%", (int)lroundf((float)g->sn.ui.lfo[n] * 100.f / 32767.f));   /* (read only) */
        LBL(g, c, vw - TW(g, 10.f, 620.f, 1.4f, 0, t), by + 12.f, 10.f, routed ? C_RED : C_DIM, t);
        yy = by + 116.f;
        LBL(g, c, 16.f, yy, 10.f, C_DIM, "ROUTES (THE MATRIX)");
        for (int s = 0; s < 16; s++) {   /* the matrix slots it drives */
            int i = txp_param_index(ID_MAT(s, 0)), it = txp_param_index(ID_MAT(s, 1)), id = txp_param_index(ID_MAT(s, 2));
            if (i < 0 || g->sn.pv[i] != n + 1 || it < 0 || !g->sn.pv[it]) continue;
            snprintf(t, sizeof t, "MOD %d  \xE2\x86\x92  %s  %s", s + 1, g->sn.ptx[it], id >= 0 ? g->sn.ptx[id] : "");
            TX(g, c, 16.f, yy + 20.f + 17.f * (float)nr, 11.5f, 500.f, 0.f, FRUI_TNUM, C_RED, t);
            nr++;
        }
        if (!nr) TX(g, c, 16.f, yy + 20.f, 11.5f, 450.f, 0.f, 0, C_DIM, "Not routed: set a matrix slot's SOURCE to it.");
        snprintf(t, sizeof t, "LFO %d", n + 1);
        fx_cells(g, c, t, vw + 16.f, by, g->W - vw - 32.f, 4, 34.f);
    } else if (tab == TAB_ENV) {         /* the envelopes and the followers: a column each */
        static const char *const M[4] = { "ENV 1", "ENV 2", "FOLLOW 1", "FOLLOW 2" };
        float cw = (g->W - 32.f - 3.f * 16.f) / 4.f;
        for (int i = 0; i < 4; i++) {
            float cx = 16.f + (cw + 16.f) * (float)i;
            LBL(g, c, cx, by + 12.f, 12.f, C_INK, M[i]);
            fx_cells(g, c, M[i], cx, by + 24.f, cw, 1, 28.f);
        }
    } else {                             /* the matrix: sixteen slots, SOURCE TARGET DEPTH SPREAD; MOD CC */
        float colw = (g->W - 48.f) / 2.f, rh = fminf((bh - 46.f) / 8.f, 40.f);
        LBL(g, c, 16.f, by + 12.f, 12.f, C_INK, "MATRIX");
        TX(g, c, 100.f, by + 12.f, 11.5f, 450.f, 0.f, 0, C_DIM, "A source moves a target by DEPTH; SPREAD offsets each lane's.");
        pcell(g, c, g->W - 16.f - 200.f, by, 200.f, 20.f, TXP_ID_MODCC, "MOD CC", 0);
        for (int s = 0; s < 16; s++) {   /* (the columns named once over each half; the cells their values alone) */
            float cx = 16.f + (colw + 16.f) * (float)(s / 8), cy = by + 46.f + rh * (float)(s % 8), w1 = 52.f,
                  ws = (colw - w1 - 3.f * 6.f) * 0.27f, wt = (colw - w1 - 3.f * 6.f) * 0.39f, wd = (colw - w1 - 3.f * 6.f) * 0.17f;
            int is = txp_param_index(ID_MAT(s, 0)), it = txp_param_index(ID_MAT(s, 1)), on = is >= 0 && it >= 0 && g->sn.pv[is] && g->sn.pv[it];
            if (s % 8 == 0) {
                LBL(g, c, cx + w1 + 9.f, cy - 8.f, 9.5f, C_DIM, "SOURCE");
                LBL(g, c, cx + w1 + ws + 15.f, cy - 8.f, 9.5f, C_DIM, "TARGET");
                LBL(g, c, cx + w1 + ws + wt + 21.f, cy - 8.f, 9.5f, C_DIM, "DEPTH");
                LBL(g, c, cx + w1 + ws + wt + wd + 27.f, cy - 8.f, 9.5f, C_DIM, "SPREAD");
            }
            snprintf(t, sizeof t, "MOD %d", s + 1);
            LBL(g, c, cx, cy + (rh - 8.f) * 0.5f + CAP(g, 10.f) * 0.5f, 10.f, on ? C_RED : C_DIM, t);
            pcell(g, c, cx + w1, cy, ws, rh - 8.f, ID_MAT(s, 0), "", 0);
            pcell(g, c, cx + w1 + ws + 6.f, cy, wt, rh - 8.f, ID_MAT(s, 1), "", 0);
            pcell(g, c, cx + w1 + ws + wt + 12.f, cy, wd, rh - 8.f, ID_MAT(s, 2), "", 0);
            pcell(g, c, cx + w1 + ws + wt + wd + 18.f, cy, wd, rh - 8.f, ID_MAT(s, 3), "", 0);
        }
    }
}
/* ---- SAVE: the scene that plays into a user preset U01 .. U16, named; kept with the project */
static float save_px(const txg_t *g) { float x = g->sv_x - 160.f; return x + 420.f > g->W - 8.f ? g->W - 428.f : x; }
static void draw_save(txg_t *g, frui_canvas *c)
{
    const float w = 420.f, h = 276.f, y = TOP_H + 2.f;
    float x = save_px(g), sw = (w - 32.f - 3.f * 6.f) / 4.f;
    char t[48];
    wid_add(g, TXG_W_PICKER, -2, x, y, w, h);
    frui_rrect_shadow(c, x * g->S, y * g->S, w * g->S, h * g->S, 4.f * g->S, 16.f * g->S, 0x90000000u);
    RR(g, c, x, y, w, h, 4.f, C_BTN); RRS(g, c, x, y, w, h, 4.f, C_DIM);
    LBL(g, c, x + 16.f, y + 24.f, 12.f, C_INK, "SAVE AS A USER PRESET");
    TX(g, c, x + 16.f, y + 42.f, 11.f, 450.f, 0.f, 0, C_DIM, "Kept with the project; load it in PRESETS: USER.");
    for (int i = 0; i < (int)TXP_USER_N; i++) {
        float sx = x + 16.f + (sw + 6.f) * (float)(i % 4), sy = y + 54.f + 34.f * (float)(i / 4);
        int u = (int)(g->sn.ui.user >> i & 1u), on = i == g->sv_slot;
        RR(g, c, sx, sy, sw, 28.f, 2.f, on ? C_SEG : C_WELL); RRS(g, c, sx, sy, sw, 28.f, 2.f, on ? C_RED : C_LINE);
        snprintf(t, sizeof t, "U%02d", i + 1);
        TX(g, c, sx + 6.f, sy + 12.f, 10.f, 500.f, 0.f, FRUI_TNUM, on ? C_RED : C_DIM, t);
        clipL(g, c, sx, sy, sw - 4.f, 28.f);
        TX(g, c, sx + 6.f, sy + 24.f, 10.f, 500.f, 0.f, 0, u ? C_INK : C_FAINT, u ? g->sn.ui.uname[i] : "empty");
        frui_clip_reset(c);
        wid_add(g, TXG_W_SLOT, i, sx, sy, sw, 28.f);
    }
    {
        float ny = y + 210.f, nw = w - 32.f - 100.f;
        LBL(g, c, x + 16.f, ny - 4.f, 9.5f, C_DIM, "NAME (TYPE IT; EMPTY: THE LANDSCAPE'S)");
        RR(g, c, x + 16.f, ny, nw, 26.f, 3.f, C_WELL); RRS(g, c, x + 16.f, ny, nw, 26.f, 3.f, C_DIM);
        {
            float tw = TX(g, c, x + 26.f, ny + 13.f + CAP(g, 12.f) * 0.5f, 12.f, 500.f, 0.6f, 0, C_INK, g->sv_name);
            R(g, c, x + 27.f + tw, ny + 6.f, 1.2f, 14.f, C_RED);
        }
        wid_add(g, TXG_W_NAME, -1, x + 16.f, ny, nw, 26.f);
        button(g, c, x + w - 16.f - 90.f, ny, 90.f, 26.f, "SAVE", 1);
        wid_add(g, TXG_W_SAVEGO, -1, x + w - 16.f - 90.f, ny, 90.f, 26.f);
        if (g->sn.ui.saving || g->sv_wait > 0) snprintf(t, sizeof t, "SAVING U%02d ...", g->sv_slot + 1);
        else if (g->sn.ui.user >> g->sv_slot & 1u) snprintf(t, sizeof t, "U%02d HOLDS \"%s\": SAVE REPLACES IT", g->sv_slot + 1, g->sn.ui.uname[g->sv_slot]);
        else snprintf(t, sizeof t, "U%02d IS EMPTY", g->sv_slot + 1);
        LBL(g, c, x + 16.f, ny + 46.f, 9.5f, g->sn.ui.saving || g->sv_wait > 0 ? C_RED : C_DIM, t);
    }
}
/* the colour picker: the engine's swatches in a small panel near where it was asked (a click outside closes it) */
static void draw_picker(txg_t *g, frui_canvas *c)
{
    const float w = 21.f * TXP_COL_N + 22.f, h = 62.f;
    float x = (float)g->pick_x, y = (float)g->pick_y;
    char t[48], en[24];
    if (g->pick < 0 || g->pick >= g->cat->neng) return;
    x = x + w > g->W - 8.f ? g->W - 8.f - w : x < 8.f ? 8.f : x;
    y = y + h > g->H - 8.f ? y - h - 30.f : y;
    wid_add(g, TXG_W_PICKER, -1, x, y, w, h);
    frui_rrect_shadow(c, x * g->S, y * g->S, w * g->S, h * g->S, 4.f * g->S, 14.f * g->S, 0x90000000u);
    RR(g, c, x, y, w, h, 4.f, C_BTN); RRS(g, c, x, y, w, h, 4.f, C_DIM);
    upper(en, g->cat->eng[g->pick], sizeof en);
    {
        int ci = txp_ui_col(g->c, g->pick);
        snprintf(t, sizeof t, "%s COLOUR  \xC2\xB7  %s", en, PAL_N[ci >= 0 && ci < TXP_COL_N ? ci : 0]);
    }
    LBL(g, c, x + 12.f, y + 20.f, 10.f, C_DIM, t);
    swatches(g, c, x + 11.f, y + 31.f, g->pick);
}
static void draw_all(txg_t *g, frui_canvas *c)
{
    g->nw = 0;
    frui_clip_reset(c);
    frui_fill(c, C_PANEL);
    draw_top(g, c);
    if (g->ov == OV_SET) draw_settings(g, c);
    else if (g->ov == OV_SND) draw_sounds(g, c);
    else if (g->ov == OV_PRE) draw_presets(g, c);
    else {
        if (g->ov == OV_FX) draw_fx(g, c);
        else { draw_rows(g, c); draw_place(g, c); draw_drawer(g, c); }
        draw_strip(g, c);
        if (g->ov == OV_SAVE) draw_save(g, c);
    }
    draw_picker(g, c);
}

/* ---- reading what plays */
int txg_tick(txg_t *g)
{
    uint32_t h = 2166136261u, actn0 = g->actn;
    int k, sel;
    txp_ui_snap(g->c, &g->sn);
    g->nlane = g->sn.ui.nlane < 1 ? 1 : g->sn.ui.nlane > TXP_LANES ? (int)TXP_LANES : g->sn.ui.nlane;
    sel = txp_ui_sel(g->c);
    if (sel >= g->nlane) sel = g->nlane - 1;
    if (sel != g->sel_seen) {            /* (a lane picked, here or by the keys: its row in sight) */
        g->sel_seen = sel;
        if (sel < g->rtop) g->rtop = sel;
        if (g->rvis > 0 && sel >= g->rtop + g->rvis) g->rtop = sel - g->rvis + 1;
    }
    g->sel = sel;
    txp_ui_mutes(g->c, &g->mute, &g->solo);
    for (k = 0; k < (int)TXP_LANES; k++) {
        const txp_ui_lane_t *l = &g->sn.ui.lane[k];
        g->col[k] = l->used ? txp_ui_col(g->c, l->eng) : 0;
        if (k >= g->nlane || !l->used) { g->level[k] = g->pan[k] = g->cnt[k] = g->act[k] = 0; g->dist[k] = -1; continue; }
        g->level[k] = txp_ui_get(g->c, TXP_ID_LANE(k, TXP_PL_LEVEL));
        g->pan[k] = txp_ui_get(g->c, TXP_ID_LANE(k, TXP_PL_PAN));
        g->cnt[k] = txp_ui_get(g->c, TXP_ID_LANE(k, TXP_PL_COUNT));
        g->act[k] = txp_ui_get(g->c, TXP_ID_LANE(k, TXP_PL_ACTIVE));
        g->dist[k] = l->dist != 0xFF ? txp_ui_get(g->c, TXP_ID_LANE(k, TXP_PL_CARD0 + l->dist)) : -1;
    }
    g->actn = txp_ui_act(g->c, g->ev, g->pk, NBLK);
    if (g->actn != actn0 || !g->drawn)   /* the LFOs' traces: a value a tick while the app plays */
        for (int n = 0; n < (int)TXP_LFO_N; n++) {
            if (!g->drawn && !g->lfo_seen) for (int i = 0; i < LFO_H; i++) g->lfoh[n][i] = g->sn.ui.lfo[n];   /* (the first:
                                          * a flat trace at the value now) */
            memmove(g->lfoh[n], g->lfoh[n] + 1, (LFO_H - 1) * sizeof g->lfoh[n][0]);
            g->lfoh[n][LFO_H - 1] = g->sn.ui.lfo[n];
        }
    g->lfo_seen = 1;
    if (g->sb_pend > 0 && --g->sb_pend == 0 && g->ov == OV_SND && g->sn.ui.lane[g->sb_lane].run)   /* (an audition
                                          * waited for the keys to rest) */
        txp_ui_sound(g->c, g->sb_lane, g->sb_cf, g->sb_ci, 0);
    if (g->sv_wait > 0) g->sv_wait--;
    /* the signature of what is drawn but the live values: the values the modulation moves now, the meters and the LFOs
     * are drawn with the activity (the partial repaint), not here */
    {
        static txp_ui_snap_t u;          /* (the main thread's alone) */
        u = g->sn.ui;
        for (k = 0; k < (int)TXP_LANES; k++) { u.lane[k].lpan = u.lane[k].ldist = 0; memset(u.card[k].live, 0, sizeof u.card[k].live); }
        memset(u.lfo, 0, sizeof u.lfo);
        u.comp_gr10 = 0; u.out_db10 = 0;
        h = fnv(h, &u, sizeof u);
    }
    h = fnv(h, g->sn.pv, sizeof g->sn.pv);
    h = fnv(h, g->sn.ptx, sizeof g->sn.ptx);
    h = fnv(h, &g->nlane, sizeof g->nlane); h = fnv(h, &g->sel, sizeof g->sel); h = fnv(h, &g->ov, sizeof g->ov);
    h = fnv(h, &g->view, sizeof g->view); h = fnv(h, &g->pick, sizeof g->pick); h = fnv(h, g->col, sizeof g->col);
    h = fnv(h, &g->mute, sizeof g->mute); h = fnv(h, &g->solo, sizeof g->solo);
    h = fnv(h, g->level, sizeof g->level); h = fnv(h, g->pan, sizeof g->pan); h = fnv(h, g->dist, sizeof g->dist);
    h = fnv(h, g->cnt, sizeof g->cnt); h = fnv(h, g->act, sizeof g->act);
    h = fnv(h, &g->hover_kind, sizeof g->hover_kind); h = fnv(h, &g->hover_lane, sizeof g->hover_lane);
    h = fnv(h, &g->drag, sizeof g->drag); h = fnv(h, &g->rtop, sizeof g->rtop); h = fnv(h, &g->fx_tab, sizeof g->fx_tab);
    h = fnv(h, &g->sn.tempo10, sizeof g->sn.tempo10);
    {
        int v[9] = { g->sb_fam, g->sb_cf << 8 | g->sb_ci, g->sb_top, g->pb_bank, g->pb_cur, g->pb_top, g->sv_slot, g->sv_wait > 0, txp_ui_win_w(g->c) };
        h = fnv(h, v, sizeof v);
        h = fnv(h, g->q, sizeof g->q);
        h = fnv(h, g->sv_name, sizeof g->sv_name);
    }
    if (g->ov == OV_SET || g->ov == OV_PRE) {   /* (the settings they show: the window's own writes too) */
        static const int GL[] = { TXP_PG_SCNTEMPO, TXP_PG_SCNFX, TXP_PG_GAMOUNT, TXP_PG_GTYPE, TXP_PG_GSHIFT, TXP_PG_SCALE,
                                  TXP_PG_ROOT, TXP_PG_SELCH, TXP_PG_SCMODE, TXP_PG_TRANS, TXP_PG_CHG };
        int gv[sizeof GL / sizeof GL[0] + TXP_LANES + 2];
        size_t i;
        for (i = 0; i < sizeof GL / sizeof GL[0]; i++) gv[i] = txp_ui_get(g->c, TXP_ID_GLOB(GL[i]));
        for (k = 0; k < (int)TXP_LANES; k++) gv[i + (size_t)k] = txp_ui_get(g->c, TXP_ID_LCH(k));
        gv[i + TXP_LANES] = txp_ui_get(g->c, TXP_ID_MODCC);
        gv[i + TXP_LANES + 1] = txp_ui_get(g->c, TXP_ID_KEYS);
        h = fnv(h, gv, sizeof gv);
    }
    g->sig = h;
    return !g->drawn || g->sig != g->sig_drawn || g->actn != g->actn_drawn;
}

/* ---- painting: a whole repaint, or only what moves with the activity (frui's base clip: the same pixels as a whole
 * repaint): the place, the event lines, the drawer and the strip, the output's meter */
static void fit(txg_t *g, int cw, int ch)
{
    float s = fminf((float)cw / (float)TXG_W, (float)ch / (float)TXG_H);
    g->S = s > 0.f ? s : 1.f;
    g->W = (float)cw / g->S; g->H = (float)ch / g->S;
    lay(g);
}
static void region(txg_t *g, frui_canvas *c, float x0, float y0, float x1, float y1)
{
    frui_clip_base(c, (int)floorf(x0 * g->S), (int)floorf(y0 * g->S), (int)ceilf(x1 * g->S), (int)ceilf(y1 * g->S));
    draw_all(g, c);
}
static int paint_into(txg_t *g, frui_canvas *c)
{
    int full = !g->drawn || c->px != g->last_px || c->w != g->last_w || c->h != g->last_h || g->sig != g->sig_drawn;
    fit(g, c->w, c->h);
    if (full || (g->actn != g->actn_drawn && g->ov == OV_FX && g->pick < 0)) {
        frui_clip_base(c, 0, 0, c->w, c->h);
        draw_all(g, c);
    } else if (g->actn != g->actn_drawn && (g->ov == OV_NONE || g->ov == OV_SAVE) && g->pick < 0) {
        rcol_t rc = rcols(g);
        region(g, c, 0, TOP_H, g->PW, g->dr_y);
        region(g, c, rc.dot - 8.f, TOP_H + HDR_H, rc.tl + rc.tlw + 1.f, g->dr_y);
        region(g, c, 0, g->dr_y, g->W, g->H);
        region(g, c, meter_x(g) - 2.f, 0, meter_x(g) + 104.f, TOP_H - 1.f);
        frui_clip_base(c, 0, 0, c->w, c->h);
    } else { g->actn_drawn = g->actn; return 0; }
    g->drawn = 1;
    g->last_px = c->px; g->last_w = c->w; g->last_h = c->h;
    g->sig_drawn = g->sig;
    g->actn_drawn = g->actn;
    return 1;
}
int txg_paint(txg_t *g)
{
    frui_canvas c;
    if (!g->px) return 0;
    frui_canvas_init(&c, g->px, g->pw, g->ph);
    return paint_into(g, &c);
}
const uint32_t *txg_pixels(const txg_t *g, int *w, int *h) { *w = g->pw; *h = g->ph; return g->px; }
int txg_resize(txg_t *g, int pw, int ph)
{
    uint32_t *px;
    if (pw < 16 || ph < 16) return -1;
    px = calloc((size_t)pw * (size_t)ph, 4);
    if (!px) return -1;
    free(g->px);
    g->px = px; g->pw = pw; g->ph = ph;
    g->drawn = 0;
    fit(g, pw, ph);
    return 0;
}
float txg_scale_of(int pw, int ph)       /* canvas pixels a logical pixel, for a canvas of pw x ph (the views' input) */
{
    float s = fminf((float)pw / (float)TXG_W, (float)ph / (float)TXG_H);
    return s > 0.f ? s : 1.f;
}

/* ---- input */
static const wid_t *hit(const txg_t *g, float x, float y)   /* the last-drawn control under (x, y): a panel over the
                                                             * rest first (the picker, SAVE), the map's dots nearest
                                                             * first, then the topmost */
{
    const wid_t *best = 0;
    float bd = 1e9f;
    if (g->pick >= 0 || g->ov == OV_SAVE) {
        for (int i = g->nw - 1; i >= 0; i--) {
            const wid_t *w = &g->w[i];
            int panel = w->kind == TXG_W_PICKER;
            if (x >= w->r[0] && x < w->r[0] + w->r[2] && y >= w->r[1] && y < w->r[1] + w->r[3]) {
                if (g->pick >= 0 && w->kind == TXG_W_SWATCH) return w;
                if (g->ov == OV_SAVE && g->pick < 0 && (w->kind == TXG_W_SLOT || w->kind == TXG_W_NAME || w->kind == TXG_W_SAVEGO)) return w;
                if (panel && !best) best = w;
            }
        }
        if (best) return best;
        if (g->pick >= 0) return 0;
    }
    for (int i = 0; i < g->nw; i++) {
        const wid_t *w = &g->w[i];
        if (w->kind != TXG_W_DOT) continue;
        float dx = x - (w->r[0] + w->r[2] * 0.5f), dy = y - (w->r[1] + w->r[3] * 0.5f), d = dx * dx + dy * dy;
        if (d < 14.f * 14.f && d < bd) { bd = d; best = w; }
    }
    if (best) return best;
    for (int i = g->nw - 1; i >= 0; i--) {
        const wid_t *w = &g->w[i];
        if (w->kind == TXG_W_DOT || w->kind == TXG_W_PLACE || w->kind == TXG_W_MAPR) continue;
        if (x >= w->r[0] && x < w->r[0] + w->r[2] && y >= w->r[1] && y < w->r[1] + w->r[3]) {
            if (w->kind == TXG_W_ROW || w->kind == TXG_W_ROWS) { if (!best || best->kind == TXG_W_ROWS) best = w; continue; }   /* (a row:
                                          * only when nothing in it is hit) */
            return w;
        }
    }
    return best;
}
static void step_param(txg_t *g, uint32_t id, int d, int wrap)
{
    int v = txp_ui_get(g->c, id), hi = txp_ui_hi(id);
    if (v < 0 || hi < 0) return;
    v += d;
    if (wrap) v = v < 0 ? hi : v > hi ? 0 : v;
    else v = v < 0 ? 0 : v > hi ? hi : v;
    txp_ui_begin(g->c, id); txp_ui_set(g->c, id, v); txp_ui_end(g->c, id);
}
static void set_param(txg_t *g, uint32_t id, int v) { txp_ui_begin(g->c, id); txp_ui_set(g->c, id, v); txp_ui_end(g->c, id); }
static int p_default(const txg_t *g, uint32_t id)   /* a double-click's value: a card its sound's own, PAN the middle */
{
    int k = lane_of_id(id), i = (int)(id & 0xFFu);
    if (k >= 0 && i >= TXP_PL_CARD0 && i < TXP_PL_CARD0 + (int)TXP_CARD_N) return g->sn.ui.card[k].def[i - TXP_PL_CARD0];
    if (k >= 0 && i == TXP_PL_PAN) return 64;
    if (k >= 0 && i == TXP_PL_LEVEL) return 100;
    return txp_ui_default(g->c, id);
}
static int choice(uint32_t id)           /* stepped by a click (a choice, a switch) */
{
    const txp_param_t *p = pinfo(id);
    return p && (p->is_enum || p->hi < 20);
}
static void vol_at(txg_t *g, int k, float x)
{
    const rcol_t rc = rcols(g);
    int v = (int)lroundf((x - rc.vol) / RW_VOL * 127.f);
    txp_ui_set(g->c, TXP_ID_LANE(k, TXP_PL_LEVEL), v < 0 ? 0 : v > 127 ? 127 : v);
}
static int steps_at(const txg_t *g, int act, float x)   /* HOW MANY / HOW ACTIVE at x in its blocks (0: left of them) */
{
    int n = act ? 9 : 6;
    float x0 = g->drag_rx + 2.f, bw = (RW_STEP + 2.f) / (float)n;
    int v = (int)floorf((x - x0) / bw) + 1;
    if (x < x0) v = 0;
    return v < 0 ? 0 : v > n ? n : v;
}
static void dot_at(txg_t *g, int k, float x, float y)
{
    int pan, dist;
    if (g->view) sp_inv(g, x - g->drag_dx, y - g->drag_dy, g->drag_dist < 0, &pan, &dist);
    else map_inv(g, x, y, &pan, &dist);
    txp_ui_set(g->c, TXP_ID_LANE(k, TXP_PL_PAN), pan);
    if (g->drag_dist >= 0) txp_ui_set(g->c, TXP_ID_LANE(k, TXP_PL_CARD0 + g->drag_dist), dist);
    g->pan[k] = pan;
    if (g->drag_dist >= 0) g->dist[k] = dist;
}
static void param_drag(txg_t *g, float x, float y)   /* TXG_W_PARAM: the value from the press's, the pointer's way */
{
    const txp_param_t *p = pinfo(g->drag_id);
    float d = (x - g->drag_x0) - (y - g->drag_y0);
    int v;
    if (!p) return;
    if (!g->drag_moved && fabsf(d) < 3.f) return;
    g->drag_moved = 1;
    if (choice(g->drag_id)) v = g->drag_v0 + (int)lroundf(d / 14.f);
    else v = g->drag_v0 + (int)lroundf(d * (float)(p->hi + 1) / (g->drag_fine ? 900.f : 220.f));
    v = v < 0 ? 0 : v > p->hi ? p->hi : v;
    if (!g->drag_begun) { txp_ui_begin(g->c, g->drag_id); g->drag_begun = 1; }
    txp_ui_set(g->c, g->drag_id, v);
}
static void drag_end(txg_t *g)
{
    int k = g->drag_lane;
    if (g->drag == TXG_W_VOL) txp_ui_end(g->c, TXP_ID_LANE(k, TXP_PL_LEVEL));
    else if (g->drag == TXG_W_DOT) {
        txp_ui_end(g->c, TXP_ID_LANE(k, TXP_PL_PAN));
        if (g->drag_dist >= 0) txp_ui_end(g->c, TXP_ID_LANE(k, TXP_PL_CARD0 + g->drag_dist));
    } else if (g->drag == TXG_W_PARAM || g->drag == TXG_W_MANY || g->drag == TXG_W_ACT) {
        if (g->drag_begun) txp_ui_end(g->c, g->drag_id);
    }
    g->drag = 0;
    g->drag_begun = 0;
}
static void select_lane(txg_t *g, int k) { if (k >= 0 && k < g->nlane) { txp_ui_sel_set(g->c, k); g->sel = k; } }
/* the browsers: open, move, keep, close */
static void sb_open(txg_t *g, int k)
{
    const txp_ui_lane_t *l = &g->sn.ui.lane[k];
    select_lane(g, k);
    g->ov = OV_SND; g->pick = -1; g->q[0] = 0; g->sb_top = 0; g->sb_pend = 0;
    g->sb_lane = k;
    g->sb_kf = l->used ? l->fi : -1; g->sb_ki = l->used ? l->si : -1;
    g->sb_cf = l->used ? l->fi : (g->sn.ui.land < g->cat->nfam ? g->sn.ui.land : 0);
    g->sb_ci = l->used ? l->si : 0;
    g->sb_fam = g->sb_cf;
}
static void sb_close(txg_t *g, int keep)
{
    const txp_ui_lane_t *l = &g->sn.ui.lane[g->sb_lane];
    g->sb_pend = 0;
    if (keep) {                          /* (the cursor's sound on the lane: a stopped lane gets it stopped) */
        if (!(g->sb_cf == g->sb_kf && g->sb_ci == g->sb_ki) || !l->used) txp_ui_sound(g->c, g->sb_lane, g->sb_cf, g->sb_ci, 1);
    } else if (g->sb_kf >= 0 && l->used && !(l->fi == g->sb_kf && l->si == g->sb_ki))
        txp_ui_sound(g->c, g->sb_lane, g->sb_kf, g->sb_ki, 0);   /* (an audition ends: the kept sound back) */
    g->ov = OV_NONE; g->q[0] = 0;
}
static void sb_cursor(txg_t *g, int f, int i, int now)   /* the cursor on a sound: heard on the lane while it plays */
{
    g->sb_cf = f; g->sb_ci = i;
    if (!g->sn.ui.lane[g->sb_lane].run) return;
    if (now) { g->sb_pend = 0; txp_ui_sound(g->c, g->sb_lane, f, i, 0); }
    else g->sb_pend = 12;                /* (the keys: heard once they rest, ~0.4 s, as on the FM-1) */
}
static void pb_open(txg_t *g)
{
    const txp_catalog_t *ct = g->cat;
    int s = g->sn.ui.scene;
    g->ov = OV_PRE; g->pick = -1; g->q[0] = 0; g->pb_top = 0;
    if (s != 0xFF && s < ct->npr) {
        g->pb_cur = s;
        for (int i = 0; i < ct->ncat; i++) if (s >= ct->cat_at[i] && s < ct->cat_at[i + 1]) g->pb_bank = i;
    } else if (s != 0xFF) { g->pb_cur = s; g->pb_bank = PB_USER; }
    else { g->pb_cur = TXP_LOAD_LS + g->sn.ui.land; g->pb_bank = PB_LAND; }
}
static void pb_load(txg_t *g)
{
    int v = g->pb_cur, loaded = g->sn.ui.scene != 0xFF ? g->sn.ui.scene : TXP_LOAD_LS + g->sn.ui.land;
    if (v < 0) return;
    txp_ui_load(g->c, v, v == loaded && g->sn.ui.fx_yours);
}
static void save_open(txg_t *g)          /* SAVE: on the user preset loaded (its name), else the first empty one */
{
    int s = g->sn.ui.scene, n = g->cat->npr;
    g->ov = OV_SAVE; g->pick = -1;
    g->sv_slot = -1; g->sv_name[0] = 0;
    if (s != 0xFF && s >= n && s < n + (int)TXP_USER_N) { g->sv_slot = s - n; snprintf(g->sv_name, sizeof g->sv_name, "%.11s", g->sn.ui.uname[s - n]); }
    for (int i = 0; i < (int)TXP_USER_N && g->sv_slot < 0; i++) if (!(g->sn.ui.user >> i & 1u)) g->sv_slot = i;
    if (g->sv_slot < 0) g->sv_slot = 0;
}
static void save_go(txg_t *g)
{
    if (g->sn.ui.saving || g->sv_wait > 0) return;
    txp_ui_save(g->c, g->sv_slot, g->sv_name);
    g->sv_wait = 20;
}
static void list_move(txg_t *g, int d)   /* a browser's cursor, d items on (no wrap) */
{
    int cur = -1, n;
    if (g->ov == OV_SND) sb_items(g); else pb_items(g);
    n = g->nitems;
    if (!n) return;
    for (int j = 0; j < n; j++) if (g->items[j] == (g->ov == OV_SND ? (g->sb_cf << 4 | g->sb_ci) : g->pb_cur)) cur = j;
    cur = cur < 0 ? 0 : cur + d;
    cur = cur < 0 ? 0 : cur >= n ? n - 1 : cur;
    if (g->ov == OV_SND) sb_cursor(g, g->items[cur] >> 4, g->items[cur] & 15, 0);
    else g->pb_cur = g->items[cur];
}
static void tempo_step(txg_t *g, int d)
{
    int b = g->sn.tempo10 + d;
    txp_ui_tempo(g->c, b < 400 ? 400 : b > 2400 ? 2400 : b);
}
static int ov_key(txg_t *g, const frg_input *e)   /* the keys while an overlay is open */
{
    size_t n = strlen(g->q);
    if (e->key == FRK_ESCAPE) {
        if (g->pick >= 0) { g->pick = -1; return 3; }
        if (g->q[0]) { g->q[0] = 0; return 3; }
        if (g->ov == OV_SND) sb_close(g, 0); else g->ov = OV_NONE;
        return 3;
    }
    if (g->ov == OV_SAVE) {              /* the name: the save mode's letters (A .. Z, 0 .. 9, space - & ' .) */
        size_t m = strlen(g->sv_name);
        int ch = e->key;
        if (ch == FRK_RETURN) { save_go(g); return 3; }
        if (ch == FRK_BACKSPACE || ch == FRK_DELETE) { if (m) g->sv_name[m - 1] = 0; return 3; }
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch > 0 && ch < 0x7F && strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-&'. ", ch) && m < 11u) { g->sv_name[m] = (char)ch; g->sv_name[m + 1] = 0; return 3; }
        return 0;
    }
    if (g->ov == OV_SET || g->ov == OV_FX) return 0;
    if (e->key == FRK_UP || e->key == FRK_DOWN) { list_move(g, e->key == FRK_UP ? -1 : 1); return 3; }
    if (e->key == FRK_PGUP || e->key == FRK_PGDN) { list_move(g, e->key == FRK_PGUP ? -10 : 10); return 3; }
    if ((e->key == FRK_LEFT || e->key == FRK_RIGHT) && !g->q[0]) {   /* the next family / bank */
        int d = e->key == FRK_LEFT ? -1 : 1;
        if (g->ov == OV_SND) {
            g->sb_fam = (g->sb_fam + d + g->cat->nfam) % g->cat->nfam;
            sb_cursor(g, g->sb_fam, 0, 0);
        } else {
            int b = g->pb_bank == PB_USER ? -1 : g->pb_bank == PB_LAND ? g->cat->ncat : g->pb_bank;
            b += d;
            if (b < (g->sn.ui.user ? -1 : 0)) b = g->cat->ncat;
            if (b > g->cat->ncat) b = g->sn.ui.user ? -1 : 0;
            g->pb_bank = b < 0 ? PB_USER : b == g->cat->ncat ? PB_LAND : b;
            pb_items(g);
            if (g->nitems) g->pb_cur = g->items[0];
            g->pb_top = 0;
        }
        return 3;
    }
    if (e->key == FRK_RETURN) { if (g->ov == OV_SND) sb_close(g, 1); else pb_load(g); return 3; }
    if (e->key == FRK_BACKSPACE || e->key == FRK_DELETE) { if (n) g->q[n - 1] = 0; g->sb_top = g->pb_top = 0; return 3; }
    if (e->key >= 0x20 && e->key < 0x7F && n + 1 < sizeof g->q) {
        g->q[n] = (char)e->key; g->q[n + 1] = 0;
        g->sb_top = g->pb_top = 0;
        if (g->ov == OV_SND) { sb_items(g); if (g->nitems) { g->sb_cf = g->items[0] >> 4; g->sb_ci = g->items[0] & 15; } }
        else { pb_items(g); if (g->nitems) g->pb_cur = g->items[0]; }
        return 3;
    }
    return 0;
}
int txg_input(txg_t *g, const frg_input *e)
{
    const wid_t *w;
    int k;
    switch (e->kind) {
    case FRG_DOWN: case FRG_DOUBLE:
        if (e->button != FRG_LEFT) return 0;
        if (g->drag) drag_end(g);
        w = hit(g, e->x, e->y);
        if (g->pick >= 0) {              /* the picker: a swatch, or anything else closes it */
            if (w && w->kind == TXG_W_SWATCH) { txp_ui_col_set(g->c, (int)w->id, w->v); return 3; }
            if (!w || w->kind != TXG_W_PICKER) g->pick = -1;
            return 3;
        }
        if (g->ov == OV_SAVE && (!w || (w->kind != TXG_W_SLOT && w->kind != TXG_W_NAME && w->kind != TXG_W_SAVEGO &&
                                        !(w->kind == TXG_W_PICKER && w->lane == -2)))) {   /* (outside SAVE: it closes;
                                          * on SAVE itself: closed) */
            g->ov = OV_NONE;
            if (w && w->kind == TXG_W_SAVE) return 3;
        }
        if (!w) return 2;
        k = w->lane;
        switch (w->kind) {
        case TXG_W_PREV: step_param(g, TXP_ID_PRESET, -1, 1); break;
        case TXG_W_NEXT: step_param(g, TXP_ID_PRESET, 1, 1); break;
        case TXG_W_PRESET:
            if (g->ov == OV_PRE) g->ov = OV_NONE;
            else { if (g->ov == OV_SND) sb_close(g, 0); pb_open(g); }
            break;
        case TXG_W_LDN: step_param(g, TXP_ID_LANES, -1, 0); break;
        case TXG_W_LUP: case TXG_W_ADD: step_param(g, TXP_ID_LANES, 1, 0); break;
        case TXG_W_SET:
            if (g->ov == OV_SND) sb_close(g, 0);
            g->ov = g->ov == OV_SET ? OV_NONE : OV_SET; g->pick = -1; g->q[0] = 0;
            break;
        case TXG_W_SAVE: if (g->ov == OV_SND) sb_close(g, 0); save_open(g); break;
        case TXG_W_UNDO: txp_ui_undo(g->c); break;
        case TXG_W_REC: txp_ui_rec(g->c); break;
        case TXG_W_SLOT: if (k >= 0 && k < (int)TXP_USER_N) { g->sv_slot = k; if (g->sn.ui.user >> k & 1u) snprintf(g->sv_name, sizeof g->sv_name, "%.11s", g->sn.ui.uname[k]); } if (e->kind == FRG_DOUBLE) save_go(g); break;
        case TXG_W_SAVEGO: save_go(g); break;
        case TXG_W_NAME: break;
        case TXG_W_CLOSE: if (g->ov == OV_SND) sb_close(g, 0); else g->ov = OV_NONE; break;
        case TXG_W_KEEP: sb_close(g, 1); break;
        case TXG_W_LOAD: pb_load(g); break;
        case TXG_W_SEARCH: break;
        case TXG_W_NAV:
            g->q[0] = 0;
            if (g->ov == OV_SND) { g->sb_fam = k; g->sb_top = 0; }
            else { g->pb_bank = k; g->pb_top = 0; pb_items(g); if (g->nitems) g->pb_cur = g->items[0]; }
            break;
        case TXG_W_LIST:
            if (k < 0 || k >= g->nitems) break;
            if (g->ov == OV_SND) {
                sb_cursor(g, g->items[k] >> 4, g->items[k] & 15, 1);
                if (e->kind == FRG_DOUBLE) sb_close(g, 1);
            } else {
                g->pb_cur = g->items[k];
                if (e->kind == FRG_DOUBLE) pb_load(g);
            }
            break;
        case TXG_W_SEG: set_param(g, w->id, w->v); break;
        case TXG_W_FIELD:
            if (e->kind == FRG_DOUBLE && txp_ui_default(g->c, w->id) >= 0) { set_param(g, w->id, txp_ui_default(g->c, w->id)); break; }
            step_param(g, w->id, (e->mods & (FRG_SHIFT | FRG_ALT)) ? -1 : 1, 1);
            break;
        case TXG_W_TEMPO: tempo_step(g, (e->mods & (FRG_SHIFT | FRG_ALT)) ? -w->v : w->v); break;
        case TXG_W_FXOPEN:
            if (g->ov == OV_FX && (k < SEC_MOVE ? g->fx_tab == k : g->fx_tab >= TAB_LFO0)) g->ov = OV_NONE;
            else { g->ov = OV_FX; g->fx_tab = k < SEC_MOVE ? k : (g->fx_tab >= TAB_LFO0 ? g->fx_tab : TAB_LFO0); }
            break;
        case TXG_W_FXTAB: g->ov = OV_FX; g->fx_tab = k; break;
        case TXG_W_WSIZE: txp_ui_win_resize(g->c, w->v); break;
        case TXG_W_VIEWSEL: g->view = w->v; txp_ui_view_set(g->c, w->v); break;
        case TXG_W_VMAP: g->view = 0; txp_ui_view_set(g->c, 0); break;
        case TXG_W_VSPACE: g->view = 1; txp_ui_view_set(g->c, 1); break;
        case TXG_W_SND: sb_open(g, k); break;
        case TXG_W_COLDOT:
            if ((int)w->id < 0 || (int)w->id >= g->cat->neng) break;
            g->pick = (int)w->id; g->pick_x = (int)w->r[0]; g->pick_y = (int)(w->r[1] + w->r[3] + 6.f);
            break;
        case TXG_W_SWATCH: txp_ui_col_set(g->c, (int)w->id, w->v); break;
        case TXG_W_COLRESET: txp_ui_col_reset(g->c); break;
        case TXG_W_ROW: select_lane(g, k); break;
        case TXG_W_MUTE: txp_ui_mute(g->c, (uint32_t)k, !(g->mute >> k & 1u)); g->mute ^= 1u << k; break;
        case TXG_W_SOLO: {
            uint32_t m = g->solo ^ (1u << k);
            if (e->mods & (FRG_ALT | FRG_CMD | FRG_CTRL)) m = g->solo == (1u << k) ? 0u : 1u << k;   /* (alone) */
            txp_ui_solo(g->c, m); g->solo = m;
            break;
        }
        case TXG_W_VOL:
            select_lane(g, k);
            if (e->kind == FRG_DOUBLE) { set_param(g, TXP_ID_LANE(k, TXP_PL_LEVEL), 100); break; }   /* (the sound as tuned) */
            g->drag = TXG_W_VOL; g->drag_lane = k;
            txp_ui_begin(g->c, TXP_ID_LANE(k, TXP_PL_LEVEL));
            vol_at(g, k, e->x);
            break;
        case TXG_W_MANY: case TXG_W_ACT: {   /* a block: that many; the top lit one again: one less (the first: OFF) */
            int act = w->kind == TXG_W_ACT, cur = act ? g->act[k] : g->cnt[k], v;
            select_lane(g, k);
            g->drag = w->kind; g->drag_lane = k; g->drag_id = w->id; g->drag_rx = w->r[0]; g->drag_begun = 0;
            v = steps_at(g, act, e->x);
            if (v == cur && v > 0) v--;
            txp_ui_begin(g->c, w->id); g->drag_begun = 1;
            txp_ui_set(g->c, w->id, v);
            if (act) g->act[k] = v; else g->cnt[k] = v;
            break;
        }
        case TXG_W_PARAM:
            if (lane_of_id(w->id) >= 0) select_lane(g, lane_of_id(w->id));
            if (e->kind == FRG_DOUBLE) { int d = p_default(g, w->id); if (d >= 0) set_param(g, w->id, d); break; }
            g->drag = TXG_W_PARAM; g->drag_id = w->id; g->drag_v0 = txp_ui_get(g->c, w->id);
            g->drag_x0 = e->x; g->drag_y0 = e->y; g->drag_moved = 0; g->drag_begun = 0;
            g->drag_fine = (e->mods & FRG_SHIFT) != 0;
            g->drag_rw = (float)(e->mods & (FRG_ALT | FRG_CMD | FRG_CTRL) ? -1 : 1);
            break;
        case TXG_W_DOT:
            select_lane(g, k);
            g->drag = TXG_W_DOT; g->drag_lane = k;
            g->drag_dist = g->sn.ui.lane[k].dist != 0xFF ? g->sn.ui.lane[k].dist : -1;
            {   /* (SPACE: the dot keeps its place under the pointer: the offset kept) */
                float x, y;
                sp_pos(g, g->pan[k], g->dist[k], &x, &y, 0);
                g->drag_dx = e->x - x; g->drag_dy = e->y - y;
            }
            txp_ui_begin(g->c, TXP_ID_LANE(k, TXP_PL_PAN));
            if (g->drag_dist >= 0) txp_ui_begin(g->c, TXP_ID_LANE(k, TXP_PL_CARD0 + g->drag_dist));
            break;
        }
        return 3;
    case FRG_MOVE:
        if (g->drag == TXG_W_VOL) { vol_at(g, g->drag_lane, e->x); return 3; }
        if (g->drag == TXG_W_DOT) { dot_at(g, g->drag_lane, e->x, e->y); return 3; }
        if (g->drag == TXG_W_PARAM) { param_drag(g, e->x, e->y); return 3; }
        if (g->drag == TXG_W_MANY || g->drag == TXG_W_ACT) {
            int act = g->drag == TXG_W_ACT, v = steps_at(g, act, e->x);
            txp_ui_set(g->c, g->drag_id, v);
            if (act) g->act[g->drag_lane] = v; else g->cnt[g->drag_lane] = v;
            return 3;
        }
        w = hit(g, e->x, e->y);
        {
            int hk = 0, hl = -1;
            if (w && (w->kind == TXG_W_ADD || w->kind == TXG_W_DOT || w->kind == TXG_W_LIST)) { hk = w->kind; hl = w->lane; }
            if (hk != g->hover_kind || hl != g->hover_lane) { g->hover_kind = hk; g->hover_lane = hl; return 1; }
        }
        return 0;
    case FRG_UP:
        if (g->drag == TXG_W_PARAM && !g->drag_moved && choice(g->drag_id)) {   /* a click on a choice: the next */
            g->drag = 0;
            step_param(g, g->drag_id, g->drag_rw < 0.f ? -1 : 1, 1);
            return 3;
        }
        if (g->drag) { drag_end(g); return 3; }
        return 0;
    case FRG_LEAVE: case FRG_FOCUS_LOST:
        if (g->drag) drag_end(g);
        g->hover_kind = 0; g->hover_lane = -1;
        return 1;
    case FRG_WHEEL:
        w = hit(g, e->x, e->y);
        if (g->ov == OV_SND || g->ov == OV_PRE) {   /* a list: scrolled; a choice: stepped */
            int d = e->wheel > 0.f ? -1 : 1;
            if (w && w->kind == TXG_W_SEG) { step_param(g, w->id, -d, 0); return 3; }
            if (g->ov == OV_SND) g->sb_top += d * 2; else g->pb_top += d * 2;
            if (g->sb_top < 0) g->sb_top = 0;
            if (g->pb_top < 0) g->pb_top = 0;
            return 3;
        }
        if (w && (w->kind == TXG_W_FIELD || w->kind == TXG_W_SEG || w->kind == TXG_W_PARAM || w->kind == TXG_W_MANY || w->kind == TXG_W_ACT)) {
            const txp_param_t *p = pinfo(w->id);
            int d = e->wheel > 0.f ? 1 : -1;
            if (p && !choice(w->id) && p->hi >= 100 && fabsf(e->wheel) >= 1.f) d *= 2;
            step_param(g, w->id, d, 0);
            return 3;
        }
        if (w && w->kind == TXG_W_TEMPO) { tempo_step(g, (e->wheel > 0.f ? 1 : -1) * w->v); return 3; }
        if (w && w->kind == TXG_W_VOL) {
            uint32_t id = TXP_ID_LANE(w->lane, TXP_PL_LEVEL);
            int d = (int)lroundf(e->wheel * 2.f);
            if (!d) d = e->wheel > 0.f ? 1 : -1;
            step_param(g, id, d, 0);
            return 3;
        }
        if (w && (w->kind == TXG_W_ROW || w->kind == TXG_W_ROWS || w->kind == TXG_W_SND || w->kind == TXG_W_MUTE || w->kind == TXG_W_SOLO || w->kind == TXG_W_ADD)) {
            int t0 = g->rtop;            /* (the rows: scrolled) */
            g->rtop += e->wheel > 0.f ? -1 : 1;
            rows_clamp(g);
            return t0 != g->rtop ? 3 : 2;
        }
        return 0;
    case FRG_KEY:
        if (g->ov || g->pick >= 0) {
            if (!g->ov && e->key == FRK_ESCAPE) { g->pick = -1; return 3; }
            if (g->ov) return ov_key(g, e);
        }
        if (e->key == FRK_UP && g->sel > 0) { select_lane(g, g->sel - 1); return 3; }
        if (e->key == FRK_DOWN && g->sel + 1 < g->nlane) { select_lane(g, g->sel + 1); return 3; }
        if (e->key == FRK_RETURN) { sb_open(g, g->sel); return 3; }   /* (the selected lane's sound browser) */
        if ((e->key == 'm' || e->key == 'M') && used(g, g->sel)) { txp_ui_mute(g->c, (uint32_t)g->sel, !(g->mute >> g->sel & 1u)); g->mute ^= 1u << g->sel; return 3; }
        if ((e->key == 's' || e->key == 'S') && used(g, g->sel)) { g->solo ^= 1u << g->sel; txp_ui_solo(g->c, g->solo); return 3; }
        if (e->key == 'v' || e->key == 'V') { g->view = !g->view; txp_ui_view_set(g->c, g->view); return 3; }
        if (e->key == ' ') { txp_ui_rec(g->c); return 3; }   /* (space: PLAY ALL / STOP ALL) */
        return 0;
    }
    return 0;
}
int txg_find(const txg_t *g, int kind, int lane, float r[4])
{
    for (int i = 0; i < g->nw; i++)
        if (g->w[i].kind == kind && g->w[i].lane == lane) { memcpy(r, g->w[i].r, 4 * sizeof(float)); return 1; }
    return 0;
}

/* ---- the platform's client */
static void cl_paint(void *ctx, frui_canvas *c, float scale)
{
    txg_t *g = ctx;
    (void)scale;                        /* (the canvas's size is the scale: txg_scale_of) */
    (void)txg_tick(g);
    (void)paint_into(g, c);
}
static int cl_input(void *ctx, const frg_input *e)
{
    txg_t *g = ctx;
    int r = txg_input(g, e);
    if (r & 1) (void)txg_tick(g);
    return r;
}
static int cl_tick(void *ctx) { return txg_tick((txg_t *)ctx); }
const frg_client *txg_client(txg_t *g) { return &g->cl; }
void txg_fit_size(uint32_t *w, uint32_t *h)   /* any aspect: the width and the height each in its range */
{
    *w = *w < TXG_MIN_W ? TXG_MIN_W : *w > TXG_MAX_W ? TXG_MAX_W : *w;
    *h = *h < TXG_MIN_H ? TXG_MIN_H : *h > TXG_MAX_H ? TXG_MAX_H : *h;
}
txg_t *txg_new(txp_core *c)
{
    txg_t *g = calloc(1, sizeof *g);
    if (!g) return 0;
    g->c = c;
    g->cat = txp_catalog();
    g->font = frui_font_load(txg_font_text, (size_t)txg_font_text_len);
    if (!g->font) { free(g); return 0; }
    g->hover_lane = -1;
    g->drag_dist = -1;
    g->pick = -1;
    g->sb_kf = g->sb_ki = -1;
    g->pb_cur = -1;
    g->sel_seen = -1;
    g->view = txp_ui_view(c);
    fit(g, TXG_W, TXG_H);
    g->cl.ctx = g; g->cl.paint = cl_paint; g->cl.input = cl_input; g->cl.tick = cl_tick;
    (void)txg_tick(g);
    return g;
}
void txg_free(txg_t *g)
{
    if (!g) return;
    if (g->drag) drag_end(g);
    if (g->ov == OV_SND) sb_close(g, 0);
    frui_font_free(g->font);
    free(g->px);
    free(g);
}
