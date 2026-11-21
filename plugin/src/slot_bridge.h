/* SPDX-License-Identifier: GPL-3.0-only */
/* One slot's glue (plugin/src/bridge.h): included by plugin/src/engine_unit.c after the app, so the app's statics are
 * in scope; everything here is static too. From Frodhes' plugin/src/slot_bridge.h (its time model, queue, FIFO and
 * coroutine), with Textura's own parameters and state. Float is allowed in plugin/ (the app itself stays integer);
 * this file needs none.
 *
 * The time model equals the host build (tests/host.c):
 *   step:  1. ms += 1
 *          2. the queued events due by ms: MIDI into web_hal.c's 64-entry ring (a burst waits here, never dropped),
 *             parameter writes the MIDI way (params.c p_midi_set: as a CC or NRPN would set them)
 *          3. a pending state (the mailbox), once no scene load runs
 *          4. the audio owed: every 256 frames app_audio(256) into the FIFO (int32, Q15, no clip)
 *          5. one main-loop pass of app_main() (the coroutine) up to its next platform_poll()
 *          6. the host's tempo, the values published for the main thread, a state capture when asked
 * That is the ms test mode's order (bridge.h: Time). As the plugin ships (exact), an event carries its app frame
 * (sb_putx): step 2 hands over only the events for the next block to render, and step 5 runs before step 4, so they
 * act from that block's start (the lanes take their queue at each block's start, as on the FM-1).
 *
 * The app runs with SC_N lanes (16: TX_MIX_SLOTS, plugin/cmake/slots.cmake), LANES of them in use (params.c PG_NLANE:
 * 4 at the start; a sound put on a lane past it takes it up; lowered, the lanes past it are emptied). Lanes 1 .. 4 keep
 * the FM-1's ids (scopes 0 .. 3), lanes 5 .. 16 are scopes 16 .. 27 (params.c P_LS).
 *
 * The host's parameters (sb_params): the app's own, by their stable ids (params.c), the same for every lane: SOUND,
 * HOW MANY, HOW ACTIVE, LEVEL, PAN, the eight cards (0 .. 127 onto the card's range, as their CC: what a card turns
 * depends on the lane's sound), GROOVE's PART and CYCLE; the effects on the mix (GRAIN DELAY and its SYNC, REVERB,
 * COMPRESSOR, DRIVE, FILTER), GROOVE, the keys' SCALE, MIDI IN's channels (each lane's, SELECTED CH, FX CHANNEL),
 * LANES, EFFECTS ON LOAD; the eight LFOs, the envelopes, the followers, the matrix and MOD CC; and the plugin's own
 * PRESET (F001 .. F100, a program change on channel 1); then, appended last, the M/S's MID, SIDE,
 * WIDTH (SIDE against MID: set, it moves SIDE) and MONO. The FM-1's tempo, clock, screen and status settings are not
 * host parameters: the host's tempo leads (step 6) when it gives one.
 *
 * The state: what a user scene keeps (scenes.c scn_capture: the landscape, LANES, each lane's sound, HOW MANY, HOW
 * ACTIVE, level, pan, mute and cards, the effects, the modulation, GROOVE, the tempo), the scene loaded last, and the
 * settings that are parameters (the keys' SCALE, MIDI IN, EFFECTS ON LOAD, PRESETS CHANGE TEMPO). Loaded, it is
 * applied as a scene with its effects (scn_apply, scn_fx_force); outside processing the slot runs until the load is
 * done (sb_settle), so the values read back at once are the state's. Nothing is written to disk: the user scenes
 * U01 .. U16 (the window's SAVE: scenes.c scn_save_start, as the FM-1's save mode) live in the slot's flash window
 * (web_hal.c) and go with the state ("user": each saved one's record), so they stay with the project; a state loaded
 * puts its own bank back (none in it: no user scene). The plugin's SOLO (the window's S: no FM-1 has it) is in the
 * state too ("solo"): the lanes soloed play, the others are shut at the mix as a mute shuts them (lanes.c ln_gate,
 * sb_solo), and the app's own mutes stay as they are (a scene keeps those). So are the selected lane ("sel", the app's
 * own: scape.c sc.sel) and KEYS ("keys").
 *
 * KEYS (the plugin's own parameter, TXP_ID_KEYS): PLAY (the default) leaves every note to the app (notes.c nt_midi: on
 * SELECTED CH the selected lane plays it, as sent); FM-1 KEYS makes SELECTED CH's notes F3 .. G5 (53 .. 79) the FM-1's
 * 27 keys in LANDSCAPE mode (web_hal.c fm1_in.notes, read by notes.c nt_poll: the black keys' groups select lanes
 * 1 .. 4, held 600 ms they mute it, F3 .. E4 HOW MANY, F4 .. G5 HOW ACTIVE), and the black keys above G5 (G#5, A#5,
 * C#6 .. A#7: twelve) select lanes 5 .. 16 (sb_keys).
 *
 * The window (plugin/gui_core.c) reads what each lane plays (txp_ui_snap_t, published with the values) and the lanes'
 * activity: after every app block each lane's engine events and peak (lanes.c ln.lev, ln.lpk) go into a ring of
 * TXP_ACT_N blocks (sb_act). With the values the M/S's meters are published (its peaks and correlation, kept by the
 * mix for the window always: tx_core_ms.h meter bit 1) and the output's last block for the window's goniometer.
 *
 * The sound browser's preview (sb_pv_*) is the window's audition, not an edit: while it plays, the host reads the
 * lane's HOW MANY and HOW ACTIVE as they were before it (no automation of the preview's own) and a state captured
 * then keeps the lane as it was (its sound, HOW MANY, HOW ACTIVE, playing or not, its mute, its cards). */
#include <math.h>
#include <stdatomic.h>
#include <time.h>
#include "coro/coro.h"

#define SB_STACK (256u * 1024u)          /* the coroutine's stack (app_main and what it calls) */
#define SB_FIFO 2048u                    /* frames (power of two); the instance never asks more than 1024 ahead */
#define SB_Q 4096u                       /* the event queue (power of two) */
#define SB_HOST_RING 64u                 /* web_hal.c's mq[] */
#define SB_PUB_EVERY 8u                  /* steps between two publications of the values */
#define SB_SETTLE_MS 6000u               /* sb_settle's longest run (scenes.c SCN_STUCK_MS) */
#define SB_NSET 24u                      /* the settings in the state */
#define SB_SCN_HEX (2u * sizeof(scn_t))
#define SB_SCN_V1_HEX (2u * sizeof(scn_v1_t))   /* (a state of before: the FM-1's four-lane scene record) */
#define SB_USER_HEX (SCN_NU * 2u * sizeof(scn_t))   /* the user scenes, all sixteen saved */
_Static_assert(SB_SCN_HEX + SB_USER_HEX + 2048u < TXP_STATE_MAX, "the state fits bridge.h TXP_STATE_MAX");
#define SB_KEY0 53u                      /* KEYS: F3, the FM-1's first key (notes.c) */

typedef struct { uint8_t kind, x; uint32_t at, a; int32_t v; } sb_ev_t;   /* x: at is an app frame x 256 (exact) */
static const uint8_t SB_SET_IDX[SB_NSET] = { PG_SCNFX, PG_SCNTEMPO, PG_SCMODE, PG_SCALE, PG_ROOT, PG_TRANS, PG_SELCH,
                                             PG_LCH0, PG_LCH0 + 1, PG_LCH0 + 2, PG_LCH0 + 3,
                                             PG_LCH4, PG_LCH4 + 1, PG_LCH4 + 2, PG_LCH4 + 3, PG_LCH4 + 4, PG_LCH4 + 5,
                                             PG_LCH4 + 6, PG_LCH4 + 7, PG_LCH4 + 8, PG_LCH4 + 9, PG_LCH4 + 10,
                                             PG_LCH4 + 11, PG_CHG };
typedef char sb_set_lanes[SC_N == 16u ? 1 : -1];   /* (SB_SET_IDX names sixteen lanes' channels) */

static struct {
    coro_t co;
    void *stack;
    int booted;
    uint32_t ms, owed;
    int ev_wait;                         /* sb_events left host events waiting for room (the next step goes on) */
    int exact;                           /* the step's order (bridge.h: Time): 1 exact, 0 ms */
    int32_t fifo[2u * SB_FIFO];
    uint32_t fr, fw;
    sb_ev_t q[SB_Q];
    _Atomic uint32_t qw, qr;             /* one producer and one consumer, never at once (bridge.h: threads) */
    _Atomic int st_flag;                 /* the state mailbox: 0 free, 1 being written, 2 ready, 3 being applied */
    scn_t st_scn, ap_scn;                /* the mailbox's scene; the one a load runs from (scn_ap points at it) */
    uint8_t st_cur;
    int16_t st_set[SB_NSET];             /* -1: not in the state (stays) */
    _Atomic uint32_t pub_seq;            /* the published copy (odd: being written) */
    txp_snap_t pub;
    _Atomic uint32_t cap_seq;            /* the state's capture (odd: being written) */
    _Atomic int cap_req;                 /* the main thread asks for a capture (the next step makes it) */
    scn_t cap_scn;
    uint8_t cap_cur;
    int16_t cap_set[SB_NSET];
    uint32_t rev, pub_rev, pub_n;
    uint8_t last_fact;                   /* the last factory scene (PRESET's value) */
    _Atomic uint32_t tempo10;
    txp_stats_t st;
    txp_param_t tab[TXP_NPARAM_MAX];
    uint32_t ntab;
    char boot_state[TXP_STATE_MAX];
    /* the SOLO (sb_solo): the mask, whether it was on at the last pass, the gates as sb_solo last set them and the
     * app's own last gate (scape.c sc.a_gate) seen then */
    uint16_t solo, cap_solo;
    int32_t st_solo;                     /* (the state mailbox's: -1 none in it) */
    uint8_t solo_was, gopen[SC_N], agseen[SC_N];
    /* the activity ring (sb_act): one entry an app block */
    uint8_t act_ev[TXP_ACT_N][TXP_LANES], act_pk[TXP_ACT_N][TXP_LANES];
    _Atomic uint32_t act_n;
    /* KEYS (sb_keys): the mode, the FM-1 keys held (web_hal.c fm1_in.notes), those let go (cleared after a pass) */
    uint8_t keys, cap_keys;
    int8_t st_keys;                      /* (the mailbox's: -1 none in it) */
    uint32_t kmask, koff;
    /* the selected lane in the state (the mailbox's: -1 none in it) */
    uint8_t cap_sel;
    int8_t st_sel;
    /* the user scenes in the state: the mailbox's (st_umask -1: no "user" in it) and the capture's */
    int32_t st_umask;
    uint16_t cap_umask;
    scn_t st_user[SCN_NU], cap_user[SCN_NU];
    char save_nm[12];                    /* TXP_EV_NAME: the next save's name */
    /* the output's peak since the last publication (Q15), the texts' values and their refresh count (sb_publish) */
    int32_t opk;
    int16_t odb10;                       /* (the output's peak published last: 1000 + dB x 10; 0 none yet) */
    uint32_t opk_blk;
    /* the compressor's meters since the last publication: its detector's power (its input, Q26 full scale), that
     * through its gain (its output), its gain reduction (power log2 Q12); published last (dB x 10, as txp_ui_snap_t) */
    float cin_pk, cout_pk;
    int32_t cgr_pk;
    int16_t cin10, cout10, cgr10;
    /* the goniometer's points: the last block's frames (every TXP_GON_STEP-th, Q15) and that block's count */
    int16_t gon[TXP_GON_N][2];
    uint32_t gon_blk;
    const scn_t *ap_seen;                /* the scene applied last that sb_scene_mem saw (and when) */
    uint32_t ap_seen_t;
    int16_t ptx_v[TXP_NPARAM_MAX];
    uint32_t ptx_n;
} sbx;
typedef char sb_ui_lanes[SC_N == TXP_LANES ? 1 : -1];
/* the sound browser's preview (TXP_EV_PREVIEW, sb_pv_play / sb_pv_end): the lane as it was before it (its sound, HOW
 * MANY, HOW ACTIVE, playing or stopped, what PLAY ALL brings back, its mute, its cards set, LANES, the scene's edited
 * mark), put back when the preview ends; its cards once its sound's engine is in again (card, from t) */
static struct {
    uint8_t on, k, used, f, i, run, cnt, step, mcnt, mstep, mute, nlane, edited, card;
    uint8_t cv[SCD_MAX];
    uint32_t t;
} sbp;

/* ---- the coroutine (web_hal.c platform_poll asks the clock, then yields) */
static double move_now_ms(void) { return (double)sbx.ms + 1.0; }   /* (plugin/compat/emscripten.h: the pass after it
                                          * runs in the next millisecond, as tests/host.c's) */
static void move_yield(void) { coro_yield(&sbx.co); }
static void sb_entry(void *arg)
{
    (void)arg;
    coro_entered(&sbx.co);
    app_main();
    for (;;) coro_yield(&sbx.co);
}

/* ---- the host's parameters */
static void sb_tab_add(uint32_t id, uint32_t hi, int scaled, int is_enum, const char *mod, const char *pfx, const char *nm)
{
    txp_param_t *p;
    if (sbx.ntab >= TXP_NPARAM_MAX) return;
    p = &sbx.tab[sbx.ntab++];
    memset(p, 0, sizeof *p);
    p->id = id; p->hi = (uint16_t)hi; p->scaled = (uint8_t)scaled; p->is_enum = (uint8_t)is_enum;
    snprintf(p->name, sizeof p->name, "%s%s", pfx, nm);
    snprintf(p->module, sizeof p->module, "%s", mod);
}
static int sb_glob_hidden(uint32_t i)    /* the FM-1's own settings: the tempo, the clock, the screen */
{
    return i == PG_BPM || i == PG_FINE || i == PG_STATUS || i == PG_RUN || i == PG_TAP || i == PG_CLOCK ||
           i == PG_INUSB || i == PG_INTRS || i == PG_OUT || i == PG_COLOR;
}
static void sb_tab_make(void)
{
    static const char *const LL[] = {"ENGINE", "HOW MANY", "HOW ACTIVE", "LEVEL", "PAN"};
    uint32_t k, i, f, smax = 0;
    char mod[24], pfx[16], nm[24];
    sbx.ntab = 0;
    for (f = 0; f < LS_N; f++) if (LS[f].n > smax) smax = LS[f].n;
    sb_tab_add(TXP_ID_PRESET, PR_N - 1u, 0, 1, "Scenes", "", "PRESET");
    for (k = 0; k < SC_N; k++) {
        uint32_t s = P_LS(k);
        snprintf(mod, sizeof mod, "Lane %u", (unsigned)k + 1u);
        snprintf(pfx, sizeof pfx, "L%u ", (unsigned)k + 1u);
        for (i = 0; i <= PL_PAN; i++) {
            uint32_t id = PID(s, i), hi = i == PL_SOUND ? smax - 1u : (uint32_t)p_hi(id);
            sb_tab_add(id, hi, 0, i == PL_SOUND || i == PL_COUNT || i == PL_ACTIVE, mod, pfx, LL[i]);
        }
        for (i = 0; i < SCD_MAX; i++) {
            snprintf(nm, sizeof nm, "CARD %u", (unsigned)i + 1u);
            sb_tab_add(PID(s, PL_CARD0 + i), 127u, 1, 0, mod, pfx, nm);
        }
        sb_tab_add(PID(s, PL_GPART), (uint32_t)p_hi(PID(s, PL_GPART)), 0, 1, mod, pfx, "GROOVE PART");
        sb_tab_add(PID(s, PL_GCYC), (uint32_t)p_hi(PID(s, PL_GCYC)), 0, 1, mod, pfx, "GROOVE CYCLE");
    }
    for (i = 0; i < PGX_LANE; i++) {    /* (params.c p_gx: a global index from PGX_LANE up would find a lane's
                                         * GROOVE PART / CYCLE, and p_set would write past pv_glob) */
        uint32_t id = PID(P_GLOBAL, i);
        const char *m, *px;
        if (sb_glob_hidden(i) || PG_IS_MS(i) || !(p_fx_desc(id) || p_gx(id))) continue;   /* (the M/S: last, below) */
        if (i < PG_REVERB0) { m = "Grain delay"; px = "GRAIN "; }
        else if (i < PG_COMP0) { m = "Reverb"; px = "REVERB "; }
        else if (i < PG_BPM) { m = "Compressor"; px = "COMP "; }
        else if (i >= PG_DRIVE0 && i < PG_MF0) { m = "Drive"; px = "DRIVE "; }
        else if (i >= PG_MF0 && i < PG_GRV0) { m = "Filter"; px = "FILTER "; }
        else if (i >= PG_GRV0 && i <= PG_GSHIFT) { m = "Groove"; px = "GROOVE "; }
        else if (i == PG_SCALE || i == PG_ROOT || i == PG_SCMODE || i == PG_TRANS) { m = "Keys"; px = "KEYS "; }
        else if (i == PG_SELCH || i == PG_CHG || p_lch_lane(i) >= 0) { m = "MIDI"; px = "MIDI "; }
        else if (i == PG_NLANE) { m = "Lanes"; px = ""; }
        else { m = "Scenes"; px = ""; }
        sb_tab_add(id, (uint32_t)p_hi(id), 0, p_kind(id) != PK_AMOUNT, m, px, p_lab(id));
    }
    for (k = 0; k < TX_LFO_N; k++)
        for (i = 0; i < TX_LP_N; i++) {
            uint32_t id = PID(P_LFO, k * 16u + i);
            snprintf(mod, sizeof mod, "LFO %u", (unsigned)k + 1u);
            snprintf(pfx, sizeof pfx, "LFO%u ", (unsigned)k + 1u);
            sb_tab_add(id, (uint32_t)mod_hi(id), 0, mod_choice(id), mod, pfx, mod_lab(id));
        }
    for (i = 0; i < 64u; i++) {
        uint32_t id = PID(P_ENV, i);
        if (!mod_valid(id)) continue;
        if (mod_is_env(i)) { snprintf(mod, sizeof mod, "ENV %u", (unsigned)(i / 16u) + 1u); snprintf(pfx, sizeof pfx, "ENV%u ", (unsigned)(i / 16u) + 1u); }
        else { snprintf(mod, sizeof mod, "FOLLOW %u", (unsigned)((i - MV_FOL0) / 16u) + 1u); snprintf(pfx, sizeof pfx, "FOLLOW%u ", (unsigned)((i - MV_FOL0) / 16u) + 1u); }
        sb_tab_add(id, (uint32_t)mod_hi(id), 0, mod_choice(id), mod, pfx, mod_lab(id));
    }
    for (i = 0; i < MOD_NS * MQ_N; i++) {
        uint32_t id = PID(P_MAT, i);
        snprintf(pfx, sizeof pfx, "MOD%u ", (unsigned)(i / MQ_N) + 1u);
        sb_tab_add(id, (uint32_t)mod_hi(id), 0, mod_choice(id), "Matrix", pfx, mod_lab(id));
    }
    sb_tab_add(PID(P_MAT, MQ_CC), (uint32_t)mod_hi(PID(P_MAT, MQ_CC)), 0, 0, "Matrix", "", "MOD CC");
    sb_tab_add(TXP_ID_KEYS, 1u, 0, 1, "MIDI", "MIDI ", "KEYS");
    for (i = PG_MS0; i < PG_MS0 + TX_MS_NP; i++)   /* the M/S (MID SIDE WIDTH MONO): appended, so no id before moves */
        sb_tab_add(PID(P_GLOBAL, i), (uint32_t)p_hi(PID(P_GLOBAL, i)), 0, p_kind(PID(P_GLOBAL, i)) != PK_AMOUNT, "M/S", "M/S ",
                   p_lab(PID(P_GLOBAL, i)));
}
static uint32_t sb_params(txp_param_t *tab, uint32_t max)
{
    uint32_t n;
    if (!sbx.ntab) sb_tab_make();         /* (constant tables only: any thread, before the boot too) */
    n = sbx.ntab < max ? sbx.ntab : max;
    memcpy(tab, sbx.tab, n * sizeof *tab);
    return n;
}
static int sb_is_card(uint32_t id) { return P_IS_LANE(P_SCOPE(id)) && P_IDX(id) >= PL_CARD0 && P_IDX(id) < PL_CARD0 + SCD_MAX; }
/* a value's text, from constant tables only (the main thread asks while the audio thread runs the app): the formats
 * whose text is the value's alone (params.c p_text_v); -1 for the rest (the host shows the number) */
static int sb_text(uint32_t id, int32_t v, char *buf, uint32_t n)
{
    uint32_t s = P_SCOPE(id), i = P_IDX(id), f;
    char t[32];
    const char *u;
    if (id == TXP_ID_PRESET) {
        char nm[20];
        if (v < 0 || v >= (int32_t)PR_N) return -1;
        scn_name_of((uint32_t)v, nm);
        snprintf(buf, n, "%s", nm);
        return 0;
    }
    if (id == TXP_ID_KEYS) {
        if (v < 0 || v > 1) return -1;
        snprintf(buf, n, "%s", v ? "FM-1 KEYS" : "PLAY");
        return 0;
    }
    if (P_MODS(s) || (s > P_GLOBAL && !P_IS_LANE(s))) return -1;
    if (P_IS_LANE(s) && !(i == PL_COUNT || i == PL_ACTIVE || i == PL_PAN || i == PL_GPART || i == PL_GCYC)) return -1;
    f = p_fmt(id);
    if (f == F_SOUND || f == F_MS_GD || f == F_STATUS) return -1;
    if (v < 0 || v > p_hi(id)) return -1;
    u = p_text_v(id, v, t);
    snprintf(buf, n, "%s%s%s", t, u ? " " : "", u ? u : "");
    return 0;
}
static int32_t sb_value(const txp_param_t *p)   /* a host parameter's value now (the app's thread) */
{
    int32_t v, h;
    if (p->id == TXP_ID_PRESET) return sbx.last_fact;
    if (p->id == TXP_ID_KEYS) return sbx.keys;
    if (P_IS_LANE(P_SCOPE(p->id)) && P_IDX(p->id) != PL_GPART && P_IDX(p->id) != PL_GCYC && sc.snd[P_LANE_OF(P_SCOPE(p->id))] < 0)
        return P_IDX(p->id) == PL_PAN ? 64 : P_IDX(p->id) == PL_LEVEL ? 100 : 0;   /* an empty lane: what it plays, nothing
                                          * (its pan and cards of before are no part of a scene: a state loaded into another
                                          * instance would read others) */
    if (sbp.on && P_IS_LANE(P_SCOPE(p->id)) && P_LANE_OF(P_SCOPE(p->id)) == sbp.k &&
        (P_IDX(p->id) == PL_COUNT || P_IDX(p->id) == PL_ACTIVE))   /* the preview's lane: as it was before it (its
                                          * HOW MANY / HOW ACTIVE are the audition's, never the host's automation) */
        return !sbp.used || !sbp.run ? 0 : P_IDX(p->id) == PL_COUNT ? sbp.cnt : (int32_t)sc_act_of(sbp.step) + 1;
    if (P_IS_LANE(P_SCOPE(p->id)) && P_IDX(p->id) == PL_COUNT) {   /* HOW MANY under a modulation: the user's value,
                                          * as a scene keeps it (scenes.c scn_capture), not the one the matrix moves it to */
        uint32_t k = P_LANE_OF(P_SCOPE(p->id));
        if (m_cb_ok[k] && sc.run[k] && m_cbase[k]) return m_cbase[k];
    }
    if (!p->scaled) return p_get(p->id);
    if (!p_valid(p->id)) return 0;       /* (a card the lane's sound has not) */
    v = p_get(p->id); h = p_hi(p->id);
    {   /* a card the user has not set: its default. On the FM-1's page a hosted engine's card shows the engine's value
         * now, which the sound's own curves move (params.c p_card_get); to the host that would be a parameter moving
         * by itself, and a second instance loading the same state would read another value */
        p_cards_t *c = p_cards(P_LANE_OF(P_SCOPE(p->id)));
        uint32_t i = P_IDX(p->id) - PL_CARD0;
        if (i < c->n && c->cv[i] == 0xFF) v = c->def[i];
    }
    return h <= 0 ? 0 : h == 127 ? v : (v * 127 + h / 2) / h;
}
static void sb_param_set(uint32_t id, int32_t v)   /* a host parameter written, as MIDI writes it (params.c) */
{
    if (id == TXP_ID_PRESET) {
        if (v >= 0 && v < (int32_t)PR_N) { (void)pr_load((uint32_t)v); sbx.last_fact = (uint8_t)v; }
        return;
    }
    if (id == TXP_ID_KEYS) {             /* (the keys held let go: none stays down in the other mode) */
        if ((v != 0) != sbx.keys) { sbx.koff |= sbx.kmask; sbx.keys = (uint8_t)(v != 0); }
        return;
    }
    if (id >= TXP_ID_OWN0 || !p_valid(id)) return;
    if (sb_is_card(id)) p_midi_set(id, p_scale7(id, (uint32_t)(v < 0 ? 0 : v > 127 ? 127 : v)));
    else p_midi_set(id, v);
}

/* ---- published values (a sequence lock: the audio side writes, any thread reads) */
static int sb_is_settled(void)
{
    uint32_t k;
    if (scn_busy() || scn_gl_n || scn_cand != 0xFF || scn_sv_on || sbp.card) return 0;   /* (a SAVE written to its end
                                          * too, a preview's cards put back) */
    for (k = 0; k < SC_N; k++) if (sc.gl_on[k] || sc.a_eng[k] != sc.eng[k] || sc.reswap[k]) return 0;
    return 1;
}
/* the engines: each sound's source by name, in the order the families first name them (txp_catalog_t eng) */
static uint32_t sb_eng_of(const char *src, char (*names)[16], uint32_t *n, int add)
{
    uint32_t e;
    for (e = 0; e < *n; e++) if (!strncmp(names[e], src, 15)) return e;
    if (!add || *n >= TXP_ENG_MAX) return 0xFFu;
    snprintf(names[*n], 16, "%s", src);
    return (*n)++;
}
static char sb_engn[TXP_ENG_MAX][16];    /* (sb_boot: the engines in the catalog's order, for sb_ui) */
static uint32_t sb_neng;
static void sb_eng_list(void)
{
    uint32_t f, i;
    sb_neng = 0;
    for (f = 0; f < LS_N && f < TXP_FAM_MAX; f++)
        for (i = 0; i < LS[f].n && i < TXP_SND_MAX; i++) (void)sb_eng_of(LS[f].s[i].src, sb_engn, &sb_neng, 1);
}
static void sb_catalog(txp_catalog_t *o)  /* constant tables only (any thread) */
{
    uint32_t f, i, k;
    memset(o, 0, sizeof *o);
    o->nfam = (uint8_t)(LS_N < TXP_FAM_MAX ? LS_N : TXP_FAM_MAX);
    for (f = 0; f < o->nfam; f++) {
        uint32_t n = LS[f].n < TXP_SND_MAX ? LS[f].n : TXP_SND_MAX, ne = o->neng;
        snprintf(o->fam[f].name, sizeof o->fam[f].name, "%s", LS[f].name);
        o->fam[f].n = (uint8_t)n;
        for (i = 0; i < n; i++) {
            snprintf(o->fam[f].s[i].name, sizeof o->fam[f].s[i].name, "%s", LS[f].s[i].name);
            o->fam[f].s[i].eng = (uint8_t)sb_eng_of(LS[f].s[i].src, o->eng, &ne, 1);
        }
        o->neng = (uint8_t)ne;
    }
    o->npr = (uint8_t)(PR_N < TXP_PR_MAX ? PR_N : TXP_PR_MAX);
    o->ncat = (uint8_t)(PR_NCAT < TXP_CAT_MAX ? PR_NCAT : TXP_CAT_MAX);
    for (i = 0; i < o->npr; i++) {
        snprintf(o->pr[i], sizeof o->pr[i], "%s", pr_name(i));
        for (k = 0; k < 4u; k++) {
            pr_lane_v_t l;
            pr_lane(i, k, &l);
            o->prl[i][k].fi = l.fam < 0 ? 0xFFu : (uint8_t)l.fam; o->prl[i][k].si = l.snd < 0 ? 0xFFu : (uint8_t)l.snd;
            o->prl[i][k].cnt = l.cnt; o->prl[i][k].act = l.act; o->prl[i][k].vol = l.vol; o->prl[i][k].pan = l.pan;
            o->prl[i][k].mute = l.mute;
        }
    }
    for (i = 0; i < o->ncat; i++) {
        snprintf(o->cat[i], sizeof o->cat[i], "%s", pr_cat_name(i));
        o->cat_at[i] = (uint8_t)pr_cat_first(i);
    }
    o->cat_at[o->ncat] = (uint8_t)o->npr;
    o->nmt = (uint8_t)(MT_N < TXP_MT_MAX ? MT_N : TXP_MT_MAX);   /* the matrix's targets (mod.c's own functions) */
    for (i = 0; i < o->nmt; i++) {
        o->mt_lane[i] = !i || mod_tgt_retired(i) ? 0xFFu : (uint8_t)mod_tgt_lanes(i);
        o->mt_learn[i] = (uint8_t)!(i < MT_GRAIN0 && mod_tgt_drv(i) < TX_DR_NP);
        for (k = 0; k < TXP_LANES; k++) {
            uint32_t id = mod_tgt_pid(i, k);
            o->mt_id[i][k] = (uint16_t)(id == NOID_P ? 0xFFFFu : id);
        }
    }
}
static uint8_t sb_card127(uint32_t id, int32_t v)   /* a card's value onto 0 .. 127 (as its CC) */
{
    int32_t h = p_hi(id);
    v = h <= 0 ? 0 : h == 127 ? v : (v * 127 + h / 2) / h;
    return (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
}
static void sb_ui(txp_ui_snap_t *u)       /* what the window shows (the app's thread) */
{
    uint32_t k, i;
    memset(u, 0, sizeof *u);
    u->nlane = sc_nlane;
    u->sel = sc.sel;
    u->any_run = (uint8_t)sc_any_run();
    u->saving = scn_sv_on;
    u->undo = (uint8_t)(scn_undo_ok || scn_su.on);
    if (sbx.st.blocks != sbx.opk_blk) {  /* (the peak since the last publish that had blocks: a publish with none, a
                                          * flush's, keeps the last) */
        float db = sbx.opk > 0 ? 20.0f * log10f((float)sbx.opk * (1.0f / 32768.0f)) : -90.0f;
        sbx.odb10 = (int16_t)(1000 + (db < -90.0f ? -900 : db > 30.0f ? 300 : (int)lroundf(db * 10.0f)));
        sbx.opk = 0;
        sbx.opk_blk = sbx.st.blocks;
        /* the compressor's: its most since then (COMP off: none) */
        db = sbx.cin_pk > 0.0f ? 10.0f * log10f(sbx.cin_pk * (1.0f / 67108864.0f)) : -90.0f;
        sbx.cin10 = (int16_t)(db < -90.0f ? -900 : db > 30.0f ? 300 : (int)lroundf(db * 10.0f));
        db = sbx.cout_pk > 0.0f ? 10.0f * log10f(sbx.cout_pk * (1.0f / 67108864.0f)) : -90.0f;
        sbx.cout10 = (int16_t)(db < -90.0f ? -900 : db > 30.0f ? 300 : (int)lroundf(db * 10.0f));
        sbx.cgr10 = (int16_t)((sbx.cgr_pk * 10 + 680) / 1361);   /* (tx_comp_gr_db10's) */
        sbx.cin_pk = sbx.cout_pk = 0.0f;
        sbx.cgr_pk = 0;
    }
    if (!pv_glob[PG_COMP0 + TX_CP_ON]) sbx.cin10 = sbx.cout10 = -900, sbx.cgr10 = 0;
    {   /* the M/S's meters (its own, tx_core_ms.h: peaks Q23 held and falling), the goniometer's last block */
        const tx_ms_t *ms = &ln.mix.ms;
        for (i = 0; i < 4u; i++) {
            float db = ms->pk[i] > 0 ? 20.0f * log10f((float)ms->pk[i] * (1.0f / 8388608.0f)) : -90.0f;
            u->ms_db10[i] = (int16_t)(db < -90.0f ? -900 : db > 30.0f ? 300 : (int)lroundf(db * 10.0f));
        }
        u->ms_corr = (int16_t)(ms->corr > 32767 ? 32767 : ms->corr < -32767 ? -32767 : ms->corr);
        for (i = 0; i < TX_MS_NP && i < 8u; i++) if (mod_holds(PID(P_GLOBAL, PG_MS0 + i))) u->ms_mod |= (uint8_t)(1u << i);
        memcpy(u->gon, sbx.gon, sizeof u->gon);
        u->gon_blk = sbx.gon_blk;
    }
    u->comp_gr10 = sbx.cgr10;
    u->comp_in10 = sbx.cin10;
    u->comp_out10 = sbx.cout10;
    u->out_db10 = (int16_t)(sbx.odb10 ? sbx.odb10 - 1000 : -900);
    for (i = 0; i < 64u && i < PGX_LANE; i++) {
        uint32_t id = PID(P_GLOBAL, i);
        if ((p_fx_desc(id) || p_gx(id)) && mod_holds(id)) u->gmod[i / 32u] |= 1u << (i % 32u);
    }
    for (i = 0; i < TX_ENV_N + TX_FOL_N && i < 4u; i++) {   /* the envelopes and followers now (the window's traces) */
        int32_t v = mod_ext[ME_ENV1 + i];
        u->envf[i] = (int16_t)(v < 0 ? 0 : v > 32767 ? 32767 : v);
    }
    for (i = 0; i < TX_LFO_N && i < TXP_LFO_N; i++)
        if (mod_holds(PID(P_LFO, i * 16u + TX_LP_RATE)) || mod_holds(PID(P_LFO, i * 16u + TX_LP_LEVEL))) u->lfo_mod |= (uint8_t)(1u << i);
    u->edited = scn_edited;
    u->solo = sbx.solo;
    u->scene = scn_cur;
    u->land = sc.fam;
    u->fx_yours = (uint8_t)(scn_fx_mine && scn_fx_mode == SCN_FX_SMART);
    if (scn_cur != 0xFF && scn_cur < SCN_N) { char nm[20]; scn_name_of(scn_cur, nm); snprintf(u->preset, sizeof u->preset, "%s", nm); }
    for (i = 0; i < SCN_NU && i < TXP_USER_N; i++)
        if (scn_meta[i].used) { u->user |= (uint16_t)(1u << i); snprintf(u->uname[i], sizeof u->uname[i], "%.12s", scn_meta[i].name); }
    for (i = 0; i < TX_LFO_N && i < TXP_LFO_N; i++) {   /* the LFOs: their values now, routed ones named by their
                                          * first route */
        uint32_t s;
        u->lfo[i] = (int16_t)(mod_lfo[i].out > 32767 ? 32767 : mod_lfo[i].out < -32767 ? -32767 : mod_lfo[i].out);
        for (s = 0; s < MOD_NS; s++)
            if (mslot[s].src == MS_LFO1 + i && mslot[s].tgt && mslot[s].depth != 100u) {
                char t[40];
                u->lfo_on |= (uint8_t)(1u << i);
                mod_tgt_text(mslot[s].tgt, t);
                snprintf(u->lfo_to[i], sizeof u->lfo_to[i], "%s", t);
                break;
            }
    }
    for (k = 0; k < SC_N; k++) {
        txp_ui_lane_t *l = &u->lane[k];
        uint32_t ls = P_LS(k);
        int32_t v;
        l->ch = nt_lch[k];
        l->dist = 0xFF; l->fi = l->si = l->eng = 0xFF;
        if (sc.snd[k] < 0) continue;
        l->used = 1;
        l->mute = sc.mute[k];
        l->run = sc.run[k];
        l->cnt = (uint8_t)sc_cnt_of(k);
        if (!sc.run[k] && sc.mem_cnt[k]) { l->mcnt = sc.mem_cnt[k]; l->mact = (uint8_t)(sc_act_of(sc.mem_step[k]) + 1u); }
        l->fi = sc.lf[k]; l->si = (uint8_t)sc.snd[k];
        {
            uint32_t ne = sb_neng;
            l->eng = (uint8_t)sb_eng_of(LS[sc.lf[k]].s[sc.snd[k]].src, sb_engn, &ne, 0);
        }
        snprintf(l->snd, sizeof l->snd, "%s", sc_sound_name(k));
        snprintf(l->fam, sizeof l->fam, "%s", LS[sc.lf[k]].name);
        {
            p_cards_t *c = p_cards(k);
            txp_ui_cards_t *cd = &u->card[k];
            for (i = 0; i < c->n; i++) if (sc_eq(c->lab[i], "DIST")) { l->dist = (uint8_t)i; break; }
            cd->n = (uint8_t)(c->n < TXP_CARD_N ? c->n : TXP_CARD_N);
            for (i = 0; i < cd->n; i++) {  /* (the drawer: names, the sound's own values, what the modulation moves) */
                uint32_t id = PID(ls, PL_CARD0 + i);
                snprintf(cd->lab[i], sizeof cd->lab[i], "%s", c->lab[i] ? c->lab[i] : "");
                cd->def[i] = sb_card127(id, c->def[i]);
                cd->live[i] = sb_card127(id, p_get(id));
                if (mod_live(id, &v)) { cd->mod |= (uint8_t)(1u << i); cd->live[i] = sb_card127(id, v); }
            }
        }
        l->lpan = sc.pan[k];
        if (mod_live(PID(ls, PL_PAN), &v)) { l->modm |= TXP_MOD_PAN; l->lpan = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v); }
        if (mod_holds(PID(ls, PL_LEVEL))) l->modm |= TXP_MOD_LEVEL;
        if (mod_holds(PID(ls, PL_COUNT))) l->modm |= TXP_MOD_COUNT;
        if (mod_holds(PID(ls, PL_ACTIVE))) l->modm |= TXP_MOD_ACTIVE;
        if (l->dist != 0xFF) {
            uint32_t id = PID(ls, PL_CARD0 + l->dist);
            l->ldist = sb_card127(id, p_get(id));
            if (mod_live(id, &v)) { l->modm |= TXP_MOD_DIST; l->ldist = sb_card127(id, v); }
        }
    }
}
static void sb_publish(void)
{
    uint32_t q = atomic_load_explicit(&sbx.pub_seq, memory_order_relaxed), i;
    if (!sbx.ntab) sb_tab_make();
    if (scn_cur != 0xFF && scn_cur < SCN_NF) sbx.last_fact = scn_cur;
    atomic_store_explicit(&sbx.pub_seq, q + 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    for (i = 0; i < sbx.ntab; i++) sbx.pub.pv[i] = (int16_t)sb_value(&sbx.tab[i]);
    {   /* the values' texts, as the app's pages write them: those that moved, all of them every 32nd time (a text
         * that follows another value: a RATE its SYNC, a TARGET its lane's sound) */
        int all = sbx.ptx_n++ % 32u == 0u;
        for (i = 0; i < sbx.ntab; i++) {
            const txp_param_t *p = &sbx.tab[i];
            char t[32];
            const char *un;
            int32_t v = sbx.pub.pv[i];
            if (!all && v == sbx.ptx_v[i]) continue;
            sbx.ptx_v[i] = (int16_t)v;
            t[0] = 0;
            if (p->id >= TXP_ID_OWN0) { if (sb_text(p->id, v, t, sizeof t)) snprintf(t, sizeof t, "%d", (int)v); }
            else if (P_IS_LANE(P_SCOPE(p->id)) && P_IDX(p->id) != PL_GPART && P_IDX(p->id) != PL_GCYC && sc.snd[P_LANE_OF(P_SCOPE(p->id))] < 0) {}
            else if (p_valid(p->id)) {
                un = p_text(p->id, t);
                if (un) { size_t n = strlen(t); snprintf(t + n, sizeof t - n, " %s", un); }
            }
            snprintf(sbx.pub.ptx[i], sizeof sbx.pub.ptx[i], "%s", t);
        }
    }
    sb_ui(&sbx.pub.ui);
    sbx.pub.scene = scn_cur;
    sbx.pub.settled = (uint8_t)sb_is_settled();
    sbx.pub.tempo10 = (uint16_t)tempo_bpm10();
    sbx.pub.rev = sbx.rev;
    atomic_store_explicit(&sbx.pub_seq, q + 2u, memory_order_release);
    sbx.pub_rev = sbx.rev;
    sbx.pub_n = 0;
}
static void sb_snapshot(txp_snap_t *out)
{
    for (;;) {
        uint32_t q1 = atomic_load_explicit(&sbx.pub_seq, memory_order_acquire), q2;
        if (q1 & 1u) continue;
        *out = sbx.pub;
        atomic_thread_fence(memory_order_acquire);
        q2 = atomic_load_explicit(&sbx.pub_seq, memory_order_relaxed);
        if (q1 == q2) return;
    }
}
/* the sound browser's preview in a captured scene: its lane as it was before it (sb_pv_play kept that), so a project
 * saved during an audition saves the lane, not the audition; the cards of a preview just ended too, while they are
 * not back yet (sb_pv_cards) */
static void sb_capture_pv(scn_t *s)
{
    uint32_t k = sbp.k, i, n = 0, id0 = PID(P_LS(k), PL_CARD0);
    if ((!sbp.on && !sbp.card) || k >= SC_N) return;
    if (!sbp.on && (sc.snd[k] != (int)sbp.i || sc.lf[k] != sbp.f)) return;   /* (another sound since: its own cards) */
    for (i = 0; i < s->nrec; i++)        /* (the lane's cards out: the ones it had go back in below) */
        if (s->r[i].id < id0 || s->r[i].id >= id0 + SCD_MAX) s->r[n++] = s->r[i];
    s->nrec = (uint16_t)n;
    if (sbp.on) {
        s->mute &= ~(1u << k);
        s->lf[k] = 0;
        if (!sbp.used) {                 /* (an empty lane: empty again, LANES as it was; as scn_capture keeps one) */
            s->snd[k] = -1; s->cnt[k] = s->step[k] = s->run[k] = 0; s->pan[k] = 64;
#if SC_N > 4
            s->nlane = sbp.nlane;
#endif
            return;
        }
        s->snd[k] = (int8_t)sbp.i; SCN_LF_SET(s, k, sbp.f);
        s->cnt[k] = sbp.cnt; s->step[k] = sbp.step; s->run[k] = sbp.run;
        if (sbp.mute) SCN_MUTE_SET(s, k);
    }
    for (i = 0; i < SCD_MAX; i++) if (sbp.cv[i] != 0xFF) scn_add(s, id0 + i, sbp.cv[i]);
}
static void sb_capture(void)             /* the state as it plays now, for sb_state_get */
{
    uint32_t q = atomic_load_explicit(&sbx.cap_seq, memory_order_relaxed), i;
    atomic_store_explicit(&sbx.cap_seq, q + 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    scn_capture(&sbx.cap_scn);
    sb_capture_pv(&sbx.cap_scn);
    sbx.cap_cur = scn_cur;
    sbx.cap_solo = sbx.solo;
    sbx.cap_keys = sbx.keys;
    sbx.cap_sel = sc.sel;
    sbx.cap_umask = 0;
    for (i = 0; i < SCN_NU; i++)         /* (the user scenes: each saved one's record, as the bank holds it) */
        if (scn_meta[i].used && !scn_read(i, &sbx.cap_user[i])) sbx.cap_umask |= (uint16_t)(1u << i);
    for (i = 0; i < SB_NSET; i++) sbx.cap_set[i] = (int16_t)p_get(PID(P_GLOBAL, SB_SET_IDX[i]));
    atomic_store_explicit(&sbx.cap_seq, q + 2u, memory_order_release);
}

/* ---- the state: {"textura":"2","cur":"N","set":"I=V,...","solo":"M","keys":"K","sel":"S","umask":"U",
 * "user":"<scn_t, hex, each saved one in order>","scene":"<scn_t, hex>"} (M: the SOLO's lanes, a mask, absent in a
 * state of before: none; K KEYS, S the selected lane, U the user scenes saved, a mask: absent, the defaults and no user
 * scene; the scene record as the user bank
 * keeps it, scenes.c scn_t: version 2, SC_N lanes and LANES; a later firmware's records load as a scene of before
 * does). A state of before, "textura":"1" (the four-lane plugin's: its scene the FM-1's record, version 1), loads
 * into lanes 1 .. 4, the others empty (scenes.c scn_from_v1) */
static const char SB_HEX[] = "0123456789abcdef";
static const char *sb_json_str(const char *j, const char *key, uint32_t *len)   /* "key":"value" in a flat object */
{
    char pat[24];
    const char *p, *q;
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    p = strstr(j, pat);
    if (!p) return 0;
    p += strlen(pat);
    q = strchr(p, '"');
    if (!q) return 0;
    *len = (uint32_t)(q - p);
    return p;
}
static char *sb_hex_put(char *o, const void *p, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) { *o++ = SB_HEX[((const uint8_t *)p)[i] >> 4]; *o++ = SB_HEX[((const uint8_t *)p)[i] & 15u]; }
    return o;
}
static int sb_state_get(char *buf, int n, int fresh)   /* fresh: ask the audio side for a capture first (processing) */
{
    scn_t s;
    uint8_t cur, keys, sel;
    uint16_t solo, um;
    int16_t set[SB_NSET];
    char *o = buf;
    uint32_t i;
    static _Thread_local scn_t user[SCN_NU];
    if (n < (int)(SB_SCN_HEX + SB_USER_HEX + 1024u)) return -1;
    if (fresh) {                         /* the next step captures (a host that stops calling process: the last one) */
        uint32_t c0 = atomic_load(&sbx.cap_seq);
        struct timespec ts = { 0, 1000000 };
        atomic_store(&sbx.cap_req, 1);
        for (i = 0; i < 200u && atomic_load(&sbx.cap_seq) == c0; i++) nanosleep(&ts, 0);
    }
    for (;;) {
        uint32_t q1 = atomic_load_explicit(&sbx.cap_seq, memory_order_acquire), q2;
        if (q1 & 1u) continue;
        s = sbx.cap_scn; cur = sbx.cap_cur; solo = sbx.cap_solo; memcpy(set, sbx.cap_set, sizeof set);
        keys = sbx.cap_keys; sel = sbx.cap_sel; um = sbx.cap_umask;
        for (i = 0; i < SCN_NU; i++) if (um >> i & 1u) user[i] = sbx.cap_user[i];
        atomic_thread_fence(memory_order_acquire);
        q2 = atomic_load_explicit(&sbx.cap_seq, memory_order_relaxed);
        if (q1 == q2) break;
    }
    o += sprintf(o, "{\"textura\":\"2\",\"cur\":\"%u\",\"set\":\"", (unsigned)cur);
    for (i = 0; i < SB_NSET; i++) o += sprintf(o, "%s%u=%d", i ? "," : "", (unsigned)PID(P_GLOBAL, SB_SET_IDX[i]), set[i]);
    o += sprintf(o, "\",\"solo\":\"%u\",\"keys\":\"%u\",\"sel\":\"%u\",\"umask\":\"%u\",\"user\":\"", (unsigned)solo,
                 (unsigned)keys, (unsigned)sel, (unsigned)um);
    for (i = 0; i < SCN_NU; i++) if (um >> i & 1u) o = sb_hex_put(o, &user[i], sizeof user[i]);
    o += sprintf(o, "\",\"scene\":\"");
    o = sb_hex_put(o, &s, sizeof s);
    o += sprintf(o, "\"}");
    return (int)(o - buf);
}
static int32_t sb_json_num(const char *j, const char *key, int32_t none)   /* "key":"N" (absent or not a number: none) */
{
    uint32_t l = 0, i, n = 0;
    const char *p = j ? sb_json_str(j, key, &l) : 0;
    if (!p || !l || l > 6u) return none;
    for (i = 0; i < l; i++) { if (p[i] < '0' || p[i] > '9') return none; n = n * 10u + (uint32_t)(p[i] - '0'); }
    return (int32_t)n;
}
static int sb_hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int sb_state_set(const char *j)   /* into the mailbox; sb_state_apply applies it between two passes */
{
    uint32_t lv = 0, ls = 0, lc = 0, lt = 0, i;
    const char *v = j ? sb_json_str(j, "textura", &lv) : 0, *s = j ? sb_json_str(j, "scene", &ls) : 0;
    const char *c = j ? sb_json_str(j, "cur", &lc) : 0, *t = j ? sb_json_str(j, "set", &lt) : 0;
    uint32_t lo = 0;
    const char *so = j ? sb_json_str(j, "solo", &lo) : 0;
    int expect = 0, v1;
    scn_t x;
    static scn_v1_t x1;                  /* (the main thread's alone) */
    uint8_t *b8;
    if (!v || lv != 1u || (v[0] != '1' && v[0] != '2') || !s) { sbx.st.state_bad++; return -1; }
    v1 = v[0] == '1';
    if (ls != (v1 ? SB_SCN_V1_HEX : SB_SCN_HEX)) { sbx.st.state_bad++; return -1; }
    b8 = v1 ? (uint8_t *)&x1 : (uint8_t *)&x;
    for (i = 0; i < ls / 2u; i++) {
        int a = sb_hexv(s[2u * i]), b = sb_hexv(s[2u * i + 1u]);
        if (a < 0 || b < 0) { sbx.st.state_bad++; return -1; }
        b8[i] = (uint8_t)(a << 4 | b);
    }
    if (v1 ? scn_from_v1(&x, &x1) : (x.ver != SCN_VER || x.nrec > SCN_REC_MAX)) { sbx.st.state_bad++; return -1; }
    {   /* the user scenes (a state without them, or one unreadable: none) */
        static scn_t ux[SCN_NU];         /* (the main thread's alone) */
        int32_t um = sb_json_num(j, "umask", -1);
        uint32_t lu = 0, at = 0;
        const char *u = sb_json_str(j, "user", &lu);
        uint32_t m = um > 0 ? (uint32_t)um & 0xFFFFu : 0u, cnt = 0;
        for (i = 0; i < SCN_NU; i++) cnt += m >> i & 1u;
        if (!u || lu != cnt * 2u * sizeof(scn_t)) m = 0;
        for (i = 0; i < SCN_NU && m; i++) {
            uint8_t *b = (uint8_t *)&ux[i];
            if (!(m >> i & 1u)) continue;
            for (uint32_t q = 0; q < sizeof(scn_t); q++, at += 2u) {
                int a = sb_hexv(u[at]), c2 = sb_hexv(u[at + 1u]);
                if (a < 0 || c2 < 0) { m = 0; break; }
                b[q] = (uint8_t)(a << 4 | c2);
            }
            if (m && (ux[i].ver != SCN_VER || !ux[i].used || ux[i].nrec > SCN_REC_MAX)) m &= ~(1u << i);
        }
        um = (int32_t)m;
        expect = 0;
        while (!atomic_compare_exchange_weak(&sbx.st_flag, &expect, 1)) {
            if (expect == 3) { sbx.st.state_bad++; return -1; }   /* (being applied right now: the host may retry) */
            if (expect != 0 && expect != 2) expect = 0;
        }
        sbx.st_umask = um;
        for (i = 0; i < SCN_NU; i++) if (m >> i & 1u) sbx.st_user[i] = ux[i];
        sbx.st_keys = (int8_t)(sb_json_num(j, "keys", -1) == 1 ? 1 : sb_json_num(j, "keys", -1) == 0 ? 0 : -1);
        {
            int32_t se = sb_json_num(j, "sel", -1);
            sbx.st_sel = (int8_t)(se >= 0 && se < (int32_t)SC_N ? se : -1);
        }
    }
    sbx.st_scn = x;
    sbx.st_cur = 0xFF;
    if (c && lc > 0 && lc < 4) { uint32_t n = 0; for (i = 0; i < lc; i++) n = n * 10u + (uint32_t)(c[i] - '0'); if (n < SCN_N) sbx.st_cur = (uint8_t)n; }
    sbx.st_solo = -1;
    if (so && lo > 0 && lo < 6) { uint32_t n = 0; for (i = 0; i < lo && so[i] >= '0' && so[i] <= '9'; i++) n = n * 10u + (uint32_t)(so[i] - '0'); sbx.st_solo = (int32_t)(n & 0xFFFFu); }
    for (i = 0; i < SB_NSET; i++) sbx.st_set[i] = -1;
    while (t && lt) {                    /* "id=value" pairs: a setting this build has not is skipped */
        uint32_t id = 0, k = 0;
        int32_t val = 0, neg = 0;
        while (k < lt && t[k] >= '0' && t[k] <= '9') id = id * 10u + (uint32_t)(t[k++] - '0');
        if (k < lt && t[k] == '=') k++;
        if (k < lt && t[k] == '-') { neg = 1; k++; }
        while (k < lt && t[k] >= '0' && t[k] <= '9') val = val * 10 + (t[k++] - '0');
        for (i = 0; i < SB_NSET; i++) if (id == PID(P_GLOBAL, SB_SET_IDX[i])) sbx.st_set[i] = (int16_t)(neg ? -val : val);
        while (k < lt && t[k] != ',') k++;
        if (k < lt) k++;
        t += k; lt -= k;
    }
    atomic_store(&sbx.st_flag, 2);
    return 0;
}
/* the user scenes' bank put back as a state has it (m: those saved): both copies erased, copy A written whole (its
 * records, then its header, as scn_bank.c writes one), then read as at the boot (scenes.c scn_bank_load) */
static void sb_user_put(uint32_t m)
{
    static scn_t z;                      /* (an empty slot: zeros, as scn_chunk writes one) */
    uint32_t i, c = 0xFFFFFFFFu;
    scn_hdr_t h;
    for (i = 0; i < 2u * SCN_BANK_SECT; i++) (void)fl_window_erase(SCN_BANK_A + i * 0x1000u);
    if (m) {
        for (i = 0; i < SCN_NU; i++) {
            const scn_t *s = m >> i & 1u ? &sbx.st_user[i] : &z;
            (void)fl_window_write(SCN_BANK_A + sizeof h + i * SCN_SLOT, s, SCN_SLOT);
            c = scn_crc(c, (const uint8_t *)s, SCN_SLOT);
        }
        h.magic = SCN_MAGIC; h.seq = 1u; h.len = SCN_NU * SCN_SLOT; h.crc = ~c;
        (void)fl_window_write(SCN_BANK_A, &h, sizeof h);
    }
    scn_bank_load();
}
static void sb_state_apply(void)
{
    int expect = 2;
    uint32_t i;
    if (atomic_load(&sbx.st_flag) != 2 || scn_busy() || scn_sv_on) return;   /* (a load running reads its own scene:
                                          * after it; a save running writes the bank: after it) */
    if (!atomic_compare_exchange_strong(&sbx.st_flag, &expect, 3)) return;
    for (i = 0; i < SB_NSET; i++) if (sbx.st_set[i] >= 0) p_set(PID(P_GLOBAL, SB_SET_IDX[i]), sbx.st_set[i]);
    sb_user_put(sbx.st_umask > 0 ? (uint32_t)sbx.st_umask : 0u);
    if ((sbx.st_keys == 1) != sbx.keys) { sbx.koff |= sbx.kmask; sbx.keys = sbx.st_keys == 1; }
    sbx.ap_scn = sbx.st_scn;
    scn_cand = 0xFF;                     /* (a program change still waiting: the state comes after it) */
    scn_fx_force = 1;                    /* (its effects too, whatever EFFECTS ON LOAD says) */
    for (i = 0; i < sbx.ap_scn.nrec; i++) /* (its tempo: whatever PRESETS CHANGE TEMPO says; the host's leads) */
        if (sbx.ap_scn.r[i].id == PID(P_GLOBAL, PG_FINE)) (void)tempo_set10((uint32_t)(sbx.ap_scn.r[i].v + 400));
    scn_apply(&sbx.ap_scn, sbx.st_cur);
    if (sbx.st_sel >= 0) sc_select((uint32_t)sbx.st_sel);
    sbx.solo = (uint16_t)(sbx.st_solo > 0 ? sbx.st_solo : 0);   /* (a state without one: no lane soloed) */
    if (sbx.st_cur < SCN_NF) sbx.last_fact = sbx.st_cur;
    sbx.st.state_ok++; sbx.rev++;
    atomic_store(&sbx.st_flag, 0);
}

/* ---- the queue */
static int sb_put_any(int kind, int x, uint32_t at, uint32_t a, int32_t v)
{
    uint32_t w = atomic_load_explicit(&sbx.qw, memory_order_relaxed), r = atomic_load_explicit(&sbx.qr, memory_order_acquire);
    sb_ev_t *e;
    if (w - r >= SB_Q) { sbx.st.q_full++; return -1; }
    e = &sbx.q[w & (SB_Q - 1u)];
    e->kind = (uint8_t)kind; e->x = (uint8_t)x; e->at = at; e->a = a; e->v = v;
    atomic_store_explicit(&sbx.qw, w + 1u, memory_order_release);
    return 0;
}
static int sb_put(int kind, uint32_t at, uint32_t a, int32_t v) { return sb_put_any(kind, 0, at, a, v); }
static int sb_putx(int kind, uint32_t at_q8, uint32_t a, int32_t v) { return sb_put_any(kind, 1, at_q8, a, v); }
static void sb_set_exact(int on) { sbx.exact = on != 0; }
static uint32_t sb_efr(void) { return sbx.fr; }
static uint32_t sb_efw(void) { return sbx.fw; }
static uint32_t sb_room(void)
{
    return SB_Q - (atomic_load_explicit(&sbx.qw, memory_order_relaxed) - atomic_load_explicit(&sbx.qr, memory_order_acquire));
}
static void sb_notes_off(void) { while (nt_mn) nt_mq_off(0); sbx.koff |= sbx.kmask; }   /* every MIDI note let go
                                          * (notes.c), every FM-1 key too */
/* KEYS = FM-1 KEYS: a note on SELECTED CH taken as the FM-1's keys (1), or left to the app (0). F3 .. G5 press and let
 * go the key (the app reads it at its next pass; one let go before it: a pass later); the black keys above G5 select
 * lanes 5 .. 16; a note off whose key is not held (a note played before the mode changed) is the app's */
static int sb_keys(uint8_t st, uint8_t d1, uint8_t d2)
{
    uint32_t ch = st & 15u, ty = st & 0xF0u, on = ty == 0x90u && d2 > 0u;
    if (!sbx.keys || !nt_selch || ch + 1u != nt_selch || (ty != 0x90u && ty != 0x80u)) return 0;
    if (d1 >= SB_KEY0 && d1 < SB_KEY0 + TX_NKEYS) {
        uint32_t b = 1u << (d1 - SB_KEY0);
        if (on) { sbx.kmask |= b; sbx.koff &= ~b; return 1; }
        if (sbx.kmask & b) { sbx.koff |= b; return 1; }
        return 0;
    }
    if (d1 >= SB_KEY0 + TX_NKEYS && on && (0x54Au >> (d1 % 12u) & 1u)) {   /* (C# D# F# G# A#) */
        uint32_t n = 0, x;
        for (x = SB_KEY0 + TX_NKEYS; x <= d1; x++) n += 0x54Au >> (x % 12u) & 1u;
        if (n >= 1u && 3u + n < SC_N) sc_select(3u + n);
        return 1;
    }
    return 0;
}
/* ---- the sound browser's preview: the sound under the cursor always heard on the lane. A lane that plays plays it as
 * it plays (as the FM-1's browser); a stopped lane (or all stopped) starts at the preview's HOW MANY and HOW ACTIVE, a
 * muted one is unmuted, a lane outside the solo is heard. Its end puts the lane back as it was: its sound, HOW MANY,
 * HOW ACTIVE, stopped or playing, what PLAY ALL brings back, its mute, its cards (once its engine is in again, as a
 * scene's: scenes.c scn_apply_cards), an empty lane emptied again. KEEP keeps the sound, the rest as it was (stopped
 * stays stopped) unless the preview's HOW MANY / HOW ACTIVE are kept too */
static void sb_pv_runback(uint32_t k)    /* the lane playing or stopped as before, at its HOW MANY and HOW ACTIVE */
{
    if (sbp.run) { sc_count(k, sbp.cnt); sc_step(k, sbp.step); }
    else { if (sc.run[k]) sc_start(k, 0); sc.cnt[k] = sbp.cnt; sc.step[k] = sbp.step; sc_sync(k); }
    sc.mem_cnt[k] = sbp.mcnt; sc.mem_step[k] = sbp.mstep;
    if (sc.snd[k] >= 0 && sc.mute[k] != sbp.mute) sc_mute(k, sbp.mute);
}
static void sb_pv_end(uint32_t k, int keep, int vals, uint32_t f, uint32_t i)
{
    int ok = k < SC_N && f < LS_N && i < LS[f].n;
    if (!sbp.on || sbp.k != k) {          /* (no preview: KEEP as the FM-1's browser, the lane playing on as it was) */
        if (keep && ok && !(sc.snd[k] == (int)i && sc.lf[k] == f)) { sc_sound_set(k, f, i); scn_touch(); }
        return;
    }
    sbp.on = 0;
    if (keep && ok) {
        int moved = !(sbp.used && sbp.f == f && sbp.i == i);
        sc_sound_set(k, f, i);
        if (!vals) sb_pv_runback(k);
        else if (sc.mute[k] != sbp.mute) sc_mute(k, sbp.mute);
        if (moved || vals) scn_touch(); else scn_edited = sbp.edited;
        return;
    }
    if (!sbp.used) {                     /* (an empty lane: empty again, LANES as it was) */
        scn_lane_clear(k);
        sc_nlane = sbp.nlane;
    } else {
        sc_sound_set(k, sbp.f, sbp.i);
        sb_pv_runback(k);
        for (uint32_t j = 0; j < SCD_MAX; j++) if (sbp.cv[j] != 0xFF) { sbp.card = 1; sbp.t = fm1_ms; }
    }
    scn_edited = sbp.edited;
}
static void sb_pv_play(uint32_t k, uint32_t f, uint32_t i, uint32_t cnt, uint32_t act, int force)
{
    if (k >= SC_N || f >= LS_N || i >= LS[f].n) return;
    if (sbp.on && sbp.k != k) sb_pv_end(sbp.k, 0, 0, 0, 0);
    if (!sbp.on) {                       /* (the lane as it is now: what the end puts back) */
        int pend = sbp.card && sbp.k == k;   /* (its cards not back yet from a preview just ended: those still) */
        sbp.on = 1; sbp.k = (uint8_t)k; sbp.used = sc.snd[k] >= 0;
        sbp.f = sc.lf[k]; sbp.i = (uint8_t)(sc.snd[k] >= 0 ? sc.snd[k] : 0);
        sbp.run = sc.run[k]; sbp.cnt = sc.cnt[k]; sbp.step = sc.step[k];
        sbp.mcnt = sc.mem_cnt[k]; sbp.mstep = sc.mem_step[k]; sbp.mute = sc.mute[k];
        sbp.nlane = sc_nlane; sbp.edited = scn_edited;
        if (!pend) {
            p_cards_t *c = sbp.used ? p_cards(k) : 0;
            memset(sbp.cv, 0xFF, sizeof sbp.cv);
            for (uint32_t j = 0; c && j < c->n && j < SCD_MAX; j++) sbp.cv[j] = c->cv[j];
        }
        sbp.card = 0;
    }
    sc_sound_set(k, f, i);
    if (sc.mute[k]) sc_mute(k, 0);
    if ((!sbp.run || force) && cnt) {
        sc_count(k, cnt < LS_NCNT ? cnt : LS_NCNT - 1u);
        sc_step(k, sc_act_step(act > 1u ? (act > LS_NACT ? LS_NACT : act) - 1u : 0u));
    }
}
static void sb_pv_cards(void)            /* after an app pass: an ended preview's cards, once its engine is in */
{
    uint32_t k = sbp.k, j;
    if (!sbp.card) return;
    if (sbp.on || k >= SC_N || sc.snd[k] != (int)sbp.i || sc.lf[k] != sbp.f) { sbp.card = 0; return; }   /* (another
                                          * sound since: its own cards) */
    if (sc.a_eng[k] != sc.eng[k] || sc.reswap[k] || fm1_ms - sc.sel_t[k] < SCN_CARDS_MS || fm1_ms - sbp.t < 40u) return;
    if (ln.qw - ln.qr >= LN_QN / 2u) return;
    (void)p_cards(k);
    for (j = 0; j < SCD_MAX; j++) if (sbp.cv[j] != 0xFF) p_set(PID(P_LS(k), PL_CARD0 + j), sbp.cv[j]);
    sbp.card = 0;
    sbx.rev++;
}
/* apply the queued events; timed: only those due by sbx.ms, an exact one only for the next block to render (and a
 * full MIDI ring makes the rest wait a millisecond) */
static void sb_events(int timed)
{
    sbx.ev_wait = 0;
    for (;;) {
        uint32_t r = atomic_load_explicit(&sbx.qr, memory_order_relaxed);
        sb_ev_t *e;
        if (r == atomic_load_explicit(&sbx.qw, memory_order_acquire)) return;
        e = &sbx.q[r & (SB_Q - 1u)];
        if (e->x) {
            int32_t d = (int32_t)(e->at - sbx.fw * 256u);   /* (sbx.fw: the next block's first frame) */
            if (timed && d >= 65536) return;              /* (a later block's) */
        } else if (timed && e->at && (int32_t)(e->at - sbx.ms) > 0) return;
        if (e->kind == TXP_EV_MIDI) {
            uint8_t st = (uint8_t)e->a, d1 = (uint8_t)(e->a >> 8) & 127u, d2 = (uint8_t)(e->a >> 16) & 127u;
            if (mq_w - mq_r >= SB_HOST_RING) { sbx.st.ring_wait++; sbx.ev_wait = 1; return; }   /* the rest wait a
                                                 * millisecond */
            if (sb_keys(st, d1, d2)) sbx.rev++;
            else web_midi(st, d1, d2);
            sbx.st.midi++;
        } else if (e->kind == TXP_EV_PARAM) {
            /* the engines' queue (lanes.c LN_QN) half full: the rest wait for the next step. A value can send a lane
             * several events (a SOUND: its engine, PRESET, TYPE, its cards), and the app drops what finds the queue
             * full (scape.c sc_sync counts it sent): a host's flush of every parameter, 16 lanes' worth, would lose
             * a lane's PRESET, its source never playing the sound's own engine. Scene loads wait the same way */
            if (ln.qw - ln.qr >= LN_QN / 2u) { sbx.st.ring_wait++; sbx.ev_wait = 1; return; }
            sb_param_set(e->a, e->v);
            sbx.st.params++; sbx.rev++;
        } else if (e->kind == TXP_EV_MUTE) {   /* (as a black key held on the FM-1: ui.c ui_lane_mute) */
            if (e->a < SC_N && sc.snd[e->a] >= 0 && sc.mute[e->a] != (e->v != 0)) { sc_mute(e->a, e->v != 0); scn_touch(); }
            sbx.rev++;
        } else if (e->kind == TXP_EV_SOLO) {
            sbx.solo = (uint16_t)e->v;
            sbx.rev++;
        } else if (e->kind == TXP_EV_SOUND) {   /* (as the FM-1's sound browser: an audition, or EDIT keeping it) */
            uint32_t f = (uint32_t)(e->v >> 4) & 0xFFu, i = (uint32_t)e->v & 15u;
            if (ln.qw - ln.qr >= LN_QN / 2u) { sbx.st.ring_wait++; sbx.ev_wait = 1; return; }   /* (as a parameter) */
            if (e->a < SC_N && f < LS_N && i < LS[f].n) {
                sc_sound_set(e->a, f, i);
                if (e->v & TXP_SND_KEEP) scn_touch();
            }
            sbx.rev++;
        } else if (e->kind == TXP_EV_LOAD) {   /* (as the FM-1's presets list: EDIT) */
            if (e->v >= TXP_LOAD_LS && (uint32_t)(e->v - TXP_LOAD_LS) < LS_N) ui_land_go((uint32_t)(e->v - TXP_LOAD_LS));
            else if (e->v >= 0 && (uint32_t)e->v < SCN_N && ((uint32_t)e->v < SCN_NF || scn_meta[e->v - SCN_NF].used))
                scn_pb_go((uint32_t)e->v, e->a & 1u);
            sbx.rev++;
        } else if (e->kind == TXP_EV_SEL) {   /* (as a black key: the lane the keys and SELECTED CH play) */
            if (e->a < SC_N) sc_select(e->a);
            sbx.rev++;
        } else if (e->kind == TXP_EV_REC) {   /* (as the FM-1's REC pressed once: ui.c ui_rec, without its taps; the
                                          * selected lane kept: the FM-1's landscape start picks lane 1) */
            int run = sc_any_run();
            uint8_t sel = sc.sel;
            uint8_t mc[SC_N], ms[SC_N];
            memcpy(mc, sc.mem_cnt, sizeof mc); memcpy(ms, sc.mem_step, sizeof ms);
            sc_all_toggle();
            if (run)                     /* (STOP ALL: a lane its own STOP had stopped keeps what that remembered) */
                for (uint32_t k = 0; k < SC_N; k++)
                    if (!sc.run[k] && mc[k] && sc.mem_cnt[k] == 0) { sc.mem_cnt[k] = mc[k]; sc.mem_step[k] = ms[k]; }
            if (sel < SC_N && sc.snd[sel] >= 0) sc.sel = sel;
            scn_touch();
            (void)tempo_transport(!run);
            sbx.rev++;
        } else if (e->kind == TXP_EV_LANERUN) {   /* (STOP ALL / PLAY ALL for one lane: sc_all_toggle's own steps) */
            uint32_t k = e->a;
            if (k < SC_N && sc.snd[k] >= 0) {
                if (!e->v && sc.run[k]) {
                    sc.mem_cnt[k] = (uint8_t)sc_cnt_of(k); sc.mem_step[k] = (uint8_t)sc_step_of(k);
                    sc_count(k, 0);
                    scn_touch();
                } else if (e->v && !sc.run[k]) {
                    static const uint8_t C[3] = {2, 1, 1};   /* (sc_start_landscape's: 2 / 1 / 1, the rarest ACTIVE) */
                    if (sc.mem_cnt[k]) { sc_count(k, sc.mem_cnt[k]); sc_step(k, sc.mem_step[k]); }
                    else { sc_count(k, k < 3u ? C[k] : 1u); sc_step(k, sc_act_step(0)); }
                    scn_touch();
                }
            }
            sbx.rev++;
        } else if (e->kind == TXP_EV_UNDO) {   /* (as SAVE held: scenes.c) */
            if (scn_su.on) scn_snd_undo(); else scn_undo();
            sbx.rev++;
        } else if (e->kind == TXP_EV_NAME) {   /* (four characters, the save mode's alphabet: others a space) */
            for (uint32_t i = 0; i < 4u && e->a + i < 11u; i++) {
                char ch = (char)((uint32_t)e->v >> (8u * i) & 0xFFu);
                if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
                sbx.save_nm[e->a + i] = ch && strchr(SCN_CHARS, ch) ? ch : ' ';
            }
        } else if (e->kind == TXP_EV_SAVE) {   /* (as the save mode's SAVE: scenes.c scn_save_start) */
            if (e->a < SCN_NU && !scn_sv_on && !scn_busy()) {
                uint32_t i;
                for (i = 0; i < 11u; i++) scn_nm[i] = sbx.save_nm[i] ? sbx.save_nm[i] : ' ';
                scn_save_start(e->a);
                memset(sbx.save_nm, 0, sizeof sbx.save_nm);
            }
            sbx.rev++;
        } else if (e->kind == TXP_EV_TEMPO) {   /* (the app's own: the host's leads, sb_tempo) */
            if (e->v >= 400 && e->v <= 2400) { (void)tempo_set10((uint32_t)e->v); scn_touch(); }
            sbx.rev++;
        } else if (e->kind == TXP_EV_PREVIEW) {   /* (the sound browser's preview, sb_pv_play / sb_pv_end) */
            uint32_t f = (uint32_t)(e->v >> 4) & 15u, i = (uint32_t)e->v & 15u;
            if (ln.qw - ln.qr >= LN_QN / 2u) { sbx.st.ring_wait++; sbx.ev_wait = 1; return; }   /* (as a parameter) */
            if (e->v & TXP_PV_PLAY)
                sb_pv_play(e->a, f, i, (uint32_t)(e->v >> 8) & 15u, (uint32_t)(e->v >> 12) & 15u, (e->v & TXP_PV_FORCE) != 0);
            else if (e->v & TXP_PV_END) sb_pv_end(e->a, (e->v & TXP_PV_KEEP) != 0, (e->v & TXP_PV_VALS) != 0, f, i);
            sbx.rev++;
        } else if (e->kind == TXP_EV_RESET || e->kind == TXP_EV_RELEASE) sb_notes_off();
        atomic_store_explicit(&sbx.qr, r + 1u, memory_order_release);
    }
}
static void sb_tempo(void)               /* the host's tempo leads (none given: the app's own) */
{
    uint32_t h = atomic_load_explicit(&sbx.tempo10, memory_order_relaxed);
    if (h < 200u || h > 9990u) return;
    h = h < 400u ? 400u : h > 2400u ? 2400u : h;
    if (tm.clock != CLK_FOLLOW && !clk_follows() && tempo_bpm10() != h) (void)tempo_set10(h);
}
/* the SOLO: while lanes are soloed, every other lane is shut at the mix (lanes.c ln_gate, as a mute), the soloed ones
 * open unless muted; the app's own mute (scape.c sc_sync) sends its gate only when the mute changes, so after any
 * gate the app sent, this one's goes again. Solo off: every lane's gate back to its own mute, then nothing more is
 * sent (the app alone, as on the FM-1). After each pass of the app (its sends first) */
static void sb_solo(void)
{
    uint32_t k, live = (uint32_t)sbx.solo & ((1u << sc_nlane) - 1u);
    if (live && sbp.on) live |= 1u << sbp.k;   /* (the sound browser's preview: heard) */
    if (!live && !sbx.solo_was) return;
    for (k = 0; k < SC_N; k++) {
        uint8_t open = (uint8_t)(!sc.mute[k] && (!live || (live >> k & 1u)));
        if (sbx.solo_was && live && open == sbx.gopen[k] && sc.a_gate[k] == sbx.agseen[k]) continue;
        if (ln_gate(k, open) < 0) return;  /* (the queue full: the next pass) */
        sbx.gopen[k] = open; sbx.agseen[k] = sc.a_gate[k];
    }
    sbx.solo_was = live != 0;
}
static uint8_t sb_pk8(int32_t pk)        /* a Q23 peak: 0 silent, 1 .. 255 = -72 .. 0 dBFS */
{
    float db;
    if (pk <= 0) return 0;
    db = 20.0f * log10f((float)pk * (1.0f / 8388608.0f));
    if (db <= -72.0f) return 0;
    return (uint8_t)(db >= 0.0f ? 255.0f : 1.0f + (db + 72.0f) * (254.0f / 72.0f));
}
static void sb_act_put(void)             /* after each app block: each lane's events and peak */
{
    uint32_t n = atomic_load_explicit(&sbx.act_n, memory_order_relaxed), w = n & (TXP_ACT_N - 1u), k;
    for (k = 0; k < SC_N; k++) {
        uint32_t e = ln.mix.e[k] ? ln.lev[k] : 0u;
        sbx.act_ev[w][k] = (uint8_t)(e > 255u ? 255u : e);
        sbx.act_pk[w][k] = sb_pk8(ln.lpk[k]);
    }
    atomic_store_explicit(&sbx.act_n, n + 1u, memory_order_release);
}
static uint32_t sb_act(uint8_t (*ev)[TXP_LANES], uint8_t (*pk)[TXP_LANES], uint32_t n)
{
    uint32_t c = atomic_load_explicit(&sbx.act_n, memory_order_acquire), i;
    if (n > TXP_ACT_N / 2u) n = TXP_ACT_N / 2u;   /* (half the ring: the writer is never on what is read) */
    for (i = 0; i < n; i++) {
        uint32_t b = c - n + i;
        if ((int32_t)b < 0 || b >= c) { memset(ev[i], 0, TXP_LANES); memset(pk[i], 0, TXP_LANES); continue; }
        memcpy(ev[i], sbx.act_ev[b & (TXP_ACT_N - 1u)], TXP_LANES);
        memcpy(pk[i], sbx.act_pk[b & (TXP_ACT_N - 1u)], TXP_LANES);
    }
    return c;
}
/* a scene just applied (a load, the project's state, an undo): its lanes it keeps stopped with a HOW MANY of their own
 * (saved after STOP ALL) remembered as STOP ALL remembers them, so PLAY ALL brings each one back as it was (the app
 * clears that memory on a load: the FM-1's REC then starts the landscape's first three lanes instead) */
static void sb_scene_mem(void)
{
    uint32_t k;
    if (!scn_ap_stage || (scn_ap == sbx.ap_seen && scn_ap_t == sbx.ap_seen_t)) return;
    sbx.ap_seen = scn_ap; sbx.ap_seen_t = scn_ap_t;
    for (k = 0; k < SC_N; k++)
        if (scn_ap->snd[k] >= 0 && sc.snd[k] >= 0 && !scn_ap->run[k] && !sc.run[k] && scn_ap->cnt[k] && !sc.mem_cnt[k]) {
            sc.mem_cnt[k] = (uint8_t)(scn_ap->cnt[k] < LS_NCNT ? scn_ap->cnt[k] : LS_NCNT - 1u);
            sc.mem_step[k] = (uint8_t)(scn_ap->step[k] < 1u ? 1u : scn_ap->step[k] > SC_STEPS ? SC_STEPS : scn_ap->step[k]);
        }
}
static void sb_pass(void)                /* one pass of the app: the FM-1 keys as held now, then those let go */
{
    fm1_in.notes = sbx.kmask;
    coro_resume(&sbx.co);
    sbx.kmask &= ~sbx.koff;
    sbx.koff = 0;
    sb_solo();
    sb_scene_mem();
    sb_pv_cards();
}
static void sb_step(void)
{
    sbx.ms++;
    sbx.st.steps++;
    sb_events(1);
    sb_state_apply();
    sb_scene_mem();
    if (sbx.exact) sb_pass();            /* (exact: the events just handed over go into this step's block) */
    sbx.owed += TXP_FS;
    while (sbx.owed >= 1000u * HALF_FRAMES) {
        static int32_t blk[2u * HALF_FRAMES];
        uint32_t i;
        sbx.owed -= 1000u * HALF_FRAMES;
        ln.mix.ms.meter |= 2u;           /* (the M/S's meters: the window's, always; they change no sample) */
        app_audio(blk, HALF_FRAMES);     /* (APP_OUT_WRITTEN: every sample written) */
        for (i = 0; i < TXP_GON_N; i++) {   /* (the goniometer's points: the block as it leaves, Q15) */
            int32_t l = blk[2u * i * TXP_GON_STEP], r = blk[2u * i * TXP_GON_STEP + 1u];
            sbx.gon[i][0] = (int16_t)(l > 32767 ? 32767 : l < -32767 ? -32767 : l);
            sbx.gon[i][1] = (int16_t)(r > 32767 ? 32767 : r < -32767 ? -32767 : r);
        }
        sbx.gon_blk = sbx.st.blocks + 1u;
        for (i = 0; i < HALF_FRAMES; i++) {
            uint32_t w = (sbx.fw + i) & (SB_FIFO - 1u);
            int32_t a = blk[2u * i] < 0 ? -blk[2u * i] : blk[2u * i], b = blk[2u * i + 1u] < 0 ? -blk[2u * i + 1u] : blk[2u * i + 1u];
            sbx.fifo[2u * w] = blk[2u * i];
            sbx.fifo[2u * w + 1u] = blk[2u * i + 1u];
            if (a > sbx.opk) sbx.opk = a;
            if (b > sbx.opk) sbx.opk = b;
        }
        {   /* the compressor's meters: read off its own state (tx_core_comp.h), the block's most */
            const tx_comp_t *cp = &ln.mix.comp;
            if (cp->on && pv_glob[PG_COMP0 + TX_CP_ON]) {
                float det = (float)(cp->ef > cp->es ? cp->ef : cp->es), gn = (float)cp->g * (1.0f / 16384.0f);
                if (det > sbx.cin_pk) sbx.cin_pk = det;
                if (det * gn * gn > sbx.cout_pk) sbx.cout_pk = det * gn * gn;
                if (cp->gr > sbx.cgr_pk) sbx.cgr_pk = cp->gr;
            }
        }
        sbx.fw += HALF_FRAMES;
        sbx.st.blocks++;
        sb_act_put();
    }
    if (!sbx.exact) sb_pass();
    sb_tempo();
    if (sbx.rev != sbx.pub_rev || ++sbx.pub_n >= SB_PUB_EVERY) sb_publish();
    if (atomic_load_explicit(&sbx.cap_req, memory_order_acquire)) { atomic_store(&sbx.cap_req, 0); sb_capture(); }
}
static void sb_settle(void)              /* main thread, not processing: run until a load is done; the audio dropped */
{
    uint32_t n;
    for (n = 0; n < SB_SETTLE_MS; n++) {
        if (n >= 2u && atomic_load(&sbx.st_flag) == 0 && !sbx.ev_wait && sb_is_settled()) break;
        sb_step();
    }
    sbx.fr = sbx.fw;
    sb_publish();
    sb_capture();
}
static int sb_boot(void)
{
    const char *test = getenv("TEXTURA_PLUGIN_TEST");
    if (sbx.booted) return 0;
    sbx.stack = aligned_alloc(16, SB_STACK);
    if (!sbx.stack) return -1;
    memset(sbx.stack, 0, SB_STACK);       /* (every page touched now, not on the audio thread) */
    if (!sbx.ntab) sb_tab_make();
    sb_eng_list();
    sbx.last_fact = 0;
    coro_init(&sbx.co, sbx.stack, SB_STACK, sb_entry, 0);
    coro_resume(&sbx.co);                 /* app_main up to its first poll */
    sbx.booted = 1;
    if (!(test && !strcmp(test, "ms"))) sb_settle();   /* (the boot's scene loaded: the values a host reads at once
                                          * are its; the ms test mode starts from the boot itself, as tests/host.c) */
    else { sb_publish(); sb_capture(); }
    if (sb_state_get(sbx.boot_state, (int)sizeof sbx.boot_state, 0) < 0) sbx.boot_state[0] = 0;
    return 0;
}
static int sb_booted(void) { return sbx.booted; }
static void sb_drain(void)
{
    if (!sbx.booted) return;
    sb_events(0);
    sb_state_apply();
    sb_solo();
    sb_publish();
    sb_capture();
}
static void sb_reuse(void)               /* as booted again: the boot's state, the last instance's events dropped */
{
    if (!sbx.booted) return;
    atomic_store(&sbx.qr, atomic_load(&sbx.qw));
    sb_notes_off();
    if (sbx.boot_state[0]) (void)sb_state_set(sbx.boot_state);
    sb_settle();
    sbx.rev++;
    sb_publish();
}
static uint32_t sb_avail(void) { return sbx.fw - sbx.fr; }
static void sb_read(int32_t *lr, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = (sbx.fr + i) & (SB_FIFO - 1u);
        lr[2u * i] = sbx.fifo[2u * r];
        lr[2u * i + 1u] = sbx.fifo[2u * r + 1u];
    }
    sbx.fr += n;
}
static uint32_t sb_now(void) { return sbx.ms; }
static void sb_set_tempo(uint32_t bpm10) { atomic_store_explicit(&sbx.tempo10, bpm10, memory_order_relaxed); }
static void sb_stats(txp_stats_t *out) { *out = sbx.st; out->ms = sbx.ms; }
