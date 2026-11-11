/* SPDX-License-Identifier: GPL-3.0-only */
/* The keys and their LEDs. The FM-1 has 27 keys, F3 .. G5 (key k = MIDI 53 + k). Two modes, SEL switches:
 *
 *   LANDSCAPE  as the audition player's lane mode:
 *                the black keys      by their groups (3, 2, 3, 2, 1): group n selects lane n, shown on LANDSCAPE (ui.c
 *                                    ui_lane_focus); held NT_HOLD_MS (as PLAY's hold) it also mutes the lane, or
 *                                    unmutes it (scape.c sc_mute: the lane plays on, only its output is gated, so it
 *                                    comes back at once); F#5 (the fifth) nothing. The sounds are picked in the sound
 *                                    browser (SEQ, ui.c), not by a key;
 *                F3 .. E4 (7 white)  HOW MANY of the selected lane: off, 1, 2, 4, 10, 19, 32 (each sounds the same more);
 *                F4 .. G5 (9 white)  HOW ACTIVE: rare .. busy (steps 1 .. 14, G5 the 14th).
 *              No key changes the landscape (ALGORITHM does, on its page).
 *              LEDs: a lane's black keys blink while it plays (1.6 s rare .. 0.12 s busy), glow steadily while it
 *              plays muted, and glow when it is selected but stopped; the white keys show the selected lane's two bars.
 *   PLAY       the keys play the selected lane, pitched (OCT- / OCT+ the octave); the LEDs show the held keys.
 *
 * Main loop only. */

#define NT_VEL 100u                             /* the keys' velocity (keyvel.c later) */
#define NT_SPAN TX_NKEYS                        /* F3 .. G5 */
#define NT_HOLD_MS 600u                         /* a black key held this long: its lane muted / unmuted (ui.c TAP_HOLD_MS's) */
static const uint8_t NT_GROUP[11] = {0, 0, 0, 1, 1, 2, 2, 2, 3, 3, 0xFF};   /* the n-th black key's lane (F#5: none) */
static void ui_lane_focus(uint32_t k);          /* (ui.c) */
static void ui_lane_mute(uint32_t k);

enum { NT_LANDSCAPE, NT_PLAY };
static struct {
    uint8_t mode;
    int8_t oct;                                 /* PLAY: -2 .. +2 */
    uint32_t down;                              /* the keys as last seen */
    uint8_t note[TX_NKEYS], lane[TX_NKEYS];     /* PLAY: what each held key sounds */
    uint32_t playing;                           /* PLAY: keys with a note on */
    uint8_t hold_k;                             /* LANDSCAPE: the black key held, for its mute (NT_SPAN: none) */
    uint32_t hold_t;
} nt = {.hold_k = NT_SPAN};

static int nt_black(uint32_t k) { uint32_t n = (TX_KEY_F3 + k) % 12u; return n == 1u || n == 3u || n == 6u || n == 8u || n == 10u; }
/* key k (< NT_SPAN) as the n-th black or the n-th white key */
static uint32_t nt_index(uint32_t k)
{
    uint32_t i, n = 0;
    for (i = 0; i < k; i++) if (nt_black(i) == nt_black(k)) n++;
    return n;
}
static uint32_t nt_key_of(int black, uint32_t n)   /* the inverse: the n-th black / white key (NT_SPAN: none) */
{
    uint32_t k;
    for (k = 0; k < NT_SPAN; k++) if (nt_black(k) == black && nt_index(k) == n) return k;
    return NT_SPAN;
}

static void nt_play_off(void)                   /* PLAY: every held note released */
{
    uint32_t k;
    for (k = 0; k < TX_NKEYS; k++) if (nt.playing >> k & 1u) ln_key(nt.lane[k], nt.note[k], 0, 0);
    nt.playing = 0;
}
static void nt_mode_set(uint32_t m)
{
    if (m == nt.mode) return;
    nt_play_off();
    nt.mode = (uint8_t)m;
}
static void nt_oct_step(int32_t d) { int32_t o = nt.oct + d; nt.oct = (int8_t)(o < -2 ? -2 : o > 2 ? 2 : o); }

static void scn_touch(void);
static void nt_landscape_key(uint32_t k)
{
    uint32_t n;
    if (k >= NT_SPAN) return;
    n = nt_index(k);
    if (nt_black(k)) {
        if (n < sizeof NT_GROUP && NT_GROUP[n] < SC_N) { ui_lane_focus(NT_GROUP[n]); nt.hold_k = (uint8_t)k; nt.hold_t = fm1_ms; }
        return;
    }
    if (sc.snd[sc.sel] < 0) return;
    scn_touch();                                /* (the scene edited: scenes.c) */
    if (n < LS_NCNT) sc_count(sc.sel, n);
    else if (n - LS_NCNT < LS_NACT) sc_step(sc.sel, sc_act_step(n - LS_NCNT));
}
static void nt_poll(void)
{
    uint32_t now = fm1_in.notes & ((1u << TX_NKEYS) - 1u), on = now & ~nt.down, off = nt.down & ~now, k;
    nt.down = now;
    if (nt.hold_k < NT_SPAN && off >> nt.hold_k & 1u) nt.hold_k = NT_SPAN;   /* (let go before the hold: a select only) */
    for (k = 0; k < TX_NKEYS; k++) {
        if (off >> k & 1u && nt.playing >> k & 1u) { ln_key(nt.lane[k], nt.note[k], 0, 0); nt.playing &= ~(1u << k); }
        if (!(on >> k & 1u)) continue;
        if (nt.mode == NT_LANDSCAPE) nt_landscape_key(k);
        else {
            int32_t n = (int32_t)(TX_KEY_F3 + k) + 12 * nt.oct;
            if (n < 0 || n > 127) continue;
            sc_play_ready(sc.sel);
            nt.note[k] = (uint8_t)n; nt.lane[k] = sc.sel;
            ln_key(sc.sel, (uint32_t)n, NT_VEL, 1);
            nt.playing |= 1u << k;
        }
    }
    if (nt.hold_k < NT_SPAN && (nt.mode != NT_LANDSCAPE || !(nt.down >> nt.hold_k & 1u))) nt.hold_k = NT_SPAN;
    if (nt.hold_k < NT_SPAN && fm1_ms - nt.hold_t >= NT_HOLD_MS) {   /* held: the lane muted / unmuted, once a press */
        ui_lane_mute(NT_GROUP[nt_index(nt.hold_k)]);
        nt.hold_k = NT_SPAN;
    }
}

/* ---- the LEDs: the keys (lit, and the dim glow layer) and the buttons; set only when they change */
static void nt_leds(uint32_t btn)
{
    static struct { uint32_t lit, glow, btn; uint8_t done; } was;
    uint32_t lit = 0, glow = 0, k;
    if (nt.mode == NT_PLAY) lit = nt.down;
    else {
        for (k = 0; k < sizeof NT_GROUP; k++) {   /* the lanes: their black keys */
            uint32_t l = NT_GROUP[k], key = nt_key_of(1, k), p;
            if (l >= SC_N || key >= NT_SPAN) continue;
            if (sc.run[l] && sc.mute[l]) glow |= 1u << key;   /* (muted: a steady glow) */
            else if (sc.run[l]) {
                p = sc_blink_ms(l);
                if (fm1_ms % p < p / 2u) lit |= 1u << key;
                else glow |= 1u << key;
            } else if (l == sc.sel) glow |= 1u << key;
        }
        if (sc.run[sc.sel]) {                   /* the selected lane's bars (HOW MANY off: none lit) */
            uint32_t c = sc.cnt[sc.sel], a = sc_act_of(sc.step[sc.sel]);
            for (k = 1; k <= c && k < LS_NCNT; k++) lit |= 1u << nt_key_of(0, k);
            for (k = 0; k <= a && k < LS_NACT; k++) lit |= 1u << nt_key_of(0, LS_NCNT + k);
        }
    }
    if (was.done && lit == was.lit && glow == was.glow && btn == was.btn) return;
    was.done = 1; was.lit = lit; was.glow = glow; was.btn = btn;
    for (k = 0; k < 14u; k++) {                 /* the buttons: lit when on, else a glow (every one does something) */
        fm1_led_key(k, btn >> k & 1u);
        fm1_led_key_glow(k, !(btn >> k & 1u));
    }
    for (k = 0; k < TX_NKEYS; k++) {
        fm1_led_key(KEY_LED(k), lit >> k & 1u);
        fm1_led_key_glow(KEY_LED(k), glow >> k & 1u && !(lit >> k & 1u));
    }
    fm1_led_build();
}
