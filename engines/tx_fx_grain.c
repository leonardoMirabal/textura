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
 *   the GRAINS: GRAINS voices (1 .. 30) staggered on one clock (each grain SIZE long, Hann windows: they add up to a
 *     constant), each started at TIME + a random SPRAY, at PITCH (semitone steps, -12 .. +12), reversed by chance
 *     (REVERSE), panned at random within SPREAD; read with linear interpolation (whole samples at rate 1: exact)
 *   out = the input x cos + (ECHO .. GRAINS by GRAIN) x sin of MIX's place (tx_fx_eqp: 0 the input alone .. 127 the
 *     delay alone, equal power: level through the middle); the feedback: that, its make-up, through TONE's low-pass,
 *     a soft knee (above half scale only: the quiet repeats stay exact), FEEDBACK's gain and a DC block, into the line
 *   the make-up: the grains lose level on every pass (scattered by SPRAY and PITCH they add up in power, not in
 *     amplitude; SPREAD's places; the blend with the echo), so above FEEDBACK 64 the loop gets back what it lost: the
 *     echo's level over the loop's, both smoothed over ~1.5 s, +0.3 .. +15.6 dB (none under -100 dBFS), in fully from
 *     FEEDBACK 96; GRAIN 0 (the echo alone) needs none. FEEDBACK (TX_FX_GD_FB): to 64 as before (x 258), then a
 *     pass's gain rises to 1 at 110
 *     (the repeats go on at every GRAIN, SPRAY, SPREAD) and past it to +2.5 dB a pass at 127: the delay runs away into
 *     a wall held at a -14.6 dBFS peak (the loop as heard, before its make-up; a peak that falls back over ~3 s: 6 dB
 *     under it the whole gain past 1, at it 1, 2 dB over it -0.75 dB a pass, in proportion between; the soft knee
 *     stays the guard) that stops when FEEDBACK comes down
 *   FREEZE: the line stops taking input (a 10 ms fade): the whole line (2.55 s) circles, the grains keep reading them
 * The dynamic cap (cap, set by the chain each block): grains beyond it finish their window and do not start again, so
 * the delay costs what the effects reserve leaves (on the FM-1: ~14 us a sounding grain at rate 1, ~42 us pitched or
 * reversed, ~220 us the line, the echo and the feedback; each grain decodes its run of the line once a block).
 * Off (MIX 0, after a 46 ms fade): nothing computed, the mix untouched. A line switched on reads as empty until it has
 * been written (no stale sound, no memset). */
#include "tx_fx.h"

#define GD_GUARD 288u               /* every read this far behind the write at the block's start (a block + a gathered
                                     * one): a block's reads never see its own writes, so the grains run grain by grain */
#define GD_KNEE (1 << 21)           /* the feedback's soft knee: half of full scale (Q22) */
#define GD_MKFLOOR 2560u            /* the make-up's floor: the loop's |x| / 4 summed a block at a mean |x| of -100 dBFS */
#define GD_WALL (3 << 18)           /* FEEDBACK past 1: the loop's peak it runs up to (Q22: -14.6 dBFS; a dense wall's
                                     * rms ~-20 dBFS at MIX 127) */

const tx_param_t tx_fx_grain_params[TX_GD_NP] = {
    {"MIX", 127, 0, "dry .. wet: the sound alone (0, the delay off) .. the delay alone (127), equal power between"},
    {"TIME", TX_GD_TIME_MAX, 70, "the echo's time: 20 ms .. 1.66 s (127) .. 2.00 s (exponential); the grains read around it"},
    {"FEEDBACK", 127, 50, "how long the repeats go on (each one grained again): none .. endless (110) .. running away into a held wall"},
    {"GRAINS", TX_GD_MAXG - 1u, 3, "grain voices on the repeats: 1 .. 30 (fewer when the CPU is short)"},
    {"SIZE", 127, 64, "each grain's length: 20 .. 500 ms"},
    {"SPRAY", 127, 20, "where each grain starts: at TIME .. up to 0.5 s later, at random"},
    {"PITCH", 127, 64, "the grains' pitch: -12 .. +12 semitones (64 = as played)"},
    {"REVERSE", 127, 0, "the chance a grain plays backwards: never .. always"},
    {"SPREAD", 127, 64, "each grain's place: all in the middle .. anywhere left to right"},
    {"TONE", 127, 100, "the repeats' top: dark (400 Hz) .. open (16 kHz)"},
    {"GRAIN", 127, 64, "the clean echo (0) .. the grains (127)"},
    {"FREEZE", 1, 0, "hold the line: the last 2.55 s go round, the grains keep playing them"},
};

TX_RAM static uint32_t rnd(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s; }   /* (the grains' rng: d->rng,
                                                         * or the second half's copy, tx_fx_grain_split) */

void tx_fx_grain_init(tx_fx_grain_t *d)
{
    unsigned i;
    tx_fx_memset(d, 0, sizeof *d);                              /* (once: the line's 96 KB too) */
    for (i = 0; i < TX_GD_NP; i++) d->p[i] = (uint8_t)tx_fx_grain_params[i].def;
    d->rng = 0x2545F491u;
    d->wg = 32768;
    d->cap = TX_GD_MAXG;
    d->gc = d->gct = d->gcr = d->hot = 4096;
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
    uint32_t r = rnd(rs), back = d->tap + (uint32_t)(((uint64_t)(r >> 8) * ((uint32_t)d->p[TX_GD_SPRAY] * 174u)) >> 24);
    int32_t semi = ((int32_t)d->p[TX_GD_PITCH] - 64) * 12 / 63, rate, sp = d->p[TX_GD_SPREAD];
    int64_t lo, hi, far;
    uint32_t pr = (uint32_t)TX_FX_GD_PITCH[semi + 12];
    rate = (r & 127u) < d->p[TX_GD_REVERSE] ? -(int32_t)pr : (int32_t)pr;
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
            else if (start) gd_spawn(d, g, len, w0, j, rs);
            if (g->on) { gd_seg(d, g, ph, inc, j, 1u, sl, sr); on++; }
            ph += inc;
            j++;
        }
    }
    g->vp = ph;
    return on;
}

/* A block in three steps (tx_fx_grain_process runs them in turn): begin (MIX on: the line's start), the voices (the
 * echo's run and the grains: they read only the line as the blocks before left it, never this block's input, so
 * they may run before the input is there, on another core: tx_fx_bus_begin), the end (the blend with the input, the
 * feedback, the write). begin returns 1 when the voices may run ahead: the line is not silent (a silent line idles
 * on a silent input: that needs the input first) */
TX_AUDIO_CODE int tx_fx_grain_begin(tx_fx_grain_t *d, unsigned n)
{
    int32_t mv = d->p[TX_GD_MIX], gb = d->p[TX_GD_GRAIN] * 258, wgt = d->p[TX_GD_FREEZE] ? 0 : 32768;
    uint32_t k, tgt = d->sync ? d->sync : TX_FX_GD_TIME[d->p[TX_GD_TIME]];
    (void)n;
    d->ahead = 0;
    if (!d->on) {
        if (!mv) return 0;
        d->on = 1; d->fill = 0; d->mix = 0; d->zrun = 0; d->tap = tgt; d->tx = 0; d->lp = d->dcy = d->lpe = d->dce = 0;
        d->wg = 0;                                      /* (the line fades in, 10 ms: its first echo never starts on a
                                                         * step of the sound it caught mid-way) */
        d->gbl = gb > 32768 ? 32768 : gb;
        d->gc = d->gct = d->gcr = d->hot = 4096; d->sae = d->saf = d->slv = 0;
        for (k = 0; k < TX_GD_MAXG; k++) d->g[k].on = 0;
    }
    d->ahead = !(d->zrun >= TX_GD_N && wgt && !d->tx);
    return d->ahead;
}
/* the echo's tap: a new TIME starts its crossfade (before the voices: a grain starts TIME behind the write) */
TX_INLINE void gd_tap(tx_fx_grain_t *d)
{
    uint32_t tgt = d->sync ? d->sync : TX_FX_GD_TIME[d->p[TX_GD_TIME]];
    if (!d->tx && tgt != d->tap) { d->tap0 = d->tap; d->tap = tgt; d->tx = 1; }
}
/* the grains this block will start on voice g (2 rng draws each): its wraps on the clock, as gd_voice walks them */
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
    uint32_t G = d->p[TX_GD_GRAINS] + 1u, cap = d->cap < G ? d->cap : G, inc = TX_FX_GD_SIZE[d->p[TX_GD_SIZE]];
    uint32_t step = 0xFFFFFFFFu / G + 1u, k, K = (G + 1u) / 2u, r = d->rng, c = 0;
    if (n > TX_BLOCK) n = TX_BLOCK;
    gd_tap(d);
    for (k = 0; k < K; k++)
        if (k < G && k < cap) c += gd_spawns(&d->g[k], d->ph + k * step, inc, n);   /* (gd_voice's start) */
    for (k = 0; k < 2u * c; k++) r = r * 1664525u + 1013904223u;
    d->vk = (uint8_t)K; d->rng2 = r;
}
/* voice slots k0 .. k1 - 1 into sl, sr (0: dry), their count; rs their rng */
TX_RAM static unsigned gd_voices(tx_fx_grain_t *d, unsigned k0, unsigned k1, unsigned n, int32_t *sl, int32_t *sr, uint32_t *rs)
{
    uint32_t G = d->p[TX_GD_GRAINS] + 1u, cap = d->cap < G ? d->cap : G, inc = TX_FX_GD_SIZE[d->p[TX_GD_SIZE]];
    uint32_t len = 0xFFFFFFFFu / inc, step = 0xFFFFFFFFu / G + 1u, k, j;
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
    ok = d->tap <= d->fill;
    if (ok) gd_run(d, gd_idx(w0, d->tap), n, e);
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
    d->ph += TX_FX_GD_SIZE[d->p[TX_GD_SIZE]] * n;
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
TX_RAM TX_AUDIO_CODE void tx_fx_grain_end(tx_fx_grain_t *d, int32_t *L, int32_t *R, unsigned n)
{
    int32_t mv = d->p[TX_GD_MIX], mixt = TX_FX_MIXQ(mv), gb = d->p[TX_GD_GRAIN] * 258, fbk = TX_FX_GD_FB[d->p[TX_GD_FEEDBACK]];
    int32_t pf = d->p[TX_GD_FEEDBACK], cw = pf <= 64 ? 0 : pf >= 96 ? 4096 : (pf - 64) * 128;   /* the make-up's share, Q12 */
    uint32_t ae = 0, ar = 0, af = 0, al = 0;            /* (the block's echo, the grains' reference and the loop, |x| / 4
                                                         * summed; the loop as heard, its peak, for the wall) */
    uint32_t rb = 0;                                    /* (the reference: the line where the grains read on average,
                                                         * TIME + half of SPRAY's reach; 0 none) */
    int32_t ta = TX_FX_GD_TONE[d->p[TX_GD_TONE]], wgt = d->p[TX_GD_FREEZE] ? 0 : 32768;
    uint32_t G = d->p[TX_GD_GRAINS] + 1u, cap = d->cap < G ? d->cap : G, j, w0;
    int32_t gnt = cap ? (int32_t)(65536u / (cap < 2u ? 2u : cap)) : 0;   /* the grains' level: 2 / sounding (Q15) */
    int32_t *e = d->we, *sl = d->wl, *sr = d->wr;
    int32_t gs[2], gc[2];                               /* MIX: the wet / dry gains at the block's edges (Q15) */
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
        d->ph += TX_FX_GD_SIZE[d->p[TX_GD_SIZE]] * n;
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
    dry = !gb && !d->gbl;
    w0 = d->w;
    TX_FAR(tx_fx_mix_glide)(&d->mix, mixt, 16 * (int32_t)n, 1u, gs, gc);   /* (MIX glides: 46 ms end to end; in flash) */
    /* the blend, the feedback into the line (gathered in sl, written after), the output */
    {
    uint32_t wj = w0, fj = d->fill;                     /* (the write index and the fill, sample by sample) */
    for (j = 0; j < n; j++) {
        int32_t x = (L[j] >> 2) + (R[j] >> 2), wl, wr, f;   /* the mid, Q22 */
        if (d->gn != gnt) d->gn += d->gn < gnt ? (gnt - d->gn > 8 ? 8 : gnt - d->gn) : -(d->gn - gnt > 8 ? 8 : d->gn - gnt);
        if (dry) wl = wr = e[j];                        /* (GRAIN 0: the echo alone, exactly) */
        else {
            int32_t gl = mq15(sl[j], d->gn), gr = mq15(sr[j], d->gn);
            if (d->gbl != gb) d->gbl += d->gbl < gb ? (gb - d->gbl > 16 ? 16 : gb - d->gbl) : -(d->gbl - gb > 16 ? 16 : d->gbl - gb);
            wl = e[j] + mq15(gl - e[j], d->gbl);       /* the echo .. the grains (GRAIN glides: 46 ms end to end) */
            wr = e[j] + mq15(gr - e[j], d->gbl);
        }
        f = (wl >> 1) + (wr >> 1);                      /* the feedback: its make-up, TONE, the soft knee, FEEDBACK,
                                                         * the DC block */
        if (!dry) {
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
        f = mq15z(f, fbk);
        d->dcy = op15(d->dcy, f, 8, &d->dce);
        f -= d->dcy;
        if (d->wg != wgt) d->wg += d->wg < wgt ? (wgt - d->wg > 74 ? 74 : wgt - d->wg) : -(d->wg - wgt > 74 ? 74 : d->wg - wgt);
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
    }
    TX_FAR(tx_fx_drywet)(L, R, e, sr, n, gc[0], gc[1], gs[0], gs[1], 2);   /* dry x cos + the delay x sin (Q22 -> Q23) */
    gd_wrn(d, sl, n);
    if (dry) d->gct = 4096;                             /* the make-up: the echo's level over the loop's (held while the
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
