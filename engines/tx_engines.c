/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura engine registry: every engine in the build (the player's selector and the bench list come from here). */
#include "tx_engine.h"

extern const tx_engine_t tx_eng_test;
extern const tx_engine_t tx_eng_drip;
extern const tx_engine_t tx_eng_candle;
extern const tx_engine_t tx_eng_sparks;
extern const tx_engine_t tx_eng_gravel;
extern const tx_engine_t tx_eng_rainstick;
extern const tx_engine_t tx_eng_mineral;
extern const tx_engine_t tx_eng_pebble;
extern const tx_engine_t tx_eng_thunder;
extern const tx_engine_t tx_eng_bees;
extern const tx_engine_t tx_eng_sandsong;
extern const tx_engine_t tx_eng_aeolian;
extern const tx_engine_t tx_eng_sand;
extern const tx_engine_t tx_eng_lakeice;
extern const tx_engine_t tx_eng_spring;
extern const tx_engine_t tx_eng_chimes;
extern const tx_engine_t tx_eng_rainsurf;
extern const tx_engine_t tx_eng_rockfall;
extern const tx_engine_t tx_eng_crickets;
extern const tx_engine_t tx_eng_cicadas;
extern const tx_engine_t tx_eng_katydids;

const tx_engine_t *const tx_engines[] = {
    &tx_eng_test,
    &tx_eng_drip,
    &tx_eng_candle,
    &tx_eng_sparks,
    &tx_eng_gravel,
    &tx_eng_rainstick,
    &tx_eng_mineral,
    &tx_eng_pebble,
    &tx_eng_thunder,
    &tx_eng_bees,
    &tx_eng_sandsong,
    &tx_eng_aeolian,
    &tx_eng_sand,
    &tx_eng_lakeice,
    &tx_eng_spring,
    &tx_eng_chimes,
    &tx_eng_rainsurf,
    &tx_eng_rockfall,
    &tx_eng_crickets,
    &tx_eng_cicadas,
    &tx_eng_katydids,
    0,
};

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int same(const char *a, const char *b)
{
    while (*a && lower(*a) == lower(*b)) { a++; b++; }
    return !*a && !*b;
}

const tx_engine_t *tx_engine_find(const char *name)
{
    unsigned i;
    for (i = 0; tx_engines[i]; i++)
        if (same(tx_engines[i]->name, name)) return tx_engines[i];
    return 0;
}

int tx_param_find(const tx_engine_t *e, const char *name)
{
    unsigned i;
    for (i = 0; i < e->n_params; i++)
        if (same(e->params[i].name, name)) return (int)i;
    return -1;
}
