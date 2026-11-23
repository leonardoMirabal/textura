/* SPDX-License-Identifier: GPL-3.0-only */
/* The factory presets: PR_N places, each four lanes of sounds from any family, grouped in PR_NCAT categories (the
 * table: presets.h, written by tools/gen_presets.py from firmware/presets.txt; the levels there are measured, the
 * tool says how). They are the factory scenes F001 .. (PR_N of them): a load goes through scenes.c (scn_load: the lanes, then
 * the records), so a preset is a scene like any other, and a user scene saved from one keeps what was changed.
 *
 * A preset holds, per lane: the sound (its family and its index there, or none), HOW MANY (0 off .. 6), HOW ACTIVE
 * (the key 0 .. 8), the LEVEL (0 .. 127, 100 = the sound as tuned), the PAN (0 .. 127) and the mute; and one byte for
 * its category and its effects (an index into PR_FX, 16 sets at most: a touch of reverb, an echo, none, or a place
 * beyond the real one: the vast reverbs, SHIMMER, a grain cloud, the FILTER and the M/S's SIDE with them; each set the
 * values it moves off their defaults, scenes.c scn_factory FX_ID its columns). The levels and pans are
 * plain values (no level hidden in another parameter), so a load can glide them. The effects come with a load only
 * where the scene's effects do (EFFECTS ON LOAD, the boot's load: scenes.c); kept, they stay as they are.
 *
 * For the browser (ui.c):
 *   pr_count()            the presets (PR_N)
 *   pr_cat_count()        the categories (PR_NCAT)
 *   pr_cat_name(c)        "RAIN", "STORM" ...
 *   pr_cat_first(c), pr_cat_n(c)   the category's presets: pr_cat_first(c) .. + pr_cat_n(c) - 1 (the table is in
 *                         category order)
 *   pr_cat_of(i)          preset i's category
 *   pr_name(i)            "SOFT RAIN" (11 characters at most)
 *   pr_lane(i, k, &l)     lane k of preset i, decoded (pr_lane_v_t)
 *   pr_load(i)            loads preset i as a MIDI program change does (scenes.c: at once, or when the load still
 *                         running is done; the last one asked wins); 0, or -1 out of range
 *
 * Main loop only; the table is const (flash). */
#ifndef PR_TABLE
#define PR_TABLE "presets.h"                    /* (a measurement build gives its own table: tools/pr_level.py) */
#endif
#include PR_TABLE

typedef struct {
    int8_t fam, snd;                            /* the sound (-1, -1: none on this lane) */
    uint8_t cnt, act, vol, pan, mute;           /* HOW MANY 0 .. 6, HOW ACTIVE key 0 .. 8 (cnt 0: loaded, not playing) */
} pr_lane_v_t;

static uint32_t pr_count(void) { return PR_N; }
static uint32_t pr_cat_count(void) { return PR_NCAT; }
static const char *pr_cat_name(uint32_t c) { return c < PR_NCAT ? PR_CAT[c] : ""; }
static uint32_t pr_cat_first(uint32_t c) { return c < PR_NCAT ? PR_CAT_AT[c] : PR_N; }
static uint32_t pr_cat_n(uint32_t c) { return c < PR_NCAT ? (uint32_t)(PR_CAT_AT[c + 1u] - PR_CAT_AT[c]) : 0u; }
static uint32_t pr_cat_of(uint32_t i) { return i < PR_N ? (uint32_t)(PR[i].cf >> 4) : 0u; }
static uint32_t pr_fx_of(uint32_t i) { return i < PR_N ? (uint32_t)(PR[i].cf & 15u) : 0u; }
static const char *pr_name(uint32_t i) { return i < PR_N ? PR[i].name : ""; }
static void pr_lane(uint32_t i, uint32_t k, pr_lane_v_t *o)
{
    const pr_lane_t *l;
    o->fam = o->snd = -1; o->cnt = o->act = o->mute = 0; o->vol = 100; o->pan = 64;
    if (i >= PR_N || k >= 4u) return;
    l = &PR[i].l[k];
    o->vol = l->vol; o->pan = l->pan;
    if (l->fs == 0xFFu || (l->fs >> 4) >= LS_N || (l->fs & 15u) >= LS[l->fs >> 4].n) return;
    o->fam = (int8_t)(l->fs >> 4); o->snd = (int8_t)(l->fs & 15u);
    o->cnt = (uint8_t)(l->ca >> 4 & 7u); o->act = (uint8_t)(l->ca & 15u); o->mute = (uint8_t)(l->ca >> 7);
    if (o->cnt >= LS_NCNT) o->cnt = LS_NCNT - 1u;
    if (o->act >= LS_NACT) o->act = LS_NACT - 1u;
}
static int pr_load(uint32_t i);                 /* (scenes.c) */
