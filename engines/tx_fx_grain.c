/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura GRAIN DELAY (on the mix, before the reverb): a delay whose repeats are grained. Our own code; the model is
 * the one dm-GrainDelay (GPL-3.0) describes: an echo line, and grain voices on one clock reading the delayed signal,
 * each grain with its own start (SPRAY), pitch, direction and place, their sum fed back so every repeat is grained
 * again. Integers only; 32-bit products per sample (the wide ones split in two).
 *
 *   in (the mix's mid) + the feedback -> the line (mono, TX_GD_N samples (tx_fx_gd_len.h: 2.551 s) of block float,
 *   12-bit mantissas and a 4-bit shift per 16 samples: 113 KB, its noise follows the signal: ~-109 dBFS at -40 dBFS,
 *   far below at -70)
 *   the ECHO: one tap at TIME (20 ms .. 1.66 s at 127, on to 2.00 s; a new TIME crossfades from the old tap in 23 ms: the
 *     echo moves without a level step)
 *   the GRAINS: GRAINS voices (1 .. 30; those the cap lets sound) staggered on one clock (each grain SIZE long, Hann
 *     windows: they add up to a constant), each started at TIME + a random SPRAY, at PITCH (semitone steps,
 *     -12 .. +12), reversed by chance (REVERSE), panned at random within SPREAD; read with linear interpolation (whole samples at rate 1: exact)
 *   out = the input x the dry's gain + (ECHO .. GRAINS by GRAIN) x the wet's (MIX, tx_fx.h: 0 the input alone, 64 both
 *     whole, 127 the delay alone); the feedback: that (its mid, as below), through TONE's low-pass,
 *     a soft knee (above half scale only: the quiet repeats stay exact), 4 allpasses (GD_DIFF, tx_fx_kern.h: each
 *     pass diffused again, flat; not at GRAIN 0, the echo exact), FEEDBACK's gain and a DC block, into the line
 *   the loop keeps what each pass grains again, its level and its top, but for FEEDBACK and TONE: the grains' level
 *     as they add (gd_level: in power when scattered, in amplitude as copies, and in amplitude below a crossover
 *     set by how far their reads spread: gd_lowlevel), their mid x sqrt 2 (their places' equal power undone), the
 *     GRAIN blend equal-power (linear for copies), TONE 127 open, the DC block at 15 Hz (no build-up under the
 *     crossover). (A measured make-up did this before, +0.3 .. +15.6 dB over ~1.5 s: it lagged, and the bottom ran
 *     away under it; off now.) FEEDBACK (TX_FX_GD_FB): to 64 as before (x 258), then a
 *     pass's gain rises to 1 at 110
 *     (the repeats go on at every GRAIN, SPRAY, SPREAD) and past it to +2.5 dB a pass at 127: the delay runs away into
 *     a wall held at a -14.6 dBFS peak (the loop as heard, before its make-up; a peak that falls back over ~3 s: 6 dB
 *     under it the whole gain past 1, at it 1, 2 dB over it -0.75 dB a pass, in proportion between; the soft knee
 *     stays the guard) that stops when FEEDBACK comes down
 *   FREEZE: the line stops taking input (a 10 ms fade): the whole line (2.55 s) circles, the grains keep reading them
 * The dynamic cap (cap, set by the chain each block): grains beyond it finish their window and do not start again, so
 * the delay costs what the effects reserve leaves (on the FM-1: ~14 us a sounding grain at rate 1, ~42 us pitched or
 * reversed, ~220 us the line, the echo and the feedback, +40 the allpasses; each grain decodes its run of the line
 * once a block). Off (MIX 0, after a 46 ms fade): nothing computed, the mix untouched. A line switched on reads as
 * empty until it has been written (no stale sound, no memset). */
#include "tx_fx.h"

#define GD_GUARD 288u               /* every read this far behind the write at the block's start (a block + a gathered
                                     * one): a block's reads never see its own writes, so the grains run grain by grain */
#define GD_WOW 983                  /* the delay's drift velocity at most (Q16 a sample): a 1.5 % bend, ~26 cents (settled MOD
                                     * reaches ~0.9 %; gd_anim) */
#define GD_KNEE (1 << 21)           /* the feedback's soft knee: half of full scale (Q22) */
#define GD_MKFLOOR 2560u            /* the make-up's floor: the loop's |x| / 4 summed a block at a mean |x| of -100 dBFS */
#define GD_WALL (3 << 18)           /* FEEDBACK past 1: the loop's peak it runs up to (Q22: -14.6 dBFS; a dense wall's
                                     * rms ~-20 dBFS at MIX 127) */

const tx_param_t tx_fx_grain_params[TX_GD_NP] = {
    {"MIX", 127, 0, TX_TEXT("0 the sound alone (the delay off); up to 64 the echoes come in, the sound stays whole; 64 both whole; above it the sound fades out, 127 the echoes alone")},
    {"TIME", TX_GD_TIME_MAX, 70, TX_TEXT("the echo's time: 20 ms .. 1.66 s (127) .. 2.00 s (exponential); the grains read around it")},
    {"FEEDBACK", 127, 50, TX_TEXT("how long the repeats go on (each one grained again): none .. endless (110) .. running away into a held wall")},
    {"GRAINS", TX_GD_MAXG - 1u, 3, TX_TEXT("grain voices on the repeats: 1 .. 30 (fewer when the CPU is short)")},
    {"SIZE", 127, 64, TX_TEXT("each grain's length: 20 .. 500 ms")},
    {"SPRAY", 127, 20, TX_TEXT("where each grain starts: at TIME .. up to 0.5 s later, at random")},
    {"PITCH", 127, 64, TX_TEXT("the grains' pitch: -12 .. +12 semitones (64 = as played)")},
    {"REVERSE", 127, 0, TX_TEXT("the chance a grain plays backwards: never .. always")},
    {"SPREAD", 127, 64, TX_TEXT("each grain's place: all in the middle .. anywhere left to right")},
    {"TONE", 127, 127, TX_TEXT("the repeats' top: dark (400 Hz) .. 16 kHz (126) .. open (127: the repeats keep their top)")},
    {"GRAIN", 127, 64, TX_TEXT("the clean echo (0) .. the grains (127)")},
    {"FREEZE", 1, 0, TX_TEXT("hold the line: the last 2.55 s go round, the grains keep playing them")},
    {"MOD", 127, 0, TX_TEXT("the animation: none (the plain delay) .. an evolving cloud: the time drifts (a slow wow), the grains' SIZE, start, direction and place vary and drift, more of them leave unison for INTERVALS")},
    {"MOD RATE", 127, 40, TX_TEXT("how fast the animation moves, always at random: ~8 s .. ~0.1 s between its turns")},
    {"INTERVALS", TX_GDI_N - 1u, TX_GDI_5OCT, TX_TEXT("the pitches MOD gives grains (exact ratios): none | octaves | 5th, octave | 4th, 5th | major | minor | all | up (5th, octave: repeats climb)")},
};

/* INTERVALS: a grain that leaves unison takes one of 8 ratios (Q16, just: 2/1, 3/2, 4/3, 5/4, 6/5 and their
 * inverses: the grains are copies of one sound at once, so the ratios fuse without beating) */
#define GD_R(a, b) ((65536u * (a) + (b) / 2u) / (b))
static const uint32_t GD_IVQ[TX_GDI_N][8] = {
    {GD_R(1, 1), GD_R(1, 1), GD_R(1, 1), GD_R(1, 1), GD_R(1, 1), GD_R(1, 1), GD_R(1, 1), GD_R(1, 1)},
    {GD_R(1, 2), GD_R(2, 1), GD_R(1, 2), GD_R(2, 1), GD_R(1, 2), GD_R(2, 1), GD_R(1, 2), GD_R(2, 1)},
    {GD_R(1, 2), GD_R(2, 3), GD_R(3, 2), GD_R(2, 1), GD_R(1, 2), GD_R(2, 3), GD_R(3, 2), GD_R(2, 1)},
    {GD_R(3, 4), GD_R(4, 3), GD_R(2, 3), GD_R(3, 2), GD_R(3, 4), GD_R(4, 3), GD_R(2, 3), GD_R(3, 2)},
    {GD_R(5, 4), GD_R(3, 2), GD_R(2, 1), GD_R(1, 2), GD_R(5, 4), GD_R(3, 2), GD_R(2, 1), GD_R(1, 2)},
    {GD_R(6, 5), GD_R(3, 2), GD_R(2, 1), GD_R(1, 2), GD_R(6, 5), GD_R(3, 2), GD_R(2, 1), GD_R(1, 2)},
    {GD_R(1, 2), GD_R(2, 3), GD_R(3, 4), GD_R(4, 5), GD_R(5, 4), GD_R(4, 3), GD_R(3, 2), GD_R(2, 1)},
    {GD_R(3, 2), GD_R(2, 1), GD_R(3, 2), GD_R(2, 1), GD_R(3, 2), GD_R(2, 1), GD_R(3, 2), GD_R(2, 1)},
};

/* the voices GRAINS runs: as many, but GRAINS 1 two heads, half a window apart, sharing the grain's place: one grain
 * stream, each grain crossfading into the next (a lone Hann window gated every pass: at FEEDBACK 96 the repeats fell
 * -4.0 dB a pass, the echo's -1.0; a burst caught near a window's end lost ~8 dB a pass) */
#ifndef GD_HEADS
#define GD_HEADS 1
#endif
TX_INLINE uint32_t gd_nv(const tx_fx_grain_t *d) { uint32_t G = d->p[TX_GD_GRAINS] + 1u; return GD_HEADS && G == 1u ? 2u : G; }
/* the voices that may sound: the cap (the chain's, by the CPU), but GRAINS 1's two heads are one grain stream: a cap
 * of one keeps both (one head alone is the fade they cure; the governor's floor is one grain, never a weaker sound) */
TX_INLINE uint32_t gd_ncap(const tx_fx_grain_t *d, uint32_t G)
{
    uint32_t c = d->cap;
    if (GD_HEADS && G == 2u && d->p[TX_GD_GRAINS] == 0u && c == 1u) c = 2u;
    return c < G ? c : G;
}
TX_RAM static uint32_t rnd(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }   /* (the grains' rng: d->rng,
                                                         * or the second half's copy, tx_fx_grain_split) */

void tx_fx_grain_init(tx_fx_grain_t *d)
{
    unsigned i;
    tx_fx_memset(d, 0, sizeof *d);                              /* (once: the line's 96 KB too) */
    for (i = 0; i < TX_GD_NP; i++) d->p[i] = (uint8_t)tx_fx_grain_params[i].def;
    d->rng = 0x2545F491u;
    d->mrng = 0x9E3779B9u;
    d->wg = 32768;
    d->cap = TX_GD_MAXG;
    d->gc = d->gct = d->gcr = d->hot = 4096;
    d->fbl = -1;
    d->bwg = -1;
}
void tx_fx_grain_sync(tx_fx_grain_t *d, unsigned samples)
{
    d->sync = samples > TX_FX_GD_TIME[TX_GD_TIME_MAX] ? TX_FX_GD_TIME[TX_GD_TIME_MAX] : samples;   /* (a new delay
                                                 * crossfades as TIME's; SYNC up to TIME's top) */
}
void tx_fx_grain_param(tx_fx_grain_t *d, unsigned id, unsigned v)
{
    if (id < TX_GD_NP) d->p[id] = (uint8_t)(v > tx_fx_grain_params[id].max ? tx_fx_grain_params[id].max : v);
}
int tx_fx_grain_quiet(const tx_fx_grain_t *d) { return !d->on; }

/* the line: a sample (Q22), decoded */
TX_RAM static inline int32_t gd_rd(const tx_fx_grain_t *d, uint32_t i)
{
    const uint8_t *b = d->pk + ((i * 3u) >> 1);
    uint32_t k = i >> 4;
    int32_t m = (int32_t)(((uint32_t)b[0] | ((uint32_t)b[1] << 8)) >> ((i & 1u) << 2));
    m = (int32_t)((uint32_t)m << 20) >> 20;                     /* the 12 bits, sign-extended */
    return (m * 4096) >> ((d->ex[k >> 1] >> ((k & 1u) << 2)) & 15u);
}
/* the line: n samples (Q22) written from d->w on, a block's worth at once (sample for sample as one at a time):
 * each 16-sample block encoded when its last sample is in */
TX_RAM static void gd_wrn(tx_fx_grain_t *d, const int32_t *x, unsigned n)
{
    uint32_t w = d->w;
    int32_t pk = d->stpk;
    unsigned j = 0, t, k;
    while (j < n) {
        t = 16u - (w & 15u);
        if (t > n - j) t = n - j;
        for (k = 0; k < t; k++) {
            int32_t v = x[j + k], a;
            v = v > 8388607 ? 8388607 : v < -8388607 ? -8388607 : v;
            d->stg[(w + k) & 15u] = v;
            a = v < 0 ? -v : v;
            if (a > pk) pk = a;
        }
        w += t;
        j += t;
        if (!(w & 15u)) {                               /* the block's in: its shift from its peak, then 8 triples */
            uint32_t sh = 0, b0 = w - 16u, blk = b0 >> 4;
            uint8_t *b = d->pk + ((b0 * 3u) >> 1);
            while (sh < 12u && (pk << (sh + 1u)) < (1 << 23)) sh++;
            d->ex[blk >> 1] = (uint8_t)((d->ex[blk >> 1] & (0xF0u >> ((blk & 1u) << 2))) | (sh << ((blk & 1u) << 2)));
            for (k = 0; k < 16u; k += 2u, b += 3) {
                int32_t m0 = (d->stg[k] * (1 << sh) + 2048) >> 12, m1 = (d->stg[k + 1u] * (1 << sh) + 2048) >> 12;
                m0 = m0 > 2047 ? 2047 : m0 < -2048 ? -2048 : m0;
                m1 = m1 > 2047 ? 2047 : m1 < -2048 ? -2048 : m1;
                b[0] = (uint8_t)m0;
                b[1] = (uint8_t)(((uint32_t)m0 >> 8 & 15u) | ((uint32_t)m1 << 4));
                b[2] = (uint8_t)((uint32_t)m1 >> 4);
            }
            pk = 0;
            if (w >= TX_GD_N) w = 0;                    /* (TX_GD_N a multiple of 16: the wrap is a block's end) */
        }
    }
    d->w = w;
    d->stpk = pk;
    d->fill = d->fill + n < TX_GD_N ? d->fill + n : TX_GD_N;
}
/* a * q (q Q15, |a| up to 2^24): two 32-bit products, exact to the bit (floored) */
TX_RAM static inline int32_t mq15(int32_t a, int32_t q) { return (a >> 15) * q + (((a & 32767) * q) >> 15); }
/* the same rounded toward zero (q >= 0): a loop gain under 1 that lets a tail die to exact zero */
TX_RAM static inline int32_t mq15z(int32_t a, int32_t q) { return a < 0 ? -mq15(-a, q) : mq15(a, q); }
/* y += (x - y) q with its remainder carried (*e): settles on x exactly (a one-pole that leaves no DC, no stuck LSB) */
TX_RAM static inline int32_t op15(int32_t y, int32_t x, int32_t q, int32_t *e)
{
    int32_t dv = x - y, lo = (dv & 32767) * q + *e;
    *e = lo & 32767;
    return y + (dv >> 15) * q + (lo >> 15);
}
/* the index `back` samples behind w (back <= TX_GD_N) */
TX_RAM static inline uint32_t gd_idx(uint32_t w, uint32_t back) { return w >= back ? w - back : w + TX_GD_N - back; }

/* a grain starts (at sample j of the block, w0 the write at the block's start): TIME + SPRAY behind the write, kept
 * GD_GUARD behind it and inside the line over its whole length at its rate; silent (off) if that reaches back past
 * what was written since the delay came on */
TX_RAM static void gd_spawn(tx_fx_grain_t *d, tx_gd_grain_t *g, uint32_t len, uint32_t w0, uint32_t j, uint32_t *rs)
{
    uint32_t r = rnd(rs), spr = d->p[TX_GD_SPRAY], rev = d->p[TX_GD_REVERSE], back;
    int32_t semi = ((int32_t)d->p[TX_GD_PITCH] - 64) * 12 / 63, rate, sp = d->p[TX_GD_SPREAD], m = d->mg;
    int64_t lo, hi, far;
    uint32_t pr = (uint32_t)TX_FX_GD_PITCH[semi + 12];
    if (m) {                                            /* MOD: more SPRAY, REVERSE, SPREAD (drifting), an interval */
        uint32_t r3 = rnd(rs);
        int32_t pu = (m * 3) >> 5;                      /* (the share leaving unison, Q12: up to 3 / 4, drifting +-25 %) */
        spr += (uint32_t)((m * (48 + ((d->du[2] * 40) >> 15))) >> 15);
        rev += (uint32_t)((m * (16 + ((d->du[3] * 12) >> 15))) >> 15);
        sp += ((127 - sp) * m) >> 15;
        if (spr > 127u) spr = 127u;
        pu += (pu * d->du[3]) >> 17;
        if (((r3 >> 20) & 4095u) < (uint32_t)pu) {
            pr = (uint32_t)(((uint64_t)pr * GD_IVQ[d->p[TX_GD_IVL]][r3 >> 29]) >> 16);
            while (pr > 131072u) pr >>= 1;              /* (past 2x: an octave down, the pitch class kept) */
        }
        pr = (uint32_t)(((uint64_t)pr * (uint32_t)(d->wvel < 65536 ? 65536 - d->wvel : 1)) >> 16);   /* (the wow's bend) */
        if (pr > 131072u) pr = 131072u;                 /* (a run's span: gd_seg's buffer) */
    }
    back = d->tap + (uint32_t)(((uint64_t)(r >> 8) * (spr * 174u)) >> 24) + (uint32_t)(d->wq0 >> 16);
    rate = (r & 127u) < rev ? -(int32_t)pr : (int32_t)pr;
    lo = (int64_t)GD_GUARD + (rate > 65536 ? (((int64_t)(rate - 65536) * len) >> 16) : 0);
    hi = (int64_t)TX_GD_N - 2 * (int64_t)GD_GUARD - ((((int64_t)65536 - rate) * len) >> 16);
    if ((int64_t)back < lo) back = (uint32_t)lo;
    if ((int64_t)back > hi) back = hi > (int64_t)GD_GUARD ? (uint32_t)hi : GD_GUARD;
    far = (int64_t)back + (rate < 65536 ? ((((int64_t)65536 - rate) * len) >> 16) : 0) + 2;   /* its farthest read */
    g->on = far <= (int64_t)d->fill;                     /* (older than the delay's start: not there, stay silent) */
    g->pos = gd_idx(w0 + j >= TX_GD_N ? w0 + j - TX_GD_N : w0 + j, back);
    g->frac = 0;
    g->rate = rate;
    {   /* the place: equal power at a random point within +-SPREAD */
        int32_t pn = (int32_t)((rnd(rs) >> 17) & 32767u) - 16384;
        uint32_t th = 0x20000000u + (uint32_t)((((pn * sp) >> 6) * 4096));
        g->gl = (int16_t)tx_fx_sine(th + 0x40000000u);
        g->gr = (int16_t)tx_fx_sine(th);
    }
}

/* the line: cnt samples from index s on (wrapping), decoded into o: whole 16-sample blocks one shift and 8 triples
 * at a time, the ragged ends sample by sample */
TX_RAM static void gd_run(const tx_fx_grain_t *d, uint32_t s, unsigned cnt, int32_t *o)
{
    while (cnt && (s & 15u)) { *o++ = gd_rd(d, s); s = s + 1u >= TX_GD_N ? 0u : s + 1u; cnt--; }
    while (cnt >= 16u) {
        uint32_t k = s >> 4, sh = 8u + ((d->ex[k >> 1] >> ((k & 1u) << 2)) & 15u), t;
        const uint8_t *b = d->pk + ((s * 3u) >> 1);
        for (t = 0; t < 8u; t++, b += 3, o += 2) {      /* (m << 20) >> (8 + shift) = (m * 4096) >> shift, signed */
            o[0] = (int32_t)(((uint32_t)b[0] << 20) | ((uint32_t)b[1] << 28)) >> sh;
            o[1] = (int32_t)(((uint32_t)b[1] << 16 & 0x00F00000u) | ((uint32_t)b[2] << 24)) >> sh;
        }
        s = s + 16u >= TX_GD_N ? 0u : s + 16u;           /* (TX_GD_N a multiple of 16) */
        cnt -= 16u;
    }
    while (cnt) { *o++ = gd_rd(d, s); s = s + 1u >= TX_GD_N ? 0u : s + 1u; cnt--; }
}

/* a sounding grain over m samples (from sample j0, its window phase ph before the first): its run of the line decoded
 * once (rate 1 from a whole sample: m samples; else the span its position covers, +1 for the interpolation), read
 * with a Q16 position in that run, windowed and placed into sl / sr. sl 0 (GRAIN at 0, the grains unheard): only its
 * position moves, as far as the reads would have taken it */
/* a rate-1 segment's output: the window's gains only when it steps (the pitched loop keeps the product: its registers
 * are full, the test and its spills cost more there than they save, measured on the FM-1) */
TX_RAM __attribute__((noinline)) static void gd_win1(const int32_t *buf, unsigned m, uint32_t ph, uint32_t inc,
                                                   int32_t gl, int32_t gr, int32_t *sl, int32_t *sr)
{
    int32_t ql = 0, qr = 0;
    uint32_t wi = 1024u;                                /* (the window's step whose gains ql, qr hold: none yet) */
    unsigned j;
    for (j = 0; j < m; j++) {
        int32_t b = buf[j];
        ph += inc;
        if (ph >> 22 != wi) {                           /* (a new step of the window: its gains) */
            int32_t wv = TX_FX_HANN[wi = ph >> 22];
            ql = (wv * gl) >> 15; qr = (wv * gr) >> 15;
        }
        sl[j] += mq15(b, ql);                           /* the window and the place in one gain per side */
        sr[j] += mq15(b, qr);
    }
}

TX_RAM static void gd_seg(const tx_fx_grain_t *d, tx_gd_grain_t *g, uint32_t ph, uint32_t inc, unsigned j0, unsigned m,
                   int32_t *sl, int32_t *sr)
{
    int32_t buf[2u * TX_BLOCK + 4u], gl = g->gl, gr = g->gr;
    unsigned j;
    if (!sl) {
        if (g->rate == 65536 && !g->frac) {
            g->pos = g->pos + m >= TX_GD_N ? g->pos + m - TX_GD_N : g->pos + m;
            return;
        }
    } else {
        sl += j0;                                       /* (the run's first sample) */
        sr += j0;
    }
    if (g->rate == 65536 && !g->frac) {
        gd_run(d, g->pos, m, buf);
        g->pos = g->pos + m >= TX_GD_N ? g->pos + m - TX_GD_N : g->pos + m;
        gd_win1(buf, m, ph, inc, gl, gr, sl, sr);
    } else {
        int32_t r = g->rate, end = ((int32_t)g->frac + r * (int32_t)(m - 1u)) >> 16, lo = end < 0 ? end : 0;
        uint32_t base = lo < 0 ? gd_idx(g->pos, (uint32_t)-lo) : g->pos, P = g->frac + ((uint32_t)-lo << 16);
        if (!sl) P += (uint32_t)r * m;
        else {
            gd_run(d, base, (unsigned)((end > 0 ? end : 0) - lo + 2), buf);
            for (j = 0; j < m; j++) {
                int32_t wv, a = buf[P >> 16], x = a + mq15(buf[(P >> 16) + 1u] - a, (int32_t)((P & 0xFFFFu) >> 1));
                ph += inc;
                wv = TX_FX_HANN[ph >> 22];
                sl[j] += mq15(x, (wv * gl) >> 15);
                sr[j] += mq15(x, (wv * gr) >> 15);
                P += (uint32_t)r;
            }
        }
        {   /* (backwards, the position may end just below the run: a signed whole part) */
            int32_t q = (int32_t)P >> 16;
            g->frac = P & 0xFFFFu;
            base = q < 0 ? gd_idx(base, (uint32_t)-q) : base + (uint32_t)q;
            g->pos = base >= TX_GD_N ? base - TX_GD_N : base;
        }
    }
}

/* one voice over the block: its window on the shared clock (T at the block's start), a new grain where the window
 * wraps (start: under the cap), in at most two runs split there. Returns its sounding samples. A sounding grain off
 * the clock (GRAINS moved: the voices' spacing changed) finishes its window on its own phase; at its end the voice
 * goes back onto the clock silent and starts again at the clock's next wrap (a window never jumps: no click) */
TX_RAM static unsigned gd_voice(tx_fx_grain_t *d, tx_gd_grain_t *g, uint32_t T, uint32_t inc, unsigned n, int start, uint32_t len,
                         uint32_t w0, int32_t *sl, int32_t *sr, uint32_t *rs)
{
    unsigned j = 0, on = 0;
    uint32_t ph = g->on ? g->vp : T;                    /* (silent: on the clock at once) */
    while (j < n) {
        uint32_t rem = 0u - ph, m = rem ? (rem - 1u) / inc : n;  /* the samples before the window wraps */
        if (m > n - j) m = n - j;
        if (m && g->on) { gd_seg(d, g, ph, inc, j, m, sl, sr); on += m; }
        ph += inc * m;
        j += m;
        if (j < n) {                                    /* the wrap: a new grain, its first sample here */
            g->on = 0;
            if (ph != T + inc * j) ph = T + inc * j;    /* (off the clock: back on it, silent until its next wrap) */
            else if (start) {
                gd_spawn(d, g, len, w0, j, rs);
                if (GD_HEADS && g == &d->g[1] && d->p[TX_GD_GRAINS] == 0u) { g->gl = d->g[0].gl; g->gr = d->g[0].gr; }   /* (GRAINS 1: the
                                                         * second head at the grain's place) */
            }
            if (g->on) { gd_seg(d, g, ph, inc, j, 1u, sl, sr); on++; }
            ph += inc;
            j++;
        }
    }
    g->vp = ph;
    return on;
}

/* MOD, once a block (before the voices: they read what it leaves): its amount glides (0.7 s end to end); four
 * smoothed random drifts (a target at random times, ~8 s .. ~0.1 s apart on average by MOD RATE, never on a beat;
 * two poles after it, their time half that): the delay's time (its reach such that the bend stays under ~1 %, ~17
 * cents, at any MOD RATE, and inside the line's guard), SIZE (+-1 octave of length), SPRAY's added reach, the grains'
 * share off unison and REVERSE's added chance (gd_spawn). MOD 0, settled: none of it, the plain delay to the bit */
TX_INLINE uint32_t mrnd(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }   /* (in flash, as begin: rnd
                                                         * is in RAM, out of a flash call's reach) */
static void gd_anim(tx_fx_grain_t *d, unsigned n, uint32_t tgt)
{
    uint32_t inc = TX_FX_GD_SIZE[d->p[TX_GD_SIZE]], rt = d->p[TX_GD_MODRATE], p16, k, i, tm;
    int32_t mt = d->p[TX_GD_MOD] * 258, wmt, x;
    if (mt > 32768) mt = 32768;
    d->wq0 = d->wq1;
    if (!d->mg && mt)                                   /* (MOD comes up: every drift sets off at once) */
        for (i = 0; i < 4u; i++) d->dt[i] = (int32_t)(mrnd(&d->mrng) >> 16) - 32768;
    if (d->mg != mt) d->mg += d->mg < mt ? (mt - d->mg > 256 ? 256 : mt - d->mg) : -(d->mg - mt > 256 ? 256 : d->mg - mt);
    d->ginc = inc; d->glen = 0xFFFFFFFFu / inc;
    if (!d->mg) { d->wq1 = d->wvel = 0; return; }
    p16 = 48u << (rt / 20u); p16 += p16 * (rt % 20u) / 20u;   /* (a turn's chance a block, Q16: 48 .. 4147) */
    k = 2u * p16;                                       /* (the poles, Q16) */
    for (i = 0; i < 4u; i++) {
        if ((mrnd(&d->mrng) >> 16) < p16) d->dt[i] = (int32_t)(mrnd(&d->mrng) >> 16) - 32768;
        d->da[i] += ((d->dt[i] - d->da[i]) * (int32_t)k) >> 16;
        d->du[i] += ((d->da[i] - d->du[i]) * (int32_t)k) >> 16;
    }
    x = (d->du[1] * d->mg) >> 15;                       /* SIZE: the clock x 0.5 .. 2 */
    d->ginc = (uint32_t)(((uint64_t)inc * (uint32_t)(65536 + (x >= 0 ? 2 * x : x))) >> 16);
    d->glen = 0xFFFFFFFFu / (inc >> 1);                 /* (its longest: the bounds hold as the clock drifts) */
    tm = d->tap < tgt ? d->tap : tgt;                   /* the delay's drift: its reach (glides; the guard at once) */
    wmt = (int32_t)(209715u / k);
    d->wm += (wmt - d->wm) / 16 + (wmt > d->wm) - (wmt < d->wm);
    if (d->wm > (int32_t)tm - (int32_t)GD_GUARD - 4) d->wm = (int32_t)tm - (int32_t)GD_GUARD - 4;
    if (d->wm < 0) d->wm = 0;
    x = (d->du[0] * d->mg) >> 15;
    {   /* the velocity toward the drift's place, bounded to the design's bend (GD_WOW): the reach glides 1 / 16 a block
         * while a MOD RATE jump makes the drifts fast at once, which bent the echo up to ~58 %; the place stays inside
         * the guard (as the reach's own clamp) even so, and the read always runs forward (r = 65536 - wvel > 0: gd_echo's buffer) */
        int32_t nn = (int32_t)(n ? n : 1u), lim = (int32_t)tm - (int32_t)GD_GUARD - 4, v = (x * d->wm * 2 - d->wq0) / nn;
        if (lim < 0) lim = 0;
        if (lim > 32767) lim = 32767;
        lim *= 65536;                                   /* (the guard's reach, Q16: the gliding reach may lag it) */
        if (v > GD_WOW) v = GD_WOW;
        if (v < -GD_WOW) v = -GD_WOW;
        if (d->wq0 + v * nn > lim) v = (lim - d->wq0) / nn;
        if (d->wq0 + v * nn < -lim) v = (-lim - d->wq0) / nn;
        if (v > 32768) v = 32768;
        if (v < -32768) v = -32768;
        d->wvel = v;
    }
    d->wq1 = d->wq0 + d->wvel * (int32_t)n;             /* (exactly where the block's read ends) */
}
/* the grains' level (Q15) for N sounding: their Hann windows, evenly spaced, sum to N / 2, their squares to 3 N / 8;
 * copies of one read (SPRAY near 0, unison, forward, MOD 0) add in amplitude (2 / N: the echo exactly), scattered
 * ones in power (sqrt(8 / 3N)); between, by how far SPRAY spreads the reads (coherent c, Q8: 1 at SPRAY 0, 0 from
 * 20 ms on). So the delay keeps its level at every GRAINS and every cap: a sparse repeat and a dense cloud alike
 * (it fell 1 / sqrt(N): -12 dB from 2 grains to 30) */
static int32_t gd_level(const tx_fx_grain_t *d, uint32_t N);
/* the count the level law takes: N (GRAINS, capped); after a change, the voices sounding, kept between the old count
 * and N, until they reach N (or a grain's length has passed). GRAINS 30 -> 1 on copies (SPRAY 0) took one grain's
 * level while the 28 others still sounded out their windows (gliding there in 170 ms: ~9 dB over both held renders);
 * 1 -> 30 kept the old level while the new voices started. At rest, N exactly */
static uint32_t gd_nlaw(tx_fx_grain_t *d, uint32_t N, unsigned n)
{
    uint32_t o = d->nlv, a = 0, k, lo, hi;
    if (!o || o == N) { d->nlv = (uint8_t)N; d->nlt = 0; return N; }
    for (k = 0; k < TX_GD_MAXG; k++) a += d->g[k].on;
    lo = o < N ? o : N; hi = o < N ? N : o;
    a = a < lo ? lo : a > hi ? hi : a;
    d->nlt += n;
    if (a == N || d->nlt >= d->glen) { d->nlv = (uint8_t)N; d->nlt = 0; return N; }
    return a;
}
/* the same below the crossover: scattered grains still read alike far below 1 / their reads' spread (SPRAY's reach,
 * and PITCH's drift between grains over a window), so there they add in amplitude: 2 / N (the power law let them
 * gain up to +13 dB a pass at 3 Hz (30 grains, SPRAY 20) or 50 Hz (SPRAY 2): at high FEEDBACK the bottom ran away,
 * the knee and the make-up pressed the rest down, the repeats went dull and faint after a second). The crossover a
 * one-pole near 1 / (pi spread); reversed grains read no alike: none */
static void gd_lowlevel(tx_fx_grain_t *d, uint32_t N)
{
    uint32_t p = d->p[TX_GD_PITCH], dp = p > 64u ? p - 64u : 64u - p;
    uint32_t R = d->p[TX_GD_SPRAY] ? (uint32_t)d->p[TX_GD_SPRAY] * 174u
                                   : (d->glen < 0x100000u ? d->glen : 0x100000u) * dp * 6u / 100u;
    if (N < 2u || d->p[TX_GD_REVERSE]) { d->gnlt = d->gnt; return; }   /* (one grain: no other to add to. The crossover
                                                         * keeps its pole while the low share glides out: forced
                                                         * open at once, its low-pass jumped to the whole signal, a
                                                         * step: REVERSE 0 -> 1 under a tone 11x a held one's d2) */
    d->gnlt = (int32_t)(65536u / N > 65536u ? 65536u : 65536u / N);
#ifndef GD_XM
#define GD_XM 6u
#endif
    d->xk = R ? (int32_t)(65536u * GD_XM / R > 32768u ? 32768u : 65536u * GD_XM / R) : 32768;
    if (d->xk < 3) d->xk = 3;
}
static int32_t gd_level(const tx_fx_grain_t *d, uint32_t N)
{
    uint32_t reach = (uint32_t)d->p[TX_GD_SPRAY] * 174u, c, D, r, b;
    if (!N) return 0;
    c = d->p[TX_GD_PITCH] != 64u || d->p[TX_GD_REVERSE] || d->p[TX_GD_MOD] || reach || N < 2u ? 0u : 256u;
    D = (256u - c) * (3u * N * 8192u) + c * (N * N * 16384u);   /* (N / 4 .. N^2 / 4, Q16 after the >> 8) */
    D >>= 8;
    for (r = 0, b = 1u << 30; b; b >>= 2)               /* r = sqrt(D) (Q8) */
        if (D >= r + b) { D -= r + b; r = (r >> 1) + b; } else r >>= 1;
    r = 8388608u / (r ? r : 1u);
    return (int32_t)(r > 65536u ? 65536u : r);
}
/* the echo's delay: TIME's, or SYNC's. With SYNC and the loop's allpasses on (GD_DIFF, GRAIN above 0), each pass
 * also takes their group delay (282 samples, 6.4 ms: an allpass's mean group delay is its length, its impulse
 * response's energy centre there; less ~1 sample the DC block takes off the diffused repeats: GD_SYNCC), and the
 * repeats drifted off the beat by that much a pass (0.12 s by the 20th): the tap is that much shorter and the input
 * goes into the line that much later (d->pre), so the first echo is on the beat and so is every pass after it. SYNC
 * off, or GRAIN 0 (no allpasses): the delay as before */
TX_INLINE uint32_t gd_target(const tx_fx_grain_t *d)
{
    if (!d->sync) return TX_FX_GD_TIME[d->p[TX_GD_TIME]];
#if GD_DIFF
    if (d->p[TX_GD_GRAIN] && d->sync >= TX_FX_GD_TIME[0] + GD_SYNCC) return d->sync - GD_SYNCC;
#endif
    return d->sync;
}
/* A block in three steps (tx_fx_grain_process runs them in turn): begin (MIX on: the line's start), the voices (the
 * echo's run and the grains: they read only the line as the blocks before left it, never this block's input, so
 * they may run before the input is there, on another core: tx_fx_bus_begin), the end (the blend with the input, the
 * feedback, the write). begin returns 1 when the voices may run ahead: the line is not silent (a silent line idles
 * on a silent input: that needs the input first) */
TX_AUDIO_CODE int tx_fx_grain_begin(tx_fx_grain_t *d, unsigned n)
{
    int32_t mv = d->p[TX_GD_MIX], gb = d->p[TX_GD_GRAIN] * 258, wgt = d->p[TX_GD_FREEZE] ? 0 : 32768;
    uint32_t k, tgt = gd_target(d);
    (void)n;
    d->ahead = 0;
    if (!d->on) {
        if (!mv) return 0;
        d->on = 1; d->fill = 0; d->mix = 0; d->zrun = 0; d->tap = tgt; d->tx = 0; d->lp = d->dcy = d->lpe = d->dce = 0;
        d->wg = 0;                                      /* (the line fades in, 10 ms: its first echo never starts on a
                                                         * step of the sound it caught mid-way) */
        d->gbl = gb > 32768 ? 32768 : gb;
        d->gc = d->gct = d->gcr = d->hot = 4096; d->sae = d->saf = d->slv = 0; d->fbl = -1;
        for (k = 0; k < TX_GD_MAXG; k++) d->g[k].on = 0;
        d->mg = d->wm = d->wq1 = 0;
        for (k = 0; k < 4u; k++) d->dt[k] = d->da[k] = d->du[k] = 0;
        for (k = 0; k < 2u; k++) d->xl[k] = d->xle[k] = 0;
        d->bwg = -1;
        d->nlv = 0;
#if GD_DIFF
        d->apw = 0;
        d->sc = 0; d->pw = -1; d->prei = 0;           /* (pw -1: set below, to where this block's SYNC puts it) */
        for (k = 0; k < GD_SYNCC; k++) d->pre[k] = 0;
#endif
    }
#if GD_DIFF
    d->sc = tgt != (d->sync ? d->sync : TX_FX_GD_TIME[d->p[TX_GD_TIME]]);   /* (SYNC's compensation: gd_target) */
    if (d->pw < 0) d->pw = d->sc ? 32768 : 0;          /* (the delay comes on: no glide into its first block) */
#endif
    gd_anim(d, n, tgt);
    k = gd_nv(d);
    k = gd_nlaw(d, gd_ncap(d, k), n);
    d->gnt = gd_level(d, k);
    d->blin = !d->p[TX_GD_SPRAY] && d->p[TX_GD_PITCH] == 64u && !d->p[TX_GD_REVERSE] && !d->p[TX_GD_MOD];
    gd_lowlevel(d, k);
    d->ahead = !(d->zrun >= TX_GD_N && wgt && !d->tx);
    return d->ahead;
}
/* the echo's tap: a new TIME starts its crossfade (before the voices: a grain starts TIME behind the write) */
TX_INLINE void gd_tap(tx_fx_grain_t *d)
{
    uint32_t tgt = gd_target(d);
    if (!d->tx && tgt != d->tap) { d->tap0 = d->tap; d->tap = tgt; d->tx = 1; }
}
/* the grains this block will start on voice g (2 rng draws each, 3 with MOD): its wraps on the clock, as gd_voice walks them */
static unsigned gd_spawns(const tx_gd_grain_t *g, uint32_t T, uint32_t inc, unsigned n)
{
    unsigned j = 0, c = 0;
    uint32_t ph = g->on ? g->vp : T;
    while (j < n) {
        uint32_t rem = 0u - ph, m = rem ? (rem - 1u) / inc : n;
        if (m > n - j) m = n - j;
        ph += inc * m;
        j += m;
        if (j < n) {
            if (ph != T + inc * j) ph = T + inc * j;
            else c++;
            ph += inc;
            j++;
        }
    }
    return c;
}
/* the voices in two halves (two cores: tx_fx_grain_voices_a / _b at once): the first K voice slots and the echo, the
 * rest; the second half's rng starts where the first half's ends (its draws counted here, before either runs), so
 * the grains are the same as in one pass. K 0: one pass (tx_fx_grain_voices) */
TX_AUDIO_CODE void tx_fx_grain_split(tx_fx_grain_t *d, unsigned n)
{
    uint32_t G = gd_nv(d), cap = gd_ncap(d, G), inc = d->ginc;
    uint32_t step = 0xFFFFFFFFu / (cap ? cap : 1u) + 1u, k, K = (G + 1u) / 2u, r = d->rng, c = 0;
    if (n > TX_BLOCK) n = TX_BLOCK;
    gd_tap(d);
    for (k = 0; k < K; k++)
        if (k < G && k < cap) c += gd_spawns(&d->g[k], d->ph + k * step, inc, n);   /* (gd_voice's start) */
    for (k = 0; k < (d->mg ? 3u : 2u) * c; k++) r = r * 1664525u + 1013904223u;   /* (MOD: a third draw) */
    d->vk = (uint8_t)K; d->rng2 = r;
}
/* voice slots k0 .. k1 - 1 into sl, sr (0: dry), their count; rs their rng */
TX_RAM static unsigned gd_voices(tx_fx_grain_t *d, unsigned k0, unsigned k1, unsigned n, int32_t *sl, int32_t *sr, uint32_t *rs)
{
    uint32_t G = gd_nv(d), cap = gd_ncap(d, G), inc = d->ginc;
    uint32_t len = d->glen, step = 0xFFFFFFFFu / (cap ? cap : 1u) + 1u, k, j;   /* (spaced by those that may
                                                         * sound: a cap under GRAINS bunched them, a gap each turn) */
    unsigned act = 0;
    if (sl) for (j = 0; j < n; j++) sl[j] = sr[j] = 0;
    for (k = k0; k < k1; k++)                           /* (past GRAINS: a sounding grain finishes its window) */
        if (k < G || d->g[k].on)
            act += gd_voice(d, &d->g[k], d->ph + k * step, inc, n, k < cap && k < G, len, d->w, sl, sr, rs);
    return act;
}
TX_RAM static int gd_dry(const tx_fx_grain_t *d)
{
    int32_t gb = d->p[TX_GD_GRAIN] * 258;
    if (gb > 32768) gb = 32768;
    return !gb && !d->gbl;                              /* GRAIN at 0 and settled: the grains move on unheard */
}
/* the echo's run into e (the tap's run, and the old tap's crossfading, each 0 where the line was not written yet) */
TX_RAM static void gd_echo(tx_fx_grain_t *d, unsigned n)
{
    uint32_t j, w0 = d->w, ok;
    int32_t *e = d->we;
    if (d->wq0 | d->wq1) {                              /* MOD: the tap drifts (a moving read, interpolated) */
        int32_t buf[2u * TX_BLOCK + 4u], r = 65536 - d->wvel;
        if (r < 1) r = 1;                               /* (a guard: gd_anim keeps r in 0.5 .. 1.5; the run fits buf) */
        if (r > 131072) r = 131072;
        uint32_t whole = d->tap + (uint32_t)(d->wq0 >> 16), P = 65536u - ((uint32_t)d->wq0 & 0xFFFFu);
        if (whole + 1u <= d->fill) {
            gd_run(d, gd_idx(w0, whole + 1u), ((P + (uint32_t)r * (n - 1u)) >> 16) + 2u, buf);
            for (j = 0; j < n; j++, P += (uint32_t)r) {
                int32_t a = buf[P >> 16];
                e[j] = a + mq15(buf[(P >> 16) + 1u] - a, (int32_t)((P & 0xFFFFu) >> 1));
            }
        } else for (j = 0; j < n; j++) e[j] = 0;
    } else if ((ok = d->tap <= d->fill)) gd_run(d, gd_idx(w0, d->tap), n, e);
    else for (j = 0; j < n; j++) e[j] = 0;
    if (d->tx) {
        uint32_t i0 = gd_idx(w0, d->tap0), ok0 = d->tap0 <= d->fill;
        for (j = 0; j < n; j++) {
            int32_t o = ok0 ? gd_rd(d, i0) : 0;
            i0 = i0 + 1u >= TX_GD_N ? 0u : i0 + 1u;
            if (d->tx) { e[j] = o + mq15(e[j] - o, (int32_t)(d->tx << 5)); if (++d->tx > 1024u) d->tx = 0; }
        }
    }
}
/* the echo and every voice, one pass */
TX_RAM TX_AUDIO_CODE void tx_fx_grain_voices(tx_fx_grain_t *d, unsigned n)
{
    int dry = gd_dry(d);
    if (n > TX_BLOCK) n = TX_BLOCK;
    d->vk = 0;
    gd_tap(d);
    gd_echo(d, n);
    d->vact = gd_voices(d, 0, TX_GD_MAXG, n, dry ? 0 : d->wl, dry ? 0 : d->wr, &d->rng);
    d->ph += d->ginc * n;
}
/* after tx_fx_grain_split: the echo and the first half (a), the second half (b, into its own block): either core,
 * both at once; the end adds them */
TX_RAM TX_AUDIO_CODE void tx_fx_grain_voices_a(tx_fx_grain_t *d, unsigned n)
{
    int dry = gd_dry(d);
    if (n > TX_BLOCK) n = TX_BLOCK;
    gd_echo(d, n);
    d->vact = gd_voices(d, 0, d->vk, n, dry ? 0 : d->wl, dry ? 0 : d->wr, &d->rng);
}
TX_RAM TX_AUDIO_CODE void tx_fx_grain_voices_b(tx_fx_grain_t *d, unsigned n)
{
    int dry = gd_dry(d);
    if (n > TX_BLOCK) n = TX_BLOCK;
    d->vact2 = gd_voices(d, d->vk, TX_GD_MAXG, n, dry ? 0 : d->wl2, dry ? 0 : d->wr2, &d->rng2);
}
#if GD_DIFF
/* the loop's diffusion (GD_DIFF): 4 allpasses, g 0.5, in series on the feedback (flat: each pass keeps its level and
 * its top; its echoes smeared over a few ms again each pass, the cloud denser the longer it goes); rounded toward 0
 * (silence ends in zeros) */
#define GD_AP1 (37u * GD_APX)
#define GD_AP2 (53u * GD_APX)
#define GD_AP3 (79u * GD_APX)
#define GD_AP4 (113u * GD_APX)
TX_INLINE int32_t gd_ap1(int32_t *b, uint32_t *i, uint32_t len, int32_t x)   /* one allpass, its place i (0 .. len - 1) */
{
    int32_t v = b[*i], w = x + ((v - (v >> 31)) >> 1);
    b[*i] = w;
    *i = *i + 1u >= len ? 0u : *i + 1u;
    return v - ((w - (w >> 31)) >> 1);
}
TX_INLINE int32_t gd_diffuse(int32_t *ap, uint32_t *ai, int32_t x)
{
    x = gd_ap1(ap, &ai[0], GD_AP1, x);
    x = gd_ap1(ap + GD_AP1, &ai[1], GD_AP2, x);
    x = gd_ap1(ap + GD_AP1 + GD_AP2, &ai[2], GD_AP3, x);
    return gd_ap1(ap + GD_AP1 + GD_AP2 + GD_AP3, &ai[3], GD_AP4, x);
}
#endif
TX_RAM TX_AUDIO_CODE void tx_fx_grain_end(tx_fx_grain_t *d, int32_t *L, int32_t *R, unsigned n)
{
    int32_t mv = d->p[TX_GD_MIX], mixt = TX_FX_MIXQ(mv), gb = d->p[TX_GD_GRAIN] * 258, fbk = TX_FX_GD_FB[d->p[TX_GD_FEEDBACK]];
    int32_t cw = 0;                                     /* the make-up's share (Q12): none now. It made up the loss the
                                                         * grains' 2 / N law, their places and the linear GRAIN blend
                                                         * left in the loop; those are exact now (gd_level, the
                                                         * crossover, the mid x sqrt 2, the equal-power blend), and its
                                                         * +0.3 dB a pass would only grow the repeats */
    uint32_t ae = 0, ar = 0, af = 0, al = 0;            /* (the block's echo, the grains' reference and the loop, |x| / 4
                                                         * summed; the loop as heard, its peak, for the wall) */
    uint32_t rb = 0;                                    /* (the reference: the line where the grains read on average,
                                                         * TIME + half of SPRAY's reach; 0 none) */
    int32_t ta = d->p[TX_GD_TONE] >= 127u ? 32768 : TX_FX_GD_TONE[d->p[TX_GD_TONE]], wgt = d->p[TX_GD_FREEZE] ? 0 : 32768;
    uint32_t j, w0;
    int32_t gnt = d->gnt;                               /* the grains' level (begin: gd_level) */
    int32_t *e = d->we, *sl = d->wl, *sr = d->wr;
    int32_t gs[2], gc[2];                               /* MIX: the wet / dry gains at the block's edges (Q15) */
    int32_t fq, dfq;                                    /* FEEDBACK's gain along the block (Q8 of Q15), its step */
    int dry;
    if (n > TX_BLOCK) n = TX_BLOCK;
    if (!d->on) return;                                 /* (MIX 0 and faded: begin left it off) */
    if (!d->ahead) {                                    /* the line silent (and taking input): idle while the input is */
        if (d->zrun >= TX_GD_N && wgt && !d->tx) {
            for (j = 0; j < n && !L[j] && !R[j]; j++) {}
            if (j == n) { d->act = 0; d->mix = mixt; if (!mv) d->on = 0; return; }
        }
        tx_fx_grain_voices(d, n);
    }
    d->ahead = 0;
    if (d->vk) {                                        /* (two halves: the second's grains added, its rng and count) */
        if (!gd_dry(d)) for (j = 0; j < n; j++) { sl[j] += d->wl2[j]; sr[j] += d->wr2[j]; }
        d->vact += d->vact2;
        d->rng = d->rng2;
        d->ph += d->ginc * n;
        d->vk = 0;
    }
    if (gb > 32768) gb = 32768;
    if (!(!gb && !d->gbl)) {
        rb = d->tap + (uint32_t)d->p[TX_GD_SPRAY] * 87u;
        if (rb > d->fill || rb > TX_GD_N - GD_GUARD) rb = 0;
    }
    if (fbk > 32768)                                    /* (past 1: the share the level allows; over the wall, less) */
        fbk = d->hot >= 0 ? 32768 + (((fbk - 32768) * d->hot) >> 12) : 32768 + ((5208 * d->hot) >> 12);   /* (5208:
                                                         * -1.5 dB at -4096, -0.75 at the floor) */
    {   /* FEEDBACK's gain ramps over the block from the last block's (Frodhes a539d91d: it stepped once a block, a zipper
         * in every repeat while FEEDBACK moved or the wall's share changed); Q8 steps, 0 while it holds */
        int32_t f0 = d->fbl < 0 ? fbk : d->fbl;
        fq = f0 * 256; dfq = (fbk - f0) * 256 / (int32_t)n;
        d->fbl = fbk;
    }
    dry = !gb && !d->gbl;
    w0 = d->w;
    TX_FAR(tx_fx_mix_glide)(&d->mix, mixt, 16 * (int32_t)n, 1u, gs, gc);   /* (MIX glides: 46 ms end to end; in flash) */
    /* the blend, the feedback into the line (gathered in sl, written after), the output */
    {
    uint32_t wj = w0, fj = d->fill;                     /* (the write index and the fill, sample by sample) */
#if GD_DIFF
    uint32_t ai[GD_APN] = {d->api[0], d->api[1], d->api[2], d->api[3]};   /* (the allpasses' places, held here) */
    uint32_t pi = d->prei;
    int32_t pwt = d->sc ? 32768 : 0;                    /* (SYNC's compensation: the pre-delayed input's share) */
#endif
    for (j = 0; j < n; j++) {
        int32_t x = (L[j] >> 2) + (R[j] >> 2), wl, wr, f, fm;   /* the mid, Q22 */
        if (d->gn != gnt) {                             /* (8 a sample, or 1 / 256 of the way: a voice count's step
                                                         * in ~6 ms, as the voices come and go, gd_nlaw) */
            int32_t st = (gnt > d->gn ? gnt - d->gn : d->gn - gnt) >> 8;
            st = st > 8 ? st : 8;
            d->gn += d->gn < gnt ? (gnt - d->gn > st ? st : gnt - d->gn) : -(d->gn - gnt > st ? st : d->gn - gnt);
        }
        if (d->gnl != d->gnlt) {                        /* (the same, below the crossover) */
            int32_t st = (d->gnlt > d->gnl ? d->gnlt - d->gnl : d->gnl - d->gnlt) >> 8;
            st = st > 8 ? st : 8;
            d->gnl += d->gnl < d->gnlt ? (d->gnlt - d->gnl > st ? st : d->gnlt - d->gnl) : -(d->gnl - d->gnlt > st ? st : d->gnl - d->gnlt);
        }
        if (dry) wl = wr = fm = e[j];                   /* (GRAIN 0: the echo alone, exactly) */
        else {
            int32_t gl = mq15(sl[j], d->gn), gr = mq15(sr[j], d->gn), lo;   /* (gd_level's; below the crossover,
                                                         * on the mid, 2 / N: the difference added to both sides) */
            if (d->gnl != d->gn) {
                d->xl[0] = op15(d->xl[0], (sl[j] >> 1) + (sr[j] >> 1), d->xk, &d->xle[0]);
                d->xl[1] = op15(d->xl[1], d->xl[0], d->xk, &d->xle[1]);
                lo = mq15z(d->xl[1], d->gnl - d->gn);
                gl += lo; gr += lo;
            }
            if (d->gbl != gb) d->gbl += d->gbl < gb ? (gb - d->gbl > 16 ? 16 : gb - d->gbl) : -(d->gbl - gb > 16 ? 16 : d->gbl - gb);
            wl = e[j] + mq15(gl - e[j], d->gbl);       /* the echo .. the grains (GRAIN glides: 46 ms end to end) */
            wr = e[j] + mq15(gr - e[j], d->gbl);
            gl = mq15z((gl >> 1) + (gr >> 1), 46341);   /* (the loop: the grains' mid x sqrt 2, their places' equal
                                                         * power undone: a centred grain is 0.71 a side, its mid lost
                                                         * 3 dB a pass; SPREAD reaches +-11 degrees, 0.03 dB) */
            if (d->blin) fm = e[j] + mq15(gl - e[j], d->gbl);   /* (copies: the echo's own level) */
            else {                                      /* (apart: they add in power, the blend's equal-power,
                                                         * sin(pi b / 2), its Taylor terms to b^5: 0.04 dB; made
                                                         * again only as GRAIN glides) */
                if (d->bwg != d->gbl) {
                    int32_t b1 = d->gbl, b0 = 32768 - b1, q1 = (b1 * b1) >> 15, q0 = (b0 * b0) >> 15;
                    b1 = (b1 * (51472 - ((21168 * q1) >> 15) + ((2611 * ((q1 * q1) >> 15)) >> 15))) >> 15;
                    b0 = (b0 * (51472 - ((21168 * q0) >> 15) + ((2611 * ((q0 * q0) >> 15)) >> 15))) >> 15;
                    d->bw1 = b1 > 32768 ? 32768 : b1; d->bw0 = b0 > 32768 ? 32768 : b0; d->bwg = d->gbl;
                }
                fm = mq15z(e[j], d->bw0) + mq15z(gl, d->bw1);   /* (toward 0: no -1 kept alive) */
            }
        }
        f = fm;                                         /* the feedback: its make-up, TONE, the soft knee, FEEDBACK,
                                                         * the DC block */
        if (cw && !dry) {                               /* (the make-up's measures: none now) */
            ae += (uint32_t)(e[j] < 0 ? -e[j] : e[j]) >> 2; af += (uint32_t)(f < 0 ? -f : f) >> 2;
            if (rb && !(j & 3u)) { int32_t r = gd_rd(d, gd_idx(wj, rb)); ar += (uint32_t)(r < 0 ? -r : r); }
        }
        if (d->gc != d->gct) d->gc += d->gc < d->gct ? 1 : -1;   /* (a step a sample: 0 .. +15.6 dB in 0.46 s) */
        if ((uint32_t)(f < 0 ? -f : f) > al) al = (uint32_t)(f < 0 ? -f : f);
        if (d->gc != 4096) f = (f >> 12) * d->gc + (((f & 4095) * d->gc) >> 12);
        d->lp = op15(d->lp, f, ta, &d->lpe);
        f = d->lp;
        if (f > GD_KNEE) f = GD_KNEE + tx_fx_softclip((f - GD_KNEE) >> 6) * 64;
        else if (f < -GD_KNEE) f = -GD_KNEE - tx_fx_softclip((-f - GD_KNEE) >> 6) * 64;
#if GD_DIFF
        if (!dry || d->apw) {                           /* (GRAIN 0: the echo alone, exactly; the allpasses fade in and
                                                         * out with the grains, 23 ms, from clean lines) */
            int32_t y;
            if (!d->apw) { uint32_t k; for (k = 0; k < GD_APSUM; k++) d->ap[k] = 0; ai[0] = ai[1] = ai[2] = ai[3] = 0; }
            y = gd_diffuse(d->ap, ai, f);
            d->apw += dry ? (d->apw > 32 ? -32 : -d->apw) : (d->apw < 32768 - 32 ? 32 : 32768 - d->apw);
            f = d->apw == 32768 ? y : f + mq15z(y - f, d->apw);
        }
#endif
        if (dfq) { fq += dfq; fbk = fq >> 8; }
        f = mq15z(f, fbk);
        d->dcy = op15(d->dcy, f, 70, &d->dce);         /* (15 Hz: what the grains' crossover leaves under it
                                                         * never builds up pass on pass) */
        f -= d->dcy;
        if (d->wg != wgt) d->wg += d->wg < wgt ? (wgt - d->wg > 74 ? 74 : wgt - d->wg) : -(d->wg - wgt > 74 ? 74 : d->wg - wgt);
#if GD_DIFF
        {                                               /* SYNC: the input GD_SYNCC samples later (gd_target); the
                                                         * pre-delay runs all the time the delay is on, and the line
                                                         * crossfades between it and the direct input over 23 ms when
                                                         * the compensation comes on or goes off (as gd_tap's tap does:
                                                         * no stretch of silence, no dropped stretch, into the repeats) */
            int32_t o = d->pre[pi];
            d->pre[pi] = x;
            pi = pi + 1u >= GD_SYNCC ? 0u : pi + 1u;
            if (d->pw != pwt) d->pw += d->pw < pwt ? 32 : -32;
            if (d->pw) x = d->pw == 32768 ? o : x + mq15(o - x, d->pw);
        }
#endif
        x += f;
        if (d->wg != 32768) {                          /* FREEZE: the line keeps what it had here */
            int32_t o = fj >= TX_GD_N ? gd_rd(d, wj) : 0;
            x = o + mq15(x - o, d->wg);
        }
        d->zrun = x ? 0 : d->zrun + 1u;
        sl[j] = x;
        wj = wj + 1u >= TX_GD_N ? 0u : wj + 1u;
        if (fj < TX_GD_N) fj++;
        e[j] = wl; sr[j] = wr;                          /* (the delay's out, for the blend below) */
    }
#if GD_DIFF
    d->api[0] = (uint16_t)ai[0]; d->api[1] = (uint16_t)ai[1]; d->api[2] = (uint16_t)ai[2]; d->api[3] = (uint16_t)ai[3];
    d->prei = (uint16_t)pi;
#endif
    }
    TX_FAR(tx_fx_drywet)(L, R, e, sr, n, gc[0], gc[1], gs[0], gs[1], 2);   /* dry x its gain + the delay x the wet's (Q22 -> Q23) */
    gd_wrn(d, sl, n);
    if (dry || !cw) d->gct = 4096;                      /* the make-up: the echo's level over the loop's (held while the
                                                         * loop is too quiet to tell), its share by FEEDBACK */
    else {
        uint32_t a, b;
        if (rb) {                                       /* (the reference: the echo .. the grains' place, as GRAIN) */
            int32_t dv = (int32_t)ar - (int32_t)ae;
            ae = (uint32_t)((int32_t)ae + (dv >> 15) * d->gbl + (((dv & 32767) * d->gbl) >> 15));
        }
        d->sae += ((int32_t)ae - d->sae) >> 7;          /* (~1.5 s: longer than SPRAY's reach) */
        d->saf += ((int32_t)af - d->saf) >> 7;
        a = (uint32_t)d->sae; b = (uint32_t)d->saf;
        if (b >= GD_MKFLOOR) {
            while (a >= 1u << 19) { a >>= 1; b >>= 1; }
            a = (a << 12) / (b ? b : 1u);
            a = a < 4096u ? 4096u : a > 23740u ? 23740u : a;
            d->gcr = (int32_t)((a * 4239u) >> 12);      /* (+0.3 dB: the measure reads the loss a little short) */
            d->gct = 4096 + (((d->gcr - 4096) * cw) >> 12);
        } else d->gct = 4096;                           /* (under -100 dBFS none: there the grains' products floor
                                                         * to nothing, the loss reads huge, and a residue made up
                                                         * would never end; a tail dies to exact zeros as before) */
    }
    if ((int32_t)al > d->slv) d->slv = (int32_t)al;     /* (the peak: at once up, back down over ~3 s: longer than
                                                         * TIME's top, a sparse loop's peaks are held between them) */
    else d->slv -= (d->slv - (int32_t)al) >> 9;
    {   /* the wall: the level over the wall's (Q12) -> the share past 1 (0.5: all of it, 1: none, 1.25 and over: -0.75 dB
         * a pass) */
        int32_t r = d->slv / (GD_WALL >> 12), h = 8192 - 2 * r;
        d->hot = h > 4096 ? 4096 : h < -2048 ? -2048 : h;
    }
    d->act = (uint8_t)(n ? (d->vact + n / 2u) / n : 0);
    if (!mv && !d->mix) d->on = 0;
}
TX_RAM TX_AUDIO_CODE void tx_fx_grain_process(tx_fx_grain_t *d, int32_t *L, int32_t *R, unsigned n)
{
    if (TX_FAR(tx_fx_grain_begin)(d, n)) tx_fx_grain_voices(d, n);   /* (begin in flash) */
    tx_fx_grain_end(d, L, R, n);
}
