/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura MIX core: stage S3, four layers. Up to TX_MIX_SLOTS engine instances (each in its own state arena, the
 * caller's), each panned (constant power, unity at the centre: gains sqrt(2) sin / cos), summed into a stereo pair;
 * the bus compressor (tx_core_comp.h: comp, the first effect, as Frodhes; COMP off: bypassed); the pair's DRIVE and
 * FILTER (pre: tx_fx.h's; AMOUNT 0, the filter open: bypassed); the CHAMBER (optional, mono) on the pair's mid, its
 * added body and tail fed back to both sides; then the bus effects (the grain delay, then the reverb on what it gives),
 * the M/S (tx_core_ms.h: the mix's middle and sides, the last effect; at 0 dB untouched) and the output limiter, linked across the two channels (tx_engine.h TX_ST_*: the order the FX pages show). Integer, Q23 in int32 like every mix here.
 *
 *   for each block: tx_mix_begin(m, n); tx_mix_slot_render(m, k, n) for each slot (the caller may time each one);
 *                   tx_mix_end(m, L, R, n) -> L, R limited (|x| <= -1 dBFS); m->raw_peak = the pair's peak before it */
#ifndef TX_CORE_MIX_H
#define TX_CORE_MIX_H
#include <stdint.h>
#include "tx_engine.h"
#include "tx_core_chamber.h"
#include "tx_core_limit.h"
#include "tx_core_comp.h"
#include "tx_core_ms.h"

#ifndef TX_MIX_SLOTS
#define TX_MIX_SLOTS 4u                    /* (the FM-1's four lanes; the desktop plugin's build sets 16) */
#endif
#define TX_MIX_GATE_N 512u                 /* the gate's ramp: 11.6 ms at 44.1 kHz, no click */
/* the output mode (tx_mix_phones): SPEAKERS pans by level alone (constant power), HEADPHONES binaurally: the far ear
 * gets the slot late (the interaural time difference of a spherical head, up to 29 samples = 0.66 ms at a hard pan,
 * whole samples at rest, read between two while it glides), through the head's shadow (a one-pole low-pass, 1.8 kHz at a hard pan, as a high shelf:
 * its highs kept at 0.4) and softer (constant power on a narrower arc: the far ear -10 dB at a hard pan). At the
 * centre the two modes are the same, bit for bit (a slot's peak never over 8 full scale) */
#define TX_MIX_SPEAKERS 0u
#define TX_MIX_HEADPHONES 1u
/* HEADPHONES' two settings (tx_mix_phones_amt, tx_mix_phones_head): AMOUNT, 0 .. 100 % of every binaural cue (the far
 * ear's delay, its shadow's depth, the level panning's softening; 0 %: SPEAKERS, bit for bit, and its cost), and the
 * head's size: a spherical head of radius 7.6, 8.8 or 10.0 cm (c = 343 m/s), its largest time difference 25, 29 or 33
 * samples (0.57, 0.66, 0.75 ms: Woodworth's r (1 + pi/2) / c), its shadow's corner 2.08, 1.8 or 1.58 kHz (a head's
 * shadow scales as 1 / r: k r the same); the shelf's depth and the level softening are the same for all three */
#define TX_MIX_HEAD_SMALL 0u
#define TX_MIX_HEAD_MEDIUM 1u              /* (the default: tx_mix_init's, the 29 samples of before) */
#define TX_MIX_HEAD_LARGE 2u
#define TX_MIX_HEAD_N 3u
#define TX_MIX_AMT_DEF 100u                /* AMOUNT's default: every cue in full */
#define TX_BIN_N 36u                       /* a slot's history for the far ear: its last samples, at least the 33 + 1 read
                                            * back (the largest delay, and the sample before it while a glide reads
                                            * between two); a multiple of 4 */
/* HEADPHONES' cost, FM-1 us a slot a block, as the governor's stand-in where nothing is timed (the host, the web: the
 * sources' pool carries it, tx_mix_phones_us, firmware/src/lanes.c: the governor's extra; on the FM-1 each binaural
 * lane's pan is timed instead). Its dearest path, the pans moving every block (each ear gliding): make tx-budget 90.0 a
 * lane (2026-11-24; 133.5 before the pass-per-ear rewrite; held pans 49.6, the level pan 13.1), rounded up with room.
 * Was 45: the held pans' extra alone, under a moving scene's cost by ~3x */
#define TX_MIX_HP_US 100u
#ifndef TX_LINE                    /* (as tx_fx_drv.h's: a buffer one core writes while the other works, its own lines) */
#ifdef __PI32V2__
#define TX_LINE __attribute__((aligned(64)))
#else
#define TX_LINE
#endif
#endif

typedef struct {
    const tx_engine_t *e[TX_MIX_SLOTS];  /* 0 = an empty slot */
    void *st[TX_MIX_SLOTS];              /* each slot's state (TX_STATE_MAX bytes, the caller's) */
    int32_t gl[TX_MIX_SLOTS], gr[TX_MIX_SLOTS];   /* pan gains, Q14 (16384 = 1.0) */
    int32_t tg[TX_MIX_SLOTS];            /* the slot's trim before its pan, Q12 (4096 = 1.0: none; up to +24 dB) */
    int32_t gc[TX_MIX_SLOTS], gt[TX_MIX_SLOTS];   /* the slot's gate (a lane's mute), Q15: now and where it ramps to
                                                   * (32768 open, 0 shut), TX_MIX_GATE_N samples end to end */
    int32_t lc[TX_MIX_SLOTS], lv[TX_MIX_SLOTS];   /* the slot's level (a lane's LEVEL, tx_level_q15: 32768 = 100),
                                                   * now and wanted: the next block ramps to it, sample by sample */
    int32_t slot_peak[TX_MIX_SLOTS];     /* each slot's peak since the caller last cleared it */
    tx_chamber_t *ch;                    /* optional (the caller's, ~22 KB), used while ch_on */
    int ch_on;
    /* optional effects (tx_fx.h; 0 = none): ins on each slot's block before it is panned, pre on the stereo pair
     * after comp (the DRIVE, the FILTER), bus on the stereo pair before the limiter, busq: the bus quiet; fx their state */
    void (*ins)(void *fx, unsigned k, int32_t *x, unsigned n);
    void (*pre)(void *fx, int32_t *L, int32_t *R, unsigned n);
    void (*bus)(void *fx, int32_t *L, int32_t *R, unsigned n);
    int (*busq)(void *fx);
    void (*pre1)(void *fx, int32_t *L, int32_t *R, unsigned n);   /* pre and bus as core 1 runs them (tx_mix_run's c:
                                                                   * the XIP twins, no job handed on; 0 = none) */
    void (*bus1)(void *fx, int32_t *L, int32_t *R, unsigned n);
    void *fx;
    tx_comp_t comp;                      /* the bus compressor, the first effect (COMP off: bypassed) */
    tx_ms_t ms;                          /* the M/S, the last effect: after the bus, before the limiter (0 dB, MONO
                                          * off: bypassed, bit for bit) */
    tx_limit_t lim;
    int32_t raw_peak;                    /* the pair's peak before the limiter, this block */
    int32_t L[TX_BLOCK];
    int32_t R[TX_BLOCK] TX_LINE;         /* (its own lines: the pair's DRIVE may run R on the other core) */
    int32_t blk[TX_BLOCK], mid[TX_BLOCK], ilv[2 * TX_BLOCK];   /* (free while pre runs: TX_MIX_SCR) */
    int32_t hb[TX_BIN_N + TX_BLOCK];     /* HEADPHONES: the slot being added, its history then its block in a row (the
                                          * ears read back from it in one pass each: tx_mix_add_bin) */
    /* HEADPHONES (tx_mix_phones): the mode, and per slot its ears' state (each ear: its delay, Q16 samples; its
     * shadow's low-pass coefficient a and high-shelf gain b, Q15; its gain, Q14; a, b and the gain kept << 8 so a
     * block's ramp lands on its target; now and wanted, ramped over a block, the delay at most 1/16 sample a sample) and
     * its history; run: the binaural path on (HEADPHONES, or back to SPEAKERS until every ear has reached it) */
    unsigned hp;                         /* the binaural path wanted: HEADPHONES with AMOUNT over 0 */
    uint8_t mode, amt, head;             /* tx_mix_phones', tx_mix_phones_amt's (0 .. 100), tx_mix_phones_head's */
    struct {
        struct { int32_t d, dt, a, at, b, bt, g, gt, lp; } e[2];   /* 0 left, 1 right */
        int32_t r[TX_BIN_N];             /* the slot's last TX_BIN_N samples, the oldest first */
        uint8_t pan, run;
    } bin[TX_MIX_SLOTS];
} tx_mix_t;
/* blk, mid, ilv in a row: 4 KB that pre may use as the other core's scratch (they start on a line after R's) */
#define TX_MIX_SCR(m) ((void *)(m)->blk)
#define TX_MIX_SCR_BYTES (4u * TX_BLOCK * sizeof(int32_t))

void tx_mix_init(tx_mix_t *m);
/* put an engine in slot k (initialised here with seed), or empty it (e = 0) */
void tx_mix_set(tx_mix_t *m, unsigned k, const tx_engine_t *e, void *st, uint32_t seed);
void tx_mix_pan(tx_mix_t *m, unsigned k, unsigned pan);   /* 0 left .. 64 centre .. 127 right */
/* the output mode: TX_MIX_SPEAKERS (the default: tx_mix_init's) or TX_MIX_HEADPHONES (binaural); every slot glides to
 * it over a block or two, no click */
void tx_mix_phones(tx_mix_t *m, unsigned mode);
/* HEADPHONES' AMOUNT, 0 .. 100 % (TX_MIX_AMT_DEF at first: every binaural cue scaled; 0: SPEAKERS, bit for bit, and its
 * cost), and the head's size (TX_MIX_HEAD_SMALL .. LARGE; MEDIUM at first). Either glides as tx_mix_phones does */
void tx_mix_phones_amt(tx_mix_t *m, unsigned pct);
void tx_mix_phones_head(tx_mix_t *m, unsigned size);
/* slot k's binaural path's extra cost in its last tx_mix_add, FM-1 us (0 in SPEAKERS at rest: its level pan alone) */
static inline uint32_t tx_mix_phones_us(const tx_mix_t *m, unsigned k)
{
    return k < TX_MIX_SLOTS && (m->hp | m->bin[k].run) ? TX_MIX_HP_US : 0u;
}
/* slot k pans binaurally (HEADPHONES, or gliding back to SPEAKERS until every ear is there): tx_mix_add takes
 * tx_mix_add_bin's path */
static inline int tx_mix_binaural(const tx_mix_t *m, unsigned k) { return k < TX_MIX_SLOTS && (m->hp | m->bin[k].run); }
/* that path, into a pair of the caller's: to->L, to->R added to, to->hb its scratch (TX_BIN_N + TX_BLOCK words); slot
 * k's state as tx_mix_add leaves it (its peak, gate, level and ears). tx_mix_add's own with the mix's pair; a caller
 * may pan a slot on the other core into a pair of its own and add that pair to the mix's (firmware/src/lanes.c
 * LN_PAR: core 1's copy, TX_C1). In RAM on the FM-1 (TX_RAM) */
typedef struct { int32_t *L, *R, *hb; } tx_mix_to_t;
TX_RAM void tx_mix_add_bin(tx_mix_t *m, unsigned k, const int32_t *x, unsigned n, const tx_mix_to_t *to);
TX_TWIN(tx_mix_add_bin);
void tx_mix_trim(tx_mix_t *m, unsigned k, unsigned q12);  /* the slot's level trim, Q12 (a landscape sound's own level) */
/* the slot's level, 0..127 (100: unity; tx_level_q15's square law): on its output, whatever the engine (no voice
 * waits to renew), ramped over the next block so it never clicks */
void tx_mix_level(tx_mix_t *m, unsigned k, unsigned v);
/* the slot's gate: shut (a muted lane: the engine runs on, its output ramps to silence, the slot's peak is still the
 * engine's) or open again; either way a TX_MIX_GATE_N ramp */
void tx_mix_gate(tx_mix_t *m, unsigned k, int open);
TX_AUDIO_CODE void tx_mix_begin(tx_mix_t *m, unsigned n);
TX_AUDIO_CODE void tx_mix_slot_render(tx_mix_t *m, unsigned k, unsigned n);
/* or: the caller rendered slot k's block itself (a scene script, say): pan it and add it */
TX_AUDIO_CODE void tx_mix_add(tx_mix_t *m, unsigned k, const int32_t *x, unsigned n);
TX_AUDIO_CODE void tx_mix_end(tx_mix_t *m, int32_t *L, int32_t *R, unsigned n);
/* tx_mix_end in two: run (the DRIVE, the compressor, the chamber, the bus, the M/S, the limiter: its result kept in ilv) on
 * core c (1: core 1's twins in XIP and pre1 / bus1, as one block behind: firmware/src/lanes.c LN_PIPE), then out (the
 * pair from ilv, any time before the next run) */
TX_AUDIO_CODE void tx_mix_run(tx_mix_t *m, unsigned n, unsigned c);
TX_AUDIO_CODE void tx_mix_out(const tx_mix_t *m, int32_t *L, int32_t *R, unsigned n);
/* all slots quiet (and the chamber idle) */
int tx_mix_quiet(const tx_mix_t *m);

#endif
