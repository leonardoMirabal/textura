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
 *             largest output (never over -2 dBFS: judged)
 *     bass    a 55 Hz sine 2 dB over: H2..H9 re H1 (dB), reported
 *   COMPRESSOR (tx_core_comp at THR 127 (-40 dBFS), RAT 3 (4:1), MKUP 0): a 41 Hz sine at -30 dBFS: the worst of H2..H5 re H1,
 *     reported
 *     chords  three chords (131 165 196, 98 123 147 196, 220 277 330 Hz; -14 dBFS a tone) held: the energy off the tones
 *             re the tones (dB; the gain riding the beats), the worst chord: at the defaults, and the median and the
 *             worst over THR 64 / 127 x RAT 1.5:1 / 4:1 / 20:1 x ATK 0 / 40 / 108 x REL 0 / 30 / 59; and at 4:1 ATK 40
 *             REL 48 (THR -40 / -30 dB); there and at the defaults also off a band of +-15 Hz round each tone (Frodhes'
 *             measure, tools/dry_dist.py: the gain's slow drift inside the bands). Reported
 *     mkup    MKUP 0 -> 127 -> 0 in one step (THR 0, a 220 Hz sine at -30 dBFS): the largest second difference within
 *             50 ms of a step re the held MKUP 127 render's largest (Frodhes' control-sweep click; 1 = none). Reported
 *     strike  a struck chord (-2.5 dBFS, down to -10.5 dB in ~120 ms, held): the largest sample re COMP off (never
 *             over: judged) over THR -30 / -40 dB x 4:1 / 20:1 x ATK 40 / 100; the fatness (body - strike, on less off)
 *             at -40 dB 20:1; the body's level re COMP off over THR (4:1, ATK 40). MKUP 0
 *     sweep   THR -40 -> 0 dB under a held chord, in one step and swept: the gain's dip under its settled ends (a step:
 *             judged under 0.2 dB) or under the gains held at the THR values passed (swept: reported), the statics
 *             and their largest rise as THR goes down (judged under 0.1 dB)
 *   REVERB PLATE: MOD 127 against MOD 0 (MIX 127, DECAY 64) on a chord's tail: the energy in 2 .. 12 kHz re the whole
 *     (a grain from the modulated reads shows as more of it), the largest second difference re MOD 0's (Frodhes' d2: a
 *     jumping read is a burst of it) and the tail's level. Reported
 *   REVERB TYPE change under a held chord (MIX 127): the largest second difference in the 200 ms after the change re
 *     the dry chord's (a step into the new TYPE's empty lines shows as a burst of it). Reported
 *   REVERB quiet: a 331 Hz sine of 8 / 32 / 128 Q15 steps held through HALL, PLATE, ROOM, INFINITE (MIX 127): the worst
 *     of H2..H6 re H1 in its second second (the tanks are linear: the harmonics are their rounding's). Reported
 *   REVERB quiet tail: a chord at -40 / -60 dBFS a tone against the same at -30 through HALL, PLATE, ROOM, INFINITE
 *     (MIX 127): the quiet tail scaled up less the loud, re the loud, over the tail's first second (the rounding's
 *     grain on a quiet note), and the -60 tail's level at its end re the loud. Reported
 *   REVERB SIZE steps under a held chord (MIX 127): SIZE stepped and back; the largest second difference in the 1.2 s
 *     after a step re the held tail before and after (Frodhes' control click; 1 = none). Reported
 *   REVERB MOD 0 <-> 127 under a held chord: the same measure, HALL, ROOM, INFINITE, PLATE; 2 at most */
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

/* DRIVE on and off (the control sweep: AMOUNT 0 -> 1 rose 4.7 dB for ~40 ms: the 46 ms on-glide went through MIX's
 * law, the dry whole while the wet came in, and at a low AMOUNT the two are one signal): AMOUNT 0 -> 1 at block 100,
 * 1 -> 0 at block 200, every TYPE, a steady 2-cycle sine; 128-sample windows from the switch over 100 ms against the
 * louder of the levels before and after it settled: never more than 1 dB over */
static double drv_onoff1(unsigned type, unsigned amt)
{
    double x[NB], y[NB], lv[300 * 2], w = -100;
    int b, j, s;
    drv_setup(type, 0);
    for (b = 0; b < 300; b++) {
        if (b == 100) tx_fx_drive_set(fx, TX_DR_AMOUNT, amt);
        if (b == 200) tx_fx_drive_set(fx, TX_DR_AMOUNT, 0);
        for (j = 0; j < (int)NB; j++) x[j] = lrint(3000.0 * sin(2.0 * M_PI * 2.0 * j / NB));
        drv_block(x, y);
        for (s = 0; s < 2; s++) {
            double e = 0;
            for (j = 0; j < (int)NB / 2; j++) e += y[s * NB / 2 + j] * y[s * NB / 2 + j];
            lv[b * 2 + s] = db(e / (NB / 2) + 1e-9);
        }
    }
    for (s = 100; s <= 200; s += 100) {
        double ref = fmax(lv[(s - 5) * 2], lv[(s + 40) * 2]);
        for (j = s * 2; j < s * 2 + 36; j++) if (lv[j] - ref > w) w = lv[j] - ref;
    }
    return w;
}
static void drv_onoff(void)
{
    static const char *const N[TX_DRT_N] = {"FDRV", "KHLON", "TSH8", "OHCD", "BHLUE"};
    double w = -100, v[TX_DRT_N][2];
    unsigned t;
    for (t = 0; t < TX_DRT_N; t++) {
        v[t][0] = drv_onoff1(t, 1); v[t][1] = drv_onoff1(t, 64);
        if (fmax(v[t][0], v[t][1]) > w) w = fmax(v[t][0], v[t][1]);
    }
    printf("DRIVE on / off (AMOUNT 0 <-> 1, 0 <-> 64) under a steady sine: the level over the louder settled side, dB:");
    for (t = 0; t < TX_DRT_N; t++) printf(" %s %+.2f / %+.2f", N[t], v[t][0], v[t][1]);
    printf(" (1 at most)\n");
    judge("DRIVE on / off level", w, 1.0, 1);
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
    double *o = malloc(sizeof(double) * nb * NB), pk = 0, a = 6663308.0 * pow(10.0, 4.0 / 20.0) / 4.0, hf = 0, tot = 0;
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
    judge("limiter chord: over the ceiling (-2 dBFS)", pk, 6663308.0, 1);
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
        for (i = 0; i < (int)NB; i++) v[2 * i] = v[2 * i + 1] = (int32_t)lrint(6663308.0 * pow(10.0, 0.1) * sin(2.0 * M_PI * 55 * (double)(b * NB + i) / FS));
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
static double cc_band;                         /* the last chords' worst energy outside +-15 Hz of the tones re the tones (dB) */
static double comp_chord_one(const int *F, unsigned thr, unsigned rat, unsigned atk, unsigned rel, double *grd, double *band)
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
    {   /* Frodhes' measure (tools/dry_dist.py): off a band of +-15 Hz round each tone (the gain's slow drift inside it) */
        double inb = 0;
        int f;
        for (k = 0; k < nt; k++) for (f = F[1 + k] - 15; f <= F[1 + k] + 15; f++) inb += 2.0 * bin2(t, FS, f);
        *band = db((tot - inb) / ton);
    }
    *grd = tx_comp_gr_db10(c) / 10.0;
    free(c); free(o);
    return db((tot - ton) / ton);
}
static double comp_chords(unsigned thr, unsigned rat, unsigned atk, unsigned rel, double *grd)
{
    double w = -300, g, bd;
    int k;
    cc_band = -300;
    for (k = 0; k < 3; k++) {
        double v = comp_chord_one(CHD[k], thr, rat, atk, rel, &g, &bd);
        if (v > w) { w = v; *grd = g; }
        if (bd > cc_band) cc_band = bd;
    }
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
    printf("COMPRESSOR chords (3, each tone -14 dBFS): the intermodulation re the tones at the defaults %.1f dB (+-15 Hz: %.1f; "
           "GR %.1f); over the settings median %.1f, worst %.1f (THR %d RAT %d ATK %d REL %d, GR %.1f)\n",
           dv, cc_band, g, 0.5 * (all[26] + all[27]), w, TH[wi / 27], RA[wi / 9 % 3], AT[wi / 3 % 3], RE[wi % 3], gw);
    dv = comp_chords(127, 3, 40, 48, &g);
    printf("COMPRESSOR chords at 4:1 ATK 40 REL 48: THR -40 dB %.1f dB (+-15 Hz: %.1f; GR %.1f)", dv, cc_band, g);
    dv = comp_chords(96, 3, 40, 48, &g);
    printf(", THR -30 dB %.1f dB (+-15 Hz: %.1f; GR %.1f)\n", dv, cc_band, g);
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

/* the compressor over x (Q23, both sides alike) into y, nb blocks; pv: ON THR ATK REL RAT MKUP; from block b2 THR
 * goes to thr2, in one step (ramp 0) or by one a block (ramp 1) */
static void comp_run(const double *x, double *y, int nb, const unsigned *pv, int b2, unsigned thr2, int ramp)
{
    tx_comp_t *c = malloc(sizeof *c);
    int32_t a[NB], r[NB];
    int b, i;
    unsigned k, thr = pv[1];
    tx_comp_init(c);
    for (k = 0; k < TX_CP_NP; k++) tx_comp_param(c, k, pv[k]);
    for (b = 0; b < nb; b++) {
        if (b >= b2 && thr != thr2) { thr = ramp ? (thr2 > thr ? thr + 1 : thr - 1) : thr2; tx_comp_param(c, TX_CP_THR, thr); }
        for (i = 0; i < (int)NB; i++) a[i] = r[i] = (int32_t)lrint(x[b * NB + i]);
        tx_comp_process(c, a, r, NB);
        for (i = 0; i < (int)NB; i++) y[b * NB + i] = a[i];
    }
    free(c);
}
static double rms_db(const double *x, int n) { double e = 0; int i; for (i = 0; i < n; i++) e += x[i] * x[i]; return db(e / n); }

/* a struck chord (131 165 196 Hz, each tone 0.25 of full scale: -2.5 dBFS at its peak) after 0.1 s of silence: 2 ms
 * on, then down to 0.3 of the strike (-10.5 dB) with a 120 ms time constant, held to 2 s. The strike: the first 50 ms;
 * the body: 0.8 .. 1.8 s. Fatness: (body - strike) with COMP on less with it off (dB; up: the body brought up against
 * the strike, Frodhes' measure); the body's level re COMP off (MKUP 0: what the make-up gives back); the largest
 * sample re COMP off's (never over: judged) */
#define CS_NB (2 * FS / (int)NB + 1)
static double *cs_x;
static void comp_strike1(const unsigned *pv, double *fat, double *body, double *pk)
{
    double *y = malloc(sizeof(double) * CS_NB * NB), px = 0, py = 0;
    int i, n0 = FS / 10, n1 = n0 + FS / 20, n2 = n0 + 8 * FS / 10, n3 = n0 + 18 * FS / 10;
    if (!cs_x) {
        cs_x = malloc(sizeof(double) * CS_NB * NB);
        for (i = 0; i < CS_NB * (int)NB; i++) {
            double t = (double)(i - n0) / FS, e = t < 0 ? 0 : (t < 0.002 ? t / 0.002 : 1) * (0.3 + 0.7 * exp(-t / 0.12));
            cs_x[i] = e < 1e-9 ? 0 : lrint(8388608.0 * 0.25 * e * (sin(2 * M_PI * 131 * t) + sin(2 * M_PI * 165 * t + 1) + sin(2 * M_PI * 196 * t + 2)));
        }
    }
    comp_run(cs_x, y, CS_NB, pv, CS_NB, pv[1], 0);
    for (i = 0; i < CS_NB * (int)NB; i++) { if (fabs(cs_x[i]) > px) px = fabs(cs_x[i]); if (fabs(y[i]) > py) py = fabs(y[i]); }
    *fat = (rms_db(y + n2, n3 - n2) - rms_db(y + n0, n1 - n0)) - (rms_db(cs_x + n2, n3 - n2) - rms_db(cs_x + n0, n1 - n0));
    *body = rms_db(y + n2, n3 - n2) - rms_db(cs_x + n2, n3 - n2);
    *pk = 20.0 * log10(py / px);
    free(y);
}
static void comp_strike(void)
{
    static const uint8_t TH[2] = {95, 127}, RA[2] = {3, 7}, AT[2] = {40, 100}, TB[6] = {56, 64, 80, 96, 112, 127};
    double fat, body, pk, pw = -300;
    int a, b, c, wa = 0, wb = 0, wc = 0;
    for (a = 0; a < 2; a++) for (b = 0; b < 2; b++) for (c = 0; c < 2; c++) {
        unsigned pv[TX_CP_NP] = {1, TH[a], AT[c], 59, RA[b], 0};
        comp_strike1(pv, &fat, &body, &pk);
        if (getenv("TXP_ALL")) printf("  comp strike THR %3d RAT %d ATK %3d: fatness %+.2f dB, body %+.2f dB, peak %+.2f dB\n", TH[a], RA[b], AT[c], fat, body, pk);
        if (pk > pw) { pw = pk; wa = a; wb = b; wc = c; }
    }
    printf("COMPRESSOR struck chord (MKUP 0): the largest sample re COMP off %+.2f dB at worst (THR %d RAT %d ATK %d; "
           "THR -30 / -40 dB x 4:1 / 20:1 x ATK 40 / 100)\n", pw, TH[wa], RA[wb], AT[wc]);
    judge("compressor struck chord: a peak over COMP off (dB)", pw, 0.01, 1);
    printf("COMPRESSOR fatness (body - strike, COMP on less off; THR -40 dB, 20:1, MKUP 0):");
    for (c = 1; c >= 0; c--) {
        unsigned pv[TX_CP_NP] = {1, 127, AT[c], 59, 7, 0};
        comp_strike1(pv, &fat, &body, &pk);
        printf(" ATK %d %+.2f dB (body %+.2f)%s", AT[c], fat, body, c ? "," : "\n");
    }
    printf("COMPRESSOR the body re COMP off over THR (4:1, ATK 40, MKUP 0):");
    for (a = 0; a < 6; a++) {
        unsigned pv[TX_CP_NP] = {1, TB[a], 40, 59, 3, 0};
        comp_strike1(pv, &fat, &body, &pk);
        printf(" %d %+.2f%s", TB[a], body, a < 5 ? "," : " dB\n");
    }
}

/* THR swept under a held chord (131 165 196 Hz, 4:1, ATK 40, REL 59 / 127, MKUP 0): from -40 dB to 0 at 1.5 s, in one
 * step or by one a block (0.74 s). The gain: 23 ms windows, out re in (dB). A step: never under both its settled ends
 * (Frodhes' control sweep: the auto make-up dropping at once while the reduction smoothed away, a dip). Swept: never
 * under the gains held at the THR values it passed (the statics, 1 s each). The dip: how far under (dB, 0: none; the
 * step's judged; swept, the reduction's smoothed release lags its falling target: reported). The statics beside it,
 * and their largest rise: THR going down never brings the level up (judged under 0.1 dB; past THR 64 the auto make-up
 * and the lift give back at most down to THR 64's reduction) */
static double comp_gain_held(const double *x, int nb, unsigned *pv)
{
    double *y = malloc(sizeof(double) * nb * NB), g;
    comp_run(x, y, nb, pv, nb, pv[1], 0);
    g = rms_db(y + (nb * (int)NB - 4096), 4096) - rms_db(x + (nb * (int)NB - 4096), 4096);
    free(y);
    return g;
}
static void comp_sweep(void)
{
    static const double LV[2] = {0.4, 0.2};                 /* -8 / -14 dBFS a tone */
    int nb = 4 * FS / (int)NB, b2 = 3 * FS / 2 / (int)NB, i, w, k, v, nw = nb * (int)NB / 1024, ns = FS / (int)NB;
    double *x = malloc(sizeof(double) * nb * NB), *y = malloc(sizeof(double) * nb * NB), dw = 0, st[128], rw = 0;
    for (v = 0; v < 2; v++) {
        double smin = 300, smax = -300;
        for (i = 0; i < nb * (int)NB; i++) {
            double t = (double)i / FS;
            x[i] = lrint(8388608.0 * LV[v] * (sin(2 * M_PI * 131 * t) + sin(2 * M_PI * 165 * t + 1) + sin(2 * M_PI * 196 * t + 2)));
        }
        printf("COMPRESSOR THR -40 -> 0 dB under a held chord (%s dBFS a tone): the dip", v ? "-14" : "-8");
        for (k = 0; k < 4; k++) {
            unsigned pv[TX_CP_NP] = {1, 127, 40, k & 1 ? 127 : 59, 3, 0};
            double lo = 300, d = 0;
            for (i = 0; i < 128 && !k; i++) {                   /* (the statics: REL does not move them) */
                pv[1] = (unsigned)i; st[i] = comp_gain_held(x, ns, pv);
                if (st[i] < smin) smin = st[i];
                if (st[i] > smax) smax = st[i];
            }
            pv[1] = 127;
            comp_run(x, y, nb, pv, b2, 0, k >> 1);
            if (!(k >> 1)) {                                  /* a step: under both settled ends */
                double g0 = rms_db(y + (b2 * (int)NB - 4096), 4096) - rms_db(x + (b2 * (int)NB - 4096), 4096);
                double g1 = rms_db(y + (nb * (int)NB - 4096), 4096) - rms_db(x + (nb * (int)NB - 4096), 4096);
                for (w = b2 * (int)NB / 1024; w < nw; w++) { double g = rms_db(y + w * 1024, 1024) - rms_db(x + w * 1024, 1024); if (g < lo) lo = g; }
                d = (g0 < g1 ? g0 : g1) - lo;
            } else                                            /* swept: under the statics it passed */
                for (w = b2 * (int)NB / 1024; w < nw; w++) {
                    int bk = w * 1024 / (int)NB - b2, t = bk < 0 ? 127 : 127 - (bk < 127 ? bk : 127), u;
                    double g = rms_db(y + w * 1024, 1024) - rms_db(x + w * 1024, 1024), m = 300;
                    for (u = t; u <= 127; u++) if (st[u] < m) m = st[u];
                    if (m - g > d) d = m - g;
                }
            d = d < 0 ? 0 : d;
            if (!(k >> 1) && d > dw) dw = d;
            printf(" %s REL %d %.2f dB%s", k >> 1 ? "swept" : "step", k & 1 ? 127 : 59, d, k < 3 ? "," : "");
        }
        {   /* the statics' rise: THR going down never brings the level up (the largest rise over a lower level before) */
            double lo = st[0], ru = 0;
            for (i = 1; i < 128; i++) { if (st[i] - lo > ru) ru = st[i] - lo; if (st[i] < lo) lo = st[i]; }
            if (ru > rw) rw = ru;
            printf("; the statics %+.2f .. %+.2f dB, THR 0 / 64 / 88 / 127: %+.2f / %+.2f / %+.2f / %+.2f, the largest rise %.2f dB\n",
                   smin, smax, st[0], st[64], st[88], st[127], ru);
        }
    }
    judge("compressor THR step: a dip (dB)", dw, 0.2, 1);
    judge("compressor statics: the level up as THR goes down (dB)", rw, 0.1, 1);
    free(x); free(y);
}

/* REVERB PLATE, MOD 0 against 127 (MIX 127, DECAY 64): a chord (196 247 294 392 Hz, 0.3 s, 5 ms edges), then its tail.
 * Over the tail's 1 s from 0.5 s: the largest second difference, the energy in 2 .. 12 kHz re the whole (dB) and the
 * whole's level.
 * MOD 127 should sound the same tail, only moving: a grain (the modulated reads' jumps) shows as more 2 .. 12 kHz */
static void rev_plate_mod1(unsigned mod, double *hf, double *lv, double *d2)
{
    static const int F[4] = {196, 247, 294, 392};
    tx_fx_reverb_t *r = malloc(sizeof *r);
    int nb = 2 * FS / (int)NB, b, i, k, f;
    int32_t a[NB], c[NB];
    double *o = malloc(sizeof(double) * nb * NB), tot = 0, h = 0;
    const double *t;
    tx_fx_reverb_init(r);
    tx_fx_reverb_param(r, TX_RV_TYPE, TX_RVT_PLATE); tx_fx_reverb_param(r, TX_RV_MIX, 127);
    tx_fx_reverb_param(r, TX_RV_DECAY, 64); tx_fx_reverb_param(r, TX_RV_MOD, mod);
    for (b = 0; b < nb; b++) {
        for (i = 0; i < (int)NB; i++) {
            int n = b * NB + i, e = FS / 200, len = 3 * FS / 10;
            double g = n >= len ? 0 : n < e ? 0.5 - 0.5 * cos(M_PI * n / e) : n > len - e ? 0.5 - 0.5 * cos(M_PI * (len - n) / e) : 1, s = 0;
            for (k = 0; k < 4; k++) s += sin(2.0 * M_PI * F[k] * n / FS + k);
            a[i] = c[i] = (int32_t)lrint(8388608.0 * 0.12 * g * s);
        }
        tx_fx_reverb_process(r, a, c, NB);
        for (i = 0; i < (int)NB; i++) o[b * NB + i] = a[i];
    }
    t = o + FS / 2;
    for (i = 0; i < FS; i++) tot += t[i] * t[i];
    for (f = 2000; f <= 12000; f += 25) h += bin2(t, FS, f) * 25.0;
    *hf = db(h / (tot * FS / 2.0));
    *lv = tot;
    for (*d2 = 0, i = 0; i < FS - 2; i++) { double d = fabs(t[i + 2] - 2 * t[i + 1] + t[i]); if (d > *d2) *d2 = d; }
    free(r); free(o);
}
static void rev_plate_mod(void)
{
    double h0, h1, l0, l1, d0, d1;
    rev_plate_mod1(0, &h0, &l0, &d0);
    rev_plate_mod1(127, &h1, &l1, &d1);
    printf("REVERB PLATE MOD 127 against MOD 0 (a chord's tail): the largest second difference %.2fx MOD 0's; 2 .. 12 kHz "
           "%.1f dB re the whole (MOD 0 %.1f); the tail's level %+.2f dB\n", d1 / d0, h1, h0, db(l1 / l0));
}

/* REVERB TYPE changes under a held chord (MIX 127: the wet alone): 1 s of the first TYPE, then the second; the
 * largest second difference out in the 200 ms after the change (the old TYPE fading out, the new one starting) re the
 * dry chord's largest (a step into the new TYPE's empty lines shows as a burst of it) */
static double rev_type1(unsigned a, unsigned b2)
{
    static const int F[4] = {196, 247, 294, 392};
    tx_fx_reverb_t *r = malloc(sizeof *r);
    int nb = 3 * FS / (int)NB, b, i, k, sw = FS / (int)NB;
    int32_t x[NB], y[NB];
    double *o = malloc(sizeof(double) * nb * NB), *dr = malloc(sizeof(double) * nb * NB), d0 = 0, d1 = 0;
    tx_fx_reverb_init(r);
    tx_fx_reverb_param(r, TX_RV_TYPE, a); tx_fx_reverb_param(r, TX_RV_MIX, 127);
    for (b = 0; b < nb; b++) {
        if (b == sw) tx_fx_reverb_param(r, TX_RV_TYPE, b2);
        for (i = 0; i < (int)NB; i++) {
            double s = 0;
            for (k = 0; k < 4; k++) s += sin(2.0 * M_PI * F[k] * (double)(b * NB + i) / FS + k);
            x[i] = y[i] = (int32_t)lrint(8388608.0 * 0.12 * s);
            dr[b * NB + i] = x[i];
        }
        tx_fx_reverb_process(r, x, y, NB);
        for (i = 0; i < (int)NB; i++) o[b * NB + i] = x[i];
    }
    for (i = sw * NB - 2; i < sw * (int)NB + FS / 5; i++) { double d = fabs(o[i + 2] - 2 * o[i + 1] + o[i]); if (d > d0) d0 = d; }
    for (i = 0; i < FS; i++) { double d = fabs(dr[i + 2] - 2 * dr[i + 1] + dr[i]); if (d > d1) d1 = d; }
    free(r); free(o); free(dr);
    return d0 / d1;
}
static void rev_type(void)
{
    printf("REVERB TYPE change under a held chord: the largest second difference after it re the dry chord's: "
           "PLATE -> INFINITE %.2f, HALL -> PLATE %.2f, OFF -> HALL %.2f, INFINITE -> ROOM %.2f\n",
           rev_type1(TX_RVT_PLATE, TX_RVT_INF), rev_type1(TX_RVT_HALL, TX_RVT_PLATE), rev_type1(TX_RVT_OFF, TX_RVT_HALL),
           rev_type1(TX_RVT_INF, TX_RVT_ROOM));
}

/* REVERB quiet: a 331 Hz sine of a LSB (Q15 steps, the lines' own) held through the TYPE (MIX 127, the wet alone); the
 * worst of H2..H6 re H1 in its second second (the tank is linear: harmonics are its rounding's) */
static double rev_quiet1(unsigned ty, double a)
{
    tx_fx_reverb_t *r = malloc(sizeof *r);
    int nb = 2 * FS / (int)NB + 1, b, i, k;
    int32_t x[NB], y[NB];
    double *o = malloc(sizeof(double) * nb * NB), h1, w = -200;
    tx_fx_reverb_init(r);
    tx_fx_reverb_param(r, TX_RV_TYPE, ty); tx_fx_reverb_param(r, TX_RV_MIX, 127);
    for (b = 0; b < nb; b++) {
        for (i = 0; i < (int)NB; i++) x[i] = y[i] = (int32_t)lrint(256.0 * a * sin(2.0 * M_PI * 331 * (double)(b * NB + i) / FS));
        tx_fx_reverb_process(r, x, y, NB);
        for (i = 0; i < (int)NB; i++) o[b * NB + i] = x[i];
    }
    h1 = bin2(o + FS, FS, 331);
    for (k = 2; k <= 6; k++) { double d = db(bin2(o + FS, FS, 331 * k) / h1); if (d > w) w = d; }
    free(r); free(o);
    return w;
}
static void rev_quiet(void)
{
    static const unsigned T[4] = {TX_RVT_HALL, TX_RVT_PLATE, TX_RVT_ROOM, TX_RVT_INF};
    static const char *const N[4] = {"HALL", "PLATE", "ROOM", "INFINITE"};
    int t;
    printf("REVERB quiet: a 331 Hz sine held (MIX 127), the worst of H2..H6 re H1 dB, per level (Q15 LSB): 8 / 32 / 128\n ");
    for (t = 0; t < 4; t++) printf(" %s %.1f / %.1f / %.1f", N[t], rev_quiet1(T[t], 8), rev_quiet1(T[t], 32), rev_quiet1(T[t], 128));
    printf("\n");
}

/* REVERB quiet tail: a chord (196 247 294 392 Hz, 0.3 s, 5 ms edges, each tone -30 dBFS) and the same chord
 * q dB down through the TYPE (MIX 127, the wet alone, its defaults); the quiet render scaled back up, less the loud:
 * the tanks are linear, so what differs is their rounding at the low level (the grain a quiet note's tail carries).
 * Over the tail's first second (from 0.1 s after the chord): the difference re the loud tail (dB); and the quiet
 * tail's level re the loud in that second's last 0.25 s (a rounding that eats the low end decays it faster) */
static double rev_tail1(unsigned ty, double q, double *lv)
{
    static const int F[4] = {196, 247, 294, 392};
    double *o[2], w, g = pow(10.0, -q / 20.0);
    int nb = 3 * FS / 2 / (int)NB, b, i, k, v, t0 = 4 * FS / 10, wn = FS / 4;
    for (v = 0; v < 2; v++) {
        tx_fx_reverb_t *r = malloc(sizeof *r);
        int32_t x[NB], y[NB];
        o[v] = malloc(sizeof(double) * nb * NB);
        tx_fx_reverb_init(r);
        tx_fx_reverb_param(r, TX_RV_TYPE, ty); tx_fx_reverb_param(r, TX_RV_MIX, 127);
        for (b = 0; b < nb; b++) {
            for (i = 0; i < (int)NB; i++) {
                int n = b * NB + i, e = FS / 200, len = 3 * FS / 10;
                double a = n >= len ? 0 : n < e ? 0.5 - 0.5 * cos(M_PI * n / e) : n > len - e ? 0.5 - 0.5 * cos(M_PI * (len - n) / e) : 1, s = 0;
                for (k = 0; k < 4; k++) s += sin(2.0 * M_PI * F[k] * n / FS + k);
                x[i] = y[i] = (int32_t)lrint(8388608.0 * 0.0316 * (v ? g : 1.0) * a * s);
            }
            tx_fx_reverb_process(r, x, y, NB);
            for (i = 0; i < (int)NB; i++) o[v][b * NB + i] = v ? x[i] / g : x[i];
        }
        free(r);
    }
    {
        double e = 0, l = 0;
        for (k = t0; k < t0 + 4 * wn; k++) { double d = o[1][k] - o[0][k]; e += d * d; l += o[0][k] * o[0][k]; }
        w = db(e / l);
        for (e = l = 0, k = t0 + 3 * wn; k < t0 + 4 * wn; k++) { e += o[1][k] * o[1][k]; l += o[0][k] * o[0][k]; }
        *lv = l > 0 ? db(e / l) : 1;               /* (1: the loud tail has ended there) */
    }
    free(o[0]); free(o[1]);
    return w;
}
static void rev_tail(void)
{
    static const unsigned T[4] = {TX_RVT_HALL, TX_RVT_PLATE, TX_RVT_ROOM, TX_RVT_INF};
    static const char *const N[4] = {"HALL", "PLATE", "ROOM", "INFINITE"};
    int t;
    printf("REVERB quiet tail: a chord, each tone -40 / -60 dBFS, re the same at -30 (under INFINITE's input limiter): "
           "the tails' difference over the first second, dB (the -60's level at its end):");
    for (t = 0; t < 4; t++) {
        double a, b, l2;
        a = rev_tail1(T[t], 10, &l2); b = rev_tail1(T[t], 30, &l2);
        if (l2 > 0) printf(" %s %.1f / %.1f (ended)", N[t], a, b);
        else printf(" %s %.1f / %.1f (%+.1f)", N[t], a, b, l2);
    }
    printf("\n");
}

/* REVERB SIZE steps under a held chord (MIX 127, the wet alone): SIZE z0 -> z1 at 1.5 s, back at 3 s; the largest
 * second difference in the 1.2 s after a step (a glide's or a fade's whole span) re the largest of the held tail in
 * the 0.5 s before it and the 0.3 s after that span (Frodhes' control click; 1 = none): the worse of the two steps */
static double rev_step1(unsigned ty, unsigned id, unsigned z0, unsigned z1);
static double rev_size1(unsigned ty, unsigned z0, unsigned z1) { return rev_step1(ty, TX_RV_SIZE, z0, z1); }
static double rev_step1(unsigned ty, unsigned id, unsigned z0, unsigned z1)
{
    static const int F[4] = {196, 247, 294, 392};
    tx_fx_reverb_t *r = malloc(sizeof *r);
    int nb = 5 * FS / (int)NB, b, i, k, s, s1 = 3 * FS / 2 / (int)NB, s2 = 3 * FS / (int)NB;
    int32_t x[NB], y[NB];
    double *o = malloc(sizeof(double) * nb * NB), w = 0;
    tx_fx_reverb_init(r);
    tx_fx_reverb_param(r, TX_RV_TYPE, ty); tx_fx_reverb_param(r, TX_RV_MIX, 127); tx_fx_reverb_param(r, id, z0);
    for (b = 0; b < nb; b++) {
        if (b == s1) tx_fx_reverb_param(r, id, z1);
        if (b == s2) tx_fx_reverb_param(r, id, z0);
        for (i = 0; i < (int)NB; i++) {
            double v = 0;
            for (k = 0; k < 4; k++) v += sin(2.0 * M_PI * F[k] * (double)(b * NB + i) / FS + k);
            x[i] = y[i] = (int32_t)lrint(8388608.0 * 0.12 * v);
        }
        tx_fx_reverb_process(r, x, y, NB);
        for (i = 0; i < (int)NB; i++) o[b * NB + i] = x[i];
    }
    for (s = 0; s < 2; s++) {
        int e = (s ? s2 : s1) * (int)NB;
        double d0 = 0, d1 = 0;
        for (i = e - FS / 2; i < e - 2; i++) { double d = fabs(o[i + 2] - 2 * o[i + 1] + o[i]); if (d > d0) d0 = d; }
        for (i = e + FS * 6 / 5; i < e + FS * 3 / 2 - 2; i++) { double d = fabs(o[i + 2] - 2 * o[i + 1] + o[i]); if (d > d0) d0 = d; }
        for (i = e - 2; i < e + FS * 6 / 5 - 2; i++) { double d = fabs(o[i + 2] - 2 * o[i + 1] + o[i]); if (d > d1) d1 = d; }
        if (d1 / d0 > w) w = d1 / d0;
    }
    free(r); free(o);
    return w;
}
static void rev_size(void)
{
    printf("REVERB SIZE steps under a held chord: the largest second difference in the 1.2 s after a step re the held: "
           "HALL AUTO <-> 1 %.2f, HALL 40 <-> 41 %.2f, PLATE AUTO <-> 1 %.2f, PLATE 64 <-> 70 %.2f, INFINITE 64 <-> 70 "
           "%.2f, INFINITE 90 <-> 30 %.2f (no change: INFINITE 64 <-> 64 %.2f)\n",
           rev_size1(TX_RVT_HALL, 0, 1), rev_size1(TX_RVT_HALL, 40, 41), rev_size1(TX_RVT_PLATE, 0, 1),
           rev_size1(TX_RVT_PLATE, 64, 70), rev_size1(TX_RVT_INF, 64, 70), rev_size1(TX_RVT_INF, 90, 30),
           rev_size1(TX_RVT_INF, 64, 64));
}

/* REVERB MOD moved at once under a held chord (Frodhes 0.9.6, BUGHUNT #10: HALL's read offset moved a whole sample
 * every 8 MOD steps, INFINITE's and PLATE's depth followed MOD at once): MOD 0 -> 127 at 1.5 s, back at 3 s, the
 * largest second difference in the 1.2 s after re the held tail (as the SIZE steps); 2 at most */
static void rev_mod(void)
{
    double h = rev_step1(TX_RVT_HALL, TX_RV_MOD, 0, 127), m = rev_step1(TX_RVT_ROOM, TX_RV_MOD, 0, 127);
    double i = rev_step1(TX_RVT_INF, TX_RV_MOD, 0, 127), p = rev_step1(TX_RVT_PLATE, TX_RV_MOD, 0, 127);
    printf("REVERB MOD 0 <-> 127 under a held chord: the largest second difference in the 1.2 s after a step re the "
           "held: HALL %.2f, ROOM %.2f, INFINITE %.2f, PLATE %.2f (2 at most)\n", h, m, i, p);
    judge("REVERB MOD step HALL", h, 2.0, 1); judge("REVERB MOD step ROOM", m, 2.0, 1);
    judge("REVERB MOD step INFINITE", i, 2.0, 1); judge("REVERB MOD step PLATE", p, 2.0, 1);
}

/* REVERB TYPE within the INFINITE kernel under a held chord (the control sweep: INFINITE, DARK, DRIFT, SWELL are one
 * kernel, no fade between them; INFINITE -> DARK stood the reads still and jumped the gains and the loss at once:
 * 17.5x): t0 -> t1 at 1.5 s, back at 3 s; the largest second difference in the 1.2 s after each step re the larger
 * of the two TYPEs held over the same 1.2 s (their own tails: DRIFT's walk is peaky of itself); 2 at most */
static double *rev_ty_render(unsigned t0, unsigned t1)
{
    static const int F[4] = {196, 247, 294, 392};
    tx_fx_reverb_t *r = malloc(sizeof *r);
    int nb = 5 * FS / (int)NB, b, i, k, s1 = 3 * FS / 2 / (int)NB, s2 = 3 * FS / (int)NB;
    int32_t x[NB], y[NB];
    double *o = malloc(sizeof(double) * nb * NB);
    tx_fx_reverb_init(r);
    tx_fx_reverb_param(r, TX_RV_TYPE, t0); tx_fx_reverb_param(r, TX_RV_MIX, 127);
    for (b = 0; b < nb; b++) {
        if (b == s1) tx_fx_reverb_param(r, TX_RV_TYPE, t1);
        if (b == s2) tx_fx_reverb_param(r, TX_RV_TYPE, t0);
        for (i = 0; i < (int)NB; i++) {
            double v = 0;
            for (k = 0; k < 4; k++) v += sin(2.0 * M_PI * F[k] * (double)(b * NB + i) / FS + k);
            x[i] = y[i] = (int32_t)lrint(8388608.0 * 0.12 * v);
        }
        tx_fx_reverb_process(r, x, y, NB);
        for (i = 0; i < (int)NB; i++) o[b * NB + i] = x[i];
    }
    free(r);
    return o;
}
static double d2win(const double *o, int a, int e)
{
    double m = 0;
    int i;
    for (i = a; i < e - 2; i++) { double d = fabs(o[i + 2] - 2 * o[i + 1] + o[i]); if (d > m) m = d; }
    return m;
}
static double rev_ty1(unsigned t0, unsigned t1)
{
    double *mv = rev_ty_render(t0, t1), *h0 = rev_ty_render(t0, t0), *h1 = rev_ty_render(t1, t1), w = 0;
    int s;
    for (s = 0; s < 2; s++) {
        int e = (s ? 3 * FS / (int)NB : 3 * FS / 2 / (int)NB) * (int)NB, z = e + FS * 6 / 5;
        double ref = fmax(d2win(h0, e - 2, z), d2win(h1, e - 2, z));
        if (d2win(mv, e - 2, z) / ref > w) w = d2win(mv, e - 2, z) / ref;
    }
    free(mv); free(h0); free(h1);
    return w;
}
static void rev_dark(void)
{
    double a = rev_ty1(TX_RVT_INF, TX_RVT_DARK), b = rev_ty1(TX_RVT_DARK, TX_RVT_DRIFT);
    double c = rev_ty1(TX_RVT_DARK, TX_RVT_SWELL), d = rev_ty1(TX_RVT_INF, TX_RVT_DRIFT);
    printf("REVERB TYPE within the INFINITE kernel under a held chord: the largest second difference after a step re both "
           "TYPEs held: INFINITE <-> DARK %.2f, DARK <-> DRIFT %.2f, DARK <-> SWELL %.2f, INFINITE <-> DRIFT %.2f (2 at "
           "most)\n", a, b, c, d);
    judge("REVERB TYPE INFINITE <-> DARK", a, 2.0, 1); judge("REVERB TYPE DARK <-> DRIFT", b, 2.0, 1);
    judge("REVERB TYPE DARK <-> SWELL", c, 2.0, 1); judge("REVERB TYPE INFINITE <-> DRIFT", d, 2.0, 1);
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
    drv_onoff();
    gd_zipper();
    lim_chord();
    lim_bass();
    comp_quiet();
    comp_chord();
    comp_mkup();
    comp_strike();
    comp_sweep();
    rev_plate_mod();
    rev_type();
    rev_quiet();
    rev_tail();
    rev_size();
    rev_mod();
    rev_dark();
    free(fx);
    if (check) printf("tx-pristine: %s (%d over)\n", fails ? "FAIL" : "ok", fails);
    return check && fails ? 1 : 0;
}
