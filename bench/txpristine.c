/* SPDX-License-Identifier: GPL-3.0-only */
/* txpristine: the effects' pristine measurement (host). Textura's mix chain, stage by stage, on test signals: what an
 * effect adds that it should not (a staircase on quiet notes, a DC that stays, a step when a stage rests, a zipper when
 * a knob moves, a click when the limiter acts). The idea of Frodhes' pristine gate (its tools/fx_pristine.py), on
 * Textura's own stages, called directly (no engine: the signals are exact).
 *
 *   txpristine            the table
 *   txpristine --check    the table, exit 1 when a judged value passes its limit (make tx-pristine)
 *
 * Units: the drive's own grid, LSB = 2^-15 of full scale (the mix's Q23 / 256). Sines on whole hertz over 1 s windows
 * (whole cycles: each harmonic and each folded image on its own DFT bin, no window, no leakage).
 *
 *   DRIVE (the mix's pair, through tx_fx_drive_mix), each TYPE at AMOUNT 20 / 64 / 127, TONE 64, MIX 127:
 *     quiet   a 41 Hz sine of 10 / 40 / 160 / 640 LSB: the worst of H2..H5 re H1 (dB) and the DC out (LSB). The dry
 *             sine's own (its rounding to the grid) beside it. Judged: worst harmonic at 160 and 640 LSB (AMOUNT 20)
 *     loud    a 2489 Hz sine at -6 dBFS: the folded images (energy off every harmonic) re the whole (dB). Reported
 *     rest    1 s of a 100 Hz sine (3000 LSB, 20 ms fade-out), then zeros: the largest step out after the input stops
 *             (a stage dropping a held DC at once: a click) and the largest value in the last 0.5 s (a DC that stays).
 *             Judged: exact zero at the end; the step <= 2 LSB
 *     zipper  a 220 Hz sine (8000 LSB) while AMOUNT moves 20 .. 100 .. 20 by 3 a block (a modulated AMOUNT): the
 *             largest second difference at a block's edge re the largest inside the blocks (1 = none). Reported
 *     dip     KHLON only, AMOUNT 125 <-> 126 (its two modes, through the dry signal) every 40 blocks, a 345 Hz sine
 *             (2 cycles a block: each block's level steady): the largest block-to-block level jump (dB). Reported
 *   LIMITER (tx_core_limit, the pair interleaved as the mix runs it):
 *     chord   four sines (196 247 294 392 Hz) peaking 4 dB over the ceiling, 0.25 s on / 0.25 s off (5 ms edges):
 *             the energy above 5 kHz re the whole (dB; the chord has none: the gain's own steps and clicks), the
 *             largest output (never over -1 dBFS: judged)
 *     bass    a 55 Hz sine 2 dB over: H2..H9 re H1 (dB), reported
 *   COMPRESSOR (tx_core_comp at THR 40 RAT 80 (4:1), MKUP 0): a 41 Hz sine at -30 dBFS: the worst of H2..H5 re H1,
 *     reported */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tx_fx.h"
#include "tx_core_limit.h"
#include "tx_core_comp.h"

#define FS 44100
#define NB TX_BLOCK
#define LSB 256.0                                       /* Q23 per drive LSB */
static const char *const DN[TX_DRT_N] = {"FDRV", "KHLON", "TSH8", "OHCD", "BHLUE"};
static int fails, check;

/* |X(f)|^2 of x[0..n) at a whole-hertz f over n = FS samples (one bin) */
static double bin2(const double *x, int n, int f)
{
    double c = 0, s = 0, w = 2.0 * M_PI * f / FS;
    int i;
    for (i = 0; i < n; i++) { c += x[i] * cos(w * i); s += x[i] * sin(w * i); }
    return c * c + s * s;
}
static double db(double r) { return 10.0 * log10(r + 1e-30); }
static void judge(const char *what, double v, double lim, int below)
{
    int bad = below ? v > lim : v < lim;
    if (bad && check) { printf("  FAIL %s: %.2f (limit %.2f)\n", what, v, lim); fails++; }
}

/* ---- the DRIVE */
static tx_fx_t *fx;
static int32_t L[NB], R[NB];
static void drv_setup(unsigned type, unsigned amt)
{
    tx_fx_init(fx);
    tx_fx_drive_set(fx, TX_DR_TYPE, type);
    tx_fx_drive_set(fx, TX_DR_AMOUNT, amt);
    tx_fx_drive_set(fx, TX_DR_TONE, 64);
    tx_fx_drive_set(fx, TX_DR_MIX, 127);
}
/* one block of the drive: x (drive LSB, whole numbers) -> y (drive LSB, may be fractional: the mix's Q23) */
static void drv_block(const double *x, double *y)
{
    unsigned j;
    for (j = 0; j < NB; j++) L[j] = R[j] = (int32_t)lrint(x[j]) * 256;
    tx_fx_drive_mix(fx, L, R, NB);
    for (j = 0; j < NB; j++) y[j] = L[j] / LSB;
}
/* a sine of amplitude a (drive LSB, rounded to whole LSB: the grid's own) at f Hz for secs; the last second analysed:
 * the worst of H2..H5 re H1, H3 re H1, the DC, and (alias) the energy off every harmonic re the whole */
typedef struct { double hw, h3, dc, alias; } spec_t;
static spec_t drv_sine(int f, double a, double secs, int dry)
{
    int n = (int)(secs * FS), nb = (n + NB - 1) / NB, b, j, k;
    double *x = malloc(sizeof(double) * nb * NB), *y = malloc(sizeof(double) * nb * NB), *t;
    spec_t s;
    double h1, hk, tot = 0, harm = 0, dc = 0;
    for (b = 0; b < nb; b++) {
        for (j = 0; j < (int)NB; j++) x[b * NB + j] = lrint(a * sin(2.0 * M_PI * f * (double)(b * NB + j) / FS));
        if (dry) memcpy(y + b * NB, x + b * NB, sizeof(double) * NB);
        else drv_block(x + b * NB, y + b * NB);
    }
    t = y + nb * NB - FS;
    for (j = 0; j < FS; j++) dc += t[j];
    dc /= FS;
    for (j = 0; j < FS; j++) { t[j] -= dc; tot += t[j] * t[j]; }
    tot *= FS / 2.0;                                    /* (Parseval: a bin's |X|^2 = FS / 2 x its energy) */
    h1 = bin2(t, FS, f);
    s.hw = -300; s.h3 = -300;
    for (k = 2; k * f < FS / 2; k++) {
        hk = bin2(t, FS, k * f);
        harm += hk;
        if (k <= 5 && db(hk / h1) > s.hw) s.hw = db(hk / h1);
        if (k == 3) s.h3 = db(hk / h1);
        if (f > 1000 && k > 9) break;                   /* (loud: the images matter; harmonics past 9 are few) */
    }
    s.dc = dc;
    s.alias = db((tot - h1 - harm) / (tot > 0 ? tot : 1));
    free(x); free(y);
    return s;
}
static void drv_quiet(void)
{
    static const double A[4] = {10, 40, 160, 640};
    static const unsigned AM[3] = {20, 64, 127};
    unsigned t, i, q;
    spec_t s;
    printf("DRIVE quiet: a 41 Hz sine, the worst of H2..H5 re H1 dB [DC LSB], per input level (LSB)\n");
    printf("  %-14s", "");
    for (q = 0; q < 4; q++) printf("  %11.0f LSB   ", A[q]);
    printf("\n  %-14s", "dry");
    for (q = 0; q < 4; q++) { s = drv_sine(41, A[q], 1.0, 1); printf("  %6.1f [%6.2f] ", s.hw, s.dc); }
    printf("\n");
    for (t = 0; t < TX_DRT_N; t++)
        for (i = 0; i < 3; i++) {
            printf("  %-6s AMT %3u", DN[t], AM[i]);
            for (q = 0; q < 4; q++) {
                char w[64];
                drv_setup(t, AM[i]);
                s = drv_sine(41, A[q], 3.0, 0);
                printf("  %6.1f [%6.2f] ", s.hw, s.dc);
                if (AM[i] == 20 && q >= 2) {
                    snprintf(w, sizeof w, "%s AMOUNT 20 quiet %.0f LSB worst harmonic", DN[t], A[q]);
                    judge(w, s.hw, q == 2 ? -40.0 : -45.0, 1);
                }
            }
            printf("\n");
        }
}
static void drv_loud(void)
{
    unsigned t;
    printf("DRIVE loud: a 2489 Hz sine at -6 dBFS, AMOUNT 127: the folded images re the whole (dB)\n ");
    for (t = 0; t < TX_DRT_N; t++) { spec_t s; drv_setup(t, 127); s = drv_sine(2489, 16384, 2.0, 0); printf(" %s %.1f", DN[t], s.alias); }
    printf("\n");
}
static void drv_rest(void)
{
    static const unsigned AM[3] = {20, 64, 127};
    unsigned t, i;
    printf("DRIVE rest: 1 s of a 100 Hz sine, then zeros: the largest step out after it stops, the largest |out| in the "
           "last 0.5 s (LSB)\n");
    for (t = 0; t < TX_DRT_N; t++) {
        printf(" ");
        for (i = 0; i < 3; i++) {
            double x[NB], y[NB], prev = 0, step = 0, tail = 0;
            int b, j, nb = (int)(4.0 * FS / NB), on = FS / NB, k0 = (int)(0.98 * FS);
            char w[80];
            drv_setup(t, AM[i]);
            for (b = 0; b < nb; b++) {
                for (j = 0; j < (int)NB; j++) {
                    int k = b * NB + j;
                    double g = k < k0 ? 1.0 : k < FS ? 0.5 + 0.5 * cos(M_PI * (k - k0) / (FS - k0)) : 0.0;
                    x[j] = k < FS ? lrint(3000.0 * g * sin(2.0 * M_PI * 100.0 * k / FS)) : 0.0;
                }
                drv_block(x, y);
                for (j = 0; j < (int)NB; j++) {
                    int k = b * NB + j;
                    double d = fabs(y[j] - prev);
                    if (k > FS + FS / 50 && d > step) step = d;   /* (20 ms after: the tail's own decay is smooth) */
                    if (k >= nb * (int)NB - FS / 2 && fabs(y[j]) > tail) tail = fabs(y[j]);
                    prev = y[j];
                }
                (void)on;
            }
            printf(" %s/%u step %.2f end %.2f", DN[t], AM[i], step, tail);
            snprintf(w, sizeof w, "%s AMOUNT %u rest: a value left at the end", DN[t], AM[i]);
            judge(w, tail, 0.0, 1);
            snprintf(w, sizeof w, "%s AMOUNT %u rest: the step when it rests", DN[t], AM[i]);
            judge(w, step, 2.0, 1);
        }
        printf("\n");
    }
}
static void drv_zipper(void)
{
    unsigned t;
    printf("DRIVE zipper: AMOUNT 20 .. 100 .. 20 by 3 a block under a 220 Hz sine: the largest second difference at a "
           "block edge re inside (1 = none)\n ");
    for (t = 0; t < TX_DRT_N; t++) {
        double x[NB], y[NB], p1 = 0, p2 = 0, edge = 0, in = 0;
        int b, j, a = 20, da = 3;
        drv_setup(t, 20);
        for (b = 0; b < 400; b++) {
            if (b >= 100) { a += da; if (a >= 100 || a <= 20) da = -da; tx_fx_drive_set(fx, TX_DR_AMOUNT, (unsigned)a); }
            for (j = 0; j < (int)NB; j++) x[j] = lrint(8000.0 * sin(2.0 * M_PI * 220.0 * (double)(b * NB + j) / FS));
            drv_block(x, y);
            for (j = 0; j < (int)NB; j++) {
                double d2 = fabs(y[j] - 2 * p1 + p2);
                if (b >= 100) { if (j == 0) { if (d2 > edge) edge = d2; } else if (j > 1 && d2 > in) in = d2; }
                p2 = p1; p1 = y[j];
            }
        }
        printf(" %s %.2f", DN[t], edge / (in > 0 ? in : 1));
    }
    printf("\n");
}
static void drv_dip(void)
{
    double x[NB], y[NB], prev = 0, jump = 0;
    int b, j;
    drv_setup(TX_DRT_KHLON, 125);
    for (b = 0; b < 400; b++) {
        double e = 0, lv;
        if (b >= 100 && b % 40 == 0) tx_fx_drive_set(fx, TX_DR_AMOUNT, (b / 40) & 1 ? 126 : 125);
        for (j = 0; j < (int)NB; j++) x[j] = lrint(3000.0 * sin(2.0 * M_PI * 2.0 * j / NB));   /* (2 cycles a block) */
        drv_block(x, y);
        for (j = 0; j < (int)NB; j++) e += y[j] * y[j];
        lv = db(e / NB);
        if (b > 100 && fabs(lv - prev) > jump) jump = fabs(lv - prev);
        prev = lv;
    }
    printf("DRIVE KHLON dip (AMOUNT 125 <-> 126): the largest block-to-block level jump %.2f dB\n", jump);
}

/* ---- the LIMITER and the COMPRESSOR */
static void lim_chord(void)
{
    static const int F[4] = {196, 247, 294, 392};
    tx_limit_t *l = malloc(sizeof *l);
    int n = 3 * FS, i, k, nb = n / (int)NB, b;
    int32_t *v = malloc(sizeof(int32_t) * 2 * NB);
    double *o = malloc(sizeof(double) * nb * NB), pk = 0, a = 7476717.0 * pow(10.0, 4.0 / 20.0) / 4.0, hf = 0, tot = 0;
    tx_limit_init(l); l->ch = 2;
    for (b = 0; b < nb; b++) {
        for (i = 0; i < (int)NB; i++) {
            int t = b * NB + i, ph = t % (FS / 2), e = FS / 200;
            double g = ph < FS / 4 ? (ph < e ? 0.5 - 0.5 * cos(M_PI * ph / e) : ph > FS / 4 - e ? 0.5 - 0.5 * cos(M_PI * (FS / 4 - ph) / e) : 1.0) : 0.0, s = 0;
            for (k = 0; k < 4; k++) s += cos(2.0 * M_PI * F[k] * t / FS);   /* (cos: all peak together at t = 0) */
            v[2 * i] = v[2 * i + 1] = (int32_t)lrint(a * g * s);
        }
        tx_limit_process(l, v, 2 * NB);
        for (i = 0; i < (int)NB; i++) { o[b * NB + i] = v[2 * i]; if (fabs((double)v[2 * i]) > pk) pk = fabs((double)v[2 * i]); }
    }
    {   /* the energy above 5 kHz: a 1-s window, a crude DFT over the bins from 5 kHz (step 25 Hz) */
        const double *t = o + FS;
        int f;
        for (i = 0; i < FS; i++) tot += t[i] * t[i];
        tot *= FS / 2.0;
        for (f = 5000; f < FS / 2; f += 25) hf += bin2(t, FS, f) * 25.0;
    }
    printf("LIMITER chord (4 dB over, bursts): the energy above 5 kHz %.1f dB re the whole; the largest out %.3f dBFS\n",
           db(hf / tot), 20 * log10(pk / 8388608.0));
    judge("limiter chord: over the ceiling (-1 dBFS)", pk, 7476717.0, 1);
    free(l); free(v); free(o);
}
static void lim_bass(void)
{
    tx_limit_t *l = malloc(sizeof *l);
    int nb = 2 * FS / (int)NB, b, i, k;
    int32_t v[2 * NB];
    double *o = malloc(sizeof(double) * nb * NB), h1, hw = -300;
    tx_limit_init(l); l->ch = 2;
    for (b = 0; b < nb; b++) {
        for (i = 0; i < (int)NB; i++) v[2 * i] = v[2 * i + 1] = (int32_t)lrint(7476717.0 * pow(10.0, 0.1) * sin(2.0 * M_PI * 55 * (double)(b * NB + i) / FS));
        tx_limit_process(l, v, 2 * NB);
        for (i = 0; i < (int)NB; i++) o[b * NB + i] = v[2 * i];
    }
    h1 = bin2(o + nb * NB - FS, FS, 55);
    for (k = 2; k <= 9; k++) { double h = db(bin2(o + nb * NB - FS, FS, 55 * k) / h1); if (h > hw) hw = h; }
    printf("LIMITER bass (55 Hz, 2 dB over): the worst of H2..H9 re H1 %.1f dB\n", hw);
    free(l); free(o);
}
static void comp_quiet(void)
{
    tx_comp_t *c = malloc(sizeof *c);
    int nb = 4 * FS / (int)NB, b, i, k;
    int32_t a[NB], r[NB];
    double *o = malloc(sizeof(double) * nb * NB), h1, hw = -300;
    tx_comp_init(c);
    tx_comp_param(c, TX_CP_ON, 1); tx_comp_param(c, TX_CP_THR, 40); tx_comp_param(c, TX_CP_RATIO, 80); tx_comp_param(c, TX_CP_MKUP, 0);
    for (b = 0; b < nb; b++) {
        for (i = 0; i < (int)NB; i++) a[i] = r[i] = (int32_t)lrint(8388608.0 * pow(10.0, -1.5) * sin(2.0 * M_PI * 41 * (double)(b * NB + i) / FS));
        tx_comp_process(c, a, r, NB);
        for (i = 0; i < (int)NB; i++) o[b * NB + i] = a[i];
    }
    h1 = bin2(o + nb * NB - FS, FS, 41);
    for (k = 2; k <= 5; k++) { double h = db(bin2(o + nb * NB - FS, FS, 41 * k) / h1); if (h > hw) hw = h; }
    printf("COMPRESSOR (THR 40, 4:1) a 41 Hz sine at -30 dBFS: the worst of H2..H5 re H1 %.1f dB (gain reduction %.1f dB)\n",
           hw, tx_comp_gr_db10(c) / 10.0);
    free(c); free(o);
}

int main(int argc, char **argv)
{
    check = argc > 1 && !strcmp(argv[1], "--check");
    fx = calloc(1, sizeof *fx);
    if (!fx) return 2;
    drv_quiet();
    drv_loud();
    drv_rest();
    drv_zipper();
    drv_dip();
    lim_chord();
    lim_bass();
    comp_quiet();
    free(fx);
    if (check) printf("tx-pristine: %s (%d over)\n", fails ? "FAIL" : "ok", fails);
    return check && fails ? 1 : 0;
}
