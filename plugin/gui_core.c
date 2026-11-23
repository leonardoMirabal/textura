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
#define C_REDBG 0xFF2A1715u                /* a control a source moves, in its mode */
#define C_GREY 0xFF6D6863u                 /* a value in a source's mode (its fill) */
#define C_GREYH 0xFF8F8983u                /* (its handle) */
#define C_MENU 0xFF232120u
#define C_MSTRK 0xFF2C2A28u                /* the M/S tab's bars and correlation track */
/* the two zones under the rows: SOUND (the selected lane's drawer: warmer, lighter, a red rail) and EFFECTS (the strip:
 * the whole mix, darker and cooler, under a dark gap and its band) */
#define C_SND 0xFF1D1918u
#define C_SNDCARD 0xFF262120u
#define C_SNDLINE 0xFF3A302Du
#define C_SNDSEG 0xFF352D2Bu                /* (a choice's box, a hovered button there) */
#define C_SNDTRK 0xFF3A3331u                /* (a fader's track there) */
#define C_DESC 0xFFB8B0A8u                  /* (a card's description) */
#define C_FX 0xFF0D0D0Fu
#define C_FXSEC 0xFF141417u
#define C_FXOPEN 0xFF1C1C21u                /* (a section whose drawer is open) */
#define C_FXMOV 0xFF1A1312u                 /* (MOVEMENT's section) */
#define C_FXLINE 0xFF24242Au
#define C_FXHD 0xFF9A9AA6u
#define C_FXNAME 0xFFD6D2CFu
#define C_FXSEG 0xFF24242Bu
#define C_FXTRK 0xFF2A2A30u
#define C_FXARR 0xFF55555Fu
#define C_GAP 0xFF050505u
#define ZFIT(cond) do { if (!(cond)) g->zone_fit = 0; } while (0)   /* (a text of the zones past its room: txg_fits 5 says so) */

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
#define DR_H 150.f                       /* the drawer: the selected lane's cards */
#define FX_H 148.f                       /* the strip: the effects in signal order, the movement */
#define FX_GAP 4.f                       /* (its top: a dark gap, then its band, then the sections) */
#define FX_BAND 20.f
#define FX_SY (FX_GAP + FX_BAND + 2.f)     /* (a section's top under the strip's) */
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
#define LFO_H 48                         /* the sources' traces: the last 48 ticks */
#define SRC_N 12                         /* the sources a tile shows: LFO 1 .. 8, ENV 1 .. 2, FOLLOW 1 .. 2 */
#define MB_H 36.f                        /* a source's mode: its bar over the content */
#define LH_N 32                          /* the links' undo: the matrix as it was, this many times back */
#define MAX_W 900
#define LIST_MAX 128

enum { OV_NONE, OV_SET, OV_SND, OV_PRE, OV_FX, OV_SAVE, OV_RND };   /* (OV_RND: the top bar's dice's arrow, RANDOM's list) */
enum { PB_USER = 100, PB_LAND = 101 };   /* the presets browser's banks past the factory's categories */
/* the strip's sections, in signal order (the expanded view's tabs: these, LFO 1 .. 8, ENV and FOLLOW, the matrix) */
enum { SEC_COMP, SEC_DRIVE, SEC_FILTER, SEC_DELAY, SEC_REVERB, SEC_MS, SEC_MOVE, SEC_N };
enum { TAB_LFO0 = SEC_MOVE, TAB_ENV = TAB_LFO0 + 8, TAB_MAT, TAB_N };
static const char *const SEC_NAME[SEC_N] = { "COMP", "DRIVE", "FILTER", "DELAY", "REVERB", "M/S", "MOVEMENT" };
static const float SEC_WT[SEC_N] = { 1.f, 1.f, 1.6f, 1.f, 1.f, 1.f, 1.3f };
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
#define ID_CATK G_ID(28)
#define ID_CREL G_ID(29)
#define ID_MSMID G_ID(TXP_PG_MSMID)      /* the M/S (tx_core_ms.h): MID, SIDE 0 -inf, v (v - 115) / 2 dB; WIDTH 0 .. 200 % */
#define ID_MSSIDE G_ID(TXP_PG_MSSIDE)
#define ID_MSWIDTH G_ID(TXP_PG_MSWIDTH)
#define ID_MSMONO G_ID(TXP_PG_MSMONO)
#define MS_0DB 115                       /* MID / SIDE: 0 dB */
#define MS_W100 50                       /* WIDTH: 100 % */
#define GON_H 8                          /* the goniometer's persistence: the last blocks of points it shows */
#define ID_LFO(n, p) (5u << 8 | (uint32_t)(n) * 16u | (uint32_t)(p))
#define ID_MAT(s, q) (7u << 8 | (uint32_t)(s) * 4u | (uint32_t)(q))
enum { LP_SHAPE, LP_RATE, LP_SYNC, LP_MODE, LP_PHASE, LP_WIDTH, LP_JITTER, LP_FADE, LP_RESTART, LP_LEVEL };

typedef struct { int kind, lane; uint32_t id; int v; float r[4], t0, tw; } wid_t;   /* (t0, tw: its track, a source's mode's
                                          * drag: a fader's, else its own width) */

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
    float dr_h;                          /* the drawer's height: the lane's (DR_H), or an effect's drawer's (fxd_h) */
    int rtop, rvis;                      /* the rows: the first shown, how many fit */
    /* what is drawn */
    txp_snap_t sn;
    int nlane, sel, sel_seen, ov, view, fx_tab;
    uint32_t mute, solo;
    int level[TXP_LANES], pan[TXP_LANES], dist[TXP_LANES], cnt[TXP_LANES], act[TXP_LANES], col[TXP_LANES];
    uint8_t ev[NBLK][TXP_LANES], pk[NBLK][TXP_LANES];
    uint32_t actn, actn_drawn, sig, sig_drawn;
    int16_t lfoh[SRC_N][LFO_H];          /* the sources' values, a tick each (the newest last; SRCS' order) */
    int lfo_seen;
    /* COMP's meters as drawn (dB, txg_tick's ballistics), the input's peak held (and its ticks left); changed since
     * the last paint (a repaint of the activity's regions) */
    float cm_in, cm_out, cm_gr, cm_pk, cm_drawn[4];
    int cm_hold, cm_dirty;
    /* the M/S's meters as drawn (L R M S, dB, COMP's ballistics), the correlation (-1 .. 1, smoothed), as drawn last;
     * the goniometer's last blocks of points (the newest at gon_at), how many, the block count of the newest, its
     * scale (the picture follows the level: the cloud's edge near the circle) */
    float ms_db[4], ms_c, ms_drawn[5], gon_k;
    int16_t gon[GON_H][TXP_GON_N][2];
    int gon_at, gon_n;
    uint32_t gon_blk;
    int drawn;                           /* the canvas holds a whole draw */
    /* the colour picker (over anything): the engine it colours, -1 closed; where it opens */
    int pick, pick_x, pick_y;
    /* the engine browser: its lane, the family shown, the cursor (family, sound), the lane's kept sound, the search, an
     * audition waiting (ticks) */
    int sb_lane, sb_fam, sb_cf, sb_ci, sb_kf, sb_ki, sb_pend, sb_top;
    /* its preview: under way (the window asked for it), its HOW MANY (1 .. 6) and HOW ACTIVE (1 .. 9), set by the user
     * (then a lane that plays takes them too, and KEEP keeps them) */
    int pv_on, pv_cnt, pv_act, pv_user;
    /* the presets browser: the bank shown (a category, PB_USER, PB_LAND), the cursor (a scene, TXP_LOAD_LS + n) */
    int pb_bank, pb_cur, pb_top;
    char q[24];                          /* a browser's search (typed) */
    int items[LIST_MAX], nitems;         /* the list as drawn last */
    /* SAVE: the user scene chosen, its name, the save asked (ticks it waits to show it done); the name typed (0: drawn
     * or a used scene's, selected: the first key typed replaces it), the last one drawn (a free scene takes it back) */
    int sv_slot, sv_wait, sv_typed, sv_fit;   /* (sv_fit: every text of the panel fits its room, as drawn last) */
    char sv_name[12], sv_gen[12];
    float sv_x;                          /* (where SAVE is in the top bar: the panel opens under it) */
    /* input */
    int drag, drag_lane, drag_dist;      /* drag: 0 none, TXG_W_DOT, TXG_W_VOL, TXG_W_PARAM, TXG_W_MANY, ... */
    float drag_dx, drag_dy;              /* (SPACE: the pointer off the dot when it was taken) */
    uint32_t drag_id;                    /* TXG_W_PARAM: the parameter; its value at the press, the press, moved */
    int drag_v0, drag_begun, drag_moved, drag_fine;
    float drag_x0, drag_y0, drag_rx, drag_rw;
    int hover_kind, hover_lane;
    /* the movement: the source mode (its matrix SOURCE, 0 none), the content's top (under the mode's bar), a message
     * (ticks it shows), the control dragged in the mode (its id, its value 0 .. 1), the grip dragged (where, the control
     * under it and its rectangle), the right-click menu (its control, where), CLEAR ALL's question, the links' undo (the
     * matrix as it was, the matrix as the window last knew it: another change clears the undo) */
    int msrc, msg_t, menu_on, clr_ask, lh_n;
    float ct, mv_f, gx, gy, menu_x, menu_y, gover_r[4];
    uint32_t mv_id, gover, menu_id, lh_key;
    char msg[112];
    int16_t lh[LH_N][16][4], mx_known[16][4];
    int tag_below;                       /* (the strip's cells: their tags under them) */
    /* the texts as drawn last, each inside its room (txg_fits): the place's tabs and the M/S tab's, the place's head's
     * (the selected lane's name, and the widest a lane can show) */
    int ms_fit, head_fit;
    float snd_s;                         /* (the widest sound name: the scale it was found at, its family and index;
                                          * snd_widest) */
    int snd_wf, snd_wi;
    int zone;                            /* the zone drawn (pcell's look): 0 none, 1 SOUND (the drawer), 2 EFFECTS (the strip) */
    int names_fit;                       /* every sound's name and its family's tag fit the drawer (at names_s: the scale) */
    float names_s;
    /* HELP: the tour's step (-1: none), started by the first open (its card says so); the texts' fit as drawn last (the
     * top bar's, the card's, the menu's: txg_fits); the widest preset name's width (the top bar's room), for which scale
     * and user names; the tour's flag's folder ("": none) */
    int tour, tour_first, top_fit, tour_fit, zone_fit;   /* (zone_fit: the drawer's and the strip's texts, txg_fits 5) */
    float pname_w, pname_s, top_need, top_room;
    uint32_t pname_key;
    char udir[1024];
    wid_t w[MAX_W];
    int nw;
    frg_client cl;
};

static void pv_play(txg_t *g);
static void sb_close(txg_t *g, int keep);

/* ---- drawing helpers (logical coordinates) */
static void wid_add2(txg_t *g, int kind, int lane, uint32_t id, int v, float x, float y, float w, float h)
{
    if (g->nw >= MAX_W) return;
    g->w[g->nw].kind = kind; g->w[g->nw].lane = lane; g->w[g->nw].id = id; g->w[g->nw].v = v;
    g->w[g->nw].r[0] = x; g->w[g->nw].r[1] = y; g->w[g->nw].r[2] = w; g->w[g->nw].r[3] = h;
    g->w[g->nw].t0 = x; g->w[g->nw].tw = w;
    g->nw++;
}
static void wid_add(txg_t *g, int kind, int lane, float x, float y, float w, float h) { wid_add2(g, kind, lane, 0, 0, x, y, w, h); }
static void wid_track(txg_t *g, float t0, float tw) { if (g->nw > 0) { g->w[g->nw - 1].t0 = t0; g->w[g->nw - 1].tw = tw; } }   /* (the
                                          * control just added: its track) */
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
/* a name too long for the room beside its value: its short form (the strip's narrow cells: HP, LP ...), else as is */
static const char *lab_fit(txg_t *g, const char *lab, float room, float ns)
{
    static const char *const AB[][2] = { { "GROOVE PART", "GROOVE" }, { "HIGH PASS", "HP" }, { "LOW PASS", "LP" }, { "RESONANCE", "RESO" },
                                         { "FEEDBACK", "FDBK" }, { "PREDELAY", "PRE" }, { "INTERVALS", "IVL" } };
    if (LBLW(g, ns, lab) <= room) return lab;
    for (size_t i = 0; i < sizeof AB / sizeof AB[0]; i++)
        if (!strcmp(lab, AB[i][0])) return AB[i][1];
    return lab;
}
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
/* the engine browser's buttons: a swap icon (two opposite arrows, drawn), centred on (x, y), s wide */
static void swap_icon(txg_t *g, frui_canvas *c, float x, float y, float s, uint32_t col)
{
    float hw = s * 0.5f, a = 2.4f;
    LN(g, c, x - hw, y - 2.6f, x + hw, y - 2.6f, 1.2f, col);
    LN(g, c, x + hw, y - 2.6f, x + hw - a, y - 2.6f - a, 1.2f, col);
    LN(g, c, x + hw, y - 2.6f, x + hw - a, y - 2.6f + a, 1.2f, col);
    LN(g, c, x - hw, y + 2.6f, x + hw, y + 2.6f, 1.2f, col);
    LN(g, c, x - hw, y + 2.6f, x - hw + a, y + 2.6f - a, 1.2f, col);
    LN(g, c, x - hw, y + 2.6f, x - hw + a, y + 2.6f + a, 1.2f, col);
}
static void caret_dn(txg_t *g, frui_canvas *c, float x, float y, uint32_t col)   /* a small down triangle, centred */
{
    frui_path p;
    frui_path_init(&p);
    frui_path_move(&p, (x - 3.5f) * g->S, (y - 2.f) * g->S);
    frui_path_line(&p, (x + 3.5f) * g->S, (y - 2.f) * g->S);
    frui_path_line(&p, x * g->S, (y + 2.5f) * g->S);
    frui_path_fill(c, &p, col);
    frui_path_free(&p);
}
/* a button as the top bar's SAVE / UNDO: ink on a filled box with a line, lit when hovered. t: a label (the swap icon,
 * the label, a down caret) or none (the icon alone, centred); it opens lane k's engine browser (kind TXG_W_SND or
 * TXG_W_SWAP) */
static void chg_button(txg_t *g, frui_canvas *c, int kind, int k, float x, float y, float w, float h, const char *t)
{
    int hot = g->hover_kind == kind && g->hover_lane == k;
    float yc = y + h * 0.5f, fs = w < 130.f ? 9.5f : 10.5f;
    RR(g, c, x, y, w, h, 3.f, g->zone == 1 ? (hot ? C_SNDSEG : C_SNDCARD) : hot ? C_SEG : C_BTN);   /* (in SOUND: off its
                                          * warmer ground) */
    RRS(g, c, x, y, w, h, 3.f, hot ? C_DIM : g->zone == 1 ? C_SNDLINE : C_LINE);
    if (t) {
        float lw = LBLW(g, fs, t), tw = 10.f + 5.f + lw + 5.f + 7.f, x0 = x + (w - tw) * 0.5f;
        if (g->zone) ZFIT(tw + 4.f <= w);   /* (2 px clear at each end) */
        swap_icon(g, c, x0 + 5.f, yc, 10.f, C_INK);
        LBL(g, c, x0 + 15.f, yc + CAP(g, fs) * 0.5f, fs, C_INK, t);
        caret_dn(g, c, x0 + tw - 3.5f, yc, C_INK);
    } else swap_icon(g, c, x + w * 0.5f, yc, 10.f, C_INK);
    wid_add(g, kind, k, x, y, w, h);
}
/* a dice: a rounded square, s wide, centred on (x, y), its five pips (firmware/src/dice.c: the dice's rolls) */
static void dice_icon(txg_t *g, frui_canvas *c, float x, float y, float s, uint32_t col)
{
    static const float P[5][2] = { { 0.27f, 0.27f }, { 0.73f, 0.27f }, { 0.5f, 0.5f }, { 0.27f, 0.73f }, { 0.73f, 0.73f } };
    float k = s / 20.f, x0 = x - s * 0.5f + 1.5f * k, y0 = y - s * 0.5f + 1.5f * k, d = 17.f * k, pr = fmaxf(1.55f * k, 1.f);
    frui_rrect_stroke(c, x0 * g->S, y0 * g->S, d * g->S, d * g->S, 4.f * k * g->S, fmaxf(1.6f * k, 1.f) * g->S, col);
    for (int i = 0; i < 5; i++) CI(g, c, x0 + P[i][0] * d, y0 + P[i][1] * d, pr, col);
}
/* a row's dice, as its ENGINE button (lane k: a random engine on it); an empty lane: dim, nothing to click */
static void dice_button(txg_t *g, frui_canvas *c, int k, float x, float y, float w, float h, int on)
{
    int hot = on && g->hover_kind == TXG_W_DICE && g->hover_lane == k;
    RR(g, c, x, y, w, h, 3.f, hot ? C_SEG : C_BTN);
    RRS(g, c, x, y, w, h, 3.f, on ? (hot ? C_DIM : C_LINE) : 0x80262424u);
    dice_icon(g, c, x + w * 0.5f, y + h * 0.5f, 14.f, on ? C_INK : C_FAINT);
    if (on) wid_add(g, TXG_W_DICE, k, x, y, w, h);
}
/* a lane's STOP / PLAY: a square as M and S (t: a label after the icon, or none: the icon alone). Playing: a stop square
 * in ink; stopped: a play triangle in red; no engine: both dim, nothing to click (kind TXG_W_LRUN: the row's, TXG_W_DRUN:
 * the drawer's) */
static int used(const txg_t *g, int k);
static void run_button(txg_t *g, frui_canvas *c, int kind, int k, float x, float y, float w, float h, const char *t)
{
    const txp_ui_lane_t *l = &g->sn.ui.lane[k];
    int u = used(g, k), run = u && l->run, hot = u && g->hover_kind == kind && g->hover_lane == k;
    float yc = y + h * 0.5f, ix = t ? x + 12.f : x + w * 0.5f;
    uint32_t ic = !u ? C_FAINT : run ? C_INK : C_RED;
    RR(g, c, x, y, w, h, 3.f, g->zone == 1 ? (hot ? C_SNDSEG : C_SNDCARD) : hot ? C_SEG : C_BTN);
    RRS(g, c, x, y, w, h, 3.f, u ? (hot ? C_DIM : g->zone == 1 ? C_SNDLINE : C_LINE) : 0x80262424u);
    if (run) R(g, c, ix - 4.f, yc - 4.f, 8.f, 8.f, ic);
    else {
        frui_path p;
        frui_path_init(&p);
        frui_path_move(&p, (ix - 3.f) * g->S, (yc - 5.f) * g->S);
        frui_path_line(&p, (ix + 5.f) * g->S, yc * g->S);
        frui_path_line(&p, (ix - 3.f) * g->S, (yc + 5.f) * g->S);
        frui_path_fill(c, &p, ic);
        frui_path_free(&p);
    }
    if (t) LBL(g, c, x + 24.f, yc + CAP(g, 9.5f) * 0.5f, 9.5f, C_INK, t);
    if (t && g->zone) ZFIT(24.f + LBLW(g, 9.5f, "PLAY LANE") + 2.f <= w && 24.f + LBLW(g, 9.5f, "STOP LANE") + 2.f <= w);
    wid_add(g, kind, k, x, y, w, h);
}
/* a text cut to fit w with "..." (on a character boundary), as the font draws it: size, weight, tracking */
static void ell(txg_t *g, char *d, size_t n, const char *s, float w, float size, float wt, float track)
{
    size_t l;
    snprintf(d, n, "%s", s);
    if (TW(g, size, wt, track, 0, d) <= w) return;
    for (l = strlen(d); l > 0; l--) {
        if (((unsigned char)d[l] & 0xC0u) == 0x80u) continue;
        d[l] = 0;
        if (l + 3 < n) memcpy(d + l, "...", 4);
        if (TW(g, size, wt, track, 0, d) <= w) return;
        d[l] = 0;
    }
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

/* ---- the layout: the place at the left (round at any size), the rows beside it, the drawer and the strip under both.
 * An effect's drawer (OV_FX) rises from the strip in the lane drawer's place, as tall as it needs: the place and the
 * rows above it get the rest (the rows scroll) */
#define FXD_BODY 164.f                   /* an effect's drawer: its picture and its values */
#define FXD_TABS 34.f                    /* MOVEMENT's: its tabs over its body */
#define FXD_ENV (12.f + 48.f + 16.f + 4.f * 30.f + 3.f * 6.f + 12.f)
#define FXD_MAT (12.f + 14.f + 8.f * 30.f + 8.f)
#define MS_GS 170.f                      /* M/S's: its goniometer's square, the correlation under it (MS_CH), as tall */
#define MS_CH 46.f
#define FXD_MS (38.f + 8.f + MS_GS + MS_CH + 8.f + 12.f)
static float fxd_h(int tab)
{
    if (tab == SEC_MS) return FXD_MS;
    if (tab < SEC_MOVE) return FXD_BODY;
    if (tab < TAB_ENV) return FXD_TABS + FXD_BODY;
    return FXD_TABS + (tab == TAB_ENV ? FXD_ENV : FXD_MAT);
}
static void lay(txg_t *g)
{
    float top, bot, room;
    g->ct = TOP_H + (g->msrc ? MB_H : 0.f);   /* (a source's mode: its bar over the content, which moves down) */
    top = g->ct + PH_H;
    g->PW = floorf(g->W * 0.3667f);
    g->PW = g->PW < 400.f ? 400.f : g->PW > 640.f ? 640.f : g->PW;
    g->fx_y = g->H - FX_H;
    g->dr_h = g->ov == OV_FX ? fxd_h(g->fx_tab) : DR_H;
    g->dr_y = g->fx_y - g->dr_h;
    bot = g->dr_y;
    room = bot - top - 52.f;             /* (FAR over the outer ring, YOU under you) */
    g->mr = fminf((g->PW - 72.f) * 0.5f, room);
    g->mcx = g->PW * 0.5f;
    g->mcy = top + 20.f + g->mr + (room - g->mr) * 0.5f;
    g->sp_y0 = top + 14.f;
    g->sp_y1 = bot - 18.f;
    g->sp_hz = g->sp_y0 + (g->sp_y1 - g->sp_y0) * 0.1f;
    g->rvis = (int)floorf((g->dr_y - g->ct - HDR_H) / ROW_H);
    if (g->rvis < 1) g->rvis = 1;
}
/* a row's columns (x from the window's left): the engine's name (the square ENGINE button after it), the event line, STOP / PLAY, MANY, ACTIVE, PAN, CH, VOL, M S */
typedef struct { float num, dot, name, tl, tlw, run, many, act, pan, ch, vol, ms; } rcol_t;
#define RW_NAME 204.f
#define RW_CHG 22.f                      /* a row's ENGINE button (the engine browser): a square icon after the name */
#define RW_RUN 22.f                      /* a row's STOP / PLAY, before HOW MANY */
#define RW_STEP 78.f
#define RW_PAN 40.f
#define RW_CH 34.f
#define RW_VOL 80.f
#define RW_MS 48.f
#define R_GAP 8.f
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
    r.run = r.many - R_GAP - RW_RUN;
    r.tlw = r.run - R_GAP - r.tl;
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
/* COMP's ATTACK and REL in ms (the FM-1's page shows Frodhes' 0.0 .. 10.0): their time constants, tx_core_comp.c's
 * (ATTACK 0.1 ms x 500^(v / 127), REL 20 ms x 75^(v / 127)) */
static int comp_ms_text(uint32_t id, int v, char *out, uint32_t n)
{
    float ms;
    if (id != ID_CATK && id != ID_CREL) return 0;
    ms = id == ID_CATK ? 0.1f * powf(500.f, (float)v / 127.f) : 20.f * powf(75.f, (float)v / 127.f);
    if (ms < 9.95f) snprintf(out, n, "%.1f ms", (double)ms); else snprintf(out, n, "%d ms", (int)lroundf(ms));
    return 1;
}
/* a value's text as the window shows it: the app's own (its page's: "4.2 kHz", a card in its unit), or, while the
 * window's own write waits for the app, the host's text of it */
static void vtext(const txg_t *g, uint32_t id, char *out, uint32_t n)
{
    int i = txp_param_index(id), v = txp_ui_get(g->c, id);
    if (i < 0) { out[0] = 0; return; }
    if (comp_ms_text(id, v, out, n)) return;
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
    if ((id >> 8) == 4u && i >= TXP_PG_MSMID && i <= TXP_PG_MSMONO) return (u->ms_mod >> (i - TXP_PG_MSMID) & 1u) != 0;
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

/* a hint wrapped into lines lines at most, lh apart (what does not fit ends in "...": the whole hint shows on hover,
 * hint_tip); 1 if it was cut */
static int wrapn(txg_t *g, frui_canvas *c, float x, float y, float w, const char *s, int lines, float lh, float sz, uint32_t col)
{
    char ln[160];
    const char *p = s;
    int cut_any = 0;
    for (int l = 0; l < lines; l++) {
        size_t n = 0, cut = 0;
        ln[0] = 0;
        while (*p == ' ') p++;
        while (p[n]) {
            char t[160];
            size_t e = n;
            while (p[e] && p[e] != ' ') e++;
            if (e >= sizeof t - 4) break;
            memcpy(t, p, e); t[e] = 0;
            if (TW(g, sz, 450.f, 0.f, 0, t) > w) break;
            cut = e;
            n = e;
            while (p[n] == ' ') n++;
        }
        if (!cut && p[0]) for (cut = 0; p[cut] && p[cut] != ' ' && cut < 40; cut++) {}   /* (a word longer than the line) */
        memcpy(ln, p, cut); ln[cut] = 0;
        p += cut;
        while (*p == ' ') p++;
        if (l == lines - 1 && *p) {      /* (more: the last line ends in "...") */
            size_t m = strlen(ln);
            while (m > 0 && TW(g, sz, 450.f, 0.f, 0, ln) + TW(g, sz, 450.f, 0.f, 0, "...") > w) ln[--m] = 0;
            while (m > 0 && ln[m - 1] == ' ') ln[--m] = 0;
            strcat(ln, "...");
            cut_any = 1;
        }
        if (c) TX(g, c, x, y + lh * (float)l, sz, 450.f, 0.f, 0, col, ln);
        if (!*p) break;
    }
    return cut_any;
}
static int hintn(txg_t *g, frui_canvas *c, float x, float y, float w, const char *s, int lines, float lh, uint32_t col)
{
    return wrapn(g, c, x, y, w, s, lines, lh, 9.5f, col);
}
/* ---- a value: its name, its text and its control. An amount: a fader (a track, its filled part, a handle; the
 * rows' VOL's look), its live place in red while the modulation moves it. A choice: its value between steppers
 * (< >). Drag, wheel, double-click (its default); a click steps a choice. hint: lines under it (the drawer's cards; cut:
 * the whole of it on hover) */
static int is_bip(uint32_t id)           /* drawn from the middle: PAN, a route's DEPTH, TRANSPOSE */
{
    int k = lane_of_id(id);
    return (k >= 0 && (id & 0xFFu) == TXP_PL_PAN) || ((id >> 8) == 7u && (id & 0xFFu) < 64u && (id & 3u) == 2u) ||
           id == TXP_ID_GLOB(TXP_PG_TRANS);
}
static int is_choice(const txp_param_t *p) { return p->is_enum || p->hi < 20; }
/* a fader's place for a value: v / hi, but the M/S's MID and SIDE on a fader's taper (the FM-1's M/S page's: 0 dB at
 * three quarters, the gain's square root under it, +6 dB at the end; 0, -inf, at the left), and WIDTH at 0 while MONO
 * is on (it reads 0 %, SIDE kept for when MONO goes off). ms_val: a place back to a value */
static int ms_lvl(uint32_t id) { return id == ID_MSMID || id == ID_MSSIDE; }
static float ms_pos(float v)
{
    if (v <= 0.f) return 0.f;
    if (v >= (float)MS_0DB) return 0.75f + (v - (float)MS_0DB) * 0.25f / (127.f - (float)MS_0DB);
    return 0.75f * sqrtf(powf(10.f, (v - (float)MS_0DB) * 0.025f));   /* (v - 115) / 2 dB: the gain's root */
}
static float ms_val(float f)
{
    if (f >= 0.75f) return (float)MS_0DB + (f - 0.75f) * (127.f - (float)MS_0DB) / 0.25f;
    if (f <= 0.f) return 0.f;
    {
        float v = (float)MS_0DB + 40.f * log10f(f / 0.75f) * 2.f;   /* (the gain (f / 0.75)^2 in dB, 2 steps a dB) */
        return v < 0.5f ? 0.f : v < 1.f ? 1.f : v;
    }
}
static int ms_mono_on(const txg_t *g) { return txp_ui_get(g->c, ID_MSMONO) > 0; }
static float ctl_pos(const txg_t *g, uint32_t id, float v, int hi)
{
    float f;
    if (ms_lvl(id)) f = ms_pos(v);
    else if (id == ID_MSWIDTH && ms_mono_on(g)) f = 0.f;
    else f = v / (float)(hi > 0 ? hi : 1);
    return f < 0.f ? 0.f : f > 1.f ? 1.f : f;
}
/* ---- the movement: the matrix's links seen per control. The matrix (mod.c): 16 slots, each a SOURCE, a TARGET (an
 * index into the app's one list of targets, txp_catalog_t mt_*), a DEPTH (0 .. 200: -100 .. +100 %, 100 none) and a
 * SPREAD, all host parameters. A link: a slot with a source, a target and a depth. The window's sources (its tiles):
 * LFO 1 .. 8, ENV 1 .. 2, FOLLOW 1 .. 2 (mod.c MS_*). Every change is a host parameter's (the project keeps it, the host's
 * undo sees it); the window's own undo keeps the matrix as it was before each change (UNDO) */
#define MX_N 16
static const int SRCS[SRC_N] = { 1, 2, 3, 4, 5, 6, 7, 8, 16, 17, 18, 19 };
static void src_name(int s, char *o, size_t n)
{
    static const char *const X[7] = { "VELOCITY", "MOD WHEEL", "AFTERTOUCH", "CC", "ACTIVE", "RANDOM", "CONSTANT" };
    if (s >= 1 && s <= 8) snprintf(o, n, "LFO %d", s);
    else if (s == 16 || s == 17) snprintf(o, n, "ENV %d", s - 15);
    else if (s == 18 || s == 19) snprintf(o, n, "FOLLOW %d", s - 17);
    else if (s >= 9 && s <= 15) snprintf(o, n, "%s", X[s - 9]);
    else snprintf(o, n, "NONE");
}
static int src_tile(int s) { return (s >= 1 && s <= 8) || (s >= 16 && s <= 19); }
static int src_row(int s) { return s <= 8 ? s - 1 : s - 8; }   /* (its place in SRCS, lfoh) */
static float src_now(const txg_t *g, int s)   /* its value now: an LFO -1 .. 1, ENV and FOLLOW 0 .. 1; others 0 */
{
    if (s >= 1 && s <= 8) return (float)g->sn.ui.lfo[s - 1] / 32767.f;
    if (s >= 16 && s <= 19) return (float)g->sn.ui.envf[s - 16] / 32767.f;
    return 0.f;
}
static int mx(const txg_t *g, int s, int q) { return txp_ui_get(g->c, ID_MAT(s, q)); }
static int mx_used(const txg_t *g, int s) { return mx(g, s, 0) > 0 && mx(g, s, 1) > 0; }   /* (held: mod.c mod_learn) */
static int mx_link(const txg_t *g, int s) { return mx_used(g, s) && mx(g, s, 2) != 100; }
static int src_links(const txg_t *g, int src) { int n = 0; for (int s = 0; s < MX_N; s++) n += mx_link(g, s) && mx(g, s, 0) == src; return n; }
static int all_links(const txg_t *g) { int n = 0; for (int s = 0; s < MX_N; s++) n += mx_link(g, s); return n; }
/* the target a source moves parameter id by (the app's own mod learn's: mod.c mod_tgt_of), 0 none; why: 1 a choice, 2 a
 * lane past the fourth (the matrix reaches those only as ALL LANES), 3 the matrix has no such target */
static int ctl_tgt(const txg_t *g, uint32_t id, int *why)
{
    const txp_catalog_t *ct = g->cat;
    int k = lane_of_id(id), i = (int)(id & 0xFFu);
    const txp_param_t *p = pinfo(id);
    if (why) *why = p && is_choice(p) ? 1 : 3;
    if (!p || id >= TXP_ID_OWN0 || (id >> 8) == 7u) return 0;
    if (k >= 0) {
        if (!used(g, k)) return 0;
        if (i >= TXP_PL_CARD0 && i < TXP_PL_CARD0 + (int)TXP_CARD_N) {   /* (a card that is a choice: TYPE, PRESET) */
            const txp_ui_cards_t *cd = &g->sn.ui.card[k];
            int j = i - TXP_PL_CARD0;
            if (j >= cd->n || !strcmp(cd->lab[j], "TYPE") || !strcmp(cd->lab[j], "PRESET")) { if (why) *why = 1; return 0; }
        }
        if (k >= 4 && (i == TXP_PL_LEVEL || i == TXP_PL_PAN || i == TXP_PL_COUNT || i == TXP_PL_ACTIVE ||
                       (i >= TXP_PL_CARD0 && i < TXP_PL_CARD0 + (int)TXP_CARD_N))) { if (why) *why = 2; return 0; }
    }
    for (int T = 1; T < ct->nmt; T++) {
        int l = ct->mt_lane[T];
        if (l == 0xFF || l == 4 || !ct->mt_learn[T] || (l < 4 && l != k)) continue;
        if (ct->mt_id[T][k < 0 ? 0 : k] == id) return T;
    }
    return 0;
}
static int tgt_hits(const txg_t *g, int T, uint32_t id)   /* target T moves parameter id (ALL LANES: every lane's) */
{
    const txp_catalog_t *ct = g->cat;
    int k = lane_of_id(id), l;
    if (T <= 0 || T >= ct->nmt || (l = ct->mt_lane[T]) == 0xFF) return 0;
    if (l < 4) return k == l && ct->mt_id[T][l] == id;
    if (l == 4) return k >= 0 && ct->mt_id[T][k] == id;
    return k < 0 && ct->mt_id[T][0] == id;
}
typedef struct { int src, amt, all, slot; } lk_t;
static int ctl_links(const txg_t *g, uint32_t id, lk_t *o)   /* the links that move parameter id (MX_N at most) */
{
    int n = 0;
    for (int s = 0; s < MX_N; s++) {
        int T;
        if (!mx_link(g, s) || !tgt_hits(g, (T = mx(g, s, 1)), id)) continue;
        o[n].src = mx(g, s, 0); o[n].amt = mx(g, s, 2) - 100; o[n].all = g->cat->mt_lane[T] == 4; o[n].slot = s;
        n++;
    }
    return n;
}
static int link_slot(const txg_t *g, int src, int T)   /* the slot holding src -> T (any depth), -1 none */
{
    for (int s = 0; s < MX_N; s++) if (mx(g, s, 0) == src && mx(g, s, 1) == T) return s;
    return -1;
}
static int link_amt(const txg_t *g, int src, uint32_t id)   /* how far src moves id now (%), by id's own target */
{
    int T = ctl_tgt(g, id, 0), s = T ? link_slot(g, src, T) : -1;
    return s >= 0 ? mx(g, s, 2) - 100 : 0;
}
/* the range its links reach round value f (0 .. 1): an LFO both ways, an envelope or a follower its own way */
static void ctl_reach(const lk_t *lk, int n, float f, float *lo, float *hi)
{
    *lo = *hi = f;
    for (int i = 0; i < n; i++) {
        float a = (float)lk[i].amt / 100.f;
        if (lk[i].src >= 1 && lk[i].src <= 8) { *lo -= fabsf(a); *hi += fabsf(a); }
        else if (a > 0.f) *hi += a; else *lo += a;
    }
    *lo = *lo < 0.f ? 0.f : *lo > 1.f ? 1.f : *lo;
    *hi = *hi < 0.f ? 0.f : *hi > 1.f ? 1.f : *hi;
}
static float ctl_live(const txg_t *g, const lk_t *lk, int n, float f)   /* where its links have it now (0 .. 1) */
{
    for (int i = 0; i < n; i++) f += (float)lk[i].amt / 100.f * src_now(g, lk[i].src);
    return f < 0.f ? 0.f : f > 1.f ? 1.f : f;
}
/* a control's name in a message ("FILTER LOW PASS", "LANE 2 GUST") */
static void ctl_name(const txg_t *g, uint32_t id, char *o, size_t n)
{
    int k = lane_of_id(id), i = (int)(id & 0xFFu);
    const txp_param_t *p = pinfo(id);
    if (k >= 0) {
        static const char *const L[5] = { "ENGINE", "HOW MANY", "HOW ACTIVE", "LEVEL", "PAN" };
        if (i < 5) snprintf(o, n, "LANE %d %s", k + 1, L[i]);
        else if (i >= TXP_PL_CARD0 && i < TXP_PL_CARD0 + (int)TXP_CARD_N && g->sn.ui.card[k].lab[i - TXP_PL_CARD0][0])
            snprintf(o, n, "LANE %d %s", k + 1, g->sn.ui.card[k].lab[i - TXP_PL_CARD0]);
        else snprintf(o, n, "LANE %d %s", k + 1, p ? p->name : "");
        return;
    }
    if (p) upper(o, p->name, n); else snprintf(o, n, "IT");
}
static void say(txg_t *g, const char *s) { snprintf(g->msg, sizeof g->msg, "%s", s); g->msg_t = 150; }   /* (~5 s) */
/* a control's tags: "~ LFO 1 +25%" a link (ALL: on ALL LANES), as many as fit in wmax from x, then "+2" (none fit: "~ 2");
 * each one a click: its source's mode. fs: the size; the count of links */
static int tags_draw(txg_t *g, frui_canvas *c, uint32_t id, float x, float yb, float wmax, float fs)
{
    lk_t lk[MX_N];
    int n = ctl_links(g, id, lk), i;
    float xx = x, tw;
    char t[40], nm[16];
    for (i = 0; i < n; i++) {
        src_name(lk[i].src, nm, sizeof nm);
        snprintf(t, sizeof t, "~ %s %+d%%%s", nm, lk[i].amt, lk[i].all ? " ALL" : "");
        tw = TW(g, fs, 620.f, fs * 0.08f, 0, t);
        if (xx + tw > x + wmax) break;
        TX(g, c, xx, yb, fs, 620.f, fs * 0.08f, 0, C_RED, t);
        wid_add2(g, TXG_W_TAG, (int)id, id, lk[i].src, xx - 2.f, yb - fs - 1.f, tw + 4.f, fs + 5.f);
        xx += tw + 8.f;
    }
    if (i < n) {                         /* (the rest: their number) */
        snprintf(t, sizeof t, i ? "+%d" : "~ %d", n - i);
        tw = TW(g, fs, 620.f, fs * 0.08f, 0, t);
        if (xx + tw <= x + wmax + 0.5f) {
            TX(g, c, xx, yb, fs, 620.f, fs * 0.08f, 0, C_RED, t);
            wid_add2(g, TXG_W_TAG, (int)id, id, lk[i].src, xx - 2.f, yb - fs - 1.f, tw + 4.f, fs + 5.f);
        }
    }
    return n;
}
/* a fader in a source's mode: the value greyed, the source's reach in red from it to where the source pushes it (amt:
 * -1 .. 1 of the whole range), ending in a red handle (th: the track's thickness, hw x hh: the handle) */
static void fader_mode(txg_t *g, frui_canvas *c, float x, float yc, float w, float f, float f0, float amt, float th, float hw, float hh)
{
    float e;
    f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
    e = f + amt; e = e < 0.f ? 0.f : e > 1.f ? 1.f : e;
    RR(g, c, x, yc - th * 0.5f, w, th, th * 0.5f, C_FAINT);
    if (fabsf(f - f0) * w >= 0.5f) RR(g, c, x + w * fminf(f0, f), yc - th * 0.5f, w * fabsf(f - f0), th, th * 0.5f, C_GREY);
    if (fabsf(e - f) * w >= 0.5f) RR(g, c, x + w * fminf(f, e), yc - th * 0.5f - 1.f, w * fabsf(e - f), th + 2.f, th * 0.5f + 1.f, C_RED);
    RR(g, c, x + w * f - hw * 0.5f, yc - hh * 0.5f, hw, hh, 2.f, C_GREYH);
    if (amt != 0.f) {
        RR(g, c, x + w * e - hw * 0.5f - 1.f, yc - hh * 0.5f - 1.f, hw + 2.f, hh + 2.f, 2.f, C_RED);
        RRS(g, c, x + w * e - hw * 0.5f - 1.f, yc - hh * 0.5f - 1.f, hw + 2.f, hh + 2.f, 2.f, C_INK);
    }
}
static void amt_text(char *t, size_t n, int amt) { if (amt) snprintf(t, n, "%+d%%", amt); else snprintf(t, n, "0%%"); }
static void chevron(txg_t *g, frui_canvas *c, float x, float yc, int dir, uint32_t col)   /* < (dir -1) or > (+1) */
{
    LN(g, c, x - 2.f * (float)dir, yc - 3.5f, x + 2.f * (float)dir, yc, 1.3f, col);
    LN(g, c, x + 2.f * (float)dir, yc, x - 2.f * (float)dir, yc + 3.5f, 1.3f, col);
}
/* a fader: x .. x + w, its track centred at yc (f, live: 0 .. 1; live -1: not moved; f0 its zero: 0 or the middle); lo ..
 * hi: the reach of the sources that move it (a faint red band behind the track; lo > hi: none), live their red tick;
 * mk: a mark on the track (the M/S's 0 dB, 100 %), -1 none */
static void fader(txg_t *g, frui_canvas *c, float x, float yc, float w, float f, float f0, float live, int big, float lo, float hi,
                  float mk)
{
    float th = big ? 4.f : 3.f, hw = big ? 8.f : 7.f, hh = big ? 12.f : 10.f, hx;
    f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
    if (hi > lo && (hi - lo) * w >= 0.5f) RR(g, c, x + w * lo, yc - th * 0.5f - 1.5f, w * (hi - lo), th + 3.f, th * 0.5f + 1.5f, frui_alpha(C_RED, 0.35f));
    RR(g, c, x, yc - th * 0.5f, w, th, th * 0.5f, g->zone == 1 ? C_SNDTRK : g->zone == 2 ? C_FXTRK : C_FAINT);
    if (fabsf(f - f0) * w >= 0.5f) RR(g, c, x + w * fminf(f0, f), yc - th * 0.5f, w * fabsf(f - f0), th, th * 0.5f, frui_alpha(C_INK, 0.85f));
    if (f0 > 0.f) R(g, c, x + w * f0 - 0.5f, yc - th, 1.f, th * 2.f, C_DIM);   /* (a bipolar one: its middle) */
    if (mk >= 0.f) R(g, c, x + w * mk - 0.5f, yc - th - 1.f, 1.f, th * 2.f + 2.f, C_DIM);
    hx = x + w * f - hw * 0.5f;
    RR(g, c, hx, yc - hh * 0.5f, hw, hh, 2.f, C_INK);
    R(g, c, hx + hw * 0.5f - 0.5f, yc - hh * 0.5f + 3.f, 1.f, hh - 6.f, C_DIM);   /* (its grip) */
    if (live >= 0.f) {                   /* (where the sources have it now: a red tick) */
        float lx = x + w * (live < 0.f ? 0.f : live > 1.f ? 1.f : live);
        RR(g, c, lx - 1.5f, yc - hh * 0.5f + 1.f, 3.f, hh - 2.f, 1.5f, C_RED);
    }
}
/* a value: its name, its text and its control (above). Moved by a source: its name red, the reach and the live tick on
 * its fader, a tag per link (in the cell above its fader when there is room, else beside its name; the strip's: under
 * the cell). In a source's mode (g->msrc) a control the source can move shows the source's link instead: its amount
 * for the text, the reach on its fader (fader_mode), red when linked */
static void pcell(txg_t *g, frui_canvas *c, float x, float y, float w, float h, uint32_t id, const char *lab, const char *hint)
{
    const txp_param_t *p = pinfo(id);
    char t[40], nm[40];
    int v, hi, live, m, ch, stack = 0, tight = 0, T, amt = 0, nl;
    float ty, nw, tcy, tagy = 0.f, rh = 0.f;
    lk_t lk[MX_N];
    /* the zone's look: a SOUND card (ns: its name's size, vs its value's, vwt its weight; its description lighter, a size
     * up), a SOUND row (no box: a line under it), an EFFECTS row (no box, on its section); else a well */
    int z = g->zone, card = z == 1 && hint, flat = z && !card;
    float ns = card ? 11.f : 10.5f, vs = card ? 13.f : z == 2 ? 11.5f : 11.f, vwt = card ? 600.f : 500.f,
          hs = card ? 11.f : 9.5f, hlh = card ? 14.f : 10.5f, hy = card ? 29.f : 27.f;
    uint32_t seg = z == 1 ? C_SNDSEG : z == 2 ? C_FXSEG : C_SEG, hcol = card ? C_DESC : C_DIM, ncol = z == 2 ? C_FXNAME : C_INK;
    if (!p) return;
    v = txp_ui_get(g->c, id); hi = p->hi;
    T = g->msrc ? ctl_tgt(g, id, 0) : 0;
    nl = g->msrc ? 0 : ctl_links(g, id, lk);
    m = pmod(g, id, &live);
    if (T) amt = link_amt(g, g->msrc, id);
    ch = is_choice(p);
    if (amt) { RR(g, c, x, y, w, h, 2.f, C_REDBG); RRS(g, c, x, y, w, h, 2.f, C_RED); }
    else if (card) RR(g, c, x, y, w, h, 5.f, C_SNDCARD);
    else if (z == 1) R(g, c, x, y + h, w, 1.f, C_SNDLINE);
    else if (!flat) RR(g, c, x, y, w, h, 2.f, C_WELL);
    if (!lab) { short_name(p, nm, sizeof nm); lab = nm; }
    /* the text line: the name at the left, the value at the right; under it the fader (an amount) */
    tcy = hint ? y + h - 6.f : y + h - fmaxf(6.f, h * 0.25f);
    if (hint) ty = y + 14.f;
    else if (ch) ty = y + h * 0.5f + CAP(g, ns) * 0.5f;
    else ty = fminf(tcy - 7.f, y + (tcy - y) * 0.5f + CAP(g, ns) * 0.5f);
    if (T) amt_text(t, sizeof t, amt);
    else if (id == ID_MSWIDTH && ms_mono_on(g)) snprintf(t, sizeof t, "0 %%");   /* (MONO: no width; SIDE kept) */
    else vtext(g, id, t, sizeof t);
    if (ch && hi >= 1 && !hint && h >= 34.f &&  /* (a choice too wide for its name beside it: the name above, the value under) */
        x + w - 4.f - (TW(g, vs, vwt, 0.f, FRUI_TNUM, t) + 34.f) < x + 9.f + LBLW(g, ns, lab) + 6.f) {
        stack = 1;
        ty = y + 6.f + CAP(g, ns);
        if (z) ZFIT(LBLW(g, ns, lab) <= w - 18.f && TW(g, vs, vwt, 0.f, FRUI_TNUM, t) + 24.f <= w - 18.f);
    }
    if (!stack) {                        /* (the name and the value side by side: a name that would run into the value
                                          * takes its short form; a choice's box first gives up some of its padding) */
        float vw = TW(g, vs, T ? 700.f : vwt, 0.f, FRUI_TNUM, t);   /* (the widest value it can show, not the
                                          * one now: a name keeps its form while the value moves) */
        for (int k = 0, step = hi <= 32 ? 1 : hi / 16; hi > 0 && k <= hi; k += step) {
            char s[40];
            ptext(id, k, s, sizeof s);
            vw = fmaxf(vw, TW(g, vs, vwt, 0.f, FRUI_TNUM, s));
        }
        if (ch && hi >= 1) { vw += 24.f; tight = 1; }
        lab = lab_fit(g, lab, w - (ch && hi >= 1 ? 4.f : 8.f) - vw - 6.f - 9.f, ns);
        if (z) ZFIT(LBLW(g, ns, lab) <= w - (ch && hi >= 1 ? 4.f : 8.f) - vw - 6.f - 9.f + 0.01f);   /* (the name
                                          * clear of the widest value) */
        if (ch && hi >= 1 && x + 9.f + LBLW(g, ns, lab) + 6.f <= x + w - 4.f - (vw + 10.f)) tight = 0;
    }
    clipL(g, c, x, y, w, h);
    nw = 9.f;
    if (lab[0]) nw += LBL(g, c, x + nw, ty, ns, (m || nl) && !g->msrc ? C_RED : ncol, lab);
    if (ch && hi >= 1) {                 /* a choice: < its value > (the arrows step it) */
        float tw = TW(g, vs, vwt, 0.f, FRUI_TNUM, t), bw = tw + (tight ? 24.f : 34.f), bh = fminf(h - 6.f, 18.f), bx = x + w - 4.f - bw,
              byc = hint ? ty - CAP(g, vs) * 0.5f : y + h * 0.5f;
        if (stack) {
            bh = fminf(18.f, y + h - 4.f - (ty + 5.f));
            bx = x + 9.f; bw = fminf(bw, w - 18.f); byc = ty + 5.f + bh * 0.5f;
        }
        if (bx < x + 4.f) { bx = x + 4.f; bw = w - 8.f; }
        RR(g, c, bx, byc - bh * 0.5f, bw, bh, 2.f, seg);
        if ((m || nl) && !g->msrc) R(g, c, bx + 2.f, byc + bh * 0.5f - 1.5f, bw - 4.f, 1.5f, C_RED);
        if (T && amt) {                  /* (in the mode: the reach from the middle of the box) */
            float e = fmaxf(-1.f, fminf(1.f, (float)amt / 100.f));
            R(g, c, bx + bw * 0.5f + fminf(0.f, e) * (bw * 0.5f - 2.f), byc + bh * 0.5f - 2.f, fabsf(e) * (bw * 0.5f - 2.f), 2.f, C_RED);
        }
        chevron(g, c, bx + (tight ? 6.f : 8.f), byc, -1, C_DIM);
        chevron(g, c, bx + bw - (tight ? 6.f : 8.f), byc, 1, C_DIM);
        TX(g, c, bx + (bw - tw) * 0.5f, byc + CAP(g, vs) * 0.5f, vs, T ? 700.f : vwt, 0.f, FRUI_TNUM, T ? (amt ? C_RED : C_FAINT) : C_INK, t);
        if (hint && hint[0]) (void)wrapn(g, c, x + 9.f, y + hy, w - 18.f, hint, card ? (int)((h - hy + hlh - 8.f) / hlh) : (int)((h - 20.f) / 10.5f), hlh, hs, hcol);
        frui_clip_reset(c);
        wid_add2(g, TXG_W_PARAM, (int)id, id, 0, x, y, w, h);
        wid_track(g, bx, bw);
        wid_add2(g, TXG_W_STEP, (int)id, id, -1, bx, byc - bh * 0.5f, 16.f, bh);
        wid_add2(g, TXG_W_STEP, (int)id, id, 1, bx + bw - 16.f, byc - bh * 0.5f, 16.f, bh);
        if (nl && g->tag_below) (void)tags_draw(g, c, id, x + 2.f, y + h + 9.f, w - 2.f, 8.5f);
        else if (nl) { float tx0 = x + nw + 8.f; (void)tags_draw(g, c, id, tx0, ty, bx - 6.f - tx0, 9.f); }
        return;
    }
    {
        float tw = TW(g, vs, T ? 700.f : vwt, 0.f, FRUI_TNUM, t);
        TX(g, c, x + w - 8.f - tw, ty, vs, T ? 700.f : vwt, 0.f, FRUI_TNUM, T ? (amt ? C_RED : C_FAINT) : C_INK, t);
        rh = tw;
    }
    if (hint && hint[0]) {
        int lines = (int)((tcy - 10.f - (y + hy)) / hlh) + 1;   /* (the last line clear of the fader's handle; with
                                          * tags: theirs) */
        int hl = nl && lines > 1 ? lines - 1 : lines;
        if (wrapn(g, 0, x + 9.f, y + hy, w - 18.f, hint, hl < 1 ? 1 : hl, hlh, hs, hcol) || hl < lines) wid_add2(g, TXG_W_TIP, (int)id, id, 0, x, y, w, h);
        (void)wrapn(g, c, x + 9.f, y + hy, w - 18.f, hint, hl < 1 ? 1 : hl, hlh, hs, hcol);
        if (nl && lines > 1) tagy = y + hy + hlh * (float)(lines - 1);   /* (where its tags go) */
    }
    frui_clip_reset(c);
    if (hi > 0) {   /* (the reach, the live place: of the value, v / hi, then on the fader's own taper: ctl_pos) */
        float fx = x + 10.f, fw = w - 20.f, fv = (float)v / (float)hi, f = ctl_pos(g, id, (float)v, hi), f0 = is_bip(id) ? 0.5f : 0.f,
              mk = ms_lvl(id) ? ms_pos((float)MS_0DB) : id == ID_MSWIDTH ? (float)MS_W100 / (float)hi : -1.f;
        int big = h >= 30.f;
        if (T) fader_mode(g, c, fx, tcy, fw, f, f0, ctl_pos(g, id, (fv + (float)amt / 100.f) * (float)hi, hi) - f, big ? 4.f : 3.f,
                          big ? 8.f : 7.f, big ? 12.f : 10.f);
        else {
            float rlo = 1.f, rhi = 0.f, lv = m && live >= 0 ? ctl_pos(g, id, (float)live, hi) : -1.f;
            if (nl) {
                ctl_reach(lk, nl, fv, &rlo, &rhi);
                rlo = ctl_pos(g, id, rlo * (float)hi, hi); rhi = ctl_pos(g, id, rhi * (float)hi, hi);
                if (lv < 0.f) lv = ctl_pos(g, id, ctl_live(g, lk, nl, fv) * (float)hi, hi);
            }
            fader(g, c, fx, tcy, fw, f, f0, lv, big, rlo, rhi, mk);
        }
    }
    wid_add2(g, TXG_W_PARAM, (int)id, id, 0, x, y, w, h);
    if (hi > 0) wid_track(g, x + 10.f, w - 20.f);
    if (nl) {                            /* its tags: above the fader, under the cell (the strip) or beside the name */
        if (tagy > 0.f) (void)tags_draw(g, c, id, x + 9.f, tagy, w - 18.f, 9.f);
        else if (g->tag_below) (void)tags_draw(g, c, id, x + 2.f, y + h + 9.f, w - 2.f, 8.5f);
        else if (!hint && h >= 44.f) (void)tags_draw(g, c, id, x + 9.f, ty + 13.f, w - 18.f, 9.f);
        else { float tx0 = x + nw + 8.f; (void)tags_draw(g, c, id, tx0, ty, x + w - 8.f - rh - 8.f - tx0, 9.f); }
    }
}
/* the whole hint of a card it cut, over the window while the pointer is on the card */
static void hint_tip(txg_t *g, frui_canvas *c)
{
    const char *s = 0;
    float r[4];
    int k, i;
    if (g->hover_kind != TXG_W_TIP || g->ov != OV_NONE || g->pick >= 0) return;
    if (!txg_find(g, TXG_W_TIP, g->hover_lane, r)) return;
    k = lane_of_id((uint32_t)g->hover_lane); i = (int)((uint32_t)g->hover_lane & 0xFFu) - TXP_PL_CARD0;
    if (k < 0 || i < 0 || i >= (int)TXP_CARD_N || !used(g, k)) return;
    s = card_hint(g->sn.ui.lane[k].snd, g->sn.ui.card[k].lab[i]);
    if (!s || !s[0]) return;
    {
        float w = fmaxf(r[2] + 40.f, 280.f), x = r[0] - 20.f, n, h, y;
        if (x + w > g->W - 8.f) x = g->W - 8.f - w;
        if (x < 8.f) x = 8.f;
        n = 1.f;
        {   /* (its lines: as many as it needs) */
            int l = 1;
            while (l < 12 && hintn(g, 0, 0.f, 0.f, w - 24.f, s, l, 12.f, C_INK)) l++;
            n = (float)l;
        }
        h = 30.f + 12.f * n;
        y = r[1] - h - 6.f;
        if (y < g->ct + 4.f) y = r[1] + r[3] + 6.f;
        frui_rrect_shadow(c, x * g->S, y * g->S, w * g->S, h * g->S, 4.f * g->S, 12.f * g->S, 0x90000000u);
        RR(g, c, x, y, w, h, 4.f, C_BTN); RRS(g, c, x, y, w, h, 4.f, C_DIM);
        LBL(g, c, x + 12.f, y + 17.f, 9.5f, C_DIM, g->sn.ui.card[k].lab[i]);
        (void)hintn(g, c, x + 12.f, y + 32.f, w - 24.f, s, (int)n, 12.f, C_INK);
    }
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
static void draw_meter(txg_t *g, frui_canvas *c, float x, float w, float yc)   /* the output's level: LEVEL -6.0 dB, a
                                                                                * bar, w wide */
{
    char t[24];
    int d = g->sn.ui.out_db10;
    float f = d <= -600 ? 0.f : (float)(d + 600) / 600.f;
    f = f > 1.f ? 1.f : f;
    LBL(g, c, x, yc - 2.f, 9.5f, C_DIM, "LEVEL");
    if (d <= -900) snprintf(t, sizeof t, "-inf dB"); else snprintf(t, sizeof t, "%.1f dB", d / 10.0);
    TX(g, c, x + w - TW(g, 11.f, 500.f, 0.f, FRUI_TNUM, t), yc - 2.f, 11.f, 500.f, 0.f, FRUI_TNUM, d > -10 ? C_RED : C_INK, t);
    RR(g, c, x, yc + 5.f, w, 4.f, 2.f, C_FAINT);
    if (f > 0.f) RR(g, c, x, yc + 5.f, w * f, 4.f, 2.f, d > -10 ? C_RED : C_DIM);
    wid_add(g, TXG_W_LVL, -1, x, yc - 2.f - CAP(g, 11.f), w, CAP(g, 11.f) + 11.f);
}
/* the top bar's right end, sized by its texts: LEVEL (its label and widest value, its bar), the dice (a roll) with its
 * arrow (RANDOM's list: what a roll changes), SETTINGS (its icon, its label) */
#define TOP_ROLL_W 32.f                  /* (the dice) */
#define TOP_DICE_W 52.f                  /* (the dice and its arrow) */
typedef struct { float mx, mw, dx, sx, sw; } top_r_t;
static top_r_t top_r(txg_t *g)
{
    top_r_t r;
    float lv = LBLW(g, 9.5f, "LEVEL") + 12.f + TW(g, 11.f, 500.f, 0.f, FRUI_TNUM, "-88.8 dB");
    r.sw = 31.f + LBLW(g, 12.f, "SETTINGS") + 12.f;
    r.sx = g->W - 16.f - r.sw;
    r.dx = r.sx - 6.f - TOP_DICE_W;
    r.mw = lv > 100.f ? lv : 100.f;
    r.mx = r.dx - 14.f - r.mw;
    return r;
}
static float meter_x(txg_t *g) { return top_r(g).mx; }
/* the top bar's left: TEXTURA, HELP (a ? in a ring and its label: the tour), then the previous scene, the scene's box and
 * the next (the box: its code, its name and "*" when edited, a caret at its right end). The box's room is measured at the
 * widest name a scene can show (every factory preset, landscape and user preset now) with the widest code: top_fit; a name wider still (a user preset typed later) ends in "..." */
#define HELP_D 16.f                      /* HELP's ? (round, in its button) */
#define HELP_GAP 12.f                    /* (the logo to it) */
#define NAV_GAP 8.f                      /* (it to the previous scene's button) */
static float tag_w(txg_t *g, const char *t) { return TW(g, 13.f, 500.f, 0.4f, FRUI_TNUM, t); }
static float pn_w(txg_t *g, const char *t) { return TW(g, 13.f, 500.f, 0.6f, 0, t); }
static float pname_widest(txg_t *g)       /* the widest name the box can show now (cached by scale and user names) */
{
    uint32_t key = fnv(2166136261u, &g->sn.ui.user, sizeof g->sn.ui.user);
    char nm[24];
    float w = 0.f;
    key = fnv(key, g->sn.ui.uname, sizeof g->sn.ui.uname);
    if (g->pname_s == g->S && g->pname_key == key && g->pname_w > 0.f) return g->pname_w;
    for (int i = 0; i < g->cat->npr; i++) w = fmaxf(w, pn_w(g, g->cat->pr[i]));
    for (int f = 0; f < g->cat->nfam; f++) { upper(nm, g->cat->fam[f].name, sizeof nm); w = fmaxf(w, pn_w(g, nm)); }
    for (int u = 0; u < (int)TXP_USER_N; u++) if (g->sn.ui.user >> u & 1u) w = fmaxf(w, pn_w(g, g->sn.ui.uname[u]));
    w = fmaxf(w, pn_w(g, "NO SCENE"));
    g->pname_w = w; g->pname_s = g->S; g->pname_key = key;
    return w;
}
static float help_w(txg_t *g) { return ceilf(9.f + HELP_D + 7.f + LBLW(g, 11.f, "HELP") + 12.f); }   /* (its label measured) */
static void help_button(txg_t *g, frui_canvas *c, float x, float y, float h)   /* a ? in a ring and HELP: the tour */
{
    int hot = g->hover_kind == TXG_W_HELP || g->tour >= 0;
    float w = help_w(g), r = HELP_D * 0.5f, cx = x + 9.f + r, yc = y + h * 0.5f;
    RR(g, c, x, y, w, h, 3.f, hot ? C_SEG : C_BTN); RRS(g, c, x, y, w, h, 3.f, hot ? C_DIM : C_LINE);
    RI(g, c, cx, yc, r - 0.7f, 1.4f, C_INK);
    TX(g, c, cx - TW(g, 11.f, 650.f, 0.f, 0, "?") * 0.5f, yc + CAP(g, 11.f) * 0.5f, 11.f, 650.f, 0.f, 0, C_INK, "?");
    LBL(g, c, cx + r + 7.f, yc + CAP(g, 11.f) * 0.5f, 11.f, C_INK, "HELP");
    wid_add(g, TXG_W_HELP, -1, x, y, w, h);
}
static void draw_top(txg_t *g, frui_canvas *c)
{
    const float y = 12.f, h = 28.f, yc = y + h * 0.5f;
    char tag[8] = "", name[24] = "", t[32];
    const char *pr = g->sn.ui.preset, *sp = strchr(pr, ' ');
    /* (PLAY ALL / STOP ALL: its icon, its label at its widest, 12 each side) */
    float rw = ceilf(28.f + fmaxf(LBLW(g, 11.f, "STOP ALL"), LBLW(g, 11.f, "PLAY ALL")) + 12.f);
    float x, bx0, xl = meter_x(g) - 146.f, xr = xl - 10.f - rw, xu = xr - 66.f, xs = xu - 66.f, bx1 = xs - 40.f, nr, need;
    int fit = 1;
    R(g, c, 0, TOP_H - 1.f, g->W, 1.f, C_LINE);
    x = 18.f + TX(g, c, 18.f, yc + CAP(g, 19.f) * 0.5f, 19.f, 700.f, 5.f, 0, C_INK, "TEXTUR") + 5.f;
    x += TX(g, c, x, yc + CAP(g, 19.f) * 0.5f, 19.f, 700.f, 0.f, 0, C_RED, "A");
    wid_add(g, TXG_W_LOGO, -1, 18.f, y, x - 18.f, h);
    help_button(g, c, x + HELP_GAP, y, h);
    bx0 = x + HELP_GAP + help_w(g) + NAV_GAP + 34.f;   /* (the previous scene's button after HELP) */
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
    /* (the name's room: from the code to the caret; the code, the name and "*" at their widest must fit it. The lane count
     * is LANES' own, beside it) */
    nr = bx1 - 26.f;
    need = 12.f + fmaxf(fmaxf(tag_w(g, "F000"), tag_w(g, "U00")), tag_w(g, "L00")) + 12.f + pname_widest(g) + 6.f +
           TW(g, 13.f, 500.f, 0.f, 0, "*");
    fit &= bx0 + need <= nr;
    clipL(g, c, bx0, y, nr - bx0, h);
    x = bx0 + 12.f;
    if (tag[0]) x += TX(g, c, x, yc + CAP(g, 13.f) * 0.5f, 13.f, 500.f, 0.4f, FRUI_TNUM, C_RED, tag) + 12.f;
    {   /* (a name wider than its room, as a user preset can be: cut with "...", "*" after it) */
        char nm[32];
        float sw = g->sn.ui.edited ? 6.f + TW(g, 13.f, 500.f, 0.f, 0, "*") : 0.f;
        ell(g, nm, sizeof nm, name, nr - x - sw, 13.f, 500.f, 0.6f);
        x += TX(g, c, x, yc + CAP(g, 13.f) * 0.5f, 13.f, 500.f, 0.6f, 0, C_INK, nm);
    }
    if (g->sn.ui.edited) TX(g, c, x + 6.f, yc + CAP(g, 13.f) * 0.5f, 13.f, 500.f, 0.f, 0, C_DIM, "*");
    frui_clip_reset(c);
    caret(g, c, bx1 - 14.f, yc, C_DIM);
    /* (the buttons' labels in their boxes, LANES before its minus: the rest of top_fit; HELP's is measured, help_w) */
    fit &= LBLW(g, 11.f, "SAVE") + 16.f <= 60.f && LBLW(g, 11.f, "UNDO") + 16.f <= 60.f;
    fit &= LBLW(g, 11.f, "LANES") + 6.f <= 54.f && xl + 54.f + 56.f + 24.f + 8.f <= meter_x(g);
    g->top_fit = fit;
    g->top_need = need; g->top_room = nr - bx0;
    /* SAVE, UNDO, PLAY ALL / STOP ALL (the FM-1's REC: all stopped is plain to see) */
    g->sv_x = xs;
    RR(g, c, xs, y, 60.f, h, 3.f, g->ov == OV_SAVE ? C_SEG : C_BTN); RRS(g, c, xs, y, 60.f, h, 3.f, g->ov == OV_SAVE ? C_DIM : C_LINE);
    LBL(g, c, xs + (60.f - LBLW(g, 11.f, "SAVE")) * 0.5f, yc + CAP(g, 11.f) * 0.5f, 11.f, C_INK, "SAVE");
    wid_add(g, TXG_W_SAVE, -1, xs, y, 60.f, h);
    RR(g, c, xu, y, 60.f, h, 3.f, C_BTN); RRS(g, c, xu, y, 60.f, h, 3.f, C_LINE);
    LBL(g, c, xu + (60.f - LBLW(g, 11.f, "UNDO")) * 0.5f, yc + CAP(g, 11.f) * 0.5f, 11.f, g->sn.ui.undo || g->lh_n ? C_INK : C_FAINT, "UNDO");
    wid_add(g, TXG_W_UNDO, -1, xu, y, 60.f, h);
    {
        int run = g->sn.ui.any_run, stopped = !run && any_used(g);
        const char *rt = run ? "STOP ALL" : "PLAY ALL";
        RR(g, c, xr, y, rw, h, 3.f, stopped ? C_INK : C_BTN); RRS(g, c, xr, y, rw, h, 3.f, stopped ? C_INK : C_LINE);
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
        wid_add(g, TXG_W_REC, -1, xr, y, rw, h);
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
    {
        top_r_t tr = top_r(g);
        int on = g->ov == OV_RND, hr = g->hover_kind == TXG_W_RND, hm = g->hover_kind == TXG_W_RNDMENU;
        const float aw = TOP_DICE_W - TOP_ROLL_W, ax = tr.dx + TOP_ROLL_W;
        draw_meter(g, c, tr.mx, tr.mw, yc);
        /* the dice (a click: a roll, as ticked) and its arrow (RANDOM's list), one button split by a line */
        x = tr.dx;
        RR(g, c, x, y, TOP_DICE_W, h, 3.f, C_BTN);
        if (hr) RR(g, c, x, y, TOP_ROLL_W, h, 3.f, C_SEG);
        if (on || hm) RR(g, c, ax, y, aw, h, 3.f, C_SEG);
        RRS(g, c, x, y, TOP_DICE_W, h, 3.f, on || hr || hm ? C_DIM : C_LINE);
        R(g, c, ax, y + 5.f, 1.f, h - 10.f, C_LINE);
        dice_icon(g, c, x + TOP_ROLL_W * 0.5f, yc, 17.f, C_INK);
        LN(g, c, ax + aw * 0.5f - 3.5f, yc - 1.5f, ax + aw * 0.5f, yc + 2.f, 1.4f, on ? C_RED : hm ? C_INK : C_DIM);
        LN(g, c, ax + aw * 0.5f, yc + 2.f, ax + aw * 0.5f + 3.5f, yc - 1.5f, 1.4f, on ? C_RED : hm ? C_INK : C_DIM);
        wid_add(g, TXG_W_RND, -1, x, y, TOP_ROLL_W, h);
        wid_add(g, TXG_W_RNDMENU, -1, ax, y, aw, h);
        /* SETTINGS */
        x = tr.sx;
        RR(g, c, x, y, tr.sw, h, 3.f, g->ov == OV_SET ? C_SEG : C_BTN); RRS(g, c, x, y, tr.sw, h, 3.f, C_LINE);
        RI(g, c, x + 17.f, yc, 3.6f, 1.5f, C_INK);
        for (int i = 0; i < 8; i++) {
            float a = (float)i * 0.785398f, ca = cosf(a), sa = sinf(a);
            LN(g, c, x + 17.f + ca * 5.6f, yc + sa * 5.6f, x + 17.f + ca * 7.4f, yc + sa * 7.4f, 1.6f, C_INK);
        }
        LBL(g, c, x + 31.f, yc + CAP(g, 12.f) * 0.5f, 12.f, C_INK, "SETTINGS");
        wid_add(g, TXG_W_SET, -1, x, y, tr.sw, h);
    }
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
/* the place's tabs: MAP | SPACE | M/S, each its label and 14 each side (the selected: lit, a red line under it) */
static const char *const VIEW_N[3] = { "MAP", "SPACE", "M/S" };
static float vtab_w(txg_t *g, int i) { return ceilf(LBLW(g, 11.f, VIEW_N[i]) + 28.f); }
static float vtab_end(txg_t *g) { return 16.f + vtab_w(g, 0) + vtab_w(g, 1) + vtab_w(g, 2); }
static void draw_view_switch(txg_t *g, frui_canvas *c)
{
    static const int K[3] = { TXG_W_VMAP, TXG_W_VSPACE, TXG_W_VMS };
    float x = 16.f, y = g->ct + 12.f, h = 24.f, tw = vtab_end(g) - x;
    RR(g, c, x, y, tw, h, 3.f, C_PANEL);
    for (int i = 0; i < 3; i++) {
        float w = vtab_w(g, i);
        if (g->view == i) { R(g, c, x, y, w, h, C_SEG); R(g, c, x, y + h - 2.f, w, 2.f, C_RED); }
        if (i) R(g, c, x, y, 1.f, h, C_LINE);
        LBL(g, c, x + (w - LBLW(g, 11.f, VIEW_N[i])) * 0.5f, y + h * 0.5f + CAP(g, 11.f) * 0.5f, 11.f, g->view == i ? C_INK : C_DIM,
            VIEW_N[i]);
        wid_add(g, K[i], -1, x, y, w, h);
        x += w;
    }
    RRS(g, c, 16.f, y, tw, h, 3.f, C_LINE);
}
/* the selected lane at the place's head: its number and family, its engine and ENGINE (the engine browser), its
 * engine's colour (a click: the picker) */
#define HEAD_SZ 15.f                     /* the head's name: its size, smaller (to HEAD_MIN) when the room is short */
#define HEAD_MIN 11.5f
static float head_w(txg_t *g, float sz, const char *s) { return TW(g, sz, 650.f, 0.2f, 0, s); }
static float head_sz(txg_t *g, const char *s, float room)
{
    float sz = HEAD_SZ;
    while (sz > HEAD_MIN && head_w(g, sz, s) > room) sz -= 0.5f;
    return sz;
}
static void snd_widest(txg_t *g, char *nm, size_t n)   /* the widest sound name a lane can show (cached by scale) */
{
    if (g->snd_s != g->S) {
        float w = -1.f;
        for (int f = 0; f < g->cat->nfam; f++)
            for (int i = 0; i < g->cat->fam[f].n; i++) {
                float t;
                cap_name(nm, g->cat->fam[f].s[i].name, n);
                if ((t = head_w(g, HEAD_SZ, nm)) > w) { w = t; g->snd_wf = f; g->snd_wi = i; }
            }
        g->snd_s = g->S;
    }
    cap_name(nm, snd_name(g, g->snd_wf, g->snd_wi), n);
}
static void draw_place_head(txg_t *g, frui_canvas *c)
{
    int k = g->sel, fit = 1;
    const txp_ui_lane_t *l = &g->sn.ui.lane[k];
    float x = vtab_end(g) + 18.f, y = g->ct + 8.f, w, room = g->PW - x - 40.f - RW_CHG - 30.f, sz;
    char t[48], nm[32];
    snprintf(t, sizeof t, "LANE %d%s%s", k + 1, used(g, k) ? "  \xC2\xB7  " : "", used(g, k) ? l->fam : "");
    upper(t, t, sizeof t);
    fit &= x + LBL(g, c, x, y + 10.f, 10.f, C_RED, t) <= g->PW - 8.f;
    snd_widest(g, nm, sizeof nm);        /* (the widest name a lane can show fits at the smallest size) */
    fit &= head_w(g, HEAD_MIN, nm) <= room;
    clipL(g, c, x, y, room, 40.f);
    if (used(g, k)) {
        cap_name(nm, l->snd, sizeof nm);
        sz = head_sz(g, nm, room);
        w = TX(g, c, x, y + 29.f, sz, 650.f, 0.2f, 0, (g->mute >> k & 1u) ? C_DIM : C_INK, nm);
    } else w = TX(g, c, x, y + 29.f, 15.f, 600.f, 0.2f, 0, C_FAINT, "Empty lane");
    frui_clip_reset(c);
    fit &= w <= room;
    g->head_fit = fit;
    if (w > room) w = room;
    chg_button(g, c, TXG_W_SND, k, x + w + 10.f, y + 14.f, RW_CHG, 22.f, 0);
    w += 10.f + RW_CHG;
    if (used(g, k)) {
        float xx = x + w + 18.f;
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
static void draw_ms_view(txg_t *g, frui_canvas *c);
static void draw_place(txg_t *g, frui_canvas *c)
{
    R(g, c, 0, g->ct, g->PW, g->dr_y - g->ct, C_WELL);
    R(g, c, g->PW - 1.f, g->ct, 1.f, g->dr_y - g->ct, C_LINE);
    wid_add(g, TXG_W_PLACE, -1, 0, g->ct, g->PW, g->dr_y - g->ct);   /* (the tests: the place, the map's half circle) */
    wid_add(g, TXG_W_MAPR, -1, g->mcx - g->mr, g->mcy - g->mr, 2.f * g->mr, g->mr);
    draw_view_switch(g, c);
    if (g->view == 2) { draw_ms_view(g, c); return; }   /* (M/S: the mix's stereo picture, no lane at its head) */
    draw_place_head(g, c);
    if (g->view == 1) draw_space(g, c); else draw_map_field(g, c);
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
    int n = act ? 9 : 6, v = act ? g->act[k] : g->cnt[k], live, m = pmod(g, id, &live), T = g->msrc ? ctl_tgt(g, id, 0) : 0,
        amt = T ? link_amt(g, g->msrc, id) : 0, hi = txp_ui_hi(id), e0 = 0, e1 = 0;
    float bw = (RW_STEP - 2.f * (float)(n - 1)) / (float)n;
    char t[16], lab[24];
    lk_t lk[MX_N];
    int nl = g->msrc ? 0 : ctl_links(g, id, lk);
    if (T) amt_text(t, sizeof t, amt); else ptext(id, v, t, sizeof t);
    if (T && amt) {                      /* (in the mode: the blocks the source's reach covers, red) */
        int e = (int)lroundf(((float)v / (float)(hi > 0 ? hi : 1) + (float)amt / 100.f) * (float)n);
        e = e < 0 ? 0 : e > n ? n : e;
        e0 = e < v ? e : v; e1 = e < v ? v : e;
        RR(g, c, x - 2.f, yc - 14.f, RW_STEP + 4.f, 28.f, 2.f, C_REDBG);
        RRS(g, c, x - 2.f, yc - 14.f, RW_STEP + 4.f, 28.f, 2.f, C_RED);
    }
    snprintf(lab, sizeof lab, "%s", act ? "ACTIVE" : "MANY");
    LBL(g, c, x, yc - 4.f, 9.f, (m || nl) && !g->msrc ? C_RED : C_DIM, lab);
    TX(g, c, x + RW_STEP - TW(g, 10.5f, T ? 700.f : 500.f, 0.f, FRUI_TNUM, t), yc - 4.f, 10.5f, T ? 700.f : 500.f, 0.f, FRUI_TNUM,
       T ? (amt ? C_RED : C_FAINT) : v ? ink : C_FAINT, t);
    {   /* (stopped by STOP ALL: the value PLAY ALL brings back, as outlined blocks and its text, dim) */
        const txp_ui_lane_t *l = &g->sn.ui.lane[k];
        int mem = !v && !l->run && !T ? (act ? l->mact : l->mcnt) : 0;
        if (mem) {
            char mt[16];
            ptext(id, mem, mt, sizeof mt);
            TX(g, c, x + RW_STEP - TW(g, 10.5f, 500.f, 0.f, FRUI_TNUM, t) - 6.f - TW(g, 10.5f, 500.f, 0.f, FRUI_TNUM, mt), yc - 4.f, 10.5f,
               500.f, 0.f, FRUI_TNUM, C_DIM, mt);
        }
        for (int i = 0; i < n; i++) {
            float bx = x + (bw + 2.f) * (float)i;
            uint32_t bc = i < v ? (m ? C_RED : sel ? C_RED : ink) : C_FAINT;
            if (T) bc = i >= e0 && i < e1 ? C_RED : i < v ? C_GREY : C_FAINT;
            R(g, c, bx, yc + 3.f, bw, 6.f, bc);
            if (i < mem) RRS(g, c, bx, yc + 3.f, bw, 6.f, 1.f, C_DIM);
        }
    }
    wid_add2(g, act ? TXG_W_ACT : TXG_W_MANY, k, id, 0, x - 2.f, yc - 14.f, RW_STEP + 4.f, 28.f);
    wid_track(g, x, RW_STEP);
    if (nl) (void)tags_draw(g, c, id, x, yc + 18.5f, RW_STEP + 4.f, 8.f);
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
    const float x0 = g->PW, y0 = g->ct + HDR_H, x1 = g->W;
    char t[48], nm[32];
    rows_clamp(g);
    {   /* the columns' names */
        float yb = g->ct + HDR_H * 0.5f + CAP(g, 10.f) * 0.5f;
        LBL(g, c, rc.num, yb, 10.f, C_DIM, "#");
        LBL(g, c, rc.name, yb, 10.f, C_DIM, "ENGINE");
        LBL(g, c, rc.tl, yb, 10.f, C_DIM, "EVENTS");
        if (rc.tlw >= TW(g, 10.f, 620.f, 1.4f, 0, "EVENTS") + 2.f * TW(g, 10.f, 620.f, 1.4f, 0, "6 S") + 8.f) LBL(g, c, rc.tl + rc.tlw - TW(g, 10.f, 620.f, 1.4f, 0, "6 S"), yb, 10.f, C_FAINT, "6 S");
        LBL(g, c, rc.many, yb, 10.f, C_DIM, "HOW MANY");   /* (STOP / PLAY: no header) */
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
        if (sel) { R(g, c, x0, y, x1 - x0, ROW_H, C_SEL); R(g, c, x0, y, 3.f, ROW_H, C_RED); }
        R(g, c, x0, y + ROW_H - 1.f, x1 - x0, 1.f, C_LINE);
        wid_add(g, TXG_W_ROW, k, x0, y, x1 - x0, ROW_H);
        snprintf(t, sizeof t, "%d", k + 1);
        TX(g, c, rc.num, yc + CAP(g, 11.f) * 0.5f, 11.f, sel ? 650.f : 500.f, 0.f, FRUI_TNUM, sel ? C_RED : C_DIM, t);
        wid_add(g, TXG_W_SELDOT, k, x0 + 4.f, yc - 13.f, rc.dot + 7.f - x0 - 4.f, 26.f);   /* (the number and the dot;
                                          * the rest of the row selects too: TXG_W_ROW) */
        if (sel) CI(g, c, rc.dot, yc, 4.5f, C_RED);   /* (the selected lane's dot: solid red) */
        {   /* the engine's name and family (cut with "..." before the icons), then ENGINE and the dice in one column at the
             * right end (the same x on every row; the row itself: a click selects the lane) */
            float ix = rc.tl - 14.f - RW_CHG - 4.f - RW_CHG, tw = ix - 6.f - rc.name;
            if (u) {
                float a = glow(g, k);
                if (!sel) CI(g, c, rc.dot, yc, 3.5f, frui_alpha(lane_col(g, k), (0.35f + 0.65f * a) * mf));
                cap_name(nm, l->snd, sizeof nm);
                ell(g, t, sizeof t, nm, tw, 12.5f, 600.f, 0.f);
                TX(g, c, rc.name, yc - 1.f, 12.5f, 600.f, 0.f, 0, ink, t);
                ell(g, t, sizeof t, l->fam, tw, 10.f, 450.f, 0.3f);
                TX(g, c, rc.name, yc + 12.f, 10.f, 450.f, 0.3f, 0, C_DIM, t);
            } else TX(g, c, rc.name, yc + CAP(g, 12.f) * 0.5f, 12.f, 450.f, 0.3f, 0, C_FAINT, "empty");
            chg_button(g, c, TXG_W_SND, k, ix, yc - 11.f, RW_CHG, 22.f, 0);
            dice_button(g, c, k, ix + RW_CHG + 4.f, yc - 11.f, RW_CHG, 22.f, u);
        }
        draw_line(g, c, k, rc.tl, yc - th * 0.5f, rc.tlw, th, mf);
        if (stopped) {                   /* (a stopped lane: plain to see; HOW MANY or HOW ACTIVE starts it) */
            const char *s = l->mcnt ? "STOPPED  \xC2\xB7  PLAY ALL BRINGS IT BACK" : "STOPPED  \xC2\xB7  SET HOW MANY TO PLAY";
            if (TW(g, 9.5f, 620.f, 1.3f, 0, s) > rc.tlw - 16.f) s = "STOPPED";
            LBL(g, c, rc.tl + 8.f, yc + CAP(g, 9.5f) * 0.5f, 9.5f, C_DIM, s);
        }
        if (u) {
            steps(g, c, k, 0, rc.many, yc, sel, ink);
            steps(g, c, k, 1, rc.act, yc, sel, ink);
            {   /* PAN (a drag moves it; red while a source does; in a source's mode: the source's amount) */
                uint32_t id = TXP_ID_LANE(k, TXP_PL_PAN);
                int live, m = pmod(g, id, &live), T = g->msrc ? ctl_tgt(g, id, 0) : 0, amt = T ? link_amt(g, g->msrc, id) : 0;
                lk_t lk[MX_N];
                int nl = g->msrc ? 0 : ctl_links(g, id, lk);
                char a[16];
                if (T) {
                    amt_text(a, sizeof a, amt);
                    if (amt) { RR(g, c, rc.pan - 4.f, yc - 12.f, RW_PAN + 4.f, 24.f, 2.f, C_REDBG); RRS(g, c, rc.pan - 4.f, yc - 12.f, RW_PAN + 4.f, 24.f, 2.f, C_RED); }
                    TX(g, c, rc.pan, yc + CAP(g, 12.f) * 0.5f, 12.f, 700.f, 0.f, FRUI_TNUM, amt ? C_RED : C_FAINT, a);
                } else {
                    ptext(id, m && live >= 0 ? live : g->pan[k], a, sizeof a);
                    if (m) { snprintf(t, sizeof t, "~%s", a); snprintf(a, sizeof a, "%s", t); }
                    TX(g, c, rc.pan, yc + CAP(g, 12.f) * 0.5f, 12.f, 500.f, 0.f, FRUI_TNUM, m || nl ? C_RED : ink, a);
                }
                wid_add2(g, TXG_W_PARAM, (int)id, id, 0, rc.pan - 4.f, yc - 12.f, RW_PAN + 4.f, 24.f);
                if (nl) (void)tags_draw(g, c, id, rc.pan, yc + 17.f, RW_PAN + R_GAP + RW_CH, 8.f);   /* (under CH too) */
            }
        }
        {   /* the channel (a click: the next; the lane's own, SETTINGS: MIDI IN too) */
            uint32_t id = TXP_ID_LCH(k);
            if (l->ch) snprintf(t, sizeof t, "%d", l->ch); else snprintf(t, sizeof t, "OFF");
            TX(g, c, rc.ch, yc + CAP(g, 12.f) * 0.5f, 12.f, 500.f, 0.f, FRUI_TNUM, u ? C_INK : C_FAINT, t);
            wid_add2(g, TXG_W_PARAM, (int)id, id, 0, rc.ch - 4.f, yc - 12.f, RW_CH + 4.f, 24.f);
        }
        if (u) {   /* VOL (red while a source moves it: its reach, its live tick, its tags; in a source's mode: the
                    * source's amount and reach) */
            uint32_t id = TXP_ID_LANE(k, TXP_PL_LEVEL);
            float v = (float)g->level[k] / 127.f, ty = yc + 3.f;
            int T = g->msrc ? ctl_tgt(g, id, 0) : 0, amt = T ? link_amt(g, g->msrc, id) : 0, mod = (l->modm & TXP_MOD_LEVEL) != 0;
            lk_t lk[MX_N];
            int nl = g->msrc ? 0 : ctl_links(g, id, lk);
            if (T) {
                amt_text(t, sizeof t, amt);
                if (amt) { RR(g, c, rc.vol - 4.f, yc - 14.f, RW_VOL + 8.f, 28.f, 2.f, C_REDBG); RRS(g, c, rc.vol - 4.f, yc - 14.f, RW_VOL + 8.f, 28.f, 2.f, C_RED); }
                TX(g, c, rc.vol + RW_VOL - TW(g, 11.f, 700.f, 0.f, FRUI_TNUM, t), yc - 5.f, 11.f, 700.f, 0.f, FRUI_TNUM, amt ? C_RED : C_FAINT, t);
                fader_mode(g, c, rc.vol, ty + 2.f, RW_VOL, v, 0.f, (float)amt / 100.f, 4.f, 8.f, 12.f);
            } else {
                snprintf(t, sizeof t, "%d", g->level[k]);
                TX(g, c, rc.vol + RW_VOL - TW(g, 11.f, 500.f, 0.f, FRUI_TNUM, t), yc - 5.f, 11.f, 500.f, 0.f, FRUI_TNUM, mod || nl ? C_RED : ink, t);
                if (nl) {                /* (its reach, behind the track) */
                    float lo, hi;
                    ctl_reach(lk, nl, v, &lo, &hi);
                    if (hi > lo) RR(g, c, rc.vol + RW_VOL * lo, ty - 1.5f, RW_VOL * (hi - lo), 7.f, 3.5f, frui_alpha(C_RED, 0.35f));
                }
                RR(g, c, rc.vol, ty, RW_VOL, 4.f, 2.f, C_FAINT);
                RR(g, c, rc.vol, ty, RW_VOL * v, 4.f, 2.f, mf < 1.f ? C_DIM : C_INK);
                RR(g, c, rc.vol + RW_VOL * v - 4.f, ty - 4.f, 8.f, 12.f, 2.f, mf < 1.f ? C_DIM : C_INK);
                if (nl) RR(g, c, rc.vol + RW_VOL * ctl_live(g, lk, nl, v) - 1.5f, ty - 3.f, 3.f, 10.f, 1.5f, C_RED);
            }
            wid_add2(g, TXG_W_VOL, k, id, 0, rc.vol - 4.f, yc - 14.f, RW_VOL + 8.f, 28.f);
            wid_track(g, rc.vol, RW_VOL);
            if (nl) (void)tags_draw(g, c, id, rc.vol, yc + 18.5f, RW_VOL + 8.f, 8.f);
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
        run_button(g, c, TXG_W_LRUN, k, rc.run, yc - 11.f, 22.f, 22.f, 0);
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

/* the drawer's tag: SOUND in red, then the lane and its family dim (the mockup's tracking); the rest a size down when
 * it would pass room (a long family past lane 9). Its width; c 0: measured only */
static float snd_tag(txg_t *g, frui_canvas *c, float x, float y, const char *rest, float room)
{
    float w0 = TW(g, 10.5f, 620.f, 1.68f, 0, "SOUND") + 8.f, fs = 10.5f;
    if (w0 + TW(g, fs, 620.f, fs * 0.12f, 0, rest) > room) fs = 10.f;
    if (c) {
        TX(g, c, x, y, 10.5f, 620.f, 1.68f, 0, C_RED, "SOUND");
        TX(g, c, x + w0, y, fs, 620.f, fs * 0.12f, 0, C_DIM, rest);
    }
    return w0 + TW(g, fs, 620.f, fs * 0.12f, 0, rest);
}
/* ---- the drawer, the SOUND zone: the selected lane on a warmer, lighter ground with a red rail at its left; its tag
 * (SOUND, the lane, the family), its cards (name, value, what the card does; DIST among them), PAN, LEVEL, GROOVE. Every
 * text measured in its room (zone_fit) */
static void draw_drawer(txg_t *g, frui_canvas *c)
{
    const float y0 = g->dr_y, x1 = g->W;
    int k = g->sel;
    const txp_ui_lane_t *l = &g->sn.ui.lane[k];
    const txp_ui_cards_t *cd = &g->sn.ui.card[k];
    char t[64], nm[32], a[16], b[16];
    float x = 16.f, cx0 = 236.f, cx1 = x1 - 196.f, room = cx0 - x - 16.f, ns = 22.f;
    R(g, c, 0, y0, x1, DR_H, C_SND);
    R(g, c, 0, y0, x1, 1.f, C_SNDLINE);
    R(g, c, 0, y0, 4.f, DR_H, C_RED);
    g->zone = 1;
    snprintf(t, sizeof t, "\xC2\xB7 LANE %d%s%s", k + 1, used(g, k) ? " \xC2\xB7 " : "", used(g, k) ? l->fam : "");
    upper(t, t, sizeof t);
    ZFIT(snd_tag(g, c, x, y0 + 21.f, t, room + 8.f) <= room + 8.f);   /* (the tag: to 8 px short of the cards) */
    if (!used(g, k)) {
        static const char *const NO = "No engine on this lane: CHOOSE ENGINE (or the row's ENGINE button) picks one.";
        TX(g, c, x, y0 + 48.f, 22.f, 700.f, 1.1f, 0, C_FAINT, "EMPTY");
        chg_button(g, c, TXG_W_SND, k, x, y0 + 116.f, 156.f, 26.f, "CHOOSE ENGINE");
        TX(g, c, cx0, y0 + 40.f, 12.5f, 450.f, 0.f, 0, C_DIM, NO);
        ZFIT(TW(g, 12.5f, 450.f, 0.f, 0, NO) <= x1 - 16.f - cx0);
        g->zone = 0;
        return;
    }
    if (g->names_s != g->S) {            /* (every sound of the catalog, once a scale: its name at the smallest size, its
                                          * tag with the widest lane number) */
        g->names_fit = 1;
        for (int f = 0; f < g->cat->nfam; f++) {
            char u[48];
            snprintf(u, sizeof u, "\xC2\xB7 LANE 16 \xC2\xB7 %s", g->cat->fam[f].name);
            upper(u, u, sizeof u);
            g->names_fit &= snd_tag(g, 0, 0.f, 0.f, u, room + 8.f) <= room + 8.f;
            for (int i = 0; i < g->cat->fam[f].n; i++) {
                upper(u, g->cat->fam[f].s[i].name, sizeof u);
                g->names_fit &= TW(g, 15.f, 700.f, 0.75f, 0, u) <= room;
            }
        }
        g->names_s = g->S;
    }
    ZFIT(g->names_fit);
    upper(nm, l->snd, sizeof nm);
    while (ns > 15.f && TW(g, ns, 700.f, ns * 0.05f, 0, nm) > room) ns -= 1.f;   /* (a long name: a size down, never cut) */
    ZFIT(TW(g, ns, 700.f, ns * 0.05f, 0, nm) <= room);
    clipL(g, c, x, y0, room, DR_H);
    TX(g, c, x, y0 + 48.f, ns, 700.f, ns * 0.05f, 0, (g->mute >> k & 1u) ? C_DIM : C_INK, nm);
    frui_clip_reset(c);
    chg_button(g, c, TXG_W_SND, k, x, y0 + 116.f, 122.f, 26.f, "CHANGE ENGINE");   /* (its icon, label and caret inside it) */
    run_button(g, c, TXG_W_DRUN, k, x + 126.f, y0 + 116.f, 88.f, 26.f, l->run ? "STOP LANE" : "PLAY LANE");
    if (l->eng < g->cat->neng) {
        char en[24];
        upper(en, g->cat->eng[l->eng], sizeof en);
        snprintf(t, sizeof t, "%s COLOUR", en);
        CI(g, c, x + 5.f, y0 + 66.f, 4.5f, lane_col(g, k));
        RI(g, c, x + 5.f, y0 + 66.f, 7.f, 1.f, g->pick >= 0 && g->pick == l->eng ? C_INK : C_FAINT);
        clipL(g, c, x, y0 + 56.f, room, 20.f);
        LBL(g, c, x + 17.f, y0 + 66.f + CAP(g, 9.5f) * 0.5f, 9.5f, C_DIM, t);
        frui_clip_reset(c);
        ZFIT(17.f + LBLW(g, 9.5f, t) <= room);
        wid_add2(g, TXG_W_COLDOT, k, (uint32_t)l->eng, 0, x - 4.f, y0 + 56.f, 24.f + LBLW(g, 9.5f, t), 20.f);
    }
    ptext(TXP_ID_LANE(k, TXP_PL_COUNT), g->cnt[k], a, sizeof a);
    ptext(TXP_ID_LANE(k, TXP_PL_ACTIVE), g->act[k], b, sizeof b);
    if (!l->run && l->mcnt) {            /* (stopped by STOP ALL: what PLAY ALL brings back) */
        ptext(TXP_ID_LANE(k, TXP_PL_COUNT), l->mcnt, a, sizeof a);
        ptext(TXP_ID_LANE(k, TXP_PL_ACTIVE), l->mact, b, sizeof b);
        snprintf(t, sizeof t, "PLAY ALL: MANY %s  \xC2\xB7  ACTIVE %s  \xC2\xB7  VOL %d", a, b, g->level[k]);
    } else snprintf(t, sizeof t, "MANY %s  \xC2\xB7  ACTIVE %s  \xC2\xB7  VOL %d", a, b, g->level[k]);
    TX(g, c, x, y0 + 90.f, 10.5f, 450.f, 0.2f, FRUI_TNUM, l->run ? C_DIM : 0xFF625B57u, t);   /* (stopped: dimmer, still legible on the warm ground) */
    ZFIT(TW(g, 10.5f, 450.f, 0.2f, FRUI_TNUM, t) <= room);
    if (l->ch) snprintf(t, sizeof t, "MIDI CH %d%s%s", l->ch, l->run ? "" : "  \xC2\xB7  STOPPED", (g->mute >> k & 1u) ? "  \xC2\xB7  MUTED" : "");
    else snprintf(t, sizeof t, "MIDI OFF%s%s", l->run ? "" : "  \xC2\xB7  STOPPED", (g->mute >> k & 1u) ? "  \xC2\xB7  MUTED" : "");
    TX(g, c, x, y0 + 106.f, 10.5f, 450.f, 0.2f, FRUI_TNUM, C_DIM, t);
    ZFIT(TW(g, 10.5f, 450.f, 0.2f, FRUI_TNUM, t) <= room);
    /* the cards: four a row */
    {
        float gap = 6.f, cw = (cx1 - cx0 - 3.f * gap) / 4.f, ch = (DR_H - 24.f - gap) / 2.f;
        if (!cd->n) TX(g, c, cx0, y0 + 40.f, 12.5f, 450.f, 0.f, 0, C_DIM, "This engine has no cards.");
        for (int i = 0; i < cd->n && i < (int)TXP_CARD_N; i++) {
            float xx = cx0 + (cw + gap) * (float)(i % 4), yy = y0 + 12.f + (ch + gap) * (float)(i / 4);
            pcell(g, c, xx, yy, cw, ch, TXP_ID_LANE(k, TXP_PL_CARD0 + i), cd->lab[i], card_hint(l->snd, cd->lab[i]));
        }
    }
    /* the lane's own: PAN, DIST, LEVEL, GROOVE's PART and CYCLE */
    {
        float xx = x1 - 182.f, yy = y0 + 12.f, w = 166.f, h = 24.f;   /* (DIST: a card, there with its hint) */
        pcell(g, c, xx, yy, w, h, TXP_ID_LANE(k, TXP_PL_PAN), "PAN", 0); yy += 32.f;
        pcell(g, c, xx, yy, w, h, TXP_ID_LANE(k, TXP_PL_LEVEL), "LEVEL", 0); yy += 32.f;
        pcell(g, c, xx, yy, w, h, TXP_ID_LANE(k, 48), "GROOVE PART", 0); yy += 32.f;
        pcell(g, c, xx, yy, w, h, TXP_ID_LANE(k, 49), "CYCLE", 0);
    }
    g->zone = 0;
}

/* ---- the strip: COMP, DRIVE, FILTER, DELAY, REVERB, M/S (the mix's chain, in its order), the movement */
/* COMP's meters, off the app's compressor (txg_tick's ballistics: up fast, down ~24 dB/s, the input's peak held a
 * second): its input (the detector's level), its output (through its gain), the gain reduction (from the right), on a
 * dB scale; THR marked on the levels. Big (the effect's drawer): taller (its curve beside it: comp_curve) */
#define CM_LO (-60.f)
static float cm_x(float db) { float f = (db - CM_LO) / -CM_LO; return f < 0.f ? 0.f : f > 1.f ? 1.f : f; }
static void comp_meter(txg_t *g, frui_canvas *c, float x, float y, float w, float h, int on)
{
    static const char *const LAB[3] = { "IN", "OUT", "GR" };
    static const int SC[5] = { -60, -40, -20, -10, 0 };
    int big = h >= 70.f, low = h < 30.f;   /* (low: the strip's, under its band) */
    float rh = big ? 12.f : low ? 5.f : 7.f, gap = big ? 12.f : low ? 3.5f : 4.f, lw = big ? 30.f : 24.f, nw = big ? 52.f : 34.f,
          fs = big ? 10.f : low ? 7.5f : 8.5f,
          bx = x + lw, bw = w - lw - nw, thr = -40.f * (float)txp_ui_get(g->c, ID_CTHR) / 127.f, yy;
    float val[3] = { g->cm_in, g->cm_out, g->cm_gr };
    wid_add(g, TXG_W_CMETER, -1, x, y, w, 3.f * rh + 2.f * gap);
    for (int i = 0; i < 3; i++) {
        char t[16];
        uint32_t ink = on ? C_INK : C_FAINT;
        yy = y + (rh + gap) * (float)i;
        LBL(g, c, x, yy + rh * 0.5f + CAP(g, fs) * 0.5f, fs, on ? C_DIM : C_FAINT, LAB[i]);
        RR(g, c, bx, yy, bw, rh, 1.5f, C_WELL);
        for (int j = 1; j < 4; j++) R(g, c, bx + bw * cm_x((float)SC[j]), yy, 1.f, rh, C_LINE);   /* (the scale's marks) */
        if (i < 2) {
            float f = on ? cm_x(val[i]) : 0.f;
            if (f > 0.f) RR(g, c, bx, yy, bw * f, rh, 1.5f, frui_alpha(ink, i ? 0.85f : 0.6f));
            if (on && i == 0 && g->cm_pk > CM_LO) R(g, c, bx + bw * cm_x(g->cm_pk) - 1.f, yy, 2.f, rh, C_INK);   /* (the peak held) */
            R(g, c, bx + bw * cm_x(thr) - 0.5f, yy - 2.f, 1.f, rh + 4.f, on ? C_DIM : C_FAINT);   /* (THR) */
            if (!on) snprintf(t, sizeof t, "OFF");
            else if (val[i] <= -89.f) snprintf(t, sizeof t, "-inf");
            else snprintf(t, sizeof t, "%.1f", val[i]);
        } else {                           /* the gain reduction: from the right, 24 dB the whole width */
            float f = on ? val[2] / 24.f : 0.f;
            f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
            if (f > 0.f) RR(g, c, bx + bw * (1.f - f), yy, bw * f, rh, 1.5f, ink);
            if (!on) snprintf(t, sizeof t, "OFF"); else snprintf(t, sizeof t, "-%.1f", val[2] < 0.f ? 0.f : val[2]);
        }
        TX(g, c, x + w - TW(g, fs + 1.f, 500.f, 0.f, FRUI_TNUM, t), yy + rh * 0.5f + CAP(g, fs + 1.f) * 0.5f, fs + 1.f, 500.f, 0.f,
           FRUI_TNUM, on ? C_INK : C_FAINT, t);
    }
    yy = y + 3.f * rh + 2.f * gap + (big ? 14.f : 9.f);   /* the scale: dBFS (the gain reduction: 0 .. 24 dB); low: its
                                          * marks only */
    for (int j = 0; j < 5 && !low; j++) {
        char t[8];
        float tx;
        if (j == 3 && !big) continue;    /* (-10: its mark only, beside 0) */
        snprintf(t, sizeof t, "%d", SC[j]);
        tx = bx + bw * cm_x((float)SC[j]) - TW(g, big ? 9.f : 7.5f, 450.f, 0.f, FRUI_TNUM, t) * (j == 0 ? 0.f : j == 4 ? 1.f : 0.5f);
        TX(g, c, tx, yy, big ? 9.f : 7.5f, 450.f, 0.f, FRUI_TNUM, C_FAINT, t);
    }
}
/* COMP's curve (out over in, -60 .. 0 dBFS; THR and RATIO the app's), the input on it now (the effect's drawer) */
static void comp_curve(txg_t *g, frui_canvas *c, float cx, float cy0, float cw, float ch, int on)
{
    float thr = -40.f * (float)txp_ui_get(g->c, ID_CTHR) / 127.f, px = 0.f, py = 0.f, ratio = 2.f;
    int ri = txp_param_index(ID_CRATIO);
    if (ri >= 0 && g->sn.ptx[ri][0]) ratio = (float)atof(g->sn.ptx[ri]);
    if (ratio < 1.f) ratio = 1.f;
    RR(g, c, cx, cy0, cw, ch, 2.f, C_WELL);
    RRS(g, c, cx, cy0, cw, ch, 2.f, C_LINE);
    LN(g, c, cx, cy0 + ch, cx + cw, cy0, lw1(g), C_LINE);
    dashed(g, c, cx + cw * cm_x(thr), cy0 + 2.f, cy0 + ch - 2.f, C_FAINT);
    for (int i = 0; i <= 40; i++) {
        float din = CM_LO + -CM_LO * (float)i / 40.f, dout = din > thr ? thr + (din - thr) / ratio : din,
              xx = cx + cw * cm_x(din), yy2 = cy0 + ch - ch * cm_x(dout);
        if (i) LN(g, c, px, py, xx, yy2, g->S < 1.5f ? 1.4f / g->S : 1.2f, on ? C_INK : C_FAINT);
        px = xx; py = yy2;
    }
    if (on && g->cm_in > CM_LO) {
        float din = g->cm_in, dout = din > thr ? thr + (din - thr) / ratio : din;
        CI(g, c, cx + cw * cm_x(din), cy0 + ch - ch * cm_x(dout), 3.5f, C_INK);
    }
    TX(g, c, cx + 5.f, cy0 + 11.f, 8.5f, 450.f, 0.f, 0, C_FAINT, "OUT");
    TX(g, c, cx + cw - 5.f - TW(g, 8.5f, 450.f, 0.f, 0, "IN"), cy0 + ch - 5.f, 8.5f, 450.f, 0.f, 0, C_FAINT, "IN");
}
/* ---- the M/S's pictures (the mix as it leaves the M/S, live): its meters (L R M S, their peaks on -48 .. 0 dBFS,
 * the sides' in red; txg_tick's ballistics), its goniometer (each point a frame of the output: up the middle, across
 * the sides, so tall is centred and wide is spread; the last GON_H blocks, the older fading; scaled to the level, the
 * cloud's edge near the circle) and its correlation (-1 the sides cancel .. 0 wide .. +1 mono) */
#define MSM_LO (-48.f)
static void ms_meters(txg_t *g, frui_canvas *c, float x, float y, float bw, float gap, float sp, float h, float fs, uint32_t lc)
{
    static const char *const N[4] = { "L", "R", "M", "S" };
    wid_add(g, TXG_W_MSM, 0, x, y, 4.f * bw + 3.f * gap + sp, h + fs + 4.f);
    for (int i = 0; i < 4; i++) {
        float bx = x + (bw + gap) * (float)i + (i >= 2 ? sp : 0.f), f = (g->ms_db[i] - MSM_LO) / -MSM_LO;
        f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
        RR(g, c, bx, y, bw, h, 2.f, C_FAINT);
        if (f * h >= 0.5f) {             /* (from the bottom, the corners square where it is cut: as the mockup's) */
            clipL(g, c, bx, y + h * (1.f - f), bw, h * f);
            RR(g, c, bx, y, bw, h, 2.f, i == 3 ? C_RED : C_INK);
            frui_clip_reset(c);
        }
        TX(g, c, bx + (bw - TW(g, fs, 600.f, fs * 0.1f, 0, N[i])) * 0.5f, y + h + 4.f + CAP(g, fs), fs, 600.f, fs * 0.1f, 0, lc, N[i]);
    }
}
static void ms_gonio(txg_t *g, frui_canvas *c, float x, float y, float sz)
{
    static const uint32_t GRID = 0xFF2A2827u;
    float cx = x + sz * 0.5f, cy = y + sz * 0.5f, r = sz * 0.46f, d = r * 0.7f, lw = lw1(g), k = g->gon_k * r;
    wid_add(g, TXG_W_MSM, 1, x, y, sz, sz);
    LN(g, c, cx, cy - r, cx, cy + r, lw, GRID); LN(g, c, cx - r, cy, cx + r, cy, lw, GRID);
    LN(g, c, cx - d, cy - d, cx + d, cy + d, lw, GRID); LN(g, c, cx + d, cy - d, cx - d, cy + d, lw, GRID);
    TX(g, c, cx - TW(g, 10.f, 600.f, 0.f, 0, "M") * 0.5f, cy - r + 11.f, 10.f, 600.f, 0.f, 0, C_FAINT, "M");
    TX(g, c, cx - d * 0.95f - TW(g, 10.f, 600.f, 0.f, 0, "L") * 0.5f, cy - d * 0.95f + 10.f, 10.f, 600.f, 0.f, 0, C_FAINT, "L");
    TX(g, c, cx + d * 0.95f - TW(g, 10.f, 600.f, 0.f, 0, "R") * 0.5f, cy - d * 0.95f + 10.f, 10.f, 600.f, 0.f, 0, C_FAINT, "R");
    TX(g, c, cx + r - 7.f - TW(g, 10.f, 600.f, 0.f, 0, "S") * 0.5f, cy + 3.5f, 10.f, 600.f, 0.f, 0, C_FAINT, "S");
    clipL(g, c, x, y, sz, sz);
    for (int a = g->gon_n - 1; a >= 0; a--) {   /* (the oldest first, fading: the newest over them) */
        int b = (g->gon_at - a + GON_H) % GON_H;
        float al = 1.f - (float)a / (float)GON_H, ps = g->S < 1.5f ? 1.5f : 1.25f;
        for (int i = 0; i < (int)TXP_GON_N; i++) {
            float l = (float)g->gon[b][i][0] / 32768.f, rr = (float)g->gon[b][i][1] / 32768.f, px = cx + (l - rr) * k, py = cy - (l + rr) * k;
            R(g, c, px - ps * 0.5f, py - ps * 0.5f, ps, ps, i % 9 == 0 ? frui_alpha(C_RED, 0.75f * al) : frui_alpha(C_INK, 0.55f * al));
        }
    }
    frui_clip_reset(c);
}
static void ms_corr(txg_t *g, frui_canvas *c, float x, float y, float w)
{
    static const uint32_t A = 0xFF5A2420u, B = 0xFF34312Fu, C2 = 0xFF3A3734u;   /* (the bar: red-brown where the sides cancel) */
    float f = (g->ms_c + 1.f) * 0.5f, by = y + 16.f;
    LBL(g, c, x, y + 9.f, 9.5f, C_DIM, "CORRELATION");
    clipL(g, c, x, by, w, 8.f);
    for (int i = 0; i < 48; i++) {       /* (its colour across: A to B by the middle, B to C2 after) */
        float t = (float)i / 47.f, u = t < 0.5f ? t * 2.f : (t - 0.5f) * 2.f;
        uint32_t p = t < 0.5f ? A : B, q = t < 0.5f ? B : C2, col = 0xFF000000u;
        for (int sh = 0; sh < 24; sh += 8) {
            float a = (float)(p >> sh & 255u), b = (float)(q >> sh & 255u);
            col |= (uint32_t)lroundf(a + (b - a) * u) << sh;
        }
        R(g, c, x + w * (float)i / 48.f, by, w / 48.f + 0.5f, 8.f, col);
    }
    frui_clip_reset(c);
    RRS(g, c, x, by, w, 8.f, 4.f, C_DRAW);   /* (its ends round) */
    f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
    RR(g, c, x + w * f - 2.f, by - 4.f, 4.f, 16.f, 2.f, C_INK);
    TX(g, c, x, by + 22.f, 9.f, 450.f, 0.f, 0, C_FAINT, "\xE2\x88\x92" "1 out of phase");
    TX(g, c, x + (w - TW(g, 9.f, 450.f, 0.f, 0, "0 wide")) * 0.5f, by + 22.f, 9.f, 450.f, 0.f, 0, C_FAINT, "0 wide");
    TX(g, c, x + w - TW(g, 9.f, 450.f, 0.f, 0, "+1 mono"), by + 22.f, 9.f, 450.f, 0.f, 0, C_FAINT, "+1 mono");
}
/* the place's M/S tab: the mix's stereo picture large, as it leaves the M/S (its drawer's feed: txg_tick). A half
 * circle: up the middle (M), L and R at 45 degrees, the sides across (+S at the left, -S at the right); a frame under
 * the middle is folded up (both channels turned over), so what cancels in mono lies along the sides. Under it L R MID
 * SIDE in dB with their bars (-48 .. 0 dBFS, the sides' red), the correlation (-1 .. +1, its middle marked) and WIDTH's
 * value (MONO on: 0 %). It only shows: MID and SIDE are the strip's M/S section's, WIDTH its drawer's */
#define MSV_TOP 67.f                     /* (the tabs and the M over the half circle) */
#define MSV_BOT 125.f                    /* (the levels, the correlation and WIDTH under it, a margin) */
static void draw_ms_view(txg_t *g, frui_canvas *c)
{
    static const char *const N[4] = { "L", "R", "MID", "SIDE" };
    const float top = g->ct, bot = g->dr_y, W = g->PW, mx = 24.f, room = bot - top, lw = lw1(g);
    float rad = fminf((W - 60.f) * 0.5f, room - MSV_TOP - MSV_BOT), cx = W * 0.5f, cy, k, yy, cw, yc, x0, x1, vw, f, lx;
    int fit = 1;
    char t[16];
    if (rad < 40.f) { fit = 0; rad = fmaxf(rad, 20.f); }   /* (an effect's drawer over it at the smallest window: still round) */
    cy = top + MSV_TOP + fmaxf(0.f, (room - MSV_TOP - rad - MSV_BOT) * 0.5f) + rad;
    {   /* the half circle, the middle and the 45 degree lines (L, R), their names */
        float px = cx - rad, py = cy, wl = TW(g, 11.f, 600.f, 0.f, 0, "L"), wr = TW(g, 11.f, 600.f, 0.f, 0, "R");
        for (int i = 1; i <= 72; i++) {
            float a = 3.14159265f * (1.f - (float)i / 72.f), x = cx + cosf(a) * rad, y = cy - sinf(a) * rad;
            LN(g, c, px, py, x, y, lw, C_LINE);
            px = x; py = y;
        }
        for (int a = -45; a <= 45; a += 45) {
            float r = (float)a * 3.14159265f / 180.f;
            LN(g, c, cx, cy, cx + sinf(r) * rad, cy - cosf(r) * rad, lw, C_LINE);
        }
        TX(g, c, cx - TW(g, 11.f, 600.f, 0.f, 0, "M") * 0.5f, cy - rad - 4.f, 11.f, 600.f, 0.f, 0, C_DIM, "M");
        lx = cx - rad * 0.74f - 7.f - wl * 0.5f;
        TX(g, c, lx, cy - rad * 0.74f, 11.f, 600.f, 0.f, 0, C_DIM, "L");
        TX(g, c, cx + rad * 0.74f + 7.f - wr * 0.5f, cy - rad * 0.74f, 11.f, 600.f, 0.f, 0, C_DIM, "R");
        TX(g, c, cx - rad + 5.f - TW(g, 11.f, 600.f, 0.f, 0, "+S") * 0.5f, cy - 6.f, 11.f, 600.f, 0.f, 0, C_DIM, "+S");
        TX(g, c, cx + rad - 5.f - TW(g, 11.f, 600.f, 0.f, 0, "\xE2\x88\x92S") * 0.5f, cy - 6.f, 11.f, 600.f, 0.f, 0, C_DIM, "\xE2\x88\x92S");
        fit &= lx >= 4.f && cy - rad - 4.f - CAP(g, 11.f) >= top + 40.f;   /* (inside the place, under the tabs) */
    }
    /* the points: the last GON_H blocks, the older fading (the drawer's goniometer's colours and scale) */
    k = g->gon_k * rad;
    clipL(g, c, cx - rad, cy - rad - 2.f, 2.f * rad, rad + 2.f);
    for (int a = g->gon_n - 1; a >= 0; a--) {
        int b = (g->gon_at - a + GON_H) % GON_H;
        float al = 1.f - (float)a / (float)GON_H, ps = g->S < 1.5f ? 1.5f : 1.25f;
        for (int i = 0; i < (int)TXP_GON_N; i++) {
            float l = (float)g->gon[b][i][0] / 32768.f, rr = (float)g->gon[b][i][1] / 32768.f, px, py;
            if (l + rr < 0.f) { l = -l; rr = -rr; }
            px = cx - (l - rr) * k; py = cy - (l + rr) * k;
            R(g, c, px - ps * 0.5f, py - ps * 0.5f, ps, ps, i % 9 == 0 ? frui_alpha(C_RED, 0.75f * al) : frui_alpha(C_INK, 0.55f * al));
        }
    }
    frui_clip_reset(c);
    wid_add(g, TXG_W_MSM, 1, cx - rad, cy - rad, 2.f * rad, rad);
    /* the levels: four columns, each its name, its peak in dB, its bar */
    yy = cy + 20.f;
    cw = (W - 2.f * mx - 30.f) / 4.f;
    vw = fmaxf(TW(g, 12.f, 500.f, 0.f, FRUI_TNUM, "-88.8 dB"), TW(g, 12.f, 500.f, 0.f, FRUI_TNUM, "-inf dB"));
    fit &= vw <= cw;
    for (int i = 0; i < 4; i++) {
        float x = mx + (cw + 10.f) * (float)i, db = g->ms_db[i];
        fit &= LBL(g, c, x, yy + CAP(g, 10.5f), 10.5f, C_DIM, N[i]) <= cw;
        if (db <= -89.f) snprintf(t, sizeof t, "-inf dB"); else snprintf(t, sizeof t, "%.1f dB", db);
        TX(g, c, x, yy + 16.6f + CAP(g, 12.f), 12.f, 500.f, 0.f, FRUI_TNUM, C_INK, t);
        RR(g, c, x, yy + 35.f, cw, 4.f, 2.f, C_MSTRK);
        f = (db - MSM_LO) / -MSM_LO;
        f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
        if (f * cw >= 1.f) RR(g, c, x, yy + 35.f, cw * f, 4.f, 2.f, i == 3 ? C_RED : C_INK);
    }
    wid_add(g, TXG_W_MSM, 0, mx, yy, W - 2.f * mx, 39.f);
    /* the correlation: its track (its middle marked), where it is, its value */
    yc = yy + 57.f;
    x0 = mx + LBL(g, c, mx, yc + CAP(g, 10.5f) * 0.5f, 10.5f, C_DIM, "CORRELATION") + 10.f;
    x1 = W - mx - 40.f - 10.f;
    fit &= x1 - x0 >= 60.f;
    RR(g, c, x0, yc - 2.f, x1 - x0, 4.f, 2.f, C_MSTRK);
    R(g, c, x0 + (x1 - x0) * 0.5f - 0.5f, yc - 6.f, 1.f, 12.f, C_DIM);
    f = (g->ms_c + 1.f) * 0.5f;
    f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
    RR(g, c, x0 + (x1 - x0) * f - 3.f, yc - 7.f, 6.f, 14.f, 2.f, C_INK);
    snprintf(t, sizeof t, "%+.2f", fabsf(g->ms_c) < 0.005f ? 0.0 : (double)g->ms_c);
    TX(g, c, W - mx - TW(g, 12.f, 500.f, 0.f, FRUI_TNUM, t), yc + CAP(g, 12.f) * 0.5f, 12.f, 500.f, 0.f, FRUI_TNUM, C_INK, t);
    fit &= TW(g, 12.f, 500.f, 0.f, FRUI_TNUM, "-1.00") <= 40.f;
    /* WIDTH: its name, its value at the right (as the correlation's) */
    yc += 22.f;
    x0 = mx + LBL(g, c, mx, yc + CAP(g, 10.5f) * 0.5f, 10.5f, C_DIM, "WIDTH") + 10.f;
    if (ms_mono_on(g)) snprintf(t, sizeof t, "0 %%"); else vtext(g, ID_MSWIDTH, t, sizeof t);
    TX(g, c, W - mx - TW(g, 12.f, 500.f, 0.f, FRUI_TNUM, t), yc + CAP(g, 12.f) * 0.5f, 12.f, 500.f, 0.f, FRUI_TNUM, C_INK, t);
    fit &= x0 + TW(g, 12.f, 500.f, 0.f, FRUI_TNUM, "200 %") <= W - mx && yc + 8.f <= bot - 4.f;
    g->ms_fit = fit;
}
/* MONO: its name and a switch (OFF; ON lit red); a click on the cell turns it */
static void ms_mono(txg_t *g, frui_canvas *c, float x, float y, float w, float h)
{
    int on = ms_mono_on(g);
    const char *t = on ? "ON" : "OFF";
    float bw = LBLW(g, 10.f, t) + 20.f, bh = 22.f, bx = x + w - 10.f - bw, byy = y + (h - bh) * 0.5f;
    RR(g, c, x, y, w, h, 2.f, C_WELL);
    LBL(g, c, x + 9.f, y + h * 0.5f + CAP(g, 10.5f) * 0.5f, 10.5f, C_INK, "MONO");
    RR(g, c, bx, byy, bw, bh, 3.f, on ? C_RED : C_DRAW);
    RRS(g, c, bx, byy, bw, bh, 3.f, on ? C_RED : C_LINE);
    LBL(g, c, bx + 10.f, byy + bh * 0.5f + CAP(g, 10.f) * 0.5f, 10.f, on ? 0xFFFFFFFFu : C_INK, t);
    wid_add2(g, TXG_W_PARAM, (int)ID_MSMONO, ID_MSMONO, 0, x, y, w, h);
}
static float hz_x(float v127) { return v127 / 127.f; }   /* the FILTER's steps are log Hz already (params.c F_HZ_*) */
static void viz(txg_t *g, frui_canvas *c, int sec, float x, float y, float w, float h, int on)
{
    uint32_t ink = on ? C_INK : C_FAINT;
    float lw2 = g->S < 1.5f ? 1.2f / g->S : 1.f, px = 0.f, py = 0.f;
    R(g, c, x, y + h - 1.f, w, 1.f, C_LINE);
    if (sec == SEC_COMP) {                /* its meters: in, out, the gain reduction (live, the app's compressor) */
        comp_meter(g, c, x, y, w, h, on);
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
    case SEC_REVERB: return txp_ui_get(g->c, ID_RMIX) > 0 && txp_ui_get(g->c, ID_RTYPE) > 0;   /* (TYPE OFF: none) */
    case SEC_MS: return txp_ui_get(g->c, ID_MSMID) != MS_0DB || txp_ui_get(g->c, ID_MSSIDE) != MS_0DB ||
                        txp_ui_get(g->c, ID_MSWIDTH) != MS_W100 || ms_mono_on(g);
    }
    return g->sn.ui.lfo_on != 0;
}
/* a source's trace: its last LFO_H ticks (row: SRCS' order; an LFO about the middle, an envelope or a follower from the
 * bottom) */
static void lfo_trace(txg_t *g, frui_canvas *c, int row, float x, float y, float w, float h, uint32_t col)
{
    float px = 0.f, py = 0.f, lw = g->S < 1.5f ? 1.f / g->S : 0.9f;
    int bip = row < 8;
    R(g, c, x, y + h - 1.f, w, 1.f, C_FAINT);
    for (int i = 0; i < LFO_H; i++) {
        float v = (float)g->lfoh[row][i] / 32767.f, xx = x + w * (float)i / (float)(LFO_H - 1),
              yy = y + h - 2.f - (bip ? v * 0.5f + 0.5f : v) * (h - 4.f);
        if (i) LN(g, c, px, py, xx, yy, lw * 1.2f, col);
        px = xx; py = yy;
    }
    CI(g, c, px, py, 2.2f, col);
}
/* a source's grip (2 x 3 dots in a red box): dragged onto a control, a link at +25 % and the source's mode */
static void grip(txg_t *g, frui_canvas *c, int src, float x, float y, float s)
{
    RRS(g, c, x, y, s, s, 3.f, C_RED);
    for (int r = 0; r < 3; r++)
        for (int q = 0; q < 2; q++) CI(g, c, x + s * 0.5f + ((float)q - 0.5f) * s * 0.27f, y + s * 0.5f + (float)(r - 1) * s * 0.23f, 1.25f, C_RED);
    wid_add(g, TXG_W_GRIP, src, x - 3.f, y - 3.f, s + 6.f, s + 6.f);
}
/* "moves 2 controls" / "not connected" */
static void src_count(const txg_t *g, int src, char *o, size_t n)
{
    int k = src_links(g, src);
    if (k) snprintf(o, n, "moves %d control%s", k, k > 1 ? "s" : ""); else snprintf(o, n, "not connected");
}
/* what a source is set to: an LFO its SHAPE, an envelope its TRIGGER, a follower its INPUT */
static void src_kind(const txg_t *g, int src, char *o, size_t n)
{
    uint32_t id = src <= 8 ? ID_LFO(src - 1, LP_SHAPE) : src <= 17 ? (6u << 8 | (uint32_t)(src - 16) * 16u) : (6u << 8 | (32u + (uint32_t)(src - 18) * 16u));
    int i = txp_param_index(id);
    snprintf(o, n, "%s", i >= 0 ? g->sn.ptx[i] : "");
}
/* a source's tile: its grip, its name and kind, its live trace, how many controls it moves; a click: its mode (again:
 * out of it). In its mode: red */
static void src_tile_draw(txg_t *g, frui_canvas *c, int src, float x, float y, float w, float h)
{
    int on = g->msrc == src, hot = g->hover_kind == TXG_W_SRC && g->hover_lane == src, n = src_links(g, src);
    char nm[16], kd[24], t[48];
    float gs = h >= 46.f ? 20.f : 18.f, tx = x + 8.f + gs + 8.f;
    RR(g, c, x, y, w, h, 3.f, on ? C_REDBG : C_WELL);
    if (on || hot) RRS(g, c, x, y, w, h, 3.f, on ? C_RED : frui_alpha(C_RED, 0.6f));
    wid_add(g, TXG_W_SRC, src, x, y, w, h);
    grip(g, c, src, x + 8.f, y + (h >= 46.f ? 8.f : (h - gs) * 0.5f), gs);
    src_name(src, nm, sizeof nm);
    src_kind(g, src, kd, sizeof kd);
    clipL(g, c, x, y, h >= 46.f ? w - 4.f : w * 0.55f - 6.f, h);
    tx += LBL(g, c, tx, y + 15.f, 9.5f, C_RED, nm) + 7.f;
    if (tx + LBLW(g, 8.5f, kd) <= x + (h >= 46.f ? w - 8.f : w * 0.55f - 6.f)) LBL(g, c, tx, y + 15.f, 8.5f, C_DIM, kd);   /* (its kind
                                          * where it fits) */
    src_count(g, src, t, sizeof t);
    if (g->zone) ZFIT(TW(g, 9.5f, 450.f, 0.f, 0, t) <= (h >= 46.f ? w - gs - 24.f : w * 0.55f - gs - 22.f) &&
                                tx - 7.f <= x + (h >= 46.f ? w - 4.f : w * 0.55f - 6.f));   /* (its count, its name) */
    if (h >= 46.f) {                     /* (tall: the trace across, the count under it) */
        lfo_trace(g, c, src_row(src), x + 8.f + gs + 8.f, y + 20.f, w - gs - 26.f, 16.f, n ? C_RED : C_DIM);
        TX(g, c, x + 8.f + gs + 8.f, y + h - 6.f, 9.5f, 450.f, 0.f, 0, C_DIM, t);
    } else {                             /* (low: the count under the name, the trace at the right) */
        TX(g, c, x + 8.f + gs + 8.f, y + h - 7.f, 9.5f, 450.f, 0.f, 0, C_DIM, t);
        frui_clip_reset(c);
        lfo_trace(g, c, src_row(src), x + w * 0.55f, y + 7.f, w * 0.45f - 10.f, h - 14.f, n ? C_RED : C_DIM);
    }
    frui_clip_reset(c);
}
/* an LFO's own well in its drawer: its grip, its trace (a click: its mode) */
static void src_well(txg_t *g, frui_canvas *c, int src, float x, float y, float w, float h)
{
    int on = g->msrc == src, hot = g->hover_kind == TXG_W_SRC && g->hover_lane == src;
    RR(g, c, x, y, w, h, 2.f, on ? C_REDBG : C_WELL);
    if (on || hot) RRS(g, c, x, y, w, h, 2.f, on ? C_RED : frui_alpha(C_RED, 0.6f));
    wid_add(g, TXG_W_SRC, src, x, y, w, h);
    grip(g, c, src, x + 8.f, y + 8.f, 20.f);
    lfo_trace(g, c, src_row(src), x + 38.f, y + 6.f, w - 48.f, h - 12.f, src_links(g, src) ? C_RED : C_DIM);
}
/* the strip, the EFFECTS zone: the whole mix, after all lanes, darker and cooler than SOUND; a dark gap over it, its band
 * (what it is), then its sections, each on its own ground, a small arrow between them in the order the sound goes through
 * (COMP > DRIVE > FILTER > DELAY > REVERB > M/S; MOVEMENT, tinted, moves things and takes no sound). Every text measured
 * in its room (zone_fit) */
static void draw_strip(txg_t *g, frui_canvas *c)
{
    const float y0 = g->fx_y, sy = y0 + FX_SY, sh = FX_H - FX_SY, hy = sy + 12.f;
    float x = 0.f, tot = 0.f;
    for (int s = 0; s < SEC_N; s++) tot += SEC_WT[s];
    R(g, c, 0, y0, g->W, FX_H, C_FX);
    R(g, c, 0, y0, g->W, FX_GAP, C_GAP);
    {   /* the band */
        static const char *const B0 = "EFFECTS", *const B1 = "\xC2\xB7 THE WHOLE MIX, AFTER ALL LANES, LEFT TO RIGHT";
        float by = y0 + FX_GAP + FX_BAND * 0.5f + CAP(g, 10.f) * 0.5f, bx = 14.f;
        R(g, c, 0, y0 + FX_GAP + FX_BAND - 1.f, g->W, 1.f, C_FXLINE);
        bx += TX(g, c, bx, by, 10.f, 620.f, 1.6f, 0, C_INK, B0) + 10.f;
        bx += TX(g, c, bx, by, 10.f, 620.f, 1.6f, 0, C_FXHD, B1);
        ZFIT(bx <= g->W - 14.f);
    }
    g->zone = 2;
    for (int s = 0; s < SEC_N; s++) {
        float w = g->W * SEC_WT[s] / tot, sx = x + 6.f, sw = w - 12.f, ix = sx + 10.f, iw = sw - 20.f, hx, ax;
        int on = sec_on(g, s), open = g->ov == OV_FX && (s < SEC_MOVE ? g->fx_tab == s : g->fx_tab >= TAB_LFO0);
        const char *at = open ? "CLOSE \xC3\x97" : "ALL \xE2\x80\xBA";
        RR(g, c, sx, sy, sw, sh + 6.f, 5.f, open ? C_FXOPEN : s == SEC_MOVE ? C_FXMOV : C_FXSEC);   /* (its foot under the
                                          * window's edge: square there) */
        if (open) R(g, c, sx + 4.f, sy, sw - 8.f, 2.f, C_RED);
        if (s < SEC_MS) {                /* (the arrow to the next) */
            float aw = TW(g, 13.f, 600.f, 0.f, 0, "\xE2\x80\xBA");
            TX(g, c, x + w - aw * 0.5f, sy + sh * 0.5f + CAP(g, 13.f) * 0.5f, 13.f, 600.f, 0.f, 0, C_FXARR, "\xE2\x80\xBA");
            ZFIT(aw + 2.f <= 12.f);
        }
        /* the head: its dot, its name (a click: its drawer, every parameter of it), "ALL" (open: "CLOSE") */
        CI(g, c, ix + 2.5f, hy, 2.6f, on ? (s == SEC_MOVE ? C_RED : C_INK) : C_FAINT);
        hx = ix + 11.f + LBL(g, c, ix + 11.f, hy + CAP(g, 12.f) * 0.5f, 12.f, C_INK, SEC_NAME[s]);
        ax = sx + sw - 10.f - LBLW(g, 9.f, at);
        wid_add(g, TXG_W_FXOPEN, s, sx, sy, sw, 22.f);
        if (s == SEC_COMP) {             /* (COMP's own switch: its ON; a click turns it) */
            const char *t = on ? "ON" : "OFF";
            float tw = LBLW(g, 9.f, t) + 12.f, tx = hx + 10.f;
            RR(g, c, tx, hy - 8.f, tw, 16.f, 2.f, on ? C_FXSEG : C_FX); RRS(g, c, tx, hy - 8.f, tw, 16.f, 2.f, C_FXLINE);
            LBL(g, c, tx + 6.f, hy + CAP(g, 9.f) * 0.5f, 9.f, on ? C_INK : C_DIM, t);
            wid_add2(g, TXG_W_PARAM, (int)ID_CON, ID_CON, 0, tx, hy - 8.f, tw, 16.f);
            hx = tx + LBLW(g, 9.f, "OFF") + 12.f;   /* (its widest) */
        }
        if (s == SEC_MOVE) {             /* the movement: two sources, those connected first (else LFO 1, 2): their
                                          * tiles (a click: the source's mode; the grip: dragged onto a control) */
            int shown = 0, more = 0, pick[2] = { -1, -1 };
            for (int i = 0; i < SRC_N; i++)
                if (src_links(g, SRCS[i]) || g->msrc == SRCS[i]) { if (shown < 2) pick[shown++] = SRCS[i]; else more++; }
            for (int i = 0; shown < 2 && i < SRC_N; i++) if (SRCS[i] != pick[0]) pick[shown++] = SRCS[i];
            if (more) {
                char t[8];
                snprintf(t, sizeof t, "+%d", more);
                hx += 8.f + LBL(g, c, hx + 8.f, hy + CAP(g, 10.f) * 0.5f, 10.f, C_RED, t);
            }
            for (int i = 0; i < 2; i++) src_tile_draw(g, c, pick[i], ix, sy + 22.f + 51.f * (float)i, iw, 46.f);
        }
        if (hx + 8.f > ax) { at = open ? "\xC3\x97" : "\xE2\x80\xBA"; ax = sx + sw - 10.f - LBLW(g, 9.f, at); }   /* (no room
                                          * for its word: the sign alone) */
        ZFIT(hx + 8.f <= ax);
        LBL(g, c, ax, hy + CAP(g, 9.f) * 0.5f, 9.f, open ? C_INK : C_DIM, at);
        if (s == SEC_MS) {               /* the M/S: its meters, MID and SIDE */
            ms_meters(g, c, ix, sy + 22.f, 10.f, 4.f, 0.f, 14.f, 8.5f, C_DIM);
            g->tag_below = 1;
            pcell(g, c, ix, sy + 52.f, iw, 24.f, ID_MSMID, 0, 0);
            pcell(g, c, ix, sy + 88.f, iw, 24.f, ID_MSSIDE, 0, 0);
            g->tag_below = 0;
        } else if (s < SEC_MOVE) {
            /* (two controls each, full width, their names in full: the rest in the section's drawer) */
            static const uint32_t K[5][2] = { { ID_CTHR, ID_CRATIO }, { ID_DAMT, ID_DTYPE }, { ID_FHP, ID_FLP },
                                              { ID_GMIX, ID_GTIME }, { ID_RTYPE, ID_RMIX } };
            viz(g, c, s, ix, sy + 22.f, iw, 26.f, on);
            g->tag_below = 1;            /* (a cell's tags under it: the 12 px over the next) */
            pcell(g, c, ix, sy + 52.f, iw, 24.f, K[s][0], s == SEC_FILTER ? "HIGH PASS" : 0, 0);
            pcell(g, c, ix, sy + 88.f, iw, 24.f, K[s][1], s == SEC_FILTER ? "LOW PASS" : 0, 0);
            g->tag_below = 0;
        }
        x += w;
    }
    g->zone = 0;
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
static float nav_rh = 28.f;               /* a nav row's height (the presets' nav: less when its rows would not fit) */
static void nav_item(txg_t *g, frui_canvas *c, float y, const char *t, int n, int on, int lane)
{
    char b[8];
    if (on) { R(g, c, 0, y, OV_NAV - 1.f, nav_rh, C_NAV); R(g, c, 0, y, 2.f, nav_rh, C_RED); }
    TX(g, c, 16.f, y + nav_rh * 0.5f + CAP(g, 12.5f) * 0.5f, 12.5f, 600.f, 0.f, 0, on ? C_INK : C_DIM, t);
    snprintf(b, sizeof b, "%d", n);
    TX(g, c, OV_NAV - 16.f - TW(g, 11.f, 450.f, 0.f, FRUI_TNUM, b), y + nav_rh * 0.5f + CAP(g, 11.f) * 0.5f, 11.f, 450.f, 0.f, FRUI_TNUM, C_DIM, b);
    wid_add(g, TXG_W_NAV, lane, 0, y, OV_NAV - 1.f, nav_rh);
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
static void sb_items(txg_t *g)           /* the engine browser's list: the family's sounds, or the search's (f << 4 | i) */
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
/* a sound's line: its engine's first sentence in the manual (docs/MANUAL.md section 5, card_hints.h TXH_ENG) */
static const char *snd_desc(const char *snd)
{
    const char *eng = 0;
    size_t i;
    for (i = 0; i < sizeof TXH_SND / sizeof TXH_SND[0] && !eng; i++) if (!strcmp(TXH_SND[i].snd, snd)) eng = TXH_SND[i].eng;
    for (i = 0; eng && i < sizeof TXH_ENG / sizeof TXH_ENG[0]; i++) if (!strcmp(TXH_ENG[i].eng, eng)) return TXH_ENG[i].desc;
    return "An engine of its own, computed as it plays.";
}
static void play_mark(txg_t *g, frui_canvas *c, float x, float yc, float s, uint32_t col)   /* a small play triangle */
{
    frui_path p;
    frui_path_init(&p);
    frui_path_move(&p, x * g->S, (yc - s) * g->S);
    frui_path_line(&p, (x + s * 1.6f) * g->S, yc * g->S);
    frui_path_line(&p, x * g->S, (yc + s) * g->S);
    frui_path_fill(c, &p, col);
    frui_path_free(&p);
}
/* the preview's HOW MANY (lane 0) or HOW ACTIVE (1): its value between steppers */
static void pv_step(txg_t *g, frui_canvas *c, int act, float x, float y, float w)
{
    char t[16];
    float h = 26.f, bw = 74.f, bx = x + w - bw, yc = y + h * 0.5f, tw;
    ptext(TXP_ID_LANE(0, act ? TXP_PL_ACTIVE : TXP_PL_COUNT), act ? g->pv_act : g->pv_cnt, t, sizeof t);
    RR(g, c, x, y, w, h, 2.f, C_WELL);
    LBL(g, c, x + 9.f, yc + CAP(g, 10.f) * 0.5f, 10.f, C_INK, act ? "ACTIVE" : "MANY");
    RR(g, c, bx, y + 4.f, bw - 4.f, h - 8.f, 2.f, C_SEG);
    chevron(g, c, bx + 8.f, yc, -1, C_DIM);
    chevron(g, c, bx + bw - 12.f, yc, 1, C_DIM);
    tw = TW(g, 11.f, 500.f, 0.f, FRUI_TNUM, t);
    TX(g, c, bx + (bw - 4.f - tw) * 0.5f, yc + CAP(g, 11.f) * 0.5f, 11.f, 500.f, 0.f, FRUI_TNUM, g->pv_user ? C_INK : C_DIM, t);
    wid_add2(g, TXG_W_PVSTEP, act, 0, -1, bx, y, 18.f, h);
    wid_add2(g, TXG_W_PVSTEP, act, 0, 1, bx + bw - 22.f, y, 22.f, h);
}
static void draw_sounds(txg_t *g, frui_canvas *c)
{
    char t[96], nm[32];
    int k = g->sb_lane, tot = 0, cur = -1;
    const txp_ui_lane_t *l = &g->sn.ui.lane[k];
    float y, lx = OV_NAV, lw = g->W - OV_NAV - OV_PREV, rows = (g->H - OV_Y0 - 12.f) / OV_RH;
    for (int f = 0; f < g->cat->nfam; f++) tot += g->cat->fam[f].n;
    snprintf(t, sizeof t, "LANE %d", k + 1);
    snprintf(nm, sizeof nm, "Type to search %d engines", tot);
    ov_header(g, c, "ENGINE FOR", t, 1, nm);
    if (g->pv_on) {                      /* (the header: the preview under way) */
        char a[32];
        float x = 18.f + TW(g, 17.f, 700.f, 2.4f, 0, "ENGINE FOR") + 8.f + TW(g, 17.f, 700.f, 2.4f, 0, t) + 28.f;
        upper(a, snd_name(g, g->sb_cf, g->sb_ci), sizeof a);
        snprintf(t, sizeof t, "PREVIEW ON LANE %d: %s", k + 1, a);
        play_mark(g, c, x - 2.f, TOP_H + 23.f, 4.5f, C_RED);
        TX(g, c, x + 11.f, TOP_H + 23.f + CAP(g, 11.f) * 0.5f, 11.f, 500.f, 0.3f, 0, C_INK, t);
    }
    /* the families */
    R(g, c, OV_NAV - 1.f, OV_Y0, 1.f, g->H - OV_Y0, C_LINE);
    for (int f = 0; f < g->cat->nfam; f++) nav_item(g, c, OV_Y0 + 6.f + 28.f * (float)f, g->cat->fam[f].name, g->cat->fam[f].n, !g->q[0] && f == g->sb_fam, f);
    /* the sounds (the one the lane plays now: >) */
    sb_items(g);
    for (int j = 0; j < g->nitems; j++) if (g->items[j] == (g->sb_cf << 4 | g->sb_ci)) cur = j;
    list_view(g, cur, rows);
    if (g->sb_top > g->nitems - (int)rows) g->sb_top = g->nitems - (int)rows;
    if (g->sb_top < 0) g->sb_top = 0;
    clipL(g, c, lx, OV_Y0, lw, g->H - OV_Y0);
    if (!g->nitems) TX(g, c, lx + 16.f, OV_Y0 + 30.f, 12.5f, 450.f, 0.f, 0, C_DIM, "No engine has that name.");
    for (int j = g->sb_top; j < g->nitems && (float)(j - g->sb_top) < rows; j++) {
        int f = g->items[j] >> 4, i = g->items[j] & 15, m = lane_of_snd(g, f, i), e = snd_eng(g, f, i);
        int kept = f == g->sb_kf && i == g->sb_ki, cu = j == cur, plays = l->used && l->run && l->fi == f && l->si == i;
        float ry = OV_Y0 + 4.f + OV_RH * (float)(j - g->sb_top), yc = ry + OV_RH * 0.5f, x;
        if (g->hover_kind == TXG_W_LIST && g->hover_lane == j) R(g, c, lx, ry, lw, OV_RH, C_HOV);
        if (kept) { R(g, c, lx, ry, lw, OV_RH, C_NAV); R(g, c, lx, ry, 2.f, OV_RH, C_RED); }
        if (cu) RRS(g, c, lx + 1.f, ry + 1.f, lw - 2.f, OV_RH - 2.f, 2.f, C_DIM);
        if (plays) play_mark(g, c, lx + 6.f, yc, 3.5f, C_RED);
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
    /* the right: the cursor's sound, its line, what plays on the lane (its events, its level), PREVIEW with its HOW
     * MANY and HOW ACTIVE, the engine's colour, KEEP / CLOSE */
    {
        float px = g->W - OV_PREV + 16.f, pw = OV_PREV - 32.f;
        int e = snd_eng(g, g->sb_cf, g->sb_ci);
        R(g, c, g->W - OV_PREV, OV_Y0, 1.f, g->H - OV_Y0, C_LINE);
        y = OV_Y0 + 24.f;
        snprintf(t, sizeof t, "%s  \xC2\xB7  SOURCE %s", g->sb_cf >= 0 ? g->cat->fam[g->sb_cf].name : "", e >= 0 && e < g->cat->neng ? g->cat->eng[e] : "");
        upper(t, t, sizeof t);
        LBL(g, c, px, y, 10.5f, C_DIM, t);
        upper(nm, snd_name(g, g->sb_cf, g->sb_ci), sizeof nm);
        clipL(g, c, px, y, pw, 50.f);
        TX(g, c, px, y + 36.f, 28.f, 700.f, 1.1f, 0, C_INK, nm);
        frui_clip_reset(c);
        y += 58.f;
        (void)wrapn(g, c, px, y, pw, snd_desc(snd_name(g, g->sb_cf, g->sb_ci)), 3, 15.f, 11.5f, C_INK);
        y += 52.f;
        {   /* what the lane plays now: its sound, its events, its level */
            float lh = 40.f, f = 0.f;
            int pk = 0;
            if (l->used && l->run) {
                char a[32];
                cap_name(a, l->snd, sizeof a);
                snprintf(t, sizeof t, "LANE %d PLAYS  \xC2\xB7  ", k + 1);
                upper(t, t, sizeof t);
                play_mark(g, c, px, y - 3.5f, 3.5f, C_RED);
                clipL(g, c, px, y - 12.f, pw, 16.f);
                TX(g, c, px + 10.f + LBL(g, c, px + 10.f, y, 10.f, C_DIM, t), y, 11.f, 600.f, 0.f, 0, C_INK, a);
                frui_clip_reset(c);
            } else {
                snprintf(t, sizeof t, "LANE %d %s", k + 1, l->used ? "IS STOPPED: PREVIEW PLAYS IT" : "IS EMPTY: PREVIEW PLAYS IT");
                LBL(g, c, px, y, 10.f, C_DIM, t);
            }
            draw_line(g, c, k, px, y + 8.f, pw - 56.f, lh, 1.f);
            for (uint32_t b = NBLK - GLOW_BLK; b < NBLK; b++) if (g->pk[b][k] > pk) pk = g->pk[b][k];
            f = (float)pk / 255.f;
            RR(g, c, px + pw - 48.f, y + 8.f, 8.f, lh, 2.f, C_WELL);
            if (f > 0.f) RR(g, c, px + pw - 48.f, y + 8.f + lh * (1.f - f), 8.f, lh * f, 2.f, f > 0.97f ? C_RED : C_INK);
            if (pk) snprintf(t, sizeof t, "%.0f", (double)(-72.f + 72.f * (float)(pk - 1) / 254.f)); else snprintf(t, sizeof t, "-inf");
            TX(g, c, px + pw - TW(g, 10.f, 500.f, 0.f, FRUI_TNUM, t), y + 22.f, 10.f, 500.f, 0.f, FRUI_TNUM, C_INK, t);
            TX(g, c, px + pw - TW(g, 8.5f, 450.f, 0.f, 0, "dB"), y + 36.f, 8.5f, 450.f, 0.f, 0, C_DIM, "dB");
            y += lh + 22.f;
        }
        {   /* PREVIEW (play / stop), its HOW MANY and HOW ACTIVE */
            float bw = 120.f, h = 30.f;
            int hot = g->hover_kind == TXG_W_PREVIEW;
            RR(g, c, px, y, bw, h, 3.f, g->pv_on ? C_SEG : hot ? C_INK : C_BTN);
            RRS(g, c, px, y, bw, h, 3.f, g->pv_on ? C_RED : C_INK);
            if (g->pv_on) R(g, c, px + 14.f, y + h * 0.5f - 4.5f, 9.f, 9.f, C_INK);
            else play_mark(g, c, px + 14.f, y + h * 0.5f, 5.f, hot ? C_PANEL : C_INK);
            LBL(g, c, px + 32.f, y + h * 0.5f + CAP(g, 11.f) * 0.5f, 11.f, !g->pv_on && hot ? C_PANEL : C_INK, g->pv_on ? "STOP" : "PREVIEW");
            wid_add(g, TXG_W_PREVIEW, -1, px, y, bw, h);
            y += h + 10.f;
            LBL(g, c, px, y + 8.f, 9.5f, C_DIM, l->used && l->run && !g->pv_user ? "WHEN STOPPED, PREVIEW AT" : "PREVIEW AT");
            pv_step(g, c, 0, px, y + 16.f, (pw - 8.f) * 0.5f);
            pv_step(g, c, 1, px + (pw - 8.f) * 0.5f + 8.f, y + 16.f, (pw - 8.f) * 0.5f);
            y += 54.f;
        }
        snprintf(t, sizeof t, "A click or the arrows play an engine on lane %d, started if", k + 1);
        TX(g, c, px, y, 11.f, 450.f, 0.f, 0, C_DIM, t);
        TX(g, c, px, y + 15.f, 11.f, 450.f, 0.f, 0, C_DIM, "stopped. KEEP keeps it; CLOSE puts the lane back");
        TX(g, c, px, y + 30.f, 11.f, 450.f, 0.f, 0, C_DIM, "as it was (engine, cards, stopped or playing).");
        y += 52.f;
        if (e >= 0 && y + 24.f < g->H - 64.f) {
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
    {   /* (the rows 28 high, less when they would not all fit: the window at its least height) */
        int nr = ct->ncat + 1 + (nu ? 1 : 0);
        float fit = (g->H - OV_Y0 - 6.f - 26.f * (float)(nu ? 3 : 2)) / (float)nr;
        nav_rh = fit < 28.f ? (fit > 20.f ? fit : 20.f) : 28.f;
    }
    if (nu) { nav_group(g, c, y, "MINE"); y += 26.f; nav_item(g, c, y, "User", nu, !g->q[0] && g->pb_bank == PB_USER, PB_USER); y += nav_rh; }
    nav_group(g, c, y, "FACTORY"); y += 26.f;
    for (int i = 0; i < ct->ncat; i++) {
        char nm[16];
        cap_name(nm, ct->cat[i], sizeof nm);
        for (char *q = nm + 1; *q; q++) if (*q >= 'A' && *q <= 'Z') *q = (char)(*q + 32);
        nav_item(g, c, y, nm, ct->cat_at[i + 1] - ct->cat_at[i], !g->q[0] && g->pb_bank == i, i);
        y += nav_rh;
    }
    nav_group(g, c, y, "START FROM"); y += 26.f;
    nav_item(g, c, y, "Landscapes", ct->nfam, !g->q[0] && g->pb_bank == PB_LAND, PB_LAND);
    nav_rh = 28.f;
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
        for (int i = 0; i < 3; i++) {
            const char *n = VIEW_N[i];
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
    /* SOURCE COLOURS: the sources the lanes run on (the whole last column) */
    x = 3.f * gw + pad; y = OV_Y0 + 8.f;
    group(g, c, x, y, "SOURCE COLOURS", "Bone at first; a colour follows its source.");
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
        if (!n) TX(g, c, x, y + 14.f, 12.f, 450.f, 0.f, 0, C_DIM, "No lane has an engine.");
        y = g->H - 44.f;
        button(g, c, x, y, 170.f, 26.f, "RESET ALL TO BONE", 0);
        wid_add(g, TXG_W_COLRESET, -1, x, y, 170.f, 26.f);
    }
}
/* ---- an effect's drawer: it rises from the strip over the lane's drawer (the place and the rows stay above it): at
 * the left a large picture, live (COMP's meters and curve, DRIVE's curve, FILTER's response in Hz, DELAY's echoes and
 * grains, REVERB's tail, M/S's goniometer and meters); at the right every parameter of the effect in groups by purpose.
 * MOVEMENT's: its tabs (LFO 1 .. 8, ENV and FOLLOW, the matrix) over the same */
typedef struct { const char *t; int n; uint32_t id[8]; } grp_t;
/* groups: their values in columns of rows (filled down), each group as many columns as it needs, named over them */
static void groups_x(const grp_t *gr, int ng, int rows, float x, float w, float *gx, float *gw)
{
    int cols = 0, gaps = 0;
    float cw, xx = x;
    for (int i = 0; i < ng; i++) { int gc = (gr[i].n + rows - 1) / rows; cols += gc; gaps += gc - 1; }
    cw = (w - 18.f * (float)(ng - 1) - 6.f * (float)gaps) / (float)(cols > 0 ? cols : 1);
    for (int i = 0; i < ng; i++) {
        int gc = (gr[i].n + rows - 1) / rows;
        gx[i] = xx; gw[i] = cw * (float)gc + 6.f * (float)(gc - 1);
        xx += gw[i] + 18.f;
    }
}
static void groups(txg_t *g, frui_canvas *c, const grp_t *gr, int ng, int rows, float x, float y, float w, float h)
{
    int cols = 0, gaps = 0;
    float cw, ch, xx = x;
    for (int i = 0; i < ng; i++) { int gc = (gr[i].n + rows - 1) / rows; cols += gc; gaps += gc - 1; }
    cw = (w - 18.f * (float)(ng - 1) - 6.f * (float)gaps) / (float)(cols > 0 ? cols : 1);
    ch = (h - 16.f - 6.f * (float)(rows - 1)) / (float)rows;
    ch = ch > 60.f ? 60.f : ch;
    for (int i = 0; i < ng; i++) {
        int gc = (gr[i].n + rows - 1) / rows;
        float gw = cw * (float)gc + 6.f * (float)(gc - 1);
        LBL(g, c, xx, y + 9.f, 9.5f, C_DIM, gr[i].t);
        R(g, c, xx + LBLW(g, 9.5f, gr[i].t) + 8.f, y + 5.f, fmaxf(gw - LBLW(g, 9.5f, gr[i].t) - 8.f, 0.f), 1.f, C_LINE);
        for (int j = 0; j < gr[i].n; j++)
            pcell(g, c, xx + (cw + 6.f) * (float)(j / rows), y + 16.f + (ch + 6.f) * (float)(j % rows), cw, ch, gr[i].id[j], 0, 0);
        xx += gw + 18.f;
    }
}
static void fxd_title(txg_t *g, frui_canvas *c, float x, float y, float w, const char *name, uint32_t col, const char *why)
{
    float nw = LBL(g, c, x, y + 10.f, 12.f, col, name);
    clipL(g, c, x + nw + 12.f, y - 2.f, w - nw - 12.f, 16.f);
    TX(g, c, x + nw + 12.f, y + 10.f, 10.f, 450.f, 0.f, 0, C_DIM, why);
    frui_clip_reset(c);
}
/* the large pictures: as the strip's (viz), with their numbers */
static void graph_big(txg_t *g, frui_canvas *c, int sec, float x, float y, float w, float h, int on)
{
    char t[40];
    uint32_t ink = on ? C_INK : C_FAINT;
    float lw2 = g->S < 1.5f ? 1.4f / g->S : 1.2f, px = 0.f, py = 0.f;
    if (sec == SEC_COMP) {               /* the meters, the curve beside them */
        float cs = fminf(h, w * 0.36f);
        comp_meter(g, c, x, y + 4.f, w - cs - 18.f, 80.f, on);
        comp_curve(g, c, x + w - cs, y, cs, cs, on);
        return;
    }
    if (sec == SEC_DRIVE) {              /* the curve over the straight line (no drive), the input at the bottom */
        float amt = (float)txp_ui_get(g->c, ID_DAMT) / 127.f, k = 1.f + amt * 8.f, cs = fminf(h, w * 0.5f), cx = x;
        RR(g, c, cx, y, cs, cs, 2.f, C_WELL);
        LN(g, c, cx, y + cs, cx + cs, y, lw1(g), C_LINE);
        LN(g, c, cx, y + cs * 0.5f, cx + cs, y + cs * 0.5f, lw1(g), C_FAINT);
        for (int i = 0; i <= 48; i++) {
            float a = (float)i / 24.f - 1.f, b = tanhf(a * k) / tanhf(k), xx = cx + (a + 1.f) * 0.5f * cs, yy = y + cs * 0.5f - b * cs * 0.46f;
            if (i) LN(g, c, px, py, xx, yy, lw2, ink);
            px = xx; py = yy;
        }
        {
            static const uint32_t ID[4] = { ID_DAMT, ID_DTONE, ID_DMIX, ID_DTYPE };
            static const char *const N[4] = { "AMOUNT", "TONE", "MIX", "TYPE" };
            for (int i = 0; i < 4; i++) {
                vtext(g, ID[i], t, sizeof t);
                LBL(g, c, cx + cs + 16.f, y + 14.f + 18.f * (float)i, 9.f, C_DIM, N[i]);
                TX(g, c, cx + cs + 80.f, y + 14.f + 18.f * (float)i, 11.f, 500.f, 0.f, FRUI_TNUM, ink, t);
            }
        }
        return;
    }
    RR(g, c, x, y, w, h, 2.f, C_WELL);
    if (sec == SEC_FILTER) {             /* the response: HIGH PASS then LOW PASS, RESONANCE at each cutoff, in Hz */
        static const float GF[4] = { 100.f, 1000.f, 10000.f, 0.f };
        static const char *const GN[3] = { "100", "1k", "10k" };
        int hp = txp_ui_get(g->c, ID_FHP), lp = txp_ui_get(g->c, ID_FLP), sl = txp_ui_get(g->c, ID_FSLOPE), live;
        int mh = pmod(g, ID_FHP, &live), ml = pmod(g, ID_FLP, &live);
        float res = (float)txp_ui_get(g->c, ID_FRES) / 127.f, st = 0.10f + 0.06f * (float)sl, ix = x + 8.f, iw = w - 16.f,
              iy = y + 18.f, ih = h - 34.f, xh = ix + iw * hz_x((float)hp), xl = ix + iw * hz_x((float)lp);
        for (int i = 0; i < 3; i++) {    /* (the decades: 20 Hz .. 20 kHz, log) */
            float gx = ix + iw * log10f(GF[i] / 20.f) / 3.f;
            R(g, c, gx, iy, 1.f, ih, C_LINE);
            TX(g, c, gx - TW(g, 8.5f, 450.f, 0.f, 0, GN[i]) * 0.5f, y + h - 5.f, 8.5f, 450.f, 0.f, 0, C_FAINT, GN[i]);
        }
        TX(g, c, ix, y + h - 5.f, 8.5f, 450.f, 0.f, 0, C_FAINT, "20");
        TX(g, c, ix + iw - TW(g, 8.5f, 450.f, 0.f, 0, "20k"), y + h - 5.f, 8.5f, 450.f, 0.f, 0, C_FAINT, "20k");
        for (int i = 0; i <= 96; i++) {
            float a = (float)i / 96.f, att = 0.f, bump = 0.f, yy;
            if (hp > 0) {
                float d = hz_x((float)hp) - a;
                if (d > 0.f) att += d / st;
                bump += res * expf(-d * d * 900.f);
            }
            if (lp < 127) {
                float d = a - hz_x((float)lp);
                if (d > 0.f) att += d / st;
                bump += res * expf(-d * d * 900.f);
            }
            att = att > 1.f ? 1.f : att;
            yy = iy + ih * 0.22f + att * ih * 0.78f - bump * ih * 0.2f;
            if (i) LN(g, c, px, py, ix + a * iw, yy, lw2, C_INK);
            px = ix + a * iw; py = yy;
        }
        if (hp > 0) {
            dashed(g, c, xh, iy, iy + ih, mh ? C_RED : C_DIM);
            vtext(g, ID_FHP, t, sizeof t);
            TX(g, c, fminf(xh + 4.f, ix + iw - TW(g, 9.5f, 500.f, 0.f, FRUI_TNUM, t)), y + 12.f, 9.5f, 500.f, 0.f, FRUI_TNUM, mh ? C_RED : C_INK, t);
        }
        if (lp < 127) {
            dashed(g, c, xl, iy, iy + ih, ml ? C_RED : C_DIM);
            vtext(g, ID_FLP, t, sizeof t);
            TX(g, c, fmaxf(xl - 4.f - TW(g, 9.5f, 500.f, 0.f, FRUI_TNUM, t), ix), y + 12.f, 9.5f, 500.f, 0.f, FRUI_TNUM, ml ? C_RED : C_INK, t);
        }
        if (hp <= 0 && lp >= 127) TX(g, c, ix, y + 12.f, 9.5f, 450.f, 0.f, 0, C_DIM, "Both open: the mix passes as it is.");
    } else if (sec == SEC_DELAY) {       /* the echoes: spaced by TIME, fading by FEEDBACK, each a cloud of GRAINS */
        float tm = (float)txp_ui_get(g->c, ID_GTIME) / 133.f, fb = (float)txp_ui_get(g->c, ID_GFB) / 127.f,
              ng = (float)txp_ui_get(g->c, G_ID(3)), spr = (float)txp_ui_get(g->c, G_ID(5)) / 127.f,
              mix = (float)txp_ui_get(g->c, ID_GMIX) / 127.f, sp = 10.f + tm * 54.f, a = 1.f, ix = x + 10.f, base = y + h - 16.f;
        int n = 0;
        uint32_t r = 0x9E3779B9u;
        LN(g, c, ix, base, x + w - 8.f, base, lw1(g), C_LINE);
        for (float xx = ix; xx < x + w - 8.f && a > 0.03f; xx += sp, n++) {
            float hh = (h - 40.f) * a;
            int k = n ? 1 + (int)(ng / 3.f) : 1;
            R(g, c, xx, base - hh, 2.f, hh, frui_alpha(ink, n ? 0.6f + 0.4f * mix : 1.f));
            for (int i = 1; n && i < k; i++) {   /* (the grains round the echo: SPRAY spreads them) */
                float o;
                r = r * 1664525u + 1013904223u;
                o = ((float)(r >> 8) / 16777216.f - 0.5f) * sp * (0.2f + 0.8f * spr);
                R(g, c, xx + o, base - hh * 0.55f, 1.f, hh * 0.55f, frui_alpha(ink, 0.4f * (0.6f + 0.4f * mix)));
            }
            a *= 0.35f + 0.6f * fb;
        }
        vtext(g, ID_GTIME, t, sizeof t);
        LBL(g, c, ix, y + 14.f, 9.f, C_DIM, "TIME");
        TX(g, c, ix + 40.f, y + 14.f, 10.5f, 500.f, 0.f, FRUI_TNUM, ink, t);
        vtext(g, ID_GFB, t, sizeof t);
        LBL(g, c, ix + 120.f, y + 14.f, 9.f, C_DIM, "FEEDBACK");
        TX(g, c, ix + 190.f, y + 14.f, 10.5f, 500.f, 0.f, FRUI_TNUM, ink, t);
        vtext(g, G_ID(3), t, sizeof t);
        LBL(g, c, ix + 250.f, y + 14.f, 9.f, C_DIM, "GRAINS");
        TX(g, c, ix + 306.f, y + 14.f, 10.5f, 500.f, 0.f, FRUI_TNUM, ink, t);
    } else if (sec == SEC_REVERB) {      /* its tail: after PREDELAY, as long as DECAY, as dense as SIZE */
        float dc = (float)txp_ui_get(g->c, ID_RDECAY) / 127.f, pd = (float)txp_ui_get(g->c, G_ID(20)) / 127.f,
              sz = (float)txp_ui_get(g->c, G_ID(21)) / 127.f, ix = x + 10.f, iw = w - 20.f, base = y + h - 16.f, hh = h - 40.f,
              x0 = ix + iw * 0.12f * pd;
        uint32_t r = 0x2545F491u;
        LN(g, c, ix, base, ix + iw, base, lw1(g), C_LINE);
        R(g, c, ix, base - hh, 2.f, hh, ink);   /* (the sound itself) */
        for (int i = 0; i < 160; i++) {  /* (its reflections, thinning out) */
            float a = (float)i / 160.f, e = expf(-a * (7.f - 6.f * dc)), xx = x0 + a * (ix + iw - x0), v;
            r = r * 1664525u + 1013904223u;
            v = 0.45f + 0.55f * (float)(r >> 8) / 16777216.f;
            if ((r >> 4 & 7u) > (uint32_t)(2.f + 5.f * sz)) continue;
            R(g, c, xx, base - hh * e * v, 1.f, hh * e * v, frui_alpha(ink, 0.35f));
        }
        px = x0; py = base - hh;
        for (int i = 1; i <= 60; i++) {
            float a = (float)i / 60.f, e = expf(-a * (7.f - 6.f * dc)), xx = x0 + a * (ix + iw - x0), yy = base - e * hh;
            LN(g, c, px, py, xx, yy, lw2, ink);
            px = xx; py = yy;
        }
        vtext(g, ID_RDECAY, t, sizeof t);
        LBL(g, c, ix + iw * 0.4f, y + 14.f, 9.f, C_DIM, "DECAY");
        TX(g, c, ix + iw * 0.4f + 50.f, y + 14.f, 10.5f, 500.f, 0.f, FRUI_TNUM, ink, t);
        vtext(g, ID_RTYPE, t, sizeof t);
        LBL(g, c, ix + iw * 0.4f, y + 30.f, 9.f, C_DIM, "TYPE");
        TX(g, c, ix + iw * 0.4f + 50.f, y + 30.f, 10.5f, 500.f, 0.f, 0, ink, t);
    }
}
/* M/S's drawer: at the left its goniometer and the correlation under it (one well); at the right its meters, then its
 * values: LEVEL (MID, SIDE), WIDTH (WIDTH, MONO), in columns of about 200 px as many as fit */
static void draw_ms_fxd(txg_t *g, frui_canvas *c, float by, float x1)
{
    float bw = fmaxf(MS_GS + 16.f, 236.f), bh = MS_GS + MS_CH + 16.f, sx = 16.f, sy = by + 38.f, mx0 = sx + bw + 20.f, cx0, aw, cw, ch = 60.f, yy;
    int ncol;
    fxd_title(g, c, 16.f, by + 12.f, x1 - 32.f, SEC_NAME[SEC_MS], C_INK, "The mix's middle and sides, after the reverb.");
    RR(g, c, sx, sy, bw, bh, 4.f, C_WELL);
    RRS(g, c, sx, sy, bw, bh, 4.f, C_LINE);
    ms_gonio(g, c, sx + (bw - MS_GS) * 0.5f, sy + 8.f, MS_GS);
    ms_corr(g, c, sx + 10.f, sy + 8.f + MS_GS + 6.f, bw - 20.f);
    ms_meters(g, c, mx0, sy + 4.f, 12.f, 6.f, 8.f, bh - 22.f, 10.f, C_DIM);
    cx0 = mx0 + 4.f * 12.f + 3.f * 6.f + 8.f + 16.f;
    aw = x1 - 16.f - cx0;
    ncol = (int)floorf((aw + 8.f) / 198.f);
    ncol = ncol < 2 ? 2 : ncol;
    cw = (aw - 8.f * (float)(ncol - 1)) / (float)ncol;
    yy = sy;
    for (int gr = 0; gr < 2; gr++) {     /* (a group's name over a line across, its cells under it) */
        const char *gn = gr ? "WIDTH" : "LEVEL";
        LBL(g, c, cx0, yy + 9.f, 9.5f, C_DIM, gn);
        R(g, c, cx0 + LBLW(g, 9.5f, gn) + 8.f, yy + 5.f, aw - LBLW(g, 9.5f, gn) - 8.f, 1.f, C_LINE);
        yy += 16.f;
        if (!gr) {
            pcell(g, c, cx0, yy, cw, ch, ID_MSMID, "MID", "The middle: what both speakers share");
            pcell(g, c, cx0 + cw + 8.f, yy, cw, ch, ID_MSSIDE, "SIDE", "The sides: what differs left to right");
        } else {
            pcell(g, c, cx0, yy, cw, ch, ID_MSWIDTH, "WIDTH", "0 % mono \xC2\xB7 100 % as it is \xC2\xB7 200 % twice as wide");
            ms_mono(g, c, cx0 + cw + 8.f, yy, cw, ch);
        }
        yy += ch + 12.f;
    }
}
static void draw_fxd(txg_t *g, frui_canvas *c)
{
    static const char *const TABN[TAB_N] = { "", "", "", "", "", "", "LFO 1", "LFO 2", "LFO 3", "LFO 4", "LFO 5", "LFO 6", "LFO 7",
                                             "LFO 8", "ENV \xC2\xB7 FOLLOW", "MATRIX" };
    static const char *const WHY[5] = { "The mix's compressor: the lanes together, before the drive.",
                                        "Saturation on the mix: how much, its colour.",
                                        "HIGH PASS then LOW PASS on the mix, at real cutoffs.",
                                        "The grain delay: echoes cut into grains, pitched, sprayed, frozen.",
                                        "The reverb: its space, its colour, its mix." };
    const float y0 = g->dr_y, y1 = g->fx_y, x1 = g->W;
    float by = y0, gw = floorf(g->W * 0.28f);
    int tab = g->fx_tab;
    char t[64];
    gw = gw < 280.f ? 280.f : gw > 520.f ? 520.f : gw;
    R(g, c, 0, y0, x1, y1 - y0, C_DRAW);
    R(g, c, 0, y0, x1, 1.f, C_LINE);
    wid_add(g, TXG_W_FXD, tab, 0, y0, x1, y1 - y0);
    if (tab >= SEC_MOVE) {               /* MOVEMENT: its tabs */
        float x = 16.f;
        for (int i = TAB_LFO0; i < TAB_N; i++) {
            float w = LBLW(g, 10.5f, TABN[i]) + 18.f;
            int on = i == tab, lit = i < TAB_ENV && (g->sn.ui.lfo_on >> (i - TAB_LFO0) & 1u);
            if (i == TAB_ENV) x += 10.f;
            RR(g, c, x, y0 + 8.f, w, 22.f, 2.f, on ? C_SEG : C_PANEL); RRS(g, c, x, y0 + 8.f, w, 22.f, 2.f, C_LINE);
            if (on) R(g, c, x + 1.f, y0 + 28.f, w - 2.f, 2.f, C_RED);
            LBL(g, c, x + 9.f, y0 + 19.f + CAP(g, 10.5f) * 0.5f, 10.5f, on ? C_INK : lit ? C_RED : C_DIM, TABN[i]);
            wid_add(g, TXG_W_FXTAB, i, x, y0 + 8.f, w, 22.f);
            x += w + 4.f;
        }
        {   /* CLEAR ALL MOVEMENT (at the right; MATRIX: left of MOD CC), asked first */
            int na = all_links(g);
            float rx = tab == TAB_MAT ? x1 - 16.f - 200.f - 12.f : x1 - 16.f, bh = 22.f, by0 = y0 + 8.f, lx;
            if (g->clr_ask && na) {
                char q[48];
                float wk = LBLW(g, 9.5f, "KEEP") + 20.f, wc = LBLW(g, 9.5f, "CLEAR") + 20.f, wq;
                snprintf(q, sizeof q, "Remove all %d link%s?", na, na > 1 ? "s" : "");
                wq = TW(g, 10.5f, 500.f, 0.f, 0, q);
                lx = rx - wk - 6.f - wc - 10.f - wq;
                TX(g, c, lx, by0 + bh * 0.5f + CAP(g, 10.5f) * 0.5f, 10.5f, 500.f, 0.f, 0, C_INK, q);
                RRS(g, c, rx - wk - 6.f - wc, by0, wc, bh, 2.f, C_RED);
                LBL(g, c, rx - wk - 6.f - wc + 10.f, by0 + bh * 0.5f + CAP(g, 9.5f) * 0.5f, 9.5f, C_RED, "CLEAR");
                wid_add(g, TXG_W_CLRYES, -1, rx - wk - 6.f - wc, by0, wc, bh);
                RRS(g, c, rx - wk, by0, wk, bh, 2.f, C_LINE);
                LBL(g, c, rx - wk + 10.f, by0 + bh * 0.5f + CAP(g, 9.5f) * 0.5f, 9.5f, C_INK, "KEEP");
                wid_add(g, TXG_W_CLRNO, -1, rx - wk, by0, wk, bh);
            } else {
                const char *ct = "CLEAR ALL MOVEMENT";
                float wb = LBLW(g, 9.5f, ct) + 20.f;
                int hot = na && g->hover_kind == TXG_W_CLRALL;
                lx = rx - wb;
                if (hot) RR(g, c, lx, by0, wb, bh, 2.f, C_REDBG);
                RRS(g, c, lx, by0, wb, bh, 2.f, na ? C_RED : frui_alpha(C_RED, 0.3f));
                LBL(g, c, lx + 10.f, by0 + bh * 0.5f + CAP(g, 9.5f) * 0.5f, 9.5f, na ? C_RED : frui_alpha(C_RED, 0.3f), ct);
                if (na) wid_add(g, TXG_W_CLRALL, -1, lx, by0, wb, bh);
            }
            if (tab == TAB_MAT) {
                const char *mt = "A source moves a target by DEPTH; SPREAD offsets each lane's.";
                if (x + 14.f + TW(g, 10.5f, 450.f, 0.f, 0, mt) < lx - 12.f)
                    TX(g, c, x + 14.f, y0 + 19.f + CAP(g, 10.5f) * 0.5f, 10.5f, 450.f, 0.f, 0, C_DIM, mt);
                pcell(g, c, x1 - 16.f - 200.f, y0 + 5.f, 200.f, 26.f, TXP_ID_MODCC, "MOD CC", 0);
            }
        }
        by = y0 + FXD_TABS;
    }
    if (tab == SEC_MS) draw_ms_fxd(g, c, by, x1);
    else if (tab < SEC_MOVE) {           /* an effect: its picture, its values in groups */
        static const grp_t G[5][4] = {
            { { "LEVEL", 2, { ID_CTHR, G_ID(31) } }, { "TIME", 2, { G_ID(28), G_ID(29) } }, { "SHAPE", 2, { ID_CRATIO, ID_CON } } },
            { { "AMOUNT", 2, { ID_DAMT, ID_DMIX } }, { "COLOUR", 2, { ID_DTONE, ID_DTYPE } } },
            { { "CUTOFF", 2, { ID_FHP, ID_FLP } }, { "SHAPE", 2, { ID_FRES, ID_FSLOPE } } },
            { { "TIME", 4, { ID_GTIME, ID_GFB, G_ID(12), G_ID(11) } }, { "GRAINS", 5, { G_ID(3), G_ID(4), G_ID(5), G_ID(8), G_ID(7) } },
              { "COLOUR", 6, { G_ID(6), G_ID(15), G_ID(9), G_ID(10), G_ID(13), G_ID(14) } }, { "MIX", 1, { ID_GMIX } } },
            { { "SPACE", 3, { G_ID(21), ID_RDECAY, G_ID(20) } }, { "COLOUR", 5, { G_ID(19), G_ID(23), G_ID(24), G_ID(25), G_ID(22) } },
              { "TYPE \xC2\xB7 MIX", 2, { ID_RTYPE, ID_RMIX } } } };
        static const int NG[5] = { 3, 2, 2, 4, 3 }, ROWS[5] = { 2, 2, 2, 3, 3 };
        fxd_title(g, c, 16.f, by + 12.f, gw, SEC_NAME[tab], C_INK, WHY[tab]);
        graph_big(g, c, tab, 16.f, by + 38.f, gw, y1 - 12.f - (by + 38.f), sec_on(g, tab));
        groups(g, c, G[tab], NG[tab], ROWS[tab], gw + 40.f, by + 12.f, x1 - gw - 56.f, y1 - 12.f - (by + 12.f));
    } else if (tab < TAB_ENV) {          /* an LFO: its trace, NOW, its routes; its values in groups */
        int n = tab - TAB_LFO0, nl = src_links(g, n + 1), nr = 0;
        float yy;
        char cn[32];
        grp_t G[3] = { { "WAVE", 4, { ID_LFO(n, LP_SHAPE), ID_LFO(n, LP_WIDTH), ID_LFO(n, LP_PHASE), ID_LFO(n, LP_JITTER) } },
                       { "TIME", 4, { ID_LFO(n, LP_RATE), ID_LFO(n, LP_SYNC), ID_LFO(n, LP_FADE), ID_LFO(n, LP_RESTART) } },
                       { "OUTPUT", 2, { ID_LFO(n, LP_MODE), ID_LFO(n, LP_LEVEL) } } };
        snprintf(t, sizeof t, "LFO %d", n + 1);
        src_count(g, n + 1, cn, sizeof cn);
        fxd_title(g, c, 16.f, by + 12.f, gw - 70.f, t, nl ? C_RED : C_INK, cn);
        snprintf(t, sizeof t, "NOW %+d %%", (int)lroundf((float)g->sn.ui.lfo[n] * 100.f / 32767.f));   /* (read only) */
        LBL(g, c, 16.f + gw - TW(g, 10.f, 620.f, 1.4f, 0, t), by + 22.f, 10.f, nl ? C_RED : C_DIM, t);
        src_well(g, c, n + 1, 16.f, by + 32.f, gw, 64.f);
        yy = by + 112.f;
        for (int s = 0; s < 16 && nr < 3; s++) {   /* what it moves (the matrix's slots) */
            int it = txp_param_index(ID_MAT(s, 1));
            if (!mx_link(g, s) || mx(g, s, 0) != n + 1 || it < 0) continue;
            snprintf(t, sizeof t, "\xE2\x86\x92  %s  %+d%%", g->sn.ptx[it], mx(g, s, 2) - 100);
            clipL(g, c, 16.f, yy - 12.f + 16.f * (float)nr, gw, 16.f);
            TX(g, c, 16.f, yy + 16.f * (float)nr, 11.f, 500.f, 0.f, FRUI_TNUM, C_RED, t);
            frui_clip_reset(c);
            nr++;
        }
        if (!nr) {
            clipL(g, c, 16.f, yy - 12.f, gw, 32.f);
            TX(g, c, 16.f, yy, 11.f, 450.f, 0.f, 0, C_DIM, "Click the tile for its mode, or drag its grip");
            TX(g, c, 16.f, yy + 16.f, 11.f, 450.f, 0.f, 0, C_DIM, "onto a control.");
            frui_clip_reset(c);
        }
        groups(g, c, G, 3, 2, gw + 40.f, by + 12.f, x1 - gw - 56.f, y1 - 12.f - (by + 12.f));
    } else if (tab == TAB_ENV) {         /* the envelopes and the followers: a group each */
        grp_t G[4];
        static const char *const M[4] = { "ENV 1", "ENV 2", "FOLLOW 1", "FOLLOW 2" };
        for (int m = 0; m < 4; m++) {
            G[m].t = M[m]; G[m].n = m < 2 ? 8 : 4;
            for (int p = 0; p < G[m].n; p++) G[m].id[p] = 6u << 8 | (uint32_t)m * 16u | (uint32_t)p;
        }
        {   /* (each source's tile over its group: a click its mode, its grip) */
            float gx[4], gwd[4];
            groups_x(G, 4, 4, 16.f, x1 - 32.f, gx, gwd);
            for (int m = 0; m < 4; m++) src_tile_draw(g, c, 16 + m, gx[m], by + 12.f, gwd[m], 40.f);
        }
        groups(g, c, G, 4, 4, 16.f, by + 12.f + 48.f, x1 - 32.f, y1 - 12.f - (by + 12.f + 48.f));
    } else {                             /* the matrix: sixteen slots, SOURCE TARGET DEPTH SPREAD */
        float colw = (x1 - 48.f) / 2.f, rh = 30.f;
        for (int s = 0; s < 16; s++) {   /* (the columns named once over each half; the cells their values alone) */
            float cx = 16.f + (colw + 16.f) * (float)(s / 8), cy = by + 26.f + rh * (float)(s % 8), w1 = 52.f,
                  ws = (colw - w1 - 3.f * 6.f) * 0.27f, wt = (colw - w1 - 3.f * 6.f) * 0.39f, wd = (colw - w1 - 3.f * 6.f) * 0.17f;
            int is = txp_param_index(ID_MAT(s, 0)), it = txp_param_index(ID_MAT(s, 1)), on = is >= 0 && it >= 0 && g->sn.pv[is] && g->sn.pv[it];
            if (s % 8 == 0) {
                LBL(g, c, cx + w1 + 9.f, cy - 8.f, 9.5f, C_DIM, "SOURCE");
                LBL(g, c, cx + w1 + ws + 15.f, cy - 8.f, 9.5f, C_DIM, "TARGET");
                LBL(g, c, cx + w1 + ws + wt + 21.f, cy - 8.f, 9.5f, C_DIM, "DEPTH");
                LBL(g, c, cx + w1 + ws + wt + wd + 27.f, cy - 8.f, 9.5f, C_DIM, "SPREAD");
            }
            snprintf(t, sizeof t, "MOD %d", s + 1);
            LBL(g, c, cx, cy + (rh - 6.f) * 0.5f + CAP(g, 10.f) * 0.5f, 10.f, on ? C_RED : C_DIM, t);
            pcell(g, c, cx + w1, cy, ws, rh - 6.f, ID_MAT(s, 0), "", 0);
            pcell(g, c, cx + w1 + ws + 6.f, cy, wt, rh - 6.f, ID_MAT(s, 1), "", 0);
            pcell(g, c, cx + w1 + ws + wt + 12.f, cy, wd, rh - 6.f, ID_MAT(s, 2), "", 0);
            pcell(g, c, cx + w1 + ws + wt + wd + 18.f, cy, wd, rh - 6.f, ID_MAT(s, 3), "", 0);
        }
    }
}
/* ---- SAVE: the scene that plays into a user preset U01 .. U16, named; kept with the project. It opens on the first
 * free one with a name drawn (txp_ui_name: two words, never a user preset's name); a used one is replaced only when
 * chosen, the line under the name says so in red; all 16 used: on the one loaded, else U01 */
static int save_all(const txg_t *g) { return (g->sn.ui.user & ((1u << TXP_USER_N) - 1u)) == (1u << TXP_USER_N) - 1u; }
static int save_line(txg_t *g, char *t, size_t n, float room)   /* the line under the name: what SAVE does; 1: red */
{
    int k = g->sv_slot, busy = g->sn.ui.saving || g->sv_wait > 0;
    if (busy) { snprintf(t, n, "SAVING U%02d ...", k + 1); return 1; }
    if (!(g->sn.ui.user >> k & 1u)) { snprintf(t, n, "U%02d IS EMPTY: SAVE MAKES A NEW PRESET THERE", k + 1); return 0; }
    if (save_all(g)) {
        snprintf(t, n, "ALL 16 ARE USED: SAVE REPLACES U%02d \"%s\"", k + 1, g->sn.ui.uname[k]);
        if (LBLW(g, 9.5f, t) > room) snprintf(t, n, "ALL 16 ARE USED: SAVE REPLACES U%02d", k + 1);
    } else {
        snprintf(t, n, "U%02d HOLDS \"%s\": SAVE REPLACES IT", k + 1, g->sn.ui.uname[k]);
        if (LBLW(g, 9.5f, t) > room) snprintf(t, n, "U%02d IS USED: SAVE REPLACES IT", k + 1);
    }
    return 1;
}
static float save_px(const txg_t *g) { float x = g->sv_x - 160.f; return x + 420.f > g->W - 8.f ? g->W - 428.f : x; }
static void draw_save(txg_t *g, frui_canvas *c)
{
    const float w = 420.f, h = 276.f, y = TOP_H + 2.f;
    float x = save_px(g), sw = (w - 32.f - 3.f * 6.f) / 4.f;
    char t[64];
    int fit = 1;
    wid_add(g, TXG_W_PICKER, -2, x, y, w, h);
    frui_rrect_shadow(c, x * g->S, y * g->S, w * g->S, h * g->S, 4.f * g->S, 16.f * g->S, 0x90000000u);
    RR(g, c, x, y, w, h, 4.f, C_BTN); RRS(g, c, x, y, w, h, 4.f, C_DIM);
    fit &= LBL(g, c, x + 16.f, y + 24.f, 12.f, C_INK, "SAVE AS A USER PRESET") <= w - 32.f;
    fit &= TX(g, c, x + 16.f, y + 42.f, 11.f, 450.f, 0.f, 0, C_DIM, "Kept with the project; load it in PRESETS: USER.") <= w - 32.f;
    for (int i = 0; i < (int)TXP_USER_N; i++) {
        float sx = x + 16.f + (sw + 6.f) * (float)(i % 4), sy = y + 54.f + 34.f * (float)(i / 4);
        int u = (int)(g->sn.ui.user >> i & 1u), on = i == g->sv_slot;
        RR(g, c, sx, sy, sw, 28.f, 2.f, on ? C_SEG : C_WELL); RRS(g, c, sx, sy, sw, 28.f, 2.f, on ? C_RED : C_LINE);
        snprintf(t, sizeof t, "U%02d", i + 1);
        TX(g, c, sx + 6.f, sy + 12.f, 10.f, 500.f, 0.f, FRUI_TNUM, on ? C_RED : C_DIM, t);
        clipL(g, c, sx, sy, sw - 4.f, 28.f);
        fit &= TX(g, c, sx + 6.f, sy + 24.f, 10.f, 500.f, 0.f, 0, u ? C_INK : C_FAINT, u ? g->sn.ui.uname[i] : "empty") <= sw - 10.f;
        frui_clip_reset(c);
        wid_add(g, TXG_W_SLOT, i, sx, sy, sw, 28.f);
    }
    {
        const float dw = 22.f;           /* (the dice beside the name, as the window's dice) */
        float ny = y + 210.f, nw = w - 32.f - 100.f - dw - 6.f, dx = x + 16.f + nw + 6.f;
        int red;
        fit &= LBL(g, c, x + 16.f, ny - 4.f, 9.5f, C_DIM, "NAME (TYPE IT, OR THE DICE FOR ANOTHER)") <= w - 32.f;
        RR(g, c, x + 16.f, ny, nw, 26.f, 3.f, C_WELL); RRS(g, c, x + 16.f, ny, nw, 26.f, 3.f, C_DIM);
        {
            float bx = x + 26.f, by = ny + 13.f + CAP(g, 12.f) * 0.5f, tw = TW(g, 12.f, 500.f, 0.6f, 0, g->sv_name);
            if (!g->sv_typed && g->sv_name[0]) R(g, c, bx - 3.f, ny + 5.f, tw + 6.f, 16.f, C_SEG);   /* (selected: a key
                                          * typed replaces it) */
            tw = TX(g, c, bx, by, 12.f, 500.f, 0.6f, 0, C_INK, g->sv_name);
            R(g, c, bx + 1.f + tw, ny + 6.f, 1.2f, 14.f, C_RED);
            fit &= bx + tw + 6.f <= x + 16.f + nw;
        }
        wid_add(g, TXG_W_NAME, -1, x + 16.f, ny, nw, 26.f);
        {
            int hot = g->hover_kind == TXG_W_NAMEDICE;
            RR(g, c, dx, ny + 2.f, dw, dw, 3.f, hot ? C_SEG : C_BTN); RRS(g, c, dx, ny + 2.f, dw, dw, 3.f, hot ? C_DIM : C_LINE);
            dice_icon(g, c, dx + dw * 0.5f, ny + 13.f, 14.f, C_INK);
            wid_add(g, TXG_W_NAMEDICE, -1, dx, ny + 2.f, dw, dw);
        }
        button(g, c, x + w - 16.f - 90.f, ny, 90.f, 26.f, "SAVE", 1);
        wid_add(g, TXG_W_SAVEGO, -1, x + w - 16.f - 90.f, ny, 90.f, 26.f);
        red = save_line(g, t, sizeof t, w - 32.f);
        fit &= LBL(g, c, x + 16.f, ny + 46.f, 9.5f, red ? C_RED : C_DIM, t) <= w - 32.f;
    }
    g->sv_fit = fit;
}
/* RANDOM's list: under the top bar's dice, right-aligned to it. Four ticks (txp_ui_dice: kept with the project), CLOSE,
 * ROLL (red: txp_ui_roll), what a roll keeps. Esc, a click outside or CLOSE close it */
#define RND_W 300.f
static const char *const RND_N[4] = { "ENGINES", "SOUND", "EFFECTS", "MOVEMENT" };
static const char *const RND_D[4] = { "a new engine on every lane", "each engine's controls",
                                      "COMP to M/S: types and amounts; long, unreal reverbs can come up",
                                      "LFOs, ENVs, FOLLOWs and where they go" };
static const char RND_KEEP[] = "Kept as they are: LANES, each lane's LEVEL, PAN, HOW MANY, HOW ACTIVE, MIDI CH. An "
                               "empty lane gets a sound with ENGINES. Levels stay even.";
static int wrap_lines(txg_t *g, float w, const char *s, float lh, float sz)   /* the lines s needs in w */
{
    int l = 1;
    while (l < 8 && wrapn(g, 0, 0.f, 0.f, w, s, l, lh, sz, C_INK)) l++;
    return l;
}
static void draw_rnd(txg_t *g, frui_canvas *c)
{
    const float w = RND_W, pad = 14.f, iw = w - 2.f * pad, dw = iw - 28.f, lt = 12.f * 1.2f, ld = 15.5f, lk = 15.f;
    top_r_t tr = top_r(g);
    float x = tr.dx + TOP_DICE_W - w, y = 12.f + 28.f + 9.f, h, cy, bw = (iw - 8.f) * 0.5f;
    int on = txp_ui_dice(g->c), n[4];
    h = pad + 15.f + 17.f + 10.f;
    for (int i = 0; i < 4; i++) { n[i] = wrap_lines(g, dw, RND_D[i], ld, 11.5f); h += 7.f + lt + 3.f + ld * (float)n[i] + 4.f; }
    h += 10.f + 30.f + 9.f + lk * (float)wrap_lines(g, iw, RND_KEEP, lk, 11.f) + 12.f - 4.f;
    wid_add(g, TXG_W_PICKER, -3, x, y, w, h);
    frui_rrect_shadow(c, x * g->S, y * g->S, w * g->S, h * g->S, 6.f * g->S, 16.f * g->S, 0xAA000000u);
    RR(g, c, x, y, w, h, 6.f, C_MENU); RRS(g, c, x, y, w, h, 6.f, 0xFF34312Fu);
    cy = y + pad;
    LBL(g, c, x + pad, cy + CAP(g, 11.f), 11.f, C_DIM, "RANDOM");
    cy += 15.f;
    clipL(g, c, x + pad, cy, iw, 17.f);   /* (the window's message here while the list is open: it covers the line) */
    if (g->msg_t > 0) TX(g, c, x + pad, cy + CAP(g, 12.f), 12.f, 500.f, 0.f, 0, C_RED, g->msg);
    else TX(g, c, x + pad, cy + CAP(g, 12.f), 12.f, 450.f, 0.f, 0, C_DIM, "One roll, everything ticked. UNDO takes it back.");
    frui_clip_reset(c);
    cy += 17.f + 10.f;
    for (int i = 0; i < 4; i++) {
        float oh = 7.f + lt + 3.f + ld * (float)n[i] + 4.f, bx = x + pad, by = cy + 8.f;
        int t = on >> i & 1, hot = g->hover_kind == TXG_W_RNDTICK && g->hover_lane == i;
        R(g, c, x + pad, cy, iw, 1.f, 0xFF2E2B2Au);
        if (t) {
            RR(g, c, bx, by, 16.f, 16.f, 3.f, C_RED);
            LN(g, c, bx + 4.f, by + 8.f, bx + 7.f, by + 11.f, 2.f, 0xFFFFFFFFu);
            LN(g, c, bx + 7.f, by + 11.f, bx + 12.5f, by + 5.f, 2.f, 0xFFFFFFFFu);
        } else RRS(g, c, bx, by, 16.f, 16.f, 3.f, hot ? C_INK : C_DIM);
        TX(g, c, bx + 28.f, cy + 7.f + CAP(g, 12.f), 12.f, 650.f, 12.f * 0.08f, 0, C_INK, RND_N[i]);
        (void)wrapn(g, c, bx + 28.f, cy + 7.f + lt + 3.f + CAP(g, 11.5f), dw, RND_D[i], n[i], ld, 11.5f, C_DIM);
        wid_add(g, TXG_W_RNDTICK, i, x + pad, cy, iw, oh);
        cy += oh;
    }
    cy += 10.f - 4.f;
    {
        int hc = g->hover_kind == TXG_W_CLOSE && g->hover_lane == -3, hr = g->hover_kind == TXG_W_RNDGO;
        float lw;
        RR(g, c, x + pad, cy, bw, 30.f, 4.f, hc ? C_SEG : C_BTN);
        lw = LBLW(g, 12.f, "CLOSE");
        LBL(g, c, x + pad + (bw - lw) * 0.5f, cy + 15.f + CAP(g, 12.f) * 0.5f, 12.f, C_INK, "CLOSE");
        wid_add(g, TXG_W_CLOSE, -3, x + pad, cy, bw, 30.f);
        RR(g, c, x + pad + bw + 8.f, cy, bw, 30.f, 4.f, hr ? 0xFFF25D50u : C_RED);
        lw = LBLW(g, 12.f, "ROLL");
        LBL(g, c, x + pad + bw + 8.f + (bw - lw) * 0.5f, cy + 15.f + CAP(g, 12.f) * 0.5f, 12.f, 0xFFFFFFFFu, "ROLL");
        wid_add(g, TXG_W_RNDGO, -1, x + pad + bw + 8.f, cy, bw, 30.f);
    }
    cy += 30.f + 9.f;
    (void)wrapn(g, c, x + pad, cy + CAP(g, 11.f), iw, RND_KEEP, 8, lk, 11.f, C_DIM);
}
/* the dice's hints, under them while hovered: a row's, the top bar's (its list closed) */
static void dice_tip(txg_t *g, frui_canvas *c)
{
    char t[48];
    const char *s2;
    float r[4], w, x, y;
    if (g->pick >= 0 || g->menu_on) return;
    if (g->hover_kind == TXG_W_DICE && g->ov != OV_RND && txg_find(g, TXG_W_DICE, g->hover_lane, r)) {
        snprintf(t, sizeof t, "Random engine on lane %d", g->hover_lane + 1);
        s2 = "  \xC2\xB7  UNDO takes it back";
    } else if (g->hover_kind == TXG_W_RND && txg_find(g, TXG_W_RND, -1, r)) {
        snprintf(t, sizeof t, "Roll");
        s2 = "  \xC2\xB7  what the arrow has ticked; UNDO takes it back";
    } else if (g->hover_kind == TXG_W_RNDMENU && g->ov != OV_RND && txg_find(g, TXG_W_RNDMENU, -1, r)) {
        snprintf(t, sizeof t, "What a roll changes");
        s2 = "  \xC2\xB7  engines, sound, effects, movement";
    } else return;
    w = TW(g, 11.5f, 450.f, 0.f, 0, t) + TW(g, 11.5f, 450.f, 0.f, 0, s2) + 18.f;
    x = r[0]; y = r[1] + r[3] + 6.f;
    if (x + w > g->W - 10.f) x = g->W - 10.f - w;
    RR(g, c, x, y, w, 24.f, 4.f, 0xFF000000u); RRS(g, c, x, y, w, 24.f, 4.f, 0xFF3A3736u);
    x += 9.f + TX(g, c, x + 9.f, y + 12.f + CAP(g, 11.5f) * 0.5f, 11.5f, 450.f, 0.f, 0, C_INK, t);
    TX(g, c, x, y + 12.f + CAP(g, 11.5f) * 0.5f, 11.5f, 450.f, 0.f, 0, C_DIM, s2);
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
/* a source's mode: its bar under the top bar (the content under it): "LFO 1 MODE", what to do (or the message), CLEAR
 * LFO 1, DONE */
static void draw_mbar(txg_t *g, frui_canvas *c)
{
    const float y = TOP_H, h = MB_H, yc = y + h * 0.5f;
    char nm[16], t[64], hint[112];
    float x = 18.f, rx = g->W - 16.f, wd, wc = 0.f;
    int n = src_links(g, g->msrc);
    src_name(g->msrc, nm, sizeof nm);
    R(g, c, 0, y, g->W, h, C_RED);
    wid_add(g, TXG_W_MBAR, g->msrc, 0, y, g->W, h);
    snprintf(t, sizeof t, "%s MODE", nm);
    x += LBL(g, c, x, yc + CAP(g, 12.f) * 0.5f, 12.f, 0xFFFFFFFFu, t) + 10.f;
    x += TX(g, c, x, yc + CAP(g, 12.f) * 0.5f, 12.f, 700.f, 0.f, 0, 0xD9FFFFFFu, "\xC2\xB7") + 10.f;
    /* DONE, CLEAR <SOURCE> (only while it moves something) at the right */
    wd = LBLW(g, 10.f, "DONE") + 24.f;
    RRS(g, c, rx - wd, y + 6.f, wd, h - 12.f, 3.f, g->hover_kind == TXG_W_MDONE ? 0xFFFFFFFFu : 0x99FFFFFFu);
    LBL(g, c, rx - wd + 12.f, yc + CAP(g, 10.f) * 0.5f, 10.f, 0xFFFFFFFFu, "DONE");
    wid_add(g, TXG_W_MDONE, g->msrc, rx - wd, y + 6.f, wd, h - 12.f);
    snprintf(t, sizeof t, "CLEAR %s", nm);   /* (nothing to clear: dim) */
    wc = LBLW(g, 10.f, t) + 24.f;
    RRS(g, c, rx - wd - 8.f - wc, y + 6.f, wc, h - 12.f, 3.f, !n ? 0x4DFFFFFFu : g->hover_kind == TXG_W_MCLEAR ? 0xFFFFFFFFu : 0x99FFFFFFu);
    LBL(g, c, rx - wd - 8.f - wc + 12.f, yc + CAP(g, 10.f) * 0.5f, 10.f, n ? 0xFFFFFFFFu : 0x4DFFFFFFu, t);
    if (n) wid_add(g, TXG_W_MCLEAR, g->msrc, rx - wd - 8.f - wc, y + 6.f, wc, h - 12.f);
    wc += 8.f;
    snprintf(hint, sizeof hint, "Drag a control to set how far %s moves it. Double-click removes.", nm);
    clipL(g, c, x, y, rx - wd - wc - 12.f - x, h);
    TX(g, c, x, yc + CAP(g, 12.f) * 0.5f, 12.f, 500.f, 0.f, 0, 0xF2FFFFFFu, hint);
    frui_clip_reset(c);
}
/* the message: a line over the rows' head (~5 s) */
static void draw_msg(txg_t *g, frui_canvas *c)
{
    float x = rcols(g).tl - 8.f, y = g->ct + 2.f, w = TW(g, 11.f, 500.f, 0.f, 0, g->msg) + 24.f, h = HDR_H - 4.f;
    if (w > g->W - x - 12.f) w = g->W - x - 12.f;
    RR(g, c, x, y, w, h, 3.f, C_BTN); RRS(g, c, x, y, w, h, 3.f, C_RED);
    clipL(g, c, x, y, w - 8.f, h);
    TX(g, c, x + 12.f, y + h * 0.5f + CAP(g, 11.f) * 0.5f, 11.f, 500.f, 0.f, 0, C_INK, g->msg);
    frui_clip_reset(c);
    wid_add(g, TXG_W_MSG, 1, x, y, w, h);
}
/* which controls the mode's source can move: those that cannot dim (marked TXG_W_NOMOVE), those it does not move yet
 * get a thin red outline (those it moves are red already). The matrix's own cells, MOD CC: as they are */
static int mode_skip(uint32_t id) { return (id >> 8) == 7u || id >= TXP_ID_OWN0; }
static void mode_marks(txg_t *g, frui_canvas *c)
{
    int n = g->nw;
    for (int i = 0; i < n; i++) {
        const wid_t w = g->w[i];
        uint32_t id = w.id;
        if (w.kind == TXG_W_VOL) id = TXP_ID_LANE(w.lane, TXP_PL_LEVEL);
        else if (w.kind != TXG_W_PARAM && w.kind != TXG_W_MANY && w.kind != TXG_W_ACT) continue;
        if (!id || mode_skip(id)) continue;
        if (!ctl_tgt(g, id, 0)) {
            R(g, c, w.r[0], w.r[1], w.r[2], w.r[3], frui_alpha(C_PANEL, 0.7f));
            wid_add2(g, TXG_W_NOMOVE, (int)id, id, 0, w.r[0], w.r[1], w.r[2], w.r[3]);
        } else if (!link_amt(g, g->msrc, id)) RRS(g, c, w.r[0], w.r[1], w.r[2], w.r[3], 2.f, frui_alpha(C_RED, 0.35f));
    }
}
/* the right-click menu: MOVE WITH the sources (a click: that source's mode, a link at +25 % if it had none), REMOVE
 * MOVEMENT; a control no source can move: why */
#define MN_W 230.f
#define MN_IH 22.f
static float menu_h(const txg_t *g, int tgt) { (void)g; return tgt ? 4.f + MN_IH * (float)SRC_N + 9.f + MN_IH + 4.f : 4.f + MN_IH + 4.f; }
static void draw_menu(txg_t *g, frui_canvas *c)
{
    int why, T = ctl_tgt(g, g->menu_id, &why);
    float h = menu_h(g, T), x = g->menu_x, y = g->menu_y;
    char nm[48], t[96], sn[16];
    x = x + MN_W > g->W - 8.f ? g->W - 8.f - MN_W : x;
    y = y + h > g->H - 8.f ? g->H - 8.f - h : y;
    y = y < TOP_H + 4.f ? TOP_H + 4.f : y;
    wid_add(g, TXG_W_MENU, -1, x, y, MN_W, h);
    frui_rrect_shadow(c, x * g->S, y * g->S, MN_W * g->S, h * g->S, 4.f * g->S, 14.f * g->S, 0x90000000u);
    RR(g, c, x, y, MN_W, h, 5.f, C_MENU); RRS(g, c, x, y, MN_W, h, 5.f, C_LINE);
    ctl_name(g, g->menu_id, nm, sizeof nm);
    clipL(g, c, x, y, MN_W - 6.f, h);
    if (!T) {                            /* (why, on hover too: the refusal's message) */
        (void)why;
        snprintf(t, sizeof t, "%s cannot be moved", nm);
        TX(g, c, x + 14.f, y + 4.f + MN_IH * 0.5f + CAP(g, 11.5f) * 0.5f, 11.5f, 500.f, 0.f, 0, C_FAINT, t);
        frui_clip_reset(c);
        return;
    }
    for (int i = 0; i < SRC_N; i++) {
        float iy = y + 4.f + MN_IH * (float)i;
        int a = link_amt(g, SRCS[i], g->menu_id), hot = g->hover_kind == TXG_W_MENU && g->hover_lane == i;
        if (hot) RR(g, c, x + 4.f, iy, MN_W - 8.f, MN_IH, 3.f, 0xFF34312Fu);
        src_name(SRCS[i], sn, sizeof sn);
        snprintf(t, sizeof t, "Move with %s", sn);
        TX(g, c, x + 14.f, iy + MN_IH * 0.5f + CAP(g, 11.5f) * 0.5f, 11.5f, 500.f, 0.f, 0, C_INK, t);
        if (a) {
            snprintf(t, sizeof t, "%+d%%", a);
            TX(g, c, x + MN_W - 14.f - TW(g, 11.5f, 500.f, 0.f, FRUI_TNUM, t), iy + MN_IH * 0.5f + CAP(g, 11.5f) * 0.5f, 11.5f, 500.f, 0.f, FRUI_TNUM, C_DIM, t);
        }
        wid_add(g, TXG_W_MENU, i, x + 4.f, iy, MN_W - 8.f, MN_IH);
    }
    {
        float sy = y + 4.f + MN_IH * (float)SRC_N + 4.f, iy = sy + 5.f;
        lk_t lk[MX_N];
        int nl = ctl_links(g, g->menu_id, lk), own = 0, hot = g->hover_kind == TXG_W_MENU && g->hover_lane == SRC_N;
        for (int i = 0; i < nl; i++) own += !lk[i].all;
        R(g, c, x + 8.f, sy, MN_W - 16.f, 1.f, C_LINE);
        if (hot && own) RR(g, c, x + 4.f, iy, MN_W - 8.f, MN_IH, 3.f, 0xFF34312Fu);
        TX(g, c, x + 14.f, iy + MN_IH * 0.5f + CAP(g, 11.5f) * 0.5f, 11.5f, 500.f, 0.f, 0, own ? C_INK : C_FAINT, "Remove movement");
        if (own) wid_add(g, TXG_W_MENU, SRC_N, x + 4.f, iy, MN_W - 8.f, MN_IH);
    }
    frui_clip_reset(c);
}
/* a grip dragged: the source's name at the pointer, the control under it outlined (red: it can be moved) */
static void draw_ghost(txg_t *g, frui_canvas *c)
{
    char nm[16];
    float w;
    if (g->gover) {
        int T = ctl_tgt(g, g->gover, 0);
        RR(g, c, g->gover_r[0], g->gover_r[1], g->gover_r[2], g->gover_r[3], 2.f, T ? frui_alpha(C_RED, 0.14f) : frui_alpha(C_PANEL, 0.6f));
        RRS(g, c, g->gover_r[0], g->gover_r[1], g->gover_r[2], g->gover_r[3], 2.f, T ? C_RED : C_DIM);
    }
    src_name(g->drag_lane, nm, sizeof nm);
    w = LBLW(g, 10.f, nm) + 20.f;
    RR(g, c, g->gx + 12.f, g->gy + 12.f, w, 22.f, 3.f, C_RED);
    LBL(g, c, g->gx + 22.f, g->gy + 23.f + CAP(g, 10.f) * 0.5f, 10.f, 0xFFFFFFFFu, nm);
}
/* ---- HELP: the top bar's ? starts the tour; its last card opens the online manual in the system's browser (gui_platform.h
 * frg_open_url) */
#define TXG_MANUAL_URL "https://github.com/leonardoMirabal/textura/blob/main/docs/PLUGIN-MANUAL.md"
static void manual_open(txg_t *g)         /* (a test run: said, never opened) */
{
    if (txp_ui_test(g->c)) say(g, "The manual opens in your browser (not in a test run).");
    else if (frg_open_url(TXG_MANUAL_URL)) say(g, "No browser could be opened: " TXG_MANUAL_URL);
    else say(g, "The manual opens in your browser.");
}
/* ---- the tour: eight steps in the order a place is made (a preset, the lanes, the place, the lane's controls, the
 * effects, the movement, the dice, HELP). Each step lights its part of the window (from the controls as drawn: any
 * size) and dims the rest; its card goes under the part, else over it, else beside it, always inside the window. NEXT
 * (Right, Return), BACK (Left), SKIP (Esc); DONE on the last. SKIP and DONE write the flag (txg_first_run) */
#define TOUR_N 8
#define TC_W 300.f                       /* the card */
#define TC_PAD 16.f
#define TC_LH 18.75f                     /* (its text's line: 12.5 x 1.5) */
#define TC_GAP 14.f                      /* (the card to the part it is about) */
static const char *const TOUR_T[TOUR_N] = { "Start from a place", "Lanes: one source each", "Place them",
                                            "Shape the selected lane", "The effects, in signal order", "Movement",
                                            "Roll the dice", "Help is here" };
static const char *const TOUR_P[TOUR_N] = {
    "Pick a preset with the arrows or the list. Each is a whole place: up to sixteen lanes of sound, the effects and the "
    "movement.",
    "Every lane plays one sound source. HOW MANY: how many play at once. HOW ACTIVE: how busy they are. VOL, PAN, M and "
    "S as on a mixer.",
    "Drag a lane's dot: left and right, near and far. Far sounds are quieter and darker, as outdoors.",
    "The lane you clicked shows its controls here, each named for what it does to the sound, with a line saying how.",
    "COMP, DRIVE, FILTER, DELAY, REVERB, M/S, left to right as the sound goes through them. ALL opens one large.",
    "LFOs, envelopes and followers move controls over time. ALL opens them; a control that moves shows it in red.",
    "A lane's dice gives it a new engine. The dice at the top right rolls the whole place; its arrow picks what a roll "
    "changes. UNDO takes a roll back.",
    "HELP starts this tour again. MANUAL opens the full manual in your browser. Enjoy the places." };
static int tour_rect(txg_t *g, int i, float o[4])   /* the part step i lights, from the controls as drawn now */
{
    float a[4], b[4];
    switch (i) {
    case 0:                              /* the scene: from the previous to the next button */
        if (!txg_find(g, TXG_W_PREV, -1, a) || !txg_find(g, TXG_W_NEXT, -1, b)) return 0;
        o[0] = a[0] - 6.f; o[1] = a[1] - 6.f; o[2] = b[0] + b[2] + 6.f - o[0]; o[3] = a[3] + 12.f;
        return 1;
    case 1: {                            /* the rows: their names and the lanes in sight */
        int n = g->nlane - g->rtop < g->rvis ? g->nlane - g->rtop : g->rvis;
        if (!txg_find(g, TXG_W_ROWS, -1, a)) return 0;
        o[0] = a[0]; o[1] = a[1] - HDR_H; o[2] = a[2]; o[3] = fminf(HDR_H + ROW_H * (float)(n > 0 ? n : 1) + 4.f, a[3] + HDR_H);
        return 1;
    }
    case 2:                              /* the place */
        if (!txg_find(g, TXG_W_PLACE, -1, a)) return 0;
        memcpy(o, a, sizeof a);
        return 1;
    case 3:                              /* the drawer: its name and cards (draw_drawer: PAN .. CYCLE from W - 182) */
        o[0] = 0.f; o[1] = g->dr_y; o[2] = g->W - 182.f - 6.f; o[3] = g->dr_h;
        return 1;
    case 4: case 5:                      /* the strip: the effects, then MOVEMENT */
        if (!txg_find(g, TXG_W_FXOPEN, SEC_MOVE, a)) return 0;
        o[1] = g->fx_y; o[3] = FX_H;
        if (i == 4) { o[0] = 0.f; o[2] = a[0] - 1.f; } else { o[0] = a[0] - 1.f; o[2] = g->W - o[0]; }
        return 1;
    case 6: {                            /* the rows' dice (none: the top bar's) */
        int n = 0;
        for (int k = g->rtop; k < g->nlane && k < g->rtop + g->rvis; k++) {
            if (!txg_find(g, TXG_W_DICE, k, b)) continue;
            if (!n++) memcpy(a, b, sizeof a);
            else { float y1 = fmaxf(a[1] + a[3], b[1] + b[3]); a[1] = fminf(a[1], b[1]); a[3] = y1 - a[1]; }
        }
        if (!n && !txg_find(g, TXG_W_RND, -1, a)) return 0;
        o[0] = a[0] - 6.f; o[1] = a[1] - 7.f; o[2] = a[2] + 12.f; o[3] = a[3] + 14.f;
        return 1;
    }
    case 7:                              /* HELP */
        if (!txg_find(g, TXG_W_HELP, -1, a)) return 0;
        o[0] = a[0] - 7.f; o[1] = a[1] - 7.f; o[2] = a[2] + 14.f; o[3] = a[3] + 14.f;
        return 1;
    }
    return 0;
}
static float tour_card_h(txg_t *g, int i, int *lines)
{
    int n = wrap_lines(g, TC_W - 2.f * TC_PAD, TOUR_P[i], TC_LH, 12.5f);
    if (lines) *lines = n;
    return TC_PAD + 13.2f + 6.f + 19.2f + 6.f + TC_LH * (float)n + 12.f + 6.f + 12.f + 28.f + 14.f;
}
static void tour_card_at(const txg_t *g, const float r[4], float ch, float *cx, float *cy)   /* under, over, right, left */
{
    float x = r[0], y = r[1], w = r[2], h = r[3];
    if (y + h + TC_GAP + ch <= g->H - 8.f) { *cy = y + h + TC_GAP; *cx = x + w * 0.5f - TC_W * 0.5f; }
    else if (y - TC_GAP - ch >= 8.f) { *cy = y - TC_GAP - ch; *cx = x + w * 0.5f - TC_W * 0.5f; }
    else if (x + w + TC_GAP + TC_W <= g->W - 8.f) { *cx = x + w + TC_GAP; *cy = y + h * 0.5f - ch * 0.5f; }
    else { *cx = x - TC_GAP - TC_W; *cy = y + h * 0.5f - ch * 0.5f; }
    *cx = fmaxf(8.f, fminf(g->W - 8.f - TC_W, *cx));
    *cy = fmaxf(8.f, fminf(g->H - 8.f - ch, *cy));
}
static void tour_dim(txg_t *g, frui_canvas *c, const float r[4], float rad)   /* the window dimmed but r (rounded) */
{
    frui_path p;
    float S = g->S, x0 = r[0] * S, y0 = r[1] * S, x1 = (r[0] + r[2]) * S, y1 = (r[1] + r[3]) * S, k = rad * S;
    frui_path_init(&p);
    frui_path_move(&p, 0.f, 0.f); frui_path_line(&p, g->W * S, 0.f); frui_path_line(&p, g->W * S, g->H * S);
    frui_path_line(&p, 0.f, g->H * S);
    frui_path_move(&p, x1 - k, y0); frui_path_line(&p, x0 + k, y0); frui_path_quad(&p, x0, y0, x0, y0 + k);   /* (the hole:
                                          * the other way round) */
    frui_path_line(&p, x0, y1 - k); frui_path_quad(&p, x0, y1, x0 + k, y1);
    frui_path_line(&p, x1 - k, y1); frui_path_quad(&p, x1, y1, x1, y1 - k);
    frui_path_line(&p, x1, y0 + k); frui_path_quad(&p, x1, y0, x1 - k, y0);
    frui_path_fill(c, &p, 0xB8050404u);  /* (72 %) */
    frui_path_free(&p);
}
static void draw_tour(txg_t *g, frui_canvas *c)
{
    const int i = g->tour;
    float r[4], ch, cx, cy, ty, x1, bw, nw, bx, nx, room = TC_W - 2.f * TC_PAD;
    int n, fit = 1, last = i == TOUR_N - 1;
    char t[24];
    const char *nt = last ? "DONE" : "NEXT", *side = g->tour_first ? "FIRST-RUN GUIDE" : "THE TOUR";
    if (!tour_rect(g, i, r)) { r[0] = g->W * 0.5f; r[1] = TOP_H; r[2] = 0.f; r[3] = 0.f; }
    {   /* (2 px in from the window's edges: its outline whole) */
        float x1 = fminf(r[0] + r[2], g->W - 2.f), y1 = fminf(r[1] + r[3], g->H - 2.f);
        r[0] = fmaxf(r[0], 2.f); r[1] = fmaxf(r[1], 2.f); r[2] = fmaxf(x1 - r[0], 0.f); r[3] = fmaxf(y1 - r[1], 0.f);
    }
    tour_dim(g, c, r, 6.f);
    frui_rrect_stroke(c, (r[0] - 1.f) * g->S, (r[1] - 1.f) * g->S, (r[2] + 2.f) * g->S, (r[3] + 2.f) * g->S, 7.f * g->S, 2.f * g->S, C_RED);
    wid_add(g, TXG_W_TOURLIT, i, r[0], r[1], r[2], r[3]);
    ch = tour_card_h(g, i, &n);
    tour_card_at(g, r, ch, &cx, &cy);
    wid_add(g, TXG_W_TOUR, i, cx, cy, TC_W, ch);
    frui_rrect_shadow(c, cx * g->S, (cy + 8.f) * g->S, TC_W * g->S, ch * g->S, 8.f * g->S, 20.f * g->S, 0xCC000000u);
    RR(g, c, cx, cy, TC_W, ch, 8.f, 0xFF141313u); RRS(g, c, cx, cy, TC_W, ch, 8.f, 0xFF3A3736u);
    x1 = cx + TC_W - TC_PAD;
    ty = cy + TC_PAD;
    snprintf(t, sizeof t, "STEP %d OF %d", i + 1, TOUR_N);
    {
        float sw = TW(g, 11.f, 500.f, 11.f * 0.08f, 0, side), lw;
        lw = TX(g, c, cx + TC_PAD, ty + 6.6f + CAP(g, 11.f) * 0.5f, 11.f, 620.f, 11.f * 0.16f, 0, C_RED, t);
        TX(g, c, x1 - sw, ty + 6.6f + CAP(g, 11.f) * 0.5f, 11.f, 500.f, 11.f * 0.08f, 0, C_DIM, side);
        fit &= lw + 12.f + sw <= room;
    }
    ty += 13.2f + 6.f;
    fit &= TX(g, c, cx + TC_PAD, ty + 9.6f + CAP(g, 16.f) * 0.5f, 16.f, 620.f, 0.16f, 0, C_INK, TOUR_T[i]) <= room;
    ty += 19.2f + 6.f;
    (void)wrapn(g, c, cx + TC_PAD, ty + TC_LH * 0.5f + CAP(g, 12.5f) * 0.5f, room, TOUR_P[i], n, TC_LH, 12.5f, 0xFFCFC9C3u);
    fit &= n < 8;                        /* (wrap_lines' most: the text whole) */
    ty += TC_LH * (float)n + 12.f;
    for (int k = 0; k < TOUR_N; k++) CI(g, c, cx + TC_PAD + 3.f + 11.f * (float)k, ty + 3.f, 3.f, k == i ? C_RED : C_FAINT);
    ty += 6.f + 12.f;
    /* SKIP (a word, at the left), MANUAL (the last step), BACK, NEXT / DONE (at the right) */
    {
        int hs = g->hover_kind == TXG_W_TOURBTN && g->hover_lane == 0, hb = g->hover_kind == TXG_W_TOURBTN && g->hover_lane == 1,
            hn = g->hover_kind == TXG_W_TOURBTN && g->hover_lane == 2;
        float sk = TW(g, 11.5f, 620.f, 1.15f, 0, "SKIP");
        float yb = ty + 14.f + CAP(g, 11.5f) * 0.5f;
        nw = TW(g, 11.5f, 620.f, 1.15f, 0, nt) + 24.f; bw = TW(g, 11.5f, 620.f, 1.15f, 0, "BACK") + 24.f;
        nx = x1 - nw; bx = nx - 8.f - bw;
        TX(g, c, cx + TC_PAD + 4.f, yb, 11.5f, 620.f, 1.15f, 0, hs ? C_INK : C_DIM, "SKIP");
        wid_add(g, TXG_W_TOURBTN, 0, cx + TC_PAD, ty, sk + 8.f, 28.f);
        if (i > 0) {
            RR(g, c, bx, ty, bw, 28.f, 4.f, hb ? C_SEG : C_BTN);
            TX(g, c, bx + 12.f, yb, 11.5f, 620.f, 1.15f, 0, C_INK, "BACK");
            wid_add(g, TXG_W_TOURBTN, 1, bx, ty, bw, 28.f);
        } else {                         /* (the first step: BACK there, dim) */
            RR(g, c, bx, ty, bw, 28.f, 4.f, frui_alpha(C_BTN, 0.35f));
            TX(g, c, bx + 12.f, yb, 11.5f, 620.f, 1.15f, 0, frui_alpha(C_INK, 0.35f), "BACK");
        }
        RR(g, c, nx, ty, nw, 28.f, 4.f, hn ? 0xFFF25D50u : C_RED);
        TX(g, c, nx + 12.f, yb, 11.5f, 620.f, 1.15f, 0, 0xFFFFFFFFu, nt);
        wid_add(g, TXG_W_TOURBTN, 2, nx, ty, nw, 28.f);
        if (last) {                      /* (the last step: MANUAL before BACK) */
            int hm = g->hover_kind == TXG_W_TOURBTN && g->hover_lane == 3;
            float mw = TW(g, 11.5f, 620.f, 1.15f, 0, "MANUAL") + 24.f, mx = bx - 8.f - mw;
            RR(g, c, mx, ty, mw, 28.f, 4.f, hm ? C_SEG : C_BTN);
            TX(g, c, mx + 12.f, yb, 11.5f, 620.f, 1.15f, 0, C_INK, "MANUAL");
            wid_add(g, TXG_W_TOURBTN, 3, mx, ty, mw, 28.f);
            bx = mx;
        }
        fit &= cx + TC_PAD + sk + 8.f + 8.f <= bx;
    }
    g->tour_fit = fit;
}
static void draw_all(txg_t *g, frui_canvas *c)
{
    g->nw = 0;
    g->zone_fit = 1;
    frui_clip_reset(c);
    frui_fill(c, C_PANEL);
    draw_top(g, c);
    if (g->ov == OV_SET) draw_settings(g, c);
    else if (g->ov == OV_SND) draw_sounds(g, c);
    else if (g->ov == OV_PRE) draw_presets(g, c);
    else {
        draw_rows(g, c); draw_place(g, c);
        if (g->ov == OV_FX) draw_fxd(g, c); else draw_drawer(g, c);
        draw_strip(g, c);
        if (g->msrc) { mode_marks(g, c); draw_mbar(g, c); }
        if (g->msg_t > 0 && g->ov != OV_RND) draw_msg(g, c);   /* (RANDOM's list open: in it) */
        if (g->drag == TXG_W_GRIP && g->drag_moved) draw_ghost(g, c);
        if (g->ov == OV_SAVE) draw_save(g, c);
        if (g->ov == OV_RND) draw_rnd(g, c);
                if (g->menu_on) draw_menu(g, c);
    }
    hint_tip(g, c);
    dice_tip(g, c);
    draw_picker(g, c);
    if (g->tour >= 0) draw_tour(g, c);
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
    if (g->actn != actn0 || !g->drawn)   /* the sources' traces: a value a tick while the app plays */
        for (int n = 0; n < SRC_N; n++) {
            int16_t v = n < 8 ? g->sn.ui.lfo[n] : g->sn.ui.envf[n - 8];
            if (!g->drawn && !g->lfo_seen) for (int i = 0; i < LFO_H; i++) g->lfoh[n][i] = v;   /* (the first: a flat
                                          * trace at the value now) */
            memmove(g->lfoh[n], g->lfoh[n] + 1, (LFO_H - 1) * sizeof g->lfoh[n][0]);
            g->lfoh[n][LFO_H - 1] = v;
        }
    g->lfo_seen = 1;
    if (g->msg_t > 0) g->msg_t--;
    {   /* the links' undo: the matrix changed by anything but the window (a preset, the host): forgotten */
        int16_t now[16][4];
        for (int s = 0; s < 16; s++) for (int q = 0; q < 4; q++) now[s][q] = (int16_t)mx(g, s, q);
        if (memcmp(now, g->mx_known, sizeof now)) { g->lh_n = 0; memcpy(g->mx_known, now, sizeof now); }
    }
    {   /* COMP's meters: a VU's ballistics on the app's values (its most since the last publication): up fast (half the
         * way a tick), down 0.8 dB a tick (~24 dB/s); the input's peak held ~1 s; the gain reduction up at once */
        float t[3] = { (float)g->sn.ui.comp_in10 / 10.f, (float)g->sn.ui.comp_out10 / 10.f, (float)g->sn.ui.comp_gr10 / 10.f };
        float *d[3] = { &g->cm_in, &g->cm_out, &g->cm_gr };
        if (!g->drawn) { for (int i = 0; i < 3; i++) *d[i] = t[i]; g->cm_pk = t[0]; g->cm_hold = 30; }
        else {
            for (int i = 0; i < 2; i++) {
                if (t[i] > *d[i]) *d[i] += (t[i] - *d[i]) * 0.55f; else *d[i] = fmaxf(t[i], *d[i] - 0.8f);
                if (*d[i] - t[i] < 0.05f && *d[i] > t[i]) *d[i] = t[i];
            }
            if (t[2] > g->cm_gr) g->cm_gr = t[2]; else g->cm_gr = fmaxf(t[2], g->cm_gr - 0.5f);
            if (t[0] >= g->cm_pk) { g->cm_pk = t[0]; g->cm_hold = 30; }
            else if (g->cm_hold > 0) g->cm_hold--;
            else g->cm_pk = fmaxf(t[0], g->cm_pk - 0.8f);
        }
        if (fabsf(g->cm_in - g->cm_drawn[0]) >= 0.05f || fabsf(g->cm_out - g->cm_drawn[1]) >= 0.05f ||
            fabsf(g->cm_gr - g->cm_drawn[2]) >= 0.05f || fabsf(g->cm_pk - g->cm_drawn[3]) >= 0.05f)
            g->cm_dirty = 1;
    }
    {   /* M/S's meters: COMP's ballistics on L R M S (the app's held peaks); the correlation smoothed; the goniometer's
         * points a block at a time (a new block: the oldest dropped), its scale following the level (up at once, down
         * slowly) so that a quiet mix still fills it */
        const txp_ui_snap_t *u = &g->sn.ui;
        float cr = (float)u->ms_corr / 32767.f;
        if (!g->drawn) { for (int i = 0; i < 4; i++) g->ms_db[i] = (float)u->ms_db10[i] / 10.f; g->ms_c = cr; g->gon_k = 0.45f / 0.016f; }
        else {
            for (int i = 0; i < 4; i++) {
                float t = (float)u->ms_db10[i] / 10.f;
                if (t > g->ms_db[i]) g->ms_db[i] += (t - g->ms_db[i]) * 0.55f; else g->ms_db[i] = fmaxf(t, g->ms_db[i] - 0.8f);
                if (g->ms_db[i] - t < 0.05f && g->ms_db[i] > t) g->ms_db[i] = t;
            }
            g->ms_c += (cr - g->ms_c) * 0.35f;
        }
        if (u->gon_blk != g->gon_blk) {
            float pk = 0.f, kt;
            g->gon_blk = u->gon_blk;
            g->gon_at = (g->gon_at + 1) % GON_H;
            memcpy(g->gon[g->gon_at], u->gon, sizeof g->gon[0]);
            if (g->gon_n < GON_H) g->gon_n++;
            for (int a = 0; a < g->gon_n; a++) {
                const int16_t (*q)[2] = g->gon[(g->gon_at - a + GON_H) % GON_H];
                for (unsigned i = 0; i < TXP_GON_N; i++) {
                    float l = fabsf((float)q[i][0]), r = fabsf((float)q[i][1]);
                    pk = fmaxf(pk, fmaxf(l, r));
                }
            }
            kt = 0.45f / fmaxf(pk / 32767.f, 0.016f);
            g->gon_k += (kt - g->gon_k) * (kt < g->gon_k ? 1.f : 0.05f);
            g->cm_dirty = 1;
        }
        for (int i = 0; i < 4; i++) if (fabsf(g->ms_db[i] - g->ms_drawn[i]) >= 0.05f) g->cm_dirty = 1;
        if (fabsf(g->ms_c - g->ms_drawn[4]) >= 0.005f) g->cm_dirty = 1;
    }
    if (g->sb_pend > 0 && --g->sb_pend == 0 && g->ov == OV_SND) pv_play(g);   /* (a preview waited for the keys to
                                          * rest) */
    if (g->sv_wait > 0) g->sv_wait--;
    /* the signature of what is drawn but the live values: the values the modulation moves now, the meters and the LFOs
     * are drawn with the activity (the partial repaint), not here */
    {
        static txp_ui_snap_t u;          /* (the main thread's alone) */
        u = g->sn.ui;
        for (k = 0; k < (int)TXP_LANES; k++) { u.lane[k].lpan = u.lane[k].ldist = 0; memset(u.card[k].live, 0, sizeof u.card[k].live); }
        memset(u.lfo, 0, sizeof u.lfo);
        memset(u.envf, 0, sizeof u.envf);
        u.comp_gr10 = u.comp_in10 = u.comp_out10 = 0; u.out_db10 = 0;
        memset(u.ms_db10, 0, sizeof u.ms_db10); u.ms_corr = 0; memset(u.gon, 0, sizeof u.gon); u.gon_blk = 0;
        for (k = 0; k < (int)TXP_LANES; k++) u.lane[k].cnt = 0;
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
        int v[15] = { g->sb_fam, g->sb_cf << 8 | g->sb_ci, g->sb_top, g->pb_bank, g->pb_cur, g->pb_top, g->sv_slot, g->sv_wait > 0, txp_ui_win_w(g->c),
                      g->pv_on, g->pv_cnt, g->pv_act, g->pv_user, g->ov, txp_ui_dice(g->c) };
        h = fnv(h, v, sizeof v);
        h = fnv(h, g->q, sizeof g->q);
        h = fnv(h, g->sv_name, sizeof g->sv_name);
        h = fnv(h, &g->sv_typed, sizeof g->sv_typed);
    }
    {   /* the movement: the mode, the message, the menu, a grip dragged, CLEAR ALL's question, the undo, the matrix */
        int v[12 + 64], n = 0;
        int gd = g->drag == TXG_W_GRIP && g->drag_moved;
        v[n++] = g->msrc; v[n++] = g->msg_t > 0; v[n++] = g->menu_on; v[n++] = (int)g->menu_x; v[n++] = (int)g->menu_y;
        v[n++] = (int)g->menu_id; v[n++] = gd ? (int)g->gx : 0; v[n++] = gd ? (int)g->gy : 0; v[n++] = gd ? (int)g->gover : 0;
        v[n++] = g->clr_ask; v[n++] = g->lh_n; v[n++] = g->drag_lane;
        for (int s = 0; s < 16; s++) for (int q = 0; q < 4; q++) v[n++] = mx(g, s, q);
        h = fnv(h, v, sizeof v);
        if (g->msg_t > 0) h = fnv(h, g->msg, sizeof g->msg);
    }
    { int v[2] = { g->tour, g->tour_first }; h = fnv(h, v, sizeof v); }   /* (HELP: the tour's step) */
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
    return !g->drawn || g->sig != g->sig_drawn || g->actn != g->actn_drawn || g->cm_dirty;
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
    int moved = g->actn != g->actn_drawn || g->cm_dirty;
    if (full) {
        frui_clip_base(c, 0, 0, c->w, c->h);
        draw_all(g, c);
    } else if (moved && g->ov == OV_SND && g->pick < 0) {   /* (the engine browser: its preview's event line, meter) */
        region(g, c, g->W - OV_PREV + 1.f, OV_Y0, g->W, g->H);
        frui_clip_base(c, 0, 0, c->w, c->h);
    } else if (moved && (g->ov == OV_NONE || g->ov == OV_SAVE || g->ov == OV_RND || g->ov == OV_FX) && g->pick < 0) {
        rcol_t rc = rcols(g);
        top_r_t tr = top_r(g);
        region(g, c, 0, g->ct, g->PW, g->dr_y);
        region(g, c, rc.dot - 8.f, g->ct + HDR_H, rc.tl + rc.tlw + 1.f, g->dr_y);
        region(g, c, rc.many - 4.f, g->ct + HDR_H, rc.ms - 2.f, g->dr_y);   /* (the rows' values: their live red marks) */
        region(g, c, 0, g->dr_y, g->W, g->H);
        region(g, c, tr.mx - 2.f, 0, tr.mx + tr.mw + 4.f, TOP_H - 1.f);
        frui_clip_base(c, 0, 0, c->w, c->h);
    } else { g->actn_drawn = g->actn; g->cm_dirty = 0; return 0; }
    g->cm_drawn[0] = g->cm_in; g->cm_drawn[1] = g->cm_out; g->cm_drawn[2] = g->cm_gr; g->cm_drawn[3] = g->cm_pk;
    for (int i = 0; i < 4; i++) g->ms_drawn[i] = g->ms_db[i];
    g->ms_drawn[4] = g->ms_c;
    g->cm_dirty = 0;
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
    if (g->tour >= 0) {                  /* (the tour: its card's buttons, else nothing) */
        for (int i = g->nw - 1; i >= 0; i--) {
            const wid_t *w = &g->w[i];
            if ((w->kind == TXG_W_TOURBTN || w->kind == TXG_W_TOUR) && x >= w->r[0] && x < w->r[0] + w->r[2] && y >= w->r[1] &&
                y < w->r[1] + w->r[3]) return w;
        }
        return 0;
    }
    if (g->menu_on) {                    /* (the right-click menu: its items, else nothing) */
        for (int i = g->nw - 1; i >= 0; i--) {
            const wid_t *w = &g->w[i];
            if (w->kind == TXG_W_MENU && x >= w->r[0] && x < w->r[0] + w->r[2] && y >= w->r[1] && y < w->r[1] + w->r[3]) {
                if (w->lane >= 0) return w;
                if (!best) best = w;
            }
        }
        return best;
    }
    if (g->pick >= 0 || g->ov == OV_SAVE || g->ov == OV_RND) {
        for (int i = g->nw - 1; i >= 0; i--) {
            const wid_t *w = &g->w[i];
            int panel = w->kind == TXG_W_PICKER;
            if (x >= w->r[0] && x < w->r[0] + w->r[2] && y >= w->r[1] && y < w->r[1] + w->r[3]) {
                if (g->pick >= 0 && w->kind == TXG_W_SWATCH) return w;
                if (g->ov == OV_SAVE && g->pick < 0 && (w->kind == TXG_W_SLOT || w->kind == TXG_W_NAME || w->kind == TXG_W_SAVEGO ||
                                                        w->kind == TXG_W_NAMEDICE)) return w;
                if (g->ov == OV_RND && g->pick < 0 && (w->kind == TXG_W_RNDTICK || w->kind == TXG_W_RNDGO ||
                                                       (w->kind == TXG_W_CLOSE && w->lane == -3))) return w;
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
        if (w->kind == TXG_W_DOT || w->kind == TXG_W_PLACE || w->kind == TXG_W_MAPR || w->kind == TXG_W_CMETER || w->kind == TXG_W_TIP ||
            w->kind == TXG_W_FXD || w->kind == TXG_W_MBAR || w->kind == TXG_W_MSG || w->kind == TXG_W_NOMOVE || w->kind == TXG_W_MENU ||
            w->kind == TXG_W_MSM || w->kind == TXG_W_LVL || w->kind == TXG_W_LOGO || w->kind == TXG_W_TOURLIT) continue;
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
/* SIDE or WIDTH changed while MONO is on: MONO off first (as on the FM-1: the change is heard) */
static void ms_unmono(txg_t *g, uint32_t id) { if ((id == ID_MSSIDE || id == ID_MSWIDTH) && ms_mono_on(g)) set_param(g, ID_MSMONO, 0); }
static void lh_push(txg_t *g)            /* the matrix as it is, kept for UNDO */
{
    if (g->lh_n == LH_N) { memmove(g->lh[0], g->lh[1], (LH_N - 1) * sizeof g->lh[0]); g->lh_n--; }
    for (int s = 0; s < 16; s++) for (int q = 0; q < 4; q++) g->lh[g->lh_n][s][q] = (int16_t)mx(g, s, q);
    g->lh_n++;
}
static void lh_undo(txg_t *g)            /* the matrix back as it was before the last change (DEPTH first) */
{
    static const int Q[4] = { 2, 0, 1, 3 };
    if (!g->lh_n) return;
    g->lh_n--;
    for (int s = 0; s < 16; s++)
        for (int j = 0; j < 4; j++) if (mx(g, s, Q[j]) != g->lh[g->lh_n][s][Q[j]]) set_param(g, ID_MAT(s, Q[j]), g->lh[g->lh_n][s][Q[j]]);
}
static void slot_clear(txg_t *g, int s)  /* a slot freed (mod.c: no source, no target, no depth) */
{
    set_param(g, ID_MAT(s, 2), 100); set_param(g, ID_MAT(s, 0), 0); set_param(g, ID_MAT(s, 1), 0);
}
static int link_make(txg_t *g, int src, int T)   /* the slot of src -> T, made in the first free one (full: said, -1) */
{
    int s = link_slot(g, src, T);
    if (s >= 0) return s;
    for (s = 0; s < MX_N && (mx(g, s, 0) || mx(g, s, 1)); s++) {}
    if (s >= MX_N) { say(g, "All 16 links are in use. Remove one first (CLEAR, or the MATRIX tab)."); return -1; }
    set_param(g, ID_MAT(s, 2), 100); set_param(g, ID_MAT(s, 3), 0); set_param(g, ID_MAT(s, 0), src); set_param(g, ID_MAT(s, 1), T);
    return s;
}
static void say_link(txg_t *g, int src, uint32_t id, int amt)
{
    char sn[16], nm[48], t[112];
    src_name(src, sn, sizeof sn); ctl_name(g, id, nm, sizeof nm);
    if (amt) snprintf(t, sizeof t, "%s moves %s by %+d%%.", sn, nm, amt); else snprintf(t, sizeof t, "%s no longer moves %s.", sn, nm);
    say(g, t);
}
static void say_refuse(txg_t *g, uint32_t id, int why)
{
    char nm[48], t[112];
    ctl_name(g, id, nm, sizeof nm);
    if (why == 1) snprintf(t, sizeof t, "%s cannot be moved: it is a choice.", nm);
    else if (why == 2) snprintf(t, sizeof t, "%s: lanes 5 to 16 are moved only through ALL LANES (MATRIX tab).", nm);
    else snprintf(t, sizeof t, "No source can move %s.", nm);
    say(g, t);
}
static void link_to(txg_t *g, int src, uint32_t id, int amt)   /* src moves id by amt % (0: the link removed) */
{
    int T = ctl_tgt(g, id, 0), s;
    if (!T) return;
    s = link_slot(g, src, T);
    if (amt > -2 && amt < 2) { if (s >= 0) slot_clear(g, s); say_link(g, src, id, 0); return; }
    if (s < 0 && (s = link_make(g, src, T)) < 0) return;
    set_param(g, ID_MAT(s, 2), 100 + amt);
    say_link(g, src, id, amt);
}
static void src_clear(txg_t *g, int src)  /* every link of src removed */
{
    for (int s = 0; s < MX_N; s++) if (mx(g, s, 0) == src && (mx(g, s, 1) || mx(g, s, 2) != 100)) slot_clear(g, s);
}
static void mode_set(txg_t *g, int src) { g->msrc = src; g->menu_on = 0; g->clr_ask = 0; lay(g); }
/* the mode's drag at x: the reach's end where the pointer is on the control's track (left of the value: less); the
 * link made on the first amount, kept for UNDO before its first change */
static void mamt_at(txg_t *g, float x)
{
    float f = (x - g->drag_rx) / g->drag_rw;
    int a;
    if (ms_lvl(g->mv_id)) {              /* (M/S's MID, SIDE: the place on the taper back to a value) */
        const txp_param_t *p = pinfo(g->mv_id);
        f = ms_val(f < 0.f ? 0.f : f > 1.f ? 1.f : f) / (float)(p && p->hi > 0 ? p->hi : 1);
    }
    a = (int)lroundf((f - g->mv_f) * 100.f);
    a = a < -100 ? -100 : a > 100 ? 100 : a;
    if (!g->drag_begun) {
        int T = ctl_tgt(g, g->mv_id, 0), s = link_slot(g, g->msrc, T);
        if (s < 0 && a > -2 && a < 2) return;
        if (s >= 0 && mx(g, s, 2) - 100 == a) return;
        lh_push(g);
        if (s < 0 && (s = link_make(g, g->msrc, T)) < 0) { g->lh_n--; g->drag = 0; return; }
        g->drag_id = (uint32_t)s;
        txp_ui_begin(g->c, ID_MAT(s, 2)); g->drag_begun = 1;
    }
    txp_ui_set(g->c, ID_MAT((int)g->drag_id, 2), 100 + a);
    say_link(g, g->msrc, g->mv_id, a > -2 && a < 2 ? 0 : a);
}
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
    if (g->view == 1) sp_inv(g, x - g->drag_dx, y - g->drag_dy, g->drag_dist < 0, &pan, &dist);
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
    else if (ms_lvl(g->drag_id)) {       /* (M/S's MID, SIDE: along the fader's taper, as it is drawn) */
        float f = ms_pos((float)g->drag_v0) + d / (g->drag_fine ? 900.f : 220.f);
        v = (int)lroundf(ms_val(f < 0.f ? 0.f : f > 1.f ? 1.f : f));
    } else v = g->drag_v0 + (int)lroundf(d * (float)(p->hi + 1) / (g->drag_fine ? 900.f : 220.f));
    v = v < 0 ? 0 : v > p->hi ? p->hi : v;
    if (!g->drag_begun) { ms_unmono(g, g->drag_id); txp_ui_begin(g->c, g->drag_id); g->drag_begun = 1; }
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
    } else if (g->drag == TXG_W_MAMT && g->drag_begun) {   /* (the amount set: ~0 removes the link) */
        int s = (int)g->drag_id, a = mx(g, s, 2) - 100;
        txp_ui_end(g->c, ID_MAT(s, 2));
        if (a > -2 && a < 2) slot_clear(g, s);
        say_link(g, g->msrc, g->mv_id, a > -2 && a < 2 ? 0 : a);
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
    if (g->ov == OV_SND) sb_close(g, 0);
    g->ov = OV_SND; g->pick = -1; g->q[0] = 0; g->sb_top = 0; g->sb_pend = 0;
    g->sb_lane = k;
    g->sb_kf = l->used ? l->fi : -1; g->sb_ki = l->used ? l->si : -1;
    g->sb_cf = l->used ? l->fi : (g->sn.ui.land < g->cat->nfam ? g->sn.ui.land : 0);
    g->sb_ci = l->used ? l->si : 0;
    g->sb_fam = g->sb_cf;
    g->pv_on = 0; g->pv_user = 0;
}
/* the preview (slot_bridge.h sb_pv_*): the cursor's sound on the lane, always heard (a stopped lane started at the
 * preview's HOW MANY / HOW ACTIVE); stopped: the lane as it was */
static void pv_play(txg_t *g)
{
    g->sb_pend = 0;
    txp_ui_preview(g->c, g->sb_lane, g->sb_cf, g->sb_ci, g->pv_cnt, g->pv_act, g->pv_user);
    g->pv_on = 1;
}
static void pv_stop(txg_t *g)
{
    g->sb_pend = 0;
    if (g->pv_on) txp_ui_preview_end(g->c, g->sb_lane, g->sb_kf, g->sb_ki, 0, 0);
    g->pv_on = 0;
}
static void sb_close(txg_t *g, int keep)
{
    const txp_ui_lane_t *l = &g->sn.ui.lane[g->sb_lane];
    g->sb_pend = 0;
    if (keep) {                          /* (the cursor's sound kept; the lane otherwise as it was: stopped stays stopped,
                                          * unless the preview's HOW MANY / HOW ACTIVE were set) */
        if (g->pv_on || !(g->sb_cf == g->sb_kf && g->sb_ci == g->sb_ki) || !l->used)
            txp_ui_preview_end(g->c, g->sb_lane, g->sb_cf, g->sb_ci, 1, g->pv_on && g->pv_user);
    } else pv_stop(g);                   /* (CLOSE: the lane exactly as it was) */
    g->pv_on = 0;
    g->ov = OV_NONE; g->q[0] = 0;
}
static void sb_cursor(txg_t *g, int f, int i, int now)   /* the cursor on a sound: previewed on the lane */
{
    g->sb_cf = f; g->sb_ci = i;
    if (now) pv_play(g);
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
static void save_name(txg_t *g)          /* the name for the slot chosen: a used one's own, a free one's the last drawn
                                          * (none yet: one drawn); a name typed stays */
{
    int k = g->sv_slot;
    if (g->sv_typed) return;
    if (g->sn.ui.user >> k & 1u) { snprintf(g->sv_name, sizeof g->sv_name, "%.11s", g->sn.ui.uname[k]); return; }
    if (!g->sv_gen[0]) txp_ui_name(g->c, 0, g->sv_gen);
    memcpy(g->sv_name, g->sv_gen, sizeof g->sv_name);
}
static void save_roll(txg_t *g)          /* the dice beside the name: another name drawn */
{
    txp_ui_name(g->c, g->sv_name, g->sv_gen);
    memcpy(g->sv_name, g->sv_gen, sizeof g->sv_name);
    g->sv_typed = 0;
}
static void save_open(txg_t *g)          /* SAVE: the first free user preset, a name drawn; all used: the one loaded,
                                          * else U01, its own name */
{
    int s = g->sn.ui.scene, n = g->cat->npr;
    g->ov = OV_SAVE; g->pick = -1;
    g->sv_slot = -1; g->sv_name[0] = 0; g->sv_gen[0] = 0; g->sv_typed = 0;
    for (int i = 0; i < (int)TXP_USER_N && g->sv_slot < 0; i++) if (!(g->sn.ui.user >> i & 1u)) g->sv_slot = i;
    if (g->sv_slot < 0) g->sv_slot = s != 0xFF && s >= n && s < n + (int)TXP_USER_N ? s - n : 0;
    save_name(g);
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
        if (ch == FRK_BACKSPACE || ch == FRK_DELETE) { if (m) g->sv_name[m - 1] = 0; g->sv_typed = 1; return 3; }
        if (ch >= 'a' && ch <= 'z') ch -= 32;
        if (ch > 0 && ch < 0x7F && strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-&'. ", ch)) {
            if (!g->sv_typed) { m = 0; g->sv_typed = 1; }   /* (the name drawn, selected: replaced) */
            if (m < 11u) { g->sv_name[m] = (char)ch; g->sv_name[m + 1] = 0; }
            return 3;
        }
        return 0;
    }
    if (g->ov == OV_SET || g->ov == OV_RND) return 0;
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
        if (g->ov == OV_SND) { sb_items(g); if (g->nitems) sb_cursor(g, g->items[0] >> 4, g->items[0] & 15, 0); }
        else { pb_items(g); if (g->nitems) g->pb_cur = g->items[0]; }
        return 3;
    }
    return 0;
}
/* the tour's flag: a file in the window's own folder (txg_tour_dir), written by SKIP or DONE */
#define TOUR_FLAG "tour-seen"
static void tour_end(txg_t *g)
{
    g->tour = -1;
    g->hover_kind = 0; g->hover_lane = -1;
    if (g->udir[0]) {
        char p[1100];
        FILE *f;
        snprintf(p, sizeof p, "%s/%s", g->udir, TOUR_FLAG);
        if ((f = fopen(p, "wb"))) { fputs("1\n", f); fclose(f); }
    }
}
static void tour_go(txg_t *g, int d)      /* NEXT (+1; the last: DONE) or BACK (-1; none on the first) */
{
    if (d > 0 && g->tour >= TOUR_N - 1) { tour_end(g); return; }
    if (d < 0 && g->tour <= 0) return;
    g->tour += d;
}
static int tour_input(txg_t *g, const frg_input *e)   /* the tour on: its card's buttons, its keys; the rest waits */
{
    const wid_t *w;
    switch (e->kind) {
    case FRG_DOWN: case FRG_DOUBLE:
        if (e->button != FRG_LEFT) return 2;
        w = hit(g, e->x, e->y);
        if (!w || w->kind != TXG_W_TOURBTN) return 2;
        if (w->lane == 3) manual_open(g);
        else if (w->lane == 0) tour_end(g); else tour_go(g, w->lane == 1 ? -1 : 1);
        return 3;
    case FRG_MOVE: {
        int hk = 0, hl = -1;
        w = hit(g, e->x, e->y);
        if (w && w->kind == TXG_W_TOURBTN) { hk = w->kind; hl = w->lane; }
        if (hk != g->hover_kind || hl != g->hover_lane) { g->hover_kind = hk; g->hover_lane = hl; return 1; }
        return 0;
    }
    case FRG_KEY:
        if (e->key == FRK_RIGHT || e->key == FRK_RETURN) tour_go(g, 1);
        else if (e->key == FRK_LEFT) tour_go(g, -1);
        else if (e->key == FRK_ESCAPE) tour_end(g);
        else return 2;
        return 3;
    case FRG_LEAVE: case FRG_FOCUS_LOST:
        g->hover_kind = 0; g->hover_lane = -1;
        return 1;
    case FRG_WHEEL: return 2;
    }
    return 0;
}
void txg_tour_start(txg_t *g, int first)
{
    if (g->drag) drag_end(g);
    if (g->ov == OV_SND) sb_close(g, 0);
    mode_set(g, 0);
    g->ov = OV_NONE; g->pick = -1; g->q[0] = 0;
    g->hover_kind = 0; g->hover_lane = -1;
    g->tour = 0; g->tour_first = first != 0;
}
void txg_tour_dir(txg_t *g, const char *dir) { snprintf(g->udir, sizeof g->udir, "%s", dir ? dir : ""); }
int txg_first_run(const char *dir, int test)
{
    char p[1100];
    FILE *f;
    if (test || !dir || !dir[0]) return 0;
    snprintf(p, sizeof p, "%s/%s", dir, TOUR_FLAG);
    if ((f = fopen(p, "rb"))) { fclose(f); return 0; }
    return 1;
}
int txg_fits(const txg_t *g, int what) { return what == 0 ? g->top_fit : what == 3 ? g->ms_fit : what == 4 ? g->head_fit : what == 5 ? g->zone_fit : g->tour_fit; }
static int input_(txg_t *g, const frg_input *e)
{
    const wid_t *w;
    int k;
    if (g->tour >= 0) return tour_input(g, e);
    switch (e->kind) {
    case FRG_DOWN: case FRG_DOUBLE:
        if (e->button == FRG_RIGHT && e->kind == FRG_DOWN) {   /* the right-click menu on a value */
            if (g->drag) drag_end(g);
            g->menu_on = 0;
            w = hit(g, e->x, e->y);
            if (g->pick >= 0 || (g->ov != OV_NONE && g->ov != OV_FX) || !w) return 3;
            {
                uint32_t id = w->kind == TXG_W_VOL ? TXP_ID_LANE(w->lane, TXP_PL_LEVEL) : w->id;
                if ((w->kind == TXG_W_PARAM || w->kind == TXG_W_VOL || w->kind == TXG_W_MANY || w->kind == TXG_W_ACT ||
                     w->kind == TXG_W_STEP || w->kind == TXG_W_TAG) && id && !mode_skip(id)) {
                    g->menu_on = 1; g->menu_id = id; g->menu_x = e->x; g->menu_y = e->y;
                }
            }
            return 3;
        }
        if (e->button != FRG_LEFT) return 0;
        if (g->drag) drag_end(g);
        w = hit(g, e->x, e->y);
        if (g->menu_on) {                /* the menu: an item, or anything else closes it */
            g->menu_on = 0;
            if (w && w->kind == TXG_W_MENU && w->lane >= 0 && w->lane < SRC_N) {
                int src = SRCS[w->lane], T = ctl_tgt(g, g->menu_id, 0);
                if (T && link_slot(g, src, T) < 0) {
                    int s;
                    lh_push(g);
                    if ((s = link_make(g, src, T)) >= 0) { set_param(g, ID_MAT(s, 2), 125); say_link(g, src, g->menu_id, 25); }
                    else g->lh_n--;
                }
                if (T) mode_set(g, src);
            } else if (w && w->kind == TXG_W_MENU && w->lane == SRC_N) {   /* (its own links; ALL LANES' stay) */
                lk_t lk[MX_N];
                int nl = ctl_links(g, g->menu_id, lk), all = 0;
                char nm[48], t[112];
                lh_push(g);
                for (int i = 0; i < nl; i++) { if (lk[i].all) all++; else slot_clear(g, lk[i].slot); }
                ctl_name(g, g->menu_id, nm, sizeof nm);
                if (all) snprintf(t, sizeof t, "%s: movement removed (ALL LANES links stay: MATRIX tab).", nm);
                else snprintf(t, sizeof t, "%s: movement removed.", nm);
                say(g, t);
            }
            return 3;
        }
        if (w && g->msrc) {              /* a source's mode: a value sets the source's amount on it */
            uint32_t id = w->kind == TXG_W_VOL ? TXP_ID_LANE(w->lane, TXP_PL_LEVEL) : w->id;
            if ((w->kind == TXG_W_PARAM || w->kind == TXG_W_VOL || w->kind == TXG_W_MANY || w->kind == TXG_W_ACT || w->kind == TXG_W_STEP) &&
                id && !mode_skip(id) && g->pick < 0) {
                int why, T = ctl_tgt(g, id, &why);
                if (!T) { say_refuse(g, id, why); return 3; }
                if (e->kind == FRG_DOUBLE) {
                    if (link_amt(g, g->msrc, id)) { lh_push(g); link_to(g, g->msrc, id, 0); }
                    return 3;
                }
                {
                    const txp_param_t *pp = pinfo(id);
                    int hi = pp ? pp->hi : 1;
                    g->drag = TXG_W_MAMT; g->mv_id = id;
                    g->mv_f = (float)txp_ui_get(g->c, id) / (float)(hi > 0 ? hi : 1);
                    g->drag_begun = 0;
                    g->drag_rx = w->t0; g->drag_rw = w->tw > 8.f ? w->tw : 8.f;
                    mamt_at(g, e->x);
                }
                return 3;
            }
        }
        if (g->pick >= 0) {              /* the picker: a swatch, or anything else closes it */
            if (w && w->kind == TXG_W_SWATCH) { txp_ui_col_set(g->c, (int)w->id, w->v); return 3; }
            if (!w || w->kind != TXG_W_PICKER) g->pick = -1;
            return 3;
        }
        if (g->ov == OV_SAVE && (!w || (w->kind != TXG_W_SLOT && w->kind != TXG_W_NAME && w->kind != TXG_W_SAVEGO &&
                                        w->kind != TXG_W_NAMEDICE &&
                                        !(w->kind == TXG_W_PICKER && w->lane == -2)))) {   /* (outside SAVE: it closes;
                                          * on SAVE itself: closed) */
            g->ov = OV_NONE;
            if (w && w->kind == TXG_W_SAVE) return 3;
        }
        if (g->ov == OV_RND && (!w || (w->kind != TXG_W_RNDTICK && w->kind != TXG_W_RNDGO && w->kind != TXG_W_CLOSE &&
                                       !(w->kind == TXG_W_PICKER && w->lane == -3)))) {   /* (outside RANDOM's list: it
                                          * closes, but on the dice (a roll, the list kept); on the arrow: closed) */
            if (!w || w->kind != TXG_W_RND) g->ov = OV_NONE;
            if (w && w->kind == TXG_W_RNDMENU) return 3;
        }
        if (!w) return 2;
        k = w->lane;
        switch (w->kind) {
        case TXG_W_PREV: step_param(g, TXP_ID_PRESET, -1, 1); break;
        case TXG_W_NEXT: step_param(g, TXP_ID_PRESET, 1, 1); break;
        case TXG_W_PRESET:
            if (g->ov == OV_PRE) g->ov = OV_NONE;
            else { if (g->ov == OV_SND) sb_close(g, 0); mode_set(g, 0); pb_open(g); }
            break;
        case TXG_W_LDN: step_param(g, TXP_ID_LANES, -1, 0); break;
        case TXG_W_LUP: case TXG_W_ADD: step_param(g, TXP_ID_LANES, 1, 0); break;
        case TXG_W_SET:
            if (g->ov == OV_SND) sb_close(g, 0);
            mode_set(g, 0);
            g->ov = g->ov == OV_SET ? OV_NONE : OV_SET; g->pick = -1; g->q[0] = 0;
            break;
        case TXG_W_SAVE: if (g->ov == OV_SND) sb_close(g, 0); mode_set(g, 0); save_open(g); break;
        case TXG_W_RNDMENU: if (g->ov == OV_SND) sb_close(g, 0); mode_set(g, 0); g->ov = OV_RND; g->pick = -1; break;
        case TXG_W_RNDTICK: if (k >= 0 && k < 4) txp_ui_dice_set(g->c, txp_ui_dice(g->c) ^ 1 << k); break;
        case TXG_W_HELP: txg_tour_start(g, 0); break;
        case TXG_W_RND: case TXG_W_RNDGO:
            if (txp_ui_roll(g->c, -1)) { say(g, "Nothing ticked: tick at least one to roll."); break; }
            if (txp_ui_dice(g->c) & 8) g->lh_n = 0;   /* (MOVEMENT rolled: the links' own undo no longer holds) */
            say(g, "Rolled. UNDO takes it back.");
            break;
        case TXG_W_DICE: {
            char t[64];
            if (txp_ui_roll(g->c, k)) break;
            snprintf(t, sizeof t, "Lane %d: a random engine. UNDO takes it back.", k + 1);
            say(g, t);
            break;
        }
        case TXG_W_UNDO:                 /* (the links' first: the window's own undo of them) */
            if (g->lh_n) { lh_undo(g); say(g, "Undone."); } else txp_ui_undo(g->c);
            break;
        case TXG_W_SRC: mode_set(g, g->msrc == k ? 0 : k); break;
        case TXG_W_GRIP:                 /* (dragged onto a control: a link; a click: as the tile's) */
            g->drag = TXG_W_GRIP; g->drag_lane = k; g->drag_x0 = g->gx = e->x; g->drag_y0 = g->gy = e->y; g->drag_moved = 0;
            g->gover = 0;
            break;
        case TXG_W_TAG:
            if (src_tile(w->v)) mode_set(g, w->v);
            else { mode_set(g, 0); g->ov = OV_FX; g->fx_tab = TAB_MAT; }   /* (a source with no tile: the matrix) */
            break;
        case TXG_W_MCLEAR: {
            char sn[16], t[48];
            lh_push(g); src_clear(g, g->msrc);
            src_name(g->msrc, sn, sizeof sn); snprintf(t, sizeof t, "%s cleared. UNDO brings it back.", sn); say(g, t);
            break;
        }
        case TXG_W_MDONE: mode_set(g, 0); break;
        case TXG_W_CLRALL: g->clr_ask = 1; break;
        case TXG_W_CLRNO: g->clr_ask = 0; break;
        case TXG_W_CLRYES:
            lh_push(g);
            for (int s = 0; s < MX_N; s++) if (mx(g, s, 0) || mx(g, s, 1) || mx(g, s, 2) != 100) slot_clear(g, s);
            g->clr_ask = 0;
            say(g, "All movement cleared. UNDO brings it back.");
            break;
        case TXG_W_REC: txp_ui_rec(g->c); break;
        case TXG_W_SLOT: if (k >= 0 && k < (int)TXP_USER_N) { g->sv_slot = k; save_name(g); } if (e->kind == FRG_DOUBLE) save_go(g); break;
        case TXG_W_SAVEGO: save_go(g); break;
        case TXG_W_NAMEDICE: save_roll(g); break;
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
        case TXG_W_VMS: g->view = 2; txp_ui_view_set(g->c, 2); break;
        case TXG_W_SND: case TXG_W_SWAP: mode_set(g, 0); sb_open(g, k); break;
        case TXG_W_COLDOT:
            if ((int)w->id < 0 || (int)w->id >= g->cat->neng) break;
            g->pick = (int)w->id; g->pick_x = (int)w->r[0]; g->pick_y = (int)(w->r[1] + w->r[3] + 6.f);
            break;
        case TXG_W_SWATCH: txp_ui_col_set(g->c, (int)w->id, w->v); break;
        case TXG_W_COLRESET: txp_ui_col_reset(g->c); break;
        case TXG_W_ROW: case TXG_W_SELDOT: select_lane(g, k); break;
        case TXG_W_PREVIEW: if (g->pv_on) pv_stop(g); else pv_play(g); break;
        case TXG_W_PVSTEP: {             /* the preview's HOW MANY (1 .. 6) / HOW ACTIVE (1 .. 9): set, heard at once */
            int *v = k ? &g->pv_act : &g->pv_cnt, hi = k ? 9 : 6;
            *v += w->v;
            *v = *v < 1 ? 1 : *v > hi ? hi : *v;
            g->pv_user = 1;
            if (g->pv_on) pv_play(g);
            break;
        }
        case TXG_W_STEP:                 /* a choice's < >: the previous, the next */
            if ((w->id >> 8) == 7u) lh_push(g);
            if (lane_of_id(w->id) >= 0) select_lane(g, lane_of_id(w->id));
            step_param(g, w->id, w->v, 1);
            break;
        case TXG_W_LRUN: case TXG_W_DRUN:
            if (!used(g, k)) break;      /* (no engine: nothing) */
            select_lane(g, k);
            txp_ui_lane_run(g->c, (uint32_t)k, !g->sn.ui.lane[k].run);
            break;
        case TXG_W_MUTE: select_lane(g, k); txp_ui_mute(g->c, (uint32_t)k, !(g->mute >> k & 1u)); g->mute ^= 1u << k; break;
        case TXG_W_SOLO: {
            uint32_t m = g->solo ^ (1u << k);
            select_lane(g, k);
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
            if ((w->id >> 8) == 7u && w->id != TXP_ID_MODCC) lh_push(g);   /* (a matrix cell: its change undone by UNDO) */
            if (lane_of_id(w->id) >= 0) select_lane(g, lane_of_id(w->id));
            else for (int j = 0; j < g->nlane; j++) if (w->id == TXP_ID_LCH(j)) select_lane(g, j);   /* (a row's CH) */
            if (e->kind == FRG_DOUBLE) { int d = p_default(g, w->id); if (d >= 0) { ms_unmono(g, w->id); set_param(g, w->id, d); } break; }
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
        if (g->drag == TXG_W_MAMT) { mamt_at(g, e->x); return 3; }   /* (the mode's amount) */
        if (g->drag == TXG_W_GRIP) {
            g->gx = e->x; g->gy = e->y;
            if (!g->drag_moved && fabsf(e->x - g->drag_x0) + fabsf(e->y - g->drag_y0) < 4.f) return 0;
            g->drag_moved = 1;
            w = hit(g, e->x, e->y);
            g->gover = 0;
            if (w && (w->kind == TXG_W_PARAM || w->kind == TXG_W_VOL || w->kind == TXG_W_MANY || w->kind == TXG_W_ACT || w->kind == TXG_W_STEP)) {
                uint32_t id = w->kind == TXG_W_VOL ? TXP_ID_LANE(w->lane, TXP_PL_LEVEL) : w->id;
                if (id && !mode_skip(id)) { g->gover = id; memcpy(g->gover_r, w->r, sizeof g->gover_r); }
            }
            return 3;
        }
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
            float r[4];
            if (w && (w->kind == TXG_W_ADD || w->kind == TXG_W_DOT || w->kind == TXG_W_LIST || w->kind == TXG_W_SND ||
                      w->kind == TXG_W_LRUN || w->kind == TXG_W_DRUN || w->kind == TXG_W_PREVIEW || w->kind == TXG_W_SRC ||
                      w->kind == TXG_W_GRIP || w->kind == TXG_W_MENU || w->kind == TXG_W_CLRALL || w->kind == TXG_W_MDONE ||
                      w->kind == TXG_W_MCLEAR || w->kind == TXG_W_DICE || w->kind == TXG_W_RND || w->kind == TXG_W_RNDTICK ||
                      w->kind == TXG_W_RNDGO || (w->kind == TXG_W_CLOSE && w->lane == -3) || w->kind == TXG_W_HELP ||
                      w->kind == TXG_W_RNDMENU)) {
                hk = w->kind == TXG_W_GRIP ? TXG_W_SRC : w->kind; hl = w->lane;   /* (the grip lights its tile) */
            }
            else if (w && w->kind == TXG_W_PARAM && txg_find(g, TXG_W_TIP, w->lane, r) && e->x >= r[0] && e->x < r[0] + r[2] &&
                     e->y >= r[1] && e->y < r[1] + r[3]) { hk = TXG_W_TIP; hl = w->lane; }   /* (a card whose hint was
                                          * cut: all of it shows) */
            if (hk != g->hover_kind || hl != g->hover_lane) { g->hover_kind = hk; g->hover_lane = hl; return 1; }
        }
        return 0;
    case FRG_UP:
        if (g->drag == TXG_W_GRIP) {     /* dropped on a control: a link at +25 % (kept if it had one), the mode */
            int src = g->drag_lane;
            g->drag = 0;
            if (!g->drag_moved) { mode_set(g, g->msrc == src ? 0 : src); return 3; }
            if (g->gover) {
                int why, T = ctl_tgt(g, g->gover, &why);
                if (!T) say_refuse(g, g->gover, why);
                else {
                    if (link_slot(g, src, T) < 0) {
                        int s;
                        lh_push(g);
                        if ((s = link_make(g, src, T)) >= 0) { set_param(g, ID_MAT(s, 2), 125); say_link(g, src, g->gover, 25); }
                        else g->lh_n--;
                    }
                    mode_set(g, src);
                }
            }
            g->gover = 0;
            return 3;
        }
        if (g->drag == TXG_W_MAMT) { if (g->drag_begun) drag_end(g); else g->drag = 0; return 3; }
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
        if (g->msrc && w && (w->kind == TXG_W_PARAM || w->kind == TXG_W_VOL || w->kind == TXG_W_MANY || w->kind == TXG_W_ACT ||
                                                  w->kind == TXG_W_STEP)) {   /* the mode: the amount, 4 % a step (shift: 1) */
            uint32_t id = w->kind == TXG_W_VOL ? TXP_ID_LANE(w->lane, TXP_PL_LEVEL) : w->id;
            if (id && !mode_skip(id)) {
                int why, T = ctl_tgt(g, id, &why), a;
                if (!T) { say_refuse(g, id, why); return 3; }
                a = link_amt(g, g->msrc, id) + (e->wheel > 0.f ? 1 : -1) * ((e->mods & FRG_SHIFT) ? 1 : 4);
                a = a < -100 ? -100 : a > 100 ? 100 : a;
                lh_push(g);
                link_to(g, g->msrc, id, a);
                return 3;
            }
        }
        if (g->ov == OV_SND || g->ov == OV_PRE) {   /* a list: scrolled; a choice: stepped */
            int d = e->wheel > 0.f ? -1 : 1;
            if (w && w->kind == TXG_W_SEG) { step_param(g, w->id, -d, 0); return 3; }
            if (g->ov == OV_SND) g->sb_top += d * 2; else g->pb_top += d * 2;
            if (g->sb_top < 0) g->sb_top = 0;
            if (g->pb_top < 0) g->pb_top = 0;
            return 3;
        }
        if (w && (w->kind == TXG_W_FIELD || w->kind == TXG_W_SEG || w->kind == TXG_W_PARAM || w->kind == TXG_W_STEP || w->kind == TXG_W_MANY || w->kind == TXG_W_ACT)) {
            const txp_param_t *p = pinfo(w->id);
            int d = e->wheel > 0.f ? 1 : -1;
            if (p && !choice(w->id) && p->hi >= 100 && fabsf(e->wheel) >= 1.f) d *= 2;
            ms_unmono(g, w->id);
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
        if (w && (w->kind == TXG_W_ROW || w->kind == TXG_W_ROWS || w->kind == TXG_W_SND || w->kind == TXG_W_LRUN || w->kind == TXG_W_MUTE || w->kind == TXG_W_SOLO || w->kind == TXG_W_ADD)) {
            int t0 = g->rtop;            /* (the rows: scrolled) */
            g->rtop += e->wheel > 0.f ? -1 : 1;
            rows_clamp(g);
            return t0 != g->rtop ? 3 : 2;
        }
        return 0;
    case FRG_KEY:
        if (e->key == FRK_ESCAPE && g->menu_on) { g->menu_on = 0; return 3; }
        if (e->key == FRK_ESCAPE && g->clr_ask) { g->clr_ask = 0; return 3; }
        if (e->key == FRK_ESCAPE && g->msrc && g->pick < 0 && (g->ov == OV_NONE || g->ov == OV_FX)) { mode_set(g, 0); return 3; }
        if (g->ov || g->pick >= 0) {
            if (!g->ov && e->key == FRK_ESCAPE) { g->pick = -1; return 3; }
            if (g->ov && (g->ov != OV_FX || e->key == FRK_ESCAPE || g->pick >= 0)) return ov_key(g, e);   /* (an effect's
                                          * drawer: the lanes' keys still work) */
        }
        if (e->key == FRK_UP && g->sel > 0) { select_lane(g, g->sel - 1); return 3; }
        if (e->key == FRK_DOWN && g->sel + 1 < g->nlane) { select_lane(g, g->sel + 1); return 3; }
        if (e->key == FRK_RETURN) { sb_open(g, g->sel); return 3; }   /* (the selected lane's engine browser) */
        if ((e->key == 'm' || e->key == 'M') && used(g, g->sel)) { txp_ui_mute(g->c, (uint32_t)g->sel, !(g->mute >> g->sel & 1u)); g->mute ^= 1u << g->sel; return 3; }
        if ((e->key == 's' || e->key == 'S') && used(g, g->sel)) { g->solo ^= 1u << g->sel; txp_ui_solo(g->c, g->solo); return 3; }
        if (e->key == 'v' || e->key == 'V') { g->view = (g->view + 1) % 3; txp_ui_view_set(g->c, g->view); return 3; }   /* (MAP,
                                          * SPACE, M/S in turn) */
        if (e->key == ' ') { txp_ui_rec(g->c); return 3; }   /* (space: PLAY ALL / STOP ALL) */
        return 0;
    }
    return 0;
}
int txg_input(txg_t *g, const frg_input *e)
{
    int r = input_(g, e);
    for (int s = 0; s < 16; s++) for (int q = 0; q < 4; q++) g->mx_known[s][q] = (int16_t)mx(g, s, q);   /* (the window's
                                          * own changes: its undo stays) */
    return r;
}
int txg_save_info(txg_t *g, int *slot, char name[12], char *line, int n, int *fit)
{
    if (g->ov != OV_SAVE) return 0;
    *slot = g->sv_slot; *fit = g->sv_fit;
    memcpy(name, g->sv_name, 12);
    (void)save_line(g, line, (size_t)n, 420.f - 32.f);
    return 1;
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
    g->pv_cnt = 2; g->pv_act = 4;        /* (the preview: a few, fairly calm) */
    g->sel_seen = -1;
    g->tour = -1;
    g->top_fit = g->tour_fit = g->ms_fit = g->head_fit = g->zone_fit = 1;
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
