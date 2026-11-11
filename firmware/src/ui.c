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
 *   SELECT     the pages, in order (the first to the last, no wrap): LANDSCAPE (titled TEXTURA), the lane pages HOW
 *              MANY, HOW ACTIVE, PAN, ON / MUTE (each the four lanes as rows, Kn lane n's), SOUND 1..n (the selected
 *              lane's sound: its cards, four a page), EFFECTS (the front page: REVERB MIX, DELAY MIX, HIGH CUT, DRIVE),
 *              REVERB 1/3 .. 3/3, GRAIN 1/4 .. 4/4 (the grain delay: in series, it takes what the reverb gives;
 *              MIX on DRIVE, GRAIN, REVERB reads DRY, D100 W50 .. D100 W100 (64) .. D50 W100, WET), FILTER (the mix's, right after the DRIVE in the chain: LOW CUT
 *              HIGH CUT RESONANCE SLOPE, a band), DRIVE (the mix's first effect: AMOUNT TONE MIX TYPE), COMPRESSOR 1/2
 *              2/2 (Frodhes': THR RATIO ATTACK MAKEUP, then COMP REL; the footer its gain-reduction meter), LFO 1/3 .. 3/3,
 *              ENV 1/2 2/2 (FOLLOW: one page), MATRIX, RHYTHM (BPM, the groove's AMOUNT and SHIFT, the transport),
 *              GROOVE LANES (each lane's ROLE on its knob), SCALE (SCALE ROOT: the notes the keys and MIDI play),
 *              TEMPO (FINE, CLOCK, STATUS), MIDI (the clock), MIDI IN (each lane's channel), DISPLAY (COLOR). Each
 *              page titled by what it does, no fractions: pips at the right for its button's pages
 *   ALGORITHM  named on the footer's ALGORITHM line with its value. LANDSCAPE: picks a landscape (the footer names it:
 *              EDIT loads it, HOME keeps what plays; a turn never loads); SOUND: opens the sound browser; EFFECTS,
 *              REVERB: the reverb's type; DRIVE: its TYPE; FILTER: its SLOPE; COMPRESSOR: COMP (up on, down off);
 *              GRAIN: FREEZE (up on, down off); LFO: which (1..8); ENV: which (ENV 1, ENV 2, FOLLOW 1, FOLLOW 2);
 *              MATRIX: which slot (1..16); RHYTHM, GROOVE LANES: the groove's TYPE; SCALE: SCALE; MIDI IN: SELECTED CH
 *              (the channel whose notes play the selected lane)
 *   K1..K4     LANDSCAPE: the volumes of lanes 1..4 (a lane's sound: its SOUND page; HOW MANY, HOW ACTIVE: the white
 *              keys too); the lane pages, GROOVE LANES: lanes 1..4; else the cards
 *   HOME       LANDSCAPE; on LANDSCAPE: back to the last page left, however it was left (P7; the footer names it)
 *   EDIT       SOUND (again: its next page)        FX  on its release: EFFECTS (again: each page of REVERB, GRAIN,
 *              FILTER, DRIVE, COMPRESSOR, then round); held 600 ms, from any page: FILTER (the mix's filter at once;
 *              HOME, then HOME again, comes back to it)
 *   GLO        RHYTHM (again: GROOVE LANES, SCALE, TEMPO, MIDI, MIDI IN, DISPLAY, then round)        LFO  the LFO pages; held + a card
 *              turned: mod learn (the selected LFO onto that card, the turn its depth; back to 0 frees the slot)    ARP
 *              MATRIX. Each group button lit on its own pages only; every group page's footer says where it goes next
 *   ENV        the ENV / FOLLOW pages; held + a card turned: mod learn from the selected ENV or FOLLOW
 *   SEL        key mode: tapped, LANDSCAPE / PLAY; held, the other mode while it is held (ui_sel), so in PLAY SEL
 *              held + a black key picks that lane ("LANE 2: RAIN"), OCT the previous / next one, and the keys play it
 *              once SEL is let go      OCT- OCT+  LANDSCAPE mode: the previous / next lane (a lane's page, SOUND, any
 *              of its pages, stays, for the new lane; any other goes to LANDSCAPE); PLAY mode: the octave
 *   black keys LANDSCAPE mode, by their groups: lanes 1..4, shown on LANDSCAPE (it stays, any other page goes to it;
 *              EDIT then opens the lane's SOUND); held 600 ms (notes.c NT_HOLD_MS): that lane muted / unmuted too (it
 *              plays on, only its output gated: back at once, a short ramp; its card dimmed, MUTED; a scene keeps it)
 *   REC        LANDSCAPE mode: all lanes and the transport stop (remembered) / start again (bar 1 there); pressed
 *              again within 1.5 s: taps, the tempo from the third (ui_rec)
 *   SEQ        SOUNDS: the sound browser for the selected lane, over the rows (any page stays under it): level 1 the
 *              families, level 2 a family's sounds (the lane's own marked, the other lanes' numbers at the right; any
 *              sound on any lane, the same one on two lanes too). ALGORITHM / K1 the cursor (level 2: it plays on the
 *              lane UB_PREVIEW_MS after the last detent, while the lane plays), SELECT (level 2) the family, EDIT opens
 *              / picks, HOME back / cancel (the sound it opened with back), OCT / a black key: another lane; SEQ again
 *              or any other button: closed, what plays kept
 *   SAVE       the save mode (scenes.c: PRESETS / K1 the user scene, SAVE again saves, HOME cancels); held 700 ms:
 *              undo the last scene load
 *   PRESETS    the presets list, over the rows like SOUNDS: one list, each family's header then its presets, USER
 *              (the saved scenes) last; it opens on the preset loaded, not moved. PRESETS / ALGORITHM / K1 move the
 *              cursor, nothing loads; SELECT the next / previous family; EDIT loads the one under the cursor, the list
 *              stays (on the one loaded with FX: YOURS: with its effects); the loaded one solid, the cursor an outline,
 *              the title PRESETS: BROWSING off it; FX: LOAD OPTIONS (K1 EFFECTS ON LOAD, K2 PRESETS CHANGE TEMPO; FX
 *              again: the list); HOME or any other button closes it, what plays kept; SEQ: the sound browser
 *              instead; SAVE lit while it is open
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
enum { G_LAND, G_LANES, G_SOUND, G_FX, G_REVERB, G_GRAIN, G_MFILT, G_DRIVE, G_COMP, G_LFO, G_ENV, G_MATRIX, G_RHYTHM,
       G_GLANES, G_SCALE, G_TEMPO, G_MIDI, G_MIDIIN, G_DISPLAY, G_N };
/* the groups' names for the host and the web panel (the screen titles each page by what it does: PG_T) */
static const char *const G_TITLE[G_N] = {"LANDSCAPE", "LANES", "SOUND", "EFFECTS", "REVERB", "GRAIN", "FILTER", "DRIVE",
                                         "COMPRESSOR", "LFO", "ENV", "MATRIX", "RHYTHM", "GROOVE LANES", "SCALE", "TEMPO",
                                         "MIDI", "MIDI IN", "DISPLAY"};
static const uint8_t G_NSUB[G_N] = {1, 4, 0, 1, 3, 4, 1, 1, 2, 3, 2, 1, 1, 1, 1, 1, 1, 1, 1};   /* (SOUND: as many as its cards
                                                 * need; ENV: a FOLLOW has one) */
/* the pages' titles on the screen, by what they do, the groups' pages in order (SOUND: "SOUND"; a FOLLOW: "FOLLOW") */
static const char *const PG_T[] = {"TEXTURA", "HOW MANY", "HOW ACTIVE", "PAN", "ON / MUTE", "EFFECTS", "REVERB",
                                   "REVERB SPACE", "SHIMMER", "DELAY", "DELAY MOTION", "GRAINS", "GRAIN TONE", "FILTER",
                                   "DRIVE", "COMPRESSOR", "COMP RELEASE", "LFO", "LFO PHASE", "LFO LEVEL", "ENV",
                                   "ENV SHAPE", "MOD MATRIX", "RHYTHM", "GROOVE LANES", "SCALE", "TEMPO", "MIDI CLOCK",
                                   "MIDI IN", "DISPLAY"};
/* the group buttons: each owns a run of groups (its LED lit there, it steps their pages, the title's pips count them):
 * HOME the home page and the lane pages (SELECT's), EDIT SOUND, FX the effects, LFO, ENV, ARP the matrix, GLO the
 * rhythm and the setup */
static const uint8_t G_BTN[G_N] = {BTN_HOME, BTN_HOME, BTN_EDIT, BTN_FX, BTN_FX, BTN_FX, BTN_FX, BTN_FX, BTN_FX, BTN_LFO,
                                   BTN_ENV, BTN_ARP, BTN_GLO, BTN_GLO, BTN_GLO, BTN_GLO, BTN_GLO, BTN_GLO, BTN_GLO};
#define LA P_LANE_ANY
#define NOID 0xFFFFu
/* the effects' pages: a parameter per knob (a global, or the selected LFO's, ENV's, slot's), from G_FX0 */
static const uint8_t G_FX0[G_N] = {0, 0, 0, 13, 5, 1, 21, 0, 19, 8, 14, 11, 22, 0, 23, 12, 17, 24, 18};   /* (ENV: 14, 15; FOLLOW:
                                                 * 16) */
#define LFO_ANY(p) PID(P_LFO, 0xF0u | (p))        /* the selected LFO's (ui_lfo) */
#define ENV_ANY(p) PID(P_ENV, 0xF0u | (p))        /* the selected ENV's or FOLLOW's (ui_env) */
#define MAT_ANY(q) PID(P_MAT, 0xF0u | (q))        /* the selected slot's (ui_slot) */
#define CMP(p) PID(P_GLOBAL, PG_COMP0 + (p))
#define DRV(p) PID(P_GLOBAL, PG_DRIVE0 + (p))
#define MFL(p) PID(P_GLOBAL, PG_MF0 + (p))
#define GDL(p) PID(P_GLOBAL, PG_GD(p))
#define GRV(i) PID(P_GLOBAL, PG_GRV0 + (i))
static const uint16_t FX_IDS[25][4] = {
    {DRV(TX_DR_AMOUNT), DRV(TX_DR_TONE), DRV(TX_DR_MIX), DRV(TX_DR_TYPE)},   /* DRIVE: the mix's */
    {PID(P_GLOBAL, PG_GRAIN0 + TX_GD_MIX), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_TIME), PID(P_GLOBAL, PG_GSYNC), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_FEEDBACK)},
    {GDL(TX_GD_MOD), GDL(TX_GD_MODRATE), GDL(TX_GD_IVL), GDL(TX_GD_GRAIN)},   /* GRAIN 2/4: the animation, the grains' share */
    {PID(P_GLOBAL, PG_GRAIN0 + TX_GD_GRAINS), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_SIZE), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_SPRAY), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_PITCH)},
    {PID(P_GLOBAL, PG_GRAIN0 + TX_GD_REVERSE), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_SPREAD), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_TONE), NOID},
    {PID(P_GLOBAL, PG_REVERB0 + TX_RV_TYPE), PID(P_GLOBAL, PG_REVERB0 + TX_RV_MIX), PID(P_GLOBAL, PG_REVERB0 + TX_RV_DECAY), PID(P_GLOBAL, PG_REVERB0 + TX_RV_TONE)},
    {PID(P_GLOBAL, PG_REVERB0 + TX_RV_PREDELAY), PID(P_GLOBAL, PG_REVERB0 + TX_RV_SIZE), PID(P_GLOBAL, PG_REVERB0 + TX_RV_MOD), PID(P_GLOBAL, PG_REVERB0 + TX_RV_LOWCUT)},
    {PID(P_GLOBAL, PG_REVERB0 + TX_RV_SHIMMER), PID(P_GLOBAL, PG_REVERB0 + TX_RV_SHIFT), NOID, NOID},
    {LFO_ANY(TX_LP_SHAPE), LFO_ANY(TX_LP_RATE), LFO_ANY(TX_LP_SYNC), LFO_ANY(TX_LP_MODE)},
    {LFO_ANY(TX_LP_PHASE), LFO_ANY(TX_LP_WIDTH), LFO_ANY(TX_LP_JITTER), LFO_ANY(TX_LP_FADE)},
    {LFO_ANY(TX_LP_RESTART), LFO_ANY(TX_LP_LEVEL), LFO_ANY(ML_NOW), LFO_ANY(ML_ROUTES)},
    {MAT_ANY(MQ_SRC), MAT_ANY(MQ_TGT), MAT_ANY(MQ_DEPTH), MAT_ANY(MQ_SPREAD)},
    {PID(P_GLOBAL, PG_FINE), PID(P_GLOBAL, PG_CLOCK), PID(P_GLOBAL, PG_STATUS), NOID},   /* TEMPO: the fine tempo, what
                                                 * leads it */
    {PID(P_GLOBAL, PG_REVERB0 + TX_RV_MIX), PID(P_GLOBAL, PG_GRAIN0 + TX_GD_MIX), MFL(TX_MF_HIGH), DRV(TX_DR_AMOUNT)},   /*
                                                 * EFFECTS: the front page, each effect's most used */
    {ENV_ANY(TX_EP_TRIGGER), ENV_ANY(TX_EP_ATTACK), ENV_ANY(TX_EP_DECAY), ENV_ANY(TX_EP_RELEASE)},
    {ENV_ANY(TX_EP_SUSTAIN), ENV_ANY(TX_EP_CURVE), ENV_ANY(TX_EP_VARY), ENV_ANY(TX_EP_LOOP)},
    {ENV_ANY(TX_FP_INPUT), ENV_ANY(TX_FP_ATTACK), ENV_ANY(TX_FP_RELEASE), ENV_ANY(TX_FP_GAIN)},
    {PID(P_GLOBAL, PG_CLOCK), PID(P_GLOBAL, PG_INUSB), PID(P_GLOBAL, PG_INTRS), PID(P_GLOBAL, PG_OUT)},
    {PID(P_GLOBAL, PG_COLOR), NOID, NOID, NOID},
    {CMP(TX_CP_THR), CMP(TX_CP_RATIO), CMP(TX_CP_ATK), CMP(TX_CP_MKUP)},   /* COMPRESSOR: Frodhes' four knobs */
    {CMP(TX_CP_ON), CMP(TX_CP_REL), NOID, NOID},
    {MFL(TX_MF_LOW), MFL(TX_MF_HIGH), MFL(TX_MF_RESO), MFL(TX_MF_SLOPE)},   /* FILTER: the mix's, after the DRIVE */
    {PID(P_GLOBAL, PG_BPM), GRV(0), GRV(2), PID(P_GLOBAL, PG_RUN)},   /* RHYTHM: the tempo, the groove's AMOUNT and
                                                 * SHIFT, the transport */
    {PID(P_GLOBAL, PG_SCALE), PID(P_GLOBAL, PG_ROOT), NOID, NOID},   /* SCALE: the keys' and MIDI's notes (notes.c) */
    {PID(P_GLOBAL, PG_LCH0), PID(P_GLOBAL, PG_LCH0 + 1), PID(P_GLOBAL, PG_LCH0 + 2), PID(P_GLOBAL, PG_LCH0 + 3)},   /* MIDI
                                                 * IN: each lane's channel */
};
/* what ALGORITHM turns on an effect's page */
#define ALG_DRIVE DRV(TX_DR_TYPE)
#define ALG_MFILT MFL(TX_MF_SLOPE)
#define ALG_FREEZE PID(P_GLOBAL, PG_GRAIN0 + TX_GD_FREEZE)
#define ALG_REVERB PID(P_GLOBAL, PG_REVERB0 + TX_RV_TYPE)
#define ALG_COMP CMP(TX_CP_ON)
#define ALG_GROOVE GRV(1)
static const uint16_t G_ALG_[G_N] = {NOID, NOID, NOID, ALG_REVERB, ALG_REVERB, ALG_FREEZE, ALG_MFILT, ALG_DRIVE, ALG_COMP,
                                     NOID, NOID, NOID, ALG_GROOVE, ALG_GROOVE, PID(P_GLOBAL, PG_SCALE), NOID, NOID,
                                     PID(P_GLOBAL, PG_SELCH), NOID};   /* what ALGORITHM turns on a
                                                 * page, named on the ALGORITHM line */
static const char *const G_ALGN_[G_N] = {0, 0, 0, "REVERB TYPE", "REVERB TYPE", "FREEZE", "SLOPE", "DRIVE TYPE", "COMP", 0, 0,
                                         0, "GROOVE TYPE", "GROOVE TYPE", "SCALE", 0, 0, "SELECTED CH", 0};
#define G_ALG(g) G_ALG_[g]
#define G_ALGN(g) G_ALGN_[g]
enum { E_HEADER = 0, E_TITLE = 1, E_ROW0 = 2, E_FOOTER = 6, E_N = 7 };

static uint8_t ui_g, ui_sub;                    /* the page: its group, its sub-page */
/* the cards' labels where a page mixes things (EFFECTS, RHYTHM: the effect's or the groove's name with it) */
static const char *ui_lab(uint32_t id)
{
    if (ui_g == G_FX) { static const char *const L[4] = {"REVERB MIX", "DELAY MIX", "HIGH CUT", "DRIVE"}; uint32_t k; for (k = 0; k < 4u; k++) if (FX_IDS[13][k] == id) return L[k]; }
    if (id == GRV(0)) return "GROOVE AMOUNT";
    if (id == GRV(2)) return "GROOVE SHIFT";
    return p_lab(id);
}
static uint8_t ui_lfo, ui_slot;                 /* the LFO and the matrix slot ALGORITHM picked */
static uint8_t ui_env;                          /* ENV 1, ENV 2, FOLLOW 1, FOLLOW 2 (0..3), ALGORITHM's on ENV */
#define UI_NENV (TX_ENV_N + TX_FOL_N)
static uint32_t ui_dirty, ui_last_draw, ui_sig[E_N], ui_sig_t;
static uint8_t ui_back_g = 0xFF, ui_back_sub;  /* HOME's way back from the home page: the last other page (0xFF: none) */
static char ui_msg[32];
static uint32_t ui_msg_t;
static uint8_t ui_hot_card = 0xFF;
static uint32_t ui_hot_t;
static uint8_t ui_blink_shown[SC_N];
/* the load toast: every load (PRESETS' EDIT, a program change, the boot, an undo, a landscape) names what comes, big,
 * over the rows for TOAST_MS ("LOADED . COAST", "F034 HARBOUR NIGHT"); the header keeps the name after */
#define TOAST_MS 1500u
static char ui_toast_hd[12], ui_toast_fam[16], ui_toast_nm[24];
static uint32_t ui_toast_t;
/* the sound browser (SEQ, or ALGORITHM on SOUND): a list over the four rows, level 1 the families, level 2 a family's
 * sounds; the cursor's sound previewed on the lane UB_PREVIEW_MS after the last detent while the lane plays. The
 * presets browser (PRESETS, pr) is the same list: level 1 the categories (and USER: the scenes saved), level 2 a
 * category's presets; the cursor's preset loads UB_PREVIEW_MS after the last detent (the lanes switch smoothly) */
#define UB_PREVIEW_MS 400u
#define UB_ROWS 8u                              /* list rows shown: two in each row slot */
static struct {
    uint8_t on, lv, f, cur, top, lane;          /* lv 1 families, 2 sounds; f the family shown at level 2 */
    uint8_t pr;                                 /* the presets list: cur a row of it, f the cursor's family (UB_USER:
                                                 * the user scenes) */
    uint8_t opt;                                /* the presets list's LOAD OPTIONS shown (FX) */
    uint8_t of;                                 /* the lane's sound when it opened (to put back: HOME) */
    int8_t oi;
    uint8_t pend;                               /* a preview waiting since t */
    uint32_t t;
} ub;
#define UB_USER PR_NCAT                         /* (the presets list's last family: the scenes saved) */
static uint32_t ub_ncat(void) { return PR_NCAT + (scn_user_n() ? 1u : 0u); }
static uint32_t ub_cat_n(uint32_t c) { return c < PR_NCAT ? pr_cat_n(c) : scn_user_n(); }   /* a family's presets */
static uint32_t ub_n(void)                      /* the list's length (the presets list: its families' headers too) */
{
    if (!ub.pr) return ub.lv == 1 ? LS_N : LS[ub.f].n;
    return ub_ncat() + PR_N + scn_user_n();
}
/* the presets list: a family's header row, its presets, the next family's header ...; USER (the scenes saved) last.
 * Row i: 1 a preset (*v its scene number), 0 a family's header (*v the family) */
static int ub_row(uint32_t i, uint32_t *v)
{
    uint32_t c, n;
    for (c = 0; c < ub_ncat(); c++) {
        if (!i) { *v = c; return 0; }
        n = ub_cat_n(c);
        if (--i < n) { *v = c < PR_NCAT ? pr_cat_first(c) + i : SCN_NF + scn_user_at(i); return 1; }
        i -= n;
    }
    *v = 0;
    return 0;
}
static uint32_t ub_head_of(uint32_t c)          /* family c's header row */
{
    uint32_t i, r = 0;
    for (i = 0; i < c && i < ub_ncat(); i++) r += ub_cat_n(i) + 1u;
    return r;
}
static uint32_t ub_pos(void)                    /* the cursor's place in its family (0: the first) */
{
    uint32_t h = ub_head_of(ub.f);
    return ub.cur > h ? ub.cur - h - 1u : 0u;
}
static uint32_t ub_loaded(void) { return scn_busy() ? scn_ap_idx : scn_cur; }   /* (a load under way: the one coming) */


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
#define TO_X 6
#define TO_Y (RW_Y0 + RW_DY)                    /* (over rows 2 and 3) */
#define TO_W RW_W
#define TO_H (RW_DY + RW_H)
#define HOT_MS 1000u
#define UI_SIG_MS 20u
#define UI_SPLASH_MS 1600u

static void cv_frame(int32_t x, int32_t y, int32_t w, int32_t h, int32_t s)   /* a frame s px thick, in the frame's colour */
{
    cv_rect(x, y, w, s, mt.frame); cv_rect(x, y + h - s, w, s, mt.frame);
    cv_rect(x, y, s, h, mt.frame); cv_rect(x + w - s, y, s, h, mt.frame);
}
static void ui_mark(uint32_t e) { ui_dirty |= 1u << e; }
static void ui_all(void) { ui_dirty |= (1u << E_N) - 1u; }
static void ui_say(const char *s)
{
    p_strcpy(ui_msg, s, sizeof ui_msg - 1u);
    ui_msg_t = fm1_ms ? fm1_ms : 1u;
    ui_mark(E_FOOTER);
}
static int ui_toast_on(void) { return ui_toast_t && fm1_ms - ui_toast_t < TOAST_MS; }
static void ui_toast_text(const char *hd, const char *fam, const char *nm)
{
    p_strcpy(ui_toast_hd, hd, 11); p_strcpy(ui_toast_fam, fam, 15); p_strcpy(ui_toast_nm, nm, 23);
    ui_toast_t = fm1_ms ? fm1_ms : 1u;
    ui_mark(E_ROW0 + 1u); ui_mark(E_ROW0 + 2u);
}
static void ui_toast(const char *hd, uint32_t idx)   /* scene idx (0xFF: the landscape that plays) */
{
    char f[16], s[24];
    if (idx < SCN_N) { p_strcpy(f, idx < SCN_NF ? pr_cat_name(pr_cat_of(idx)) : "USER", 15); scn_name_of(idx, s); }
    else { p_strcpy(f, "LANDSCAPE", 15); p_upper(s, LS[sc.fam].name, 20); }
    ui_toast_text(hd, f, s);
}
static void ui_say_n(const char *s, int32_t v)  /* "HOW MANY 4" */
{
    char t[32], n[12];
    p_strcpy(t, s, 20); p_fmt_int(n, v); p_cat(t, " ", 31); p_cat(t, n, 31);
    ui_say(t);
}
static void ft_c(int32_t yc, const char *s, uint16_t c) { aa_text_c(0, S_TOP(yc), 240, &AF_S, s, c, FC_BG); }
static void ft_two(int32_t yc, const char *a, const char *b, uint16_t c)   /* "A . B", centred */
{
    int32_t x = (240 - aa_text_w(&AF_S, a) - 9 - aa_text_w(&AF_S, b)) / 2;
    x = aa_text(x, S_TOP(yc), &AF_S, a, c, FC_BG);
    cv_rect(x + 3, yc - 1, 2, 2, FC_DIM);
    aa_text(x + 9, S_TOP(yc), &AF_S, b, c, FC_BG);
}
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
static int ui_lane_page(void) { return ui_g == G_SOUND; }   /* (the selected lane's pages) */
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
    if (ui_g == G_LANES) return ui_sub < 3u && sc.snd[k] >= 0 ? PID(k, ui_sub == 2u ? PL_PAN : PL_COUNT + ui_sub) : NOID;   /* (HOW MANY, HOW
                                                 * ACTIVE, PAN: lane k's; ON / MUTE: drawn, turned here) */
    if (ui_g < G_FX || ui_g == G_GLANES) return NOID;
    {
        uint32_t id = FX_IDS[G_FX0[ui_g] + (ui_g == G_ENV && ui_env >= TX_ENV_N ? 2u : ui_sub)][k];
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
/* ---- header: the preset that plays, its family first, "*" once edited (scn_edited); on a lane's pages the lane and
 * its sound; LFO, ENV, MATRIX: which one; the key mode, the tempo and the battery at the right */
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
    if (batt_shown() != batt_seen) { batt_seen = (uint8_t)batt_shown(); ui_mark(E_HEADER); ui_mark(E_TITLE); }
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
    p_fmt_dec(b, (int32_t)tempo_bpm10());
}

/* what plays: the preset's family and its name ("COAST", "F034 HARBOUR NIGHT"; a load under way: the one coming),
 * 1; none loaded: "LANDSCAPE" and the landscape's name, 0 */
static int ui_preset_of(char *fam, char *nm)
{
    uint32_t q = scn_busy() ? scn_ap_idx : scn_cur;
    if (q >= SCN_N) { p_strcpy(fam, "LANDSCAPE", 15); p_upper(nm, LS[sc.fam].name, 20); return 0; }
    p_strcpy(fam, q < SCN_NF ? pr_cat_name(pr_cat_of(q)) : "USER", 15);
    scn_name_of(q, nm);
    return 1;
}
/* right to left on the line's canvas: the battery, the tempo, PLAY / the octave; their left edge (before the canvas:
 * the edge alone) */
static int32_t ui_status_at(int draw)
{
    char m[12];
    int32_t x = 230 - 17 - 6;
    if (draw) batt_icon(230 - 17, 6, FC_LAB, FC_VAL, FC_BG);
    ui_bpm_text(m);
    x -= aa_text_w(&AF_S, m);
    if (draw) aa_text(x, S_TOP(10), &AF_S, m, FC_LAB, FC_BG);
    x -= 6;
    if (nt.mode == NT_PLAY) {
        p_strcpy(m, "PLAY", 5);
        if (nt.oct) { char o[4] = {' ', nt.oct < 0 ? '-' : '+', (char)('0' + (nt.oct < 0 ? -nt.oct : nt.oct)), 0}; p_cat(m, o, 11); }
        x -= aa_text_w(&AF_S, m);
        if (draw) aa_text(x, S_TOP(10), &AF_S, m, UI_ACC, FC_BG);
        x -= 6;
    }
    return x;
}
static int32_t ui_status(void) { return ui_status_at(0); }
static void draw_header(void)
{
    char nm[32];
    int32_t x = ui_g == G_LAND ? 230 : ui_status();   /* (the home page: its title line has them) */
    cv_begin(240, HD_H, FC_BG);
    if (ui_g != G_LAND) (void)ui_status_at(1);
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
    } else if (ui_lane_page()) {
        char l[8] = {'L', 'A', 'N', 'E', ' ', (char)('1' + sc.sel), 0};
        int32_t xl = aa_text(10, S_TOP(10), &AF_S, l, UI_ACC, FC_BG) + 6;
        p_upper(nm, sc.snd[sc.sel] >= 0 ? sc_sound_name(sc.sel) : "-", 30);
        ui_fit(nm, x - xl - 4);
        aa_text(xl, S_TOP(10), &AF_S, nm, FC_VAL, FC_BG);
    } else {                                    /* the home page: "COAST . F034 HARBOUR NIGHT*" (none loaded:
                                                 * "LANDSCAPE . WATER"); the others: "F034 HARBOUR NIGHT*" */
        char fam[16];
        int32_t xl = 10;
        (void)ui_preset_of(fam, nm);
        if (ui_g == G_LAND) { xl = aa_text(10, S_TOP(10), &AF_S, fam, FC_LAB, FC_BG) + 4; cv_rect(xl, 9, 2, 2, FC_LAB); xl += 6; }
        ui_fit(nm, x - xl - 4 - (scn_edited ? aa_text_w(&AF_S, "*") : 0));
        if (scn_edited) p_cat(nm, "*", 31);
        aa_text(xl, S_TOP(10), &AF_S, nm, FC_VAL, FC_BG);
    }
    cv_blit(0, 0);
}
static void ui_frac(char *t, uint32_t a, uint32_t b)   /* "a/b" */
{
    char n[4];
    p_fmt_int(t, (int32_t)a); p_cat(t, "/", 11); p_fmt_int(n, (int32_t)b); p_cat(t, n, 11);
}
/* a page's title (PG_T), the page a group button goes to from here (its own pages: the next, wrapping within the
 * groups it owns; else its first) and its name, the title's pips (the button's pages, this one lit) */
static const char *ui_title_of(uint32_t g, uint32_t sub)
{
    uint32_t i, n = 0;
    if (g == G_SOUND) return "SOUND";
    if (g == G_ENV && ui_env >= TX_ENV_N) return "FOLLOW";
    for (i = 0; i < g; i++) n += G_NSUB[i];
    return PG_T[n + sub];
}
static void ui_next(uint32_t btn, uint8_t *g, uint8_t *s)
{
    uint32_t f = 0;
    while (G_BTN[f] != btn) f++;
    if (G_BTN[ui_g] != btn) { *g = (uint8_t)f; *s = 0; return; }
    *g = ui_g; *s = (uint8_t)(ui_sub + 1u);
    if (*s < ui_nsub(ui_g)) return;
    *s = 0;
    *g = (uint8_t)(ui_g + 1u < G_N && G_BTN[ui_g + 1u] == btn ? ui_g + 1u : f);
}
static void ui_next_text(char *t, uint32_t btn)   /* "FX: NEXT > FILTER" */
{
    uint8_t g, s;
    ui_next(btn, &g, &s);
    p_strcpy(t, btn == BTN_FX ? "FX" : btn == BTN_GLO ? "GLO" : btn == BTN_EDIT ? "EDIT" : btn == BTN_LFO ? "LFO" : "ENV", 5);
    p_cat(t, ": NEXT > ", 31); p_cat(t, ui_title_of(g, s), 31);
}
static uint32_t ui_pips(uint32_t *at)           /* the pages of this page's button, *at this one */
{
    uint32_t g, n = 0;
    for (g = 0; g < G_N; g++) if (G_BTN[g] == G_BTN[ui_g]) { if (g == ui_g) *at = n + ui_sub; n += ui_nsub(g); }
    return n;
}
static int ui_fx_yours(void) { return scn_fx_mine && scn_fx_mode == SCN_FX_SMART; }   /* (SMART: the next load keeps them) */
static void draw_title(void)
{
    char t[12];
    int32_t x;
    uint32_t n = ui_nsub(ui_g);
    cv_begin(240, TI_H, FC_BG);
    if (ub.on && ub.pr) {                       /* the presets list: "PRESETS: BROWSING" off the preset loaded, "FOREST
                                                 * 8/10" at the right (no room: "8/10"); LOAD OPTIONS */
        char r[24], n[8];
        uint32_t v;
        int on = ub_row(ub.cur, &v) && v == ub_loaded();
        x = aa_text(10, S_TOP(10), &AF_S, ub.opt ? "LOAD OPTIONS" : "PRESETS", UI_ACC, FC_BG);
        if (!ub.opt && !on) x = aa_text(x, S_TOP(10), &AF_S, ": BROWSING", FC_VAL, FC_BG);
        if (!ub.opt) {
            ui_frac(n, ub_pos() + 1u, ub_cat_n(ub.f));
            p_strcpy(r, ub.f < PR_NCAT ? pr_cat_name(ub.f) : "USER", 15); p_cat(r, " ", 23); p_cat(r, n, 23);
            if (x + 8 + aa_text_w(&AF_S, r) > 230) p_strcpy(r, n, 23);
            aa_text_r(230, S_TOP(10), &AF_S, r, FC_LAB, FC_BG);
        }
        cv_blit(0, TI_Y);
        return;
    }
    if (ub.on) {                                /* the browser: "L2 FAMILY 3/10", "L2 ICE & GLASS 2/10" */
        char l[4] = {'L', (char)('1' + ub.lane), 0}, nm[32];
        x = aa_text(10, S_TOP(10), &AF_S, l, UI_ACC, FC_BG) + 6;
        if (ub.lv == 1) p_strcpy(nm, "SOUNDS: FAMILY", 31);
        else p_upper(nm, LS[ub.f].name, 31);
        ui_fit(nm, 230 - 40 - x);
        aa_text(x, S_TOP(10), &AF_S, nm, ub.lv == 1 ? UI_ACC : FC_VAL, FC_BG);
        ui_frac(t, ub.cur + 1u, ub_n());
        aa_text_r(230, S_TOP(10), &AF_S, t, FC_LAB, FC_BG);
        cv_blit(0, TI_Y);
        return;
    }
    x = aa_text(10, S_TOP(10), &AF_S, ui_title_of(ui_g, ui_sub), UI_ACC, FC_BG);   /* (the home page shows TEXTURA, as
                                                 * Frodhes' main page FRODHES; its name stays LANDSCAPE for the host /
                                                 * web API) */
    if (ui_g == G_LAND) {                       /* (no page count; the battery, the tempo, PLAY at the right, the
                                                 * header holding the preset's name; FX: YOURS between, room kept) */
        int32_t xr = ui_status_at(1);
        if (ui_fx_yours() && x + 10 + aa_text_w(&AF_S, "FX: YOURS") < xr) aa_text(x + 10, S_TOP(10), &AF_S, "FX: YOURS", FC_VAL, FC_BG);
        cv_blit(0, TI_Y);
        return;
    }
    {                                           /* no fractions: the button's pages as pips at the right, this one lit */
        uint32_t at = 0, i;
        n = ui_pips(&at);
        for (i = 0; n > 1u && i < n; i++) cv_rect(230 - 8 * (int32_t)(n - i) + 3, 8, 5, 5, i == at ? UI_ACC : CD_PIPOFF);
    }
    (void)x;
    cv_blit(0, TI_Y);
}

/* ---- LANDSCAPE (the home page, titled TEXTURA): a card per lane. Its fill: the lane's volume (dimmer while
 * stopped). Top: LANE n, then OFF (stopped), MUTED, or MANY (HOW MANY, 6 blocks) and ACTIVE (HOW ACTIVE, 9), the blink
 * dot at the right; below: the sound (large when every one fits), VOL and the volume, and Kn, the knob that turns it,
 * at the right edge. The selected lane: LANE n in the accent, bold with ">", and a frame. An empty lane says how to
 * fill it */
#define ST_DOT_X 214
#define ST_DOT_Y 6
#define ST_DOT 8
#define ST_BLK 4                                /* a meter block's pitch (3 px wide) */
#define ST_KX ((int32_t)RW_W - 6)               /* Kn's right edge */
static int ui_blink_on(uint32_t k) { uint32_t p = sc_blink_ms(k); return sc.run[k] && !sc.mute[k] && fm1_ms % p < p / 2u; }   /* (muted: still) */
static void ui_blink_dot(uint32_t k, int on, int canvas)
{
    uint16_t c = on ? mt.edge : sc.run[k] ? mt.fill : CD_SEGOFF;
    if (canvas) cv_rect(ST_DOT_X, ST_DOT_Y, ST_DOT, ST_DOT, c);
    else lcd_fill(RW_X + ST_DOT_X, RW_Y0 + RW_DY * k + ST_DOT_Y, ST_DOT, ST_DOT, c);
    ui_blink_shown[k] = (uint8_t)on;
}
static void cd_edge(int32_t ex, const int16_t *bx, uint32_t nb);
static int32_t ui_home_col(void) { return aa_text_w(&AF_S, "VOL 127") + 8 + aa_text_w(&AF_S, "K4"); }   /* the right
                                                 * column's width, its widest */
/* the sounds' names: all four in one size, large only when every one ends 6 px clear of the right column, so no name
 * changes size with its volume */
static int ui_home_big(void)
{
    char nm[32];
    uint32_t k;
    for (k = 0; k < SC_N; k++) {
        if (sc.snd[k] < 0) continue;
        p_upper(nm, sc_sound_name(k), 30);
        if (!aa_covers(&AF_L, nm) || 6 + aa_text_w(&AF_L, nm) + 6 > ST_KX - ui_home_col()) return 0;
    }
    return 1;
}
static int32_t ui_dot_ov(int32_t x, int32_t yc, uint16_t c) { cv_rect(x + 2, yc - 1, 2, 2, c); return x + 8; }   /* a
                                                 * separating dot, mid-line */
static void draw_strip(uint32_t k)
{
    char nm[32], t[8], l[12], kk[3] = {'K', (char)('1' + k), 0};
    int sel = k == sc.sel, run = sc.run[k], mut = sc.mute[k], lit = run && !mut;   /* (muted: drawn as stopped, MUTED) */
    uint32_t i, c = sc_cnt_of(k), a = ui_lane_act(k), v = sc_vol_of(k);
    const int32_t base = (int32_t)RW_H - 4;     /* the name's and the volume's baseline, under the status line */
    int32_t x, vx, fw;
    int big;
    uint16_t tag = sel ? UI_ACC : mt.tag;
    p_strcpy(l, sel ? "LANE 1 >" : "LANE 1", 11); l[5] = (char)('1' + k);
    cv_begin(RW_W, RW_H, C_BLACK);
    if (sc.snd[k] < 0) {                        /* "LANE 4", "EMPTY . SEQ: PICK A SOUND" */
        aa_text_ov(6, S_TOP(9), &AF_S, l, tag);
        x = aa_text_ov(6, base - AF_S.asc, &AF_S, "EMPTY", FC_DIM);
        aa_text_ov(ui_dot_ov(x, base - AF_S_CAP_H / 2, FC_DIM), base - AF_S.asc, &AF_S, "SEQ: PICK A SOUND", FC_DIM);
        if (sel) {
            cv_frame(0, 0, RW_W, RW_H, 2);
        }
        cv_blit(RW_X, RW_Y0 + RW_DY * k);
        return;
    }
    fw = (int32_t)RW_W * (int32_t)v / 127;      /* the level: the card's fill (dimmer while the lane is off) */
    if (fw > 0) cv_rect(0, 0, fw, RW_H, lit ? mt.fill : cd_shade(mt.fill, 45));
    p_fmt_int(t, (int32_t)v);
    vx = ST_KX - aa_text_w(&AF_S, "K4") - 8;    /* the volume's right edge */
    big = ui_home_big();
    p_upper(nm, sc_sound_name(k), 30);
    if (!big) ui_fit(nm, ST_KX - ui_home_col() - 6 - 6);
    if (fw > 2 && lit) {                        /* the bright edge, clear of the name, the right column, the status */
        int16_t bx[12] = {6, (int16_t)(6 + aa_text_w(big ? &AF_L : &AF_S, nm)), (int16_t)(base - (big ? AF_L_CAP_H : AF_S_CAP_H)),
                          (int16_t)(base - 1), (int16_t)(vx - aa_text_w(&AF_S, "VOL 127")), (int16_t)ST_KX,
                          (int16_t)(base - AF_S_CAP_H), (int16_t)(base - 1),
                          6, (int16_t)(ST_DOT_X + ST_DOT), 2, 13};
        cd_edge(fw - 2, bx, 3);
    }
    x = aa_text_ov(6, S_TOP(9), &AF_S, l, sel ? UI_ACC : (fw > 40 ? RGB(170, 170, 176) : mt.tag)) + 8;
    if (mut) aa_text_ov(x, S_TOP(9), &AF_S, run ? "MUTED" : "MUTED, OFF", UI_ACC);
    else if (!run) aa_text_ov(x, S_TOP(9), &AF_S, "OFF", fw > x + 30 ? RGB(206, 206, 210) : FC_LAB);
    else {                                      /* MANY (6 blocks) and ACTIVE (9), labelled, on the status line */
        uint16_t lc = fw > x + 40 ? RGB(206, 206, 210) : FC_LAB;
        x = aa_text_ov(x, S_TOP(9), &AF_S, "MANY", lc) + 3;
        for (i = 0; i < LS_NCNT - 1u; i++) cv_rect(x + ST_BLK * (int32_t)i, 5, 3, 6, i < c ? mt.edge : CD_SEGOFF);
        x += ST_BLK * (int32_t)(LS_NCNT - 1u) + 6;
        x = aa_text_ov(x, S_TOP(9), &AF_S, "ACTIVE", lc) + 3;
        for (i = 0; i < LS_NACT; i++) cv_rect(x + ST_BLK * (int32_t)i, 5, 3, 6, i < a ? FC_VAL : CD_SEGOFF);
    }
    ui_blink_dot(k, ui_blink_on(k), 1);
    aa_text_ovr(vx, base - AF_S.asc, &AF_S, t, lit ? FC_VAL : FC_LAB);   /* VOL 89, then K1 at the edge */
    aa_text_ovr(vx - aa_text_w(&AF_S, t) - 4, base - AF_S.asc, &AF_S, "VOL", FC_LAB);
    aa_text_ovr(ST_KX, base - AF_S.asc, &AF_S, kk, mt.tag);
    aa_text_ov(6, base - (big ? AF_L.asc : AF_S.asc), big ? &AF_L : &AF_S, nm, lit ? FC_VAL : FC_LAB);
    if (sel) {
        cv_frame(0, 0, RW_W, RW_H, 2);
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
        cv_frame(0, 0, RW_W, RW_H, 1);
    }
    cv_blit(RW_X, RW_Y0 + RW_DY * k);
}
static void ui_mix_label(uint32_t k, char *l)   /* "1 RAIN ON WATER" */
{
    l[0] = (char)('1' + k); l[1] = ' ';
    p_upper(l + 2, sc_sound_name(k), 28);
}
static uint32_t ui_look(uint32_t id);
/* ---- the lane pages (HOW MANY, HOW ACTIVE, PAN, ON / MUTE): a card per lane, Kn lane n's; an empty lane says so */
static void draw_lane_row(uint32_t k)
{
    char l[32], v[24];
    uint32_t id = ui_row_id(k);
    int32_t lv;
    if (sc.snd[k] < 0) { p_strcpy(l, "1 EMPTY", 31); l[0] = (char)('1' + k); draw_card(k, l, "-", 0, 0, 0, LK_TEXT); return; }
    ui_mix_label(k, l);
    if (id == NOID) { draw_card(k, l, sc.mute[k] ? "MUTED" : "ON", 0, !sc.mute[k], 1, LK_LED); return; }
    if (mod_live(id, &lv)) ui_card_live = lv;
    { const char *u = p_text(id, v); draw_card(k, l, v, u, p_ord(id, p_get(id)), p_hi(id), ui_look(id)); }
}
/* ---- GROOVE LANES: lane n's ROLE on Kn, one list over its PART and CYCLE (stored as they were: scenes and MIDI
 * unchanged): FREE, AUTO, PULSE, OFFBEAT, FILL, then its own cycle of 2 .. 9 pulses over the TYPE's span */
#define ROLE_N 13u
static uint32_t ui_role(uint32_t k)
{
    uint32_t p = gr_set[0][k], c = gr_set[1][k];
    return p == TX_GRP_FREE ? 0u : c ? 4u + c : p + 1u;
}
static void ui_role_set(uint32_t k, uint32_t r)
{
    scn_touch_id(PID(k, PL_GPART));
    p_set(PID(k, PL_GPART), r == 0u ? TX_GRP_FREE : r <= 4u ? (int32_t)r - 1 : TX_GRP_AUTO);
    p_set(PID(k, PL_GCYC), r > 4u ? (int32_t)r - 4 : 0);
}
/* lane k's role as the row reads it ("FREE", "AUTO: PULSE", "OFFBEAT", "5 PER 4 BEATS"); its slots into pos, w */
static unsigned ui_role_text(uint32_t k, char *r, uint8_t *pos, uint8_t *w)
{
    char n[4];
    unsigned beats = 4, ns = tx_groove_slots(&gr_lane[k], gr_type, pos, w, 32u, &beats);
    uint32_t role = ui_role(k), part = gr_lane[k].part;
    if (role == 0u) p_strcpy(r, "FREE", 23);
    else if (role >= 5u) { p_fmt_int(r, (int32_t)role - 3); p_cat(r, " PER ", 23); p_fmt_int(n, (int32_t)beats); p_cat(r, n, 23); p_cat(r, " BEATS", 23); }
    else { p_strcpy(r, role == 1u ? "AUTO" : N_GPART[role - 1u], 23); if (role == 1u && gr_wl(k) && part && part < TX_GRP_FREE) { p_cat(r, ": ", 23); p_cat(r, N_GPART[part], 23); } }
    return ns | beats << 8;
}
/* a row: the lane and its sound, the role by what it does ("AUTO: PULSE", "5 PER 4 BEATS") and Kn; under them what it
 * means ("ON THE BEAT"; a sound the groove leaves alone: "WIND . FLOWS FREE", the row dimmed) and the lane's pulse:
 * its slots over the span (the strong ones larger), the beats as ticks */
static void draw_groove_row(uint32_t k)
{
    static const char *const MEAN[5] = {"NO PULSE", "ON THE BEAT", "BETWEEN BEATS", "IN THE GAPS", "OWN CYCLE"};
    char l[32], r[24], kk[3] = {'K', (char)('1' + k), 0};
    uint8_t pos[32], w[32];
    unsigned beats, ns, j;
    uint32_t role = ui_role(k), part = gr_lane[k].part, x;
    int fr = !gr_wl(k), hot = ui_hot_card == k && ui_hot_t && fm1_ms - ui_hot_t < HOT_MS;
    const int32_t X0 = 124, W = 96;
    cv_begin(RW_W, RW_H, C_BLACK);
    if (sc.snd[k] < 0) { p_strcpy(l, "1 EMPTY", 31); l[0] = (char)('1' + k); aa_text_ov(6, S_TOP(9), &AF_S, l, FC_DIM); cv_blit(RW_X, RW_Y0 + RW_DY * k); return; }
    ns = ui_role_text(k, r, pos, w);
    beats = ns >> 8; ns &= 255u;
    x = (uint32_t)aa_text_ovr(ST_KX, S_TOP(9), &AF_S, kk, mt.tag);
    x = (uint32_t)aa_text_ovr((int32_t)RW_W - 6 - aa_text_w(&AF_S, "K4") - 8, S_TOP(9), &AF_S, r, fr ? FC_DIM : FC_VAL);
    (void)x;
    ui_mix_label(k, l);
    ui_fit(l, (int32_t)RW_W - 6 - aa_text_w(&AF_S, "K4") - 8 - aa_text_w(&AF_S, r) - 12);
    aa_text_ov(6, S_TOP(9), &AF_S, l, fr ? FC_DIM : FC_LAB);
    if (fr) {                                   /* "WIND . FLOWS FREE", a flat line */
        int32_t e;
        p_upper(l, LS[sc.lf[k]].name, 20);
        e = aa_text_ov(6, S_TOP(27), &AF_S, l, FC_DIM);
        e = aa_text_ov(ui_dot_ov(e, 27, FC_DIM), S_TOP(27), &AF_S, "FLOWS FREE", FC_DIM);
        if (e + 6 < X0) cv_rect(X0, 27, W, 1, CD_SEGDIM);
    } else {
        aa_text_ov(6, S_TOP(27), &AF_S, MEAN[role >= 5u ? 4u : role == 0u || !part || part >= TX_GRP_FREE ? 0u : part], FC_DIM);
        for (j = 0; j < beats && j < 16u; j++) cv_rect(X0 + W * (int32_t)j / (int32_t)beats, 30, 1, 5, CD_SEGDIM);
        if (!ns) cv_rect(X0, 27, W, 1, CD_SEGDIM);
        for (j = 0; j < ns; j++) {
            int32_t s = w[j] >= 28u ? 5 : w[j] >= 8u ? 3 : 0, px = X0 + W * (int32_t)pos[j] / 256;
            if (s) cv_rect(px, 27 - s / 2, s, s, s == 5 ? mt.edge : FC_LAB);
        }
    }
    if (hot) {
        cv_frame(0, 0, RW_W, RW_H, 1);
    }
    cv_blit(RW_X, RW_Y0 + RW_DY * k);
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
/* the presets list's row slot k (two rows each, as the sound browser's): a family's header (its name in the accent, a
 * line); a preset: its name, its number at the right; the one loaded solid (filled, an accent bar), the cursor an
 * outline (2 px) */
static void draw_pr_row(uint32_t k)
{
    int32_t h = k < 3u ? (int32_t)RW_DY : (int32_t)RW_H, y0 = (int32_t)RW_DY * (int32_t)k;
    uint32_t r, n = ub_n(), q = ub_loaded();
    cv_begin(RW_W, (uint32_t)h, C_BLACK);
    for (r = 2u * k; r < 2u * k + 2u; r++) {
        uint32_t i = ub.top + r, v;
        int32_t ya = UB_Y(r) - y0, yb = UB_Y(r + 1u) - y0, yc = (ya + yb) / 2;
        char nm[24], s[20];
        int cur, on;
        if (i >= n) continue;
        if (!ub_row(i, &v)) {                   /* a family: its name, a line to the edge */
            int32_t x = aa_text_ov(6, S_TOP(yc), &AF_S, v < PR_NCAT ? pr_cat_name(v) : "USER", UI_ACC) + 6;
            if (x < (int32_t)RW_W - 6) cv_rect(x, yc, (int32_t)RW_W - 6 - x, 1, MT_DIMLINE);
            continue;
        }
        cur = i == ub.cur; on = v == q;
        scn_name_of(v, s);
        { uint32_t c = v < SCN_NF ? 4u : 3u; p_strcpy(nm, s + c + 1u, 23); s[c] = 0; }
        if (on) { cv_rect(0, ya, RW_W, yb - ya - 1, mt.fill); cv_rect(0, ya, 4, yb - ya - 1, UI_ACC); }
        if (cur) {
            cv_frame(0, ya, RW_W, yb - ya - 1, 2);
        }
        ui_fit(nm, (int32_t)RW_W - 18 - 44);
        aa_text_ov(18, S_TOP(yc), &AF_S, nm, cur || on ? FC_VAL : FC_LAB);
        aa_text_ovr((int32_t)RW_W - 8, S_TOP(yc), &AF_S, s, on ? FC_VAL : FC_DIM);
    }
    cv_blit(RW_X, RW_Y0 + RW_DY * k);
}
/* LOAD OPTIONS (FX in the presets list): K1 EFFECTS ON LOAD, K2 PRESETS CHANGE TEMPO, each a row of its choices (the
 * chosen filled); what the effects' choice does; whether the effects now are the player's */
static void draw_opt_row(uint32_t k)
{
    static const char *const DO[3][2] = {{"EVERY PRESET BRINGS", "ITS OWN EFFECTS"}, {"PRESETS BRING THEIR EFFECTS,", "UNTIL YOU TURN ONE: YOURS STAY"},
                                         {"YOUR EFFECTS STAY,", "WHATEVER YOU LOAD"}};
    uint32_t m = scn_fx_mode < 3u ? scn_fx_mode : SCN_FX_SMART;
    cv_begin(RW_W, RW_H, C_BLACK);
    if (k < 2u) {
        uint32_t id = PID(P_GLOBAL, k ? PG_SCNTEMPO : PG_SCNFX), i, n = (uint32_t)p_hi(id) + 1u, c = (uint32_t)p_get(id);
        int32_t x = 6;
        char kk[3] = {'K', (char)('1' + k), 0};
        aa_text_ov(6, S_TOP(9), &AF_S, p_lab(id), FC_LAB);
        aa_text_ovr((int32_t)RW_W - 6, S_TOP(9), &AF_S, kk, mt.tag);
        for (i = 0; i < n; i++) {               /* the choices, the chosen one filled and framed */
            const char *s = p_names(id)[i];
            int32_t w = aa_text_w(&AF_S, s) + 12;
            if (i == c) { cv_rect(x, 19, w, 17, mt.fill); cv_frame(x, 19, w, 17, 1); }
            aa_text_ov(x + 6, S_TOP(27), &AF_S, s, i == c ? FC_VAL : FC_DIM);
            x += w + 6;
        }
    } else if (k == 2u) {
        aa_text_ov(6, S_TOP(9), &AF_S, DO[m][0], FC_LAB);
        aa_text_ov(6, S_TOP(27), &AF_S, DO[m][1], FC_LAB);
    } else {
        aa_text_ov(6, S_TOP(9), &AF_S, m != SCN_FX_SMART ? "A LOAD'S EFFECTS GLIDE," : scn_fx_mine ? "FX NOW: YOURS" : "FX NOW: THE PRESET'S", m != SCN_FX_SMART ? FC_DIM : FC_VAL);
        aa_text_ov(6, S_TOP(27), &AF_S, "THEIR TAILS RING ON", FC_DIM);
    }
    cv_blit(RW_X, RW_Y0 + RW_DY * k);
}
static void draw_browser_row(uint32_t k)
{
    int32_t h = k < 3u ? (int32_t)RW_DY : (int32_t)RW_H, y0 = (int32_t)RW_DY * (int32_t)k;
    uint32_t r, n = ub_n();
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
            cv_frame(0, ya, RW_W, yb - ya - 1, 1);
        }
        if (mine) cv_rect(6, yc - 3, 6, 6, UI_ACC);
        ui_fit(nm, (int32_t)RW_W - 18 - 40);
        aa_text_ov(18, S_TOP(yc), &AF_S, nm, cur ? FC_VAL : mine ? FC_VAL : FC_LAB);
        if (t[0]) aa_text_ovr((int32_t)RW_W - 8, S_TOP(yc), &AF_S, t, cur ? FC_VAL : FC_DIM);
    }
    cv_blit(RW_X, RW_Y0 + RW_DY * k);
}
/* the toast: a box over rows 2 and 3 (both rows draw it whole, in two strips: the canvas holds 64 rows), its head
 * small in the accent with the family ("LOADED . COAST"), the name large when the large face has its letters and room */
static void draw_toast(void)
{
    char h[32], code[8] = {0};
    const char *nm = ui_toast_nm;
    int32_t x, part;
    int big = aa_covers(&AF_L, nm) && aa_text_w(&AF_L, nm) <= (int32_t)TO_W - 24;
    if (!big && (nm[0] == 'F' || nm[0] == 'U') && (nm[3] == ' ' || nm[4] == ' ')) {   /* too long large: its number
                                                 * ("F034") goes up by the family, the name alone large */
        uint32_t c = nm[3] == ' ' ? 3u : 4u;
        p_strcpy(code, nm, c); nm += c + 1u;
        big = aa_covers(&AF_L, nm) && aa_text_w(&AF_L, nm) <= (int32_t)TO_W - 24;
    }
    for (part = 0; part < 2; part++) {
        cv_begin(TO_W, TO_H / 2u, RGB(20, 20, 24));
        cv_oy = -part * (int32_t)(TO_H / 2u);
        cv_frame(0, 0, TO_W, TO_H, 2);
        x = aa_text_ov(12, S_TOP(20), &AF_S, ui_toast_hd, UI_ACC);
        cv_rect(x + 3, 19, 2, 2, FC_LAB);
        x = aa_text_ov(x + 9, S_TOP(20), &AF_S, ui_toast_fam, FC_LAB);
        if (code[0]) aa_text_ov(x + 8, S_TOP(20), &AF_S, code, FC_VAL);
        if (big) aa_text_ov(12, L_TOP(52), &AF_L, nm, FC_VAL);
        else { p_strcpy(h, nm, 31); ui_fit(h, (int32_t)TO_W - 24); aa_text_ov(12, S_TOP(52), &AF_S, h, FC_VAL); }
        cv_oy = 0;
        cv_blit(TO_X, TO_Y + (uint32_t)part * (TO_H / 2u));
    }
}
static void draw_row(uint32_t k)
{
    char v[24];
    uint32_t id;
    if (ui_toast_on() && (k == 1u || k == 2u)) { draw_toast(); return; }
    if (ub.on && ub.pr) { if (ub.opt) draw_opt_row(k); else draw_pr_row(k); return; }
    if (ub.on) { draw_browser_row(k); return; }
    if (ui_g == G_LAND) { draw_strip(k); return; }
    if (ui_g == G_LANES) { draw_lane_row(k); return; }
    if (ui_g == G_GLANES) { draw_groove_row(k); return; }
    id = ui_row_id(k);
    if (id == NOID) { cv_begin(RW_W, RW_H, C_BLACK); cv_blit(RW_X, RW_Y0 + RW_DY * k); return; }
    {
        const char *u = p_text(id, v);
        int32_t lv, val = p_ord(id, p_get(id)), hi = p_hi(id);
        uint32_t look = ui_look(id);
        if (P_SCOPE(id) == P_LFO && P_IDX(id) % 16u == ML_NOW) { look = LK_MID; hi = 64; }   /* (NOW: from the middle) */
        if (mod_live(id, &lv)) ui_card_live = lv;
        draw_card(k, ui_lab(id), v, u, val, hi, look);
    }
}

static void draw_meter(void);
/* the ALGORITHM line: the encoder named as a knob, what it turns and its value ("ALGORITHM  GROOVE TYPE  LOOSE 16") */
static void ft_alg(int32_t yc, const char *what, const char *val)
{
    char v[24];
    int32_t wa = aa_text_w(&AF_S, "ALGORITHM") + 6 + aa_text_w(&AF_S, what) + 6, x;
    p_strcpy(v, val, 23); ui_fit(v, 236 - wa);
    x = (240 - wa - aa_text_w(&AF_S, v)) / 2;
    x = aa_text(x, S_TOP(yc), &AF_S, "ALGORITHM", FC_DIM, FC_BG) + 6;
    x = aa_text(x, S_TOP(yc), &AF_S, what, FC_LAB, FC_BG) + 6;
    aa_text(x, S_TOP(yc), &AF_S, v, FC_VAL, FC_BG);
}
/* the footer's line 2 on a group's page: where its button goes next, beside the page's hold gesture when it fits (the
 * effects: FX held opens FILTER; LFO, ENV: held + a card turned learns a route), else the gesture's short form */
#ifdef DINGHY_HOST
static char ui_ft2[48];                         /* (the host's state "ft2": the last line 2 ft_next wrote, "A | B") */
#endif
static void ft_next(char *t)
{
    int lfo = ui_g == G_LFO || ui_g == G_ENV, i;
    const char *h = lfo ? (ui_g == G_LFO ? "HOLD LFO + TURN: LEARN" : "HOLD ENV + TURN: LEARN")
                  : G_BTN[ui_g] == BTN_FX && ui_g != G_MFILT ? "HOLD: FILTER" : 0;
    ui_next_text(t, G_BTN[ui_g]);
    for (i = 0; lfo && i < 3 && aa_text_w(&AF_S, t) + aa_text_w(&AF_S, h) + 9 > 236; i++) {
        if (i == 0) h = "HOLD + TURN: LEARN";       /* (too wide: the hint drops the button, then the NEXT its */
        else { while (*t && *t != (i == 1 ? ' ' : '>')) t++; if (i == 1 && *t) t++; }   /* button's name, then
                                                 * "NEXT ": "> LFO PHASE") */
    }
    if (h && aa_text_w(&AF_S, t) + aa_text_w(&AF_S, h) + 9 <= 236) ft_two(22, t, h, FC_DIM);
    else { if (lfo) t = (char *)h; h = 0; ft_c(22, t, FC_DIM); }   /* (still too wide: LFO, ENV the gesture alone, as
                                                 * before the NEXT line; the effects their NEXT) */
#ifdef DINGHY_HOST
    p_strcpy(ui_ft2, t, 47); if (h) { p_cat(ui_ft2, " | ", 47); p_cat(ui_ft2, h, 47); }
#endif
}
/* the footer: line 1 what ALGORITHM does here (or the page's hint), line 2 where the page's button goes next */
static void draw_footer(void)
{
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
    else if (ub.on && ub.opt) { ft_c(7, "K1 K2: CHOOSE", FC_LAB); ft_two(22, "FX: PRESETS", "HOME: CLOSE", FC_DIM); }
    else if (ub.on && ub.pr) {                  /* the presets list: what is loaded, what EDIT does */
        char a[24], b[24], c[8];
        uint32_t v, q = ub_loaded();
        (void)ub_row(ub.cur, &v);
        scn_name_of(v, b); b[v < SCN_NF ? 4 : 3] = 0; p_strcpy(c, b, 7);
        p_strcpy(b, "EDIT: LOAD ", 23); p_cat(b, c, 23);
        if (q < SCN_N) { scn_name_of(q, a); a[q < SCN_NF ? 4 : 3] = 0; p_strcpy(c, a, 7); p_strcpy(a, "LOADED ", 23); p_cat(a, c, 23); }
        if (q >= SCN_N) ft_c(7, b, FC_LAB);
        else if (v != q) ft_two(7, a, b, FC_LAB);
        else if (ui_fx_yours()) ft_two(7, "FX: YOURS", "EDIT: ITS FX TOO", FC_LAB);
        else ft_two(7, a, "EDIT: AGAIN", FC_LAB);
        ft_two(22, "HOME: CLOSE", "FX: LOAD OPTIONS", FC_DIM);
    }
    else if (ub.on) {                           /* the browser: what the controls do */
        ft_c(7, ub.lv == 1 ? "ALGORITHM: MOVE   EDIT: OPEN" : "ALGORITHM: SOUND   SELECT: FAMILY", FC_LAB);
        ft_c(22, ub.lv == 1 ? "HOME: CANCEL" : "EDIT: PICK   HOME: BACK", FC_DIM);
    }
    else if (ui_g == G_LAND && sc.fam_pend == SC_FAM_PICKED) {   /* ALGORITHM picked a landscape: EDIT loads it */
        char t[32];
        p_strcpy(t, "LANDSCAPE: ", 31); p_upper(t + 11, LS[sc.fam_want].name, 20);
        ft_c(7, t, FC_VAL);
        ft_c(22, "EDIT: LOAD   HOME: KEEP", FC_DIM);
    }
    else if (ui_g == G_LAND) {                  /* the buttons by what they open (their printed names do not say); HOME's
                                                 * way back named once there is one */
        char t[40];
        ft_two(7, "SEQ: SOUNDS", "FX: EFFECTS", FC_LAB);
        if (ui_back_g == 0xFF) ft_two(22, "GLO: RHYTHM", "SELECT: LANE PAGES", FC_LAB);
        else {
            p_strcpy(t, "HOME: BACK TO ", 39); p_cat(t, ui_title_of(ui_back_g, ui_back_sub), 39);
            if (aa_text_w(&AF_S, t) + aa_text_w(&AF_S, "GLO: RHYTHM") + 9 <= 236) ft_two(22, "GLO: RHYTHM", t, FC_LAB);
            else ft_c(22, t, FC_LAB);
        }
    } else if (ui_g == G_COMP && pv_glob[PG_COMP0 + TX_CP_ON]) {   /* the gain-reduction meter (Frodhes' draw_meter) on
                                                 * line 1, where FX goes next on line 2 as on every effects page */
        char t[32];
        draw_meter();
        ft_next(t);
    }
    else {
        char t[32], v[24];
        uint32_t id = G_ALG(ui_g);
        if (id != NOID) { (void)p_text(id, v); ft_alg(7, G_ALGN(ui_g), v); }
        else if (ui_g == G_LANES) ft_c(7, "K1-K4: LANES 1-4", FC_LAB);
        else if (ui_g == G_SOUND) ft_c(7, sc.snd[sc.sel] < 0 ? "NO SOUND ON THIS LANE" : "ALGORITHM: PICK A SOUND", FC_LAB);
        else if (ui_g == G_LFO) { p_strcpy(v, "LFO 1", 23); v[4] = (char)('1' + ui_lfo); ft_alg(7, "WHICH LFO", v); }
        else if (ui_g == G_ENV) { ui_env_name(v); ft_alg(7, "WHICH ONE", v); }
        else if (ui_g == G_MATRIX) {
            if (mod_cc_learn) ft_c(7, "SEND A CC: MOD CC", FC_LAB);
            else { p_strcpy(v, "SLOT ", 23); p_fmt_int(v + 5, ui_slot + 1); ft_alg(7, "WHICH SLOT", v); }
        }
        else ft_c(7, ui_g == G_TEMPO ? "PLAY: TAP, HOLD: START / STOP" : ui_g == G_MIDI ? "CLOCK OUT: IN MASTER" : "THE SCREEN'S COLOUR, KEPT", FC_LAB);
        if (ui_g == G_LANES) {                  /* (SELECT walks them) */
            p_strcpy(t, "SELECT: NEXT > ", 31); p_cat(t, ui_title_of(ui_sub < 3u ? G_LANES : G_SOUND, ui_sub < 3u ? ui_sub + 1u : 0u), 31);
            ft_c(22, t, FC_DIM);
        }
        else if (ui_g == G_SOUND) ft_c(22, "OCT - / +: OTHER LANE", FC_DIM);
        else if (ui_g == G_MATRIX) {            /* the slot's target now: a bar */
            int32_t lv, mid = mslot[ui_slot].src && mslot[ui_slot].tgt ? (int32_t)mod_tgt_pid(mslot[ui_slot].tgt, mod_tgt_lanes(mslot[ui_slot].tgt) < 4u ? mod_tgt_lanes(mslot[ui_slot].tgt) : 0u) : -1;
            if (mid >= 0 && mod_live((uint32_t)mid, &lv) && p_hi((uint32_t)mid) > 0) {
                cv_rect(20, 20, 200, 4, CD_SEGDIM); cv_rect(20, 20, 200 * clampi(lv, 0, p_hi((uint32_t)mid)) / p_hi((uint32_t)mid), 4, mt.edge);
            }
        }
        else ft_next(t);                        /* (where the button goes next, and the page's hold gesture) */
        if (ui_g == G_ENV) {                    /* its value now: a bar */
            int32_t e = mod_ext[ui_env < TX_ENV_N ? ME_ENV1 + ui_env : ME_FOL1 + ui_env - TX_ENV_N];
            cv_rect(20, 14, 200, 3, CD_SEGDIM); cv_rect(20, 14, 200 * clampi(e, 0, 32767) / 32767, 3, mt.edge);
        }
    }
    cv_blit(0, FT_Y);
}

/* ---- the compressor's gain reduction, as a hardware meter reads (Frodhes' draw_meter): a bar on the footer's line 1
 * filled from the right (1 .. 30 dB on the scale GR_DB10, its 15 steps evenly apart), a peak that holds for a second,
 * the reading in dB; it rises at once and falls at 15 dB/s, like a meter's needle (meter_poll, each signature pass) */
#define GR_SEGS 15u
#define GR_X 6
#define GR_W 228                                /* (the scale's width) */
#define GR_W1 160                               /* the bar drawn: line 1 beside the reading (line 2: where FX goes) */
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
    char t[12] = "-";
    int32_t db10 = gr_db10, fw = gr_px(db10) * GR_W1 / GR_W, pk = gr_peak ? gr_px(GR_DB10[gr_peak - 1u]) * GR_W1 / GR_W : 0;
    cv_rect(GR_X, 3, GR_W1, 9, mt.track);
    cv_rect(GR_X + GR_W1 - fw, 3, fw, 9, mt.fill);
    cv_rect(GR_X + GR_W1 - (fw > 2 ? fw : 2), 3, 2, 9, mt.edge);   /* the fill's edge (at 0: the bar's right end) */
    if (pk > fw) cv_rect(GR_X + GR_W1 - pk, 3, 2, 9, FC_VAL);   /* the peak */
    p_fmt_dec(t + 1, db10); p_cat(t, " dB", 11);
    aa_text_r(234, S_TOP(7), &AF_S, db10 ? t : "0.0 dB", db10 ? FC_VAL : FC_DIM, FC_BG);
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
        h = sig_mix(h, 0xB0u | ub.lv | (uint32_t)ub.pr << 4 | (uint32_t)ub.opt << 5); h = sig_mix(h, ub_loaded()); h = sig_mix(h, ub.f); h = sig_mix(h, ub.cur); h = sig_mix(h, ub.top); h = sig_mix(h, ub.lane);
        h = sig_mix(h, scn_fx_mode); h = sig_mix(h, tm.scn_tempo); h = sig_mix(h, scn_fx_mine);
        if (e == E_ROW0 + 1u || e == E_ROW0 + 2u) h = sig_mix(h, ui_toast_on() ? ui_toast_t : 0u);
        for (k = 0; k < SC_N; k++) { h = sig_mix(h, (uint32_t)sc.snd[k]); h = sig_mix(h, sc.lf[k]); }
        if (e == E_FOOTER) h = sig_mix(h, ui_msg_t && fm1_ms - ui_msg_t < MSG_MS ? ui_msg_t : 0u);
        return h;
    }
    if (ui_lane_page()) { h = sig_mix(h, sc.sel); h = sig_mix(h, (uint32_t)sc.snd[sc.sel]); h = sig_mix(h, sc.lf[sc.sel]); }
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
    if (e == E_HEADER) {
        h = sig_mix(h, tempo_bpm10()); h = sig_mix(h, batt_shown()); h = sig_mix(h, sc.fam); h = sig_mix(h, nt.mode); h = sig_mix(h, (uint32_t)nt.oct);
        h = sig_mix(h, scn_busy() ? scn_ap_idx : scn_cur); h = sig_mix(h, scn_edited);
    }
    else if (e == E_TITLE) {
        h = sig_mix(h, ui_nsub(ui_g)); h = sig_mix(h, ui_fx_yours());
        if (ui_g == G_LAND) { h = sig_mix(h, tempo_bpm10()); h = sig_mix(h, batt_shown()); h = sig_mix(h, nt.mode); h = sig_mix(h, (uint32_t)nt.oct); }
    }
    else if (e == E_FOOTER) {
        h = sig_mix(h, ui_msg_t && fm1_ms - ui_msg_t < MSG_MS ? ui_msg_t : 0u);
        h = sig_mix(h, nt.mode); h = sig_mix(h, sc.sel); h = sig_mix(h, sc.mute[sc.sel]); h = sig_mix(h, sc.fam_pend); h = sig_mix(h, sc.fam_want);
        h = sig_mix(h, scn_save_mode ? scn_save_slot + 1u : 0u);
        h = sig_mix(h, ui_back_g); h = sig_mix(h, ui_back_sub); h = sig_mix(h, ui_lfo); h = sig_mix(h, ui_slot);
        if (G_ALG(ui_g) != NOID) h = sig_mix(h, (uint32_t)p_get(G_ALG(ui_g)));
        if (ui_g == G_COMP) h = sig_mix(h, meter_poll());
        if (scn_save_mode) { uint32_t i; h = sig_mix(h, scn_nm_pos); for (i = 0; i < 11u; i++) h = sig_mix(h, (uint8_t)scn_nm[i]); }
    } else if (e >= E_ROW0 && e < E_ROW0 + 4u) {
        k = e - E_ROW0;
        if (k == 1u || k == 2u) h = sig_mix(h, ui_toast_on() ? ui_toast_t : 0u);
        h = sig_mix(h, ui_hot_card == k && ui_hot_t && fm1_ms - ui_hot_t < HOT_MS);
        if (ui_g <= G_LANES || ui_g == G_GLANES) {
            h = sig_mix(h, sc.lf[k]); h = sig_mix(h, (uint32_t)sc.snd[k]); h = sig_mix(h, sc.run[k]);
            h = sig_mix(h, sc_vol_of(k)); h = sig_mix(h, sc.pan[k]); h = sig_mix(h, sc.mute[k]);
            h = sig_mix(h, sc_cnt_of(k)); h = sig_mix(h, ui_lane_act(k));
            if (ui_g == G_LANES) { int32_t lv; uint32_t id = ui_row_id(k); h = sig_mix(h, id != NOID && mod_live(id, &lv) ? (uint32_t)lv / 4u + 1u : 0u); }
            if (ui_g == G_LAND) h = sig_mix(h, k == sc.sel);
            if (ui_g == G_GLANES) { h = sig_mix(h, gr_set[0][k] | gr_set[1][k] << 4); h = sig_mix(h, gr_lane[k].part | gr_lane[k].wl << 4); h = sig_mix(h, gr_type); }
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

/* ---- the splash: the mark (three planes of straight lines), the name, what it is, the version */
static void sp_pix(int32_t x, int32_t y, uint16_t c, uint32_t a)   /* c at a/256 over what is there: the brighter wins */
{
    uint32_t i, o, r, g, b;
    y += cv_oy;
    if ((uint32_t)x >= cv_w || (uint32_t)y >= cv_h) return;
    i = (uint32_t)y * cv_w + (uint32_t)x;
    o = swap16(cv_px[i]);
    r = ((c >> 11) * a) >> 8; g = ((c >> 5 & 63u) * a) >> 8; b = ((c & 31u) * a) >> 8;
    if ((o >> 11) > r) r = o >> 11;
    if ((o >> 5 & 63u) > g) g = o >> 5 & 63u;
    if ((o & 31u) > b) b = o & 31u;
    cv_px[i] = swap16((r << 11) | (g << 5) | b);
}
static void sp_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)   /* anti-aliased, ends in 1/16 px */
{
    int32_t steep = (y1 > y0 ? y1 - y0 : y0 - y1) > (x1 > x0 ? x1 - x0 : x0 - x1), t, x, e, q;
    if (steep) { t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    if (x1 == x0) return;
    for (x = x0 >> 4, e = x1 >> 4; x <= e; x++) {
        q = y0 * 16 + ((x * 16 + 8 - x0) * (y1 - y0) * 16) / (x1 - x0);   /* the line's y here, 1/256 px */
        if (steep) { sp_pix(q >> 8, x, c, 256u - (uint32_t)(q & 255)); sp_pix((q >> 8) + 1, x, c, (uint32_t)(q & 255)); }
        else { sp_pix(x, q >> 8, c, 256u - (uint32_t)(q & 255)); sp_pix(x, (q >> 8) + 1, c, (uint32_t)(q & 255)); }
    }
}
static const int16_t SP_PLANE[3][8] = {         /* a plane: lines strung from edge A-B to edge D-C (A, B, C, D: x, y) */
    {34, 28, 150, 16, 206, 90, 70, 116}, {150, 16, 206, 90, 70, 116, 34, 28}, {92, 16, 206, 54, 150, 116, 34, 78}};
static void sp_mark(void)
{
    uint32_t k, i;
    for (k = 0; k < 3u; k++) {
        const int16_t *p = SP_PLANE[k];
        for (i = 0; i <= 28u; i++) {            /* 29 lines a plane, ends in 1/16 px */
            int32_t u = (int32_t)i;
            sp_line(p[0] * 16 + (p[2] - p[0]) * 16 * u / 28, p[1] * 16 + (p[3] - p[1]) * 16 * u / 28,
                    p[6] * 16 + (p[4] - p[6]) * 16 * u / 28, p[7] * 16 + (p[5] - p[7]) * 16 * u / 28, pal[1u + k]);
        }
    }
}
static uint32_t ui_splash_t;
static uint8_t ui_splash_on;
static void ui_splash(void)
{
    char v[24];
    mt_update();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    cv_begin(240, 64, C_BLACK);                 /* the mark, rows 16 .. 116, in two strips */
    cv_oy = -16; sp_mark(); cv_blit(0, 16);
    cv_begin(240, 37, C_BLACK);
    cv_oy = -80; sp_mark(); cv_blit(0, 80);
    cv_oy = 0;
    cv_begin(240, 64, C_BLACK);
    ft_big(16, "TEXTURA", UI_ACC);
    ft_c(48, "TEXTURE SYNTHESIZER", FC_LAB);
    cv_blit(0, 130);
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
        if (ui_toast_t) ui_toast_t = fm1_ms ? fm1_ms : 1u;   /* (the boot's load: shown once the splash is gone) */
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
            if (sc.snd[k] >= 0 && ui_blink_on(k) != ui_blink_shown[k] && !((k == 1u || k == 2u) && ui_toast_on()))   /* (not
                                                 * under the toast) */
                ui_blink_dot(k, ui_blink_on(k), 0);
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
    if (ui_g != G_LAND) { ui_back_g = ui_g; ui_back_sub = ui_sub; }   /* (HOME's way back: the last page left, however) */
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
static void ub_close(void) { ub.on = ub.pr = ub.opt = 0; ub.pend = 0; ui_all(); }   /* (what plays stays) */
/* ---- the presets list's moves: the cursor walks the presets (a family's header passed over), its family kept in
 * ub.f; the window shows a family's header when its first preset is at its top */
static void ub_pr_view(void)
{
    uint32_t c, v;
    for (c = 0; c + 1u < ub_ncat() && ub.cur >= ub_head_of(c + 1u); c++) {}
    ub.f = (uint8_t)c;
    ub_view();
    if (ub.cur == ub.top && ub.top && !ub_row(ub.top - 1u, &v)) ub.top--;
}
static void ub_pr_frame(void)                   /* (opened, a family jumped to: the window from the cursor's family's
                                                 * header, the cursor in it; far down a family: the cursor mid-window) */
{
    uint32_t h, n = ub_n();
    ub_pr_view();
    h = ub_head_of(ub.f);
    ub.top = (uint8_t)(ub.cur < h + UB_ROWS ? h : ub.cur - UB_ROWS / 2u);
    if (n > UB_ROWS && ub.top > n - UB_ROWS) ub.top = (uint8_t)(n - UB_ROWS);
}
static void ub_pr_open(void)                    /* PRESETS: the list, the cursor on the preset loaded (none: the first),
                                                 * not moved */
{
    uint32_t q = ub_loaded(), i, v, n;
    ub.on = 1; ub.pr = 1; ub.opt = 0; ub.lv = 2; ub.pend = 0; ub.top = 0; ub.cur = 1;
    for (i = 0, n = ub_n(); i < n; i++) if (ub_row(i, &v) && v == q) { ub.cur = (uint8_t)i; break; }
    ub_pr_frame();
    ui_all();
}
static void ub_follow(void)                     /* another lane (OCT, a black key): the browser on it, its sound kept */
{
    if (ub.pr || sc.sel == ub.lane) return;
    ub.lane = sc.sel; ub.of = sc.lf[ub.lane]; ub.oi = sc.snd[ub.lane];
    ub_at_lane();
}
static void ub_move(int32_t d)                  /* ALGORITHM, K1 (the presets: PRESETS too): the cursor (no wrap); the
                                                 * sounds' level 2: the preview's wait; the presets: nothing loads */
{
    int32_t n = (int32_t)ub_n(), c = (int32_t)ub.cur + d;
    if (ub.pr) {
        int32_t s = d > 0 ? 1 : -1, k = d > 0 ? d : -d, c2;
        uint32_t v;
        for (c = ub.cur; k > 0; k--) {          /* d presets on, the headers passed over */
            for (c2 = c + s; c2 >= 0 && c2 < n && !ub_row((uint32_t)c2, &v); c2 += s) {}
            if (c2 < 0 || c2 >= n) break;
            c = c2;
        }
        ub.cur = (uint8_t)c;
        ub_pr_view();
        return;
    }
    ub.cur = (uint8_t)clampi(c, 0, n - 1);
    ub_view();
    if (ub.lv == 2) { ub.pend = 1; ub.t = fm1_ms; }
}
static void ub_family(int32_t d)                /* SELECT at level 2: the previous / next family, its first */
{
    int32_t f = clampi((int32_t)ub.f + d, 0, (int32_t)(ub.pr ? ub_ncat() : LS_N) - 1);
    if (f == ub.f) return;
    if (ub.pr) { ub.cur = (uint8_t)(ub_head_of((uint32_t)f) + 1u); ub_pr_frame(); return; }
    ub.f = (uint8_t)f; ub.top = 0;
    ub.cur = (uint8_t)(sc.lf[ub.lane] == f && sc.snd[ub.lane] >= 0 ? sc.snd[ub.lane] : 0);
    ub_view();
    ub.pend = 1; ub.t = fm1_ms;
}
static void ub_say(void) { char t[32]; p_upper(t, sc_sound_name(ub.lane), 31); ui_say(t); }
static void ub_edit(void)                       /* EDIT: open the family / pick the sound; the presets: load the one under
                                                 * the cursor (the list stays; on the one loaded with FX: YOURS: with its
                                                 * effects) */
{
    if (ub.pr) {
        uint32_t v;
        if (ub_row(ub.cur, &v)) scn_pb_go(v, v == ub_loaded() && ui_fx_yours());
        return;
    }
    if (ub.lv == 1) {
        ub.f = ub.cur; ub.lv = 2; ub.top = 0;
        ub.cur = (uint8_t)(sc.lf[ub.lane] == ub.f && sc.snd[ub.lane] >= 0 ? sc.snd[ub.lane] : 0);
        ub_view();
        return;
    }
    sc_sound_set(ub.lane, ub.f, ub.cur);        /* (a stopped lane: loaded stopped) */
    scn_touch();
    ub_close();
    ub_say();
}
static void ub_home(void)                       /* HOME: back to the families; there: the sound it opened with, closed;
                                                 * the presets: closed, what plays kept */
{
    if (ub.pr) { ub_close(); return; }
    if (ub.lv == 2) { ub.lv = 1; ub.cur = ub.f; ub.top = 0; ub.pend = 0; ub_view(); return; }
    if (ub.oi >= 0) sc_sound_set(ub.lane, ub.of, (uint32_t)ub.oi);
    ub_close();
    ui_say("SOUND KEPT");
}
static void ub_poll(void)                       /* the preview: the cursor's sound on the lane, while it plays */
{
    if (!ub.on || ub.pr || !ub.pend || fm1_ms - ub.t < UB_PREVIEW_MS) return;
    ub.pend = 0;
    if (ub.lv == 2 && sc.run[ub.lane]) { sc_sound_set(ub.lane, ub.f, ub.cur); scn_touch(); }
}
static void ui_say_lane(void)                   /* "LANE 2: RAIN" (an empty lane: "LANE 2: EMPTY") */
{
    char t[32];
    p_strcpy(t, "LANE 1: ", 8); t[5] = (char)('1' + sc.sel);
    p_upper(t + 8, sc.snd[sc.sel] >= 0 ? sc_sound_name(sc.sel) : "EMPTY", 23);
    ui_say(t);
}
static void ui_lane_focus(uint32_t k)           /* a black key: lane k, shown on LANDSCAPE (the 4 lanes, as HOME): it
                                                 * stays there (HOME's way back kept), any other page goes to it */
{
    sc_select(k);
    if (ub.on && !ub.pr) ub_follow();           /* (the browser stays, on this lane) */
    else ui_goto(G_LAND, 0);
    ui_say_lane();
}
static void ui_lane_mute(uint32_t k)            /* a black key held: lane k muted / unmuted ("LANE 2 MUTED") */
{
    char t[24];
    if (k >= SC_N) return;
    if (sc.snd[k] < 0) { ui_say("NO SOUND ON THIS LANE"); return; }
    sc_mute(k, !sc.mute[k]);
    scn_touch();
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
    ui_say_lane();
}
/* a parameter turned by d detents: an amount or a choice by d, a toggle on (up) / off (down) */
static void ui_turn(uint32_t id, int32_t d)
{
    uint32_t kd = p_kind(id);
    int32_t v = p_ord(id, p_get(id));                  /* (in the panel's order: SLOPE 12, 18, 24) */
    if (kd == PK_TOGGLE) v = d > 0;
    else v += d;
    scn_touch_id(id);
    p_set(id, p_ord(id, clampi(v, 0, p_hi(id))));
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
    if (ui_g == G_LAND) id = PID(k, PL_LEVEL);   /* (LANDSCAPE: lane k's volume) */
    else id = ui_row_id(k);
    ui_lfo_learned = 1;
    s = id == NOID ? -1 : mod_learn(ui_lsrc, id, d);
    if (s >= 0) scn_touch();
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
    if (ui_g == G_LANES && ui_sub == 3u) { if (sc.snd[k] >= 0 && sc.mute[k] != (d < 0)) ui_lane_mute(k); return; }   /* (ON
                                                 * / MUTE: up on, down muted) */
    if (ui_g == G_GLANES) { if (sc.snd[k] >= 0) ui_role_set(k, (uint32_t)clampi((int32_t)ui_role(k) + d, 0, ROLE_N - 1)); return; }
    if (ui_g != G_LAND) {
        uint32_t id = ui_row_id(k);
        if (id != NOID) ui_turn(id, ui_accel(k, p_kind(id) == PK_AMOUNT ? p_hi(id) : 0, d));
        return;
    }
    if (sc.snd[k] < 0) return;                  /* LANDSCAPE: lane k's volume (an empty lane: none) */
    scn_touch();
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
    uint32_t id;
    if (ui_g == G_LAND) { sc_family_turn(d); return; }   /* (picked, not loaded: EDIT loads it, P4) */
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
    id = G_ALG(ui_g);
    if (id == NOID) return;
    ui_turn(id, d);
    ui_hot_card = 0xFF;
    ui_hot_id(id);
    {                                           /* "FREEZE ON", "REVERB HALL" */
        char t[32], v[24];
        p_strcpy(t, G_ALGN(ui_g), 12); p_cat(t, " ", 31);
        (void)p_text(id, v); p_cat(t, v, 31);
        ui_say(t);
    }
}
static void ui_tap(int say)                     /* a tap (PLAY's, REC's): the third says "TEMPO 120.0" (FINE's text); none
                                                 * while a clock leads (say: "EXT CLOCK") */
{
    char t[24], n[16];
    if (tm.clock == CLK_FOLLOW || clk_follows()) { if (say) ui_say("EXT CLOCK"); return; }
    if (!tempo_tap()) return;
    (void)p_text(PID(P_GLOBAL, PG_FINE), n);
    p_strcpy(t, "TEMPO ", 7); p_cat(t, n, 23);
    ui_say(t);
}
/* PLAY: a tap on the press (its time exact); held 600 ms, the transport instead, and the press's tap taken back */
#define TAP_HOLD_MS 600u
static uint32_t ui_play_t;
static uint8_t ui_play_down, ui_play_held;
static void ui_play(uint32_t press, uint32_t release)
{
    if (press) {
        ui_play_t = fm1_ms; ui_play_down = 1; ui_play_held = 0;
        ui_tap(1);
    }
    if (release) ui_play_down = 0;
    if (ui_play_down && !ui_play_held && fm1_ms - ui_play_t >= TAP_HOLD_MS) {
        ui_play_held = 1;
        tempo_tap_undo();
        if (tempo_transport(!tm_running())) ui_say(tm_running() ? "TRANSPORT: RUNNING" : "TRANSPORT: STOPPED");
        else ui_say("EXT CLOCK");
    }
}
/* REC: a press alone stops all lanes and the transport together, or starts them (bar 1 there: the grooved lanes start
 * in phase); presses within REC_TAP_MS of the one before are taps: the tempo from the third press (as PLAY's taps;
 * none while a clock leads, REC still starts and stops then), and a series that stopped things at its first press
 * starts them again at its second, so tapping never leaves them stopped */
#define REC_TAP_MS 1500u
static uint32_t ui_rec_t;
static uint8_t ui_rec_stopped;
static void ui_rec(void)
{
    int run = sc_any_run(), tap = fm1_ms - ui_rec_t < REC_TAP_MS && ui_rec_t, on = tap || !run;
    ui_rec_t = fm1_ms ? fm1_ms : 1u;
    if (!tap || ui_rec_stopped) {               /* (a series' first press; its second, the first having stopped) */
        if (on != run) { sc_all_toggle(); scn_touch(); }
        (void)tempo_transport(on);              /* (0: a clock leads, it moves the transport) */
        ui_rec_stopped = (uint8_t)!on;
        ui_say(on ? "ALL PLAYING" : "ALL STOPPED");
    }
    ui_tap(tap);                                /* (the third: the tempo, bar 1 there; none while a clock leads) */
}
static uint32_t ui_fx_t;
static uint8_t ui_fx_down;                      /* FX pressed, its release or its hold to come */
static void ui_sel(uint32_t b, uint32_t rel)    /* SEL: a tap switches the key mode; held, the other mode while held
                                                 * (a key or a button used meanwhile: back when let go) */
{
    if (b >> BTN_SEL & 1u) {
        if (nt.mode == NT_PLAY) ui_say("SEL + BLACK KEY: LANE");   /* (held: how to pick a lane while playing) */
        nt_mode_set(nt.mode ^ 1u); nt.sel_down = 1; nt.sel_used = 0; b &= ~(1u << BTN_SEL);
    }
    if (nt.sel_down && b) nt.sel_used = 1;
    if (!(rel >> BTN_SEL & 1u) || !nt.sel_down) return;
    nt.sel_down = 0;
    if (nt.sel_used) nt_mode_set(nt.mode ^ 1u);
    else ui_say(nt.mode == NT_PLAY ? "KEYS: PLAY" : "KEYS: LANDSCAPE");
}
static void ui_input(uint32_t b, uint32_t rel)
{
    int32_t d[7];
    uint32_t i;
    for (i = 0; i < 7u; i++) d[i] = fm1_enc_take(i);
    if (ui_splash_on) for (i = 0; i < 7u; i++) if (d[i] || b) ui_splash_t = fm1_ms - UI_SPLASH_MS;
    if (scn_input(b, rel, d[ENC_PRESETS], d[ENC_KNOB1], d[ENC_KNOB2], d[ENC_KNOB3])) {   /* (the save mode takes
                                                         * PRESETS, K1 .. K3 and HOME) */
        if (ub.on) ub_close();
        d[ENC_PRESETS] = d[ENC_KNOB1] = d[ENC_KNOB2] = d[ENC_KNOB3] = 0;
        b &= ~(1u << BTN_HOME | 1u << BTN_OCT_DOWN);
    }
    ub_poll();
    if (ub.on) {                                /* the browser takes ALGORITHM, K1, SELECT, EDIT, HOME (SEQ: closes the
                                                 * sound browser; the presets one: closed, the sound browser opens);
                                                 * any other button closes it, what plays kept; PRESETS: the presets
                                                 * list's cursor (the sound browser: closed, the presets list opens).
                                                 * The presets list: FX its LOAD OPTIONS (again: the list), K1 K2 there
                                                 * their choices */
        uint32_t own = 1u << BTN_EDIT | 1u << BTN_HOME | 1u << BTN_SEQ | (ub.pr ? 1u << BTN_FX : 1u << BTN_OCT_DOWN | 1u << BTN_OCT_UP);
        if (b >> BTN_SEQ & 1u) { if (!ub.pr) b &= ~(1u << BTN_SEQ); ub_close(); }
        else if (b & ~own || (d[ENC_PRESETS] && !ub.pr)) ub_close();
        else if (ub.opt || b >> BTN_FX & 1u) {  /* LOAD OPTIONS */
            if (b >> BTN_FX & 1u) { ub.opt ^= 1u; ui_all(); }
            else if (b >> BTN_HOME & 1u) ub_close();
            else {
                if (d[ENC_KNOB1]) { ui_turn(PID(P_GLOBAL, PG_SCNFX), d[ENC_KNOB1]); ui_all(); }
                if (d[ENC_KNOB2]) { ui_turn(PID(P_GLOBAL, PG_SCNTEMPO), d[ENC_KNOB2]); ui_all(); }
            }
            b &= ~(1u << BTN_FX | 1u << BTN_EDIT | 1u << BTN_HOME);
            d[ENC_PRESETS] = d[ENC_ALGORITHM] = d[ENC_SELECT] = 0;
            for (i = 0; i < 4u; i++) d[ENC_KNOB1 + i] = 0;
        }
        else {
            if (b >> BTN_EDIT & 1u) ub_edit();
            else if (b >> BTN_HOME & 1u) ub_home();
            if (d[ENC_PRESETS] && ub.on) ub_move(d[ENC_PRESETS]);
            d[ENC_PRESETS] = 0;
            if (d[ENC_ALGORITHM]) ub_move(d[ENC_ALGORITHM]);
            if (d[ENC_KNOB1]) ub_move(d[ENC_KNOB1]);
            if (d[ENC_SELECT]) { if (ub.lv == 2) ub_family(d[ENC_SELECT]); else ub_move(d[ENC_SELECT]); }
            b &= ~(1u << BTN_EDIT | 1u << BTN_HOME);
            d[ENC_ALGORITHM] = d[ENC_SELECT] = 0;
            for (i = 0; i < 4u; i++) d[ENC_KNOB1 + i] = 0;
        }
    }
    if (!ub.on && b >> BTN_SEQ & 1u) ub_open(); /* SEQ (SOUNDS): the browser, for the selected lane */
    else if (!ub.on && d[ENC_PRESETS]) ub_pr_open();   /* PRESETS: the presets list (its first detent opens it) */
    if (b >> BTN_HOME & 1u && ui_g == G_LAND && sc.fam_pend == SC_FAM_PICKED) {   /* (a landscape picked: kept as is) */
        sc.fam_want = sc.fam; sc.fam_pend = 0;
        ui_say("LANDSCAPE KEPT");
        b &= ~(1u << BTN_HOME);
    }
    if (b >> BTN_EDIT & 1u && ui_g == G_LAND && sc.fam_pend == SC_FAM_PICKED) {   /* EDIT: the landscape picked loads */
        char t[32];
        sc_family_go();
        scn_cur = 0xFF; scn_edited = 0;         /* (no preset now: the header names the landscape) */
        p_strcpy(t, "LANDSCAPE ", 31); p_upper(t + 10, LS[sc.fam].name, 20);
        ui_say(t);
        ui_toast("LOADED", 0xFF);
        b &= ~(1u << BTN_EDIT);
    }
    if (b >> BTN_HOME & 1u) {                   /* HOME: the home page; there: back to the last page (P7) */
        if (ui_g != G_LAND) ui_goto(G_LAND, 0);
        else if (ui_back_g != 0xFF) ui_goto(ui_back_g, ui_back_sub);
    }
    if (b >> BTN_EDIT & 1u) ui_goto(G_SOUND, ui_g == G_SOUND ? (ui_sub + 1u) % ui_nsub(G_SOUND) : 0u);   /* (the lane's
                                                 * SOUND; again: its next page) */
    if (b >> BTN_FX & 1u) { ui_fx_t = fm1_ms; ui_fx_down = 1; }   /* FX: on the release (held TAP_HOLD_MS: FILTER) */
    if (ui_fx_down && fm1_ms - ui_fx_t >= TAP_HOLD_MS) { ui_fx_down = 0; ui_goto(G_MFILT, 0); }
    for (i = 0; i < 2u; i++)                    /* FX (EFFECTS, REVERB, DELAY, FILTER, DRIVE, COMPRESSOR), GLO (RHYTHM,
                                                 * GROOVE LANES, SCALE, TEMPO, MIDI, MIDI IN, DISPLAY): every page in
                                                 * turn, wrapping */
        if (i ? b >> BTN_GLO & 1u : rel >> BTN_FX & ui_fx_down) { uint8_t g, s; ui_next(i ? BTN_GLO : BTN_FX, &g, &s); ui_goto(g, s); }
    if (rel >> BTN_FX & 1u) ui_fx_down = 0;
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
    ui_sel(b, rel);
    if (b >> BTN_OCT_DOWN & 1u) { if (nt.mode == NT_PLAY) nt_oct_step(-1); else ui_lane_step(-1); }
    if (b >> BTN_OCT_UP & 1u) { if (nt.mode == NT_PLAY) nt_oct_step(1); else ui_lane_step(1); }
    if (b >> BTN_REC & 1u && nt.mode == NT_LANDSCAPE) ui_rec();
    if (d[ENC_SELECT]) ui_page_step(d[ENC_SELECT]);
    if (d[ENC_ALGORITHM]) ui_algorithm(d[ENC_ALGORITHM]);
    for (i = 0; i < 4u; i++) if (d[ENC_KNOB1 + i]) ui_knob(i, d[ENC_KNOB1 + i]);
}
/* the buttons' LEDs: HOME (the home page), EDIT, FX, LFO, ENV, ARP or GLO on their own pages only, PLAY the beat, SEL in PLAY mode, REC while anything plays, OCT where it can
 * still go */
static uint32_t ui_btn_leds(void)
{
    uint32_t b = 0;
    if (ui_g != G_LANES) b |= 1u << G_BTN[ui_g];   /* (the lane pages: SELECT's, no button's) */
    if (ui_latched()) b |= 1u << (ui_lsrc >= MS_ENV1 ? BTN_ENV : BTN_LFO);   /* (learn latched) */
    if (ub.on && !ub.pr) b |= 1u << BTN_SEQ;    /* (the sound browser open) */
    if (ub.on && ub.pr) b |= 1u << BTN_SAVE;    /* (the presets list open: SAVE stores what plays) */
    if (ub.opt) b |= 1u << BTN_FX;              /* (its LOAD OPTIONS) */
    if (scn_save_mode || (scn_sv_on && fm1_ms >> 7 & 1u)) b |= 1u << BTN_SAVE;   /* (the save mode; saving: blinking) */
    if (tempo_beat_led()) b |= 1u << TX_BTN_TAP;
    if (nt.mode == NT_PLAY) b |= 1u << BTN_SEL;
    if (sc_any_run()) b |= 1u << BTN_REC;
    if (nt.mode == NT_PLAY ? nt.oct < 0 : sc.sel > 0) b |= 1u << BTN_OCT_DOWN;
    if (nt.mode == NT_PLAY ? nt.oct > 0 : sc.sel < SC_N - 1u) b |= 1u << BTN_OCT_UP;
    return b;
}
