/* SPDX-License-Identifier: GPL-3.0-only */
/* The plugin window's core (gui_core.h). Float is allowed in plugin/ (the app itself stays integer). */
#include "gui_core.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* the engines' colours (txp_ui.h TXP_COL_N; 0 bone, every engine's at the start). No red: red is the selection's and
 * the LFOs' */
static const uint32_t PAL[TXP_COL_N] = { C_INK, 0xFFE07A4Au, 0xFFD9A650u, 0xFFCBB48Au, 0xFF9AA86Au, 0xFF8FB39Cu,
                                         0xFF5FA3A0u, 0xFF9CC7D6u, 0xFF7F93C4u, 0xFFA993C7u, 0xFFC98A9Cu };
static const char *const PAL_N[TXP_COL_N] = { "BONE", "EMBER", "AMBER", "SAND", "MOSS", "SAGE", "TEAL", "ICE", "SLATE",
                                              "LILAC", "ROSE" };

#define NBLK 1034u                       /* the event lines' span: 6 s of app blocks (256 frames at 44.1 kHz) */
#define GLOW_BLK 26u                     /* a dot's glow: the last 0.15 s */
#define TOP_H 52.f
#define MAP_W 440.f
#define HDR_H 30.f
#define ROW_Y0 (TOP_H + HDR_H)
#define ROWS_H (TXG_H - ROW_Y0 - 10.f)
#define ROW_HMIN 39.f
#define ROW_HMAX 56.f
/* a row's columns, from the rows' left edge (MAP_W) */
#define RX_NUM 16.f
#define RX_DOT 44.f
#define RX_NAME 56.f
#define RX_TL 200.f
#define R_PADR 16.f
#define RW_MS 48.f
#define RW_VOL 100.f
#define RW_CH 44.f
/* the map's field: you at (MAP_CX, MAP_CY), the outer ring MAP_RX x MAP_RY */
#define MAP_CX (MAP_W * 0.5f)
#define MAP_CY (TXG_H - 44.f)
#define MAP_RX 196.f
#define MAP_RY 380.f
#define MAP_ANG 1.3962634f               /* PAN at either end: 80 degrees off the middle */
#define MAP_R0 0.12f                     /* DIST 0's distance (the nearest ring: 0.25) */
#define MAP_RNONE 0.5f                   /* a sound without DIST: the middle ring */
#define MAP_CROWD 8                      /* more lanes than this on the map: a label names only the selected or hovered */
/* SPACE: the ground in perspective under the selected lane's lines, the horizon at SP_HZ */
#define SP_Y0 252.f
#define SP_Y1 (TXG_H - 14.f)
#define SP_HZ (SP_Y0 + (SP_Y1 - SP_Y0) * 0.1f)
#define SP_NEAR 0.36f                    /* the ground's half width at the outer ring's side: near (+ SP_FAR far) x MAP_W */
#define SP_FAR 0.14f
/* the overlays (the browsers, SETTINGS): a header, then a nav, a list, a preview */
#define OV_HDR 46.f
#define OV_Y0 (TOP_H + OV_HDR)
#define OV_NAV 190.f
#define OV_PREV 320.f
#define OV_RH 28.f
#define LFO_H 48                         /* the LFOs' traces: the last 48 ticks */
#define MAX_W 480
#define LIST_MAX 128

enum { OV_NONE, OV_SET, OV_SND, OV_PRE };
enum { PB_USER = 100, PB_LAND = 101 };   /* the presets browser's banks past the factory's categories */

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
    /* what is drawn */
    txp_snap_t sn;
    int nlane, sel, ov, view;
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
    /* input */
    int drag, drag_lane, drag_dist;      /* drag: 0 none, TXG_W_DOT, TXG_W_VOL */
    float drag_dx, drag_dy;              /* (SPACE: the pointer off the dot when it was taken) */
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
static void upper(char *d, const char *s, size_t n)
{
    size_t i;
    for (i = 0; i + 1 < n && s[i]; i++) d[i] = s[i] >= 'a' && s[i] <= 'z' ? (char)(s[i] - 32) : s[i];
    d[i] = 0;
}
/* a button: a label centred in a box (on: the selected segment's look) */
static void button(txg_t *g, frui_canvas *c, float x, float y, float w, float h, const char *t, int pri)
{
    RR(g, c, x, y, w, h, 3.f, pri ? C_INK : C_BTN);
    RRS(g, c, x, y, w, h, 3.f, pri ? C_INK : C_LINE);
    LBL(g, c, x + (w - LBLW(g, 11.f, t)) * 0.5f, y + h * 0.5f + CAP(g, 11.f) * 0.5f, 11.f, pri ? C_PANEL : C_INK, t);
}

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--) { h ^= *b++; h *= 16777619u; }
    return h;
}

/* ---- the layout */
static float row_h(const txg_t *g)
{
    float h = ROWS_H / (float)(g->nlane + (g->nlane < (int)TXP_LANES ? 1 : 0));
    return h < ROW_HMIN ? ROW_HMIN : h > ROW_HMAX ? ROW_HMAX : h;
}
static float tl_w(void) { return TXG_W - MAP_W - RX_TL - (14.f + RW_CH + 12.f + RW_VOL + 12.f + RW_MS + R_PADR); }
static int used(const txg_t *g, int k) { return k < g->nlane && g->sn.ui.lane[k].used; }
static int faded(const txg_t *g, int k)   /* muted, or another lane soloed */
{
    uint32_t live = g->solo & ((1u << g->nlane) - 1u);
    return (g->mute >> k & 1u) || (live && !(live >> k & 1u));
}
static uint32_t eng_col(const txg_t *g, int eng) { int i = eng >= 0 && eng < (int)TXP_ENG_MAX ? txp_ui_col(g->c, eng) : 0; return PAL[i >= 0 && i < TXP_COL_N ? i : 0]; }
static uint32_t lane_col(const txg_t *g, int k) { return PAL[g->col[k] >= 0 && g->col[k] < TXP_COL_N ? g->col[k] : 0]; }
static void map_pos(int pan, int dist, float *x, float *y)
{
    float a = (float)(pan - 64) / 64.f * MAP_ANG, r = dist < 0 ? MAP_RNONE : MAP_R0 + (1.f - MAP_R0) * (float)dist / 127.f;
    *x = MAP_CX + sinf(a) * r * MAP_RX;
    *y = MAP_CY - cosf(a) * r * MAP_RY;
}
static void map_inv(float x, float y, int *pan, int *dist)
{
    float dx = (x - MAP_CX) / MAP_RX, dy = (MAP_CY - y) / MAP_RY, a, r;
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
static void sp_pos(int pan, int dist, float *x, float *y, float *sc)   /* (the map's place on the ground: its side
                                          * across, its ahead into the distance) */
{
    float a = (float)(pan - 64) / 64.f * MAP_ANG, r = dist < 0 ? MAP_RNONE : MAP_R0 + (1.f - MAP_R0) * (float)dist / 127.f,
          depth = 1.f - cosf(a) * r;
    depth = depth < 0.f ? 0.f : depth;
    *y = SP_HZ + (SP_Y1 - SP_HZ) * powf(0.06f + depth * 0.9f, 1.7f);
    *x = MAP_CX + sinf(a) * r * MAP_W * (SP_FAR + SP_NEAR * depth);
    if (sc) *sc = 0.3f + depth;
}
static void sp_inv(float x, float y, int fixed, int *pan, int *dist)   /* fixed: no DIST (the middle ring: PAN only) */
{
    float t = (y - SP_HZ) / (SP_Y1 - SP_HZ), depth, ahead, side, a, r;
    t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
    depth = (powf(t, 1.f / 1.7f) - 0.06f) / 0.9f;
    depth = depth < 0.f ? 0.f : depth > 1.f ? 1.f : depth;
    ahead = 1.f - depth;
    side = (x - MAP_CX) / (MAP_W * (SP_FAR + SP_NEAR * depth));
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
static void draw_top(txg_t *g, frui_canvas *c)
{
    const float y = 12.f, h = 28.f, yc = y + h * 0.5f;
    char tag[8] = "", name[24] = "", t[32];
    const char *pr = g->sn.ui.preset, *sp = strchr(pr, ' ');
    float x, bx0, bx1 = TXG_W - 372.f;
    R(g, c, 0, TOP_H - 1.f, TXG_W, 1.f, C_LINE);
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
    x = bx0 + 12.f;
    if (tag[0]) x += TX(g, c, x, yc + CAP(g, 13.f) * 0.5f, 13.f, 500.f, 0.4f, FRUI_TNUM, C_RED, tag) + 12.f;
    x += TX(g, c, x, yc + CAP(g, 13.f) * 0.5f, 13.f, 500.f, 0.6f, 0, C_INK, name);
    if (g->sn.ui.edited) TX(g, c, x + 6.f, yc + CAP(g, 13.f) * 0.5f, 13.f, 500.f, 0.f, 0, C_DIM, "*");
    snprintf(t, sizeof t, "%d LANE%s", g->nlane, g->nlane == 1 ? "" : "S");
    LBL(g, c, bx1 - 26.f - LBLW(g, 10.5f, t), yc + CAP(g, 10.5f) * 0.5f, 10.5f, C_DIM, t);
    caret(g, c, bx1 - 14.f, yc, C_DIM);
    /* LANES */
    x = TXG_W - 318.f;
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
    /* SETTINGS */
    x = TXG_W - 140.f;
    RR(g, c, x, y, 124.f, h, 3.f, g->ov == OV_SET ? C_SEG : C_BTN); RRS(g, c, x, y, 124.f, h, 3.f, C_LINE);
    RI(g, c, x + 20.f, yc, 3.6f, 1.5f, C_INK);
    for (int i = 0; i < 8; i++) {
        float a = (float)i * 0.785398f, ca = cosf(a), sa = sinf(a);
        LN(g, c, x + 20.f + ca * 5.6f, yc + sa * 5.6f, x + 20.f + ca * 7.4f, yc + sa * 7.4f, 1.6f, C_INK);
    }
    LBL(g, c, x + 34.f, yc + CAP(g, 12.f) * 0.5f, 12.f, C_INK, "SETTINGS");
    wid_add(g, TXG_W_SET, -1, x, y, 124.f, h);
}

static void ellipse_arc(txg_t *g, frui_canvas *c, float r, uint32_t col)   /* the upper half of a ring */
{
    float lw = g->S < 1.5f ? 1.f / g->S : 0.75f, px = MAP_CX - r * MAP_RX, py = MAP_CY;
    for (int i = 1; i <= 64; i++) {
        float a = 3.14159265f * (1.f - (float)i / 64.f), x = MAP_CX + cosf(a) * r * MAP_RX, y = MAP_CY - sinf(a) * r * MAP_RY;
        LN(g, c, px, py, x, y, lw, col);
        px = x; py = y;
    }
}
/* the LFOs routed (red: what moves): each one's trace over the last ticks and its first route, at the map's top */
static void draw_lfos(txg_t *g, frui_canvas *c)
{
    float x = 160.f, y = TOP_H + 14.f, lw = g->S < 1.5f ? 1.f / g->S : 0.9f;
    int shown = 0, more = 0;
    for (int n = 0; n < (int)TXP_LFO_N; n++) {
        char t[40];
        float px = 0.f, py = 0.f, w = 44.f, h = 22.f;
        if (!(g->sn.ui.lfo_on >> n & 1u)) continue;
        if (shown >= 2) { more++; continue; }
        snprintf(t, sizeof t, "LFO %d", n + 1);
        LBL(g, c, x, y + 9.f, 9.f, C_RED, t);
        TX(g, c, x, y + 21.f, 9.5f, 450.f, 0.2f, 0, C_DIM, g->sn.ui.lfo_to[n]);
        {
            float xw = x + 82.f;
            R(g, c, xw, y + h - 1.f, w, 1.f, C_FAINT);
            for (int i = 0; i < LFO_H; i++) {
                float xx = xw + w * (float)i / (float)(LFO_H - 1), yy = y + h - 2.f - ((float)g->lfoh[n][i] / 32767.f * 0.5f + 0.5f) * (h - 4.f);
                if (i) LN(g, c, px, py, xx, yy, lw * 1.2f, C_RED);
                px = xx; py = yy;
            }
            CI(g, c, px, py, 2.2f, C_RED);
        }
        x += 136.f;
        shown++;
    }
    if (more) { char t[8]; snprintf(t, sizeof t, "+%d", more); LBL(g, c, x, y + 15.f, 10.f, C_RED, t); }
}
/* the selected lane over the field: its family, sound (a click: the sound browser), its engine's colour (a click: the
 * picker), PAN, DIST, HOW MANY, HOW ACTIVE, the channel; a value an LFO moves now red, with a "~" */
static void draw_selected(txg_t *g, frui_canvas *c)
{
    int k = g->sel;
    float x = 20.f, y = TOP_H + 78.f;
    const txp_ui_lane_t *l = &g->sn.ui.lane[k];
    char t[64], nm[32];
    snprintf(t, sizeof t, "LANE %d%s%s", k + 1, used(g, k) ? "  \xC2\xB7  " : "", used(g, k) ? l->fam : "");
    upper(t, t, sizeof t);
    LBL(g, c, x, y, 11.f, C_RED, t);
    if (used(g, k)) {
        char a[16], b[16];
        float w, xx;
        upper(nm, l->snd, sizeof nm);
        clipL(g, c, 0, TOP_H, MAP_W - 12.f, 120.f);
        w = TX(g, c, x, y + 36.f, 30.f, 700.f, 1.2f, 0, (g->mute >> k & 1u) ? C_DIM : C_INK, nm);
        frui_clip_reset(c);
        if (w > MAP_W - 90.f) w = MAP_W - 90.f;
        caret(g, c, x + w + 12.f, y + 24.f, C_DIM);
        wid_add(g, TXG_W_SND, k, x - 4.f, y + 8.f, w + 26.f, 34.f);
        xx = x + w + 34.f;
        CI(g, c, xx, y + 24.f, 6.f, lane_col(g, k));
        RI(g, c, xx, y + 24.f, 8.5f, 1.f, g->pick >= 0 && g->pick == l->eng ? C_INK : C_FAINT);
        wid_add2(g, TXG_W_COLDOT, k, (uint32_t)l->eng, 0, xx - 11.f, y + 13.f, 22.f, 22.f);
        ptext(TXP_ID_LANE(k, TXP_PL_PAN), (l->modm & TXP_MOD_PAN) ? l->lpan : g->pan[k], a, sizeof a);
        if (g->dist[k] >= 0) snprintf(b, sizeof b, "%d", (l->modm & TXP_MOD_DIST) ? l->ldist : g->dist[k]);
        else snprintf(b, sizeof b, "\xE2\x80\x94");
        {
            float xp = x;
            snprintf(t, sizeof t, "%sPAN %s", (l->modm & TXP_MOD_PAN) ? "~ " : "", a);
            xp += TX(g, c, xp, y + 62.f, 12.f, 450.f, 0.3f, FRUI_TNUM, (l->modm & TXP_MOD_PAN) ? C_RED : C_DIM, t);
            xp += TX(g, c, xp, y + 62.f, 12.f, 450.f, 0.3f, 0, C_DIM, "   \xC2\xB7   ");
            snprintf(t, sizeof t, "%sDIST %s", (l->modm & TXP_MOD_DIST) ? "~ " : "", b);
            TX(g, c, xp, y + 62.f, 12.f, 450.f, 0.3f, FRUI_TNUM, (l->modm & TXP_MOD_DIST) ? C_RED : C_DIM, t);
        }
        ptext(TXP_ID_LANE(k, TXP_PL_COUNT), g->cnt[k], a, sizeof a);
        {
            float xp = x;
            snprintf(t, sizeof t, "%sHOW MANY %s", (l->modm & TXP_MOD_COUNT) ? "~ " : "", a);
            xp += TX(g, c, xp, y + 82.f, 12.f, 450.f, 0.3f, FRUI_TNUM, (l->modm & TXP_MOD_COUNT) ? C_RED : C_DIM, t);
            xp += TX(g, c, xp, y + 82.f, 12.f, 450.f, 0.3f, 0, C_DIM, "   \xC2\xB7   ");
            ptext(TXP_ID_LANE(k, TXP_PL_ACTIVE), g->act[k], a, sizeof a);
            snprintf(t, sizeof t, "%sHOW ACTIVE %s", (l->modm & TXP_MOD_ACTIVE) ? "~ " : "", a);
            TX(g, c, xp, y + 82.f, 12.f, 450.f, 0.3f, FRUI_TNUM, (l->modm & TXP_MOD_ACTIVE) ? C_RED : C_DIM, t);
        }
        if (l->ch) snprintf(t, sizeof t, "MIDI CH %d%s", l->ch, (g->mute >> k & 1u) ? "   \xC2\xB7   MUTED" : "");
        else snprintf(t, sizeof t, "MIDI OFF%s", (g->mute >> k & 1u) ? "   \xC2\xB7   MUTED" : "");
        TX(g, c, x, y + 102.f, 12.f, 450.f, 0.3f, FRUI_TNUM, C_DIM, t);
    } else {
        float w = TX(g, c, x, y + 36.f, 30.f, 700.f, 1.2f, 0, C_FAINT, "EMPTY");
        caret(g, c, x + w + 12.f, y + 24.f, C_FAINT);
        wid_add(g, TXG_W_SND, k, x - 4.f, y + 8.f, w + 26.f, 34.f);
        TX(g, c, x, y + 62.f, 12.f, 450.f, 0.3f, 0, C_FAINT, "Choose a sound: click the name");
    }
}
/* a lane's label on a place: "3 Campfire", or "3" alone when the place is crowded and the lane is neither selected nor
 * under the mouse */
static void lane_label(const txg_t *g, int k, int crowd, char *t, size_t n)
{
    char nm[32];
    cap_name(nm, g->sn.ui.lane[k].snd, sizeof nm);
    if (crowd && k != g->sel && !(g->hover_kind == TXG_W_DOT && g->hover_lane == k)) snprintf(t, n, "%d", k + 1);
    else snprintf(t, n, "%d %s", k + 1, nm);
}
static int n_used(const txg_t *g) { int n = 0; for (int k = 0; k < g->nlane; k++) n += used(g, k); return n; }
static void draw_view_switch(txg_t *g, frui_canvas *c)
{
    float x = 16.f, y = TOP_H + 14.f, wm = 58.f, ws = 66.f, h = 24.f, xo = g->view ? x + wm : x, wo = g->view ? ws : wm;
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
/* the lanes' labels on a place (pts: the dots, on: drawn): right, left, above, below the dot ...: the first place clear
 * of the dots, the other labels and the edges (y0 .. y1), the selected lane's first, then the one under the mouse */
static void place_labels(txg_t *g, frui_canvas *c, float (*pts)[2], const int *on, float y0, float y1)
{
    float boxes[TXP_LANES * 2][4];
    int nb = 0, k, order[TXP_LANES], no = 0, crowd = n_used(g) > MAP_CROWD;
    char t[64];
    for (k = 0; k < g->nlane; k++) {
        if (!on[k]) continue;
        boxes[nb][0] = pts[k][0] - 7.f; boxes[nb][1] = pts[k][1] - 7.f; boxes[nb][2] = pts[k][0] + 7.f; boxes[nb][3] = pts[k][1] + 7.f;
        nb++;
    }
    if (g->sel < g->nlane && on[g->sel]) order[no++] = g->sel;
    if (g->hover_kind == TXG_W_DOT && g->hover_lane >= 0 && g->hover_lane < g->nlane && on[g->hover_lane] && g->hover_lane != g->sel)
        order[no++] = g->hover_lane;
    for (k = 0; k < g->nlane; k++) if (on[k] && k != g->sel && !(g->hover_kind == TXG_W_DOT && k == g->hover_lane)) order[no++] = k;
    for (int o = 0; o < no; o++) {
        float x, y, w, tries[8][2], *at = 0;
        k = order[o]; x = pts[k][0]; y = pts[k][1];
        lane_label(g, k, crowd, t, sizeof t);
        w = TW(g, 11.f, 600.f, 0.f, 0, t);
        tries[0][0] = x + 11.f; tries[0][1] = y - 6.f; tries[1][0] = x - 11.f - w; tries[1][1] = y - 6.f;
        tries[2][0] = x - w * 0.5f; tries[2][1] = y - 22.f; tries[3][0] = x - w * 0.5f; tries[3][1] = y + 10.f;
        tries[4][0] = x + 11.f; tries[4][1] = y - 18.f; tries[5][0] = x - 11.f - w; tries[5][1] = y + 6.f;
        tries[6][0] = x + 11.f; tries[6][1] = y + 6.f; tries[7][0] = x - 11.f - w; tries[7][1] = y - 18.f;
        for (int i = 0; i < 8 && !at; i++) {
            float b[4] = { tries[i][0] - 2.f, tries[i][1] - 1.f, tries[i][0] + w + 2.f, tries[i][1] + 13.f };
            int hit = b[0] < 2.f || b[2] > MAP_W - 2.f || b[1] < y0 || b[3] > y1;
            for (int j = 0; j < nb && !hit; j++)
                hit = b[0] < boxes[j][2] && b[2] > boxes[j][0] && b[1] < boxes[j][3] && b[3] > boxes[j][1];
            if (!hit) at = tries[i];
        }
        if (!at) at = tries[0];
        if (nb < (int)(TXP_LANES * 2)) {
            boxes[nb][0] = at[0] - 2.f; boxes[nb][1] = at[1] - 1.f; boxes[nb][2] = at[0] + w + 2.f; boxes[nb][3] = at[1] + 13.f;
            nb++;
        }
        TX(g, c, at[0], at[1] + 10.f, 11.f, 600.f, 0.f, 0, k == g->sel ? C_INK : faded(g, k) ? C_FAINT : C_DIM, t);
    }
}
static void draw_map_field(txg_t *g, frui_canvas *c)
{
    static const float RINGS[4] = {0.25f, 0.5f, 0.75f, 1.f};
    float lw = g->S < 1.5f ? 1.f / g->S : 0.75f, pts[TXP_LANES][2];
    int on[TXP_LANES], k;
    for (int i = 0; i < 4; i++) ellipse_arc(g, c, RINGS[i], C_FAINT);
    for (int a = -60; a <= 60; a += 30) {
        float r = (float)a * 3.14159265f / 180.f;
        LN(g, c, MAP_CX, MAP_CY, MAP_CX + sinf(r) * MAP_RX, MAP_CY - cosf(r) * MAP_RY, lw, C_FAINT);
    }
    LN(g, c, MAP_CX - MAP_RX - 10.f, MAP_CY, MAP_CX + MAP_RX + 10.f, MAP_CY, lw, C_FAINT);
    {
        frui_path p;
        frui_path_init(&p);
        frui_path_move(&p, MAP_CX * g->S, (MAP_CY - 7.f) * g->S);
        frui_path_line(&p, (MAP_CX + 6.f) * g->S, (MAP_CY + 4.f) * g->S);
        frui_path_line(&p, (MAP_CX - 6.f) * g->S, (MAP_CY + 4.f) * g->S);
        frui_path_fill(c, &p, C_INK);
        frui_path_free(&p);
    }
    TX(g, c, MAP_CX - TW(g, 10.f, 500.f, 1.f, 0, "YOU") * 0.5f, MAP_CY + 22.f, 10.f, 500.f, 1.f, 0, C_DIM, "YOU");
    TX(g, c, MAP_CX - MAP_RX - 22.f, MAP_CY + 4.f, 10.f, 500.f, 0.f, 0, C_DIM, "L");
    TX(g, c, MAP_CX + MAP_RX + 15.f, MAP_CY + 4.f, 10.f, 500.f, 0.f, 0, C_DIM, "R");
    TX(g, c, MAP_CX - TW(g, 10.f, 500.f, 1.f, 0, "FAR") * 0.5f, MAP_CY - MAP_RY - 8.f, 10.f, 500.f, 1.f, 0, C_DIM, "FAR");
    /* the lanes */
    for (k = 0; k < g->nlane; k++) {
        on[k] = used(g, k);
        if (!on[k]) continue;
        map_pos(g->pan[k], g->dist[k], &pts[k][0], &pts[k][1]);
    }
    for (k = 0; k < g->nlane; k++) {
        const txp_ui_lane_t *l = &g->sn.ui.lane[k];
        float a, mf;
        uint32_t col = lane_col(g, k);
        if (!on[k]) continue;
        a = glow(g, k); mf = faded(g, k) ? 0.25f : 1.f;
        if (l->modm & (TXP_MOD_PAN | TXP_MOD_DIST)) {   /* where the LFO has it now: a red trace off the dot */
            float lx, ly;
            map_pos((l->modm & TXP_MOD_PAN) ? l->lpan : g->pan[k], g->dist[k] < 0 ? -1 : (l->modm & TXP_MOD_DIST) ? l->ldist : g->dist[k], &lx, &ly);
            LN(g, c, pts[k][0], pts[k][1], lx, ly, 1.f, frui_alpha(C_RED, 0.6f * mf));
            CI(g, c, lx, ly, 2.6f, frui_alpha(C_RED, mf));
        }
        CI(g, c, pts[k][0], pts[k][1], 6.f + 6.f * a, frui_alpha(col, (0.08f + 0.22f * a) * mf));
        if (g->dist[k] >= 0) CI(g, c, pts[k][0], pts[k][1], 4.f, frui_alpha(col, mf < 1.f ? 0.35f : 1.f));
        else RI(g, c, pts[k][0], pts[k][1], 3.4f, 1.4f, frui_alpha(col, mf < 1.f ? 0.35f : 1.f));
        if (k == g->sel) RI(g, c, pts[k][0], pts[k][1], 9.f, 1.4f, C_RED);
        wid_add(g, TXG_W_DOT, k, pts[k][0] - 11.f, pts[k][1] - 11.f, 22.f, 22.f);
    }
    place_labels(g, c, pts, on, MAP_CY - MAP_RY - 24.f, MAP_CY - 2.f);
}
/* SPACE: the same lanes on the ground in perspective, far ones first; each one's events of the last 6 s drift from it
 * toward the horizon (its trail is its event line), its glow its newest; a drag moves it on the ground */
static void draw_space(txg_t *g, frui_canvas *c)
{
    float lw = g->S < 1.5f ? 1.f / g->S : 0.75f, vx = MAP_CX;
    float pts[TXP_LANES][2];
    int order[TXP_LANES], on[TXP_LANES] = {0}, no = 0;
    char t[64];
    clipL(g, c, 0, SP_Y0 - 8.f, MAP_W - 1.f, SP_Y1 - SP_Y0 + 22.f);
    for (int i = 0; i <= 7; i++) {        /* the ground: depth lines, rays to the vanishing point */
        float y = SP_HZ + (SP_Y1 - SP_HZ) * powf((float)i / 7.f, 1.7f);
        LN(g, c, 0.f, y, MAP_W, y, lw, frui_alpha(C_FAINT, 0.35f + 0.5f * (float)i / 7.f));
    }
    for (int i = -6; i <= 6; i++) LN(g, c, vx + (float)i * MAP_W * 0.025f, SP_HZ, vx + (float)i * MAP_W * 0.15f, SP_Y1, lw, frui_alpha(C_FAINT, 0.45f));
    TX(g, c, 12.f, SP_HZ - 6.f, 10.f, 500.f, 1.f, 0, C_DIM, "FAR");
    TX(g, c, 12.f, SP_Y1 + 4.f, 10.f, 500.f, 1.f, 0, C_DIM, "L");
    TX(g, c, MAP_W - 20.f, SP_Y1 + 4.f, 10.f, 500.f, 1.f, 0, C_DIM, "R");
    TX(g, c, MAP_CX - TW(g, 10.f, 500.f, 1.f, 0, "YOU") * 0.5f, SP_Y1 + 4.f, 10.f, 500.f, 1.f, 0, C_DIM, "YOU");
    for (int k = 0; k < g->nlane; k++)
        if (used(g, k)) { order[no++] = k; on[k] = 1; sp_pos(g->pan[k], g->dist[k], &pts[k][0], &pts[k][1], 0); }
    for (int i = 1; i < no; i++)         /* far first */
        for (int j = i; j > 0 && pts[order[j - 1]][1] > pts[order[j]][1]; j--) { int a = order[j - 1]; order[j - 1] = order[j]; order[j] = a; }
    for (int o = 0; o < no; o++) {
        int k = order[o];
        const txp_ui_lane_t *l = &g->sn.ui.lane[k];
        float x, y, sc, mf = faded(g, k) ? 0.25f : 1.f, a = glow(g, k), ex = 0.f, ey = 0.f;
        uint32_t col = lane_col(g, k);
        sp_pos(g->pan[k], g->dist[k], &x, &y, &sc);
        ex = vx + (x - vx) * 0.25f; ey = SP_HZ;
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
            sp_pos((l->modm & TXP_MOD_PAN) ? l->lpan : g->pan[k], g->dist[k] < 0 ? -1 : (l->modm & TXP_MOD_DIST) ? l->ldist : g->dist[k], &lx, &ly, 0);
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
    place_labels(g, c, pts, on, SP_HZ - 4.f, SP_Y1 - 2.f);
    if (g->drag == TXG_W_DOT && g->drag_lane < g->nlane && used(g, g->drag_lane)) {   /* the drag: where it is, in numbers */
        int k = g->drag_lane;
        float x, y;
        char a[16];
        sp_pos(g->pan[k], g->dist[k], &x, &y, 0);
        ptext(TXP_ID_LANE(k, TXP_PL_PAN), g->pan[k], a, sizeof a);
        if (g->dist[k] >= 0) snprintf(t, sizeof t, "PAN %s  \xC2\xB7  DIST %d", a, g->dist[k]);
        else snprintf(t, sizeof t, "PAN %s", a);
        {
            float w = TW(g, 10.5f, 500.f, 0.3f, FRUI_TNUM, t) + 14.f, bx = x - w * 0.5f;
            bx = bx < 4.f ? 4.f : bx + w > MAP_W - 4.f ? MAP_W - 4.f - w : bx;
            RR(g, c, bx, y - 34.f, w, 18.f, 3.f, C_BTN);
            TX(g, c, bx + 7.f, y - 21.f, 10.5f, 500.f, 0.3f, FRUI_TNUM, C_INK, t);
        }
    }
    frui_clip_reset(c);
    TX(g, c, MAP_W - 12.f - TW(g, 10.f, 450.f, 0.2f, 0, "Drag on the ground  \xC2\xB7  MAP places exactly"), SP_Y0 - 6.f, 10.f,
       450.f, 0.2f, 0, C_FAINT, "Drag on the ground  \xC2\xB7  MAP places exactly");
}
static void draw_map(txg_t *g, frui_canvas *c)
{
    R(g, c, 0, TOP_H, MAP_W, TXG_H - TOP_H, C_WELL);
    R(g, c, MAP_W - 1.f, TOP_H, 1.f, TXG_H - TOP_H, C_LINE);
    draw_view_switch(g, c);
    draw_lfos(g, c);
    draw_selected(g, c);
    if (g->view) draw_space(g, c); else draw_map_field(g, c);
}

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
static void draw_rows(txg_t *g, frui_canvas *c)
{
    const float x0 = MAP_W, rh = row_h(g), tw = tl_w(), xtl = x0 + RX_TL, xch = xtl + tw + 14.f, xvol = xch + RW_CH + 12.f,
                xms = xvol + RW_VOL + 12.f;
    char t[48], nm[32];
    /* the columns' names */
    {
        float yb = TOP_H + HDR_H * 0.5f + CAP(g, 10.f) * 0.5f;
        LBL(g, c, x0 + RX_NUM, yb, 10.f, C_DIM, "LANE");
        LBL(g, c, x0 + RX_NAME, yb, 10.f, C_DIM, "SOUND");
        LBL(g, c, xtl, yb, 10.f, C_DIM, "EVENTS");
        LBL(g, c, xtl + tw - TW(g, 10.f, 620.f, 1.4f, 0, "6 S"), yb, 10.f, C_FAINT, "6 S");
        LBL(g, c, xch, yb, 10.f, C_DIM, "CH");
        LBL(g, c, xvol, yb, 10.f, C_DIM, "VOL");
        LBL(g, c, xms + 7.f, yb, 10.f, C_DIM, "M");
        LBL(g, c, xms + 33.f, yb, 10.f, C_DIM, "S");
        R(g, c, x0, ROW_Y0 - 1.f, TXG_W - x0, 1.f, C_LINE);
    }
    for (int k = 0; k < g->nlane; k++) {
        const txp_ui_lane_t *l = &g->sn.ui.lane[k];
        float y = ROW_Y0 + rh * (float)k, yc = y + rh * 0.5f, mf = faded(g, k) ? 0.25f : 1.f, th = rh - 14.f;
        int u = used(g, k), sel = k == g->sel;
        uint32_t ink = u ? (mf < 1.f ? C_DIM : C_INK) : C_FAINT;
        if (sel) { R(g, c, x0, y, TXG_W - x0, rh, C_SEL); R(g, c, x0, y, 2.f, rh, C_RED); }
        R(g, c, x0, y + rh - 1.f, TXG_W - x0, 1.f, C_LINE);
        wid_add(g, TXG_W_ROW, k, x0, y, TXG_W - x0, rh);
        snprintf(t, sizeof t, "%d", k + 1);
        TX(g, c, x0 + RX_NUM, yc + CAP(g, 11.f) * 0.5f, 11.f, 500.f, 0.f, FRUI_TNUM, sel ? C_RED : C_DIM, t);
        if (u) {
            float a = glow(g, k), w;
            CI(g, c, x0 + RX_DOT, yc, 3.5f, frui_alpha(lane_col(g, k), (0.35f + 0.65f * a) * mf));
            cap_name(nm, l->snd, sizeof nm);
            clipL(g, c, x0 + RX_NAME, y, RX_TL - RX_NAME - 8.f, rh);
            w = TX(g, c, x0 + RX_NAME, yc - 1.f, 12.5f, 600.f, 0.f, 0, ink, nm);
            TX(g, c, x0 + RX_NAME, yc + 12.f, 10.f, 450.f, 0.3f, 0, C_DIM, l->fam);
            frui_clip_reset(c);
            if (w > RX_TL - RX_NAME - 8.f) w = RX_TL - RX_NAME - 8.f;
            wid_add(g, TXG_W_SND, k, x0 + RX_NAME - 2.f, yc - 13.f, w + 6.f, 17.f);
        } else {
            TX(g, c, x0 + RX_NAME, yc + CAP(g, 12.f) * 0.5f, 12.f, 450.f, 0.3f, 0, C_FAINT, "empty");
            wid_add(g, TXG_W_SND, k, x0 + RX_NAME - 2.f, yc - 9.f, 44.f, 18.f);
        }
        draw_line(g, c, k, xtl, yc - th * 0.5f, tw, th, mf);
        /* the channel */
        if (l->ch) snprintf(t, sizeof t, "%d", l->ch); else snprintf(t, sizeof t, "OFF");
        TX(g, c, xch, yc + CAP(g, 12.f) * 0.5f, 12.f, 500.f, 0.f, FRUI_TNUM, u ? C_INK : C_FAINT, t);
        /* VOL (red while an LFO moves it) */
        if (u) {
            float v = (float)g->level[k] / 127.f, ty = yc + 3.f;
            uint32_t kc = (l->modm & TXP_MOD_LEVEL) ? C_RED : mf < 1.f ? C_DIM : C_INK;
            snprintf(t, sizeof t, "%s%d", (l->modm & TXP_MOD_LEVEL) ? "~ " : "", g->level[k]);
            TX(g, c, xvol + RW_VOL - TW(g, 11.f, 500.f, 0.f, FRUI_TNUM, t), yc - 5.f, 11.f, 500.f, 0.f, FRUI_TNUM,
               (l->modm & TXP_MOD_LEVEL) ? C_RED : ink, t);
            RR(g, c, xvol, ty, RW_VOL, 4.f, 2.f, C_FAINT);
            RR(g, c, xvol, ty, RW_VOL * v, 4.f, 2.f, mf < 1.f ? C_DIM : C_INK);
            RR(g, c, xvol + RW_VOL * v - 4.f, ty - 4.f, 8.f, 12.f, 2.f, kc);
            wid_add(g, TXG_W_VOL, k, xvol - 4.f, yc - 14.f, RW_VOL + 8.f, 28.f);
        }
        /* M, S */
        {
            float by = yc - 11.f;
            int m = (int)(g->mute >> k & 1u), s = (int)(g->solo >> k & 1u);
            if (m) RR(g, c, xms, by, 22.f, 22.f, 3.f, C_INK); else RRS(g, c, xms, by, 22.f, 22.f, 3.f, u ? C_LINE : 0x80262424u);
            TX(g, c, xms + (22.f - TW(g, 11.f, 700.f, 0.f, 0, "M")) * 0.5f, yc + CAP(g, 11.f) * 0.5f, 11.f, 700.f, 0.f, 0,
               m ? C_PANEL : u ? C_DIM : C_FAINT, "M");
            if (s) RR(g, c, xms + 26.f, by, 22.f, 22.f, 3.f, C_RED); else RRS(g, c, xms + 26.f, by, 22.f, 22.f, 3.f, u ? C_LINE : 0x80262424u);
            TX(g, c, xms + 26.f + (22.f - TW(g, 11.f, 700.f, 0.f, 0, "S")) * 0.5f, yc + CAP(g, 11.f) * 0.5f, 11.f, 700.f, 0.f, 0,
               s ? 0xFFFFFFFFu : u ? C_DIM : C_FAINT, "S");
            if (u) { wid_add(g, TXG_W_MUTE, k, xms, by, 22.f, 22.f); wid_add(g, TXG_W_SOLO, k, xms + 26.f, by, 22.f, 22.f); }
        }
    }
    if (g->nlane < (int)TXP_LANES) {   /* ADD LANE */
        float y = ROW_Y0 + rh * (float)g->nlane, yc = y + rh * 0.5f;
        int hv = g->hover_kind == TXG_W_ADD;
        LN(g, c, x0 + RX_NUM + 1.f, yc, x0 + RX_NUM + 9.f, yc, 1.3f, hv ? C_INK : C_DIM);
        LN(g, c, x0 + RX_NUM + 5.f, yc - 4.f, x0 + RX_NUM + 5.f, yc + 4.f, 1.3f, hv ? C_INK : C_DIM);
        LBL(g, c, x0 + RX_NAME, yc + CAP(g, 10.5f) * 0.5f, 10.5f, hv ? C_INK : C_DIM, "ADD LANE");
        wid_add(g, TXG_W_ADD, -1, x0, y, RX_TL - 8.f, rh);
    }
}

/* ---- the overlays: a header (title, the search, CLOSE), then their bodies */
static void ov_header(txg_t *g, frui_canvas *c, const char *title, const char *red, int search, const char *ph)
{
    float y = TOP_H, x;
    R(g, c, 0, y, TXG_W, TXG_H - y, C_OVL);
    R(g, c, 0, y + OV_HDR - 1.f, TXG_W, 1.f, C_LINE);
    x = 18.f + TX(g, c, 18.f, y + 23.f + CAP(g, 17.f) * 0.5f, 17.f, 700.f, 2.4f, 0, C_INK, title);
    if (red) TX(g, c, x + 8.f, y + 23.f + CAP(g, 17.f) * 0.5f, 17.f, 700.f, 2.4f, 0, C_RED, red);
    if (search) {                        /* the search: what is typed (the keys go to it while the browser is open) */
        float sw = 280.f, sx = TXG_W - 98.f - 14.f - sw;
        RR(g, c, sx, y + 10.f, sw, 26.f, 3.f, C_WELL); RRS(g, c, sx, y + 10.f, sw, 26.f, 3.f, g->q[0] ? C_DIM : C_LINE);
        if (g->q[0]) {
            float w = TX(g, c, sx + 10.f, y + 23.f + CAP(g, 12.f) * 0.5f, 12.f, 500.f, 0.3f, 0, C_INK, g->q);
            R(g, c, sx + 11.f + w, y + 16.f, 1.2f, 14.f, C_RED);
        } else TX(g, c, sx + 10.f, y + 23.f + CAP(g, 12.f) * 0.5f, 12.f, 450.f, 0.3f, 0, C_DIM, ph);
        wid_add(g, TXG_W_SEARCH, -1, sx, y + 10.f, sw, 26.f);
    }
    x = TXG_W - 98.f;
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
static int lane_of_snd(const txg_t *g, int f, int i, int *tags)   /* the lanes playing family f's sound i: a mask */
{
    int m = 0;
    (void)tags;
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
    float y, lx = OV_NAV, lw = TXG_W - OV_NAV - OV_PREV, rows = (TXG_H - OV_Y0 - 12.f) / OV_RH;
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
    R(g, c, OV_NAV - 1.f, OV_Y0, 1.f, TXG_H - OV_Y0, C_LINE);
    for (int f = 0; f < g->cat->nfam; f++) nav_item(g, c, OV_Y0 + 6.f + 28.f * (float)f, g->cat->fam[f].name, g->cat->fam[f].n, !g->q[0] && f == g->sb_fam, f);
    /* the sounds */
    sb_items(g);
    for (int j = 0; j < g->nitems; j++) if (g->items[j] == (g->sb_cf << 4 | g->sb_ci)) cur = j;
    list_view(g, cur, rows);
    if (g->sb_top > g->nitems - (int)rows) g->sb_top = g->nitems - (int)rows;
    if (g->sb_top < 0) g->sb_top = 0;
    clipL(g, c, lx, OV_Y0, lw, TXG_H - OV_Y0);
    if (!g->nitems) TX(g, c, lx + 16.f, OV_Y0 + 30.f, 12.5f, 450.f, 0.f, 0, C_DIM, "No sound has that name.");
    for (int j = g->sb_top; j < g->nitems && (float)(j - g->sb_top) < rows; j++) {
        int f = g->items[j] >> 4, i = g->items[j] & 15, m = lane_of_snd(g, f, i, 0), e = snd_eng(g, f, i);
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
        float px = TXG_W - OV_PREV + 16.f, pw = OV_PREV - 32.f;
        int e = snd_eng(g, g->sb_cf, g->sb_ci);
        R(g, c, TXG_W - OV_PREV, OV_Y0, 1.f, TXG_H - OV_Y0, C_LINE);
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
        y = TXG_H - 56.f;
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
    float y, lx = OV_NAV, lw = TXG_W - OV_NAV - OV_PREV, rows = (TXG_H - OV_Y0 - 12.f) / OV_RH;
    int cur = -1, nu = 0, loaded = g->sn.ui.scene != 0xFF ? (g->sn.ui.scene < ct->npr ? g->sn.ui.scene : g->sn.ui.scene) : TXP_LOAD_LS + g->sn.ui.land;
    for (int u = 0; u < (int)TXP_USER_N; u++) nu += (int)(g->sn.ui.user >> u & 1u);
    ov_header(g, c, "PRESETS", 0, 1, "Type to search presets");
    R(g, c, OV_NAV - 1.f, OV_Y0, 1.f, TXG_H - OV_Y0, C_LINE);
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
    clipL(g, c, lx, OV_Y0, lw, TXG_H - OV_Y0);
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
        float px = TXG_W - OV_PREV + 16.f, pw = OV_PREV - 32.f, mh = 120.f;
        int v = g->pb_cur, lanes = 0;
        R(g, c, TXG_W - OV_PREV, OV_Y0, 1.f, TXG_H - OV_Y0, C_LINE);
        y = OV_Y0 + 24.f;
        if (v >= 0) {
            int fi[4], si[4], cn[4], ac[4], pn[4], n = 0;
            pb_code(g, v, code, sizeof code, name, sizeof name, &lanes);
            snprintf(t, sizeof t, "%s  \xC2\xB7  %s", code, v >= TXP_LOAD_LS ? "A LANDSCAPE'S START" : v >= ct->npr ? "YOURS" : ct->cat[v < ct->npr ? 0 : 0]);
            if (v < ct->npr) for (int i = 0; i < ct->ncat; i++) if (v >= ct->cat_at[i] && v < ct->cat_at[i + 1]) snprintf(t, sizeof t, "%s  \xC2\xB7  %s", code, ct->cat[i]);
            LBL(g, c, px, y, 10.5f, C_DIM, t);
            clipL(g, c, px, y, pw, 50.f);
            TX(g, c, px, y + 36.f, 28.f, 700.f, 1.1f, 0, C_INK, name);
            frui_clip_reset(c);
            y += 52.f;
            for (int k = 0; k < 4; k++) {   /* its lanes */
                fi[k] = si[k] = -1; cn[k] = ac[k] = 0; pn[k] = 64;
                if (v < ct->npr && ct->prl[v][k].fi != 0xFF) { fi[k] = ct->prl[v][k].fi; si[k] = ct->prl[v][k].si; cn[k] = ct->prl[v][k].cnt; ac[k] = ct->prl[v][k].act; pn[k] = ct->prl[v][k].pan; }
                else if (v >= TXP_LOAD_LS && k < ct->fam[v - TXP_LOAD_LS].n) { fi[k] = v - TXP_LOAD_LS; si[k] = k; pn[k] = 64; }
                if (fi[k] >= 0) n++;
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
                        float a = 3.14159265f * (1.f - (float)i / 32.f), xx = cx + cosf(a) * rr * (float)r / 3.f, yy = cy - sinf(a) * rr * (float)r / 3.f * 0.9f;
                        LN(g, c, ppx, ppy, xx, yy, 0.75f, C_FAINT);
                        ppx = xx; ppy = yy;
                    }
                }
                CI(g, c, cx, cy, 2.5f, C_INK);
                for (int k = 0; k < 4; k++) {
                    float a, xx, yy;
                    if (fi[k] < 0) continue;
                    a = (float)(pn[k] - 64) / 64.f * MAP_ANG;
                    xx = cx + sinf(a) * rr * 0.62f; yy = cy - cosf(a) * rr * 0.62f * 0.9f;
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
                        if (v < ct->npr) {
                            char hm[16];
                            ptext(TXP_ID_LANE(0, TXP_PL_COUNT), cn[k], hm, sizeof hm);
                            char ha[16];
                            ptext(TXP_ID_LANE(0, TXP_PL_ACTIVE), cn[k] ? ac[k] + 1 : 0, ha, sizeof ha);
                            snprintf(t, sizeof t, "MANY %s \xC2\xB7 ACTIVE %s", hm, ha);
                            TX(g, c, px + pw - TW(g, 10.5f, 450.f, 0.f, FRUI_TNUM, t), yc + CAP(g, 10.5f) * 0.5f, 10.5f, 450.f, 0.f, FRUI_TNUM, C_DIM, t);
                        }
                    } else TX(g, c, px + 28.f, yc + CAP(g, 12.f) * 0.5f, 12.f, 450.f, 0.f, 0, C_FAINT, "empty");
                    y += 19.f;
                }
                (void)n;
                y += 8.f;
            }
        }
        LBL(g, c, px, y + 10.f, 10.5f, C_DIM, "EFFECTS ON LOAD");
        TX(g, c, px + pw - TW(g, 10.5f, 450.f, 0.f, 0, "yours stay once you turn one"), y + 10.f, 10.5f, 450.f, 0.f, 0, C_FAINT, "yours stay once you turn one");
        seg_param(g, c, px, y + 18.f, pw, TXP_ID_GLOB(TXP_PG_SCNFX), &y);
        LBL(g, c, px, y + 22.f, 10.5f, C_DIM, "PRESETS CHANGE TEMPO");
        seg_param(g, c, px, y + 30.f, pw, TXP_ID_GLOB(TXP_PG_SCNTEMPO), &y);
        y = TXG_H - 56.f;
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
static void group(txg_t *g, frui_canvas *c, float x, float y, const char *h, const char *why)
{
    TX(g, c, x, y + 20.f, 15.f, 700.f, 2.4f, 0, C_INK, h);
    TX(g, c, x, y + 40.f, 12.f, 450.f, 0.f, 0, C_DIM, why);
}
static void draw_settings(txg_t *g, frui_canvas *c)
{
    const float gw = TXG_W / 3.f, gh = (TXG_H - OV_Y0) / 2.f, pad = 18.f, iw = gw - 2.f * pad;
    char t[48];
    float x, y;
    ov_header(g, c, "SETTINGS", 0, 0, 0);
    TX(g, c, 18.f + TW(g, 17.f, 700.f, 2.4f, 0, "SETTINGS") + 24.f, TOP_H + 23.f + CAP(g, 11.f) * 0.5f, 11.f, 450.f, 0.3f, 0, C_DIM,
       "Saved with the project; MIDI, scale and loading are host parameters too");
    for (int i = 1; i < 3; i++) R(g, c, gw * (float)i, OV_Y0, 1.f, TXG_H - OV_Y0, C_LINE);
    R(g, c, 0, OV_Y0 + gh, TXG_W, 1.f, C_LINE);
    /* MIDI IN */
    x = pad; y = OV_Y0 + 8.f;
    group(g, c, x, y, "MIDI IN", "Each lane listens on its own channel: notes and CCs.");
    y += 54.f;
    for (int k = 0; k < (int)TXP_LANES; k++) {
        float cw = (iw - 18.f) / 4.f, cx = x + (cw + 6.f) * (float)(k % 4), cy = y + 36.f * (float)(k / 4);
        int v = txp_ui_get(g->c, TXP_ID_LCH(k)), on = k < g->nlane;
        RR(g, c, cx, cy, cw, 30.f, 2.f, C_WELL);
        snprintf(t, sizeof t, "LANE %d", k + 1);
        LBL(g, c, cx + 8.f, cy + 11.f, 9.5f, on ? C_DIM : C_FAINT, t);
        if (v > 0) snprintf(t, sizeof t, "CH %d", v); else snprintf(t, sizeof t, "OFF");
        TX(g, c, cx + 8.f, cy + 25.f, 12.f, 500.f, 0.f, FRUI_TNUM, on ? C_INK : C_DIM, t);
        wid_add2(g, TXG_W_FIELD, (int)(TXP_ID_LCH(k) & 0xFFFFu), TXP_ID_LCH(k), 0, cx, cy, cw, 30.f);
    }
    y += 36.f * 4.f + 4.f;
    y = field(g, c, x, y, iw, "Effects, tempo, groove and scale CCs", TXP_ID_GLOB(TXP_PG_CHG));
    y = field(g, c, x, y, iw, "Channel 1 notes play the selected lane", TXP_ID_GLOB(TXP_PG_SELCH));
    (void)field(g, c, x, y, iw, "MOD CC (a matrix source)", TXP_ID_MODCC);
    /* SCALE */
    x = gw + pad; y = OV_Y0 + 8.f;
    group(g, c, x, y, "SCALE", "How the keys play the sounds.");
    y += 58.f;
    LBL(g, c, x, y + 4.f, 10.5f, C_DIM, "MODE");
    seg_param(g, c, x, y + 12.f, iw, TXP_ID_GLOB(TXP_PG_SCMODE), &y);
    LBL(g, c, x, y + 18.f, 10.5f, C_DIM, "SCALE");
    seg_param(g, c, x, y + 26.f, iw, TXP_ID_GLOB(TXP_PG_SCALE), &y);
    y += 10.f;
    y = field(g, c, x, y, iw, "Root", TXP_ID_GLOB(TXP_PG_ROOT));
    (void)field(g, c, x, y, iw, "Transpose", TXP_ID_GLOB(TXP_PG_TRANS));
    /* TIMING */
    x = 2.f * gw + pad; y = OV_Y0 + 8.f;
    group(g, c, x, y, "TIMING", "The host's tempo leads when it gives one.");
    y += 58.f;
    snprintf(t, sizeof t, "%u.%u BPM", (unsigned)g->sn.tempo10 / 10u, (unsigned)g->sn.tempo10 % 10u);
    TX(g, c, x, y + 11.f + CAP(g, 12.5f) * 0.5f, 12.5f, 450.f, 0.f, 0, C_INK, "Tempo now");
    TX(g, c, x + iw - 8.f - TW(g, 12.f, 500.f, 0.f, FRUI_TNUM, t), y + 11.f + CAP(g, 12.f) * 0.5f, 12.f, 500.f, 0.f, FRUI_TNUM, C_DIM, t);
    y += 28.f;
    y = field(g, c, x, y, iw, "Groove", TXP_ID_GLOB(TXP_PG_GTYPE));
    y = field(g, c, x, y, iw, "Groove amount", TXP_ID_GLOB(TXP_PG_GAMOUNT));
    (void)field(g, c, x, y, iw, "Groove shift", TXP_ID_GLOB(TXP_PG_GSHIFT));
    /* LOADING */
    x = pad; y = OV_Y0 + gh + 8.f;
    group(g, c, x, y, "LOADING", "What a preset brings with it.");
    y += 58.f;
    LBL(g, c, x, y + 4.f, 10.5f, C_DIM, "EFFECTS ON LOAD");
    seg_param(g, c, x, y + 12.f, iw, TXP_ID_GLOB(TXP_PG_SCNFX), &y);
    LBL(g, c, x, y + 18.f, 10.5f, C_DIM, "PRESETS CHANGE TEMPO");
    seg_param(g, c, x, y + 26.f, iw, TXP_ID_GLOB(TXP_PG_SCNTEMPO), &y);
    y += 12.f;
    (void)field(g, c, x, y, iw, "Lanes in use", TXP_ID_LANES);
    /* ENGINE COLOURS: the engines the lanes play */
    x = gw + pad; y = OV_Y0 + gh + 8.f;
    group(g, c, x, y, "ENGINE COLOURS", "Bone at first. A colour follows its engine everywhere.");
    y += 56.f;
    {
        int seen[TXP_ENG_MAX] = {0}, n = 0;
        for (int k = 0; k < g->nlane && n < 12; k++) {
            int e = g->sn.ui.lane[k].eng;
            float cx, cy;
            char en[24];
            if (!used(g, k) || e >= (int)TXP_ENG_MAX || seen[e]) continue;
            seen[e] = 1;
            cx = x + (iw * 0.5f + 6.f) * (float)(n % 2); cy = y + 24.f * (float)(n / 2);
            cap_name(en, g->cat->eng[e], sizeof en);
            TX(g, c, cx, cy + 10.f + CAP(g, 12.5f) * 0.5f, 12.5f, 450.f, 0.f, 0, C_INK, en);
            CI(g, c, cx + iw * 0.5f - 22.f, cy + 10.f, 6.f, eng_col(g, e));
            RI(g, c, cx + iw * 0.5f - 22.f, cy + 10.f, 8.5f, 1.f, g->pick == e ? C_INK : C_FAINT);
            wid_add2(g, TXG_W_COLDOT, -1, (uint32_t)e, 0, cx, cy, iw * 0.5f - 6.f, 20.f);
            n++;
        }
        if (!n) TX(g, c, x, y + 14.f, 12.f, 450.f, 0.f, 0, C_DIM, "No lane plays a sound.");
        y = OV_Y0 + 2.f * gh - 44.f;
        button(g, c, x, y, 170.f, 26.f, "RESET ALL TO BONE", 0);
        wid_add(g, TXG_W_COLRESET, -1, x, y, 170.f, 26.f);
    }
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
        snprintf(t, sizeof t, "HQ build  \xC2\xB7  %d lanes", (int)TXP_LANES);
        TX(g, c, x, y + 11.f, 12.f, 450.f, 0.f, 0, C_DIM, t);
    }
}
/* the colour picker: the engine's swatches in a small panel near where it was asked (a click outside closes it) */
static void draw_picker(txg_t *g, frui_canvas *c)
{
    const float w = 21.f * TXP_COL_N + 22.f, h = 62.f;
    float x = (float)g->pick_x, y = (float)g->pick_y;
    char t[48], en[24];
    if (g->pick < 0 || g->pick >= g->cat->neng) return;
    x = x + w > TXG_W - 8.f ? TXG_W - 8.f - w : x < 8.f ? 8.f : x;
    y = y + h > TXG_H - 8.f ? y - h - 30.f : y;
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
    else { draw_rows(g, c); draw_map(g, c); }
    draw_picker(g, c);
}

/* ---- reading what plays */
int txg_tick(txg_t *g)
{
    uint32_t h = 2166136261u, actn0 = g->actn;
    int k;
    txp_ui_lane_t keep[TXP_LANES];
    txp_ui_snap(g->c, &g->sn);
    g->nlane = g->sn.ui.nlane < 1 ? 1 : g->sn.ui.nlane > TXP_LANES ? (int)TXP_LANES : g->sn.ui.nlane;
    g->sel = txp_ui_sel(g->c);
    if (g->sel >= g->nlane) g->sel = g->nlane - 1;
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
    /* the signature of what is drawn but the activity: the values the modulation moves now are drawn with the activity
     * (the map's partial repaint), not here */
    memcpy(keep, g->sn.ui.lane, sizeof keep);
    for (k = 0; k < (int)TXP_LANES; k++) { g->sn.ui.lane[k].lpan = g->sn.ui.lane[k].ldist = 0; }
    {
        int16_t lv[TXP_LFO_N];
        memcpy(lv, g->sn.ui.lfo, sizeof lv);
        memset(g->sn.ui.lfo, 0, sizeof g->sn.ui.lfo);
        h = fnv(h, &g->sn.ui, sizeof g->sn.ui);
        memcpy(g->sn.ui.lfo, lv, sizeof lv);
    }
    memcpy(g->sn.ui.lane, keep, sizeof keep);
    h = fnv(h, &g->nlane, sizeof g->nlane); h = fnv(h, &g->sel, sizeof g->sel); h = fnv(h, &g->ov, sizeof g->ov);
    h = fnv(h, &g->view, sizeof g->view); h = fnv(h, &g->pick, sizeof g->pick); h = fnv(h, g->col, sizeof g->col);
    h = fnv(h, &g->mute, sizeof g->mute); h = fnv(h, &g->solo, sizeof g->solo);
    h = fnv(h, g->level, sizeof g->level); h = fnv(h, g->pan, sizeof g->pan); h = fnv(h, g->dist, sizeof g->dist);
    h = fnv(h, g->cnt, sizeof g->cnt); h = fnv(h, g->act, sizeof g->act);
    h = fnv(h, &g->hover_kind, sizeof g->hover_kind); h = fnv(h, &g->hover_lane, sizeof g->hover_lane);
    h = fnv(h, &g->drag, sizeof g->drag);
    if (g->ov) {                         /* (the overlays' own state and the settings they show) */
        int v[6] = { g->sb_fam, g->sb_cf << 8 | g->sb_ci, g->sb_top, g->pb_bank, g->pb_cur, g->pb_top };
        h = fnv(h, v, sizeof v);
        h = fnv(h, g->q, sizeof g->q);
        h = fnv(h, &g->sn.tempo10, sizeof g->sn.tempo10);
        if (g->ov == OV_SET || g->ov == OV_PRE) {
            static const int GL[] = { TXP_PG_SCNTEMPO, TXP_PG_SCNFX, TXP_PG_GAMOUNT, TXP_PG_GTYPE, TXP_PG_GSHIFT, TXP_PG_SCALE,
                                      TXP_PG_ROOT, TXP_PG_SELCH, TXP_PG_SCMODE, TXP_PG_TRANS, TXP_PG_CHG };
            int gv[sizeof GL / sizeof GL[0] + TXP_LANES + 2];
            size_t i;
            for (i = 0; i < sizeof GL / sizeof GL[0]; i++) gv[i] = txp_ui_get(g->c, TXP_ID_GLOB(GL[i]));
            for (k = 0; k < (int)TXP_LANES; k++) gv[i + (size_t)k] = txp_ui_get(g->c, TXP_ID_LCH(k));
            gv[i + TXP_LANES] = txp_ui_get(g->c, TXP_ID_MODCC);
            gv[i + TXP_LANES + 1] = txp_ui_win_w(g->c);
            h = fnv(h, gv, sizeof gv);
        }
    }
    g->sig = h;
    return !g->drawn || g->sig != g->sig_drawn || g->actn != g->actn_drawn;
}

/* ---- painting: a whole repaint, or only the map and the event lines (frui's base clip) */
static int paint_into(txg_t *g, frui_canvas *c)
{
    int full = !g->drawn || c->px != g->last_px || c->w != g->last_w || c->h != g->last_h || g->sig != g->sig_drawn;
    g->S = (float)c->w / (float)TXG_W;
    if (full) {
        frui_clip_base(c, 0, 0, c->w, c->h);
        draw_all(g, c);
    } else if (g->actn != g->actn_drawn && !g->ov && g->pick < 0) {
        float tx0 = MAP_W + RX_DOT - 8.f, tx1 = MAP_W + RX_TL + tl_w() + 1.f;
        frui_clip_base(c, 0, (int)floorf(TOP_H * g->S), (int)ceilf(MAP_W * g->S), c->h);
        draw_all(g, c);
        frui_clip_base(c, (int)floorf(tx0 * g->S), (int)floorf(ROW_Y0 * g->S), (int)ceilf(tx1 * g->S), c->h);
        draw_all(g, c);
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
    if (pw * TXG_H > ph * TXG_W) pw = ph * TXG_W / TXG_H; else ph = pw * TXG_H / TXG_W;
    px = calloc((size_t)pw * (size_t)ph, 4);
    if (!px) return -1;
    free(g->px);
    g->px = px; g->pw = pw; g->ph = ph;
    g->drawn = 0;
    return 0;
}

/* ---- input */
static const wid_t *hit(const txg_t *g, float x, float y)   /* the last-drawn control under (x, y): the picker first,
                                                             * the map's dots nearest first, then the topmost */
{
    const wid_t *best = 0;
    float bd = 1e9f;
    for (int i = g->nw - 1; i >= 0 && g->pick >= 0; i--) {
        const wid_t *w = &g->w[i];
        if (w->kind != TXG_W_PICKER && w->kind != TXG_W_SWATCH) continue;
        if (x >= w->r[0] && x < w->r[0] + w->r[2] && y >= w->r[1] && y < w->r[1] + w->r[3]) {
            if (w->kind == TXG_W_SWATCH) return w;
            best = w;
        }
    }
    if (best) return best;
    for (int i = 0; i < g->nw; i++) {
        const wid_t *w = &g->w[i];
        if (w->kind != TXG_W_DOT) continue;
        float dx = x - (w->r[0] + w->r[2] * 0.5f), dy = y - (w->r[1] + w->r[3] * 0.5f), d = dx * dx + dy * dy;
        if (d < 14.f * 14.f && d < bd) { bd = d; best = w; }
    }
    if (best) return best;
    for (int i = g->nw - 1; i >= 0; i--) {
        const wid_t *w = &g->w[i];
        if (w->kind == TXG_W_DOT) continue;
        if (x >= w->r[0] && x < w->r[0] + w->r[2] && y >= w->r[1] && y < w->r[1] + w->r[3]) {
            if (w->kind == TXG_W_ROW) { best = w; continue; }   /* (a row: only when nothing in it is hit) */
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
static void vol_at(txg_t *g, int k, float x)
{
    const float xvol = MAP_W + RX_TL + tl_w() + 14.f + RW_CH + 12.f;
    int v = (int)lroundf((x - xvol) / RW_VOL * 127.f);
    txp_ui_set(g->c, TXP_ID_LANE(k, TXP_PL_LEVEL), v < 0 ? 0 : v > 127 ? 127 : v);
}
static void dot_at(txg_t *g, int k, float x, float y)
{
    int pan, dist;
    if (g->view) sp_inv(x - g->drag_dx, y - g->drag_dy, g->drag_dist < 0, &pan, &dist);
    else map_inv(x, y, &pan, &dist);
    txp_ui_set(g->c, TXP_ID_LANE(k, TXP_PL_PAN), pan);
    if (g->drag_dist >= 0) txp_ui_set(g->c, TXP_ID_LANE(k, TXP_PL_CARD0 + g->drag_dist), dist);
    g->pan[k] = pan;
    if (g->drag_dist >= 0) g->dist[k] = dist;
}
static void drag_end(txg_t *g)
{
    int k = g->drag_lane;
    if (g->drag == TXG_W_VOL) txp_ui_end(g->c, TXP_ID_LANE(k, TXP_PL_LEVEL));
    else if (g->drag == TXG_W_DOT) {
        txp_ui_end(g->c, TXP_ID_LANE(k, TXP_PL_PAN));
        if (g->drag_dist >= 0) txp_ui_end(g->c, TXP_ID_LANE(k, TXP_PL_CARD0 + g->drag_dist));
    }
    g->drag = 0;
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
static int ov_key(txg_t *g, const frg_input *e)   /* the keys while an overlay is open */
{
    size_t n = strlen(g->q);
    if (e->key == FRK_ESCAPE) {
        if (g->pick >= 0) { g->pick = -1; return 3; }
        if (g->q[0]) { g->q[0] = 0; return 3; }
        if (g->ov == OV_SND) sb_close(g, 0); else g->ov = OV_NONE;
        return 3;
    }
    if (g->ov == OV_SET) return 0;
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
            if (e->kind == FRG_DOUBLE) {    /* (the default: 100, the sound as tuned) */
                uint32_t id = TXP_ID_LANE(k, TXP_PL_LEVEL);
                txp_ui_begin(g->c, id); txp_ui_set(g->c, id, txp_ui_default(g->c, id) > 0 ? txp_ui_default(g->c, id) : 100); txp_ui_end(g->c, id);
                break;
            }
            g->drag = TXG_W_VOL; g->drag_lane = k;
            txp_ui_begin(g->c, TXP_ID_LANE(k, TXP_PL_LEVEL));
            vol_at(g, k, e->x);
            break;
        case TXG_W_DOT:
            select_lane(g, k);
            g->drag = TXG_W_DOT; g->drag_lane = k;
            g->drag_dist = g->sn.ui.lane[k].dist != 0xFF ? g->sn.ui.lane[k].dist : -1;
            {   /* (SPACE: the dot keeps its place under the pointer: the offset kept) */
                float x, y;
                sp_pos(g->pan[k], g->dist[k], &x, &y, 0);
                g->drag_dx = e->x - x; g->drag_dy = g->drag_dist >= 0 ? e->y - y : 0.f;
                if (g->drag_dist < 0) g->drag_dy = e->y - y;
            }
            txp_ui_begin(g->c, TXP_ID_LANE(k, TXP_PL_PAN));
            if (g->drag_dist >= 0) txp_ui_begin(g->c, TXP_ID_LANE(k, TXP_PL_CARD0 + g->drag_dist));
            break;
        }
        return 3;
    case FRG_MOVE:
        if (g->drag == TXG_W_VOL) { vol_at(g, g->drag_lane, e->x); return 3; }
        if (g->drag == TXG_W_DOT) { dot_at(g, g->drag_lane, e->x, e->y); return 3; }
        w = hit(g, e->x, e->y);
        {
            int hk = 0, hl = -1;
            if (w && (w->kind == TXG_W_ADD || w->kind == TXG_W_DOT || w->kind == TXG_W_LIST)) { hk = w->kind; hl = w->lane; }
            if (hk != g->hover_kind || hl != g->hover_lane) { g->hover_kind = hk; g->hover_lane = hl; return 1; }
        }
        return 0;
    case FRG_UP:
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
        if (w && (w->kind == TXG_W_FIELD || w->kind == TXG_W_SEG)) { step_param(g, w->id, e->wheel > 0.f ? 1 : -1, 0); return 3; }
        if (w && w->kind == TXG_W_VOL) {
            uint32_t id = TXP_ID_LANE(w->lane, TXP_PL_LEVEL);
            int d = (int)lroundf(e->wheel * 2.f);
            if (!d) d = e->wheel > 0.f ? 1 : -1;
            step_param(g, id, d, 0);
            return 3;
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
    (void)scale;                        /* (the canvas's width is the scale: c->w / TXG_W) */
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
void txg_fit_size(uint32_t *w, uint32_t *h)
{
    uint32_t x = *w;
    if (*h * TXG_W < x * TXG_H) x = *h * TXG_W / TXG_H;   /* (the largest that fits both) */
    if (x < TXG_MIN_W) x = TXG_MIN_W;
    if (x > TXG_MAX_W) x = TXG_MAX_W;
    *w = x;
    *h = (x * TXG_H + TXG_W / 2u) / TXG_W;
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
    g->view = txp_ui_view(c);
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
