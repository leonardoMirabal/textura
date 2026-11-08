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
 *   SELECT     the pages, in order (the first to the last, no wrap): LANDSCAPE, MIXER 1/2 (levels) 2/2 (pans), SOUND 1..n (the selected lane's sound:
 *              its cards, four a page), FILTER 1/3 (K1..K4: FREQ of lanes 1..4) 2/3 3/3 (the selected lane's: TYPE
 *              FREQ RESO LFO, then RATE SHAPE), DRIVE (the mix's, its first effect: AMOUNT TONE MIX TYPE),
 *              COMPRESSOR 1/2 2/2 (the mix's, after the DRIVE, Frodhes': THR RAT ATK MKUP, then COMP REL; the footer
 *              its gain-reduction meter), GRAIN 1/3 .. 3/3, REVERB 1/3 .. 3/3 (in series: the reverb takes what the
 *              grain delay gives; MIX on DRIVE, GRAIN, REVERB reads DRY..WET), LFO 1/3 .. 3/3,
 *              ENV 1/2 2/2 (FOLLOW: one page), MATRIX, TEMPO 1/2 2/2, MIDI, DISPLAY (COLOR: the screen's palette)
 *   ALGORITHM  LANDSCAPE: the landscape (loaded 300 ms after the last detent); SOUND: opens the sound browser; FILTER: the
 *              selected lane's type; DRIVE: its TYPE; COMPRESSOR: COMP (up on, down off); GRAIN: FREEZE (up on, down
 *              off); REVERB: its type; LFO: which (1..8); ENV:
 *              which (ENV 1, ENV 2, FOLLOW 1, FOLLOW 2); MATRIX: which slot (1..16)
 *   K1..K4     LANDSCAPE: the volumes of lanes 1..4 (a lane's sound: its SOUND page; HOW MANY, HOW ACTIVE: the white
 *              keys); MIXER, FILTER 1/3: lanes 1..4 (a turn there selects the lane: the detail pages and the white
 *              keys follow it); else the cards
 *   HOME       LANDSCAPE, remembering the page it came from; on LANDSCAPE: back to that page (none remembered,
 *              or it was left another way: LANDSCAPE stays)
 *   EDIT       SOUND (again: FILTER)               FX  DRIVE (again: COMPRESSOR, GRAIN, REVERB)
 *   GLO        TEMPO (again: MIDI, DISPLAY)        LFO  the LFO pages; held + a card turned: mod learn (the
 *              selected LFO onto that card, the turn its depth; back to 0 frees the slot)    ARP  MATRIX
 *   ENV        the ENV / FOLLOW pages; held + a card turned: mod learn from the selected ENV or FOLLOW
 *   SEL        key mode: LANDSCAPE / PLAY      OCT- OCT+  LANDSCAPE mode: the previous / next lane (a lane's page,
 *              SOUND or FILTER, any of their pages, stays, for the new lane; any other goes to LANDSCAPE); PLAY mode: the octave
 *   black keys LANDSCAPE mode, by their groups: lanes 1..4, shown on LANDSCAPE (it stays, any other page goes to it;
 *              EDIT then opens the lane's SOUND); held 600 ms (notes.c NT_HOLD_MS): that lane muted / unmuted too (it
 *              plays on, only its output gated: back at once, a short ramp; its card dimmed, MUTED; a scene keeps it)
 *   REC        LANDSCAPE mode: all lanes stop (remembered) / start again
 *   SEQ        SOUNDS: the sound browser for the selected lane, over the rows (any page stays under it): level 1 the
 *              families, level 2 a family's sounds (the lane's own marked, the other lanes' numbers at the right; any
 *              sound on any lane, the same one on two lanes too). ALGORITHM / K1 the cursor (level 2: it plays on the
 *              lane UB_PREVIEW_MS after the last detent, while the lane plays), SELECT (level 2) the family, EDIT opens
 *              / picks, HOME back / cancel (the sound it opened with back), OCT / a black key: another lane; SEQ again
 *              or any other button: closed, what plays kept
 *   SAVE       the save mode (scenes.c: PRESETS / K1 the user scene, SAVE again saves, HOME cancels); held 700 ms:
 *              undo the last scene load            PRESETS  browse the scenes (loads 400 ms after the last detent)
 *   PLAY       (TX_BTN_TAP) a tap: TAP TEMPO (on the press); held 600 ms: the transport starts / stops (and that
 *              press's tap is taken back); its light: the beat
 * The cards' values are the params table's (params.c): the same ids MIDI reaches. */
#ifdef DINGHY_HOST
#define POOL
#else
#define POOL __attribute__((section(".pool")))
#endif
#include "aatext.c"

#define TX_BTN_TAP BTN_PLAY                     /* TAP TEMPO is the PLAY button, as in Frodhes (the user, 2026-10-25) */

/* the pages: a group (SELECT walks through each group's sub-pages, then the next group) */
enum { G_LAND, G_MIX, G_SOUND, G_FILTER, G_DRIVE, G_COMP, G_GRAIN, G_REVERB, G_LFO, G_ENV, G_MATRIX, G_TEMPO, G_MIDI,
       G_DISPLAY, G_N };
static const char *const G_TITLE[G_N] = {"LANDSCAPE", "MIXER", "SOUND", "FILTER", "DRIVE", "COMPRESSOR", "GRAIN", "REVERB",
                                         "LFO", "ENV", "MATRIX", "TEMPO", "MIDI", "DISPLAY"};
static const uint8_t G_NSUB[G_N] = {1, 2, 0, 3, 1, 2, 3, 3, 3, 2, 1, 2, 1, 1};   /* (SOUND: as many as its cards need; ENV:
                                                         * a FOLLOW has one) */
#define LA P_LANE_ANY
#define NOID 0xFFFFu
/* the effects' pages: a parameter per knob, the selected lane's (LA) or a global; FILTER: its first page the four
 * lanes' (FX_IDS 22: UI_OVERVIEW), then the selected lane's from G_FX0 */
static const uint8_t G_FX0[G_N] = {0, 0, 0, 1, 0, 20, 3, 6, 9, 15, 12, 13, 18, 19};   /* (ENV: 15, 16; FOLLOW: 17) */
#define LFO_ANY(p) PID(P_LFO, 0xF0u | (p))        /* the selected LFO's (ui_lfo) */
#define ENV_ANY(p) PID(P_ENV, 0xF0u | (p))        /* the selected ENV's or FOLLOW's (ui_env) */
#define MAT_ANY(q) PID(P_MAT, 0xF0u | (q))        /* the selected slot's (ui_slot) */
#define CMP(p) PID(P_GLOBAL, PG_COMP0 + (p))
#define DRV(p) PID(P_GLOBAL, PG_DRIVE0 + (p))
static const uint16_t FX_IDS[23][4] = {
    {DRV(TX_DR_AMOUNT), DRV(TX_DR_TONE), DRV(TX_DR_MIX), DRV(TX_DR_TYPE)},   /* DRIVE: the mix's */
    {PID(LA, PL_FILTER0 + TX_FL_TYPE), PID(LA, PL_FILTER0 + TX_FL_FREQ), PID(LA, PL_FILTER0 + TX_FL_RESO), PID(LA, PL_FILTER0 + TX_FL_LFO)},
    {PID(LA, PL_FILTER0 + TX_FL_RATE), PID(LA, PL_FILTER0 + TX_FL_SHAPE), NOID, NOID},
    {PID(P_GLOBAL, PG_GRAIN0 + TX_GD_MIX), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_TIME), PID(P_GLOBAL, PG_GSYNC), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_FEEDBACK)},
    {PID(P_GLOBAL, PG_GRAIN0 + TX_GD_GRAINS), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_SIZE), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_SPRAY), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_PITCH)},
    {PID(P_GLOBAL, PG_GRAIN0 + TX_GD_REVERSE), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_SPREAD), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_TONE), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_GRAIN)},
    {PID(P_GLOBAL, PG_REVERB0 + TX_RV_TYPE), PID(P_GLOBAL, PG_REVERB0 + TX_RV_MIX), PID(P_GLOBAL, PG_REVERB0 + TX_RV_DECAY), PID(P_GLOBAL, PG_REVERB0 + TX_RV_TONE)},
    {PID(P_GLOBAL, PG_REVERB0 + TX_RV_PREDELAY), PID(P_GLOBAL, PG_REVERB0 + TX_RV_SIZE), PID(P_GLOBAL, PG_REVERB0 + TX_RV_MOD), PID(P_GLOBAL, PG_REVERB0 + TX_RV_LOWCUT)},
    {PID(P_GLOBAL, PG_REVERB0 + TX_RV_SHIMMER), PID(P_GLOBAL, PG_REVERB0 + TX_RV_SHIFT), NOID, NOID},
    {LFO_ANY(TX_LP_SHAPE), LFO_ANY(TX_LP_RATE), LFO_ANY(TX_LP_SYNC), LFO_ANY(TX_LP_MODE)},
    {LFO_ANY(TX_LP_PHASE), LFO_ANY(TX_LP_WIDTH), LFO_ANY(TX_LP_JITTER), LFO_ANY(TX_LP_FADE)},
    {LFO_ANY(TX_LP_RESTART), LFO_ANY(TX_LP_LEVEL), LFO_ANY(ML_NOW), LFO_ANY(ML_ROUTES)},
    {MAT_ANY(MQ_SRC), MAT_ANY(MQ_TGT), MAT_ANY(MQ_DEPTH), MAT_ANY(MQ_SPREAD)},
    {PID(P_GLOBAL, PG_BPM), PID(P_GLOBAL, PG_FINE), PID(P_GLOBAL, PG_STATUS), PID(P_GLOBAL, PG_RUN)},
    {PID(P_GLOBAL, PG_SCNTEMPO), PID(P_GLOBAL, PG_SCNFX), NOID, NOID},
    {ENV_ANY(TX_EP_TRIGGER), ENV_ANY(TX_EP_ATTACK), ENV_ANY(TX_EP_DECAY), ENV_ANY(TX_EP_RELEASE)},
    {ENV_ANY(TX_EP_SUSTAIN), ENV_ANY(TX_EP_CURVE), ENV_ANY(TX_EP_VARY), ENV_ANY(TX_EP_LOOP)},
    {ENV_ANY(TX_FP_INPUT), ENV_ANY(TX_FP_ATTACK), ENV_ANY(TX_FP_RELEASE), ENV_ANY(TX_FP_GAIN)},
    {PID(P_GLOBAL, PG_CLOCK), PID(P_GLOBAL, PG_INUSB), PID(P_GLOBAL, PG_INTRS), PID(P_GLOBAL, PG_OUT)},
    {PID(P_GLOBAL, PG_COLOR), NOID, NOID, NOID},
    {CMP(TX_CP_THR), CMP(TX_CP_RATIO), CMP(TX_CP_ATK), CMP(TX_CP_MKUP)},   /* COMPRESSOR: Frodhes' four knobs */
    {CMP(TX_CP_ON), CMP(TX_CP_REL), NOID, NOID},
    {PID(0, PL_FILTER0 + TX_FL_FREQ), PID(1, PL_FILTER0 + TX_FL_FREQ), PID(2, PL_FILTER0 + TX_FL_FREQ), PID(3, PL_FILTER0 + TX_FL_FREQ)},
};
/* what ALGORITHM turns on an effect's page */
#define ALG_DRIVE DRV(TX_DR_TYPE)
#define ALG_FILTER PID(LA, PL_FILTER0 + TX_FL_TYPE)
#define ALG_FREEZE PID(P_GLOBAL, PG_GRAIN0 + TX_GD_FREEZE)
#define ALG_REVERB PID(P_GLOBAL, PG_REVERB0 + TX_RV_TYPE)
#define ALG_COMP CMP(TX_CP_ON)
enum { E_HEADER = 0, E_TITLE = 1, E_ROW0 = 2, E_FOOTER = 6, E_N = 7 };

static uint8_t ui_g, ui_sub;                    /* the page: its group, its sub-page */
static uint8_t ui_lfo, ui_slot;                 /* the LFO and the matrix slot ALGORITHM picked */
static uint8_t ui_env;                          /* ENV 1, ENV 2, FOLLOW 1, FOLLOW 2 (0..3), ALGORITHM's on ENV */
#define UI_NENV (TX_ENV_N + TX_FOL_N)
static uint32_t ui_dirty, ui_last_draw, ui_sig[E_N], ui_sig_t;
static char ui_msg[32];
static uint32_t ui_msg_t;
static uint8_t ui_hot_card = 0xFF;
static uint32_t ui_hot_t;
static uint8_t ui_blink_shown[SC_N];
/* the sound browser (SEQ, or ALGORITHM on SOUND): a list over the four rows, level 1 the families, level 2 a family's
 * sounds; the cursor's sound previewed on the lane UB_PREVIEW_MS after the last detent while the lane plays */
#define UB_PREVIEW_MS 400u
#define UB_ROWS 8u                              /* list rows shown: two in each row slot */
static struct {
    uint8_t on, lv, f, cur, top, lane;          /* lv 1 families, 2 sounds; f the family shown at level 2 */
    uint8_t of;                                 /* the lane's sound when it opened (to put back: HOME) */
    int8_t oi;
    uint8_t pend;                               /* a preview waiting since t */
    uint32_t t;
} ub;


/* ---- the tokens (Frodhes' MT_LOOKS[0] on the palette) */
#define MT_DIMLINE RGB(16, 4, 8)
static struct { uint16_t lab, val, tag, unit, dim, fill, edge, frame, acc, track; } mt;
#define CD_SEGOFF RGB(8, 8, 8)
#define CD_PIPOFF RGB(56, 56, 60)
#define CD_SEGDIM RGB(36, 36, 40)                /* a choice's other options */
/* the meters' colours (Frodhes' look A, on every palette): greys fixed; the title, the edge, the frame and a turned
 * knob's label the palette's accent (pal[3]); the fader's fill the accent x 45 % (cd_shade: params.c) */
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
static uint32_t ui_lane_act(uint32_t k) { return sc.run[k] ? sc_act_of(sc.step[k]) + 1u : 0u; }   /* 0 off, 1 .. 9 */
static void ui_pan_text(uint32_t p, char *t)     /* 0 .. 127: L64 .. C .. R63 */
{
    if (p == 64u) { p_strcpy(t, "C", 2); return; }
    t[0] = p < 64u ? 'L' : 'R';
    p_fmt_int(t + 1, p < 64u ? 64 - (int32_t)p : (int32_t)p - 64);
}
static uint32_t ui_nsub(uint32_t g)             /* a group's sub-pages */
{
    if (g == G_SOUND) { uint32_t n = sc.snd[sc.sel] >= 0 ? (p_cards(sc.sel)->n + 3u) / 4u : 0u; return n ? n : 1u; }
    if (g == G_ENV && ui_env >= TX_ENV_N) return 1u;   /* (a FOLLOW: one page) */
    return G_NSUB[g];
}
static int ui_lane_page(void) { return ui_g == G_SOUND || ui_g == G_FILTER; }
#define UI_OVERVIEW() (ui_g == G_FILTER && ui_sub == 0u)   /* the four lanes' FILTER */
static int ui_lane_detail(void) { return ui_lane_page() && !UI_OVERVIEW(); }   /* a page of the selected lane's */
static uint32_t ui_id_lane(uint32_t id)         /* a page's id made concrete: the selected lane, LFO, slot */
{
    if (P_SCOPE(id) == LA) return PID(sc.sel, P_IDX(id));
    if (P_SCOPE(id) == P_LFO && P_IDX(id) >= 0xF0u) return PID(P_LFO, ui_lfo * 16u + (P_IDX(id) & 15u));
    if (P_SCOPE(id) == P_MAT && P_IDX(id) >= 0xF0u) return PID(P_MAT, ui_slot * 4u + (P_IDX(id) & 15u));
    if (P_SCOPE(id) == P_ENV && P_IDX(id) >= 0xF0u)
        return ui_env < TX_ENV_N ? PID(P_ENV, ui_env * 16u + (P_IDX(id) & 15u)) : PID(P_ENV, 32u + (ui_env - TX_ENV_N) * 16u + (P_IDX(id) & 15u));
    return id;
}
/* the parameter knob k turns on this page (NOID: none) */
static uint32_t ui_row_id(uint32_t k)
{
    if (ui_g == G_SOUND) {
        uint32_t i = ui_sub * 4u + k;
        return sc.snd[sc.sel] >= 0 && i < p_cards(sc.sel)->n ? PID(sc.sel, PL_CARD0 + i) : NOID;
    }
    if (ui_g < G_FILTER) return NOID;
    if (UI_OVERVIEW()) return FX_IDS[22][k];
    {
        uint32_t id = FX_IDS[G_FX0[ui_g] + (ui_g == G_ENV && ui_env >= TX_ENV_N ? 2u : ui_lane_page() ? ui_sub - 1u : ui_sub)][k];
        return id == NOID ? NOID : ui_id_lane(id);
    }
}
static void ui_hot(uint32_t k) { ui_hot_card = (uint8_t)k; ui_hot_t = fm1_ms ? fm1_ms : 1u; }
static void ui_hot_id(uint32_t id)              /* the card showing id, if this page has it */
{
    uint32_t k;
    for (k = 0; k < 4u; k++) if (ui_row_id(k) == id) { ui_hot(k); return; }
}

static void ui_env_name(char *l)                /* "ENV 2", "FOLLOW 1" */
{
    p_strcpy(l, ui_env < TX_ENV_N ? "ENV " : "FOLLOW ", 7);
    { char n[2] = {(char)('1' + (ui_env < TX_ENV_N ? ui_env : ui_env - TX_ENV_N)), 0}; p_cat(l, n, 11); }
}
/* ---- header: the landscape (the one ALGORITHM is turning to); on a lane's pages the lane and its sound; the key
 * mode at the right */
/* ---- the battery (Frodhes' ui.c, cfabf632): the FM-1's PB1 divider on ADC ch3 (hal/fm1_adc.h), read from the main
 * loop every BATT_POLL_MS, smoothed over ~10 s, shown as 3 segments in the header with a hysteresis so it never
 * flickers between two; USB power: a bolt. PROVISIONAL thresholds: the stock firmware's (531 / 561 / 591) */
#define BATT_T1 531
#define BATT_T2 561
#define BATT_T3 591
#define BATT_HYST 4                             /* a step needs the reading this far past its threshold */
#define BATT_POLL_MS 500u
#define BATT_HOST_RAW 600                       /* the host / web build: no ADC, a full battery */
static const int16_t BATT_T[3] = {BATT_T1, BATT_T2, BATT_T3};
static int32_t batt_avg;                        /* the smoothed reading x 256 (0: none yet) */
static uint8_t batt_lvl, batt_seen = 0xFF;      /* the segments shown; the level the header last drew */
#ifdef DINGHY_HOST
static int32_t batt_adc(void) { return BATT_HOST_RAW; }
static int batt_usb(void) { return 0; }
#else
static int32_t batt_adc(void) { return fm1_adc_read(FM1_ADC_BATT); }
static int batt_usb(void) { return usb.config && !usb.suspended; }
#endif
static uint32_t batt_shown(void) { return batt_usb() ? 4u : batt_lvl; }
static void batt_poll(void)
{
    static uint32_t t;
    int32_t a, r;
    uint32_t l;
    if (batt_avg && fm1_ms - t < BATT_POLL_MS) return;
    t = fm1_ms;
    a = batt_adc();
    if (a <= 0) return;                         /* (a timeout, or nothing on the pin) */
    if (!batt_avg) batt_avg = a * 256;
    else { int32_t d = a * 256 - batt_avg; batt_avg += (d + (d > 0 ? 10 : -10)) / 20; }
    r = (batt_avg + 128) / 256;
    for (l = 0; l < 3u && r >= BATT_T[l]; l++) {}
    if (batt_seen == 0xFF) batt_lvl = (uint8_t)l;   /* the first reading: at once */
    else if (l > batt_lvl && r >= BATT_T[l - 1u] + BATT_HYST) batt_lvl = (uint8_t)l;
    else if (l < batt_lvl && r < BATT_T[batt_lvl - 1u] - BATT_HYST) batt_lvl = (uint8_t)l;
    if (batt_shown() != batt_seen) { batt_seen = (uint8_t)batt_shown(); ui_mark(E_HEADER); }
}
/* the icon, 18 x 9 at (x, y): a body with a tip, three segments, or a bolt on USB power */
static void batt_icon(int32_t x, int32_t y, uint16_t ink, uint16_t hi, uint16_t bg)
{
    static const uint8_t BOLT[7] = {0x08, 0x10, 0x30, 0x7E, 0x0C, 0x08, 0x10};   /* 7 x 7, bit 6 = left */
    uint32_t lv = batt_shown(), i, j;
    uint16_t o = lv == 0u ? UI_ACC : ink;
    cv_rect(x, y, 15, 9, o); cv_rect(x + 1, y + 1, 13, 7, bg);
    cv_rect(x + 15, y + 2, 2, 5, o);
    if (lv == 4u) {
        for (j = 0; j < 7u; j++)
            for (i = 0; i < 7u; i++)
                if (BOLT[j] >> (6u - i) & 1u) cv_pset(x + 4 + (int32_t)i, y + 1 + (int32_t)j, hi);
        return;
    }
    for (i = 0; i < lv; i++) cv_rect(x + 2 + 4 * (int32_t)i, y + 2, 3, 5, lv == 1u ? UI_ACC : hi);
}
static void ui_bpm_text(char *b)                /* "120.0" */
{
    uint32_t t = tempo_bpm10();
    char d[3] = {'.', (char)('0' + t % 10u), 0};
    p_fmt_int(b, (int32_t)(t / 10u)); p_cat(b, d, 7);
}

static void draw_header(void)
{
    char nm[32], m[12];
    int32_t x = 230;
    cv_begin(240, HD_H, FC_BG);
    batt_icon(230 - 17, 6, FC_LAB, FC_VAL, FC_BG);   /* right to left: the battery, the tempo, PLAY / the octave */
    ui_bpm_text(m);
    x = 230 - 17 - 6 - aa_text_w(&AF_S, m);
    aa_text(x, S_TOP(10), &AF_S, m, FC_LAB, FC_BG);
    x -= 6;
    if (nt.mode == NT_PLAY) {
        p_strcpy(m, "PLAY", 5);
        if (nt.oct) { char o[4] = {' ', nt.oct < 0 ? '-' : '+', (char)('0' + (nt.oct < 0 ? -nt.oct : nt.oct)), 0}; p_cat(m, o, 11); }
        x -= aa_text_w(&AF_S, m); aa_text(x, S_TOP(10), &AF_S, m, UI_ACC, FC_BG); x -= 6;
    }
    if (ui_g == G_LFO || ui_g == G_MATRIX || ui_g == G_ENV) {   /* "LFO 3  SMOOTH", "SLOT 4  LFO 1 > L2 SIZE", "ENV 1  KEY" */
        char l[12], t[24];
        int32_t xl;
        if (ui_g == G_LFO) { p_strcpy(l, "LFO ", 4); l[4] = (char)('1' + ui_lfo); l[5] = 0; }
        else if (ui_g == G_ENV) ui_env_name(l);
        else { p_strcpy(l, "SLOT ", 5); p_fmt_int(l + 5, ui_slot + 1); }
        xl = aa_text(10, S_TOP(10), &AF_S, l, UI_ACC, FC_BG) + 6;
        if (ui_g == G_LFO) (void)p_text(PID(P_LFO, ui_lfo * 16u + TX_LP_SHAPE), nm);
        else if (ui_g == G_ENV) (void)p_text(ui_id_lane(ENV_ANY(0)), nm);   /* (its TRIGGER, or its INPUT) */
        else {
            (void)p_text(PID(P_MAT, ui_slot * 4u + MQ_SRC), nm); p_cat(nm, " > ", 31);
            (void)p_text(PID(P_MAT, ui_slot * 4u + MQ_TGT), t); p_cat(nm, t, 31);
        }
        ui_fit(nm, x - xl - 4);
        aa_text(xl, S_TOP(10), &AF_S, nm, FC_VAL, FC_BG);
    } else if (ui_lane_detail()) {
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
    if (ub.on) {                                /* the browser: "L2 FAMILY 3/10", "L2 ICE & GLASS 2/10" */
        char l[4] = {'L', (char)('1' + ub.lane), 0}, nm[32];
        x = aa_text(10, S_TOP(10), &AF_S, l, UI_ACC, FC_BG) + 6;
        if (ub.lv == 1) p_strcpy(nm, "SOUNDS: FAMILY", 31); else p_upper(nm, LS[ub.f].name, 31);
        ui_fit(nm, 230 - 40 - x);
        aa_text(x, S_TOP(10), &AF_S, nm, ub.lv == 1 ? UI_ACC : FC_VAL, FC_BG);
        ui_frac(t, ub.cur + 1u, ub.lv == 1 ? LS_N : LS[ub.f].n);
        aa_text_r(230, S_TOP(10), &AF_S, t, FC_LAB, FC_BG);
        cv_blit(0, TI_Y);
        return;
    }
    x = aa_text(10, S_TOP(10), &AF_S, ui_g == G_ENV && ui_env >= TX_ENV_N ? "FOLLOW" : ui_g == G_LAND ? "TEXTURA"
                : G_TITLE[ui_g], UI_ACC, FC_BG);   /* (the home page shows TEXTURA, as Frodhes' main page FRODHES; its
                                                 * name stays LANDSCAPE for the host / web API) */
    if (n > 1u) { ui_frac(t, ui_sub + 1u, n); x = aa_text(x + 5, S_TOP(10), &AF_S, t, FC_VAL, FC_BG); }
    if (ui_lane_detail() && ui_g != G_SOUND) {  /* "FILTER 2/3  LANE 2": the lane these cards are */
        char l[8] = {'L', 'A', 'N', 'E', ' ', (char)('1' + sc.sel), 0};
        aa_text(x + 10, S_TOP(10), &AF_S, l, UI_ACC, FC_BG);
    }
    ui_frac(t, ui_g + 1u, G_N);
    aa_text_r(230, S_TOP(10), &AF_S, t, FC_LAB, FC_BG);
    cv_blit(0, TI_Y);
}

/* ---- LANDSCAPE (the home page, titled TEXTURA): a card per lane, as the other pages' K1..K4. Its fill: the lane's
 * volume (dimmer while stopped). Top: the K tag, then OFF (stopped) or HOW MANY (6 blocks) and HOW ACTIVE (8), the
 * volume and the blink dot; below: the sound, large when it fits. The selected lane: its tag in the accent, a frame */
#define ST_DOT_X 214
#define ST_DOT_Y 6
#define ST_DOT 8
static int ui_blink_on(uint32_t k) { uint32_t p = sc_blink_ms(k); return sc.run[k] && !sc.mute[k] && fm1_ms % p < p / 2u; }   /* (muted: still) */
static void ui_blink_dot(uint32_t k, int on, int canvas)
{
    uint16_t c = on ? mt.edge : sc.run[k] ? mt.fill : CD_SEGOFF;
    if (canvas) cv_rect(ST_DOT_X, ST_DOT_Y, ST_DOT, ST_DOT, c);
    else lcd_fill(RW_X + ST_DOT_X, RW_Y0 + RW_DY * k + ST_DOT_Y, ST_DOT, ST_DOT, c);
    ui_blink_shown[k] = (uint8_t)on;
}
static void cd_edge(int32_t ex, const int16_t *bx, uint32_t nb);
/* the sounds' names: all four in one size, large only when every one ends 4 px clear of the volume column (its
 * widest value, 127, right-aligned by the dot), so no name changes size with its volume */
static int ui_home_big(void)
{
    char nm[32];
    uint32_t k;
    for (k = 0; k < SC_N; k++) {
        if (sc.snd[k] < 0) continue;
        p_upper(nm, sc_sound_name(k), 30);
        if (!aa_covers(&AF_L, nm) || 6 + aa_text_w(&AF_L, nm) + 4 > ST_DOT_X - 6 - aa_text_w(&AF_S, "127")) return 0;
    }
    return 1;
}
static void draw_strip(uint32_t k)
{
    char nm[32], t[8], kk[3] = {'K', (char)('1' + k), 0};
    int sel = k == sc.sel, run = sc.run[k], mut = sc.mute[k], lit = run && !mut;   /* (muted: drawn as stopped, MUTED) */
    uint32_t i, c = sc_cnt_of(k), a = ui_lane_act(k), v = sc_vol_of(k);
    const int32_t base = (int32_t)RW_H - 4;     /* the name's and the level's baseline, under the status line */
    int32_t x, vw, fw;
    int big;
    cv_begin(RW_W, RW_H, C_BLACK);
    if (sc.snd[k] < 0) {
        aa_text_ov(6, S_TOP(9), &AF_S, kk, mt.tag);
        aa_text_ov(6, base - AF_S.asc, &AF_S, "-", FC_DIM);
        cv_blit(RW_X, RW_Y0 + RW_DY * k);
        return;
    }
    fw = (int32_t)RW_W * (int32_t)v / 127;      /* the level: the card's fill (dimmer while the lane is off) */
    if (fw > 0) cv_rect(0, 0, fw, RW_H, lit ? mt.fill : cd_shade(mt.fill, 45));
    p_fmt_int(t, (int32_t)v);
    vw = aa_text_w(&AF_S, t);
    big = ui_home_big();
    p_upper(nm, sc_sound_name(k), 30);
    if (!big) ui_fit(nm, ST_DOT_X - 6 - aa_text_w(&AF_S, "127") - 4);
    if (fw > 2 && lit) {                        /* the bright edge, clear of the name and the level */
        int16_t bx[8] = {6, (int16_t)(6 + aa_text_w(big ? &AF_L : &AF_S, nm)), (int16_t)(base - (big ? AF_L_CAP_H : AF_S_CAP_H)),
                         (int16_t)(base - 1), (int16_t)(ST_DOT_X - 6 - vw), (int16_t)(ST_DOT_X - 7),
                         (int16_t)(S_TOP(9) + AF_S_CAP_Y), (int16_t)(S_TOP(9) + AF_S_CAP_Y + AF_S_CAP_H - 1)};
        cd_edge(fw - 2, bx, 2);
    }
    x = aa_text_ov(6, S_TOP(9), &AF_S, kk, sel ? UI_ACC : (fw > 40 ? RGB(170, 170, 176) : mt.tag)) + 8;
    if (mut) aa_text_ov(x, S_TOP(9), &AF_S, run ? "MUTED" : "MUTED, OFF", UI_ACC);
    else if (!run) aa_text_ov(x, S_TOP(9), &AF_S, "OFF", fw > x + 30 ? RGB(206, 206, 210) : FC_LAB);
    else {                                      /* HOW MANY (6) and HOW ACTIVE (9), on the status line */
        for (i = 0; i < LS_NCNT - 1u; i++) cv_rect(x + 7 * (int32_t)i, 5, 5, 6, i < c ? mt.edge : CD_SEGOFF);
        x += 7 * (int32_t)(LS_NCNT - 1u) + 8;
        for (i = 0; i < LS_NACT; i++) cv_rect(x + 7 * (int32_t)i, 5, 5, 6, i < a ? FC_VAL : CD_SEGOFF);
    }
    ui_blink_dot(k, ui_blink_on(k), 1);
    aa_text_ovr(ST_DOT_X - 6, S_TOP(9), &AF_S, t, lit ? FC_VAL : FC_LAB);   /* the level, by the dot */
    aa_text_ov(6, base - (big ? AF_L.asc : AF_S.asc), big ? &AF_L : &AF_S, nm, lit ? FC_VAL : FC_LAB);
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
enum { LK_FILL, LK_MID, LK_SEG, LK_LED, LK_TEXT };   /* (LK_TEXT: read only, the value alone) */
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
static int32_t ui_card_live = -1;               /* the card being drawn is modulated: its value now (-1: not) */
static void draw_card(uint32_t k, const char *lab, const char *v, const char *unit, int32_t val, int32_t hi, uint32_t look)
{
    char kk[3] = {'K', (char)('1' + k), 0}, l[32];
    int32_t live = ui_card_live;
    int hot = ui_hot_card == k && ui_hot_t && fm1_ms - ui_hot_t < HOT_MS;
    const int32_t ltop = CD_BASE - AF_S.asc, vtop = CD_BASE - AF_L.asc;
    int big = aa_covers(&AF_L, v) && aa_text_w(&AF_L, v) <= CD_VR - CD_PAD - CD_LABMIN;
    int32_t kw = aa_text_w(&AF_S, kk), lw, vw = big ? aa_text_w(&AF_L, v) : aa_text_w(&AF_S, v), fw = 0;
    int filled;
    if (look <= LK_MID && hi > 0) fw = (int32_t)RW_W * clampi(val, 0, hi) / hi;
    filled = look == LK_FILL && fw > CD_PAD;
    ui_card_live = -1;
    if (live >= 0) { l[0] = '~'; p_strcpy(l + 1, lab, 30); }   /* (modulated: "~" and a marker at its value now) */
    else p_strcpy(l, lab, 31);
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
    if (live >= 0 && hi > 0) {                  /* the live value: a tick at the top and the bottom */
        int32_t x = clampi((int32_t)RW_W * clampi(live, 0, hi) / hi - 1, 0, (int32_t)RW_W - 3);
        cv_rect(x, 0, 3, 4, FC_VAL); cv_rect(x, RW_H - 4, 3, 4, FC_VAL);
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
    if (p_hi(id) == 0) return LK_TEXT;
    if (kd == PK_TOGGLE) return LK_LED;
    if (kd == PK_SELECT) return LK_SEG;
    return f == F_PAN || f == F_SEMI ? LK_MID : LK_FILL;
}
/* the browser's row slot k: list rows 2k, 2k + 1 of the window (8 rows over the slots' 164 px: 20 or 21 px each, the
 * slot's canvas taking the gap below it, the last none). A row: the lane's own family / sound marked (a square), the
 * name, at the right the family's count (level 1) or the other lanes with that sound (level 2); the cursor filled and
 * framed, as the selected card */
#define UB_Y(j) ((int32_t)(RW_DY * 4u - (RW_DY - RW_H)) * (int32_t)(j) / (int32_t)UB_ROWS)
static void draw_browser_row(uint32_t k)
{
    int32_t h = k < 3u ? (int32_t)RW_DY : (int32_t)RW_H, y0 = (int32_t)RW_DY * (int32_t)k;
    uint32_t r, n = ub.lv == 1 ? LS_N : LS[ub.f].n;
    cv_begin(RW_W, (uint32_t)h, C_BLACK);
    for (r = 2u * k; r < 2u * k + 2u; r++) {
        uint32_t i = ub.top + r, j;
        int32_t ya = UB_Y(r) - y0, yb = UB_Y(r + 1u) - y0, yc = (ya + yb) / 2;
        char nm[32], t[12];
        int cur, mine;
        if (i >= n) continue;
        cur = i == ub.cur;
        if (ub.lv == 1) { mine = sc.snd[ub.lane] >= 0 && sc.lf[ub.lane] == i; p_upper(nm, LS[i].name, 31); p_fmt_int(t, LS[i].n); }
        else {
            mine = sc.snd[ub.lane] == (int)i && sc.lf[ub.lane] == ub.f;
            p_upper(nm, LS[ub.f].s[i].name, 31);
            t[0] = 0;
            for (j = 0; j < SC_N; j++)
                if (j != ub.lane && sc.snd[j] == (int)i && sc.lf[j] == ub.f) { char c[3] = {(char)('1' + j), ' ', 0}; p_cat(t, c, 11); }
        }
        if (cur) {
            cv_rect(0, ya, RW_W, yb - ya - 1, mt.fill);
            cv_rect(0, ya, RW_W, 1, mt.frame); cv_rect(0, yb - 2, RW_W, 1, mt.frame);
            cv_rect(0, ya, 1, yb - ya - 1, mt.frame); cv_rect(RW_W - 1, ya, 1, yb - ya - 1, mt.frame);
        }
        if (mine) cv_rect(6, yc - 3, 6, 6, UI_ACC);
        ui_fit(nm, (int32_t)RW_W - 18 - 40);
        aa_text_ov(18, S_TOP(yc), &AF_S, nm, cur ? FC_VAL : mine ? FC_VAL : FC_LAB);
        if (t[0]) aa_text_ovr((int32_t)RW_W - 8, S_TOP(yc), &AF_S, t, cur ? FC_VAL : FC_DIM);
    }
    cv_blit(RW_X, RW_Y0 + RW_DY * k);
}
static void draw_row(uint32_t k)
{
    char l[32], v[24];
    uint32_t id;
    if (ub.on) { draw_browser_row(k); return; }
    if (ui_g == G_LAND) { draw_strip(k); return; }
    if (ui_g == G_MIX) {
        ui_mix_label(k, l);
        { int32_t lv; if (mod_live(PID(k, ui_sub ? PL_PAN : PL_LEVEL), &lv)) ui_card_live = lv; }
        if (ui_sub == 0u) { p_fmt_int(v, (int32_t)sc_vol_of(k)); draw_card(k, l, v, 0, (int32_t)sc_vol_of(k), 127, LK_FILL); }
        else { ui_pan_text(sc.pan[k], v); draw_card(k, l, v, 0, sc.pan[k], 127, LK_MID); }
        return;
    }
    id = ui_row_id(k);
    if (id == NOID) { cv_begin(RW_W, RW_H, C_BLACK); cv_blit(RW_X, RW_Y0 + RW_DY * k); return; }
    if (UI_OVERVIEW()) {                        /* lane k's AMOUNT / FREQ, its card named as MIXER's ("2 RAIN") */
        const char *u = p_text(id, v);
        int32_t lv;
        ui_mix_label(k, l);
        if (mod_live(id, &lv)) ui_card_live = lv;
        draw_card(k, l, v, u, p_get(id), p_hi(id), ui_look(id));
        return;
    }
    {
        const char *u = p_text(id, v);
        int32_t lv, val = p_get(id), hi = p_hi(id);
        uint32_t look = ui_look(id);
        if (P_SCOPE(id) == P_LFO && P_IDX(id) % 16u == ML_NOW) { look = LK_MID; hi = 64; }   /* (NOW: from the middle) */
        if (mod_live(id, &lv)) ui_card_live = lv;
        draw_card(k, p_lab(id), v, u, val, hi, look);
    }
}

static void draw_meter(void);
static void draw_footer(void)
{
    static const char *const HINT[G_N] = {"", "", "ALGORITHM: THE SOUND", "ALGORITHM: THE FILTER'S TYPE",
                                          "ALGORITHM: THE DRIVE'S TYPE", "ALGORITHM: COMP ON / OFF", "ALGORITHM: FREEZE",
                                          "ALGORITHM: THE REVERB'S TYPE",
                                          "ALGORITHM: WHICH LFO", "ALGORITHM: ENV 1 2, FOLLOW 1 2", "ALGORITHM: WHICH SLOT",
                                          "PLAY: TAP, HOLD: START / STOP", "CLOCK OUT: IN MASTER", "THE SCREEN'S COLOUR, KEPT"};
    cv_begin(240, FT_H, FC_BG);
    if (scn_save_mode) {                   /* the save mode: where to, the name (its letter underlined) */
        char t[32], m[20];
        int32_t x0, xc, wc;
        scn_name_of(SCN_NF + scn_save_slot, m); m[4] = 0;
        p_strcpy(t, "SAVE TO ", 31); p_cat(t, m, 31); p_cat(t, scn_nm, 31);
        x0 = (240 - aa_text_w(&AF_S, t)) / 2;
        ft_c(7, t, FC_VAL);
        { char c[2] = {scn_nm[scn_nm_pos], 0}; t[12 + scn_nm_pos] = 0; xc = x0 + aa_text_w(&AF_S, t); wc = c[0] != ' ' ? aa_text_w(&AF_S, c) : 6; }
        cv_rect(xc, 13, wc, 2, mt.edge);
        ft_c(23, "K2 K3: NAME  SAVE: YES  HOME: NO", FC_DIM);
    }
    else if (ui_msg_t && fm1_ms - ui_msg_t < MSG_MS) ft_c(15, ui_msg, FC_VAL);
    else if (ub.on) {                           /* the browser: what the controls do */
        ft_c(7, ub.lv == 1 ? "ALGORITHM: MOVE   EDIT: OPEN" : "ALGORITHM: SOUND   SELECT: FAMILY", FC_LAB);
        ft_c(22, ub.lv == 1 ? "HOME: CANCEL" : "EDIT: PICK   HOME: BACK", FC_DIM);
    }
    else if (ui_g == G_LAND) {
        char t[32];
        p_strcpy(t, nt.mode == NT_PLAY ? "PLAY: KEYS INTO LANE " : "LANE ", 24);
        { char n[2] = {(char)('1' + sc.sel), 0}; p_cat(t, n, 31); }
        if (sc.mute[sc.sel]) p_cat(t, " (MUTED)", 31);
        ft_c(7, t, FC_LAB);
        ft_c(22, "SOUND   MANY   ACTIVE   VOLUME", FC_DIM);
    } else if (ui_g == G_MIX) ft_c(19, ui_sub == 0u ? "K1-K4: THE LANES' LEVELS" : "K1-K4: THE LANES' PANS", FC_DIM);
    else if (UI_OVERVIEW()) {                   /* the four lanes' page: what K1..K4 turn, then the detail's lane */
        char t[32];
        ft_c(7, "K1-K4: THE LANES' CUTOFF", FC_DIM);
        p_strcpy(t, "ALGORITHM: LANE ", 31); { char n[2] = {(char)('1' + sc.sel), 0}; p_cat(t, n, 31); } p_cat(t, "'S TYPE", 31);
        ft_c(22, t, FC_DIM);
    }
    else if (ui_g == G_SOUND && sc.snd[sc.sel] < 0) ft_c(19, "NO SOUND ON THIS LANE", FC_DIM);
    else if (ui_g == G_COMP) {                  /* the gain-reduction meter (Frodhes' draw_meter), or how to start it */
        if (pv_glob[PG_COMP0 + TX_CP_ON]) { draw_meter(); return; }
        ft_c(19, "ALGORITHM: COMP ON", FC_DIM);
    }
    else if (ui_g == G_LFO) { ft_c(7, HINT[ui_g], FC_DIM); ft_c(22, "HOLD LFO + TURN A CARD: LEARN", FC_DIM); }
    else if (ui_g == G_ENV) {                   /* the hint, the learn, and its value now: a bar */
        int32_t v = mod_ext[ui_env < TX_ENV_N ? ME_ENV1 + ui_env : ME_FOL1 + ui_env - TX_ENV_N];
        ft_c(7, HINT[ui_g], FC_DIM); ft_c(22, "HOLD ENV + TURN A CARD: LEARN", FC_DIM);
        cv_rect(20, 14, 200, 3, CD_SEGDIM); cv_rect(20, 14, 200 * clampi(v, 0, 32767) / 32767, 3, mt.edge);
    }
    else if (ui_g == G_MATRIX && mslot[ui_slot].src && mslot[ui_slot].tgt) {   /* the slot's target now: a bar */
        int32_t lv, id = (int32_t)mod_tgt_pid(mslot[ui_slot].tgt, mod_tgt_lanes(mslot[ui_slot].tgt) < 4u ? mod_tgt_lanes(mslot[ui_slot].tgt) : 0u);
        ft_c(7, mod_cc_learn ? "SEND A CC: MOD CC" : HINT[ui_g], FC_DIM);
        if (mod_live((uint32_t)id, &lv) && p_hi((uint32_t)id) > 0) {
            int32_t w = 200 * clampi(lv, 0, p_hi((uint32_t)id)) / p_hi((uint32_t)id);
            cv_rect(20, 20, 200, 4, CD_SEGDIM); cv_rect(20, 20, w, 4, mt.edge);
        }
    }
    else ft_c(19, HINT[ui_g], FC_DIM);
    cv_blit(0, FT_Y);
}

/* ---- the compressor's gain reduction, as a hardware meter reads (Frodhes' draw_meter): a full-width bar filled from
 * the right (1 .. 30 dB on the scale GR_DB10, its 15 steps evenly apart), a peak that holds for a second, the reading
 * in dB; it rises at once and falls at 15 dB/s, like a meter's needle (meter_poll, each signature pass) */
#define GR_SEGS 15u
#define GR_X 6
#define GR_W 228
static const int16_t GR_DB10[GR_SEGS] = {10, 20, 30, 40, 50, 60, 80, 100, 120, 150, 180, 200, 240, 270, 300};
static int32_t gr_db10;
static uint8_t gr_peak;
static uint32_t gr_t, gr_peak_t;
static int32_t gr_px(int32_t db10)              /* a reduction's place on the bar, from its right end */
{
    uint32_t i;
    int32_t lo = 0;
    for (i = 0; i < GR_SEGS; i++) {
        if (db10 <= GR_DB10[i]) return ((int32_t)i * GR_W + GR_W * (db10 - lo) / (GR_DB10[i] - lo)) / (int32_t)GR_SEGS;
        lo = GR_DB10[i];
    }
    return GR_W;
}
static void draw_meter(void)
{
    char t[12];
    uint32_t n = 0;
    int32_t db10 = gr_db10, fw = gr_px(db10), pk = gr_peak ? gr_px(GR_DB10[gr_peak - 1u]) : 0;
    cv_begin(240, FT_H, FC_BG);
    cv_rect(GR_X, 3, GR_W, 9, mt.track);
    cv_rect(GR_X + GR_W - fw, 3, fw, 9, mt.fill);
    cv_rect(GR_X + GR_W - (fw > 2 ? fw : 2), 3, 2, 9, mt.edge);   /* the fill's edge (at 0: the bar's right end) */
    if (pk > fw) cv_rect(GR_X + GR_W - pk, 3, 2, 9, FC_VAL);    /* the peak */
    t[n++] = '-'; p_fmt_int(t + n, db10 / 10);
    while (t[n]) n++;
    t[n++] = '.'; t[n++] = (char)('0' + db10 % 10); t[n++] = ' '; t[n++] = 'd'; t[n++] = 'B'; t[n] = 0;
    aa_text(10, S_TOP(21), &AF_S, "GAIN REDUCTION", FC_LAB, FC_BG);
    aa_text_r(230, S_TOP(21), &AF_S, db10 ? t : "0.0 dB", db10 ? FC_VAL : FC_DIM, FC_BG);
    cv_blit(0, FT_Y);
}
static uint32_t meter_poll(void)                /* ~30 Hz on the COMPRESSOR page: the meter's state (a signature) */
{
    uint32_t segs = 0;
    int32_t g;
    if (fm1_ms - gr_t >= 33u) {
        gr_t = fm1_ms;
        g = pv_glob[PG_COMP0 + TX_CP_ON] ? tx_comp_gr_db10(&ln.mix.comp) : 0;
        if (g > 300) g = 300;
        gr_db10 = g > gr_db10 ? g : gr_db10 > g + 5 ? gr_db10 - 5 : g;
        while (segs < GR_SEGS && gr_db10 >= GR_DB10[segs]) segs++;
        if (segs >= gr_peak || fm1_ms - gr_peak_t > 1000u) { gr_peak = (uint8_t)segs; gr_peak_t = fm1_ms; }
    }
    return (uint32_t)gr_db10 | (uint32_t)gr_peak << 9 | (uint32_t)pv_glob[PG_COMP0 + TX_CP_ON] << 14;
}

/* ---- the signatures: an element is drawn again when its content changes */
static uint32_t sig_mix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }
static uint32_t ui_sig_of(uint32_t e)
{
    uint32_t h = 2166136261u, k;
    h = sig_mix(h, ui_g); h = sig_mix(h, ui_sub);
    if (ub.on && e != E_HEADER) {               /* the browser: the list, the lanes' sounds in it */
        h = sig_mix(h, 0xB0u | ub.lv); h = sig_mix(h, ub.f); h = sig_mix(h, ub.cur); h = sig_mix(h, ub.top); h = sig_mix(h, ub.lane);
        for (k = 0; k < SC_N; k++) { h = sig_mix(h, (uint32_t)sc.snd[k]); h = sig_mix(h, sc.lf[k]); }
        if (e == E_FOOTER) h = sig_mix(h, ui_msg_t && fm1_ms - ui_msg_t < MSG_MS ? ui_msg_t : 0u);
        return h;
    }
    if (ui_lane_page()) { h = sig_mix(h, sc.sel); h = sig_mix(h, (uint32_t)sc.snd[sc.sel]); h = sig_mix(h, sc.lf[sc.sel]); }
    if (UI_OVERVIEW() && e >= E_ROW0 && e < E_ROW0 + 4u) h = sig_mix(h, (uint32_t)sc.snd[e - E_ROW0]);   /* (its name) */
    if (ui_g == G_ENV) {
        h = sig_mix(h, ui_env);
        if (e == E_HEADER) h = sig_mix(h, (uint32_t)p_get(ui_id_lane(ENV_ANY(0))));
        if (e == E_FOOTER) h = sig_mix(h, (uint32_t)mod_ext[ui_env < TX_ENV_N ? ME_ENV1 + ui_env : ME_FOL1 + ui_env - TX_ENV_N] >> 10);
    }
    if (ui_g == G_LFO || ui_g == G_MATRIX) {
        h = sig_mix(h, ui_lfo); h = sig_mix(h, ui_slot);
        if (e == E_HEADER) { h = sig_mix(h, mod_lfo[ui_lfo].p[TX_LP_SHAPE]); h = sig_mix(h, mslot[ui_slot].src); h = sig_mix(h, mslot[ui_slot].tgt); }
        if (e == E_FOOTER) { int32_t lv; h = sig_mix(h, mod_cc_learn); if (ui_g == G_MATRIX && mslot[ui_slot].tgt) h = sig_mix(h, mod_live((uint32_t)mod_tgt_pid(mslot[ui_slot].tgt, mod_tgt_lanes(mslot[ui_slot].tgt) < 4u ? mod_tgt_lanes(mslot[ui_slot].tgt) : 0u), &lv) ? (uint32_t)lv : 0xFFFFu); }
    }
    if (e == E_HEADER) { h = sig_mix(h, tempo_bpm10()); h = sig_mix(h, batt_shown()); h = sig_mix(h, sc.fam_want); h = sig_mix(h, sc.fam_pend); h = sig_mix(h, nt.mode); h = sig_mix(h, (uint32_t)nt.oct); }
    else if (e == E_TITLE) h = sig_mix(h, ui_nsub(ui_g));
    else if (e == E_FOOTER) {
        h = sig_mix(h, ui_msg_t && fm1_ms - ui_msg_t < MSG_MS ? ui_msg_t : 0u);
        h = sig_mix(h, nt.mode); h = sig_mix(h, sc.sel); h = sig_mix(h, sc.mute[sc.sel]);
        h = sig_mix(h, scn_save_mode ? scn_save_slot + 1u : 0u);
        if (ui_g == G_COMP) h = sig_mix(h, meter_poll());
        if (scn_save_mode) { uint32_t i; h = sig_mix(h, scn_nm_pos); for (i = 0; i < 11u; i++) h = sig_mix(h, (uint8_t)scn_nm[i]); }
    } else if (e >= E_ROW0 && e < E_ROW0 + 4u) {
        k = e - E_ROW0;
        h = sig_mix(h, ui_hot_card == k && ui_hot_t && fm1_ms - ui_hot_t < HOT_MS);
        if (ui_g <= G_MIX) {
            h = sig_mix(h, sc.lf[k]); h = sig_mix(h, (uint32_t)sc.snd[k]); h = sig_mix(h, sc.run[k]);
            h = sig_mix(h, sc_vol_of(k)); h = sig_mix(h, sc.pan[k]); h = sig_mix(h, sc.mute[k]);
            if (ui_g == G_MIX) { int32_t lv; h = sig_mix(h, mod_live(PID(k, ui_sub ? PL_PAN : PL_LEVEL), &lv) ? (uint32_t)lv / 4u + 1u : 0u); }
            if (ui_g == G_LAND) { h = sig_mix(h, sc_cnt_of(k)); h = sig_mix(h, ui_lane_act(k)); h = sig_mix(h, k == sc.sel); }
        } else {
            uint32_t id = ui_row_id(k);
            h = sig_mix(h, id);
            if (id != NOID) h = sig_mix(h, (uint32_t)p_get(id));
            if (ui_g == G_GRAIN) { h = sig_mix(h, tm.gsync); h = sig_mix(h, tm.gsync_half); }   /* (TIME: the division) */
            if (id != NOID) { int32_t lv; if (mod_live(id, &lv)) h = sig_mix(h, (uint32_t)(lv * 32 / (p_hi(id) ? p_hi(id) : 1)) + 1000u); }
            if (ui_g == G_MATRIX || ui_g == G_LFO) { h = sig_mix(h, sc.sel); h = sig_mix(h, (uint32_t)sc.snd[sc.sel]); }
            if (ui_g >= G_TEMPO) { h = sig_mix(h, tempo_bpm10()); h = sig_mix(h, (uint32_t)clk_follows()); h = sig_mix(h, clk_src); }
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
    batt_poll();
    if (ui_splash_on) {
        if (fm1_ms - ui_splash_t < UI_SPLASH_MS) return;
        ui_splash_on = 0; cleared = 0;
        for (e = 0; e < E_N; e++) ui_sig[e] = 0;
        ui_all();
    }
    if (!cleared) { lcd_fill(0, 0, 240, 240, C_BLACK); cleared = 1; }
    {   /* COLOR turned (a knob, MIDI, the web): the whole screen again in it */
        static uint16_t acc;
        if (acc != pal[3]) { if (acc) ui_all(); acc = pal[3]; }
    }
    if (ui_sub >= ui_nsub(ui_g)) ui_sub = (uint8_t)(ui_nsub(ui_g) - 1u);   /* (a sound with fewer cards) */
    if (p_touched) { ui_hot_id(p_touched - 1u); p_touched = 0; }          /* MIDI turned a card on this page */
    if (fm1_ms - ui_sig_t >= UI_SIG_MS) {       /* what changed */
        ui_sig_t = fm1_ms;
        for (e = 0; e < E_N; e++) {
            uint32_t s = ui_sig_of(e);
            if (s != ui_sig[e]) { ui_sig[e] = s; ui_mark(e); }
        }
    }
    if (ui_g == G_LAND && !ub.on && !(ui_dirty >> E_ROW0 & 0xFu))   /* the blink dots: a few pixels, at once */
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
static uint8_t ui_back_g = 0xFF, ui_back_sub;  /* HOME's way back: the page it left for LANDSCAPE (0xFF: none) */
static void ui_goto(uint32_t g, uint32_t sub)
{
    if (g != G_LAND) ui_back_g = 0xFF;          /* (LANDSCAPE left: the way back is gone) */
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
/* ---- the sound browser's moves */
static void ub_view(void)                       /* the window kept round the cursor */
{
    if (ub.cur < ub.top) ub.top = ub.cur;
    if (ub.cur >= ub.top + UB_ROWS) ub.top = (uint8_t)(ub.cur - UB_ROWS + 1u);
}
static void ub_at_lane(void)                    /* level 2 at the lane's sound (an empty lane: the landscape's first) */
{
    uint32_t k = ub.lane;
    ub.lv = 2; ub.f = sc.snd[k] >= 0 ? sc.lf[k] : sc.fam;
    ub.cur = (uint8_t)(sc.snd[k] >= 0 ? sc.snd[k] : 0); ub.top = 0; ub.pend = 0;
    ub_view();
}
static void ub_open(void)
{
    ub.on = 1; ub.lane = sc.sel;
    ub.of = sc.lf[ub.lane]; ub.oi = sc.snd[ub.lane];
    ub_at_lane();
    ui_all();
}
static void ub_close(void) { ub.on = 0; ub.pend = 0; ui_all(); }   /* (what plays stays) */
static void ub_follow(void)                     /* another lane (OCT, a black key): the browser on it, its sound kept */
{
    if (sc.sel == ub.lane) return;
    ub.lane = sc.sel; ub.of = sc.lf[ub.lane]; ub.oi = sc.snd[ub.lane];
    ub_at_lane();
}
static void ub_move(int32_t d)                  /* ALGORITHM, K1: the cursor (no wrap); level 2: the preview's wait */
{
    int32_t n = ub.lv == 1 ? (int32_t)LS_N : (int32_t)LS[ub.f].n;
    ub.cur = (uint8_t)clampi((int32_t)ub.cur + d, 0, n - 1);
    ub_view();
    if (ub.lv == 2) { ub.pend = 1; ub.t = fm1_ms; }
}
static void ub_family(int32_t d)                /* SELECT at level 2: the previous / next family, its first sound */
{
    int32_t f = clampi((int32_t)ub.f + d, 0, (int32_t)LS_N - 1);
    if (f == ub.f) return;
    ub.f = (uint8_t)f; ub.top = 0;
    ub.cur = (uint8_t)(sc.lf[ub.lane] == f && sc.snd[ub.lane] >= 0 ? sc.snd[ub.lane] : 0);
    ub_view();
    ub.pend = 1; ub.t = fm1_ms;
}
static void ub_say(void) { char t[32]; p_upper(t, sc_sound_name(ub.lane), 31); ui_say(t); }
static void ub_edit(void)                       /* EDIT: open the family / pick the sound */
{
    if (ub.lv == 1) {
        ub.f = ub.cur; ub.lv = 2; ub.top = 0;
        ub.cur = (uint8_t)(sc.lf[ub.lane] == ub.f && sc.snd[ub.lane] >= 0 ? sc.snd[ub.lane] : 0);
        ub_view();
        return;
    }
    sc_sound_set(ub.lane, ub.f, ub.cur);        /* (a stopped lane: loaded stopped) */
    ub_close();
    ub_say();
}
static void ub_home(void)                       /* HOME: back to the families; there: the sound it opened with, closed */
{
    if (ub.lv == 2) { ub.lv = 1; ub.cur = ub.f; ub.top = 0; ub.pend = 0; ub_view(); return; }
    if (ub.oi >= 0) sc_sound_set(ub.lane, ub.of, (uint32_t)ub.oi);
    ub_close();
    ui_say("SOUND KEPT");
}
static void ub_poll(void)                       /* the preview: the cursor's sound on the lane, while it plays */
{
    if (!ub.on || !ub.pend || fm1_ms - ub.t < UB_PREVIEW_MS) return;
    ub.pend = 0;
    if (ub.lv == 2 && sc.run[ub.lane]) sc_sound_set(ub.lane, ub.f, ub.cur);
}
static void ui_lane_focus(uint32_t k)           /* a black key: lane k, shown on LANDSCAPE (the 4 lanes, as HOME): it
                                                 * stays there (HOME's way back kept), any other page goes to it */
{
    sc_select(k);
    if (ub.on) { ub_follow(); ui_say_n("LANE", sc.sel + 1); return; }   /* (the browser stays, on this lane) */
    ui_goto(G_LAND, 0);
    ui_say_n("LANE", sc.sel + 1);
}
static void ui_lane_mute(uint32_t k)            /* a black key held: lane k muted / unmuted ("LANE 2 MUTED") */
{
    char t[24];
    if (k >= SC_N) return;
    if (sc.snd[k] < 0) { ui_say("NO SOUND ON THIS LANE"); return; }
    sc_mute(k, !sc.mute[k]);
    p_strcpy(t, "LANE ", 23); { char n[2] = {(char)('1' + k), 0}; p_cat(t, n, 23); }
    p_cat(t, sc.mute[k] ? " MUTED" : " ON", 23);
    ui_say(t);
}
static void ui_lane_step(int32_t d)             /* OCT: the previous / next lane; a lane's page stays with it, any other
                                                 * goes to LANDSCAPE */
{
    sc_select((uint32_t)clampi((int32_t)sc.sel + d, 0, (int32_t)SC_N - 1));
    if (ub.on) ub_follow();
    else if (ui_lane_page()) ui_goto(ui_g, ui_sub); else ui_goto(G_LAND, 0);
    ui_say_n("LANE", sc.sel + 1);
}
/* a parameter turned by d detents: an amount or a choice by d, a toggle on (up) / off (down) */
static void ui_turn(uint32_t id, int32_t d)
{
    uint32_t kd = p_kind(id);
    int32_t v = p_get(id);
    if (kd == PK_TOGGLE) v = d > 0;
    else v += d;
    p_set(id, clampi(v, 0, p_hi(id)));
}
/* LFO held: mod learn. A card turned routes the selected LFO onto it, the turn its depth (ENV held: the selected ENV
 * or FOLLOW). The web panel (a mouse cannot hold LFO and turn a knob): a click on LFO (ENV) away from its pages
 * latches learn for LATCH_MS or until a card turns; the button again while latched: its pages */
#define LATCH_MS 3000u
static uint8_t ui_lfo_down, ui_lfo_learned;     /* (LFO or ENV held: ui_lsrc says which) */
static uint8_t ui_lsrc;                         /* the learn's source: MS_LFO1 + n, MS_ENV1 + n, MS_FOL1 + n */
static uint8_t ui_lsrc_of(uint32_t env)         /* the selected LFO's, or ENV's / FOLLOW's */
{
    if (!env) return (uint8_t)(MS_LFO1 + ui_lfo);
    return (uint8_t)(ui_env < TX_ENV_N ? MS_ENV1 + ui_env : MS_FOL1 + ui_env - TX_ENV_N);
}
static void ui_src_name(uint32_t src, char *t)  /* "LFO3", "ENV1", "FOL2" */
{
    char n[2] = {0, 0};
    if (src >= MS_FOL1) { p_strcpy(t, "FOL", 3); n[0] = (char)('1' + src - MS_FOL1); }
    else if (src >= MS_ENV1) { p_strcpy(t, "ENV", 3); n[0] = (char)('1' + src - MS_ENV1); }
    else { p_strcpy(t, "LFO", 3); n[0] = (char)('1' + src - MS_LFO1); }
    p_cat(t, n, 7);
}
#ifdef DINGHY_WEB
static uint8_t ui_click_learn = 1;
#else
static uint8_t ui_click_learn;
#endif
static uint32_t ui_latch_t;                     /* the latch's start (0: off) */
static void ui_click_learn_set(int on) { ui_click_learn = on != 0; ui_latch_t = 0; }
static int ui_latched(void) { if (ui_latch_t && fm1_ms - ui_latch_t >= LATCH_MS) ui_latch_t = 0; return ui_latch_t != 0; }
static void ui_learn(uint32_t k, int32_t d)
{
    uint32_t id = NOID;
    int s;
    if (ui_g == G_MIX) id = PID(k, ui_sub ? PL_PAN : PL_LEVEL);
    else if (ui_g == G_LAND) id = PID(k, PL_LEVEL);   /* (LANDSCAPE: lane k's volume) */
    else id = ui_row_id(k);
    ui_lfo_learned = 1;
    s = id == NOID ? -1 : mod_learn(ui_lsrc, id, d);
    if (s == -1) { ui_say("NOT A MOD TARGET"); return; }
    if (s == -2) { ui_say("MATRIX FULL"); return; }
    {                                           /* "LFO3 > L2 SIZE +24%" */
        char t[40], v[24];
        ui_src_name(ui_lsrc, t); p_cat(t, " > ", 39);
        mod_tgt_text(mod_tgt_of(id), v); p_cat(t, v, 39); p_cat(t, " ", 39);
        if (!mslot[s].src) p_cat(t, "OFF", 39);
        else { (void)p_text(PID(P_MAT, (uint32_t)s * 4u + MQ_DEPTH), v); p_cat(t, v, 39); p_cat(t, "%", 39); }
        ui_say(t);
    }
    ui_hot(k);
}
/* knob acceleration (as Frodhes'): a fast turn moves an amount more per detent, so 0 .. 127 takes about one turn; a
 * slow turn keeps single steps. The time per detent since that knob's last one: under 40 ms x2, 20 ms x4, 10 ms x8; a
 * turn's first detents (after 150 ms at rest) are single steps, however many arrive at once. Only amounts of 32 steps
 * or more; choices, toggles and short ranges keep one step a detent */
static uint32_t ui_acc_t[4];
#if defined(DINGHY_HOST) && !defined(DINGHY_WEB)
static int ui_accel_on_ = -1;                   /* the host's scripts turn exact detents: off unless TX_KNOB_ACCEL
                                                 * is set */
#define ui_accel_on (ui_accel_on_ >= 0 ? ui_accel_on_ : (ui_accel_on_ = getenv("TX_KNOB_ACCEL") != NULL))
#else
#define ui_accel_on 1
#endif
static int32_t ui_accel(uint32_t k, int32_t hi, int32_t d)
{
    uint32_t a = (uint32_t)(d < 0 ? -d : d), gap = fm1_ms - ui_acc_t[k], dt = gap / a;
    ui_acc_t[k] = fm1_ms;
    if (!ui_accel_on || gap > 150u || hi < 32) return d;
    return d * (dt < 10u ? 8 : dt < 20u ? 4 : dt < 40u ? 2 : 1);
}
static void ui_knob(uint32_t k, int32_t d)
{
    if (ui_lfo_down) { ui_learn(k, d); return; }
    if (ui_latched()) { ui_learn(k, d); ui_latch_t = fm1_ms - (LATCH_MS - 800u); if (!ui_latch_t) ui_latch_t = 1u; return; }
                                                /* (latched: this turn's detents, until 800 ms after the last) */
    ui_hot(k);
    if (ui_g == G_MIX) {
        d = ui_accel(k, 127, d);
        if (ui_sub == 0u) sc_volume(k, (uint32_t)clampi((int32_t)sc_vol_of(k) + d, 0, 127));
        else sc_pan(k, (uint32_t)clampi((int32_t)sc.pan[k] + d, 0, 127));
        return;
    }
    if (UI_OVERVIEW() && sc.sel != k) sc_select(k);   /* (a lane's card turned: that lane is the selected one) */
    if (ui_g != G_LAND) {
        uint32_t id = ui_row_id(k);
        if (id != NOID) ui_turn(id, ui_accel(k, p_kind(id) == PK_AMOUNT ? p_hi(id) : 0, d));
        return;
    }
    if (sc.snd[k] < 0) return;                  /* LANDSCAPE: lane k's volume (an empty lane: none) */
    sc_volume(k, (uint32_t)clampi((int32_t)sc_vol_of(k) + ui_accel(k, 127, d), 0, 127));
    {
        char t[24], n[8];
        p_strcpy(t, "LANE ", 23); p_fmt_int(n, (int32_t)k + 1); p_cat(t, n, 23); p_cat(t, " VOLUME ", 23);
        p_fmt_int(n, (int32_t)sc_vol_of(k)); p_cat(t, n, 23);
        ui_say(t);
    }
}
static void ui_algorithm(int32_t d)
{
    uint32_t id = NOID;
    if (ui_g == G_LAND) { sc_family_turn(d); return; }
    if (ui_g == G_SOUND) { ub_open(); return; }   /* (the first detent opens the sound browser; it does not move) */
    if (ui_g == G_ENV) {                        /* which ENV / FOLLOW */
        char t[12];
        ui_env = (uint8_t)clampi((int32_t)ui_env + (d > 0 ? 1 : -1), 0, UI_NENV - 1);
        ui_env_name(t); p_upper(t, t, 11);
        ui_say(t);
        if (ui_sub >= ui_nsub(G_ENV)) ui_sub = 0;
        ui_all();
        return;
    }
    if (ui_g == G_LFO || ui_g == G_MATRIX) {   /* which LFO, which slot */
        char t[12];
        if (ui_g == G_LFO) { ui_lfo = (uint8_t)clampi((int32_t)ui_lfo + (d > 0 ? 1 : -1), 0, TX_LFO_N - 1); ui_say_n("LFO", ui_lfo + 1); }
        else { ui_slot = (uint8_t)clampi((int32_t)ui_slot + (d > 0 ? 1 : -1), 0, MOD_NS - 1); ui_say_n("SLOT", ui_slot + 1); }
        (void)t;
        ui_all();
        return;
    }
    if (ui_g == G_DRIVE) id = ALG_DRIVE;
    else if (ui_g == G_FILTER) id = ui_id_lane(ALG_FILTER);
    else if (ui_g == G_COMP) id = ALG_COMP;
    else if (ui_g == G_GRAIN) id = ALG_FREEZE;
    else if (ui_g == G_REVERB) id = ALG_REVERB;
    if (id == NOID) return;
    ui_turn(id, d);
    ui_hot_card = 0xFF;
    ui_hot_id(id);
    {                                           /* "FREEZE ON", "REVERB HALL" */
        char t[32], v[24];
        p_strcpy(t, ui_g == G_GRAIN ? "FREEZE" : ui_g == G_COMP ? "COMP" : G_TITLE[ui_g], 12); p_cat(t, " ", 31);
        if (ui_g == G_FILTER) {                 /* ("L2 FILTER LP12": the selected lane's) */
            char n[4] = {'L', (char)('1' + sc.sel), ' ', 0};
            p_strcpy(t, n, 3); p_cat(t, G_TITLE[ui_g], 31); p_cat(t, " ", 31);
        }
        (void)p_text(id, v); p_cat(t, v, 31);
        ui_say(t);
    }
}
/* PLAY: a tap on the press (its time exact); held 600 ms, the transport instead, and the press's tap taken back */
#define TAP_HOLD_MS 600u
static uint32_t ui_play_t;
static uint8_t ui_play_down, ui_play_held;
static void ui_play(uint32_t press, uint32_t release)
{
    if (press) {
        ui_play_t = fm1_ms; ui_play_down = 1; ui_play_held = 0;
        if (tm.clock == CLK_FOLLOW || clk_follows()) ui_say("EXT CLOCK");
        else if (tempo_tap()) {
            char t[16], n[8], d[2] = {(char)('0' + tempo_bpm10() % 10u), 0};
            p_strcpy(t, "TEMPO ", 6); p_fmt_int(n, (int32_t)(tempo_bpm10() / 10u)); p_cat(t, n, 15);
            p_cat(t, ".", 15); p_cat(t, d, 15);
            ui_say(t);
        }
    }
    if (release) ui_play_down = 0;
    if (ui_play_down && !ui_play_held && fm1_ms - ui_play_t >= TAP_HOLD_MS) {
        ui_play_held = 1;
        tempo_tap_undo();
        if (tempo_transport(!tm_running())) ui_say(tm_running() ? "TRANSPORT: RUNNING" : "TRANSPORT: STOPPED");
        else ui_say("EXT CLOCK");
    }
}
static void ui_input(uint32_t b, uint32_t rel)
{
    int32_t d[7];
    uint32_t i;
    for (i = 0; i < 7u; i++) d[i] = fm1_enc_take(i);
    if (ui_splash_on) for (i = 0; i < 7u; i++) if (d[i] || b) ui_splash_t = fm1_ms - UI_SPLASH_MS;
    if (scn_input(b, rel, d[ENC_PRESETS], d[ENC_KNOB1], d[ENC_KNOB2], d[ENC_KNOB3])) {   /* (the save mode takes K1 .. K3
                                                         * and HOME) */
        d[ENC_KNOB1] = d[ENC_KNOB2] = d[ENC_KNOB3] = 0;
        b &= ~(1u << BTN_HOME | 1u << BTN_OCT_DOWN);
    }
    ub_poll();
    if (ub.on) {                                /* the browser takes ALGORITHM, K1, SELECT, EDIT, HOME (SEQ: closes); any
                                                 * other button or PRESETS closes it, what plays kept */
        uint32_t own = 1u << BTN_EDIT | 1u << BTN_HOME | 1u << BTN_SEQ | 1u << BTN_OCT_DOWN | 1u << BTN_OCT_UP;
        if (b >> BTN_SEQ & 1u) { ub_close(); b &= ~(1u << BTN_SEQ); }
        else if (b & ~own || d[ENC_PRESETS]) ub_close();
        else {
            if (b >> BTN_EDIT & 1u) ub_edit();
            else if (b >> BTN_HOME & 1u) ub_home();
            if (d[ENC_ALGORITHM]) ub_move(d[ENC_ALGORITHM]);
            if (d[ENC_KNOB1]) ub_move(d[ENC_KNOB1]);
            if (d[ENC_SELECT]) { if (ub.lv == 2) ub_family(d[ENC_SELECT]); else ub_move(d[ENC_SELECT]); }
            b &= ~(1u << BTN_EDIT | 1u << BTN_HOME);
            d[ENC_ALGORITHM] = d[ENC_SELECT] = 0;
            for (i = 0; i < 4u; i++) d[ENC_KNOB1 + i] = 0;
        }
    } else if (b >> BTN_SEQ & 1u) ub_open();   /* SEQ (SOUNDS): the browser, for the selected lane */
    if (b >> BTN_HOME & 1u) {
        if (ui_g != G_LAND) {
            uint8_t g = ui_g, s = ui_sub;
            ui_goto(G_LAND, 0);
            ui_back_g = g;
            ui_back_sub = s;
        } else if (ui_back_g != 0xFF) ui_goto(ui_back_g, ui_back_sub);
    }
    if (b >> BTN_EDIT & 1u) ui_goto(ui_g == G_SOUND ? G_FILTER : G_SOUND, 0);   /* (the lanes' pages: SOUND, FILTER) */
    if (b >> BTN_FX & 1u) ui_goto(ui_g == G_DRIVE ? G_COMP : ui_g == G_COMP ? G_GRAIN : ui_g == G_GRAIN ? G_REVERB : G_DRIVE, 0);
                                                /* (the mix's: DRIVE, COMPRESSOR, GRAIN, REVERB) */
    if (b >> BTN_GLO & 1u) ui_goto(ui_g == G_TEMPO ? G_MIDI : ui_g == G_MIDI ? G_DISPLAY : G_TEMPO, 0);
    if (b & ~(1u << BTN_LFO | 1u << BTN_ENV)) ui_latch_t = 0;   /* (another button: learn unlatched) */
    if (b >> BTN_ARP & 1u) ui_goto(G_MATRIX, 0);
    if ((b >> BTN_ENV & 1u && ui_lsrc < MS_ENV1) || (b >> BTN_LFO & 1u && ui_lsrc >= MS_ENV1)) ui_latch_t = 0;   /* (the
                                                 * other one's latch) */
    for (i = 0; i < 2u; i++) {                  /* LFO, ENV: their pages on the release, unless they learned */
        uint32_t bt = i ? BTN_ENV : BTN_LFO, g = i ? G_ENV : G_LFO;
        if (b >> bt & 1u) { ui_lfo_down = 1; ui_lfo_learned = 0; ui_lsrc = ui_lsrc_of(i); }
        if (rel >> bt & 1u) {
            if (ui_lfo_down && !ui_lfo_learned) {
                if (ui_click_learn && ui_g != g && !ui_latched()) {   /* (the web: latch learn) */
                    char t[32], n[8];
                    p_strcpy(t, "LEARN ", 31); ui_src_name(ui_lsrc, n); p_cat(t, n, 31); p_cat(t, ": TURN A CARD", 31);
                    ui_latch_t = fm1_ms ? fm1_ms : 1u;
                    ui_say(t);
                } else { ui_latch_t = 0; ui_goto(g, ui_g == g ? (ui_sub + 1u) % ui_nsub(g) : 0u); }
            }
            ui_lfo_down = 0;
        }
    }
    ui_play(b >> TX_BTN_TAP & 1u, rel >> TX_BTN_TAP & 1u);
    if (b >> BTN_SEL & 1u) { nt_mode_set(nt.mode == NT_PLAY ? NT_LANDSCAPE : NT_PLAY); ui_say(nt.mode == NT_PLAY ? "KEYS: PLAY" : "KEYS: LANDSCAPE"); }
    if (b >> BTN_OCT_DOWN & 1u) { if (nt.mode == NT_PLAY) nt_oct_step(-1); else ui_lane_step(-1); }
    if (b >> BTN_OCT_UP & 1u) { if (nt.mode == NT_PLAY) nt_oct_step(1); else ui_lane_step(1); }
    if (b >> BTN_REC & 1u && nt.mode == NT_LANDSCAPE) { sc_all_toggle(); ui_say(sc_any_run() ? "ALL PLAYING" : "ALL STOPPED"); }
    if (d[ENC_SELECT]) ui_page_step(d[ENC_SELECT]);
    if (d[ENC_ALGORITHM]) ui_algorithm(d[ENC_ALGORITHM]);
    for (i = 0; i < 4u; i++) if (d[ENC_KNOB1 + i]) ui_knob(i, d[ENC_KNOB1 + i]);
}
/* the buttons' LEDs: HOME, EDIT, FX or GLO on their pages, PLAY the beat, SEL in PLAY mode, REC while anything plays, OCT where it can
 * still go */
static uint32_t ui_btn_leds(void)
{
    uint32_t b = 0;
    if (ui_g <= G_MIX) b |= 1u << BTN_HOME;
    else if (ui_g <= G_FILTER) b |= 1u << BTN_EDIT;
    else if (ui_g <= G_REVERB) b |= 1u << BTN_FX;
    else if (ui_g == G_LFO) b |= 1u << BTN_LFO;
    else if (ui_g == G_ENV) b |= 1u << BTN_ENV;
    if (ui_latched()) b |= 1u << (ui_lsrc >= MS_ENV1 ? BTN_ENV : BTN_LFO);   /* (learn latched) */
    if (ub.on) b |= 1u << BTN_SEQ;              /* (the sound browser open) */
    if (scn_save_mode || (scn_sv_on && fm1_ms >> 7 & 1u)) b |= 1u << BTN_SAVE;   /* (the save mode; saving: blinking) */
    else if (ui_g == G_MATRIX) b |= 1u << BTN_ARP;
    else b |= 1u << BTN_GLO;
    if (tempo_beat_led()) b |= 1u << TX_BTN_TAP;
    if (nt.mode == NT_PLAY) b |= 1u << BTN_SEL;
    if (sc_any_run()) b |= 1u << BTN_REC;
    if (nt.mode == NT_PLAY ? nt.oct < 0 : sc.sel > 0) b |= 1u << BTN_OCT_DOWN;
    if (nt.mode == NT_PLAY ? nt.oct > 0 : sc.sel < SC_N - 1u) b |= 1u << BTN_OCT_UP;
    return b;
}
