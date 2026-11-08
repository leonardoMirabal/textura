/* SPDX-License-Identifier: GPL-3.0-only */
/* Frodhes tempo and scales (IMPLEMENTATION-v3 §11), main loop only.
 *
 * Tempo: bpm10 (40.0 .. 240.0 BPM) in the settings; PLAY taps it (the last 4 taps; from the 3rd; 2 s resets);
 * one sync table in 1/96-quarter units drives the delay time and the tremolo rate (published as volatiles the
 * ISR reads). Time base: fm1_ms (32-bit, wraps after 49 days; compared by difference).
 *
 * Textura (from Frodhes v0.9.5's tempo.c): the tempo, TAP and the MIDI clock in as there; no scales, no arpeggiator
 * or sequencer, and no LFOs to lock yet (they come with the modulation: Frodhes' tempo_lock_poll then). Added:
 * - the CLOCK modes: INT (the tempo is the BPM and the taps; a clock in is ignored), MASTER (as INT, and the clock sent
 *   out over USB when CLK OUT is USB: 24 pulses a beat from the main loop, each on its 1 ms tick, late ones caught up
 *   in order, never dropped; START at the transport's start, its first pulse one pulse later; STOP at its stop; a
 *   clock in is ignored, so one echoed back cannot drive it), FOLLOW (the clock's tempo only: the BPM and the taps change nothing, and
 *   without a clock the transport is stopped), AUTO (the clock's while one comes, else the BPM's; the default);
 * - each port's clock in on / off (CLK IN USB, CLK IN TRS): an off port's F8 FA FB FC F2 are dropped;
 * - the transport: PLAY held (ui.c) starts / stops it, START re-phases the beat; a clock's Start / Continue / Stop
 *   move it while that clock leads, and it goes on as it was when the clock goes;
 * - the grain delay's SYNC: its echo a division of the beat (halved until it fits the line: tm_gsync_half);
 * - SCENE TEMPO: a scene's load sets the tempo it was saved with (off: the tempo stays; scenes.c).
 * The settings record keeps CLOCK, the ports, CLK OUT, SCENE TEMPO and the BPM (scenes.c). */

static const uint16_t SYNC_UNITS[15] = {0, 12, 16, 24, 32, 36, 48, 64, 72, 96, 144, 192, 384, 768, 1536};
static const char *const SYNC_NAMES[15] = {"FREE", "1/32", "1/16T", "1/16", "1/8T", "1/16D", "1/8", "1/4T", "1/8D",
                                           "1/4", "1/4D", "1/2", "1 BAR", "2 BAR", "4 BAR"};
enum { CLK_INT, CLK_MASTER, CLK_FOLLOW, CLK_AUTO };
static struct {
    uint16_t bpm10;                             /* the BPM (the knob's and the taps'), 400 .. 2400 */
    uint8_t clock, in_usb, in_trs;              /* CLOCK mode; the ports' clock in */
    uint8_t run;                                /* the transport (the FM-1's own, and a clock's last word) */
    uint8_t gsync;                              /* the grain delay's SYNC (an index into SYNC_UNITS; 0 FREE) */
    uint32_t gsync_smp, gsync_sent;             /* its echo, samples; as sent to the chain (0xFFFFFFFF: never) */
    uint8_t gsync_half;                         /* halved to fit the line */
    uint8_t out_usb;                            /* CLK OUT: USB (MASTER sends the clock) */
    uint8_t scn_tempo;                          /* SCENE TEMPO: a scene's load sets its tempo */
} tm = {1200, CLK_AUTO, 1, 1, 0, 0, 0, 0xFFFFFFFFu, 0, 1, 0};
static uint32_t tap_t[4], tap_n, beat_t0, beat_ms = 500;
static uint32_t beat_k0;                     /* the pulse count (24 a beat) at beat_t0: the bars for the LFOs' lock */
static uint32_t beat_smp = 22050;                                            /* samples a beat (the clock's, exact) */

static uint32_t beat_samples(void) { return beat_smp; }   /* 26460000 / bpm10 (fs * 60 * 10 / bpm10), or the clock's */
static int tm_ext(void) { return tm.clock == CLK_FOLLOW || tm.clock == CLK_AUTO; }   /* a clock in may lead */

static void tempo_sync_publish(void)
{
    uint32_t g = tm.gsync, s = 0;
    tm.gsync_half = 0;
    if (g) {
        s = beat_samples() * SYNC_UNITS[g] / 96u;
        while (s > TX_FX_GD_TIME[TX_GD_TIME_MAX]) { s >>= 1; tm.gsync_half = 1; }   /* (TIME's top: 2.00 s) */
    }
    tm.gsync_smp = s;
    if (s != tm.gsync_sent && !ln_gsync(s)) tm.gsync_sent = s;   /* (a full queue: tempo_poll sends it again) */
}

static uint8_t clk_owns;                         /* the tempo is the clock's (tempo_bpm10 shows clk_bpm10, not the setting) */
static uint16_t clk_bpm10;
static uint32_t tempo_bpm10(void) { return clk_owns ? clk_bpm10 : tm.bpm10; }   /* the BPM shown (ui.c) */
static int clk_follows(void);
static int tempo_set10(uint32_t b)              /* 1: taken */
{
    if (tm.clock == CLK_FOLLOW || clk_follows()) return 0;   /* a clock leads: its tempo stays (no jump) */
    if ((int32_t)b < 400) b = 400;
    if (b > 2400) b = 2400;
    if (clk_owns) { clk_owns = 0; beat_ms = 0; }   /* the FM-1's own tempo again */
    if (b == tm.bpm10 && beat_ms == 600000u / b) return 1;
    tm.bpm10 = (uint16_t)b;
    beat_ms = 600000u / tm.bpm10;
    beat_smp = 26460000u / tm.bpm10;
    tempo_sync_publish();
    return 1;
}

/* a tap (PLAY's press): 1 when it set the tempo. tempo_tap_undo takes the last one back (PLAY held: the transport) */
static struct { uint32_t t[4], n, t0, k0; uint16_t bpm10; uint8_t ok; } tap_was;
static int tempo_tap(void)
{
    uint32_t now = fm1_ms, i;
    if (tm.clock == CLK_FOLLOW || clk_follows()) return 0;   /* (a clock leads: the taps change nothing) */
    for (i = 0; i < 4u; i++) tap_was.t[i] = tap_t[i];
    tap_was.n = tap_n; tap_was.t0 = beat_t0; tap_was.k0 = beat_k0; tap_was.bpm10 = tm.bpm10; tap_was.ok = 1;
    if (tap_n && now - tap_t[tap_n - 1u] > 2000u) tap_n = 0;
    if (tap_n == 4u) { tap_t[0] = tap_t[1]; tap_t[1] = tap_t[2]; tap_t[2] = tap_t[3]; tap_n = 3; }
    tap_t[tap_n++] = now;
    if (tap_n >= 3u && tap_t[tap_n - 1u] != tap_t[0]) {
        tempo_set10(600000u * (tap_n - 1u) / (tap_t[tap_n - 1u] - tap_t[0]));
        beat_t0 = now; beat_k0 = 0;                                          /* re-phase on the tap (bar 1 here) */
        return 1;
    }
    return 0;
}
static void tempo_tap_undo(void)
{
    uint32_t i;
    if (!tap_was.ok) return;
    tap_was.ok = 0;
    for (i = 0; i < 4u; i++) tap_t[i] = tap_was.t[i];
    tap_n = tap_was.n; beat_t0 = tap_was.t0; beat_k0 = tap_was.k0;
    if (tap_was.bpm10 != tm.bpm10 && !clk_owns) tempo_set10(tap_was.bpm10);
}

/* MIDI clock in (CLOCK EXT; USB-MIDI and the TRS jack alike), 0.9.5. Each clock / transport message carries its
 * arrival time and a count of the clock pulses received (usb.c / midi_uart.c, in the timer ISR: 12 bits of fm1_ms and
 * 4 bits of count in the packet's free bytes), so a busy main loop, a USB packet of several pulses or a full ring
 * (a pulse dropped) do not matter. 0.9.4 and the 0.9.5 candidates timed the pulses when the main loop read them and
 * reset the count on any interval under 8 ms: pulses read together (every screen draw) kept the tempo from ever
 * updating, and nothing locked the LFOs, the arpeggiator or the sequencer to the clock.
 * - The tempo: the time over the last 24..96 pulses (1..4 beats), smoothed over beats (1/8 a pulse) and published
 *   (the delay time, the LFOs' increments, the BPM shown) on a beat when it moved by over 0.1 %: the stamps' jitter
 *   (the timer's polls wait for the audio ISR, up to ~5 ms) does not move the delay. A tempo change: three pulses in a
 *   row whose last beat differs from the estimate by over 8 % (one late pulse is not one); the history restarts
 *   from its last 8 pulses and the tempo is published at once. 40..240 BPM (the BPM card's); outside, not followed.
 *   A gap over 200 ms restarts the history; 250 ms without pulses ends the lock and the tempo goes on as it was.
 * - The beat grid: pulse clk_gk at clk_gt (Q8 ms), each pulse moving it 1/8 of the way to its stamp. Start (FA)
 *   makes the next pulse pulse 0 (beat 1 of bar 1), Continue (FB) goes on from the song position, Stop (FC) stops
 *   its count, Song Position (F2) sets it. */
#define CLK_N 96u
static uint32_t clk_hist[CLK_N + 1u], clk_h, clk_hw;  /* the last CLK_N + 1 stamps (Q8 ms): a ring, clk_h of them valid */
static uint32_t clk_last, clk_last_ms, clk_b8, clk_pub, clk_k, clk_gk, clk_gt;   /* clk_b8: the beat (Q8 ms),
                                                        * smoothed; clk_pub: as published */
static uint8_t clk_lock, clk_fa, clk_chg, clk_dev, clk_stop = 1, clk_wait;   /* (clk_stop: no Start yet) */
static uint32_t clk_sp, clk_wait_t;   /* the song position (pulses from Start, counted while not stopped); Start /
                                       * Continue's time while they wait for the first pulse (clk_wait) */
static uint8_t clk_src, clk_cnt[3], clk_cnt_ok[3];    /* the source followed (1 USB, 2 TRS), its pulse counts */
static uint32_t clk_src_t;                             /* its last clock or transport message (ms) */
static int32_t clk_trend;                              /* pulses in a row the tempo moved one way (a ramp) */
static uint8_t clk_oor;                                /* beats in a row out of 40..240 BPM */

static uint32_t clk_ago(uint32_t j) { return clk_hist[(clk_hw + 2u * (CLK_N + 1u) - 1u - j) % (CLK_N + 1u)]; }
static void clk_push(uint32_t t)
{
    clk_hist[clk_hw] = t;
    clk_hw = (clk_hw + 1u) % (CLK_N + 1u);
    if (clk_h < CLK_N + 1u) clk_h++;
}

static uint32_t tempo_pubs;                             /* (the host tests: how often the clock's tempo was published) */
static void tempo_clock_set(uint32_t b8)                /* publish a beat (Q8 ms): the delay, the BPM shown */
{
    uint32_t bpm100 = 1536000000u / b8, b = (bpm100 + 5u) / 10u;   /* 60000 * 256 * 100 / b8 */
    b = b < 400u ? 400u : b > 2400u ? 2400u : b;
    clk_pub = b8;
    beat_ms = (b8 + 128u) >> 8;
    beat_smp = b8 * 441u / 2560u;                       /* 44.1 samples a ms */
    if (!clk_owns || (b != clk_bpm10 && (bpm100 + 7u < clk_bpm10 * 10u || bpm100 > clk_bpm10 * 10u + 7u)))
        clk_bpm10 = (uint16_t)b;                        /* (0.1 BPM, with hysteresis: no redraw for nothing) */
    clk_owns = 1;
    tempo_pubs++;
    tempo_sync_publish();
}

static int clk_port_on(int src) { return src == 2 ? tm.in_trs : tm.in_usb; }

static void tempo_spp(uint32_t pos, int src)            /* Song Position (F2): 16ths from the start */
{
    if (!tm_ext() || !clk_port_on(src) || !clk_stop) return;   /* (only while stopped, as the MIDI spec has it) */
    if (clk_src && src != clk_src && fm1_ms - clk_src_t < 500u) return;   /* (the source followed only) */
    clk_sp = pos * 6u;                                  /* (the song only: the beat grid goes on with the clock until
                                                         * Continue) */
}

static int tempo_clock(uint8_t st, uint32_t stamp, int src)   /* 1: a clock or transport message (taken or dropped) */
{
    uint32_t now = fm1_ms, t, dt, n = 1, cnt = stamp >> 12 & 15u, j, ne;
    if (st != 0xF8u && st != 0xFAu && st != 0xFBu && st != 0xFCu) return 0;
    if (src < 1 || src > 2) src = 1;
    if (st == 0xF8u && clk_cnt_ok[src]) n = (cnt - clk_cnt[src]) & 15u;   /* the pulses since the last one (> 1: the
                                                         * ring was full and dropped some) */
    clk_cnt[src] = (uint8_t)cnt; clk_cnt_ok[src] = 1;   /* (every source's count kept, followed or not, INT too) */
    if (!tm_ext() || !clk_port_on(src)) return 1;       /* (INT, MASTER: a clock in is ignored, and so a computer
                                                         * that echoes the clock back drives nothing; a port off) */
    if (clk_src && src != clk_src && now - clk_src_t < 500u) return 1;   /* two clocks: the first one, until it is
                                                                          * quiet for 500 ms */
    if (clk_src != src) { clk_h = 0; clk_lock = 0; }   /* (another source: its own history) */
    clk_src = (uint8_t)src;
    clk_src_t = now;
    if (st == 0xFAu) { clk_k = 0; clk_sp = 0; clk_stop = 0; clk_fa = 1; clk_wait = 1; clk_wait_t = now; beat_t0 = now; beat_k0 = 0; tm.run = 1; return 1; }
    if (st == 0xFBu) { clk_k = clk_sp; clk_stop = 0; clk_fa = 1; clk_wait = 1; clk_wait_t = now; tm.run = 1; return 1; }   /* Continue:
                                                         * from the song position (Stop's, or a Song Position's) */
    if (st == 0xFCu) { clk_stop = 1; tm.run = 0; return 1; }
    if (!n) n = 1;
    t = (now - ((now - stamp) & 0xFFFu)) << 8 | 128u;   /* its arrival (12 bits of fm1_ms), mid-millisecond, Q8 */
    dt = t - clk_last;
    clk_last = t;
    clk_last_ms = now;
    if (clk_lock && clk_b8 && clk_h && !clk_fa && dt < (2000u << 8)) {   /* the tempo known (not right after Start /
                                                         * Continue): the pulses the time says */
        ne = (dt * 24u + clk_b8 / 2u) / clk_b8;
        if (n > 1u) {                                   /* (the count wraps at 16: the time picks the right lap) */
            int32_t lap = ((int32_t)ne - (int32_t)n + 8) >> 4;
            if (lap > 0) n += 16u * (uint32_t)lap;
        } else if (!clk_stop && dt > (100u << 8) && ne >= 5u) n = ne;   /* (not while stopped: the song does not move)
                                                         * a gap with nothing received (the line, the computer): its
                                                         * pulses were sent all the same */
    }
    if (!clk_h || dt > (200u << 8) * n) { clk_h = 0; clk_lock = 0; clk_chg = 0; clk_dev = 0; clk_oor = 0; n = 1; }
    for (j = 1; j < n && j < 2u * CLK_N; j++) clk_push(t - dt + dt / n * j);   /* (the missing pulses: in between) */
    clk_push(t);
    if (clk_h >= 25u) clk_chg = 0;
    if (clk_h >= 25u || (clk_chg && clk_h >= 4u)) {
        uint32_t m = clk_h - 1u, bl = (t - clk_ago(m)) * 24u / m, was = clk_lock, chg = 0;
        {   /* out of range: the last beat, or the last 3 pulses (+-30 % for their jitter): not followed, held */
            uint32_t b24 = clk_h >= 25u ? t - clk_ago(24) : 0u, b3 = (t - clk_ago(3)) * 8u;
            if ((b24 && (b24 < (249u << 8) || b24 > (1506u << 8))) || b3 < (174u << 8) || b3 > (1958u << 8)) {
                if (++clk_oor >= 3u) { clk_h = 0; clk_lock = 0; clk_oor = 0; }
            } else clk_oor = 0;
        }
        if (clk_h >= 25u && clk_b8) {
            uint32_t b24 = t - clk_ago(24);
            if (dt / n >= clk_b8 / 72u) {               /* (a pulse that came with others in a burst: not judged) */
                int32_t tr = b24 > clk_b8 + clk_b8 / 50u ? 1 : b24 + clk_b8 / 50u < clk_b8 ? -1 : 0;
                if (b24 > clk_b8 + clk_b8 / 12u || b24 + clk_b8 / 12u < clk_b8) clk_dev++;
                else clk_dev = 0;
                clk_trend = tr && (tr > 0) == (clk_trend > 0) ? clk_trend + tr : tr;   /* a ramp: the last beat 2 %
                                                         * off the same way for 12 pulses: the history is that beat */
                if (clk_trend >= 12 || clk_trend <= -12) { clk_h = 25u; m = 24u; bl = b24; clk_trend = 0; chg = 1; }
            }
            if (clk_dev >= 3u) {                        /* a tempo change: from its last 3 pulses on (after it) */
                clk_h = 4u; clk_chg = 1; clk_dev = 0; chg = 1;
                bl = (t - clk_ago(3)) * 8u;
            }
        }
        if (!clk_h) {
            /* (just found out of range: nothing published) */
        } else if (clk_oor) {
            /* (an out-of-range beat: wait and see) */
        } else if (bl >= (249u << 8) && bl <= (1506u << 8)) {        /* 240 .. 40 BPM (the BPM card's; +-0.4 %) */
            if (!was || chg || !clk_b8 || clk_chg) clk_b8 = bl;  /* (new, or changed: at once) */
            else clk_b8 += (uint32_t)(((int32_t)(bl - clk_b8)) / 8);   /* (else over beats) */
            clk_lock = 1;
            if (!was) clk_gk = clk_k - 1u, clk_gt = t - clk_b8 / 24u;    /* (the grid from here: snapped below) */
            if (!was || chg || !clk_owns ||
                (clk_chg && (clk_k % 6u == 0u || clk_b8 > clk_pub + clk_pub / 100u || clk_b8 + clk_pub / 100u < clk_pub)))
                tempo_clock_set(clk_b8);                /* (after a change: every 6 pulses, or on a 1 % move) */
        } else clk_lock = 0;
    }
    clk_k += n - 1u;
    if (!clk_stop) clk_sp += n - 1u;
    if (clk_b8) {                                       /* the grid */
        uint32_t dk = clk_k - clk_gk, pred = clk_gt + dk * clk_b8 / 24u;
        int32_t err = (int32_t)(t - pred);
        if (clk_fa || dk > 96u || err > (int32_t)(clk_b8 / 6u) || -err > (int32_t)(clk_b8 / 6u)) clk_gt = t;
        else clk_gt = pred + (uint32_t)(err / 8);
        clk_gk = clk_k;
        if (clk_k % 24u == 0u && clk_lock) {            /* a beat: the LED, the internal grid after */
            beat_t0 = now - (uint32_t)((int32_t)((now << 8) - clk_gt) >> 8);   /* (Q8 differences: no wrap) */
            beat_k0 = clk_k;
            if (!clk_oor && (clk_b8 > clk_pub + clk_pub / 1024u || clk_b8 + clk_pub / 1024u < clk_pub)) tempo_clock_set(clk_b8);
        }
    }
    clk_fa = 0;
    clk_wait = 0;
    clk_k++;
    if (!clk_stop) clk_sp++;
    return 1;
}

static int clk_live(void)                               /* pulses coming, or Start / Continue waiting for the first */
{
    if (!tm_ext()) return 0;
    if (clk_wait && fm1_ms - clk_wait_t <= 100u) return 1;   /* (Start / Continue wait for the next pulse; without
                                                             * a clock they go on their own after 100 ms) */
    return clk_last_ms && fm1_ms - clk_last_ms <= 250u && !clk_wait;
}
static int clk_follows(void) { return clk_lock && clk_live(); }   /* a clock's tempo leads now */
static void tempo_clock_int(void)                       /* the BPM's tempo again (a mode without a clock in) */
{
    if (!clk_owns) return;
    clk_owns = 0; clk_lock = 0;
    beat_ms = 600000u / tm.bpm10;
    beat_smp = 26460000u / tm.bpm10;
    tempo_sync_publish();
}

/* ---- the transport and the settings (the TEMPO and MIDI pages, params.c) */
static int tm_running(void)
{
    if (clk_follows()) return !clk_stop;
    if (tm.clock == CLK_FOLLOW) return 0;               /* (FOLLOW without a clock: stopped) */
    return tm.run;
}
static int tempo_transport(int on)                      /* PLAY held: 1 taken; 0 a clock leads (it moves it) */
{
    if (tm.clock == CLK_FOLLOW || clk_follows()) return 0;
    tm.run = (uint8_t)(on != 0);
    if (on) { beat_t0 = fm1_ms; beat_k0 = 0; }          /* START: bar 1 from here */
    return 1;
}
static void tempo_mode(uint32_t m)
{
    tm.clock = (uint8_t)(m > CLK_AUTO ? CLK_AUTO : m);
    if (!tm_ext()) { clk_lock = 0; clk_src = 0; tempo_clock_int(); }
}
static void tempo_port(int src, int on)
{
    if (src == 2) tm.in_trs = (uint8_t)(on != 0); else tm.in_usb = (uint8_t)(on != 0);
    if (!on && clk_src == src) { clk_lock = 0; clk_src = 0; tempo_clock_int(); }
}
static void tempo_out(uint32_t on) { tm.out_usb = (uint8_t)(on != 0); }
static void tempo_scene_flag(uint32_t on) { tm.scn_tempo = (uint8_t)(on != 0); }
static void tempo_gsync(uint32_t g) { tm.gsync = (uint8_t)(g > 14u ? 14u : g); tempo_sync_publish(); }
/* the state of the clock, as the TEMPO page's STATUS writes it: "INT", "MASTER", "USB 120.0", "TRS 98.4", "WAITING" */
static void tempo_status(char *t)
{
    uint32_t b = tempo_bpm10();
    char n[8], d[2] = {0, 0};
    if (clk_follows()) {
        p_strcpy(t, clk_src == 2 ? "TRS " : "USB ", 4);
        p_fmt_int(n, (int32_t)(b / 10u)); p_cat(t, n, 15); p_cat(t, ".", 15);
        d[0] = (char)('0' + b % 10u); p_cat(t, d, 15);
        return;
    }
    p_strcpy(t, tm.clock == CLK_INT ? "INT" : tm.clock == CLK_MASTER ? "MASTER" : tm.clock == CLK_FOLLOW ? "WAITING" : "INT", 8);
}
/* every main-loop pass: a clock gone hands the tempo back (AUTO: the BPM's tempo; FOLLOW: it holds), and the delay's
 * echo the queue could not take is sent again */
/* the clock out (MASTER, CLK OUT USB): 24 pulses a beat at the BPM, each sent on its ms (Q8 kept, so the beat is exact
 * on average), at most CLKO_BURST a pass when the loop was held up (none dropped); START / STOP as the transport moves */
#define CLKO_BURST 4u
static uint8_t clko_on, clko_run;
static uint32_t clko_next, clko_sent;           /* the next pulse (ms, Q8); pulses sent (the host tests) */
static void tempo_clock_out(void)
{
    uint32_t per = 6400000u / tm.bpm10, now = fm1_ms << 8, n = 0;   /* a pulse: 60000 / 24 / BPM ms, Q8 */
    if (tm.clock != CLK_MASTER || !tm.out_usb) {
        if (clko_on && clko_run) midi_send(0xFC, 0, 0);   /* (stopped: whoever follows stops too) */
        clko_on = 0; clko_run = 0;
        return;
    }
    if (!clko_on) { clko_on = 1; clko_run = 0; clko_next = now + per; }
    if (tm.run != clko_run) {
        clko_run = tm.run;
        midi_send(clko_run ? 0xFA : 0xFC, 0, 0);
        if (clko_run) clko_next = (beat_t0 << 8) + per;   /* (the first pulse one pulse after START: the downbeat) */
    }
    while ((int32_t)(now - clko_next) >= 0 && n < CLKO_BURST) { midi_send(0xF8, 0, 0); clko_next += per; clko_sent++; n++; }
}
static void tempo_poll(void)
{
    tempo_clock_out();
    if (clk_owns && !clk_live() && tm.clock == CLK_AUTO && clk_last_ms && fm1_ms - clk_last_ms > 2000u) {
        /* (AUTO: the clock's last tempo stays 2 s, then the BPM's again) */
        clk_owns = 0;
        beat_ms = 600000u / tm.bpm10;
        beat_smp = 26460000u / tm.bpm10;
        tempo_sync_publish();
    }
    if (tm.gsync_smp != tm.gsync_sent && !ln_gsync(tm.gsync_smp)) tm.gsync_sent = tm.gsync_smp;
}

/* the beat light (PLAY's LED): on for 1/6 of a beat while the transport runs or the delay is synced; a tap's flash */
static uint32_t tempo_beat_led(void)
{
    uint32_t ph = (fm1_ms - beat_t0) % (beat_ms ? beat_ms : 1u);
    if (tap_n && fm1_ms - tap_t[tap_n - 1u] < 80u) return 1;
    if (!tm_running() && !tm.gsync) return 0;
    return ph < beat_ms / 6u;
}
static const char *tempo_div_name(uint32_t g) { return SYNC_NAMES[g < 15u ? g : 0u]; }
static uint32_t tm_get(uint32_t what)           /* (params.c) */
{
    switch (what) {
    case TG_GSYNC: return tm.gsync;
    case TG_HALF: return tm.gsync_half;
    case TG_CLOCK: return tm.clock;
    case TG_INUSB: return tm.in_usb;
    case TG_INTRS: return tm.in_trs;
    case TG_OUT: return tm.out_usb;
    case TG_SCNTEMPO: return tm.scn_tempo;
    }
    return 0;
}
