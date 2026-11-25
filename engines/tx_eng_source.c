/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura SOURCE: one plastic sound source in four stages (the design: what happens, what strikes, what rings, where
 * it is), the first prototype with three presets (rain on water, a stone cloud, cicadas) and a particle-level MORPH
 * between any two of them. The engines it grew from (rainwater, stonecloud, cicadas) stay as they are.
 *
 * Playing: a held key keeps the source going (it fades in over ~0.2 s; released, or at ACTIVITY 0, no new event
 * starts at once: only tails ring); the key moves the size (a semitone per key from C4, as the presets' own engines
 * do) and a press adds one near event (a big drop, a thrown stone, a cicada close by). How much happens is ACTIVITY: one fader, on each preset's own scale and
 * through its natural transition (stage 1):
 *   rain     30 .. 20000 drops/s per m^2 over the ~6 m^2 heard (ours; drizzle .. downpour); the rain rate behind it
 *            (Marshall-Palmer's flux ~ 415 R^0.65 per m^2, from note 02's table, ours: R ~ 0.01 .. 50 mm/h) moves the
 *            drop sizes (Lambda = 4.1 R^-0.21 / mm; arrivals weighted by fall speed ~ D^0.67, ours as a gamma of shape
 *            1.67). Over 5000 drops/s each explicit drop stands for several (their bubbles to the bed, their clicks
 *            spread over the block).
 *            Classes as rainwater's (note 02): 0.7-1.2 mm a type I bubble (13-20 kHz) half the time, 1.2-2.4 mm a
 *            click, 2.4-4.1 mm a click and a type II bubble 50-80 ms later, larger a type III one too. The
 *            transition: single plinks fuse into a hiss: bubbles beyond the heroes (or over the per-block cap)
 *            become a noise bed of the same power (Campbell), its two bands (type I 13-20 kHz, type II 3-10 kHz).
 *   stones   0.2 .. 500 knocks/s of pieces as stonecloud's (flexural modes of a flake, f1 ~ 0.128 c / L, constant Q
 *            by mineral, Hunt-Crossley contacts, power-law energies). The transition: an avalanche. Each knock
 *            may set off others (stage 1's clock, tx_core_cluster.h: branching 0.3 .. 0.85, Omori c 30 ms, p 1.5),
 *            the stones grow and are thrown harder; knocks beyond the heroes leave their attack residue (a slide).
 *   cicadas  1 .. 30 callers, each a tymbal voice as the cicadas engine's (ribs, glide, air sac), persistent members
 *            with their own carrier, rate, phrase and distance. The transition: falling into step. Above a third
 *            the callers' pulse groups couple (Kuramoto, the pull rising with ACTIVITY to 80, then held), so a big
 *            chorus pulses together; above 80 each group its own length (up to +-20 %): together, never one
 *            clock. Callers beyond the heroes (the farthest, quietest first) join a bed: noise through a two-pole
 *            at their mean carrier, pulsed by their mean phrase, of the same power.
 * Loudness: each preset's trim puts ACTIVITY 64 at -26 LUFS (a key held 10 s at 3 m); a fader law (amplitude ~
 * amount^-1/4 around the middle, events only: ours) keeps the ends playable: rain -37 .. -20, stones -21 .. -15
 * (sparse knocks are loud ones), cicadas -29 .. -22 LUFS.
 * ACTIVITY is the one rate knob (the old rate-only trim merged in), perceptually even: each sound has its own curve,
 * measured (its loudness and its sounding share, tools/lin_sounds.txt, tools/gen_lin.py), so each step sounds the same
 * more, from its value at 0 to its audible top (rain: ~2500 drops/s; past it nothing more is heard); a sound not
 * measured runs its activity (events/s, its singing, its flow) lo + (hi - lo) x^2.5 between its old ends;
 * the paths below (written at ACTIVITY 0 / 64 / 127) are walked at the place the line asks for (lin_row, tx_core_lin.h,
 * tools/gen_lin.py). COUNT runs 1 + 63 x^2.5 individuals (measured per sound too: its own curves were no evener). EXTRA is the sound's own extra card knob; SPACING moves the timing between
 * clumped (bursts), the preset's own (Poisson rain) and even. LIFE and TIME: organic modulation (tx_core_life.h: OU
 * walks, 1/f, swells and lulls) of ACTIVITY (full depth), SIZE and DIST (a quarter). DIST and DEPTH: where (tx_core_
 * place.h): every particle carries 1 / r and the air's loss at its own frequency; beds take the air as a one-pole.
 * LOCK, DIV and GRID are the MIDI clock's hooks (stored, no effect yet). Every knob is a parameter (a sequencer or a
 * modulator can reach any of them later). */
#include "tx_engine.h"
#include "tx_core_trim.h"
#include "tx_core_vary.h"
#include "tx_core_cloud.h"
#include "tx_core_bubble.h"
#include "tx_core_modal.h"
#include "tx_core_contact.h"
#include "tx_core_insect.h"
#include "tx_core_place.h"
#include "tx_core_life.h"
#include "tx_core_bird.h"
#include "tx_core_cluster.h"
#include "tx_core_grains.h"
#include "tx_core_lin.h"
#include "tx_src_lin.h"

#define OUT_TRIM 4096               /* output gain, Q12 (the presets' own levels are in PRE[].trim) */

#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 1

enum { P_PRESET, P_INTENS, P_EXTRA, P_SPACING, P_SIZE, P_SPREAD, P_TONE, P_HARD, P_PROX, P_DEPTH,
       P_LIFE, P_TIME, P_LOCK, P_DIV, P_GRID, P_TYPE, P_COUNT, NP };
enum { PR_RAIN, PR_STONES, PR_CICADAS, PR_WATER, PR_WIND, PR_FIRE, PR_CRICKETS, PR_SAND, PR_GRAVEL, PR_KATYDIDS, PR_BEES, PR_ICE, PR_WOOD, PR_MINERALS, PR_GLASS, PR_SONGBIRD, PR_CHIRPER, PR_CROW,
       PR_GULL, PR_BLACKBIRD, PR_THRUSH, PR_NIGHTINGALE, PR_ROBIN, PR_WREN, PR_HERMIT, PR_SKYLARK,
       PR_WOODTHRUSH, PR_WARBLER, PR_CURLEW, PR_OYSTER, PR_CUCKOO, PR_WOODPECKER, PR_PIGEON, PR_NIGHTJAR, PR_BITTERN, NPRE };
enum { V_NONE, V_BUBBLE, V_STONE, V_CALLER, V_BIRD };

static const tx_param_t PARAMS[NP] = {
    {"PRESET", NPRE - 1, 0, "the source: 0 rain on water | 1 stone cloud | 2 cicadas | 3 water | 4 wind | 5 fire | 6 crickets | 7 sand | 8 gravel | 9 katydids | 10 bees | 11 ice | 12 wood | 13 minerals | 14 glass | 15 songbird | 16 chirper | 17 crow | 18 gull | 19 blackbird | 20 song thrush | 21 nightingale | 22 robin | 23 wren | 24 hermit thrush | 25 skylark | 26 wood thrush | 27 reed warbler | 28 curlew | 29 oystercatcher | 30 cuckoo | 31 woodpecker | 32 wood pigeon | 33 nightjar | 34 bittern"},
    {"ACTIVITY", 127, 96, TX_TEXT("how active each individual is (COUNT says how many), perceptually even: each step sounds the same amount more of the sound's activity (events/s, its singing, its flow), on the sound's own measured curve up to its audible top (tx_src_lin.h), on the preset's own scale and through its transition: a rain patch 0.2 .. 1500 drops/s (a stray drop .. a downpour on it), a stone 0.05 .. 60 knocks/s, a drip point one per 40 s .. 8 /s, a cicada rare short calls .. singing on, a bird singing now and then .. often; hosted engines: their own path (wind a calm .. 28 m/s, fire embers .. a blaze, ...)")},
    {"EXTRA", 127, 64, TX_TEXT("the sound's own extra parameter (its card names it; 64 = the sound's own value)")},
    {"SPACING", 127, 64, TX_TEXT("the timing: clumped bursts .. Poisson (64 up: the preset's own, never an even beat)")},
    {"SIZE", 127, 64, TX_TEXT("the size: 64 = the preset's, +-2 octaves (bigger drops, stones, lower cicadas)")},
    {"SPREAD", 127, 64, TX_TEXT("how mixed the sizes are: alike .. the preset's (64) .. twice as mixed")},
    {"TONE", 127, 64, TX_TEXT("the body: 64 = the preset's; the stones' Q x/ 4 (mineral), the cicadas' tymbal Q; rain: the bubbles' depth")},
    {"HARD", 127, 64, TX_TEXT("the strike: 64 = the preset's; stones: softer ground above; cicadas: buzzier above; rain: the clicks' level")},
    {"PROXIMITY", 127, 64, TX_TEXT("far .. close: 64 = the preset's own distance (3 m; 16 steps = twice as near: +6 dB, sharper, all the highs, down to 0.5 m; far: 1 / r, the air takes the highs, up to 3 km)")},
    {"DEPTH", 127, 40, TX_TEXT("the population's spread in distance: all at DIST .. x/ 8 (near and far at once)")},
    {"LIFE", 127, 50, TX_TEXT("organic modulation depth (walks, swells and lulls; never a sine): still .. alive")},
    {"TIME", 127, 64, TX_TEXT("LIFE's time scale: 1 s .. 5 min (log)")},
    {"LOCK", 1, 0, TX_TEXT("MIDI clock lock (a hook: stored, no effect until the clock sync is built)")},
    {"DIV", 9, 4, TX_TEXT("beat division when locked: 1/1 1/2 1/4 1/8 1/16 1/32 1/4T 1/8T 1/16T, 9 = a probability per bar (a hook)")},
    {"GRID", 127, 0, TX_TEXT("grid pull: the probability an event lands on the clock's grid (a hook)")},
    {"TYPE", 127, 0, TX_TEXT("the kind within the family: 0 = the preset's own; above, the hosted engine's own kinds over the range (wind: grass .. pines .. leafy trees .. a moor .. the coast .. a ridge .. autumn .. a winter storm .. a desert; crickets, katydids, bees: species; rain: surfaces 1 .. 99, hail and sleet 101 .. 127; water: drops 0, cave drips 16, eaves 32, a brook 48, a spring 64, a waterfall 80, waves on sand 96, on shingle 112, a lake lapping on a shore 120 .. on a hull 127; ice: icicles 8, cubes in a glass 24, frost 40, snow 56, lake ice 72, sea ice 88, a far lake booming 104, thin ice 120; glass: a pane 0, bottles 64, shards 110; wood: spruce .. green branch; minerals: granite .. quartz; sand: hand .. tube)")},
    {"COUNT", 127, 53, TX_TEXT("how many individuals at once, each its own distance, size and character: 0 none (silence; tails ring), then perceptually even (1 + 63 x^2.5, each step sounds the same more): 1, 20, 39, 53, 77, 96, 127 = 1, 2, 4, 8, 19, 32, 64 (rain: patches of rain, stones: rolling stones or pouring points, water: dripping points, cicadas, birds; hosted engines: their keys or presses)")},
};

#define NV 16u                      /* heroes */
#define NM 32u                      /* members (callers) */
#define NPEND 40u
#define EVMAX 24u                   /* explicit heroes started per block */
#define ST_HEROES 8u                /* the stone cloud's heroes at most: its pieces' modes cost ~58 us a hero on the
                                     * FM-1 (6 at SPACING 0 reached ~910 us in a block: 5 until the second core and
                                     * the cache layout's room, the device pool's half now the rule); the rest ring as
                                     * grains */
#define EV_BLK 28u                  /* rain: explicit events a block at most, on average (3400 /s; clumped rain's children
                                     * count too): above, each is kept at random and stands for the ones thinned out */
#define EV_HARD 14u                 /* rain: explicit events in one block at most (a clumped burst's): the rest are carried
                                     * by the next kept one (owe_q8). With XBIG: worst block ~650 us FM-1 at SIZE 127 +
                                     * SPACING 0 (was ~2400), w8 under 600 */
#define XBIG 1u                     /* rain: a big drop standing for several places this many more of them at most */
#define RATE_CAP 2500u              /* explicit events per second at most: above, each stands for several (rain; its grains) */

typedef struct {
    uint8_t kind;                   /* 0 events, 1 callers, 2 an engine hosted whole (HOST[act_lo]), 4 birds (type act_lo),
                                     * 3 water: events
                                     * (drops) and an engine (HOST[act_lo]: the stream) together */
    int32_t act_lo, act_oct;        /* ACTIVITY's scale: log2 of the 0 % value, its range (Q16) */
    int32_t reg;                    /* natural spacing: -64 clumped .. 0 Poisson (never more even: no regular beat) */
    int32_t trim;                   /* the preset's level, Q12 */
    int32_t law;                    /* a fader law: amplitude ~ amount^(-law / 32) (0 for all: nature's law, more = louder) */
} preset_t;

static const preset_t PRE[NPRE] = {
    {0, 490986, 614782, 0, 22300, 0},   /* rain: 30 .. 20000 drops/s per m^2 (drizzle .. downpour), over ~6 m^2 heard (ours; -7 dB: nearer) */
    {0, -152170, 739752, 0, 39470, 0},  /* stones: 0.2 .. 500 knocks/s (+3.3 dB: most knocks now far, note 14) */
    {1, 0, 321577, 0, 6800, 0},     /* cicadas: 1 .. 30 callers (+2.4 dB: their pauses, most far) */
    {3, 1, 0, -6, 19800, 0},        /* water: drops (WATER_RATE) + the stream engine (HOST[1]) fading in */
    {2, 0, 0, 0, 4096, 0},          /* wind: the wind engine hosted (TYPE 64 up: whistling wires) */
    {2, 2, 0, 0, 4096, 0},          /* fire */
    {2, 3, 0, 0, 4096, 0},          /* crickets */
    {2, 4, 0, 0, 4096, 0},          /* sand */
    {2, 5, 0, 0, 4096, 0},          /* gravel */
    {2, 6, 0, 0, 4096, 0},          /* katydids */
    {2, 7, 0, 0, 4096, 0},          /* bees */
    {2, 8, 0, 0, 4096, 0},          /* ice */
    {2, 9, 0, 0, 4096, 0},          /* wood */
    {2, 10, 0, 0, 4096, 0},         /* minerals (knocked by hand; never an even beat) */
    {2, 11, 0, 0, 4096, 0},         /* glass */
    {4, 0, 0, 0, 410, 0},          /* birds (tx_core_bird type act_lo): songbirds (a woodland mix) */
    {4, 1, 0, 0, 410, 0},          /* chirper (chaffinch, sparrow) */
    {4, 2, 0, 0, 1630, 0},          /* crow */
    {4, 4, 0, 0, 860, 0},          /* gull */
    {4, 5, 0, 0, 410, 0},          /* blackbird (levels trimmed inside the bird core) */
    {4, 6, 0, 0, 520, 0},          /* song thrush (+2.1 dB: fewer notes, the level kept) */
    {4, 7, 0, 0, 410, 0},          /* nightingale */
    {4, 8, 0, 0, 410, 0},          /* robin */
    {4, 9, 0, 0, 410, 0},          /* wren */
    {4, 10, 0, 0, 410, 0},         /* hermit thrush */
    {4, 11, 0, 0, 410, 0},         /* skylark */
    {4, 12, 0, 0, 205, 0},         /* wood thrush */
    {4, 13, 0, 0, 1640, 0},        /* reed warbler (a wetland; some birds sedge-like; +2 dB: slower chatter, the
                                    * level kept) */
    {4, 14, 0, 0, 1100, 0},        /* curlew (an estuary) */
    {4, 15, 0, 0, 920, 0},        /* oystercatcher (a beach) */
    {4, 16, 0, 0, 240, 0},        /* cuckoo (spring) */
    {4, 17, 0, 0, 217, 0},     /* woodpecker (drumming, a forest; -7.6 dB for its tree's longer ring, Q 16) */
    {4, 18, 0, 0, 205, 0},     /* wood pigeon */
    {4, 19, 0, 0, 410, 0},     /* nightjar (a heath at night) */
    {4, 20, 0, 0, 410, 0},     /* bittern (a reedbed) */
};

/* stage 1's character per preset (tx_core_cluster.h; note 14, measured against the references), three values at
 * ACTIVITY 0 / 64 / 127 (linear between): sparse individuals .. bunched .. a fused cloud.
 *   cox     the rate's slow walk (gusts, runs, quiet spells): its sd, 1/100 octave; ctau its time constant, 0.1 s
 *   br      self-excitation: the branching ratio (1/100), children after an exponential delay (mean tau_ms) or an
 *           Omori one (c = tau_ms, p 1.5), at most gens generations, each a level step child (sd csd), 1/10 octave
 *   near, span, dim   salience: an event's distance over the ground (dim 2) or a volume (3) from DIST + near out to
 *           span farther (both in DEPTH's spread x 1/10: DEPTH 0 puts all at DIST), at ACTIVITY 0 and 127: sparse
 *           individuals are heard where they are, a dense cloud spreads over the ground (most of it far); dim 0: the
 *           plain draw */
typedef struct {
    uint8_t cox[3], ctau, br[3], omori, gens;
    uint16_t tau_ms;
    int8_t child, csd, near[2], span[2];
    uint8_t dim;
} clu_pre_t;
#define CLU_NONE {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {0, 0}, {0, 0}, 0}
static const clu_pre_t CLU[NPRE] = {
    /* rain: Poisson (the refs' 2 s level sd 0.3-0.8 dB); over the ground around, most drops far (a downpour's few near
     * drops stand out of its far mass: note 16, the dense end was a flat hiss) */
    {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {-10, -20}, {20, 45}, 2},   /* (a downpour: louder than a shower, never pushed off into the distance) */
    /* stones: runs and quiet spells (fewer at the top: a slide that does not stop); a knock sets others off (Omori),
     * from a few (0.3) to an avalanche (0.85); pieces over the slope around, most far: a few near knocks stand out */
    {{90, 90, 40}, 30, {30, 45, 85}, 1, 12, 30, -5, 10, {-10, -21}, {20, 42}, 2},
    /* cicadas: callers (their bouts in callers_block); over the ground around, most far */
    {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {-10, -15}, {20, 40}, 2},
    /* water: drip points come and go (runs), a drop's splash sets off another now and then */
    {{60, 50, 30}, 15, {15, 15, 10}, 0, 3, 120, -10, 10, {0, 0}, {0, 0}, 0},
    CLU_NONE, CLU_NONE, CLU_NONE, CLU_NONE, CLU_NONE, CLU_NONE, CLU_NONE,   /* wind .. bees: their engines' own timing */
    /* the event engines' presses: ice, one icicle bringing down others (runs, quiet spells); wood, cracks in runs; minerals
     * knocked by hand (near even, a little wander); glass, a break setting off another */
    {{80, 80, 80}, 50, {30, 40, 50}, 0, 3, 600, -5, 5, {0, 0}, {0, 0}, 0},
    {{60, 60, 60}, 50, {20, 30, 40}, 0, 3, 1500, -5, 5, {0, 0}, {0, 0}, 0},
    {{40, 40, 40}, 30, {0, 0, 0}, 0, 0, 0, 0, 0, {0, 0}, {0, 0}, 0},
    {{60, 60, 60}, 50, {20, 30, 40}, 0, 3, 800, -5, 5, {0, 0}, {0, 0}, 0},
};

/* HOSTED presets: an existing engine run whole inside the source (its own physics and defaults: at ACTIVITY 64 it is
 * that engine at its defaults), ACTIVITY moving its parameters along the preset's path through its transition: each
 * curve a parameter's value at ACTIVITY 0, 64 and 127 (linear between); (the first curve once followed the old ACTIVITY trim too: merged). The
 * level is set with a gain at 0 / 64 / 127 (tenths of a dB, linear between: ours, by render: every preset at ACTIVITY 64
 * about -27 LUFS, a key held 20 s, the mean of 3 seeds, so lanes sit together in a landscape). Hosted
 * presets do not morph (one engine fits the arena). An engine with its own COUNT (crickets, katydids) is sent the
 * source's (at most its own maximum), its heroes the lane's PARTICLES (the governor's) up to HOST_HEROES; the
 * individuals past them are its far callers (FIELD, where it has one). Where each held key is an individual (bees: a singer;
 * fire: a log), the source holds down one key per individual COUNT asks for, at most k127 (<= 8, a few semitones
 * apart): more keys, more of them, coming one at a time; COUNT past them moves the first curve k64 knob units a
 * doubling (a fire: its logs burn livelier; with no keys, or an event engine, about COUNT 8 = the curves as written, fewer
 * less; k0 & 8: the curve falls with more, from 8). k0 & 2: held in bouts below ACTIVITY 64 (host_gate). k0 & 4: each
 * key a crack of its own: the extras come at an engine's own pace (exponential, mean 2 s), not ~0.2 s apart. k0 & 16 (an
 * event engine): each press goes to one of COUNT distinct keys (up to 8), at random: COUNT pebbles. With k0 & 1, a
 * held key, picked at random, is pressed afresh after
 * 3 s + an exponential wait (mean 7 s; no clock): a singer leaves and a new one comes with the parameters of now (these engines read them at a press; a
 * fire's logs just burn: a press there would throw a new log on, its burst louder than the fire it lands in). k0 & 32 (an
 * engine with its own weather, the wind, a fire): LIFE's walk on the activity at a quarter, and the bouts read the
 * fader alone (the engine's own gusts and lulls already move it; the full walk on top swung a held wind by +-7 dB and, wandering
 * under 64, cut it into bouts at the default). */
#define HOST_NC 7u
#define HOST_NK 8u                  /* character maps per hosted engine (HMAP): the card's knobs, ACTIVITY and PROXIMITY aside */
#define GATE_ON 120u                /* a bout held, mean blocks (0.7 s) */
#define HOST_HEROES 12u             /* an engine counting its own individuals: heroes at most (CPU: half the pool) */
#define HOST_BYTES 12544u           /* the largest hosted engine's state (sand 12 528 B) */
typedef struct { const char *name; int8_t v0; uint8_t v64, v127; } curve_t;   /* (v0 < 0: off until the line crosses 0) */
/* a row's curves are a list of their own, as long as the row has curves (CURVES: the list and its length; a curve
 * past nc is off, as a nameless one): no HOST_NC slots a row */
#define CURVES(...) (const curve_t[]){__VA_ARGS__}, (uint8_t)(sizeof (const curve_t[]){__VA_ARGS__} / sizeof(curve_t))
#define NO_CURVES 0, 0
typedef struct {
    const char *eng; int16_t g0, g64, g127; const curve_t *c; uint8_t nc, k0, k64, k127;
    uint16_t e0, e64, e127;         /* event engines: the mean time between presses (0.1 s; log between; 0 = none) */
    uint8_t oneshot;                /* 1: each press released after 150 ms (a strike); 0: the held key pressed afresh */
} host_t;   /* (g: one calibration, flat:
                                                     * more activity is louder by the engine's own physics) */
static const host_t HOST[] = {
    /* wind: a lone gust now and then (never a dead calm: COUNT 0 is the silence) .. a breeze .. a storm: gustier, more
     * gusts passing, more buffeting; an edge tone only now and then in the strong gusts. In bouts below ACTIVITY 64
     * (k0 2): light air comes in puffs with lulls between; the held key is never pressed afresh (each press is a gust: a
     * renewal every ~10 s was a train of big gusts no knob moved); its own weather (k0 32); 3 dB up
     * with it (the renewed gusts had carried the level) */
    {"wind", 130, -43, -90, CURVES({"WIND", 20, 64, 127}, {"GUST", 40, 70, 110}, {"WHOOSH", 30, 64, 90}, {"BUFFET", 30, 48, 80},
                    {"EDGE", 0, 12, 28}), 34, 1, 4, 0, 0, 0, 0},
    /* stream: a trickle of plinks .. a babbling brook .. a torrent: bigger gulping bubbles, eddies, the low roar */
    {"stream", -42, -42, -42, CURVES({"FLOW", 10, 70, 105}, {"ROAR", 0, 0, 50}, {"EDDY", 20, 30, 60}, {"NEAR", 80, 64, 50},
                            {"SPREAD", 80, 64, 45}), 0, 0, 0, 0, 0, 0, 0},
    /* fire: embers (a rare crackle, a faint roar) .. a campfire .. a blaze (a big fire, dense crackles in bursts, roar, gusts) */
    {"fire", 110, 110, 80, CURVES({"MOIST", 0, 0, 95}, {"SIZE", 8, 50, 100}, {"ROAR", -30, 28, 100}, {"DRAUGHT", 10, 30, 70},
                          {"BURSTY", 0, 24, 40}), 34, 26, 4, 0, 0, 0, 0},   /* (the top: no denser than the references, and
                                                                     * under the output limiter: agent D, 2026-10-25;
                                                                     * its own weather and no bouts (k0 34: a fire
                                                                     * does not stop and restart), 4 logs at most and
                                                                     * MOIST's middle at 0: the references' ~2 crackles
                                                                     * a second, 2026-10-26) */
    /* crickets (COUNT: the engine's own, up to 32): each singing now and then .. all the time, a little in step,
     * warmer (a little faster) */
    {"crickets", -12, -12, -12, CURVES({"ACTIVITY", 0, 64, 127}, {"SYNC", 30, 40, 50}, {"TEMP", 70, 76, 84}), 0, 0, 0, 0, 0, 0, 0},
    /* sand: a trickle of grains through a narrow neck .. a pour .. a heavy pour from higher, pulsing */
    {"sand", -37, -37, -37, CURVES({"ORIFICE", 20, 50, 115}, {"HEIGHT", 30, 50, 90}, {"PULSE", 0, 0, 40}), 2, 16, 2, 0, 0, 0, 0},
    /* gravel: a few stones now and then .. footsteps on gravel .. a slide (many stones, long rattles) */
    {"gravel", 16, 16, 16, CURVES({"RATE", 20, 59, 110}, {"AMOUNT", 20, 64, 120}, {"SHAKE", 20, 30, 70}), 2, 16, 4, 0, 0, 0, 0},
    /* katydids (COUNT: the engine's own, up to 16; they come and go on their own): a call now and then on a cold
     * night .. singing on, warm, falling into step. By default a chorus, as the references: the near ones singing on
     * with short pauses, the far callers (FIELD) between their calls (the references' gaps 20-35 dB under the calls,
     * never silent); -1.8 dB: the katydid and the grasshopper within 0.4 LU of the lone singers they were (agent D,
     * 2026-10-26) */
    {"katydids", -18, -18, -18, CURVES({"ACTIVITY", 0, 110, 127}, {"TEMP", 40, 76, 90}, {"SYNC", 40, 50, 64}, {"FIELD", 0, 70, 90}), 0, 0, 0, 0, 0, 0, 0},
    /* bees: one bee wandering farther off, slower .. one at its ease .. a swarm of 8 close by */
    {"bees", -45, -45, -45, CURVES({"SWARM", 0, 0, 64}, {"DIST", 90, 50, 30}, {"SPEED", 40, 64, 80}), 3, 10, 4, 0, 0, 0, 0},
    /* ice: an icicle breaking off now and then .. icicles falling every few seconds, more chunks each */
    {"ice", 175, 175, 100, CURVES({"AMOUNT", 20, 64, 127}, {"FIZZ", 10, 20, 40}), 0, 12, 0,
     300, 20, 3, 1},                /* (the top: a pile of icicles shattering, 3-5 breaks/s as the references; the middle
                                     * a press every ~2 s: was 7 s, icicles 2.25 from their references, 1.8 now; cubes,
                                     * frost and snow, whose press starts a bout, were mostly silence) */
    /* wood: a branch taken and bent now and then, creaking .. load building faster, snapping every few seconds */
    {"wood", -35, -35, -35, CURVES({"LOAD", 110, 40, 0}, {"CREAK", 15, 25, 30}), 10, 12, 0,
     300, 80, 4, 0},                /* (the top: a snap every ~0.4 s, each bend 0.3 s: a press sooner cuts the bend) */
    /* minerals: a knock on a stone now and then .. knocking and clinking, quicker */
    {"minerals", -40, -40, -40, CURVES({"HOLD", 50, 36, 30}), 0, 0, 0,
     100, 12, 2, 1},
    /* glass: a pane cracking now and then .. smashing every few seconds, harder */
    {"glass", 0, 0, 0, CURVES({"ENERGY", 60, 100, 127}), 0, 8, 0,
     400, 35, 3, 1},                /* (the top: a pane collapsing, 4-5 breaks/s; the middle a break every ~3.5 s, as the
                                     * references: was 9 s, a pane 3.1 from them, 2.0 now) */
    /* the family members TYPE reaches (FAM below; note 16): */
    /* rain on surfaces (rain): drizzle on a roof .. a downpour, bigger drops */
    {"rainsurf", -10, -10, -10, CURVES({"RATE", -20, 60, 120}, {"DROPS", 40, 64, 110}, {"AREA", -20, 50, 50}), 0, 16, 4, 0, 0, 0, 0},
    /* lake ice (ice): a crack now and then .. a lake singing and booming */
    {"lakeice", -30, -30, -30, CURVES({"RATE", 8, 34, 90}), 4, 16, 4, 0, 0, 0, 0},
    /* rolling balls and marbles (gravel): a ball pushed now and then .. marbles rolling about (COUNT: the marbles,
     * each its own key, up to 8) */
    {"pebble", -105, -105, -105, CURVES({"SPEED", 40, 64, 96}), 18, 0, 0,
     60, 20, 6, 0},
    /* a flame without crackle (fire): a candle .. a gas burner roaring in a draught */
    {"candle", 30, 30, 30, CURVES({"FLOW", 5, 15, 90}, {"DRAUGHT", 5, 20, 70}, {"SPUTTER", 30, 100, 100}), 2, 8, 8, 0, 0, 0, 0},
    /* singing dunes (sand): a slip below the threshold .. a booming avalanche */
    {"sandsong", 0, 0, 0, CURVES({"FLOW", 60, 100, 127}, {"STEADY", 50, 100, 127}), 2, 8, 2, 0, 0, 0, 0},
    /* stone friction (minerals): a slow rub .. scraping hard and fast */
    {"mineral", -45, -45, -45, CURVES({"SPEED", 30, 64, 100}, {"PRESSURE", 40, 64, 100}), 2, 8, 2, 0, 0, 0, 0},
    /* rockfall (stones): a fall now and then .. falls every few seconds, bigger */
    {"rockfall", 80, 80, 80, CURVES({"VOLUME", 0, 64, 110}, {"FRAG", 40, 64, 100}), 0, 12, 0,
     300, 100, 60, 1},
    /* ringing rocks (stones): a boulder struck now and then .. struck quickly */
    {"stonedrum", -50, -50, -50, NO_CURVES, 0, 0, 0,
     60, 12, 3, 1},
    /* whistling wires (wind): a breath (1.6 m/s) .. a gale (20 m/s); gusts deepen; at the top the strings lock in */
    {"aeolian", -78, -78, -78, CURVES({"WIND", 45, 88, 127}, {"GUST", 15, 40, 90}, {"LOCK", 60, 90, 120}), 0, 0, 0, 0, 0, 0, 0},
    /* hail and sleet (rain): a lone stone now and then, a small patch heard .. a hailstorm hammering, bigger stones */
    {"rainsurf", -125, -125, -125, CURVES({"RATE", 34, 115, 127}, {"DROPS", 40, 64, 100}, {"AREA", 30, 90, 90}), 0, 12, 0, 0, 0, 0, 0},
    /* drips in a cave (water): a drop into a pool now and then, from high up .. drip points all over, quicker (one per
     * 10 s .. 4 /s each, at random: a cave's drips are Poisson) */
    {"drip", -135, -135, -135, CURVES({"RATE", 0, 38, 88}, {"HEIGHT", 95, 100, 105}, {"SIZE", 70, 70, 70}, {"CLUMP", 0, 0, 0},
                     {"DEPTH", 30, 30, 30}, {"CAVE", 64, 64, 64}), 4, 12, 8, 0, 0, 0, 0},   /* (k0 4: each drip point its own: they
                                                                     * begin at their own pace, not as a burst) */
    /* eaves dripping (water): the run-off after heavy rain, a dribble of small drops all along the edge (DRIBBLE, never
     * a noise bed) under the bigger drips off the eaves, in runs and quiet spells as the wind shakes it; a thin
     * dribble .. pouring off the gutter */
    {"drip", -105, -105, -105, CURVES({"RATE", 0, 90, 120}, {"CLUMP", 60, 60, 70}, {"HEIGHT", 60, 64, 70}, {"SIZE", 45, 45, 45},
                     {"SCATTER", 40, 40, 40}, {"DRIBBLE", 80, 115, 127}), 4, 12, 8, 0, 0, 0, 0},
    /* a brook (water): a little gurgle now and then .. a babbling brook .. a rushing one (COUNT: the riffles heard) */
    {"stream", -113, -113, -113, CURVES({"FLOW", 0, 64, 95}, {"EDDY", 18, 21, 30}, {"NEAR", 80, 64, 50}, {"SPREAD", 80, 64, 45},
                             {"ROAR", 0, 0, 30}, {"SIZE", 90, 90, 90}), 2, 8, 2, 0, 0, 0, 0},
    /* a bubbling spring (water): water welling up through the sand, a gulp now and then .. bubbling on, bigger bubbles
     * close by */
    {"stream", -82, -82, -82, CURVES({"FLOW", 0, 15, 40}, {"SIZE", 45, 45, 51}, {"NEAR", 100, 100, 100}, {"EDDY", 30, 30, 32},
                             {"SPREAD", 30, 30, 30}), 2, 8, 2, 0, 0, 0, 0},
    /* a waterfall (water): a thin fall now and then .. pouring .. a big fall's roar from farther off */
    {"stream", -100, -100, -100, CURVES({"FLOW", 30, 95, 105}, {"ROAR", 0, 8, 18}, {"SIZE", 30, 30, 30}, {"NEAR", 20, 20, 20},
                             {"EDDY", 10, 12, 14}, {"SPREAD", 127, 127, 127}), 2, 6, 2, 0, 0, 0, 0},
    /* waves on a sandy beach (water): a small wave every 15 s .. breakers every 6 s, higher (COUNT: the engine's wave
     * trains along the shore, up to 8; past them higher); no far-surf floor, the sea starts partway into a period */
    {"surf", -25, -25, -25, CURVES({"HEIGHT", 0, 64, 110}, {"PERIOD", 127, 70, 30}, {"BACKWASH", 20, 26, 32}, {"DIST", 70, 64, 50},
                     {"FLOOR", 0, 0, 0}, {"STRIKE", 0, 0, 0}), 0, 10, 0, 0, 0, 0, 0},
    /* waves on shingle (water): the same sea on a stony beach close by: the backwash draining loud through the stones */
    {"surf", -100, -100, -100, CURVES({"HEIGHT", 0, 50, 100}, {"PERIOD", 127, 80, 40}, {"BACKWASH", 50, 58, 66}, {"DIST", 40, 40, 40},
                     {"FLOOR", 40, 40, 40}, {"STRIKE", 0, 0, 0}, {"SHORE", 127, 127, 127}), 0, 10, 0, 0, 0, 0, 0},
    /* lake lapping (water): ripples lapping now and then .. a lake's short waves in a breeze, higher and quicker
     * (TYPE: on a sloping shore .. against a hull or a jetty; COUNT: the engine's wave trains, up to 8) */
    {"surf", -65, -65, -65, CURVES({"WATER", 1, 1, 1}, {"STRIKE", 0, 0, 0}, {"HEIGHT", 0, 64, 110}, {"PERIOD", 127, 80, 50},
                     {"CHOP", 10, 10, 10}, {"DIST", 50, 30, 20}, {"FLOOR", 0, 0, 0}), 0, 0, 0, 0, 0, 0, 0},
};

/* the loud hosted sounds' nearest gain, Q12 (the rest: 32767, the source's own cap): their peaks at ACTIVITY 127 a few
 * metres nearer than their own place stay under 16x full scale (txtest's max scene, every seed); the levels farther
 * out are their own. At 0.5 m (PROXIMITY 127) and ACTIVITY 127: fire -5.2 dB (under the 32767 it was held at), gravel
 * -2.5, bees -2.9, glass -5.9 (under their 1 / r); after the output limiter ~1-2 LU quieter, the defaults unchanged */
typedef struct { const char *eng; uint16_t g; } near_t;
static const near_t NEAR[] = {{"fire", 18000}, {"gravel", 22100}, {"bees", 10500}, {"glass", 12500}};
static uint16_t host_near(const char *eng)
{
    unsigned i, k;
    for (i = 0; i < sizeof NEAR / sizeof NEAR[0]; i++) {
        for (k = 0; NEAR[i].eng[k] && NEAR[i].eng[k] == eng[k]; k++) ;
        if (NEAR[i].eng[k] == eng[k]) return NEAR[i].g;
    }
    return 32767;
}
#define H_MEMBER 12u                 /* the first member row */

typedef struct {
    tx_tymbal_t ty;
    tx_ireso_t lo;                  /* a small cicada's second band (~0.42 x its carrier, broad: its own resonator, as a
                                     * switched tymbal pitch leaks a low floor), its input differenced once more (lc: the */
    int16_t lc[2];                  /* pull-backs due in the next block's first two samples) */
    tx_tev_t pend[NPEND];
    uint32_t next_q8, per_q8, f;
    int32_t a1s, a1o, dur, jit;
    uint8_t npend, side, member, pad;
} caller_t;

typedef struct {
    uint8_t kind, pre, pad[2];      /* (pad[1]: a stone's first sample in the block it was struck in) */
    int32_t last;                   /* for the declick when cut */
    union {
        tx_bubble_t b;
        struct { tx_mode_t m[4]; } s;
        caller_t c;
        tx_bird_voice_t bv;
    } u;
} voice_t;

typedef struct {                    /* a caller of the population */
    int32_t f_l2, rate_l2, r_l2;    /* carrier (log2 Hz), click rate (log2 Hz), distance (log2 m), Q16 */
    int32_t gain;                   /* Q12 (1 / r, the air at its carrier, the preset's level) */
    int32_t env;                    /* the swell, Q15 */
    uint32_t pph, pinc;             /* the phrase (pulse groups): phase and step per block */
    int32_t pmod;                   /* the phrase gain now, Q15 */
    uint8_t calling, voice, fresh, pad;
    uint8_t song, fade;             /* in a song bout (else pausing); its end's fade (blocks per e-fold, log2) */
    int8_t q_off, pad2;             /* its own tymbal Q (timbre), 1/32 octave; a small cicada: its call's age (0 .. 127) */
    uint16_t bout;                  /* blocks to the bout's end */
    uint16_t pad3;
    uint16_t gmul, gpad;            /* this pulse group's length (the step x gmul, Q14; 0: 1) */
} member_t;

#define NB 16u                      /* birds in a lane at most (12 sing at the top) */
typedef struct {                    /* a bird of the population: persistent, its own voice and songs */
    int32_t f_off, r_l2, gain;      /* its pitch (log2 Q16 re its type), distance (log2 m Q16), gain (Q12) */
    uint32_t seed, wait;            /* its songs' seed; samples to its next song */
    uint32_t sung;                  /* samples of the song now */
    uint8_t voice, songs, active, near;
} birdm_t;


#define NI 64u                      /* individuals (COUNT) at most */
typedef struct { int32_t r_l2; int16_t sz; uint16_t life; } indiv_t;   /* its distance, size (log2 Q12), blocks to go */

typedef struct {
    tx_rng_t rng;
    tx_rng_t grng;                  /* the cicadas' group lengths (own draws: the population as drawn) */
    tx_pool_t pool;
    tx_declick_t dc;
    tx_dcblock_t hp;
    union {                         /* the particle presets' pools, or a hosted engine's whole state */
        struct {
            voice_t v[NV];
            union { struct { member_t m[NM]; tx_grains_t cg; }; birdm_t bm[NB]; tx_grains_t gr; };   /* (gr: rain, stones; cg: the far callers) */
        };
        uint64_t host[HOST_BYTES / 8];
    };
    const tx_engine_t *hst;         /* the hosted engine now (0: the particle presets) */
    const tx_engine_t *wst;         /* water: the stream engine under the drops (its state at host + WATER_OFF) */
    const void *hcur;               /* what the arena holds now (a host, the water, or 0) */
    int8_t wid[HOST_NC];            /* the water stream's parameter ids */
    uint8_t wval[HOST_NC], wheld, wnote;  /* (wnote: the key the stream holds, released by the same key) */
    const host_t *hd;
    int8_t hid[HOST_NC];            /* its curves' parameter ids */
    uint8_t hval[HOST_NC];          /* the values last sent */
    int8_t kid[HOST_NK], kcur[HOST_NK];   /* the character maps: parameter ids, the curve they offset (-1: none) */
    uint8_t kdef[HOST_NK], kval[HOST_NK]; /* the engine's own value, the value last sent */
    const clu_pre_t *hclu;          /* the hosted engine's press clock character (its preset's, or its member's) */
    int8_t hreg;                    /* and spacing */
    uint8_t tlo, tspan, tpad;       /* TYPE's range for the kind now: its value = (TYPE - tlo) x 127 / tspan */
    uint8_t lvl_v, part_v;          /* LEVEL and PARTICLES as last set (0: not set) */
    uint8_t hkeys, hbase, hhot, hon;  /* (hon: the player's key is down on the engine: held, COUNT > 0; hhot: its
                                     * heroes as cut for a fast press clock, HOT; 0: none) */
    int8_t hcid, hfid;              /* the engine's own COUNT (its individuals) and FIELD (its far callers), -1: none */
    uint8_t hcnt, hpart, hfld, hgate;   /* the COUNT, PARTICLES and FIELD last sent to it; the bout gate open */
    uint16_t hgt;                   /* blocks to the gate's next turn */
    uint32_t ovm;                   /* the dev view's overrides: the hosted engine's own parameters (bit = id < 32) set
                                     * directly, winning over the curves and maps until the sound changes */
    uint8_t ovv[32];                /* their values */
    uint8_t start, hpad4;           /* start: a first press (the population starts: ACTIVITY at the fader) */
    uint16_t hrenew;                /* blocks to the next renewed key */
    uint8_t hoff, hpn, koff, hpad2; /* blocks to its release, its note; koff: TX_PID_KEYAT (a lane key's sample) */         /* keys the source holds on the hosted engine (k curves; the first is the player's) */
    int32_t hgain, last_out, hlp;        /* the hosted output gain (Q12, ramped), the last sample out (for a switch) */
    int32_t buf[TX_BLOCK], ck[TX_BLOCK];
    uint8_t p[NP], limit, quiet, held, pad0;
    uint8_t note, vel, pad[2];
    int32_t level;
    int32_t run;                    /* the source on (Q15 ramp) */
    uint32_t now;                   /* sample clock */
    int32_t i_eff;                  /* ACTIVITY with LIFE, Q8 knob units */
    int32_t i_glide;                /* ACTIVITY gliding to the fader, Q8, on the sound's own path (lin_at) */
    int32_t u_glide;                /* the fader gliding, Q8 knob units (each step sounds the same more) */
    const uint8_t *lsnd;            /* the sound's own measured curve (tx_src_lin.h), or 0: the path's (lin_row) */
    int32_t trim_c;                 /* the callers' level now (pre_trim), Q12 */
    tx_life_t la, ls, ld;           /* LIFE: intensity, size, distance */
    tx_life_t lw;                   /* the wind's gusts (rain on water: HARD) */
    int32_t gust;                   /* their walk now, Q15 */
    int32_t life_s, life_d;         /* their offsets now (Q16 log2) */
    /* the beds: rain's two bands, stones' residue, the callers' chorus */
    int32_t res, res_x1, res_in;    /* the stones' attack residue; res_in: this block's, added at sample res_at */
    int32_t clp;                    /* click low-pass */
    uint16_t dropped, events;
    uint32_t total, steals;
    tx_mallet_t mallet;
    uint32_t pm;                    /* the stones' power-law cut */
    tx_vary_t vy;
    uint32_t x32;                   /* bed noise */
    tx_clu_t clu;                   /* stage 1's clock: Cox walk, children (tx_core_cluster.h) */
    indiv_t ind[NI];                /* the events' individuals (rain patches, stones, drip points), ni drawn */
    uint8_t ni, cur_on, res_at, ipad1;
    int32_t cur_r, cur_sz;          /* the individual of the event being made (cur_on) */
    uint32_t ev_est;                /* rain: its events a block lately (Q8), for the thinning (EV_BLK) */
    uint32_t owe_q8;                /* rain: the drops held back by EV_HARD, carried by the next kept one (Q8) */
    uint16_t hnear;                 /* the hosted sound's nearest gain (Q12; 0: 32767) */
    uint8_t hskip, pad_n;           /* 1: the hosted sound was switched in for this block (it renders from the next) */
    tx_clu_cfg_t cf;                /* its character at the activity now */
} src_t;

/* ---- helpers */
static int32_t clampi(int32_t x, int32_t lo, int32_t hi) { return x < lo ? lo : x > hi ? hi : x; }
static int32_t knob(const src_t *s, unsigned id) { return (int32_t)s->p[id] - 64; }   /* centred knobs: -64 .. 63 */
static int32_t gauss(src_t *s) { return tx_life_gauss(&s->rng); }                    /* N(0, 1), Q12 */
static uint32_t isqrt(uint32_t x)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) { if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return r;
}
/* the distance: the preset's own (3 m: the particle presets' levels are set there) halved every 16 steps of PROXIMITY */
static int32_t dist_l2(const src_t *s)                    /* (at most 0.5 m: arm's length, the peaks' headroom) */
{
    int32_t d = 103872 - ((int32_t)s->p[P_PROX] - 64) * 4096 + s->life_d;
    return d < -65536 ? -65536 : d;
}
static int32_t depth_l2(const src_t *s) { return (int32_t)s->p[P_DEPTH] * 1548; }   /* 0 .. 3 octaves */
/* the morph weight of B (Q15) */
static int32_t morph(const src_t *s)
{
    (void)s;                                                 /* no morphing (the user: the activity fader is the change */
    return 0;                                                /* within one nature of sound); the particle-level mixing */
}                                                            /* stays below, inert, for a family's stages later */

/* the effective activity of a preset's rate (Q8 knob units, 0 .. 127 << 8) */
static int32_t act_idx(const src_t *s) { return s->i_eff; }   /* (ACTIVITY is the one rate knob: the old trim merged in) */
/* the preset's rate (events/s Q8) or callers (Q8) at the effective intensity */
/* water: the drops' rate along the fader (log2 /s at ACTIVITY 0, 40, 90, 127: one per 30 s .. 2.8 /s (dripping: the
 * user's references 3 and 4) .. 60 /s (a trickle of drops) .. 40 /s as the flow takes over) */
/* the stream inside the water, along the fader: its gain (tenths of a dB; off below 36) and FLOW, ROAR, EDDY, NEAR,
 * SPREAD at ACTIVITY 36, 64, 100, 127: a trickle under the drops .. a brook .. a river with its roar */
#define WATER_ON (36 << 8)
#define WATER_OFF ((8u * sizeof(voice_t) + 63u) & ~63u)   /* the stream's state, after the drops' 8 heroes */
#define WATER_HEROES 16u            /* the stream's bubble heroes at most (its 16 cost ~500 us at the top: 8 until the
                                     * second core and the cache layout's room) */
static const int32_t WATER_AT[4] = { 36 << 8, 64 << 8, 100 << 8, 127 << 8 };
static const int32_t WATER_G[4] = { -360, -100, -100, -150 };
static const int32_t WATER_C[HOST_NC][4] = { {0, 30, 100, 127}, {0, 0, 40, 80}, {20, 30, 45, 60}, {80, 64, 55, 50},
                                             {80, 64, 55, 45} };
/* piecewise linear over the points at[0..3] (32-bit: the fraction first, Q8) */
static int32_t pw4(const int32_t *at, const int32_t *y, int32_t x)
{
    int k = 0;
    if (x <= at[0]) return y[0];
    if (x >= at[3]) return y[3];
    while (k < 2 && x > at[k + 1]) k++;
    return y[k] + (((y[k + 1] - y[k]) * (((x - at[k]) << 8) / (at[k + 1] - at[k]))) >> 8);
}

/* COUNT: the individuals, Q8 (0: none; perceptually even, 1 + 63 x^2.5 over 1 .. 127 (tx_core_lin.h): 1, 20, 39, 53,
 * 58, 77, 96, 127 = 1, 1.6, 4.1, 8, 9.7, 19, 32, 64) and whole (at least 1 when any) */
static uint32_t count_q8(const src_t *s)
{
    unsigned v = s->p[P_COUNT];
    return v ? 256u + ((16128u * (tx_lin_pow(((uint32_t)(v - 1u) << 16) / 126u) >> 4)) >> 12) : 0u;
}
/* COUNT as it was on its old doubling scale (1 + 21 log2 n), for what still counts in doublings (the keys a hosted
 * engine holds) */
static int32_t count_kl(const src_t *s)
{
    uint32_t q = count_q8(s);
    return q ? 1 + ((21 * (tx_place_log2(q) - (8 << 16)) + 32768) >> 16) : 0;
}
static unsigned count_n(const src_t *s)
{
    uint32_t c = (count_q8(s) + 128u) >> 8;
    return c > NI ? NI : c ? c : s->p[P_COUNT] ? 1u : 0u;
}

/* each individual's events at ACTIVITY 0, 9, 64, 127 (log2 /s, Q16): a rain patch 0.2, 0.3, 500, 1500 (a stray drop ..
 * a downpour on it); a stone (rolling, or a pouring point) 0.05, 0.1, 1.25, 60 (the top a steady slide); a drip point 1 / 40 s, 1 / 15 s, 1.5,
 * 8 /s (ours: COUNT 64 = 8 of them at ACTIVITY 64 as the old one-fader scale there) */
static const int32_t IND_AT[4] = { 0, 9 << 8, 64 << 8, 127 << 8 };
static const int32_t IND_Y[3][4] = {
    { -152170, -110500, 587500, 691400 },    /* rain */
    { -283000, -217700, 21100, 387000 },     /* stones */
    { -348800, -256000, 38300, 196600 },     /* water: drip points */
};

/* the events per second (Q8) of all the individuals, or the callers (kind 1, Q8) */
static uint32_t pre_amount(const src_t *s, unsigned pre)
{
    const preset_t *q = &PRE[pre];
    int32_t a = act_idx(s);
    if (q->kind == 1) return count_q8(s);
    if (q->kind == 0 || q->kind == 3) {
        const int32_t *y = IND_Y[pre == PR_RAIN ? 0 : pre == PR_STONES ? 1 : 2];
        int32_t w = pre == PR_RAIN && knob(s, P_HARD) > 0 ? (s->gust * knob(s, P_HARD)) / 12 : 0;   /* WIND (HARD over
                                                             * the middle): the gusts swell and slacken the rain, up to
                                                             * +-2.6 octaves of its rate at the walk's sd */
        return (uint32_t)(((uint64_t)tx_exp2(pw4(IND_AT, y, a) + w + (8 << 16)) * count_q8(s)) >> 8);
    }
    {
        int32_t l = q->act_lo + (((q->act_oct >> 5) * a) / (127 << 8)) * 32;   /* 32-bit: < 2^30 */
        return tx_exp2(l + (8 << 16));
    }
}

/* the preset's level at the effective intensity (Q12): its trim x the fader law, amplitude ~ amount^(-1/4) around
 * the middle (ours: so a thousandfold rate is +15 dB, not +30: one fader stays playable from a drizzle to a downpour) */
static int32_t pre_trim(const src_t *s, unsigned pre)
{
    const preset_t *q = &PRE[pre];
    int32_t l = -((((q->act_oct >> 5) * (act_idx(s) - (64 << 8))) / (127 << 8)) * q->law);   /* -oct (a - 64) / 127 / 4 */
    return (int32_t)(((int64_t)q->trim * (int32_t)tx_exp2(l + (12 << 16))) >> 12);
}

/* the hosted engines' character: the SOURCE's SIZE, TONE, HARD, SPREAD (centred at 64: the engine's own value or its
 * ACTIVITY curve, +-depth over the knob's range) and TYPE (0: the engine's own; above, its value directly) reach each
 * engine's own parameters, so a hosted preset covers its family's sizes, species, materials and surfaces (note 16).
 * Indexed as HOST[]. */
typedef struct { const char *name; uint8_t knob; int8_t depth; } kmap_t;   /* depth 0 with P_TYPE: the value itself */
/* a row's maps are a list of their own, as long as the row has maps (KMAPS: the list and its length; a map past n is
 * off, as a nameless one): no HOST_NK slots a row */
#define KMAPS(...) (const kmap_t[]){__VA_ARGS__}, (uint8_t)(sizeof (const kmap_t[]){__VA_ARGS__} / sizeof(kmap_t))
typedef struct { const kmap_t *k; uint8_t n; } kmap_row_t;
static const kmap_row_t HMAP[] = {
    /* wind: the place (TYPE 1 .. 63: grass, pines, leafy trees, a moor, the coast, a ridge, autumn, a winter storm, a
     * desert), bigger obstacles lower, leafier, gustier, more gusts passing; COUNT: the keys held, 1 .. 4 (more gust
     * sources and more canopy around) */
    {KMAPS({"SCALE", P_SIZE, -64}, {"FOLIAGE", P_TONE, 64}, {"GUST", P_HARD, 48}, {"WHOOSH", P_SPREAD, 48}, {"KIND", P_TYPE, 0})},
    {0, 0},                       /* (stream: inside the water) */
    /* fire: a bigger fire, drier wood, burstier crackle, a draught */
    {KMAPS({"SIZE", P_SIZE, 64}, {"WOOD", P_TONE, 64}, {"BURSTY", P_HARD, 64}, {"DRAUGHT", P_SPREAD, 48}, {"KIND", P_TYPE, 0})},
    /* crickets: species, warmer (faster), less in step */
    {KMAPS({"SPECIES", P_TYPE, 0}, {"TEMP", P_TONE, 24}, {"SYNC", P_SPREAD, -64})},
    /* sand: grain, landing surface, container, fall height, pulsing */
    {KMAPS({"GRAIN", P_SIZE, 64}, {"SURFACE", P_TYPE, 0}, {"SIZE", P_TONE, 64}, {"HEIGHT", P_HARD, 64}, {"PULSE", P_SPREAD, 48})},
    /* gravel: stone size, the ground (a box, a floor), crunch .. rattle, grain shapes */
    {KMAPS({"SIZE", P_SIZE, 64}, {"GROUND", P_TONE, 64}, {"SHAKE", P_HARD, -64}, {"SPREAD", P_SPREAD, 64})},
    /* katydids: species, warmer, pegs, rasp */
    {KMAPS({"SPECIES", P_TYPE, 0}, {"TEMP", P_TONE, 48}, {"PEGS", P_HARD, 64}, {"JITTER", P_SPREAD, 64})},
    /* bees: species, flight speed, wingbeat spread */
    {KMAPS({"SPECIES", P_TYPE, 0}, {"SPEED", P_HARD, 48}, {"SPREAD", P_SPREAD, 64})},
    /* ice: icicles or cubes, size, colder (ringing) .. wet, more chunks */
    {KMAPS({"ACTION", P_TYPE, 0}, {"SIZE", P_SIZE, 64}, {"TEMP", P_TONE, -64}, {"AMOUNT", P_HARD, 48})},
    /* wood: species, piece length, dry .. wet, creak pitch */
    {KMAPS({"WOOD", P_TYPE, 0}, {"SIZE", P_SIZE, 64}, {"MOIST", P_TONE, -64}, {"PITCH", P_HARD, 64})},
    /* minerals: mineral, specimen size (bigger = lower), free ring .. gripped, strike .. clink .. rub */
    {KMAPS({"MINERAL", P_TYPE, 0}, {"SIZE", P_SIZE, -64}, {"HOLD", P_TONE, -64}, {"ACTION", P_HARD, 64})},
    /* glass: shard size, ring (soft floor .. crystal), bounce, height, kind (a pane, bottles, shards) */
    {KMAPS({"SIZE", P_SIZE, 64}, {"RING", P_TONE, 64}, {"BOUNCE", P_HARD, 64}, {"HEIGHT", P_SPREAD, 48}, {"KIND", P_TYPE, 0})},
    /* rain on surfaces: the surface (leaves .. metal roof), drop sizes, the area heard, inside */
    {KMAPS({"SURFACE", P_TYPE, 0}, {"DROPS", P_SIZE, 64}, {"AREA", P_SPREAD, 64}, {"INSIDE", P_TONE, -64})},
    /* lake ice: thicker (lower), colder (ringing), farther (harder: nearer), fresh .. sea ice */
    {KMAPS({"ICE", P_TYPE, 0}, {"THICK", P_SIZE, 64}, {"COLD", P_TONE, 64}, {"DIST", P_HARD, -64})},
    /* balls: the surface (wood .. metal), radius, rough, out of round, drag */
    {KMAPS({"SURFACE", P_TYPE, 0}, {"SIZE", P_SIZE, 64}, {"ROUGH", P_HARD, 64}, {"ROUND", P_SPREAD, 64}, {"DRAG", P_TONE, -64})},
    /* flame: size, its roar's slope, sputter, spacing */
    {KMAPS({"SIZE", P_SIZE, 64}, {"SLOPE", P_TONE, -64}, {"SPUTTER", P_HARD, 64}, {"SPACING", P_SPREAD, 64})},
    /* dunes: grain (bigger = lower), sorting, detune */
    {KMAPS({"GRAIN", P_SIZE, 64}, {"SORTING", P_SPREAD, 64}, {"DETUNE", P_TONE, 64})},
    /* friction: the stone, its pitch (bigger = lower), rough, stick-slip */
    {KMAPS({"STONE", P_TYPE, 0}, {"SIZE", P_SIZE, -64}, {"ROUGH", P_HARD, 64}, {"STICK", P_TONE, 64})},
    /* rockfall: volume, ground (soft .. hard), fragmentation, height */
    {KMAPS({"VOLUME", P_SIZE, 64}, {"GROUND", P_HARD, 64}, {"FRAG", P_SPREAD, 64}, {"HEIGHT", P_TONE, 64})},
    /* ringing rocks: material, its tuning (bigger = lower), mallet, a hand on it, where struck */
    {KMAPS({"MATERIAL", P_TYPE, 0}, {"TUNE", P_SIZE, -64}, {"MALLET", P_HARD, -64}, {"DAMP", P_TONE, -64}, {"POS", P_SPREAD, 64})},
    /* whistling wires: thicker wires lower, locking harder, gustier */
    {KMAPS({"WIRE", P_SIZE, 64}, {"LOCK", P_TONE, 48}, {"GUST", P_HARD, 48})},
    /* hail: the kind (a metal roof, a car, leaves, sleet on a window), stone sizes, the area heard, inside */
    {KMAPS({"HAIL", P_TYPE, 0}, {"DROPS", P_SIZE, 64}, {"AREA", P_SPREAD, 64}, {"INSIDE", P_TONE, -64})},
    /* drips (cave, eaves): bubble size, fall height, the pool (TONE up: shallower, the plinks rise; down: deep plops),
     * size spread, rhythm (SPACING: random .. in runs; no steady end, the no-regular-beat rule); the cave's tail on
     * EXTRA (its card: CAVE; 0 = dry) */
    {KMAPS({"SIZE", P_SIZE, 64}, {"HEIGHT", P_HARD, 64}, {"DEPTH", P_TONE, -64}, {"SCATTER", P_SPREAD, 64},
     {"CLUMP", P_SPACING, 64}, {"CAVE", P_EXTRA, 64})},
    {KMAPS({"SIZE", P_SIZE, 64}, {"HEIGHT", P_HARD, 64}, {"DEPTH", P_TONE, -64}, {"SCATTER", P_SPREAD, 64},
     {"CLUMP", P_SPACING, 64})},
    /* streams (brook, spring, waterfall): bubble size, nearer the surface (babble), eddies (GURGLE), size spread; the
     * low roar on EXTRA (its card: ROAR) */
    {KMAPS({"SIZE", P_SIZE, 64}, {"NEAR", P_TONE, 48}, {"EDDY", P_HARD, 48}, {"SPREAD", P_SPREAD, 64}, {"ROAR", P_EXTRA, 64})},
    {KMAPS({"SIZE", P_SIZE, 64}, {"NEAR", P_TONE, 48}, {"EDDY", P_HARD, 48}, {"SPREAD", P_SPREAD, 64}, {"ROAR", P_EXTRA, 64})},
    {KMAPS({"SIZE", P_SIZE, 64}, {"NEAR", P_TONE, 48}, {"EDDY", P_HARD, 48}, {"SPREAD", P_SPREAD, 64}, {"ROAR", P_EXTRA, 64})},
    /* waves (sand, shingle): height, backwash, chop, the far surf (FLOOR; distance is PROXIMITY's); the period on
     * EXTRA (its card: PERIOD; up = longer) */
    {KMAPS({"HEIGHT", P_SIZE, 64}, {"BACKWASH", P_TONE, 64}, {"CHOP", P_HARD, 64}, {"FLOOR", P_SPREAD, 64}, {"PERIOD", P_EXTRA, 64})},
    {KMAPS({"HEIGHT", P_SIZE, 64}, {"BACKWASH", P_TONE, 64}, {"CHOP", P_HARD, 64}, {"FLOOR", P_SPREAD, 64}, {"PERIOD", P_EXTRA, 64})},
    /* lake lapping: what the waves meet (TYPE: a shore .. a hull), height, chop; the period on EXTRA (its card:
     * PERIOD; up = longer) */
    {KMAPS({"FACE", P_TYPE, 0}, {"HEIGHT", P_SIZE, 64}, {"CHOP", P_HARD, 64}, {"PERIOD", P_EXTRA, 64})},
};

/* the family members TYPE reaches (note 16: every family had members no knob could reach): TYPE from lo up to the
 * next row's lo (or 127) runs HOST[host] in place of the preset's own sound, its kind parameter (the row's P_TYPE map)
 * swept over that range; below the first member, TYPE sweeps the preset's own kinds (a hosted preset) */
typedef struct { uint8_t pre, lo, host; clu_pre_t clu; } fam_t;
static const fam_t FAM[] = {
    {PR_RAIN, 1, H_MEMBER + 0, {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {0, 0}, {0, 0}, 0}},
    {PR_ICE, 64, H_MEMBER + 1, {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {0, 0}, {0, 0}, 0}},
    {PR_GRAVEL, 64, H_MEMBER + 2, {{60, 60, 60}, 30, {20, 30, 40}, 0, 2, 300, -5, 5, {0, 0}, {0, 0}, 0}},
    {PR_FIRE, 64, H_MEMBER + 3, {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {0, 0}, {0, 0}, 0}},
    {PR_SAND, 64, H_MEMBER + 4, {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {0, 0}, {0, 0}, 0}},
    {PR_MINERALS, 64, H_MEMBER + 5, {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {0, 0}, {0, 0}, 0}},
    {PR_STONES, 1, H_MEMBER + 6, {{80, 80, 80}, 50, {20, 30, 40}, 0, 2, 1500, -5, 5, {0, 0}, {0, 0}, 0}},
    {PR_STONES, 64, H_MEMBER + 7, {{60, 60, 60}, 30, {20, 30, 40}, 1, 3, 200, -5, 10, {0, 0}, {0, 0}, 0}},
    {PR_WIND, 64, H_MEMBER + 8, {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {0, 0}, {0, 0}, 0}},
    {PR_RAIN, 100, H_MEMBER + 9, {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {0, 0}, {0, 0}, 0}},   /* hail: 101 .. 127 */
    {PR_WATER, 16, H_MEMBER + 10, {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {0, 0}, {0, 0}, 0}},   /* cave drips */
    {PR_WATER, 32, H_MEMBER + 11, {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {0, 0}, {0, 0}, 0}},   /* eaves */
    {PR_WATER, 48, H_MEMBER + 12, {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {0, 0}, {0, 0}, 0}},   /* a brook */
    {PR_WATER, 64, H_MEMBER + 13, {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {0, 0}, {0, 0}, 0}},   /* a spring */
    {PR_WATER, 80, H_MEMBER + 14, {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {0, 0}, {0, 0}, 0}},   /* a waterfall */
    {PR_WATER, 96, H_MEMBER + 15, {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {0, 0}, {0, 0}, 0}},   /* waves on sand */
    {PR_WATER, 112, H_MEMBER + 16, {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {0, 0}, {0, 0}, 0}},   /* waves on shingle */
    {PR_WATER, 120, H_MEMBER + 17, {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {0, 0}, {0, 0}, 0}},   /* lake lapping: a
                                                                     * shore 120 .. a hull 127 */
};
#define NFAM (sizeof FAM / sizeof FAM[0])

/* heroes at most for a hosted engine whose heroes are dear (FM-1 worst block under half the pool, ~870 us) */
static const struct { const char *eng; uint8_t max; } HCAP[] = {
    {"stonedrum", 4},               /* (ringing rocks ring ~4 s: 6 heroes reached w8 ~1000 us at ACTIVITY 127, 4 ~640 us) */
    {"minerals", 3},                /* (~150 us a hero at ACTION's rub: 6 reached 1170 us at COUNT 127, 4 957 us) */
    {"surf", 6},                    /* (shingle's top: 8 heroes reached ~940 us on the FM-1, 18 us a hero) */
    {"stream", 8},                  /* (a brook's or a waterfall's top: 16 heroes reached ~1100 us, 10 ~900 us; as
                                     * WATER_HEROES, the rest go to the bed) */
};
static unsigned host_heroes(const host_t *hd, unsigned v)
{
    unsigned i, k;
    for (i = 0; hd && i < sizeof HCAP / sizeof HCAP[0]; i++) {
        for (k = 0; hd->eng[k] && hd->eng[k] == HCAP[i].eng[k]; k++) { }   /* (no libc: the firmware) */
        if (hd->eng[k] == HCAP[i].eng[k] && v > HCAP[i].max) return HCAP[i].max;
    }
    return v;
}

typedef char hmap_rows_ok[(sizeof HMAP / sizeof HMAP[0]) == (sizeof HOST / sizeof HOST[0]) ? 1 : -1];

static uint32_t gate_draw(src_t *s, uint32_t mean);

/* stage 1's character now (once per block) */
static void clu_cfg_row(src_t *s, const clu_pre_t *c, int32_t reg)
{
    tx_clu_cfg_t *f = &s->cf;
    int32_t a = act_idx(s);
    f->cox = tx_clu_at(c->cox[0], c->cox[1], c->cox[2], a) * 655;
    f->cox_tau_l2 = c->ctau ? tx_place_log2(c->ctau) + 269090 : 0;   /* x 17.23 blocks per 0.1 s */
    f->branch = tx_clu_at(c->br[0], c->br[1], c->br[2], a) * 655;
    f->tau = (int32_t)c->tau_ms * 441 / 10;
    f->child_l2 = c->child * 6554;
    f->child_sd = c->csd * 6554;
    f->reg = clampi(reg + knob(s, P_SPACING), -64, 0);   /* (Poisson at most: no regular beat) */
    f->omori = c->omori; f->gens = c->gens;
}
static void clu_cfg(src_t *s, unsigned pre) { clu_cfg_row(s, &CLU[pre], PRE[pre].reg); }

/* an event's distance: the preset's salience law, or (dim 0) the plain draw around DIST */
static int32_t ev_dist(src_t *s, unsigned pre)
{
    const clu_pre_t *c = &CLU[pre];
    int32_t d = depth_l2(s), a = s->i_eff >> 4;               /* (0 .. 2032) */
    int32_t nr = c->near[0] * 2032 + (c->near[1] - c->near[0]) * a, sp = c->span[0] * 2032 + (c->span[1] - c->span[0]) * a;
    if (s->cur_on) return s->cur_r;                         /* (an individual's event: its place, drawn as below) */
    if (!c->dim) return tx_place_draw(&s->rng, dist_l2(s), d);
    nr = nr * 16 / 2032; sp = sp * 16 / 2032;                /* 1/160 of DEPTH's spread */
    return tx_clu_dist(&s->rng, dist_l2(s) + (nr * d) / 160, (sp * d) / 160, c->dim);
}

/* the far events' grains (tx_core_grains.h): rain's bubbles and splashes, the stones' far knocks */
static void grains_setup(src_t *s, unsigned pre)
{
    if (PRE[pre].kind == 1) {                                /* far callers: a click rings the body at its carrier (Q 12) */
        static const uint16_t E[7] = { 800, 1600, 3000, 5000, 7500, 10500, 15000 };
        unsigned i;
        tx_gr_init(&s->cg);
        for (i = 0; i < 6; i++) tx_gr_band(&s->cg, i, E[i], E[i + 1], 192, 256);
        return;
    }
    tx_gr_init(&s->gr);
    if (pre == PR_STONES) {                                  /* a far knock: its piece rings a moment (Q 20) */
        tx_gr_band(&s->gr, 0, 300, 1200, 320, 256);
        tx_gr_band(&s->gr, 1, 1200, 3000, 320, 256);
        tx_gr_band(&s->gr, 2, 3000, 7000, 320, 256);
        tx_gr_band(&s->gr, 3, 7000, 15000, 320, 256);
        return;
    }
    tx_gr_band(&s->gr, 0, 11000, 20000, 224, 256);           /* type I bubbles (Q 14: tonal plinks, never a hiss) */
    tx_gr_band(&s->gr, 1, 2500, 11000, 224, 256);            /* type II (Q 14) */
    tx_gr_band(&s->gr, 2, 120, 700, 40, 256);                /* splashes (Q 2.5) */
    tx_gr_band(&s->gr, 3, 700, 2499, 40, 256);
}

/* ---- voices */
static void voice_cut(src_t *s, unsigned i)
{
    voice_t *v = &s->v[i];
    tx_declick_add(&s->dc, v->last);
    v->last = 0;
    if (v->kind == V_CALLER && v->u.c.member < NM) s->m[v->u.c.member].voice = 0xFF;
    if (v->kind == V_BIRD && v->pad[0] < NB && s->bm[v->pad[0]].voice == i) s->bm[v->pad[0]].voice = 0xFF;
    v->kind = V_NONE;
}

/* take a hero for an event of level lv (Q30): a free one, else the quietest settled one if quieter; -1 = none */
/* the heroes the own presets may have: the lane's PARTICLES, the stone cloud's at most ST_HEROES */
static uint32_t pool_lim(const src_t *s)
{
    return s->p[P_PRESET] == PR_STONES && s->limit > ST_HEROES ? ST_HEROES : s->limit;
}
static int take(src_t *s, uint32_t lv)
{
    uint32_t stolen;
    int k = tx_pool_take(&s->pool, lv ? lv : 1u, &stolen);
    if (k < 0) { s->dropped++; return -1; }
    if (s->v[k].kind != V_NONE) { voice_cut(s, (unsigned)k); s->steals++; }
    return k;
}

/* ---- rain */
#define A_I_L2 ((28 << 16) + 32728)
#define A_II_L2 ((28 << 16) + 10460)
#define CLICK_M 2000                /* (the impacts 6 dB under the first model: the drop's plink leads, as the references' and the water drops') */
#define CLICK_L 2350
#define BED_G 160                   /* a far bubble's grain re its own amplitude, Q8 (calibrated by render) */
#define SPLASH_G 100                /* a drop's splash grain re its click, Q8 (ours; 300 made a dull 1 kHz wash) */
#define RI_LV (2 << 16)             /* type I bubbles down 12 dB (in air a shimmer under the plinks: the references'
                                     * 12-16 kHz sit 8-12 dB under their 5-10 kHz peak) */
#define RAIN_RMIN 103872             /* log2 3 mm/h, Q16 */
#define RAIN_RTOP 387122             /* log2 60 mm/h: the sparse drops' sizes */
#define RM_P 39322u                 /* a medium drop's bubble: 0.6 (Q16; the references: most drops plink) */
#define RM_LV 0                     /* its level: as a large drop's */

/* a bubble of radius a_l2 (log2 mm, Q16) at amplitude l2 (Q30 log2), after wait, at distance r */
static void bubble(src_t *s, int32_t a_l2, int32_t amp_l2, uint32_t wait, uint32_t xi_q16, int32_t r_l2, int32_t trim,
                   unsigned pre, uint32_t bed_q8)
{
    uint32_t f = ((tx_bubble_inc(a_l2) >> 16) * TX_FS) >> 16;   /* Minnaert, Hz */
    int32_t g = (int32_t)(((int64_t)tx_place_gain_q12(r_l2) * tx_place_air_q15(f, r_l2)) >> 15);   /* Q12 */
    uint32_t env;
    int k = -1;
    amp_l2 = amp_l2 > (29 << 16) ? (29 << 16) : amp_l2;
    env = tx_exp2(amp_l2);
    env = (uint32_t)(((uint64_t)env * (uint32_t)s->level) >> 15);
    env = (uint32_t)(((uint64_t)env * (uint32_t)g) >> 12);
    env = (uint32_t)(((uint64_t)env * (uint32_t)trim) >> 12);
    if (env < TX_BUBBLE_FLOOR) return;
    if (bed_q8 || s->events >= EVMAX || (k = take(s, env)) < 0) {   /* no hero: a grain of its own (never a hiss) */
        int32_t e15 = (int32_t)(env >> 15) * BED_G >> 8;     /* amplitude Q15 */
        unsigned c = bed_q8 ? (bed_q8 + (tx_rand(&s->rng) >> 24)) >> 8 : 1u, q;
        if (c > 2u) { e15 = (int32_t)(((((int64_t)e15 * (int32_t)isqrt(c << 16)) >> 8) * 181) >> 8); c = 2u; }   /* (power kept) */
        for (q = 0; q < c; q++) {                            /* (the drops this one stands for: each its own) */
            unsigned at = (!bed_q8 && wait < TX_BLOCK) ? wait : tx_rand_below(&s->rng, TX_BLOCK);
            int32_t aj = (int32_t)(((int64_t)e15 * (int32_t)((tx_rand(&s->rng) >> 17) | 32768u)) >> 16);   /* -6 .. 0 dB */
            tx_gr_add(&s->gr, &s->rng, at, f, (tx_rand(&s->rng) & 1u) ? aj : -aj);
        }
        return;
    }
    s->events++;
    s->v[k].kind = V_BUBBLE; s->v[k].pre = (uint8_t)pre; s->v[k].last = 0;
    tx_bubble_start(&s->v[k].u.b, a_l2, xi_q16, env, wait);
}

/* a drop's splash (the crown and the cavity's impact, ours): a short low-Q grain whose pitch falls with the drop's
 * size (~1.2 kHz at 2.4 mm, ~0.5 kHz at 6 mm, +-1 octave), so a downpour's wash is many coloured splashes, not a
 * hiss (note 16: the rain-on-water recordings hold their level down to 125 Hz; ours fell 15-30 dB below 1 kHz) */
static void splash(src_t *s, unsigned off, int32_t d_q12, int32_t a)
{
    uint32_t f = (uint32_t)((1200u * 9830u) / (uint32_t)(d_q12 > 4096 ? d_q12 : 4096));
    f = (uint32_t)(((uint64_t)f * tx_exp2((int32_t)(tx_rand(&s->rng) >> 15) - 65536 + (16 << 16))) >> 16);   /* +-1 oct */
    tx_gr_add(&s->gr, &s->rng, off, f < 120u ? 120u : f > 2499u ? 2499u : f, (int32_t)(((int64_t)a * SPLASH_G) >> 8));
}

/* a drop's impact: a sharp zero-mean doublet straight into the block (broadband, as the water drops' and the
 * references' vertical lines at each drop; a low-passed thump read dull against them) */
static void rclick(src_t *s, unsigned off, unsigned n, int32_t a)
{
    a *= 256;
    s->buf[off] += a;
    if (off + 1u < n) s->buf[off + 1u] -= a - a / 4;
    if (off + 2u < n) s->buf[off + 2u] -= a / 4;
}

/* a type II bubble (as the water drops' plop): radius log-normal about RB_A (0.38 mm, ~8.6 kHz; sd 0.8 octave, 0.25 ..
 * 1.5 mm: the references' plinks 2-11 kHz), most rising (xi 0.05 .. 0.15), a third pinched off deeper (no glide) */
#define RB_A (-91750)               /* log2 0.38 mm (~8.6 kHz: the references' plinks peak 7-10 kHz) */
#define RB_SD 52429                 /* 0.8 octave */
static void rbubble(src_t *s, int32_t lv, uint32_t at, int32_t r_l2, int32_t trim, unsigned pre, uint32_t bed_q8)
{
    tx_rng_t *r = &s->rng;
    int32_t a_l2 = clampi(RB_A + (int32_t)(((int64_t)gauss(s) * RB_SD) >> 12) - knob(s, P_SIZE) * 1024, -(2 << 16), 39322);
    int32_t l = A_II_L2 + lv + a_l2 / 2 - (int32_t)(tx_rand(r) >> 16) + knob(s, P_TONE) * 1024;
    if (bed_q8) { bubble(s, a_l2, l, at, 0, r_l2, trim, pre, bed_q8); return; }   /* (the drops one stands for) */
    if ((tx_rand(r) >> 16) < 43690u) bubble(s, a_l2, l, at, 3277u + ((tx_rand(r) >> 16) * 6554u >> 16), r_l2, trim, pre, 0);
    else bubble(s, a_l2, l - 65536, at, 0, r_l2, trim, pre, 0);
}

/* one drop: diameter from the size distribution, its class's sounds */
/* one drop, or (w_q8 > 256: the rain over the explicit cap) w drops alike: the extra small ones' bubbles join the
 * bed by their energy, the extra clicks and big ones are placed at random in the block */
static void drop(src_t *s, unsigned off, unsigned n, int near, int32_t vel, unsigned pre, int32_t trim, uint32_t w_q8)
{
    tx_rng_t *r = &s->rng;
    int32_t r_l2 = near ? dist_l2(s) - 65536 : ev_dist(s, pre);
    int32_t lam_l2, d_q12, c;
    int32_t g = (int32_t)(((int64_t)tx_place_gain_q12(r_l2) * tx_place_air_q15(5000u, r_l2)) >> 15);   /* clicks */
    g = (int32_t)(((int64_t)g * trim) >> 12);
    /* the rain rate behind ACTIVITY's flux F (drops / m^2 / s): Marshall-Palmer's flux ~ 415 R^0.65 (note 02's table, ours),
     * so log2 R = 1.54 (log2 F - log2 415); Lambda = 4.1 R^-0.21 / mm; SIZE +-2 octaves of the sizes */
    {
        int32_t lf = 321577 + (((PRE[PR_RAIN].act_oct >> 5) * s->i_eff) / (127 << 8)) * 32;   /* log2 30 + ... */
        int32_t lr = clampi(((lf - 569967) * 197) >> 7, RAIN_RMIN, 472000);   /* R 3 .. 150 mm/h (the sizes) */
        int32_t sp = (40 << 8) - s->i_eff;                   /* sparse (ACTIVITY under 40): the big scattered drops a
                                                             * shower begins with (convective: R's sizes up to 60 mm/h at
                                                             * 0), not a drizzle's 15 kHz ticks */
        if (sp > 0) lr += (int32_t)(((int64_t)sp * (RAIN_RTOP - RAIN_RMIN)) / (40 << 8));
        lam_l2 = 133431 - ((lr * 215) >> 10) - knob(s, P_SIZE) * 2048 - s->life_s - (s->cur_on ? s->cur_sz : 0);
    }
    if (near) d_q12 = 10240 + (vel * 12288) / 127;           /* a press: 2.5 .. 5.5 mm, by velocity */
    else {
        int32_t e1 = (int32_t)((tx_neglog2_q12(tx_rand(r)) * 2839u) >> 12), e2 = (int32_t)((tx_neglog2_q12(tx_rand(r)) * 1902u) >> 12);
        int32_t inv = (int32_t)tx_exp2((12 << 16) - lam_l2);   /* 1 / Lambda, mm Q12 */
        if (inv > (1 << 16)) inv = 1 << 16;
        d_q12 = 2867 + (int32_t)(((int64_t)(e1 + e2) * inv) >> 12);   /* 0.7 mm + gamma(1.67) / Lambda */
        if (d_q12 > 6 << 12) d_q12 = 6 << 12;
    }
    c = (int32_t)(((int64_t)(s->level) * g) >> 12);          /* click gain, Q15 x Q12 -> Q15 */
    if (c > 65535) c = 65535;                                /* +6 dB over full scale at most (a click at 0.25 m) */
    if (d_q12 < 4915) {                                      /* small: a type I bubble, half the time */
        if (tx_rand(r) & 1u) {
            int32_t a_l2 = -170000 + (int32_t)tx_rand_below(r, 43000u);   /* 0.17 .. 0.25 mm */
            int32_t l = A_I_L2 - RI_LV + (3 * a_l2) / 2 - (int32_t)(tx_rand(r) >> 16) + knob(s, P_TONE) * 1024;
            bubble(s, a_l2, l, off, 0, r_l2, trim, pre, 0);
            if (w_q8 > 256u) bubble(s, a_l2, l, off, 0, r_l2, trim, pre, w_q8 - 256u);   /* (half of them: the */
        }                                                                            /* other half was the coin) */
        return;
    }
    if (d_q12 < 9830) {                                      /* medium: the click */
        int32_t a = (CLICK_M * c) >> 15;
        uint32_t k = w_q8 > 256u ? (w_q8 + (tx_rand(r) >> 24)) >> 8 : 1u;
        rclick(s, off, n, (tx_rand(r) & 1u) ? a : -a);
        if ((tx_rand(r) >> 16) < RM_P - (knob(s, P_HARD) > 0 ? (uint32_t)knob(s, P_HARD) * 307u : 0u))   /* now and then a
                                                             * bubble too (the references' plinks; in the wind, slanting
                                                             * drops trap fewer: 0.6 .. 0.3) */
            rbubble(s, -RM_LV, off + 220u + tx_rand_below(r, 880u), r_l2, trim, pre, 0);
        if (w_q8 > 256u)                                     /* the drops this one stands for: their bubbles as grains */
            rbubble(s, -RM_LV, off, r_l2, trim, pre, (((w_q8 - 256u) * (RM_P - (knob(s, P_HARD) > 0 ? (uint32_t)knob(s, P_HARD) * 307u : 0u))) >> 16) + 1u);
        if (k > 4u) { a = (int32_t)(((int64_t)a * (int32_t)isqrt(((k - 1u) << 16) / 3u)) >> 8); k = 4u; }   /* (3 grains */
        while (--k) {                                        /* for many, power kept; one draw per statement: C leaves the order of */
            unsigned at = tx_rand_below(r, n);               /* two draws in one expression open, and wasm and */
            splash(s, at, d_q12, (tx_rand(r) & 1u) ? a : -a);   /* native builds then differ); the drops this one */
        }                                                    /* stands for: their splashes, each its own grain */
        return;
    }
    {                                                        /* large, very large */
        int vl = d_q12 >= 16794;
        int32_t a = ((vl ? CLICK_L * 2 : CLICK_L) * c) >> 15, a_l2, l;
        rclick(s, off, n, (tx_rand(r) & 1u) ? a : -a);
        splash(s, off, d_q12, (tx_rand(r) & 1u) ? a : -a);  /* the crown's splash: a big drop's body */
        rbubble(s, 0, off + 2205u + tx_rand_below(r, 1323u), r_l2, trim, pre, 0);   /* type II, 50-80 ms on */
        if (w_q8 > 256u) rbubble(s, 0, off, r_l2, trim, pre, w_q8 - 255u);   /* (the ones it stands for: grains) */
        if (vl) {
            a_l2 = -170000 + (int32_t)tx_rand_below(r, 60000u);
            l = A_II_L2 - 65536 + a_l2 / 2 - (int32_t)(tx_rand(r) >> 16);
            uint32_t at = off + 4410u + tx_rand_below(r, 6615u);
            bubble(s, a_l2, l, at, (tx_rand(r) & 1u) ? 6554u : 0u, r_l2, trim, pre, 0);
        }
        if (w_q8 > 256u) {                                   /* the big drops this one stands for: w - 1 of them (rounded at
                                                             * random), at most XBIG placed in the block, the rest's power in
                                                             * theirs (a chain of one more each, as before, ran up to w - 1
                                                             * full drops in one block: the CPU's worst) */
            uint32_t k = (w_q8 - 256u + (tx_rand(r) >> 24)) >> 8, m = k > XBIG ? XBIG : k;
            int32_t tr = trim;
            if (k > XBIG) tr = (trim * (int32_t)isqrt((k << 16) / XBIG)) >> 8;   /* (power kept; trim < 2^20, the root < 2^12) */
            while (m--) {                                    /* (one draw per statement: see the clicks above) */
                unsigned at = tx_rand_below(r, n);
                drop(s, at, n, 0, vel, pre, tr, 256u);
            }
        }
    }
}

/* ---- water drops (as the drip engine's physics, note 02): an impact click, then the bubble the drop traps */
#define WD_AMP_L2 (28 << 16)        /* a bubble of the reference radius at U_REF: 2^28 Q30 */
#define WD_A_L2 18000              /* the reference radius: ~1.2 mm (Minnaert ~2.7 kHz: between the references' medians) */
#define WD_CLICK 4000               /* the impact at U_REF, Q15 (the references: the first 2 ms ~ -24 dB of a plop) */

/* one drop: its fall height (speed), size, the click now, the bubble after the crater closes */
static void wdrop(src_t *s, unsigned off, unsigned n, int near, int32_t vel, unsigned pre, int32_t trim)
{
    tx_rng_t *r = &s->rng;
    int32_t r_l2 = near ? dist_l2(s)                         /* (a press: at the place; a drip point: where it is) */
                   : s->cur_on ? s->cur_r : tx_place_draw(r, dist_l2(s), depth_l2(s));
    int32_t g = (int32_t)(((int64_t)tx_place_gain_q12(r_l2) * trim) >> 12), lu, a_l2, rel, amp, sp, c;
    uint32_t wait, u_q8, p_bub;
    int32_t hi = clampi(s->i_eff - (64 << 8), 0, 63 << 8);  /* above the middle: a trickle, then a stream */
    /* the fall: U / U_ref log-normal, sd ~0.6 octave (the references' level spread 6-11 dB), velocity for a press */
    lu = near ? ((vel - 100) * 1024) + (int32_t)(((int64_t)gauss(s) * 13107) >> 12) : (int32_t)(((int64_t)gauss(s) * 39322) >> 12);
    if (s->cur_on && !near) lu = lu / 2 + s->cur_sz / 2;   /* (a drip point: its own fall height, its drops alike) */
    lu += knob(s, P_HARD) * 1024;                            /* HEIGHT: the fall, +-1 octave of the impact speed (~ x/ 4 the
                                                             * height): the click +-12 dB, the plink +-6, quicker */
    lu = clampi(lu, -(3 << 16), 3 << 16);
    u_q8 = tx_exp2(lu + 549325);                            /* U_ref 1.29 m/s: log2(330) = 8.366 Q16 */
    /* the click: level ~ U^2, a soft impact (one-pole) */
    c = (int32_t)(((int64_t)s->level * g) >> 12);
    c = (int32_t)(((int64_t)c * (int32_t)tx_exp2(2 * lu + (15 << 16) - (2 << 16))) >> 15);   /* x (U/Uref)^2 / 4 */
    c = clampi(c, 0, 65535);
    {   /* the impact: a sharp zero-mean doublet straight into the block (broadband to 20 kHz, as the references' */
        int32_t a = ((WD_CLICK * c) >> 15) * 256;           /* vertical lines at each drop) */
        s->buf[off] += a;
        if (off + 1u < n) s->buf[off + 1u] -= a - a / 4;
        if (off + 2u < n) s->buf[off + 2u] -= a / 4;
    }
    /* the bubble: P ~0.9 above 1 m/s, falling to 0.2 at 0.3 m/s */
    p_bub = u_q8 >= 256u ? 62259u : u_q8 <= 77u ? 39321u : 39321u + (u_q8 - 77u) * 128u;   /* (drips: 0.6 .. 0.95) */
    if (knob(s, P_TONE) > 32) p_bub = p_bub * (uint32_t)(96 - knob(s, P_TONE)) >> 6;   /* POOL (TONE): the top a film of
                                                             * water: the crater has no depth to close over, a click and
                                                             * less and less often a plink (to a half) */
    if ((tx_rand(r) >> 16) >= p_bub) return;
    sp = (65536 * (int32_t)s->p[P_SPREAD]) / 64;            /* +-SD 1 octave (references 1.1 .. 9 kHz, level sd 6-11 dB) */
    a_l2 = WD_A_L2 - knob(s, P_SIZE) * 2048 - ((int32_t)s->note - 60) * 5461 + s->life_s
           + (int32_t)(((int64_t)gauss(s) * sp) >> (s->cur_on ? 13 : 12)) + hi   /* a stream: bigger bubbles too; a */
           + (s->cur_on ? s->cur_sz : 0);                   /* drip point: its own size, its drops alike */
    rel = a_l2 - WD_A_L2;
    rel = clampi(rel + rel / 2, -(3 << 16), 3 << 16);       /* amplitude ~ a^1.5 */
    amp = WD_AMP_L2 + rel + clampi(lu, -(6 << 16), 5 << 15);
    {   /* crater collapse: 25 ms x (1.4 / U)^1.75 for a deep pool (note 02), here x 0.25 (ours: drips into a shallow
         * pool or a basin, the references: click and plop together within a few ms), +-25 %, 1 .. 12 ms */
        int32_t l = 662300 - 131072 + (7 * (tx_place_log2(358u) - tx_place_log2(u_q8))) / 4;
        wait = tx_exp2(l);
        wait = wait + (uint32_t)(((int32_t)wait * (tx_rand_s15(r) / 4)) >> 15);
        wait = wait < 44u ? 44u : wait > 529u ? 529u : wait;
        wait += off;
    }
    /* most rise (xi 0.05 .. 0.15: the rising chirp), a third pinch off deeper (no glide, half as loud); POOL (TONE)
     * down: a deep pool, most pinch off deep (plops), up: a basin, nearly all rise (plinks) */
    if ((int32_t)(tx_rand(r) >> 16) < clampi(43690 + knob(s, P_TONE) * 512, 8192, 60000))
        bubble(s, a_l2, amp, wait, 3277u + ((tx_rand(r) >> 16) * 6554u >> 16), r_l2, trim, pre, 0);
    else
        bubble(s, a_l2, amp - 65536, wait, 0, r_l2, trim, pre, 0);
    /* a fast drop may pinch off a second, bigger bubble; in a stream more often */
    if (u_q8 > 512u || hi) {
        uint32_t p2 = (u_q8 > 512u ? (u_q8 - 512u) * 32u : 0u) + (uint32_t)hi * 2u;
        if ((tx_rand(r) >> 16) < (p2 > 39321u ? 39321u : p2)) {
            int32_t big = 39322 + (int32_t)((tx_rand(r) >> 16) * 65536u >> 16);
            bubble(s, a_l2 + big, amp - 65536 + big + big / 2, wait + 220u + tx_rand_below(r, 441u), 0, r_l2, trim, pre, 0);
        }
    }
}

/* ---- stones */
#define NMO 4u
#define A_HIT 300000000
#define STONE_GR 128                /* a far knock's grain re its strike, Q8 (calibrated by render) */
#define RANGE_L2 27213

static int32_t stone_energy(src_t *s)
{
    uint32_t u = 0xFFFFFFFFu - (uint32_t)(((uint64_t)tx_rand(&s->rng) * s->pm) >> 32);
    int32_t x = (int32_t)(((int64_t)tx_neglog2_q12(u) * 8192) >> 12);   /* alpha 1.5: 1 / (alpha - 1) = 2 */
    return x > RANGE_L2 ? RANGE_L2 : x;
}

/* a knock's uniqueness (tx_core_cluster.h; note 14: the references' per-event centroid spreads 0.6-1.1 octave, the
 * strongest peak 0.6-2.3 octave): a bigger piece rings lower, longer and with a softer contact; where it is struck
 * (brightness: the upper modes' share), how it lands (soil .. rock: the contact time), its cracks (Q) all differ */
static const tx_clu_spread_t STONE_VAR = { 39322, 45875, 26214, 52429, 3277, 128, 64 };
/* (sd: size 0.6 octave over the size law's own, contact 0.7, Q 0.4, brightness 0.8, modes 0.05; couplings: contact
 * +0.5, Q +0.25 octave per octave of size) */

/* a knock: speed lv (log2 m/s Q16), piece length size_l2 (log2 m Q16), at sample offset off (near: a press) */
static void knock(src_t *s, unsigned off, int32_t lv, int32_t size_l2, unsigned pre, int32_t trim, int near)
{
    tx_rng_t *r = &s->rng;
    int32_t r_l2 = near ? dist_l2(s) - 65536 : ev_dist(s, pre);
    int32_t g = (int32_t)(((int64_t)tx_place_gain_q12(r_l2) * trim) >> 12), a, f_l2;
    int32_t q;
    uint32_t tc, lvl;
    int32_t ev;
    tx_clu_var_t vr;
    int k;
    unsigned i;
    voice_t *v;
    tx_clu_vary(r, &STONE_VAR, &vr);
    size_l2 += vr.size;
    q = (int32_t)tx_exp2(tx_place_log2(2880u) + knob(s, P_TONE) * 2048 + clampi(vr.decay, -(1 << 16), 1 << 16));   /* Q x16: granite 180, x/ 4 */
    tx_contact_hit(&s->mallet, lv, &tc, &ev);
    tc = (uint32_t)(((uint64_t)tc * tx_exp2(clampi(vr.hard, -(2 << 16), 2 << 16) + (16 << 16))) >> 16);
    a = (int32_t)(((int64_t)A_HIT * (int32_t)tx_exp2(lv - 65536 + (15 << 16))) >> 15);
    a = (int32_t)(((int64_t)a * s->level) >> 15);
    a = (int32_t)(((int64_t)a * g) >> 12);
    if (a < (1 << 14)) return;
    lvl = (uint32_t)a;
    if (s->events >= EVMAX || (k = take(s, lvl)) < 0) {      /* no hero: a grain, the piece's own short ring (no */
        uint32_t f1 = tx_exp2(tx_log2_q16(4300u) - 194372 - size_l2);   /* noise bed: the user wants it clean) */
        int32_t ga = (int32_t)(((int64_t)(a >> 15) * tx_contact_weight(tc, f1 > 20000u ? 20000u : f1)) >> 15);
        ga = (int32_t)(((int64_t)ga * tx_place_air_q15(f1, r_l2)) >> 15) * STONE_GR >> 8;
        tx_gr_add(&s->gr, r, off, f1 < 300u ? 300u : f1 > 15000u ? 15000u : f1, (tx_rand(r) & 1u) ? ga : -ga);
        return;
    }
    s->events++;
    v = &s->v[k];
    v->kind = V_STONE; v->pre = (uint8_t)pre; v->last = 0;
    v->pad[1] = (uint8_t)(off < TX_BLOCK ? off : TX_BLOCK - 1u);   /* (the modes start at the knock's own sample) */
    f_l2 = tx_log2_q16(4300u) - 194372 - size_l2;            /* f1 = 0.128 c / L, granite c 4.3 km/s */
    for (i = 0; i < NMO; i++) {
        uint32_t f;
        int32_t gm;
        if (i) f_l2 += 13215 + (int32_t)tx_rand_below(r, 47480u) + tx_clu_detune(r, &vr);   /* x 1.15 .. 1.9 */
        f = tx_exp2(f_l2);
        v->u.s.m[i].amp = 0;
        if (f < 60u || f > 20000u) continue;
        v->u.s.m[i].inc = f * 97391u;
        v->u.s.m[i].m16 = tx_mode_decay(f, (uint32_t)q, 179u);
        v->u.s.m[i].ph = 0;
        gm = i ? 16384 + (int32_t)tx_rand_below(r, 16383u) : 32767;
        if (i) gm = (int32_t)(((int64_t)gm * tx_exp2(clampi((vr.bright * (int32_t)i) / 3, -(3 << 16), 1 << 16) + (15 << 16))) >> 15);
        gm = gm > 32767 ? 32767 : gm;
        gm = (int32_t)(((int64_t)gm * tx_contact_weight(tc, f)) >> 15);
        gm = (int32_t)(((int64_t)gm * tx_place_air_q15(f, r_l2)) >> 15);
        tx_mode_strike(&v->u.s.m[i], (int32_t)(((int64_t)a * gm) >> 15));
    }
    if (!s->res_in || off < s->res_at) s->res_at = (uint8_t)(off < TX_BLOCK ? off : TX_BLOCK - 1u);   /* (the block's */
    s->res_in += (int32_t)(((int64_t)(a >> 5) * tx_contact_weight(tc, 6000u)) >> 15);   /* first knock's sample) */
    if (s->res_in > (1 << 28)) s->res_in = 1 << 28;
}

/* a knock of the cloud: size around the preset's (SIZE, the key, the slide's bigger stones), power-law energy; lv_off
 * the cluster's level step (a child knock: set off by another, mostly smaller) */
static void stone(src_t *s, unsigned off, int near, int32_t vel, unsigned pre, int32_t trim, int32_t lv_off)
{
    int32_t sp = (40 * 774 * (int32_t)s->p[P_SPREAD]) / 64;   /* +-SD octaves, Q16 */
    int32_t size_l2 = -189876 + knob(s, P_SIZE) * 2048 - ((int32_t)s->note - 60) * 5461 + s->life_s
                      + (int32_t)(((int64_t)gauss(s) * sp) >> (s->cur_on ? 14 : 12))
                      + clampi(s->i_eff - (64 << 8), 0, 63 << 8) * 2
                      + (s->cur_on ? s->cur_sz : 0);  /* (a stone: its own size, a little each knock) */
    int32_t lv = 65536 + (vel - 100) * 1638 + (((stone_energy(s) - RANGE_L2) * 16) >> 1)
                 + clampi(s->i_eff - (64 << 8), 0, 63 << 8) * 2 + lv_off / 2;   /* (speed: half the level step) */
    knock(s, off, lv, size_l2, pre, trim, near);
}

/* ---- cicadas */
#define RIB_AMP 3000
#define CGR_A 2600                  /* a far caller's click grain re its gain (calibrated by render against a hero) */
#define LO_F 400u                   /* a small cicada's second band: ~0.4 x the carrier (x1000), */
#define LO_Q 80u                    /* Q 5 (x16), */
#define SMALL_HEROES 4u            /* small cicadas with a hero at most (each ~110 us on the FM-1: the rest call as grains) */
#define LO_G 40                     /* its level (Q8 of the caller's gain; by render against the close-up) */

/* the species (TYPE: 0 the preset's own; 1-42, 43-84, 85-127; note 16: the references are a close-up singing at
 * 11 kHz in pulse trains, a chorus at ~5 kHz, a low one at ~2 kHz): carrier, click rate (log2 Hz, Q16), the phrase (its
 * cycle per block, Q32 / 2^16; the pulse groups come twice per cycle), its floor (0 = full depth: a pulse train) and
 * shape (1: sharper pulses; 2: a steady tone with short dips; 3: every second pulse group weaker), the calls (song
 * and pause medians, log2 blocks Q16), the tymbal's Q (TONE units) */
typedef struct { int32_t f_l2, rate_l2; uint32_t pinc; int32_t floor; uint8_t sharp; int32_t song_l2, pause_l2; int8_t q; int16_t g; } cic_t;
static const cic_t CIC[4] = {   /* (g: its level, tenths of a dB, by render: each kind about -26 LUFS at ACTIVITY 64, the small one close by -25) */
    {788474, 449072, 2100, 18022, 0, 683500, 639000, 0, 0},       /* the preset's own: ~4.2 kHz, a pulsing buzz, songs ~8 s */
    {907100, 498000, 2475, 3277, 2, 694700, 683500, 40, 40},   /* a small cicada close by: ~13.5 kHz (and a broad ~6 kHz band), pulses ~13 /s with
                                                                   * short dips, calls ~9 s sinking ~15 dB, pauses ~8 s */
    {801700, 449072, 1030, 1500, 3, 750000, 600000, -4, 55},   /* a chorus singer: ~4.8 kHz, throbbing (~5 /s) in strong-weak pairs, long songs ~14 s */
    {718600, 400000, 400, 16000, 0, 683500, 639000, -10, 0},     /* a low singer: ~2 kHz, slow throbs (~2 /s), calls ~8 s, pauses ~5 s */
};
static unsigned cic_kind(const src_t *s) { unsigned t = s->p[P_TYPE]; return t == 0 ? 0u : t < 43 ? 1u : t < 85 ? 2u : 3u; }
#define G_CALL 5000                 /* a caller's gain at 1 m, Q12 (as the cicadas engine's G_REF) */

static void caller_tune(src_t *s, caller_t *c)
{
    int32_t a2, b0, a1e, sa1, sa2, sb0, qv = clampi(45 + knob(s, P_TONE) + s->m[c->member < NM ? c->member : 0].q_off, 0, 127);
    uint32_t f = c->f, q = tx_exp2(262144 + qv * 1504);
    tx_ireso_coefs(f * 1150u / 1000u, q, &c->a1s, &a2, &b0);
    tx_ireso_coefs(f * 1400u / 1000u, q, &c->a1o, &a2, &b0);
    tx_ireso_coefs(f * 850u / 1000u, q, &a1e, &a2, &b0);
    tx_ireso_coefs(f, q, &c->ty.a1, &c->ty.a2, &c->ty.b0);
    tx_ireso_coefs(f, 54u, &sa1, &sa2, &sb0);
    c->ty.a1_end = a1e;
    c->ty.sa1 = sa1; c->ty.sa2 = sa2; c->ty.sb0 = sb0;
    c->dur = (int32_t)(15u * TX_FS / f);
    c->ty.da1 = (int32_t)((uint32_t)(a1e - c->a1s > 0 ? a1e - c->a1s : 0) * 256u / (uint32_t)(c->dur + 1));
    c->jit = clampi(27 + knob(s, P_HARD), 0, 127) * 258;
    if (s->m[c->member < NM ? c->member : 0].pad == 1u) {   /* a small cicada: one clean ring, no glide (the close-up */
        c->a1s = c->a1o = c->ty.a1_end = c->ty.a1;          /* reference: a line at ~13.5 kHz, a broad band at 5-7 kHz, */
        c->ty.da1 = 0;                                      /* nothing under 4 kHz) */
        tx_ireso_set(&c->lo, f * LO_F / 1000u, LO_Q);
    }
}

/* a calling bout (song) or a pause, in blocks: log-normal (note 14: the references' callers enter and leave in steps of
 * 5-15 dB lasting seconds, a song may end in a long decrescendo): songs median 8 s (sd 0.8 octave), pauses 5 s (sd 1) */
static uint16_t bout_len(src_t *s, int song, unsigned sp)
{
    int32_t a = (act_idx(s) >> 8) - 64;                     /* ACTIVITY: how active a cicada is (songs x/ 2, pauses x/ 4; */
    int32_t k = a < 0 ? 3 : 2;                               /* down to rare short calls: songs / 9, ~1 s, pauses x 6.5, */
    int32_t l = (song ? CIC[sp].song_l2 + a * 512 * (a < 0 ? 5 : 2) : CIC[sp].pause_l2 - a * 1024 * k)   /* ~40 s) */
                + (int32_t)(((int64_t)gauss(s) * (song ? 52429 : 65536)) >> 12);   /* log2 blocks */
    uint32_t b = tx_exp2(clampi(l, 5 << 16, 16 << 16));
    return (uint16_t)(b > 65535u ? 65535u : b < 32u ? 32u : b);
}

/* a member drawn afresh: a new cicada (carrier CV 4 % x SPREAD, rate CV 5 %, its own distance and phrase) */
static void member_new(src_t *s, member_t *m, int near)
{
    int32_t sp = (int32_t)s->p[P_SPREAD];
    const cic_t *c = &CIC[m->pad = (uint8_t)cic_kind(s)];   /* (pad: its species) */
    m->f_l2 = c->f_l2 + knob(s, P_SIZE) * -1536 - ((int32_t)s->note - 60) * -2731 + s->life_s / 4   /* SIZE: +-1.5 oct */
              + (int32_t)((((int64_t)gauss(s) * 3718 * sp) >> 6) >> 12) + (tx_vary_tone(&s->vy) >> 3);
    m->rate_l2 = c->rate_l2 + (int32_t)(((int64_t)gauss(s) * 4648) >> 12);
    m->r_l2 = near ? dist_l2(s) - 65536 : ev_dist(s, PR_CICADAS);   /* most far, a few near (note 14) */
    m->q_off = (int8_t)clampi(c->q + ((gauss(s) * 12) >> 12), -40, 40);   /* its own tymbal (timbre): sd 0.28 octave of Q */
    m->song = 1; m->fade = 6; m->pad2 = 0;
    m->bout = bout_len(s, 1, m->pad);
    m->pph = tx_rand(&s->rng);
    m->pinc = (uint32_t)(((uint64_t)c->pinc * 65536u * (60000u + tx_rand_below(&s->rng, 11000u))) >> 16);
    m->env = 0;
    m->fresh = 1;
}

static void member_gain(src_t *s, member_t *m)
{
    uint32_t f = tx_exp2(m->f_l2);
    int32_t g = (int32_t)(((int64_t)tx_place_gain_q12(m->r_l2) * G_CALL) >> 12);
    g = (int32_t)(((int64_t)g * tx_place_air_q15(f > 14000u ? 14000u : f, m->r_l2)) >> 15);
    g = (int32_t)(((int64_t)g * s->trim_c) >> 12);
    if (CIC[m->pad & 3u].g) g = (int32_t)(((int64_t)g * (int32_t)tx_exp2(TX_DB10(CIC[m->pad & 3u].g) + (12 << 16))) >> 12);
    m->gain = g;
}

static void caller_start(src_t *s, unsigned mi, int steal)
{
    member_t *m = &s->m[mi];
    voice_t *v;
    caller_t *c;
    uint32_t lv = (uint32_t)m->gain << 14;
    int k;
    if (!steal && tx_pool_count(&s->pool) >= s->limit) return;
    if ((k = take(s, lv)) < 0) return;
    v = &s->v[k];
    v->kind = V_CALLER; v->pre = PR_CICADAS; v->last = 0;
    c = &v->u.c;
    tx_zero(c, sizeof *c);
    c->member = (uint8_t)mi;
    c->f = tx_exp2(m->f_l2);
    if (c->f > 16000u) c->f = 16000u;
    if (c->f < 800u) c->f = 800u;
    c->ty.zin = c->f > 7000u ? 2 : 1;                       /* (no low leak under the carrier; a high one twice) */
    caller_tune(s, c);
    c->per_q8 = (TX_FS << 7) / tx_exp2(m->rate_l2);
    c->next_q8 = tx_rand_below(&s->rng, 65535u) % (c->per_q8 * 2u);
    m->voice = (uint8_t)k;
}

static void push(caller_t *c, int32_t off, int32_t amp, int32_t a1)
{
    tx_tev_t *t;
    if (c->npend >= NPEND) return;
    t = &c->pend[c->npend++];
    t->off = (uint16_t)(off < 0 ? 0 : off > 65535 ? 65535 : off);
    t->amp = (int16_t)(amp > 32767 ? 32767 : amp < -32767 ? -32767 : amp);
    t->a1 = a1;
}

static void clicks(src_t *s, caller_t *c, const member_t *m, unsigned n)
{
    tx_rng_t *r = &s->rng;
    int32_t cyc = (int32_t)(TX_FS * 2u / c->f), k;
    while ((c->next_q8 >> 8) < n) {
        int32_t t0 = (int32_t)(c->next_q8 >> 8);
        int32_t a = (int32_t)(((int64_t)RIB_AMP * m->env) >> 15);
        a = (int32_t)(((int64_t)a * m->pmod) >> 15);
        if (c->side) a = (a * 29205) >> 15;
        for (k = 0; k < 4; k++) {
            int32_t dj = (int32_t)(((int64_t)tx_rand_s15(r) * c->jit) >> 15) * cyc >> 16;
            int32_t aj = a + (int32_t)(((int64_t)tx_rand_s15(r) * c->jit >> 15) * a >> 16);
            push(c, t0 + k * cyc + (k ? dj : 0), aj, k ? 0 : c->a1s);
        }
        push(c, t0 + (int32_t)(c->per_q8 >> 8), (a * 2600) >> 15, c->a1o);
        c->side ^= 1u;
        {
            int32_t pj = (int32_t)(((int64_t)tx_rand_s15(r) * c->jit) >> 15);
            c->next_q8 += (uint32_t)((int32_t)c->per_q8 + (int32_t)(((int64_t)(int32_t)(c->per_q8 >> 4) * pj * 5) >> 16));
        }
    }
    c->next_q8 -= n << 8;
}

static void caller_render(src_t *s, voice_t *v, unsigned n)
{
    caller_t *c = &v->u.c;
    member_t *m = &s->m[c->member];
    unsigned j, q, mcount;
    int32_t gc;
    if (m->env > 0) clicks(s, c, m, n);
    for (j = 1; j < c->npend; j++) {
        tx_tev_t t = c->pend[j];
        unsigned k = j;
        while (k && c->pend[k - 1].off > t.off) { c->pend[k] = c->pend[k - 1]; k--; }
        c->pend[k] = t;
    }
    for (mcount = 0; mcount < c->npend && c->pend[mcount].off < n; mcount++) { }
    {
        int32_t g = (int32_t)(((int64_t)m->gain * s->level) >> 15);
        g = (g * 100 * 100) >> 14;                            /* velocity 100: (v / 127)^2 */
        gc = g > 12000 ? 12000 : g;                           /* (a guard only: a close one stays louder than a far one) */
        c->ty.g = gc;
    }
    for (q = 0; q < mcount; q++) {
        int32_t d = ((c->pend[q].amp * gc) >> 4) >> (m->pad == 1u ? 2 : 0);   /* (a small cicada: a cleaner tone, */
        unsigned o = c->pend[q].off;                        /* its tick a second difference: nothing low) */
        s->buf[o] += d;
        if (m->pad == 1u) { if (o + 1u < n) s->buf[o + 1u] -= 2 * d; if (o + 2u < n) s->buf[o + 2u] += d; }
        else if (o + 1u < n) s->buf[o + 1u] -= d;
    }
    tx_tymbal_add(&c->ty, c->pend, mcount, s->buf, n);
    if (m->pad == 1u) {   /* the second band: each click in, and pulled back two samples later ((1 - z^-2) twice with */
        uint16_t lo_off[2 * NPEND + 2];                     /* the resonator's own: nothing low) */
        int16_t lo_imp[2 * NPEND + 2];
        unsigned a = 0, b = 0, k = 0;
        if (c->lc[0]) { lo_off[k] = 0; lo_imp[k++] = c->lc[0]; }
        if (c->lc[1]) { lo_off[k] = 1; lo_imp[k++] = c->lc[1]; }
        c->lc[0] = c->lc[1] = 0;
        while (a < mcount || b < mcount) {                  /* merge: the clicks and their pull-backs, in time order */
            unsigned oa = a < mcount ? c->pend[a].off : 0xFFFFu, ob = b < mcount ? c->pend[b].off + 2u : 0xFFFFu;
            if (oa <= ob) { lo_off[k] = (uint16_t)oa; lo_imp[k++] = (int16_t)(c->pend[a++].amp >> 1); }
            else if (ob < n) { lo_off[k] = (uint16_t)ob; lo_imp[k++] = (int16_t)(-(c->pend[b++].amp >> 1)); }
            else { c->lc[ob - n] = (int16_t)(c->lc[ob - n] - (c->pend[b++].amp >> 1)); }
        }
        if (k > 1) {                                        /* (the carried ones at 0, 1 may sit after a click there) */
            unsigned i2;
            for (i2 = 1; i2 < k; i2++) {
                uint16_t o = lo_off[i2]; int16_t v = lo_imp[i2]; unsigned j2 = i2;
                while (j2 && lo_off[j2 - 1] > o) { lo_off[j2] = lo_off[j2 - 1]; lo_imp[j2] = lo_imp[j2 - 1]; j2--; }
                lo_off[j2] = o; lo_imp[j2] = v;
            }
        }
        c->lo.g = (gc * LO_G) >> 8;
        tx_ireso_add(&c->lo, lo_off, lo_imp, k, s->buf, n);
    }
    for (j = mcount; j < c->npend; j++) { c->pend[j - mcount] = c->pend[j]; c->pend[j - mcount].off = (uint16_t)(c->pend[j].off - n); }
    c->npend = (uint8_t)(c->npend - mcount);
    v->last = s->buf[n - 1];
}

/* the population per block: how many call (ACTIVITY), who swells in or fades, who has a hero, the bed, the sync */
static void callers_block(src_t *s, uint32_t want_q8)
{
    unsigned i, want = (want_q8 + 128u) >> 8, calling = 0, sing_l = 0, kc, nheroes = 0;
    int32_t sx = 0, sy = 0, kpull, gj;   /* (gj: the groups' length jitter, Q8) */
    for (i = 0; i < NM; i++) sing_l += s->m[i].calling && s->m[i].song;
    /* chorus waves: the others' singing shortens a pause (contagion), more as the chorus grows (0 .. 2 blocks per
     * block per singer, over the callers) */
    kc = (unsigned)(clampi(s->i_eff - (20 << 8), 0, 107 << 8) * 2 / 107) ;      /* Q8: 0 .. 512 */
    if (want) sing_l = (sing_l * 256u) / want;              /* the singing share, Q8 */
    if (want > NM) want = NM;
    /* members 0 .. want-1 call; the others fall silent (a member that stops is a new cicada when it calls again). With
     * a margin (+-0.4 caller): a count wavering at a half no longer starts and stops a new cicada every few blocks */
    for (i = 0; i < NM; i++) {
        member_t *m = &s->m[i];
        uint32_t th = i * 256u + 128u;
        int on = m->calling ? want_q8 + 96u > th : want_q8 > th + 96u;
        if (on && !m->calling) {                             /* one arriving: somewhere in its pause (the chorus */
            if (!m->fresh) {                                 /* builds as they come in, never all at once; the one */
                member_new(s, m, 0);                         /* a key brings sings now) */
                m->song = 0; m->bout = (uint16_t)(1u + tx_rand_below(&s->rng, bout_len(s, 0, m->pad)));
            }
            m->fresh = 0; m->calling = 1;
        }
        if (!on && m->calling) m->calling = 0;
        if (m->calling) {                                    /* its bouts: songs and pauses; a pause is cut short */
            unsigned dec = 1u + (m->song ? 0u : (unsigned)((sing_l * (uint32_t)kc) >> 16));   /* by the others singing */
            if (m->bout > dec) m->bout = (uint16_t)(m->bout - dec);
            else {
                m->song ^= 1u;
                s->total += m->song;                         /* (a song begins: the stats' event) */
                m->bout = bout_len(s, m->song, m->pad);
                m->pad2 = 0;
                if (!m->song) m->fade = (uint8_t)(m->pad == 1u ? 2u + tx_rand_below(&s->rng, 2u)   /* ends: 25-50 ms */
                                                              : 6u + tx_rand_below(&s->rng, 1u + (unsigned)clampi(act_idx(s) >> 12, 0, 4)));
                /* (0.4 .. 6 s; a cicada calling rarely: its call ends in ~1 s) */
            }
        }
        if (m->calling && m->song) {
            if (m->pad != 1u) m->env += (32767 - m->env) >> (m->pad == 3u ? 5 : 7);   /* the swell: ~0.75 s (a low */
                                                                                     /* singer: ~0.2 s) */
            else if (!m->pad2) { m->env += (32767 - m->env) >> 4; if (m->env > 31000) m->pad2 = 1; }   /* a small */
            else {                                           /* cicada: in at once (~0.1 s), then sinking ~0.7 dB/s as */
                m->env -= m->env >> 11;                      /* its pulses shorten (pad2: the call's age, ~6 s to the */
                if (m->pad2 < 127 && !tx_rand_below(&s->rng, 8u)) m->pad2++;   /* shortest) */
            }
        }
        else if (m->env) { m->env -= (m->env >> (m->calling ? m->fade : s->run ? 6 : 4)) + 1; if (m->env < 0) m->env = 0; }
        if (!m->calling && !m->env && m->voice == 0xFF) m->fresh = 0;
        if (m->calling || m->env) {
            member_gain(s, m);
            sx += tx_sin_q15(m->pph) >> 5; sy += tx_sin_q15(m->pph + 0x40000000u) >> 5;
            calling++;
        }
    }
    /* falling into step: the pull rises above a third of ACTIVITY (0 .. ~0.27 of the phrase rate per block) */
    kpull = clampi(s->i_eff - (42 << 8), 0, 38 << 8) / 85;   /* Q8: 0 .. 114 (held from ACTIVITY 80 up) */
    /* above ACTIVITY 80, each pulse group its own length, up to +-20 % at 127: a full chorus pulled into one clock
     * stood 10-20 dB over every chorus recording's envelope line (refs: none .. 31 dB; ours 41-51 dB). Below 80
     * (the default's whole LIFE range): as before */
    gj = clampi(s->i_eff - (80 << 8), 0, 47 << 8) * 20 / (47 * 100);   /* Q8 fraction: 0 .. 51 (0.2) */
    for (i = 0; i < NM; i++) nheroes += s->m[i].voice != 0xFF;
    for (i = 0; i < NM; i++) {
        member_t *m = &s->m[i];
        int32_t sn, cs, t;
        if (!(m->calling || m->env)) continue;
        sn = tx_sin_q15(m->pph); cs = tx_sin_q15(m->pph + 0x40000000u);
        t = calling > 1u ? (int32_t)((((int64_t)sx * cs - (int64_t)sy * sn) >> 10)) / (int32_t)calling : 0;   /* Q15 */
        t = clampi(t, -32767, 32767);
        {
            uint32_t p0 = m->pph, inc = m->gmul ? (uint32_t)(((uint64_t)m->pinc * m->gmul) >> 14) : m->pinc;
            m->pph += inc + (uint32_t)(int32_t)(((int64_t)(int32_t)(m->pinc >> 2) * t * kpull) >> 23);
            if (gj && ((m->pph ^ p0) & 0x80000000u))
                m->gmul = (uint16_t)(16384 + ((tx_rand_s15(&s->grng) * gj) >> 9));
            else if (!gj) m->gmul = 0;
        }
        sn = tx_sin_q15(m->pph);
        {   /* the phrase: pulse groups over the species' floor (0: a pulse train at full depth) */
            int32_t b2 = (int32_t)(((int64_t)sn * sn) >> 15), fl = CIC[m->pad & 3u].floor;   /* 0 .. 32767 */
            unsigned sh = CIC[m->pad & 3u].sharp;
            if (sh == 1u) b2 = (b2 * b2) >> 15;
            else if (sh == 2u) {                             /* pulses (the close-up: ~13 /s): each swells ~6 dB, then */
                int32_t u = (int32_t)((m->pph << 1) >> 17);      /* falls ~17 dB and fades; the call opens nearly steady */
                int32_t d = 26214 - (int32_t)m->pad2 * 143;      /* (on 80 %) and ends in short pulses (on 25 %) */
                if (u < d) b2 = 16422 + (u * 16345) / d;
                else {                                       /* no two alike: the fall's depth, the pulses' length */
                    int32_t w = 32767 - ((u - d) * 32767) / (32768 - d);
                    w = (w * w) >> 15;
                    b2 = (int32_t)(((3000u + tx_rand_below(&s->rng, 3500u)) * (uint32_t)w) >> 15);
                }
                fl = 0;
                m->pph += (uint32_t)(((int64_t)tx_rand_s15(&s->rng) * (int32_t)(m->pinc >> 3)) >> 15);   /* (+-12 % a block) */
            } else if (sh == 3u) {                           /* throbs falling ~20 dB within ~80 ms (the chorus) */
                b2 = (b2 * b2) >> 15;
                if (sn < 0) b2 = (b2 * 14746) >> 15;         /* the second of the pair -7 dB */
            }
            m->pmod = fl + (int32_t)(((int64_t)b2 * (32767 - fl)) >> 15);
        }
        /* a hero: the swelling member takes one (stealing the quietest), the others wait for a free one */
        if (m->voice == 0xFF && m->env > 0 && m->calling && m->song && (m->pad != 1u || nheroes < SMALL_HEROES)) {
            caller_start(s, i, m->env < 2048);
            nheroes += m->voice != 0xFF;
        }
        if (m->voice == 0xFF && m->env > 0) {               /* no hero: its clicks as grains at its carrier (never */
            uint32_t acc = m->pad3 + tx_exp2(m->rate_l2) * 380u;   /* a noise bed); clicks per block, Q16 */
            int32_t a = (int32_t)(((int64_t)m->gain * CGR_A) >> 12);
            uint32_t f = tx_exp2(m->f_l2);
            a = (int32_t)(((int64_t)a * m->env) >> 15);
            a = (int32_t)(((int64_t)a * m->pmod) >> 15);
            f = f > 16000u ? 16000u : f < 800u ? 800u : f;
            while (acc >= 65536u) { acc -= 65536u; tx_gr_add(&s->cg, &s->rng, tx_rand_below(&s->rng, TX_BLOCK), f, (tx_rand(&s->rng) & 1u) ? a : -a); }
            m->pad3 = (uint16_t)acc;
        }
    }
}

/* ---- birds: the population sings by turns, more and more often, then all at once (a dawn chorus) */
static void bird_draw(src_t *s, birdm_t *b, int near, unsigned type)
{
    int32_t sp = (9830 * (int32_t)s->p[P_SPREAD]) / 64;      /* +-0.15 octave between birds */
    b->f_off = tx_bird_size(type, knob(s, P_SIZE)) + (int32_t)(((int64_t)gauss(s) * sp) >> 12) + s->life_s / 4;
    b->r_l2 = near ? dist_l2(s) : tx_place_draw(&s->rng, dist_l2(s), depth_l2(s));
    b->seed = tx_rand(&s->rng);
    b->songs = (uint8_t)(2u + tx_rand_below(&s->rng, 3u));
    b->voice = 0xFF; b->near = (uint8_t)near;
}

static uint32_t bird_pause(src_t *s, unsigned type)           /* samples to a bird's next song: busier as activity rises */
{
    int32_t l = tx_place_log2(TX_BIRD_PAUSE[type]) - (act_idx(s) - (64 << 8)) * 8;   /* +-2 octaves */
    uint32_t mean = tx_exp2(l + 793350);                     /* 0.1 s -> samples (x 4410: log2 = 12.106) */
    return 2205u + (uint32_t)(((uint64_t)mean * ((tx_neglog2_q12(tx_rand(&s->rng)) * 2839u) >> 8)) >> 16);
}

static void bird_sing(src_t *s, unsigned bi, unsigned type, int steal, int32_t key_l2)
{
    birdm_t *b = &s->bm[bi];
    int32_t sz = tx_bird_size(type, knob(s, P_SIZE));
    tx_bird_ctl_t c;                                         /* how they sing: SPACING the tempo, EXTRA the song's length,
                                                              * TONE the variety, HARD the excitement (the cards name them) */
    uint32_t f = tx_exp2(TX_BIRD_F0[type] + b->f_off - sz), lv;   /* (the air at its kind's pitch: SIZE keeps level) */
    int32_t g = (int32_t)(((int64_t)tx_place_gain_q12(b->r_l2) * tx_place_air_q15(f, b->r_l2)) >> 15);
    int k;
    g = (int32_t)(((int64_t)g * tx_bird_size_gain(type, sz)) >> 12);
    g = (int32_t)(((int64_t)g * pre_trim(s, s->p[P_PRESET])) >> 12);
    g = (int32_t)(((int64_t)g * s->level) >> 15);
    lv = (uint32_t)(g < 1 ? 1 : g) << 14;
    if (!steal && tx_pool_count(&s->pool) >= s->limit) return;
    if ((k = take(s, lv)) < 0) return;
    s->v[k].kind = V_BIRD; s->v[k].pre = (uint8_t)s->p[P_PRESET]; s->v[k].pad[0] = (uint8_t)bi; s->v[k].last = 0;
    s->total++;                                              /* (a song: the stats' event) */
    c.tempo = (int8_t)knob(s, P_SPACING); c.length = (int8_t)knob(s, P_EXTRA);
    c.variety = (int8_t)knob(s, P_TONE); c.excite = (int8_t)knob(s, P_HARD);
    tx_bird_song(&s->v[k].u.bv, type, b->seed, tx_rand_below(&s->rng, b->songs), tx_rand(&s->rng), b->f_off + key_l2,
                 sz, &c, g);
    b->voice = (uint8_t)k;
    b->sung = 0;
    s->events++;
}

static void birds_block(src_t *s, unsigned n)
{   /* the flock (tx_core_bird): who is here, how far, who sings now; neighbours in a band take turns, others answer */
    unsigned type = (unsigned)PRE[s->p[P_PRESET]].act_lo, i, j, want, singing = 0, allowed;
    int32_t a = act_idx(s);
    want = count_n(s);                                       /* COUNT: the birds here (ACTIVITY: how often each sings) */
    if (want > NB) want = NB;
    allowed = tx_bird_overlap(type, a) * ((count_n(s) + 7u) >> 3);   /* (the flock's rule within a neighbourhood of ~8;
                                                                     * more birds: more neighbourhoods singing at once) */
    for (i = 0; i < NB; i++) if (s->bm[i].voice != 0xFF && s->bm[i].active) singing++;
    for (i = 0; i < NB; i++) {
        birdm_t *b = &s->bm[i];
        if (i < want && !b->active && s->run > 4096) {
            bird_draw(s, b, 0, type);
            b->r_l2 = tx_bird_dist(&s->rng, type, dist_l2(s), depth_l2(s), a);   /* most far, a few near */
            b->gain = tx_bird_band(type, b->seed, b->f_off);  /* (birds keep their band here) */
            b->active = 1; b->wait = tx_rand_below(&s->rng, 65535u) * 2u;
        }
        if (b->near) b->r_l2 = dist_l2(s) + tx_bird_near(type, a);   /* the key's bird: near only as the place fills */
        if (b->active && b->voice < NV && s->v[b->voice].kind == V_BIRD && (i >= want || s->run < 4096))
            tx_bird_stop(&s->v[b->voice].u.bv);              /* leaving: it ends the phrase it is singing */
        if (b->voice < NV && s->v[b->voice].kind == V_BIRD && a < (32 << 8)) {   /* singing rarely: a phrase or */
            uint32_t lim = 66150u << ((unsigned)(a >> 8) >> 3);   /* two (1.5 s at ACTIVITY 0, x 2 every 8) */
            b->sung += n;
            if (b->sung > lim) tx_bird_stop(&s->v[b->voice].u.bv);
        }
        if (!b->active || b->voice != 0xFF) continue;
        if (i >= want || s->run < 4096) { b->active = 0; continue; }   /* (a bird leaves after its song) */
        if (b->wait > n) { b->wait -= n; continue; }
        for (j = 0; j < NB; j++)                             /* a neighbour in its band sings: it waits its turn */
            if (j != i && s->bm[j].active && s->bm[j].voice != 0xFF && tx_bird_yield(type, b->gain, b->r_l2, s->bm[j].gain, s->bm[j].r_l2)) break;
        if ((singing < allowed || b->near) && j >= NB) {     /* (the bird close by is not held back by far ones) */
            bird_sing(s, i, type, 0, 0);
            if (b->voice != 0xFF) {
                s->v[b->voice].u.bv.wait = b->wait < n ? b->wait : n - 1u;   /* (its first note on its own sample) */
                singing++;
                unsigned idle = 0;
                int32_t ex = knob(s, P_HARD) ? clampi(a + knob(s, P_HARD) * 256, 0, 127 << 8) : a;   /* (excited: they
                                                    * answer as a busier flock would: the colony's call rate) */
                for (j = 0; j < NB; j++) idle += j != i && s->bm[j].active && s->bm[j].voice == 0xFF;
                for (j = 0; j < NB; j++)                     /* the others may answer */
                    if (j != i && s->bm[j].active && s->bm[j].voice == 0xFF) s->bm[j].wait = tx_bird_answer(&s->rng, type, ex, s->bm[j].wait, idle);
                continue;
            }
        }
        b->wait = 13230u + tx_rand_below(&s->rng, 39690u);   /* the other one sings: wait 0.3 .. 1.2 s (countersinging) */
    }
}

static void event(src_t *s, unsigned off, unsigned n, unsigned pre, int near, int32_t vel, uint32_t w_q8, int32_t lv_off)
{
    int32_t trim = pre_trim(s, pre);
    if (lv_off && pre != PR_STONES) trim = (int32_t)(((int64_t)trim * tx_exp2(clampi(lv_off, -(15 << 16), 2 << 16) + (16 << 16))) >> 16);
    if (pre == PR_RAIN) drop(s, off, n, near, vel, pre, trim, w_q8);
    else if (pre == PR_WATER) wdrop(s, off, n, near, vel, pre, trim);
    else if (pre == PR_STONES) stone(s, off, near, vel, pre, trim, lv_off);
}

/* ---- hosted presets */
static int32_t curve_at(const curve_t *c, int32_t i_q8)     /* the curve's value at intensity i (Q8 knob units) */
{
    int32_t a = c->v0, b = c->v64, x = i_q8;
    if (x >= (64 << 8)) { a = c->v64; b = c->v127; x -= 64 << 8; return a + ((b - a) * x + (63 << 7)) / (63 << 8); }
    return a + ((b - a) * x + (64 << 7)) / (64 << 8);
}

static const int8_t HOST_KEYS[8] = { 0, -3, 4, -7, 7, -5, 2, -9 };   /* the extra keys, semitones from the played one */
/* an event engine's rate (events/s Q8): the mean gap at the intensity now (0.1 s; log between e0, e64, e127) */
static uint32_t host_rate(src_t *s)
{
    const host_t *h = s->hd;
    int32_t a = act_idx(s), l0 = tx_place_log2(h->e0), l1 = tx_place_log2(h->e64), l2 = tx_place_log2(h->e127), l;
    l = a < (64 << 8) ? l0 + (((l1 - l0) * (a >> 6)) >> 8) : l1 + (((l2 - l1) * (((a - (64 << 8)) << 8) / (63 << 8))) >> 8);
    return (uint32_t)(2560u << 8) / tx_exp2(l + (8 << 16)) ;   /* 10 / mean, Q8 (mean in 0.1 s, Q8) */
}

#define HOST_FIRST 128u               /* an event engine's first press: at least 0.5 /s (Q8; a random gap, mean <= 2 s) */

/* heroes at most while an event engine's presses come fast (the dear top corner only: the defaults never reach it) */
static const struct { const char *eng; uint16_t r_q8; uint8_t max; } HOT[] = {
    {"ice", 512, 4},                /* (4: 3 until the device pool's half became the rule; icicles over 2 presses /s: at ACTIVITY or COUNT 127 with SPACING 0, 6 heroes
                                     * reached w8 ~900-2100 us on the FM-1 est., 3 ~660-730; the defaults press ~0.5 /s) */
};
static unsigned host_hot(const host_t *hd, uint32_t r_q8)
{
    unsigned i, k;
    for (i = 0; i < sizeof HOT / sizeof HOT[0]; i++) {
        for (k = 0; hd->eng[k] && hd->eng[k] == HOT[i].eng[k]; k++) { }   /* (no libc: the firmware) */
        if (hd->eng[k] == HOT[i].eng[k]) return r_q8 > HOT[i].r_q8 ? HOT[i].max : 0u;
    }
    return 0u;
}

/* a press on the hosted engine at sample at of the coming block (TX_PID_KEYAT: never on the block grid) */
static void host_press(src_t *s, uint8_t note, uint8_t vel, unsigned at)
{
    s->hst->param(s->host, TX_PID_KEYAT, (uint8_t)(at < TX_BLOCK ? at : TX_BLOCK - 1u));
    s->hst->key(s->host, note, vel, 1);
}
/* a press decided per block (a Bernoulli draw, a counter in blocks): anywhere in the block, as likely */
static unsigned host_at(src_t *s) { return tx_rand(&s->rng) >> 24; }

/* an event engine's presses: stage 1's clock (tx_core_cluster.h: the preset's runs and the breaks one break sets off;
 * a child's level step moves the velocity), at most one press per block; a strike's key is released after 150 ms */
static void host_clock(src_t *s)
{
    tx_clu_ev_t ce;
    int off, got = 0, at = 0;
    int32_t lv = 0;
    if (!s->hd->e64) return;
    if (s->hoff && !--s->hoff) s->hst->key(s->host, s->hpn, 0, 0);
    clu_cfg_row(s, s->hclu, s->hreg);
    if (!s->hon) s->clu.nk = 0;                              /* (cut: no queued presses) */
    {
        uint32_t rq = s->hon ? (uint32_t)(((uint64_t)host_rate(s) * count_q8(s)) >> 11) : 0u;
        unsigned cut = host_hot(s->hd, rq), base = host_heroes(s->hd, s->part_v ? s->part_v : s->hst->particles_def);
        if (cut && base > cut && s->hhot != cut) { s->hhot = (uint8_t)cut; s->hst->param(s->host, TX_PID_PARTICLES, (uint8_t)cut); }
        else if (!cut && s->hhot) { s->hhot = 0; s->hst->param(s->host, TX_PID_PARTICLES, (uint8_t)base); }
        tx_clu_block(&s->clu, &s->rng, &s->cf, s->clu.next_q8 == 0xFFFFFFFFu && rq && rq < HOST_FIRST ? HOST_FIRST : rq);
    }                                                        /* (the first press: its gap drawn at 0.5 /s at least, so the
                                                             * rarest setting is heard within seconds, then sparse) */
    while ((off = tx_clu_next(&s->clu, &s->rng, &s->cf, TX_BLOCK, &ce)) >= 0) if (!got) { got = 1; lv = ce.lv; at = off; }
    tx_clu_end(&s->clu, TX_BLOCK);
    if (got && s->hon) {                                     /* (presses: each an individual's event, COUNT / 8 x) */
        int32_t v = (int32_t)(((int64_t)s->vel * tx_exp2(clampi(lv / 2, -(3 << 16), 1 << 16) + (16 << 16))) >> 16);
        v = clampi(v, 16, 127);
        if (s->hoff) { s->hst->key(s->host, s->hpn, 0, 0); s->hoff = 0; }
        if (!(s->hd->k0 & 16u)) s->hpn = s->note;
        else {                                               /* (COUNT distinct keys: one of them pushed) */
            unsigned nk = count_n(s);
            uint8_t nt;
            if (nk > 8u) nk = 8u;
            nt = (uint8_t)(nk > 1u ? s->note + HOST_KEYS[tx_rand_below(&s->rng, nk)] : s->note);
            if (!s->hd->oneshot && nt != s->hpn) s->hst->key(s->host, s->hpn, 0, 0);   /* (the last one coasts on) */
            s->hpn = nt;
        }
        if (!s->hd->oneshot) s->hst->key(s->host, s->hpn, 0, 0);
        host_press(s, s->hpn, (uint8_t)v, (unsigned)at);    /* (at the clock's own sample, never on the grid) */
        if (s->hd->oneshot) s->hoff = 26;
    }
}

/* a character map's offset now (a centred knob x its depth) */
static int32_t kmap_off(const src_t *s, unsigned j)
{
    const kmap_t *k = &HMAP[s->hd - HOST].k[j];          /* (j < n: a map with a parameter) */
    if (k->knob == P_TYPE) return 0;
    return (knob(s, k->knob) * k->depth) / 64;
}

#define OVR(s, id) ((id) >= 0 && (id) < 32 && ((s)->ovm >> (id) & 1u))   /* (a dev-view override holds it) */
static void host_apply(src_t *s, int force)               /* the curves at the intensity now (only changed values sent) */
{
    unsigned i;
    if (s->hcid >= 0) {   /* the engine's own individuals: COUNT; a hero each up to HOST_HEROES or the governor's cut */
        unsigned n = (unsigned)clampi((int32_t)count_n(s), 1, s->hst->params[s->hcid].max), e;
        unsigned pp = s->part_v ? s->part_v : s->hst->particles_def;   /* (the lane's PARTICLES: the governor's) */
        if (pp > HOST_HEROES) pp = HOST_HEROES;
        pp = host_heroes(s->hd, pp);
        if (pp > s->hst->particles_max) pp = s->hst->particles_max;
        if ((force || n != s->hcnt) && !OVR(s, s->hcid)) { s->hcnt = (uint8_t)n; s->hst->param(s->host, (uint8_t)s->hcid, (uint8_t)n); }
        if (force || pp != s->hpart) { s->hpart = (uint8_t)pp; s->hst->param(s->host, TX_PID_PARTICLES, (uint8_t)pp); }
        e = n > pp ? n - pp : 0u;                            /* the rest: the engine's cheap far callers (FIELD), */
        if (s->hfid >= 0) {                                  /* louder as they are more (power ~ their number) */
            unsigned f = e ? (unsigned)clampi((int32_t)(24u * isqrt(e << 8)) >> 4, 0, 127) : 0u;
            if ((force || f != s->hfld) && !OVR(s, s->hfid)) { s->hfld = (uint8_t)f; s->hst->param(s->host, (uint8_t)s->hfid, (uint8_t)f); }
        }
    }
    if (s->hd->k127) {                                       /* the keys held: more singers */
        unsigned want = s->hon ? (unsigned)clampi((int32_t)count_n(s), 1, s->hd->k127 > 8 ? 8 : s->hd->k127) : 0u;   /* (COUNT: one key an individual) */
        if (s->hkeys <= 1) s->hbase = s->note;               /* (the extras keep the note they were pressed from) */
        if (s->hkeys && s->hkeys < want && !tx_rand_below(&s->rng, (s->hd->k0 & 4u) ? 344u : 32u)) {   /* one at a time (~0.2 s apart: logs */
            host_press(s, (uint8_t)(s->hbase + HOST_KEYS[s->hkeys]), s->vel, host_at(s));   /* thrown on together */
            s->hkeys++;                                      /* would crack at once) */
        }
        while (s->hkeys > want && s->hkeys > 1) { s->hkeys--; s->hst->key(s->host, (uint8_t)(s->hbase + HOST_KEYS[s->hkeys]), 0, 0); }
        if (!want) s->hkeys = 0;
        if ((s->hd->k0 & 1u) && s->hkeys && !s->hrenew--) {                      /* renew one key: a singer leaves, a new one comes */
            unsigned k = tx_rand_below(&s->rng, s->hkeys);   /* (any of them: no round) */
            uint8_t nt = (uint8_t)(k ? s->hbase + HOST_KEYS[k] : s->note);
            s->hst->key(s->host, nt, 0, 0);
            host_press(s, nt, s->vel, host_at(s));
            { uint32_t g = 516u + gate_draw(s, 1200u); s->hrenew = (uint16_t)(g > 65535u ? 65535u : g); }   /* 3 s + exponential, mean 7 s more (no clock) */
        }
    }
    for (i = 0; i < HOST_NC; i++) {
        int32_t v;
        unsigned j;
        if (s->hid[i] < 0 || !s->hd->c[i].name) continue;   /* (hid >= 0: i < nc) */
        v = curve_at(&s->hd->c[i], i ? s->i_eff : act_idx(s));
        if (!i && s->hd->k64) {                              /* COUNT past the keys: each doubling more on the first */
            int32_t kb = s->hd->k127 >= 8 ? 63 : s->hd->k127 >= 4 ? 42 : s->hd->k127 >= 2 ? 21 : 0;   /* curve (the */
            int32_t over = count_kl(s) - 1 - kb;                                         /* keys' 21 log2) */
            if (s->hd->e64 || !s->hd->k127) over -= 63;      /* (no keys: the curves are its 8: fewer, less) */
            if (s->hd->k0 & 8u) { if (over > 0) v -= over * s->hd->k64 / 21; }   /* (k0 & 8: the curve falls as they
                                                             * are more, never rises: wood's LOAD, at 127 a bed of creaks) */
            else if (over > 0 || s->hd->e64 || !s->hd->k127) v += over * s->hd->k64 / 21;
        }
        for (j = 0; j < HOST_NK; j++) if (s->kcur[j] == (int8_t)i) v += kmap_off(s, j);
        v = clampi(v, 0, 127);
        if ((force || v != s->hval[i]) && !OVR(s, s->hid[i])) { s->hval[i] = (uint8_t)v; s->hst->param(s->host, (uint8_t)s->hid[i], (uint8_t)v); }
    }
    for (i = 0; i < HOST_NK; i++) {                          /* the character maps on the engine's other parameters */
        const kmap_t *k;
        int32_t v;
        if (s->kid[i] < 0 || s->kcur[i] >= 0) continue;
        k = &HMAP[s->hd - HOST].k[i];                       /* (kid >= 0: i < n) */
        if (k->knob == P_TYPE)                               /* the kind: over TYPE's range for it (0: the own) */
            v = s->p[P_TYPE] >= s->tlo && (s->tlo || s->p[P_TYPE])
                ? clampi(((int32_t)(s->p[P_TYPE] - s->tlo) * 127) / s->tspan, 0, 127) : s->kdef[i];
        else v = clampi((int32_t)s->kdef[i] + kmap_off(s, i), 0, 127);
        if ((force || v != s->kval[i]) && !OVR(s, s->kid[i])) { s->kval[i] = (uint8_t)v; s->hst->param(s->host, (uint8_t)s->kid[i], (uint8_t)v); }
    }
}

/* what plays for the preset and TYPE now: a family member's row (fm), the preset's own hosted row, or none (0) */
static const host_t *host_for(const src_t *s, unsigned pre, const fam_t **fm, unsigned *lo, unsigned *span)
{
    unsigned i, t = s->p[P_TYPE], first = 128;
    const fam_t *f = 0;
    for (i = 0; i < NFAM; i++) if (FAM[i].pre == pre) {
        if (FAM[i].lo < first) first = FAM[i].lo;
        if (t >= FAM[i].lo && (!f || FAM[i].lo > f->lo)) f = &FAM[i];
    }
    *fm = f;
    if (f) {                                                 /* a member: TYPE over its range */
        unsigned hi = 128;
        for (i = 0; i < NFAM; i++) if (FAM[i].pre == pre && FAM[i].lo > f->lo && FAM[i].lo < hi) hi = FAM[i].lo;
        *lo = f->lo; *span = hi - 1u - f->lo;
        return &HOST[f->host];
    }
    *lo = 0; *span = first < 128 ? first - 1u : 127u;        /* the own kinds below the first member */
    return PRE[pre].kind == 2 ? &HOST[PRE[pre].act_lo] : 0;
}

/* the preset (or TYPE) changed: start its hosted engine (or bring the particle pools back); the old sound's last
 * sample decays */
/* a held engine in bouts (k0 & 2): one individual held all the time is a bed at the bottom; so below ACTIVITY 64
 * it is held in bouts (~0.7 s, an exponential), off between them for on x (1 - p) / p, p its share on: 0.03 at 0 ..
 * 1 at 64, rising as the square (a pour, a squeak, a flare, a bee passing now and then .. on all the time). A press while
 * held opens a bout. Switched on or first pressed, the gate is somewhere in its cycle (hgate 2: drawn at the next block,
 * the fader settled): in a bout with its share p, else in a pause, cut to at most 2 .. 3 s (a sound switched on answers
 * soon, never with a bout at once). */
static uint32_t gate_draw(src_t *s, uint32_t mean)
{
    uint32_t b = (uint32_t)(((uint64_t)mean * ((tx_neglog2_q12(tx_rand(&s->rng)) * 2839u) >> 12)) >> 12);
    return b < 20u ? 20u : b > 65535u ? 65535u : b;
}
static void host_gate(src_t *s)
{
    int32_t a = ((s->hd->k0 & 32u) ? s->i_glide : act_idx(s)) >> 8;   /* (k0 & 32: the fader alone) */
    uint32_t p = a >= 64 ? 4096u : 120u + (((uint32_t)(a * a) * 3976u) >> 12);   /* Q12 (square: LIFE's wander */
                                                             /* near 0 stays rare) */
    if (p >= 4096u) { s->hgate = 1; s->hgt = 0; return; }
    if (s->hgate == 2u) {                                    /* (a random point in the cycle) */
        uint32_t c = 344u + tx_rand_below(&s->rng, 173u);    /* (the first pause: 2 .. 3 s at most) */
        uint16_t on = s->hgt;                                /* (a bout's length drawn at the switch, or none) */
        s->hgate = (uint8_t)(tx_rand_below(&s->rng, 4096u) < p);
        s->hgt = (uint16_t)(s->hgate && on ? on : gate_draw(s, s->hgate ? GATE_ON : (GATE_ON * (4096u - p)) / p));
        if (!s->hgate && s->hgt > c) s->hgt = (uint16_t)c;
        return;
    }
    if (s->hgt) { s->hgt--; return; }
    s->hgate = (uint8_t)!s->hgate;
    s->hgt = (uint16_t)gate_draw(s, s->hgate ? GATE_ON : (GATE_ON * (4096u - p)) / p);
}

/* the sound's own curve: the measured sound of this preset nearest the TYPE, within the TYPE's range for the kind now
 * (a TYPE between two measured sounds morphs between them: the nearer one's curve) */
static unsigned lin_snd(unsigned pre, unsigned t, unsigned lo, unsigned span)
{
    unsigned i, best = SRC_LIN_NSND, bd = 256u;
    for (i = 0; i < SRC_LIN_NSND; i++) {
        unsigned tk = SRC_LIN_SND_KEY[i][1], dd = tk > t ? tk - t : t - tk;
        if (SRC_LIN_SND_KEY[i][0] != pre || tk < lo || tk > lo + span) continue;
        if (dd < bd) { bd = dd; best = i; }
    }
    return best;                                             /* (SRC_LIN_NSND: none measured) */
}

static void host_switch(src_t *s, unsigned pre)
{
    const fam_t *fm;
    unsigned tlo, tspan;
    const host_t *hd = host_for(s, pre, &fm, &tlo, &tspan);
    const tx_engine_t *ne = hd ? tx_engine_find(hd->eng) : 0;
    const void *want = hd ? (const void *)hd : (const void *)&PRE[pre];   /* (each particle preset its own memory: grains, callers, birds) */
    unsigned i;
    s->tlo = (uint8_t)tlo; s->tspan = (uint8_t)(tspan ? tspan : 1u);
    {
        unsigned li = lin_snd(pre, s->p[P_TYPE], tlo, tspan);   /* (each TYPE its own curve, even on one engine) */
        s->lsnd = li < SRC_LIN_NSND ? SRC_LIN_SND[li] : 0;
    }
    if (want == s->hcur) return;
    s->hcur = want;
    s->hclu = fm ? &fm->clu : &CLU[pre]; s->hreg = (int8_t)(fm ? 0 : PRE[pre].reg);
    s->wst = 0; s->wheld = 0;
    tx_declick_add(&s->dc, s->last_out);
    s->last_out = 0;
    tx_zero(s->host, sizeof s->host);
    s->hst = ne; s->hd = hd;
    s->hnear = hd ? host_near(hd->eng) : 32767;
    /* the particle presets' beds and filters start from silence (their last output went to the declick) */
    s->res = s->res_x1 = s->res_in = s->clp = 0;
    s->hp.x1 = s->hp.y1 = 0; s->vy.lp = 0; s->run = 0; s->hgain = 0;
    for (i = 0; i < TX_BLOCK; i++) s->buf[i] = s->ck[i] = 0;
    if (ne && ne->state_bytes <= HOST_BYTES) {
        ne->init(s->host, tx_rand(&s->rng));
        for (i = 0; i < HOST_NC; i++) s->hid[i] = (int8_t)(i < hd->nc && hd->c[i].name ? tx_param_find(ne, hd->c[i].name) : -1);
        for (i = 0; i < HOST_NK; i++) {                      /* the character maps: on a curve, or on their own */
            const kmap_t *k = i < HMAP[hd - HOST].n ? &HMAP[hd - HOST].k[i] : 0;
            unsigned c;
            s->kid[i] = (int8_t)(k && k->name ? tx_param_find(ne, k->name) : -1);
            s->kcur[i] = -1;
            s->kdef[i] = s->kid[i] >= 0 ? ne->params[s->kid[i]].def : 0;
            s->kval[i] = 0xFF;
            for (c = 0; c < HOST_NC && s->kid[i] >= 0; c++) if (s->hid[c] == s->kid[i]) s->kcur[i] = (int8_t)c;
        }
        if (s->lvl_v) ne->param(s->host, TX_PID_LEVEL, s->lvl_v);
        {   /* (the lane's PARTICLES, or the engine's own: at most its cap) */
            unsigned pp = host_heroes(hd, s->part_v ? s->part_v : ne->particles_def);
            if (s->part_v || pp != ne->particles_def) ne->param(s->host, TX_PID_PARTICLES, (uint8_t)pp);
        }
        s->hcid = (int8_t)tx_param_find(ne, "COUNT");
        s->hfid = (int8_t)(s->hcid >= 0 ? tx_param_find(ne, "FIELD") : -1);
        host_apply(s, 1);
        s->hkeys = 0; s->hrenew = 258; s->hoff = 0; s->hhot = 0;
        s->hgate = 2; s->hgt = (uint16_t)gate_draw(s, GATE_ON);   /* (a sound switched on: the gate's phase drawn at
                                                             * the next block; a bout's length ready for it) */
        tx_clu_init(&s->clu);                       /* (the first renewal after 1.5 s: past the fader's glide) */
        s->clu.next_q8 = 0xFFFFFFFFu;               /* (an event engine's first press after a gap of its own, as at
                                                     * a first key: never a strike the moment the sound changes) */
        s->hon = 0;
        if (s->held && s->p[P_COUNT] && !(hd->k0 & 2u)) {
            if (!hd->e64) ne->key(s->host, s->note, s->vel, 1);   /* (an event engine: its clock starts, no strike of its */
            s->hkeys = 1; s->hon = 1;                            /* own, as at a first press: never a strike at the key's */
        }                                                    /* velocity the moment the sound changes) */
                                                             /* (in bouts: host_render presses it when the gate opens) */
        s->quiet = 0;
        s->hskip = 1;                                        /* (its first render: the next block) */
    } else {                                                 /* the particle presets: fresh pools */
        s->hst = 0; s->hd = 0;
        tx_pool_init(&s->pool, NV, pool_lim(s));
        if (PRE[pre].kind == 0) grains_setup(s, pre);       /* (the members' memory: the grains) */
        else {
            for (i = 0; i < NM; i++) s->m[i].voice = 0xFF;
            if (PRE[pre].kind == 1) grains_setup(s, pre);   /* the far callers' click grains */
        }
        tx_clu_init(&s->clu);
        if (PRE[pre].kind == 4) {                            /* birds: their members fresh (over the callers' memory) */
            tx_zero(s->bm, sizeof s->bm);
            for (i = 0; i < NB; i++) s->bm[i].voice = 0xFF;
        }
        if (PRE[pre].kind == 3) {                            /* water: 8 heroes for the drops, the stream after them */
            const tx_engine_t *we = tx_engine_find(HOST[PRE[pre].act_lo].eng);
            tx_pool_init(&s->pool, 8, s->limit > 8 ? 8 : s->limit);
            if (we && we->state_bytes <= HOST_BYTES - WATER_OFF) {
                void *ws = (uint8_t *)s->host + WATER_OFF;
                s->wst = we;
                we->init(ws, tx_rand(&s->rng));
                for (i = 0; i < HOST_NC; i++) {
                    const host_t *wh = &HOST[PRE[pre].act_lo];
                    const char *nm = i < wh->nc ? wh->c[i].name : 0;
                    s->wid[i] = (int8_t)(nm ? tx_param_find(we, nm) : -1);
                    s->wval[i] = 0xFF;
                }
                if (s->lvl_v) we->param(ws, TX_PID_LEVEL, s->lvl_v);
                we->param(ws, TX_PID_PARTICLES, (uint8_t)(s->part_v && s->part_v < WATER_HEROES ? s->part_v : WATER_HEROES));
            }
        }
    }
}

/* ACTIVITY's path for the sound now (tx_src_lin.h, tools/gen_lin.py): the knob perceptually even in its activity */
static const int32_t *lin_row(const src_t *s)
{
    unsigned pre = s->p[P_PRESET];
    if (s->hd) return SRC_LIN_HOST[s->hd - HOST];
    if (PRE[pre].kind == 4) return SRC_LIN_PRE[SRC_LIN_BIRDS];
    if (PRE[pre].kind == 1) return SRC_LIN_PRE[SRC_LIN_CICADAS];
    return SRC_LIN_PRE[pre == PR_RAIN ? SRC_LIN_RAIN : pre == PR_STONES ? SRC_LIN_STONES : SRC_LIN_WATER];
}
typedef char lin_rows_ok[SRC_LIN_NHOST == sizeof HOST / sizeof HOST[0] ? 1 : -1];

/* LIFE, once per block: the intensity's glide and walk, the size and distance walks */
static void life_block(src_t *s)
{
    int32_t tau = 482152 + (int32_t)s->p[P_TIME] * 4258;       /* log2 blocks: 1 s (172) .. 300 s (51680) */
    int32_t depth = (int32_t)s->p[P_LIFE];
    int32_t a = tx_life_step(&s->la, tau), sz = tx_life_step(&s->ls, tau), dd = tx_life_step(&s->ld, tau + 65536);
    if (s->start) { s->start = 0; s->u_glide = (int32_t)s->p[P_INTENS] << 8; }   /* (starting: where the fader is) */
    s->u_glide += ((int32_t)s->p[P_INTENS] * 256 - s->u_glide) >> 6;   /* a fader move glides in (~1 s to 95 %: the mean
                                                             * rate thickens or thins, as weather does, never a step) */
    {   /* LIFE: +-40 at 127, narrowing to none at the fader's ends (ACTIVITY 0 stays the least active: the walk */
        int32_t w = ((a * depth) >> 7) * 5 >> 4, edge = s->u_glide < (127 << 8) - s->u_glide ? s->u_glide   /* no */
                                                                                          : (127 << 8) - s->u_glide;
        const int32_t *l = lin_row(s);
        if (edge < (32 << 8)) w = (int32_t)(((int64_t)w * (edge < 0 ? 0 : edge)) >> 13);   /* longer lifts it) */
        if (s->hd && (s->hd->k0 & 32u)) w /= 4;              /* (an engine with its own weather: a quarter) */
        if (s->lsnd) {                                       /* (the sound's own measured curve) */
            s->i_glide = tx_lin_tab(s->lsnd, s->u_glide);
            s->i_eff = tx_lin_tab(s->lsnd, clampi(s->u_glide + w, 0, 127 << 8));
        } else {
            s->i_glide = tx_lin_knob(l, s->u_glide);         /* (the knob even, the walk on it: the density wanders */
            s->i_eff = tx_lin_knob(l, clampi(s->u_glide + w, 0, 127 << 8));   /* around a mean on the line) */
        }
    }
    s->life_s = (sz * depth) >> 8;                           /* +-0.25 octave at LIFE 127 */
    s->life_d = (dd * depth) >> 6;                           /* +-1 octave of distance */
    s->gust = tx_life_step(&s->lw, 482152 + (2 << 16));      /* gusts: a walk of ~4 s (T, T / 4, T / 16) */
}

/* the hosted engine's block: LIFE and the curves, its render, the preset's gain, out */
/* a hosted engine's block out: its render, the gain gdb (tenths of a dB) x PROXIMITY's 1 / r re 3 m (ramped over the
 * block), the air's one-pole, the declick, into the mix */
/* tx_headroom's step on one sample (tx_core_trim.h) */
#define HEAD1(v) do {                                                                                     \
        if (v > TX_HEAD_KNEE) { v = TX_HEAD_KNEE + ((v - TX_HEAD_KNEE) >> 2); v = v > TX_HEAD_MAX ? TX_HEAD_MAX : v; } \
        else if (v < -TX_HEAD_KNEE) {                                                                     \
            v = -TX_HEAD_KNEE - ((-TX_HEAD_KNEE - v) >> 2);                                               \
            v = v < -TX_HEAD_MAX ? -TX_HEAD_MAX : v;                                                      \
        }                                                                                                 \
    } while (0)
TX_AUDIO_CODE static void hosted_out(src_t *s, const tx_engine_t *e, void *st, int32_t *mix, unsigned n, int32_t gdb)
{
    unsigned j;
    int32_t g0 = s->hgain, g1, d;
    int32_t z = 0;
    for (j = 0; j < n; j++) s->buf[j] = 0;
    e->render(st, s->buf, n);
    g1 = (int32_t)tx_exp2((12 << 16) + TX_DB10(gdb) + 103872 - clampi(dist_l2(s), TX_PLACE_R_MIN, 14 << 16));
    if (g1 > (s->hnear ? (int32_t)s->hnear : 32767)) g1 = s->hnear ? (int32_t)s->hnear : 32767;   /* (a loud sound's own
                                                             * nearest gain) */
    for (j = 0; j < n; j++) z |= s->buf[j];
    if (!z && !s->hlp && !s->dc.v) {                         /* a quiet block, the air and the declick settled: every pass
                                                              * below would give zeros (the trim, the headroom of 0 is 0),
                                                              * so only the gain's state moves on (the cost pass, A2) */
        s->hgain = g1;
        s->last_out = 0;
        return;
    }
    if (!g0) g0 = g1;
    d = (g1 - g0) / (int32_t)n;
    {   /* the air between: a one-pole whose corner falls with the distance (no change closer than ~10 m); the gain
         * ramp; the headroom (a loud sound's sharpest peaks, nearest: above 8x full scale only, so a sound at its
         * defaults is unchanged): one pass, each sample's steps in that order */
        int32_t lp = s->hlp, al = tx_place_lp_q15(dist_l2(s)), *b = s->buf, g = g0;
        if (al < 32000) {
            for (j = 0; j < n; j++, g += d) {
                int32_t dd = b[j] - lp, v;
                lp += ((dd >> 15) * al) + (((dd & 0x7FFF) * al) >> 15);
                v = tx_trim1(lp, g);
                HEAD1(v);
                b[j] = v;
            }
            s->hlp = lp;
        } else {
            s->hlp = b[n - 1];
            for (j = 0; j < n; j++, g += d) {
                int32_t v = tx_trim1(b[j], g);
                HEAD1(v);
                b[j] = v;
            }
        }
    }
    s->hgain = g1;
    tx_declick_run(&s->dc, s->buf, n);
    s->last_out = s->buf[n - 1];
    for (j = 0; j < n; j++) { mix[j] += s->buf[j]; s->buf[j] = 0; }   /* into the mix, the buffer cleared */
}

TX_AUDIO_CODE static void host_render(src_t *s, int32_t *mix, unsigned n)
{
    tx_stats_t st;
    int32_t gdb, x = s->i_eff, a = s->hd->g0, b = s->hd->g64;
    life_block(s);
    if (s->hskip) {                                          /* the block the sound was switched in: its state set up */
        unsigned j;                                          /* there (the clear, init, curves), its first render from */
        s->hskip = 0;                                        /* the next block: the two costs never in one block (5.8 ms */
        for (j = 0; j < n; j++) s->buf[j] = 0;               /* of the new sound later; the last one's declick on) */
        tx_declick_run(&s->dc, s->buf, n);
        for (j = 0; j < n; j++) { mix[j] += s->buf[j]; s->buf[j] = 0; }
        s->now += n;
        s->quiet = 0;
        return;
    }
    {   /* COUNT to 0 and back while held: the player's key up / down on the engine (tails ring) */
        int on;
        if (s->hd->k0 & 2u) host_gate(s);
        on = s->held && s->p[P_COUNT] && (!(s->hd->k0 & 2u) || s->hgate);
        if (!on && s->hon) {
            s->hst->key(s->host, s->note, 0, 0);
            if (s->hpn != s->note) s->hst->key(s->host, s->hpn, 0, 0);
            s->hon = 0;
        }
        if (on && !s->hon) {
            s->hon = 1;
            if (!s->hkeys) s->hkeys = 1;
            host_apply(s, 1);                                /* (the engine reads them at the press) */
            if (!s->hd->oneshot) host_press(s, s->note, s->vel, s->koff ? s->koff : host_at(s));   /* (a lane key's sample, else a gate's: anywhere) */
        }
    }
    host_clock(s);
    host_apply(s, 0);
    if (x >= (64 << 8)) { a = s->hd->g64; b = s->hd->g127; x -= 64 << 8; gdb = a + ((b - a) * x) / (63 << 8); }
    else gdb = a + ((b - a) * x) / (64 << 8);
    hosted_out(s, s->hst, s->host, mix, n, gdb);
    s->now += n;
    s->hst->stats(s->host, &st);
    s->quiet = (uint8_t)(st.quiet && !s->dc.v && !s->held);
}

/* water: the stream under the drops, fading in above WATER_ON with its own path (held while the water runs); returns
 * 1 while it still sounds */
TX_AUDIO_CODE static int water_stream(src_t *s, int32_t *mix, unsigned n)
{
    void *ws = (uint8_t *)s->host + WATER_OFF;
    tx_stats_t st;
    unsigned i;
    int on = s->run > 4096 && s->i_eff > WATER_ON;
    if (!s->wst) return 0;
    if (on != s->wheld) {                                    /* (s->note moves with each press: the stream's own key) */
        s->wheld = (uint8_t)on;
        if (on) s->wnote = s->note;
        s->wst->key(ws, s->wnote, on ? 100 : 0, (uint8_t)on);
    }
    for (i = 0; i < HOST_NC; i++) {
        int32_t v;
        if (s->wid[i] < 0) continue;
        v = clampi(pw4(WATER_AT, WATER_C[i], s->i_eff), 0, 127);
        if (v != s->wval[i]) { s->wval[i] = (uint8_t)v; s->wst->param(ws, (uint8_t)s->wid[i], (uint8_t)v); }
    }
    s->wst->stats(ws, &st);
    if (st.quiet && !on) return 0;
    hosted_out(s, s->wst, ws, mix, n, pw4(WATER_AT, WATER_G, s->i_eff));
    return 1;
}

/* ---- the engine */
static void src_init(void *st, uint32_t seed)
{
    src_t *s = (src_t *)st;
    uint8_t *b = (uint8_t *)st;
    unsigned i;
    tx_zero(b, sizeof *s);
    tx_rng_seed(&s->rng, seed);
    tx_rng_seed(&s->grng, seed ^ 0x6C1CADAu);
    s->limit = 6;
    tx_pool_init(&s->pool, NV, s->limit);
    for (i = 0; i < NP; i++) s->p[i] = PARAMS[i].def;
    if (PRE[s->p[P_PRESET]].kind == 0) { grains_setup(s, s->p[P_PRESET]); s->hcur = &PRE[s->p[P_PRESET]]; }   /* (rain) */
    else for (i = 0; i < NM; i++) s->m[i].voice = 0xFF;
    tx_mallet_mix(&s->mallet, 0);
    s->mallet.lm -= 217706;
    s->pm = 0xFFFFFFFFu - tx_exp2((32 << 16) - ((RANGE_L2 * 2048) >> 12) * 16);
    s->level = tx_level_q15(100);
    s->note = 60; s->vel = 100;
    s->quiet = 1;
    s->x32 = seed * 2654435761u + 77u;
    if (!s->x32) s->x32 = 1;
    s->i_eff = s->i_glide = s->u_glide = 64 << 8;
    tx_life_init(&s->la, seed * 3u + 1u);
    tx_life_init(&s->ls, seed * 5u + 2u);
    tx_life_init(&s->ld, seed * 7u + 3u);
    tx_life_init(&s->lw, seed * 11u + 5u);
    tx_vary_init(&s->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

/* one of the individuals (rain, stones, water drops) acts now: an event of its own, where it is and as loud as its own
 * events are, setting off its own (a stone's bounces); queued as stage 1's next event (src_render) */
static void indiv_act(src_t *s)
{
    tx_clu_kid_t *k;
    if (s->clu.nk >= TX_CLU_Q) return;
    k = &s->clu.k[s->clu.nk++];
    k->t = s->clu.now + s->koff; k->lv = 0; k->gen = 0;     /* (at the lane key's own sample) */
    k->tag = (uint8_t)(s->ni > 1u ? tx_rand_below(&s->rng, s->ni) : 0u);
    s->total++;
}

/* an event engine's individual acts now: its clock's next press, queued as stage 1's (host_clock: at the lane's
 * velocity, its level step none) */
static void host_act(src_t *s)
{
    tx_clu_kid_t *k;
    if (s->clu.nk >= TX_CLU_Q) return;
    k = &s->clu.k[s->clu.nk++];
    k->t = s->clu.now + s->koff; k->lv = 0; k->gen = 0; k->tag = 0;
    s->total++;
}

static void src_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    src_t *s = (src_t *)st;
    if (on && vel && !s->held) { s->start = 1; s->clu.next_q8 = 0xFFFFFFFFu; s->ni = 0; }   /* a first press: a new
                                                             * population (drawn afresh: never the last one's) starts
                                                             * at the fader's ACTIVITY, its first event after a gap of its
                                                             * own (stage 1 draws it), never at once */
    if (s->hst) {                                            /* a hosted engine: its own keys */
        if (on && vel && s->hd && (s->hd->k0 & 2u)) {        /* the first press: the gate's phase drawn; one while */
            if (!s->held) { s->hgate = 2; s->hgt = 0; }      /* held: a bout now */
            else if (!s->hgate) { s->hgate = 1; s->hgt = (uint16_t)gate_draw(s, GATE_ON); }
        }
        if (s->hd && s->hd->k127 && s->hon && s->hkeys) {    /* keys are individuals (COUNT holds them): another */
            if (on && vel) {                                 /* press is one of them acting afresh (a log thrown on, */
                unsigned k = tx_rand_below(&s->rng, s->hkeys);   /* a singer starting again), never a new key: the */
                uint8_t nt = (uint8_t)(k ? s->hbase + HOST_KEYS[k] : s->note);   /* engine's keys stay theirs */
                s->held++; s->quiet = 0;                     /* (at its own velocity: the lane's, not the note's) */
                s->hst->key(s->host, nt, 0, 0);
                host_press(s, nt, s->vel, s->koff);
                s->koff = 0;
                return;
            }
            if (s->held && --s->held) return;
            s->hon = 0;                                      /* the last key up: all of them */
            s->hst->key(s->host, s->note, 0, 0);
            while (s->hkeys > 1) { s->hkeys--; s->hst->key(s->host, (uint8_t)(s->hbase + HOST_KEYS[s->hkeys]), 0, 0); }
            s->hkeys = 0;
            return;
        }
        if (on && vel && s->held && s->hd) {                 /* a press while held: */
            s->held++;                                       /* one of them acts at its own level (an event engine: its */
            if (s->hd->e64) host_act(s);                     /* clock's next press now; an engine with its own */
            else if (s->hcid >= 0 && s->hon) host_press(s, s->note, s->vel, s->koff);   /* individuals: its key again, */
            return;                                          /* one in a pause sings now (crickets, katydids); others */
                                                             /* sing on by themselves: no new key at the note's pitch) */
        }
        if (on && vel) {
            s->held++; s->note = note; s->vel = vel; s->quiet = 0;
            if (!s->p[P_COUNT]) return;                      /* (COUNT 0: none; host_render presses it when COUNT rises) */
            if (s->hd && s->hd->e64) { if (!s->hkeys) s->hkeys = 1; s->hon = 1; return; }   /* an event engine: the
                                                             * first press starts its clock, no strike of its own */
            if (s->hd) return;                               /* a held engine: host_render presses it, its parameters
                                                             * at the fader first (never the engine's defaults) */
            if (!s->hkeys) s->hkeys = 1;
            s->hon = 1;
        } else if (s->held) s->held--;
        if (!s->held) s->hon = 0;
        if (s->hd) { if (!s->held) s->hst->key(s->host, s->note, 0, 0); }   /* (a hosted engine has the first key only) */
        else {                                               /* (a classic engine: the lane's key at its own sample) */
            if (on && vel) s->hst->param(s->host, TX_PID_KEYAT, s->koff);
            s->hst->key(s->host, note, vel, on);
        }
        if (!s->held && s->hd && !s->hd->oneshot && s->hpn != note) s->hst->key(s->host, s->hpn, 0, 0);   /* the last key up: the
                                                                     * one the clock pressed afresh is released too */
        return;
    }
    if (on && vel) {
        unsigned pre = s->p[P_PRESET];
        tx_vary_press(&s->vy);
        s->held++;
        s->note = note; s->vel = vel;
        if (!s->p[P_COUNT]) { s->quiet = 0; return; }        /* (COUNT 0: none) */
        /* the first press only starts the population (no strike); a press while held: one of them acts, where it is
         * and as loud as it is: a quiet bird sings at its next turn (its own song; none quiet: none, a song is never
         * cut), a cicada in its pause begins its song (one singing sings on), a drop or a stone falls (indiv_act) */
        if (s->held > 1 && PRE[pre].kind == 4) {
            unsigned q, c = 0, pick;
            for (q = 0; q < NB; q++) c += s->bm[q].active && s->bm[q].voice == 0xFF;
            if (c) {
                pick = tx_rand_below(&s->rng, c);
                for (q = 0; q < NB; q++) if (s->bm[q].active && s->bm[q].voice == 0xFF && !pick--) { s->bm[q].wait = s->koff; break; }   /* (at the press's sample) */
            }
        } else if (s->held > 1 && PRE[pre].kind == 1) {
            unsigned q, c = 0, pick;
            for (q = 0; q < NM; q++) c += s->m[q].calling && !s->m[q].song;
            if (c) {
                pick = tx_rand_below(&s->rng, c);
                for (q = 0; q < NM; q++) if (s->m[q].calling && !s->m[q].song && !pick--) { s->m[q].bout = 1; break; }
            }
        } else if (s->held > 1) indiv_act(s);
        s->quiet = 0;
    } else if (s->held && !--s->held && s->run < 30000 && (PRE[s->p[P_PRESET]].kind == 0 || PRE[s->p[P_PRESET]].kind == 3)
               && s->p[P_COUNT]) indiv_act(s);              /* a tap (up before the population is going): one acts */
}

/* before the hero pool shrinks to lim: the louder heroes move below lim, so the ones cut are the quietest (the
 * farthest of a population are its quietest); the members follow their voices */
static void voice_moved(src_t *s, unsigned to)
{
    voice_t *v = &s->v[to];
    if (v->kind == V_CALLER && v->u.c.member < NM) s->m[v->u.c.member].voice = (uint8_t)to;
    if (v->kind == V_BIRD && v->pad[0] < NB) s->bm[v->pad[0]].voice = (uint8_t)to;
}
static void keep_loudest(src_t *s, unsigned lim)
{
    tx_pool_t *p = &s->pool;
    unsigned hi, lo;
    for (hi = lim; hi < p->max; hi++) {
        unsigned q = p->max;
        uint32_t ql = 0xFFFFFFFFu;
        if (!(p->alive >> hi & 1u)) continue;
        for (lo = 0; lo < lim; lo++) {                        /* the quietest below the limit (a free slot first) */
            uint32_t l = (p->alive >> lo & 1u) ? p->level[lo] : 0u;
            if (l < ql) { ql = l; q = lo; }
        }
        if (q < p->max && ql < p->level[hi]) {
            voice_t t = s->v[q];
            uint32_t bit_q = p->alive >> q & 1u, yq = p->young >> q & 1u, y2q = p->young2 >> q & 1u, lq = p->level[q];
            s->v[q] = s->v[hi]; s->v[hi] = t;
            p->level[q] = p->level[hi]; p->level[hi] = lq;
            p->alive = (p->alive & ~(1u << q)) | (1u << q); p->alive = (p->alive & ~(1u << hi)) | (bit_q << hi);
            p->young = (p->young & ~((1u << q) | (1u << hi))) | ((p->young >> hi & 1u) << q) | (yq << hi);
            p->young2 = (p->young2 & ~((1u << q) | (1u << hi))) | ((p->young2 >> hi & 1u) << q) | (y2q << hi);
            voice_moved(s, q);
            if (bit_q) voice_moved(s, hi); else s->v[hi].kind = V_NONE;
        }
    }
}

static void src_param(void *st, uint8_t id, uint8_t val)
{
    src_t *s = (src_t *)st;
    if (id == TX_PID_KEYAT) { s->koff = val; return; }     /* (the next key's sample: kept to the block's end) */
    if (s->hst && id == TX_PID_LEVEL) s->hst->param(s->host, id, val);
    if (s->hst && id == TX_PID_PARTICLES) {                  /* (the governor's: at most the engine's cap) */
        s->hpart = (uint8_t)host_heroes(s->hd, val);
        s->hst->param(s->host, id, s->hhot && s->hpart > s->hhot ? s->hhot : s->hpart);   /* (a cut kept: HOT) */
    }
    if (s->wst && id == TX_PID_LEVEL) s->wst->param((uint8_t *)s->host + WATER_OFF, id, val);
    if (s->wst && id == TX_PID_PARTICLES)                    /* (the stream's bubbles: the lane's, at most WATER_HEROES) */
        s->wst->param((uint8_t *)s->host + WATER_OFF, id, (uint8_t)(val && val < WATER_HEROES ? val : WATER_HEROES));
    if (id == TX_PID_PARTICLES) s->part_v = val;
    if (id == TX_PID_LEVEL) s->lvl_v = val ? val : 1u;
    if (id == TX_PID_PARTICLES && s->hst) s->limit = (uint8_t)(val < 1u ? 1u : val > NV ? NV : val);
    else if (id == TX_PID_PARTICLES) {
        uint32_t cut, i;
        s->limit = (uint8_t)(val < 1u ? 1u : val > NV ? NV : val);
        keep_loudest(s, pool_lim(s));                        /* (a lower limit lets the quietest go: the governor) */
        cut = tx_pool_set_limit(&s->pool, pool_lim(s));
        for (i = 0; i < s->pool.max; i++) if (cut & (1u << i)) voice_cut(s, i);
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else if (id < NP) {
        s->p[id] = (uint8_t)(val > PARAMS[id].max ? PARAMS[id].max : val);
        if (id == P_PRESET || id == P_TYPE) {               /* (set up anew: the dev view's overrides go) */
            s->ovm = 0;
            host_switch(s, s->p[P_PRESET]);
        }
        if (id == P_HARD) {                                  /* stones: stone on stone .. soft ground above 64 */
            tx_mallet_mix(&s->mallet, clampi(knob(s, P_HARD), 0, 63) * 520);
            s->mallet.lm -= 217706;
        }
    }
}

static int voice_live(const voice_t *v)
{
    unsigned k;
    if (v->kind == V_BUBBLE) return v->u.b.env >= TX_BUBBLE_FLOOR;
    if (v->kind == V_STONE) { for (k = 0; k < NMO; k++) if (v->u.s.m[k].amp) return 1; return 0; }
    if (v->kind == V_CALLER || v->kind == V_BIRD) return 1;
    return 0;
}

TX_AUDIO_CODE static void src_render(void *st, int32_t *mix, unsigned n)
{
    src_t *s = (src_t *)st;
    tx_rng_t *r = &s->rng;
    unsigned i, j, pa = s->p[P_PRESET], pb = pa;
    int32_t w = morph(s), any = 0;
    if (n > TX_BLOCK) n = TX_BLOCK;
    if (s->quiet && !s->held) { s->koff = 0; return; }
    if (s->hst) { host_render(s, mix, n); s->koff = 0; return; }   /* (a lane key's sample: this block's only) */
    s->koff = 0;
    /* (buf and ck are cleared after each block, not here: a key's press may already have put its click in them) */
    s->events = 0;
    /* the source on / off: in ~0.2 s; out at once (a key up, or COUNT 0: no new event, no queued child, only the tails
     * of what already sounds) */
    if (s->held && s->p[P_COUNT]) s->run += (32767 - s->run) >> 3;
    else {
        if (s->run > 30000) s->clu.nk = 0;                  /* (a tap keeps its own bounces) */
        s->run = 0;
    }
    life_block(s);
    /* stage 1: events (rain, stones; mixed by MORPH) */
    int ea = PRE[pa].kind == 0 || PRE[pa].kind == 3, eb = PRE[pb].kind == 0 && w;
    uint32_t w_q8 = 256u;
    if (s->run) {
        uint32_t rate = 0;
        if (ea && eb) {
            int32_t la = tx_place_log2(pre_amount(s, pa)), lb = tx_place_log2(pre_amount(s, pb));
            rate = tx_exp2(la + (int32_t)(((int64_t)(lb - la) * w) >> 15));
        } else if (ea) rate = (uint32_t)(((uint64_t)pre_amount(s, pa) * (uint32_t)(32768 - w)) >> 15);
        else if (eb) rate = (uint32_t)(((uint64_t)pre_amount(s, pb) * (uint32_t)w) >> 15);
        rate = (uint32_t)(((uint64_t)rate * (uint32_t)s->run) >> 15);
        {
            uint32_t cap = pa == PR_RAIN ? RATE_CAP : RATE_CAP * 2u;   /* (rain: each explicit drop may stand for several) */
            if (rate > (cap << 8)) { w_q8 = rate / cap; rate = cap << 8; }   /* over the cap: weighted */
        }
        clu_cfg(s, pa);
        tx_clu_block(&s->clu, r, &s->cf, ea || eb ? rate : 0u);
    } else tx_clu_block(&s->clu, r, &s->cf, 0u);
    /* the individuals (COUNT): who is here; a new one drawn afresh (its place, most far, and its size); stones come and
     * go (a stone rolls a while, ~4 .. 20 s, then another starts) */
    if (ea) {
        unsigned cn = count_n(s), k;
        int32_t ssd = pa == PR_RAIN ? 9830 : pa == PR_STONES ? 39322 : 26214;   /* size sd: 0.15 / 0.6 / 0.4 octave */
        for (k = 0; k < cn; k++) {
            indiv_t *d = &s->ind[k];
            if (k >= s->ni || (pa == PR_STONES && !--d->life)) {
                s->cur_on = 0;
                d->r_l2 = ev_dist(s, pa);
                d->sz = (int16_t)clampi(((int32_t)(((int64_t)gauss(s) * ssd) >> 12)) >> 4, -32767, 32767);
                d->life = (uint16_t)(689u + tx_rand_below(r, 2756u));
            }
        }
        s->ni = (uint8_t)cn;
    }
    /* the events of this block in time order: immigrants and the children due (tx_core_cluster.h); each an individual's
     * (a child its parent's) */
    {
        unsigned guard = 0, cn = count_n(s), nev = 0, nk = 0;
        tx_clu_ev_t ce;
        int off;
        uint32_t keep = 65536u, wk = w_q8;                   /* rain thinned: kept at random (Q16), each standing for 1 / keep */
        if (pa == PR_RAIN && s->ev_est > (EV_BLK << 8)) {
            keep = (EV_BLK << 24) / s->ev_est;                /* (32-bit: ev_est <= 64 << 8, w_q8 well under 2^16) */
            wk = (w_q8 > 65535u ? 65535u : w_q8) * (s->ev_est >> 4) / (EV_BLK << 4);
        }
        s->clu.itag = (uint8_t)(cn > 1u ? tx_rand_below(r, cn) : 0u);
        while (guard++ < (pa == PR_RAIN ? 64u : 112u) && (off = tx_clu_next(&s->clu, r, &s->cf, n, &ce)) >= 0) {
            unsigned pre = pa;
            nev++;
            if (keep < 65536u && pre == PR_RAIN && (tx_rand(r) >> 16) >= keep) { if (!ce.kid) s->total++; continue; }
            if (pre == PR_RAIN && nk >= EV_HARD) { s->owe_q8 += wk; if (s->owe_q8 > (64u << 8)) s->owe_q8 = 64u << 8; if (!ce.kid) s->total++; continue; }
            if (eb && !ce.kid && (!ea || (tx_rand(r) >> 17) < (uint32_t)w)) pre = pb;
            if (pre == pa && ea && ce.tag < s->ni) {         /* its place: a stone's or a drip point's where it is; a rain */
                const indiv_t *d = &s->ind[ce.tag];          /* patch's drops over the ground around, most far (the */
                s->cur_r = pa == PR_RAIN ? ev_dist(s, pa)    /* salience law: rain falls all around), its drop sizes */
                           : d->r_l2 + (pa == PR_STONES ? (int32_t)(((int64_t)gauss(s) * 3277) >> 12) : 0);
                s->cur_sz = (int32_t)d->sz * 16;
                s->cur_on = 1;
            }
            if (pre == PR_RAIN) { nk++; event(s, (unsigned)off, n, pre, 0, 100, wk + s->owe_q8, ce.lv); s->owe_q8 = 0; }
            else event(s, (unsigned)off, n, pre, 0, 100, 256u, ce.lv);
            s->cur_on = 0;
            if (!ce.kid) s->total++;
            s->clu.itag = (uint8_t)(cn > 1u ? tx_rand_below(r, cn) : 0u);
        }
        tx_clu_end(&s->clu, n);
        s->ev_est = (uint32_t)((int32_t)s->ev_est + (((int32_t)(nev << 8) - (int32_t)s->ev_est) >> 3));   /* (~8 blocks) */
    }
    /* callers (cicadas): their number, weighted by the morph */
    {
        uint32_t want = 0;
        if (PRE[pa].kind == 1) want += (uint32_t)(((uint64_t)pre_amount(s, pa) * (uint32_t)(32768 - w)) >> 15);
        if (PRE[pb].kind == 1 && w) want += (uint32_t)(((uint64_t)pre_amount(s, pb) * (uint32_t)w) >> 15);
        want = (uint32_t)(((uint64_t)want * (uint32_t)s->run) >> 15);
        if (s->run && want < 256u && (PRE[pa].kind == 1 || (PRE[pb].kind == 1 && w))) want = 256u;   /* a press: at least one */
        if (!s->run) want = 0;
        s->trim_c = pre_trim(s, PR_CICADAS);
        if (PRE[pa].kind == 4) birds_block(s, n);            /* birds (their members share that memory) */
        else if (PRE[pa].kind == 1) callers_block(s, want);  /* (water: the members' memory holds the stream; rain and
                                                              * stones: their grains) */
    }
    /* the heroes */
    for (i = 0; i < s->pool.max; i++) {
        voice_t *v = &s->v[i];
        if (v->kind == V_NONE) continue;
        if (v->kind == V_BUBBLE) { tx_bubble_render(&v->u.b, s->buf, n); v->last = v->u.b.last; s->pool.level[i] = v->u.b.env; }
        else if (v->kind == V_STONE) {
            int32_t l = 0;
            unsigned k;
            tx_modal_run(v->u.s.m, NMO, s->buf, v->pad[1] < n ? v->pad[1] : 0u, n);   /* (struck in this block: from */
            v->pad[1] = 0;                                   /* its sample) */
            for (k = 0; k < NMO; k++) l += v->u.s.m[k].amp >> 1;
            s->pool.level[i] = (uint32_t)l;
            v->last = 0;
        } else if (v->kind == V_BIRD) {
            birdm_t *b = &s->bm[v->pad[0] < NB ? v->pad[0] : 0];
            s->pool.level[i] = (uint32_t)v->u.bv.gain << 14;
            if (!tx_bird_render(&v->u.bv, s->buf, n)) {      /* the song is over: the bird rests */
                tx_declick_add(&s->dc, v->u.bv.last);
                v->kind = V_NONE;
                if (b->voice == i) { b->voice = 0xFF; b->wait = bird_pause(s, (unsigned)PRE[s->p[P_PRESET]].act_lo); }
            }
            v->last = v->u.bv.last;
        } else if (v->kind == V_CALLER) {
            member_t *m = &s->m[v->u.c.member];
            caller_render(s, v, n);
            s->pool.level[i] = (uint32_t)(((uint64_t)(uint32_t)m->gain * (uint32_t)m->env) >> 1);
            if (!m->env && (!m->calling || !m->song) && tx_tymbal_quiet(&v->u.c.ty) && tx_ireso_quiet(&v->u.c.lo)
                && !v->u.c.lc[0] && !v->u.c.lc[1] && !v->u.c.npend) { m->voice = 0xFF; v->kind = V_NONE; }
            else if (!m->env && (!m->calling || !m->song)) v->u.c.npend = 0;
        }
        if (v->kind != V_NONE && !voice_live(v)) {
            if (v->kind == V_BUBBLE) tx_declick_add(&s->dc, v->last);
            v->kind = V_NONE;
        }
        if (v->kind == V_NONE) tx_pool_free(&s->pool, i); else any = 1;
    }
    tx_pool_tick(&s->pool);
    /* the clicks: a soft impact (one-pole ~4 kHz) */
    {
        int32_t lp = s->clp, in = 0;
        if (!lp) for (j = 0; j < n; j++) in |= s->ck[j];     /* (no click and none ringing: the pass adds zeros; A2) */
        if (lp || in) {
            in = 0;
            for (j = 0; j < n; j++) { in |= s->ck[j]; lp += ((s->ck[j] - lp) * 15) >> 5; s->buf[j] += lp * 256; }
            s->clp = (lp > -8 && lp < 8 && !in) ? 0 : lp;
        }
    }
    /* the far drops (rain): grains, each its own bubble or splash (tx_core_grains.h) */
    if (PRE[pa].kind == 0) tx_gr_render(&s->gr, r, s->buf, n);
    /* the stones' residue: low-passed noise, tau 2 ms */
    if (s->res || s->res_in) {                               /* (a knock's share from its own sample) */
        int32_t en = s->res, x1 = s->res_x1;
        unsigned at = s->res_in ? s->res_at : n;
        for (j = 0; j < n; j++) {
            int32_t x;
            if (j == at) { en += s->res_in; if (en > (1 << 28)) en = 1 << 28; s->res_in = 0; }
            if (en <= 0) { if (j >= at) break; j = at - 1u; continue; }   /* (silent up to the knock) */
            x = (int32_t)(tx_rand(r) >> 17) - 16384;
            x1 += (x - x1) >> 1;
            s->buf[j] += (x1 * (en >> 14)) >> 3;
            en -= (en >> 6) + 1;
            if (en <= 0) en = 0;
        }
        s->res = en; s->res_x1 = en ? x1 : 0; s->res_in = 0;
    }
    /* the far callers' clicks (grains at their carriers) */
    if (PRE[pa].kind == 1) tx_gr_render(&s->cg, r, s->buf, n);
    tx_declick_run(&s->dc, s->buf, n);
    tx_vary_block(&s->vy, s->buf, n);
    tx_trim(s->buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, s->buf, mix, n);
    s->last_out = s->hp.y1;                                  /* (the dcblock's last output: for a switch's declick) */
    for (j = 0; j < TX_BLOCK; j++) s->buf[j] = s->ck[j] = 0;
    if (s->wst) any |= water_stream(s, mix, n);              /* water: the stream under the drops */
    s->now += n;
    {
        int32_t z = any | s->run | s->clp | (PRE[pa].kind == 0 && !tx_gr_quiet(&s->gr))
                    | s->res | (PRE[pa].kind == 1 && !tx_gr_quiet(&s->cg)) | s->dc.v | s->hp.x1 | s->hp.y1 | s->clu.nk
                    | s->vy.lp | s->held;
        if (PRE[pa].kind == 1) for (i = 0; i < NM; i++) z |= s->m[i].env;
        s->quiet = (uint8_t)!z;
    }
}

static void src_stats(const void *st, tx_stats_t *o)
{
    const src_t *s = (const src_t *)st;
    if (s->hst) { s->hst->stats(s->host, o); o->quiet = s->quiet; return; }
    o->particles = (uint16_t)tx_pool_count(&s->pool);
    o->limit = s->limit;
    o->events = s->events;
    o->dropped = 0;
    o->steals = s->steals;
    o->total = s->total;
    o->quiet = s->quiet;
}

/* the worst case: each preset at full intensity near, the morph in the middle, every hero busy */
static const char SCENE_MAX[] =
    TX_TEXT("0 particles 16\n0 param ACTIVITY 127\n0 param PROXIMITY 127\n0 param PRESET 2\n0 on 60 127\n"
    "1000 param PRESET 1\n2000 param PRESET 0\n3000 param PRESET 2\n4000 param PRESET 3\n"
    "5000 param PRESET 4\n6000 param PRESET 5\n7000 param PRESET 6\n8000 param PRESET 7\n"
    "9000 param PRESET 8\n10000 param PRESET 3\n11000 param PRESET 9\n12000 param PRESET 10\n"
    "13000 param PRESET 11\n14000 param PRESET 12\n15000 param PRESET 13\n16000 param PRESET 14\n"
    "17000 param PRESET 15\n18000 param PRESET 16\n19000 param PRESET 17\n20000 param PRESET 18\n21000 param PRESET 19\n"
    "22000 off 60\n23000 end\n");

/* the hosted engines that do not fit (txtest: a host left out would play as a particle preset or fall silent, not an
 * error): every HOST row in the whole arena, the water's stream after the drops' heroes; the first one's name in *bad */
unsigned tx_source_misfits(const char **bad)
{
    unsigned i, n = 0;
    for (i = 0; i < sizeof HOST / sizeof HOST[0]; i++) {
        const tx_engine_t *e = tx_engine_find(HOST[i].eng);
        if (!e || e->state_bytes > HOST_BYTES) { if (!n) *bad = HOST[i].eng; n++; }
    }
    for (i = 0; i < NPRE; i++) if (PRE[i].kind == 3) {
        const tx_engine_t *e = tx_engine_find(HOST[PRE[i].act_lo].eng);
        if (!e || e->state_bytes > HOST_BYTES - WATER_OFF) { if (!n) *bad = HOST[PRE[i].act_lo].eng; n++; }
    }
    return n;
}

/* the dev view (the player's hosted block): the hosted engine playing now (0: none), and its own parameters read and
 * set directly. A value set here is an override: it wins over the source's curves, maps and COUNT until the sound
 * (PRESET or TYPE) changes, which clears every override. */
const tx_engine_t *tx_source_hosted(const void *st)
{
    const src_t *s = (const src_t *)st;
    return s->hst && s->hd ? s->hst : 0;
}
int tx_source_host_value(const void *st, unsigned id)    /* the value now: the override, else as last sent, else -1 */
{
    const src_t *s = (const src_t *)st;
    unsigned i;
    if (!tx_source_hosted(st) || id >= s->hst->n_params) return -1;
    if (id < 32u && (s->ovm >> id & 1u)) return s->ovv[id];
    for (i = 0; i < HOST_NC; i++) if (s->hid[i] == (int)id) return s->hval[i];
    for (i = 0; i < HOST_NK; i++) if (s->kid[i] == (int)id && s->kval[i] != 0xFF) return s->kval[i];
    if (s->hcid == (int)id) return s->hcnt;
    if (s->hfid == (int)id) return s->hfld;
    return -1;                                               /* (never sent: the engine's own default) */
}
int tx_source_host_over(const void *st, unsigned id)    /* 1: the dev view holds it */
{
    const src_t *s = (const src_t *)st;
    return tx_source_hosted(st) && id < 32u && (s->ovm >> id & 1u);
}
void tx_source_host_param(void *st, unsigned id, unsigned val)   /* an override (val > 255: released) */
{
    src_t *s = (src_t *)st;
    if (!tx_source_hosted(st) || id >= s->hst->n_params || id >= 32u) return;
    if (val > 255u) {                                        /* the source's value back (or the engine's default) */
        s->ovm &= ~(1u << id);
        host_apply(s, 1);
        if (tx_source_host_value(st, id) < 0) s->hst->param(s->host, (uint8_t)id, s->hst->params[id].def);
        return;
    }
    if (val > s->hst->params[id].max) val = s->hst->params[id].max;
    s->ovm |= 1u << id; s->ovv[id] = (uint8_t)val;
    s->hst->param(s->host, (uint8_t)id, (uint8_t)val);
}

const tx_engine_t tx_eng_source = {
    "source", TX_TEXT("Landscape"), NP, PARAMS, NV, 6, TX_STATE_SIZE(src_t), SCENE_MAX,
    src_init, src_key, src_param, src_render, src_stats,
};
