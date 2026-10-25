/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura's screen and panel, in Frodhes' look (its ui.c, v0.9.5): the anti-aliased Inter Tight faces (aatext.c), the
 * colour tokens, the header / title / four rows / footer layout, the cards (style A: an amount is a fader fill with a
 * bright edge kept clear of the text; a choice is a row of segments; a toggle is a light), the splash, and a draw
 * scheduler that sends one element per pass, at least 4 ms apart (a full screen is ~77 ms of SPI: drawn at once it
 * would delay the keys).
 *
 * An element is drawn again when what it shows changes: each pass (every UI_SIG_MS) works out a signature of each
 * element's content and marks the ones that differ.
 *
 *   SELECT     the pages, in order: LANDSCAPE, MIXER 1/2 (levels) 2/2 (pans), SOUND 1..n (the selected lane's sound:
 *              its cards, four a page), DRIVE, FILTER 1/2 2/2, GRAIN 1/3 .. 3/3, REVERB 1/3 .. 3/3
 *   ALGORITHM  LANDSCAPE: the landscape (loaded 300 ms after the last detent); SOUND: the lane's sound; DRIVE: its
 *              type; FILTER: its type; GRAIN: FREEZE (up on, down off); REVERB: its type
 *   K1..K4     LANDSCAPE: the selected lane's SOUND, HOW MANY, HOW ACTIVE, VOLUME; MIXER: lanes 1..4; else the cards
 *   HOME       LANDSCAPE (again: MIXER)        EDIT  SOUND (again: DRIVE)        FX  GRAIN (again: REVERB)
 *   SEL        key mode: LANDSCAPE / PLAY      OCT- OCT+  LANDSCAPE mode: the lane; PLAY mode: the octave
 *   REC        LANDSCAPE mode: all lanes stop (remembered) / start again
 *   PLAY       (TX_BTN_TAP: tap tempo, held: transport; the tempo comes with piece 5)
 * The cards' values are the params table's (params.c): the same ids MIDI reaches. */
#ifdef DINGHY_HOST
#define POOL
#else
#define POOL __attribute__((section(".pool")))
#endif
#include "aatext.c"

#define TX_BTN_TAP BTN_PLAY                     /* TAP TEMPO is the PLAY button, as in Frodhes (the user, 2026-10-25) */

/* the pages: a group (SELECT walks through each group's sub-pages, then the next group) */
enum { G_LAND, G_MIX, G_SOUND, G_DRIVE, G_FILTER, G_GRAIN, G_REVERB, G_N };
static const char *const G_TITLE[G_N] = {"LANDSCAPE", "MIXER", "SOUND", "DRIVE", "FILTER", "GRAIN", "REVERB"};
static const uint8_t G_NSUB[G_N] = {1, 2, 0, 1, 2, 3, 3};   /* (SOUND: as many as its cards need) */
#define LA P_LANE_ANY
#define NOID 0xFFFFu
/* the effects' pages: a parameter per knob, the selected lane's (LA) or a global */
static const uint8_t G_FX0[G_N] = {0, 0, 0, 0, 1, 3, 6};
static const uint16_t FX_IDS[9][4] = {
    {PID(LA, PL_DRIVE0 + TX_DR_AMOUNT), PID(LA, PL_DRIVE0 + TX_DR_TONE), PID(LA, PL_DRIVE0 + TX_DR_MIX), PID(LA, PL_DRIVE0 + TX_DR_TYPE)},
    {PID(LA, PL_FILTER0 + TX_FL_TYPE), PID(LA, PL_FILTER0 + TX_FL_FREQ), PID(LA, PL_FILTER0 + TX_FL_RESO), PID(LA, PL_FILTER0 + TX_FL_LFO)},
    {PID(LA, PL_FILTER0 + TX_FL_RATE), PID(LA, PL_FILTER0 + TX_FL_SHAPE), NOID, NOID},
    {PID(P_GLOBAL, PG_GRAIN0 + TX_GD_MIX), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_TIME), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_FEEDBACK), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_GRAINS)},
    {PID(P_GLOBAL, PG_GRAIN0 + TX_GD_SIZE), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_SPRAY), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_PITCH), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_REVERSE)},
    {PID(P_GLOBAL, PG_GRAIN0 + TX_GD_SPREAD), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_TONE), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_GRAIN), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_FREEZE)},
    {PID(P_GLOBAL, PG_REVERB0 + TX_RV_TYPE), PID(P_GLOBAL, PG_REVERB0 + TX_RV_MIX), PID(P_GLOBAL, PG_REVERB0 + TX_RV_DECAY), PID(P_GLOBAL, PG_REVERB0 + TX_RV_TONE)},
    {PID(P_GLOBAL, PG_REVERB0 + TX_RV_PREDELAY), PID(P_GLOBAL, PG_REVERB0 + TX_RV_SIZE), PID(P_GLOBAL, PG_REVERB0 + TX_RV_MOD), PID(P_GLOBAL, PG_REVERB0 + TX_RV_LOWCUT)},
    {PID(P_GLOBAL, PG_REVERB0 + TX_RV_SHIMMER), PID(P_GLOBAL, PG_REVERB0 + TX_RV_SHIFT), NOID, NOID},
};
/* what ALGORITHM turns on an effect's page */
#define ALG_DRIVE PID(LA, PL_DRIVE0 + TX_DR_TYPE)
#define ALG_FILTER PID(LA, PL_FILTER0 + TX_FL_TYPE)
#define ALG_FREEZE PID(P_GLOBAL, PG_GRAIN0 + TX_GD_FREEZE)
#define ALG_REVERB PID(P_GLOBAL, PG_REVERB0 + TX_RV_TYPE)
enum { E_HEADER = 0, E_TITLE = 1, E_ROW0 = 2, E_FOOTER = 6, E_N = 7 };

static uint8_t ui_g, ui_sub;                    /* the page: its group, its sub-page */
static uint32_t ui_dirty, ui_last_draw, ui_sig[E_N], ui_sig_t;
static char ui_msg[32];
static uint32_t ui_msg_t;
static uint8_t ui_hot_card = 0xFF;
static uint32_t ui_hot_t;
static uint8_t ui_blink_shown[SC_N];


/* ---- the tokens (Frodhes' MT_LOOKS[0] on the palette) */
#define MT_DIMLINE RGB(16, 4, 8)
static struct { uint16_t lab, val, tag, unit, dim, fill, edge, frame, acc, track; } mt;
#define CD_SEGOFF RGB(8, 8, 8)
#define CD_PIPOFF RGB(56, 56, 60)
#define CD_SEGDIM RGB(36, 36, 40)                /* a choice's other options */
static uint16_t cd_shade(uint16_t c, uint32_t pct)
{
    uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    return (uint16_t)((r * pct / 100u) << 11 | (g * pct / 100u) << 5 | (b * pct / 100u));
}
static void mt_update(void)
{
    mt.lab = RGB(122, 122, 128); mt.val = RGB(236, 236, 230); mt.tag = RGB(100, 100, 106);
    mt.unit = RGB(122, 122, 128); mt.dim = RGB(90, 90, 96); mt.track = MT_DIMLINE;
    mt.acc = mt.edge = mt.frame = pal[3];
    mt.fill = cd_shade(pal[3], 45);
}
#define UI_ACC mt.acc
#define FC_BG C_BLACK
#define FC_LAB mt.lab
#define FC_VAL mt.val
#define FC_DIM mt.dim
#define S_TOP(yc) ((yc) - AF_S_CAP_Y - AF_S_CAP_H / 2)
#define L_TOP(yc) ((yc) - AF_L_CAP_Y - AF_L_CAP_H / 2)
#define HD_H 20u
#define TI_Y 20u
#define TI_H 20u
#define RW_X 6u
#define RW_Y0 46u
#define RW_DY 42u
#define RW_W 228u
#define RW_H 38u
#define FT_Y 210u
#define FT_H 30u
#define MSG_MS 1500u
#define HOT_MS 1000u
#define UI_SIG_MS 20u
#define UI_SPLASH_MS 1600u

static void ui_mark(uint32_t e) { ui_dirty |= 1u << e; }
static void ui_all(void) { ui_dirty |= (1u << E_N) - 1u; }
static void ui_say(const char *s)
{
    p_strcpy(ui_msg, s, sizeof ui_msg - 1u);
    ui_msg_t = fm1_ms ? fm1_ms : 1u;
    ui_mark(E_FOOTER);
}
static void ui_say_n(const char *s, int32_t v)  /* "HOW MANY 4" */
{
    char t[32], n[12];
    p_strcpy(t, s, 20); p_fmt_int(n, v); p_cat(t, " ", 31); p_cat(t, n, 31);
    ui_say(t);
}
static void ft_c(int32_t yc, const char *s, uint16_t c) { aa_text_c(0, S_TOP(yc), 240, &AF_S, s, c, FC_BG); }
static void ft_big(int32_t yc, const char *s, uint16_t c) { aa_text_c(0, L_TOP(yc), 240, &AF_L, s, c, FC_BG); }
/* a name cut to w px */
static void ui_fit(char *s, int32_t w)
{
    uint32_t n = 0;
    while (s[n]) n++;
    while (n && aa_text_w(&AF_S, s) > w) s[--n] = 0;
}

/* ---- what is shown */
static uint32_t ui_lane_act(uint32_t k) { return sc.run[k] ? sc_act_of(sc.step[k]) + 1u : 0u; }   /* 0 off, 1 .. 8 */
static const char *ui_count_text(uint32_t c, char *t)
{
    if (!c) { p_strcpy(t, "OFF", 4); return t; }
    p_fmt_int(t, 1 << (c - 1u));
    return t;
}
static void ui_pan_text(uint32_t p, char *t)     /* 0 .. 127: L64 .. C .. R63 */
{
    if (p == 64u) { p_strcpy(t, "C", 2); return; }
    t[0] = p < 64u ? 'L' : 'R';
    p_fmt_int(t + 1, p < 64u ? 64 - (int32_t)p : (int32_t)p - 64);
}
static uint32_t ui_nsub(uint32_t g)             /* a group's sub-pages */
{
    if (g == G_SOUND) { uint32_t n = sc.snd[sc.sel] >= 0 ? (p_cards(sc.sel)->n + 3u) / 4u : 0u; return n ? n : 1u; }
    return G_NSUB[g];
}
static int ui_lane_page(void) { return ui_g == G_SOUND || ui_g == G_DRIVE || ui_g == G_FILTER; }
static uint32_t ui_id_lane(uint32_t id) { return P_SCOPE(id) == LA ? PID(sc.sel, P_IDX(id)) : id; }
/* the parameter knob k turns on this page (NOID: none) */
static uint32_t ui_row_id(uint32_t k)
{
    if (ui_g == G_SOUND) {
        uint32_t i = ui_sub * 4u + k;
        return sc.snd[sc.sel] >= 0 && i < p_cards(sc.sel)->n ? PID(sc.sel, PL_CARD0 + i) : NOID;
    }
    if (ui_g < G_DRIVE) return NOID;
    {
        uint32_t id = FX_IDS[G_FX0[ui_g] + ui_sub][k];
        return id == NOID ? NOID : ui_id_lane(id);
    }
}
static void ui_hot(uint32_t k) { ui_hot_card = (uint8_t)k; ui_hot_t = fm1_ms ? fm1_ms : 1u; }
static void ui_hot_id(uint32_t id)              /* the card showing id, if this page has it */
{
    uint32_t k;
    for (k = 0; k < 4u; k++) if (ui_row_id(k) == id) { ui_hot(k); return; }
}

/* ---- header: the landscape (the one ALGORITHM is turning to); on a lane's pages the lane and its sound; the key
 * mode at the right */
static void draw_header(void)
{
    char nm[32], m[12];
    int32_t x = 230;
    cv_begin(240, HD_H, FC_BG);
    if (nt.mode == NT_PLAY) {
        p_strcpy(m, "PLAY", 5);
        if (nt.oct) { char o[4] = {' ', nt.oct < 0 ? '-' : '+', (char)('0' + (nt.oct < 0 ? -nt.oct : nt.oct)), 0}; p_cat(m, o, 11); }
        x -= aa_text_w(&AF_S, m); aa_text(x, S_TOP(10), &AF_S, m, UI_ACC, FC_BG); x -= 6;
    }
    if (ui_lane_page()) {
        char l[8] = {'L', 'A', 'N', 'E', ' ', (char)('1' + sc.sel), 0};
        int32_t xl = aa_text(10, S_TOP(10), &AF_S, l, UI_ACC, FC_BG) + 6;
        p_upper(nm, sc.snd[sc.sel] >= 0 ? sc_sound_name(sc.sel) : "-", 30);
        ui_fit(nm, x - xl - 4);
        aa_text(xl, S_TOP(10), &AF_S, nm, FC_VAL, FC_BG);
    } else {
        p_upper(nm, LS[sc.fam_want].name, 20);
        ui_fit(nm, x - 14);
        aa_text(10, S_TOP(10), &AF_S, nm, sc.fam_pend ? FC_VAL : FC_LAB, FC_BG);
    }
    cv_blit(0, 0);
}
static void ui_frac(char *t, uint32_t a, uint32_t b)   /* "a/b" */
{
    char n[4];
    p_fmt_int(t, (int32_t)a); p_cat(t, "/", 11); p_fmt_int(n, (int32_t)b); p_cat(t, n, 11);
}
static void draw_title(void)
{
    char t[12];
    int32_t x;
    uint32_t n = ui_nsub(ui_g);
    cv_begin(240, TI_H, FC_BG);
    x = aa_text(10, S_TOP(10), &AF_S, G_TITLE[ui_g], UI_ACC, FC_BG);
    if (n > 1u) { ui_frac(t, ui_sub + 1u, n); aa_text(x + 5, S_TOP(10), &AF_S, t, FC_VAL, FC_BG); }
    ui_frac(t, ui_g + 1u, G_N);
    aa_text_r(230, S_TOP(10), &AF_S, t, FC_LAB, FC_BG);
    cv_blit(0, TI_Y);
}

/* ---- LANDSCAPE: a strip per lane. Top: the lane's number, its sound, OFF (stopped) and the blink dot; bottom: HOW
 * MANY (6 blocks), HOW ACTIVE (8 blocks), the volume. The selected lane: its number in the accent and a frame */
#define ST_DOT_X 214
#define ST_DOT_Y 6
#define ST_DOT 8
static int ui_blink_on(uint32_t k) { uint32_t p = sc_blink_ms(k); return sc.run[k] && fm1_ms % p < p / 2u; }
static void ui_blink_dot(uint32_t k, int on, int canvas)
{
    uint16_t c = on ? mt.edge : sc.run[k] ? mt.fill : CD_SEGOFF;
    if (canvas) cv_rect(ST_DOT_X, ST_DOT_Y, ST_DOT, ST_DOT, c);
    else lcd_fill(RW_X + ST_DOT_X, RW_Y0 + RW_DY * k + ST_DOT_Y, ST_DOT, ST_DOT, c);
    ui_blink_shown[k] = (uint8_t)on;
}
static void draw_strip(uint32_t k)
{
    char nm[32], t[8];
    int sel = k == sc.sel, run = sc.run[k];
    uint32_t i, c = sc_cnt_of(k), a = ui_lane_act(k);
    cv_begin(RW_W, RW_H, C_BLACK);
    t[0] = (char)('1' + k); t[1] = 0;
    aa_text_ov(6, S_TOP(11), &AF_S, t, sel ? UI_ACC : mt.tag);
    if (sc.snd[k] >= 0) {
        p_upper(nm, sc_sound_name(k), 30);
        ui_fit(nm, ST_DOT_X - 22 - (run ? 0 : 30));
        aa_text_ov(20, S_TOP(11), &AF_S, nm, run ? FC_VAL : FC_LAB);
        if (!run) aa_text_ovr(ST_DOT_X - 6, S_TOP(11), &AF_S, "OFF", FC_DIM);
        for (i = 0; i < LS_NCNT - 1u; i++)      /* HOW MANY: 1 2 4 8 16 32 */
            cv_rect(20 + 11 * (int32_t)i, 24, 8, 7, i < c ? mt.edge : CD_SEGOFF);
        for (i = 0; i < LS_NACT; i++)           /* HOW ACTIVE: rare .. busy */
            cv_rect(92 + 12 * (int32_t)i, 24, 10, 7, i < a ? mt.edge : CD_SEGOFF);
        p_fmt_int(t, (int32_t)sc_vol_of(k));
        aa_text_ovr(222, S_TOP(28), &AF_S, t, mt.unit);
        ui_blink_dot(k, ui_blink_on(k), 1);
    } else aa_text_ov(20, S_TOP(11), &AF_S, "-", FC_DIM);
    if (sel) {
        cv_rect(0, 0, RW_W, 1, mt.frame); cv_rect(0, RW_H - 1, RW_W, 1, mt.frame);
        cv_rect(0, 0, 1, RW_H, mt.frame); cv_rect(RW_W - 1, 0, 1, RW_H, mt.frame);
    }
    cv_blit(RW_X, RW_Y0 + RW_DY * k);
}

/* ---- a card (Frodhes' style A): the K tag and the label at the left, the value large at the right (small when it
 * will not fit), its unit in a column of its own. Its look: an amount fills the row from the left (LK_MID: from the
 * middle, a pan or a pitch) to the fraction with a 2 px bright edge that stops clear of the text; a choice is a row
 * of segments under the label, the chosen one lit; a toggle is a light by the value */
enum { LK_FILL, LK_MID, LK_SEG, LK_LED };
#define CD_PAD 6
#define CD_PAD_R 12
#define CD_UNIT_W 22
#define CD_UGAP 5
#define CD_VR ((int32_t)RW_W - CD_PAD_R - CD_UNIT_W - CD_UGAP)
#define CD_BASE (((int32_t)RW_H + AF_L_CAP_H + 1) / 2)
#define CD_GAP 14
#define CD_KEEP 3
#define CD_MINSEG 3
#define CD_LABMIN 64                            /* the label keeps this much: a wider value is set small */
#define CD_SEGMAX 16u                           /* a choice of more is shown by its value alone */
#define CD_KTAG_TOP S_TOP(9)
static void cd_edge(int32_t ex, const int16_t *bx, uint32_t nb)
{
    uint64_t blocked = 0;
    uint32_t i;
    int32_t y, y0 = -1;
    for (i = 0; i < nb; i++) {
        const int16_t *b = bx + 4u * i;
        if (ex + 1 < b[0] - CD_KEEP || ex > b[1] + CD_KEEP) continue;
        for (y = b[2] - CD_KEEP; y <= b[3] + CD_KEEP; y++) if (y >= 0 && y < (int32_t)RW_H) blocked |= 1ull << y;
    }
    for (y = 0; y <= (int32_t)RW_H; y++) {
        int free_row = y < (int32_t)RW_H && !(blocked >> y & 1u);
        if (free_row && y0 < 0) y0 = y;
        if (!free_row && y0 >= 0) { if (y - y0 >= CD_MINSEG) cv_rect(ex, y0, 2, y - y0, mt.edge); y0 = -1; }
    }
}
static void draw_card(uint32_t k, const char *lab, const char *v, const char *unit, int32_t val, int32_t hi, uint32_t look)
{
    char kk[3] = {'K', (char)('1' + k), 0}, l[32];
    int hot = ui_hot_card == k && ui_hot_t && fm1_ms - ui_hot_t < HOT_MS;
    const int32_t ltop = CD_BASE - AF_S.asc, vtop = CD_BASE - AF_L.asc;
    int big = aa_covers(&AF_L, v) && aa_text_w(&AF_L, v) <= CD_VR - CD_PAD - CD_LABMIN;
    int32_t kw = aa_text_w(&AF_S, kk), lw, vw = big ? aa_text_w(&AF_L, v) : aa_text_w(&AF_S, v), fw = 0;
    int filled;
    if (look <= LK_MID && hi > 0) fw = (int32_t)RW_W * clampi(val, 0, hi) / hi;
    filled = look == LK_FILL && fw > CD_PAD;
    p_strcpy(l, lab, 31);
    ui_fit(l, CD_VR - vw - CD_GAP - CD_PAD);
    lw = aa_text_w(&AF_S, l);
    cv_begin(RW_W, RW_H, C_BLACK);
    if (fw > 0) {
        int16_t bx[8];
        int32_t x0 = look == LK_MID ? (int32_t)RW_W / 2 : 0;
        if (look == LK_MID && fw < x0) cv_rect(fw, 0, x0 - fw, RW_H, mt.fill);
        else cv_rect(x0, 0, fw - x0, RW_H, mt.fill);
        bx[0] = CD_PAD; bx[1] = (int16_t)(CD_PAD + (lw > kw ? lw : kw) - 1); bx[2] = (int16_t)(CD_KTAG_TOP + AF_S_CAP_Y); bx[3] = (int16_t)(CD_BASE - 1);
        bx[4] = (int16_t)(CD_VR - vw); bx[5] = (int16_t)(CD_VR - 1); bx[6] = (int16_t)(CD_BASE - (big ? AF_L_CAP_H : AF_S_CAP_H)); bx[7] = (int16_t)(CD_BASE - 1);
        cd_edge(clampi(look == LK_MID && fw <= (int32_t)RW_W / 2 ? fw : fw - 2, 0, (int32_t)RW_W - 2), bx, 2);
    }
    if (look == LK_SEG && hi >= 1 && (uint32_t)hi < CD_SEGMAX) {   /* a choice: hi + 1 segments, the chosen lit */
        int32_t i, n = hi + 1, x0 = CD_PAD, w = CD_VR - CD_PAD, sw = (w - 2 * (n - 1)) / n;
        for (i = 0; i < n; i++) cv_rect(x0 + i * (sw + 2), RW_H - 6, sw, 3, i == val ? mt.edge : CD_SEGDIM);
    }
    if (look == LK_LED) cv_rect(CD_VR + CD_UGAP + 6, CD_BASE - 10, 10, 10, val ? mt.edge : CD_SEGDIM);
    if (filled) {                               /* a strong fill under the K tag / label: lighter greys */
        aa_text_ov(CD_PAD, CD_KTAG_TOP, &AF_S, kk, RGB(170, 170, 176));
        aa_text_ov(CD_PAD, ltop, &AF_S, l, hot ? FC_VAL : RGB(206, 206, 210));
    } else {
        aa_text_ov(CD_PAD, CD_KTAG_TOP, &AF_S, kk, mt.tag);
        aa_text_ov(CD_PAD, ltop, &AF_S, l, hot ? UI_ACC : FC_LAB);
    }
    if (big) aa_text_ovr(CD_VR, vtop, &AF_L, v, FC_VAL);
    else aa_text_ovr(CD_VR, ltop, &AF_S, v, FC_VAL);
    if (unit) aa_text_ov(CD_VR + CD_UGAP, ltop, &AF_S, unit, filled && fw > CD_VR ? RGB(206, 206, 210) : mt.unit);
    if (hot) {
        cv_rect(0, 0, RW_W, 1, mt.frame); cv_rect(0, RW_H - 1, RW_W, 1, mt.frame);
        cv_rect(0, 0, 1, RW_H, mt.frame); cv_rect(RW_W - 1, 0, 1, RW_H, mt.frame);
    }
    cv_blit(RW_X, RW_Y0 + RW_DY * k);
}
static void ui_mix_label(uint32_t k, char *l)   /* "1 RAIN ON WATER" */
{
    l[0] = (char)('1' + k); l[1] = ' ';
    p_upper(l + 2, sc_sound_name(k), 28);
}
static uint32_t ui_look(uint32_t id)
{
    uint32_t f = p_fmt(id), kd = p_kind(id);
    if (kd == PK_TOGGLE) return LK_LED;
    if (kd == PK_SELECT) return LK_SEG;
    return f == F_PAN || f == F_SEMI ? LK_MID : LK_FILL;
}
static void draw_row(uint32_t k)
{
    char l[32], v[24];
    uint32_t id;
    if (ui_g == G_LAND) { draw_strip(k); return; }
    if (ui_g == G_MIX) {
        ui_mix_label(k, l);
        if (ui_sub == 0u) { p_fmt_int(v, (int32_t)sc_vol_of(k)); draw_card(k, l, v, 0, (int32_t)sc_vol_of(k), 127, LK_FILL); }
        else { ui_pan_text(sc.pan[k], v); draw_card(k, l, v, 0, sc.pan[k], 127, LK_MID); }
        return;
    }
    id = ui_row_id(k);
    if (id == NOID) { cv_begin(RW_W, RW_H, C_BLACK); cv_blit(RW_X, RW_Y0 + RW_DY * k); return; }
    {
        const char *u = p_text(id, v);
        draw_card(k, p_lab(id), v, u, p_get(id), p_hi(id), ui_look(id));
    }
}

static void draw_footer(void)
{
    static const char *const HINT[G_N] = {"", "", "ALGORITHM: THE SOUND", "ALGORITHM: THE DRIVE'S TYPE",
                                          "ALGORITHM: THE FILTER'S TYPE", "ALGORITHM: FREEZE", "ALGORITHM: THE REVERB'S TYPE"};
    cv_begin(240, FT_H, FC_BG);
    if (ui_msg_t && fm1_ms - ui_msg_t < MSG_MS) ft_c(15, ui_msg, FC_VAL);
    else if (ui_g == G_LAND) {
        char t[32];
        p_strcpy(t, nt.mode == NT_PLAY ? "PLAY: KEYS INTO LANE " : "LANE ", 24);
        { char n[2] = {(char)('1' + sc.sel), 0}; p_cat(t, n, 31); }
        ft_c(7, t, FC_LAB);
        ft_c(22, "SOUND   MANY   ACTIVE   VOLUME", FC_DIM);
    } else if (ui_g == G_MIX) ft_c(19, ui_sub == 0u ? "K1-K4: THE LANES' LEVELS" : "K1-K4: THE LANES' PANS", FC_DIM);
    else if (ui_g == G_SOUND && sc.snd[sc.sel] < 0) ft_c(19, "NO SOUND ON THIS LANE", FC_DIM);
    else ft_c(19, HINT[ui_g], FC_DIM);
    cv_blit(0, FT_Y);
}

/* ---- the signatures: an element is drawn again when its content changes */
static uint32_t sig_mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t ui_sig_of(uint32_t e)
{
    uint32_t h = 2166136261u, k;
    h = sig_mix(h, ui_g); h = sig_mix(h, ui_sub);
    if (ui_lane_page()) { h = sig_mix(h, sc.sel); h = sig_mix(h, (uint32_t)sc.snd[sc.sel]); h = sig_mix(h, sc.fam); }
    if (e == E_HEADER) { h = sig_mix(h, sc.fam_want); h = sig_mix(h, sc.fam_pend); h = sig_mix(h, nt.mode); h = sig_mix(h, (uint32_t)nt.oct); }
    else if (e == E_TITLE) h = sig_mix(h, ui_nsub(ui_g));
    else if (e == E_FOOTER) {
        h = sig_mix(h, ui_msg_t && fm1_ms - ui_msg_t < MSG_MS ? ui_msg_t : 0u);
        h = sig_mix(h, nt.mode); h = sig_mix(h, sc.sel);
    } else if (e >= E_ROW0 && e < E_ROW0 + 4u) {
        k = e - E_ROW0;
        h = sig_mix(h, ui_hot_card == k && ui_hot_t && fm1_ms - ui_hot_t < HOT_MS);
        if (ui_g <= G_MIX) {
            h = sig_mix(h, sc.fam); h = sig_mix(h, (uint32_t)sc.snd[k]); h = sig_mix(h, sc.run[k]);
            h = sig_mix(h, sc_vol_of(k)); h = sig_mix(h, sc.pan[k]);
            if (ui_g == G_LAND) { h = sig_mix(h, sc_cnt_of(k)); h = sig_mix(h, ui_lane_act(k)); h = sig_mix(h, k == sc.sel); }
        } else {
            uint32_t id = ui_row_id(k);
            h = sig_mix(h, id);
            if (id != NOID) h = sig_mix(h, (uint32_t)p_get(id));
        }
    }
    return h;
}

/* ---- the splash: the mark, the name, what it is, the version */
static uint32_t ui_splash_t;
static uint8_t ui_splash_on;
static void ui_splash(void)
{
    uint32_t i, r = 0x2545F491u;
    char v[24];
    mt_update();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    cv_begin(240, 64, C_BLACK);                 /* particles scattered over a line, denser to the middle */
    for (i = 0; i < 48u; i++) {
        int32_t x, y, s;
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        x = 120 + (int32_t)((r & 127u) + (r >> 7 & 127u)) - 127;
        y = 6 + (int32_t)(r >> 14 & 31u) - (int32_t)((x > 120 ? x - 120 : 120 - x) >> 3);
        s = 2 + (int32_t)(r >> 20 & 1u);
        if (y < 2) y = 2;
        cv_rect(x, y, s, s, i % 7u == 3u ? C_HI : i & 1u ? C_GRAY : C_DIM);
    }
    cv_rect(40, 44, 160, 2, C_LINE);
    cv_blit(0, 46);
    cv_begin(240, 64, C_BLACK);
    ft_big(16, "TEXTURA", UI_ACC);
    ft_c(48, "TEXTURE SYNTHESIZER", FC_LAB);
    cv_blit(0, 110);
    cv_begin(240, 20, C_BLACK);
    p_strcpy(v, "VERSION ", 8); p_upper(v + 8, APP_VER, 12);
    ft_c(10, v, FC_DIM);
    cv_blit(0, 210);
    ui_splash_t = fm1_ms;
    ui_splash_on = 1;
}

static void ui_draw(void)
{
    static uint8_t cleared;
    uint32_t e, d, k;
    mt_update();
    if (ui_splash_on) {
        if (fm1_ms - ui_splash_t < UI_SPLASH_MS) return;
        ui_splash_on = 0; cleared = 0;
        for (e = 0; e < E_N; e++) ui_sig[e] = 0;
        ui_all();
    }
    if (!cleared) { lcd_fill(0, 0, 240, 240, C_BLACK); cleared = 1; }
    if (ui_sub >= ui_nsub(ui_g)) ui_sub = (uint8_t)(ui_nsub(ui_g) - 1u);   /* (a sound with fewer cards) */
    if (p_touched) { ui_hot_id(p_touched - 1u); p_touched = 0; }          /* MIDI turned a card on this page */
    if (fm1_ms - ui_sig_t >= UI_SIG_MS) {       /* what changed */
        ui_sig_t = fm1_ms;
        for (e = 0; e < E_N; e++) {
            uint32_t s = ui_sig_of(e);
            if (s != ui_sig[e]) { ui_sig[e] = s; ui_mark(e); }
        }
    }
    if (ui_g == G_LAND && !(ui_dirty >> E_ROW0 & 0xFu))   /* the blink dots: a few pixels, at once */
        for (k = 0; k < SC_N; k++)
            if (sc.snd[k] >= 0 && ui_blink_on(k) != ui_blink_shown[k]) ui_blink_dot(k, ui_blink_on(k), 0);
    d = ui_dirty & ((1u << E_N) - 1u);
    if (!d || fm1_ms - ui_last_draw < 4u) return;
    e = d >> E_FOOTER & 1u ? E_FOOTER : d & 1u ? E_HEADER : d >> E_TITLE & 1u ? E_TITLE : 0xFF;
    if (e == 0xFF) for (e = E_ROW0; e < E_N && !(d >> e & 1u); e++) {}
    ui_dirty &= ~(1u << e);
    ui_last_draw = fm1_ms;
    if (e == E_HEADER) draw_header();
    else if (e == E_TITLE) draw_title();
    else if (e == E_FOOTER) draw_footer();
    else draw_row(e - E_ROW0);
}

/* ---- input */
static void ui_goto(uint32_t g, uint32_t sub)
{
    ui_g = (uint8_t)(g < G_N ? g : G_LAND);
    ui_sub = (uint8_t)(sub < ui_nsub(ui_g) ? sub : ui_nsub(ui_g) - 1u);
    ui_hot_card = 0xFF;
    ui_all();
}
static void ui_page_step(int32_t d)             /* SELECT: the next sub-page, then the next group */
{
    if (d > 0) {
        if (ui_sub + 1u < ui_nsub(ui_g)) ui_goto(ui_g, ui_sub + 1u);
        else if (ui_g + 1u < G_N) ui_goto(ui_g + 1u, 0);
    } else {
        if (ui_sub > 0u) ui_goto(ui_g, ui_sub - 1u);
        else if (ui_g > 0u) ui_goto(ui_g - 1u, ui_nsub(ui_g - 1u) - 1u);
    }
}
static void ui_lane_step(int32_t d)
{
    int32_t k = (int32_t)sc.sel + d;
    sc_select((uint32_t)clampi(k, 0, (int32_t)SC_N - 1));
    ui_say_n("LANE", sc.sel + 1);
}
/* a parameter turned by d detents: an amount by d, a choice by one, a toggle on (up) / off (down) */
static void ui_turn(uint32_t id, int32_t d)
{
    uint32_t kd = p_kind(id);
    int32_t v = p_get(id);
    if (kd == PK_TOGGLE) v = d > 0;
    else if (kd == PK_SELECT) v += d > 0 ? 1 : -1;
    else v += d;
    p_set(id, clampi(v, 0, p_hi(id)));
}
static void ui_knob(uint32_t k, int32_t d)
{
    uint32_t l = sc.sel;
    ui_hot(k);
    if (ui_g == G_MIX) {
        if (ui_sub == 0u) sc_volume(k, (uint32_t)clampi((int32_t)sc_vol_of(k) + d, 0, 127));
        else sc_pan(k, (uint32_t)clampi((int32_t)sc.pan[k] + d, 0, 127));
        return;
    }
    if (ui_g != G_LAND) { uint32_t id = ui_row_id(k); if (id != NOID) ui_turn(id, d); return; }
    if (sc.snd[l] < 0) return;
    if (k == 0u) {
        char t[32];
        sc_sound_turn(d > 0 ? 1 : -1);
        p_upper(t, sc_sound_name(l), 31);
        ui_say(t);
    } else if (k == 1u) {
        char t[12], m[24];
        sc_count(l, (uint32_t)clampi((int32_t)sc_cnt_of(l) + (d > 0 ? 1 : -1), 0, (int32_t)LS_NCNT - 1));
        p_strcpy(m, "HOW MANY ", 10); p_cat(m, ui_count_text(sc_cnt_of(l), t), 23);
        ui_say(m);
    } else if (k == 2u) {
        int32_t a = (int32_t)ui_lane_act(l) + (d > 0 ? 1 : -1);   /* 0 off, 1 .. 8 */
        if (a <= 0) { if (sc.run[l]) sc_count(l, 0); ui_say("HOW ACTIVE OFF"); return; }
        sc_step(l, sc_act_step((uint32_t)clampi(a - 1, 0, (int32_t)LS_NACT - 1)));
        ui_say_n("HOW ACTIVE", (int32_t)ui_lane_act(l));
    } else {
        sc_volume(l, (uint32_t)clampi((int32_t)sc_vol_of(l) + d, 0, 127));
        ui_say_n("VOLUME", (int32_t)sc_vol_of(l));
    }
}
static void ui_algorithm(int32_t d)
{
    uint32_t id = NOID;
    if (ui_g == G_LAND) { sc_family_turn(d); return; }
    if (ui_g == G_SOUND) {
        char t[32];
        if (sc.snd[sc.sel] < 0) return;
        sc_sound_turn(d > 0 ? 1 : -1);
        p_upper(t, sc_sound_name(sc.sel), 31);
        ui_say(t);
        return;
    }
    if (ui_g == G_DRIVE) id = ui_id_lane(ALG_DRIVE);
    else if (ui_g == G_FILTER) id = ui_id_lane(ALG_FILTER);
    else if (ui_g == G_GRAIN) id = ALG_FREEZE;
    else if (ui_g == G_REVERB) id = ALG_REVERB;
    if (id == NOID) return;
    ui_turn(id, d);
    ui_hot_card = 0xFF;
    ui_hot_id(id);
    {                                           /* "FREEZE ON", "REVERB HALL" */
        char t[32], v[24];
        p_strcpy(t, ui_g == G_GRAIN ? "FREEZE" : G_TITLE[ui_g], 12); p_cat(t, " ", 31);
        (void)p_text(id, v); p_cat(t, v, 31);
        ui_say(t);
    }
}
static void ui_input(uint32_t b, uint32_t rel)
{
    int32_t d[7];
    uint32_t i;
    (void)rel;
    for (i = 0; i < 7u; i++) d[i] = fm1_enc_take(i);
    if (ui_splash_on) for (i = 0; i < 7u; i++) if (d[i] || b) ui_splash_t = fm1_ms - UI_SPLASH_MS;
    if (b >> BTN_HOME & 1u) ui_goto(ui_g == G_LAND ? G_MIX : G_LAND, 0);
    if (b >> BTN_EDIT & 1u) ui_goto(ui_g == G_SOUND ? G_DRIVE : G_SOUND, 0);
    if (b >> BTN_FX & 1u) ui_goto(ui_g == G_GRAIN ? G_REVERB : G_GRAIN, 0);
    if (b >> BTN_SEL & 1u) { nt_mode_set(nt.mode == NT_PLAY ? NT_LANDSCAPE : NT_PLAY); ui_say(nt.mode == NT_PLAY ? "KEYS: PLAY" : "KEYS: LANDSCAPE"); }
    if (b >> BTN_OCT_DOWN & 1u) { if (nt.mode == NT_PLAY) nt_oct_step(-1); else ui_lane_step(-1); }
    if (b >> BTN_OCT_UP & 1u) { if (nt.mode == NT_PLAY) nt_oct_step(1); else ui_lane_step(1); }
    if (b >> BTN_REC & 1u && nt.mode == NT_LANDSCAPE) { sc_all_toggle(); ui_say(sc_any_run() ? "ALL PLAYING" : "ALL STOPPED"); }
    if (d[ENC_SELECT]) ui_page_step(d[ENC_SELECT]);
    if (d[ENC_ALGORITHM]) ui_algorithm(d[ENC_ALGORITHM]);
    for (i = 0; i < 4u; i++) if (d[ENC_KNOB1 + i]) ui_knob(i, d[ENC_KNOB1 + i]);
}
/* the buttons' LEDs: HOME, EDIT or FX on their pages, SEL in PLAY mode, REC while anything plays, OCT where it can
 * still go */
static uint32_t ui_btn_leds(void)
{
    uint32_t b = 0;
    if (ui_g <= G_MIX) b |= 1u << BTN_HOME;
    else if (ui_g <= G_FILTER) b |= 1u << BTN_EDIT;
    else b |= 1u << BTN_FX;
    if (nt.mode == NT_PLAY) b |= 1u << BTN_SEL;
    if (sc_any_run()) b |= 1u << BTN_REC;
    if (nt.mode == NT_PLAY ? nt.oct < 0 : sc.sel > 0) b |= 1u << BTN_OCT_DOWN;
    if (nt.mode == NT_PLAY ? nt.oct > 0 : sc.sel < SC_N - 1u) b |= 1u << BTN_OCT_UP;
    return b;
}
