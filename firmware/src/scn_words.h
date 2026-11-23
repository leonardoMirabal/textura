/* SPDX-License-Identifier: GPL-3.0-only */
/* A new user scene's name: two words of places and their weather, "MOSSY DUSK", "SALT WIND", "EMBER COVE". Each word
 * five letters at most, so the two with their space fit the 11 characters a scene's name has (scenes.c), all from the
 * save mode's characters. Never the name of a user scene already saved (taken): another draw, a few times, then a
 * shorter pair with a digit after it ("FAR BOG 2"). The randomness is the caller's (rnd): the FM-1's dice (dice.c
 * dice_scn_name), the plugin's own seed (plugin/src/textura_clap.c txp_ui_name). Included by both; const, in flash;
 * integers only. */
#ifndef SCN_WORDS_H
#define SCN_WORDS_H
#include <stdint.h>

static const char SCN_WA[][6] = {
    "AMBER", "ASHEN", "BLUE", "BRINE", "CEDAR", "CLAY", "COLD", "DAMP", "DARK", "DEEP", "DRY", "DUSTY", "EARLY",
    "EMBER", "FAR", "FERN", "FOGGY", "FROST", "GREY", "HAZY", "HIGH", "ICY", "IRON", "LATE", "LOW", "MISTY", "MOSSY",
    "NIGHT", "OLD", "OPEN", "PALE", "PINE", "QUIET", "RAINY", "REED", "SALT", "SANDY", "SLATE", "SOFT", "STILL",
    "STONY", "WARM", "WET", "WILD"
};
static const char SCN_WB[][6] = {
    "BANK", "BAY", "BOG", "BROOK", "CAVE", "COAST", "COVE", "CREEK", "DAWN", "DELTA", "DELL", "DUNE", "DUSK", "FALLS",
    "FEN", "FIELD", "FJORD", "FORD", "GALE", "GLADE", "GLEN", "GORGE", "GROVE", "HAIL", "HEATH", "HILL", "ISLE",
    "LAKE", "LEDGE", "MARSH", "MERE", "MOOR", "PASS", "POND", "POOL", "RAIN", "REEF", "RIDGE", "SHORE", "SPRAY",
    "SURF", "TIDE", "VALE", "WIND", "WOODS"
};
#define SCN_WA_N ((uint32_t)(sizeof SCN_WA / sizeof SCN_WA[0]))
#define SCN_WB_N ((uint32_t)(sizeof SCN_WB / sizeof SCN_WB[0]))
#define SCN_W_TRIES 8u                          /* draws before the digit */

typedef uint32_t (*scn_rnd_f)(void *ctx);       /* a random 32-bit number */
typedef int (*scn_taken_f)(const char *nm, void *ctx);   /* 1: a user scene has that name already (or it is the one
                                                 * shown now) */

static uint32_t scn_w_pair(char out[12], scn_rnd_f rnd, void *ctx)   /* two words drawn; their length */
{
    const char *a = SCN_WA[(rnd(ctx) >> 8) % SCN_WA_N], *b = SCN_WB[(rnd(ctx) >> 8) % SCN_WB_N];
    uint32_t n = 0;
    while (*a) out[n++] = *a++;
    out[n++] = ' ';
    while (*b) out[n++] = *b++;
    out[n] = 0;
    return n;
}
static void scn_words(char out[12], scn_rnd_f rnd, scn_taken_f taken, void *ctx)
{
    uint32_t i, n = 0, d;
    for (i = 0; i < SCN_W_TRIES; i++) {
        (void)scn_w_pair(out, rnd, ctx);
        if (!taken(out, ctx)) return;
    }
    for (i = 0; i < 4u * SCN_W_TRIES && (n = scn_w_pair(out, rnd, ctx)) > 9u; i++) {}   /* (room for " 2") */
    if (n > 9u) { n = 9u; out[9] = 0; }
    for (d = 2u; d <= 9u; d++) {
        out[n] = ' '; out[n + 1u] = (char)('0' + d); out[n + 2u] = 0;
        if (!taken(out, ctx)) return;
    }
}
#endif
