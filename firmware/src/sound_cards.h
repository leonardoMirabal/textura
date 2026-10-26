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
 * A classic engine (storm, thunder) listed here: its cards are its own parameters (CK_SRC: the lane's engine), less the
 * ones its activity map moves (landscapes.h LS_QUICK). A sound not listed gets the source's general cards
 * (SC_SRC_DEFAULT), a classic engine all its own parameters less those.
 *
 * Water: the names and order of the user's approved study (2026-10-26). The source's EXTRA knob is the sound's own
 * extra (CAVE on cave drips, ROAR on the streams, PERIOD on the waves); water drops are the source's own drops, their
 * HEIGHT and POOL on its HARD and TONE (the drip's splash is part of every drop, not a card); rain on water's HARD
 * above the middle is WIND. A card whose parameter this build's engine has not got is left out. Still to come with the engines: RAIN (rain's own rate apart
 * from HOW ACTIVE), EAVES, DROP, SHORE, TRAINS.
 *
 * Storm: the names and order of the approved study (2026-10-26): thunder's JAGGED is its TORT, the gales' EXPOSED the
 * wind's BUFFET, hail's STONES the rain's DROPS. Listed ahead of their engines, and left out until the engine has
 * the parameter by that name: rain on water's AREA, rain on a roof's EAVES and WIND, hail's and sleet's RAIN. Hail has no
 * SURFACE card (its kind is the sound).
 *
 * Wind: the approved study's ranking (2026-10-26), four on the first page, by the place: the trees' FOLIAGE early, the
 * open places' EXPOSED (the wind's BUFFET); winter's and the desert's DRIFT; autumn's SKITTER (the dry leaves over the
 * ground, left out until the engine has it). The source's EXTRA, SPACING and DEPTH do nothing on a wind, so no general
 * card is left. The gales in the Storm landscape keep their cards (by the sound's name). */
#ifndef TX_SOUND_CARDS_H
#define TX_SOUND_CARDS_H
#include <stdint.h>

#define SCD_MAX 8u                              /* cards a sound has at most (two screens) */
enum { CK_SRC = 0, CK_HOST = 1, CK_INV = 2 };
typedef struct { const char *lab, *par; uint8_t kind; } scd_card_t;
typedef struct { const char *sound, *hosted; uint8_t n; scd_card_t c[SCD_MAX]; } scd_sound_t;

#define DIST_CARD {"DIST", "PROXIMITY", CK_INV}
static const scd_sound_t SCD[] = {
    {"rain on water", 0, 7, {{"DROPS", "SIZE", 0}, DIST_CARD, {"SURFACE", "TONE", 0}, {"WIND", "HARD", 0},
                             {"CLUMPS", "SPACING", 0}, {"LIFE", "LIFE", 0}, {"AREA", "AREA", 0}}},
    {"rain on a roof", "rainsurf", 7, {{"SURFACE", "SURFACE", CK_HOST}, {"DROPS", "DROPS", CK_HOST},
                                       {"INSIDE", "INSIDE", CK_HOST}, {"AREA", "AREA", CK_HOST}, DIST_CARD,
                                       {"EAVES", "EAVES", CK_HOST}, {"WIND", "WIND", CK_HOST}}},
    {"water drops", 0, 7, {{"SIZE", "SIZE", 0}, {"HEIGHT", "HARD", 0}, {"POOL", "TONE", 0}, {"SCATTER", "SPREAD", 0},
                           {"CLUMPS", "SPACING", 0}, {"LIFE", "LIFE", 0}, DIST_CARD}},
    {"cave drips", "drip", 7, {{"SIZE", "SIZE", CK_HOST}, {"HEIGHT", "HEIGHT", CK_HOST}, {"POOL", "DEPTH", CK_HOST},
                               {"CAVE", "EXTRA", 0}, {"SCATTER", "SCATTER", CK_HOST}, {"RHYTHM", "CLUMP", CK_HOST}, DIST_CARD}},
    {"eaves dripping", "drip", 6, {{"SIZE", "SIZE", CK_HOST}, {"HEIGHT", "HEIGHT", CK_HOST}, {"RHYTHM", "CLUMP", CK_HOST},
                                   {"POOL", "DEPTH", CK_HOST}, {"SCATTER", "SCATTER", CK_HOST}, DIST_CARD}},
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
    /* Storm */
    {"storm", 0, 8, {{"RAIN", "RAIN", 0}, {"WIND", "WIND", 0}, {"FLASHES", "FLASHES", 0}, {"DIST", "DIST", 0},
                     {"AGE", "AGE", 0}, {"SURFACE", "SURFACE", 0}, {"PASSING", "PASSING", 0}, {"INSIDE", "INSIDE", 0}}},
    {"thunder", 0, 3, {{"JAGGED", "TORT", 0}, {"HEIGHT", "HEIGHT", 0}, {"GROUND", "GROUND", 0}}},
    /* Wind (and the gales in Storm) */
#define W_GUST {"GUST", "GUST", CK_HOST}
#define W_SCALE {"SCALE", "SCALE", CK_HOST}
#define W_EXPOSED {"EXPOSED", "BUFFET", CK_HOST}
#define W_FOLIAGE {"FOLIAGE", "FOLIAGE", CK_HOST}
#define W_WHOOSH {"WHOOSH", "WHOOSH", CK_HOST}
#define W_EDGE {"EDGE", "EDGE", CK_HOST}
#define W_DRIFT {"DRIFT", "DRIFT", CK_HOST}
#define W_TREES {W_GUST, W_FOLIAGE, W_SCALE, W_EXPOSED, W_WHOOSH, W_EDGE, DIST_CARD}
#define W_OPEN {W_GUST, W_EXPOSED, W_SCALE, W_WHOOSH, W_FOLIAGE, W_EDGE, DIST_CARD}
    {"wind", "wind", 7, W_TREES},
    {"wind in pines", "wind", 7, W_TREES},
    {"leafy trees", "wind", 7, W_TREES},
    {"breeze in grass", "wind", 6, {W_GUST, W_SCALE, W_FOLIAGE, W_WHOOSH, W_EXPOSED, DIST_CARD}},
    {"open moor", "wind", 7, W_OPEN},
    {"ridge gale", "wind", 7, W_OPEN},
    {"sea wind", "wind", 6, {W_GUST, W_EXPOSED, W_WHOOSH, W_SCALE, W_FOLIAGE, DIST_CARD}},
    {"gusty autumn", "wind", 7, {W_GUST, W_FOLIAGE, {"SKITTER", "SKITTER", CK_HOST}, W_SCALE, W_EXPOSED, W_WHOOSH,
                                 DIST_CARD}},
    {"winter storm", "wind", 8, {W_GUST, W_EXPOSED, W_DRIFT, W_SCALE, W_WHOOSH, W_FOLIAGE, W_EDGE, DIST_CARD}},
    {"desert wind", "wind", 6, {W_DRIFT, W_GUST, W_EXPOSED, W_WHOOSH, W_FOLIAGE, DIST_CARD}},
    /* Storm's hail */
#define HAIL_CARDS {{"STONES", "DROPS", CK_HOST}, {"AREA", "AREA", CK_HOST}, {"INSIDE", "INSIDE", CK_HOST}, \
                    {"RAIN", "RAIN", CK_HOST}, {"BOUNCE", "BOUNCE", CK_HOST}, DIST_CARD}
    {"hail on a roof", "rainsurf", 6, HAIL_CARDS},
    {"hail on a car", "rainsurf", 6, HAIL_CARDS},
    {"hail in leaves", "rainsurf", 6, HAIL_CARDS},
    {"sleet on a window", "rainsurf", 5, {{"STONES", "DROPS", CK_HOST}, {"AREA", "AREA", CK_HOST},
                                          {"INSIDE", "INSIDE", CK_HOST}, DIST_CARD, {"RAIN", "RAIN", CK_HOST}}},
};
#define SCD_N (sizeof SCD / sizeof SCD[0])
/* the source's general cards (every sound not in SCD) */
static const scd_sound_t SC_SRC_DEFAULT = {"", 0, 8, {{"SIZE", "SIZE", 0}, {"SPREAD", "SPREAD", 0}, {"TONE", "TONE", 0},
    {"HARD", "HARD", 0}, {"NEAR", "PROXIMITY", 0}, {"DEPTH", "DEPTH", 0}, {"LIFE", "LIFE", 0}, {"CLUMPS", "SPACING", 0}}};

#endif
