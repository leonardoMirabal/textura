/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura SOURCE: one plastic sound source in four stages (the design: what happens, what strikes, what rings, where
 * it is), the first prototype with three presets (rain on water, a stone cloud, cicadas) and a particle-level MORPH
 * between any two of them. The engines it grew from (rainwater, stonecloud, cicadas) stay as they are.
 *
 * Playing: a held key keeps the source going (it fades in over ~0.2 s; released, or at INTENSITY 0, no new event
 * starts at once: only tails ring); the key moves the size (a semitone per key from C4, as the presets' own engines
 * do) and a press adds one near event (a big drop, a thrown stone, a cicada close by). How much happens is INTENSITY: one fader, on each preset's own scale and
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
 *            the callers' pulse groups couple (Kuramoto, the pull rising with INTENSITY), so a big chorus pulses
 *            together. Callers beyond the heroes (the farthest, quietest first) join a bed: noise through a two-pole
 *            at their mean carrier, pulsed by their mean phrase, of the same power.
 * Loudness: each preset's trim puts INTENSITY 64 at -26 LUFS (a key held 10 s at 3 m); a fader law (amplitude ~
 * amount^-1/4 around the middle, events only: ours) keeps the ends playable: rain -37 .. -20, stones -21 .. -15
 * (sparse knocks are loud ones), cicadas -29 .. -22 LUFS.
 * ACTIVITY trims the rate (or the callers) alone, without the coupled physics; SPACING moves the timing between
 * clumped (bursts), the preset's own (Poisson rain) and even. LIFE and TIME: organic modulation (tx_core_life.h: OU
 * walks, 1/f, swells and lulls) of INTENSITY (full depth), SIZE and DIST (a quarter). DIST and DEPTH: where (tx_core_
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

#define OUT_TRIM 4096               /* output gain, Q12 (the presets' own levels are in PRE[].trim) */

#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 1

enum { P_PRESET, P_INTENS, P_ACT, P_SPACING, P_SIZE, P_SPREAD, P_TONE, P_HARD, P_PROX, P_DEPTH,
       P_LIFE, P_TIME, P_LOCK, P_DIV, P_GRID, P_TYPE, P_COUNT, NP };
enum { PR_RAIN, PR_STONES, PR_CICADAS, PR_WATER, PR_WIND, PR_FIRE, PR_CRICKETS, PR_SAND, PR_GRAVEL, PR_KATYDIDS, PR_BEES, PR_ICE, PR_WOOD, PR_MINERALS, PR_GLASS, PR_SONGBIRD, PR_CHIRPER, PR_CROW,
       PR_GULL, PR_BLACKBIRD, PR_THRUSH, PR_NIGHTINGALE, PR_ROBIN, PR_WREN, PR_HERMIT, PR_SKYLARK,
       PR_WOODTHRUSH, PR_WARBLER, PR_CURLEW, PR_OYSTER, PR_CUCKOO, PR_WOODPECKER, PR_PIGEON, PR_NIGHTJAR, PR_BITTERN, NPRE };
enum { V_NONE, V_BUBBLE, V_STONE, V_CALLER, V_BIRD };

static const tx_param_t PARAMS[NP] = {
    {"PRESET", NPRE - 1, 0, "the source: 0 rain on water | 1 stone cloud | 2 cicadas | 3 water | 4 wind | 5 fire | 6 crickets | 7 sand | 8 gravel | 9 katydids | 10 bees | 11 ice | 12 wood | 13 minerals | 14 glass | 15 songbird | 16 chirper | 17 crow | 18 gull | 19 blackbird | 20 song thrush | 21 nightingale | 22 robin | 23 wren | 24 hermit thrush | 25 skylark | 26 wood thrush | 27 reed warbler | 28 curlew | 29 oystercatcher | 30 cuckoo | 31 woodpecker | 32 wood pigeon | 33 nightjar | 34 bittern"},
    {"INTENSITY", 127, 64, "how active each individual is (COUNT says how many), on the preset's own scale and through its transition: a rain patch 0.2 .. 1500 drops/s (a stray drop .. a downpour on it), a stone 0.05 .. 60 knocks/s, a drip point one per 40 s .. 8 /s, a cicada rare short calls .. singing on, a bird singing now and then .. often; hosted engines: their own path (wind a calm .. 28 m/s, fire embers .. a blaze, ...)"},
    {"ACTIVITY", 127, 64, "the rate (or the callers) alone, without the coupled physics: 64 = as INTENSITY says, +-8 octaves (+-5 for callers)"},
    {"SPACING", 127, 64, "the timing: clumped bursts .. the preset's own (64; Poisson for rain) .. even"},
    {"SIZE", 127, 64, "the size: 64 = the preset's, +-2 octaves (bigger drops, stones, lower cicadas)"},
    {"SPREAD", 127, 64, "how mixed the sizes are: alike .. the preset's (64) .. twice as mixed"},
    {"TONE", 127, 64, "the body: 64 = the preset's; the stones' Q x/ 4 (mineral), the cicadas' tymbal Q; rain: the bubbles' depth"},
    {"HARD", 127, 64, "the strike: 64 = the preset's; stones: softer ground above; cicadas: buzzier above; rain: the clicks' level"},
    {"PROXIMITY", 127, 64, "far .. close: 64 = the preset's own distance (3 m; 16 steps = twice as near: +6 dB, sharper, all the highs, down to 0.5 m; far: 1 / r, the air takes the highs, up to 3 km)"},
    {"DEPTH", 127, 40, "the population's spread in distance: all at DIST .. x/ 8 (near and far at once)"},
    {"LIFE", 127, 50, "organic modulation depth (walks, swells and lulls; never a sine): still .. alive"},
    {"TIME", 127, 64, "LIFE's time scale: 1 s .. 5 min (log)"},
    {"LOCK", 1, 0, "MIDI clock lock (a hook: stored, no effect until the clock sync is built)"},
    {"DIV", 9, 4, "beat division when locked: 1/1 1/2 1/4 1/8 1/16 1/32 1/4T 1/8T 1/16T, 9 = a probability per bar (a hook)"},
    {"GRID", 127, 0, "grid pull: the probability an event lands on the clock's grid (a hook)"},
    {"TYPE", 127, 0, "the kind within the family: 0 = the preset's own; above, the hosted engine's own kinds over the range (crickets, katydids, bees: species; ice: icicles | cubes in a glass; wood: spruce .. green branch; minerals: granite .. quartz; sand: hand .. tube)"},
    {"COUNT", 127, 64, "how many individuals at once, each its own distance, size and character: 0 none (silence; tails ring), 1 + 21 steps per doubling: 1, 22, 43, 64, 85, 106, 127 = 1, 2, 4, 8, 16, 32, 64 (rain: patches of rain, stones: rolling stones or pouring points, water: dripping points, cicadas, birds; hosted engines: their keys or presses)"},
};

#define NV 16u                      /* heroes */
#define NM 32u                      /* members (callers) */
#define NPEND 40u
#define EVMAX 24u                   /* explicit heroes started per block */
#define RATE_CAP 2500u              /* explicit events per second at most: above, each stands for several (rain; its grains) */

typedef struct {
    uint8_t kind;                   /* 0 events, 1 callers, 2 an engine hosted whole (HOST[act_lo]), 4 birds (type act_lo),
                                     * 3 water: events
                                     * (drops) and an engine (HOST[act_lo]: the stream) together */
    int32_t act_lo, act_oct;        /* INTENSITY's scale: log2 of the 0 % value, its range (Q16) */
    int32_t reg;                    /* natural spacing: -64 clumped .. 0 Poisson .. 63 even */
    int32_t trim;                   /* the preset's level, Q12 */
    int32_t law;                    /* a fader law: amplitude ~ amount^(-law / 32) (0 for all: nature's law, more = louder) */
} preset_t;

static const preset_t PRE[NPRE] = {
    {0, 490986, 614782, 0, 50000, 0},   /* rain: 30 .. 20000 drops/s per m^2 (drizzle .. downpour), over ~6 m^2 heard (ours) */
    {0, -152170, 739752, 10, 39470, 0}, /* stones: 0.2 .. 500 knocks/s (+3.3 dB: most knocks now far, note 14) */
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
    {2, 10, 0, 30, 4096, 0},        /* minerals (knocked by hand: near even, note 14) */
    {2, 11, 0, 0, 4096, 0},         /* glass */
    {4, 0, 0, 0, 410, 0},          /* birds (tx_core_bird type act_lo): songbirds (a woodland mix) */
    {4, 1, 0, 0, 410, 0},          /* chirper (chaffinch, sparrow) */
    {4, 2, 0, 0, 1630, 0},          /* crow */
    {4, 4, 0, 0, 860, 0},          /* gull */
    {4, 5, 0, 0, 410, 0},          /* blackbird (levels trimmed inside the bird core) */
    {4, 6, 0, 0, 410, 0},          /* song thrush */
    {4, 7, 0, 0, 410, 0},          /* nightingale */
    {4, 8, 0, 0, 410, 0},          /* robin */
    {4, 9, 0, 0, 410, 0},          /* wren */
    {4, 10, 0, 0, 410, 0},         /* hermit thrush */
    {4, 11, 0, 0, 410, 0},         /* skylark */
    {4, 12, 0, 0, 205, 0},         /* wood thrush */
    {4, 13, 0, 0, 1300, 0},        /* reed warbler (a wetland; some birds sedge-like) */
    {4, 14, 0, 0, 1100, 0},        /* curlew (an estuary) */
    {4, 15, 0, 0, 920, 0},        /* oystercatcher (a beach) */
    {4, 16, 0, 0, 240, 0},        /* cuckoo (spring) */
    {4, 17, 0, 0, 520, 0},     /* woodpecker (drumming, a forest) */
    {4, 18, 0, 0, 205, 0},     /* wood pigeon */
    {4, 19, 0, 0, 410, 0},     /* nightjar (a heath at night) */
    {4, 20, 0, 0, 410, 0},     /* bittern (a reedbed) */
};

/* stage 1's character per preset (tx_core_cluster.h; note 14, measured against the references), three values at
 * INTENSITY 0 / 64 / 127 (linear between): sparse individuals .. bunched .. a fused cloud.
 *   cox     the rate's slow walk (gusts, runs, quiet spells): its sd, 1/100 octave; ctau its time constant, 0.1 s
 *   br      self-excitation: the branching ratio (1/100), children after an exponential delay (mean tau_ms) or an
 *           Omori one (c = tau_ms, p 1.5), at most gens generations, each a level step child (sd csd), 1/10 octave
 *   near, span, dim   salience: an event's distance over the ground (dim 2) or a volume (3) from DIST + near out to
 *           span farther (both in DEPTH's spread x 1/10: DEPTH 0 puts all at DIST), at INTENSITY 0 and 127: sparse
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
    {{0, 0, 0}, 0, {0, 0, 0}, 0, 0, 0, 0, 0, {-10, -20}, {20, 75}, 2},
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

/* HOSTED presets: an existing engine run whole inside the source (its own physics and defaults: at INTENSITY 64 it is
 * that engine at its defaults), INTENSITY moving its parameters along the preset's path through its transition: each
 * curve a parameter's value at INTENSITY 0, 64 and 127 (linear between); the first curve follows ACTIVITY too. The
 * level is set with a gain at 0 / 64 / 127 (tenths of a dB, linear between: ours, by render: every preset at INTENSITY 64
 * about -27 LUFS, a key held 20 s, the mean of 3 seeds, so lanes sit together in a landscape). Hosted
 * presets do not morph (one engine fits the arena). An engine with its own COUNT (crickets, katydids) is sent the
 * source's (at most its own maximum), its heroes the lane's PARTICLES (the governor's) up to HOST_HEROES; the
 * individuals past them are its far callers (FIELD, where it has one). Where each held key is an individual (bees: a singer;
 * fire: a log), the source holds down one key per individual COUNT asks for, at most k127 (<= 8, a few semitones
 * apart): more keys, more of them, coming one at a time; COUNT past them moves the first curve k64 knob units a
 * doubling (a fire: its logs burn livelier). k0 & 2: held in bouts below INTENSITY 64 (host_gate). With k0 & 1, a
 * held key, picked at random, is pressed afresh every
 * 6 .. 16 s: a singer leaves and a new one comes with the parameters of now (these engines read them at a press; a
 * fire's logs just burn: a press there would throw a new log on, its burst louder than the fire it lands in). */
#define HOST_NC 5u
#define HOST_NK 5u                  /* character maps per hosted engine (HMAP) */
#define GATE_ON 120u                /* a bout held, mean blocks (0.7 s) */
#define HOST_HEROES 12u             /* an engine counting its own individuals: heroes at most (CPU: half the pool) */
#define HOST_BYTES 12544u           /* the largest hosted engine's state (sand 12 464 B) */
typedef struct { const char *name; int8_t v0; uint8_t v64, v127; } curve_t;   /* (v0 < 0: off until the line crosses 0) */
typedef struct {
    const char *eng; int16_t g0, g64, g127; curve_t c[HOST_NC]; uint8_t k0, k64, k127;
    uint16_t e0, e64, e127;         /* event engines: the mean time between presses (0.1 s; log between; 0 = none) */
    uint8_t oneshot;                /* 1: each press released after 150 ms (a strike); 0: the held key pressed afresh */
} host_t;   /* (g: one calibration, flat:
                                                     * more activity is louder by the engine's own physics) */
static const host_t HOST[] = {
    /* wind: a calm (silence) .. a breath now and then .. a breeze .. a storm: gustier, more gusts passing, more
     * buffeting; an edge tone only now and then in the strong gusts */
    {"wind", 100, -73, -120, {{"WIND", 0, 64, 127}, {"GUST", 40, 70, 110}, {"WHOOSH", 30, 64, 90}, {"BUFFET", 30, 48, 80},
                    {"EDGE", 0, 12, 28}}, 1, 1, 4, 0, 0, 0, 0},
    /* stream: a trickle of plinks .. a babbling brook .. a torrent: bigger gulping bubbles, eddies, the low roar */
    {"stream", -42, -42, -42, {{"FLOW", 10, 70, 105}, {"ROAR", 0, 0, 50}, {"EDDY", 20, 30, 60}, {"NEAR", 80, 64, 50},
                            {"SPREAD", 80, 64, 45}}, 0, 0, 0, 0, 0, 0, 0},
    /* fire: embers (a rare crackle, a faint roar) .. a campfire .. a blaze (a big fire, dense crackles in bursts, roar, gusts) */
    {"fire", 110, 110, 80, {{"MOIST", 0, 25, 95}, {"SIZE", 20, 50, 100}, {"ROAR", -30, 28, 100}, {"DRAUGHT", 10, 30, 70},
                          {"BURSTY", 0, 24, 40}}, 2, 26, 8, 0, 0, 0, 0},   /* (the top: no denser than the references, and
                                                                     * under the output limiter: agent D, 2026-10-25) */
    /* crickets (COUNT: the engine's own, up to 32): each singing now and then .. all the time, a little in step,
     * warmer (a little faster) */
    {"crickets", -12, -12, -12, {{"ACTIVITY", 0, 64, 127}, {"SYNC", 30, 40, 50}, {"TEMP", 70, 76, 84}, {0, 0, 0, 0},
                             {0, 0, 0, 0}}, 0, 0, 0, 0, 0, 0, 0},
    /* sand: a trickle of grains through a narrow neck .. a pour .. a heavy pour from higher, pulsing */
    {"sand", -37, -37, -37, {{"ORIFICE", 20, 50, 115}, {"HEIGHT", 30, 50, 90}, {"PULSE", 0, 0, 40}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 2, 16, 2, 0, 0, 0, 0},
    /* gravel: a few stones now and then .. footsteps on gravel .. a slide (many stones, long rattles) */
    {"gravel", 16, 16, 16, {{"AMOUNT", 20, 64, 120}, {"RATE", 20, 59, 110}, {"SHAKE", 20, 30, 70}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 2, 16, 4, 0, 0, 0, 0},
    /* katydids (COUNT: the engine's own, up to 16; they come and go on their own): a call now and then on a cold
     * night .. singing on, warm, falling into step */
    {"katydids", 0, 0, 0, {{"ACTIVITY", 0, 64, 127}, {"TEMP", 40, 76, 90}, {"SYNC", 40, 50, 64}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 0, 0, 0, 0, 0, 0, 0},
    /* bees: one bee wandering farther off, slower .. one at its ease .. a swarm of 8 close by */
    {"bees", -45, -45, -45, {{"SWARM", 0, 0, 64}, {"DIST", 90, 50, 30}, {"SPEED", 40, 64, 80}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 3, 0, 4, 0, 0, 0, 0},
    /* ice: an icicle breaking off now and then .. icicles falling every few seconds, more chunks each */
    {"ice", 175, 175, 100, {{"AMOUNT", 20, 64, 127}, {"FIZZ", 10, 20, 40}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 0, 0, 0,
     300, 70, 3, 1},                /* (the top: a pile of icicles shattering, 3-5 breaks/s as the references) */
    /* wood: a branch taken and bent now and then, creaking .. load building faster, snapping every few seconds */
    {"wood", -35, -35, -35, {{"LOAD", 110, 40, 0}, {"CREAK", 15, 25, 30}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 0, 0, 0,
     300, 80, 4, 0},                /* (the top: a snap every ~0.4 s, each bend 0.3 s: a press sooner cuts the bend) */
    /* minerals: a knock on a stone now and then .. knocking and clinking, quicker */
    {"minerals", -40, -40, -40, {{"HOLD", 50, 36, 30}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 0, 0, 0,
     100, 12, 2, 1},
    /* glass: a pane cracking now and then .. smashing every few seconds, harder */
    {"glass", 0, 0, 0, {{"ENERGY", 60, 100, 127}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 0, 0, 0,
     400, 90, 3, 1},                /* (the top: a pane collapsing, 4-5 breaks/s) */
    /* the family members TYPE reaches (FAM below; note 16): */
    /* rain on surfaces (rain): drizzle on a roof .. a downpour, bigger drops */
    {"rainsurf", -10, -10, -10, {{"RATE", -20, 60, 120}, {"DROPS", 40, 64, 110}, {"AREA", -20, 50, 50}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 0, 16, 4, 0, 0, 0, 0},
    /* lake ice (ice): a crack now and then .. a lake singing and booming */
    {"lakeice", -30, -30, -30, {{"RATE", 8, 34, 90}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 0, 16, 4, 0, 0, 0, 0},
    /* rolling balls and marbles (gravel): a ball pushed now and then .. marbles rolling about */
    {"pebble", -5, -5, -5, {{"SPEED", 40, 64, 96}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 2, 0, 0,
     60, 20, 6, 0},
    /* a flame without crackle (fire): a candle .. a gas burner roaring in a draught */
    {"candle", 30, 30, 30, {{"FLOW", 5, 15, 90}, {"DRAUGHT", 5, 20, 70}, {"SPUTTER", 30, 100, 100}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 2, 0, 8, 0, 0, 0, 0},
    /* singing dunes (sand): a slip below the threshold .. a booming avalanche */
    {"sandsong", 0, 0, 0, {{"FLOW", 60, 100, 127}, {"STEADY", 50, 100, 127}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 2, 8, 2, 0, 0, 0, 0},
    /* stone friction (minerals): a slow rub .. scraping hard and fast */
    {"mineral", -45, -45, -45, {{"SPEED", 30, 64, 100}, {"PRESSURE", 40, 64, 100}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 2, 8, 2, 0, 0, 0, 0},
    /* rockfall (stones): a fall now and then .. falls every few seconds, bigger */
    {"rockfall", 80, 80, 80, {{"VOLUME", 0, 64, 110}, {"FRAG", 40, 64, 100}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 0, 0, 0,
     300, 100, 60, 1},
    /* ringing rocks (stones): a boulder struck now and then .. struck quickly */
    {"stonedrum", -50, -50, -50, {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 0, 0, 0,
     60, 12, 3, 1},
    /* whistling wires (wind): a breath (1.6 m/s) .. a gale (20 m/s); gusts deepen; at the top the strings lock in */
    {"aeolian", -78, -78, -78, {{"WIND", 45, 88, 127}, {"GUST", 15, 40, 90}, {"LOCK", 60, 90, 120}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 0, 0, 0, 0, 0, 0, 0},
};
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
    uint8_t kind, pre, pad[2];
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
    uint8_t wval[HOST_NC], wheld, wpad;
    const host_t *hd;
    int8_t hid[HOST_NC];            /* its curves' parameter ids */
    uint8_t hval[HOST_NC];          /* the values last sent */
    int8_t kid[HOST_NK], kcur[HOST_NK];   /* the character maps: parameter ids, the curve they offset (-1: none) */
    uint8_t kdef[HOST_NK], kval[HOST_NK]; /* the engine's own value, the value last sent */
    const clu_pre_t *hclu;          /* the hosted engine's press clock character (its preset's, or its member's) */
    int8_t hreg;                    /* and spacing */
    uint8_t tlo, tspan, tpad;       /* TYPE's range for the kind now: its value = (TYPE - tlo) x 127 / tspan */
    uint8_t lvl_v, part_v;          /* LEVEL and PARTICLES as last set (0: not set) */
    uint8_t hkeys, hbase, hpad, hon;  /* (hon: the player's key is down on the engine: held, COUNT > 0) */
    int8_t hcid, hfid;              /* the engine's own COUNT (its individuals) and FIELD (its far callers), -1: none */
    uint8_t hcnt, hpart, hfld, hgate;   /* the COUNT, PARTICLES and FIELD last sent to it; the bout gate open */
    uint16_t hgt;                   /* blocks to the gate's next turn */
    uint8_t start, hpad4;           /* start: a first press (the population starts: INTENSITY at the fader) */
    uint16_t hrenew;                /* blocks to the next renewed key */
    uint8_t hoff, hpn, hpad2[2];    /* blocks to its release, its note */         /* keys the source holds on the hosted engine (k curves; the first is the player's) */
    int32_t hgain, last_out, hlp;        /* the hosted output gain (Q12, ramped), the last sample out (for a switch) */
    int32_t buf[TX_BLOCK], ck[TX_BLOCK];
    uint8_t p[NP], limit, quiet, held, pad0;
    uint8_t note, vel, pad[2];
    int32_t level;
    int32_t run;                    /* the source on (Q15 ramp) */
    uint32_t now;                   /* sample clock */
    int32_t i_eff;                  /* INTENSITY with LIFE, Q8 knob units */
    int32_t i_glide;                /* INTENSITY gliding to the fader, Q8 */
    int32_t trim_c;                 /* the callers' level now (pre_trim), Q12 */
    tx_life_t la, ls, ld;           /* LIFE: intensity, size, distance */
    int32_t life_s, life_d;         /* their offsets now (Q16 log2) */
    /* the beds: rain's two bands, stones' residue, the callers' chorus */
    int32_t res, res_x1;            /* the stones' attack residue */
    int32_t clp;                    /* click low-pass */
    uint16_t dropped, events;
    uint32_t total, steals;
    tx_mallet_t mallet;
    uint32_t pm;                    /* the stones' power-law cut */
    tx_vary_t vy;
    uint32_t x32;                   /* bed noise */
    tx_clu_t clu;                   /* stage 1's clock: Cox walk, children (tx_core_cluster.h) */
    indiv_t ind[NI];                /* the events' individuals (rain patches, stones, drip points), ni drawn */
    uint8_t ni, cur_on, ipad[2];
    int32_t cur_r, cur_sz;          /* the individual of the event being made (cur_on) */
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

/* the effective intensity of a preset's rate (Q8 knob units, 0 .. 127 << 8), with ACTIVITY's trim */
static int32_t act_idx(const src_t *s) { return clampi(s->i_eff + knob(s, P_ACT) * 256, 0, 127 << 8); }
/* the preset's rate (events/s Q8) or callers (Q8) at the effective intensity */
/* water: the drops' rate along the fader (log2 /s at INTENSITY 0, 40, 90, 127: one per 30 s .. 2.8 /s (dripping: the
 * user's references 3 and 4) .. 60 /s (a trickle of drops) .. 40 /s as the flow takes over) */
/* the stream inside the water, along the fader: its gain (tenths of a dB; off below 36) and FLOW, ROAR, EDDY, NEAR,
 * SPREAD at INTENSITY 36, 64, 100, 127: a trickle under the drops .. a brook .. a river with its roar */
#define WATER_ON (36 << 8)
#define WATER_OFF ((8u * sizeof(voice_t) + 63u) & ~63u)   /* the stream's state, after the drops' 8 heroes */
#define WATER_HEROES 8u             /* the stream's bubble heroes at most (CPU: its 16 cost ~500 us at the top) */
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

/* COUNT: the individuals, Q8 (0: none; 1, 22, 43 .. 127 = 1, 2, 4 .. 64) and whole (at least 1 when any) */
static uint32_t count_q8(const src_t *s)
{
    unsigned v = s->p[P_COUNT];
    return v ? tx_exp2((int32_t)(((v - 1u) << 16) / 21u) + (8 << 16)) : 0u;
}
static unsigned count_n(const src_t *s)
{
    uint32_t c = (count_q8(s) + 128u) >> 8;
    return c > NI ? NI : c ? c : s->p[P_COUNT] ? 1u : 0u;
}

/* each individual's events at INTENSITY 0, 9, 64, 127 (log2 /s, Q16): a rain patch 0.2, 0.3, 500, 1500 (a stray drop ..
 * a downpour on it); a stone (rolling, or a pouring point) 0.05, 0.1, 1.25, 60 (the top a steady slide); a drip point 1 / 40 s, 1 / 15 s, 1.5,
 * 8 /s (ours: COUNT 64 = 8 of them at INTENSITY 64 as the old one-fader scale there) */
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
        return (uint32_t)(((uint64_t)tx_exp2(pw4(IND_AT, y, a) + (8 << 16)) * count_q8(s)) >> 8);
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
 * INTENSITY curve, +-depth over the knob's range) and TYPE (0: the engine's own; above, its value directly) reach each
 * engine's own parameters, so a hosted preset covers its family's sizes, species, materials and surfaces (note 16).
 * Indexed as HOST[]. */
typedef struct { const char *name; uint8_t knob; int8_t depth; } kmap_t;   /* depth 0 with P_TYPE: the value itself */
static const kmap_t HMAP[][HOST_NK] = {
    /* wind: the place (TYPE 1 .. 63: grass, pines, leafy trees, a moor, the coast, a ridge, autumn, a winter storm, a
     * desert), bigger obstacles lower, leafier, gustier, more gusts passing; COUNT: the keys held, 1 .. 4 (more gust
     * sources and more canopy around) */
    {{"SCALE", P_SIZE, -64}, {"FOLIAGE", P_TONE, 64}, {"GUST", P_HARD, 48}, {"WHOOSH", P_SPREAD, 48}, {"KIND", P_TYPE, 0}},
    {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}},                       /* (stream: inside the water) */
    /* fire: a bigger fire, drier wood, burstier crackle, a draught */
    {{"SIZE", P_SIZE, 64}, {"WOOD", P_TONE, 64}, {"BURSTY", P_HARD, 64}, {"DRAUGHT", P_SPREAD, 48}, {0, 0, 0}},
    /* crickets: species, warmer (faster), less in step */
    {{"SPECIES", P_TYPE, 0}, {"TEMP", P_TONE, 24}, {"SYNC", P_SPREAD, -64}, {0, 0, 0}, {0, 0, 0}},
    /* sand: grain, landing surface, container, fall height, pulsing */
    {{"GRAIN", P_SIZE, 64}, {"SURFACE", P_TYPE, 0}, {"SIZE", P_TONE, 64}, {"HEIGHT", P_HARD, 64}, {"PULSE", P_SPREAD, 48}},
    /* gravel: stone size, the ground (a box, a floor), crunch .. rattle, grain shapes */
    {{"SIZE", P_SIZE, 64}, {"GROUND", P_TONE, 64}, {"SHAKE", P_HARD, -64}, {"SPREAD", P_SPREAD, 64}, {0, 0, 0}},
    /* katydids: species, warmer, pegs, rasp */
    {{"SPECIES", P_TYPE, 0}, {"TEMP", P_TONE, 48}, {"PEGS", P_HARD, 64}, {"JITTER", P_SPREAD, 64}, {0, 0, 0}},
    /* bees: species, flight speed, wingbeat spread */
    {{"SPECIES", P_TYPE, 0}, {"SPEED", P_HARD, 48}, {"SPREAD", P_SPREAD, 64}, {0, 0, 0}, {0, 0, 0}},
    /* ice: icicles or cubes, size, colder (ringing) .. wet, more chunks */
    {{"ACTION", P_TYPE, 0}, {"SIZE", P_SIZE, 64}, {"TEMP", P_TONE, -64}, {"AMOUNT", P_HARD, 48}, {0, 0, 0}},
    /* wood: species, piece length, dry .. wet, creak pitch */
    {{"WOOD", P_TYPE, 0}, {"SIZE", P_SIZE, 64}, {"MOIST", P_TONE, -64}, {"PITCH", P_HARD, 64}, {0, 0, 0}},
    /* minerals: mineral, specimen size (bigger = lower), free ring .. gripped, strike .. clink .. rub */
    {{"MINERAL", P_TYPE, 0}, {"SIZE", P_SIZE, -64}, {"HOLD", P_TONE, -64}, {"ACTION", P_HARD, 64}, {0, 0, 0}},
    /* glass: shard size, ring (soft floor .. crystal), bounce, height */
    {{"SIZE", P_SIZE, 64}, {"RING", P_TONE, 64}, {"BOUNCE", P_HARD, 64}, {"HEIGHT", P_SPREAD, 48}, {0, 0, 0}},
    /* rain on surfaces: the surface (leaves .. metal roof), drop sizes, the area heard, inside */
    {{"SURFACE", P_TYPE, 0}, {"DROPS", P_SIZE, 64}, {"AREA", P_SPREAD, 64}, {"INSIDE", P_TONE, -64}, {0, 0, 0}},
    /* lake ice: thicker (lower), colder (ringing), farther (harder: nearer), fresh .. sea ice */
    {{"ICE", P_TYPE, 0}, {"THICK", P_SIZE, 64}, {"COLD", P_TONE, 64}, {"DIST", P_HARD, -64}, {0, 0, 0}},
    /* balls: the surface (wood .. metal), radius, rough, out of round, drag */
    {{"SURFACE", P_TYPE, 0}, {"SIZE", P_SIZE, 64}, {"ROUGH", P_HARD, 64}, {"ROUND", P_SPREAD, 64}, {"DRAG", P_TONE, -64}},
    /* flame: size, its roar's slope, sputter, spacing */
    {{"SIZE", P_SIZE, 64}, {"SLOPE", P_TONE, -64}, {"SPUTTER", P_HARD, 64}, {"SPACING", P_SPREAD, 64}, {0, 0, 0}},
    /* dunes: grain (bigger = lower), sorting, detune */
    {{"GRAIN", P_SIZE, 64}, {"SORTING", P_SPREAD, 64}, {"DETUNE", P_TONE, 64}, {0, 0, 0}, {0, 0, 0}},
    /* friction: the stone, its pitch (bigger = lower), rough, stick-slip */
    {{"STONE", P_TYPE, 0}, {"SIZE", P_SIZE, -64}, {"ROUGH", P_HARD, 64}, {"STICK", P_TONE, 64}, {0, 0, 0}},
    /* rockfall: volume, ground (soft .. hard), fragmentation, height */
    {{"VOLUME", P_SIZE, 64}, {"GROUND", P_HARD, 64}, {"FRAG", P_SPREAD, 64}, {"HEIGHT", P_TONE, 64}, {0, 0, 0}},
    /* ringing rocks: material, its tuning (bigger = lower), mallet, a hand on it, where struck */
    {{"MATERIAL", P_TYPE, 0}, {"TUNE", P_SIZE, -64}, {"MALLET", P_HARD, -64}, {"DAMP", P_TONE, -64}, {"POS", P_SPREAD, 64}},
    /* whistling wires: thicker wires lower, locking harder, gustier */
    {{"WIRE", P_SIZE, 64}, {"LOCK", P_TONE, 48}, {"GUST", P_HARD, 48}, {0, 0, 0}, {0, 0, 0}},
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
};
#define NFAM (sizeof FAM / sizeof FAM[0])

/* heroes at most for a hosted engine whose heroes are dear (FM-1 worst block under half the pool, ~870 us) */
static const struct { const char *eng; uint8_t max; } HCAP[] = {
    {"minerals", 3},                /* (~150 us a hero at ACTION's rub: 6 reached 1170 us at COUNT 127, 4 957 us) */
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
    f->reg = clampi(reg + knob(s, P_SPACING), -64, 63);
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
#define CLICK_M 4000
#define CLICK_L 4700
#define BED_G 160                   /* a far bubble's grain re its own amplitude, Q8 (calibrated by render) */
#define SPLASH_G 300                /* a drop's splash grain re its click, Q8 (ours) */

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
    /* the rain rate behind INTENSITY's flux F (drops / m^2 / s): Marshall-Palmer's flux ~ 415 R^0.65 (note 02's table, ours),
     * so log2 R = 1.54 (log2 F - log2 415); Lambda = 4.1 R^-0.21 / mm; SIZE +-2 octaves of the sizes */
    {
        int32_t lf = 321577 + (((PRE[PR_RAIN].act_oct >> 5) * s->i_eff) / (127 << 8)) * 32;   /* log2 30 + ... */
        int32_t lr = clampi(((lf - 569967) * 197) >> 7, -435412, 472000);   /* R 0.01 .. 150 mm/h */
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
    c = (int32_t)(((int64_t)c * (32768 + knob(s, P_HARD) * 400)) >> 15);
    if (c > 65535) c = 65535;                                /* +6 dB over full scale at most (a click at 0.25 m) */
    if (d_q12 < 4915) {                                      /* small: a type I bubble, half the time */
        if (tx_rand(r) & 1u) {
            int32_t a_l2 = -170000 + (int32_t)tx_rand_below(r, 43000u);   /* 0.17 .. 0.25 mm */
            int32_t l = A_I_L2 + (3 * a_l2) / 2 - (int32_t)(tx_rand(r) >> 16) + knob(s, P_TONE) * 1024;
            bubble(s, a_l2, l, off, 0, r_l2, trim, pre, 0);
            if (w_q8 > 256u) bubble(s, a_l2, l, off, 0, r_l2, trim, pre, w_q8 - 256u);   /* (half of them: the */
        }                                                                            /* other half was the coin) */
        return;
    }
    if (d_q12 < 9830) {                                      /* medium: the click */
        int32_t a = (CLICK_M * c) >> 15;
        uint32_t k = w_q8 > 256u ? (w_q8 + (tx_rand(r) >> 24)) >> 8 : 1u;
        s->ck[off] += (tx_rand(r) & 1u) ? a : -a;
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
        s->ck[off] += (tx_rand(r) & 1u) ? a : -a;
        splash(s, off, d_q12, (tx_rand(r) & 1u) ? a : -a);  /* the crown's splash: a big drop's body */
        a_l2 = -104858 + (int32_t)(444u * (tx_rand(r) >> 24));   /* 0.33 .. 1.1 mm */
        l = A_II_L2 + a_l2 / 2 - (int32_t)(tx_rand(r) >> 16) + knob(s, P_TONE) * 1024;
        {
            uint32_t at = off + 2205u + tx_rand_below(r, 1323u);
            bubble(s, a_l2, l, at, (tx_rand(r) & 1u) ? 6554u : 0u, r_l2, trim, pre, 0);
        }
        if (vl) {
            a_l2 = -170000 + (int32_t)tx_rand_below(r, 60000u);
            l = A_II_L2 - 65536 + a_l2 / 2 - (int32_t)(tx_rand(r) >> 16);
            uint32_t at = off + 4410u + tx_rand_below(r, 6615u);
            bubble(s, a_l2, l, at, (tx_rand(r) & 1u) ? 6554u : 0u, r_l2, trim, pre, 0);
        }
        if (w_q8 > 256u && (tx_rand(r) >> 24) < ((w_q8 - 256u) & 255u)) drop(s, tx_rand_below(r, n), n, 0, vel, pre, trim, w_q8 - 256u);
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
    lu = clampi(lu, -(3 << 16), 3 << 16);
    u_q8 = tx_exp2(lu + 549325);                            /* U_ref 1.29 m/s: log2(330) = 8.366 Q16 */
    /* the click: level ~ U^2, a soft impact (one-pole) */
    c = (int32_t)(((int64_t)s->level * g) >> 12);
    c = (int32_t)(((int64_t)c * (32768 + knob(s, P_HARD) * 400)) >> 15);
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
    if ((tx_rand(r) >> 16) >= p_bub) return;
    sp = (65536 * (int32_t)s->p[P_SPREAD]) / 64;            /* +-SD 1 octave (references 1.1 .. 9 kHz, level sd 6-11 dB) */
    a_l2 = WD_A_L2 - knob(s, P_SIZE) * 2048 - ((int32_t)s->note - 60) * 5461 + s->life_s
           + (int32_t)(((int64_t)gauss(s) * sp) >> (s->cur_on ? 13 : 12)) + hi   /* a stream: bigger bubbles too; a */
           + (s->cur_on ? s->cur_sz : 0);                   /* drip point: its own size, its drops alike */
    rel = a_l2 - WD_A_L2;
    rel = clampi(rel + rel / 2, -(3 << 16), 3 << 16);       /* amplitude ~ a^1.5 */
    amp = WD_AMP_L2 + rel + clampi(lu, -(6 << 16), 5 << 15) + knob(s, P_TONE) * 1024;
    {   /* crater collapse: 25 ms x (1.4 / U)^1.75 for a deep pool (note 02), here x 0.25 (ours: drips into a shallow
         * pool or a basin, the references: click and plop together within a few ms), +-25 %, 1 .. 12 ms */
        int32_t l = 662300 - 131072 + (7 * (tx_place_log2(358u) - tx_place_log2(u_q8))) / 4;
        wait = tx_exp2(l);
        wait = wait + (uint32_t)(((int32_t)wait * (tx_rand_s15(r) / 4)) >> 15);
        wait = wait < 44u ? 44u : wait > 529u ? 529u : wait;
        wait += off;
    }
    /* most rise (xi 0.05 .. 0.15: the rising chirp), a third pinch off deeper (no glide, half as loud) */
    if ((tx_rand(r) >> 16) < 43690u)
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
    (void)off;                                               /* (the modes start at the block's start) */
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
    s->res += (int32_t)(((int64_t)(a >> 5) * tx_contact_weight(tc, 6000u)) >> 15);
    if (s->res > (1 << 28)) s->res = 1 << 28;
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
static const cic_t CIC[4] = {   /* (g: its level, tenths of a dB, by render: each kind about -26 LUFS at INTENSITY 64, the small one close by -25) */
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
    int32_t a = (act_idx(s) >> 8) - 64;                     /* INTENSITY: how active a cicada is (songs x/ 2, pauses x/ 4; */
    int32_t k = a < 0 ? 3 : 2;                               /* down to rare short calls: songs / 2.6, pauses x 6.5) */
    int32_t l = (song ? CIC[sp].song_l2 + a * 512 * k : CIC[sp].pause_l2 - a * 1024 * k)
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
    { uint8_t *b = (uint8_t *)c; unsigned i; for (i = 0; i < sizeof *c; i++) b[i] = 0; }
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

/* the population per block: how many call (INTENSITY), who swells in or fades, who has a hero, the bed, the sync */
static void callers_block(src_t *s, uint32_t want_q8)
{
    unsigned i, want = (want_q8 + 128u) >> 8, calling = 0, sing_l = 0, kc, nheroes = 0;
    int32_t sx = 0, sy = 0, kpull;
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
    /* falling into step: the pull rises above a third of INTENSITY (0 .. ~0.6 of the phrase rate per block) */
    kpull = clampi(s->i_eff - (42 << 8), 0, 85 << 8) / 85;   /* Q8: 0 .. 256 */
    for (i = 0; i < NM; i++) nheroes += s->m[i].voice != 0xFF;
    for (i = 0; i < NM; i++) {
        member_t *m = &s->m[i];
        int32_t sn, cs, t;
        if (!(m->calling || m->env)) continue;
        sn = tx_sin_q15(m->pph); cs = tx_sin_q15(m->pph + 0x40000000u);
        t = calling > 1u ? (int32_t)((((int64_t)sx * cs - (int64_t)sy * sn) >> 10)) / (int32_t)calling : 0;   /* Q15 */
        t = clampi(t, -32767, 32767);
        m->pph += m->pinc + (uint32_t)(int32_t)(((int64_t)(int32_t)(m->pinc >> 2) * t * kpull) >> 23);
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
static void bird_draw(src_t *s, birdm_t *b, int near)
{
    int32_t sp = (9830 * (int32_t)s->p[P_SPREAD]) / 64;      /* +-0.15 octave between birds */
    b->f_off = knob(s, P_SIZE) * -2048 + (int32_t)(((int64_t)gauss(s) * sp) >> 12) + s->life_s / 4;
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
    uint32_t f = tx_exp2(TX_BIRD_F0[type] + b->f_off), lv;
    int32_t g = (int32_t)(((int64_t)tx_place_gain_q12(b->r_l2) * tx_place_air_q15(f, b->r_l2)) >> 15);
    int k;
    g = (int32_t)(((int64_t)g * pre_trim(s, s->p[P_PRESET])) >> 12);
    g = (int32_t)(((int64_t)g * s->level) >> 15);
    lv = (uint32_t)(g < 1 ? 1 : g) << 14;
    if (!steal && tx_pool_count(&s->pool) >= s->limit) return;
    if ((k = take(s, lv)) < 0) return;
    s->v[k].kind = V_BIRD; s->v[k].pre = (uint8_t)s->p[P_PRESET]; s->v[k].pad[0] = (uint8_t)bi; s->v[k].last = 0;
    s->total++;                                              /* (a song: the stats' event) */
    tx_bird_song(&s->v[k].u.bv, type, b->seed, tx_rand_below(&s->rng, b->songs), tx_rand(&s->rng), b->f_off + key_l2,
                 g);
    b->voice = (uint8_t)k;
    b->sung = 0;
    s->events++;
}

static void birds_block(src_t *s, unsigned n)
{   /* the flock (tx_core_bird): who is here, how far, who sings now; neighbours in a band take turns, others answer */
    unsigned type = (unsigned)PRE[s->p[P_PRESET]].act_lo, i, j, want, singing = 0, allowed;
    int32_t a = act_idx(s);
    want = count_n(s);                                       /* COUNT: the birds here (INTENSITY: how often each sings) */
    if (want > NB) want = NB;
    allowed = tx_bird_overlap(type, a) * ((count_n(s) + 7u) >> 3);   /* (the flock's rule within a neighbourhood of ~8;
                                                                     * more birds: more neighbourhoods singing at once) */
    for (i = 0; i < NB; i++) if (s->bm[i].voice != 0xFF && s->bm[i].active) singing++;
    for (i = 0; i < NB; i++) {
        birdm_t *b = &s->bm[i];
        if (i < want && !b->active && s->run > 4096) {
            bird_draw(s, b, 0);
            b->r_l2 = tx_bird_dist(&s->rng, type, dist_l2(s), depth_l2(s), a);   /* most far, a few near */
            b->gain = tx_bird_band(type, b->seed, b->f_off);  /* (birds keep their band here) */
            b->active = 1; b->wait = tx_rand_below(&s->rng, 65535u) * 2u;
        }
        if (b->near) b->r_l2 = dist_l2(s) + tx_bird_near(type, a);   /* the key's bird: near only as the place fills */
        if (b->active && b->voice < NV && s->v[b->voice].kind == V_BIRD && (i >= want || s->run < 4096))
            tx_bird_stop(&s->v[b->voice].u.bv);              /* leaving: it ends the phrase it is singing */
        if (b->voice < NV && s->v[b->voice].kind == V_BIRD && a < (32 << 8)) {   /* singing rarely: a phrase or */
            uint32_t lim = 66150u << ((unsigned)(a >> 8) >> 3);   /* two (1.5 s at INTENSITY 0, x 2 every 8) */
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
                singing++;
                unsigned idle = 0;
                for (j = 0; j < NB; j++) idle += j != i && s->bm[j].active && s->bm[j].voice == 0xFF;
                for (j = 0; j < NB; j++)                     /* the others may answer */
                    if (j != i && s->bm[j].active && s->bm[j].voice == 0xFF) s->bm[j].wait = tx_bird_answer(&s->rng, type, a, s->bm[j].wait, idle);
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

/* an event engine's presses: stage 1's clock (tx_core_cluster.h: the preset's runs and the breaks one break sets off;
 * a child's level step moves the velocity), at most one press per block; a strike's key is released after 150 ms */
static void host_clock(src_t *s)
{
    tx_clu_ev_t ce;
    int off, got = 0;
    int32_t lv = 0;
    if (!s->hd->e64) return;
    if (s->hoff && !--s->hoff) s->hst->key(s->host, s->hpn, 0, 0);
    clu_cfg_row(s, s->hclu, s->hreg);
    if (!s->hon) s->clu.nk = 0;                              /* (cut: no queued presses) */
    tx_clu_block(&s->clu, &s->rng, &s->cf, s->hon ? (uint32_t)(((uint64_t)host_rate(s) * count_q8(s)) >> 11) : 0u);
    while ((off = tx_clu_next(&s->clu, &s->rng, &s->cf, TX_BLOCK, &ce)) >= 0) if (!got) { got = 1; lv = ce.lv; }
    tx_clu_end(&s->clu, TX_BLOCK);
    if (got && s->hon) {                                     /* (presses: each an individual's event, COUNT / 8 x) */
        int32_t v = (int32_t)(((int64_t)s->vel * tx_exp2(clampi(lv / 2, -(3 << 16), 1 << 16) + (16 << 16))) >> 16);
        v = clampi(v, 16, 127);
        if (s->hoff) { s->hst->key(s->host, s->hpn, 0, 0); s->hoff = 0; }
        s->hpn = s->note;
        if (s->hd->oneshot) { s->hst->key(s->host, s->hpn, (uint8_t)v, 1); s->hoff = 26; }
        else { s->hst->key(s->host, s->hpn, 0, 0); s->hst->key(s->host, s->hpn, (uint8_t)v, 1); }
    }
}

/* a character map's offset now (a centred knob x its depth) */
static int32_t kmap_off(const src_t *s, unsigned j)
{
    const kmap_t *k = &HMAP[s->hd - HOST][j];
    if (k->knob == P_TYPE) return 0;
    return (knob(s, k->knob) * k->depth) / 64;
}

static void host_apply(src_t *s, int force)               /* the curves at the intensity now (only changed values sent) */
{
    unsigned i;
    if (s->hcid >= 0) {   /* the engine's own individuals: COUNT; a hero each up to HOST_HEROES or the governor's cut */
        unsigned n = (unsigned)clampi((int32_t)count_n(s), 1, s->hst->params[s->hcid].max), e;
        unsigned pp = s->part_v ? s->part_v : s->hst->particles_def;   /* (the lane's PARTICLES: the governor's) */
        if (pp > HOST_HEROES) pp = HOST_HEROES;
        pp = host_heroes(s->hd, pp);
        if (pp > s->hst->particles_max) pp = s->hst->particles_max;
        if (force || n != s->hcnt) { s->hcnt = (uint8_t)n; s->hst->param(s->host, (uint8_t)s->hcid, (uint8_t)n); }
        if (force || pp != s->hpart) { s->hpart = (uint8_t)pp; s->hst->param(s->host, TX_PID_PARTICLES, (uint8_t)pp); }
        e = n > pp ? n - pp : 0u;                            /* the rest: the engine's cheap far callers (FIELD), */
        if (s->hfid >= 0) {                                  /* louder as they are more (power ~ their number) */
            unsigned f = e ? (unsigned)clampi((int32_t)(24u * isqrt(e << 8)) >> 4, 0, 127) : 0u;
            if (force || f != s->hfld) { s->hfld = (uint8_t)f; s->hst->param(s->host, (uint8_t)s->hfid, (uint8_t)f); }
        }
    }
    if (s->hd->k127) {                                       /* the keys held: more singers */
        unsigned want = s->hon ? (unsigned)clampi((int32_t)count_n(s), 1, s->hd->k127 > 8 ? 8 : s->hd->k127) : 0u;   /* (COUNT: one key an individual) */
        if (s->hkeys <= 1) s->hbase = s->note;               /* (the extras keep the note they were pressed from) */
        if (s->hkeys && s->hkeys < want && !tx_rand_below(&s->rng, 32u)) {   /* one at a time (~0.2 s apart: logs */
            s->hst->key(s->host, (uint8_t)(s->hbase + HOST_KEYS[s->hkeys]), s->vel, 1);   /* thrown on together */
            s->hkeys++;                                      /* would crack at once) */
        }
        while (s->hkeys > want && s->hkeys > 1) { s->hkeys--; s->hst->key(s->host, (uint8_t)(s->hbase + HOST_KEYS[s->hkeys]), 0, 0); }
        if (!want) s->hkeys = 0;
        if ((s->hd->k0 & 1u) && s->hkeys && !s->hrenew--) {                      /* renew one key: a singer leaves, a new one comes */
            unsigned k = tx_rand_below(&s->rng, s->hkeys);   /* (any of them: no round) */
            uint8_t nt = (uint8_t)(k ? s->hbase + HOST_KEYS[k] : s->note);
            s->hst->key(s->host, nt, 0, 0);
            s->hst->key(s->host, nt, s->vel, 1);
            s->hrenew = 1033u + tx_rand_below(&s->rng, 1723u);   /* 6 .. 16 s in blocks */
        }
    }
    for (i = 0; i < HOST_NC; i++) {
        int32_t v;
        unsigned j;
        if (!s->hd->c[i].name || s->hid[i] < 0) continue;
        v = curve_at(&s->hd->c[i], i ? s->i_eff : act_idx(s));
        if (!i && s->hd->k64) {                              /* COUNT past the keys: each doubling more on the first */
            int32_t kb = s->hd->k127 >= 8 ? 63 : s->hd->k127 >= 4 ? 42 : s->hd->k127 >= 2 ? 21 : 0;   /* curve (the */
            int32_t over = (int32_t)s->p[P_COUNT] - 1 - kb;                                         /* keys' 21 log2) */
            if (over > 0) v += (over * s->hd->k64) / 21;
        }
        for (j = 0; j < HOST_NK; j++) if (s->kcur[j] == (int8_t)i) v += kmap_off(s, j);
        v = clampi(v, 0, 127);
        if (force || v != s->hval[i]) { s->hval[i] = (uint8_t)v; s->hst->param(s->host, (uint8_t)s->hid[i], (uint8_t)v); }
    }
    for (i = 0; i < HOST_NK; i++) {                          /* the character maps on the engine's other parameters */
        const kmap_t *k = &HMAP[s->hd - HOST][i];
        int32_t v;
        if (s->kid[i] < 0 || s->kcur[i] >= 0) continue;
        if (k->knob == P_TYPE)                               /* the kind: over TYPE's range for it (0: the own) */
            v = s->p[P_TYPE] >= s->tlo && (s->tlo || s->p[P_TYPE])
                ? clampi(((int32_t)(s->p[P_TYPE] - s->tlo) * 127) / s->tspan, 0, 127) : s->kdef[i];
        else v = clampi((int32_t)s->kdef[i] + kmap_off(s, i), 0, 127);
        if (force || v != s->kval[i]) { s->kval[i] = (uint8_t)v; s->hst->param(s->host, (uint8_t)s->kid[i], (uint8_t)v); }
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
/* a held engine in bouts (k0 & 2): one individual held all the time is a bed at the bottom; so below INTENSITY 64
 * it is held in bouts (~0.7 s, an exponential), off between them for on x (1 - p) / p, p its share on: 0.03 at 0 ..
 * 1 at 64, rising as the square (a pour, a squeak, a flare, a bee passing now and then .. on all the time). A press opens a bout. */
static uint32_t gate_draw(src_t *s, uint32_t mean)
{
    uint32_t b = (uint32_t)(((uint64_t)mean * ((tx_neglog2_q12(tx_rand(&s->rng)) * 2839u) >> 12)) >> 12);
    return b < 20u ? 20u : b > 65535u ? 65535u : b;
}
static void host_gate(src_t *s)
{
    int32_t a = act_idx(s) >> 8;
    uint32_t p = a >= 64 ? 4096u : 120u + (((uint32_t)(a * a) * 3976u) >> 12);   /* Q12 (square: LIFE's wander */
                                                             /* near 0 stays rare) */
    if (p >= 4096u) { s->hgate = 1; s->hgt = 0; return; }
    if (s->hgt) { s->hgt--; return; }
    s->hgate = (uint8_t)!s->hgate;
    s->hgt = (uint16_t)gate_draw(s, s->hgate ? GATE_ON : (GATE_ON * (4096u - p)) / p);
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
    if (want == s->hcur) return;
    s->hcur = want;
    s->hclu = fm ? &fm->clu : &CLU[pre]; s->hreg = (int8_t)(fm ? 0 : PRE[pre].reg);
    s->wst = 0; s->wheld = 0;
    tx_declick_add(&s->dc, s->last_out);
    s->last_out = 0;
    { uint8_t *b = (uint8_t *)s->host; for (i = 0; i < sizeof s->host; i++) b[i] = 0; }
    s->hst = ne; s->hd = hd;
    /* the particle presets' beds and filters start from silence (their last output went to the declick) */
    s->res = s->res_x1 = s->clp = 0;
    s->hp.x1 = s->hp.y1 = 0; s->vy.lp = 0; s->run = 0; s->hgain = 0;
    for (i = 0; i < TX_BLOCK; i++) s->buf[i] = s->ck[i] = 0;
    if (ne && ne->state_bytes <= HOST_BYTES) {
        ne->init(s->host, tx_rand(&s->rng));
        for (i = 0; i < HOST_NC; i++) s->hid[i] = (int8_t)(hd->c[i].name ? tx_param_find(ne, hd->c[i].name) : -1);
        for (i = 0; i < HOST_NK; i++) {                      /* the character maps: on a curve, or on their own */
            const kmap_t *k = &HMAP[hd - HOST][i];
            unsigned c;
            s->kid[i] = (int8_t)(k->name ? tx_param_find(ne, k->name) : -1);
            s->kcur[i] = -1;
            s->kdef[i] = s->kid[i] >= 0 ? ne->params[s->kid[i]].def : 0;
            s->kval[i] = 0xFF;
            for (c = 0; c < HOST_NC && s->kid[i] >= 0; c++) if (s->hid[c] == s->kid[i]) s->kcur[i] = (int8_t)c;
        }
        if (s->lvl_v) ne->param(s->host, TX_PID_LEVEL, s->lvl_v);
        if (s->part_v) ne->param(s->host, TX_PID_PARTICLES, (uint8_t)host_heroes(hd, s->part_v));
        s->hcid = (int8_t)tx_param_find(ne, "COUNT");
        s->hfid = (int8_t)(s->hcid >= 0 ? tx_param_find(ne, "FIELD") : -1);
        host_apply(s, 1);
        s->hkeys = 0; s->hrenew = 258; s->hoff = 0;
        s->hgate = 1; s->hgt = (uint16_t)gate_draw(s, GATE_ON);   /* (a sound switched on: a bout now) */
        tx_clu_init(&s->clu);                       /* (the first renewal after 1.5 s: past the fader's glide) */
        s->hon = 0;
        if (s->held && s->p[P_COUNT]) { ne->key(s->host, s->note, s->vel, 1); s->hkeys = 1; s->hon = 1; }
        s->quiet = 0;
    } else {                                                 /* the particle presets: fresh pools */
        s->hst = 0; s->hd = 0;
        tx_pool_init(&s->pool, NV, s->limit);
        if (PRE[pre].kind == 0) grains_setup(s, pre);       /* (the members' memory: the grains) */
        else {
            for (i = 0; i < NM; i++) s->m[i].voice = 0xFF;
            if (PRE[pre].kind == 1) grains_setup(s, pre);   /* the far callers' click grains */
        }
        tx_clu_init(&s->clu);
        if (PRE[pre].kind == 4) {                            /* birds: their members fresh (over the callers' memory) */
            { uint8_t *b = (uint8_t *)s->bm; for (i = 0; i < sizeof s->bm; i++) b[i] = 0; }
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
                    const char *nm = HOST[PRE[pre].act_lo].c[i].name;
                    s->wid[i] = (int8_t)(nm ? tx_param_find(we, nm) : -1);
                    s->wval[i] = 0xFF;
                }
                if (s->lvl_v) we->param(ws, TX_PID_LEVEL, s->lvl_v);
                we->param(ws, TX_PID_PARTICLES, (uint8_t)(s->part_v && s->part_v < WATER_HEROES ? s->part_v : WATER_HEROES));
            }
        }
    }
}

/* LIFE, once per block: the intensity's glide and walk, the size and distance walks */
static void life_block(src_t *s)
{
    int32_t tau = 482152 + (int32_t)s->p[P_TIME] * 4258;       /* log2 blocks: 1 s (172) .. 300 s (51680) */
    int32_t depth = (int32_t)s->p[P_LIFE];
    int32_t a = tx_life_step(&s->la, tau), sz = tx_life_step(&s->ls, tau), dd = tx_life_step(&s->ld, tau + 65536);
    if (s->start) { s->start = 0; s->i_glide = (int32_t)s->p[P_INTENS] << 8; }   /* (starting: where the fader is) */
    s->i_glide += ((int32_t)s->p[P_INTENS] * 256 - s->i_glide) >> 6;   /* a fader move glides in (~1 s to 95 %) */
    {   /* LIFE: +-40 at 127, narrowing to none at the fader's ends (INTENSITY 0 stays the least active: the walk */
        int32_t w = ((a * depth) >> 7) * 5 >> 4, edge = s->i_glide < (127 << 8) - s->i_glide ? s->i_glide   /* no */
                                                                                          : (127 << 8) - s->i_glide;
        if (edge < (32 << 8)) w = (int32_t)(((int64_t)w * (edge < 0 ? 0 : edge)) >> 13);   /* longer lifts it) */
        s->i_eff = clampi(s->i_glide + w, 0, 127 << 8);
    }
    s->life_s = (sz * depth) >> 8;                           /* +-0.25 octave at LIFE 127 */
    s->life_d = (dd * depth) >> 6;                           /* +-1 octave of distance */
}

/* the hosted engine's block: LIFE and the curves, its render, the preset's gain, out */
/* a hosted engine's block out: its render, the gain gdb (tenths of a dB) x PROXIMITY's 1 / r re 3 m (ramped over the
 * block), the air's one-pole, the declick, into the mix */
TX_AUDIO_CODE static void hosted_out(src_t *s, const tx_engine_t *e, void *st, int32_t *mix, unsigned n, int32_t gdb)
{
    unsigned j;
    int32_t g0 = s->hgain, g1, d;
    for (j = 0; j < n; j++) s->buf[j] = 0;
    e->render(st, s->buf, n);
    g1 = (int32_t)tx_exp2((12 << 16) + TX_DB10(gdb) + 103872 - clampi(dist_l2(s), TX_PLACE_R_MIN, 14 << 16));
    if (g1 > 32767) g1 = 32767;
    {   /* the air between: a one-pole whose corner falls with the distance (no change closer than ~10 m) */
        int32_t lp = s->hlp, al = tx_place_lp_q15(dist_l2(s));
        if (al < 32000) {
            for (j = 0; j < n; j++) { int32_t dd = s->buf[j] - lp; lp += ((dd >> 15) * al) + (((dd & 0x7FFF) * al) >> 15); s->buf[j] = lp; }
            s->hlp = lp;
        } else s->hlp = s->buf[n - 1];
    }
    if (!g0) g0 = g1;
    d = (g1 - g0) / (int32_t)n;
    for (j = 0; j < n; j++) { s->buf[j] = tx_trim1(s->buf[j], g0 + d * (int32_t)j); }
    s->hgain = g1;
    tx_declick_run(&s->dc, s->buf, n);
    for (j = 0; j < n; j++) mix[j] += s->buf[j];
    s->last_out = s->buf[n - 1];
    for (j = 0; j < n; j++) s->buf[j] = 0;
}

TX_AUDIO_CODE static void host_render(src_t *s, int32_t *mix, unsigned n)
{
    tx_stats_t st;
    int32_t gdb, x = s->i_eff, a = s->hd->g0, b = s->hd->g64;
    life_block(s);
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
            if (!s->hd->oneshot) s->hst->key(s->host, s->note, s->vel, 1);
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
    if (on != s->wheld) { s->wheld = (uint8_t)on; s->wst->key(ws, s->note, on ? 100 : 0, (uint8_t)on); }
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
    for (i = 0; i < sizeof *s; i++) b[i] = 0;
    tx_rng_seed(&s->rng, seed);
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
    s->i_eff = s->i_glide = 64 << 8;
    tx_life_init(&s->la, seed * 3u + 1u);
    tx_life_init(&s->ls, seed * 5u + 2u);
    tx_life_init(&s->ld, seed * 7u + 3u);
    tx_vary_init(&s->vy, seed, VARY_LO, VARY_HI, VARY_TILT, VARY_DRIFT, VARY_SPLIT);
}

/* one of the individuals (rain, stones, water drops) acts now: an event of its own, where it is and as loud as its own
 * events are, setting off its own (a stone's bounces); queued as stage 1's next event (src_render) */
static void indiv_act(src_t *s)
{
    tx_clu_kid_t *k;
    if (s->clu.nk >= TX_CLU_Q) return;
    k = &s->clu.k[s->clu.nk++];
    k->t = s->clu.now; k->lv = 0; k->gen = 0;
    k->tag = (uint8_t)(s->ni > 1u ? tx_rand_below(&s->rng, s->ni) : 0u);
    s->total++;
}

static void src_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    src_t *s = (src_t *)st;
    if (on && vel && !s->held) { s->start = 1; s->clu.next_q8 = 0xFFFFFFFFu; }   /* a first press: the population starts
                                                             * at the fader's INTENSITY, its first event after a gap of its
                                                             * own (stage 1 draws it), never at once */
    if (s->hst) {                                            /* a hosted engine: its own keys */
        if (on && vel && s->hd && (s->hd->k0 & 2u) && !s->hgate) { s->hgate = 1; s->hgt = (uint16_t)gate_draw(s, GATE_ON); }
        if (s->hd && s->hd->k127 && s->hon && s->hkeys) {    /* keys are individuals (COUNT holds them): another */
            if (on && vel) {                                 /* press is one of them acting afresh (a log thrown on, */
                unsigned k = tx_rand_below(&s->rng, s->hkeys);   /* a singer starting again), never a new key: the */
                uint8_t nt = (uint8_t)(k ? s->hbase + HOST_KEYS[k] : s->note);   /* engine's keys stay theirs */
                s->held++; s->vel = vel; s->quiet = 0;
                s->hst->key(s->host, nt, 0, 0);
                s->hst->key(s->host, nt, vel, 1);
                return;
            }
            if (s->held && --s->held) return;
            s->hon = 0;                                      /* the last key up: all of them */
            s->hst->key(s->host, s->note, 0, 0);
            while (s->hkeys > 1) { s->hkeys--; s->hst->key(s->host, (uint8_t)(s->hbase + HOST_KEYS[s->hkeys]), 0, 0); }
            s->hkeys = 0;
            return;
        }
        if (on && vel) {
            s->held++; s->note = note; s->vel = vel; s->quiet = 0;
            if (!s->p[P_COUNT]) return;                      /* (COUNT 0: none; host_render presses it when COUNT rises) */
            if (!s->hkeys) s->hkeys = 1;
            s->hon = 1;
        } else if (s->held) s->held--;
        if (!s->held) s->hon = 0;
        s->hst->key(s->host, note, vel, on);
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
        if (PRE[pre].kind == 4) {                            /* a bird sings now, at the key's pitch: the one close by, */
            unsigned bi = 0, q;                              /* or (it is singing) another one that is free; all of */
            birdm_t *b;                                      /* them singing: none (a song is never cut by a press) */
            if (s->bm[0].voice != 0xFF) for (bi = NB, q = 1; q < NB; q++) if (s->bm[q].active && s->bm[q].voice == 0xFF) { bi = q; break; }
            if (bi < NB) {
                b = &s->bm[bi];
                if (!bi) bird_draw(s, b, 1);
                b->active = 1;
                bird_sing(s, bi, (unsigned)PRE[pre].act_lo, 1, ((int32_t)note - 60) * 5461);
            }
        } else if (PRE[pre].kind == 1) {                     /* a cicada close by, at the key's pitch (one singing */
            member_t *m = &s->m[0];                          /* sings on: a press does not cut its song) */
            if (!(m->calling && m->song)) {
                member_new(s, m, 1);
                if (m->voice != 0xFF) { unsigned vi = m->voice; voice_cut(s, vi); tx_pool_free(&s->pool, vi); m->voice = 0xFF; }
                m->calling = 0; m->fresh = 1;
            }
        } else if (s->held > 1) indiv_act(s);               /* a press while the key is held: one of them acts now
                                                             * (the first press only starts the population: no strike) */
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
    if (s->hst && id == TX_PID_LEVEL) s->hst->param(s->host, id, val);
    if (s->hst && id == TX_PID_PARTICLES) {                  /* (the governor's: at most the engine's cap) */
        s->hpart = (uint8_t)host_heroes(s->hd, val);
        s->hst->param(s->host, id, s->hpart);
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
        keep_loudest(s, s->limit);                           /* (a lower limit lets the quietest go: the governor) */
        cut = tx_pool_set_limit(&s->pool, s->limit);
        for (i = 0; i < s->pool.max; i++) if (cut & (1u << i)) voice_cut(s, i);
    } else if (id == TX_PID_LEVEL)
        s->level = tx_level_q15(val > 127u ? 127u : val);
    else if (id < NP) {
        s->p[id] = (uint8_t)(val > PARAMS[id].max ? PARAMS[id].max : val);
        if (id == P_PRESET || id == P_TYPE) host_switch(s, s->p[P_PRESET]);
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
    if (s->quiet && !s->held) return;
    if (s->hst) { host_render(s, mix, n); return; }
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
        unsigned guard = 0, cn = count_n(s);
        tx_clu_ev_t ce;
        int off;
        s->clu.itag = (uint8_t)(cn > 1u ? tx_rand_below(r, cn) : 0u);
        while (guard++ < (pa == PR_RAIN ? 64u : 112u) && (off = tx_clu_next(&s->clu, r, &s->cf, n, &ce)) >= 0) {
            unsigned pre = pa;
            if (eb && !ce.kid && (!ea || (tx_rand(r) >> 17) < (uint32_t)w)) pre = pb;
            if (pre == pa && ea && ce.tag < s->ni) {         /* its place: a stone's or a drip point's where it is; a rain */
                const indiv_t *d = &s->ind[ce.tag];          /* patch's drops over the ground around, most far (the */
                s->cur_r = pa == PR_RAIN ? ev_dist(s, pa)    /* salience law: rain falls all around), its drop sizes */
                           : d->r_l2 + (pa == PR_STONES ? (int32_t)(((int64_t)gauss(s) * 3277) >> 12) : 0);
                s->cur_sz = (int32_t)d->sz * 16;
                s->cur_on = 1;
            }
            event(s, (unsigned)off, n, pre, 0, 100, pre == PR_RAIN ? w_q8 : 256u, ce.lv);
            s->cur_on = 0;
            if (!ce.kid) s->total++;
            s->clu.itag = (uint8_t)(cn > 1u ? tx_rand_below(r, cn) : 0u);
        }
        tx_clu_end(&s->clu, n);
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
            tx_modal_run(v->u.s.m, NMO, s->buf, 0, n);
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
        for (j = 0; j < n; j++) { in |= s->ck[j]; lp += ((s->ck[j] - lp) * 15) >> 5; s->buf[j] += lp * 256; }
        s->clp = (lp > -8 && lp < 8 && !in) ? 0 : lp;
    }
    /* the far drops (rain): grains, each its own bubble or splash (tx_core_grains.h) */
    if (PRE[pa].kind == 0) tx_gr_render(&s->gr, r, s->buf, n);
    /* the stones' residue: low-passed noise, tau 2 ms */
    if (s->res) {
        int32_t en = s->res, x1 = s->res_x1;
        for (j = 0; j < n; j++) {
            int32_t x = (int32_t)(tx_rand(r) >> 17) - 16384;
            x1 += (x - x1) >> 1;
            s->buf[j] += (x1 * (en >> 14)) >> 3;
            en -= (en >> 6) + 1;
            if (en <= 0) { en = 0; break; }
        }
        s->res = en; s->res_x1 = en ? x1 : 0;
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
    "0 particles 16\n0 param INTENSITY 127\n0 param PROXIMITY 127\n0 param PRESET 2\n0 on 60 127\n"
    "1000 param PRESET 1\n2000 param PRESET 0\n3000 param PRESET 2\n4000 param PRESET 3\n"
    "5000 param PRESET 4\n6000 param PRESET 5\n7000 param PRESET 6\n8000 param PRESET 7\n"
    "9000 param PRESET 8\n10000 param PRESET 3\n11000 param PRESET 9\n12000 param PRESET 10\n"
    "13000 param PRESET 11\n14000 param PRESET 12\n15000 param PRESET 13\n16000 param PRESET 14\n"
    "17000 param PRESET 15\n18000 param PRESET 16\n19000 param PRESET 17\n20000 param PRESET 18\n21000 param PRESET 19\n"
    "22000 off 60\n23000 end\n";

const tx_engine_t tx_eng_source = {
    "source", "Landscape", NP, PARAMS, NV, 6, sizeof(src_t), SCENE_MAX,
    src_init, src_key, src_param, src_render, src_stats,
};
