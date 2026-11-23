/* SPDX-License-Identifier: GPL-3.0-only */
/* txbintest: the mix's HEADPHONES mode (tx_core_mix.h tx_mix_phones), on the pair the slots sum into (m->L, m->R:
 * before the compressor and the limiter). Host only (the checks in double: a bench tool, not an engine). make tx-test
 * runs it; exit 1 on any failed check.
 *
 *   centre    HEADPHONES at the centre = SPEAKERS, bit for bit (noise, a trim, the level and the gate moving)
 *   back      HEADPHONES, then SPEAKERS again: once the ears are back, the slot's own path, bit for bit as never left
 *   itd       a hard-left impulse: the right ear 28..30 samples late, darker (8 kHz against 200 Hz: 3 dB under the near's)
 *             and -10 dB +-2 at low frequencies; mirrored for a hard right
 *   loud      white noise at every pan 0..127: the pair's power within +-1 dB of the centre's
 *   sweep     a 440 Hz sine under pan moves (0 -> 127 -> 0 a step a block; 0 <-> 127 jumps every 20 blocks; the
 *             mode switched both ways at a hard pan): the largest |second difference| at most 2x the largest of the
 *             renders held at 0, 64 and 127. Why 2x: a click is a step, 10x or more (txfxsweep flags over 10x); the
 *             delay's glide (at most 1/16 sample a sample) shifts the pitch by up to 1.06x (the second difference
 *             1.13x), and its start and stop kink the slope by A w / 16, about the held second difference A w^2 at
 *             440 Hz. Reported, not judged: 0 <-> 127 every block (5.8 ms: no glide ever lands, each turns back,
 *             the slope kinks twice as much)
 *   mono      the mono sum (L + R) of a hard pan's impulse, 50 Hz .. 16 kHz: its notch below the speakers' hard pan
 *             (reported; fails under -6 dB) and its ripple, peak to notch (reported; fails over 8 dB) */
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
    tx_mix_init(&ma);
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
static double highs(const double *h) { return 20 * log10(mag(h, 8000) / mag(h, 200)); }   /* dB, 8 kHz vs 200 Hz */

static void t_itd(void)
{
    unsigned s;
    for (s = 0; s < 2; s++) {
        double *nr, *fr, lo;
        unsigned pn, pf;
        char what[120];
        impulse(s ? 127u : 0u, 1);
        nr = s ? irr : irl; fr = s ? irl : irr;
        pn = peak_at(nr); pf = peak_at(fr);
        lo = 20 * log10(mag(fr, 100) / mag(nr, 100));
        printf("itd %s: near ear peak at %u, far ear at %u (lag %d); far / near at 100 Hz %.2f dB; 8 kHz vs 200 Hz near %+.1f far %+.1f dB;"
               " far ear at 1, 4, 10 kHz %.1f %.1f %.1f dB (vs its 100 Hz)\n", s ? "hard right" : "hard left", pn, pf,
               (int)pf - (int)pn, lo, highs(nr), highs(fr), 20 * log10(mag(fr, 1000) / mag(fr, 100)),
               20 * log10(mag(fr, 4000) / mag(fr, 100)), 20 * log10(mag(fr, 10000) / mag(fr, 100)));
        snprintf(what, sizeof what, "%s: far ear 28..30 samples late, darker, -10 dB +-2 at 100 Hz", s ? "hard right" : "hard left");
        check(pn == 0 && pf - pn >= 28 && pf - pn <= 30 && highs(fr) < highs(nr) - 3 && lo > -12 && lo < -8, what);
    }
}

static void t_loud(void)
{
    int32_t x[TX_BLOCK], L[TX_BLOCK], R[TX_BLOCK];
    double ref = 0, lo = 1e9, hi = -1e9;
    unsigned p, b, j, plo = 0, phi = 0;
    int pass;
    for (pass = 0; pass < 2; pass++)
        for (p = pass ? 0 : 64; p < 128; p++) {
            double e = 0, db;
            tx_mix_init(&ma); tx_mix_pan(&ma, 0, p); tx_mix_phones(&ma, TX_MIX_HEADPHONES);
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
    printf("loud: white noise, the pair's power against the centre: %.2f dB (pan %u) .. %+.2f dB (pan %u)\n", lo, plo, hi, phi);
    check(lo >= -1 && hi <= 1, "loudness across pan 0..127 within +-1 dB");
}

/* the largest |second difference| of both channels over blocks b0.. of a 440 Hz sine, pan / mode moved by mv */
static double sd_run(unsigned p0, int hp, void (*mv)(unsigned b))
{
    int32_t x[TX_BLOCK], L[TX_BLOCK], R[TX_BLOCK];
    double m = 0, l1 = 0, l2 = 0, r1 = 0, r2 = 0;
    unsigned b, j, t = 0;
    tx_mix_init(&ma); tx_mix_pan(&ma, 0, p0);
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

static void t_sweep(void)
{
    double h = 0, s, j, j1, md;
    unsigned p;
    for (p = 0; p < 3; p++) { double v = sd_run(p == 0 ? 0 : p == 1 ? 64 : 127, 1, 0); if (v > h) h = v; }
    s = sd_run(0, 1, mv_sweep); j = sd_run(0, 1, mv_jump); md = sd_run(0, 0, mv_mode); j1 = sd_run(0, 1, mv_jump1);
    printf("sweep: largest |2nd difference| held %.0f; sweep %.2fx, jumps 0 <-> 127 %.2fx, mode switches %.2fx"
           " (jumps every block: %.2fx, not judged)\n", h, s / h, j / h, md / h, j1 / h);
    check(s <= 2 * h && j <= 2 * h && md <= 2 * h, "pan moves and mode switches click-free (<= 2x the held)");
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

int main(void)
{
    printf("txbintest: the mix's HEADPHONES mode\n");
    t_centre(); t_back(); t_itd(); t_loud(); t_sweep(); t_mono();
    printf("txbintest: %s (%d failed)\n", fails ? "FAIL" : "ok", fails);
    return fails ? 1 : 0;
}
