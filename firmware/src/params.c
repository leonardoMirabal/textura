/* SPDX-License-Identifier: GPL-3.0-only */
/* The parameters: one table that the pages, MIDI (CC and NRPN) and, later, the scenes and the mod matrix all read.
 *
 * An id is scope << 8 | index: scopes 0..3 are the lanes (each lane has the same list), 4 the globals (the effects on
 * the mix). Ids never change once given (append only: tests/scripts/params.txt checks a few by number).
 *   lane    0 SOUND  1 HOW MANY  2 HOW ACTIVE  3 LEVEL  4 PAN
 *           8 .. 15   the sound's cards (what they turn depends on the sound: sound_cards.h)
 *           32 .. 35  DRIVE  AMOUNT TONE MIX TYPE          (tx_fx_drive_params order)
 *           36 .. 41  FILTER TYPE FREQ RESO LFO RATE SHAPE  (tx_fx_filter_params order)
 *   global  0 .. 11   GRAIN DELAY (tx_fx_grain_params order)   12 its SYNC (tempo.c)
 *           16 .. 25  REVERB (tx_fx_reverb_params order)
 *           26 .. 31  COMPRESSOR: COMP THR ATK REL RAT MKUP (tx_comp_params order; Frodhes' COMPRESSOR's)
 *           32 .. 37  TEMPO: BPM (40 .. 240) FINE (BPM in tenths) STATUS (read only) TRANSPORT TAP (over 63: a tap)
 *                     SCENE TEMPO (a scene's load sets its tempo: on / off, in the settings)
 *           40 .. 43  MIDI: CLOCK (INT MASTER FOLLOW AUTO) CLK IN USB, CLK IN TRS, CLK OUT (OFF / USB: MASTER sends)
 *           44        DISPLAY: COLOR (the screen's palette: COLOR_NAME; in the settings)
 *   5       LFO n (0..7): n x 16 + SHAPE RATE SYNC MODE PHASE WIDTH JITTER FADE RESTART LEVEL (tx_lfo_params order),
 *           10 NOW 11 ROUTES (read only)
 *   6       ENV n (0..1): n x 16 + TRIGGER ATTACK DECAY RELEASE SUSTAIN CURVE VARY LOOP (tx_env_params order);
 *           FOLLOW n (0..1): 32 + n x 16 + INPUT ATTACK RELEASE GAIN (tx_fol_params order)
 *   7       the matrix: slot (0..15) x 4 + SOURCE TARGET DEPTH SPREAD; 64 MOD CC
 * The lane model's entries (SOUND .. PAN) are the landscape's (scape.c); the cards set the lane's engine (or its
 * hosted engine); the effects are kept here and sent to lanes.c's chain; the tempo's and the clock's are tempo.c's.
 *
 * MIDI: a lane listens on channel 1 + lane, the globals on channel 5 (P_CH_GLOBAL); the same CC numbers on each lane
 * channel (P_CC_LANE). Every parameter is also an NRPN on any of those channels: MSB 99 = the scope, LSB 98 = the
 * index, data entry 6 (7 bits, scaled onto the range; 38, the fine byte, is taken and ignored). Main loop only. */
#include "sound_cards.h"

/* text helpers (the screen's, and the values' here) */
static void p_strcpy(char *d, const char *s, uint32_t max) { uint32_t i = 0; while (s[i] && i < max) { d[i] = s[i]; i++; } d[i] = 0; }
static void p_fmt_int(char *s, int32_t v)
{
    char t[12];
    int32_t i = 0, n = 0;
    if (v < 0) { s[n++] = '-'; v = -v; }
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v && i < 11);
    while (i) s[n++] = t[--i];
    s[n] = 0;
}
static void p_upper(char *d, const char *s, uint32_t max)   /* the screen writes in capitals */
{
    uint32_t i = 0;
    while (s[i] && i < max) { d[i] = s[i] >= 'a' && s[i] <= 'z' ? (char)(s[i] - 32) : s[i]; i++; }
    d[i] = 0;
}
static void p_cat(char *d, const char *s, uint32_t max) { uint32_t n = 0; while (d[n]) n++; if (n < max) p_strcpy(d + n, s, max - n); }

#define P_LANES SC_N
#define P_GLOBAL 4u
#define PID(scope, i) ((uint32_t)(scope) << 8 | (uint32_t)(i))
#define P_SCOPE(id) ((id) >> 8)
#define P_IDX(id) ((id) & 0xFFu)
#define P_LANE_ANY 0xFu                         /* a page's card: "the selected lane" (resolved when drawn / turned) */
#define P_LFO 5u                                /* the LFOs: n x 16 + param (mod.c) */
#define P_MAT 7u                                /* the matrix: slot x 4 + SOURCE TARGET DEPTH SPREAD, 64 MOD CC (mod.c) */
#define P_ENV 6u                                /* the envelopes: n x 16 + param; the followers: 32 + n x 16 + param (mod.c) */
#define P_MODS(s) ((s) == P_LFO || (s) == P_ENV || (s) == P_MAT)   /* the modulation's scopes (mod.c's) */
#define NOID_P 0xFFFFu
enum { PL_SOUND, PL_COUNT, PL_ACTIVE, PL_LEVEL, PL_PAN, PL_CARD0 = 8, PL_DRIVE0 = 32, PL_FILTER0 = 36, PL_NUM = 42 };
enum { PG_GRAIN0 = 0, PG_GSYNC = 12, PG_REVERB0 = 16, PG_COMP0 = 26, PG_N_ = 32, PG_BPM = 32, PG_FINE, PG_STATUS, PG_RUN,
       PG_TAP, PG_SCNTEMPO, PG_CLOCK = 40, PG_INUSB, PG_INTRS, PG_OUT, PG_COLOR };
#define P_CH_GLOBAL 4u                          /* MIDI channel 5 (0-based 4) */

/* how a value reads */
enum { F_NUM, F_ENUM, F_ONOFF, F_SOUND, F_COUNT, F_ACTIVE, F_PAN, F_HZ_FL, F_HZ_LFO, F_MS_GD, F_MS_GS, F_MS_PD, F_SEMI,
       F_PLUS1, F_DIV, F_BPM, F_BPM10, F_STATUS, F_RUN, F_THR, F_R10, F_GAIN };
static const char *const N_DRIVE[] = {"FDRV", "KHLON", "TSH8", "OHCD", "BHLUE"};
static const char *const N_FILTER[] = {"OFF", "LP12", "LP18", "HP12", "HP18", "BP", "NOTCH"};
static const char *const N_SHAPE[] = {"SINE", "TRI", "SAW UP", "SAW DN", "SQUARE", "RANDOM"};
static const char *const N_REVERB[] = {"OFF", "ROOM", "HALL", "INFINITE", "DARK", "DRIFT", "SWELL", "PLATE"};
static const char *const N_SHIFT[] = {"+OCT", "+5TH", "+OCT+5", "-OCT"};
static const char *const N_CLOCK[] = {"INT", "MASTER", "FOLLOW", "AUTO"};
static const char *const N_CLKOUT[] = {"OFF", "USB"};   /* (TRS: the FM-1's jack is an input only) */
static const char *const N_RATIO[] = {"1.5:1", "2:1", "3:1", "4:1", "6:1", "8:1", "10:1", "20:1"};   /* (Frodhes') */

/* ---- COLOR (DISPLAY K1): the screen's palette, Frodhes' (its ui.c, COLOR_ACC) with RED first: step 0 RED and 1 AMBER
 * are gfx.c's own ramps (RED the default: the screen as before), then 25 pastel hues around the colour wheel, hue
 * falling from peach through the pinks, violets, blues, cyans and greens to yellow, as one RGB565 accent each; their
 * other steps are fractions of the accent, the brightest half way to white (color_apply). The index is saved (the
 * settings, scenes.c), so the list only ever grows at its end. (Frodhes' own RED, a saturated red it appended, is
 * RED's place here: Textura's red ramp.) */
#define COLOR_N 27u
static const uint16_t COLOR_ACC[COLOR_N] = {
    0, 0,                                             /* RED, AMBER: gfx.c's ramps */
    0xFD0E, 0xFC50, 0xFCB5, 0xFCB9, 0xFCDC,
    0xFCFF, 0xED1F, 0xDD1F, 0xC53F, 0xACFF,
    0x94FF, 0x957F, 0x8E1F, 0x8EFF, 0x8FBE,
    0x8F9B, 0x8777, 0x8774, 0x8790, 0x9F90,
    0xB7AF, 0xD7AF, 0xF7CF, 0xFF0F, 0xFE2F,
};
static const char *const COLOR_NAME[COLOR_N] = {
    "RED", "AMBER", "PEACH", "CORAL", "ROSE", "PINK", "ORCHID", "MAUVE",
    "PURPLE", "VIOLET", "LILAC", "IRIS", "BLUE", "DENIM", "SKY",
    "OCEAN", "CYAN", "AQUA", "MINT", "JADE", "GREEN", "LEAF",
    "LIME", "PEAR", "LEMON", "GOLD", "MANGO",
};
static uint8_t p_color;                         /* the COLOR step (0: RED) */
static uint16_t cd_shade(uint16_t c, uint32_t pct)      /* an RGB565 colour x pct / 100 */
{
    uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    return (uint16_t)((r * pct / 100u) << 11 | (g * pct / 100u) << 5 | (b * pct / 100u));
}
static uint16_t cd_lighten(uint16_t c)                  /* an RGB565 colour half way to white */
{
    uint32_t r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
    return (uint16_t)((r + (31u - r) / 2u) << 11 | (g + (63u - g) / 2u) << 5 | (b + (31u - b) / 2u));
}
static void color_apply(uint32_t i)                     /* the COLOR step onto gfx.c's pal[] (ui.c's tokens follow) */
{
    uint16_t a;
    if (i >= COLOR_N) i = 0;
    p_color = (uint8_t)i;
    if (i < 2u) { palette_set(tx_palette_index(i ? "AMBER" : "RED")); return; }
    a = COLOR_ACC[i];
    pal[0] = cd_shade(a, 25); pal[1] = cd_shade(a, 45); pal[2] = cd_shade(a, 70); pal[3] = a; pal[4] = cd_lighten(a);
}

/* mod.c (included after this file) */
static int mod_valid(uint32_t id);
static const char *mod_lab(uint32_t id);
static int32_t mod_hi(uint32_t id);
static int32_t mod_get(uint32_t id);
static void mod_set(uint32_t id, int32_t v);
static const char *mod_text(uint32_t id, char *t);
static int mod_choice(uint32_t id);
static int mod_holds(uint32_t id);
/* tempo.c (included after this file) */
static const char *tempo_div_name(uint32_t g);
static uint32_t tempo_bpm10(void);
static int tempo_set10(uint32_t b);
static int tempo_tap(void);
static int tm_running(void);
static int tempo_transport(int on);
static void tempo_mode(uint32_t m);
static void tempo_port(int src, int on);
static void tempo_gsync(uint32_t g);
static void tempo_status(char *t);
enum { TG_GSYNC, TG_HALF, TG_CLOCK, TG_INUSB, TG_INTRS, TG_OUT, TG_SCNTEMPO };
static void tempo_out(uint32_t on);
static void tempo_scene_flag(uint32_t on);
static uint32_t tm_get(uint32_t what);
/* the globals that are not an effect's: the tempo's and the clock's (tempo.c holds their values) */
typedef struct { uint8_t idx; const char *lab; uint16_t hi; uint8_t fmt; } p_gx_t;
static const p_gx_t PGX[] = {
    {PG_GSYNC, "SYNC", 14, F_DIV}, {PG_BPM, "BPM", 200, F_BPM}, {PG_FINE, "FINE", 2000, F_BPM10},
    {PG_STATUS, "STATUS", 0, F_STATUS}, {PG_RUN, "TRANSPORT", 1, F_RUN}, {PG_TAP, "TAP", 127, F_NUM},
    {PG_CLOCK, "CLOCK", 3, F_ENUM}, {PG_INUSB, "CLK IN USB", 1, F_ONOFF}, {PG_INTRS, "CLK IN TRS", 1, F_ONOFF},
    {PG_OUT, "CLK OUT", 1, F_ENUM}, {PG_SCNTEMPO, "SCENE TEMPO", 1, F_ONOFF}, {PG_COLOR, "COLOR", COLOR_N - 1u, F_ENUM},
};
static const p_gx_t *p_gx(uint32_t id)
{
    uint32_t i;
    if (P_SCOPE(id) != P_GLOBAL) return 0;
    for (i = 0; i < sizeof PGX / sizeof PGX[0]; i++) if (PGX[i].idx == P_IDX(id)) return &PGX[i];
    return 0;
}

/* the effects' values (the lanes' drive and filter, the grain delay, the reverb) */
static uint8_t pv_lane[P_LANES][PL_NUM];
static uint8_t pv_glob[PG_N_];

/* ---- the cards of each lane's sound, resolved when its sound changes */
typedef struct {
    uint8_t n;
    const char *lab[SCD_MAX];
    uint8_t kind[SCD_MAX];                      /* CK_SRC (the lane engine's own), CK_HOST (the hosted engine's) | CK_INV */
    uint8_t id[SCD_MAX], hi[SCD_MAX], def[SCD_MAX];
    const tx_engine_t *eng, *hosted;            /* the lane's engine; the engine the source hosts for this sound */
    const tx_engine_t *live;                    /* the engine the source hosted when these were resolved */
    int8_t snd;
    uint8_t fam;
    uint8_t cv[SCD_MAX];                        /* the card's value as set (0xFF: never set: the engine's own) */
} p_cards_t;
static p_cards_t pc[P_LANES];

static int p_quick_param(const tx_engine_t *e, const char *name)   /* a parameter the classic engine's map moves */
{
    const ls_quick_t *q = sc_quick(e);
    uint32_t i;
    for (i = 0; q && i < q->np; i++) if (sc_eq(q->p[i].name, name)) return 1;
    return 0;
}
static void p_cards_resolve(uint32_t k)
{
    p_cards_t *c = &pc[k];
    const tx_engine_t *e = sc.eng[k];
    uint32_t i;
    c->n = 0; c->eng = e; c->hosted = 0; c->snd = sc.snd[k]; c->fam = sc.fam; c->live = ln_hosted(k);
    for (i = 0; i < SCD_MAX; i++) c->cv[i] = 0xFF;
    if (!e) return;
    if (e != sc.src) {                          /* a classic engine: its own parameters, less its activity map's */
        const scd_sound_t *d = 0;
        for (i = 0; i < SCD_N; i++) if (sc.snd[k] >= 0 && sc_eq(SCD[i].sound, sc_sound_name(k))) d = &SCD[i];
        for (i = 0; d && i < d->n; i++) {      /* listed: those cards, in that order (as the engine has them) */
            int id = tx_param_find(e, d->c[i].par);
            if (id < 0 || p_quick_param(e, e->params[id].name)) continue;
            c->lab[c->n] = d->c[i].lab; c->kind[c->n] = CK_SRC; c->id[c->n] = (uint8_t)id;
            c->hi[c->n] = e->params[id].max; c->def[c->n] = e->params[id].def; c->n++;
        }
        for (i = 0; !d && i < e->n_params && c->n < SCD_MAX; i++) {
            if (p_quick_param(e, e->params[i].name)) continue;
            c->lab[c->n] = e->params[i].name; c->kind[c->n] = CK_SRC; c->id[c->n] = (uint8_t)i;
            c->hi[c->n] = e->params[i].max; c->def[c->n] = e->params[i].def; c->n++;
        }
        return;
    }
    {
        const scd_sound_t *d = &SC_SRC_DEFAULT;
        for (i = 0; i < SCD_N; i++) if (sc.snd[k] >= 0 && sc_eq(SCD[i].sound, sc_sound_name(k))) d = &SCD[i];
        if (d->hosted) c->hosted = tx_engine_find(d->hosted);
        if (c->hosted != c->live) { d = &SC_SRC_DEFAULT; c->hosted = 0; }   /* (the source plays it another way: its own
                                                                      * general cards) */
        for (i = 0; i < d->n; i++) {
            const tx_engine_t *t = d->c[i].kind & CK_HOST ? c->hosted : e;
            int id = t ? tx_param_find(t, d->c[i].par) : -1;
            if (id < 0) continue;               /* (a parameter this build's engine has not got) */
            c->lab[c->n] = d->c[i].lab; c->kind[c->n] = d->c[i].kind; c->id[c->n] = (uint8_t)id;
            c->hi[c->n] = t->params[id].max; c->def[c->n] = t->params[id].def;
            if (d->c[i].kind & CK_INV) c->def[c->n] = (uint8_t)(c->hi[c->n] - c->def[c->n]);
            c->n++;
        }
    }
}
/* the lane's cards, up to date with its sound; a new sound: the cards the last one set on the source go back to the
 * source's defaults (a hosted engine's are cleared by the source itself when PRESET / TYPE change) */
static p_cards_t *p_cards(uint32_t k)
{
    p_cards_t *c = &pc[k];
    if (c->eng != sc.eng[k] || c->snd != sc.snd[k] || c->fam != sc.fam || (sc.eng[k] == sc.src && c->live != ln_hosted(k))) {
        uint32_t i;
        if (c->eng == sc.src && sc.eng[k] == sc.src)
            for (i = 0; i < c->n; i++)
                if (!(c->kind[i] & CK_HOST) && c->cv[i] != 0xFF) ln_param(k, c->id[i], c->kind[i] & CK_INV ? c->hi[i] - c->def[i] : c->def[i]);
        p_cards_resolve(k);
    }
    return c;
}
static int32_t p_card_get(uint32_t k, uint32_t i)
{
    p_cards_t *c = p_cards(k);
    int32_t v;
    if (i >= c->n) return 0;
    if (c->cv[i] != 0xFF) return c->cv[i];
    if (c->kind[i] & CK_HOST) {                 /* the hosted engine's value now (the source's curves set it) */
        v = ln_host_value(k, sc.src, c->hosted, c->id[i]);
        if (v >= 0) return c->kind[i] & CK_INV ? c->hi[i] - v : v;
    }
    return c->def[i];
}
static void p_card_set(uint32_t k, uint32_t i, uint32_t v)
{
    p_cards_t *c = p_cards(k);
    uint32_t x;
    if (i >= c->n) return;
    if (v > c->hi[i]) v = c->hi[i];
    c->cv[i] = (uint8_t)v;
    if (mod_holds(PID(k, PL_CARD0 + i))) return;   /* (modulated: the base only; mod.c sets it) */
    x = c->kind[i] & CK_INV ? c->hi[i] - v : v;
    if (c->kind[i] & CK_HOST) ln_host(k, c->id[i], x, 0);
    else ln_param(k, c->id[i], x);
}

/* ---- the table */
static const tx_param_t *p_fx_desc(uint32_t id)   /* an effect's descriptor (0: not an effect) */
{
    uint32_t s = P_SCOPE(id), i = P_IDX(id);
    if (s < P_LANES) {
        if (i >= PL_DRIVE0 && i < PL_DRIVE0 + TX_DR_NP) return &tx_fx_drive_params[i - PL_DRIVE0];
        if (i >= PL_FILTER0 && i < PL_FILTER0 + TX_FL_NP) return &tx_fx_filter_params[i - PL_FILTER0];
    } else if (s == P_GLOBAL) {
        if (i < PG_GRAIN0 + TX_GD_NP) return &tx_fx_grain_params[i - PG_GRAIN0];
        if (i >= PG_REVERB0 && i < PG_REVERB0 + TX_RV_NP) return &tx_fx_reverb_params[i - PG_REVERB0];
        if (i >= PG_COMP0 && i < PG_COMP0 + TX_CP_NP) return &tx_comp_params[i - PG_COMP0];
    }
    return 0;
}
static int p_valid(uint32_t id)
{
    uint32_t s = P_SCOPE(id), i = P_IDX(id);
    if (p_fx_desc(id) || p_gx(id)) return 1;
    if (P_MODS(s)) return mod_valid(id);
    if (s >= P_LANES) return 0;
    if (i <= PL_PAN) return 1;
    return i >= PL_CARD0 && i < PL_CARD0 + SCD_MAX && i - PL_CARD0 < p_cards(s)->n;
}
static const char *p_lab(uint32_t id)
{
    const tx_param_t *d = p_fx_desc(id);
    uint32_t s = P_SCOPE(id), i = P_IDX(id);
    static const char *const L[] = {"SOUND", "HOW MANY", "HOW ACTIVE", "LEVEL", "PAN"};
    if (d) return d->name;
    if (p_gx(id)) return p_gx(id)->lab;
    if (P_MODS(s) && mod_valid(id)) return mod_lab(id);
    if (s < P_LANES && i <= PL_PAN) return L[i];
    if (s < P_LANES && i >= PL_CARD0 && i - PL_CARD0 < p_cards(s)->n) return p_cards(s)->lab[i - PL_CARD0];
    return "";
}
static int32_t p_hi(uint32_t id)
{
    const tx_param_t *d = p_fx_desc(id);
    uint32_t s = P_SCOPE(id), i = P_IDX(id);
    if (d) return d->max;
    if (p_gx(id)) return p_gx(id)->hi;
    if (P_MODS(s)) return mod_valid(id) ? mod_hi(id) : 0;
    if (s >= P_LANES) return 0;
    if (i == PL_SOUND) return LS[sc.fam].n - 1;
    if (i == PL_COUNT) return LS_NCNT - 1;
    if (i == PL_ACTIVE) return LS_NACT;
    if (i == PL_LEVEL || i == PL_PAN) return 127;
    if (i >= PL_CARD0 && i - PL_CARD0 < p_cards(s)->n) return p_cards(s)->hi[i - PL_CARD0];
    return 0;
}
static uint32_t p_fmt(uint32_t id)
{
    uint32_t s = P_SCOPE(id), i = P_IDX(id);
    if (P_MODS(s)) return mod_choice(id) ? F_ENUM : F_NUM;
    if (s < P_LANES) {
        if (i == PL_SOUND) return F_SOUND;
        if (i == PL_COUNT) return F_COUNT;
        if (i == PL_ACTIVE) return F_ACTIVE;
        if (i == PL_PAN) return F_PAN;
        if (i == PL_DRIVE0 + TX_DR_TYPE || i == PL_FILTER0 + TX_FL_TYPE || i == PL_FILTER0 + TX_FL_SHAPE) return F_ENUM;
        if (i == PL_FILTER0 + TX_FL_FREQ) return F_HZ_FL;
        if (i == PL_FILTER0 + TX_FL_RATE) return F_HZ_LFO;
    } else if (s == P_GLOBAL) {
        if (p_gx(id)) return p_gx(id)->fmt;
        if (i == PG_GRAIN0 + TX_GD_FREEZE) return F_ONOFF;
        if (i == PG_GRAIN0 + TX_GD_TIME) return F_MS_GD;
        if (i == PG_GRAIN0 + TX_GD_SIZE) return F_MS_GS;
        if (i == PG_GRAIN0 + TX_GD_PITCH) return F_SEMI;
        if (i == PG_GRAIN0 + TX_GD_GRAINS) return F_PLUS1;
        if (i == PG_REVERB0 + TX_RV_TYPE || i == PG_REVERB0 + TX_RV_SHIFT) return F_ENUM;
        if (i == PG_REVERB0 + TX_RV_PREDELAY) return F_MS_PD;
        if (i == PG_COMP0 + TX_CP_ON) return F_ONOFF;           /* the COMPRESSOR: Frodhes' formats */
        if (i == PG_COMP0 + TX_CP_THR) return F_THR;
        if (i == PG_COMP0 + TX_CP_ATK || i == PG_COMP0 + TX_CP_REL) return F_R10;
        if (i == PG_COMP0 + TX_CP_RATIO) return F_ENUM;
        if (i == PG_COMP0 + TX_CP_MKUP) return F_GAIN;
    }
    return F_NUM;
}
static const char *const *p_names(uint32_t id)
{
    uint32_t i = P_IDX(id);
    if (P_SCOPE(id) < P_LANES) return i == PL_DRIVE0 + TX_DR_TYPE ? N_DRIVE : i == PL_FILTER0 + TX_FL_TYPE ? N_FILTER : N_SHAPE;
    if (i == PG_COMP0 + TX_CP_RATIO) return N_RATIO;
    return i == PG_REVERB0 + TX_RV_TYPE ? N_REVERB : i == PG_CLOCK ? N_CLOCK : i == PG_OUT ? N_CLKOUT : i == PG_COLOR ? COLOR_NAME : N_SHIFT;
}
static int32_t p_get(uint32_t id)
{
    uint32_t s = P_SCOPE(id), i = P_IDX(id);
    if (P_MODS(s)) return mod_valid(id) ? mod_get(id) : 0;
    if (s == P_GLOBAL) {
        switch (i) {
        case PG_GSYNC: return (int32_t)tm_get(TG_GSYNC);
        case PG_BPM: return (int32_t)((tempo_bpm10() + 5u) / 10u) - 40;
        case PG_FINE: return (int32_t)tempo_bpm10() - 400;
        case PG_RUN: return tm_running();
        case PG_CLOCK: return (int32_t)tm_get(TG_CLOCK);
        case PG_INUSB: return (int32_t)tm_get(TG_INUSB);
        case PG_INTRS: return (int32_t)tm_get(TG_INTRS);
        case PG_OUT: return (int32_t)tm_get(TG_OUT);
        case PG_SCNTEMPO: return (int32_t)tm_get(TG_SCNTEMPO);
        case PG_COLOR: return p_color;
        }
        return i < PG_N_ ? pv_glob[i] : 0;
    }
    if (s >= P_LANES) return 0;
    switch (i) {
    case PL_SOUND: return sc.snd[s] < 0 ? 0 : sc.snd[s];
    case PL_COUNT: return (int32_t)sc_cnt_of(s);
    case PL_ACTIVE: return sc.run[s] ? (int32_t)sc_act_of(sc.step[s]) + 1 : 0;
    case PL_LEVEL: return (int32_t)sc_vol_of(s);
    case PL_PAN: return sc.pan[s];
    }
    if (i >= PL_CARD0 && i < PL_CARD0 + SCD_MAX) return p_card_get(s, i - PL_CARD0);
    return i < PL_NUM ? pv_lane[s][i] : 0;
}
static void p_set(uint32_t id, int32_t v)
{
    uint32_t s = P_SCOPE(id), i = P_IDX(id);
    int32_t hi = p_hi(id);
    if (!p_valid(id)) return;
    v = v < 0 ? 0 : v > hi ? hi : v;
    if (P_MODS(s)) { mod_set(id, v); return; }
    if (s == P_GLOBAL && p_gx(id)) {        /* the tempo's and the clock's */
        switch (i) {
        case PG_GSYNC: tempo_gsync((uint32_t)v); break;
        case PG_BPM: tempo_set10((uint32_t)(v + 40) * 10u); break;
        case PG_FINE: tempo_set10((uint32_t)v + 400u); break;
        case PG_RUN: tempo_transport(v); break;
        case PG_TAP: if (v > 63) tempo_tap(); break;
        case PG_CLOCK: tempo_mode((uint32_t)v); break;
        case PG_INUSB: tempo_port(1, v); break;
        case PG_INTRS: tempo_port(2, v); break;
        case PG_OUT: tempo_out((uint32_t)v); break;
        case PG_SCNTEMPO: tempo_scene_flag((uint32_t)v); break;
        case PG_COLOR: color_apply((uint32_t)v); break;
        }
        return;
    }
    if (s == P_GLOBAL) {
        pv_glob[i] = (uint8_t)v;
        if (mod_holds(id)) return;              /* (modulated: the base only) */
        if (i >= PG_COMP0) ln_fx(LN_FX_COMP, 0, i - PG_COMP0, (uint32_t)v);
        else if (i < PG_REVERB0) ln_fx(LN_FX_GRAIN, 0, i - PG_GRAIN0, (uint32_t)v);
        else ln_fx(LN_FX_REVERB, 0, i - PG_REVERB0, (uint32_t)v);
        return;
    }
    switch (i) {
    case PL_SOUND: sc_sound_to(s, (uint32_t)v); return;
    case PL_COUNT: sc_count(s, (uint32_t)v); return;
    case PL_ACTIVE: if (!v) sc_count(s, 0); else sc_step(s, sc_act_step((uint32_t)v - 1u)); return;
    case PL_LEVEL: sc_volume(s, (uint32_t)v); return;
    case PL_PAN: sc_pan(s, (uint32_t)v); return;
    }
    if (i >= PL_CARD0 && i < PL_CARD0 + SCD_MAX) { p_card_set(s, i - PL_CARD0, (uint32_t)v); return; }
    pv_lane[s][i] = (uint8_t)v;
    if (mod_holds(id)) return;
    if (i >= PL_FILTER0) ln_fx(LN_FX_FILTER, s, i - PL_FILTER0, (uint32_t)v);
    else ln_fx(LN_FX_DRIVE, s, i - PL_DRIVE0, (uint32_t)v);
}
/* a value as the screen writes it, and its unit apart (0: none) */
static const char *p_text(uint32_t id, char *t)
{
    int32_t v = p_get(id);
    uint32_t f = p_fmt(id);
    t[0] = 0;
    if (P_MODS(P_SCOPE(id))) return mod_text(id, t);
    switch (f) {
    case F_ENUM: p_strcpy(t, p_names(id)[v], 10); return 0;
    case F_ONOFF: p_strcpy(t, v ? "ON" : "OFF", 4); return 0;
    case F_SOUND: p_upper(t, sc_sound_name(P_SCOPE(id)), 22); return 0;
    case F_COUNT: if (!v) p_strcpy(t, "OFF", 4); else p_fmt_int(t, 1 << (v - 1)); return 0;
    case F_ACTIVE: if (!v) p_strcpy(t, "OFF", 4); else { p_fmt_int(t, v); p_cat(t, "/8", 15); } return 0;
    case F_PAN:
        if (v == 64) { p_strcpy(t, "C", 2); return 0; }
        t[0] = v < 64 ? 'L' : 'R'; p_fmt_int(t + 1, v < 64 ? 64 - v : v - 64); return 0;
    case F_HZ_FL: {                             /* 30 Hz .. 16 kHz, log */
        uint32_t hz = (30u * tx_exp2((int32_t)(16 << 16) + v * 593700 / 127)) >> 16;
        if (hz >= 1000u) { p_fmt_int(t, (int32_t)(hz / 1000u)); p_cat(t, ".", 15); { char d[2] = {(char)('0' + hz / 100u % 10u), 0}; p_cat(t, d, 15); } return "kHz"; }
        p_fmt_int(t, (int32_t)hz); return "Hz";
    }
    case F_HZ_LFO: {                            /* tenths of a Hz from the LFO's phase step */
        uint32_t h10 = (((TX_FX_LFO_INC[v] >> 10) * 441000u) + (1u << 21)) >> 22;
        char d[2] = {(char)('0' + h10 % 10u), 0};
        p_fmt_int(t, (int32_t)(h10 / 10u)); p_cat(t, ".", 15); p_cat(t, d, 15); return "Hz";
    }
    case F_MS_GD:                               /* (SYNC set: the division instead) */
        if (tm_get(TG_GSYNC)) { p_strcpy(t, tm_get(TG_HALF) ? "~" : "", 1); p_cat(t, tempo_div_name(tm_get(TG_GSYNC)), 15); return 0; }
        p_fmt_int(t, (int32_t)((TX_FX_GD_TIME[v] * 10u + 220u) / 441u)); return "ms";
    case F_DIV: p_strcpy(t, tempo_div_name((uint32_t)v), 10); return 0;
    case F_BPM: p_fmt_int(t, v + 40); return "BPM";
    case F_BPM10: { char d[2] = {(char)('0' + (v + 400) % 10), 0}; p_fmt_int(t, (v + 400) / 10); p_cat(t, ".", 15); p_cat(t, d, 15); return "BPM"; }
    case F_STATUS: tempo_status(t); return 0;
    case F_RUN: p_strcpy(t, v ? "RUN" : "STOP", 4); return 0;
    case F_MS_GS: p_fmt_int(t, (int32_t)(((0xFFFFFFFFu / TX_FX_GD_SIZE[v]) * 10u + 220u) / 441u)); return "ms";
    case F_MS_PD: p_fmt_int(t, (v * 80 + 63) / 127); return "ms";
    case F_SEMI: { int32_t st = (v - 64) * 12 / 63; if (st > 0) { t[0] = '+'; p_fmt_int(t + 1, st); } else p_fmt_int(t, st); return "st"; }
    case F_PLUS1: p_fmt_int(t, v + 1); return 0;
    case F_THR: {                               /* "-20": 0 .. -40 dBFS (Frodhes' THR) */
        int32_t d = (v * 400 + 635) / 1270;
        if (d) { t[0] = '-'; p_fmt_int(t + 1, d); } else p_strcpy(t, "0", 1);
        return "dB";
    }
    case F_R10: {                               /* the panel's 0.0 .. 10.0 (Frodhes' ATK, REL) */
        int32_t r = (v * 100 + 63) / 127;
        char d[2] = {(char)('0' + r % 10), 0};
        p_fmt_int(t, r / 10); p_cat(t, ".", 15); p_cat(t, d, 15); return 0;
    }
    case F_GAIN: {                              /* "+2.6", from 10 dB on whole dB: "+12" (Frodhes' MKUP: 0 .. +24 dB) */
        int32_t g10 = (v * 240 + 63) / 127;
        t[0] = '+';
        if (g10 >= 100) p_fmt_int(t + 1, (g10 + 5) / 10);
        else { char d[2] = {(char)('0' + g10 % 10), 0}; p_fmt_int(t + 1, g10 / 10); p_cat(t, ".", 15); p_cat(t, d, 15); }
        return "dB";
    }
    }
    p_fmt_int(t, v);
    return 0;
}
/* a card's look: an amount (a fill), a choice (n options: segments), a toggle (ON / OFF) */
enum { PK_AMOUNT, PK_SELECT, PK_TOGGLE };
static uint32_t p_kind(uint32_t id)
{
    uint32_t f = p_fmt(id);
    if (f == F_ONOFF || f == F_RUN) return PK_TOGGLE;
    if (f == F_ENUM || f == F_DIV || f == F_SOUND || f == F_COUNT || f == F_ACTIVE) return PK_SELECT;
    return PK_AMOUNT;
}

static void p_init(void)
{
    uint32_t k, i;
    for (k = 0; k < P_LANES; k++) {
        for (i = 0; i < TX_DR_NP; i++) pv_lane[k][PL_DRIVE0 + i] = tx_fx_drive_params[i].def;
        for (i = 0; i < TX_FL_NP; i++) pv_lane[k][PL_FILTER0 + i] = tx_fx_filter_params[i].def;
        pc[k].snd = -2;                         /* (resolved at the first look) */
    }
    for (i = 0; i < TX_GD_NP; i++) pv_glob[PG_GRAIN0 + i] = tx_fx_grain_params[i].def;
    for (i = 0; i < TX_RV_NP; i++) pv_glob[PG_REVERB0 + i] = tx_fx_reverb_params[i].def;
    for (i = 0; i < TX_CP_NP; i++) {            /* (the mix starts with it off: the defaults sent here) */
        pv_glob[PG_COMP0 + i] = tx_comp_params[i].def;
        ln_fx(LN_FX_COMP, 0, i, pv_glob[PG_COMP0 + i]);
    }
}

/* ---- MIDI: CC numbers (the lane's channel: P_CC_LANE; channel 5: P_CC_GLOBAL), NRPN */
typedef struct { uint8_t cc, idx; } p_cc_t;
static const p_cc_t P_CC_LANE[] = {
    {7, PL_LEVEL}, {10, PL_PAN}, {20, PL_SOUND}, {21, PL_COUNT}, {22, PL_ACTIVE},
    {14, PL_CARD0}, {15, PL_CARD0 + 1}, {16, PL_CARD0 + 2}, {17, PL_CARD0 + 3}, {18, PL_CARD0 + 4}, {19, PL_CARD0 + 5},
    {23, PL_CARD0 + 6}, {24, PL_CARD0 + 7},
    {76, PL_DRIVE0 + TX_DR_AMOUNT}, {77, PL_DRIVE0 + TX_DR_TONE}, {78, PL_DRIVE0 + TX_DR_MIX}, {79, PL_DRIVE0 + TX_DR_TYPE},
    {102, PL_FILTER0 + TX_FL_TYPE}, {74, PL_FILTER0 + TX_FL_FREQ}, {71, PL_FILTER0 + TX_FL_RESO},
    {103, PL_FILTER0 + TX_FL_LFO}, {104, PL_FILTER0 + TX_FL_RATE}, {105, PL_FILTER0 + TX_FL_SHAPE},
};
static const p_cc_t P_CC_GLOBAL[] = {
    {102, PG_GRAIN0 + TX_GD_MIX}, {103, PG_GRAIN0 + TX_GD_TIME}, {104, PG_GRAIN0 + TX_GD_FEEDBACK},
    {105, PG_GRAIN0 + TX_GD_GRAINS}, {106, PG_GRAIN0 + TX_GD_SIZE}, {107, PG_GRAIN0 + TX_GD_SPRAY},
    {108, PG_GRAIN0 + TX_GD_PITCH}, {109, PG_GRAIN0 + TX_GD_REVERSE}, {110, PG_GRAIN0 + TX_GD_SPREAD},
    {111, PG_GRAIN0 + TX_GD_TONE}, {112, PG_GRAIN0 + TX_GD_GRAIN}, {113, PG_GRAIN0 + TX_GD_FREEZE},
    {85, PG_REVERB0 + TX_RV_TYPE}, {91, PG_REVERB0 + TX_RV_MIX}, {86, PG_REVERB0 + TX_RV_DECAY},
    {87, PG_REVERB0 + TX_RV_TONE}, {88, PG_REVERB0 + TX_RV_PREDELAY}, {89, PG_REVERB0 + TX_RV_SIZE},
    {90, PG_REVERB0 + TX_RV_MOD}, {92, PG_REVERB0 + TX_RV_LOWCUT}, {93, PG_REVERB0 + TX_RV_SHIMMER},
    {94, PG_REVERB0 + TX_RV_SHIFT}, {114, PG_GSYNC},
    {95, PG_COMP0 + TX_CP_ON}, {115, PG_COMP0 + TX_CP_THR}, {116, PG_COMP0 + TX_CP_ATK}, {117, PG_COMP0 + TX_CP_REL},
    {118, PG_COMP0 + TX_CP_RATIO}, {119, PG_COMP0 + TX_CP_MKUP},
    {3, PG_BPM}, {9, PG_TAP}, {80, PG_RUN},
};
/* a 7-bit value onto a parameter's range (0 .. hi): 127 always reaches hi */
static int32_t p_scale7(uint32_t id, uint32_t v)
{
    int32_t hi = p_hi(id);
    if (P_SCOPE(id) == P_MAT && P_IDX(id) < 64u && P_IDX(id) % 4u == 2u)   /* a slot's DEPTH: 64 = 0 % */
        return v <= 64u ? ((int32_t)v * 100 + 32) / 64 : 100 + (((int32_t)v - 64) * 100 + 31) / 63;
    return hi == 127 ? (int32_t)v : ((int32_t)v * hi + 63) / 127;
}
static struct { uint8_t msb[5], lsb[5], on[5]; } p_nrpn;
static uint32_t p_touched;                      /* (the screen: a parameter MIDI changed, id + 1) */
static void p_midi_set(uint32_t id, int32_t v) { if (p_valid(id)) { p_set(id, v); p_touched = id + 1u; } }
/* a control change on channel ch (0-based): 1 if it was one of ours */
static int p_cc(uint32_t ch, uint32_t cc, uint32_t v)
{
    uint32_t i, scope = ch < P_LANES ? ch : ch == P_CH_GLOBAL ? P_GLOBAL : 0xFFu;
    const p_cc_t *t = scope == P_GLOBAL ? P_CC_GLOBAL : P_CC_LANE;
    uint32_t n = scope == P_GLOBAL ? sizeof P_CC_GLOBAL / sizeof P_CC_GLOBAL[0] : sizeof P_CC_LANE / sizeof P_CC_LANE[0];
    if (scope == 0xFFu) return 0;
    if (cc == 99u) { p_nrpn.msb[ch] = (uint8_t)v; p_nrpn.on[ch] = 1; return 1; }
    if (cc == 98u) { p_nrpn.lsb[ch] = (uint8_t)v; p_nrpn.on[ch] = 1; return 1; }
    if (cc == 101u || cc == 100u) { p_nrpn.on[ch] = 0; return 1; }   /* (RPN: not ours) */
    if ((cc == 6u || cc == 38u) && p_nrpn.on[ch]) {                  /* NRPN data: MSB 99 the scope, LSB 98 the index */
        uint32_t id = PID(p_nrpn.msb[ch], p_nrpn.lsb[ch]);
        if (cc == 6u) p_midi_set(id, p_scale7(id, v));
        return 1;
    }
    for (i = 0; i < n; i++) if (t[i].cc == cc) { uint32_t id = PID(scope, t[i].idx); p_midi_set(id, p_scale7(id, v)); return 1; }
    return 0;
}
static void p_poll(void) { uint32_t k; for (k = 0; k < P_LANES; k++) (void)p_cards(k); }   /* (a lane's new sound: its cards) */
