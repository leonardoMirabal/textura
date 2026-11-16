/* SPDX-License-Identifier: GPL-3.0-only */
/* txfxsweep: the effects under one moving control, for tools/fx_sweep.py (Frodhes' control sweep, on tx_fx): a ringing
 * chord (C3 E4 G4 A4 C5, struck at 0.5 s, ~4 s decay, harmonics 1..4) through the mix's DRIVE + FILTER and the bus'
 * GRAIN DELAY + REVERB; the control held, swept 0 -> its top -> 0 (a step every 20 ms from 1 s), or moved once A -> B
 * at 2 s. Host only (the chord is computed in double: a bench tool, not an engine).
 *   txfxsweep GROUP ID SETUP MODE A B OUT   GROUP d f g r (DRIVE, FILTER, GRAIN DELAY, REVERB); SETUP "gid=v,..."
 *   (the other controls first, g a group letter); MODE s (held at A) | w (the sweep) | f (A, then B at 2 s)
 *   txfxsweep list                          every control: "GROUP ID MAX DEF NAME" a line (the tool's cases)
 * OUT: raw int32 L R interleaved, Q23 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tx_fx.h"
#define FS 44100
#define T_NOTE 500
#define T_SW0 1000
#define STEP 20
#define T_FLIP 2000
static tx_fx_t fx;
static const tx_param_t *table(char g, unsigned *n)
{
    if (g == 'd') { *n = TX_DR_NP; return tx_fx_drive_params; }
    if (g == 'f') { *n = TX_MF_NP; return tx_fx_mfilter_params; }
    if (g == 'g') { *n = TX_GD_NP; return tx_fx_grain_params; }
    *n = TX_RV_NP; return tx_fx_reverb_params;
}
static unsigned maxof(char g, unsigned id) { unsigned n; return table(g, &n)[id].max; }
static void setp(char g, unsigned id, unsigned v)
{
    if (g == 'd') tx_fx_drive_set(&fx, id, v);
    else if (g == 'f') tx_fx_drive_set(&fx, TX_FX_MF_ID + id, v);
    else if (g == 'g') tx_fx_grain_param(&fx.gd, id, v);
    else tx_fx_reverb_param(&fx.rev, id, v);
}
int main(int argc, char **argv)
{
    char g, *setup, mode, buf[400], *t, *sv;
    unsigned pid, A, B, mx, nsw, tend, nb, b, j, k, h;
    static const int notes[5] = {48, 64, 67, 69, 72};
    int32_t x[TX_BLOCK], L[TX_BLOCK], R[TX_BLOCK], out[2 * TX_BLOCK];
    double ph[5][4] = {{0}};
    FILE *o;
    if (argc == 2 && !strcmp(argv[1], "list")) {
        static const char G[] = "dfgr";
        for (k = 0; k < 4; k++) {
            unsigned n, i;
            const tx_param_t *t = table(G[k], &n);
            for (i = 0; i < n; i++) printf("%c %u %u %u %s\n", G[k], i, t[i].max, t[i].def, t[i].name);
        }
        return 0;
    }
    if (argc < 8) return 2;
    g = argv[1][0]; pid = (unsigned)atoi(argv[2]); setup = argv[3]; mode = argv[4][0];
    A = (unsigned)atoi(argv[5]); B = (unsigned)atoi(argv[6]); mx = maxof(g, pid);
    nsw = 2u * mx; tend = mode == 'w' ? T_SW0 + nsw * STEP + 800u : 4000u;
    nb = (tend * FS / 1000u + 255u) / 256u;
    o = fopen(argv[7], "wb");
    if (!o) return 2;
    tx_fx_init(&fx);
    snprintf(buf, sizeof buf, "%s", setup);
    for (t = strtok_r(buf, ",", &sv); t; t = strtok_r(0, ",", &sv)) {
        char *e = strchr(t, '=');
        if (e) setp(t[0], (unsigned)atoi(t + 1), (unsigned)atoi(e + 1));
    }
    setp(g, pid, A);
    for (b = 0; b < nb; b++) {
        unsigned ms = (unsigned)((uint64_t)b * 256u * 1000u / FS), v = A;
        if (mode == 'w' && ms >= T_SW0) { unsigned s = (ms - T_SW0) / STEP; v = s >= nsw ? 0 : s <= mx ? s : nsw - s; }
        if (mode == 'f') v = ms >= T_FLIP ? B : A;
        setp(g, pid, v);
        for (j = 0; j < TX_BLOCK; j++) {
            double tt = (double)(b * TX_BLOCK + j) / FS, s = 0;
            if (tt >= T_NOTE / 1000.0)
                for (k = 0; k < 5; k++)
                    for (h = 0; h < 4; h++) {
                        double f = 440.0 * pow(2.0, (notes[k] - 69) / 12.0) * (h + 1);
                        ph[k][h] += f / FS;
                        s += 0.06 / (h + 1) * exp(-(tt - 0.5) * (1.0 + h) / 4.0) * sin(6.283185307179586 * ph[k][h]);
                    }
            x[j] = (int32_t)lrint(s * 8388608.0);
        }
        for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = x[j];
        tx_fx_drive_mix(&fx, L, R, TX_BLOCK);
        tx_fx_bus(&fx, L, R, TX_BLOCK);
        for (j = 0; j < TX_BLOCK; j++) out[2 * j] = L[j], out[2 * j + 1] = R[j];
        fwrite(out, sizeof out, 1, o);
    }
    fclose(o);
    return 0;
}
