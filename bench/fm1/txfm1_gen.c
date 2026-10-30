/* SPDX-License-Identifier: GPL-3.0-only */
/* txfm1_gen: txfm1's table (bench/fm1/txfm1_core.h) from the same scripts as make tx-budget, parsed here so the FM-1
 * needs no parser. Writes the C header (FM_CFG, FM_EV, FM_LABEL) the device and the host twin both build from.
 *
 *   txfm1_gen OUT.h
 *
 * The table: every SOURCE preset at the defaults (ACTIVITY 64, PROXIMITY 64), ACTIVITY 0 and 127 (PROXIMITY 127, as
 * tx-budget's ACTIVITY sweep), each preset's worst w8 knob from make tx-budget or SPACING 0,
 * the hosted engines' kinds (TYPE), the engines the lanes reach directly (storm, thunder, spring: their max scenes),
 * the effects (tx-budget's fx_cost settings) and four-lane cases (the worst four, the forest, storm and cave mixes). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tx_script.h"
#include "txfm1_core.h"

#define MAXC 400u
#define MAXE 20000u
static fm_cfg_t cfg[MAXC];
static fm_ev_t ev[MAXE];
static char label[MAXC][96];
static unsigned nc, ne;

static int eng_index(const char *name)
{
    int i;
    for (i = 0; tx_engines[i]; i++) if (!strcmp(tx_engines[i]->name, name)) return i;
    fprintf(stderr, "txfm1_gen: no engine %s\n", name);
    exit(2);
}

static unsigned scene(const char *eng, const char *text, const char *lab)
{
    static tx_script_t s;
    int ei = eng_index(eng);
    const tx_engine_t *e = tx_engines[ei];
    unsigned i;
    if (tx_script_parse(&s, e, text)) { fprintf(stderr, "txfm1_gen: %s: %s\n", lab, s.err); exit(2); }
    if (nc >= MAXC || ne + s.n > MAXE) { fprintf(stderr, "txfm1_gen: table full\n"); exit(2); }
    cfg[nc].kind = FM_K_SCENE; cfg[nc].eng = (uint8_t)ei;
    cfg[nc].ev = (uint16_t)ne; cfg[nc].nev = 0;
    for (i = 0; i < s.n; i++) {
        const tx_sev_t *v = &s.ev[i];
        if (v->kind == TX_SEV_CHAMBER) continue;
        ev[ne].blk = (uint16_t)v->block; ev[ne].id = v->id; ev[ne].a = v->a; ev[ne].b = v->b;
        ev[ne].kind = v->kind == TX_SEV_ON ? FM_EV_ON : v->kind == TX_SEV_OFF ? FM_EV_OFF : FM_EV_PARAM;
        ne++; cfg[nc].nev++;
    }
    cfg[nc].nblk = (uint16_t)(s.blocks < (FM_SECS * TX_FS) / TX_BLOCK ? s.blocks : (FM_SECS * TX_FS) / TX_BLOCK);
    cfg[nc].seed = s.seed;
    snprintf(label[nc], sizeof label[nc], "%s", lab);
    return nc++;
}

static unsigned src(unsigned pre, unsigned act, unsigned prox, const char *kn, unsigned kv, const char *lab)
{
    char t[400];
    int n = snprintf(t, sizeof t, "0 particles 6\n0 param PRESET %u\n0 param ACTIVITY %u\n0 param PROXIMITY %u\n",
                     pre, act, prox);
    if (kn) n += snprintf(t + n, sizeof t - (size_t)n, "0 param %s %u\n", kn, kv);
    snprintf(t + n, sizeof t - (size_t)n, "0 on 60 127\n%u end\n", FM_SECS * 1000u);
    return scene("source", t, lab);
}

static void fx(unsigned kind, unsigned type, unsigned decay, const char *lab)
{
    cfg[nc].kind = FM_K_FX; cfg[nc].eng = (uint8_t)kind; cfg[nc].type = (uint8_t)type; cfg[nc].decay = (uint8_t)decay;
    cfg[nc].nblk = (uint16_t)((FM_SECS * TX_FS) / TX_BLOCK); cfg[nc].seed = 1;
    snprintf(label[nc], sizeof label[nc], "%s", lab);
    nc++;
}

static void lanes(const unsigned *l, unsigned n, const char *lab)
{
    unsigned k;
    cfg[nc].kind = FM_K_LANES; cfg[nc].eng = (uint8_t)n;
    for (k = 0; k < n; k++) cfg[nc].lane[k] = (uint16_t)l[k];
    cfg[nc].nblk = (uint16_t)((FM_SECS * TX_FS) / TX_BLOCK); cfg[nc].seed = 1;
    snprintf(label[nc], sizeof label[nc], "%s", lab);
    nc++;
}

/* a second-core stress (FM_K_C1): scene configuration c rendered on both cores at once for secs, compared each block */
static void c1(unsigned c, unsigned secs, unsigned ping, const char *lab)
{
    if (nc >= MAXC) { fprintf(stderr, "txfm1_gen: table full\n"); exit(2); }
    cfg[nc] = cfg[c];
    cfg[nc].kind = FM_K_C1; cfg[nc].type = (uint8_t)ping;
    cfg[nc].nblk = (uint16_t)((secs * TX_FS) / TX_BLOCK);
    snprintf(label[nc], sizeof label[nc], "%s", lab);
    nc++;
}

static const char *const PRE[35] = {"rain_on_water", "stone_cloud", "cicadas", "water", "wind", "fire", "crickets",
    "sand", "gravel", "katydids", "bees", "ice", "wood", "minerals", "glass", "songbird", "chirper", "crow", "gull",
    "blackbird", "song_thrush", "nightingale", "robin", "wren", "hermit_thrush", "skylark", "wood_thrush",
    "reed_warbler", "curlew", "oystercatcher", "cuckoo", "woodpecker", "wood_pigeon", "nightjar", "bittern"};
/* each preset's worst w8 from tx-budget (make tx-budget's "w8 (at)" column, 2026-10-26); 0 = SPACING 0 (the birds') */
static const struct { const char *k; unsigned v, act; } WORST[35] = {
    {"TYPE", 40, 127}, {"SPACING", 0, 127}, {"TYPE", 40, 127}, {"TYPE", 127, 127}, {"SPREAD", 127, 127},
    {0, 0, 127}, {"TYPE", 80, 127}, {"TYPE", 80, 127}, {0, 0, 96}, {0, 0, 64}, {0, 0, 64}, {"SPACING", 0, 127},
    {"HARD", 127, 127}, {"HARD", 127, 127}, {"EXTRA", 127, 127}, {"SPACING", 0, 127}, {"SPACING", 0, 127},
    {"SPACING", 0, 127}, {"TYPE", 40, 127}, {"SPACING", 0, 127}, {"SPACING", 0, 127}, {"SPACING", 0, 127},
    {"SPACING", 0, 127}, {"SPACING", 0, 127}, {"SPACING", 0, 127}, {"SPACING", 0, 127}, {"SPACING", 0, 127},
    {"SPACING", 0, 127}, {"SPACING", 0, 127}, {"SPACING", 0, 127}, {"SPACING", 0, 127}, {"SPACING", 0, 127},
    {"SPACING", 0, 127}, {"SPACING", 0, 127}, {"SPACING", 0, 127}};
/* the hosted kinds (TYPE) per preset */
static const struct { unsigned pre; unsigned n; unsigned t[10]; } KINDS[] = {
    {0, 9, {20, 40, 60, 80, 99, 101, 110, 120, 127}},   /* rain: surfaces, hail and sleet (the 64-bit contacts) */
    {3, 10, {1, 16, 32, 48, 64, 80, 96, 112, 120, 127}},  /* water: drops .. a hull (TYPE 0 = the preset's own) */
    {11, 8, {8, 24, 40, 56, 72, 88, 104, 120}},          /* ice */
    {4, 8, {16, 32, 48, 64, 80, 96, 112, 127}},          /* wind */
    {14, 2, {64, 110}}, {12, 2, {64, 127}}, {13, 2, {64, 127}}, {7, 2, {40, 127}},
    {6, 3, {40, 80, 127}}, {9, 3, {40, 80, 127}}, {10, 3, {40, 80, 127}},
};

int main(int argc, char **argv)
{
    unsigned i, k, worst[35], def[35];
    char lab[64];
    FILE *f;
    if (argc != 2) { fprintf(stderr, "usage: txfm1_gen OUT.h\n"); return 2; }
    for (i = 0; i < 35u; i++) {
        snprintf(lab, sizeof lab, "%s default", PRE[i]); def[i] = src(i, 64, 64, 0, 0, lab);
        snprintf(lab, sizeof lab, "%s ACT 0", PRE[i]); src(i, 0, 127, 0, 0, lab);
        snprintf(lab, sizeof lab, "%s ACT 127", PRE[i]); src(i, 127, 127, 0, 0, lab);
        if (WORST[i].k) snprintf(lab, sizeof lab, "%s %s %u", PRE[i], WORST[i].k, WORST[i].v);
        else snprintf(lab, sizeof lab, "%s ACT %u", PRE[i], WORST[i].act);
        worst[i] = src(i, WORST[i].act, 127, WORST[i].k, WORST[i].v, lab);
    }
    for (i = 0; i < sizeof KINDS / sizeof KINDS[0]; i++)
        for (k = 0; k < KINDS[i].n; k++) {
            snprintf(lab, sizeof lab, "%s TYPE %u", PRE[KINDS[i].pre], KINDS[i].t[k]);
            src(KINDS[i].pre, 64, 64, "TYPE", KINDS[i].t[k], lab);
        }
    {
        static const char *const E[] = {"storm", "thunder", "spring"};
        unsigned th = 0;
        for (i = 0; i < 3u; i++) {
            unsigned c;
            snprintf(lab, sizeof lab, "%s max", E[i]);
            c = scene(E[i], tx_engines[eng_index(E[i])]->scene_max, lab);
            if (i == 1) th = c;
        }
        {
            static const char *const RT[8] = {"off", "ROOM", "HALL", "INFINITE", "DARK", "DRIFT", "SWELL", "PLATE"};
            static const char *const DT[5] = {"FDRV", "KHLON", "TSH8", "OHCD", "BHLUE"};
            for (k = 1; k < 8u; k++) { snprintf(lab, sizeof lab, "REVERB %s", RT[k]); fx(FM_FX_REVERB, k, 100, lab); }
            fx(FM_FX_REVERB, 3, 1, "REVERB INFINITE + SHIMMER");
            for (k = 1; k <= 16u; k *= 2) {
                snprintf(lab, sizeof lab, "GRAIN %u rate 1", k); fx(FM_FX_GRAIN, k, 0, lab);
                snprintf(lab, sizeof lab, "GRAIN %u pitched", k); fx(FM_FX_GRAIN, k, 1, lab);
            }
            for (k = 0; k < 5u; k++) { snprintf(lab, sizeof lab, "DRIVE %s", DT[k]); fx(FM_FX_DRIVE, k, 0, lab); }
            fx(FM_FX_FILTER, 0, 0, "FILTER");
            fx(FM_FX_COMP, 0, 0, "COMP (the defaults)");
            fx(FM_FX_COMP, 1, 0, "COMP THR -40 20:1 ATK 0");
        }
        {   /* four lanes: the four highest w8 (rain on water, water, minerals, stone cloud at their worst), and the
             * mixes tx-budget prices (forest, storm, cave; at their intensities) */
            unsigned l[4], lw[4], ls[4], lf[4], lc[4];
            l[0] = worst[0]; l[1] = worst[3]; l[2] = worst[13]; l[3] = worst[1];
            for (k = 0; k < 4u; k++) lw[k] = l[k];
            lanes(l, 4, "4 lanes worst (rain TYPE 40, water TYPE 127, minerals HARD 127, stones SPACING 0)");
            l[0] = src(15, 64, 64, 0, 0, "songbird ACT 64 (forest)"); l[1] = src(4, 64, 64, 0, 0, "wind ACT 64 (forest)");
            l[2] = src(6, 64, 64, 0, 0, "crickets ACT 64 (forest)"); l[3] = src(3, 64, 64, 0, 0, "water ACT 64 (forest)");
            lanes(l, 4, "4 lanes forest");
            for (k = 0; k < 4u; k++) lf[k] = l[k];
            l[0] = src(0, 96, 64, 0, 0, "rain ACT 96 (storm)"); l[1] = th;
            l[2] = src(4, 96, 64, 0, 0, "wind ACT 96 (storm)"); l[3] = src(3, 96, 64, 0, 0, "water ACT 96 (storm)");
            lanes(l, 4, "4 lanes storm");
            for (k = 0; k < 4u; k++) ls[k] = l[k];
            l[0] = src(3, 16, 64, "TYPE", 16, "drips ACT 16 (cave)"); l[1] = src(13, 64, 64, 0, 0, "minerals ACT 64 (cave)");
            l[2] = def[3]; l[3] = src(4, 32, 64, 0, 0, "wind ACT 32 (cave)");
            lanes(l, 4, "4 lanes cave");
            for (k = 0; k < 4u; k++) lc[k] = l[k];
            {   /* the storm and the worst four with effects left out (TYPE: a mask, txfm1_core.c FM_NO_*): what each
                 * part of the effects costs in the mix */
                static const struct { unsigned m; const char *t; } OFF[] = {
                    {FM_NO_GRAIN, "no GRAIN"}, {FM_NO_REVERB, "no REVERB"},
                    {FM_NO_DRIVE | FM_NO_FILTER, "no DRIVE, FILTER"},
                    {FM_NO_GRAIN | FM_NO_REVERB | FM_NO_DRIVE | FM_NO_FILTER, "no effects"}};
                char lab[96];
                for (k = 0; k < 4u; k++) {
                    snprintf(lab, sizeof lab, "4 lanes storm, %s", OFF[k].t);
                    lanes(ls, 4, lab);
                    cfg[nc - 1u].type = (uint8_t)OFF[k].m;
                }
                for (k = 0; k < 4u; k++) {
                    snprintf(lab, sizeof lab, "4 lanes worst, %s", OFF[k].t);
                    lanes(lw, 4, lab);
                    cfg[nc - 1u].type = (uint8_t)OFF[k].m;
                }
            }
            fx(FM_FX_CLOCK, 0, 0, "CLOCK probe (16384 dependent adds: MHz = 16384 / mean us)");
            /* core 1 against core 0 (FM_K_C1): the ping (no engine) and each scene for 5 s (a fault shows fast), then
             * the worst scenes a minute each, then minerals for five */
            c1(worst[1], 5, 1, "C1 ping (no engine), 5 s");
            c1(worst[1], 5, 2, "C1 math ping (64-bit products, divisions), 5 s");
            c1(worst[1], 5, 3, "C1 flash ping (a table in the flash), 5 s");
            c1(worst[1], 5, 4, "C1 shared ping (data core 0 just wrote), 5 s");
            c1(worst[1], 5, 5, "C1 load (pointer chase, 16 KB each core), 5 s");
            c1(worst[1], 60, 5, "C1 load (pointer chase, 16 KB each core), 60 s");
            c1(worst[1], 5, 0, "C1 stones SPACING 0, 5 s");
            c1(worst[3], 5, 0, "C1 water TYPE 127, 5 s");
            c1(worst[13], 5, 0, "C1 minerals HARD 127, 5 s");
            c1(worst[0], 5, 0, "C1 rain TYPE 40, 5 s");
            c1(worst[0], 60, 0, "C1 rain TYPE 40, 60 s");
            c1(worst[3], 60, 0, "C1 water TYPE 127, 60 s");
            c1(worst[13], 60, 0, "C1 minerals HARD 127, 60 s");
            c1(worst[1], 60, 0, "C1 stones SPACING 0, 60 s");
            c1(worst[13], 300, 0, "C1 minerals HARD 127, 300 s");
            {   /* the lanes on two cores (FM_PAR: as firmware/src/lanes.c), each beside the same on one, the grain
                 * delay's 15 grains forced: the hashes must match (and the host's, one core); then longer runs */
                static const char *const NM[4] = {"worst (stress)", "forest", "storm", "cave"};
                const unsigned *L4[4] = {lw, lf, ls, lc};
                char lab[96];
                for (k = 0; k < 4u; k++) {
                    snprintf(lab, sizeof lab, "4 lanes %s, 15 grains, one core", NM[k]);
                    lanes(L4[k], 4, lab); cfg[nc - 1u].type = FM_G15;
                    snprintf(lab, sizeof lab, "4 lanes %s, 15 grains, two cores", NM[k]);
                    lanes(L4[k], 4, lab); cfg[nc - 1u].type = FM_G15 | FM_PAR;
                }
                lanes(ls, 4, "4 lanes storm, 15 grains, two cores, 60 s"); cfg[nc - 1u].type = FM_G15 | FM_PAR;
                cfg[nc - 1u].nblk = (uint16_t)((60u * TX_FS) / TX_BLOCK);
                lanes(lw, 4, "4 lanes worst (stress), 15 grains, two cores, 300 s"); cfg[nc - 1u].type = FM_G15 | FM_PAR;
                cfg[nc - 1u].nblk = (uint16_t)((300u * TX_FS) / TX_BLOCK);
            }
        }
    }
    if (!(f = fopen(argv[1], "w"))) { perror(argv[1]); return 2; }
    fprintf(f, "/* generated by bench/fm1/txfm1_gen.c: do not edit */\n#define FM_NCFG %uu\n", nc);
    fprintf(f, "static const fm_ev_t FM_EV[%u] = {\n", ne ? ne : 1u);
    for (i = 0; i < ne; i++) fprintf(f, "  {%u,%u,%u,%u,%u},\n", ev[i].blk, ev[i].kind, ev[i].id, ev[i].a, ev[i].b);
    if (!ne) fprintf(f, "  {0,0,0,0,0},\n");
    fprintf(f, "};\nstatic const fm_cfg_t FM_CFG[FM_NCFG] = {\n");
    for (i = 0; i < nc; i++)
        fprintf(f, "  {%u,%u,%u,%u,%u,%u,%u,{%u,%u,%u,%u},%uu},\n", cfg[i].kind, cfg[i].eng, cfg[i].type, cfg[i].decay,
                cfg[i].ev, cfg[i].nev, cfg[i].nblk, cfg[i].lane[0], cfg[i].lane[1], cfg[i].lane[2], cfg[i].lane[3],
                cfg[i].seed);
    fprintf(f, "};\n#ifdef FM_LABELS\nstatic const char *const FM_LABEL[FM_NCFG] = {\n");
    for (i = 0; i < nc; i++) fprintf(f, "  \"%s\",\n", label[i]);
    fprintf(f, "};\n#endif\n");
    fclose(f);
    fprintf(stderr, "txfm1_gen: %u configurations, %u events -> %s\n", nc, ne, argv[1]);
    return 0;
}
