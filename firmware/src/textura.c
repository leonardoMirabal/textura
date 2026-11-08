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
#define APP_OUT_SHIFT 8u                        /* the codec at full scale (as Frodhes): +6 dB on the jack over
                                                 * Dinghy's -6 dBFS ceiling; the output limiter holds -1 dBFS */
#define APP_OUT_WRITTEN 1                       /* app_audio writes every sample of out */
#ifdef DINGHY_HOST
static void kv_host_vkey(uint32_t k, uint32_t raw) { (void)k; (void)raw; }   /* (tests/host.c "vkey": no key velocity scan yet) */
#else
/* the second core (core1.h): at boot, held, the supply raised to M-VAVE's own levels (C1_RAILS), then started; held
 * again before every reset; a fault of its own stops it (no reset) and the lanes go on with one core. C1_HSB2 (a device
 * A/B only, never shipped: core1.h) slows the bus first */
static int c1_start(void);
static void c1_hold(void);
static void c1_fault(const void *c);
static void c1_rails(void);
static void c1_hsb2(uint32_t power_on);
static void tx_stk_paint(void);
#define C1_RAILS
#define APP_EARLY() (tx_stk_paint(), c1_hold(), c1_rails(), \
                     c1_hsb2((fm1_boot.p3_rst & 1u) && !(fm1_boot.p3_rst & 4u)), (void)c1_start())
#define APP_RESET_PREP() c1_hold()
#define APP_FAULT(c) c1_fault(c)
/* a fault of core 0 or a freeze: back playing by itself, soon. The crash screen shows half a second (the record outlives
 * the reset: tx_dbg items 60..), the watchdog resets a main loop stopped for 1 s; the boot loads the last scene */
#define APP_CRASH_MS 500u
#define APP_WDT_RUN 0x0Au
#endif
#include "platform.c"
#ifndef DINGHY_HOST
#include "core1.h"
/* the two stacks' deepest use (the debug reply, items 43 / 44: 4-byte words): painted at boot, interrupts still off,
 * each below where it is now (or whole but its top 64 bytes, if this code runs on the other) */
#define TX_STK_MARK 0x53544B4Du
static void tx_stk_paint1(uint32_t *lo, uint32_t *top, uint32_t here)
{
    uint32_t *p, end = here >= (uint32_t)lo && here < (uint32_t)top ? here - 256u : (uint32_t)top - 64u;
    for (p = lo; (uint32_t)p < end; p++) *p = TX_STK_MARK;
}
static void tx_stk_paint(void)
{
    volatile uint32_t here = 0;
    tx_stk_paint1((uint32_t *)_ustack_lo, (uint32_t *)_guard1, (uint32_t)&here);   /* (_guard1: its top) */
    tx_stk_paint1((uint32_t *)_sstack_lo, (uint32_t *)_sstack_top, (uint32_t)&here);
}
static uint32_t tx_stk_used(const uint32_t *lo, const uint32_t *top)
{
    const uint32_t *p = lo;
    while (p < top && *p == TX_STK_MARK) p++;
    return (uint32_t)(top - p);
}
/* lanes.c LN_PAR: some lanes' engines on core 1. Its wait: past 3 ms (a job it lost, a fault) it is held for good */
#define LN_PAR
#define LN_PAR_ON() (c1_state == C1_ON)
#define LN_PAR_RUN(fn, a) c1_run(fn, a)
#define LN_PAR_WAIT() c1_wait_us(3000u)
#define LN_PIPE                                 /* the bus one block behind, on core 1 between the interrupts (lanes.c) */
#endif
#define LN_MOD                                  /* lanes.c calls the modulation (mod.c) at each block and note */
static void mod_block(void);
static void mod_key_isr(uint32_t k, uint32_t vel);
static void mod_key_off_isr(uint32_t k);
static void mod_lane_reset_isr(uint32_t k);
#ifdef DINGHY_HOST
#define LN_COST_EST                             /* the governor's costs: the host's stand-in (lanes.c) */
#else
#define LN_CLOCK() fm1_ticks()                  /* the governor's costs: timed on the FM-1's 24 MHz clock */
#define LN_POOL __attribute__((section(".pool")))   /* the lanes' state in the platform's pool (lanes.c) */
#define LN_CLOCK_US(d) ((uint32_t)(d) / FM1_TICKS_PER_US)
#define LN_ISR_US() isr_st.last_us              /* the last block's whole interrupt, us (platform.c) */
#endif
#include "lanes.c"

#define TX_PALETTE "RED"                        /* the colour before the settings load (COLOR's default: RED) */

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
#ifdef LN_BUS_TIMED
/* the grain delay's measured room (lanes.c ln_bus), for a check on the FM-1: CC 116 on channel 16 asks, the reply is
 * CC 100 id / 101 - 102 value (14 bits, as Frodhes' CC 116 items) on channel 1: 20 the cap now, 21 the lowest cap,
 * 22 the room now (us), 23 the least room, 24 the longest audio interrupt (us), 25 the reverb (us, held), 26 the
 * lanes before the bus (held), 27 the rest (held); 21, 23 and 24 since the last ask, then they restart. Core 1
 * (core1.h): 30 its state (C1_*: 1 running), 31 the bad reads it counted, 32 / 33 the blocks it rendered lanes in
 * (low / high 14 bits), 34 the jobs lost, 35 the longest wait for it (us, since the last ask), 36 / 37 VDC14 / SYSVDD
 * as the boot loader left them, 38 / 39 now, 40 its lanes in the last block (a mask), 41 its stack's deepest (bytes),
 * 42 the governor's pool now (us), 43 / 44 the user / system stack's deepest (4-byte words, since boot); the mailbox
 * (core1.h): 45 core 1's read-again recoveries, 46 core 0's, 47 the jobs core 1 refused (done here), 48 late jobs, 49
 * holds for good, 50 retries (C1_RETRY), 51 SYS_DIV's low 14 bits now, 52 1 = C1_HSB2 wrote it; 53 the crash count
 * (fm1_crash), 54 its vector, 55 the reset's P33 reason (bit 0 power-on, 2 watchdog, 6 soft) */
/* an item: three USB-MIDI events; it waits (up to 50 ms) for room in the 64-event out queue (usb.c drops what does
 * not fit: the reply is 23 items, 69 events, and its last ones were lost) */
static void tx_dbg_item(uint32_t id, uint32_t v)
{
    uint32_t t0 = fm1_ticks();
    while (usb.config && mo_w - mo_r > MQ - 3u && fm1_ticks() - t0 < 50000u * FM1_TICKS_PER_US) fm1_wdt_feed();
    if (v > 16383u) v = 16383u;
    midi_send(0xB0, 100, id); midi_send(0xB0, 101, v >> 7); midi_send(0xB0, 102, v & 127u);
}
static void tx_dbg_reply(void)
{
    uint32_t um = ln.use_max, cl = ln.cap_lo, im = isr_st.max_us;
    ln.use_max = 0; ln.cap_lo = 0; isr_st.max_us = 0;
    tx_dbg_item(20, ln.fx.gd.cap);
    tx_dbg_item(21, TX_GD_MAXG - cl);
    tx_dbg_item(22, ln.fx.room_us > 0 ? (uint32_t)ln.fx.room_us : 0u);
    tx_dbg_item(23, um < TX_GOV_ISR_TARGET ? TX_GOV_ISR_TARGET - um : 0u);
    tx_dbg_item(24, im);
    tx_dbg_item(25, ln.rev_pk);
    tx_dbg_item(26, ln.pre_pk);
    tx_dbg_item(27, ln.post_pk);
    tx_dbg_item(30, c1_state); tx_dbg_item(31, c1_mb.bad);
    tx_dbg_item(32, ln.par_blocks & 16383u); tx_dbg_item(33, ln.par_blocks >> 14 & 16383u);
    tx_dbg_item(34, ln.par_lost);
    tx_dbg_item(35, c1_wait_max / C1_TPU); c1_wait_max = 0;
    tx_dbg_item(36, c1_rail_boot[0]); tx_dbg_item(37, c1_rail_boot[1]);
    tx_dbg_item(38, c1_rail(C1_P3_VDC14, 7u)); tx_dbg_item(39, c1_rail(C1_P3_SYSVDD, 15u));
    tx_dbg_item(40, ln.par_mask); tx_dbg_item(41, c1_stack_used());
    tx_dbg_item(42, ln.gov.budget);
    tx_dbg_item(43, tx_stk_used((const uint32_t *)_ustack_lo, (const uint32_t *)_guard1));
    tx_dbg_item(44, tx_stk_used((const uint32_t *)_sstack_lo, (const uint32_t *)_sstack_top));
    tx_dbg_item(45, c1_mb.rec); tx_dbg_item(46, c1_rec0); tx_dbg_item(47, c1_ref); tx_dbg_item(48, c1_late);
    tx_dbg_item(49, c1_holds); tx_dbg_item(50, c1_retries);
    tx_dbg_item(51, c1_hsb[1] & 16383u); tx_dbg_item(52, c1_hsb[2]);
    tx_dbg_item(53, fm1_crash.magic == FM1_CRASH_MAGIC ? fm1_crash.count : 0u);
    tx_dbg_item(54, fm1_crash.magic == FM1_CRASH_MAGIC ? fm1_crash.vec : 0u);
    tx_dbg_item(55, fm1_boot.p3_rst);
}
#endif

static void tx_midi_poll(void)
{
    uint8_t st, d1, d2;
    int src;
    while ((src = midi_get(&st, &d1, &d2)) != 0) {
        uint32_t t = st & 0xF0u, ch = st & 0x0Fu;
        if (st >= 0xF8u) { (void)tempo_clock(st, (uint32_t)d1 | (uint32_t)d2 << 8, src); continue; }   /* (d1 d2: the
                                                 * arrival and the pulse count, as platform.c stamps them) */
        if (st == 0xF2u) { tempo_spp((uint32_t)(d1 & 127u) | (uint32_t)(d2 & 127u) << 7, src); continue; }
#ifdef LN_BUS_TIMED
        if (t == 0xB0u && ch == 15u && d1 == 116u) { tx_dbg_reply(); continue; }   /* (the grain room's check) */
#endif
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
#ifndef DINGHY_HOST
        c1_retry_poll(fm1_ms);                  /* (C1_RETRY only: core 1 started again after a hold) */
#endif
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
        for (i = 0; i < TX_FL_NP; i++)
            if (ln.fx.filter[k].p[i] != pv_lane[k][PL_FILTER0 + i] && !mod_holds(PID(k, PL_FILTER0 + i))) { snprintf(msg, n, "lane %u filter %u: %u, not %u", k + 1u, i, ln.fx.filter[k].p[i], pv_lane[k][PL_FILTER0 + i]); return 1; }
    }
    for (k = 0; k < TX_DR_NP; k++)                  /* the mix's DRIVE: both sides */
        if ((ln.fx.drive[0].p[k] != pv_glob[PG_DRIVE0 + k] || ln.fx.drive[1].p[k] != pv_glob[PG_DRIVE0 + k])
            && !mod_holds(PID(P_GLOBAL, PG_DRIVE0 + k))) { snprintf(msg, n, "drive %u: %u %u, not %u", k, ln.fx.drive[0].p[k], ln.fx.drive[1].p[k], pv_glob[PG_DRIVE0 + k]); return 1; }
    for (k = 0; k < TX_GD_NP; k++)
        if (ln.fx.gd.p[k] != pv_glob[PG_GRAIN0 + k] && !mod_holds(PID(P_GLOBAL, PG_GRAIN0 + k))) { snprintf(msg, n, "grain %u: %u, not %u", k, ln.fx.gd.p[k], pv_glob[PG_GRAIN0 + k]); return 1; }
    if (ln.fx.gd.sync != tm.gsync_smp) { snprintf(msg, n, "grain sync %u, not %u", ln.fx.gd.sync, tm.gsync_smp); return 1; }
    for (k = 0; k < TX_RV_NP; k++)
        if (ln.fx.rev.p[k] != pv_glob[PG_REVERB0 + k] && !mod_holds(PID(P_GLOBAL, PG_REVERB0 + k))) { snprintf(msg, n, "reverb %u: %u, not %u", k, ln.fx.rev.p[k], pv_glob[PG_REVERB0 + k]); return 1; }
    for (k = 0; k < TX_CP_NP; k++)
        if (ln.mix.comp.p[k] != pv_glob[PG_COMP0 + k]) { snprintf(msg, n, "comp %u: %u, not %u", k, ln.mix.comp.p[k], pv_glob[PG_COMP0 + k]); return 1; }
    return 0;
}
static int host_state(const char *key, char *v, unsigned n)
{
    unsigned k, i;
    char c;
    if (!strcmp(key, "app")) snprintf(v, n, "%s", APP_NAME);
    else if (!strcmp(key, "palette")) snprintf(v, n, "%s", COLOR_NAME[p_color]);
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
    else if (!strcmp(key, "compred")) snprintf(v, n, "%d", tx_comp_gr_db10(&ln.mix.comp) >= 10);   /* the COMPRESSOR
                                                         * reducing 1 dB or more now (its meter's reading) */
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
