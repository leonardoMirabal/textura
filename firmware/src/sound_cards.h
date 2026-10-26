/* SPDX-License-Identifier: GPL-3.0-only */
/* The SOUND pages' cards, per sound: each card a physical name (8 characters at most) and the parameter it turns.
 * A card turns one of:
 *   - a parameter of the source itself (SIZE, TONE, PROXIMITY ...), by name;
 *   - a parameter of the engine the source hosts for this sound (CK_HOST: the drip's HEIGHT, the stream's ROAR ...),
 *     set over the source's own curves until the sound changes (tx_eng_source.c tx_source_host_param);
 *   - CK_INV: the card's value turned around (DIST far = PROXIMITY near).
 * HOW MANY and HOW ACTIVE stay on the keys (COUNT and ACTIVITY), so no card takes them; a rate that IS the activity
 * (the drips' RATE, the stream's FLOW) is not a card either.
 *
 * A sound not listed here gets the source's general cards (SC_SRC_DEFAULT); a classic engine (storm, thunder) gets
 * its own parameters, less the ones its activity map moves (landscapes.h LS_QUICK).
 *
 * Water: the names and order of the user's approved study (2026-10-26). The source's EXTRA knob is the sound's own
 * extra (CAVE on cave drips, ROAR on the streams, PERIOD on the waves); water drops host the drip, its HEIGHT and POOL
 * on the source's HARD and TONE; rain on water's HARD above the middle is WIND. A card whose parameter this build's
 * engine has not got (drip's SPLASH, say) is left out. Still to come with the engines: RAIN (rain's own rate apart
 * from HOW ACTIVE), EAVES, DROP, SHORE, TRAINS. */
#ifndef TX_SOUND_CARDS_H
#define TX_SOUND_CARDS_H
#include <stdint.h>

#define SCD_MAX 8u                              /* cards a sound has at most (two screens) */
enum { CK_SRC = 0, CK_HOST = 1, CK_INV = 2 };
typedef struct { const char *lab, *par; uint8_t kind; } scd_card_t;
typedef struct { const char *sound, *hosted; uint8_t n; scd_card_t c[SCD_MAX]; } scd_sound_t;

#define DIST_CARD {"DIST", "PROXIMITY", CK_INV}
static const scd_sound_t SCD[] = {
    {"rain on water", 0, 6, {{"DROPS", "SIZE", 0}, DIST_CARD, {"SURFACE", "TONE", 0}, {"CLUMPS", "SPACING", 0},
                             {"LIFE", "LIFE", 0}, {"WIND", "HARD", 0}}},
    {"rain on a roof", "rainsurf", 5, {{"SURFACE", "SURFACE", CK_HOST}, {"DROPS", "DROPS", CK_HOST},
                                       {"INSIDE", "INSIDE", CK_HOST}, {"AREA", "AREA", CK_HOST}, DIST_CARD}},
    {"water drops", "drip", 7, {{"SIZE", "SIZE", CK_HOST}, {"HEIGHT", "HARD", 0}, {"POOL", "TONE", 0},
                                {"SCATTER", "SCATTER", CK_HOST}, {"RHYTHM", "CLUMP", CK_HOST}, {"SPLASH", "SPLASH", CK_HOST},
                                DIST_CARD}},
    {"cave drips", "drip", 8, {{"SIZE", "SIZE", CK_HOST}, {"HEIGHT", "HEIGHT", CK_HOST}, {"POOL", "DEPTH", CK_HOST},
                               {"CAVE", "EXTRA", 0}, {"SCATTER", "SCATTER", CK_HOST}, {"RHYTHM", "CLUMP", CK_HOST},
                               {"SPLASH", "SPLASH", CK_HOST}, DIST_CARD}},
    {"eaves dripping", "drip", 7, {{"SIZE", "SIZE", CK_HOST}, {"HEIGHT", "HEIGHT", CK_HOST}, {"RHYTHM", "CLUMP", CK_HOST},
                                   {"POOL", "DEPTH", CK_HOST}, {"SCATTER", "SCATTER", CK_HOST},
                                   {"SPLASH", "SPLASH", CK_HOST}, DIST_CARD}},
    {"brook", "stream", 6, {{"SIZE", "SIZE", CK_HOST}, {"SURFACE", "NEAR", CK_HOST}, {"GURGLE", "EDDY", CK_HOST},
                            {"SPREAD", "SPREAD", CK_HOST}, {"ROAR", "EXTRA", 0}, DIST_CARD}},
    {"bubbling spring", "stream", 6, {{"SIZE", "SIZE", CK_HOST}, {"SURFACE", "NEAR", CK_HOST},
                                      {"SPREAD", "SPREAD", CK_HOST}, {"GURGLE", "EDDY", CK_HOST}, {"ROAR", "EXTRA", 0},
                                      DIST_CARD}},
    {"waterfall", "stream", 6, {{"ROAR", "EXTRA", 0}, {"SIZE", "SIZE", CK_HOST}, DIST_CARD,
                                {"SURFACE", "NEAR", CK_HOST}, {"SPREAD", "SPREAD", CK_HOST}, {"GURGLE", "EDDY", CK_HOST}}},
    {"waves on sand", "surf", 6, {{"HEIGHT", "HEIGHT", CK_HOST}, {"PERIOD", "EXTRA", 0}, {"DIST", "DIST", CK_HOST},
                                  {"BACKWSH", "BACKWASH", CK_HOST}, {"CHOP", "CHOP", CK_HOST}, {"FLOOR", "FLOOR", CK_HOST}}},
    {"waves on shingle", "surf", 6, {{"HEIGHT", "HEIGHT", CK_HOST}, {"PERIOD", "EXTRA", 0}, {"DIST", "DIST", CK_HOST},
                                     {"BACKWSH", "BACKWASH", CK_HOST}, {"CHOP", "CHOP", CK_HOST}, {"FLOOR", "FLOOR", CK_HOST}}},
};
#define SCD_N (sizeof SCD / sizeof SCD[0])
/* the source's general cards (every sound not in SCD) */
static const scd_sound_t SC_SRC_DEFAULT = {"", 0, 8, {{"SIZE", "SIZE", 0}, {"SPREAD", "SPREAD", 0}, {"TONE", "TONE", 0},
    {"HARD", "HARD", 0}, {"NEAR", "PROXIMITY", 0}, {"DEPTH", "DEPTH", 0}, {"LIFE", "LIFE", 0}, {"CLUMPS", "SPACING", 0}}};

#endif
