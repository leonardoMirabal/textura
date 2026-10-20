/* SPDX-License-Identifier: GPL-3.0-only */
/* txrender: a scene script -> a WAV (mono, 44.1 kHz, 24-bit) in build/wav/, deterministic, with a summary line.
 *
 *   txrender ENGINE SCENE [-o OUT.wav] [-s SEED] [-c]   SCENE: max (the engine's worst case), a name in bench/scenes/
 *                                                  (idle tap hold chord cap), or a script file
 *                                                  -c: through the CHAMBER (its defaults + the scene's chamber lines;
 *                                                  automatic when the scene has chamber lines): <engine>-<scene>-chamber.wav
 *   txrender all                                   every engine x every standard scene
 *   txrender list                                  the engines and their parameters
 * The summary: blocks, peak (dBFS), samples over full scale, most particles at once, events, steals, the output
 * limiter's activity (blocks it acted in, its deepest reduction), sha256 of the raw Q23 samples (the same hash txtest
 * keeps in bench/baselines/renders.sha256). The WAV goes through the output limiter (tx_core_limit), as the player. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "tx_host.h"
#include "tx_core_limit.h"

typedef struct {
    tx_wav_t wav;
    int use_wav;
    tx_sha256_t sha;
    int32_t peak;
    uint32_t clips, maxp, events, steals, dropped;
    tx_limit_t lim;                /* the output stage: only the WAV is limited (the hash is the raw mix) */
    uint32_t lim_blocks, lim_db10;
    int32_t lout[TX_BLOCK];
} acc_t;

static void on_block(void *u, uint32_t blk, const int32_t *mix, const tx_stats_t *st)
{
    acc_t *a = u;
    unsigned j;
    (void)blk;
    for (j = 0; j < TX_BLOCK; j++) {
        int32_t v = mix[j] < 0 ? -mix[j] : mix[j];
        if (v > a->peak) a->peak = v;
        if (v >= TX_Q23) a->clips++;
    }
    {   /* the WAV through the output limiter (the player's chain); the hash below stays on the raw mix */
        uint32_t gr;
        for (j = 0; j < TX_BLOCK; j++) a->lout[j] = mix[j];
        tx_limit_process(&a->lim, a->lout, TX_BLOCK);
        gr = tx_limit_take(&a->lim);
        if (gr) { a->lim_blocks++; if (gr > a->lim_db10) a->lim_db10 = gr; }
    }
    if (a->use_wav) tx_wav_write(&a->wav, a->lout, TX_BLOCK);
    tx_sha256_q23(&a->sha, mix, TX_BLOCK);
    if (st->particles > a->maxp) a->maxp = st->particles;
    a->events += st->events;
    a->dropped += st->dropped;
    a->steals = st->steals;
}

static int render(const tx_engine_t *e, const char *scene, const char *out, long seed, int use_ch)
{
    tx_script_t *s = malloc(sizeof *s);
    void *st = tx_state_new(e);
    tx_chamber_t *ch = 0;
    acc_t a;
    char path[512], hex[65];
    const char *base = strrchr(scene, '/') ? strrchr(scene, '/') + 1 : scene;
    memset(&a, 0, sizeof a);
    tx_limit_init(&a.lim);
    if (tx_scene_load(s, e, scene)) return 1;
    if (seed >= 0) s->seed = (uint32_t)seed;
    if (use_ch || tx_script_has_chamber(s)) ch = malloc(sizeof *ch);
    if (!out) {
        mkdir("build", 0755); mkdir("build/wav", 0755);
        snprintf(path, sizeof path, "build/wav/%s-%.*s%s.wav", e->name, (int)strcspn(base, "."), base, ch ? "-chamber" : "");
        out = path;
    }
    a.use_wav = tx_wav_open(&a.wav, out) == 0;
    tx_sha256_init(&a.sha);
    tx_script_run_ch(e, st, ch, s, on_block, &a);
    if (a.use_wav) tx_wav_close(&a.wav);
    tx_sha256_hex(&a.sha, hex);
    printf("%-8s %-6s %5u blocks  peak %6.1f dBFS  over-FS %u  particles max %2u  events %6u  dropped %5u  steals %5u"
           "  limiter %u blocks (max %.1f dB)  sha256 %.16s  -> %s\n", e->name, base, s->blocks,
           a.peak ? 20.0 * log10(a.peak / 8388608.0) : -999.0, a.clips, a.maxp, a.events, a.dropped, a.steals,
           a.lim_blocks, a.lim_db10 / 10.0, hex, out);
    free(st); free(s); free(ch);
    return 0;
}

int main(int argc, char **argv)
{
    const char *out = 0;
    long seed = -1;
    int i, rc = 0, use_ch = 0;
    const tx_engine_t *e;
    if (argc >= 2 && !strcmp(argv[1], "list")) {
        unsigned k, p;
        for (k = 0; tx_engines[k]; k++) {
            e = tx_engines[k];
            printf("%s: %s (PARTICLES 1..%u, default %u; state %u B)\n", e->name, e->title, e->particles_max,
                   e->particles_def, e->state_bytes);
            for (p = 0; p < e->n_params; p++)
                printf("  %2u %-8s 0..%-3u def %3u  %s\n", p, e->params[p].name, e->params[p].max, e->params[p].def, e->params[p].help);
        }
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "all")) {
        unsigned k, sc;
        for (k = 0; tx_engines[k]; k++)
            for (sc = 0; tx_std_scenes[sc]; sc++) rc |= render(tx_engines[k], tx_std_scenes[sc], 0, -1, 0);
        return rc;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: txrender ENGINE SCENE [-o OUT.wav] [-s SEED] [-c] | txrender all | txrender list\n");
        return 2;
    }
    for (i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) seed = strtol(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "-c")) use_ch = 1;
    }
    if (!(e = tx_engine_find(argv[1]))) { fprintf(stderr, "no engine %s (txrender list)\n", argv[1]); return 2; }
    return render(e, argv[2], out, seed, use_ch);
}
