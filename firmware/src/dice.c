/* SPDX-License-Identifier: GPL-3.0-only */
/* The dice: RANDOM. On the FM-1 OCT- and OCT+ pressed together (ui.c), in the desktop plugin its window's dice (the
 * general one, top right; each lane's beside its swap icon): one call here for both.
 *
 *   dice_roll   every lane with a sound gets the groups asked (the FM-1's chord: all four, DICE_ALL; the plugin
 *               passes its window's ticks, kept with the project): ENGINES a new engine on each (any family,
 *               never the one it had, none twice where there are enough), SOUND each engine's cards at random over their ranges, EFFECTS the
 *               mix's COMP, DRIVE, FILTER, DELAY / GRAIN, REVERB and M/S (types and amounts, in musical, safe ranges:
 *               the filter's band never closes, the drive and the make-up stay moderate, MONO stays off, long, unreal
 *               reverbs can come up), MOVEMENT the eight LFOs, the envelopes, the followers and the matrix (two to six
 *               routes onto the cards, the effects' amounts when EFFECTS is ticked, the LFOs' own RATE and LEVEL, at
 *               moderate depths: never onto a lane's LEVEL, PAN, HOW MANY or HOW ACTIVE)
 *   dice_lane   one lane a new engine (as dice_roll's ENGINES alone), its cards at the engine's own
 *
 * Kept always: LANES, each lane's LEVEL, PAN, HOW MANY, HOW ACTIVE, mute and MIDI CH, the tempo, the clock, the keys,
 * GROOVE. Levels stay even: a lane's LEVEL is its own, each sound carries its trim (landscapes.h). A roll is a scene:
 * what plays captured (scenes.c scn_capture), changed, then applied as a load is (scn_apply), so nothing cuts: a lane
 * that keeps its engine glides to its new cards, one that gets another switches as on a load, the effects glide and
 * their tails ring on (scn_fx_force when EFFECTS is ticked; else they stay whatever EFFECTS ON LOAD says). What played
 * is the undo, as a load's: SAVE held (the plugin's UNDO) takes the whole roll back, again brings it back. A general
 * roll reads "RANDOM *" (scenes.c SCN_RND), a lane's leaves the scene's name with its "*"; SAVE keeps either.
 *
 * A roll asked while a load runs waits for it (the last one asked wins). The cards of a lane that switched engine (with
 * SOUND) come once its new engine is in, a lane a pass, gliding (dice_poll). Random each time: the FM-1's 24 MHz
 * timer at the press; the plugin passes a seed of its own (fixed under its tests); the host build's run repeats.
 * Main loop only; integers only. */

#define DICE_ROUTES_MIN 2u                      /* MOVEMENT: the matrix's routes a roll makes */
#define DICE_ROUTES_MAX 6u
#define DICE_LANE 0x40u                         /* (dice_q: a lane's roll, its lane in the low bits) */
#define DICE_Q 0x80u                            /* (dice_q: a roll waiting for the load that runs) */
#define DICE_TRIES 64u                          /* (an engine's pick: random tries before it settles for any other) */
#if SC_N > 4
#define DICE_NL ((uint32_t)sc_nlane)            /* (the lanes in use) */
#else
#define DICE_NL SC_N
#endif

static uint32_t dice_rng = 0x2F6B3A1Du;
static uint8_t dice_q, dice_qw;                 /* a roll waiting (DICE_Q, or DICE_Q | DICE_LANE | lane); what it rolls */
static scn_lm_t dice_cp;                        /* lanes whose cards SOUND rolls once their new engine is in */
static void ui_toast_text(const char *hd, const char *fam, const char *nm);   /* ui.c */

static uint32_t dice_r(void) { uint32_t x = dice_rng; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return dice_rng = x; }
static uint32_t dice_n(uint32_t n) { return n ? (dice_r() >> 8) % n : 0u; }   /* 0 .. n - 1 */
static int32_t dice_in(int32_t lo, int32_t hi) { return lo + (int32_t)dice_n((uint32_t)(hi - lo + 1)); }   /* lo .. hi */
static void dice_seed(uint32_t s)
{
#ifndef DINGHY_HOST
    s ^= fm1_ticks();                           /* (the moment of the press: never the same roll twice) */
#endif
    dice_rng ^= s * 2654435761u ^ fm1_ms;
    if (!dice_rng) dice_rng = 0x2F6B3A1Du;
    (void)dice_r();
}

/* ---- a new user scene's name (scn_words.h): drawn on this dice, never a saved scene's name nor the one shown */
#include "scn_words.h"
static uint32_t dice_wr(void *ctx) { (void)ctx; return dice_r(); }
static uint32_t dice_nm_len(const char *s)       /* (its length, trailing spaces off: 11 at most) */
{
    uint32_t n = 0, e = 0;
    while (n < 11u && s[n]) { n++; if (s[n - 1u] != ' ') e = n; }
    return e;
}
static int dice_nm_eq(const char *a, const char *b)
{
    uint32_t n = dice_nm_len(a), i;
    if (n != dice_nm_len(b)) return 0;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static int dice_wtaken(const char *nm, void *ctx)
{
    uint32_t i;
    (void)ctx;
    for (i = 0; i < SCN_NU; i++) if (scn_meta[i].used && dice_nm_eq(nm, scn_meta[i].name)) return 1;
    return dice_nm_eq(nm, scn_nm);
}
static void dice_scn_name(char out[12])
{
    dice_seed(0);
    scn_words(out, dice_wr, dice_wtaken, 0);
}

/* ---- the scene's records */
static void dice_put(scn_t *t, uint32_t id, int32_t v)   /* id's record set (added if it had none) */
{
    int i = scn_rec_of(t, id);
    if (i >= 0) t->r[i].v = (int16_t)v; else scn_add(t, id, v);
}
static void dice_drop(scn_t *t, int (*is)(uint32_t id, uint32_t a), uint32_t a)   /* the records is() picks dropped */
{
    uint32_t i, n = 0;
    for (i = 0; i < t->nrec; i++) if (!is(t->r[i].id, a)) t->r[n++] = t->r[i];
    t->nrec = (uint16_t)n;
}
static int dice_is_cards(uint32_t id, uint32_t k) { return P_SCOPE(id) == P_LS(k) && P_IDX(id) >= PL_CARD0 && P_IDX(id) < PL_CARD0 + SCD_MAX; }
static int dice_is_fx(uint32_t id, uint32_t a) { (void)a; return P_SCOPE(id) == P_GLOBAL && scn_fx_id(id) && P_IDX(id) != PG_GSYNC; }
static int dice_is_mov(uint32_t id, uint32_t a) { (void)a; return P_SCOPE(id) == P_LFO || P_SCOPE(id) == P_ENV || (P_SCOPE(id) == P_MAT && P_IDX(id) < MOD_NS * MQ_N); }
static int dice_is_id(uint32_t id, uint32_t a) { return id == a; }

/* ---- ENGINES: lane k's sound in t, and a new one */
static uint32_t dice_lf(const scn_t *t, uint32_t k)   /* lane k's family in t */
{
    uint32_t lf = SCN_LF_OF(t, k);
    return lf && lf <= LS_N ? lf - 1u : t->fam % LS_N;
}
static const char *dice_name(const scn_t *t, uint32_t k)   /* lane k's sound's name in t ("": none) */
{
    uint32_t f = dice_lf(t, k);
    return t->snd[k] >= 0 && (uint32_t)t->snd[k] < LS[f].n ? LS[f].s[t->snd[k]].name : "";
}
static int dice_have(uint32_t f, uint32_t i)    /* family f's sound i plays in this build */
{
    const ls_sound_t *d = &LS[f].s[i];
    return !d->eng || tx_engine_find(d->src) != 0;
}
/* lane k's new sound in t: any family's, never the one it plays (by name: a sound in two families is one), at first
 * none another lane plays, then any */
static void dice_engine(scn_t *t, uint32_t k)
{
    const char *was = dice_name(t, k);
    uint32_t pass, n, j, f = 0, i = 0;
    int got = 0;
    for (pass = 0; pass < 2u && !got; pass++)
        for (n = 0; n < DICE_TRIES && !got; n++) {
            f = dice_n(LS_N); i = dice_n(LS[f].n);
            if (!dice_have(f, i) || sc_eq(LS[f].s[i].name, was)) continue;
            got = 1;
            for (j = 0; pass == 0u && j < DICE_NL; j++) if (j != k && sc_eq(LS[f].s[i].name, dice_name(t, j))) got = 0;
        }
    if (!got) return;
    t->snd[k] = (int8_t)i;
#if SC_N > 4
    t->lf[k] = 0;
#else
    t->lfam &= (uint16_t)~(15u << (4u * k));
#endif
    SCN_LF_SET(t, k, f);
    dice_drop(t, dice_is_cards, k);             /* (the old engine's cards: the new one starts at its own) */
}
/* ---- SOUND: a lane that keeps its engine, its cards as records (they glide there); one that switches, once its new
 * engine is in (dice_poll) */
static void dice_cards(scn_t *t, uint32_t k)
{
    p_cards_t *c = p_cards(k);
    uint32_t i;
    for (i = 0; i < c->n; i++) dice_put(t, PID(P_LS(k), PL_CARD0 + i), dice_in(0, c->hi[i]));
}

/* ---- EFFECTS: types and amounts, musical and safe (the output's limiter is the mix's, last, always on) */
static void dice_g(scn_t *t, uint32_t i, int32_t v) { dice_put(t, PID(P_GLOBAL, i), v); }
static void dice_fx(scn_t *t)
{
    static const uint8_t PITCH[8] = {64, 64, 64, 64, 64, 127, 1, 101};   /* (as played mostly; an octave up or down, a
                                                 * fifth up) */
    int32_t hp, lp, on;
    dice_drop(t, dice_is_fx, 0);
    /* COMP: on most times, a gentle ratio, a little make-up (its own make-up follows the threshold) */
    dice_g(t, PG_COMP0 + TX_CP_ON, dice_n(10) < 7u);
    dice_g(t, PG_COMP0 + TX_CP_THR, dice_in(30, 90)); dice_g(t, PG_COMP0 + TX_CP_ATK, dice_in(60, 120));
    dice_g(t, PG_COMP0 + TX_CP_REL, dice_in(30, 90)); dice_g(t, PG_COMP0 + TX_CP_RATIO, dice_in(0, 3));
    dice_g(t, PG_COMP0 + TX_CP_MKUP, dice_in(0, 20));
    /* DRIVE: off half the times, else moderate (level-neutral: tone, not volume) */
    on = dice_n(2);
    dice_g(t, PG_DRIVE0 + TX_DR_AMOUNT, on ? dice_in(8, 64) : 0); dice_g(t, PG_DRIVE0 + TX_DR_TONE, dice_in(40, 90));
    dice_g(t, PG_DRIVE0 + TX_DR_MIX, dice_in(64, 127)); dice_g(t, PG_DRIVE0 + TX_DR_TYPE, dice_in(0, TX_DRT_N - 1));
    /* FILTER: open most times; else a high-pass up to ~180 Hz and a low-pass down to ~1.5 kHz: a band, never closed */
    hp = 0; lp = 127;
    if (dice_n(5) >= 3u) { hp = dice_n(2) ? 0 : dice_in(1, 40); lp = dice_n(2) ? 127 : dice_in(80, 126); }
    dice_g(t, PG_MF0 + TX_MF_LOW, hp); dice_g(t, PG_MF0 + TX_MF_HIGH, lp);
    dice_g(t, PG_MF0 + TX_MF_RESO, dice_in(0, 50)); dice_g(t, PG_MF0 + TX_MF_SLOPE, dice_in(0, TX_MF_SN - 1));
    /* DELAY / GRAIN: on half the times, its repeats never running away (FEEDBACK under endless), never frozen */
    on = dice_n(2);
    dice_g(t, PG_GD(TX_GD_MIX), on ? dice_in(16, 56) : 0); dice_g(t, PG_GD(TX_GD_TIME), dice_in(20, 120));
    dice_g(t, PG_GD(TX_GD_FEEDBACK), dice_in(10, 90)); dice_g(t, PG_GD(TX_GD_GRAINS), dice_in(0, 11));
    dice_g(t, PG_GD(TX_GD_SIZE), dice_in(20, 110)); dice_g(t, PG_GD(TX_GD_SPRAY), dice_in(0, 90));
    dice_g(t, PG_GD(TX_GD_PITCH), PITCH[dice_n(8)]); dice_g(t, PG_GD(TX_GD_REVERSE), dice_n(2) ? 0 : dice_in(10, 60));
    dice_g(t, PG_GD(TX_GD_SPREAD), dice_in(30, 127)); dice_g(t, PG_GD(TX_GD_TONE), dice_in(60, 127));
    dice_g(t, PG_GD(TX_GD_GRAIN), dice_in(0, 127)); dice_g(t, PG_GD(TX_GD_FREEZE), 0);
    dice_g(t, PG_GD(TX_GD_MOD), dice_n(2) ? 0 : dice_in(10, 90)); dice_g(t, PG_GD(TX_GD_MODRATE), dice_in(10, 100));
    dice_g(t, PG_GD(TX_GD_IVL), dice_in(0, TX_GDI_N - 1));
    /* REVERB: a space always, its sound kept whole (MIX to 64); long and unreal decays can come (sustained, never
     * frozen), a shimmer now and then */
    dice_g(t, PG_REVERB0 + TX_RV_TYPE, dice_in(TX_RVT_ROOM, TX_RVT_N - 1)); dice_g(t, PG_REVERB0 + TX_RV_MIX, dice_in(18, 60));
    dice_g(t, PG_REVERB0 + TX_RV_DECAY, dice_in(30, 126)); dice_g(t, PG_REVERB0 + TX_RV_TONE, dice_in(30, 100));
    dice_g(t, PG_REVERB0 + TX_RV_PREDELAY, dice_in(0, 50)); dice_g(t, PG_REVERB0 + TX_RV_SIZE, dice_n(3) ? 0 : dice_in(20, 127));
    dice_g(t, PG_REVERB0 + TX_RV_MOD, dice_in(20, 110)); dice_g(t, PG_REVERB0 + TX_RV_LOWCUT, dice_in(0, 50));
    dice_g(t, PG_REVERB0 + TX_RV_SHIMMER, dice_n(3) ? 0 : dice_in(10, 60)); dice_g(t, PG_REVERB0 + TX_RV_SHIFT, dice_in(0, 3));
    /* M/S: near 0 dB, a little narrower or wider; MONO off (never MID and SIDE both gone) */
    dice_g(t, PG_MS0 + TX_MS_MID, dice_in(108, 117)); dice_g(t, PG_MS0 + TX_MS_SIDE, dice_in(100, 121));
    dice_g(t, PG_MS0 + TX_MS_MONO, 0);
}

/* ---- MOVEMENT: the sources, then two to six routes from them at moderate depths */
static uint32_t dice_used(const scn_t *t)       /* the lanes with a sound in t (bit k) */
{
    uint32_t k, m = 0;
    for (k = 0; k < DICE_NL; k++) if (t->snd[k] >= 0) m |= 1u << k;
    return m;
}
static uint32_t dice_tgt(const scn_t *t, uint32_t what)   /* a route's TARGET (mod.c's list) */
{
    static const uint8_t GRAIN[5] = {4, 5, 8, 9, 10};   /* (MT_GRAIN's SIZE SPRAY SPREAD TONE GRAIN: never its MIX, TIME,
                                                 * FEEDBACK, GRAINS) */
    static const uint8_t REV[3] = {0, 2, 5};    /* (MT_REV's MIX TONE MOD) */
    uint32_t m = dice_used(t) & 15u, k;
    for (;;) {
        uint32_t r = dice_n(what & DICE_FX ? 10u : 7u);
        if (r < 5u) {                           /* a card: lane 1 .. 4's (one with a sound) or ALL LANES' */
            k = dice_n(5);
            if (k < 4u && !(m >> k & 1u)) continue;
            return 1u + k * MT_LANE_N + 4u + dice_n(4);   /* (its first four cards: every sound has them, most) */
        }
        if (r < 7u) return MT_LFO0 + dice_n(2u * TX_LFO_N);   /* an LFO's RATE or LEVEL */
        if (r == 7u) return MT_GRAIN0 + GRAIN[dice_n(5)];
        if (r == 8u) return MT_REV0 + REV[dice_n(3)];
        return dice_n(3) == 0u ? MT_DRV0 + TX_DR_TONE : dice_n(2) ? MT_MF0 + 1u : MT_GMOD;   /* DRIVE TONE, LOW PASS,
                                                 * the grains' MOD */
    }
}
static void dice_mov(scn_t *t, uint32_t what)
{
    static const uint8_t SRC[13] = {MS_LFO1, MS_LFO1 + 1, MS_LFO1 + 2, MS_LFO1 + 3, MS_LFO1 + 4, MS_LFO1 + 5, MS_LFO1 + 6,
                                    MS_LFO1 + 7, MS_ENV1, MS_ENV1 + 1, MS_FOL1, MS_FOL1 + 1, MS_RANDOM};
    uint32_t n, i, m = dice_used(t) & 15u, nr = (uint32_t)dice_in(DICE_ROUTES_MIN, DICE_ROUTES_MAX), s = 0;
    uint16_t made[DICE_ROUTES_MAX];
    dice_drop(t, dice_is_mov, 0);
    for (n = 0; n < TX_LFO_N; n++) {            /* the LFOs: free most times (else a division of the beat, 1/4 .. 16
                                                 * bars), any shape, a key restarting one now and then */
        uint32_t b = n * 16u;
        dice_put(t, PID(P_LFO, b + TX_LP_SHAPE), dice_in(0, TX_LS_N - 1));
        dice_put(t, PID(P_LFO, b + TX_LP_RATE), dice_in(25, 95));
        dice_put(t, PID(P_LFO, b + TX_LP_SYNC), dice_n(3) ? 0 : dice_in(10, 21));
        dice_put(t, PID(P_LFO, b + TX_LP_MODE), dice_n(4) ? TX_LM_FREE : TX_LM_KEY);
        dice_put(t, PID(P_LFO, b + TX_LP_PHASE), dice_in(0, 127));
        dice_put(t, PID(P_LFO, b + TX_LP_WIDTH), dice_in(16, 111));
        dice_put(t, PID(P_LFO, b + TX_LP_JITTER), dice_in(0, 80));
        dice_put(t, PID(P_LFO, b + TX_LP_FADE), dice_in(0, 40));
        dice_put(t, PID(P_LFO, b + TX_LP_RESTART), 0);
        dice_put(t, PID(P_LFO, b + TX_LP_LEVEL), dice_in(70, 127));
    }
    for (n = 0; n < TX_ENV_N; n++) {            /* the envelopes: a key, a lane's notes or events (one with a sound), at
                                                 * random, or an LFO */
        uint32_t b = n * 16u, tg;
        do tg = (uint32_t)dice_in(TX_ET_KEY, TX_ET_LFO1 + TX_LFO_N - 1);
        while (tg >= TX_ET_LANE1 && tg < TX_ET_RANDOM && !(m >> ((tg - TX_ET_LANE1) & 3u) & 1u));
        dice_put(t, PID(P_ENV, b + TX_EP_TRIGGER), (int32_t)tg);
        dice_put(t, PID(P_ENV, b + TX_EP_ATTACK), dice_in(10, 90)); dice_put(t, PID(P_ENV, b + TX_EP_DECAY), dice_in(30, 100));
        dice_put(t, PID(P_ENV, b + TX_EP_RELEASE), dice_in(30, 105)); dice_put(t, PID(P_ENV, b + TX_EP_SUSTAIN), dice_in(0, 127));
        dice_put(t, PID(P_ENV, b + TX_EP_CURVE), dice_in(0, 127)); dice_put(t, PID(P_ENV, b + TX_EP_VARY), dice_in(0, 90));
        dice_put(t, PID(P_ENV, b + TX_EP_LOOP), dice_n(3) == 0u);
    }
    for (n = 0; n < TX_FOL_N; n++) {            /* the followers: a lane's level or events (one with a sound), the mix,
                                                 * the grains */
        uint32_t b = MV_FOL0 + n * 16u, in;
        do in = dice_n(TX_FI_N);
        while (in < TX_FI_MIX && !(m >> (in & 3u) & 1u));
        dice_put(t, PID(P_ENV, b + TX_FP_INPUT), (int32_t)in);
        dice_put(t, PID(P_ENV, b + TX_FP_ATTACK), dice_in(10, 70)); dice_put(t, PID(P_ENV, b + TX_FP_RELEASE), dice_in(30, 100));
        dice_put(t, PID(P_ENV, b + TX_FP_GAIN), dice_in(40, 100));
    }
    for (i = 0; i < nr * 4u && s < nr; i++) {   /* the routes: in the first slots, none twice, -40 .. -10 or +10 ..
                                                 * +40 % */
        uint32_t src = SRC[dice_n(13)], T = dice_tgt(t, what), j, d = (uint32_t)dice_in(10, 40);
        for (j = 0; j < s && made[j] != (uint16_t)(src << 8 | T); j++) {}
        if (j < s) continue;
        made[s] = (uint16_t)(src << 8 | T);
        dice_put(t, PID(P_MAT, s * MQ_N + MQ_SRC), (int32_t)src); dice_put(t, PID(P_MAT, s * MQ_N + MQ_TGT), (int32_t)T);
        dice_put(t, PID(P_MAT, s * MQ_N + MQ_DEPTH), (int32_t)(dice_n(2) ? 100u + d : 100u - d));
        dice_put(t, PID(P_MAT, s * MQ_N + MQ_SPREAD), mod_tgt_lanes(T) == 4u ? dice_in(0, 64) : 32);
        s++;
    }
}

/* ---- the roll: what plays captured (the undo, as a load's), changed, applied as a load */
static void dice_say(uint32_t what, int lane, const scn_t *t)   /* the toast (ui.c): "LANE 2 . RANDOM", "BROOK";
                                                 * "RANDOM . ROLLED", "ENGINES SOUND MOVEMENT" (all four: EVERYTHING) */
{
    static const char *const W[DICE_N] = {"ENGINES", "SOUND", "FX", "MOVEMENT"};
    char a[8], b[24];
    uint32_t i;
    if (lane >= 0) {
        p_strcpy(a, "LANE ", 7); p_fmt_int(a + 5, lane + 1);
        p_upper(b, dice_name(t, (uint32_t)lane), 23);
        ui_toast_text(a, "RANDOM", b);
        return;
    }
    b[0] = 0;
    if (what == DICE_ALL) p_strcpy(b, "EVERYTHING", 23);
    else for (i = 0; i < DICE_N; i++) if (what >> i & 1u) { if (b[0]) p_cat(b, " ", 23); p_cat(b, W[i], 23); }
    ui_toast_text("RANDOM", "ROLLED", b);
}
static void dice_go(uint32_t what, int lane)
{
    scn_t *t = &scn_b[scn_u ^ 1u];
    uint32_t k, used;
    scn_capture(&scn_b[scn_u]);                 /* (what played: the undo) */
    scn_bidx[scn_u] = scn_cur;
    scn_undo_ok = 1;
    scn_su.on = 0;
    memcpy(t, &scn_b[scn_u], sizeof *t);
    dice_drop(t, dice_is_id, PID(P_GLOBAL, PG_FINE));   /* (the tempo stays, whatever PRESETS CHANGE TEMPO says) */
    used = dice_used(t);
    dice_cp = 0;
    for (k = 0; k < DICE_NL; k++) {
        if (!(used >> k & 1u) || (lane >= 0 && k != (uint32_t)lane)) continue;
        if (what & DICE_ENG) dice_engine(t, k);
        if (!(what & DICE_SND)) continue;
        if (t->snd[k] == sc.snd[k] && dice_lf(t, k) == sc.lf[k]) dice_cards(t, k);   /* (its engine stays) */
        else dice_cp |= (scn_lm_t)(1u << k);
    }
    if (what & DICE_FX) dice_fx(t);
    if (what & DICE_MOV) dice_mov(t, what);
    scn_undo_fx = (uint8_t)((what & DICE_FX) != 0);
    scn_fx_force = scn_undo_fx;
    scn_ap_edit = 1;
    scn_apply(t, lane >= 0 ? scn_cur : SCN_RND);
    dice_say(what, lane, t);
}
static int dice_ask(uint32_t q, uint32_t what, uint32_t seed)
{
    dice_seed(seed);
    dice_q = (uint8_t)(DICE_Q | q); dice_qw = (uint8_t)what;
    if (!scn_busy()) { dice_q = 0; dice_go(what, q & DICE_LANE ? (int)(q & 15u) : -1); }
    return 0;
}
/* every lane: what is ticked (DICE_*; none: nothing rolls, -1); seed: the caller's own randomness (0: none) */
static int dice_roll(uint32_t what, uint32_t seed)
{
    what &= DICE_ALL;
    if (!what) return -1;
    return dice_ask(0, what, seed);
}
/* lane k a new engine, its cards at the engine's own (-1: no sound on it) */
static int dice_lane(uint32_t k, uint32_t seed)
{
    if (k >= DICE_NL || sc.snd[k] < 0) return -1;
    return dice_ask(DICE_LANE | k, DICE_ENG, seed);
}
static int dice_busy(void) { return dice_q != 0 || dice_cp != 0; }
static void dice_poll(void)
{
    uint32_t k, i;
    if (dice_q && !scn_busy()) {                /* (a roll asked while a load ran) */
        uint8_t q = dice_q;
        dice_q = 0;
        dice_go(dice_qw, q & DICE_LANE ? (int)(q & 15u) : -1);
        return;
    }
    if (!dice_cp || scn_busy() || ln.qw - ln.qr >= LN_QN / 2u) return;
    for (k = 0; !(dice_cp >> k & 1u); k++) {}
    dice_cp &= (scn_lm_t)~(1u << k);            /* (a lane a pass) */
    if (sc.snd[k] < 0 || sc.a_eng[k] != sc.eng[k] || sc.reswap[k]) return;
    {
        p_cards_t *c = p_cards(k);
        for (i = 0; i < c->n; i++) scn_put(PID(P_LS(k), PL_CARD0 + i), dice_in(0, c->hi[i]));
        if (c->n) scn_edited = 1;
    }
}
