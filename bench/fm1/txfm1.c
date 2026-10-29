/* SPDX-License-Identifier: GPL-3.0-only */
/* txfm1: a bench firmware for the FM-1 that times Textura's engines in the audio interrupt (cost pass step 1).
 * Built by tools/fm1_bench.py on Dinghy's platform (firmware/src/platform.c, the developer tick profiler on:
 * FM1_PROF_TICK); driven over USB-MIDI by tools/fm1_timing.py. It never writes the flash.
 *
 * The table (build/fm1/gen/fm1_cfg.h, bench/fm1/txfm1_gen.c) is the host twin's (txfm1_host.c). A run: the main loop
 * sets a configuration up, then the audio interrupt renders one of its blocks per interrupt, timed on the 24 MHz
 * clock minus the timer ticks nested in it (platform.c tick_st); the warm-up first (the scene's first 2 s), then the
 * measured run. The sound goes out 12 dB down (the effects' noise input is muted).
 *
 * MIDI (channel 1 control changes; replies as CC 100 item, CC 101 / 102 value MSB / LSB, 14 bits):
 *   CC 20 / 21   the configuration, MSB / LSB
 *   CC 22 n      run it n times (1..3), each with its warm-up
 *   CC 23 r      run r's results (when done): items 1 worst, 2 w8, 3 mean, 4 raw worst, 5 raw w8 (us x 2),
 *                6..8 the hash (14 + 14 + 4 bits), 10 + 2k / 11 + 2k lane k's worst / w8 (us x 2), then 99 = r
 *   CC 24        the state: items 90 state (0 idle, 1 running, 2 done), 91 configuration, 92 runs done, 93 the number
 *                of configurations, 94 / 95 the system / user stack's deepest use (bytes, since boot), 96 the
 *                longest whole interrupt since the last CC 24 (us), 97 the pool's bytes free / 32, 98 the run's size / 1024
 * Integers only. */
#include <stdint.h>
#define APP_NAME "TXFM1"
#define APP_OUT_WRITTEN 1
#include "platform.c"
#include "txfm1_core.h"
#include "fm1_cfg.h"

#define FM_RUN_MAX (304u * 1024u)
#define FM_RUNS 3u
static uint8_t fm_mem[FM_RUN_MAX] __attribute__((aligned(8), section(".pool.txfm1")));
#define RUN ((fm_run_t *)fm_mem)

static volatile uint8_t fm_state;              /* 0 idle, 1 running (the interrupt), 2 done, 3 next run (main) */
static volatile uint8_t fm_runs, fm_done;
static uint16_t fm_cfg;
static fm_stats_t fm_res[FM_RUNS];

static uint32_t ck_now(void) { return fm1_ticks(); }
static uint32_t ck_nested(void) { return tick_st.blk; }
static const fm_clock_t fm_ck = {ck_now, ck_nested};

static int32_t fm_L[TX_BLOCK], fm_R[TX_BLOCK];
static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i;
    int sh = 10;
    if (fm_state != 1u || n != TX_BLOCK) {
        for (i = 0; i < 2u * n; i++) out[i] = 0;
        return;
    }
    if (!fm_step(RUN, &fm_ck, fm_L, fm_R)) {
        fm_res[fm_done] = *fm_stats(RUN);
        fm_done++;
        fm_state = fm_done < fm_runs ? 3u : 2u;
    }
    if (FM_CFG[fm_cfg].kind == FM_K_FX) sh = 31;
    if (FM_CFG[fm_cfg].kind == FM_K_SCENE)
        for (i = 0; i < n; i++) fm_R[i] = fm_L[i];
    for (i = 0; i < n; i++) {
        int32_t l = sh > 30 ? 0 : fm_L[i] >> sh, r = sh > 30 ? 0 : fm_R[i] >> sh;
        out[2u * i] = l > 32767 ? 32767 : l < -32767 ? -32767 : l;
        out[2u * i + 1u] = r > 32767 ? 32767 : r < -32767 ? -32767 : r;
    }
}

/* the stacks' deepest use: painted at boot, scanned on request */
#define FM_PAINT 0x5AA5C33Cu
extern char _ustack_top[];                     /* (fm1_guard.h declares the others) */
#define W(x) ((uint32_t *)(void *)(x))
static void fm_paint(void)
{
    volatile uint32_t here = 0;
    uint32_t *p;
    fm1_irq_off();                             /* (no interrupt is running: the system stack is all free) */
    for (p = W(_sstack_lo); p < W(_sstack_top); p++) *p = FM_PAINT;
    fm1_irq_on();
    for (p = W(_ustack_lo); p < (uint32_t *)(uintptr_t)&here - 64; p++) *p = FM_PAINT;
}
static uint32_t fm_used(const uint32_t *lo, const uint32_t *top)
{
    const uint32_t *p = lo;
    while (p < top && *p == FM_PAINT) p++;
    return (uint32_t)((top - p) * 4);
}

static void fm_item(uint32_t id, uint32_t v)
{
    if (v > 16383u) v = 16383u;
    midi_send(0xB0, 100, id); midi_send(0xB0, 101, v >> 7); midi_send(0xB0, 102, v & 127u);
}
static uint32_t us2(uint32_t ticks) { return ticks / (FM_TPU / 2u); }   /* ticks -> us x 2 */

static void fm_show(void)
{
    char b[40];
    uint32_t i = 0, v;
    const char *s = fm_state == 1u || fm_state == 3u ? "RUN " : fm_state == 2u ? "DONE " : "IDLE ";
    while (*s) b[i++] = *s++;
    v = fm_cfg;
    if (v >= 100u) b[i++] = (char)('0' + v / 100u);
    if (v >= 10u) b[i++] = (char)('0' + v / 10u % 10u);
    b[i++] = (char)('0' + v % 10u);
    b[i++] = ' '; b[i++] = (char)('0' + fm_done); b[i++] = '/'; b[i++] = (char)('0' + fm_runs);
    b[i] = 0;
    draw_text_box(0, 120, 240, &FONT_S, b, C_WHITE, 1);
}

static void fm_start(void)
{
    isr_st.max_us = 0;
    fm_begin(RUN, FM_CFG, FM_EV, fm_cfg, 1);
    fm_state = 1;
}

static void fm_midi(void)
{
    uint8_t st, d1, d2;
    uint32_t i, r;
    while (midi_get(&st, &d1, &d2)) {
        if ((st & 0xF0u) != 0xB0u) continue;
        if (d1 == 20u && fm_state != 1u) fm_cfg = (uint16_t)((fm_cfg & 127u) | d2 << 7);
        else if (d1 == 21u && fm_state != 1u) fm_cfg = (uint16_t)((fm_cfg & ~127u) | d2);
        else if (d1 == 22u && fm_state != 1u && fm_state != 3u && fm_cfg < FM_NCFG) {
            fm_runs = (uint8_t)(d2 < 1u ? 1u : d2 > FM_RUNS ? FM_RUNS : d2);
            fm_done = 0;
            fm_start();
            fm_show();
        } else if (d1 == 23u && fm_state == 2u && d2 < fm_done) {
            const fm_stats_t *s = &fm_res[d2];
            fm_item(1, us2(s->worst)); fm_item(2, us2(s->w8)); fm_item(3, s->n ? us2(s->sum / s->n) : 0u);
            fm_item(4, us2(s->raw_worst)); fm_item(5, us2(s->raw_w8));
            fm_item(6, s->hash & 16383u); fm_item(7, s->hash >> 14 & 16383u); fm_item(8, s->hash >> 28);
            if (FM_CFG[fm_cfg].kind == FM_K_LANES)
                for (i = 0; i < 4u; i++) { fm_item(10u + 2u * i, us2(s->lane_worst[i])); fm_item(11u + 2u * i, us2(s->lane_w8[i])); }
            fm_item(99, d2);
        } else if (d1 == 24u) {
            r = isr_st.max_us; isr_st.max_us = 0;
            fm_item(90, fm_state == 3u ? 1u : fm_state); fm_item(91, fm_cfg); fm_item(92, fm_done); fm_item(93, FM_NCFG);
            fm_item(94, fm_used(W(_sstack_lo), W(_sstack_top))); fm_item(95, fm_used(W(_ustack_lo), W(_ustack_top)));
            fm_item(96, r);
            {
                extern uint32_t _pool_end[];
                fm_item(97, (0x01C20000u + 0x54000u - (uint32_t)_pool_end) / 32u);   /* (x 32 bytes) */
            }
            fm_item(98, fm_run_bytes() / 1024u);
        }
    }
}

static void app_main(void)
{
    uint8_t shown = 0xFF;
    fm_paint();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 80, 240, &FONT_S, "TEXTURA BENCH (TXFM1)", C_WHITE, 1);
    if (fm_run_bytes() > FM_RUN_MAX) draw_text_box(0, 160, 240, &FONT_S, "RUN STATE TOO BIG", C_WHITE, 1);
    for (;;) {
        platform_poll();
        if (fm_run_bytes() <= FM_RUN_MAX) fm_midi();
        if (fm_state == 3u) fm_start();
        if (shown != (uint8_t)(fm_state + 4u * fm_done)) { shown = (uint8_t)(fm_state + 4u * fm_done); fm_show(); }
    }
}
