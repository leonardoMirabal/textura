/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura: physically modelled textures for the M-VAVE FM-1 (water, rain, fire, stones, ice, wind, insects, birds ...),
 * played as particle clouds from the keys. Built on Dinghy's platform (platform.c, from Frodhes' tree).
 *
 * This file is the app: the panel, MIDI, the screen. The sound is lanes.c over engines/ (the same engines the bench
 * and the audition player run, linked as their own objects). Below both is platform.c: on the FM-1 the hardware; with
 * DINGHY_HOST tests/host.c, a fake FM-1 that runs a script and writes the screen (PPM) and the sound (WAV).
 *
 * Stage (UI-PLAN piece 1, the skeleton): the splash, the RED palette, one lane playing the SOURCE engine from the
 * keys (F3..G5 = MIDI 53..79) and from MIDI notes on channel 1. The pages, the landscapes, the tempo and the modulation
 * come in the next pieces.
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

#define TX_KEY_VEL 100u                         /* the on-board keys' velocity (a setting later) */
#define TX_PALETTE "RED"                        /* the default colour */
#define TX_SPLASH_MS 1600u

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

/* ---- screen */
static uint32_t tx_palette_index(const char *name)
{
    uint32_t i, j;
    for (i = 0; i < NPALETTES; i++) {
        for (j = 0; name[j] && PALETTES[i].name[j] == name[j]; j++) {}
        if (!name[j] && !PALETTES[i].name[j]) return i;
    }
    return 0;
}
static uint32_t tx_splash_t;
static uint8_t tx_splash_on;
static void tx_splash(void)
{
    uint32_t i, r = 0x2545F491u;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    cv_begin(240, 64, C_BLACK);                 /* the mark: particles scattered over a line, denser to the middle */
    for (i = 0; i < 48u; i++) {
        int32_t x, y, s;
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        x = 120 + (int32_t)((r & 127u) + (r >> 7 & 127u)) - 127;
        y = 6 + (int32_t)(r >> 14 & 31u) - (int32_t)((x > 120 ? x - 120 : 120 - x) >> 3);
        s = 2 + (int32_t)(r >> 20 & 1u);
        if (y < 2) y = 2;
        cv_rect(x, y, s, s, i % 7u == 3u ? C_HI : i & 1u ? C_GRAY : C_DIM);
    }
    cv_rect(40, 44, 160, 2, C_LINE);
    cv_blit(0, 46);
    draw_text_box(0, 118, 240, &FONT_L, "TEXTURA", C_HI, 1);
    draw_text_box(0, 158, 240, &FONT_S, "TEXTURE SYNTHESIZER", C_GRAY, 1);
    draw_text_box(0, 214, 240, &FONT_S, "VERSION " APP_VER, C_DIM, 1);
    tx_splash_t = fm1_ms;
    tx_splash_on = 1;
}
static void tx_screen_main(void)                /* (piece 1: what plays; the pages come next) */
{
    const tx_engine_t *e = ln_engine(0);
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 8, 240, &FONT_S, "TEXTURA", C_GRAY, 1);
    cv_begin(240, 2, C_LINE); cv_blit(0, 30);
    draw_text_box(0, 100, 240, &FONT_L, e ? e->title : "-", C_HI, 1);
    draw_text_box(0, 150, 240, &FONT_S, "LANE 1", C_AMB, 1);
}

/* ---- the sound's start: lane 1 = the SOURCE at its defaults */
static const tx_engine_t *tx_eng_source;
static void tx_sound_init(void)
{
    ln_init();
    tx_eng_source = tx_engine_find("source");
    while (tx_eng_source && ln_select(0, tx_eng_source, tx_seed())) {}
}

/* ---- keys and MIDI */
static uint32_t tx_keys_down;                   /* the note keys as last seen (bit k = key k, F3 + k) */
static void tx_keys_poll(void)
{
    uint32_t now = fm1_in.notes & ((1u << TX_NKEYS) - 1u), ch = now ^ tx_keys_down, k;
    for (k = 0; ch; k++, ch >>= 1)
        if (ch & 1u) ln_key(0, TX_KEY_F3 + k, TX_KEY_VEL, now >> k & 1u);
    tx_keys_down = now;
}
static void tx_midi_poll(void)
{
    uint8_t st, d1, d2;
    while (midi_get(&st, &d1, &d2)) {
        uint32_t t = st & 0xF0u;
        if ((st & 0x0Fu) != 0u) continue;       /* channel 1 (the lanes' channels come with the params table) */
        if (t == 0x90u) ln_key(0, d1, d2 ? d2 : 0u, d2 != 0u);
        else if (t == 0x80u) ln_key(0, d1, 0, 0);
    }
}

static void app_main(void)
{
    palette_set(tx_palette_index(TX_PALETTE));
    tx_sound_init();
    tx_splash();
    for (;;) {
        platform_poll();
        tx_keys_poll();
        tx_midi_poll();
        if (tx_splash_on && fm1_ms - tx_splash_t >= TX_SPLASH_MS) {
            tx_splash_on = 0;
            tx_screen_main();
        }
    }
}

/* ---- the host's checks (tests/host.c: "expect state K V", "dump") */
#ifdef DINGHY_HOST
static int host_preset_check(char *msg, unsigned n) { (void)msg; (void)n; return 0; }   /* (no presets yet) */
static int host_sound_check(char *msg, unsigned n)   /* 0 if lane 1 runs the SOURCE and no event was lost */
{
    if (ln_engine(0) != tx_eng_source || !tx_eng_source) { snprintf(msg, n, "lane 1 is not the source"); return 1; }
    if (ln.dropped) { snprintf(msg, n, "%u events dropped", ln.dropped); return 1; }
    return 0;
}
static int host_state(const char *key, char *v, unsigned n)
{
    if (!strcmp(key, "app")) snprintf(v, n, "%s", APP_NAME);
    else if (!strcmp(key, "palette")) snprintf(v, n, "%s", PALETTES[tx_palette_index(TX_PALETTE)].c[4] == C_HI ? TX_PALETTE : "?");
    else if (!strcmp(key, "splash")) snprintf(v, n, "%u", tx_splash_on);
    else if (!strcmp(key, "lane0")) snprintf(v, n, "%s", ln_engine(0) ? ln_engine(0)->name : "-");
    else if (!strcmp(key, "dropped")) snprintf(v, n, "%u", ln.dropped);
    else return 1;
    return 0;
}
static void host_dump(void)
{
    static const char *const K[] = {"app", "palette", "splash", "lane0", "dropped"};
    char v[40];
    uint32_t i;
    for (i = 0; i < sizeof K / sizeof K[0]; i++) if (!host_state(K[i], v, sizeof v)) printf("@ %u %s %s\n", fm1_ms, K[i], v);
}
static uint32_t crc32(const uint8_t *p, uint32_t n)   /* (tests/host.c's flash file; presets.c's own later) */
{
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) { c ^= *p++; for (k = 0; k < 8u; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}
static void eng_host_report(void) { printf("lanes  %u events dropped\n", ln.dropped); }
#endif
