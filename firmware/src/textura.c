/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura: physically modelled textures for the M-VAVE FM-1 (water, rain, fire, stones, ice, wind, insects, birds ...),
 * played as particle clouds from the keys. Built on Dinghy's platform (platform.c, from Frodhes' tree).
 *
 * This file is the app: the panel, MIDI, the screen. The sound is lanes.c over engines/ (the same engines the bench
 * and the audition player run, linked as their own objects). Below both is platform.c: on the FM-1 the hardware; with
 * DINGHY_HOST tests/host.c, a fake FM-1 that runs a script and writes the screen (PPM) and the sound (WAV).
 *
 * Stage (UI-PLAN piece 4): the ten landscapes on four lanes (scape.c), the keys in LANDSCAPE and PLAY mode with their
 * LEDs (notes.c), the params table with its CC and NRPN (params.c), the screen in Frodhes' look with the LANDSCAPE,
 * MIXER, SOUND, DRIVE, FILTER, GRAIN and REVERB pages (ui.c); MIDI notes on channel 1 play the selected lane. The
 * tempo, the modulation and the scenes come next.
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
#include "ui.c"

/* ---- MIDI: notes on channel 1 play the selected lane, pitched; control changes reach the params table (a lane's on
 * channel 1 + lane, the effects on the mix on channel 5: params.c) */
static void tx_midi_poll(void)
{
    uint8_t st, d1, d2;
    while (midi_get(&st, &d1, &d2)) {
        uint32_t t = st & 0xF0u, ch = st & 0x0Fu;
        if (t == 0xB0u) { (void)p_cc(ch, d1, d2); continue; }
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
        ui_draw();
        nt_leds(ui_btn_leds());
    }
}

#ifdef DINGHY_WEB
EMSCRIPTEN_KEEPALIVE const char *web_page(void) { return G_TITLE[ui_g]; }   /* (web/app.js: the buttons' captions) */
#endif

/* ---- the host's checks (tests/host.c: "expect state K V", "expect sound", "dump") */
#ifdef DINGHY_HOST
static int host_preset_check(char *msg, unsigned n) { (void)msg; (void)n; return 0; }   /* (no scenes yet) */
static int host_sound_check(char *msg, unsigned n)   /* 0 if every lane runs the engine the landscape put there */
{
    uint32_t k;
    for (k = 0; k < SC_N; k++)
        if (ln.mix.e[k] != sc.eng[k]) { snprintf(msg, n, "lane %u runs %s, not %s", k + 1u, ln.mix.e[k] ? ln.mix.e[k]->name : "-", sc.eng[k] ? sc.eng[k]->name : "-"); return 1; }
    if (ln.dropped) { snprintf(msg, n, "%u events dropped", ln.dropped); return 1; }
    for (k = 0; k < SC_N; k++) {                /* the chain plays the effects' values the table holds */
        uint32_t i;
        for (i = 0; i < TX_DR_NP; i++)
            if (ln.fx.drive[k].p[i] != pv_lane[k][PL_DRIVE0 + i]) { snprintf(msg, n, "lane %u drive %u: %u, not %u", k + 1u, i, ln.fx.drive[k].p[i], pv_lane[k][PL_DRIVE0 + i]); return 1; }
        for (i = 0; i < TX_FL_NP; i++)
            if (ln.fx.filter[k].p[i] != pv_lane[k][PL_FILTER0 + i]) { snprintf(msg, n, "lane %u filter %u: %u, not %u", k + 1u, i, ln.fx.filter[k].p[i], pv_lane[k][PL_FILTER0 + i]); return 1; }
    }
    for (k = 0; k < TX_GD_NP; k++)
        if (ln.fx.gd.p[k] != pv_glob[PG_GRAIN0 + k]) { snprintf(msg, n, "grain %u: %u, not %u", k, ln.fx.gd.p[k], pv_glob[PG_GRAIN0 + k]); return 1; }
    for (k = 0; k < TX_RV_NP; k++)
        if (ln.fx.rev.p[k] != pv_glob[PG_REVERB0 + k]) { snprintf(msg, n, "reverb %u: %u, not %u", k, ln.fx.rev.p[k], pv_glob[PG_REVERB0 + k]); return 1; }
    return 0;
}
static int host_state(const char *key, char *v, unsigned n)
{
    unsigned k, i;
    char c;
    if (!strcmp(key, "app")) snprintf(v, n, "%s", APP_NAME);
    else if (!strcmp(key, "palette")) snprintf(v, n, "%s", PALETTES[tx_palette_index(TX_PALETTE)].c[4] == C_HI ? TX_PALETTE : "?");
    else if (!strcmp(key, "splash")) snprintf(v, n, "%u", ui_splash_on);
    else if (!strcmp(key, "page")) { if (ui_nsub(ui_g) > 1u) snprintf(v, n, "%s %u/%u", G_TITLE[ui_g], ui_sub + 1u, ui_nsub(ui_g)); else snprintf(v, n, "%s", G_TITLE[ui_g]); }
    else if (!strcmp(key, "landscape")) snprintf(v, n, "%s", LS[sc.fam].name);
    else if (!strcmp(key, "sel")) snprintf(v, n, "%u", sc.sel + 1u);
    else if (!strcmp(key, "mode")) snprintf(v, n, "%s", nt.mode == NT_PLAY ? "PLAY" : "LANDSCAPE");
    else if (!strcmp(key, "oct")) snprintf(v, n, "%d", nt.oct);
    else if (!strcmp(key, "msg")) snprintf(v, n, "%s", ui_msg_t && fm1_ms - ui_msg_t < MSG_MS ? ui_msg : "");
    else if (!strcmp(key, "dropped")) snprintf(v, n, "%u", ln.dropped);
    else if (sscanf(key, "lane%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%s", ln_engine(k) ? ln_engine(k)->name : "-");
    else if (sscanf(key, "snd%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%s", sc_sound_name(k));
    else if (sscanf(key, "cnt%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", sc_cnt_of(k));
    else if (sscanf(key, "act%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", ui_lane_act(k));
    else if (sscanf(key, "run%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", sc.run[k]);
    else if (sscanf(key, "vol%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", sc_vol_of(k));
    else if (sscanf(key, "pan%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", sc.pan[k]);
    else if (sscanf(key, "gen%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", ln.gen_on[k]);
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
static void host_dump(void)
{
    static const char *const K[] = {"app", "palette", "splash", "page", "landscape", "sel", "mode", "oct", "dropped"};
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
