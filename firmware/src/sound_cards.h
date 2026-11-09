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
 * extra (CAVE on cave drips, ROAR on the streams, PERIOD on the waves and the lake); water drops are the source's own drops, their
 * HEIGHT and POOL on its HARD and TONE (the drip's splash is part of every drop, not a card); rain on water's HARD
 * above the middle is WIND. Lake lapping: FACE first (a shore .. a hull), then HEIGHT, PERIOD, DIST / CHOP (a lake
 * has no backwash and no floor). A card whose parameter this build's engine has not got is left out. Still to come with the engines: RAIN (rain's own rate apart
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
 * trunk's CREAK first (it creaks, it does not snap), the twigs underfoot SIZE, MOIST, WEIGHT (the step: 1 .. 4 twigs)
 * and DEPTH (the litter's: it takes the highs; the engine's LITTER, which the source leaves alone). LOAD is the activity
 * (ACTIVITY's own curve: how fast the load builds to the snap), WOOD the kind: neither is a card. */
#ifndef TX_SOUND_CARDS_H
#define TX_SOUND_CARDS_H
#include <stdint.h>

#define SCD_MAX 8u                              /* cards a sound has at most (two screens) */
enum { CK_SRC = 0, CK_HOST = 1, CK_INV = 2 };
/* A card: its label and its parameter's name; a card of the hosted engine (CK_HOST) or turned around (CK_INV) has its
 * kind as the name's first character (H, INV: "\001", "\002"), so a card is two pointers (scd_kind, scd_par). A sound's
 * cards are a list of their own (CL: a compound literal, as long as the sound has cards), not SCD_MAX slots. */
#define H "\001"
#define INV "\002"
typedef struct { const char *lab, *par; } scd_card_t;
typedef struct { const char *sound, *hosted; uint8_t n; const scd_card_t *c; } scd_sound_t;
#define CL (const scd_card_t[])
static inline uint8_t scd_kind(const scd_card_t *c) { return (uint8_t)c->par[0] < 4u ? (uint8_t)c->par[0] : CK_SRC; }
static inline const char *scd_par(const scd_card_t *c) { return c->par + (scd_kind(c) != CK_SRC); }

#define DIST_CARD {"DIST", INV "PROXIMITY"}
static const scd_sound_t SCD[] = {
    {"rain on water", 0, 7, CL {{"DROPS", "SIZE"}, DIST_CARD, {"SURFACE", "TONE"}, {"WIND", "HARD"},
                             {"CLUMPS", "SPACING"}, {"LIFE", "LIFE"}, {"AREA", "AREA"}}},
    {"rain on a roof", "rainsurf", 7, CL {{"SURFACE", H "SURFACE"}, {"DROPS", H "DROPS"},
                                       {"INSIDE", H "INSIDE"}, {"AREA", H "AREA"}, DIST_CARD,
                                       {"EAVES", H "EAVES"}, {"WIND", H "WIND"}}},
    {"water drops", 0, 7, CL {{"SIZE", "SIZE"}, {"HEIGHT", "HARD"}, {"POOL", "TONE"}, {"SCATTER", "SPREAD"},
                           {"CLUMPS", "SPACING"}, {"LIFE", "LIFE"}, DIST_CARD}},
    {"cave drips", "drip", 7, CL {{"SIZE", H "SIZE"}, {"HEIGHT", H "HEIGHT"}, {"POOL", H "DEPTH"},
                               {"CAVE", "EXTRA"}, {"SCATTER", H "SCATTER"}, {"RHYTHM", H "CLUMP"}, DIST_CARD}},
    {"eaves dripping", "drip", 6, CL {{"SIZE", H "SIZE"}, {"HEIGHT", H "HEIGHT"}, {"RHYTHM", H "CLUMP"},
                                   {"POOL", H "DEPTH"}, {"SCATTER", H "SCATTER"}, DIST_CARD}},
    {"brook", "stream", 6, CL {{"SIZE", H "SIZE"}, {"SURFACE", H "NEAR"}, {"GURGLE", H "EDDY"},
                            {"SPREAD", H "SPREAD"}, {"ROAR", "EXTRA"}, DIST_CARD}},
    {"bubbling spring", "stream", 6, CL {{"SIZE", H "SIZE"}, {"SURFACE", H "NEAR"},
                                      {"SPREAD", H "SPREAD"}, {"GURGLE", H "EDDY"}, {"ROAR", "EXTRA"},
                                      DIST_CARD}},
    {"waterfall", "stream", 6, CL {{"ROAR", "EXTRA"}, {"SIZE", H "SIZE"}, DIST_CARD,
                                {"SURFACE", H "NEAR"}, {"SPREAD", H "SPREAD"}, {"GURGLE", H "EDDY"}}},
    {"waves on sand", "surf", 6, CL {{"HEIGHT", H "HEIGHT"}, {"PERIOD", "EXTRA"}, {"DIST", H "DIST"},
                                  {"BACKWSH", H "BACKWASH"}, {"CHOP", H "CHOP"}, {"FLOOR", H "FLOOR"}}},
    {"waves on shingle", "surf", 6, CL {{"HEIGHT", H "HEIGHT"}, {"PERIOD", "EXTRA"}, {"DIST", H "DIST"},
                                     {"BACKWSH", H "BACKWASH"}, {"CHOP", H "CHOP"}, {"FLOOR", H "FLOOR"}}},
    {"lake lapping", "surf", 5, CL {{"FACE", H "FACE"}, {"HEIGHT", H "HEIGHT"}, {"PERIOD", "EXTRA"},
                                 {"DIST", H "DIST"}, {"CHOP", H "CHOP"}}},
    /* Storm */
    {"storm", 0, 8, CL {{"RAIN", "RAIN"}, {"WIND", "WIND"}, {"FLASHES", "FLASHES"}, {"DIST", "DIST"},
                     {"AGE", "AGE"}, {"SURFACE", "SURFACE"}, {"PASSING", "PASSING"}, {"INSIDE", "INSIDE"}}},
    {"thunder", 0, 3, CL {{"JAGGED", "TORT"}, {"HEIGHT", "HEIGHT"}, {"GROUND", "GROUND"}}},
    /* Wind (and the gales in Storm) */
#define W_GUST {"GUST", H "GUST"}
#define W_SCALE {"SCALE", H "SCALE"}
#define W_EXPOSED {"EXPOSED", H "BUFFET"}
#define W_FOLIAGE {"FOLIAGE", H "FOLIAGE"}
#define W_WHOOSH {"WHOOSH", H "WHOOSH"}
#define W_EDGE {"EDGE", H "EDGE"}
#define W_DRIFT {"DRIFT", H "DRIFT"}
#define W_TREES {W_GUST, W_FOLIAGE, W_SCALE, W_EXPOSED, W_WHOOSH, W_EDGE, DIST_CARD}
#define W_OPEN {W_GUST, W_EXPOSED, W_SCALE, W_WHOOSH, W_FOLIAGE, W_EDGE, DIST_CARD}
    {"wind", "wind", 7, CL W_TREES},
    {"wind in pines", "wind", 7, CL W_TREES},
    {"leafy trees", "wind", 7, CL W_TREES},
    {"breeze in grass", "wind", 6, CL {W_GUST, W_SCALE, W_FOLIAGE, W_WHOOSH, W_EXPOSED, DIST_CARD}},
    {"open moor", "wind", 7, CL W_OPEN},
    {"ridge gale", "wind", 7, CL W_OPEN},
    {"sea wind", "wind", 6, CL {W_GUST, W_EXPOSED, W_WHOOSH, W_SCALE, W_FOLIAGE, DIST_CARD}},
    {"gusty autumn", "wind", 7, CL {W_GUST, W_FOLIAGE, {"SKITTER", H "SKITTER"}, W_SCALE, W_EXPOSED, W_WHOOSH,
                                 DIST_CARD}},
    {"winter storm", "wind", 8, CL {W_GUST, W_EXPOSED, W_DRIFT, W_SCALE, W_WHOOSH, W_FOLIAGE, W_EDGE, DIST_CARD}},
    {"desert wind", "wind", 6, CL {W_DRIFT, W_GUST, W_EXPOSED, W_WHOOSH, W_FOLIAGE, DIST_CARD}},
    /* Fire */
#define F_SIZE {"SIZE", H "SIZE"}
#define F_MOIST {"MOIST", H "MOIST"}
#define F_DRAUGHT {"DRAUGHT", H "DRAUGHT"}
#define F_WOOD {"WOOD", H "WOOD"}
#define F_ROAR {"ROAR", H "ROAR"}
#define F_BURSTY {"BURSTY", H "BURSTY"}
#define F_OPEN {F_SIZE, F_MOIST, F_DRAUGHT, F_WOOD, F_ROAR, F_BURSTY, DIST_CARD}
#define F_LIGHT {F_MOIST, F_SIZE, F_DRAUGHT, F_WOOD, F_ROAR, F_BURSTY, DIST_CARD}
    {"campfire", "fire", 7, CL F_OPEN},
    {"bonfire", "fire", 7, CL F_OPEN},
    {"pine fire", "fire", 7, CL F_OPEN},
    {"kindling", "fire", 7, CL F_LIGHT},
    {"leaves, paper", "fire", 7, CL F_LIGHT},
    {"fireplace", "fire", 7, CL {F_SIZE, F_MOIST, F_ROAR, F_WOOD, F_DRAUGHT, F_BURSTY, DIST_CARD}},
    {"embers", "fire", 5, CL {F_MOIST, F_WOOD, F_BURSTY, DIST_CARD, F_SIZE}},
    {"torch", "fire", 6, CL {F_ROAR, F_DRAUGHT, F_SIZE, F_MOIST, F_WOOD, DIST_CARD}},
    {"flame", "candle", 5, CL {{"FLOW", H "FLOW"}, {"DRAUGHT", H "DRAUGHT"}, {"SPUTTER", H "SPUTTER"},
                            {"SLOPE", H "SLOPE"}, DIST_CARD}},
    /* Insects */
#define I_TEMP {"TEMP", H "TEMP"}
#define I_CHORUS {"CHORUS", H "FIELD"}
#define I_SYNC {"SYNC", H "SYNC"}
#define I_RASP {"RASP", H "JITTER"}
#define I_SIZE {"SIZE", H "SIZE"}
#define C_CARDS(a, b, c, d) {a, b, c, d, {"BUZZ", "HARD"}}
#define C_SIZE {"SIZE", "SIZE"}
#define C_RING {"RING", "TONE"}
#define C_DEPTH {"DEPTH", "DEPTH"}
#define B_CARDS {{"SWARM", H "SWARM"}, {"SPEED", H "SPEED"}, DIST_CARD, {"SPREAD", H "SPREAD"}}
    {"field cricket", "crickets", 5, CL {I_TEMP, I_CHORUS, I_SYNC, DIST_CARD, I_SIZE}},
    {"tree cricket", "crickets", 5, CL {I_TEMP, I_CHORUS, I_SYNC, DIST_CARD, I_SIZE}},
    {"mole cricket", "crickets", 5, CL {I_TEMP, I_CHORUS, DIST_CARD, I_SYNC, I_SIZE}},
    {"cicada", 0, 5, CL C_CARDS(C_SIZE, C_RING, C_DEPTH, DIST_CARD)},
    {"small cicada", 0, 5, CL C_CARDS(C_SIZE, C_RING, C_DEPTH, DIST_CARD)},
    {"cicada chorus", 0, 5, CL C_CARDS(C_DEPTH, C_SIZE, C_RING, DIST_CARD)},
    {"katydid", "katydids", 6, CL {I_TEMP, I_RASP, I_CHORUS, DIST_CARD, I_SYNC, I_SIZE}},
    {"grasshopper", "katydids", 7, CL {{"PEGS", H "PEGS"}, I_TEMP, I_RASP, DIST_CARD, I_CHORUS, I_SYNC, I_SIZE}},
    {"honeybee", "bees", 4, CL B_CARDS},
    {"bumblebee", "bees", 4, CL B_CARDS},
    /* Songbirds */
#define SB_PAGE1 {"SIZE", "SIZE"}, {"DEPTH", "DEPTH"}, DIST_CARD, {"VARY", "SPREAD"}
#define SB_SING {"TEMPO", "SPACING"}, {"SONG", "EXTRA"}, {"VARIETY", "TONE"}, {"EXCITE", "HARD"}
    {"songbird", 0, 8, CL {SB_PAGE1, SB_SING}},
    {"blackbird", 0, 8, CL {SB_PAGE1, SB_SING}},
    {"song thrush", 0, 8, CL {SB_PAGE1, SB_SING}},
    {"nightingale", 0, 8, CL {SB_PAGE1, SB_SING}},
    {"robin", 0, 8, CL {SB_PAGE1, SB_SING}},
    {"wren", 0, 8, CL {SB_PAGE1, SB_SING}},
    {"hermit thrush", 0, 8, CL {SB_PAGE1, SB_SING}},
    {"skylark", 0, 8, CL {SB_PAGE1, SB_SING}},
    {"wood thrush", 0, 8, CL {SB_PAGE1, SB_SING}},
    {"reed warbler", 0, 8, CL {SB_PAGE1, SB_SING}},
    /* Wild birds */
    {"chirper", 0, 8, CL {SB_PAGE1, SB_SING}},
    {"crow", 0, 7, CL {SB_PAGE1, {"TEMPO", "SPACING"}, {"SERIES", "EXTRA"}, {"EXCITE", "HARD"}}},
    {"gull", 0, 8, CL {SB_PAGE1,
                    {"TEMPO", "SPACING"}, {"CALLS", "EXTRA"}, {"LONGCALL", "TONE"}, {"EXCITE", "HARD"}}},
    {"curlew", 0, 8, CL {SB_PAGE1,
                      {"TEMPO", "SPACING"}, {"SONG", "EXTRA"}, {"BUBBLE", "TONE"}, {"EXCITE", "HARD"}}},
    {"oystercatcher", 0, 8, CL {SB_PAGE1,
        {"TEMPO", "SPACING"}, {"SONG", "EXTRA"}, {"PIPING", "TONE"}, {"EXCITE", "HARD"}}},
    {"cuckoo", 0, 8, CL {SB_PAGE1,
                      {"TEMPO", "SPACING"}, {"CALLS", "EXTRA"}, {"INTERVAL", "TONE"}, {"EXCITE", "HARD"}}},
    {"woodpecker", 0, 7, CL {SB_PAGE1, {"DRUM", "SPACING"}, {"ROLL", "EXTRA"}, {"EXCITE", "HARD"}}},
    {"wood pigeon", 0, 7, CL {SB_PAGE1, {"TEMPO", "SPACING"}, {"PHRASES", "EXTRA"}, {"EXCITE", "HARD"}}},
    {"nightjar", 0, 8, CL {SB_PAGE1,
                        {"RATE", "SPACING"}, {"CHURR", "EXTRA"}, {"SWITCH", "TONE"}, {"EXCITE", "HARD"}}},
    {"bittern", 0, 7, CL {SB_PAGE1, {"TEMPO", "SPACING"}, {"BOOMS", "EXTRA"}, {"EXCITE", "HARD"}}},
    /* Stones & sand */
#define S_H(n) {#n, H #n}
#define S_S(l, p) {l, p}
    {"stones", 0, 6, CL {S_S("SIZE", "SIZE"), S_S("GROUND", "HARD"), S_S("RING", "TONE"), S_S("CLUMPS", "SPACING"),
                      S_S("SPREAD", "SPREAD"), DIST_CARD}},
    {"rockfall", "rockfall", 5, CL {S_H(VOLUME), S_H(HEIGHT), S_H(GROUND), S_H(FRAG), DIST_CARD}},
    {"ringing rocks", "stonedrum", 8, CL {{"STONE", H "MATERIAL"}, S_H(TUNE), S_H(MALLET), S_H(SHAPE),
                                       S_H(DAMP), S_H(POS), S_H(VARY), DIST_CARD}},
    {"gravel", "gravel", 5, CL {S_H(SIZE), S_H(AMOUNT), S_H(SHAKE), S_H(GROUND), DIST_CARD}},
    {"marbles", "pebble", 7, CL {S_H(SIZE), S_H(SURFACE), S_H(ROUGH), S_H(SPEED), S_H(DRAG), S_H(ROUND), DIST_CARD}},
    {"sand", "sand", 6, CL {S_H(GRAIN), S_H(SURFACE), S_H(HEIGHT), S_H(PULSE), S_H(SIZE), DIST_CARD}},
    {"singing dune", "sandsong", 5, CL {S_H(GRAIN), S_H(SORTING), S_H(STEADY), DIST_CARD, S_H(DETUNE)}},
    {"minerals", "minerals", 7, CL {S_H(MINERAL), S_H(SIZE), S_H(ACTION), S_H(HOLD), S_H(HUMID), S_H(VARY), DIST_CARD}},
    {"stone friction", "mineral", 6, CL {S_H(ROUGH), S_H(STICK), S_H(PRESSURE), S_H(SPEED), S_H(SIZE), DIST_CARD}},
    /* Ice & glass */
#define G_H(n) {#n, H #n}
#define G_CLUMPS {"CLUMPS", "SPACING"}
#define G_LAKE {G_H(THICK), G_H(COLD), {"FAR", H "DIST"}, DIST_CARD}
    {"icicles", "ice", 6, CL {G_H(SIZE), G_H(TEMP), G_H(AMOUNT), G_H(FIZZ), G_CLUMPS, DIST_CARD}},
    {"ice in a glass", "ice", 4, CL {G_H(SIZE), G_H(FIZZ), G_CLUMPS, DIST_CARD}},
    {"frost tinkling", "ice", 3, CL {G_H(TEMP), G_CLUMPS, DIST_CARD}},
    {"snow underfoot", "ice", 3, CL {G_H(TEMP), G_CLUMPS, DIST_CARD}},
    {"lake ice", "lakeice", 4, CL G_LAKE},
    {"frozen lake booming", "lakeice", 4, CL G_LAKE},
    {"thin ice underfoot", "lakeice", 2, CL {G_H(THICK), DIST_CARD}},
    {"glass pane", "glass", 6, CL {G_H(SIZE), G_H(RING), G_H(ENERGY), G_H(HEIGHT), G_H(BOUNCE), DIST_CARD}},
    {"bottles", "glass", 4, CL {G_H(SIZE), G_H(RING), G_CLUMPS, DIST_CARD}},
    {"shards settling", "glass", 5, CL {G_H(SIZE), G_H(RING), G_H(ENERGY), G_H(BOUNCE), DIST_CARD}},
    /* Wood */
#define WD_H(n) {#n, H #n}
#define WD_TREE {WD_H(SIZE), WD_H(CREAK), WD_H(PITCH), WD_H(MOIST), DIST_CARD}
    {"spruce", "wood", 5, CL WD_TREE},
    {"pine", "wood", 5, CL WD_TREE},
    {"maple", "wood", 5, CL WD_TREE},
    {"oak", "wood", 5, CL WD_TREE},
    {"green branch", "wood", 5, CL {WD_H(SIZE), WD_H(CREAK), WD_H(MOIST), WD_H(PITCH), DIST_CARD}},
    {"tree trunk", "wood", 4, CL {WD_H(CREAK), WD_H(PITCH), WD_H(SIZE), DIST_CARD}},
    {"twigs underfoot", "wood", 5, CL {WD_H(SIZE), WD_H(MOIST), WD_H(WEIGHT), {"DEPTH", H "LITTER"}, DIST_CARD}},
    /* Storm's hail */
#define HAIL_CARDS {{"STONES", H "DROPS"}, {"AREA", H "AREA"}, {"INSIDE", H "INSIDE"}, \
                    {"RAIN", H "RAIN"}, {"BOUNCE", H "BOUNCE"}, DIST_CARD}
    {"hail on a roof", "rainsurf", 6, CL HAIL_CARDS},
    {"hail on a car", "rainsurf", 6, CL HAIL_CARDS},
    {"hail in leaves", "rainsurf", 6, CL HAIL_CARDS},
    {"sleet on a window", "rainsurf", 5, CL {{"STONES", H "DROPS"}, {"AREA", H "AREA"},
                                          {"INSIDE", H "INSIDE"}, DIST_CARD, {"RAIN", H "RAIN"}}},
};
#define SCD_N (sizeof SCD / sizeof SCD[0])
/* the source's general cards (every sound not in SCD) */
static const scd_sound_t SC_SRC_DEFAULT = {"", 0, 8, CL {{"SIZE", "SIZE"}, {"SPREAD", "SPREAD"}, {"TONE", "TONE"},
    {"HARD", "HARD"}, {"NEAR", "PROXIMITY"}, {"DEPTH", "DEPTH"}, {"LIFE", "LIFE"}, {"CLUMPS", "SPACING"}}};

#undef H
#undef INV
#undef CL
#endif
