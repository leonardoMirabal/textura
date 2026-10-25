/* SPDX-License-Identifier: GPL-3.0-only */
/* txgen: the lane generator's long run. A generator (tx_core_gen) plays a SOURCE preset for a long time, fast (no
 * real time), as a lane would: it sets ACTIVITY and presses its notes. One line per second goes to a CSV: the
 * activity, the pitch centre, the notes that second, their mean pitch, the recall's weight, the audio's rms (dBFS).
 * Then the checks: the activity and the pitch wander (no stuck state: the spread of every 5-minute window above a
 * floor), nothing runs away (notes per second bounded, the activity inside 0 .. 127, the audio neither silent for long
 * nor at full scale over 1 % of the time: the raw mix, before the output limiter).
 *
 *   txgen [MINUTES] [PRESET] [EVOLVE] [-o OUT.csv]     defaults: 30 min, preset 3 (water), EVOLVE 127 (2 h)
 * Exit 1 on a failed check. Plot: .local/venv/bin/python bench/txgen_plot.py OUT.csv OUT.png */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tx_host.h"
#include "tx_core_gen.h"

int main(int argc, char **argv)
{
    unsigned minutes = 30, preset = 3, evolve = 127, i, fails = 0;
    const char *out = "build/gen/longrun.csv";
    const tx_engine_t *e = tx_engine_find("source");
    tx_gen_t *g = malloc(sizeof *g);
    void *st;
    FILE *f;
    int32_t mix[TX_BLOCK];
    uint32_t blocks, b, sec = 0, nsec, notes_s = 0, psum = 0, stuck = 0, silent_run = 0, worst_silent = 0, full = 0;
    uint32_t mute_run = 0, worst_mute = 0;                  /* seconds without a note while the activity is 40 or more */
    int pa = -1, pid = -1, iid = -1;
    double e2 = 0, *act, *pcs, maxn = 0;
    tx_sha256_t h;
    char hex[65];
    for (i = 1; i < (unsigned)argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < (unsigned)argc) out = argv[++i];
        else if (pa < 0) { minutes = (unsigned)atoi(argv[i]); pa = 0; }
        else if (pa == 0) { preset = (unsigned)atoi(argv[i]); pa = 1; }
        else evolve = (unsigned)atoi(argv[i]);
    }
    if (!e || !g) { fprintf(stderr, "no source engine\n"); return 2; }
    pid = tx_param_find(e, "PRESET"); iid = tx_param_find(e, "ACTIVITY");
    st = tx_state_new(e);
    e->init(st, 5);
    e->param(st, (uint8_t)pid, (uint8_t)preset);
    tx_gen_init(g, 5);
    tx_gen_param(g, TX_GEN_EVOLVE, evolve);
    e->key(st, 60, 100, 1);                                  /* the lane runs (its bed) */
    { char d[256]; strncpy(d, out, sizeof d - 1); d[sizeof d - 1] = 0; char *sl = strrchr(d, '/'); if (sl) { *sl = 0; char cmd[300]; snprintf(cmd, sizeof cmd, "mkdir -p '%s'", d); if (system(cmd)) { } } }
    if (!(f = fopen(out, "w"))) { perror(out); return 2; }
    fprintf(f, "second,activity,pitch_centre,notes,mean_note,recall,rms_db\n");
    nsec = minutes * 60u;
    act = calloc(nsec + 1u, sizeof *act); pcs = calloc(nsec + 1u, sizeof *pcs);
    blocks = (uint32_t)(((uint64_t)nsec * TX_FS) / TX_BLOCK);
    tx_sha256_init(&h);
    for (b = 0; b < blocks; b++) {
        tx_gen_ev_t ev[TX_GEN_EV_MAX];
        uint8_t in;
        unsigned ne = tx_gen_block(g, TX_BLOCK, ev, TX_GEN_EV_MAX, &in), k, j;
        e->param(st, (uint8_t)iid, in);
        for (k = 0; k < ne; k++) {
            e->key(st, ev[k].note, ev[k].vel, ev[k].vel != 0);
            if (ev[k].vel) { notes_s++; psum += ev[k].note; }
        }
        for (j = 0; j < TX_BLOCK; j++) mix[j] = 0;
        e->render(st, mix, TX_BLOCK);
        tx_sha256_q23(&h, mix, TX_BLOCK);
        for (j = 0; j < TX_BLOCK; j++) { double v = mix[j] / 8388608.0; e2 += v * v; if (mix[j] >= TX_Q23 || mix[j] <= -TX_Q23) full++; }
        if ((uint64_t)(b + 1u) * TX_BLOCK / TX_FS != (uint64_t)b * TX_BLOCK / TX_FS) {   /* a second passed */
            double rms = 10.0 * log10(e2 / TX_FS + 1e-20);
            act[sec] = g->a / 256.0; pcs[sec] = g->pc / 256.0;
            fprintf(f, "%u,%.2f,%.2f,%u,%.1f,%.3f,%.1f\n", sec, act[sec], pcs[sec], notes_s,
                    notes_s ? (double)psum / notes_s : 0.0, g->g / 32768.0, rms);
            if (notes_s > maxn) maxn = notes_s;
            if (!notes_s && act[sec] >= 40) { if (++mute_run > worst_mute) worst_mute = mute_run; } else if (notes_s) mute_run = 0;
            if (rms < -90) { if (++silent_run > worst_silent) worst_silent = silent_run; } else silent_run = 0;
            sec++; notes_s = 0; psum = 0; e2 = 0;
        }
    }
    fclose(f);
    tx_sha256_hex(&h, hex);
    {   /* no stuck state: in every 5-minute window the activity and the pitch centre move */
        unsigned w, win = 300, nw = sec / win;
        double amin = 1e9, amax = -1e9;
        for (w = 0; w < nw; w++) {
            double ma = 0, va = 0, mp = 0, vp = 0;
            for (i = w * win; i < (w + 1) * win; i++) { ma += act[i]; mp += pcs[i]; }
            ma /= win; mp /= win;
            for (i = w * win; i < (w + 1) * win; i++) { va += (act[i] - ma) * (act[i] - ma); vp += (pcs[i] - mp) * (pcs[i] - mp); }
            if (sqrt(va / win) < 0.5 || sqrt(vp / win) < 0.05) stuck++;
        }
        for (i = 0; i < sec; i++) { if (act[i] < amin) amin = act[i]; if (act[i] > amax) amax = act[i]; }
        printf("txgen: %u min, preset %u, EVOLVE %u: activity %.1f .. %.1f, notes up to %.0f /s, %u of %u 5-min windows still,"
               " longest silence %u s, longest without a note (activity >= 40) %u s, %u samples at full scale, %u notes,"
               " sha256 %.16s -> %s\n", minutes, preset, evolve, amin, amax, maxn, stuck, nw, worst_silent, worst_mute, full,
               g->notes, hex, out);
        if (stuck) { printf("FAIL  a stuck state: a 5-minute window without motion\n"); fails++; }
        if (maxn > 40) { printf("FAIL  runaway: %.0f notes in one second\n", maxn); fails++; }
        if (amax - amin < 8) { printf("FAIL  the activity hardly wandered (%.1f .. %.1f)\n", amin, amax); fails++; }
        if (worst_silent > 120) { printf("FAIL  silent for %u s\n", worst_silent); fails++; }
        if (worst_mute > 60) { printf("FAIL  no note for %u s at activity >= 40 (stuck notes)\n", worst_mute); fails++; }
        if (full > nsec * (TX_FS / 100u)) { printf("FAIL  %u samples at full scale (over 1 %%)\n", full); fails++; }
    }
    free(act); free(pcs); free(st); free(g);
    printf(fails ? "%u check(s) FAILED\n" : "all long-run checks passed\n", fails);
    return fails ? 1 : 0;
}
