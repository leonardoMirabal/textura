/* SPDX-License-Identifier: GPL-3.0-only */
/* Scenes and the settings.
 *
 * A scene is everything a sound place is: the landscape, each lane's sound, HOW MANY, HOW ACTIVE, playing or not, its
 * level, pan and mute, its name, then a list of (parameter id, value) records: the lanes' cards (the ones set), the tempo,
 * and of the grain delay and its SYNC, the reverb, the compressor, the drive, the mix's filter, the eight LFOs, the
 * envelopes, the followers, the matrix and the GROOVE (AMOUNT, TYPE, SHIFT, each lane's PART and CYCLE) the values
 * away from their defaults (scn_def_*). A load sets every one of those to its default first,
 * then the scene's records over them, so a record of an older firmware loads into a newer one (an id it lacks gets
 * its default; ids never change, params.c). The tempo is set only with PRESETS CHANGE TEMPO at YES (tempo.c). The
 * mix's effects (DRIVE, FILTER, COMPRESSOR, GRAIN, REVERB) follow EFFECTS ON LOAD: ALWAYS a load sets them; SMART (the
 * default) it sets them unless the player turned one since the last load that set them (scn_fx_mine: then they stay,
 * the screen says FX: YOURS, and a load asked with its effects, scn_fx_force, still sets them); NEVER they stay. The
 * boot's load sets them always. Set, their amounts glide SCN_GLIDE_MS like the cards (scn_put), so nothing steps and
 * the delay's and the reverb's tails ring on (a save always keeps them). A scene saved with
 * a DRIVE on each lane (ids 32 .. 35 of a lane, retired) loads that onto the mix's one DRIVE: the lane with the most
 * AMOUNT gives its AMOUNT, TONE, MIX and TYPE (the first such lane on a tie; none above 0: the DRIVE stays off). A
 * lane's FILTER (ids 36 .. 41 of a lane, retired: the mix has one FILTER) and a matrix route onto it are skipped.
 *
 *   factory   F001 .. F100: the factory presets (presets.c: four lanes of any families, their levels balanced,
 *             a touch of reverb or echo that comes with the load as EFFECTS ON LOAD says), the modulation at its
 *             defaults
 *   user      U01 .. U16, in a bank of two copies written alternately (Frodhes' presets.c scheme): the older copy
 *             erased, the records written, the header last; the valid copy with the higher sequence loads, so a save
 *             cut short never loses the bank. Only the names stay in RAM; a record is read when it loads.
 *   PRESETS   the presets list (ui.c): one list of the factory presets by family, then USER (the saved user
 *             scenes); turning moves the cursor, nothing loads; EDIT loads the one under it (the list stays), EDIT on
 *             the one loaded with FX: YOURS loads it with its effects; HOME closes it, what plays kept
 *   SAVE      the save mode: PRESETS / K1 pick the user scene, K2 the name's letter and K3 its character (the name
 *             a used slot had, or the landscape's), SAVE again saves, HOME (or OCT-) cancels
 *   SAVE held 700 ms: undo the last load (again: redo)
 *   MIDI PC   on channel 1: 0 .. 99 F001 .. F100, 100 .. 115 U01 .. U16
 *
 * A load never cuts: a lane that keeps its sound plays on untouched; a lane that gets another one switches, its old
 * sound playing on till its turn (one lane at a time, ~150 ms apart), then fading out under the new one (lanes.c);
 * the lanes' levels, pans, HOW MANY, HOW ACTIVE (scape.c) and their numbers (the cards: scn_put) glide 500 ms; a
 * switched lane's cards are set as soon as its new engine is in. A load asked for while one runs (a preview, a
 * program change) waits for it, the last one asked wins.
 *
 * A load and a save both run beside the main loop, a slice a pass: a load sets the lanes first, waits for their
 * engines, then sends its records SCN_SLICE at a time while lanes.c's queue has room; a save erases one sector a
 * pass, then writes the bank a chunk a pass. The settings (what is not a scene: the clock and its ports, the BPM, the
 * last scene, the palette) are one small record saved as Frodhes does: SET_SAVE_MS after the last change, with
 * settings_save_start / settings_save_poll (the platform's split save).
 *
 * The flash place is settled at stage 8 (the map): the banks sit in store.h's slots area (Textura keeps no samples),
 * their sizes named here. On the host and the web the store is the platform's flash window (tests/host.c,
 * web/web_hal.c fl_window_*). Main loop only. */

#define SCN_NU 16u                              /* user scenes */
#define SCN_NF PR_N                             /* factory scenes: the presets (presets.c) */
#define SCN_PC_U 100u                           /* MIDI PC: the first user scene's program */
#define SCN_N (SCN_NF + SCN_NU)
#define SCN_SLOT 1020u                          /* a record's bytes in the bank */
#define SCN_REC_MAX 244u                        /* (id, value) records a scene holds at most */
#define SCN_BANK_SECT 4u                        /* a bank copy: 4 KiB sectors */
#define SCN_BANK_A SLOTS_AT                     /* (store.h; stage 8 settles the place) */
#define SCN_BANK_B (SLOTS_AT + SCN_BANK_SECT * 0x1000u)
#define SCN_MAGIC 0x42535854u                   /* "TXSB" */
#define SCN_VER 1u
#define SCN_CHUNK 255u                          /* a save writes this much a pass (SCN_SLOT = 4 chunks) */
#define SCN_SLICE 16u                           /* records a load sends a pass */
#define SCN_SETTLE_MS 120u                      /* a load: the lanes' engines first, this long at least */
#define SCN_BROWSE_MS 400u
#define SCN_GLIDE_MS 500u                       /* a load: a lane's numbers (its cards) glide this long */
#define SCN_GLIDE_TICK 10u
#define SCN_CARDS_MS 24u                        /* a switched lane: its cards this long after its switch (its hosted
                                                 * engine in) */
#define SCN_GL_N 80u                            /* (glides at once: 4 lanes' cards, the effects' amounts) */
#define SCN_UNDO_MS 700u
#define SET_SAVE_MS 2000u
#define SET_VER 1u
#define SET_LAYOUT 1u                           /* tx_set_t.layout: 0 the scenes as before the presets (F01 .. F10,
                                                 * then the user's), 1 F001 .. F100 then the user's */
#define SCN_NF_OLD 10u

typedef struct { uint16_t id; int16_t v; } scn_rec_t;
typedef struct {
    uint8_t ver, used, fam, mute;               /* (mute: lane k muted at bit k; a scene from before it: 0, none) */
    char name[12];
    int8_t snd[SC_N];
    uint8_t cnt[SC_N], step[SC_N], run[SC_N], vol[SC_N], pan[SC_N];
    uint16_t nrec, lfam;                        /* (lfam: lane k's family + 1 at bits 4k .. 4k+3; 0: the scene's
                                                 * landscape, as every scene before the sound browser) */
    scn_rec_t r[SCN_REC_MAX];
} scn_t;
typedef struct { uint32_t magic, seq, len, crc; } scn_hdr_t;
_Static_assert(sizeof(scn_t) == SCN_SLOT, "a scene record is SCN_SLOT bytes");
_Static_assert(SCN_SLOT % SCN_CHUNK == 0u, "a record is whole chunks");
_Static_assert(sizeof(scn_hdr_t) + SCN_NU * SCN_SLOT <= SCN_BANK_SECT * 0x1000u, "a bank copy fits its sectors");
_Static_assert(SCN_BANK_B + SCN_BANK_SECT * 0x1000u <= SLOTS_AT + NSLOT * SLOT_SECT * 0x1000u, "the banks in the slots area");

/* ---- the flash (the host and the web: the platform's window; the FM-1: platform.c's, an erase stalls the sound
 * ~50 ms until stage 8 gives the banks the split erase the settings have) */
#ifdef DINGHY_HOST
static int scn_rd(uint32_t at, void *p, uint32_t n) { return fl_window_read(at, p, n); }
static int scn_er(uint32_t at) { return fl_window_erase(at); }
static int scn_wr(uint32_t at, const void *p, uint32_t n) { return fl_window_write(at, p, n); }
#else
static int scn_rd(uint32_t at, void *p, uint32_t n) { return flash_ok ? st_read(at, p, n) : -1; }
static int scn_er(uint32_t at) { uint32_t took; return fl_erase4k(at, &took); }
static int scn_wr(uint32_t at, const void *p, uint32_t n) { return fl_write(at, p, n); }
#endif
static uint32_t scn_crc(uint32_t c, const uint8_t *p, uint32_t n)   /* running: start 0xFFFFFFFF, end ~ */
{
    uint32_t k;
    while (n--) { c ^= *p++; for (k = 0; k < 8u; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1u))); }
    return c;
}

/* ---- the state */
static struct { uint8_t used, fam; char name[12]; } scn_meta[SCN_NU];   /* the user scenes' names (RAM) */
static uint32_t scn_bank, scn_seq;             /* the valid copy (its place; 0: none yet) and its sequence */
#ifdef DINGHY_HOST
static scn_t scn_b[2];                          /* a load's scene and the undo's (their roles swap: scn_u) */
#else
static scn_t scn_b[2] __attribute__((section(".nozero")));   /* (NOINIT's spare room, app.ld: each one is written whole
                                                 * before it is read: scn_factory, scn_capture, scn_read) */
#endif
static scn_t scn_sv;                            /* the scene being saved */
static uint8_t scn_u, scn_undo_ok, scn_bidx[2];   /* (scn_bidx: each one's scene number, 0xFF: none) */
static struct { uint8_t on, k, f, i; } scn_su;   /* the last sound kept by the sound browser (scn_snd_undo) */
static uint8_t scn_cur = 0xFF;                  /* the scene loaded last (0xFF: none) */
/* a load in progress */
static const scn_t *scn_ap;
static uint8_t scn_ap_stage;                    /* 0 idle, 1 the lanes set (waiting), 2 the records */
static uint16_t scn_ap_i, scn_ap_d;            /* the next record; the next default (SCN_DEF_N: all sent) */
static uint32_t scn_ap_t;
static uint8_t scn_ap_idx = 0xFF;
static uint8_t scn_ap_drv;                      /* an older scene's lane whose DRIVE becomes the mix's (0xFF: none) */
static uint8_t scn_ap_fx, scn_fx_boot;          /* this load sets the mix's effects (EFFECTS ON LOAD, the boot) */
static uint8_t scn_fx_mine;                     /* the player turned an effect since the last load that set them
                                                 * (SMART: the next load keeps them, FX: YOURS) */
static uint8_t scn_fx_force;                    /* the next load sets the effects whatever EFFECTS ON LOAD says */
static uint8_t scn_edited;                      /* the player changed what plays since the last load or save (the
                                                 * header's "*") */
static struct { uint16_t id, t0; int16_t from, to; } scn_gl[SCN_GL_N];   /* the glides (scn_put): t0, ms mod 2^16 */
static uint8_t scn_gl_n;
static uint8_t scn_ap_sw, scn_ap_cd;            /* this load's switched lanes (bit k); those whose cards are set */
static uint32_t scn_gl_tick;
/* a save in progress */
static uint8_t scn_sv_on, scn_sv_slot;
static uint16_t scn_sv_step;                    /* sectors erased, then chunks written */
static uint32_t scn_sv_at, scn_sv_crc;
/* browsing and the save mode */
static uint8_t scn_cand = 0xFF, scn_save_mode, scn_save_slot;
static char scn_nm[12];                         /* the save mode's name (11 characters), its letter, edited or not */
static uint8_t scn_nm_pos, scn_nm_edit;
static const char SCN_CHARS[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-&'.";
static uint32_t scn_cand_t, scn_save_t;

static void ui_say(const char *s);              /* ui.c */
static void ui_all(void);
static void ui_toast(const char *hd, uint32_t idx);
static void ub_rebase(void);                    /* (ui.c: an open sound browser takes the scene's sounds) */

static void scn_name_of(uint32_t idx, char *s)  /* "F003 GARDEN RAIN", "U05 WATER", "U06 ----" (s: 20 bytes) */
{
    uint32_t n = idx < SCN_NF ? idx + 1u : idx - SCN_NF + 1u;
    if (idx < SCN_NF) {
        s[0] = 'F'; s[1] = (char)('0' + n / 100u); s[2] = (char)('0' + n / 10u % 10u); s[3] = (char)('0' + n % 10u);
        s[4] = ' '; s[5] = 0;
        p_cat(s, pr_name(idx), 19);
        return;
    }
    s[0] = 'U'; s[1] = (char)('0' + n / 10u); s[2] = (char)('0' + n % 10u); s[3] = ' '; s[4] = 0;
    if (scn_meta[idx - SCN_NF].used) { char t[13]; p_strcpy(t, scn_meta[idx - SCN_NF].name, 12); p_cat(s, t, 16); }
    else p_cat(s, "----", 16);
}

/* ---- the defaults a load starts from: the grain delay and its SYNC, the reverb, the compressor,
 * the drive, the mix's filter, the GROOVE's AMOUNT TYPE SHIFT (the globals after the filter's), the LFOs, the
 * envelopes, the followers, the matrix, each lane's GROOVE PART and CYCLE (in that order; SCN_DEF_N of them) */
#define SCN_DEF_GLOB 0u                          /* (the lanes' FILTER once came first: retired) */
#define SCN_DEF_LFO (SCN_DEF_GLOB + TX_GD_NP + 1u + TX_RV_NP + TX_CP_NP + TX_DR_NP + TX_MF_NP + 3u)
#define SCN_DEF_ENV (SCN_DEF_LFO + TX_LFO_N * TX_LP_N)
#define SCN_DEF_FOL (SCN_DEF_ENV + TX_ENV_N * TX_EP_N)
#define SCN_DEF_MAT (SCN_DEF_FOL + TX_FOL_N * TX_FP_N)
#define SCN_DEF_GRV (SCN_DEF_MAT + MOD_NS * MQ_N + 1u)   /* each lane's GROOVE PART and CYCLE */
#define SCN_DEF_N (SCN_DEF_GRV + 2u * SC_N)
typedef char scn_grv_after_mf[PG_GRV0 == PG_MF0 + TX_MF_NP ? 1 : -1];   /* (scn_def_id counts on it) */
static uint8_t scn_lfo_def[TX_LFO_N][TX_LP_N];  /* (tx_lfo_init's, per LFO: taken at boot) */
static uint32_t scn_def_id(uint32_t i)          /* the i-th default's param id */
{
    if (i < SCN_DEF_LFO) {
        i -= SCN_DEF_GLOB;
        if (i < TX_GD_NP) return PID(P_GLOBAL, PG_GD(i));
        if (i == TX_GD_NP) return PID(P_GLOBAL, PG_GSYNC);
        i -= TX_GD_NP + 1u;
        if (i < TX_RV_NP) return PID(P_GLOBAL, PG_REVERB0 + i);
        i -= TX_RV_NP;
        if (i >= TX_CP_NP + TX_DR_NP) return PID(P_GLOBAL, PG_MF0 + i - TX_CP_NP - TX_DR_NP);   /* (and GROOVE's,
                                                 * PG_GRV0 .. : right after the filter's) */
        return PID(P_GLOBAL, i < TX_CP_NP ? PG_COMP0 + i : PG_DRIVE0 + i - TX_CP_NP);
    }
    if (i < SCN_DEF_ENV) { i -= SCN_DEF_LFO; return PID(P_LFO, i / TX_LP_N * 16u + i % TX_LP_N); }
    if (i < SCN_DEF_FOL) { i -= SCN_DEF_ENV; return PID(P_ENV, i / TX_EP_N * 16u + i % TX_EP_N); }
    if (i < SCN_DEF_MAT) { i -= SCN_DEF_FOL; return PID(P_ENV, MV_FOL0 + i / TX_FP_N * 16u + i % TX_FP_N); }
    if (i < SCN_DEF_GRV) { i -= SCN_DEF_MAT; return PID(P_MAT, i < MOD_NS * MQ_N ? i : MQ_CC); }
    i -= SCN_DEF_GRV;
    return PID(i / 2u, PL_GPART + (i & 1u));
}
static int32_t scn_def_of(uint32_t id)          /* an id's default (-1: none: the cards, the tempo) */
{
    uint32_t s = P_SCOPE(id), i = P_IDX(id);
    const p_gx_t *g = p_gx(id);                 /* (SYNC; GROOVE: light and loose, each lane AUTO: a scene from before
                                                 * it gets these) */
    if (g) return g->def == PGX_NODEF ? -1 : g->def;
    if (s < SC_N) return -1;
    if (s == P_GLOBAL) {
        if (i < PG_REVERB0) return tx_fx_grain_params[PG_GDE(i)].def;
        if (i >= PG_REVERB0 && i < PG_REVERB0 + TX_RV_NP) return tx_fx_reverb_params[i - PG_REVERB0].def;
        if (i >= PG_COMP0 && i < PG_COMP0 + TX_CP_NP) return tx_comp_params[i - PG_COMP0].def;
        if (i >= PG_DRIVE0 && i < PG_DRIVE0 + TX_DR_NP) return tx_fx_drive_params[i - PG_DRIVE0].def;
        if (i >= PG_MF0 && i < PG_MF0 + TX_MF_NP) return tx_fx_mfilter_params[i - PG_MF0].def;
        return -1;
    }
    if (s == P_LFO) return i / 16u < TX_LFO_N && i % 16u < TX_LP_N ? scn_lfo_def[i / 16u][i % 16u] : -1;
    if (s == P_ENV) {
        if (mod_is_env(i)) return tx_env_params[i % 16u].def;
        if (mod_is_fol(i)) return tx_fol_params[(i - MV_FOL0) % 16u].def;
        return -1;
    }
    if (s == P_MAT) {
        if (i == MQ_CC) return 2;
        if (i >= MOD_NS * MQ_N) return -1;
        return i % MQ_N == MQ_DEPTH ? 100 : i % MQ_N == MQ_SPREAD ? 32 : 0;
    }
    return -1;
}
static void scn_add(scn_t *s, uint32_t id, int32_t v)
{
    if (s->nrec < SCN_REC_MAX) { s->r[s->nrec].id = (uint16_t)id; s->r[s->nrec].v = (int16_t)v; s->nrec++; }
}
static void scn_add_nd(scn_t *s, uint32_t id, int32_t v)   /* a record unless it is the default */
{
    if (scn_def_of(id) != v) scn_add(s, id, v);
}
static int scn_rec_of(const scn_t *s, uint32_t id)   /* the scene's record of id (-1: none) */
{
    uint32_t i;
    for (i = 0; i < s->nrec; i++) if (s->r[i].id == id) return (int)i;
    return -1;
}

/* ---- capture: the scene that plays now */
static void scn_capture(scn_t *s)
{
    uint32_t k, i;
    memset(s, 0, sizeof *s);
    s->ver = SCN_VER; s->used = 1; s->fam = sc.fam;
    if (scn_cur != 0xFF && scn_cur >= SCN_NF && scn_meta[scn_cur - SCN_NF].used) memcpy(s->name, scn_meta[scn_cur - SCN_NF].name, 12);
    else p_upper(s->name, LS[sc.fam].name, 11);
    for (k = 0; k < SC_N; k++) {
        p_cards_t *c = p_cards(k);
        s->snd[k] = sc.snd[k];
        s->cnt[k] = m_cb_ok[k] && sc.run[k] && m_cbase[k] ? m_cbase[k] : sc.cnt[k];   /* (HOW MANY modulated: its base) */
        s->step[k] = sc.step[k]; s->run[k] = sc.run[k];
        s->vol[k] = (uint8_t)sc_vol_of(k); s->pan[k] = sc.pan[k];
        if (sc.mute[k]) s->mute |= (uint8_t)(1u << k);
        if (sc.snd[k] >= 0) s->lfam |= (uint16_t)((sc.lf[k] + 1u) << (4u * k));
        for (i = 0; i < c->n; i++) if (c->cv[i] != 0xFF) scn_add(s, PID(k, PL_CARD0 + i), c->cv[i]);   /* (the ones set) */
    }
    scn_add(s, PID(P_GLOBAL, PG_FINE), (int32_t)tempo_bpm10() - 400);
    for (i = 0; i < SCN_DEF_N; i++) { uint32_t id = scn_def_id(i); scn_add_nd(s, id, p_get(id)); }
}
/* a factory scene: preset i (presets.c), its effects as records (PR_FX: the values off their defaults), the
 * modulation at its defaults; the tempo stays */
static void scn_factory(scn_t *s, uint32_t i)
{
    static const uint8_t FX_ID[PR_FX_NP] = {PG_REVERB0 + TX_RV_TYPE, PG_REVERB0 + TX_RV_MIX, PG_REVERB0 + TX_RV_DECAY,
        PG_REVERB0 + TX_RV_TONE, PG_REVERB0 + TX_RV_PREDELAY, PG_GRAIN0 + TX_GD_MIX, PG_GRAIN0 + TX_GD_TIME,
        PG_GRAIN0 + TX_GD_FEEDBACK, PG_GRAIN0 + TX_GD_TONE, PG_GRAIN0 + TX_GD_GRAIN};
    uint32_t k, fx = pr_fx_of(i);
    int first = 1;
    memset(s, 0, sizeof *s);
    s->ver = SCN_VER; s->used = 1;
    p_strcpy(s->name, pr_name(i), 11);
    for (k = 0; k < SC_N; k++) {
        pr_lane_v_t l;
        pr_lane(i, k, &l);
        s->snd[k] = l.snd; s->vol[k] = l.vol; s->pan[k] = l.pan;
        if (l.snd < 0) continue;
        if (first) { s->fam = (uint8_t)l.fam; first = 0; }   /* (the landscape: the first lane's family) */
        s->lfam |= (uint16_t)((l.fam + 1u) << (4u * k));
        if (l.mute) s->mute |= (uint8_t)(1u << k);
        if (l.cnt) { s->cnt[k] = l.cnt; s->step[k] = (uint8_t)sc_act_step(l.act); s->run[k] = 1; }
    }
    if (fx >= PR_NFX) fx = 0;
    for (k = 0; k < PR_FX_NP; k++) scn_add_nd(s, PID(P_GLOBAL, FX_ID[k]), PR_FX[fx][k]);
}

/* ---- the glides: a load sets a lane's numbers (its cards) and the effects' amounts over SCN_GLIDE_MS, from
 * what they are to the scene's, so a lane that goes on playing never steps and an effect's tail rings on; the choices
 * (a TYPE, a SHAPE, a card of under 16 values), the delay's TIME (its line glides there itself) and everything else
 * are set at once */
static int scn_fx_id(uint32_t id);
static int scn_glides(uint32_t id)              /* an id a load glides to its value */
{
    if (p_fmt(id) == F_ENUM || p_hi(id) < 16) return 0;
    if (P_SCOPE(id) < SC_N) return P_IDX(id) >= PL_CARD0;
    return P_SCOPE(id) == P_GLOBAL && scn_fx_id(id) && P_IDX(id) != PG_GRAIN0 + TX_GD_TIME;
}
static void scn_put(uint32_t id, int32_t v)
{
    uint32_t i;
    int32_t f;
    if (!scn_glides(id) || (f = p_get(id)) == v) {
        p_set(id, v);
        return;
    }
    for (i = 0; i < scn_gl_n && scn_gl[i].id != id; i++) {}
    if (i == SCN_GL_N) { p_set(id, v); return; }   /* (no room: at once) */
    if (i == scn_gl_n) scn_gl_n++;
    scn_gl[i].id = (uint16_t)id; scn_gl[i].t0 = (uint16_t)fm1_ms; scn_gl[i].from = (int16_t)f; scn_gl[i].to = (int16_t)v;
}
static void scn_gl_drop(uint32_t k)             /* lane k's glides dropped (its sound changes: other cards) */
{
    uint32_t i = 0;
    while (i < scn_gl_n) if (P_SCOPE(scn_gl[i].id) == k) scn_gl[i] = scn_gl[--scn_gl_n]; else i++;
}
static void scn_gl_poll(void)
{
    uint32_t i = 0;
    if (!scn_gl_n || fm1_ms - scn_gl_tick < SCN_GLIDE_TICK || ln.qw - ln.qr >= LN_QN / 2u) return;
    scn_gl_tick = fm1_ms;
    while (i < scn_gl_n) {
        uint32_t t = (uint16_t)((uint16_t)fm1_ms - scn_gl[i].t0), id = scn_gl[i].id;
        int32_t v = t >= SCN_GLIDE_MS ? scn_gl[i].to
                  : scn_gl[i].from + (scn_gl[i].to - scn_gl[i].from) * (int32_t)t / (int32_t)SCN_GLIDE_MS;
        if (p_valid(id) && p_get(id) != v) p_set(id, v);
        if (t >= SCN_GLIDE_MS) scn_gl[i] = scn_gl[--scn_gl_n];   /* (there: done) */
        else i++;
    }
}

/* ---- load: the lanes, then (their engines in) the records, a slice a pass */
enum { SCN_LANE_NEW, SCN_LANE_SAME, SCN_LANE_SWAP };
/* the lane's cards back to the sound's own (none set). SAME (the lane keeps its sound and plays on): the ones the
 * scene has a record of stay for it (they glide there), the others glide home; SWAP (its engine switches): dropped,
 * nothing sent (the old engine rings out as it was, the new one starts at its own) */
static void scn_cards_reset(uint32_t k, const scn_t *s, int how)
{
    p_cards_t *c = p_cards(k);
    uint32_t i;
    for (i = 0; i < c->n; i++) {
        uint32_t id = PID(k, PL_CARD0 + i);
        if (c->cv[i] == 0xFF) continue;
        if (how == SCN_LANE_SAME && scn_rec_of(s, id) >= 0) continue;
        if (how == SCN_LANE_SAME && !(c->kind[i] & CK_HOST)) { scn_put(id, c->def[i]); continue; }
        c->cv[i] = 0xFF;
        if (how == SCN_LANE_SWAP) continue;
        if (c->kind[i] & CK_HOST) ln_host(k, c->id[i], 0, 1);   /* (released: the source's curves again) */
        else ln_param(k, c->id[i], c->kind[i] & CK_INV ? c->hi[i] - c->def[i] : c->def[i]);
    }
}
/* the scene's lanes. A lane that keeps its sound plays on untouched: its level, pan, HOW MANY and HOW ACTIVE glide
 * (scape.c sc_glide). A lane that gets another sound switches: its old engine plays on till the spare is free (one
 * lane at a time), then rings out under the new one (lanes.c); the lanes the scene leaves empty stop (their tails
 * ring). No landscape start is loaded first: the scene names its landscape, its lanes are its own */
static void scn_lanes(const scn_t *s)
{
    uint32_t k, f = s->fam % LS_N;
    if (f != sc.fam) { sc.fam = (uint8_t)f; sc.fam_want = (uint8_t)f; }
    sc.fam_pend = 0;
    for (k = 0; k < SC_N; k++) {
        uint32_t lf = s->lfam >> (4u * k) & 15u;
        int how = SCN_LANE_NEW;
        lf = lf && lf <= LS_N ? lf - 1u : f;
        if (s->snd[k] >= 0 && (uint32_t)s->snd[k] >= LS[lf].n) lf = LS_N;   /* (one out of its family's range: none) */
        if (lf < LS_N && s->snd[k] >= 0) {
            const ls_sound_t *d = &LS[lf].s[s->snd[k]];
            const tx_engine_t *e = d->eng ? tx_engine_find(d->src) : sc.src;
            if (sc.snd[k] == s->snd[k] && sc.lf[k] == lf) how = SCN_LANE_SAME;
            else if (e && sc_swaps(k, d, e)) how = SCN_LANE_SWAP;
        }
        scn_cards_reset(k, s, how);
        if (how == SCN_LANE_SWAP) scn_ap_sw |= (uint8_t)(1u << k);
        sc.mem_cnt[k] = 0; m_cb_ok[k] = 0;
        sc.mute[k] = (uint8_t)(lf < LS_N && s->snd[k] >= 0 ? s->mute >> k & 1u : 0u);
        if (how != SCN_LANE_SAME) {
            scn_gl_drop(k);
            if (sc.run[k] && how != SCN_LANE_SWAP) sc_start(k, 0);   /* (a switch: sc_load lets it play on) */
            sc.snd[k] = -1;
            if (lf >= LS_N || s->snd[k] < 0) { sc_sync(k); continue; }
            sc_load(k, lf, (uint32_t)s->snd[k]);
            if (sc.snd[k] < 0) { sc_sync(k); continue; }   /* (refused: its engine not in this build; the lane
                                                 * stays empty, no sound's tables touched at -1) */
        }
        sc_glide(k);
        sc.vol[sc.lf[k]][sc.snd[k]] = s->vol[k] > 127u ? 127u : s->vol[k];
        sc.pan[k] = s->pan[k] > 127u ? 127u : s->pan[k];
        if (s->run[k] && s->cnt[k]) {
            sc.cnt[k] = (uint8_t)(s->cnt[k] < LS_NCNT ? s->cnt[k] : LS_NCNT - 1u);
            sc.step[k] = (uint8_t)(s->step[k] < 1u ? 1u : s->step[k] > SC_STEPS ? SC_STEPS : s->step[k]);
            if (!sc.run[k]) sc_start(k, 1);
            else sc_sync(k);
        } else if (sc.run[k]) sc_start(k, 0);
        else sc_sync(k);
    }
    if (sc.sel >= SC_N || sc.snd[sc.sel] < 0) sc.sel = 0;
}
static void scn_fx_early(const scn_t *s);
static void scn_apply(const scn_t *s, uint32_t idx)
{
    uint32_t i, best = 0;
    scn_ap = s; scn_ap_idx = (uint8_t)idx;
    scn_ap_fx = (uint8_t)(scn_fx_boot || scn_fx_force || scn_fx_mode == SCN_FX_ALWAYS || (scn_fx_mode == SCN_FX_SMART && !scn_fx_mine));
    if (scn_ap_fx) scn_fx_mine = 0;             /* (the effects the scene's again) */
    scn_fx_force = 0;
    scn_ap_drv = 0xFF;                          /* (a lane DRIVE's records, retired: the most AMOUNT's go to the mix's) */
    for (i = 0; i < s->nrec; i++)
        if (P_SCOPE(s->r[i].id) < SC_N && P_IDX(s->r[i].id) == PL_ODRIVE0 + TX_DR_AMOUNT && s->r[i].v > (int32_t)best) {
            best = (uint32_t)s->r[i].v; scn_ap_drv = (uint8_t)P_SCOPE(s->r[i].id);
        }
    scn_ap_sw = scn_ap_cd = 0;
    if (scn_ap_fx) scn_fx_early(s);
    scn_lanes(s);
    ub_rebase();
    scn_ap_stage = 1; scn_ap_t = fm1_ms; scn_ap_i = 0; scn_ap_d = 0;
}
static int scn_busy(void) { return scn_ap_stage != 0; }
/* the mix's effects, set as the load starts (not after its lanes' engines are in: they glide while the lanes switch,
 * so the place changes as one): the defaults the scene has no record of, then its records */
static void scn_fx_early(const scn_t *s)
{
    uint32_t i;
    for (i = 0; i < SCN_DEF_N; i++) {
        uint32_t id = scn_def_id(i);
        if (P_SCOPE(id) == P_GLOBAL && scn_fx_id(id) && scn_rec_of(s, id) < 0) scn_put(id, scn_def_of(id));
    }
    for (i = 0; i < s->nrec; i++)
        if (P_SCOPE(s->r[i].id) == P_GLOBAL && scn_fx_id(s->r[i].id) && p_valid(s->r[i].id)) scn_put(s->r[i].id, s->r[i].v);
}
/* the mix's effects: the DRIVE, its FILTER, the COMPRESSOR, the GRAIN delay (and its SYNC), the REVERB, and a retired lane DRIVE's
 * records. A load sets them or leaves them as they are (their tails ring on) as EFFECTS ON LOAD says (scn_apply) */
static int scn_fx_id(uint32_t id)
{
    uint32_t s = P_SCOPE(id), i = P_IDX(id);
    if (s < SC_N) return i >= PL_ODRIVE0 && i < PL_ODRIVE0 + TX_DR_NP;
    if (s != P_GLOBAL) return 0;
    return i < PG_COMP0 + TX_CP_NP || (i >= PG_DRIVE0 && i < PG_DRIVE0 + TX_DR_NP) || (i >= PG_MF0 && i < PG_MF0 + TX_MF_NP);
}
/* the player changed id (a knob, MIDI, the web): the scene edited ("*"), unless it is a setting (the tempo, the clock,
 * the screen: no scene keeps them); an effect: the effects theirs (SMART keeps them) */
static void scn_touch_id(uint32_t id)
{
    const p_gx_t *g = p_gx(id);
    if (g && g->def == PGX_NODEF) return;
    scn_edited = 1;
    if (scn_fx_id(id)) scn_fx_mine = 1;
}
static void scn_touch(void) { scn_edited = 1; }   /* (a lane's volume, sound, HOW MANY, mute ...) */
/* a switched lane's cards, as soon as its fresh engine is in: set at once, before it is heard much (the others'
 * glide, stage 2) */
static void scn_apply_cards(void)
{
    uint32_t k, i;
    for (k = 0; k < SC_N; k++) {
        if (!((scn_ap_sw & ~scn_ap_cd) >> k & 1u)) continue;
        if (sc.a_eng[k] != sc.eng[k] || sc.reswap[k] || fm1_ms - sc.sel_t[k] < SCN_CARDS_MS) continue;
        if (ln.qw - ln.qr >= LN_QN / 2u) return;
        (void)p_cards(k);
        for (i = 0; i < scn_ap->nrec; i++) {
            uint32_t id = scn_ap->r[i].id;
            if (P_SCOPE(id) == k && P_IDX(id) >= PL_CARD0 && P_IDX(id) < PL_CARD0 + SCD_MAX && p_valid(id)) p_set(id, scn_ap->r[i].v);
        }
        scn_ap_cd |= (uint8_t)(1u << k);
    }
}
static void scn_apply_poll(void)
{
    uint32_t k, n = 0;
    if (scn_ap_stage) scn_apply_cards();
    if (scn_ap_stage == 1) {                    /* the lanes' engines switched (and their hosted ones in): the cards */
        if (fm1_ms - scn_ap_t < SCN_SETTLE_MS) return;
        for (k = 0; k < SC_N; k++) if (sc.a_eng[k] != sc.eng[k] || sc.reswap[k]) return;   /* (a lane still waiting
                                                 * for the spare: the last switch rings out) */
        for (k = 0; k < SC_N; k++) (void)p_cards(k);
        scn_ap_stage = 2;
    }
    if (scn_ap_stage != 2) return;
    while (scn_ap_d < SCN_DEF_N && n < SCN_SLICE && ln.qw - ln.qr < LN_QN / 2u) {   /* the defaults the scene has no
                                                         * record of (theirs come next: no value set twice) */
        uint32_t id = scn_def_id(scn_ap_d++);
        if (scn_fx_id(id)) continue;            /* (the effects: set as the load started, or kept) */
        if (scn_rec_of(scn_ap, id) < 0) { scn_put(id, scn_def_of(id)); n++; }
    }
    while (scn_ap_d >= SCN_DEF_N && scn_ap_i < scn_ap->nrec && n < SCN_SLICE && ln.qw - ln.qr < LN_QN / 2u) {
        const scn_rec_t *r = &scn_ap->r[scn_ap_i++];
        uint32_t rs = P_SCOPE(r->id), ri = P_IDX(r->id);
        if (scn_fx_id(r->id) && (rs == P_GLOBAL || !scn_ap_fx)) continue;   /* (set as the load started, or kept;
                                                 * a lane's DRIVE, retired: below) */
        if (rs == P_GLOBAL && ri == PG_FINE) { if (tm.scn_tempo) tempo_set10((uint32_t)(r->v + 400)); }
        else if (rs < SC_N && ri >= PL_ODRIVE0 && ri < PL_ODRIVE0 + TX_DR_NP) {   /* (a lane's DRIVE, retired) */
            if (rs == scn_ap_drv) p_set(PID(P_GLOBAL, PG_DRIVE0 + ri - PL_ODRIVE0), r->v);
        }
        else if (rs < SC_N && ri >= PL_CARD0 && ri < PL_CARD0 + SCD_MAX && (scn_ap_cd >> rs & 1u)) continue;   /* (set) */
        else if (rs == P_MAT && ri < MOD_NS * MQ_N && ri % MQ_N == MQ_TGT && mod_tgt_retired((uint32_t)r->v)) {}
                                                /* (a route onto a lane's FILTER, retired: dropped, the slot's TARGET
                                                 * stays NONE) */
        else if (p_valid(r->id)) scn_put(r->id, r->v);
        n++;
    }
    if (scn_ap_d < SCN_DEF_N) return;
    if (scn_ap_i >= scn_ap->nrec) {
        if (ln_fx(LN_FX_RELATCH, 0, 0, 0)) return;   /* (the reverb's PLATE takes the new MIX's size at once; a full
                                                     * queue: the next pass) */
        scn_ap_stage = 0;
        scn_cur = scn_ap_idx;
        scn_edited = 0;
        ui_all();
    }
}
static int scn_read(uint32_t i, scn_t *s)       /* user scene i from the bank: 0 if it is there */
{
    if (!scn_bank || i >= SCN_NU || scn_rd(scn_bank + sizeof(scn_hdr_t) + i * SCN_SLOT, s, sizeof *s)) return -1;
    if (s->ver != SCN_VER || !s->used || s->nrec > SCN_REC_MAX) return -1;
    return 0;
}
static int scn_load(uint32_t idx)               /* 0: loading */
{
    scn_t *t = &scn_b[scn_u ^ 1u];
    if (idx >= SCN_N || scn_busy()) return -1;
    if (idx < SCN_NF) scn_factory(t, idx);
    else if (!scn_meta[idx - SCN_NF].used || scn_read(idx - SCN_NF, t)) return -1;   /* (t: no load runs from it now) */
    scn_capture(&scn_b[scn_u]);                 /* (what played: the undo) */
    scn_bidx[scn_u] = scn_cur;
    scn_undo_ok = 1;
    scn_su.on = 0;
    scn_apply(t, idx);
    ui_toast("LOADED", idx);                    /* (big, ~1.5 s: a load is never missed) */
    return 0;
}
/* the last sound the sound browser kept (EDIT): SAVE held puts the one before back on its lane (again: redone), as a
 * load's undo; a load after it: the load's undo again */
static void scn_snd_undo_set(uint32_t k, uint32_t f, uint32_t i) { scn_su.on = 1; scn_su.k = (uint8_t)k; scn_su.f = (uint8_t)f; scn_su.i = (uint8_t)i; }
static void scn_snd_undo(void)
{
    uint32_t k = scn_su.k, f = sc.lf[k];
    int i = sc.snd[k];
    char t[32];
    if (i < 0) return;
    sc_sound_set(k, scn_su.f, scn_su.i);
    scn_su.f = (uint8_t)f; scn_su.i = (uint8_t)i;
    scn_touch();
    p_strcpy(t, "UNDO: LANE 1 ", 31); t[11] = (char)('1' + k);
    p_upper(t + 13, sc_sound_name(k), 18);
    ui_say(t);
}
static void scn_undo(void)                      /* the last load undone (again: redone) */
{
    if (!scn_undo_ok || scn_busy()) return;
    scn_capture(&scn_b[scn_u ^ 1u]);
    scn_bidx[scn_u ^ 1u] = scn_cur;
    scn_apply(&scn_b[scn_u], scn_bidx[scn_u]);
    scn_u ^= 1u;
    ui_toast("UNDO: BACK", scn_bidx[scn_u ^ 1u]);
}

/* ---- the bank */
static int scn_bank_valid(uint32_t at, scn_hdr_t *h)
{
    uint8_t b[SCN_CHUNK];
    uint32_t c = 0xFFFFFFFFu, o;
    if (scn_rd(at, h, sizeof *h) || h->magic != SCN_MAGIC || h->len != SCN_NU * SCN_SLOT) return 0;
    for (o = 0; o < h->len; o += SCN_CHUNK) {
        if (scn_rd(at + sizeof *h + o, b, SCN_CHUNK)) return 0;
        c = scn_crc(c, b, SCN_CHUNK);
    }
    return ~c == h->crc;
}
static void scn_meta_load(void)
{
    uint32_t i;
    scn_t *t = &scn_b[scn_u ^ 1u];
    for (i = 0; i < SCN_NU; i++) {
        scn_meta[i].used = 0;
        if (!scn_read(i, t)) { scn_meta[i].used = 1; scn_meta[i].fam = t->fam; memcpy(scn_meta[i].name, t->name, 12); }
    }
}
static void scn_bank_load(void)
{
    scn_hdr_t ha, hb;
    int va = scn_bank_valid(SCN_BANK_A, &ha), vb = scn_bank_valid(SCN_BANK_B, &hb);
    scn_bank = 0; scn_seq = 0;
    if (va && (!vb || (int32_t)(ha.seq - hb.seq) > 0)) { scn_bank = SCN_BANK_A; scn_seq = ha.seq; }
    else if (vb) { scn_bank = SCN_BANK_B; scn_seq = hb.seq; }
    scn_meta_load();
}
static void scn_nm_take(void)                   /* the name the save mode starts from: the slot's, or the landscape's */
{
    uint32_t i;
    memset(scn_nm, 0, sizeof scn_nm);
    if (scn_meta[scn_save_slot].used) memcpy(scn_nm, scn_meta[scn_save_slot].name, 11);
    else p_upper(scn_nm, LS[sc.fam].name, 11);
    for (i = 0; i < 11u; i++) if (!scn_nm[i]) scn_nm[i] = ' ';   /* (spaces to the end: any letter can be set) */
}
static void scn_nm_turn(int32_t dpos, int32_t dch)   /* K2: the letter; K3: its character */
{
    const int32_t n = (int32_t)sizeof SCN_CHARS - 1;
    int32_t p = (int32_t)scn_nm_pos + dpos, j = 0;
    scn_nm_pos = (uint8_t)(p < 0 ? 0 : p > 10 ? 10 : p);
    if (!dch) return;
    while (j < n && SCN_CHARS[j] != scn_nm[scn_nm_pos]) j++;
    if (j == n) j = 0;                          /* (a character not on the list: from the space) */
    j = (j + dch % n + n) % n;
    scn_nm[scn_nm_pos] = SCN_CHARS[j];
    scn_nm_edit = 1;
}
static void scn_save_start(uint32_t slot)       /* the scene that plays now into user scene slot */
{
    int32_t i;
    scn_capture(&scn_sv);
    memset(scn_sv.name, 0, sizeof scn_sv.name);  /* the name as set (trailing spaces off; all spaces: the landscape's) */
    memcpy(scn_sv.name, scn_nm, 11);
    for (i = 10; i >= 0 && (scn_sv.name[i] == ' ' || !scn_sv.name[i]); i--) scn_sv.name[i] = 0;
    if (i < 0) p_upper(scn_sv.name, LS[sc.fam].name, 11);
    scn_sv_slot = (uint8_t)slot;
    scn_sv_at = scn_bank == SCN_BANK_A ? SCN_BANK_B : SCN_BANK_A;   /* (the older copy) */
    scn_sv_step = 0; scn_sv_crc = 0xFFFFFFFFu; scn_sv_on = 1;
    ui_say("SAVING...");
}
static void scn_save_fail(void) { scn_sv_on = 0; ui_say("SAVE FAILED"); }
static void scn_save_poll(void)                 /* a step a pass: a sector erased, or a chunk written */
{
    const uint32_t nch = SCN_NU * (SCN_SLOT / SCN_CHUNK);
    if (!scn_sv_on) return;
    if (scn_sv_step < SCN_BANK_SECT) {
        if (scn_er(scn_sv_at + scn_sv_step * 0x1000u)) { scn_save_fail(); return; }
        scn_sv_step++;
        return;
    }
    if (scn_sv_step < SCN_BANK_SECT + nch) {
        uint32_t j = scn_sv_step - SCN_BANK_SECT, rec = j / (SCN_SLOT / SCN_CHUNK), off = j % (SCN_SLOT / SCN_CHUNK) * SCN_CHUNK;
        uint8_t b[SCN_CHUNK];
        if (rec == scn_sv_slot) memcpy(b, (const uint8_t *)&scn_sv + off, SCN_CHUNK);
        else if (!scn_bank || scn_rd(scn_bank + sizeof(scn_hdr_t) + rec * SCN_SLOT + off, b, SCN_CHUNK)) memset(b, 0, SCN_CHUNK);
        scn_sv_crc = scn_crc(scn_sv_crc, b, SCN_CHUNK);
        if (scn_wr(scn_sv_at + sizeof(scn_hdr_t) + rec * SCN_SLOT + off, b, SCN_CHUNK)) { scn_save_fail(); return; }
        scn_sv_step++;
        return;
    }
    {                                           /* the header last; then it is the bank */
        scn_hdr_t h, v;
        char m[20];
        h.magic = SCN_MAGIC; h.seq = scn_seq + 1u; h.len = SCN_NU * SCN_SLOT; h.crc = ~scn_sv_crc;
        if (scn_wr(scn_sv_at, &h, sizeof h) || !scn_bank_valid(scn_sv_at, &v)) { scn_save_fail(); return; }
        scn_bank = scn_sv_at; scn_seq = h.seq; scn_sv_on = 0;
        scn_meta[scn_sv_slot].used = 1; scn_meta[scn_sv_slot].fam = scn_sv.fam;
        memcpy(scn_meta[scn_sv_slot].name, scn_sv.name, 12);
        scn_cur = (uint8_t)(SCN_NF + scn_sv_slot);
        scn_edited = 0;
        p_strcpy(m, "SAVED ", 19);
        { char n[20]; scn_name_of(scn_cur, n); p_cat(m, n, 19); }
        ui_say(m);
        ui_all();
    }
}

/* ---- the panel: PRESETS, SAVE (tap: the save mode, held: undo), K1 and HOME in the save mode. 1: the input was
 * the save mode's (K1 and HOME taken) */
static int scn_input(uint32_t b, uint32_t rel, int32_t d_presets, int32_t d_k1, int32_t d_k2, int32_t d_k3)
{
    if (b >> BTN_SAVE & 1u) scn_save_t = fm1_ms ? fm1_ms : 1u;
    if (scn_save_t && fm1_in.buttons >> BTN_SAVE & 1u && fm1_ms - scn_save_t >= SCN_UNDO_MS) {
        scn_save_t = 0; scn_save_mode = 0;
        if (scn_su.on) scn_snd_undo(); else scn_undo();
        return 1;
    }
    if (rel >> BTN_SAVE & 1u && scn_save_t) {   /* a tap: the save mode, or (in it) save */
        scn_save_t = 0;
        if (!scn_save_mode) {
            if (scn_sv_on) { ui_say("SAVING..."); return 1; }
            scn_save_mode = 1;
            scn_save_slot = (uint8_t)(scn_cur != 0xFF && scn_cur >= SCN_NF ? scn_cur - SCN_NF : 0u);
            if (scn_cur == 0xFF || scn_cur < SCN_NF) {   /* (from a factory scene: the first empty one) */
                uint32_t i;
                for (i = 0; i < SCN_NU; i++) if (!scn_meta[i].used) { scn_save_slot = (uint8_t)i; break; }
            }
            scn_nm_take(); scn_nm_pos = 0; scn_nm_edit = 0;
            ui_all();
            return 1;
        }
        scn_save_mode = 0;
        scn_save_start(scn_save_slot);
        ui_all();
        return 1;
    }
    if (scn_save_mode) {
        int32_t d = d_presets + d_k1;
        if (d) {
            int32_t v = (int32_t)scn_save_slot + d;
            scn_save_slot = (uint8_t)(v < 0 ? 0 : v > (int32_t)SCN_NU - 1 ? SCN_NU - 1u : (uint32_t)v);
            if (!scn_nm_edit) scn_nm_take();    /* (a name being written stays) */
            ui_all();
        }
        if (d_k2 || d_k3) { scn_nm_turn(d_k2, d_k3); ui_all(); }
        if ((b >> BTN_HOME | b >> BTN_OCT_DOWN) & 1u) { scn_save_mode = 0; ui_say("NOT SAVED"); ui_all(); }
        return 1;                               /* (the save mode takes K1 .. K3, HOME, OCT-) */
    }
    return 0;                                   /* (PRESETS outside the save mode: the presets browser, ui.c) */
}
static void scn_poll_cand(void)                 /* the scene PRESETS (a program change) asked for, once no load runs */
{
    if (scn_cand != 0xFF && fm1_ms - scn_cand_t >= SCN_BROWSE_MS && !scn_busy()) {
        uint8_t c = scn_cand;
        scn_cand = 0xFF;
        if (scn_load(c)) ui_say("NO SCENE THERE");
    }
}
/* the presets list (ui.c): EDIT loads the preset under the cursor, at once (or after the load running), the last one
 * asked wins; with its effects (whatever EFFECTS ON LOAD says) when fx: EDIT again on the one loaded */
static void scn_pb_go(uint32_t idx, int fx) { scn_cand = (uint8_t)idx; scn_cand_t = fm1_ms - SCN_BROWSE_MS; scn_fx_force = (uint8_t)(fx != 0); scn_poll_cand(); }
static uint32_t scn_user_n(void) { uint32_t i, n = 0; for (i = 0; i < SCN_NU; i++) n += scn_meta[i].used; return n; }
static uint32_t scn_user_at(uint32_t j)         /* the j-th saved user scene's slot */
{
    uint32_t i;
    for (i = 0; i < SCN_NU; i++) if (scn_meta[i].used && !j--) return i;
    return 0;
}
static int pr_load(uint32_t i)                  /* (presets.c) preset i, as a program change: at once, or when the
                                                 * load still running is done (the last one asked) */
{
    if (i >= PR_N) return -1;
    scn_cand = (uint8_t)i; scn_cand_t = fm1_ms - SCN_BROWSE_MS;
    scn_poll_cand();
    return 0;
}
static void scn_pc(uint32_t p)                  /* MIDI program change (channel 1): as PRESETS, at once (a load still
                                                 * running: when it is done, the last one asked) */
{
    if (p < SCN_NF) scn_cand = (uint8_t)p;
    else if (p >= SCN_PC_U && p < SCN_PC_U + SCN_NU) scn_cand = (uint8_t)(SCN_NF + p - SCN_PC_U);
    else return;
    scn_cand_t = fm1_ms - SCN_BROWSE_MS;
    scn_poll_cand();
}

/* ---- the settings: one small record, saved SET_SAVE_MS after its last change (the platform's split save) */
typedef struct { uint8_t ver, scene, clock, in_usb, in_trs, palette, scn_tempo, out_off; uint16_t bpm10; uint8_t scn_fx, layout;
                 uint8_t scale, root, selch, lch[SC_N], spare, scm, scf, sct, spare2; } tx_set_t;
                                                /* (scn_tempo, out_off, palette, scn_fx, layout: once spare bytes, 0 = as
                                                 * before: off, USB, RED, SMART, the scenes before the presets. scn_fx: 0
                                                 * SMART, 1 ALWAYS, 2 NEVER: the KEEP and RECALL of before read as SMART
                                                 * and ALWAYS. scale .. lch appended: the record of before, SET_OLD_SZ
                                                 * bytes, still loads, with them at 0 = the defaults: CHROMATIC, C, the
                                                 * channels as before (each stored XOR its default: SELECTED 1, lane n n).
                                                 * scm .. appended (Frodhes' SCALE page): MODE + 1 (0: a record of before,
                                                 * SET_MID_SZ bytes: MODE and SCALE from its scale, CHROMATIC = OFF, the
                                                 * others SNAP), SCALE (Frodhes' list), TRANSP + 24; scale then keeps the
                                                 * old list's number (OFF: 0), for a firmware of before) */
#define SET_OLD_SZ 12u
#define SET_MID_SZ 20u
_Static_assert(sizeof(tx_set_t) == 24u && SET_OLD_SZ == 12u && SET_MID_SZ == 20u, "the settings: the old records, then the appended bytes");
/* the old SCALE list's numbers (CHROMATIC MAJOR MINOR DORIAN PHRYGIAN LYDIAN MIXOLYDIAN HARMONIC MELODIC MAJ PENTA MIN
 * PENTA BLUES) <-> Frodhes' (MAJOR MINOR DORIAN MIXO LYDIAN PHRYG HARMON MELODIC PENTA MPENTA BLUES) */
static const uint8_t SET_SC_OLD[NT_NSCALE] = {1, 2, 3, 6, 5, 4, 7, 8, 9, 10, 11};   /* (Frodhes' n: the old number) */
static uint8_t set_fx_of(uint32_t m) { return (uint8_t)(m == SCN_FX_ALWAYS ? 1u : m == SCN_FX_NEVER ? 2u : 0u); }
static uint8_t set_fx_mode(uint32_t b) { return (uint8_t)(b == 1u ? SCN_FX_ALWAYS : b == 2u ? SCN_FX_NEVER : SCN_FX_SMART); }
static tx_set_t set_saved, set_now;
static uint8_t set_dirty, set_saving;
static uint32_t set_t;
static void set_take(tx_set_t *s)
{
    uint32_t k;
    memset(s, 0, sizeof *s);
    s->ver = SET_VER; s->scene = (uint8_t)(scn_cur == 0xFF ? 0u : scn_cur + 1u);
    s->clock = (uint8_t)tm.clock; s->in_usb = tm.in_usb; s->in_trs = tm.in_trs; s->bpm10 = (uint16_t)tempo_bpm10();
    s->scn_tempo = tm.scn_tempo; s->out_off = (uint8_t)!tm.out_usb; s->palette = p_color; s->scn_fx = set_fx_of(scn_fx_mode);
    s->layout = SET_LAYOUT;
    s->scale = (uint8_t)(nt_scmode == NT_OFF ? 0u : SET_SC_OLD[nt_scale]); s->root = nt_root;
    s->scm = (uint8_t)(nt_scmode + 1u); s->scf = nt_scale; s->sct = (uint8_t)(nt_trans + 24);
    s->selch = (uint8_t)(nt_selch ^ 1u);
    for (k = 0; k < SC_N; k++) s->lch[k] = (uint8_t)(nt_lch[k] ^ (k + 1u));
}
static void set_poll(void)
{
    if (set_saving) { set_saving = (uint8_t)(settings_save_poll() > 0); return; }
    set_take(&set_now);
    if (memcmp(&set_now, &set_saved, sizeof set_now)) {
        if (!set_dirty) set_t = fm1_ms;
        set_dirty = 1;
        if (fm1_ms - set_t >= SET_SAVE_MS && !scn_busy() && !scn_sv_on) {
            int rc = settings_save_start(&set_now, sizeof set_now);
            if (rc >= 0) { set_saved = set_now; set_dirty = 0; set_saving = (uint8_t)(rc > 0); }
            else set_t = fm1_ms;                /* (failed: again later) */
        }
    } else set_dirty = 0;
}
/* the boot: the bank's names, the settings, the scene loaded last */
static void scn_init(void)
{
    tx_set_t s;
    uint32_t k, i;
    for (k = 0; k < TX_LFO_N; k++) {            /* (the LFOs' defaults, as a factory scene has them) */
        static tx_lfo_t l;
        tx_lfo_init(&l, k, 1u);
        for (i = 0; i < TX_LP_N; i++) scn_lfo_def[k][i] = l.p[i];
    }
    scn_bank_load();
    memset(&s, 0, sizeof s);
    if ((!settings_load(&s, sizeof s) || !settings_load(&s, SET_MID_SZ) || !settings_load(&s, SET_OLD_SZ))
        && s.ver == SET_VER) {
        if (s.scm) {                            /* (Frodhes' SCALE page saved it) */
            p_set(PID(P_GLOBAL, PG_SCMODE), s.scm - 1u); p_set(PID(P_GLOBAL, PG_SCALE), s.scf);
            p_set(PID(P_GLOBAL, PG_TRANS), s.sct);
        } else {                                /* (a record of before: its old list's number) */
            for (k = 0; k < NT_NSCALE && SET_SC_OLD[k] != s.scale; k++) {}
            p_set(PID(P_GLOBAL, PG_SCMODE), k < NT_NSCALE ? NT_SNAP : NT_OFF);
            p_set(PID(P_GLOBAL, PG_SCALE), k < NT_NSCALE ? k : 0u);
            p_set(PID(P_GLOBAL, PG_TRANS), 24);
        }
        p_set(PID(P_GLOBAL, PG_ROOT), s.root);
        p_set(PID(P_GLOBAL, PG_SELCH), s.selch ^ 1u);
        for (k = 0; k < SC_N; k++) p_set(PID(P_GLOBAL, PG_LCH0 + k), s.lch[k] ^ (k + 1u));   /* (p_set keeps them in range) */
        tempo_mode(s.clock); tempo_port(1, s.in_usb); tempo_port(2, s.in_trs);
        tempo_scene_flag(s.scn_tempo); tempo_out(!s.out_off); color_apply(s.palette); scn_fx_mode = set_fx_mode(s.scn_fx);
        if (s.bpm10 >= 400u && s.bpm10 <= 2400u) tempo_set10(s.bpm10);
        set_saved = s;                          /* (an old layout: saved again in the new one, set_poll) */
        scn_fx_boot = 1;                        /* (the boot's load: its effects too, nothing plays to keep) */
        if (!s.layout && s.scene)               /* (saved before the presets: F01 .. F10 then the user's) */
            s.scene = (uint8_t)(s.scene > SCN_NF_OLD ? s.scene - SCN_NF_OLD + SCN_NF : 1u);
        if (s.scene && s.scene <= SCN_N) (void)scn_load(s.scene - 1u);
        scn_fx_boot = 0;
        scn_undo_ok = 0;                        /* (nothing played before it) */
        set_saved.scene = s.scene;
    } else set_take(&set_saved);
}
static void scn_poll(void)
{
    scn_apply_poll();
    scn_gl_poll();
    scn_save_poll();
    scn_poll_cand();
    set_poll();
}
