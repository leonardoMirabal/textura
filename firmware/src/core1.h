/* SPDX-License-Identifier: GPL-3.0-only */
/* The second core (the AC79's core 1): start, hand it one job at a time, hold it. The bench (bench/fm1/txfm1.c) and
 * the app (textura.c: lanes.c renders some lanes' engines there) share it; include it after platform.c.
 *
 * The chip (the AC79 SDK's start sequence, as documented for the FM-1):
 *   C1_CON 0x1EEE004   bit 3 enables core 1, bit 1 holds it. Start: bit 3 set, then bit 1 cleared; hold: bit 1
 *                      set, then bit 3 cleared.
 *   0x01C7FFF8         the address core 1 starts at (it boots through the chip's ROM first: the PC limits must be
 *                      open meanwhile; the word is in the reserved top of RAM: written before it is locked).
 *   0x00010008 bit 3   held while it starts.
 *   0x1EEF300..        core 1's interrupt bank (32 words): all off.
 *   0x1EEF2D0..        core 1's EMU bank (core 0's at 0x1EEF0D0): its stack limit.
 *   It must start at boot, before the audio and the timers (started later it does not reach its entry).
 *   Holding it cuts the read it has on the bus: the chip flags an AXI read invalid a few instructions later, which
 *   the bus guard would trap on core 0, so the bus traps are off around a hold and the flag is cleared.
 *   Some units read wrong words on core 1 now and then (under load most): every job carries a check word (its
 *   function, argument and number); core 1 runs only the next job with a matching check, counts anything else,
 *   and core 0 holds it at the first bad read or a job not done in time: one core from then on.
 *
 * Core 1 waits in RAM (.ram_text: no flash fetch while idle); its jobs run in the audio interrupt, which waits for
 * them before it returns, and the flash is turned off only with the interrupts off: never under a job. */
#ifndef CORE1_H
#define CORE1_H
#include <stdint.h>

#define C1_CON (*(volatile uint32_t *)0x1EEE004u)
#define C1_GATE (*(volatile uint32_t *)0x00010008u)
#define C1_BOOT (*(volatile uint32_t *)0x01C7FFF8u)
#define C1_IRQ(i) (*(volatile uint32_t *)(0x1EEF300u + 4u * (i)))
#define C1_EMU_CON (*(volatile uint32_t *)0x1EEF2D0u)
#define C1_EMU_SSP_H (*(volatile uint32_t *)0x1EEF2D8u)
#define C1_EMU_SSP_L (*(volatile uint32_t *)0x1EEF2DCu)
#define C1_EMU_USP_H (*(volatile uint32_t *)0x1EEF2E0u)
#define C1_EMU_USP_L (*(volatile uint32_t *)0x1EEF2E4u)
#define C1_SYNC() __asm__ volatile("csync" ::: "memory")
#define C1_BUS ((0x3Fu << 16) | (0x3u << 4))   /* the bus-invalid traps (fm1_guard.h FM1_GUARD_BUS) */
#define C1_TPU 24u                               /* fm1_ticks per us */
#define C1_STACK (8192u / 4u)                    /* words: a render's deepest (the bench's engines) and room */
#define C1_RAM __attribute__((section(".ram_text.c1"), used))

/* the mailbox: core 0 writes the first line only, core 1 the second only (each core's writes on lines of its own) */
typedef struct {
    volatile uint32_t seq;                       /* core 0: the job's number (done + 1 hands it over) */
    volatile uint32_t arg;
    void (*volatile fn)(uint32_t);
    volatile uint32_t chk;                       /* C1_CHK(fn, arg, seq), written before seq */
    uint32_t pad0[12];
    volatile uint32_t alive, done;               /* core 1: it runs; the last job it finished */
    volatile uint32_t bad, bad_seq, bad_done;    /* the jobs it refused (a wrong number or check): count, last */
    volatile uint32_t reg[4];                    /* its sp, usp, ssp, psr as c1_main starts (where its stacks really are) */
    uint32_t pad1[7];
} c1_mb_t;
#define C1_CHK(fn, arg, seq) ((uint32_t)(uintptr_t)(fn) ^ (arg) ^ ((seq) * 0x9E3779B1u) ^ 0xC1C1A5A5u)

static c1_mb_t c1_mb __attribute__((aligned(64)));
/* its stacks, one block (the stack limit spans both): the user stack (the jobs), then 256 bytes of system stack (its
 * exceptions only: no interrupts on core 1) */
uint32_t c1_stk[C1_STACK + 64u] __attribute__((aligned(8), section(".nozero")));   /* (NOINIT's spare room, app.ld:
                                                 * painted by c1_start, never cleared) */
#define C1_MARK 0x43314D4Bu                      /* painted at start: how deep its stack has gone (c1_stack_used) */
enum { C1_OFF, C1_ON, C1_BAD, C1_LATE, C1_DEAF, C1_LOWV };   /* not started, running, held: a bad read / a late job / no
                                                 * answer / the supply not raised (never started) */
static uint8_t c1_state;
static uint32_t c1_wait_max, c1_jobs;

void c1_main(void);
void c1_entry(void);
/* its entry: interrupts off, its stacks, then out of the start-up state into c1_main (in RAM, never returns) */
__asm__(".section .ram_text.c1e,\"ax\",@progbits\n"
        ".balign 4\n"
        ".globl c1_entry\n"
        "c1_entry:\n"
        "\tcli\n"
        "\ticfg = 0\n"
        "\tusp = c1_stk + 8192\n"
        "\tsp = c1_stk + 8448\n"
        "\tssp = c1_stk + 8448\n"
        "\treti = c1_main\n"
        "\trti\n"
        ".text\n");

void __attribute__((section(".ram_text.c1"), used, noreturn)) c1_main(void)
{
    uint32_t done, bad_last = 0xFFFFFFFFu;
    C1_EMU_CON &= ~(1u << 3);                    /* its stack limit: its two stacks (as core 0's, fm1_guard.h) */
    C1_EMU_SSP_L = (uint32_t)(uintptr_t)(c1_stk + 64);   /* (the lowest 256 bytes: the margin below the limit) */
    C1_EMU_SSP_H = (uint32_t)(uintptr_t)(c1_stk + C1_STACK + 64u) - 1u;
    C1_EMU_USP_L = (uint32_t)(uintptr_t)(c1_stk + 64);
    C1_EMU_USP_H = (uint32_t)(uintptr_t)(c1_stk + C1_STACK + 64u) - 1u;
    C1_EMU_CON = (C1_EMU_CON & ~((1u << 2) | (0x1Fu << 16))) | (1u << 3);
    {
        uint32_t a, b, c, d;
        __asm__ volatile("%0 = sp\n\t%1 = usp\n\t%2 = ssp\n\t%3 = psr" : "=r"(a), "=r"(b), "=r"(c), "=r"(d));
        c1_mb.reg[0] = a; c1_mb.reg[1] = b; c1_mb.reg[2] = c; c1_mb.reg[3] = d;
    }
    done = c1_mb.done;
    c1_mb.alive = 1;
    C1_SYNC();
    for (;;) {
        uint32_t s = c1_mb.seq;
        C1_SYNC();
        if (s == done) continue;
        if (s == done + 1u) {
            void (*fn)(uint32_t) = c1_mb.fn;
            uint32_t a = c1_mb.arg;
            if (fn && c1_mb.chk == C1_CHK(fn, a, s)) {
                fn(a);
                done = s;                        /* (its own count, in a register: a misread done never moves it) */
                C1_SYNC();
                c1_mb.done = s;
                C1_SYNC();
                continue;
            }
        }
        if (s != bad_last) {                     /* a number that is not the next job, or a check that fails: once
                                                  * per value (it stays until core 0 holds it) */
            bad_last = s;
            c1_mb.bad_seq = s; c1_mb.bad_done = done;
            C1_SYNC();
            c1_mb.bad++;
            C1_SYNC();
        }
    }
}

/* hold core 1: registers only (also before the RAM is set up, and when it never ran) */
static void c1_hold(void)
{
    uint32_t locked = !(FM1_DBG_WR_EN & 1u), en;
    volatile uint32_t k;
    if (locked) FM1_DBG_WR_EN = 0xE7u;
    en = FM1_DBG_EN;
    FM1_DBG_EN = en & ~C1_BUS;                   /* (the hold's cut read: not a trap on core 0) */
    C1_SYNC();
    C1_CON |= 0x2u;
    C1_CON &= ~0x8u;
    C1_SYNC();
    for (k = 0; k < 256u; k++) ;                 /* (the flag comes a few instructions after the hold) */
    FM1_DBG_MSG_CLR = C1_BUS;
    C1_SYNC();
    FM1_DBG_EN = en;
    if (locked && (FM1_DBG_WR_EN & 1u)) FM1_DBG_WR_EN = 0xE7u;
    C1_SYNC();
    if (c1_state == C1_ON) c1_state = C1_OFF;
}

/* the core supply (P33 analog registers, the AC79 SDK's p33.h names): VDC14 (P3_ANA_CON6 [2:0], 1.25 V + 50 mV a
 * level) feeds SYSVDD (P3_ANA_CON9 [3:0], the core and SRAM rail, 0.93 V + 30 mV a level). The boot loader leaves
 * 3 (1.40 V) and 11 (1.26 V); M-VAVE's own firmware runs 7 (1.60 V) and 15 (1.38 V). TXFM1_RAILS (the T1 bench image)
 * or C1_RAILS (the app): raised to those at boot, VDC14 first, a level per 100 us, never lowered; without it, read only (T0). Also the SYS
 * PLL's multiplier (PLL2_CON1 [11:0] + 2: 270 = 360 MHz), read only */
#define C1_P3_VDC14 0x06u
#define C1_P3_SYSVDD 0x09u
#define C1_PLL2_CON1 (*(volatile uint32_t *)0x119ACu)
static uint8_t c1_rail_boot[2], c1_rail_set[2];  /* VDC14, SYSVDD: as the boot loader left them, after c1_rails */
static uint32_t c1_rail(uint32_t reg, uint32_t mask)   /* interrupts off around it: P33 access is not reentrant */
{
    uint32_t v;
    fm1_irq_off();
    v = fm1_p33_read(reg) & mask;
    fm1_irq_on();
    return v;
}
#if defined(TXFM1_RAILS) || defined(C1_RAILS)
static void c1_rail_up(uint32_t reg, uint32_t mask, uint32_t level)
{
    uint32_t r = fm1_p33_read(reg);
    while ((r & mask) < level) {
        r = (r & ~mask) | ((r & mask) + 1u);
        fm1_p33_write(reg, (uint8_t)r);
        fm1_delay_us(100);
        r = fm1_p33_read(reg);
    }
}
#endif
static void c1_rails(void)                       /* at boot, core 1 held */
{
    fm1_irq_off();
    c1_rail_boot[0] = (uint8_t)(fm1_p33_read(C1_P3_VDC14) & 7u);
    c1_rail_boot[1] = (uint8_t)(fm1_p33_read(C1_P3_SYSVDD) & 15u);
#if defined(TXFM1_RAILS) || defined(C1_RAILS)
    c1_rail_up(C1_P3_VDC14, 7u, 7u);
    c1_rail_up(C1_P3_SYSVDD, 15u, 15u);
#endif
    c1_rail_set[0] = (uint8_t)(fm1_p33_read(C1_P3_VDC14) & 7u);
    c1_rail_set[1] = (uint8_t)(fm1_p33_read(C1_P3_SYSVDD) & 15u);
    fm1_irq_on();
}

/* the AXI bus traps (DBG_EN bits 4, 5) off while core 1 runs: they are not per core (device run c1e: core 1's bad
 * read raised bit 4, core 0 took the trap too, mid-way through core 1's report: an instant reset, the report lost).
 * Core 1's own traps (bits 16..21) stay; when it is held for good they come back */
static void c1_axi(int on)
{
    fm1__dbg_unlock();
    if (on) FM1_DBG_EN |= 0x30u; else FM1_DBG_EN &= ~0x30u;
    fm1__dbg_lock();
}

/* start core 1 (at boot, interrupts off, before the top of RAM is locked): 0 = it answered */
static int c1_start(void)
{
    uint32_t g, t0, i;
    c1_hold();
    if (c1_rail_set[0] < 7u || c1_rail_set[1] < 15u) { c1_state = C1_LOWV; return -1; }   /* (the boot loader's levels:
                                                 * core 1 misreads there, a run hangs; it stays off, one core) */
    for (i = 0; i < C1_STACK; i++) c1_stk[i] = C1_MARK;
    c1_mb.alive = 0; c1_mb.bad = 0;
    c1_mb.done = c1_mb.seq;
    c1_mb.fn = 0;
    C1_SYNC();
    for (i = 0; i < 32u; i++) C1_IRQ(i) = 0;
    fm1__dbg_unlock();                           /* the PC limits open: it starts in the chip's ROM */
    FM1_PC_LIMIT0_L = 0u; FM1_PC_LIMIT0_H = 0xFFFFFFFFu;
    FM1_PC_LIMIT1_L = 0u; FM1_PC_LIMIT1_H = 0xFFFFFFFFu;
    fm1__dbg_lock();
    C1_BOOT = (uint32_t)(uintptr_t)&c1_entry;
    g = C1_GATE;
    C1_GATE = g | 0x8u;
    C1_SYNC();
    C1_CON |= 0x8u;
    C1_CON &= ~0x2u;
    t0 = fm1_ticks();
    while (!c1_mb.alive && fm1_ticks() - t0 < 20000u * C1_TPU) C1_SYNC();   /* 20 ms */
    C1_GATE = g;
    fm1_guard_enable(FM1_GUARD_PC);              /* the PC limits as before (XIP and .ram_text) */
    if (!c1_mb.alive) { c1_hold(); c1_state = C1_DEAF; return -1; }
    c1_axi(0);
    c1_state = C1_ON;
    return 0;
}

/* hand core 1 fn(arg): 1 = taken (c1_wait before touching what it works on), 0 = core 1 is not running */
static int c1_run(void (*fn)(uint32_t), uint32_t arg)
{
    uint32_t s;
    if (c1_state != C1_ON) return 0;
    s = c1_mb.seq + 1u;
    c1_mb.fn = fn;
    c1_mb.arg = arg;
    c1_mb.chk = C1_CHK(fn, arg, s);
    C1_SYNC();
    c1_mb.seq = s;
    C1_SYNC();
    return 1;
}

/* wait for the job: 0 = done; -1 = a bad read or not done in us: core 1 held from now on (the caller redoes it) */
static int c1_wait_us(uint32_t us)
{
    uint32_t t0 = fm1_ticks();
    for (;;) {
        C1_SYNC();
        if (c1_mb.bad) { c1_hold(); c1_axi(1); c1_state = C1_BAD; return -1; }
        if (c1_mb.done == c1_mb.seq) {
            uint32_t t = fm1_ticks() - t0;
            if (t > c1_wait_max) c1_wait_max = t;
            c1_jobs++;
            return 0;
        }
        if (fm1_ticks() - t0 > us * C1_TPU) { c1_hold(); c1_axi(1); c1_state = C1_LATE; return -1; }
    }
}

static int c1_wait(void) { return c1_wait_us(4000u); }   /* (the bench's: 4 ms) */

/* a fault on core 1 (APP_FAULT, from fm1_fault: the hal's handler runs on the core that faulted and has filled
 * fm1_crash): no reset from here (core 1 would hold itself mid-report, or reset the FM-1 under core 0); it stops here,
 * core 0's wait runs out and holds it (C1_LATE), and the bench or the app goes on with one core. A fault of core 0 returns */
static volatile uint32_t c1_faulted;
#define C1_REGS_MAGIC 0xC1F4A17Eu
static volatile uint32_t c1_regs[27] __attribute__((section(".noinit")));   /* (it outlives a reset) [26]: the magic;
                                                  * its fault frame (fm1_vec.S: r0..r15, reti, rete, retx, rets, psr,
                                                  * icfg, usp, ssp, sp, the interrupted rets) */
static void c1_fault(const void *v)
{
    const fm1_crash_t *c = (const fm1_crash_t *)v;
    /* which core: core 1 runs with ICFG 0 (c1_entry), core 0 with bit 8 set (fm1_irq_init); its sp may not tell
     * (device run c1c: a fault frame with sp in core 0's system stack but 900 bytes deeper than core 0 ever goes) */
    if ((c->icfg & 0x100u) && (c->sp < (uint32_t)(uintptr_t)c1_stk || c->sp > (uint32_t)(uintptr_t)(c1_stk + C1_STACK + 64u)))
        return;
    {   /* the frame sits just below its sp at the fault (f[24]; f[25], the stub's push, at that address) */
        const uint32_t *f = (const uint32_t *)(uintptr_t)(c->sp - 100u);
        uint32_t i;
        if (c->sp >= (uint32_t)(uintptr_t)c1_stk + 100u && c->sp <= (uint32_t)(uintptr_t)(c1_stk + C1_STACK + 64u))
            for (i = 0; i < 26u; i++) c1_regs[i] = f[i];
        c1_regs[26] = C1_REGS_MAGIC ^ c->count;   /* (tied to this crash record: a later one, core 0's, unties it) */
        C1_SYNC();
    }
    c1_faulted = 1;
    C1_SYNC();
    for (;;) ;
}

/* bytes of core 1's user stack ever used (painted by c1_start, at boot) */
static uint32_t c1_stack_used(void)
{
    uint32_t i = 0;
    while (i < C1_STACK && c1_stk[i] == C1_MARK) i++;
    return (C1_STACK - i) * 4u;
}

#endif
