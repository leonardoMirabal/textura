/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura effects: the integer helpers and the reverb kernels' shared types (internal to the tx_fx_*.c files).
 * The helpers and the kernels' contract are Frodhes' (fx.c, rev_types.c: our own), taken over unchanged in what they
 * compute. Effects work at Q15 with headroom (Frodhes' scale: 32768 = full scale); tx_fx.c converts from and to the
 * mix's Q23 at its edges, and only the wet signals pass through them (the dry path stays Q23). */
#ifndef TX_FX_KERN_H
#define TX_FX_KERN_H
#include <stdint.h>
#include <stddef.h>
#include "tx_engine.h"
#include "tx_fx_tables.h"
#include "tx_fx_drv.h"                          /* (TX_LINE) */

/* no <string.h> in engines/ (freestanding on the device: its libc's memset / memcpy, a byte at a time). Clears go
 * through tx_zero (words: a reverb TYPE's start clears 14 .. 30 KB in the audio interrupt); c must be 0 */
#define tx_fx_memset(p, c, n) (tx_zero((p), (n)), (void)sizeof(char[1 - 2 * ((c) != 0)]))
#define tx_fx_memcpy __builtin_memcpy
/* n words from s to d (TX_RAM code: libc's memcpy is out of its call's reach) */
TX_INLINE void tx_fx_copy32(int32_t *d, const int32_t *s, unsigned n) { unsigned i; for (i = 0; i < n; i++) d[i] = s[i]; }

#define TX_FX_FS 44100u
#define CTL 32u                     /* a control block: 32 frames (the reverb's step) */
#define HL_NC CTL                   /* the kernels' samples a call: every kernel runs at the full rate (44.1 kHz) */
#define RV_X 2u                     /* the kernels' designs are at the half rate: their lengths x RV_X here */

TX_INLINE int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
TX_INLINE int32_t mulq15(int32_t a, int32_t b) { return (a * b) >> 15; }          /* |a|, |b| small enough */
TX_INLINE int32_t mul15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
TX_INLINE int32_t sh_tz(int32_t m, uint32_t sh) { return (m + (int32_t)((uint32_t)(m >> 31) >> (32u - sh))) >> sh; }
TX_INLINE int32_t hl_c16(int32_t v) { v = v > -32768 ? v : -32768; return v < 32767 ? v : 32767; }
TX_INLINE int32_t tx_fx_softclip(int32_t x)             /* Q15 tanh: x in Q15, flat beyond +-4 */
{
    uint32_t i;
    int32_t f;
    x = clampi(x, -131072, 131071);
    i = (uint32_t)(x + 131072) >> 10;
    f = (x + 131072) & 1023;
    return TX_FX_TANH[i] + (((TX_FX_TANH[i + 1] - TX_FX_TANH[i]) * f) >> 10);
}
TX_INLINE int32_t tx_fx_sine(uint32_t ph)               /* Q32 phase -> +-32767 */
{
    uint32_t i = ph >> 22;
    int32_t f = (int32_t)((ph >> 7) & 0x7FFFu);
    return TX_FX_SINE[i] + (((TX_FX_SINE[i + 1] - TX_FX_SINE[i]) * f) >> 15);
}
/* a design's half-rate one-pole at the full rate (TX_FX_RV_POLE: the closest response): a pole p (y = p y + (1 - p)
 * x), Q15; a weight a (y += a (x - y)) is 1 - its pole */
TX_INLINE int32_t rv_pole(int32_t p)
{
    int32_t i = p >> 9, f = p & 511;
    return i >= 64 ? 32767 : TX_FX_RV_POLE[i] + (((TX_FX_RV_POLE[i + 1] - TX_FX_RV_POLE[i]) * f) >> 9);
}
TX_INLINE int32_t rv_weight(int32_t a) { return 32768 - rv_pole(32768 - a); }
TX_INLINE uint32_t exp2_neg(uint32_t x)                 /* 2^(-x / 65536) in Q15 */
{
    uint32_t i = x >> 16, u = 32768u - ((x & 0xFFFFu) >> 1), p = 2579u;
    p = ((p * u) >> 15) + 7409u;
    p = ((p * u) >> 15) + 22780u;
    p = ((p * u) >> 15) + 32768u;
    return i < 15u ? p >> (i + 1u) : 0u;
}

/* ---- the reverb kernels' contract (Frodhes rev_types.c; there at the half rate, here at the full): HL_NC samples a
 * call ---- */
typedef struct {            /* filled by the framework once per control block */
    int32_t decay;          /* 0..127 DECAY, after the framework's slew */
    int32_t damp;           /* 0..150 (TONE: 150 dark, 60 at its middle, 0 bright) */
    int32_t size;           /* 0..127 */
    int32_t mod;            /* MOD x 16, 0..2032 (64: 1024; the framework glides it 1 a control block) */
    int32_t freeze;         /* the infinite kernel: 0 normal; 1 SUSTAIN (DECAY 120..126); 2 FREEZE (DECAY 127) */
    int32_t dark;           /* the infinite kernel: DARK (no wander, the top held down, the input darker) */
    uint32_t *drift;        /* the infinite kernel: DRIFT (the wander a slow random walk; the framework's random state) */
    const struct tx_rv_par *par;   /* the other core (0: none) */
} tx_rev_ctl_t;

/* the kernels on two cores: the app's hooks (tx_fx_reverb_t.par). A kernel hands half of its lines (HALL, INFINITE)
 * or one half of its tank (PLATE) to the other core and runs the rest here: each half writes only its own state and
 * buffers (their own cache lines), and every read of the lines is older than the block, so the samples are the same
 * on one core or two. run: fn(a) handed over (1: taken; 0: not, run it here); wait: 0 done, else lost mid-way (run
 * here again: a fault's recovery, not the same samples) */
typedef struct tx_rv_par {
    int (*run)(void (*fn)(void *), void *a);
    int (*wait)(void);
} tx_rv_par_t;
TX_INLINE int rv_fork(const tx_rv_par_t *p, void (*fn)(void *), void *a) { return p && p->run && p->run(fn, a); }
TX_INLINE void rv_join(const tx_rv_par_t *p, void (*fn)(void *), void *a, int taken)
{
    if (!taken || p->wait()) fn(a);
}

/* the input of every kernel: the level into the lines, a DC blocker (Frodhes fx.c rev_down, without its 2:1 down) */
#define HL_IN 10320                 /* the send's level into the int16 lines (4x Felucca's: 12 dB less rounding) */
void tx_rev_down(const int32_t *in, int32_t *php, int32_t *phe, int32_t *out);

/* ROOM's tail (tx_fx_hall.c: Felucca's hall, as Frodhes ran it as its HALL): int32 lines at 2^HL_HS x the old int16
 * lines' level (their full scale too: 32767 << HL_HS), its outputs at that scale (the framework takes them down) */
#define HL_NL 8u
#define HL_HS 3
#define HL_NAP 2u
#define HL_N 1536u
typedef struct {
    int32_t comb[(HL_N + 1u) * HL_NL];   /* the 8 lines' ring: row t = their samples written at t; row HL_N repeats 0 */
    int32_t ap[(131u + 307u) * RV_X];
    uint16_t w, d[HL_NAP];
    int32_t hp, he;
    int16_t g[HL_NL], sz;
    uint32_t ph;
    int32_t bx[HL_NC];
    uint32_t jw;                                 /* (the block's write row, kd, md: the other half's arguments) */
    int32_t jkd, jmd;
    struct {                                     /* lines 0..3, 4..7: their damping one-poles, their ends a block */
        int32_t lp[HL_NL / 2u], le[HL_NL / 2u];  /* (after the gain: the line's end at its gain, then damped) */
        int32_t y[HL_NC][HL_NL / 2u];
    } TX_LINE half[2];
} tx_hall_t;
void tx_hall_start(tx_hall_t *h);
/* add: HL_NC samples added to the input (the shimmer), or 0; md: MOD's depth, Q8 (gliding) */
void tx_hall_run(tx_hall_t *h, const int32_t *send, const int32_t *add, int32_t size, int32_t damp, int32_t md, int32_t *mid,
                 int32_t *side, const tx_rv_par_t *par);
void tx_hall_job(void *a);          /* (the other core's half: lines 4..7; core 1 runs its twin) */

/* HQ (tx_fx_hq.c: the HALL): 16 lines, each its own ring in the caller's memory */
#define HQ_NL 16u
#define HQ_NAP 4u
#define HQ_APSUM (211u + 157u + 563u + 409u)
#define HQ_NT 12u                   /* the early reflections: taps a side */
#define HQ_ERN 4096u                /* their ring (a power of 2, past the last tap) */
#define HQ_MODMAX 2u                /* (a ring's spare past its line: the modulated reads are only ever nearer) */
typedef struct {
    int16_t *mem;                                /* the rings (the caller's: tx_hq_words at its scale) */
    uint32_t off[HQ_NL], rl[HQ_NL], w[HQ_NL];    /* each ring's start, length, write index */
    uint16_t len[HQ_NL];                         /* each line's delay */
    int32_t g[HQ_NL], a[HQ_NL];                  /* each line's gain below the shelf and above it (Q15) */
    int32_t sz, damp;                            /* what the gains were made for (sz in Q4, gliding; -1: none yet) */
    int32_t jmd;                                 /* (the block's depth: the other half's argument) */
    int32_t bx[HL_NC];                           /* the block's send (DC-blocked by the caller), diffused in place */
    uint16_t apd[HQ_NAP];
    int16_t apb[HQ_APSUM];
    uint32_t ew;
    int16_t er[HQ_ERN];
    struct {                                     /* lines 0..7, 8..15: their one-poles, remainders, phases, ends */
        int32_t lp[HQ_NL / 2u], ge[HQ_NL / 2u];
        uint32_t ph[HQ_NL / 2u];
        int32_t y[HL_NC][HQ_NL / 2u];
        int32_t el[HL_NC], er[HL_NC];            /* (half[1]: the early reflections, L and R) */
    } TX_LINE half[2];
} tx_hq_t;
void tx_hq_start(tx_hq_t *h, int16_t *mem, uint32_t words);
uint32_t tx_hq_words(void);       /* the rings' words (HQ_HALL_WORDS) */
/* (hv: the tx_hq_t, as tx_hall_run's type; sz the size step, clamped to 0 .. 140; md Q8, as tx_hall_run's) */
void tx_hq_run(tx_hall_t *hv, const int32_t *send, const int32_t *add, int32_t sz, int32_t damp, int32_t md, int32_t *mid,
               int32_t *side, const tx_rv_par_t *par);
void tx_hq_job(void *a);            /* (the other core's half: lines 8..15) */
#define HQ_HALL_WORDS 46378u        /* the rings: the lines' 46 314 + 16 x 4 (txtest checks) */

/* GRAIN DELAY (tx_fx_grain.c): allpasses in its feedback loop */
#ifndef GD_DIFF                     /* 1: 4 allpasses (g 0.5) in the loop, each pass diffused again; 0: none (bit-exact
                                     * as before) */
#define GD_DIFF 1
#endif
#ifndef GD_APX
#define GD_APX 1u                   /* (their lengths' scale: 37, 53, 79, 113 samples x this) */
#endif
#define GD_APN 4u
#define GD_APSUM (282u * GD_APX)
#define GD_SYNCC (GD_APSUM - 1u)    /* SYNC: a pass's group delay in the loop (the allpasses' 282, each one's mean group
                                     * delay its length, less the DC block's ~1 sample a pass on the diffused repeats:
                                     * txtest "grain sync") */

/* INFINITE (tx_fx_inf.c: Frodhes' infinite tank, its bright build; DARK as its variant) */
#define RI_NL 8u
#define RI_CPT 4u
#define RI_NB (HL_NC * RI_CPT)
#define RI_N 3968u                  /* the lines' shared ring, a multiple of RI_NB (the longest line + its swing below) */
#define RI_NDIF 3u
#define RI_NLAP 4u
#ifndef RI_LAP                      /* the bright tank's 4 in-line allpasses (lines 0, 2, 4, 6; Frodhes' full build): 1 the
                                     * default, 0 the LITE tank as before (bit-exact) */
#define RI_LAP 1
#endif
#define RI_APSUM ((597u + (RI_LAP ? 460u : 0u)) * RV_X)   /* the diffusers' lines (127 + 193 + 277), the lines'
                                     * allpasses (67 + 89 + 131 + 173), x 2 */
typedef struct {
    uint32_t w, age, quiet, apfull;
    int32_t il, ie, ik;
    int32_t xp, ilim, fin, c, ca, mp, sp, gdec, gsz, gi;
    int32_t xi;
    uint8_t half;
    int32_t te;
    int32_t dh, de;
    uint32_t hw;
    int32_t rd;
    int32_t lv, tgt, dq, dqi;
    uint8_t mlo, fm, full, gdk;               /* (gdk: DARK's gains made) */
    int32_t ma[RI_NL];
    uint32_t ph[RI_NL];
    int32_t wu[RI_NL], wv[RI_NL];   /* DRIFT: each wandering read's walk (two one-poles toward a random target) */
    int32_t wt[RI_NL], wn[RI_NL];   /* its target, the reads until the next one */
    uint8_t wg;                     /* the reads on their walk (DRIFT, or gliding back onto the sine after it) */
    uint16_t mI[RI_NL];
    uint16_t len[RI_NL], tlen[RI_NL];
    uint16_t lo[RI_NL];             /* a SIZE fade: the length the line in its fade fades from (to len) */
    int32_t xw;                     /* a SIZE fade: the fading line's new read's weight at this tank block's start (Q14) */
    uint8_t xf, xk;                 /* a SIZE fade runs; the line in its fade (0..7, one after the other) */
    int16_t g[RI_NL];
    uint16_t apd[RI_NDIF + RI_NLAP];
    int32_t eb0[RI_NL], eck[RI_NL];              /* (the line ends' gains and poles, where they start: the block's, */
    uint32_t ef[RI_NL], ew;                      /* made before the two halves run; the write row) */
    struct { int32_t yp[RI_NL / 2u], op[RI_NL / 2u], oe[RI_NL / 2u];
             uint16_t lapd[2]; uint8_t lapf; } TX_LINE hs[2];   /* lines 0..3, 4..7: their interpolation, one-pole
                                                 * and remainder (each half its own lines); its two lines' allpasses:
                                                 * their index, wrapped once (bits) */
    int32_t x[RI_NB];
    int32_t m[RI_NB], s[RI_NB];
    struct { int32_t y[RI_NB][RI_NL / 2u]; } TX_LINE hy[2];   /* their ends, a tank block */
    int16_t ap[RI_APSUM];
    int16_t xt[(RI_NB + 1u) * RI_NL];            /* a SIZE fade: each line's blended rows (line k in column k: the two
                                                  * halves write their own columns) */
    int16_t ring[(RI_N + 1u) * RI_NL];
} tx_inf_t;
void tx_inf_start(tx_inf_t *s);
int tx_inf_active(const tx_inf_t *s);
void tx_inf_run(tx_inf_t *s, const int32_t *in, int32_t *mid, int32_t *side, const tx_rev_ctl_t *c);

/* PLATE (tx_fx_plate.c: Frodhes' plate kernel); its sizes here for the TYPEs' union (the same as in its file) */
#define PL_N 32768u                                    /* the tank's ring (int16) */
#define PL_IN 2048u                                    /* the input allpasses' ring */
#define PL_G (HL_NC + 1u)                              /* the guard: a block's samples + the interpolation's next */
enum { PL_S_N = 22, PL_NB = 32, PL_WN = 8 };
typedef struct {
    int16_t ring[PL_N + PL_G + 1u];                  /* (+ 1: whole words for pl_cpy) */
    int32_t id[PL_IN + PL_G + 1u];                   /* (int32, 4x the send x 1.5: the diffusion keeps its steps) */
    int32_t zi[HL_NC];              /* what an input allpass's read older than the start sees */
    int16_t zero[2u * HL_NC];       /* what a read of content older than the start sees */
    uint32_t w, e;                  /* the rings' write index (one a sample), samples since the start (to PL_WARM) */
    uint32_t ph;                    /* the LFO */
    uint32_t quiet;                 /* samples with no input and no line write (to PL_N) */
    int32_t bw, dl;                 /* the bandwidth and the left half's damping one-pole */
    int32_t g, g2, pdm, pbw;        /* decay, allpass 2's gain, the damping / bandwidth poles (Q15): g g2 pdm in
                                     * this order (pl_half's k[]) */
    int32_t sz, szo, xw;            /* the size that runs (-1 at the start); a SIZE fade: the size it fades from, the
                                     * fading site's new read's weight at this block's start (Q14) */
    uint8_t xf, xk;                 /* a SIZE fade runs; the read site in its fade (0..21, one after the other) */
    int32_t cdec, cdamp;            /* the settings the gains were made for */
    int32_t ko;                     /* the output gain (Q12) */
    int32_t lx, gp, pk;             /* the tank's level: its exponent (PL_HOT >> lx), the send's gain last block (Q12),
                                     * the tank's writes' OR of magnitudes last block */
    uint32_t qc, sc;                /* samples since the lines last reached 4096 (the step up after a whole ring);
                                     * blocks until a step down may come */
    uint16_t sb[8][PL_NB];          /* the steps the ring still owes (tx_fx_plate.c pl_scale): each segment's ages
                                     * (ascending) */
    int8_t so[8][PL_NB];            /* the steps owed from there on (+1 up, -1 down a step) */
    uint8_t sbn[8], sw, sxk;        /* the ages a segment; any owed; the segment of the SIZE fade's read settled */
    uint32_t sl[PL_S_N + 1u], sh[PL_S_N + 1u];   /* each read site's settled ages (and the fade's new read's) */
    uint32_t fix;                   /* this block: a write ran into the guard (ring 1, input 2) */
    uint16_t d[PL_S_N];             /* the delays at this size */
    uint16_t tk[PL_S_N];            /* the reads: their segment's top - their delay */
    uint16_t to[PL_S_N];            /* a SIZE fade: the reads in use (the old ones, each site's new one once faded) */
    int16_t xt[HL_NC + 2u];         /* a SIZE fade: the fading site's blended reads (a block + the interpolation's next) */
    int32_t x[HL_NC];
    struct {                        /* the right half's arguments and state (its own lines: it may run on the other
                                     * core): its reads, the left half's end, its writes, its damping one-pole */
        const int16_t *t[4], *f;
        int16_t *o[4];
        int32_t fm, dr, act, pk;
    } TX_LINE jr;
} tx_plate_t;
void tx_plate_start(tx_plate_t *s);
int tx_plate_active(const tx_plate_t *s);
TX_AUDIO_CODE void tx_plate_run(tx_plate_t *s, const int32_t *in, int32_t *mid, int32_t *side, const tx_rev_ctl_t *c);

#endif
