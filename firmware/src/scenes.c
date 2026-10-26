/* SPDX-License-Identifier: GPL-3.0-only */
/* Scenes and the settings.
 *
 * A scene is everything a sound place is: the landscape, each lane's sound, HOW MANY, HOW ACTIVE, playing or not, its
 * level and pan, then a list of (parameter id, value) records: the lanes' cards (the ones set), drive and filter, the
 * grain delay and its SYNC, the reverb, the tempo, the eight LFOs and the matrix. By id, so a record of an older
 * firmware loads into a newer one (an id it lacks keeps its value; ids never change, params.c).
 *
 *   factory   F01 .. F10: each landscape at its start, the effects and the modulation at their defaults
 *   user      U01 .. U16, in a bank of two copies written alternately (Frodhes' presets.c scheme): the older copy
 *             erased, the records written, the header last; the valid copy with the higher sequence loads, so a save
 *             cut short never loses the bank. Only the names stay in RAM; a record is read when it loads.
 *   PRESETS   browse F01 .. U16 (the empty user scenes skipped); the scene loads 400 ms after the last detent
 *   SAVE      the save mode: PRESETS / K1 pick the user scene, SAVE again saves, HOME (or OCT-) cancels
 *   SAVE held 700 ms: undo the last load (again: redo)
 *   MIDI PC   on channel 1: 0 .. 9 F01 .. F10, 16 .. 31 U01 .. U16
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
#define SCN_NF LS_N                             /* factory scenes: one a landscape */
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
#define SCN_UNDO_MS 700u
#define SET_SAVE_MS 2000u
#define SET_VER 1u

typedef struct { uint16_t id; int16_t v; } scn_rec_t;
typedef struct {
    uint8_t ver, used, fam, rsv;
    char name[12];
    int8_t snd[SC_N];
    uint8_t cnt[SC_N], step[SC_N], run[SC_N], vol[SC_N], pan[SC_N];
    uint16_t nrec, rsv2;
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
static scn_t scn_b[2];                          /* a load's scene and the undo's (their roles swap: scn_u) */
static scn_t scn_sv;                            /* the scene being saved */
static uint8_t scn_u, scn_undo_ok, scn_bidx[2];   /* (scn_bidx: each one's scene number, 0xFF: none) */
static uint8_t scn_cur = 0xFF;                  /* the scene loaded last (0xFF: none) */
/* a load in progress */
static const scn_t *scn_ap;
static uint8_t scn_ap_stage;                    /* 0 idle, 1 the lanes set (waiting), 2 the records */
static uint16_t scn_ap_i;
static uint32_t scn_ap_t;
static uint8_t scn_ap_idx = 0xFF;
/* a save in progress */
static uint8_t scn_sv_on, scn_sv_slot;
static uint16_t scn_sv_step;                    /* sectors erased, then chunks written */
static uint32_t scn_sv_at, scn_sv_crc;
/* browsing and the save mode */
static uint8_t scn_cand = 0xFF, scn_save_mode, scn_save_slot;
static uint32_t scn_cand_t, scn_save_t;

static void ui_say(const char *s);              /* ui.c */
static void ui_all(void);

static void scn_name_of(uint32_t idx, char *s)  /* "F03 FIRE", "U05 WATER", "U06 ----" */
{
    uint32_t n = idx < SCN_NF ? idx + 1u : idx - SCN_NF + 1u;
    s[0] = idx < SCN_NF ? 'F' : 'U'; s[1] = (char)('0' + n / 10u); s[2] = (char)('0' + n % 10u); s[3] = ' '; s[4] = 0;
    if (idx < SCN_NF) p_upper(s + 4, LS[idx].name, 12);
    else if (scn_meta[idx - SCN_NF].used) { char t[13]; p_strcpy(t, scn_meta[idx - SCN_NF].name, 12); p_cat(s, t, 16); }
    else p_cat(s, "----", 16);
}

/* ---- capture: the scene that plays now */
static void scn_add(scn_t *s, uint32_t id, int32_t v)
{
    if (s->nrec < SCN_REC_MAX) { s->r[s->nrec].id = (uint16_t)id; s->r[s->nrec].v = (int16_t)v; s->nrec++; }
}
static void scn_capture(scn_t *s)
{
    uint32_t k, i;
    memset(s, 0, sizeof *s);
    s->ver = SCN_VER; s->used = 1; s->fam = sc.fam;
    p_upper(s->name, LS[sc.fam].name, 11);
    for (k = 0; k < SC_N; k++) {
        p_cards_t *c = p_cards(k);
        s->snd[k] = sc.snd[k];
        s->cnt[k] = m_cb_ok[k] && sc.run[k] && m_cbase[k] ? m_cbase[k] : sc.cnt[k];   /* (HOW MANY modulated: its base) */
        s->step[k] = sc.step[k]; s->run[k] = sc.run[k];
        s->vol[k] = (uint8_t)sc_vol_of(k); s->pan[k] = sc.pan[k];
        for (i = 0; i < c->n; i++) if (c->cv[i] != 0xFF) scn_add(s, PID(k, PL_CARD0 + i), c->cv[i]);   /* (the ones set) */
        for (i = 0; i < TX_DR_NP; i++) scn_add(s, PID(k, PL_DRIVE0 + i), pv_lane[k][PL_DRIVE0 + i]);
        for (i = 0; i < TX_FL_NP; i++) scn_add(s, PID(k, PL_FILTER0 + i), pv_lane[k][PL_FILTER0 + i]);
    }
    for (i = 0; i < TX_GD_NP; i++) scn_add(s, PID(P_GLOBAL, PG_GRAIN0 + i), pv_glob[PG_GRAIN0 + i]);
    scn_add(s, PID(P_GLOBAL, PG_GSYNC), (int32_t)tm.gsync);
    for (i = 0; i < TX_RV_NP; i++) scn_add(s, PID(P_GLOBAL, PG_REVERB0 + i), pv_glob[PG_REVERB0 + i]);
    scn_add(s, PID(P_GLOBAL, PG_FINE), (int32_t)tempo_bpm10() - 400);
    for (k = 0; k < TX_LFO_N; k++) for (i = 0; i < TX_LP_N; i++) scn_add(s, PID(P_LFO, k * 16u + i), mod_lfo[k].p[i]);
    for (k = 0; k < MOD_NS; k++) {
        scn_add(s, PID(P_MAT, k * MQ_N + MQ_SRC), mslot[k].src); scn_add(s, PID(P_MAT, k * MQ_N + MQ_TGT), mslot[k].tgt);
        scn_add(s, PID(P_MAT, k * MQ_N + MQ_DEPTH), mslot[k].depth); scn_add(s, PID(P_MAT, k * MQ_N + MQ_SPREAD), mslot[k].spread);
    }
    scn_add(s, PID(P_MAT, MQ_CC), mod_cc);
}
/* a factory scene: landscape f at its start (sc_start_landscape), the effects, the LFOs and the matrix at their
 * defaults; the tempo stays */
static void scn_factory(scn_t *s, uint32_t f)
{
    static const uint8_t C[3] = {2, 1, 1};
    uint32_t k, i;
    tx_lfo_t l;
    memset(s, 0, sizeof *s);
    s->ver = SCN_VER; s->used = 1; s->fam = (uint8_t)f;
    p_upper(s->name, LS[f].name, 11);
    for (k = 0; k < SC_N; k++) {
        s->snd[k] = (int8_t)(k < LS[f].n ? (int32_t)k : -1);
        s->vol[k] = 100; s->pan[k] = 64;
        if (k < 3u && s->snd[k] >= 0) { s->cnt[k] = C[k]; s->step[k] = (uint8_t)sc_act_step(0); s->run[k] = 1; }
        for (i = 0; i < TX_DR_NP; i++) scn_add(s, PID(k, PL_DRIVE0 + i), tx_fx_drive_params[i].def);
        for (i = 0; i < TX_FL_NP; i++) scn_add(s, PID(k, PL_FILTER0 + i), tx_fx_filter_params[i].def);
    }
    for (i = 0; i < TX_GD_NP; i++) scn_add(s, PID(P_GLOBAL, PG_GRAIN0 + i), tx_fx_grain_params[i].def);
    scn_add(s, PID(P_GLOBAL, PG_GSYNC), 0);
    for (i = 0; i < TX_RV_NP; i++) scn_add(s, PID(P_GLOBAL, PG_REVERB0 + i), tx_fx_reverb_params[i].def);
    for (k = 0; k < TX_LFO_N; k++) {
        tx_lfo_init(&l, k, 1u);
        for (i = 0; i < TX_LP_N; i++) scn_add(s, PID(P_LFO, k * 16u + i), l.p[i]);
    }
    for (k = 0; k < MOD_NS; k++) {
        scn_add(s, PID(P_MAT, k * MQ_N + MQ_SRC), 0); scn_add(s, PID(P_MAT, k * MQ_N + MQ_TGT), 0);
        scn_add(s, PID(P_MAT, k * MQ_N + MQ_DEPTH), 100); scn_add(s, PID(P_MAT, k * MQ_N + MQ_SPREAD), 32);
    }
    scn_add(s, PID(P_MAT, MQ_CC), 2);
}

/* ---- load: the lanes, then (their engines in) the records, a slice a pass */
static void scn_cards_reset(uint32_t k)         /* the lane's cards back to the sound's own (none set) */
{
    p_cards_t *c = p_cards(k);
    uint32_t i;
    for (i = 0; i < c->n; i++) {
        if (c->cv[i] == 0xFF) continue;
        c->cv[i] = 0xFF;
        if (c->kind[i] & CK_HOST) ln_host(k, c->id[i], 0, 1);   /* (released: the source's curves again) */
        else ln_param(k, c->id[i], c->kind[i] & CK_INV ? c->hi[i] - c->def[i] : c->def[i]);
    }
}
static void scn_lanes(const scn_t *s)
{
    uint32_t k, f = s->fam % LS_N;
    for (k = 0; k < SC_N; k++) scn_cards_reset(k);
    if (f != sc.fam) sc_family(f);
    sc.fam_pend = 0;
    for (k = 0; k < SC_N; k++) { if (sc.run[k]) sc_start(k, 0); sc.snd[k] = -1; sc.mem_cnt[k] = 0; m_cb_ok[k] = 0; }
    for (k = 0; k < SC_N; k++) if (s->snd[k] >= 0 && (uint32_t)s->snd[k] < LS[f].n) sc_load(k, (uint32_t)s->snd[k]);
    for (k = 0; k < SC_N; k++) {
        if (sc.snd[k] < 0) continue;
        sc.vol[f][sc.snd[k]] = s->vol[k] > 127u ? 127u : s->vol[k];
        sc.pan[k] = s->pan[k] > 127u ? 127u : s->pan[k];
        if (s->run[k] && s->cnt[k]) {
            sc.cnt[k] = (uint8_t)(s->cnt[k] < LS_NCNT ? s->cnt[k] : LS_NCNT - 1u);
            sc.step[k] = (uint8_t)(s->step[k] < 1u ? 1u : s->step[k] > SC_STEPS ? SC_STEPS : s->step[k]);
            sc_start(k, 1);
        } else sc_sync(k);
    }
    if (sc.sel >= SC_N || sc.snd[sc.sel] < 0) sc.sel = 0;
}
static void scn_apply(const scn_t *s, uint32_t idx)
{
    scn_ap = s; scn_ap_idx = (uint8_t)idx;
    scn_lanes(s);
    scn_ap_stage = 1; scn_ap_t = fm1_ms; scn_ap_i = 0;
}
static int scn_busy(void) { return scn_ap_stage != 0; }
static void scn_apply_poll(void)
{
    uint32_t k, n = 0;
    if (scn_ap_stage == 1) {                    /* the lanes' engines switched (and their hosted ones in): the cards */
        if (fm1_ms - scn_ap_t < SCN_SETTLE_MS) return;
        for (k = 0; k < SC_N; k++) if (sc.a_eng[k] != sc.eng[k]) return;
        for (k = 0; k < SC_N; k++) (void)p_cards(k);
        scn_ap_stage = 2;
    }
    if (scn_ap_stage != 2) return;
    while (scn_ap_i < scn_ap->nrec && n < SCN_SLICE && ln.qw - ln.qr < LN_QN / 2u) {
        const scn_rec_t *r = &scn_ap->r[scn_ap_i++];
        if (P_SCOPE(r->id) == P_GLOBAL && P_IDX(r->id) == PG_FINE) tempo_set10((uint32_t)(r->v + 400));
        else if (p_valid(r->id)) p_set(r->id, r->v);
        n++;
    }
    if (scn_ap_i >= scn_ap->nrec) {
        scn_ap_stage = 0;
        scn_cur = scn_ap_idx;
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
    char m[20];
    if (idx >= SCN_N || scn_busy()) return -1;
    if (idx < SCN_NF) scn_factory(t, idx);
    else if (!scn_meta[idx - SCN_NF].used || scn_read(idx - SCN_NF, t)) return -1;   /* (t: no load runs from it now) */
    scn_capture(&scn_b[scn_u]);                 /* (what played: the undo) */
    scn_bidx[scn_u] = scn_cur;
    scn_undo_ok = 1;
    scn_apply(t, idx);
    scn_name_of(idx, m);
    ui_say(m);
    return 0;
}
static void scn_undo(void)                      /* the last load undone (again: redone) */
{
    if (!scn_undo_ok || scn_busy()) return;
    scn_capture(&scn_b[scn_u ^ 1u]);
    scn_bidx[scn_u ^ 1u] = scn_cur;
    scn_apply(&scn_b[scn_u], scn_bidx[scn_u]);
    scn_u ^= 1u;
    ui_say("UNDO");
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
static void scn_save_start(uint32_t slot)       /* the scene that plays now into user scene slot */
{
    scn_capture(&scn_sv);
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
        p_strcpy(m, "SAVED ", 19);
        { char n[20]; scn_name_of(scn_cur, n); p_cat(m, n, 19); }
        ui_say(m);
        ui_all();
    }
}

/* ---- the panel: PRESETS, SAVE (tap: the save mode, held: undo), K1 and HOME in the save mode. 1: the input was
 * the save mode's (K1 and HOME taken) */
static int scn_input(uint32_t b, uint32_t rel, int32_t d_presets, int32_t d_k1)
{
    char s[20];
    if (b >> BTN_SAVE & 1u) scn_save_t = fm1_ms ? fm1_ms : 1u;
    if (scn_save_t && fm1_in.buttons >> BTN_SAVE & 1u && fm1_ms - scn_save_t >= SCN_UNDO_MS) {
        scn_save_t = 0; scn_save_mode = 0; scn_undo(); return 1;
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
            ui_all();
        }
        if ((b >> BTN_HOME | b >> BTN_OCT_DOWN) & 1u) { scn_save_mode = 0; ui_say("NOT SAVED"); ui_all(); }
        return (d_k1 || (b >> BTN_HOME | b >> BTN_OCT_DOWN) & 1u) ? 1 : 0;
    }
    if (d_presets) {                            /* browse: the next scene (empty user ones skipped) */
        int32_t v0 = scn_cand != 0xFF ? scn_cand : scn_cur != 0xFF ? scn_cur : (int32_t)sc.fam, v = v0, step = d_presets > 0 ? 1 : -1;
        do v += step; while (v >= (int32_t)SCN_NF && v < (int32_t)SCN_N && !scn_meta[v - SCN_NF].used);
        if (v < 0) v = 0;
        if (v >= (int32_t)SCN_N) v = v0;
        scn_cand = (uint8_t)v; scn_cand_t = fm1_ms;
        scn_name_of((uint32_t)v, s);
        ui_say(s);
    }
    return 0;
}
static void scn_pc(uint32_t p)                  /* MIDI program change (channel 1) */
{
    if (p < SCN_NF) (void)scn_load(p);
    else if (p >= 16u && p < 16u + SCN_NU) { if (scn_load(SCN_NF + p - 16u)) ui_say("NO SCENE THERE"); }
}

/* ---- the settings: one small record, saved SET_SAVE_MS after its last change (the platform's split save) */
typedef struct { uint8_t ver, scene, clock, in_usb, in_trs, palette, rsv[2]; uint16_t bpm10, rsv2; } tx_set_t;
static tx_set_t set_saved, set_now;
static uint8_t set_dirty, set_saving;
static uint32_t set_t;
static void set_take(tx_set_t *s)
{
    memset(s, 0, sizeof *s);
    s->ver = SET_VER; s->scene = (uint8_t)(scn_cur == 0xFF ? 0u : scn_cur + 1u);
    s->clock = (uint8_t)tm.clock; s->in_usb = tm.in_usb; s->in_trs = tm.in_trs; s->bpm10 = (uint16_t)tempo_bpm10();
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
    scn_bank_load();
    if (!settings_load(&s, sizeof s) && s.ver == SET_VER) {
        tempo_mode(s.clock); tempo_port(1, s.in_usb); tempo_port(2, s.in_trs);
        if (s.bpm10 >= 400u && s.bpm10 <= 2400u) tempo_set10(s.bpm10);
        set_saved = s;
        if (s.scene && s.scene <= SCN_N) (void)scn_load(s.scene - 1u);
        scn_undo_ok = 0;                        /* (nothing played before it) */
        set_saved.scene = s.scene;
    } else set_take(&set_saved);
}
static void scn_poll(void)
{
    scn_apply_poll();
    scn_save_poll();
    if (scn_cand != 0xFF && fm1_ms - scn_cand_t >= SCN_BROWSE_MS && !scn_busy()) {
        uint8_t c = scn_cand;
        scn_cand = 0xFF;
        if (scn_load(c)) ui_say("NO SCENE THERE");
    }
    set_poll();
}
