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

/* the look: dark, nearly monochrome; red for what is selected or on */
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
#define MAX_W 160

typedef struct { int kind, lane; float r[4]; } wid_t;

struct txg {
    txp_core *c;
    frui_font *font;
    uint32_t *px;                        /* the headless canvas (txg_resize) */
    int pw, ph;
    const uint32_t *last_px;             /* the buffer painted last: another one, a whole repaint */
    int last_w, last_h;
    float S;                             /* canvas pixels a logical pixel */
    /* what is drawn */
    txp_snap_t sn;
    int nlane, sel, overlay;
    uint32_t mute, solo;
    int level[TXP_LANES], pan[TXP_LANES], dist[TXP_LANES], cnt[TXP_LANES], act[TXP_LANES];
    uint8_t ev[NBLK][TXP_LANES], pk[NBLK][TXP_LANES];
    uint32_t actn, actn_drawn, sig, sig_drawn;
    int drawn;                           /* the canvas holds a whole draw */
    /* input */
    int drag, drag_lane, drag_dist;      /* drag: 0 none, TXG_W_DOT, TXG_W_VOL */
    float drag_x0;
    int hover_kind, hover_lane;
    wid_t w[MAX_W];
    int nw;
    frg_client cl;
};

/* ---- drawing helpers (logical coordinates) */
static void wid_add(txg_t *g, int kind, int lane, float x, float y, float w, float h)
{
    if (g->nw >= MAX_W) return;
    g->w[g->nw].kind = kind; g->w[g->nw].lane = lane;
    g->w[g->nw].r[0] = x; g->w[g->nw].r[1] = y; g->w[g->nw].r[2] = w; g->w[g->nw].r[3] = h;
    g->nw++;
}
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
static void clipL(txg_t *g, frui_canvas *c, float x, float y, float w, float h) { frui_clip(c, x * g->S, y * g->S, w * g->S, h * g->S); }

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
    if (i < 0 || txp_param_text((uint32_t)i, v, out, n)) snprintf(out, n, "%d", v);
}

/* ---- the parts */
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
    /* the previous / next scene, the scene */
    RR(g, c, bx0 - 34.f, y, 28.f, h, 3.f, C_BTN); RRS(g, c, bx0 - 34.f, y, 28.f, h, 3.f, C_LINE);
    LN(g, c, bx0 - 17.f, yc - 5.f, bx0 - 22.f, yc, 1.4f, C_INK); LN(g, c, bx0 - 22.f, yc, bx0 - 17.f, yc + 5.f, 1.4f, C_INK);
    wid_add(g, TXG_W_PREV, -1, bx0 - 34.f, y, 28.f, h);
    RR(g, c, bx1 + 6.f, y, 28.f, h, 3.f, C_BTN); RRS(g, c, bx1 + 6.f, y, 28.f, h, 3.f, C_LINE);
    LN(g, c, bx1 + 17.f, yc - 5.f, bx1 + 22.f, yc, 1.4f, C_INK); LN(g, c, bx1 + 22.f, yc, bx1 + 17.f, yc + 5.f, 1.4f, C_INK);
    wid_add(g, TXG_W_NEXT, -1, bx1 + 6.f, y, 28.f, h);
    RR(g, c, bx0, y, bx1 - bx0, h, 3.f, C_WELL); RRS(g, c, bx0, y, bx1 - bx0, h, 3.f, C_LINE);
    if (sp && sp - pr < 8) { snprintf(tag, sizeof tag, "%.*s", (int)(sp - pr), pr); snprintf(name, sizeof name, "%s", sp + 1); }
    else snprintf(name, sizeof name, "%s", pr[0] ? pr : "NO SCENE");
    x = bx0 + 12.f;
    if (tag[0]) x += TX(g, c, x, yc + CAP(g, 13.f) * 0.5f, 13.f, 500.f, 0.4f, FRUI_TNUM, C_RED, tag) + 12.f;
    x += TX(g, c, x, yc + CAP(g, 13.f) * 0.5f, 13.f, 500.f, 0.6f, 0, C_INK, name);
    if (g->sn.ui.edited) TX(g, c, x + 6.f, yc + CAP(g, 13.f) * 0.5f, 13.f, 500.f, 0.f, 0, C_DIM, "*");
    snprintf(t, sizeof t, "%d LANE%s", g->nlane, g->nlane == 1 ? "" : "S");
    LBL(g, c, bx1 - 12.f - TW(g, 10.5f, 620.f, 1.47f, 0, t), yc + CAP(g, 10.5f) * 0.5f, 10.5f, C_DIM, t);
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
    /* SETTINGS (the next step's page: a stub) */
    x = TXG_W - 140.f;
    RR(g, c, x, y, 124.f, h, 3.f, g->overlay ? C_SEG : C_BTN); RRS(g, c, x, y, 124.f, h, 3.f, C_LINE);
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
static void draw_map(txg_t *g, frui_canvas *c)
{
    static const float RINGS[4] = {0.25f, 0.5f, 0.75f, 1.f};
    float lw = g->S < 1.5f ? 1.f / g->S : 0.75f, pts[TXP_LANES][2], boxes[TXP_LANES * 2][4];
    int on[TXP_LANES], nb = 0, k, order[TXP_LANES], no = 0;
    char t[64], nm[32];
    R(g, c, 0, TOP_H, MAP_W, TXG_H - TOP_H, C_WELL);
    R(g, c, MAP_W - 1.f, TOP_H, 1.f, TXG_H - TOP_H, C_LINE);
    /* MAP | SPACE (SPACE: the next step's) */
    {
        float x = 16.f, y = TOP_H + 14.f, wm = 58.f, ws = 66.f, h = 24.f;
        RR(g, c, x, y, wm + ws, h, 3.f, C_PANEL);
        R(g, c, x, y, wm, h, C_SEG);
        R(g, c, x, y + h - 2.f, wm, 2.f, C_RED);
        RRS(g, c, x, y, wm + ws, h, 3.f, C_LINE);
        R(g, c, x + wm, y, 1.f, h, C_LINE);
        LBL(g, c, x + (wm - TW(g, 11.f, 620.f, 1.54f, 0, "MAP")) * 0.5f, y + h * 0.5f + CAP(g, 11.f) * 0.5f, 11.f, C_INK, "MAP");
        LBL(g, c, x + wm + (ws - TW(g, 11.f, 620.f, 1.54f, 0, "SPACE")) * 0.5f, y + h * 0.5f + CAP(g, 11.f) * 0.5f, 11.f,
            C_FAINT, "SPACE");
    }
    /* the selected lane */
    k = g->sel;
    {
        float x = 20.f, y = TOP_H + 78.f;
        const txp_ui_lane_t *l = &g->sn.ui.lane[k];
        snprintf(t, sizeof t, "LANE %d%s%s", k + 1, used(g, k) ? "  \xC2\xB7  " : "", used(g, k) ? l->fam : "");
        for (char *q = t; *q; q++) if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
        LBL(g, c, x, y, 11.f, C_RED, t);
        if (used(g, k)) {
            char a[16], b[16];
            snprintf(nm, sizeof nm, "%s", l->snd);
            for (char *q = nm; *q; q++) if (*q >= 'a' && *q <= 'z') *q = (char)(*q - 32);
            clipL(g, c, 0, TOP_H, MAP_W - 12.f, 120.f);
            TX(g, c, x, y + 36.f, 30.f, 700.f, 1.2f, 0, (g->mute >> k & 1u) ? C_DIM : C_INK, nm);
            frui_clip_reset(c);
            ptext(TXP_ID_LANE(k, TXP_PL_PAN), g->pan[k], a, sizeof a);
            if (g->dist[k] >= 0) snprintf(b, sizeof b, "%d", g->dist[k]); else snprintf(b, sizeof b, "\xE2\x80\x94");
            snprintf(t, sizeof t, "PAN %s   \xC2\xB7   DIST %s", a, b);
            TX(g, c, x, y + 62.f, 12.f, 450.f, 0.3f, FRUI_TNUM, C_DIM, t);
            ptext(TXP_ID_LANE(k, TXP_PL_COUNT), g->cnt[k], a, sizeof a);
            snprintf(t, sizeof t, "HOW MANY %s   \xC2\xB7   HOW ACTIVE %d", a, g->act[k]);
            TX(g, c, x, y + 82.f, 12.f, 450.f, 0.3f, FRUI_TNUM, C_DIM, t);
            if (l->ch) snprintf(t, sizeof t, "MIDI CH %d%s", l->ch, (g->mute >> k & 1u) ? "   \xC2\xB7   MUTED" : "");
            else snprintf(t, sizeof t, "MIDI OFF%s", (g->mute >> k & 1u) ? "   \xC2\xB7   MUTED" : "");
            TX(g, c, x, y + 102.f, 12.f, 450.f, 0.3f, FRUI_TNUM, C_DIM, t);
        } else TX(g, c, x, y + 36.f, 30.f, 700.f, 1.2f, 0, C_FAINT, "EMPTY");
    }
    /* the field: rings (distance), rays (angle), you */
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
        boxes[nb][0] = pts[k][0] - 7.f; boxes[nb][1] = pts[k][1] - 7.f; boxes[nb][2] = pts[k][0] + 7.f; boxes[nb][3] = pts[k][1] + 7.f;
        nb++;
    }
    for (k = 0; k < g->nlane; k++) {
        float a, mf;
        if (!on[k]) continue;
        a = glow(g, k); mf = faded(g, k) ? 0.25f : 1.f;
        CI(g, c, pts[k][0], pts[k][1], 6.f + 6.f * a, frui_alpha(C_INK, (0.08f + 0.22f * a) * mf));
        if (g->dist[k] >= 0) CI(g, c, pts[k][0], pts[k][1], 4.f, frui_alpha(C_INK, mf < 1.f ? 0.35f : 1.f));
        else RI(g, c, pts[k][0], pts[k][1], 3.4f, 1.4f, frui_alpha(C_INK, mf < 1.f ? 0.35f : 1.f));
        if (k == g->sel) RI(g, c, pts[k][0], pts[k][1], 9.f, 1.4f, C_RED);
        wid_add(g, TXG_W_DOT, k, pts[k][0] - 11.f, pts[k][1] - 11.f, 22.f, 22.f);
    }
    /* labels: right, left, above, below the dot ...: the first place clear of the dots, the other labels and the edges
     * (the selected lane's first) */
    if (g->sel < g->nlane && on[g->sel]) order[no++] = g->sel;
    for (k = 0; k < g->nlane; k++) if (on[k] && k != g->sel) order[no++] = k;
    for (int o = 0; o < no; o++) {
        float x, y, w, tries[8][2], *at = 0;
        k = order[o]; x = pts[k][0]; y = pts[k][1];
        cap_name(nm, g->sn.ui.lane[k].snd, sizeof nm);
        snprintf(t, sizeof t, "%d %s", k + 1, nm);
        w = TW(g, 11.f, 600.f, 0.f, 0, t);
        tries[0][0] = x + 11.f; tries[0][1] = y - 6.f; tries[1][0] = x - 11.f - w; tries[1][1] = y - 6.f;
        tries[2][0] = x - w * 0.5f; tries[2][1] = y - 22.f; tries[3][0] = x - w * 0.5f; tries[3][1] = y + 10.f;
        tries[4][0] = x + 11.f; tries[4][1] = y - 18.f; tries[5][0] = x - 11.f - w; tries[5][1] = y + 6.f;
        tries[6][0] = x + 11.f; tries[6][1] = y + 6.f; tries[7][0] = x - 11.f - w; tries[7][1] = y - 18.f;
        for (int i = 0; i < 8 && !at; i++) {
            float b[4] = { tries[i][0] - 2.f, tries[i][1] - 1.f, tries[i][0] + w + 2.f, tries[i][1] + 13.f };
            int hit = b[0] < 2.f || b[2] > MAP_W - 2.f || b[1] < MAP_CY - MAP_RY - 24.f || b[3] > MAP_CY - 2.f;
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

static void draw_line(txg_t *g, frui_canvas *c, int k, float x, float y, float w, float h, float mf)
{
    int X0 = (int)lroundf(x * g->S), X1 = (int)lroundf((x + w) * g->S), Y1 = (int)lroundf((y + h) * g->S);
    float H = h * g->S;
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
        frui_rect(c, (float)i, (float)Y1 - 2.f - hh, 1.f, hh, frui_alpha(C_INK, (0.22f + 0.62f * (1.f - age)) * mf));
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
            float a = glow(g, k);
            CI(g, c, x0 + RX_DOT, yc, 3.5f, frui_alpha(C_INK, (0.18f + 0.82f * a) * mf));
            cap_name(nm, l->snd, sizeof nm);
            clipL(g, c, x0 + RX_NAME, y, RX_TL - RX_NAME - 8.f, rh);
            TX(g, c, x0 + RX_NAME, yc - 1.f, 12.5f, 600.f, 0.f, 0, ink, nm);
            TX(g, c, x0 + RX_NAME, yc + 12.f, 10.f, 450.f, 0.3f, 0, C_DIM, l->fam);
            frui_clip_reset(c);
        } else {
            TX(g, c, x0 + RX_NAME, yc + CAP(g, 12.f) * 0.5f, 12.f, 450.f, 0.3f, 0, C_FAINT, "empty");
        }
        draw_line(g, c, k, xtl, yc - th * 0.5f, tw, th, mf);
        /* the channel */
        if (l->ch) snprintf(t, sizeof t, "%d", l->ch); else snprintf(t, sizeof t, "OFF");
        TX(g, c, xch, yc + CAP(g, 12.f) * 0.5f, 12.f, 500.f, 0.f, FRUI_TNUM, u ? C_INK : C_FAINT, t);
        /* VOL */
        if (u) {
            float v = (float)g->level[k] / 127.f, ty = yc + 3.f;
            snprintf(t, sizeof t, "%d", g->level[k]);
            TX(g, c, xvol + RW_VOL - TW(g, 11.f, 500.f, 0.f, FRUI_TNUM, t), yc - 5.f, 11.f, 500.f, 0.f, FRUI_TNUM, ink, t);
            RR(g, c, xvol, ty, RW_VOL, 4.f, 2.f, C_FAINT);
            RR(g, c, xvol, ty, RW_VOL * v, 4.f, 2.f, mf < 1.f ? C_DIM : C_INK);
            RR(g, c, xvol + RW_VOL * v - 4.f, ty - 4.f, 8.f, 12.f, 2.f, mf < 1.f ? C_DIM : C_INK);
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
static void draw_overlay(txg_t *g, frui_canvas *c)   /* SETTINGS: the next step's page; a stub */
{
    float y = TOP_H, x;
    R(g, c, 0, y, TXG_W, TXG_H - y, C_OVL);
    R(g, c, 0, y + 46.f, TXG_W, 1.f, C_LINE);
    TX(g, c, 18.f, y + 23.f + CAP(g, 17.f) * 0.5f, 17.f, 700.f, 2.4f, 0, C_INK, "SETTINGS");
    TX(g, c, 18.f, y + 86.f, 13.f, 450.f, 0.f, 0, C_DIM, "Not built yet: MIDI channels, scale, timing, loading, engine colours and the window's size come here.");
    x = TXG_W - 98.f;
    RR(g, c, x, y + 10.f, 80.f, 26.f, 3.f, C_BTN); RRS(g, c, x, y + 10.f, 80.f, 26.f, 3.f, C_LINE);
    LBL(g, c, x + (80.f - TW(g, 11.f, 620.f, 1.54f, 0, "CLOSE")) * 0.5f, y + 23.f + CAP(g, 11.f) * 0.5f, 11.f, C_INK, "CLOSE");
    wid_add(g, TXG_W_CLOSE, -1, x, y + 10.f, 80.f, 26.f);
}
static void draw_all(txg_t *g, frui_canvas *c)
{
    g->nw = 0;
    frui_clip_reset(c);
    frui_fill(c, C_PANEL);
    draw_top(g, c);
    if (g->overlay) { draw_overlay(g, c); return; }
    draw_rows(g, c);
    draw_map(g, c);
}

/* ---- reading what plays */
int txg_tick(txg_t *g)
{
    uint32_t h = 2166136261u;
    int k;
    txp_ui_snap(g->c, &g->sn);
    g->nlane = g->sn.ui.nlane < 1 ? 1 : g->sn.ui.nlane > TXP_LANES ? (int)TXP_LANES : g->sn.ui.nlane;
    g->sel = txp_ui_sel(g->c);
    if (g->sel >= g->nlane) g->sel = g->nlane - 1;
    txp_ui_mutes(g->c, &g->mute, &g->solo);
    for (k = 0; k < (int)TXP_LANES; k++) {
        const txp_ui_lane_t *l = &g->sn.ui.lane[k];
        if (k >= g->nlane || !l->used) { g->level[k] = g->pan[k] = g->cnt[k] = g->act[k] = 0; g->dist[k] = -1; continue; }
        g->level[k] = txp_ui_get(g->c, TXP_ID_LANE(k, TXP_PL_LEVEL));
        g->pan[k] = txp_ui_get(g->c, TXP_ID_LANE(k, TXP_PL_PAN));
        g->cnt[k] = txp_ui_get(g->c, TXP_ID_LANE(k, TXP_PL_COUNT));
        g->act[k] = txp_ui_get(g->c, TXP_ID_LANE(k, TXP_PL_ACTIVE));
        g->dist[k] = l->dist != 0xFF ? txp_ui_get(g->c, TXP_ID_LANE(k, TXP_PL_CARD0 + l->dist)) : -1;
    }
    g->actn = txp_ui_act(g->c, g->ev, g->pk, NBLK);
    h = fnv(h, &g->sn.ui, sizeof g->sn.ui);
    h = fnv(h, &g->nlane, sizeof g->nlane); h = fnv(h, &g->sel, sizeof g->sel); h = fnv(h, &g->overlay, sizeof g->overlay);
    h = fnv(h, &g->mute, sizeof g->mute); h = fnv(h, &g->solo, sizeof g->solo);
    h = fnv(h, g->level, sizeof g->level); h = fnv(h, g->pan, sizeof g->pan); h = fnv(h, g->dist, sizeof g->dist);
    h = fnv(h, g->cnt, sizeof g->cnt); h = fnv(h, g->act, sizeof g->act);
    h = fnv(h, &g->hover_kind, sizeof g->hover_kind); h = fnv(h, &g->hover_lane, sizeof g->hover_lane);
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
    } else if (g->actn != g->actn_drawn && !g->overlay) {
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
static const wid_t *hit(const txg_t *g, float x, float y)   /* the last-drawn control under (x, y): the map's dots
                                                             * nearest first, then the topmost (the latest drawn) */
{
    const wid_t *best = 0;
    float bd = 1e9f;
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
static void vol_at(txg_t *g, int k, float x)
{
    const float xvol = MAP_W + RX_TL + tl_w() + 14.f + RW_CH + 12.f;
    int v = (int)lroundf((x - xvol) / RW_VOL * 127.f);
    txp_ui_set(g->c, TXP_ID_LANE(k, TXP_PL_LEVEL), v < 0 ? 0 : v > 127 ? 127 : v);
}
static void dot_at(txg_t *g, int k, float x, float y)
{
    int pan, dist;
    map_inv(x, y, &pan, &dist);
    txp_ui_set(g->c, TXP_ID_LANE(k, TXP_PL_PAN), pan);
    if (g->drag_dist >= 0) txp_ui_set(g->c, TXP_ID_LANE(k, TXP_PL_CARD0 + g->drag_dist), dist);
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
int txg_input(txg_t *g, const frg_input *e)
{
    const wid_t *w;
    int k;
    switch (e->kind) {
    case FRG_DOWN: case FRG_DOUBLE:
        if (e->button != FRG_LEFT) return 0;
        if (g->drag) drag_end(g);
        if (!(w = hit(g, e->x, e->y))) return 2;
        k = w->lane;
        if (g->overlay) { if (w->kind == TXG_W_CLOSE || w->kind == TXG_W_SET) g->overlay = 0; return 3; }
        switch (w->kind) {
        case TXG_W_PREV: step_param(g, TXP_ID_PRESET, -1, 1); break;
        case TXG_W_NEXT: step_param(g, TXP_ID_PRESET, 1, 1); break;
        case TXG_W_LDN: step_param(g, TXP_ID_LANES, -1, 0); break;
        case TXG_W_LUP: case TXG_W_ADD: step_param(g, TXP_ID_LANES, 1, 0); break;
        case TXG_W_SET: g->overlay = 1; break;
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
            g->drag = TXG_W_DOT; g->drag_lane = k; g->drag_x0 = e->x;
            g->drag_dist = g->sn.ui.lane[k].dist != 0xFF ? g->sn.ui.lane[k].dist : -1;
            txp_ui_begin(g->c, TXP_ID_LANE(k, TXP_PL_PAN));
            if (g->drag_dist >= 0) txp_ui_begin(g->c, TXP_ID_LANE(k, TXP_PL_CARD0 + g->drag_dist));
            break;
        }
        return 3;
    case FRG_MOVE:
        if (g->drag == TXG_W_VOL) { vol_at(g, g->drag_lane, e->x); return 3; }
        if (g->drag == TXG_W_DOT) { dot_at(g, g->drag_lane, e->x, e->y); return 3; }
        w = hit(g, e->x, e->y);
        k = w && w->kind == TXG_W_ADD ? TXG_W_ADD : 0;   /* (only ADD LANE lights under the mouse) */
        if (k != g->hover_kind) { g->hover_kind = k; g->hover_lane = -1; return 1; }
        return 0;
    case FRG_UP:
        if (g->drag) { drag_end(g); return 3; }
        return 0;
    case FRG_LEAVE: case FRG_FOCUS_LOST:
        if (g->drag) drag_end(g);
        g->hover_kind = 0; g->hover_lane = -1;
        return 1;
    case FRG_WHEEL:
        if ((w = hit(g, e->x, e->y)) && w->kind == TXG_W_VOL) {
            uint32_t id = TXP_ID_LANE(w->lane, TXP_PL_LEVEL);
            int d = (int)lroundf(e->wheel * 2.f);
            if (!d) d = e->wheel > 0.f ? 1 : -1;
            step_param(g, id, d, 0);
            return 3;
        }
        return 0;
    case FRG_KEY:
        if (e->key == FRK_ESCAPE && g->overlay) { g->overlay = 0; return 3; }
        if (e->key == FRK_UP && g->sel > 0) { select_lane(g, g->sel - 1); return 3; }
        if (e->key == FRK_DOWN && g->sel + 1 < g->nlane) { select_lane(g, g->sel + 1); return 3; }
        if ((e->key == 'm' || e->key == 'M') && used(g, g->sel)) { txp_ui_mute(g->c, (uint32_t)g->sel, !(g->mute >> g->sel & 1u)); g->mute ^= 1u << g->sel; return 3; }
        if ((e->key == 's' || e->key == 'S') && used(g, g->sel)) { g->solo ^= 1u << g->sel; txp_ui_solo(g->c, g->solo); return 3; }
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
    g->font = frui_font_load(txg_font_text, (size_t)txg_font_text_len);
    if (!g->font) { free(g); return 0; }
    g->hover_lane = -1;
    g->drag_dist = -1;
    g->cl.ctx = g; g->cl.paint = cl_paint; g->cl.input = cl_input; g->cl.tick = cl_tick;
    (void)txg_tick(g);
    return g;
}
void txg_free(txg_t *g)
{
    if (!g) return;
    if (g->drag) drag_end(g);
    frui_font_free(g->font);
    free(g->px);
    free(g);
}
