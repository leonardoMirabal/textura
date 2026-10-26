/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura: physically modelled textures for the M-VAVE FM-1 (water, rain, fire, stones, ice, wind, insects, birds ...),
 * played as particle clouds from the keys. Built on Dinghy's platform (platform.c, from Frodhes' tree).
 *
 * This file is the app: the panel, MIDI, the screen. The sound is lanes.c over engines/ (the same engines the bench
 * and the audition player run, linked as their own objects). Below both is platform.c: on the FM-1 the hardware; with
 * DINGHY_HOST tests/host.c, a fake FM-1 that runs a script and writes the screen (PPM) and the sound (WAV).
 *
 * Stage (UI-PLAN piece 7): the ten landscapes on four lanes (scape.c), the keys in LANDSCAPE and PLAY mode with their
 * LEDs (notes.c), the params table with its CC and NRPN (params.c), the tempo clock (tempo.c), the LFOs and the mod
 * matrix (mod.c), the scenes and the settings (scenes.c), the screen in Frodhes' look (ui.c); MIDI notes on channel 1
 * play the selected lane, program changes pick a scene.
 *
 * Rules of the road (no FPU, no libgcc): integers only, no float, no 64-bit division. */
#include <stdint.h>
#define APP_NAME "TEXTURA"
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_9TX"                   /* the package identity: a placeholder until stage 8 decides it */
#endif
#define APP_OUT_WRITTEN 1                       /* app_audio writes every sample of out */
#ifdef DINGHY_HOST
static void kv_host_vkey(uint32_t k, uint32_t raw) { (void)k; (void)raw; }   /* (tests/host.c "vkey": no key velocity scan yet) */
#endif
#include "platform.c"
#define LN_MOD                                  /* lanes.c calls the modulation (mod.c) at each block and note */
static void mod_block(void);
static void mod_key_isr(uint32_t k, uint32_t vel);
static void mod_key_off_isr(uint32_t k);
static void mod_lane_reset_isr(uint32_t k);
#ifdef DINGHY_HOST
#define LN_COST_EST                             /* the governor's costs: the host's stand-in (lanes.c) */
#else
#define LN_CLOCK() fm1_ticks()                  /* the governor's costs: timed on the FM-1's 24 MHz clock */
#define LN_CLOCK_US(d) ((uint32_t)(d) / FM1_TICKS_PER_US)
#endif
#include "lanes.c"

#define TX_PALETTE "RED"                        /* the default colour */

/* a fresh seed for each engine started: never the same twice (on the FM-1 the free-running 24 MHz timer adds the
 * moment; on the host the run stays repeatable for the tests) */
static uint32_t tx_seed(void)
{
    static uint32_t s = 0x9E3779B9u;
#ifndef DINGHY_HOST
    s ^= fm1_ticks();
#endif
    s ^= fm1_ms;
    s = s * 1664525u + 1013904223u;
    return s ^ s >> 16;
}

/* ---- audio (the interrupt): the lanes, Q23 -> the platform's Q15, rounded and saturated */
static int32_t tx_L[TX_BLOCK], tx_R[TX_BLOCK];
static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i;
    if (n > TX_BLOCK) n = TX_BLOCK;
    ln_block(tx_L, tx_R, n);
    for (i = 0; i < n; i++) {
        int32_t l = (tx_L[i] + 128) >> 8, r = (tx_R[i] + 128) >> 8;
        out[2u * i] = l > 32767 ? 32767 : l < -32767 ? -32767 : l;
        out[2u * i + 1u] = r > 32767 ? 32767 : r < -32767 ? -32767 : r;
    }
}

static uint32_t tx_palette_index(const char *name)
{
    uint32_t i, j;
    for (i = 0; i < NPALETTES; i++) {
        for (j = 0; name[j] && PALETTES[i].name[j] == name[j]; j++) {}
        if (!name[j] && !PALETTES[i].name[j]) return i;
    }
    return 0;
}

#include "scape.c"
#include "notes.c"
#include "params.c"
#include "tempo.c"
#include "mod.c"
#include "scenes.c"
#include "ui.c"

/* ---- MIDI: notes on channel 1 play the selected lane, pitched; control changes reach the params table (a lane's on
 * channel 1 + lane, the effects on the mix on channel 5: params.c); the clock and the transport go to tempo.c */
static void tx_midi_poll(void)
{
    uint8_t st, d1, d2;
    int src;
    while ((src = midi_get(&st, &d1, &d2)) != 0) {
        uint32_t t = st & 0xF0u, ch = st & 0x0Fu;
        if (st >= 0xF8u) { (void)tempo_clock(st, (uint32_t)d1 | (uint32_t)d2 << 8, src); continue; }   /* (d1 d2: the
                                                 * arrival and the pulse count, as platform.c stamps them) */
        if (st == 0xF2u) { tempo_spp((uint32_t)(d1 & 127u) | (uint32_t)(d2 & 127u) << 7, src); continue; }
        if (t == 0xB0u) { if (!p_cc(ch, d1, d2)) mod_cc_in(d1, d2); continue; }   /* (not ours: the mod wheel, MOD CC) */
        if (t == 0xD0u) { mod_at(d1); continue; }
        if (t == 0xA0u) { mod_at(d2); continue; }
        if (t == 0xC0u && ch == 0u) { scn_pc(d1); continue; }   /* (program change: a scene, scenes.c) */
        if (ch != 0u) continue;
        if (t == 0x90u) ln_key(sc.sel, d1, d2 ? d2 : 0u, d2 != 0u);
        else if (t == 0x80u) ln_key(sc.sel, d1, 0, 0);
    }
}

static void app_main(void)
{
    palette_set(tx_palette_index(TX_PALETTE));
    ln_init();
    sc_init();
    p_init();
    mod_init();
    scn_init();
    tempo_sync_publish();
    ui_splash();
    for (;;) {
        uint32_t rel, b;
        platform_poll();
        b = fm1_input_edges(&rel);
        ui_input(b, rel);
        nt_poll();
        tx_midi_poll();
        sc_poll();
        p_poll();
        tempo_poll();
        mod_poll();
        scn_poll();
        ui_draw();
        nt_leds(ui_btn_leds());
    }
}

#ifdef DINGHY_WEB
EMSCRIPTEN_KEEPALIVE const char *web_page(void) { return G_TITLE[ui_g]; }   /* (web/app.js: the buttons' captions) */
EMSCRIPTEN_KEEPALIVE const char *web_scene(void)  /* the scene loaded last ("U05 WATER", "-"; tests/web_check.py) */
{
    static char s[20];
    if (scn_cur == 0xFF) { s[0] = '-'; s[1] = 0; } else scn_name_of(scn_cur, s);
    return s;
}
#endif

/* ---- the host's checks (tests/host.c: "expect state K V", "expect sound", "dump") */
#ifdef DINGHY_HOST
static int host_preset_check(char *msg, unsigned n)   /* 0 if what plays is the scene loaded last (its lanes, records) */
{
    static scn_t now;
    const scn_t *s = scn_ap;
    uint32_t k, i, j;
    if (scn_cur == 0xFF || !s || scn_busy()) { snprintf(msg, n, "%s", scn_busy() ? "a scene still loading" : "no scene loaded"); return scn_busy(); }
    scn_capture(&now);
    if (now.fam != s->fam) { snprintf(msg, n, "landscape %u, not %u", now.fam, s->fam); return 1; }
    for (k = 0; k < SC_N; k++)
        if (now.snd[k] != s->snd[k] || now.run[k] != s->run[k] || (s->run[k] && (now.cnt[k] != s->cnt[k] || now.step[k] != s->step[k])) ||
            (s->snd[k] >= 0 && (now.vol[k] != s->vol[k] || now.pan[k] != s->pan[k]))) {
            snprintf(msg, n, "lane %u: sound %d run %u count %u step %u vol %u pan %u, not %d %u %u %u %u %u", k + 1u, now.snd[k],
                     now.run[k], now.cnt[k], now.step[k], now.vol[k], now.pan[k], s->snd[k], s->run[k], s->cnt[k], s->step[k], s->vol[k], s->pan[k]);
            return 1;
        }
    for (i = 0; i < s->nrec; i++) {             /* every record holds (the cards: as set; the rest: a value not
                                                 * recorded is its default; the tempo: with SCENE TEMPO on) */
        int32_t v;
        if (P_SCOPE(s->r[i].id) == P_GLOBAL && P_IDX(s->r[i].id) == PG_FINE && !tm.scn_tempo) continue;
        for (j = 0; j < now.nrec && now.r[j].id != s->r[i].id; j++) {}
        v = j < now.nrec ? now.r[j].v : scn_def_of(s->r[i].id);
        if (v != s->r[i].v) { snprintf(msg, n, "id 0x%03X: %d, not %d", s->r[i].id, v, s->r[i].v); return 1; }
    }
    for (j = 0; j < now.nrec; j++) {            /* and nothing set that the scene has not (a card, a value off its
                                                 * default) */
        if (P_SCOPE(now.r[j].id) == P_GLOBAL && P_IDX(now.r[j].id) == PG_FINE) continue;
        for (i = 0; i < s->nrec && s->r[i].id != now.r[j].id; i++) {}
        if (i == s->nrec) { snprintf(msg, n, "0x%03X set (%d), not in the scene", now.r[j].id, now.r[j].v); return 1; }
    }
    snprintf(msg, n, "%u records", s->nrec);
    return 0;
}
static int host_sound_check(char *msg, unsigned n)   /* 0 if every lane runs the engine the landscape put there */
{
    uint32_t k;
    for (k = 0; k < SC_N; k++)
        if (ln.mix.e[k] != sc.eng[k]) { snprintf(msg, n, "lane %u runs %s, not %s", k + 1u, ln.mix.e[k] ? ln.mix.e[k]->name : "-", sc.eng[k] ? sc.eng[k]->name : "-"); return 1; }
    if (ln.dropped) { snprintf(msg, n, "%u events dropped", ln.dropped); return 1; }
    for (k = 0; k < SC_N; k++) {                /* the chain plays the effects' values the table holds */
        uint32_t i;
        for (i = 0; i < TX_DR_NP; i++)
            if (ln.fx.drive[k].p[i] != pv_lane[k][PL_DRIVE0 + i] && !mod_holds(PID(k, PL_DRIVE0 + i))) { snprintf(msg, n, "lane %u drive %u: %u, not %u", k + 1u, i, ln.fx.drive[k].p[i], pv_lane[k][PL_DRIVE0 + i]); return 1; }
        for (i = 0; i < TX_FL_NP; i++)
            if (ln.fx.filter[k].p[i] != pv_lane[k][PL_FILTER0 + i] && !mod_holds(PID(k, PL_FILTER0 + i))) { snprintf(msg, n, "lane %u filter %u: %u, not %u", k + 1u, i, ln.fx.filter[k].p[i], pv_lane[k][PL_FILTER0 + i]); return 1; }
    }
    for (k = 0; k < TX_GD_NP; k++)
        if (ln.fx.gd.p[k] != pv_glob[PG_GRAIN0 + k] && !mod_holds(PID(P_GLOBAL, PG_GRAIN0 + k))) { snprintf(msg, n, "grain %u: %u, not %u", k, ln.fx.gd.p[k], pv_glob[PG_GRAIN0 + k]); return 1; }
    if (ln.fx.gd.sync != tm.gsync_smp) { snprintf(msg, n, "grain sync %u, not %u", ln.fx.gd.sync, tm.gsync_smp); return 1; }
    for (k = 0; k < TX_RV_NP; k++)
        if (ln.fx.rev.p[k] != pv_glob[PG_REVERB0 + k] && !mod_holds(PID(P_GLOBAL, PG_REVERB0 + k))) { snprintf(msg, n, "reverb %u: %u, not %u", k, ln.fx.rev.p[k], pv_glob[PG_REVERB0 + k]); return 1; }
    return 0;
}
static int host_state(const char *key, char *v, unsigned n)
{
    unsigned k, i;
    char c;
    if (!strcmp(key, "app")) snprintf(v, n, "%s", APP_NAME);
    else if (!strcmp(key, "palette")) snprintf(v, n, "%s", PALETTES[tx_palette_index(TX_PALETTE)].c[4] == C_HI ? TX_PALETTE : "?");
    else if (!strcmp(key, "splash")) snprintf(v, n, "%u", ui_splash_on);
    else if (!strcmp(key, "page")) {            /* (as the title: ENV's followers are FOLLOW) */
        const char *g = ui_g == G_ENV && ui_env >= TX_ENV_N ? "FOLLOW" : G_TITLE[ui_g];
        if (ui_nsub(ui_g) > 1u) snprintf(v, n, "%s %u/%u", g, ui_sub + 1u, ui_nsub(ui_g)); else snprintf(v, n, "%s", g);
    }
    else if (!strcmp(key, "landscape")) snprintf(v, n, "%s", LS[sc.fam].name);
    else if (!strcmp(key, "sel")) snprintf(v, n, "%u", sc.sel + 1u);
    else if (!strcmp(key, "mode")) snprintf(v, n, "%s", nt.mode == NT_PLAY ? "PLAY" : "LANDSCAPE");
    else if (!strcmp(key, "oct")) snprintf(v, n, "%d", nt.oct);
    else if (!strcmp(key, "msg")) snprintf(v, n, "%s", ui_msg_t && fm1_ms - ui_msg_t < MSG_MS ? ui_msg : "");
    else if (!strcmp(key, "dropped")) snprintf(v, n, "%u", ln.dropped);
    else if (!strcmp(key, "govus")) snprintf(v, n, "%u", ln_gov_us());            /* the governor: the pool's cost */
    else if (!strcmp(key, "govpool")) snprintf(v, n, "%u", ln.gov.budget);
    else if (!strcmp(key, "govcut")) { for (i = 0, k = 0; k < SC_N; k++) i += ln_gov_cur(k) < ln_gov_user(k); snprintf(v, n, "%u", i); }   /* lanes cut */
    else if (sscanf(key, "gov%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", ln_gov_cur(k));   /* heroes now */
    else if (sscanf(key, "lane%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%s", ln_engine(k) ? ln_engine(k)->name : "-");
    else if (sscanf(key, "snd%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%s", sc_sound_name(k));
    else if (sscanf(key, "cnt%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", sc_cnt_of(k));
    else if (sscanf(key, "act%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", ui_lane_act(k));
    else if (sscanf(key, "run%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", sc.run[k]);
    else if (sscanf(key, "vol%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", sc_vol_of(k));
    else if (sscanf(key, "pan%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", sc.pan[k]);
    else if (sscanf(key, "gen%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", ln.gen_on[k]);
    else if (!strcmp(key, "bpm10")) snprintf(v, n, "%u", tempo_bpm10());          /* the tempo shown (tempo.c) */
    else if (!strcmp(key, "beatsmp")) snprintf(v, n, "%u", beat_smp);
    else if (!strcmp(key, "clklock")) snprintf(v, n, "%d", clk_follows());
    else if (!strcmp(key, "clksrc")) snprintf(v, n, "%u", clk_src);
    else if (!strcmp(key, "pubs")) snprintf(v, n, "%u", tempo_pubs);
    else if (!strcmp(key, "running")) snprintf(v, n, "%d", tm_running());
    else if (!strcmp(key, "songpos")) snprintf(v, n, "%u", clk_sp);
    else if (!strcmp(key, "clkout")) snprintf(v, n, "%u", clko_sent);           /* clock pulses sent (MASTER) */
    else if (sscanf(key, "env%u%c", &k, &c) == 1 && k < TX_ENV_N) snprintf(v, n, "%d", mod_ext[ME_ENV1 + k]);   /* ENV 1, 2 now */
    else if (sscanf(key, "fol%u%c", &k, &c) == 1 && k < TX_FOL_N) snprintf(v, n, "%d", mod_ext[ME_FOL1 + k]);   /* FOLLOW 1, 2 now */
    else if (!strcmp(key, "gsync")) snprintf(v, n, "%u", tm.gsync_smp);          /* the grain delay's echo, samples */
    else if (!strcmp(key, "scene")) { char t[20]; if (scn_cur == 0xFF) snprintf(v, n, "-"); else { scn_name_of(scn_cur, t); snprintf(v, n, "%s", t); } }
    else if (!strcmp(key, "loading")) snprintf(v, n, "%u", scn_busy());
    else if (!strcmp(key, "saving")) snprintf(v, n, "%u", scn_sv_on);
    else if (!strcmp(key, "savemode")) snprintf(v, n, "%u", scn_save_mode ? scn_save_slot + 1u : 0u);
    else if (sscanf(key, "used%u%c", &k, &c) == 1 && k >= 1u && k <= SCN_NU) snprintf(v, n, "%u", scn_meta[k - 1u].used);
    else if (!strcmp(key, "mpos")) snprintf(v, n, "%u", mpos >> 8);               /* the song position (1/96 quarter) */
    else if (!strcmp(key, "routes")) snprintf(v, n, "%u", mbuf[mcur].nr);
    else if (sscanf(key, "lfo%u%c", &k, &c) == 1 && k < TX_LFO_N) snprintf(v, n, "%d", (int)mod_lfo[k].out);
    else if (sscanf(key, "live%x%c", &k, &c) == 1) { int32_t x; snprintf(v, n, "%d", mod_live(k, &x) ? (int)x : -1); }
    else if (!strcmp(key, "gdsync")) snprintf(v, n, "%u", ln.fx.gd.sync);         /* as the chain has it */
    else if (sscanf(key, "lab%u%c", &k, &c) == 1 && k < 4u) snprintf(v, n, "%s", ui_row_id(k) != NOID ? p_lab(ui_row_id(k)) : "-");
    else if (sscanf(key, "card%u%c", &k, &c) == 1 && k < 4u) {   /* the value as the card writes it */
        char t[24];
        const char *u;
        if (ui_row_id(k) == NOID) { snprintf(v, n, "-"); return 0; }
        u = p_text(ui_row_id(k), t);
        snprintf(v, n, "%s%s%s", t, u ? " " : "", u ? u : "");
    } else if (sscanf(key, "p%x%c", &k, &c) == 1) snprintf(v, n, "%d", p_valid(k) ? (int)p_get(k) : -1);   /* "p120": id 0x120 */
    else if (sscanf(key, "host%u.%u%c", &k, &i, &c) == 2 && k < SC_N)   /* the hosted engine's parameter i: its override */
        snprintf(v, n, "%d", ln.mix.e[k] == sc.src ? (int)tx_source_host_value(ln.mix.st[k], i) : -2);
    else return 1;
    return 0;
}
#ifndef DINGHY_WEB
static void host_gov_pool(uint32_t us) { ln.gov.budget = us ? us : LN_GOV_POOL_US; }   /* "T govpool US" (0: the
                                                         * device's): a smaller pool, to watch the governor act */
#endif
static void host_dump(void)
{
    static const char *const K[] = {"app", "palette", "splash", "page", "landscape", "sel", "mode", "oct", "dropped",
                                    "bpm10", "beatsmp", "clklock", "clksrc", "pubs", "running", "songpos", "clkout", "gsync", "gdsync"};
    static const char *const L[] = {"lane", "snd", "cnt", "act", "run", "vol", "pan", "gen"};
    char v[48], key[12];
    uint32_t i, k;
    for (i = 0; i < sizeof K / sizeof K[0]; i++) if (!host_state(K[i], v, sizeof v)) printf("@ %u %s %s\n", fm1_ms, K[i], v);
    for (k = 0; k < SC_N; k++)
        for (i = 0; i < sizeof L / sizeof L[0]; i++) {
            snprintf(key, sizeof key, "%s%u", L[i], k);
            if (!host_state(key, v, sizeof v)) printf("@ %u %s %s\n", fm1_ms, key, v);
        }
}
static uint32_t crc32(const uint8_t *p, uint32_t n)   /* (tests/host.c's flash file; presets.c's own later) */
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) { c ^= *p++; for (k = 0; k < 8u; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}
static void eng_host_report(void) { printf("lanes  %u events dropped\n", ln.dropped); }
#endif
