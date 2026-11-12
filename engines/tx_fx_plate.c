/* SPDX-License-Identifier: GPL-3.0-only */
/* REVERB TYPE PLATE: a figure-8 plate tank, the classic published design (integer; Frodhes runs it at the half rate,
 * Textura at the full rate: the notes below are Frodhes', at 22.05 kHz; here every length x 2, HL_NC = 32).
 *
 * Included by rev_types.c (the framework), after fx.c: it uses fx.c's sh_tz, hl_c16, clampi, exp2_neg, osc_sine,
 * CTL, FS and HL_NC. Runs at the half rate (22.05 kHz), one control block = HL_NC = 16 samples a call:
 *   in -> bandwidth one-pole -> 4 input allpasses (142 / 107 / 379 / 277, g 0.75 / 0.625) -> the tank, two halves
 *   fed crosswise, each: + the other half's end x decay -> modulated allpass (672 / 908, g -0.7) -> delay (4453 /
 *   4217) -> damping one-pole -> x decay -> allpass (1800 / 2656, g = decay + 0.15 in 0.25 .. 0.5) -> delay (3720 /
 *   3163); 7 taps a side (the design's table 2) -> mid / side.
 * The design's lengths are at 29.76 kHz: x 0.7409 here (PL_R), and x 0.5 .. x 1 by SIZE.
 *
 * Memory: the tank's 8 lines share one ring of PL_N int16 (each line its own segment, all moving with one write
 * index: a line writes at w + its top and reads w + its top - its delay; PL_GAP samples between segments keep a
 * block's 16 writes out of a neighbour's reads); the 4 input allpasses share a small one the same way (PL_IN; int32 at
 * 4x the lines' old level, Textura: the diffusion's own rounding 12 dB down).
 * Every read is at least one block (16 samples) back, so a block runs stage by stage, each stage over its 16 samples,
 * with plain pointers: the rings carry PL_G guard samples after their end, a copy of their first PL_G (made at the
 * start of each block: a read sees only older blocks' writes), and a write that runs past the end lands in the guard
 * and is copied back to the front at the end of the block (pl_ptr, pl_wptr, pl_fix).
 * Start is O(1) (the ring is not cleared): while all of a read site's 16 reads are older than the start they come from
 * a block of zeros; in the block where they cross the start they reach back at most PL_G writes before it, and the
 * start zeroed those (each line's PL_G positions below its top: 8 x 17), so stale content (the other type's) reads
 * as 0, exactly as from a cleared ring (tests/py/test_rev_plate.py: started over garbage = started over zeros).
 * Every product in the loop is truncated toward zero (sh_tz) and the one-poles decay toward their input, so with no
 * input the tail ends in exact zeros; rev_plate_active() is 0 once the input and every line write have been 0 for
 * longer than the whole ring.
 *
 * Knobs (tx_rev_ctl_t, per control block): DECAY 0..127 = T60 0.8 .. 6 s (exponential; per round of the tank the gain
 * is decay^4); TONE (damp 0..150, 60 = the default) = the in-loop damping and the input bandwidth; SIZE 0..127 = the
 * lengths x 0.5 .. x 1 (a change under a tail cross-fades the reads, 1 s: pl_ctl); MOD 0..127 = the modulated allpasses' excursion, 0 .. +-11.9
 * samples (64 = +-6, the design's +-8 at 29.76 kHz), a 1 Hz sine, the two halves in quadrature.
 *
 * Cost (REV_US_PLATE): see the table at its #define (compiled pi32v2 -Os, loop counts from build/frodhes.dis). */

/* The cost, instructions per control block (16 half-rate samples; compiled pi32v2 -Os, build/frodhes.dis, every
 * instruction outside the loops counted as run once: an upper bound):
 *   pl_in        62 x 16 (+ 25)           1017   the bandwidth one-pole + the 4 input allpasses
 *   pl_half x 2  (38 + 32) x 16 (+ 43)    2326   the tank's two halves
 *   pl_taps      16 x 16 (+ 10)            266   the 7 left taps
 *   pl_taps_ms   34 x 16 (+ 17)            561   the 7 right taps, mid / side (31 before rev-upsample's mid / side
 *                                                 history ahead of the block: +48, ~2 us, inside REV_US_PLATE)
 *   rev_plate_run: the pointers 25 x 4 + 10 x 22 + 11 x 8, the guard copies 2 x 27, the rest 317     779
 *   = 4901 at rest; at worst + a SIZE step's 3 delays 87 + the decay gain 40 + the warm-up's checked reads (the
 *   first 0.15 s after a start) 132 + a wrapped write's copy 81 = 5241.
 * HALL (fx.c rev_hall, its own decimator and DC blocker included), counted the same way: 5644 at rest, 5864 with its
 * gains made anew (DECAY moving); PLATE + the framework's decimator (rev_dec, ~372): 5273 at rest, 5613 at worst,
 * 4-7 % under HALL. In us: HALL's kernel share is REV_US_HALL (210, from the FM-1's measured 260 for the whole
 * reverb); PLATE + rev_dec at 5613 / 5864 of it = 201 -> REV_US_PLATE 191 -> 200 (+ REV_US_DEC 10). (The rule of
 * thumb, 0.8 us per instruction per half-rate sample, reads both ~1.3x high against the FM-1: 262 us for 5241.) */
/* Textura: Frodhes' kernel (firmware/src/rev_plate.c, 0.9.5) as one of the reverb's TYPEs, its own file here. What
 * it computes is unchanged; adapted to the effects' framework (tx_fx.c, tx_fx_kern.h): the names (tx_plate_t, its
 * state in tx_fx_kern.h for the TYPEs' union; tx_tx_rev_ctl_t; tx_fx_sine), the 1 Hz LFO's step as an integer, the
 * guard copies by memcpy. At the full rate (Textura): the lengths and the swing x 2, the decay from the same times,
 * the damping and bandwidth one-poles as at the half rate (rv_pole). */
#include <stddef.h>
#include "tx_fx_kern.h"

#ifndef FR_RAM
#define FR_RAM                                  /* (frodhes.c: the audio hot loops in RAM) */
#define FR_FAR(fn) fn
#endif
#define REV_US_PLATE 200u

#define PL_R(n) (((n) * 14818u + 5000u) / 10000u)      /* the design's 29.76 kHz lengths at 44.1 kHz */
#define PL_M (PL_N - 1u)                               /* (PL_N, PL_IN, PL_G: tx_fx_kern.h) */
#define PL_GAP HL_NC                                   /* between segments: >= one block */
#define PL_MSL 28u                                     /* the modulated lines' room: excursion (23.8) + interpolation */
/* the tank's 8 segments: the longest delay each holds (SIZE 127: x 255 / 256), their tops */
#define PL_LM0 (PL_R(672u) + PL_MSL)                    /* left: modulated allpass */
#define PL_LM1 PL_R(4453u)                              /* left: delay 1 */
#define PL_LM2 PL_R(1800u)                              /* left: allpass 2 */
#define PL_LM3 PL_R(3720u)                              /* left: delay 2 */
#define PL_LM4 (PL_R(908u) + PL_MSL)                    /* right: modulated allpass */
#define PL_LM5 PL_R(4217u)
#define PL_LM6 PL_R(2656u)
#define PL_LM7 PL_R(3163u)
#define PL_O0 PL_LM0
#define PL_O1 (PL_O0 + PL_GAP + PL_LM1)
#define PL_O2 (PL_O1 + PL_GAP + PL_LM2)
#define PL_O3 (PL_O2 + PL_GAP + PL_LM3)
#define PL_O4 (PL_O3 + PL_GAP + PL_LM4)
#define PL_O5 (PL_O4 + PL_GAP + PL_LM5)
#define PL_O6 (PL_O5 + PL_GAP + PL_LM6)
#define PL_O7 (PL_O6 + PL_GAP + PL_LM7)
_Static_assert(PL_O7 + PL_GAP <= PL_N, "the plate's 8 segments must fit its ring");
/* the input allpasses' segments (fixed lengths) */
#define PL_IL0 PL_R(142u)
#define PL_IL1 PL_R(107u)
#define PL_IL2 PL_R(379u)
#define PL_IL3 PL_R(277u)
#define PL_IO0 PL_IL0
#define PL_IO1 (PL_IO0 + PL_GAP + PL_IL1)
#define PL_IO2 (PL_IO1 + PL_GAP + PL_IL2)
#define PL_IO3 (PL_IO2 + PL_GAP + PL_IL3)
_Static_assert(PL_IO3 + PL_GAP <= PL_IN && PL_IL1 >= HL_NC, "the input allpasses must fit their ring");
#define PL_WARM (PL_LM1 + HL_NC)                        /* from here on no read is older than the start */
#define PL_LFO 3116529u                                /* 1 Hz, per control block: CTL / FS x 2^32 */
/* the output: the taps' 0.6 / 2 x 1.9 (Q12; / 1.5 / 4 for the lines' level, pl_in, PL_HOT) at SIZE <= 40, rising
 * 0.067 dB a SIZE step to +5 dB at 115 (pl_ctl): HALL's level at the same MIX (HALL grows with MIX's size, a plate
 * thins out as its tank grows) */
#define PL_OUT 2340
/* the tank's level (Textura): the lines run PL_HOT (Q12) x the old 1.5x, 4x, while their content is quiet, so a
 * quiet note's tail keeps 12 dB more of the int16 lines' steps; a loud send moves the whole tank down by 6 dB steps
 * (exponent lx: the ring and the damping states halved, the send's gain halved, the output's doubled: the same
 * sound, the content's scale exact at every step but the halving's half step), and back up once the lines have
 * stayed under 4096 for a whole ring. lx 2 is the old level; lx 3, 4 hold sends the old lines clipped at (hl_c16
 * under a -0.4 dBFS chord). The step down on a block whose tank writes reach 16384 (their magnitudes' OR), up after
 * a whole ring of writes under 4096; the send's peak also held under PL_XIN in the lines at once, gliding back over
 * the next block */
#define PL_HOT 16384
#define PL_XIN 20000
#define PL_EMAX 4
#define PL_SCN 8u                                      /* (8 calls: a step down at most once a 256-frame block) */
#define PL_LOUD 12                                     /* a half's reads (4, summed) from which its products round
                                                        * (HQ's HQ_QUIET: 3 a line) */
#define PL_ABS(v) ((v) < 0 ? -(v) : (v))
#define PL_RQ(m, c) (((m) + c##0 + (((m) >> 31) & c##1)) >> 15)   /* (m + c0 + (m < 0 ? c1 : 0)) / 2^15: c 0, 32767
                                                 * toward zero; 0, 0 floor; 16384, -1 half away from zero */
#define PL_LIM 262143                                  /* |mid|, |side| <= 2^18 (the framework's exact return) */
_Static_assert(14 * 32768 * 2 * PL_OUT <= 2147483647, "the output product");
/* the read sites: 0..7 the segments' own delays (0 / 4: the modulated ones, without the excursion), 8..21 the taps */

/* (tx_plate_t: tx_fx_kern.h) */

/* the design's delays at 29.76 kHz: the segments, then the taps (left: 48_54 266, 48_54 2974, 55_59 1913, 59_63 1996,
 * 24_30 1990, 31_33 187, 33_39 1066; right: 24_30 353, 24_30 3627, 31_33 1228, 33_39 2673, 48_54 2111, 55_59 335,
 * 59_63 121) and the segment each one reads */
static const uint16_t PL_D0[PL_S_N] = {672, 4453, 1800, 3720, 908, 4217, 2656, 3163,
                                       266, 2974, 1913, 1996, 1990, 187, 1066,
                                       353, 3627, 1228, 2673, 2111, 335, 121};
static const uint8_t PL_SEG[PL_S_N] = {0, 1, 2, 3, 4, 5, 6, 7, 5, 5, 6, 7, 1, 2, 3, 1, 1, 2, 3, 5, 6, 7};
static const uint16_t PL_TOP[8] = {PL_O0, PL_O1, PL_O2, PL_O3, PL_O4, PL_O5, PL_O6, PL_O7};
static const uint16_t PL_ITOP[4] = {PL_IO0, PL_IO1, PL_IO2, PL_IO3};
static const uint16_t PL_IL[4] = {PL_IL0, PL_IL1, PL_IL2, PL_IL3};

void tx_plate_start(tx_plate_t *s)
{
    uint32_t k, i;
    for (i = 0; i < 2u * HL_NC; i++)
        s->zero[i] = 0;
    s->w = s->e = s->ph = s->quiet = 0;
    for (k = 0; k < 8u; k++)                           /* each line's last PL_G writes before the start */
        for (i = PL_TOP[k] - PL_G; i < PL_TOP[k]; i++)
            s->ring[i] = 0;
    for (k = 0; k < 4u; k++)
        for (i = PL_ITOP[k] - PL_G; i < PL_ITOP[k]; i++)
            s->id[i] = 0;
    for (i = 0; i < HL_NC; i++)
        s->zi[i] = 0;
    s->bw = s->dl = s->jr.dr = 0;
    s->lx = 0, s->qc = 0, s->sc = 0, s->gp = PL_HOT, s->pk = 0, s->sw = 0;
    for (k = 0; k < 8u; k++) s->sbn[k] = 0;
    s->cdec = s->cdamp = -1;
    s->sz = -1;
    s->xf = 0;                                          /* (no SIZE fade: the union may hold another TYPE's state) */
}
int tx_plate_active(const tx_plate_t *s) { return s->quiet < PL_N; }

/* the gains and lengths for these settings. A new SIZE (Frodhes' BUGHUNT #19 and its 0.9.10 fixes: it glided one
 * step at a time, 26 samples a delay, a bend and a click per step under a ringing tail): all 22 delays at once, the
 * reads cross-fading from the old ones one site after the other, 2048 samples each (22 x 46 ms = 1.0 s, PL_XS; one site
 * at a time: the cost of one blend, not 22; one fade at a time: a SIZE turned on during it waits for its end); the
 * decay and output gains follow the lengths and the size between as it fades. From silence, at once */
#define PL_XS 8                                         /* a site's fade's step a sample (Q14): 2048 samples a site
                                                         * (Frodhes' 16 at the half rate: at 128 there a sweep's wet
                                                         * bent 6.3 cents, its level stepped 2.6 dB; at 16 1.3 / 0.9) */
_Static_assert(16384 % (HL_NC * PL_XS) == 0, "a site's fade ends on a block (pl_xw past 16384 would go negative)");
static void pl_dset(tx_plate_t *s, uint32_t k, int32_t sz)
{
    uint32_t d = (PL_R(PL_D0[k]) * (uint32_t)(128 + sz)) >> 8;
    s->d[k] = (uint16_t)d;
    s->tk[k] = (uint16_t)((PL_TOP[PL_SEG[k]] + PL_N - d) & PL_M);
}
static void pl_ctl(tx_plate_t *s, const tx_rev_ctl_t *c)
{
    uint32_t k, ls = 0, q, e;
    int32_t sz;
    int new = c->decay != s->cdec || s->xf;
    if (!s->xf && s->sz != c->size) {
        if (s->sz >= 0 && tx_plate_active(s) && s->e >= PL_WARM) {   /* ringing: the fade */
            for (k = 0; k < PL_S_N; k++)
                s->to[k] = s->tk[k];
            s->szo = s->sz, s->xf = 1, s->xk = 0, s->xw = 0;
        }
        s->sz = c->size;                                /* (the start, silence: at once) */
        for (k = 0; k < PL_S_N; k++)
            pl_dset(s, k, s->sz);
        new = 1;
    }
    if (new) {
        for (k = 0; k < 8u; k++)
            ls += s->d[k];
        sz = s->sz;
        if (s->xf) {                                    /* (the fade: the lengths in use, the size between) */
            uint32_t lo = 0, xk = s->xk;
            for (k = 0; k < 8u; k++)
                lo += (PL_TOP[k] + PL_N - s->to[k]) & PL_M;
            if (xk < 8u) {                              /* (the segment in its fade: its length between) */
                int32_t dk = (int32_t)((PL_TOP[xk] + PL_N - s->to[xk]) & PL_M);
                lo = (uint32_t)((int32_t)lo + ((((int32_t)s->d[xk] - dk) * s->xw) >> 14));
            }
            ls = lo;
            sz = s->szo + (((s->sz - s->szo) * (int32_t)((xk * 16384u + (uint32_t)s->xw) / PL_S_N)) >> 14);
        }
        ls /= RV_X;                                     /* (a round's time in the design's half-rate samples) */
        /* g^4 per round of ls samples, 60 dB in T60 = 0.8 s x 7.5^(decay / 127): g = 2^-e, e = 9.2566 ls / 2^(2.907
         * decay / 127) (Q16), 2.907 / 127 = 1500 / 65536; + 104 a step (x 1.15 at 127) makes up the damping's,
         * the modulation's and the truncation's losses at the defaults (measured: tests/py/test_rev_plate.py) */
        q = exp2_neg((uint32_t)c->decay * 1841u);
        e = (uint32_t)(((uint64_t)(ls * q) * 9592u) >> 25);
        e = e * (1270u + (uint32_t)sz) / 1270u;         /* (a bigger tank damps less a second: + 10 % at 127) */
        s->g = (int32_t)exp2_neg(e);
        s->g2 = clampi(s->g + 4915, 8192, 16384);
        s->ko = (int32_t)((((PL_OUT * 16384u / 3u + PL_HOT / 2u) / PL_HOT)
                           * exp2_neg(65536u - (uint32_t)clampi((sz - 40) * 729, 0, 54427))) >> 15);
        s->cdec = c->decay;
    }
    if (c->damp != s->cdamp) {
        s->pdm = rv_pole(clampi(c->damp * 350, 0, 31000));      /* the poles (Q15): bright (0) .. dark; 60 (TONE 64): */
        s->pbw = rv_pole(clampi(c->damp * 240, 0, 26000));      /* about HALL's colour; the input bandwidth (|b - u| < 74240: the product < 2^31) */
        s->cdamp = c->damp;
    }
}
/* a SIZE fade: n samples of the old reads (q) x a plus the new (p) x b (Q14), into t; the weights move sample by sample
 * from this block's start (a, b) toward the next block's (a1, b1), 1/32 of the way a sample (Frodhes 0.9.10: held for
 * the block, the fade was a staircase, a d2 spike at each step). Sample 0 has the block's own weights; the 33rd (the
 * interpolation's next of a modulated site) the next block's */
static void pl_xblend(int16_t *t, const int16_t *q, const int16_t *p, uint32_t n, int32_t a, int32_t b, int32_t a1,
                      int32_t b1)
{
    uint32_t i;
    int32_t da = a1 - a, db = b1 - b;
    a <<= 5, b <<= 5;
    for (i = 0; i < n; i++, a += da, b += db)
        t[i] = (int16_t)hl_c16((q[i] * (a >> 5) + p[i] * (b >> 5)) >> 14);
}
/* the fade's two weights at w (Q14, the new's share): (1 - w, w) x (1 + 0.375 w (1 - w)), +0.8 dB at the middle: the
 * old and new reads are alike after a small SIZE step, unrelated after a big one (a plain 1 - w, w sank the tail
 * 6.8 dB in the middle of SIZE 127 -> 0: the dip goes round the loop; +1.5 dB stepped the level of a SIZE sweep's
 * chained fades 2.5 dB; equal power would lift a small step 3 dB) */
static void pl_xw(int32_t w, int32_t *a, int32_t *b)
{
    int32_t g = 16384 + ((3 * ((w * (16384 - w)) >> 14)) >> 3);
    *a = ((16384 - w) * g) >> 14;
    *b = (w * g) >> 14;
}

/* a read site in the tank: P[j] (j = 0 .. 16) is the sample of delay hi - j at the block's sample 0 (the block's
 * sample i reads P[i] for delay hi, P[i + 1] for hi - 1). While every read of the block is older than the start: the
 * zeros. The block where the reads cross the start reads write-times from -17 on (a delay grows at most 13 a block,
 * e 16; at the full rate 26 and 32, PL_G 33): those were zeroed by the start */
static inline const int16_t *pl_ptr(const tx_plate_t *s, uint32_t top, uint32_t hi, uint32_t lo)
{
    if (s->e + HL_NC <= lo) return s->zero;
    return s->ring + ((s->w + top - hi) & PL_M);
}
/* a write site: 16 samples from the top; one that runs past the ring's end writes the guard (pl_fix copies it) */
static inline int16_t *pl_wptr(tx_plate_t *s, int16_t *b, uint32_t n, uint32_t at, uint32_t bit)
{
    at &= n - 1u;
    if (at > n - HL_NC) s->fix |= bit;
    return b + at;
}
static void pl_cpy32(int32_t *d, const int32_t *q)  /* PL_G + 1 int32 */
{
    uint32_t i;
    for (i = 0; i < PL_G + 1u; i++) d[i] = q[i];
}
static void pl_cpy(int16_t *d, const int16_t *q)     /* PL_G + 1 int16 (both 4-byte aligned) */
{
    uint32_t i;
    for (i = 0; i < PL_G + 1u; i++) d[i] = q[i];      /* (not memcpy: the device's copies a byte at a time) */
}

/* the input, one block: the bandwidth one-pole (toward its input: it ends at 0), then the 4 allpasses in a row, each
 * v = x - g d into its write, y = d + g v (plain shifts: with g > 0 and no input the stored value changes sign every
 * pass and never grows, so it dies out). Returns the OR of the input */
/* (g 3 / 4, 5 / 8: n x 3 >> 2, n x 5 >> 3; the input under 2^19 (its clamp, x 6), the stages' gains bound every value
 * under 2^25: no clamp) */
#define PL_AP(k, n, sh) d = r##k[i], v = y - ((d * (n)) >> (sh)), o##k[i] = v, y = d + ((v * (n)) >> (sh))
static __attribute__((noinline)) int32_t pl_in(const int32_t *const *r, int32_t *const *o, const int32_t *in,
                                               int32_t *x, int32_t *bw, int32_t pb, int32_t *pk)
{
    const int32_t *r0 = r[0], *r1 = r[1], *r2 = r[2], *r3 = r[3];
    int32_t *o0 = o[0], *o1 = o[1], *o2 = o[2], *o3 = o[3];
    uint32_t i;
    int32_t b = *bw, act = 0, pi = 0;
    for (i = 0; i < HL_NC; i++) {
        int32_t y, d, v, u = clampi(in[i], -87381, 87381) * 6;   /* the lines 1.5x hot (3.5 dB less rounding
                                                 * noise), x 4 here: the diffusion's own steps (Textura: int32) */
        int32_t e = b - u;
        if ((uint32_t)(e + 74240) <= 148480u) b = u + sh_tz(e * pb, 15);   /* (pb <= 28916) */
        else {                                  /* (a send over ~1.7x full scale: (b - u) pb past 2^31; Frodhes' 0.9.6
                                                 * fix, BUGHUNT #22, in 32 bits: the same integers, toward zero) */
            int32_t lo = (e & 32767) * pb, q = (e >> 15) * pb + (lo >> 15);
            b = u + q + (e < 0 && (lo & 32767));
        }
        act |= u;
        y = b;
        PL_AP(0, 3, 2);
        PL_AP(1, 3, 2);
        PL_AP(2, 5, 3);
        PL_AP(3, 5, 3);
        x[i] = y;
        y ^= y >> 31, pi = y > pi ? y : pi;
    }
    *bw = b;
    *pk = pi;
    return act;
}

/* one half of the tank, one block, its segments' reads r[] and writes o[]. Quiet (its reads under PL_LOUD at the
 * block's start): truncated toward zero, the two decay products, the damping one-pole (a plain shift there held a
 * dark one-pole at up to -18 for ever: a DC loop) and the allpasses' stored values, so without input the loop's
 * content can only shrink; the allpasses' outputs use plain shifts. Loud (Textura, as HALL's line ends): every
 * product rounded half away from zero (an odd function: no bias, no even harmonic; truncation's signal-locked error
 * went round the loop and ate the low end of a quiet note's tail, 3.6 dB of it in its first second at -60 dBFS). The
 * modulated read's interpolation rounds. a = x + g f (f = the other half's end); the modulated allpass (read q, the
 * interpolation fm, gain -0.7, write wm) into delay 1 (w1); delay 1's end (r1), damped (lp), x g, through allpass 2
 * (read r2, gain g2, write w2) into delay 2 (w3). Returns the OR of the writes */
static FR_RAM __attribute__((noinline)) int32_t pl_half(const int32_t *x, const int16_t *f, const int16_t *const *r,
                                                 int32_t fm, int16_t *const *o, int32_t *lp, const int32_t *k,
                                                 int32_t *pk)
{
    const int16_t *q = r[0], *r1 = r[1], *r2 = r[2];
    int16_t *wm = o[0], *w1 = o[1], *w2 = o[2], *w3 = o[3];
    uint32_t i;
    int32_t g = k[0], g2 = k[1], p = k[2], l = *lp, m = 0, a0 = 0, a1 = 32767, b0 = 0, b1 = 0;
    if (PL_ABS(f[0]) + PL_ABS(q[0]) + PL_ABS(r1[0]) + PL_ABS(r2[0]) >= PL_LOUD)   /* loud: every product rounded */
        a0 = b0 = 16384, a1 = b1 = -1;
    for (i = 0; i < HL_NC; i++) {
        int32_t a = x[i] + PL_RQ(f[i] * g, a), q0 = q[i];
        int32_t d = q0 + (((q[i + 1] - q0) * fm + 128) >> 8), v = hl_c16(a + PL_RQ(d * 22938, a));
        wm[i] = (int16_t)v; m |= v ^ (v >> 31);
        v = hl_c16(d - PL_RQ(v * 22938, b));
        w1[i] = (int16_t)v;
        m |= v ^ (v >> 31);
    }
    for (i = 0; i < HL_NC; i++) {
        int32_t u = r1[i], d = r2[i], v;
        l = u + PL_RQ((l - u) * p, a);
        u = PL_RQ(l * g, a);
        v = hl_c16(u - PL_RQ(d * g2, a));
        w2[i] = (int16_t)v; m |= v ^ (v >> 31);
        v = hl_c16(d + PL_RQ(v * g2, b));
        w3[i] = (int16_t)v;
        m |= v ^ (v >> 31);
    }
    *lp = l;
    *pk = m;
    return m;                                         /* (m: 0 only when every write was 0) */
}

/* the 7 taps of a side (+ + - + - - -), one block: the left into mid[]; then the right, and mid / side (rounded) */
static __attribute__((noinline)) void pl_taps(const int16_t *const *t, int32_t *o)
{
    const int16_t *a0 = t[0], *a1 = t[1], *a2 = t[2], *a3 = t[3], *a4 = t[4], *a5 = t[5], *a6 = t[6];
    uint32_t i;
    for (i = 0; i < HL_NC; i++)
        o[i] = a0[i] + a1[i] - a2[i] + a3[i] - a4[i] - a5[i] - a6[i];
}
static __attribute__((noinline)) void pl_taps_ms(const int16_t *const *t, int32_t *mid, int32_t *side, int32_t k,
                                                 int32_t sh)
{
    const int16_t *a0 = t[0], *a1 = t[1], *a2 = t[2], *a3 = t[3], *a4 = t[4], *a5 = t[5], *a6 = t[6];
    int32_t rh = 1 << (sh - 1);
    uint32_t i;
    for (i = 0; i < HL_NC; i++) {
        int32_t r = a0[i] + a1[i] - a2[i] + a3[i] - a4[i] - a5[i] - a6[i], l = mid[i];
        int32_t m = ((l + r) * k + rh) >> sh, d = ((l - r) * k + rh) >> sh;
        m = m > -PL_LIM ? m : -PL_LIM, d = d > -PL_LIM ? d : -PL_LIM;     /* (smax / smin) */
        mid[i] = m < PL_LIM ? m : PL_LIM;
        side[i] = d < PL_LIM ? d : PL_LIM;
    }
}

/* the tank's level step, lazy (one pass over the whole ring was ~115k instructions in one call, all in one audio
 * interrupt on the FM-1): the damping states at once, the ring's content as the reads reach it, the same integers as
 * the whole ring scaled at the step. Each segment keeps what its content still owes, by age (at this call's start;
 * all HL_NC older a call): sb / so, ascending ages and the steps owed from there on (+1 up, -1 down a step; 0 below
 * the first). A step adds its own from age 1 on (new writes are younger: its call's overwrite age 0 .. -31); the read
 * sites settle what they are about to read in the ring (pl_reads, pl_settle), so a sample is shifted once whatever
 * the steps since it was written (-2: two steps down; an up and a down: none, (2 v) >> 1 = v); what passes PL_LM
 * unread goes (the next segment's writes overwrite it). The work: the reads' samples while what they reach still
 * owes, the first blocks after a step all of a block's reads (22 x 256), the content once (~23k samples a step),
 * each half its own segments' on its core. Down: >> 1 a step (toward -inf, as the old pass: both halves of a word
 * with their own sign); up: x 2 (every value under 4096 for a whole ring: none passes the int16 range; an up comes
 * after a ring with no step: nothing owed then). (The tests: every render as with the whole-ring pass, bit for bit) */
static const uint16_t PL_LM[8] = {PL_LM0, PL_LM1, PL_LM2, PL_LM3, PL_LM4, PL_LM5, PL_LM6, PL_LM7};
static void pl_sh(int16_t *q, uint32_t n, int32_t c)   /* n samples: >> -c, or x 2 (4 a pass: the loop's cost) */
{
    if (c < 0) {
        c = -c;
        for (; n >= 4u; n -= 4u, q += 4)
            q[0] = (int16_t)(q[0] >> c), q[1] = (int16_t)(q[1] >> c), q[2] = (int16_t)(q[2] >> c),
            q[3] = (int16_t)(q[3] >> c);
        for (; n; n--, q++) *q = (int16_t)(*q >> c);
    } else {
        for (; n >= 4u; n -= 4u, q += 4)
            q[0] = (int16_t)(q[0] * 2), q[1] = (int16_t)(q[1] * 2), q[2] = (int16_t)(q[2] * 2),
            q[3] = (int16_t)(q[3] * 2);
        for (; n; n--, q++) *q = (int16_t)(*q * 2);
    }
}
static void pl_rsh(tx_plate_t *s, uint32_t k, uint32_t lo, uint32_t hi, int32_t c)   /* ages lo .. hi of segment k */
{
    uint32_t at = (s->w + PL_TOP[k] - hi) & PL_M, n = hi - lo + 1u;   /* (upward in the ring) */
    if (at + n > PL_N) pl_sh(s->ring + at, PL_N - at, c), n -= PL_N - at, at = 0;
    pl_sh(s->ring + at, n, c);
    if (at <= PL_G) pl_sh(s->ring + PL_N + at, (at + n > PL_G + 1u ? PL_G + 1u : at + n) - at, c);   /* (and the
                                                         * guard's copy: a half may settle after it is made) */
}
/* segment k: what m windows of ages (wl .. wh, ascending, apart) owe, settled: shifted in the ring, then owing
 * nothing in the list (one pass over it; its entries anew where what is owed changes) */
#define PL_EMIT(x, v) do { if ((v) != last) nb[j] = (uint16_t)(x), no[j++] = (int8_t)(v), last = (v); } while (0)
static void pl_settle(tx_plate_t *s, uint32_t k, const uint32_t *wl, const uint32_t *wh, uint32_t m)
{
    uint16_t *b = s->sb[k], nb[PL_NB + 2u * PL_WN];
    int8_t *o = s->so[k], no[PL_NB + 2u * PL_WN];
    uint32_t n = s->sbn[k], i = 0, j = 0, q, any = 0;
    int32_t cur = 0, last = 0;
    for (q = 0; q < m; q++) {
        uint32_t lo = wl[q], hi = wh[q], a = lo, e;
        for (; i < n && b[i] < lo; i++) {
            cur = o[i];
            PL_EMIT(b[i], cur);
        }
        for (;;) {                                      /* (the owing pieces in lo .. hi) */
            if (i < n && b[i] == a) cur = o[i++];
            e = i < n && b[i] <= hi ? b[i] - 1u : hi;
            if (cur) pl_rsh(s, k, a, e, cur), any = 1;
            if (e == hi) break;
            a = e + 1u;
        }
        if (i < n && b[i] == hi + 1u) cur = o[i++];
        PL_EMIT(lo, 0);
        if (hi + 1u < PL_LM[k]) PL_EMIT(hi + 1u, cur);
    }
    if (!any) return;                                   /* (nothing owed there: the list as it was) */
    for (; i < n; i++) PL_EMIT(b[i], o[i]);
    if (j > PL_NB) {                                   /* (no room: the rest of it now; not reached) */
        for (i = 0; i < j; i++)
            if (no[i]) pl_rsh(s, k, nb[i], i + 1u < j ? nb[i + 1u] - 1u : PL_LM[k] - 1u, no[i]);
        j = 0;
    }
    for (i = 0; i < j; i++) b[i] = nb[i], o[i] = no[i];
    s->sbn[k] = (uint8_t)j;
}
static void pl_scale(tx_plate_t *s, int down)      /* the step: its states now, the ring as the reads reach it */
{
    uint32_t k, i;
    int8_t c = (int8_t)(down ? -1 : 1);
    if (down) s->dl >>= 1, s->jr.dr >>= 1;
    else s->dl *= 2, s->jr.dr *= 2;
    for (k = 0; k < 8u; k++) {                      /* (every bound is older than 1) */
        uint32_t n = s->sbn[k];
        if (n == PL_NB) { uint32_t lo = 1u, hi = PL_LM[k] - 1u; pl_settle(s, k, &lo, &hi, 1u); n = s->sbn[k]; }   /* (no room:
                                                         * not reached) */
        for (i = n; i; i--) s->sb[k][i] = s->sb[k][i - 1u], s->so[k][i] = (int8_t)(s->so[k][i - 1u] + c);
        s->sb[k][0] = 1, s->so[k][0] = c, s->sbn[k] = (uint8_t)(n + 1u);
    }
    for (k = 0; k <= PL_S_N; k++) s->sl[k] = 1, s->sh[k] = 0;   /* (none settled: sl > sh) */
    s->sw = 1;
}
static void pl_age(tx_plate_t *s)                  /* a call later: all HL_NC older */
{
    uint32_t k, i, on = 0;
    for (k = 0; k < 8u; k++) {
        uint32_t n = s->sbn[k];
        for (i = 0; i < n; i++)
            if ((s->sb[k][i] += HL_NC) >= PL_LM[k]) { n = i; break; }
        s->sbn[k] = (uint8_t)n;
        on |= n;
    }
    for (k = 0; k <= PL_S_N; k++) s->sl[k] += HL_NC, s->sh[k] += HL_NC;
    s->sw = (uint8_t)(on != 0);
}
/* the read sites settled before they are read (a block + the next each: ages a .. a - HL_NC), and those of their
 * next calls to the 256-frame block's end (w: PL_SWH calls a block; a site settles once a block, the settling's own
 * cost once; a step within the block owes them again: those samples twice in that block; 4 or 2 calls ahead cost more
 * at the 99th percentile, the same at worst), each segment's at once; a site's settled ages kept (sl .. sh, all HL_NC
 * older a call; a step: none), so a call whose reads are all settled does nothing more (p: a SIZE fade's new read, in
 * segment x; x 8: none; the zeros are not the ring's) */
#define PL_SWH 8u
static const uint8_t PL_SS[8][6] = {{0, 99}, {1, 12, 15, 16, 99}, {2, 13, 17, 99}, {3, 14, 18, 99}, {4, 99},
                                    {5, 8, 9, 19, 99}, {6, 10, 20, 99}, {7, 11, 21, 99}};   /* (each segment's sites) */
static __attribute__((noinline)) void pl_reads(tx_plate_t *s, const int16_t *const *t, const int16_t *p, uint32_t x)
{
    uint32_t k, i, a, h = HL_NC * (PL_SWH - ((s->w / HL_NC) & (PL_SWH - 1u)));
    for (k = 0; k < 8u; k++) {
        uint32_t wl[PL_WN], wh[PL_WN], m = 0, q, c;
        if (!s->sbn[k]) continue;
        for (i = 0; i < 6u; i++) {                      /* (each window in age order, joined where they meet) */
            const int16_t *r;
            if ((c = PL_SS[k][i]) >= PL_S_N) {
                if (x != k) break;
                c = PL_S_N, i = 5u;
            }
            r = c < PL_S_N ? t[c] : p;
            if ((uintptr_t)r < (uintptr_t)s->ring || (uintptr_t)r >= (uintptr_t)(s->ring + PL_N + PL_G))
                continue;                               /* (the zeros) */
            a = (s->w + PL_TOP[k] - (uint32_t)(r - s->ring)) & PL_M;   /* (sample m's age: a - m) */
            if (a < s->sb[k][0] || (s->sl[c] + HL_NC <= a && a <= s->sh[c] && (c < PL_S_N || s->sxk == k)))
                continue;                               /* (nothing owed, or settled already) */
            for (q = m; q && wh[q - 1u] >= a; q--) wl[q] = wl[q - 1u], wh[q] = wh[q - 1u];
            s->sl[c] = wl[q] = a > h ? a - h : 0u, s->sh[c] = wh[q] = a, m++;
        }
        if (!m) continue;
        for (i = 1, q = 0; i < m; i++)
            if (wl[i] <= wh[q] + 1u) { if (wh[i] > wh[q]) wh[q] = wh[i]; }
            else q++, wl[q] = wl[i], wh[q] = wh[i];
        pl_settle(s, k, wl, wh, q + 1u);
        if (x == k) s->sxk = (uint8_t)k;
    }
}

static void pl_job(void *a)                          /* the right half (on the other core, or here) */
{
    tx_plate_t *s = (tx_plate_t *)a;
    s->jr.act = FR_FAR(pl_half)(s->x, s->jr.f, s->jr.t, s->jr.fm, s->jr.o, &s->jr.dr, &s->g, &s->jr.pk);
}

TX_AUDIO_CODE void tx_plate_run(tx_plate_t *s, const int32_t *in, int32_t *mid, int32_t *side,
                                                    const tx_rev_ctl_t *c)
{
    int16_t *b = s->ring, *o[8];
    int32_t *ib = s->id, *oi[4];
    const int16_t *t[PL_S_N], *xp = 0;
    const int32_t *ti[4];
    uint32_t k, w, ex, dm0, dm1, d0, d4;
    int32_t act;
    const uint16_t *rk;
    pl_ctl(s, c);
    rk = s->tk, d0 = s->d[0], d4 = s->d[4];
    if (s->xf) {                                        /* a SIZE fade: the reads in use (to) */
        rk = s->to;
        d0 = (PL_TOP[0] + PL_N - s->to[0]) & PL_M, d4 = (PL_TOP[4] + PL_N - s->to[4]) & PL_M;
    }
    ex = (uint32_t)c->mod * 48u;                       /* MOD 127: 6096 = 23.8 samples (Q8) */
    s->ph += PL_LFO;
    dm0 = (d0 << 8) + (uint32_t)((tx_fx_sine(s->ph) * (int32_t)ex) >> 15);
    dm1 = (d4 << 8) + (uint32_t)((tx_fx_sine(s->ph + 0x40000000u) * (int32_t)ex) >> 15);
    w = s->w;
    s->fix = 0;
    /* the bandwidth one-pole and the input allpasses (their ring: the guard is the front's copy) */
    pl_cpy32(ib + PL_IN, ib);
    for (k = 0; k < 4u; k++) {
        uint32_t at = (w + PL_ITOP[k]) & (PL_IN - 1u);
        ti[k] = s->e + HL_NC <= PL_IL[k] ? s->zi : ib + ((w + PL_ITOP[k] - PL_IL[k]) & (PL_IN - 1u));
        if (at > PL_IN - HL_NC) s->fix |= 2u;
        oi[k] = ib + at;
    }
    {   /* the tank's level: a step down (loud writes last block) or up (quiet for a whole ring), then the send's gain
         * on this block's diffused send: its peak held under PL_XIN in the lines, gliding from last block's */
        int32_t pi, gn, big, *x = s->x;
        uint32_t i;
        act = pl_in(ti, oi, in, x, &s->bw, s->pbw, &pi);
        if (s->sc) s->sc--;                             /* (a step down at most every PL_SCN calls: its cost) */
        if (s->pk >= 16384 && s->lx < PL_EMAX && !s->sc)
            pl_scale(s, 1), s->lx++, s->gp >>= 1, s->qc = 0, s->sc = PL_SCN;
        else if (s->pk >= 4096 || !s->lx) s->qc = 0;
        else if ((s->qc += HL_NC) >= PL_N + PL_G + 1u) pl_scale(s, 0), s->lx--, s->gp <<= 1, s->qc = 0;
        gn = PL_HOT >> s->lx;
        if ((big = pi > 131071)) pi = 131071;
        if (pi && (((pi >> 2) * gn) >> 12) > PL_XIN) gn = (int32_t)(((uint32_t)PL_XIN << 14) / (uint32_t)pi);
        if (!big && gn == s->gp)                        /* (the usual block: one gain, nothing to clamp) */
            for (i = 0; i < HL_NC; i++) x[i] = (x[i] * gn + 8192) >> 14;
        else
            for (i = 0; i < HL_NC; i++) {
                int32_t a = x[i] > 131071 ? 131071 : x[i] < -131071 ? -131071 : x[i];
                x[i] = (a * (s->gp + ((gn - s->gp) * (int32_t)(i + 1u)) / (int32_t)HL_NC) + 8192) >> 14;
            }
        s->gp = gn;
    }
    /* the tank: every read site's pointer (the zeros while it reads before the start), the guard copy, the halves */
    if (s->e >= PL_WARM)
        for (k = 0; k < PL_S_N; k++)
            t[k] = b + ((w + rk[k]) & PL_M);
    else
        for (k = 0; k < PL_S_N; k++)
            t[k] = pl_ptr(s, PL_TOP[PL_SEG[k]], s->d[k], s->d[k]);
    /* the modulated reads at dm / 256 samples: a site reads delay hi (P[i]) blended toward hi - 1 (P[i + 1]) by
     * fm / 256, so hi = ceil(dm / 256) and fm = hi x 256 - dm: below (dm + 255) >> 8 and 255 - ((dm + 255) & 255)
     * (Frodhes 3b015d55: hi = floor, fm = dm & 255 read hi - frac, the delay jumping 2 samples at each whole sample
     * of the excursion, a grain under MOD) */
    dm0 += 255u, dm1 += 255u;
    t[0] = pl_ptr(s, PL_O0, dm0 >> 8, (dm0 >> 8) - 1u);
    t[4] = pl_ptr(s, PL_O4, dm1 >> 8, (dm1 >> 8) - 1u);
    if (s->xf) {                                        /* a SIZE fade: the fading site's new read */
        uint32_t xk = s->xk;
        xp = b + ((w + s->tk[xk]) & PL_M);
        if (!(xk & 3u) && xk < 8u) {                    /* (0 / 4: modulated, the excursion on the new delay too) */
            uint32_t dm = xk ? dm1 : dm0, dc = xk ? d4 : d0;
            xp = b + ((w + PL_TOP[xk] - ((dm >> 8) - dc + s->d[xk])) & PL_M);
        }
    }
    if (s->sw) pl_reads(s, t, xp, s->xf ? PL_SEG[s->xk] : 8u);   /* (a step owed: the reads' samples settled first) */
    pl_cpy(b + PL_N, b);
    if (s->xf) {                                        /* a SIZE fade: the one site in its fade, its old read (t)
                                                         * and its new blended (a segment before the halves write:
                                                         * every delay > a block) */
        uint32_t xk = s->xk, n = !(xk & 3u) && xk < 8u ? HL_NC + 1u : HL_NC;
        int32_t xa, xb, xa1, xb1;
        const int16_t *p = xp;
        pl_xw(s->xw, &xa, &xb);
        pl_xw(s->xw + (int32_t)HL_NC * PL_XS, &xa1, &xb1);   /* (the next block's: 16384 at the site's end) */
        pl_xblend(s->xt, t[xk], p, n, xa, xb, xa1, xb1);
        t[xk] = s->xt;
    }
    for (k = 0; k < 8u; k++)
        o[k] = pl_wptr(s, b, PL_N, w + PL_TOP[k], 1u);
    {   /* the two halves: the right one on the other core (or here, after), the left one here; each reads only
         * what is at least a block old and writes its own segments */
        /* (volatile: read once; as a constant the compiler rebuilt the address in the loop, 4 instructions a sample) */
        const int32_t *volatile xv = s->x;
        int taken;
        for (k = 0; k < 4u; k++) s->jr.t[k] = t[4u + k], s->jr.o[k] = o[4u + k];
        s->jr.f = t[3];
        s->jr.fm = (int32_t)(255u - (dm1 & 255u));
        taken = rv_fork(c->par, pl_job, s);
        act |= FR_FAR(pl_half)(xv, t[7], t, (int32_t)(255u - (dm0 & 255u)), o, &s->dl, &s->g, &s->pk);
        rv_join(c->par, pl_job, s, taken);
        act |= s->jr.act;
        s->pk |= s->jr.pk;
    }
    pl_taps(t + 8, mid);
    pl_taps_ms(t + 15, mid, side, s->ko, 12 - s->lx);
    if (s->fix & 1u) pl_cpy(b, b + PL_N);            /* (the guard past the wrapped write: the front's own copy) */
    if (s->fix & 2u) pl_cpy32(ib, ib + PL_IN);
    s->w = w + HL_NC;
    if (s->sw) pl_age(s);
    if (s->xf && (s->xw += (int32_t)HL_NC * PL_XS) >= 16384) {   /* a site faded: the next; the end: the gains anew */
        s->to[s->xk] = s->tk[s->xk], s->xw = 0;
        if (++s->xk == PL_S_N) s->xf = 0, s->cdec = -1;
    }
    if (s->e < PL_WARM) s->e += HL_NC;
    s->quiet = act ? 0u : s->quiet < PL_N ? s->quiet + HL_NC : PL_N;
}
