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
 * The host's parameters (sb_params): the app's own, by their stable ids (params.c), the same for every lane: SOUND,
 * HOW MANY, HOW ACTIVE, LEVEL, PAN, the eight cards (0 .. 127 onto the card's range, as their CC: what a card turns
 * depends on the lane's sound), GROOVE's PART and CYCLE; the effects on the mix (GRAIN DELAY and its SYNC, REVERB,
 * COMPRESSOR, DRIVE, FILTER), GROOVE, the keys' SCALE, MIDI IN's channels, EFFECTS ON LOAD; the eight LFOs, the
 * envelopes, the followers, the matrix and MOD CC; and the plugin's own PRESET (F001 .. F100, a program change on
 * channel 1). The FM-1's tempo, clock, screen and status settings are not host parameters: the host's tempo leads
 * (step 6) when it gives one.
 *
 * The state: what a user scene keeps (scenes.c scn_capture: the landscape, each lane's sound, HOW MANY, HOW ACTIVE,
 * level, pan, mute and cards, the effects, the modulation, GROOVE, the tempo), the scene loaded last, and the settings
 * that are parameters (the keys' SCALE, MIDI IN, EFFECTS ON LOAD, PRESETS CHANGE TEMPO). Loaded, it is applied as a
 * scene with its effects (scn_apply, scn_fx_force); outside processing the slot runs until the load is done
 * (sb_settle), so the values read back at once are the state's. Nothing is written to disk: the user scenes' bank
 * stays in memory (the FM-1's SAVE has no key here yet). */
#include <stdatomic.h>
#include <time.h>
#include "coro/coro.h"

#define SB_STACK (256u * 1024u)          /* the coroutine's stack (app_main and what it calls) */
#define SB_FIFO 2048u                    /* frames (power of two); the instance never asks more than 1024 ahead */
#define SB_Q 4096u                       /* the event queue (power of two) */
#define SB_HOST_RING 64u                 /* web_hal.c's mq[] */
#define SB_PUB_EVERY 8u                  /* steps between two publications of the values */
#define SB_SETTLE_MS 6000u               /* sb_settle's longest run (scenes.c SCN_STUCK_MS) */
#define SB_NSET 11u                      /* the settings in the state */
#define SB_SCN_HEX (2u * sizeof(scn_t))
_Static_assert(SB_SCN_HEX + 512u < TXP_STATE_MAX, "the state fits bridge.h TXP_STATE_MAX");

typedef struct { uint8_t kind, x; uint32_t at, a; int32_t v; } sb_ev_t;   /* x: at is an app frame x 256 (exact) */
static const uint8_t SB_SET_IDX[SB_NSET] = { PG_SCNFX, PG_SCNTEMPO, PG_SCMODE, PG_SCALE, PG_ROOT, PG_TRANS, PG_SELCH,
                                             PG_LCH0, PG_LCH0 + 1, PG_LCH0 + 2, PG_LCH0 + 3 };
typedef char sb_set_lanes[SC_N == 4u ? 1 : -1];   /* (SB_SET_IDX names four lanes' channels) */

static struct {
    coro_t co;
    void *stack;
    int booted;
    uint32_t ms, owed;
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
} sbx;

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
    static const char *const LL[] = {"SOUND", "HOW MANY", "HOW ACTIVE", "LEVEL", "PAN"};
    uint32_t k, i, f, smax = 0;
    char mod[24], pfx[16], nm[24];
    sbx.ntab = 0;
    for (f = 0; f < LS_N; f++) if (LS[f].n > smax) smax = LS[f].n;
    sb_tab_add(TXP_ID_PRESET, PR_N - 1u, 0, 1, "Scenes", "", "PRESET");
    for (k = 0; k < SC_N; k++) {
        snprintf(mod, sizeof mod, "Lane %u", (unsigned)k + 1u);
        snprintf(pfx, sizeof pfx, "L%u ", (unsigned)k + 1u);
        for (i = 0; i <= PL_PAN; i++) {
            uint32_t id = PID(k, i), hi = i == PL_SOUND ? smax - 1u : (uint32_t)p_hi(id);
            sb_tab_add(id, hi, 0, i == PL_SOUND || i == PL_COUNT || i == PL_ACTIVE, mod, pfx, LL[i]);
        }
        for (i = 0; i < SCD_MAX; i++) {
            snprintf(nm, sizeof nm, "CARD %u", (unsigned)i + 1u);
            sb_tab_add(PID(k, PL_CARD0 + i), 127u, 1, 0, mod, pfx, nm);
        }
        sb_tab_add(PID(k, PL_GPART), (uint32_t)p_hi(PID(k, PL_GPART)), 0, 1, mod, pfx, "GROOVE PART");
        sb_tab_add(PID(k, PL_GCYC), (uint32_t)p_hi(PID(k, PL_GCYC)), 0, 1, mod, pfx, "GROOVE CYCLE");
    }
    for (i = 0; i < 256u; i++) {
        uint32_t id = PID(P_GLOBAL, i);
        const char *m, *px;
        if (sb_glob_hidden(i) || !(p_fx_desc(id) || p_gx(id))) continue;
        if (i < PG_REVERB0) { m = "Grain delay"; px = "GRAIN "; }
        else if (i < PG_COMP0) { m = "Reverb"; px = "REVERB "; }
        else if (i < PG_BPM) { m = "Compressor"; px = "COMP "; }
        else if (i >= PG_DRIVE0 && i < PG_MF0) { m = "Drive"; px = "DRIVE "; }
        else if (i >= PG_MF0 && i < PG_GRV0) { m = "Filter"; px = "FILTER "; }
        else if (i >= PG_GRV0 && i <= PG_GSHIFT) { m = "Groove"; px = "GROOVE "; }
        else if (i == PG_SCALE || i == PG_ROOT || i == PG_SCMODE || i == PG_TRANS) { m = "Keys"; px = "KEYS "; }
        else if (i == PG_SELCH || (i >= PG_LCH0 && i < PG_LCH0 + SC_N)) { m = "MIDI"; px = "MIDI "; }
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
}
static uint32_t sb_params(txp_param_t *tab, uint32_t max)
{
    uint32_t n;
    if (!sbx.ntab) sb_tab_make();         /* (constant tables only: any thread, before the boot too) */
    n = sbx.ntab < max ? sbx.ntab : max;
    memcpy(tab, sbx.tab, n * sizeof *tab);
    return n;
}
static int sb_is_card(uint32_t id) { return P_SCOPE(id) < SC_N && P_IDX(id) >= PL_CARD0 && P_IDX(id) < PL_CARD0 + SCD_MAX; }
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
    if (P_MODS(s) || s > P_GLOBAL) return -1;
    if (s < SC_N && !(i == PL_COUNT || i == PL_ACTIVE || i == PL_PAN || i == PL_GPART || i == PL_GCYC)) return -1;
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
    if (P_SCOPE(p->id) < SC_N && P_IDX(p->id) == PL_COUNT) {   /* HOW MANY under a modulation: the user's value, as a
                                          * scene keeps it (scenes.c scn_capture), not the one the matrix moves it to */
        uint32_t k = P_SCOPE(p->id);
        if (m_cb_ok[k] && sc.run[k] && m_cbase[k]) return m_cbase[k];
    }
    if (!p->scaled) return p_get(p->id);
    if (!p_valid(p->id)) return 0;       /* (a card the lane's sound has not) */
    v = p_get(p->id); h = p_hi(p->id);
    {   /* a card the user has not set: its default. On the FM-1's page a hosted engine's card shows the engine's value
         * now, which the sound's own curves move (params.c p_card_get); to the host that would be a parameter moving
         * by itself, and a second instance loading the same state would read another value */
        p_cards_t *c = p_cards(P_SCOPE(p->id));
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
    if (id >= TXP_ID_OWN0 || !p_valid(id)) return;
    if (sb_is_card(id)) p_midi_set(id, p_scale7(id, (uint32_t)(v < 0 ? 0 : v > 127 ? 127 : v)));
    else p_midi_set(id, v);
}

/* ---- published values (a sequence lock: the audio side writes, any thread reads) */
static int sb_is_settled(void)
{
    uint32_t k;
    if (scn_busy() || scn_gl_n || scn_cand != 0xFF) return 0;
    for (k = 0; k < SC_N; k++) if (sc.gl_on[k] || sc.a_eng[k] != sc.eng[k] || sc.reswap[k]) return 0;
    return 1;
}
static void sb_publish(void)
{
    uint32_t q = atomic_load_explicit(&sbx.pub_seq, memory_order_relaxed), i;
    if (!sbx.ntab) sb_tab_make();
    if (scn_cur != 0xFF && scn_cur < SCN_NF) sbx.last_fact = scn_cur;
    atomic_store_explicit(&sbx.pub_seq, q + 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    for (i = 0; i < sbx.ntab; i++) sbx.pub.pv[i] = (int16_t)sb_value(&sbx.tab[i]);
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
static void sb_capture(void)             /* the state as it plays now, for sb_state_get */
{
    uint32_t q = atomic_load_explicit(&sbx.cap_seq, memory_order_relaxed), i;
    atomic_store_explicit(&sbx.cap_seq, q + 1u, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    scn_capture(&sbx.cap_scn);
    sbx.cap_cur = scn_cur;
    for (i = 0; i < SB_NSET; i++) sbx.cap_set[i] = (int16_t)p_get(PID(P_GLOBAL, SB_SET_IDX[i]));
    atomic_store_explicit(&sbx.cap_seq, q + 2u, memory_order_release);
}

/* ---- the state: {"textura":"1","cur":"N","set":"I=V,...","scene":"<scn_t, hex>"} (the scene record as the FM-1's
 * user bank keeps it, scenes.c scn_t: its version inside; a later firmware's records load as a scene of before does) */
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
static int sb_state_get(char *buf, int n, int fresh)   /* fresh: ask the audio side for a capture first (processing) */
{
    scn_t s;
    uint8_t cur;
    int16_t set[SB_NSET];
    char *o = buf;
    uint32_t i;
    if (n < (int)(SB_SCN_HEX + 512u)) return -1;
    if (fresh) {                         /* the next step captures (a host that stops calling process: the last one) */
        uint32_t c0 = atomic_load(&sbx.cap_seq);
        struct timespec ts = { 0, 1000000 };
        atomic_store(&sbx.cap_req, 1);
        for (i = 0; i < 200u && atomic_load(&sbx.cap_seq) == c0; i++) nanosleep(&ts, 0);
    }
    for (;;) {
        uint32_t q1 = atomic_load_explicit(&sbx.cap_seq, memory_order_acquire), q2;
        if (q1 & 1u) continue;
        s = sbx.cap_scn; cur = sbx.cap_cur; memcpy(set, sbx.cap_set, sizeof set);
        atomic_thread_fence(memory_order_acquire);
        q2 = atomic_load_explicit(&sbx.cap_seq, memory_order_relaxed);
        if (q1 == q2) break;
    }
    o += sprintf(o, "{\"textura\":\"1\",\"cur\":\"%u\",\"set\":\"", (unsigned)cur);
    for (i = 0; i < SB_NSET; i++) o += sprintf(o, "%s%u=%d", i ? "," : "", (unsigned)PID(P_GLOBAL, SB_SET_IDX[i]), set[i]);
    o += sprintf(o, "\",\"scene\":\"");
    for (i = 0; i < sizeof s; i++) { *o++ = SB_HEX[((const uint8_t *)&s)[i] >> 4]; *o++ = SB_HEX[((const uint8_t *)&s)[i] & 15u]; }
    o += sprintf(o, "\"}");
    return (int)(o - buf);
}
static int sb_hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
static int sb_state_set(const char *j)   /* into the mailbox; sb_state_apply applies it between two passes */
{
    uint32_t lv = 0, ls = 0, lc = 0, lt = 0, i;
    const char *v = j ? sb_json_str(j, "textura", &lv) : 0, *s = j ? sb_json_str(j, "scene", &ls) : 0;
    const char *c = j ? sb_json_str(j, "cur", &lc) : 0, *t = j ? sb_json_str(j, "set", &lt) : 0;
    int expect = 0;
    scn_t x;
    if (!v || lv != 1u || v[0] != '1' || !s || ls != SB_SCN_HEX) { sbx.st.state_bad++; return -1; }
    for (i = 0; i < sizeof x; i++) {
        int a = sb_hexv(s[2u * i]), b = sb_hexv(s[2u * i + 1u]);
        if (a < 0 || b < 0) { sbx.st.state_bad++; return -1; }
        ((uint8_t *)&x)[i] = (uint8_t)(a << 4 | b);
    }
    if (x.ver != SCN_VER || x.nrec > SCN_REC_MAX) { sbx.st.state_bad++; return -1; }
    while (!atomic_compare_exchange_weak(&sbx.st_flag, &expect, 1)) {
        if (expect == 3) { sbx.st.state_bad++; return -1; }   /* (being applied right now: the host may retry) */
        if (expect != 0 && expect != 2) expect = 0;
    }
    sbx.st_scn = x;
    sbx.st_cur = 0xFF;
    if (c && lc > 0 && lc < 4) { uint32_t n = 0; for (i = 0; i < lc; i++) n = n * 10u + (uint32_t)(c[i] - '0'); if (n < SCN_N) sbx.st_cur = (uint8_t)n; }
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
static void sb_state_apply(void)
{
    int expect = 2;
    uint32_t i;
    if (atomic_load(&sbx.st_flag) != 2 || scn_busy()) return;   /* (a load running reads its own scene: after it) */
    if (!atomic_compare_exchange_strong(&sbx.st_flag, &expect, 3)) return;
    for (i = 0; i < SB_NSET; i++) if (sbx.st_set[i] >= 0) p_set(PID(P_GLOBAL, SB_SET_IDX[i]), sbx.st_set[i]);
    sbx.ap_scn = sbx.st_scn;
    scn_cand = 0xFF;                     /* (a program change still waiting: the state comes after it) */
    scn_fx_force = 1;                    /* (its effects too, whatever EFFECTS ON LOAD says) */
    for (i = 0; i < sbx.ap_scn.nrec; i++) /* (its tempo: whatever PRESETS CHANGE TEMPO says; the host's leads) */
        if (sbx.ap_scn.r[i].id == PID(P_GLOBAL, PG_FINE)) (void)tempo_set10((uint32_t)(sbx.ap_scn.r[i].v + 400));
    scn_apply(&sbx.ap_scn, sbx.st_cur);
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
static void sb_notes_off(void) { while (nt_mn) nt_mq_off(0); }   /* every MIDI note let go (notes.c) */
/* apply the queued events; timed: only those due by sbx.ms, an exact one only for the next block to render (and a
 * full MIDI ring makes the rest wait a millisecond) */
static void sb_events(int timed)
{
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
            if (mq_w - mq_r >= SB_HOST_RING) { sbx.st.ring_wait++; return; }   /* the rest wait a millisecond */
            web_midi(st, d1, d2);
            sbx.st.midi++;
        } else if (e->kind == TXP_EV_PARAM) {
            sb_param_set(e->a, e->v);
            sbx.st.params++; sbx.rev++;
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
static void sb_step(void)
{
    sbx.ms++;
    sbx.st.steps++;
    sb_events(1);
    sb_state_apply();
    if (sbx.exact) coro_resume(&sbx.co);   /* (exact: the events just handed over go into this step's block) */
    sbx.owed += TXP_FS;
    while (sbx.owed >= 1000u * HALF_FRAMES) {
        static int32_t blk[2u * HALF_FRAMES];
        uint32_t i;
        sbx.owed -= 1000u * HALF_FRAMES;
        app_audio(blk, HALF_FRAMES);     /* (APP_OUT_WRITTEN: every sample written) */
        for (i = 0; i < HALF_FRAMES; i++) {
            uint32_t w = (sbx.fw + i) & (SB_FIFO - 1u);
            sbx.fifo[2u * w] = blk[2u * i];
            sbx.fifo[2u * w + 1u] = blk[2u * i + 1u];
        }
        sbx.fw += HALF_FRAMES;
        sbx.st.blocks++;
    }
    if (!sbx.exact) coro_resume(&sbx.co);
    sb_tempo();
    if (sbx.rev != sbx.pub_rev || ++sbx.pub_n >= SB_PUB_EVERY) sb_publish();
    if (atomic_load_explicit(&sbx.cap_req, memory_order_acquire)) { atomic_store(&sbx.cap_req, 0); sb_capture(); }
}
static void sb_settle(void)              /* main thread, not processing: run until a load is done; the audio dropped */
{
    uint32_t n;
    for (n = 0; n < SB_SETTLE_MS; n++) {
        if (n >= 2u && atomic_load(&sbx.st_flag) == 0 && sb_is_settled()) break;
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
