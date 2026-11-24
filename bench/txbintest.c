/* SPDX-License-Identifier: GPL-3.0-only */
/* txbintest: the mix's HEADPHONES mode (tx_core_mix.h tx_mix_phones), on the pair the slots sum into (m->L, m->R:
 * before the compressor and the limiter). Host only (the checks in double: a bench tool, not an engine). make tx-test
 * runs it; exit 1 on any failed check.
 *
 *   centre    HEADPHONES at the centre = SPEAKERS, bit for bit (noise, a trim, the level and the gate moving)
 *   back      HEADPHONES, then SPEAKERS again: once the ears are back, the slot's own path, bit for bit as never left
 *   itd       a hard-left impulse: the right ear 28..31 samples late at 50 Hz (its phase delay: the time difference lives
 *             below the far ear's crossover, its highs on time), darker (far / near 3 dB lower at 8 kHz than at 50 Hz)
 *             and -10 dB +-2 at 50 Hz; mirrored for a hard right
 *   loud      white noise at every pan 0..127: the pair's power within +-1 dB of the centre's
 *   sweep     a 440 Hz sine under pan moves (0 -> 127 -> 0 a step a block; 0 <-> 127 jumps every 20 blocks; the
 *             mode switched both ways at a hard pan): the largest |second difference| at most 2x the largest of the
 *             renders held at 0, 64 and 127. Why 2x: a click is a step, 10x or more (txfxsweep flags over 10x); the
 *             delay's glide (at most 1/16 sample a sample) shifts the pitch by up to 1.06x (the second difference
 *             1.13x), and its start and stop kink the slope by A w / 16, about the held second difference A w^2 at
 *             440 Hz. The jumps 0 <-> 127 at most 2.5x: both ears glide at once, and 440 Hz sits at a hard pan's
 *             crossover (380 Hz), where the far ear's late low band and its on-time highs meet, so its level there
 *             turns with the delay over the glide's ~10 ms (1.94x before the crossover, 2.11x with it): a swell, no
 *             step. Reported, not judged: 0 <-> 127 every block (5.8 ms: no glide ever lands, each turns back, the
 *             slope kinks twice as much)
 *   mono      the mono sum (L + R) of a hard pan's impulse, 50 Hz .. 16 kHz: its notch below the speakers' hard pan
 *             (reported; fails under -6 dB) and its ripple, peak to notch (reported; fails over 8 dB)
 *   tones     sines 200 Hz .. 8 kHz at pans 0 .. 127: L / R correlation >= 0.5, the mono sum (M against the pair's
 *             mean power) under 1.5 dB below SPEAKERS' at that pan (a delayed far ear would cancel a tone at its half
 *             cycle)
 *   amount    AMOUNT 0 (tx_mix_phones_amt) = SPEAKERS bit for bit, panned (set at the start, and glided to from 100 %:
 *             once the ears are back), and its cost 0; AMOUNT 50: a hard pan's far ear half as late (14..15 samples at
 *             50 Hz)
 *   head      each head size (tx_mix_phones_head) at a hard pan: the far ear 25, 29, 33 samples late at 50 Hz (+-1), the
 *             larger head the darker far ear (far / near at 4 kHz against at 50 Hz)
 *   loudall   the loud check at AMOUNT 0, 25, 50, 75, 100 and every head size: within +-1 dB of the centre
 *   moves     AMOUNT (0 / 50 / 100) and the head size moved every 20 blocks at a hard pan while a sine plays: <= 2x the
 *             held second difference, as sweep
 *   ref       four slots under a random scene (pans moving, OUTPUT, AMOUNT, head, levels, gates, trims, short blocks):
 *             the mix's pass-per-ear path = the one-loop reference (below), bit for bit, samples and state */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tx_core_mix.h"

static int fails;
static void check(int ok, const char *what)
{
    printf("  %-6s %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) fails++;
}

static uint32_t rng = 1;
static int32_t noise(void) { rng = rng * 1103515245u + 12345u; return (int32_t)(rng >> 9) - (1 << 22); }   /* 1/2 FS */

/* one block of x through slot 0 of m: the pair into L, R */
static void block(tx_mix_t *m, const int32_t *x, int32_t *L, int32_t *R)
{
    tx_mix_begin(m, TX_BLOCK);
    tx_mix_add(m, 0, x, TX_BLOCK);
    memcpy(L, m->L, sizeof m->L); memcpy(R, m->R, sizeof m->R);
}

static tx_mix_t ma, mb;
static unsigned amt_ = TX_MIX_AMT_DEF, head_ = TX_MIX_HEAD_MEDIUM;   /* (impulse's, t_loud's, sd_run's) */
static void setup(tx_mix_t *m) { tx_mix_phones_amt(m, amt_); tx_mix_phones_head(m, head_); }

static void t_centre(void)
{
    int32_t x[TX_BLOCK], La[TX_BLOCK], Ra[TX_BLOCK], Lb[TX_BLOCK], Rb[TX_BLOCK];
    unsigned b, j, diff = 0;
    tx_mix_init(&ma); tx_mix_init(&mb);
    tx_mix_phones(&mb, TX_MIX_HEADPHONES);
    for (b = 0; b < 200; b++) {
        if (b == 20) { tx_mix_trim(&ma, 0, 5000); tx_mix_trim(&mb, 0, 5000); }
        if (b == 40) { tx_mix_level(&ma, 0, 80); tx_mix_level(&mb, 0, 80); }
        if (b == 60) { tx_mix_gate(&ma, 0, 0); tx_mix_gate(&mb, 0, 0); }
        if (b == 70) { tx_mix_gate(&ma, 0, 1); tx_mix_gate(&mb, 0, 1); }
        if (b == 90) { tx_mix_trim(&ma, 0, 4096); tx_mix_trim(&mb, 0, 4096); tx_mix_level(&ma, 0, 100); tx_mix_level(&mb, 0, 100); }
        for (j = 0; j < TX_BLOCK; j++) x[j] = noise();
        block(&ma, x, La, Ra); block(&mb, x, Lb, Rb);
        for (j = 0; j < TX_BLOCK; j++) diff += La[j] != Lb[j] || Ra[j] != Rb[j];
    }
    printf("centre: %u samples differ\n", diff);
    check(diff == 0, "HEADPHONES at the centre = SPEAKERS, bit for bit");
}

static void t_back(void)
{
    int32_t x[TX_BLOCK], La[TX_BLOCK], Ra[TX_BLOCK], Lb[TX_BLOCK], Rb[TX_BLOCK];
    unsigned b, j, diff = 0;
    tx_mix_init(&ma); tx_mix_init(&mb);
    tx_mix_pan(&ma, 0, 20); tx_mix_pan(&mb, 0, 20);
    tx_mix_phones(&mb, TX_MIX_HEADPHONES);
    for (b = 0; b < 60; b++) {
        if (b == 30) tx_mix_phones(&mb, TX_MIX_SPEAKERS);
        for (j = 0; j < TX_BLOCK; j++) x[j] = noise();
        block(&ma, x, La, Ra); block(&mb, x, Lb, Rb);
        if (b >= 33) for (j = 0; j < TX_BLOCK; j++) diff += La[j] != Lb[j] || Ra[j] != Rb[j];
    }
    printf("back: run %u, %u samples differ after 3 blocks\n", mb.bin[0].run, diff);
    check(!mb.bin[0].run && diff == 0, "back to SPEAKERS: the speakers' path, bit for bit");
}

/* the impulse response of slot 0 at pan p, HEADPHONES (settled first), N samples */
#define IRN 1024
static double irl[IRN], irr[IRN];
static void impulse(unsigned p, int hp)
{
    int32_t x[TX_BLOCK], L[TX_BLOCK], R[TX_BLOCK];
    unsigned b, j;
    tx_mix_init(&ma); setup(&ma);
    tx_mix_pan(&ma, 0, p);
    tx_mix_phones(&ma, hp ? TX_MIX_HEADPHONES : TX_MIX_SPEAKERS);
    memset(x, 0, sizeof x);
    for (b = 0; b < 4; b++) block(&ma, x, L, R);
    for (b = 0; b < IRN / TX_BLOCK; b++) {
        for (j = 0; j < TX_BLOCK; j++) x[j] = b == 0 && j == 0 ? 1 << 22 : 0;
        block(&ma, x, L, R);
        for (j = 0; j < TX_BLOCK; j++) { irl[b * TX_BLOCK + j] = L[j] / 4194304.0; irr[b * TX_BLOCK + j] = R[j] / 4194304.0; }
    }
}
static unsigned peak_at(const double *h)
{
    unsigned j, at = 0;
    for (j = 1; j < IRN; j++) if (fabs(h[j]) > fabs(h[at])) at = j;
    return at;
}
static double mag(const double *h, double f)   /* |H(f)| */
{
    double re = 0, im = 0, w = 2 * M_PI * f / 44100.0;
    unsigned j;
    for (j = 0; j < IRN; j++) { re += h[j] * cos(w * j); im -= h[j] * sin(w * j); }
    return sqrt(re * re + im * im);
}
static double phase(const double *h, double f)   /* arg H(f), radians */
{
    double re = 0, im = 0, w = 2 * M_PI * f / 44100.0;
    unsigned j;
    for (j = 0; j < IRN; j++) { re += h[j] * cos(w * j); im -= h[j] * sin(w * j); }
    return atan2(im, re);
}
/* the far ear's time difference: its phase delay behind the near ear's at 50 Hz, samples (the time difference lives in
 * the lows: above its crossover the far ear's highs come on time, tx_core_mix.c TX_XO) */
static double lag_lo(const double *nr, const double *fr)
{
    double p = phase(nr, 50) - phase(fr, 50);
    while (p < -M_PI) p += 2 * M_PI;
    while (p > M_PI) p -= 2 * M_PI;
    return p * 44100.0 / (2 * M_PI * 50);
}

static void t_itd(void)
{
    unsigned s;
    for (s = 0; s < 2; s++) {
        double *nr, *fr, lo, lg, dk;
        unsigned pn, pf;
        char what[120];
        impulse(s ? 127u : 0u, 1);
        nr = s ? irr : irl; fr = s ? irl : irr;
        pn = peak_at(nr); pf = peak_at(fr);
        lo = 20 * log10(mag(fr, 50) / mag(nr, 50)); lg = lag_lo(nr, fr);
        dk = 20 * log10(mag(fr, 8000) / mag(nr, 8000)) - lo;   /* (the far ear's highs against its lows, the near's 0) */
        printf("itd %s: near ear peak at %u, far ear at %u; the far ear %.1f samples late at 50 Hz; far / near at 50 Hz"
               " %.2f dB, at 8 kHz %.1f dB under that; far ear at 1, 4, 10 kHz %.1f %.1f %.1f dB (vs its 100 Hz)\n",
               s ? "hard right" : "hard left", pn, pf, lg, lo, -dk, 20 * log10(mag(fr, 1000) / mag(fr, 100)),
               20 * log10(mag(fr, 4000) / mag(fr, 100)), 20 * log10(mag(fr, 10000) / mag(fr, 100)));
        snprintf(what, sizeof what, "%s: far ear 28..31 samples late at 50 Hz, darker (3 dB), -10 dB +-2 at 50 Hz", s ? "hard right" : "hard left");
        check(pn == 0 && lg >= 28 && lg <= 31 && dk < -3 && lo > -12 && lo < -8, what);
    }
}

static int t_loud(void)
{
    int32_t x[TX_BLOCK], L[TX_BLOCK], R[TX_BLOCK];
    double ref = 0, lo = 1e9, hi = -1e9;
    unsigned p, b, j, plo = 0, phi = 0;
    int pass;
    for (pass = 0; pass < 2; pass++)
        for (p = pass ? 0 : 64; p < 128; p++) {
            double e = 0, db;
            tx_mix_init(&ma); setup(&ma); tx_mix_pan(&ma, 0, p); tx_mix_phones(&ma, TX_MIX_HEADPHONES);
            rng = 7;
            for (b = 0; b < 60; b++) {
                for (j = 0; j < TX_BLOCK; j++) x[j] = noise();
                block(&ma, x, L, R);
                if (b >= 4) for (j = 0; j < TX_BLOCK; j++) e += (double)L[j] * L[j] + (double)R[j] * R[j];
            }
            if (!pass) { ref = e; break; }
            db = 10 * log10(e / ref);
            if (db < lo) lo = db, plo = p;
            if (db > hi) hi = db, phi = p;
        }
    printf("loud: AMOUNT %u %%, head %u: white noise, the pair's power against the centre: %.2f dB (pan %u) .. %+.2f dB"
           " (pan %u)\n", amt_, head_, lo, plo, hi, phi);
    return lo >= -1 && hi <= 1;
}

/* the largest |second difference| of both channels over blocks b0.. of a 440 Hz sine, pan / mode moved by mv */
static double sd_run(unsigned p0, int hp, void (*mv)(unsigned b))
{
    int32_t x[TX_BLOCK], L[TX_BLOCK], R[TX_BLOCK];
    double m = 0, l1 = 0, l2 = 0, r1 = 0, r2 = 0;
    unsigned b, j, t = 0;
    tx_mix_init(&ma); setup(&ma); tx_mix_pan(&ma, 0, p0);
    if (hp) tx_mix_phones(&ma, TX_MIX_HEADPHONES);
    for (b = 0; b < 300; b++) {
        if (mv) mv(b);
        for (j = 0; j < TX_BLOCK; j++, t++) x[j] = (int32_t)(4194304.0 * sin(2 * M_PI * 440.0 * t / 44100.0));
        block(&ma, x, L, R);
        for (j = 0; j < TX_BLOCK; j++) {
            double dl = L[j] - 2 * l1 + l2, dr = R[j] - 2 * r1 + r2;
            if (b >= 4) { if (fabs(dl) > m) m = fabs(dl); if (fabs(dr) > m) m = fabs(dr); }
            l2 = l1; l1 = L[j]; r2 = r1; r1 = R[j];
        }
    }
    return m;
}
static void mv_sweep(unsigned b) { if (b >= 10 && b < 264) tx_mix_pan(&ma, 0, b < 137 ? b - 10 : 263 - b); }
static void mv_jump(unsigned b) { if (b >= 10 && !(b % 20)) tx_mix_pan(&ma, 0, (b / 20) & 1 ? 127 : 0); }
static void mv_jump1(unsigned b) { if (b >= 10) tx_mix_pan(&ma, 0, b & 1 ? 127 : 0); }
static void mv_mode(unsigned b) { if (b >= 10 && !(b % 20)) tx_mix_phones(&ma, (b / 20) & 1 ? TX_MIX_SPEAKERS : TX_MIX_HEADPHONES); }

static void mv_amt(unsigned b) { if (b >= 10 && !(b % 20)) tx_mix_phones_amt(&ma, (b / 20) % 3 * 50u); }
static void mv_head(unsigned b) { if (b >= 10 && !(b % 20)) tx_mix_phones_head(&ma, (b / 20) % 3); }
static void mv_both(unsigned b) { if (b >= 10 && !(b % 10)) { tx_mix_phones_amt(&ma, (b / 10) % 5 * 25u); tx_mix_phones_head(&ma, (b / 10) % 3); } }

static void t_sweep(void)
{
    double h = 0, s, j, j1, md;
    unsigned p;
    for (p = 0; p < 3; p++) { double v = sd_run(p == 0 ? 0 : p == 1 ? 64 : 127, 1, 0); if (v > h) h = v; }
    s = sd_run(0, 1, mv_sweep); j = sd_run(0, 1, mv_jump); md = sd_run(0, 0, mv_mode); j1 = sd_run(0, 1, mv_jump1);
    printf("sweep: largest |2nd difference| held %.0f; sweep %.2fx, jumps 0 <-> 127 %.2fx, mode switches %.2fx"
           " (jumps every block: %.2fx, not judged)\n", h, s / h, j / h, md / h, j1 / h);
    check(s <= 2 * h && j <= 2.5 * h && md <= 2 * h, "pan moves and mode switches click-free (<= 2x the held; jumps 2.5x)");
}

static void t_mono(void)
{
    double sp[IRN], mono[IRN], f, ref, lo = 1e9, hi = -1e9, flo = 0;
    unsigned j;
    impulse(0, 0);
    for (j = 0; j < IRN; j++) sp[j] = irl[j] + irr[j];
    ref = mag(sp, 1000);
    impulse(0, 1);
    for (j = 0; j < IRN; j++) mono[j] = irl[j] + irr[j];
    for (f = 50; f < 16000; f *= 1.01) {
        double v = 20 * log10(mag(mono, f) / ref);
        if (v < lo) lo = v, flo = f;
        if (v > hi) hi = v;
    }
    printf("mono: hard left, L + R against the speakers' hard pan: %+.2f dB .. %+.2f dB, deepest notch at %.0f Hz;"
           " ripple %.2f dB\n", lo, hi, flo, hi - lo);
    check(lo > -6 && hi - lo < 8, "mono sum: notch not under -6 dB, ripple under 8 dB");
}

/* tones: a sine at each pan of 70, 80, 96, 112, 127 (and mirrored), 200 Hz .. 8 kHz: the pair's L / R correlation and
 * what the mono sum keeps of its power (M against (L^2 + R^2) / 2), against SPEAKERS' at that pan (a hard pan's mono
 * sum is 3 dB under its pair's mean there too): a tone the far ear's delay turned to the half cycle would cancel (a
 * bird's whistle at 5.5 kHz and a 4-sample delay) */
static void t_tones(void)
{
    static const unsigned P[10] = {70, 80, 96, 112, 127, 58, 48, 32, 16, 0};
    static const double F[8] = {200, 400, 750, 1100, 1500, 2700, 5500, 8000};
    int32_t x[TX_BLOCK], L[TX_BLOCK], R[TX_BLOCK];
    double wc = 2, wm = 0, fc = 0, fm = 0;
    unsigned p, f, b, j, t, pc = 0, pm = 0, hp;
    for (p = 0; p < 10; p++)
        for (f = 0; f < 8; f++) {
            double co = 1, mo = 0;
            for (hp = 0; hp < 2u; hp++) {
                double ll = 0, rr = 0, lr = 0;
                tx_mix_init(&ma); tx_mix_pan(&ma, 0, P[p]); tx_mix_phones(&ma, hp ? TX_MIX_HEADPHONES : TX_MIX_SPEAKERS);
                for (b = 0, t = 0; b < 40; b++) {
                    for (j = 0; j < TX_BLOCK; j++, t++) x[j] = (int32_t)(4194304.0 * sin(2 * M_PI * F[f] * t / 44100.0));
                    block(&ma, x, L, R);
                    if (b >= 8) for (j = 0; j < TX_BLOCK; j++) { ll += (double)L[j] * L[j]; rr += (double)R[j] * R[j]; lr += (double)L[j] * R[j]; }
                }
                if (hp) co = ll > 0 && rr > 0 ? lr / sqrt(ll * rr) : 1;
                mo += (hp ? 1 : -1) * 10 * log10((ll + rr + 2 * lr) / 2 / (ll + rr));
            }
            if (co < wc) wc = co, pc = P[p], fc = F[f];
            if (mo < wm) wm = mo, pm = P[p], fm = F[f];
        }
    printf("tones: the lowest L / R correlation %.2f (pan %u, %.0f Hz); the mono sum's largest loss %.2f dB against"
           " SPEAKERS' (pan %u, %.0f Hz)\n",
           wc, pc, fc, wm, pm, fm);
    check(wc >= 0.5 && wm > -1.5, "tones at every pan: correlation >= 0.5, the mono sum loses under 1.5 dB");
}

static void t_amount(void)
{
    int32_t x[TX_BLOCK], La[TX_BLOCK], Ra[TX_BLOCK], Lb[TX_BLOCK], Rb[TX_BLOCK];
    unsigned b, j, diff0 = 0, diff1 = 0, cost = 0, p, lag;
    char what[120];
    tx_mix_init(&ma); tx_mix_init(&mb);                      /* AMOUNT 0 from the start, panned, a trim and a level */
    for (p = 0; p < 2; p++) {
        tx_mix_pan(&ma, 0, p ? 100 : 20); tx_mix_pan(&mb, 0, p ? 100 : 20);
        tx_mix_phones_amt(&mb, 0); tx_mix_phones(&mb, TX_MIX_HEADPHONES);
        for (b = 0; b < 40; b++) {
            if (b == 10) { tx_mix_level(&ma, 0, 80); tx_mix_level(&mb, 0, 80); }
            for (j = 0; j < TX_BLOCK; j++) x[j] = noise();
            block(&ma, x, La, Ra); block(&mb, x, Lb, Rb);
            cost += tx_mix_phones_us(&mb, 0);
            for (j = 0; j < TX_BLOCK; j++) diff0 += La[j] != Lb[j] || Ra[j] != Rb[j];
        }
    }
    tx_mix_init(&ma); tx_mix_init(&mb);                      /* 100 %, then glided to 0 */
    tx_mix_pan(&ma, 0, 10); tx_mix_pan(&mb, 0, 10);
    tx_mix_phones(&mb, TX_MIX_HEADPHONES);
    for (b = 0; b < 60; b++) {
        if (b == 30) tx_mix_phones_amt(&mb, 0);
        for (j = 0; j < TX_BLOCK; j++) x[j] = noise();
        block(&ma, x, La, Ra); block(&mb, x, Lb, Rb);
        if (b >= 33) { cost += tx_mix_phones_us(&mb, 0); for (j = 0; j < TX_BLOCK; j++) diff1 += La[j] != Lb[j] || Ra[j] != Rb[j]; }
    }
    printf("amount 0: %u samples differ from SPEAKERS (set at the start), %u (glided from 100 %%, after 3 blocks); cost %u us\n",
           diff0, diff1, cost);
    check(diff0 == 0 && diff1 == 0 && cost == 0 && !mb.bin[0].run, "AMOUNT 0 = SPEAKERS, bit for bit, at no cost");
    amt_ = 50;
    impulse(0, 1);
    lag = (unsigned)(lag_lo(irl, irr) + 0.5);
    amt_ = TX_MIX_AMT_DEF;
    printf("amount 50: hard left, the far ear %u samples late (100 %%: 29)\n", lag);
    snprintf(what, sizeof what, "AMOUNT 50: the far ear half as late (14..15 samples)");
    check(lag >= 14 && lag <= 15, what);
}

static void t_head(void)
{
    static const unsigned want[3] = {25, 29, 33};
    unsigned h, lag[3], ok = 1;
    double dark[3];
    for (h = 0; h < 3; h++) {
        head_ = h;
        impulse(127, 1);
        lag[h] = (unsigned)(lag_lo(irr, irl) + 0.5);
        dark[h] = 20 * log10(mag(irl, 4000) / mag(irr, 4000)) - 20 * log10(mag(irl, 50) / mag(irr, 50));   /* (far / near
                                                 * at 4 kHz against at 50 Hz: the lows' ripple about the crossover kept out) */
        if (lag[h] + 1 < want[h] || lag[h] > want[h] + 1) ok = 0;
    }
    head_ = TX_MIX_HEAD_MEDIUM;
    printf("head: hard right, the far ear late by %u (SMALL), %u (MEDIUM), %u (LARGE) samples; at 4 kHz %.1f, %.1f, %.1f dB"
           " (far / near, vs at 50 Hz)\n", lag[0], lag[1], lag[2], dark[0], dark[1], dark[2]);
    check(ok && dark[0] > dark[1] && dark[1] > dark[2], "head sizes: 25, 29, 33 samples (+-1), the larger the darker");
}

static void t_loudall(void)
{
    unsigned a, h, ok = 1;
    for (a = 0; a <= 100; a += 25)
        for (h = 0; h < 3; h++) { amt_ = a; head_ = h; if (!t_loud()) ok = 0; }
    amt_ = TX_MIX_AMT_DEF; head_ = TX_MIX_HEAD_MEDIUM;
    check(ok, "loudness across pan within +-1 dB at every AMOUNT (0..100 by 25) and head size");
}

static void t_moves(void)
{
    double h = 0, a, s, w;
    unsigned p;
    for (p = 0; p < 3; p++) { double v = sd_run(p == 0 ? 0 : p == 1 ? 64 : 127, 1, 0); if (v > h) h = v; }
    a = sd_run(0, 1, mv_amt); s = sd_run(127, 1, mv_head); w = sd_run(0, 1, mv_both);
    printf("moves: largest |2nd difference| held %.0f; AMOUNT 0 / 50 / 100 %.2fx, head sizes %.2fx, both every 10 blocks %.2fx\n",
           h, a / h, s / h, w / h);
    check(a <= 2 * h && s <= 2 * h && w <= 2 * h, "AMOUNT and head size moves click-free (<= 2x the held)");
}

/* ---- the reference: HEADPHONES' path as one loop doing both ears a sample at a time over a ring of history (the mix's
 * first version, 29c1263 .. 4ebfc58), on the mix's own state but its ring here; tx_core_mix.c goes a pass a job (the
 * block into a scratch, then each ear), from RAM on the FM-1, and must give the same samples and leave the same state */
#define REF_N 64u
#define REF_MULQ15(e, c) (((e) >> 15) * (c) + (int32_t)((((uint32_t)(e) & 32767u) * (uint32_t)(c)) >> 15))
#define REF_CLIP(v) ((v) > (1 << 26) ? (1 << 26) : (v) < -(1 << 26) ? -(1 << 26) : (v))
static int32_t ref_r[TX_MIX_SLOTS][REF_N];
static uint32_t ref_w[TX_MIX_SLOTS];
static int32_t ref_rl[TX_MIX_SLOTS][REF_N];     /* (each slot's low band: its crossover's, tx_core_mix.c TX_XO) */
static void ref_lo(tx_mix_t *m, unsigned k, int32_t x, int32_t c, int32_t *out)   /* the slot's low band, one sample */
{
    m->bin[k].s[0] += REF_MULQ15(x - m->bin[k].s[0], c);
    m->bin[k].s[1] += REF_MULQ15(m->bin[k].s[0] - m->bin[k].s[1], c);
    *out = m->bin[k].s[1];
}
static int32_t ref_ear(const int32_t *r, const int32_t *rl, uint32_t w, int32_t d, int32_t a, int32_t b, int32_t *lp)
{
    uint32_t i = w - ((uint32_t)d >> 16);
    int32_t f = (int32_t)(((uint32_t)d & 0xFFFFu) >> 1), x0 = rl[i & (REF_N - 1u)], y, e;
    y = x0 + REF_MULQ15(rl[(i - 1u) & (REF_N - 1u)] - x0, f);
    y = r[w & (REF_N - 1u)] - rl[w & (REF_N - 1u)] + y;   /* (the highs now, the low band late) */
    y = REF_CLIP(y);
    e = y - *lp; *lp += REF_MULQ15(e, a);
    e = y - *lp;
    return *lp + REF_MULQ15(e, b);
}
static void ref_out(int32_t *o, int32_t y, int32_t g)
{
    int32_t hi = y >> 14;
    uint32_t lo = (uint32_t)y & 0x3FFFu;
    hi = hi > 131071 ? 131071 : hi < -131072 ? -131072 : hi;
    *o += hi * g + (int32_t)((lo * (uint32_t)g) >> 14);
}
static int32_t ref_trim(int32_t v, int32_t tg)
{
    if (tg == 4096) return v;
    v = REF_CLIP(v);
    return (v >> 12) * tg + (int32_t)((((uint32_t)v & 4095u) * (uint32_t)tg) >> 12);
}
static void ref_add_rest(tx_mix_t *m, unsigned k, const int32_t *x, unsigned n, unsigned f)
{
    unsigned j;
    int32_t pk = m->slot_peak[k], tg = m->tg[k], *r = ref_r[k], *nr = f ? m->L : m->R, *fr = f ? m->R : m->L;
    int32_t gn = m->bin[k].e[f ^ 1u].g >> 8, gf = m->bin[k].e[f].g >> 8, bq = m->bin[k].e[f].b >> 8;
    int32_t a = m->bin[k].e[f].a >> 8, gb = (gf * bq) >> 15, gc = (gf * (32768 - bq)) >> 15, lp = m->bin[k].e[f].lp;
    uint32_t w = ref_w[k], di = (uint32_t)m->bin[k].e[f].d >> 16;
    int32_t c = m->bin[k].c >> 8, *rl = ref_rl[k];
    for (j = 0; j < n; j++) {
        int32_t v = ref_trim(x[j], tg), av = v < 0 ? -v : v, y;
        if (av > pk) pk = av;
        ref_out(&nr[j], v, gn);
        r[++w & (REF_N - 1u)] = REF_CLIP(v);
        ref_lo(m, k, r[w & (REF_N - 1u)], c, &rl[w & (REF_N - 1u)]);
        y = r[w & (REF_N - 1u)] - rl[w & (REF_N - 1u)] + rl[(w - di) & (REF_N - 1u)];
        y = REF_CLIP(y);
        lp += REF_MULQ15(y - lp, a);
        fr[j] += (y >> 14) * gb + (int32_t)((((uint32_t)y & 0x3FFFu) * (uint32_t)gb) >> 14)
               + (lp >> 14) * gc + (int32_t)((((uint32_t)lp & 0x3FFFu) * (uint32_t)gc) >> 14);
    }
    m->slot_peak[k] = pk; ref_w[k] = w; m->bin[k].e[f].lp = lp;
}
static void ref_add_mid(tx_mix_t *m, unsigned k, const int32_t *x, unsigned n)
{
    unsigned j;
    int32_t pk = m->slot_peak[k], tg = m->tg[k], *r = ref_r[k], gl = m->bin[k].e[0].g >> 8, gr = m->bin[k].e[1].g >> 8;
    uint32_t w = ref_w[k];
    for (j = 0; j < n; j++) {
        int32_t v = ref_trim(x[j], tg), av = v < 0 ? -v : v;
        if (av > pk) pk = av;
        ref_out(&m->L[j], v, gl); ref_out(&m->R[j], v, gr);
        w++;
        r[w & (REF_N - 1u)] = ref_rl[k][w & (REF_N - 1u)] = REF_CLIP(v);   /* (the crossover at 1: the low band is the
                                                 * sample) */
    }
    m->slot_peak[k] = pk; ref_w[k] = w;
    for (j = 0; j < 2u; j++) m->bin[k].e[j].lp = m->bin[k].s[j] = r[w & (REF_N - 1u)];
}
static int32_t ref_step(int32_t now, int32_t to, unsigned n) { return n ? (to - now) / (int32_t)n : 0; }
static void ref_add_bin(tx_mix_t *m, unsigned k, const int32_t *x, unsigned n)
{
    const int32_t st = (int32_t)(32768u / TX_MIX_GATE_N);
    unsigned j, i;
    int32_t pk = m->slot_peak[k], tg = m->tg[k], g = m->gc[k], gt = m->gt[k];
    int32_t lc = m->lc[k], lv = m->lv[k], ld = n ? (lv - lc) / (int32_t)n : 0;
    int32_t dd[2], da[2], db[2], dg[2], d[2], a[2], b[2], gg[2], lp[2], *r = ref_r[k], *rl = ref_rl[k];
    int32_t c = m->bin[k].c, dc = ref_step(c, m->bin[k].ct, n);
    uint32_t w = ref_w[k];
    if (m->hp && g == 32768 && gt == 32768 && lc == 32768 && lv == 32768 && m->bin[k].c == m->bin[k].ct) {
        unsigned f = 2u;
        for (i = 0; i < 2u; i++) {
            if (m->bin[k].e[i].d != m->bin[k].e[i].dt || m->bin[k].e[i].a != m->bin[k].e[i].at
                || m->bin[k].e[i].b != m->bin[k].e[i].bt || m->bin[k].e[i].g != m->bin[k].e[i].gt) break;
            if (m->bin[k].e[i].dt || m->bin[k].e[i].at != (32768 << 8) || m->bin[k].e[i].bt != (32768 << 8)) f = i;
        }
        if (i == 2u) { if (f == 2u) ref_add_mid(m, k, x, n); else ref_add_rest(m, k, x, n, f); return; }
    }
    for (i = 0; i < 2u; i++) {
        d[i] = m->bin[k].e[i].d; a[i] = m->bin[k].e[i].a; b[i] = m->bin[k].e[i].b; gg[i] = m->bin[k].e[i].g;
        lp[i] = m->bin[k].e[i].lp;
        dd[i] = ref_step(d[i], m->bin[k].e[i].dt, n);
        dd[i] = dd[i] > 4096 ? 4096 : dd[i] < -4096 ? -4096 : dd[i];
        da[i] = ref_step(a[i], m->bin[k].e[i].at, n);
        db[i] = ref_step(b[i], m->bin[k].e[i].bt, n);
        dg[i] = ref_step(gg[i], m->bin[k].e[i].gt, n);
    }
    for (j = 0; j < n; j++) {
        int32_t v = REF_CLIP(x[j]), av, l;
        if (tg != 4096) v = (v >> 12) * tg + (int32_t)((((uint32_t)v & 4095u) * (uint32_t)tg) >> 12);
        l = j + 1u == n ? lv : lc + ld * (int32_t)(j + 1u);
        if (l != 32768) { v = REF_CLIP(v); v = (v >> 15) * l + (int32_t)((((uint32_t)v & 32767u) * (uint32_t)l) >> 15); }
        av = v < 0 ? -v : v;
        if (av > pk) pk = av;
        if (g != gt) g += g < gt ? st : -st;
        v = REF_MULQ15(v, g);
        r[++w & (REF_N - 1u)] = REF_CLIP(v);
        ref_lo(m, k, r[w & (REF_N - 1u)], c >> 8, &rl[w & (REF_N - 1u)]);
        c += dc;
        ref_out(&m->L[j], ref_ear(r, rl, w, d[0], a[0] >> 8, b[0] >> 8, &lp[0]), gg[0] >> 8);
        ref_out(&m->R[j], ref_ear(r, rl, w, d[1], a[1] >> 8, b[1] >> 8, &lp[1]), gg[1] >> 8);
        for (i = 0; i < 2u; i++) {
            a[i] += da[i]; b[i] += db[i]; gg[i] += dg[i];
            if (dd[i]) {
                d[i] += dd[i];
                if (dd[i] > 0 ? d[i] > m->bin[k].e[i].dt : d[i] < m->bin[k].e[i].dt) d[i] = m->bin[k].e[i].dt, dd[i] = 0;
            }
        }
    }
    m->slot_peak[k] = pk; m->gc[k] = g; ref_w[k] = w;
    if (n) m->lc[k] = lv;
    if (n) m->bin[k].c = m->bin[k].ct;
    i = !m->hp;
    for (j = 0; j < 2u; j++) {
        int32_t t = m->bin[k].e[j].dt;
        if (n && (t - d[j] < (int32_t)n && d[j] - t < (int32_t)n)) d[j] = t;
        if (n) { a[j] = m->bin[k].e[j].at; b[j] = m->bin[k].e[j].bt; gg[j] = m->bin[k].e[j].gt; }
        m->bin[k].e[j].d = d[j]; m->bin[k].e[j].a = a[j]; m->bin[k].e[j].b = b[j]; m->bin[k].e[j].g = gg[j];
        m->bin[k].e[j].lp = lp[j];
        if (d[j] != t || a[j] != m->bin[k].e[j].at || b[j] != m->bin[k].e[j].bt || gg[j] != m->bin[k].e[j].gt) i = 0;
    }
    if (i) m->bin[k].run = 0;
}
/* the reference's tx_mix_add: its binaural path, else the mix's own (SPEAKERS: one code) */
static void ref_add(tx_mix_t *m, unsigned k, const int32_t *x, unsigned n)
{
    if (tx_mix_binaural(m, k)) ref_add_bin(m, k, x, n);
    else tx_mix_add(m, k, x, n);
}

/* ref: four slots under a seeded random scene (pans moved and jumped, levels, gates, trims up to +6 dB, OUTPUT, AMOUNT
 * and the head size changed, short blocks, peaks over 8 full scale): the pair, each slot's peak, gate, level and ears
 * the same as the reference's, block for block */
static void t_ref(void)
{
    static int32_t x[TX_MIX_SLOTS][TX_BLOCK];
    unsigned s, b, k, j, diff = 0, blocks = 0, bin = 0;
    for (s = 0; s < 40; s++) {
        unsigned pan[TX_MIX_SLOTS] = {64, 64, 64, 64};
        rng = 7u + s;
        tx_mix_init(&ma); tx_mix_init(&mb);
        memset(ref_r, 0, sizeof ref_r); memset(ref_rl, 0, sizeof ref_rl); memset(ref_w, 0, sizeof ref_w);
        for (b = 0; b < 240; b++) {
            unsigned n = b % 11u == 5u ? (unsigned)(noise() & 255) : TX_BLOCK, e = (unsigned)noise();
#define BOTH(f, ...) do { f(&ma, __VA_ARGS__); f(&mb, __VA_ARGS__); } while (0)
            if (b == 0 || e % 19u == 1u) BOTH(tx_mix_phones, b == 0 || e % 3u ? TX_MIX_HEADPHONES : TX_MIX_SPEAKERS);
            if (e % 29u == 2u) BOTH(tx_mix_phones_amt, e % 4u ? (e >> 8) % 101u : 0u);
            if (e % 31u == 3u) BOTH(tx_mix_phones_head, (e >> 8) % 3u);
            for (k = 0; k < TX_MIX_SLOTS; k++) {
                unsigned q = (unsigned)noise() & 0xFFFFFu;
                if (s & 1u) { int p = (int)pan[k] + (int)(q % 7u) - 3; pan[k] = (unsigned)(p < 0 ? 0 : p > 127 ? 127 : p); BOTH(tx_mix_pan, k, pan[k]); }
                switch (q % 23u) {
                case 0: pan[k] = (q >> 5) % 128u; BOTH(tx_mix_pan, k, pan[k]); break;
                case 1: pan[k] = q & 32u ? 0u : 127u; BOTH(tx_mix_pan, k, pan[k]); break;
                case 2: BOTH(tx_mix_level, k, (q >> 5) % 128u); break;
                case 3: BOTH(tx_mix_gate, k, (q >> 5) & 1u); break;
                case 4: BOTH(tx_mix_trim, k, q & 32u ? 4096u : (q >> 6) % 8192u); break;
                case 5: BOTH(tx_mix_level, k, 100); break;
                default: break;
                }
                for (j = 0; j < n; j++) {
                    int32_t v = noise();
                    x[k][j] = (v & 255) == 7 ? v * 16 : (v & 31) == 3 ? v * 8 : k == 3u && (b / 10u) % 2u ? 0 : v;
                }
            }
#undef BOTH
            tx_mix_begin(&ma, n); tx_mix_begin(&mb, n);
            for (k = 0; k < TX_MIX_SLOTS; k++) {
                bin += tx_mix_binaural(&ma, k) != 0;
                ma.slot_peak[k] = mb.slot_peak[k] = 0;
                tx_mix_add(&ma, k, x[k], n); ref_add(&mb, k, x[k], n);
            }
            for (j = 0; j < n; j++) diff += ma.L[j] != mb.L[j] || ma.R[j] != mb.R[j];
            for (k = 0; k < TX_MIX_SLOTS; k++)
                diff += ma.slot_peak[k] != mb.slot_peak[k] || ma.gc[k] != mb.gc[k] || ma.lc[k] != mb.lc[k]
                      || ma.bin[k].run != mb.bin[k].run || memcmp(ma.bin[k].e, mb.bin[k].e, sizeof ma.bin[k].e) != 0
                      || ma.bin[k].c != mb.bin[k].c || ma.bin[k].s[0] != mb.bin[k].s[0] || ma.bin[k].s[1] != mb.bin[k].s[1];
            blocks++;
        }
    }
    printf("ref: %u blocks x %u slots (%u binaural), %u differ from the one-loop reference\n", blocks, TX_MIX_SLOTS, bin, diff);
    check(diff == 0 && bin > blocks, "the pass-per-ear path = the one-loop reference, bit for bit (samples and state)");
}

int main(void)
{
    printf("txbintest: the mix's HEADPHONES mode\n");
    t_centre(); t_back(); t_itd();
    check(t_loud(), "loudness across pan 0..127 within +-1 dB");
    t_sweep(); t_mono(); t_tones();
    t_amount(); t_head(); t_loudall(); t_moves(); t_ref();
    printf("txbintest: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
