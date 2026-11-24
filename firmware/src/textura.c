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
 * matrix (mod.c), the scenes and the settings (scenes.c), RANDOM's dice (dice.c), the screen in Frodhes' look (ui.c);
 * MIDI notes on channel 1 play the selected lane, on a lane's own channel that lane (notes.c), program changes pick a
 * scene.
 *
 * Rules of the road (no FPU, no libgcc): integers only, no float, no 64-bit division. */
#include <stdint.h>
#define APP_NAME "TEXTURA"
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_9TX"                   /* the package identity: a placeholder until stage 8 decides it */
#endif
#define APP_OUT_SHIFT 8u                        /* the codec at full scale (as Frodhes): +6 dB on the jack over
                                                 * Dinghy's -6 dBFS ceiling; the output limiter holds -2 dBFS */
#define APP_OUT_WRITTEN 1                       /* app_audio writes every sample of out */
#define CV_MAX (240u * 32u)                     /* the canvas (gfx.c): 32 rows of 240 px (15 KB); a part of the screen
                                                 * taller than that is drawn in strips (ui.c ui_strips) */
/* TX_RTD: a zero-initialised buffer in RAMTEXT's room after the RAM code (app.ld .rtdata: the room .ram_text leaves,
 * ~11 KB, the region's own; cleared in APP_EARLY, before the audio and the main loop start). Only buffers the CPU alone
 * reads and writes (no DMA), the app's own (nothing the platform touches before APP_EARLY) */
#ifdef DINGHY_HOST
#define TX_RTD
#else
#define TX_RTD __attribute__((section(".rtdata")))
#endif
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
static void tx_rtd_clear(void);
#define C1_RAILS
#define APP_EARLY() (tx_rtd_clear(), tx_stk_paint(), c1_hold(), c1_rails(), \
                     c1_hsb2((fm1_boot.p3_rst & 1u) && !(fm1_boot.p3_rst & 4u)), (void)c1_start())
#define APP_RESET_PREP() c1_hold()
#define APP_FAULT(c) c1_fault(c)
static void c1_quiet(void);
#define FL_C1_QUIET c1_quiet                     /* (the flash windows wait for core 1 idle: hal/fm1_flash_map.h) */
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
static void tx_rtd_clear(void)                  /* (app.ld .rtdata: NOLOAD, so not cleared by the platform's start) */
{
    extern uint32_t _rtd_start[], _rtd_end[];
    uint32_t *p;
    for (p = _rtd_start; p < _rtd_end; p++) *p = 0;
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
#include "groove.h"                             /* (GROOVE: lanes.c's hook, LN_GROOVE) */
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
static TX_RTD int32_t tx_L[TX_BLOCK], tx_R[TX_BLOCK];
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

#ifdef DINGHY_HOST
/* the tests' faults (make fw-test, tests/fault/): HOST_ENGINE_DROP=NAME: no engine of that name in the build;
 * HOST_ENGINE_FAT=NAME: it asks more state than a lane's arena (state_bytes past TX_STATE_MAX) */
static const tx_engine_t *host_engine_find(const char *name)
{
    static tx_engine_t fat;
    const tx_engine_t *e = tx_engine_find(name);
    const char *d = getenv("HOST_ENGINE_DROP"), *f = getenv("HOST_ENGINE_FAT");
    if (e && d && !strcmp(name, d)) return 0;
    if (e && f && !strcmp(name, f)) { fat = *e; fat.state_bytes = TX_STATE_MAX + 1u; return &fat; }
    return e;
}
#define tx_engine_find host_engine_find
#endif
#include "scape.c"
#include "notes.c"
#include "params.c"
#include "tempo.c"
#include "mod.c"
#include "groove.c"
#include "presets.c"
#include "scenes.c"
#include "dice.c"
static uint32_t ui_ndraw;                       /* screen parts ui_draw drew (ui.c), for the draw time (ui_draw_timed) */
static int32_t tx_inj_steps(uint32_t e);        /* (the dev hook's turns: tx_inj, below) */
#include "ui.c"

/* ---- MIDI: notes on SELECTED CH (1) play the selected lane, on a lane's MIDI CH (lane n: n) that lane, pitched,
 * through SCALE (notes.c nt_midi); control changes reach the params table (a lane's on its MIDI CH, the effects on the
 * mix on channel 5: params.c); the clock and the transport go to tempo.c */
#ifdef LN_BUS_TIMED
/* the main loop's screen drawing (ui_draw, one part of the screen a call), timed when it drew: each call's time less
 * the audio interrupt's when one came in it (its last_us); the longest and the sum since the last ask (items 56, 57,
 * 58 below), tools/fm1_dbg.py */
static uint32_t ui_us_max, ui_us_sum, ui_us_n;
static void ui_draw_timed(void)
{
    uint32_t n0 = ui_ndraw, i0 = isr_st.t_prev, t0 = fm1_ticks(), d;
    ui_draw();
    if (ui_ndraw == n0) return;
    d = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
    if (isr_st.t_prev != i0) d = d > isr_st.last_us ? d - isr_st.last_us : 0u;
    if (d > ui_us_max) ui_us_max = d;
    ui_us_sum += d; ui_us_n++;
}
#endif
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
 * (fm1_crash), 54 its vector, 55 the reset's P33 reason (bit 0 power-on, 2 watchdog, 6 soft); the screen (ui_draw_timed):
 * 56 the longest part drawn (us), 57 the mean (us), 58 the parts drawn (to 16383), all since the last ask */
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
    tx_dbg_item(56, ui_us_max); tx_dbg_item(57, ui_us_n ? ui_us_sum / ui_us_n : 0u); tx_dbg_item(58, ui_us_n);
    ui_us_max = 0; ui_us_sum = 0; ui_us_n = 0;
}
#endif

/* ---- the panel driven over MIDI, for a check on the FM-1 (tools/fm1_uitour.py: the UI's stack, its draw time): a
 * dev hook, compiled in, inert until armed. Channel 16: CC 116 value 85 arms it for 10 s (each event it takes renews
 * that; any other CC 116 value is the debug ask); armed, CC 117 presses a button (value: its index, + 64 releases it)
 * and CC 118 turns an encoder (value: its index x 16 + 8 + the detents, -8 .. +7). The presses and turns join the
 * panel's own (app_main, ui_input) as edges and steps: nothing else changes; unarmed, CC 117 / 118 go on as before */
#define TX_INJ_ARM 85u
#define TX_INJ_MS 10000u
static struct { uint32_t t, p, r; int32_t enc[8]; } tx_inj;
static int tx_inj_cc(uint32_t cc, uint32_t v)   /* 1: taken */
{
    if (cc == 116u) { if (v != TX_INJ_ARM) return 0; tx_inj.t = fm1_ms | 1u; return 1; }
    if (!tx_inj.t || fm1_ms - tx_inj.t >= TX_INJ_MS) { tx_inj.t = 0; return 0; }
    tx_inj.t = fm1_ms | 1u;
    if (cc == 117u) { uint32_t m = 1u << (v & 31u); if (v & 64u) tx_inj.r |= m; else tx_inj.p |= m; }
    else tx_inj.enc[v >> 4 & 7u] += (int32_t)(v & 15u) - 8;
    return 1;
}
static uint32_t tx_inj_edges(uint32_t *rel)     /* (app_main: with the panel's edges) */
{
    uint32_t p = tx_inj.p;
    *rel |= tx_inj.r;
    tx_inj.p = tx_inj.r = 0;
    return p;
}
static int32_t tx_inj_steps(uint32_t e)         /* (ui_input: with the encoder's steps) */
{
    int32_t s = tx_inj.enc[e & 7u];
    tx_inj.enc[e & 7u] = 0;
    return s;
}

static void tx_midi_poll(void)
{
    uint8_t st, d1, d2;
    int src;
    while ((src = midi_get(&st, &d1, &d2)) != 0) {
        uint32_t t = st & 0xF0u, ch = st & 0x0Fu;
        if (st >= 0xF8u) { (void)tempo_clock(st, (uint32_t)d1 | (uint32_t)d2 << 8, src); continue; }   /* (d1 d2: the
                                                 * arrival and the pulse count, as platform.c stamps them) */
        if (st == 0xF2u) { tempo_spp((uint32_t)(d1 & 127u) | (uint32_t)(d2 & 127u) << 7, src); continue; }
        if (t == 0xB0u && ch == 15u && d1 >= 116u && d1 <= 118u && tx_inj_cc(d1, d2)) continue;   /* (the dev hook) */
#ifdef LN_BUS_TIMED
        if (t == 0xB0u && ch == 15u && d1 == 116u) { tx_dbg_reply(); continue; }   /* (the grain room's check) */
#endif
        if (t == 0xB0u) { if (!p_cc(ch, d1, d2)) mod_cc_in(d1, d2); continue; }   /* (not ours: the mod wheel, MOD CC) */
        if (t == 0xD0u) { mod_at(d1); continue; }
        if (t == 0xA0u) { mod_at(d2); continue; }
        if (t == 0xC0u && ch == 0u) { scn_pc(d1); continue; }   /* (program change: a scene, scenes.c) */
        if (t == 0x90u || t == 0x80u) nt_midi(ch, d1 & 127u, t == 0x90u ? d2 : 0u);   /* (notes.c: SELECTED CH, the
                                                 * lanes' own) */
    }
}

static void app_main(void)
{
    palette_set(tx_palette_index(TX_PALETTE));
    ln_init();
    sc_init();
    gr_init();
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
        b |= tx_inj_edges(&rel);                /* (the dev hook's presses: tx_inj) */
        ui_input(b, rel);
        nt_poll();
        tx_midi_poll();
        sc_poll();
        p_poll();
        tempo_poll();
        mod_poll();
        gr_poll();
        scn_poll();
        dice_poll();
#ifdef LN_BUS_TIMED
        ui_draw_timed();
#else
        ui_draw();
#endif
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
    if (scn_gl_n) { snprintf(msg, n, "%u values still gliding", scn_gl_n); return 1; }
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
                                                 * recorded is its default; the tempo: with PRESETS CHANGE TEMPO YES) */
        int32_t v;
        if (P_SCOPE(s->r[i].id) == P_GLOBAL && P_IDX(s->r[i].id) == PG_FINE && !tm.scn_tempo) continue;
        if (!scn_ap_fx && scn_fx_id(s->r[i].id)) continue;   /* (the effects kept: as they were) */
        for (j = 0; j < now.nrec && now.r[j].id != s->r[i].id; j++) {}
        v = j < now.nrec ? now.r[j].v : scn_def_of(s->r[i].id);
        if (v != s->r[i].v) { snprintf(msg, n, "id 0x%03X: %d, not %d", s->r[i].id, v, s->r[i].v); return 1; }
    }
    for (j = 0; j < now.nrec; j++) {            /* and nothing set that the scene has not (a card, a value off its
                                                 * default) */
        if (P_SCOPE(now.r[j].id) == P_GLOBAL && P_IDX(now.r[j].id) == PG_FINE) continue;
        if (!scn_ap_fx && scn_fx_id(now.r[j].id)) continue;
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
    /* the chain plays the effects' values the table holds */
    for (k = 0; k < TX_DR_NP; k++)                  /* the mix's DRIVE: both sides */
        if ((ln.fx.drive[0].p[k] != pv_glob[PG_DRIVE0 + k] || ln.fx.drive[1].p[k] != pv_glob[PG_DRIVE0 + k])
            && !mod_holds(PID(P_GLOBAL, PG_DRIVE0 + k))) { snprintf(msg, n, "drive %u: %u %u, not %u", k, ln.fx.drive[0].p[k], ln.fx.drive[1].p[k], pv_glob[PG_DRIVE0 + k]); return 1; }
    for (k = 0; k < TX_MF_NP; k++)                  /* the mix's FILTER */
        if (ln.fx.mf.p[k] != pv_glob[PG_MF0 + k] && !mod_holds(PID(P_GLOBAL, PG_MF0 + k))) { snprintf(msg, n, "filter %u: %u, not %u", k, ln.fx.mf.p[k], pv_glob[PG_MF0 + k]); return 1; }
    for (k = 0; k < TX_GD_NP; k++)
        if (ln.fx.gd.p[k] != pv_glob[PG_GD(k)] && !mod_holds(PID(P_GLOBAL, PG_GD(k)))) { snprintf(msg, n, "grain %u: %u, not %u", k, ln.fx.gd.p[k], pv_glob[PG_GD(k)]); return 1; }
    if (ln.fx.gd.sync != tm.gsync_smp) { snprintf(msg, n, "grain sync %u, not %u", ln.fx.gd.sync, tm.gsync_smp); return 1; }
    for (k = 0; k < TX_RV_NP; k++)
        if (ln.fx.rev.p[k] != pv_glob[PG_REVERB0 + k] && !mod_holds(PID(P_GLOBAL, PG_REVERB0 + k))) { snprintf(msg, n, "reverb %u: %u, not %u", k, ln.fx.rev.p[k], pv_glob[PG_REVERB0 + k]); return 1; }
    for (k = 0; k < TX_CP_NP; k++)
        if (ln.mix.comp.p[k] != pv_glob[PG_COMP0 + k]) { snprintf(msg, n, "comp %u: %u, not %u", k, ln.mix.comp.p[k], pv_glob[PG_COMP0 + k]); return 1; }
    for (k = 0; k < TX_MS_NP; k++)                  /* the M/S */
        if (ln.mix.ms.p[k] != pv_glob[PG_MS0 + k] && !mod_holds(PID(P_GLOBAL, PG_MS0 + k))) { snprintf(msg, n, "m/s %u: %u, not %u", k, ln.mix.ms.p[k], pv_glob[PG_MS0 + k]); return 1; }
    if (ln.mix.mode != p_output) { snprintf(msg, n, "output %u, not %u", ln.mix.mode, p_output); return 1; }   /* (OUTPUT) */
    if (ln.mix.amt != p_binamt || ln.mix.head != p_binhead) {   /* (its binaural AMOUNT, HEAD SIZE) */
        snprintf(msg, n, "binaural %u %u, not %u %u", ln.mix.amt, ln.mix.head, p_binamt, p_binhead); return 1;
    }
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
    else if (sscanf(key, "knote%u%c", &k, &c) == 1 && k < TX_NKEYS) { if (nt.playing >> k & 1u) snprintf(v, n, "%u:%u", nt.lane[k] + 1u, nt.note[k]); else snprintf(v, n, "-"); }   /* key
                                                 * k's lane and note sounding ("2:60"; none: "-") */
    else if (!strcmp(key, "mnotes")) {          /* the MIDI notes held: "lane:note" each ("2:62 3:62"; none: "-") */
        v[0] = 0;
        for (i = 0; i < nt_mn; i++) { char t[12]; snprintf(t, sizeof t, "%s%u:%u", i ? " " : "", nt_mq[i].lane + 1u, nt_mq[i].out); strncat(v, t, n - strlen(v) - 1u); }
        if (!v[0]) snprintf(v, n, "-");
    }
    else if (!strcmp(key, "chain") || !strcmp(key, "fxsteps") || !strcmp(key, "chainok")) {   /* the effects as the
                                                 * audio ran them in the last block (TX_STAGE), as FX steps through
                                                 * them; "chainok" 1: the steps, and the front page's knobs, in the
                                                 * audio's order */
        static const char *const NM[TX_ST_N] = {"COMP", "DRIVE", "FILTER", "GRAIN", "REVERB", "M/S"};
        uint8_t pg[8], fr[4], *a = key[0] == 'f' ? pg : tx_st_seen;
        unsigned np = ui_fx_order(pg, fr), na = key[0] == 'f' ? np : tx_st_n, i, ok = np == tx_st_n;
        v[0] = 0;
        for (i = 0; i < np && ok; i++) ok = pg[i] == tx_st_seen[i];
        for (i = 1; i < 4u && ok; i++) ok = fr[i] >= fr[i - 1];   /* (PERFORM: the FILTER's two edges side by side) */
        if (key[5] == 'o') snprintf(v, n, "%u", ok);
        else for (i = 0; i < na; i++) { if (i) p_cat(v, " ", n); p_cat(v, a[i] < TX_ST_N ? NM[a[i]] : "?", n); }
    }
    else if (!strcmp(key, "ft2")) snprintf(v, n, "%s", ui_ft2);   /* the footer's line 2 on a group's page (ft_next) */
    else if (!strcmp(key, "msg")) snprintf(v, n, "%s", ui_msg_t && fm1_ms - ui_msg_t < MSG_MS ? ui_msg : "");
    else if (!strcmp(key, "dropped")) snprintf(v, n, "%u", ln.dropped);
    else if (!strcmp(key, "mscorr")) snprintf(v, n, "%d", (int)ln.mix.ms.corr);   /* the M/S's meters (kept while its page
                                                 * shows them): the correlation, Q15; mspk0 .. 3 the L R M S peaks, Q23 */
    else if (sscanf(key, "mspk%u%c", &k, &c) == 1 && k < 4u) snprintf(v, n, "%d", (int)ln.mix.ms.pk[k]);
    else if (!strcmp(key, "govus")) snprintf(v, n, "%u", ln_gov_us());            /* the governor: the pool's cost */
    else if (!strcmp(key, "govpool")) snprintf(v, n, "%u", ln.gov.budget);
    else if (!strcmp(key, "govmix")) snprintf(v, n, "%u", ln.mix_us);   /* HEADPHONES' pan in the pool, us (SPEAKERS: 0) */
    else if (!strcmp(key, "phones")) snprintf(v, n, "%u", ln.mix.hp);   /* the mix's pan: 1 binaural (OUTPUT HEADPHONES,
                                                 * AMOUNT over 0) */
    else if (!strcmp(key, "binamt")) snprintf(v, n, "%u", ln.mix.amt);   /* the mix's binaural AMOUNT, HEAD SIZE */
    else if (!strcmp(key, "binhead")) snprintf(v, n, "%u", ln.mix.head);
    else if (!strcmp(key, "govcut")) { for (i = 0, k = 0; k < SC_N; k++) i += ln_gov_cur(k) < ln_gov_user(k); snprintf(v, n, "%u", i); }   /* lanes cut */
    else if (sscanf(key, "gov%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", ln_gov_cur(k));   /* heroes now */
    else if (sscanf(key, "lane%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%s", ln_engine(k) ? ln_engine(k)->name : "-");
    else if (sscanf(key, "snd%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%s", sc_sound_name(k));
    else if (sscanf(key, "cnt%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", sc_cnt_of(k));
    else if (sscanf(key, "gcur%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%d", (int)gr_cursor(k));   /* (GROOVE 2/2: the sweep cursor) */
    else if (sscanf(key, "act%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", ui_lane_act(k));
    else if (sscanf(key, "run%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", sc.run[k]);
    else if (sscanf(key, "vol%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", sc_vol_of(k));
    else if (sscanf(key, "pan%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", sc.pan[k]);
    else if (sscanf(key, "mute%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", sc.mute[k]);
    else if (sscanf(key, "fam%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%s", LS[sc.lf[k]].name);   /* the lane's family */
    else if (sscanf(key, "ring%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%d", tx_ring_holds(&ln_out[k].ring));   /* an old engine rings */
    else if (sscanf(key, "rings%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%d", ln_out[k].rpk > 0);   /* and sounds */
    else if (!strcmp(key, "swaps")) { for (i = 0, k = 0; k < SC_N; k++) i += sc.a_eng[k] != sc.eng[k] || sc.reswap[k]; snprintf(v, n, "%u", i); }   /* lanes waiting */
    else if (sscanf(key, "sel%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", sc.nsel[k]);   /* the lane's switches so far */
    else if (!strcmp(key, "glides")) { for (i = scn_gl_n, k = 0; k < SC_N; k++) i += sc.gl_on[k]; snprintf(v, n, "%u", i); }   /* settings on their way */
    else if (!strcmp(key, "browser")) snprintf(v, n, "%u", ub.on ? 2u : 0u);   /* a browser open: 2 (0 shut) */
    else if (!strcmp(key, "brkept")) snprintf(v, n, "%s", ub.on && !ub.pr && ub.oi >= 0 ? LS[ub.of].s[ub.oi].name : "-");   /* the sound
                                                 * browser's lane's kept sound */
    else if (!strcmp(key, "brfam")) snprintf(v, n, "%s", !ub.pr ? LS[ub.f].name : ub_cat_name(ub.f));   /* (the presets
                                                 * list: the cursor's family) */
    else if (!strcmp(key, "brpr")) snprintf(v, n, "%u", ub.on && ub.pr);   /* the browser open is the presets one */
    else if (!strcmp(key, "brcur")) snprintf(v, n, "%u", ub_pos());   /* (the place in its family) */
    else if (!strcmp(key, "bropt")) snprintf(v, n, "%u", ub.on && ub.opt);   /* the presets list's LOAD OPTIONS shown */
    else if (!strcmp(key, "leds")) {            /* the buttons lit, by name ("HOME REC") */
        static const char *const BN[14] = {"OCT-", "OCT+", "FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC"};
        uint32_t b = ui_btn_leds();
        v[0] = 0;
        for (i = 0; i < 14u; i++) if (b >> i & 1u) { if (v[0]) strncat(v, " ", n - strlen(v) - 1u); strncat(v, BN[i], n - strlen(v) - 1u); }
    }
    else if (sscanf(key, "gate%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%d", (int)ln.mix.gc[k]);   /* the mix's gate now, Q15 */
    else if (sscanf(key, "alive%u%c", &k, &c) == 1 && k < SC_N) {   /* the lane's engine made events since this was read last */
        static uint32_t was[SC_N];
        tx_stats_t st = {0};
        if (ln.mix.e[k]) ln.mix.e[k]->stats(ln.mix.st[k], &st);
        snprintf(v, n, "%d", st.total != was[k]); was[k] = st.total;
    }
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
    else if (!strcmp(key, "toast")) snprintf(v, n, "%s", ui_toast_on() ? ui_toast_nm : "");   /* the load toast's name, while shown */
    else if (!strcmp(key, "gaps")) {            /* the screen's pixels not black in the 4 px gaps between the four rows
                                                 * (an overlay closed must leave none: the pages draw only their rows) */
        unsigned x, y;
        for (i = 0, k = 0; k < 3u; k++)
            for (y = RW_Y0 + RW_DY * k + RW_H; y < RW_Y0 + RW_DY * (k + 1u); y++)
                for (x = RW_X; x < RW_X + RW_W; x++) i += fb[y * 240u + x] != 0;
        snprintf(v, n, "%u", i);
    }
    else if (!strcmp(key, "toasthd")) snprintf(v, n, "%s", ui_toast_on() ? ui_toast_hd : "");
    else if (!strcmp(key, "fxmine")) snprintf(v, n, "%u", scn_fx_mine);   /* the player turned an effect (SMART keeps them) */
    else if (!strcmp(key, "edited")) snprintf(v, n, "%u", scn_edited);    /* what plays changed since the load (the "*") */
    else if (!strcmp(key, "head")) { char f[16], s[32]; (void)ui_preset_of(f, s); snprintf(v, n, "%s %s%s", f, s, scn_edited ? "*" : ""); }   /* the
                                                 * home page's header: the family, the preset (its dot a space) */
#ifndef DINGHY_WEB                                      /* (the web has no tests/host.c sound) */
    else if (!strcmp(key, "clips") || !strcmp(key, "heard")) {   /* samples at full scale so far; something over -60 dBFS
                                                         * in the last 3 s (tests/host.c's sound; presets.txt) */
        uint32_t i, i0 = !strcmp(key, "clips") ? 0u : wav_n > 3u * FS ? wav_n - 3u * FS : 0u, cl = 0, pk = 0;
        for (i = 2u * i0; i < 2u * wav_n; i++) {
            uint32_t x = (uint32_t)(wav[i] < 0 ? -(int32_t)wav[i] : wav[i]);
            if (x >= 32767u) cl++;
            if (x > pk) pk = x;
        }
        snprintf(v, n, "%u", !strcmp(key, "clips") ? cl : pk > 33u);   /* (33: -60 dBFS) */
    }
#endif
    else if (!strcmp(key, "saving")) snprintf(v, n, "%u", scn_sv_on);
    else if (!strcmp(key, "savemode")) snprintf(v, n, "%u", scn_save_mode ? scn_save_slot + 1u : 0u);
    else if (!strcmp(key, "savename")) { snprintf(v, n, "%.11s", scn_nm); for (i = (unsigned)strlen(v); i && v[i - 1u] == ' '; i--) v[i - 1u] = 0; }   /* the
                                                 * save mode's name (trailing spaces off) */
    else if (sscanf(key, "uname%u%c", &k, &c) == 1 && k >= 1u && k <= SCN_NU) snprintf(v, n, "%s", scn_meta[k - 1u].used ? scn_meta[k - 1u].name : "-");
    else if (sscanf(key, "used%u%c", &k, &c) == 1 && k >= 1u && k <= SCN_NU) snprintf(v, n, "%u", scn_meta[k - 1u].used);
    else if (!strcmp(key, "mpos")) snprintf(v, n, "%u", mpos >> 8);               /* the song position (1/96 quarter) */
    else if (!strcmp(key, "routes")) snprintf(v, n, "%u", mbuf[mcur].nr);
    else if (sscanf(key, "lfo%u%c", &k, &c) == 1 && k < TX_LFO_N) snprintf(v, n, "%d", (int)mod_lfo[k].out);
    else if (sscanf(key, "live%x%c", &k, &c) == 1) { int32_t x; snprintf(v, n, "%d", mod_live(k, &x) ? (int)x : -1); }
    else if (!strcmp(key, "mfon")) snprintf(v, n, "%d", (ln.fx.mf.w[0] != 0) + 2 * (ln.fx.mf.w[1] != 0));   /* the mix's
                                                 * FILTER: its edges running (1 HIGH PASS, 2 LOW PASS; 0: untouched) */
    else if (!strcmp(key, "gdsync")) snprintf(v, n, "%u", ln.fx.gd.sync);         /* as the chain has it */
    else if (!strcmp(key, "gdmod")) snprintf(v, n, "%d", (int)ln.fx.gd.mg);   /* the GRAIN's MOD, gliding (Q15) */
    else if (sscanf(key, "grpart%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", gr_lane[k].part);   /* GROOVE: the
                                                 * lane's part now (1 PULSE 2 OFFBEAT 3 FILL 4 FREE) */
    else if (sscanf(key, "role%u%c", &k, &c) == 1 && k < SC_N) { uint8_t ps[32], w[32]; char r[24]; (void)ui_role_text(k, r, ps, w); snprintf(v, n, "%s%s", r, gr_wl(k) ? "" : " FLOWS FREE"); }   /* GROOVE
                                                 * LANES: the lane's row ("AUTO: PULSE", "5 PER 4 BEATS"; a free sound:
                                                 * " FLOWS FREE" after) */
    else if (sscanf(key, "grcyc%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", gr_lane[k].cycle);
    else if (sscanf(key, "grw%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", gr_lane[k].wl);   /* its sound's weight */
    else if (sscanf(key, "grm%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%d", (int)gr_m[k]);   /* its multiplier, Q12 */
    else if (sscanf(key, "grnotes%u%c", &k, &c) == 1 && k < SC_N) snprintf(v, n, "%u", grh_n[k]);   /* notes since read */
    else if (sscanf(key, "grnear%u%c", &k, &c) == 1 && k < SC_N) {   /* the share of them within 1/32 beat of its pulse
                                                 * (%; the counts start again) */
        snprintf(v, n, "%u", grh_n[k] ? grh_near[k] * 100u / grh_n[k] : 0u); grh_n[k] = grh_near[k] = 0;
    }
    else if (sscanf(key, "grfeel%u%c", &k, &c) == 1 && k < SC_N) {   /* how they fall: FREE (near its pulse no more than
                                                 * by chance, under 1.5 x), LEANS (more), SNAPS (70 % or more on it: a
                                                 * beat, never wanted; NONE: the lane has no pulse, a FREE part); the counts start again */
        uint32_t t, fr = 0, nr = grh_n[k] ? grh_near[k] * 1000u / grh_n[k] : 0u;
        for (t = 0; t < 96u * 42u; t++) fr += tx_groove_dist(&gr_lane[k], gr_type, gr_shift, t) <= 3;
        fr = fr * 1000u / (96u * 42u);          /* (by chance: the share of the time that near) */
        snprintf(v, n, "%s", !fr ? "NONE" : grh_n[k] < 20u ? "FEW" : nr >= 700u ? "SNAPS" : nr * 2u >= fr * 3u ? "LEANS" : "FREE");
        grh_n[k] = grh_near[k] = 0;
    }
    else if (!strcmp(key, "gdwow")) snprintf(v, n, "%d", (int)(ln.fx.gd.wq1 != 0));   /* its time drifting now */
    else if (!strcmp(key, "compred")) snprintf(v, n, "%d", tx_comp_gr_db10(&ln.mix.comp) >= 10);   /* the COMPRESSOR
                                                         * reducing 1 dB or more now (its meter's reading) */
    else if (sscanf(key, "lab%u%c", &k, &c) == 1 && k < 4u) snprintf(v, n, "%s", ui_row_id(k) != NOID ? ui_lab(ui_row_id(k)) : "-");
    else if (sscanf(key, "card%u%c", &k, &c) == 1 && k < 4u) {   /* the value as the card writes it */
        char t[24];
        const char *u;
        if (ui_row_id(k) == NOID) { snprintf(v, n, "-"); return 0; }
        u = p_text(ui_row_id(k), t);
        snprintf(v, n, "%s%s%s", t, u ? " " : "", u ? u : "");
    } else if (sscanf(key, "show%u%c", &k, &c) == 1 && k < 4u) {   /* the text the card draws (a MIX: DRY / 50/50 /
                                                         * WET, else "BAR": its bar alone) */
        char t[24];
        uint32_t id = ui_row_id(k);
        if (id == NOID) { snprintf(v, n, "-"); return 0; }
        if (ui_look(id) == LK_MIX) { const char *s = ui_mix_text(p_ord(id, p_get(id)), p_hi(id)); snprintf(v, n, "%s", *s ? s : "BAR"); }
        else { (void)p_text(id, t); snprintf(v, n, "%s", t); }
    } else if (sscanf(key, "p%x%c", &k, &c) == 1) snprintf(v, n, "%d", p_valid(k) ? (int)p_get(k) : -1);   /* "p120": id 0x120 */
    else if (sscanf(key, "host%u.%u%c", &k, &i, &c) == 2 && k < SC_N)   /* the hosted engine's parameter i: its override */
        snprintf(v, n, "%d", ln.mix.e[k] == sc.src ? (int)tx_source_host_value(ln.mix.st[k], i) : -2);
    else return 1;
    return 0;
}
#ifdef DINGHY_WEB
EMSCRIPTEN_KEEPALIVE const char *web_state(const char *key)   /* host_state for the web panel's checks (tests/web_check.py) */
{
    static char v[64];
    if (host_state(key, v, sizeof v)) v[0] = 0;
    return v;
}
#endif
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
