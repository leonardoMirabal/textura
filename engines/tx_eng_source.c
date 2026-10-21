/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura SOURCE: one plastic sound source in four stages (the design: what happens, what strikes, what rings, where
 * it is), the first prototype with three presets (rain on water, a stone cloud, cicadas) and a particle-level MORPH
 * between any two of them. The engines it grew from (rainwater, stonecloud, cicadas) stay as they are.
 *
 * Playing: a held key keeps the source going (it fades in over ~0.2 s and, released, passes over ~1-2 s); the key
 * moves the size (a semitone per key from C4, as the presets' own engines do) and a press adds one near event (a big
 * drop, a thrown stone, a cicada close by). How much happens is INTENSITY: one fader, on each preset's own scale and
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
 *            by mineral, Hunt-Crossley contacts, power-law energies). The transition: an avalanche. Above the
 *            middle each knock may set off others (a branching ratio rising to 0.85, Omori delays c 30 ms, p 1.5),
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

#define OUT_TRIM 4096               /* output gain, Q12 (the presets' own levels are in PRE[].trim) */

#define VARY_LO -100
#define VARY_HI 15
#define VARY_TILT 100
#define VARY_DRIFT 40
#define VARY_SPLIT 1

enum { P_PRESET, P_INTENS, P_ACT, P_SPACING, P_SIZE, P_SPREAD, P_TONE, P_HARD, P_PROX, P_DEPTH,
       P_LIFE, P_TIME, P_LOCK, P_DIV, P_GRID, NP };
enum { PR_RAIN, PR_STONES, PR_CICADAS, PR_WATER, PR_WIND, PR_FIRE, PR_CRICKETS, PR_SAND, PR_GRAVEL, PR_KATYDIDS, PR_BEES, PR_ICE, PR_WOOD, PR_MINERALS, PR_GLASS, PR_SONGBIRD, PR_CHIRPER, PR_CROW, PR_OWL,
       PR_GULL, NPRE };
enum { V_NONE, V_BUBBLE, V_STONE, V_CALLER, V_BIRD };

static const tx_param_t PARAMS[NP] = {
    {"PRESET", NPRE - 1, 0, "the source: 0 rain on water | 1 stone cloud | 2 cicadas | 3 water | 4 wind | 5 fire | 6 crickets | 7 sand | 8 gravel | 9 katydids | 10 bees | 11 ice | 12 wood | 13 minerals | 14 glass | 15 songbird | 16 chirper | 17 crow | 18 owl | 19 gull"},
    {"INTENSITY", 127, 64, "how much happens, on the preset's own scale and through its transition: rain 30 .. 20000 drops/s per m^2 (drizzle .. downpour, fusing into a hiss), stones 0.2 .. 500 knocks/s (an avalanche), cicadas 1 .. 30 callers (falling into step), water a single drop per 30 s .. dripping (2.8 /s) .. a trickle .. a stream .. a river, wind 1.6 .. 20 m/s (a breath .. a gale, wires whistling), fire 1 .. 30 crackles/s per log (embers .. a blaze), crickets 1 .. 16 (one .. a chorus falling into step), sand a trickle .. a pour, gravel a few stones .. a slide, katydids one in the cold .. a warm chorus of 8 in step, bees one far bee .. a swarm close by, ice an icicle per 30 s .. one every 1.5 s, wood a creak per 30 s .. snaps every 2 s, minerals a knock per 10 s .. 7 /s, glass a crack per 40 s .. a smash every 2 s, birds one bird singing now and then .. countersinging .. a dawn chorus of 12"},
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
};

#define NV 16u                      /* heroes */
#define NM 32u                      /* members (callers) */
#define NPEND 40u
#define NCH 32u                     /* pending knocks (an avalanche's children) */
#define EVMAX 24u                   /* explicit heroes started per block */
#define RATE_CAP 5000u              /* explicit events per second at most: above, each stands for several (rain) */

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
    {0, 490986, 614782, 0, 23500, 0},   /* rain: 30 .. 20000 drops/s per m^2 (drizzle .. downpour), over ~6 m^2 heard (ours) */
    {0, -152170, 739752, 10, 27000, 0}, /* stones: 0.2 .. 500 knocks/s */
    {1, 0, 321577, 0, 5160, 0},     /* cicadas: 1 .. 30 callers */
    {3, 1, 0, -6, 19800, 0},        /* water: drops (WATER_RATE) + the stream engine (HOST[1]) fading in */
    {2, 0, 0, 0, 4096, 0},          /* wind: the aeolian engine hosted */
    {2, 2, 0, 0, 4096, 0},          /* fire */
    {2, 3, 0, 0, 4096, 0},          /* crickets */
    {2, 4, 0, 0, 4096, 0},          /* sand */
    {2, 5, 0, 0, 4096, 0},          /* gravel */
    {2, 6, 0, 0, 4096, 0},          /* katydids */
    {2, 7, 0, 0, 4096, 0},          /* bees */
    {2, 8, 0, 0, 4096, 0},          /* ice */
    {2, 9, 0, 0, 4096, 0},          /* wood */
    {2, 10, 0, 0, 4096, 0},         /* minerals */
    {2, 11, 0, 0, 4096, 0},         /* glass */
    {4, 0, 0, 0, 410, 0},          /* birds (tx_core_bird type act_lo): songbird (blackbird, thrush) */
    {4, 1, 0, 0, 410, 0},          /* chirper (chaffinch, sparrow) */
    {4, 2, 0, 0, 1630, 0},          /* crow */
    {4, 3, 0, 0, 130, 0},          /* owl (tawny, eagle owl) */
    {4, 4, 0, 0, 860, 0},          /* gull */
};

/* HOSTED presets: an existing engine run whole inside the source (its own physics and defaults: at INTENSITY 64 it is
 * that engine at its defaults), INTENSITY moving its parameters along the preset's path through its transition: each
 * curve a parameter's value at INTENSITY 0, 64 and 127 (linear between); the first curve follows ACTIVITY too. The
 * level is set with a gain at 0 / 64 / 127 (tenths of a dB, linear between: ours, by render: every preset at INTENSITY 64
 * about -27 LUFS, a key held 20 s, the mean of 3 seeds, so lanes sit together in a landscape). Hosted
 * presets do not morph (one engine fits the arena). Where an engine spawns its singers per key press (katydids, bees),
 * k0 / k64 / k127 is how many keys the source holds down (1 .. 4, a few semitones apart): more keys, more of them;
 * and one held key at a time is pressed afresh every 6 .. 16 s: singers leave and new ones come with the
 * parameters of now (these engines read them at a press). */
#define HOST_NC 5u
#define HOST_BYTES 12544u           /* the largest hosted engine's state (sand 12 464 B) */
typedef struct { const char *name; uint8_t v0, v64, v127; } curve_t;
typedef struct {
    const char *eng; int16_t g0, g64, g127; curve_t c[HOST_NC]; uint8_t k0, k64, k127;
    uint16_t e0, e64, e127;         /* event engines: the mean time between presses (0.1 s; log between; 0 = none) */
    uint8_t oneshot;                /* 1: each press released after 150 ms (a strike); 0: the held key pressed afresh */
} host_t;   /* (g: one calibration, flat:
                                                     * more activity is louder by the engine's own physics) */
static const host_t HOST[] = {
    /* wind: a breath (1.6 m/s) .. a gale (20 m/s); gusts deepen; at the top the strings lock in and whistle */
    {"aeolian", -78, -78, -78, {{"WIND", 45, 88, 127}, {"GUST", 15, 40, 90}, {"LOCK", 60, 90, 120}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 0, 0, 0, 0, 0, 0, 0},
    /* stream: a trickle of plinks .. a babbling brook .. a torrent: bigger gulping bubbles, eddies, the low roar */
    {"stream", -42, -42, -42, {{"FLOW", 10, 70, 127}, {"ROAR", 0, 0, 100}, {"EDDY", 20, 30, 60}, {"NEAR", 80, 64, 50},
                            {"SPREAD", 80, 64, 45}}, 0, 0, 0, 0, 0, 0, 0},
    /* fire: embers (a rare crackle, a faint roar) .. a campfire .. a blaze (a big fire, dense crackles in bursts, roar, gusts) */
    {"fire", 150, 150, 150, {{"MOIST", 25, 72, 127}, {"SIZE", 20, 50, 100}, {"ROAR", 20, 80, 127}, {"DRAUGHT", 10, 30, 70},
                          {"BURSTY", 8, 24, 60}}, 0, 0, 0, 0, 0, 0, 0},
    /* crickets: one caller .. a few .. a chorus falling into step, with the distant field behind (warmer: a little faster) */
    {"crickets", -12, -12, -12, {{"CHORUS", 0, 16, 127}, {"SYNC", 30, 64, 110}, {"FIELD", 0, 0, 90}, {"TEMP", 70, 76, 84},
                             {0, 0, 0, 0}}, 0, 0, 0, 0, 0, 0, 0},
    /* sand: a trickle of grains through a narrow neck .. a pour .. a heavy pour from higher, pulsing */
    {"sand", -37, -37, -37, {{"ORIFICE", 20, 50, 115}, {"HEIGHT", 30, 50, 90}, {"PULSE", 0, 0, 40}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 0, 0, 0, 0, 0, 0, 0},
    /* gravel: a few stones now and then .. footsteps on gravel .. a slide (many stones, long rattles) */
    {"gravel", 16, 16, 16, {{"AMOUNT", 20, 64, 120}, {"RATE", 20, 59, 110}, {"SHAKE", 20, 30, 70}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 0, 0, 0, 0, 0, 0, 0},
    /* katydids: one singer on a cold night (slow calls) .. one at its ease .. a warm chorus of 8, falling into step */
    {"katydids", 0, 0, 0, {{"CHORUS", 0, 0, 50}, {"TEMP", 40, 76, 90}, {"SYNC", 40, 64, 110}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 1, 1, 2, 0, 0, 0, 0},
    /* bees: one bee wandering farther off, slower .. one at its ease .. a swarm of 8 close by */
    {"bees", -45, -45, -45, {{"SWARM", 0, 0, 64}, {"DIST", 90, 50, 30}, {"SPEED", 40, 64, 80}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 1, 1, 2, 0, 0, 0, 0},
    /* ice: an icicle breaking off now and then .. icicles falling every few seconds, more chunks each */
    {"ice", 175, 175, 175, {{"AMOUNT", 20, 64, 127}, {"FIZZ", 10, 20, 40}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 0, 0, 0,
     300, 70, 15, 1},
    /* wood: a branch taken and bent now and then, creaking .. load building faster, snapping every few seconds */
    {"wood", -35, -35, -35, {{"LOAD", 110, 40, 15}, {"CREAK", 15, 25, 70}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 0, 0, 0,
     300, 80, 20, 0},
    /* minerals: a knock on a stone now and then .. knocking and clinking, quicker */
    {"minerals", -40, -40, -40, {{"HOLD", 50, 36, 30}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 0, 0, 0,
     100, 12, 2, 1},
    /* glass: a pane cracking now and then .. smashing every few seconds, harder */
    {"glass", 0, 0, 0, {{"ENERGY", 60, 100, 127}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}}, 0, 0, 0,
     400, 90, 20, 1},
};

typedef struct {
    tx_tymbal_t ty;
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
} member_t;

#define NB 16u                      /* birds in a lane at most (12 sing at the top) */
typedef struct {                    /* a bird of the population: persistent, its own voice and songs */
    int32_t f_off, r_l2, gain;      /* its pitch (log2 Q16 re its type), distance (log2 m Q16), gain (Q12) */
    uint32_t seed, wait;            /* its songs' seed; samples to its next song */
    uint8_t voice, songs, active, near;
} birdm_t;

typedef struct {                    /* an avalanche's child knock waiting for its time */
    uint32_t t;
    int32_t lv, size_l2;
    uint8_t pre, gen, pad[2];
} child_t;

typedef struct {
    tx_rng_t rng;
    tx_pool_t pool;
    tx_declick_t dc;
    tx_dcblock_t hp;
    union {                         /* the particle presets' pools, or a hosted engine's whole state */
        struct {
            voice_t v[NV];
            union { struct { member_t m[NM]; child_t ch[NCH]; }; birdm_t bm[NB]; };
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
    uint8_t lvl_v, part_v;          /* LEVEL and PARTICLES as last set (0: not set) */
    uint8_t hkeys, hbase, hcycle, hpad;
    uint16_t hrenew;                /* blocks to the next renewed key */
    uint32_t hnext;                 /* event engines: blocks to the next press */
    uint8_t hoff, hpn, hpad2[2];    /* blocks to its release, its note */         /* keys the source holds on the hosted engine (k curves; the first is the player's) */
    int32_t hgain, last_out, hlp;        /* the hosted output gain (Q12, ramped), the last sample out (for a switch) */
    int32_t buf[TX_BLOCK], ck[TX_BLOCK];
    uint8_t p[NP], limit, quiet, held, nch;
    uint8_t note, vel, pad[2];
    int32_t level;
    int32_t run;                    /* the source on (Q15 ramp) */
    uint32_t now;                   /* sample clock */
    uint32_t next_q8;               /* samples to the next event, Q8 */
    int32_t i_eff;                  /* INTENSITY with LIFE, Q8 knob units */
    int32_t i_glide;                /* INTENSITY gliding to the fader, Q8 */
    int32_t trim_c;                 /* the callers' level now (pre_trim), Q12 */
    tx_life_t la, ls, ld;           /* LIFE: intensity, size, distance */
    int32_t life_s, life_d;         /* their offsets now (Q16 log2) */
    /* the beds: rain's two bands, stones' residue, the callers' chorus */
    uint32_t bed1_p, bed2_p;        /* rain beds, power per sample (Q30 of FS^2), smoothed */
    uint32_t acc1, acc2;            /* this block's dropped energy (Q15^2 samples / 64) */
    int32_t b1x1, b1x2, b2x1, b2lp, blp1, blp2, bed_a;
    int32_t res, res_x1;            /* the stones' attack residue */
    uint32_t cbed_p;                /* the callers' bed power (Q30) */
    int32_t cbed_f;                 /* its carrier, Hz */
    tx_ireso_t cres;                /* its resonance */
    int32_t cbed_mod;               /* its phrase, Q15 */
    int32_t clp;                    /* click low-pass */
    uint16_t dropped, events;
    uint32_t total, steals;
    tx_mallet_t mallet;
    uint32_t pm;                    /* the stones' power-law cut */
    tx_vary_t vy;
    uint32_t x32;                   /* bed noise */
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

static uint32_t pre_amount(const src_t *s, unsigned pre)
{
    const preset_t *q = &PRE[pre];
    int32_t a = act_idx(s);
    if (q->kind == 3) {
        static const int32_t AT[4] = { 0, 40 << 8, 90 << 8, 127 << 8 }, Y[4] = { -321577, 97352, 387088, 348787 };
        return tx_exp2(pw4(AT, Y, a) + (8 << 16));
    }
    int32_t l = q->act_lo + (((q->act_oct >> 5) * a) / (127 << 8)) * 32;   /* 32-bit: < 2^30 */
    return tx_exp2(l + (8 << 16));
}

/* the preset's level at the effective intensity (Q12): its trim x the fader law, amplitude ~ amount^(-1/4) around
 * the middle (ours: so a thousandfold rate is +15 dB, not +30: one fader stays playable from a drizzle to a downpour) */
static int32_t pre_trim(const src_t *s, unsigned pre)
{
    const preset_t *q = &PRE[pre];
    int32_t l = -((((q->act_oct >> 5) * (act_idx(s) - (64 << 8))) / (127 << 8)) * q->law);   /* -oct (a - 64) / 127 / 4 */
    return (int32_t)(((int64_t)q->trim * (int32_t)tx_exp2(l + (12 << 16))) >> 12);
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
    if (bed_q8 || s->events >= EVMAX || (k = take(s, env)) < 0) {   /* no hero: its energy joins the bed (Campbell) */
        uint32_t d = tx_bubble_damp_q8(a_l2) >> 8, e15 = env >> 15;   /* 1/s; amplitude Q15 */
        uint32_t e = (((e15 * e15) >> 12) * (TX_FS / 4u)) / (d ? d : 1u);   /* A^2 fs / 4d: Q18 FS^2 samples */
        if (bed_q8) e = (uint32_t)(((uint64_t)e * bed_q8) >> 8);
        uint32_t *acc = f > 10000u ? &s->acc1 : &s->acc2;
        *acc = *acc + e < *acc ? 0xFFFFFFFFu : *acc + e;
        return;
    }
    s->events++;
    s->v[k].kind = V_BUBBLE; s->v[k].pre = (uint8_t)pre; s->v[k].last = 0;
    tx_bubble_start(&s->v[k].u.b, a_l2, xi_q16, env, wait);
}

/* one drop: diameter from the size distribution, its class's sounds */
/* one drop, or (w_q8 > 256: the rain over the explicit cap) w drops alike: the extra small ones' bubbles join the
 * bed by their energy, the extra clicks and big ones are placed at random in the block */
static void drop(src_t *s, unsigned off, unsigned n, int near, int32_t vel, unsigned pre, int32_t trim, uint32_t w_q8)
{
    tx_rng_t *r = &s->rng;
    int32_t r_l2 = near ? dist_l2(s) - 65536 : tx_place_draw(r, dist_l2(s), depth_l2(s));
    int32_t lam_l2, d_q12, c;
    int32_t g = (int32_t)(((int64_t)tx_place_gain_q12(r_l2) * tx_place_air_q15(5000u, r_l2)) >> 15);   /* clicks */
    g = (int32_t)(((int64_t)g * trim) >> 12);
    /* the rain rate behind INTENSITY's flux F (drops / m^2 / s): Marshall-Palmer's flux ~ 415 R^0.65 (note 02's table, ours),
     * so log2 R = 1.54 (log2 F - log2 415); Lambda = 4.1 R^-0.21 / mm; SIZE +-2 octaves of the sizes */
    {
        int32_t lf = 321577 + (((PRE[PR_RAIN].act_oct >> 5) * s->i_eff) / (127 << 8)) * 32;   /* log2 30 + ... */
        int32_t lr = clampi(((lf - 569967) * 197) >> 7, -435412, 472000);   /* R 0.01 .. 150 mm/h */
        lam_l2 = 133431 - ((lr * 215) >> 10) - knob(s, P_SIZE) * 2048 - s->life_s;
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
        while (--k) {                                        /* (one draw per statement: C leaves the order of */
            unsigned at = tx_rand_below(r, n);               /* two draws in one expression open, and wasm and */
            s->ck[at] += (tx_rand(r) & 1u) ? a : -a;         /* native builds then differ) */
        }
        return;
    }
    {                                                        /* large, very large */
        int vl = d_q12 >= 16794;
        int32_t a = ((vl ? CLICK_L * 2 : CLICK_L) * c) >> 15, a_l2, l;
        s->ck[off] += (tx_rand(r) & 1u) ? a : -a;
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
    int32_t r_l2 = near ? dist_l2(s) : tx_place_draw(r, dist_l2(s), depth_l2(s));   /* (a press, or a lane's note: at the place) */
    int32_t g = (int32_t)(((int64_t)tx_place_gain_q12(r_l2) * trim) >> 12), lu, a_l2, rel, amp, sp, c;
    uint32_t wait, u_q8, p_bub;
    int32_t hi = clampi(s->i_eff - (64 << 8), 0, 63 << 8);  /* above the middle: a trickle, then a stream */
    /* the fall: U / U_ref log-normal, sd ~0.6 octave (the references' level spread 6-11 dB), velocity for a press */
    lu = near ? ((vel - 100) * 1024) + (int32_t)(((int64_t)gauss(s) * 13107) >> 12) : (int32_t)(((int64_t)gauss(s) * 39322) >> 12);
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
           + (int32_t)(((int64_t)gauss(s) * sp) >> 12) + hi;   /* a stream: bigger bubbles too (lower) */
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
#define RANGE_L2 27213

static int32_t stone_energy(src_t *s)
{
    uint32_t u = 0xFFFFFFFFu - (uint32_t)(((uint64_t)tx_rand(&s->rng) * s->pm) >> 32);
    int32_t x = (int32_t)(((int64_t)tx_neglog2_q12(u) * 8192) >> 12);   /* alpha 1.5: 1 / (alpha - 1) = 2 */
    return x > RANGE_L2 ? RANGE_L2 : x;
}

static void child(src_t *s, uint32_t t, int32_t lv, int32_t size_l2, unsigned pre, int gen);

/* a knock: speed lv (log2 m/s Q16), piece length size_l2 (log2 m Q16), at sample offset off; may set off others */
static void knock(src_t *s, unsigned off, int32_t lv, int32_t size_l2, unsigned pre, int32_t trim, int gen)
{
    tx_rng_t *r = &s->rng;
    int32_t r_l2 = gen < 0 ? dist_l2(s) - 65536 : tx_place_draw(r, dist_l2(s), depth_l2(s));
    int32_t g = (int32_t)(((int64_t)tx_place_gain_q12(r_l2) * trim) >> 12), a, f_l2;
    int32_t q = (int32_t)tx_exp2(tx_place_log2(2880u) + knob(s, P_TONE) * 2048);   /* Q x16: granite 180, x/ 4 */
    uint32_t tc, lvl;
    int32_t ev;
    int k;
    unsigned i;
    voice_t *v;
    tx_contact_hit(&s->mallet, lv, &tc, &ev);
    a = (int32_t)(((int64_t)A_HIT * (int32_t)tx_exp2(lv - 65536 + (15 << 16))) >> 15);
    a = (int32_t)(((int64_t)a * s->level) >> 15);
    a = (int32_t)(((int64_t)a * g) >> 12);
    if (a < (1 << 14)) return;
    if (gen >= 0 && gen < 12 && s->i_eff > (64 << 8)) {      /* the avalanche: a child, branching up to 0.85 */
        uint32_t b = ((uint32_t)(s->i_eff - (64 << 8)) * 7u) >> 1;   /* Q16 of 1: 0 .. 0.85 at 127 */
        if ((tx_rand(r) >> 16) < b) {
            uint32_t u = (tx_rand(r) >> 16) | 1u;            /* Omori p 1.5, c 30 ms: t = c (u^-2 - 1) */
            uint32_t x = tx_neglog2_q12(u << 16) << 5;       /* -log2 u, Q17 = 2 x Q16 */
            uint32_t dt = x > (5u << 16) ? 132300u : (uint32_t)(((uint64_t)1323u * (tx_exp2((int32_t)x + (16 << 16)) - 65536u)) >> 16);
            int32_t dl = (int32_t)(tx_rand(r) >> 17), ds = (int32_t)(tx_rand(r) >> 18) - 8192;   /* (one draw each) */
            child(s, s->now + off + 441u + dt, lv - dl, size_l2 + ds, pre, gen + 1);
        }
    }
    lvl = (uint32_t)a;
    if (s->events >= EVMAX || (k = take(s, lvl)) < 0) {      /* no hero: the attack residue only */
        s->res += (int32_t)(((int64_t)(a >> 4) * tx_contact_weight(tc, 6000u)) >> 15);
        if (s->res > (1 << 28)) s->res = 1 << 28;
        return;
    }
    s->events++;
    v = &s->v[k];
    v->kind = V_STONE; v->pre = (uint8_t)pre; v->last = 0;
    f_l2 = tx_log2_q16(4300u) - 194372 - size_l2;            /* f1 = 0.128 c / L, granite c 4.3 km/s */
    for (i = 0; i < NMO; i++) {
        uint32_t f;
        int32_t gm;
        if (i) f_l2 += 13215 + (int32_t)tx_rand_below(r, 47480u);   /* x 1.15 .. 1.9 */
        f = tx_exp2(f_l2);
        v->u.s.m[i].amp = 0;
        if (f < 60u || f > 20000u) continue;
        v->u.s.m[i].inc = f * 97391u;
        v->u.s.m[i].m16 = tx_mode_decay(f, (uint32_t)q, 179u);
        v->u.s.m[i].ph = 0;
        gm = i ? 16384 + (int32_t)tx_rand_below(r, 16383u) : 32767;
        gm = (int32_t)(((int64_t)gm * tx_contact_weight(tc, f)) >> 15);
        gm = (int32_t)(((int64_t)gm * tx_place_air_q15(f, r_l2)) >> 15);
        tx_mode_strike(&v->u.s.m[i], (int32_t)(((int64_t)a * gm) >> 15));
    }
    s->res += (int32_t)(((int64_t)(a >> 5) * tx_contact_weight(tc, 6000u)) >> 15);
    if (s->res > (1 << 28)) s->res = 1 << 28;
}

static void child(src_t *s, uint32_t t, int32_t lv, int32_t size_l2, unsigned pre, int gen)
{
    child_t *c;
    if (s->nch >= NCH) return;
    c = &s->ch[s->nch++];
    c->t = t; c->lv = lv; c->size_l2 = size_l2; c->pre = (uint8_t)pre; c->gen = (uint8_t)gen;
}

/* a new knock of the cloud: size around the preset's (SIZE, the key, the slide's bigger stones), power-law energy */
static void stone(src_t *s, unsigned off, int near, int32_t vel, unsigned pre, int32_t trim)
{
    int32_t sp = (40 * 774 * (int32_t)s->p[P_SPREAD]) / 64;   /* +-SD octaves, Q16 */
    int32_t size_l2 = -189876 + knob(s, P_SIZE) * 2048 - ((int32_t)s->note - 60) * 5461 + s->life_s
                      + (int32_t)(((int64_t)gauss(s) * sp) >> 12) + clampi(s->i_eff - (64 << 8), 0, 63 << 8) * 2;
    int32_t lv = 65536 + (vel - 100) * 1638 + (((stone_energy(s) - RANGE_L2) * 16) >> 1)
                 + clampi(s->i_eff - (64 << 8), 0, 63 << 8) * 2;
    knock(s, off, lv, size_l2, pre, trim, near ? -1 : 0);
}

/* ---- cicadas */
#define RIB_AMP 3000
#define CBED_SH 16                  /* the callers' bed: a voiced cicada's mean power ~ (g x 0.08)^2 (calibrated by render) */
#define G_CALL 5000                 /* a caller's gain at 1 m, Q12 (as the cicadas engine's G_REF) */

static void caller_tune(src_t *s, caller_t *c)
{
    int32_t a2, b0, a1e, sa1, sa2, sb0, qv = clampi(45 + knob(s, P_TONE), 0, 127);
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
}

/* a member drawn afresh: a new cicada (carrier CV 4 % x SPREAD, rate CV 5 %, its own distance and phrase) */
static void member_new(src_t *s, member_t *m, int near)
{
    int32_t sp = (int32_t)s->p[P_SPREAD];
    m->f_l2 = 788474 + knob(s, P_SIZE) * -1024 - ((int32_t)s->note - 60) * -2731 + s->life_s / 4
              + (int32_t)((((int64_t)gauss(s) * 3718 * sp) >> 6) >> 12) + (tx_vary_tone(&s->vy) >> 3);
    m->rate_l2 = 449072 + (int32_t)(((int64_t)gauss(s) * 4648) >> 12);
    m->r_l2 = near ? dist_l2(s) - 65536 : tx_place_draw(&s->rng, dist_l2(s), depth_l2(s));
    m->pph = tx_rand(&s->rng);
    m->pinc = (uint32_t)(((uint64_t)70u * 1963000u * (60000u + tx_rand_below(&s->rng, 11000u))) >> 16);
    m->env = 0;
    m->fresh = 1;
}

static void member_gain(src_t *s, member_t *m)
{
    uint32_t f = tx_exp2(m->f_l2);
    int32_t g = (int32_t)(((int64_t)tx_place_gain_q12(m->r_l2) * G_CALL) >> 12);
    g = (int32_t)(((int64_t)g * tx_place_air_q15(f > 8000u ? 8000u : f, m->r_l2)) >> 15);
    g = (int32_t)(((int64_t)g * s->trim_c) >> 12);
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
    if (c->f > 8000u) c->f = 8000u;
    if (c->f < 500u) c->f = 500u;
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
        c->ty.g = g > 6000 ? 6000 : g;
    }
    for (q = 0; q < mcount; q++) {
        int32_t d = (c->pend[q].amp * c->ty.g) >> 4;
        s->buf[c->pend[q].off] += d;
        if (c->pend[q].off + 1u < n) s->buf[c->pend[q].off + 1u] -= d;
    }
    tx_tymbal_add(&c->ty, c->pend, mcount, s->buf, n);
    for (j = mcount; j < c->npend; j++) { c->pend[j - mcount] = c->pend[j]; c->pend[j - mcount].off = (uint16_t)(c->pend[j].off - n); }
    c->npend = (uint8_t)(c->npend - mcount);
    v->last = s->buf[n - 1];
}

/* the population per block: how many call (INTENSITY), who swells in or fades, who has a hero, the bed, the sync */
static void callers_block(src_t *s, uint32_t want_q8)
{
    unsigned i, want = (want_q8 + 128u) >> 8, calling = 0;
    int32_t sx = 0, sy = 0, kpull;
    uint64_t bed = 0;
    int32_t bf = 0;
    uint32_t bw = 0;
    if (want > NM) want = NM;
    /* members 0 .. want-1 call; the others fall silent (a member that stops is a new cicada when it calls again) */
    for (i = 0; i < NM; i++) {
        member_t *m = &s->m[i];
        int on = i < want;
        if (on && !m->calling) { if (!m->fresh) member_new(s, m, 0); m->fresh = 0; m->calling = 1; }
        if (!on && m->calling) m->calling = 0;
        if (m->calling) m->env += (32767 - m->env) >> 7;     /* the swell: ~0.75 s */
        else if (m->env) { m->env -= (m->env >> 6) + 1; if (m->env < 0) m->env = 0; }
        if (!m->calling && !m->env && m->voice == 0xFF) m->fresh = 0;
        if (m->calling || m->env) {
            member_gain(s, m);
            sx += tx_sin_q15(m->pph) >> 5; sy += tx_sin_q15(m->pph + 0x40000000u) >> 5;
            calling++;
        }
    }
    /* falling into step: the pull rises above a third of INTENSITY (0 .. ~0.6 of the phrase rate per block) */
    kpull = clampi(s->i_eff - (42 << 8), 0, 85 << 8) / 85;   /* Q8: 0 .. 256 */
    for (i = 0; i < NM; i++) {
        member_t *m = &s->m[i];
        int32_t sn, cs, t;
        if (!(m->calling || m->env)) continue;
        sn = tx_sin_q15(m->pph); cs = tx_sin_q15(m->pph + 0x40000000u);
        t = calling > 1u ? (int32_t)((((int64_t)sx * cs - (int64_t)sy * sn) >> 10)) / (int32_t)calling : 0;   /* Q15 */
        t = clampi(t, -32767, 32767);
        m->pph += m->pinc + (uint32_t)(int32_t)(((int64_t)(int32_t)(m->pinc >> 2) * t * kpull) >> 23);
        sn = tx_sin_q15(m->pph);
        m->pmod = 18022 + (int32_t)(((int64_t)sn * sn * 14745) >> 30);
        /* a hero: the swelling member takes one (stealing the quietest), the others wait for a free one */
        if (m->voice == 0xFF && m->env > 0 && m->calling) caller_start(s, i, m->env < 2048);
        if (m->voice == 0xFF && m->env > 0) {               /* in the bed */
            uint32_t g = (uint32_t)m->gain, e = (uint32_t)m->env;
            bed += ((uint64_t)g * g * ((e * e) >> 15)) >> CBED_SH;   /* Q30 FS^2 */
            bf += m->f_l2 >> 4;
            bw++;
        }
    }
    /* the bed's power: a voiced cicada's mean power is ~ (g x 0.08)^2 of FS^2 (calibrated by render, ours) */
    {
        uint32_t pp = bed > 0x3FFFFFFFu ? 0x3FFFFFFFu : (uint32_t)bed;
        pp = (pp >> 3) * 5u;                                 /* -2 dB: matched by render (1 vs 16 heroes) */
        s->cbed_p += (uint32_t)(((int32_t)pp - (int32_t)s->cbed_p) >> 3);
        if (bw) {
            int32_t fl = (bf / (int32_t)bw) << 4;
            uint32_t f = tx_exp2(fl);
            f = f > 8000u ? 8000u : f < 500u ? 500u : f;
            if ((int32_t)f != s->cbed_f) { s->cbed_f = (int32_t)f; tx_ireso_set(&s->cres, f, 64u); }
        }
        {   /* the bed's phrase: the mean phase's pulse (in step = deep, out of step = smooth) */
            int32_t rr = calling ? (int32_t)(isqrt((uint32_t)(((int64_t)sx * sx + (int64_t)sy * sy) >> 4)) * 128u / calling) : 0;
            s->cbed_mod = clampi(rr, 0, 32767);
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
    tx_bird_song(&s->v[k].u.bv, type, b->seed, tx_rand_below(&s->rng, b->songs), tx_rand(&s->rng), b->f_off + key_l2,
                 g);
    b->voice = (uint8_t)k;
    s->events++;
}

static void birds_block(src_t *s, unsigned n)
{
    unsigned type = (unsigned)PRE[s->p[P_PRESET]].act_lo, i, want, singing = 0, allowed;
    int32_t a = act_idx(s);
    want = (tx_exp2(((a >> 4) * 1828) / 16 + (8 << 16)) + 128u) >> 8;   /* 1 .. 12 birds (log2 12 = 3.585: x 18284 / 127 / 256) */
    if (want > 12u) want = 12u;
    if (want < 1u) want = 1u;
    allowed = 1u + (unsigned)(clampi(s->i_eff - (40 << 8), 0, 87 << 8) / (14 << 8));   /* turns .. overlap (7 at the top) */
    for (i = 0; i < NB; i++) if (s->bm[i].voice != 0xFF && s->bm[i].active) singing++;
    for (i = 0; i < NB; i++) {
        birdm_t *b = &s->bm[i];
        if (i < want && !b->active && s->run > 4096) { bird_draw(s, b, 0); b->active = 1; b->wait = tx_rand_below(&s->rng, 65535u) * 2u; }
        if (!b->active || b->voice != 0xFF) continue;
        if (i >= want || s->run < 4096) { b->active = 0; continue; }   /* (a bird leaves after its song) */
        if (b->wait > n) { b->wait -= n; continue; }
        if (singing < allowed) { bird_sing(s, i, type, 0, 0); if (b->voice != 0xFF) { singing++; continue; } }
        b->wait = 13230u + tx_rand_below(&s->rng, 39690u);   /* the other one sings: wait 0.3 .. 1.2 s (countersinging) */
    }
}

/* ---- stage 1: the event clock (events presets) */
static uint32_t gap_q8(src_t *s, uint32_t rate_q8)
{
    tx_rng_t *r = &s->rng;
    int32_t reg = clampi(PRE[s->p[P_PRESET]].reg + knob(s, P_SPACING), -64, 63);
    uint32_t mean = (TX_FS << 16) / (rate_q8 ? rate_q8 : 1u);   /* samples, Q8 (fs 2^16 < 2^32) */
    uint32_t e = (uint32_t)(((uint64_t)mean * (tx_neglog2_q12(tx_rand(r)) * 2839u >> 12)) >> 12);   /* exponential */
    if (reg > 0) {                                           /* toward even: mean (1 - j) + j Exp */
        uint32_t j = (uint32_t)(64 - reg) * 1024u;           /* Q16 */
        return (uint32_t)(((uint64_t)mean * (65536u - j) + (uint64_t)e * j) >> 16);
    }
    if (reg < 0 && (tx_rand(r) >> 16) < (uint32_t)(-reg) * 900u) return e >> 4;   /* clumped: a burst */
    return reg < 0 ? (uint32_t)(((uint64_t)e * (65536u + (uint32_t)(-reg) * 900u)) >> 16) : e;
}

static void event(src_t *s, unsigned off, unsigned n, unsigned pre, int near, int32_t vel, uint32_t w_q8)
{
    int32_t trim = pre_trim(s, pre);
    if (pre == PR_RAIN) drop(s, off, n, near, vel, pre, trim, w_q8);
    else if (pre == PR_WATER) wdrop(s, off, n, near, vel, pre, trim);
    else if (pre == PR_STONES) stone(s, off, near, vel, pre, trim);
}

/* ---- hosted presets */
static int32_t curve_at(const curve_t *c, int32_t i_q8)     /* the curve's value at intensity i (Q8 knob units) */
{
    int32_t a = c->v0, b = c->v64, x = i_q8;
    if (x >= (64 << 8)) { a = c->v64; b = c->v127; x -= 64 << 8; return a + ((b - a) * x + (63 << 7)) / (63 << 8); }
    return a + ((b - a) * x + (64 << 7)) / (64 << 8);
}

static const int8_t HOST_KEYS[4] = { 0, -3, 4, -7 };      /* the extra keys, semitones from the played one */
/* an event engine's next gap (blocks): exponential around the mean at the intensity now (log between e0, e64, e127) */
static uint32_t host_gap(src_t *s)
{
    const host_t *h = s->hd;
    int32_t a = act_idx(s), l0 = tx_place_log2(h->e0), l1 = tx_place_log2(h->e64), l2 = tx_place_log2(h->e127), l, m;
    l = a < (64 << 8) ? l0 + (((l1 - l0) * (a >> 6)) >> 8) : l1 + (((l2 - l1) * (((a - (64 << 8)) << 8) / (63 << 8))) >> 8);
    m = (int32_t)tx_exp2(l + 265519);                        /* x 17.23 blocks per 0.1 s (log2 17.23 = 4.106) */
    m = (int32_t)(((int64_t)m * (int32_t)((tx_neglog2_q12(tx_rand(&s->rng)) * 2839u) >> 12)) >> 12);   /* x Exp(1) */
    return (uint32_t)(m < 26 ? 26 : m);
}

static void host_apply(src_t *s, int force)               /* the curves at the intensity now (only changed values sent) */
{
    unsigned i;
    if (s->hd->e64) {                                        /* an event engine: pressed at its rate while it runs */
        if (s->hoff && !--s->hoff) s->hst->key(s->host, s->hpn, 0, 0);
        if (s->held && !s->hoff) {
            if (!s->hnext) s->hnext = host_gap(s);
            if (!--s->hnext) {
                s->hpn = s->note;
                if (s->hd->oneshot) { s->hst->key(s->host, s->hpn, s->vel, 1); s->hoff = 26; }
                else { s->hst->key(s->host, s->hpn, 0, 0); s->hst->key(s->host, s->hpn, s->vel, 1); }
                s->hnext = host_gap(s);
            }
        }
    }
    if (s->hd->k127) {                                       /* the keys held: more singers */
        curve_t kc = { 0, s->hd->k0, s->hd->k64, s->hd->k127 };
        unsigned want = s->held ? (unsigned)clampi(curve_at(&kc, act_idx(s)), 1, 4) : 0u;
        if (s->hkeys <= 1) s->hbase = s->note;               /* (the extras keep the note they were pressed from) */
        while (s->hkeys && s->hkeys < want) { s->hst->key(s->host, (uint8_t)(s->hbase + HOST_KEYS[s->hkeys]), s->vel, 1); s->hkeys++; }
        while (s->hkeys > want && s->hkeys > 1) { s->hkeys--; s->hst->key(s->host, (uint8_t)(s->hbase + HOST_KEYS[s->hkeys]), 0, 0); }
        if (!want) s->hkeys = 0;
        if (s->hkeys && !s->hrenew--) {                      /* renew one key: a singer leaves, a new one comes */
            unsigned k = s->hcycle++ % s->hkeys;
            uint8_t nt = (uint8_t)(k ? s->hbase + HOST_KEYS[k] : s->note);
            s->hst->key(s->host, nt, 0, 0);
            s->hst->key(s->host, nt, s->vel, 1);
            s->hrenew = 1033u + tx_rand_below(&s->rng, 1723u);   /* 6 .. 16 s in blocks */
        }
    }
    for (i = 0; i < HOST_NC; i++) {
        int32_t v;
        if (!s->hd->c[i].name || s->hid[i] < 0) continue;
        v = clampi(curve_at(&s->hd->c[i], i ? s->i_eff : act_idx(s)), 0, 127);
        if (force || v != s->hval[i]) { s->hval[i] = (uint8_t)v; s->hst->param(s->host, (uint8_t)s->hid[i], (uint8_t)v); }
    }
}

/* the preset changed: start its hosted engine (or bring the particle pools back); the old sound's last sample decays */
static void host_switch(src_t *s, unsigned pre)
{
    const host_t *hd = PRE[pre].kind == 2 ? &HOST[PRE[pre].act_lo] : 0;
    const tx_engine_t *ne = hd ? tx_engine_find(hd->eng) : 0;
    const void *want = hd ? (const void *)hd : PRE[pre].kind >= 3 ? (const void *)&PRE[pre] : 0;
    unsigned i;
    if (want == s->hcur) return;
    s->hcur = want;
    s->wst = 0; s->wheld = 0;
    tx_declick_add(&s->dc, s->last_out);
    s->last_out = 0;
    { uint8_t *b = (uint8_t *)s->host; for (i = 0; i < sizeof s->host; i++) b[i] = 0; }
    s->hst = ne; s->hd = hd;
    /* the particle presets' beds and filters start from silence (their last output went to the declick) */
    s->bed1_p = s->bed2_p = s->acc1 = s->acc2 = s->cbed_p = 0;
    s->b1x1 = s->b1x2 = s->b2x1 = s->b2lp = s->res = s->res_x1 = s->clp = s->cbed_f = s->cbed_mod = 0;
    { uint8_t *b = (uint8_t *)&s->cres; for (i = 0; i < sizeof s->cres; i++) b[i] = 0; }
    s->hp.x1 = s->hp.y1 = 0; s->vy.lp = 0; s->run = 0; s->next_q8 = 0; s->hgain = 0;
    for (i = 0; i < TX_BLOCK; i++) s->buf[i] = s->ck[i] = 0;
    if (ne && ne->state_bytes <= HOST_BYTES) {
        ne->init(s->host, tx_rand(&s->rng));
        for (i = 0; i < HOST_NC; i++) s->hid[i] = (int8_t)(hd->c[i].name ? tx_param_find(ne, hd->c[i].name) : -1);
        if (s->lvl_v) ne->param(s->host, TX_PID_LEVEL, s->lvl_v);
        if (s->part_v) ne->param(s->host, TX_PID_PARTICLES, s->part_v);
        host_apply(s, 1);
        s->hkeys = 0; s->hrenew = 258; s->hnext = 0; s->hoff = 0;                       /* (the first renewal after 1.5 s: past the fader's glide) */
        if (s->held) { ne->key(s->host, s->note, s->vel, 1); s->hkeys = 1; }
        s->quiet = 0;
    } else {                                                 /* the particle presets: fresh pools */
        s->hst = 0; s->hd = 0;
        tx_pool_init(&s->pool, NV, s->limit);
        for (i = 0; i < NM; i++) s->m[i].voice = 0xFF;
        s->nch = 0;
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
    s->i_glide += ((int32_t)s->p[P_INTENS] * 256 - s->i_glide) >> 6;   /* a fader move glides in (~1 s to 95 %) */
    s->i_eff = clampi(s->i_glide + (((a * depth) >> 7) * 5 >> 4), 0, 127 << 8);   /* LIFE: +-40 at 127 */
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
    for (i = 0; i < NM; i++) s->m[i].voice = 0xFF;
    for (i = 0; i < NP; i++) s->p[i] = PARAMS[i].def;
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

static void src_key(void *st, uint8_t note, uint8_t vel, uint8_t on)
{
    src_t *s = (src_t *)st;
    if (s->hst) {                                            /* a hosted engine: its own keys */
        if (on && vel) { s->held++; s->note = note; s->vel = vel; s->quiet = 0; if (!s->hkeys) s->hkeys = 1; }
        else if (s->held) s->held--;
        s->hst->key(s->host, note, vel, on);
        return;
    }
    if (on && vel) {
        unsigned pre = s->p[P_PRESET];
        tx_vary_press(&s->vy);
        s->held++;
        s->note = note; s->vel = vel;
        if (PRE[pre].kind == 4) {                            /* a bird sings now, at the key's pitch: the one close by, */
            unsigned bi = 0, q;                              /* or (it is singing) another one that is free */
            birdm_t *b;
            if (s->bm[0].voice != 0xFF) for (q = 1; q < NB; q++) if (s->bm[q].active && s->bm[q].voice == 0xFF) { bi = q; break; }
            b = &s->bm[bi];
            if (b->voice != 0xFF) { unsigned vi = b->voice; voice_cut(s, vi); tx_pool_free(&s->pool, vi); b->voice = 0xFF; }
            if (!bi) bird_draw(s, b, 1);
            b->active = 1;
            bird_sing(s, bi, (unsigned)PRE[pre].act_lo, 1, ((int32_t)note - 60) * 5461);
        } else if (PRE[pre].kind == 1) {                     /* a cicada close by, at the key's pitch */
            member_t *m = &s->m[0];
            member_new(s, m, 1);
            if (m->voice != 0xFF) { unsigned vi = m->voice; voice_cut(s, vi); tx_pool_free(&s->pool, vi); m->voice = 0xFF; }
            m->calling = 0; m->fresh = 1;
        } else {
            s->events = 0;
            event(s, 0, TX_BLOCK, pre, 1, vel, 256u);
        }
        s->quiet = 0;
    } else if (s->held) s->held--;
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
    if (s->hst && (id == TX_PID_PARTICLES || id == TX_PID_LEVEL)) s->hst->param(s->host, id, val);
    if (s->wst && id == TX_PID_LEVEL) s->wst->param((uint8_t *)s->host + WATER_OFF, id, val);
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
        if (id == P_PRESET) host_switch(s, s->p[id]);
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
    /* the source on / off: in ~0.2 s, out over ~1.5 s */
    if (s->held) s->run += (32767 - s->run) >> 3;
    else s->run -= (s->run >> 7) + 1;
    if (s->run < 0) s->run = 0;
    life_block(s);
    /* stage 1: events (rain, stones; mixed by MORPH) */
    if (s->run) {
        int ea = PRE[pa].kind == 0 || PRE[pa].kind == 3, eb = PRE[pb].kind == 0 && w;
        uint32_t rate = 0;
        if (ea && eb) {
            int32_t la = tx_place_log2(pre_amount(s, pa)), lb = tx_place_log2(pre_amount(s, pb));
            rate = tx_exp2(la + (int32_t)(((int64_t)(lb - la) * w) >> 15));
        } else if (ea) rate = (uint32_t)(((uint64_t)pre_amount(s, pa) * (uint32_t)(32768 - w)) >> 15);
        else if (eb) rate = (uint32_t)(((uint64_t)pre_amount(s, pb) * (uint32_t)w) >> 15);
        rate = (uint32_t)(((uint64_t)rate * (uint32_t)s->run) >> 15);
        if (rate) {
            unsigned guard = 0;
            uint32_t w_q8 = 256u;
            if (rate > (RATE_CAP << 8)) { w_q8 = rate / RATE_CAP; rate = RATE_CAP << 8; }   /* over the cap: weighted */
            uint32_t mean = (TX_FS << 16) / rate;
            if (s->next_q8 / 4u > mean) s->next_q8 = gap_q8(s, rate);   /* a much faster rate starts now */
            while ((s->next_q8 >> 8) < n && guard++ < 112u) {
                unsigned off = s->next_q8 >> 8, pre = pa;
                if (eb && (!ea || (tx_rand(r) >> 17) < (uint32_t)w)) pre = pb;
                event(s, off, n, pre, 0, 100, pre == PR_RAIN ? w_q8 : 256u);
                s->total++;
                s->next_q8 += gap_q8(s, rate);
            }
            s->next_q8 = s->next_q8 > (n << 8) ? s->next_q8 - (n << 8) : 0;
        }
    }
    /* avalanche children due in this block */
    for (i = 0; i < s->nch; ) {
        int32_t dt = (int32_t)(s->ch[i].t - s->now);
        if (dt < (int32_t)n) {
            child_t c = s->ch[i];
            s->ch[i] = s->ch[--s->nch];
            knock(s, dt < 0 ? 0u : (unsigned)dt, c.lv, c.size_l2, c.pre, pre_trim(s, c.pre), c.gen);
        } else i++;
    }
    /* callers (cicadas): their number, weighted by the morph */
    {
        uint32_t want = 0;
        if (PRE[pa].kind == 1) want += (uint32_t)(((uint64_t)pre_amount(s, pa) * (uint32_t)(32768 - w)) >> 15);
        if (PRE[pb].kind == 1 && w) want += (uint32_t)(((uint64_t)pre_amount(s, pb) * (uint32_t)w) >> 15);
        want = (uint32_t)(((uint64_t)want * (uint32_t)s->run) >> 15);
        if (s->held && want < 256u && (PRE[pa].kind == 1 || (PRE[pb].kind == 1 && w))) want = 256u;   /* a press: at least one */
        if (s->run < 4096 && !s->held) want = 0;
        s->trim_c = pre_trim(s, PR_CICADAS);
        if (PRE[pa].kind == 4) birds_block(s, n);            /* birds (their members share that memory) */
        else if (PRE[pa].kind != 3) callers_block(s, want);  /* (water: the members' memory holds the stream) */
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
            if (!m->env && !m->calling && tx_tymbal_quiet(&v->u.c.ty) && !v->u.c.npend) { m->voice = 0xFF; v->kind = V_NONE; }
            else if (!m->env && !m->calling) v->u.c.npend = 0;
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
    /* the rain beds: power from the dropped bubbles, smoothed over ~8 blocks */
    {
        uint32_t p1 = s->acc1 / n, p2 = s->acc2 / n;     /* Q18 per sample -> Q30 */
        uint32_t x32 = s->x32;
        int32_t sd1, sd2;
        p1 = p1 > (1u << 18) ? 0x3FFFFFFFu : p1 << 12;
        p2 = p2 > (1u << 18) ? 0x3FFFFFFFu : p2 << 12;
        s->acc1 = s->acc2 = 0;
        s->bed1_p += (uint32_t)(((int32_t)(p1 > 0x3FFFFFFFu ? 0x3FFFFFFFu : p1) - (int32_t)s->bed1_p) >> 3);
        s->bed2_p += (uint32_t)(((int32_t)(p2 > 0x3FFFFFFFu ? 0x3FFFFFFFu : p2) - (int32_t)s->bed2_p) >> 3);
        if (s->bed1_p < 4u) s->bed1_p = 0;
        if (s->bed2_p < 4u) s->bed2_p = 0;
        sd1 = (int32_t)isqrt(s->bed1_p); sd2 = (int32_t)isqrt(s->bed2_p);   /* Q15 */
        if (sd1 || s->b1x1 || s->b1x2) {                     /* type I: 13-20 kHz, a second difference (power kept) */
            int32_t x1 = s->b1x1, x2 = s->b1x2;
            for (j = 0; j < n; j++) {
                int32_t x;
                x32 ^= x32 << 13; x32 ^= x32 >> 17; x32 ^= x32 << 5;
                x = (((int32_t)(x32 >> 17) - 16384) * (sd1 > 32767 ? 32767 : sd1)) >> 13;   /* sd ~ sd1 x 2 / sqrt 3 */
                s->buf[j] += ((x - 2 * x1 + x2) * 89);       /* / sqrt 6 x 2^8 x 0.87 */
                x2 = x1; x1 = x;
            }
            if (!sd1) x1 = x2 = 0;
            s->b1x1 = x1; s->b1x2 = x2;
        }
        if (sd2 || s->b2x1 || s->b2lp) {                     /* type II: 3-10 kHz, a first difference, a one-pole */
            int32_t x1 = s->b2x1, lp = s->b2lp;
            for (j = 0; j < n; j++) {
                int32_t x;
                x32 ^= x32 << 13; x32 ^= x32 >> 17; x32 ^= x32 << 5;
                x = (((int32_t)(x32 >> 17) - 16384) * (sd2 > 32767 ? 32767 : sd2)) >> 13;
                lp += ((x - x1) - lp) >> 1;
                s->buf[j] += lp * 256;
                x1 = x;
            }
            if (!sd2) { x1 = 0; if (lp > -4 && lp < 4) lp = 0; }
            s->b2x1 = x1; s->b2lp = lp;
        }
        s->x32 = x32 ? x32 : 1u;
    }
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
    /* the callers' bed: noise through a two-pole at their mean carrier, pulsed by their phrase */
    if (s->cbed_p || !tx_ireso_quiet(&s->cres)) {
        int32_t sd = (int32_t)isqrt(s->cbed_p);               /* Q15 */
        int16_t imp[TX_BLOCK / 4];
        uint16_t off[TX_BLOCK / 4];
        unsigned k = 0;
        sd = (int32_t)(((int64_t)sd * (32768 - (s->cbed_mod >> 1))) >> 15);
        if (sd > 32767) sd = 32767;
        for (j = 0; j < n && sd; j += 4) {                   /* sparse impulses every 4 samples (cheap) */
            uint32_t x32 = s->x32;
            x32 ^= x32 << 13; x32 ^= x32 >> 17; x32 ^= x32 << 5;
            s->x32 = x32;
            off[k] = (uint16_t)j;
            imp[k++] = (int16_t)(((((int32_t)(x32 >> 17) - 16384) * sd) >> 14));
        }
        s->cres.g = 256;
        tx_ireso_add(&s->cres, off, imp, k, s->buf, n);
        if (!sd && tx_ireso_quiet(&s->cres)) s->cbed_f = 0;
    }
    tx_declick_run(&s->dc, s->buf, n);
    tx_vary_block(&s->vy, s->buf, n);
    tx_trim(s->buf, n, OUT_TRIM);
    tx_dcblock_add(&s->hp, s->buf, mix, n);
    s->last_out = s->hp.y1;                                  /* (the dcblock's last output: for a switch's declick) */
    for (j = 0; j < TX_BLOCK; j++) s->buf[j] = s->ck[j] = 0;
    if (s->wst) any |= water_stream(s, mix, n);              /* water: the stream under the drops */
    s->now += n;
    {
        int32_t z = any | s->run | s->clp | s->b1x1 | s->b1x2 | s->b2x1 | s->b2lp | (int32_t)s->bed1_p | (int32_t)s->bed2_p
                    | s->res | (int32_t)s->cbed_p | !tx_ireso_quiet(&s->cres) | s->dc.v | s->hp.x1 | s->hp.y1 | s->nch
                    | s->vy.lp | s->held;
        if (PRE[pa].kind != 3 && PRE[pa].kind != 4) for (i = 0; i < NM; i++) z |= s->m[i].env;
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
