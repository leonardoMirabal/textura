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
 *   CC 26 0x55   a soft reset (core 1 held first)
 *   CC 24 items 30 / 31 VDC14 / SYSVDD as the boot loader left them, 32 / 33 now, 34 the SYS PLL multiplier (270 =
 *                360 MHz), 35 1 = this image raises the supply (TXFM1_RAILS); core 1's late jobs 36, holds for good 37,
 *                retries 38 (C1_RETRY); 39 SYS_DIV's low 14 bits, 58 1 = C1_HSB2 wrote it, 59 the reset's P33 reason
 *   CC 25        core 1's last fault frame (c1_regs, .noinit: it outlives a reset): 26 words as 3 items each from 1,
 *                90 present, then 99
 *   CC 24        the state: items 90 state (0 idle, 1 running, 2 done), 91 configuration, 92 runs done, 93 the number
 *                of configurations, 94 / 95 the system / user stack's deepest use (bytes, since boot), 96 the
 *                longest whole interrupt since the last CC 24 (us), 97 the pool's bytes free / 32, 98 the run's size / 1024;
 *                core 1 (firmware/src/core1.h): 80 its state (C1_*), 81 the bad reads it counted, 82 / 83 its jobs (low / high 14
 *                bits), 84 the longest wait for a job (us), 85 the last bad job number (low 14 bits), 86 its stack's
 *                deepest use (bytes), 87 its read-again recoveries, 88 core 0's, 89 the jobs it refused (core 0 did
 *                them), 79 core 1 faulted (stopped, c1_fault); the last crash record (fm1_crash, it outlives
 *                the reset): 60 present, 61 count, 62 vector, 63..77 pc, rets, sp, DBG_MSG, EMU_MSG (14 + 14 + 4 bits
 *                each), 78 its uptime / 100 ms; 40..57 its ICFG, PSR, then core 1's sp, usp, ssp, psr as it started
 *   C1 rows (FM_K_C1, CC 23): 20 / 21 the blocks core 1 rendered, 22 / 23 the blocks whose two renders differ, 24 / 25
 *                the first of them (+ 1), 26 / 27 the blocks core 0 rendered both (low / high 14 bits each); LANES rows
 *                on two cores (FM_PAR): the same items, the blocks core 1 rendered lanes in, the jobs lost, the block of
 *                the loss (+ 1), the blocks core 0 rendered all; and 30 / 31 core 0's / core 1's worst busy time in the
 *                render phase, 32 / 33 their means (us x 2)
 * Integers only. */
#include <stdint.h>
#define APP_NAME "TXFM1"
#define APP_OUT_WRITTEN 1
static int c1_start(void);
static void c1_hold(void);
static void c1_fault(const void *c);
static void c1_rails(void);
static void c1_hsb2(uint32_t power_on);
#define APP_EARLY() (c1_hold(), c1_rails(), c1_hsb2((fm1_boot.p3_rst & 1u) && !(fm1_boot.p3_rst & 4u)), \
                     (void)c1_start())          /* core 1 held, the supply (T1: raised; C1_HSB2: the bus slowed), started */
#define APP_RESET_PREP() c1_hold()             /* and held before every reset */
#define APP_FAULT(c) c1_fault(c)               /* its own faults: it stops, no reset */
#include "platform.c"
#include "core1.h"
#include "txfm1_core.h"
#include "fm1_cfg.h"

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

/* an item: three USB-MIDI events; it waits (up to 50 ms) for room in the 64-event out queue (usb.c drops what
 * does not fit, and a state reply is longer than the queue: its last items, the one the host waits for, were lost) */
static void fm_item(uint32_t id, uint32_t v)
{
    uint32_t t0 = fm1_ticks();
    while (usb.config && mo_w - mo_r > MQ - 3u && fm1_ticks() - t0 < 50000u * FM_TPU) ;
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

static const fm_par_t fm_par = {c1_run, c1_wait};
static void fm_start(void)
{
    isr_st.max_us = 0;
    fm_begin(RUN, FM_CFG, FM_EV, fm_cfg, 1);
    fm_set_par(RUN, &fm_par);
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
            fm_item(1, us2(s->worst)); fm_item(2, us2(s->w8)); fm_item(3, s->n ? us2((s->sum / s->n) << s->sum_sh) : 0u);
            fm_item(4, us2(s->raw_worst)); fm_item(5, us2(s->raw_w8));
            fm_item(6, s->hash & 16383u); fm_item(7, s->hash >> 14 & 16383u); fm_item(8, s->hash >> 28);
            if (FM_CFG[fm_cfg].kind == FM_K_LANES)
                for (i = 0; i < 4u; i++) { fm_item(10u + 2u * i, us2(s->lane_worst[i])); fm_item(11u + 2u * i, us2(s->lane_w8[i])); }
            if (FM_CFG[fm_cfg].kind == FM_K_C1 || (FM_CFG[fm_cfg].kind == FM_K_LANES && (FM_CFG[fm_cfg].type & FM_PAR))) {
                const uint32_t v[4] = {s->c1_jobs, s->c1_mis, s->c1_first, s->c1_solo};
                for (i = 0; i < 4u; i++) { fm_item(20u + 2u * i, v[i] & 16383u); fm_item(21u + 2u * i, v[i] >> 14); }
            }
            if (FM_CFG[fm_cfg].kind == FM_K_LANES && (FM_CFG[fm_cfg].type & FM_PAR)) {   /* each core's busy time */
                fm_item(30, us2(s->c0_worst)); fm_item(31, us2(s->c1_worst));
                fm_item(32, s->n ? us2((s->c0_sum / s->n) << s->sum_sh) : 0u);
                fm_item(33, s->n ? us2((s->c1_sum / s->n) << s->sum_sh) : 0u);
            }
            fm_item(99, d2);
        } else if (d1 == 26u && d2 == 0x55u) {   /* a soft reset (as the crash path's: core 1 held first) */
            APP_RESET_PREP();
            fm1_reboot();
        } else if (d1 == 25u) {                 /* core 1's fault frame (c1_regs): items 1..78 (3 each), 99 */
            for (i = 0; i < 26u; i++) {
                fm_item(1u + 3u * i, c1_regs[i] & 16383u); fm_item(2u + 3u * i, c1_regs[i] >> 14 & 16383u);
                fm_item(3u + 3u * i, c1_regs[i] >> 28);
            }
            fm_item(90, fm1_crash.magic == FM1_CRASH_MAGIC && c1_regs[26] == (C1_REGS_MAGIC ^ fm1_crash.count));
            fm_item(99, 0);
        } else if (d1 == 24u) {
            r = isr_st.max_us; isr_st.max_us = 0;
            fm_item(90, fm_state == 3u ? 1u : fm_state); fm_item(91, fm_cfg); fm_item(92, fm_done); fm_item(93, FM_NCFG);
            fm_item(94, fm_used(W(_sstack_lo), W(_sstack_top))); fm_item(95, fm_used(W(_ustack_lo), W(_ustack_top)));
            fm_item(96, r);
            {   /* the last crash record (.noinit: it outlives the reset), 32-bit values as 14 + 14 + 4 bits */
                const uint32_t cv[5] = {fm1_crash.pc, fm1_crash.rets, fm1_crash.sp, fm1_crash.dbg, fm1_crash.emu};
                fm_item(60, fm1_crash.magic == FM1_CRASH_MAGIC); fm_item(61, fm1_crash.count); fm_item(62, fm1_crash.vec);
                for (i = 0; i < 5u; i++) {
                    fm_item(63u + 3u * i, cv[i] & 16383u); fm_item(64u + 3u * i, cv[i] >> 14 & 16383u);
                    fm_item(65u + 3u * i, cv[i] >> 28);
                }
                fm_item(78, fm1_crash.uptime_ms / 100u); fm_item(79, c1_faulted);
            }
            {   /* the crash record's ICFG, PSR; core 1's sp, usp, ssp, psr as it started (40..57, 14 + 14 + 4 bits) */
                const uint32_t cv[6] = {fm1_crash.icfg, fm1_crash.psr, c1_mb.reg[0], c1_mb.reg[1], c1_mb.reg[2], c1_mb.reg[3]};
                for (i = 0; i < 6u; i++) {
                    fm_item(40u + 3u * i, cv[i] & 16383u); fm_item(41u + 3u * i, cv[i] >> 14 & 16383u);
                    fm_item(42u + 3u * i, cv[i] >> 28);
                }
            }
            fm_item(30, c1_rail_boot[0]); fm_item(31, c1_rail_boot[1]);
            fm_item(32, c1_rail(C1_P3_VDC14, 7u)); fm_item(33, c1_rail(C1_P3_SYSVDD, 15u));
            fm_item(34, (C1_PLL2_CON1 & 0xFFFu) + 2u);
#ifdef TXFM1_RAILS
            fm_item(35, 1);
#else
            fm_item(35, 0);
#endif
            fm_item(80, c1_state); fm_item(81, c1_mb.bad);
            fm_item(82, c1_jobs & 16383u); fm_item(83, c1_jobs >> 14);
            fm_item(84, c1_wait_max / C1_TPU); fm_item(85, c1_mb.bad_seq & 16383u); fm_item(86, c1_stack_used());
            fm_item(87, c1_mb.rec); fm_item(88, c1_rec0); fm_item(89, c1_ref);
            fm_item(36, c1_late); fm_item(37, c1_holds); fm_item(38, c1_retries);
            fm_item(39, c1_hsb[1] & 16383u); fm_item(58, c1_hsb[2]); fm_item(59, fm1_boot.p3_rst);
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
        c1_retry_poll(fm1_ms);                  /* (C1_RETRY only) */
        if (fm_run_bytes() <= FM_RUN_MAX) fm_midi();
        if (fm_state == 3u) fm_start();
        if (shown != (uint8_t)(fm_state + 4u * fm_done)) { shown = (uint8_t)(fm_state + 4u * fm_done); fm_show(); }
    }
}
