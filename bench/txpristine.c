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
 *   GRAIN DELAY: FEEDBACK 20 .. 60 .. 20 by 3 a block under a 220 Hz sine (GRAIN 0, MIX 127, TIME 20; under 64,
 *     where the loop's make-up stays out): the energy above 1.5 kHz re the whole (dB; the sine and its echoes have none:
 *     the loop gain's steps; FEEDBACK held: ~-63). Reported
 *   LIMITER (tx_core_limit, the pair interleaved as the mix runs it):
 *     chord   four sines (196 247 294 392 Hz) peaking 4 dB over the ceiling, 0.25 s on / 0.25 s off (5 ms edges):
 *             the energy above 5 kHz re the whole (dB; the chord has none: the gain's own steps and clicks), the
 *             largest output (never over -1 dBFS: judged)
 *     bass    a 55 Hz sine 2 dB over: H2..H9 re H1 (dB), reported
 *   COMPRESSOR (tx_core_comp at THR 127 (-40 dBFS), RAT 3 (4:1), MKUP 0): a 41 Hz sine at -30 dBFS: the worst of H2..H5 re H1,
 *     reported
 *     chords  three chords (131 165 196, 98 123 147 196, 220 277 330 Hz; -14 dBFS a tone) held: the energy off the tones
 *             re the tones (dB; the gain riding the beats), the worst chord: at the defaults, and the median and the
 *             worst over THR 64 / 127 x RAT 1.5:1 / 4:1 / 20:1 x ATK 0 / 40 / 108 x REL 0 / 30 / 59. Reported
 *     mkup    MKUP 0 -> 127 -> 0 in one step (THR 0, a 220 Hz sine at -30 dBFS): the largest second difference within
 *             50 ms of a step re the held MKUP 127 render's largest (Frodhes' control-sweep click; 1 = none). Reported */
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

/* ---- the GRAIN DELAY: FEEDBACK moving (a modulated FEEDBACK) under a steady 220 Hz sine, GRAIN 0 (the echo alone),
 * MIX 127, TIME 20: the largest second difference at a block's edge re inside (1 = none) */
static void gd_zipper(void)
{
    tx_fx_grain_t *d = calloc(1, sizeof *d);
    int b, j, f = 20, df = 3, nb = 600;
    double *o = malloc(sizeof(double) * nb * NB), tot = 0, hf = 0;
    tx_fx_grain_init(d);
    tx_fx_grain_param(d, TX_GD_MIX, 127); tx_fx_grain_param(d, TX_GD_GRAIN, 0); tx_fx_grain_param(d, TX_GD_TIME, 20);
    tx_fx_grain_param(d, TX_GD_FEEDBACK, 20);
    for (b = 0; b < nb; b++) {
        if (b >= 200) { f += df; if (f >= 60 || f <= 20) df = -df; tx_fx_grain_param(d, TX_GD_FEEDBACK, (unsigned)f); }
        for (j = 0; j < (int)NB; j++) L[j] = R[j] = (int32_t)lrint(500000.0 * sin(2.0 * M_PI * 220.0 * (double)(b * NB + j) / FS));
        tx_fx_grain_process(d, L, R, NB);
        for (j = 0; j < (int)NB; j++) o[b * NB + j] = L[j];
    }
    {   /* the last second: the energy above 1.5 kHz re the whole (the sine and its echoes are 220 Hz) */
        const double *t = o + nb * NB - FS;
        int k;
        for (j = 0; j < FS; j++) tot += t[j] * t[j];
        tot *= FS / 2.0;
        for (k = 1500; k < FS / 2; k += 10) hf += bin2(t, FS, k) * 10.0;
    }
    printf("GRAIN DELAY zipper: FEEDBACK 20 .. 60 .. 20 by 3 a block under a 220 Hz sine: the energy above 1.5 kHz "
           "%.1f dB re the whole\n", db(hf / tot));
    free(d); free(o);
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
    tx_comp_param(c, TX_CP_ON, 1); tx_comp_param(c, TX_CP_THR, 127); tx_comp_param(c, TX_CP_RATIO, 3); tx_comp_param(c, TX_CP_MKUP, 0);
    for (b = 0; b < nb; b++) {
        for (i = 0; i < (int)NB; i++) a[i] = r[i] = (int32_t)lrint(8388608.0 * pow(10.0, -1.5) * sin(2.0 * M_PI * 41 * (double)(b * NB + i) / FS));
        tx_comp_process(c, a, r, NB);
        for (i = 0; i < (int)NB; i++) o[b * NB + i] = a[i];
    }
    h1 = bin2(o + nb * NB - FS, FS, 41);
    for (k = 2; k <= 5; k++) { double h = db(bin2(o + nb * NB - FS, FS, 41 * k) / h1); if (h > hw) hw = h; }
    printf("COMPRESSOR (THR 127, 4:1) a 41 Hz sine at -30 dBFS: the worst of H2..H5 re H1 %.1f dB (gain reduction %.1f dB)\n",
           hw, tx_comp_gr_db10(c) / 10.0);
    free(c); free(o);
}

/* chords through the compressor: the intermodulation its gain adds (the gain riding the chords' beats). Three chords
 * (131 165 196, 98 123 147 196, 220 277 330 Hz; each tone -14 dBFS) held 3 s; over the last second, the energy off the
 * tones re the tones (dB), the worst of the three. At the defaults (THR 64 RAT 0 ATK 108 REL 59), and over THR 64 / 127
 * x RAT 0 / 3 / 7 x ATK 0 / 40 / 108 x REL 0 / 30 / 59: the median and the worst */
static const int CHD[3][5] = {{3, 131, 165, 196}, {4, 98, 123, 147, 196}, {3, 220, 277, 330}};
static double comp_chord_one(const int *F, unsigned thr, unsigned rat, unsigned atk, unsigned rel, double *grd)
{
    tx_comp_t *c = malloc(sizeof *c);
    int nb = 3 * FS / (int)NB + 1, b, i, k, n0, nt = F[0];
    int32_t a[NB], r[NB];
    double *o = malloc(sizeof(double) * nb * NB), tot = 0, ton = 0, amp = 8388608.0 * pow(10.0, -14.0 / 20.0);
    const double *t;
    tx_comp_init(c);
    tx_comp_param(c, TX_CP_ON, 1); tx_comp_param(c, TX_CP_THR, thr); tx_comp_param(c, TX_CP_RATIO, rat);
    tx_comp_param(c, TX_CP_ATK, atk); tx_comp_param(c, TX_CP_REL, rel); tx_comp_param(c, TX_CP_MKUP, 0);
    for (b = 0; b < nb; b++) {
        for (i = 0; i < (int)NB; i++) {
            double s = 0, tt = (double)(b * NB + i) / FS;
            for (k = 0; k < nt; k++) s += sin(2.0 * M_PI * F[1 + k] * tt + k);
            a[i] = r[i] = (int32_t)lrint(amp * s);
        }
        tx_comp_process(c, a, r, NB);
        for (i = 0; i < (int)NB; i++) o[b * NB + i] = a[i];
    }
    n0 = nb * NB - FS; t = o + n0;
    for (i = 0; i < FS; i++) tot += t[i] * t[i];
    tot *= FS;
    for (k = 0; k < nt; k++) ton += 2.0 * bin2(t, FS, F[1 + k]);
    *grd = tx_comp_gr_db10(c) / 10.0;
    free(c); free(o);
    return db((tot - ton) / ton);
}
static double comp_chords(unsigned thr, unsigned rat, unsigned atk, unsigned rel, double *grd)
{
    double w = -300, g;
    int k;
    for (k = 0; k < 3; k++) { double v = comp_chord_one(CHD[k], thr, rat, atk, rel, &g); if (v > w) { w = v; *grd = g; } }
    return w;
}
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void comp_chord(void)
{
    static const uint8_t TH[2] = {64, 127}, RA[3] = {0, 3, 7}, AT[3] = {0, 40, 108}, RE[3] = {0, 30, 59};
    double w = -300, gw = 0, g, all[54], dv;
    int a, b, c, d, wi = 0, n = 0;
    for (a = 0; a < 2; a++) for (b = 0; b < 3; b++) for (c = 0; c < 3; c++) for (d = 0; d < 3; d++) {
        double v = comp_chords(TH[a], RA[b], AT[c], RE[d], &g);
        if (getenv("TXP_ALL")) printf("  comp chord THR %3d RAT %d ATK %3d REL %2d: %6.1f dB (GR %.1f)\n", TH[a], RA[b], AT[c], RE[d], v, g);
        all[n++] = v;
        if (v > w) { w = v; gw = g; wi = ((a * 3 + b) * 3 + c) * 3 + d; }
    }
    qsort(all, 54, sizeof all[0], dcmp);
    dv = comp_chords(64, 0, 108, 59, &g);
    printf("COMPRESSOR chords (3, each tone -14 dBFS): the intermodulation re the tones at the defaults %.1f dB (GR %.1f); "
           "over the settings median %.1f, worst %.1f (THR %d RAT %d ATK %d REL %d, GR %.1f)\n",
           dv, g, 0.5 * (all[26] + all[27]), w, TH[wi / 27], RA[wi / 9 % 3], AT[wi / 3 % 3], RE[wi % 3], gw);
}

/* MKUP jumped 0 -> 127 -> 0 (THR 0: no reduction; a 220 Hz sine at -30 dBFS): the largest second difference out
 * within 50 ms of each jump re the largest of the held render at MKUP 127 (Frodhes' control sweep "click"; 1 = none) */
static void comp_mkup(void)
{
    tx_comp_t *c = malloc(sizeof *c);
    int nb = 2 * FS / (int)NB, b, i, jb[2] = {nb / 4, 3 * nb / 4}, k;
    int32_t a[NB], r[NB];
    double *o = malloc(sizeof(double) * nb * NB), hd = 0, cl = 0;
    tx_comp_init(c);
    tx_comp_param(c, TX_CP_ON, 1); tx_comp_param(c, TX_CP_THR, 0); tx_comp_param(c, TX_CP_MKUP, 0);
    for (b = 0; b < nb; b++) {
        if (b == jb[0]) tx_comp_param(c, TX_CP_MKUP, 127);
        if (b == jb[1]) tx_comp_param(c, TX_CP_MKUP, 0);
        for (i = 0; i < (int)NB; i++) a[i] = r[i] = (int32_t)lrint(8388608.0 * pow(10.0, -1.5) * sin(2.0 * M_PI * 220 * (double)(b * NB + i) / FS));
        tx_comp_process(c, a, r, NB);
        for (i = 0; i < (int)NB; i++) o[b * NB + i] = a[i];
    }
    for (i = (jb[0] + 40) * (int)NB; i < jb[1] * (int)NB - 2; i++) { double d = fabs(o[i + 2] - 2 * o[i + 1] + o[i]); if (d > hd) hd = d; }
    for (k = 0; k < 2; k++)
        for (i = jb[k] * (int)NB - 2; i < jb[k] * (int)NB + FS / 20; i++) { double d = fabs(o[i + 2] - 2 * o[i + 1] + o[i]); if (d > cl) cl = d; }
    printf("COMPRESSOR MKUP 0 -> 127 -> 0 in one step (a 220 Hz sine): the click %.2f (re the held render's largest second difference)\n", cl / hd);
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
    gd_zipper();
    lim_chord();
    lim_bass();
    comp_quiet();
    comp_chord();
    comp_mkup();
    free(fx);
    if (check) printf("tx-pristine: %s (%d over)\n", fails ? "FAIL" : "ok", fails);
    return check && fails ? 1 : 0;
}
