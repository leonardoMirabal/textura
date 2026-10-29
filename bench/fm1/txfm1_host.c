/* SPDX-License-Identifier: GPL-3.0-only */
/* txfm1_host: txfm1's table (bench/fm1/txfm1_core.h) on this computer, the host twin of the FM-1's bench firmware.
 * Same code, same table, so the device's numbers divided by these are the real host -> FM-1 factor per
 * configuration, and the hashes say whether the FM-1 computes the same samples.
 *
 *   txfm1_host [-r RUNS] [-f TEXT] > build/fm1/host.csv
 *
 * Each configuration: RUNS runs (default 3), each with its warm-up; the lowest worst / w8 / mean over the runs (the
 * host's bursts out). Times in us (the host's own, not scaled). Run it under the HEAVY lock (make fm1-host does). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tx_host.h"
#include "txfm1_core.h"
#define FM_LABELS
#include "fm1_cfg.h"

static uint32_t host_now(void) { return (uint32_t)(tx_now_ns() * FM_TPU / 1000u); }
static uint32_t host_nested(void) { return 0; }

int main(int argc, char **argv)
{
    static const fm_clock_t ck = {host_now, host_nested};
    unsigned runs = 3, i, r, a;
    const char *filter = 0;
    fm_run_t *run;
    int32_t L[TX_BLOCK], R[TX_BLOCK];
    for (a = 1; a < (unsigned)argc; a++) {
        if (!strcmp(argv[a], "-r") && a + 1 < (unsigned)argc) runs = (unsigned)atoi(argv[++a]);
        else if (!strcmp(argv[a], "-f") && a + 1 < (unsigned)argc) filter = argv[++a];
        else { fprintf(stderr, "usage: txfm1_host [-r RUNS] [-f TEXT]\n"); return 2; }
    }
    if (!runs) runs = 1;
    if (!(run = aligned_alloc(64, (fm_run_bytes() + 63u) & ~63u))) { perror("txfm1_host"); return 2; }
    tx_bench_prepare();
    printf("idx,label,worst_us,w8_us,mean_us,lane_worst_us,lane_w8_us,hash,stable\n");
    for (i = 0; i < FM_NCFG; i++) {
        double w = 1e30, w8 = 1e30, m = 1e30;
        double lw[4] = {1e30, 1e30, 1e30, 1e30}, l8[4] = {1e30, 1e30, 1e30, 1e30};
        uint32_t hash = 0;
        int stable = 1;
        unsigned k, nl = FM_CFG[i].kind == FM_K_LANES ? FM_CFG[i].eng : 0u;
        char lane[2][96];
        if (filter && !strstr(FM_LABEL[i], filter)) continue;
        for (r = 0; r < runs; r++) {
            const fm_stats_t *s;
            fm_begin(run, FM_CFG, FM_EV, i, 1);
            while (fm_step(run, &ck, L, R)) {}
            s = fm_stats(run);
            if (r && s->hash != hash) stable = 0;
            hash = s->hash;
            if (s->worst / 24.0 < w) w = s->worst / 24.0;
            if (s->w8 / 24.0 < w8) w8 = s->w8 / 24.0;
            if (s->sum / 24.0 / s->n < m) m = s->sum / 24.0 / s->n;
            for (k = 0; k < nl; k++) {
                if (s->lane_worst[k] / 24.0 < lw[k]) lw[k] = s->lane_worst[k] / 24.0;
                if (s->lane_w8[k] / 24.0 < l8[k]) l8[k] = s->lane_w8[k] / 24.0;
            }
        }
        lane[0][0] = lane[1][0] = 0;
        for (k = 0; k < nl; k++) {
            size_t n0 = strlen(lane[0]), n1 = strlen(lane[1]);
            snprintf(lane[0] + n0, sizeof lane[0] - n0, "%s%.1f", k ? " " : "", lw[k]);
            snprintf(lane[1] + n1, sizeof lane[1] - n1, "%s%.1f", k ? " " : "", l8[k]);
        }
        printf("%u,\"%s\",%.2f,%.2f,%.2f,%s,%s,%08x,%d\n", i, FM_LABEL[i], w, w8, m, lane[0], lane[1], hash, stable);
        fflush(stdout);
    }
    free(run);
    return 0;
}
