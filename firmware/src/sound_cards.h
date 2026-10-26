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
 * ground). The source's EXTRA, SPACING and DEPTH do nothing on a wind, so no general
 * card is left. The gales in the Storm landscape keep their cards (by the sound's name).
 *
 * Fire: the approved study's ranking (2026-10-26), four on the first page: SIZE, MOIST, DRAUGHT, WOOD on the open
 * fires (MOIST first on kindling and leaves, where the wet is the sound), the fireplace's ROAR (the chimney's draw)
 * third; embers MOIST, WOOD, BURSTY, DIST (no flame: no ROAR); the torch ROAR first. The flame is the candle: FLOW,
 * DRAUGHT, SPUTTER, SLOPE. The source's EXTRA, SPACING and DEPTH do nothing on a fire, so no general card is left.
 *
 * Insects: the approved study's ranking (2026-10-26). Crickets TEMP, CHORUS (their FIELD: the far chorus), SYNC; the
 * katydid's RASP is its JITTER, the grasshopper's PEGS first. The cicadas are the source's own callers: their cards
 * are its knobs by the cicada's names (SIZE the body, RING the tymbal's Q = TONE, BUZZ = HARD, DEPTH the chorus spread
 * in distance). Bees: SWARM, SPEED, the swarm's wingbeat SPREAD. The crickets' and katydids' SIZE (the body: the
 * carrier +-0.5 oct, bigger is lower) on their second page. EXTRA, SPACING and DEPTH (but the cicadas') do nothing on
 * an insect.
 *
 * Songbirds: the approved study's ranking (2026-10-26), one page for all ten, the source's own birds: SIZE (the bird's
 * size: its song's pitch), DEPTH (one bird at DIST .. birds near and far at once), DIST, VARY (= SPREAD: the birds
 * alike .. each its own pitch). The wild birds (approved 2026-10-26) the same four. Page 2 how they sing, on the
 * source's free knobs (tx_core_bird tx_bird_ctl_t): TEMPO (SPACING: faster up), SONG (EXTRA: longer songs), VARIETY
 * (TONE: a bigger repertoire, fewer repeats), EXCITE (HARD: an excited bird, shorter pauses, louder; the colony's
 * birds answer more). A wild bird's cards by what it does: the crow's caw SERIES, the gull's CALLS and LONGCALL (its
 * long call over the short ones), the curlew's BUBBLE (the bubbling song over the calls), the oystercatcher's PIPING,
 * the cuckoo's CALLS in a series and its INTERVAL, the woodpecker's DRUM (the strike rate) and ROLL (its length), the
 * pigeon's PHRASES, the nightjar's RATE (the churr's pulses), CHURR (its length) and SWITCH (how often it changes),
 * the bittern's BOOMS in a bout. A knob that does nothing on a bird has no card.
 *
 * Stones & sand: the approved study's ranking (2026-10-26). The stones are the source's own particles: SIZE, GROUND
 * (= HARD: stone on stone .. soft ground), RING (= TONE: the stones' Q), CLUMPS (= SPACING). Ringing rocks' STONE is
 * the stonedrum's MATERIAL. Gravel's RATE, sand's ORIFICE and the dune's FLOW are the activity itself (on the keys).
 * Sand's SIZE is its container's (a tray, a bulb, a tube). Rockfall's DIST is the source's PROXIMITY, as everywhere.
 *
 * Ice & glass: the approved study's ranking (2026-10-26). The ice's SIZE is the icicle's length or the glass' lowest
 * mode, TEMP its temperature (ringing .. wet), FIZZ the melt. CLUMPS is the source's SPACING (bursts .. apart): dead on
 * the lake ices, whose cracks keep their own clock. The lake ices' FAR is the engine's own DIST, the crack's distance
 * (longer sweeps, a later air wave), apart from DIST (= PROXIMITY: the listener's level and air). Lake ice's RATE is
 * the activity itself (on the keys). Glass: SIZE, RING (the Q), ENERGY (a crack .. a smash), HEIGHT, BOUNCE.
 *
 * Wood: the approved study's ranking (2026-10-26). SIZE the piece's length (a twig .. a log), CREAK the stick-slip's
 * strength, PITCH its slip rate (ratchety .. singing), MOIST dry .. wet; the green branch's MOIST before PITCH, the
 * trunk's CREAK first (it creaks, it does not snap), the twigs underfoot SIZE and MOIST only. LOAD is the activity
 * (ACTIVITY's own curve: how fast the load builds to the snap), WOOD the kind: neither is a card. */
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
    /* Fire */
#define F_SIZE {"SIZE", "SIZE", CK_HOST}
#define F_MOIST {"MOIST", "MOIST", CK_HOST}
#define F_DRAUGHT {"DRAUGHT", "DRAUGHT", CK_HOST}
#define F_WOOD {"WOOD", "WOOD", CK_HOST}
#define F_ROAR {"ROAR", "ROAR", CK_HOST}
#define F_BURSTY {"BURSTY", "BURSTY", CK_HOST}
#define F_OPEN {F_SIZE, F_MOIST, F_DRAUGHT, F_WOOD, F_ROAR, F_BURSTY, DIST_CARD}
#define F_LIGHT {F_MOIST, F_SIZE, F_DRAUGHT, F_WOOD, F_ROAR, F_BURSTY, DIST_CARD}
    {"campfire", "fire", 7, F_OPEN},
    {"bonfire", "fire", 7, F_OPEN},
    {"pine fire", "fire", 7, F_OPEN},
    {"kindling", "fire", 7, F_LIGHT},
    {"leaves, paper", "fire", 7, F_LIGHT},
    {"fireplace", "fire", 7, {F_SIZE, F_MOIST, F_ROAR, F_WOOD, F_DRAUGHT, F_BURSTY, DIST_CARD}},
    {"embers", "fire", 5, {F_MOIST, F_WOOD, F_BURSTY, DIST_CARD, F_SIZE}},
    {"torch", "fire", 6, {F_ROAR, F_DRAUGHT, F_SIZE, F_MOIST, F_WOOD, DIST_CARD}},
    {"flame", "candle", 5, {{"FLOW", "FLOW", CK_HOST}, {"DRAUGHT", "DRAUGHT", CK_HOST}, {"SPUTTER", "SPUTTER", CK_HOST},
                            {"SLOPE", "SLOPE", CK_HOST}, DIST_CARD}},
    /* Insects */
#define I_TEMP {"TEMP", "TEMP", CK_HOST}
#define I_CHORUS {"CHORUS", "FIELD", CK_HOST}
#define I_SYNC {"SYNC", "SYNC", CK_HOST}
#define I_RASP {"RASP", "JITTER", CK_HOST}
#define I_SIZE {"SIZE", "SIZE", CK_HOST}
#define C_CARDS(a, b, c, d) {a, b, c, d, {"BUZZ", "HARD", 0}}
#define C_SIZE {"SIZE", "SIZE", 0}
#define C_RING {"RING", "TONE", 0}
#define C_DEPTH {"DEPTH", "DEPTH", 0}
#define B_CARDS {{"SWARM", "SWARM", CK_HOST}, {"SPEED", "SPEED", CK_HOST}, DIST_CARD, {"SPREAD", "SPREAD", CK_HOST}}
    {"field cricket", "crickets", 5, {I_TEMP, I_CHORUS, I_SYNC, DIST_CARD, I_SIZE}},
    {"tree cricket", "crickets", 5, {I_TEMP, I_CHORUS, I_SYNC, DIST_CARD, I_SIZE}},
    {"mole cricket", "crickets", 5, {I_TEMP, I_CHORUS, DIST_CARD, I_SYNC, I_SIZE}},
    {"cicada", 0, 5, C_CARDS(C_SIZE, C_RING, C_DEPTH, DIST_CARD)},
    {"small cicada", 0, 5, C_CARDS(C_SIZE, C_RING, C_DEPTH, DIST_CARD)},
    {"cicada chorus", 0, 5, C_CARDS(C_DEPTH, C_SIZE, C_RING, DIST_CARD)},
    {"katydid", "katydids", 6, {I_TEMP, I_RASP, I_CHORUS, DIST_CARD, I_SYNC, I_SIZE}},
    {"grasshopper", "katydids", 7, {{"PEGS", "PEGS", CK_HOST}, I_TEMP, I_RASP, DIST_CARD, I_CHORUS, I_SYNC, I_SIZE}},
    {"honeybee", "bees", 4, B_CARDS},
    {"bumblebee", "bees", 4, B_CARDS},
    /* Songbirds */
#define SB_PAGE1 {"SIZE", "SIZE", 0}, {"DEPTH", "DEPTH", 0}, DIST_CARD, {"VARY", "SPREAD", 0}
#define SB_SING {"TEMPO", "SPACING", 0}, {"SONG", "EXTRA", 0}, {"VARIETY", "TONE", 0}, {"EXCITE", "HARD", 0}
    {"songbird", 0, 8, {SB_PAGE1, SB_SING}},
    {"blackbird", 0, 8, {SB_PAGE1, SB_SING}},
    {"song thrush", 0, 8, {SB_PAGE1, SB_SING}},
    {"nightingale", 0, 8, {SB_PAGE1, SB_SING}},
    {"robin", 0, 8, {SB_PAGE1, SB_SING}},
    {"wren", 0, 8, {SB_PAGE1, SB_SING}},
    {"hermit thrush", 0, 8, {SB_PAGE1, SB_SING}},
    {"skylark", 0, 8, {SB_PAGE1, SB_SING}},
    {"wood thrush", 0, 8, {SB_PAGE1, SB_SING}},
    {"reed warbler", 0, 8, {SB_PAGE1, SB_SING}},
    /* Wild birds */
    {"chirper", 0, 8, {SB_PAGE1, SB_SING}},
    {"crow", 0, 7, {SB_PAGE1, {"TEMPO", "SPACING", 0}, {"SERIES", "EXTRA", 0}, {"EXCITE", "HARD", 0}}},
    {"gull", 0, 8, {SB_PAGE1,
                    {"TEMPO", "SPACING", 0}, {"CALLS", "EXTRA", 0}, {"LONGCALL", "TONE", 0}, {"EXCITE", "HARD", 0}}},
    {"curlew", 0, 8, {SB_PAGE1,
                      {"TEMPO", "SPACING", 0}, {"SONG", "EXTRA", 0}, {"BUBBLE", "TONE", 0}, {"EXCITE", "HARD", 0}}},
    {"oystercatcher", 0, 8, {SB_PAGE1,
        {"TEMPO", "SPACING", 0}, {"SONG", "EXTRA", 0}, {"PIPING", "TONE", 0}, {"EXCITE", "HARD", 0}}},
    {"cuckoo", 0, 8, {SB_PAGE1,
                      {"TEMPO", "SPACING", 0}, {"CALLS", "EXTRA", 0}, {"INTERVAL", "TONE", 0}, {"EXCITE", "HARD", 0}}},
    {"woodpecker", 0, 7, {SB_PAGE1, {"DRUM", "SPACING", 0}, {"ROLL", "EXTRA", 0}, {"EXCITE", "HARD", 0}}},
    {"wood pigeon", 0, 7, {SB_PAGE1, {"TEMPO", "SPACING", 0}, {"PHRASES", "EXTRA", 0}, {"EXCITE", "HARD", 0}}},
    {"nightjar", 0, 8, {SB_PAGE1,
                        {"RATE", "SPACING", 0}, {"CHURR", "EXTRA", 0}, {"SWITCH", "TONE", 0}, {"EXCITE", "HARD", 0}}},
    {"bittern", 0, 7, {SB_PAGE1, {"TEMPO", "SPACING", 0}, {"BOOMS", "EXTRA", 0}, {"EXCITE", "HARD", 0}}},
    /* Stones & sand */
#define S_H(n) {#n, #n, CK_HOST}
#define S_S(l, p) {l, p, 0}
    {"stones", 0, 6, {S_S("SIZE", "SIZE"), S_S("GROUND", "HARD"), S_S("RING", "TONE"), S_S("CLUMPS", "SPACING"),
                      S_S("SPREAD", "SPREAD"), DIST_CARD}},
    {"rockfall", "rockfall", 5, {S_H(VOLUME), S_H(HEIGHT), S_H(GROUND), S_H(FRAG), DIST_CARD}},
    {"ringing rocks", "stonedrum", 8, {{"STONE", "MATERIAL", CK_HOST}, S_H(TUNE), S_H(MALLET), S_H(SHAPE),
                                       S_H(DAMP), S_H(POS), S_H(VARY), DIST_CARD}},
    {"gravel", "gravel", 5, {S_H(SIZE), S_H(AMOUNT), S_H(SHAKE), S_H(GROUND), DIST_CARD}},
    {"marbles", "pebble", 7, {S_H(SIZE), S_H(SURFACE), S_H(ROUGH), S_H(SPEED), S_H(DRAG), S_H(ROUND), DIST_CARD}},
    {"sand", "sand", 6, {S_H(GRAIN), S_H(SURFACE), S_H(HEIGHT), S_H(PULSE), S_H(SIZE), DIST_CARD}},
    {"singing dune", "sandsong", 5, {S_H(GRAIN), S_H(SORTING), S_H(STEADY), DIST_CARD, S_H(DETUNE)}},
    {"minerals", "minerals", 7, {S_H(MINERAL), S_H(SIZE), S_H(ACTION), S_H(HOLD), S_H(HUMID), S_H(VARY), DIST_CARD}},
    {"stone friction", "mineral", 6, {S_H(ROUGH), S_H(STICK), S_H(PRESSURE), S_H(SPEED), S_H(SIZE), DIST_CARD}},
    /* Ice & glass */
#define G_H(n) {#n, #n, CK_HOST}
#define G_CLUMPS {"CLUMPS", "SPACING", 0}
#define G_LAKE {G_H(THICK), G_H(COLD), {"FAR", "DIST", CK_HOST}, DIST_CARD}
    {"icicles", "ice", 6, {G_H(SIZE), G_H(TEMP), G_H(AMOUNT), G_H(FIZZ), G_CLUMPS, DIST_CARD}},
    {"ice in a glass", "ice", 4, {G_H(SIZE), G_H(FIZZ), G_CLUMPS, DIST_CARD}},
    {"frost tinkling", "ice", 3, {G_H(TEMP), G_CLUMPS, DIST_CARD}},
    {"snow underfoot", "ice", 3, {G_H(TEMP), G_CLUMPS, DIST_CARD}},
    {"lake ice", "lakeice", 4, G_LAKE},
    {"frozen lake booming", "lakeice", 4, G_LAKE},
    {"thin ice underfoot", "lakeice", 2, {G_H(THICK), DIST_CARD}},
    {"glass pane", "glass", 6, {G_H(SIZE), G_H(RING), G_H(ENERGY), G_H(HEIGHT), G_H(BOUNCE), DIST_CARD}},
    {"bottles", "glass", 4, {G_H(SIZE), G_H(RING), G_CLUMPS, DIST_CARD}},
    {"shards settling", "glass", 5, {G_H(SIZE), G_H(RING), G_H(ENERGY), G_H(BOUNCE), DIST_CARD}},
    /* Wood */
#define WD_H(n) {#n, #n, CK_HOST}
#define WD_TREE {WD_H(SIZE), WD_H(CREAK), WD_H(PITCH), WD_H(MOIST), DIST_CARD}
    {"spruce", "wood", 5, WD_TREE},
    {"pine", "wood", 5, WD_TREE},
    {"maple", "wood", 5, WD_TREE},
    {"oak", "wood", 5, WD_TREE},
    {"green branch", "wood", 5, {WD_H(SIZE), WD_H(CREAK), WD_H(MOIST), WD_H(PITCH), DIST_CARD}},
    {"tree trunk", "wood", 4, {WD_H(CREAK), WD_H(PITCH), WD_H(SIZE), DIST_CARD}},
    {"twigs underfoot", "wood", 3, {WD_H(SIZE), WD_H(MOIST), DIST_CARD}},
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
