/* SPDX-License-Identifier: GPL-3.0-only */
/* The plugin's oversampler (plugin/src/bridge.c): the app's 44.1 kHz stream to the host's rate in integer-ratio
 * linear-phase polyphase stages, in float (the plugin's own code: the app stays integer).
 *
 *   44.1 kHz             the samples themselves
 *   88.2 / 176.4 / 352.8 one, two, three 2x half-band stages
 *   48 / 96 / 192 / 384  a 160/147 polyphase stage to 48 kHz, then none to three 2x half-band stages
 *   any other rate       none here (bridge.c keeps its windowed-sinc resampler for them)
 *
 * The targets (tools/plugin_os.py measures them): passband 0 .. 20 kHz flat within +-0.05 dB, every image and alias of
 * a tone up to 20 kHz at least 100 dB under it. Each filter is a Kaiser-windowed sinc designed for TXP_OS_ATT dB:
 *   the 160/147 stage   pass 20 kHz, stop 24 kHz (48 kHz's Nyquist: nothing folds back), TXP_OS_PT taps a phase
 *   a half-band stage   pass 20 kHz, stop (its input rate - 20 kHz) for the first (44.1 -> 88.2: 24.1 kHz; 48 -> 96:
 *                       28 kHz), and pass 24.1 / 28 kHz, stop (input rate - 24.1 / 28 kHz) for the later ones (their
 *                       input holds nothing between the first stage's stop band and its Nyquist)
 * The 44.1 kHz app has content up to 22.05 kHz: between 20 kHz and its Nyquist the first stage rolls it off (its
 * transition band), and those tones' images land between 22.05 and 24 kHz (44.1 family: 24.1 kHz) partly damped:
 * above the audio band, as on any 44.1 kHz source.
 *
 * Time. A stage's output frame t stands for its input at a fixed delay (its centre tap): a half-band stage of 4K + 1
 * taps K input frames (2K output frames); the 160/147 stage of TXP_OS_PT x 160 - 1 taps (its centre on a whole
 * high-rate sample, DH) reads the high-rate sample 147 t + phi for output t. txp_os_init places the chain so that
 * output frame T is the app's frame (T - lat) x 44100 / rate exactly (pre zero frames before the app's first, phi),
 * lat the latency asked for or the next one the chain can place exactly. */
#ifndef TEXTURA_PLUGIN_OS_H
#define TEXTURA_PLUGIN_OS_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TXP_OS_ATT 110.0                 /* dB, the 160/147 stage's design stop band (the measured gate: 100) */
#define TXP_OS_HBATT 125.0               /* dB, the half-bands' (their short windows fall short of the design) */
#define TXP_OS_PASS 20000.0              /* Hz: the passband's edge */
#define TXP_OS_L 160u                    /* 44.1 -> 48 kHz: 160 / 147 */
#define TXP_OS_M 147u
#define TXP_OS_PT 80u                    /* taps a phase (the prototype: 80 x 160 - 1) */
#define TXP_OS_DH ((TXP_OS_PT * TXP_OS_L - 2u) / 2u)   /* its centre: 6399 high-rate samples */
#define TXP_OS_HBMAX 3                   /* half-band stages at most (x8) */
#define TXP_OS_HBK 64u                   /* a half-band's K at most (4K + 1 taps) */
#define TXP_OS_RING 256u                 /* a stage's input history (power of 2, >= the longest window) */

typedef struct {                         /* one 2x half-band stage */
    uint32_t k;                          /* 4K + 1 taps: the odd phase's 2K taps, the even phase a delay of K */
    float h[2u * TXP_OS_HBK];            /* the odd phase, h[1], h[3], ... reversed: h[2K - 1 - j] multiplies x[m - j]
                                          * (oldest first, so the window reads forwards) */
    float xl[2u * TXP_OS_RING], xr[2u * TXP_OS_RING];   /* the history, written twice (a window never wraps) */
    uint32_t w;                          /* inputs written */
    int odd;                             /* the next output is the odd one */
} txp_hb_t;

typedef void (*txp_os_src_fn)(void *ctx, float *l, float *r);   /* the app's next frame (x 1/32768) */

typedef struct {
    int fam;                             /* 0 identity (44.1 kHz), 1 the 44.1 family, 2 the 48 family */
    int nhb;
    txp_hb_t hb[TXP_OS_HBMAX];
    /* the 160/147 stage */
    float xl[2u * TXP_OS_RING], xr[2u * TXP_OS_RING];
    uint32_t w;                          /* its inputs written (pre zeros, then the app's frames) */
    uint64_t t;                          /* its outputs made */
    uint32_t phi;
    uint32_t pre;                        /* zero frames still to feed before the app's first */
    uint32_t lat;                        /* output frames: the app's frame (T - lat) x 44100 / rate at output T */
    txp_os_src_fn src;
    void *ctx;
} txp_os_t;

static float txp_os_poly[TXP_OS_L][TXP_OS_PT];   /* the 160/147 prototype by phase: [p][j] multiplies x[newest - j] */
static int txp_os_poly_made;

static double txp_os_i0(double x)
{
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 60; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; if (t < 1e-20 * s) break; }
    return s;
}
static double txp_os_beta(double a) { return a > 50.0 ? 0.1102 * (a - 8.7) : 0.5842 * pow(a - 21.0, 0.4) + 0.07886 * (a - 21.0); }
/* the windowed sinc at n - c (taps 0 .. 2c), cutoff fc (cycles a sample), gain g */
static double txp_os_tap(double n, double c, double fc, double g, double beta)
{
    double x = n - c, u = x / (c + 1.0), s = x == 0.0 ? 2.0 * fc : sin(2.0 * M_PI * fc * x) / (M_PI * x);
    if (u <= -1.0 || u >= 1.0) return 0.0;
    return g * s * txp_os_i0(beta * sqrt(1.0 - u * u)) / txp_os_i0(beta);
}
static void txp_os_make_poly(void)      /* once (the caller serialises: bridge.c pthread_once) */
{
    const double fs = 44100.0 * TXP_OS_L, fc = (TXP_OS_PASS + 24000.0) / 2.0 / fs, beta = txp_os_beta(TXP_OS_ATT);
    const double c = (double)TXP_OS_DH;
    if (txp_os_poly_made) return;
    for (uint32_t p = 0; p < TXP_OS_L; p++)
        for (uint32_t j = 0; j < TXP_OS_PT; j++) {
            uint32_t n = p + TXP_OS_L * j;
            txp_os_poly[p][j] = n > 2u * TXP_OS_DH ? 0.0f : (float)txp_os_tap((double)n, c, fc, (double)TXP_OS_L, beta);
        }
    txp_os_poly_made = 1;
}
/* a half-band stage for input rate fin: pass edge fp, stop edge fin - fp (symmetric about fin / 2) */
static void txp_hb_init(txp_hb_t *s, double fin, double fp)
{
    const double fout = 2.0 * fin, dw = 2.0 * M_PI * (fin - 2.0 * fp) / fout, beta = txp_os_beta(TXP_OS_HBATT);
    uint32_t n = (uint32_t)ceil((TXP_OS_HBATT - 7.95) / (2.285 * dw)) + 1u, k = (n + 2u) / 4u, j;
    if (k < 2u) k = 2u;
    if (k > TXP_OS_HBK) k = TXP_OS_HBK;
    memset(s, 0, sizeof *s);
    s->k = k;
    for (j = 0; j < 2u * k; j++)         /* tap 2j + 1 of 4K + 1 (centre 2K), cutoff fout / 4, gain 2 */
        s->h[2u * k - 1u - j] = (float)txp_os_tap(2.0 * j + 1.0, 2.0 * k, 0.25, 2.0, beta);
}
static uint32_t txp_hb_delay(const txp_hb_t *s) { return 2u * s->k; }   /* output frames */

/* the family of a rate: 1 / 2 with nhb set, 0 identity, -1 not ours */
static int txp_os_family(double rate, int *nhb)
{
    for (int h = 0; h <= TXP_OS_HBMAX; h++) {
        double m = (double)(1u << h);
        if (rate == 44100.0 * m) { *nhb = h; return h ? 1 : 0; }
        if (rate == 48000.0 * m) { *nhb = h; return 2; }
    }
    return -1;
}
/* set up for rate (txp_os_family >= 0); lat_min: the least latency wanted (output frames, the chain's delay
 * included). Returns the latency placed: lat_min or the next the chain can place exactly */
static uint32_t txp_os_init(txp_os_t *o, double rate, uint32_t lat_min, txp_os_src_fn src, void *ctx)
{
    int nhb = 0, fam = txp_os_family(rate, &nhb);
    uint32_t m, dhb = 0;
    memset(o, 0, sizeof *o);
    o->fam = fam; o->nhb = nhb; o->src = src; o->ctx = ctx;
    {
        double fin = fam == 2 ? 48000.0 : 44100.0, edge = fam == 2 ? 28000.0 : 44100.0 - TXP_OS_PASS;   /* 24.1 / 28 kHz */
        for (int i = 0; i < nhb; i++, fin *= 2.0) txp_hb_init(&o->hb[i], fin, i == 0 ? TXP_OS_PASS : edge);
    }
    if (fam == 2) txp_os_make_poly();
    m = 1u << nhb;
    for (int i = 0; i < nhb; i++) dhb = 2u * dhb + txp_hb_delay(&o->hb[i]);   /* the half-bands' delay at the output */
    if (fam != 2) {                      /* lat = m pre + dhb */
        uint32_t pre = lat_min > dhb ? (lat_min - dhb + m - 1u) / m : 0u;
        o->pre = pre; o->lat = m * pre + dhb;
    } else {                             /* lat = m l48 + dhb; 147 l48 = DH + 160 pre - phi, 0 <= phi < 160 */
        uint32_t l48 = lat_min > dhb ? (lat_min - dhb + m - 1u) / m : 0u, pre;
        if (147u * l48 < TXP_OS_DH) l48 = (TXP_OS_DH + 146u) / 147u;
        pre = (147u * l48 - TXP_OS_DH + TXP_OS_L - 1u) / TXP_OS_L;
        o->phi = TXP_OS_DH + TXP_OS_L * pre - 147u * l48;
        o->pre = pre; o->lat = m * l48 + dhb;
    }
    return o->lat;
}
static void txp_os_in(txp_os_t *o, float *l, float *r)   /* the next input of the first stage */
{
    if (o->pre) { o->pre--; *l = *r = 0.0f; return; }
    o->src(o->ctx, l, r);
}
static void txp_os_base(txp_os_t *o, float *l, float *r)   /* the first stage's next output: 44.1 or 48 kHz */
{
    if (o->fam != 2) { txp_os_in(o, l, r); return; }
    {
        uint64_t u = (uint64_t)TXP_OS_M * o->t + o->phi, newest = u / TXP_OS_L;
        uint32_t p = (uint32_t)(u % TXP_OS_L), j, b;
        const float *h = txp_os_poly[p];
        float al = 0.0f, ar = 0.0f;
        while ((uint64_t)o->w <= newest) {   /* (the inputs up to the newest the window reads) */
            float a, c;
            uint32_t q = o->w & (TXP_OS_RING - 1u);
            txp_os_in(o, &a, &c);
            o->xl[q] = o->xl[q + TXP_OS_RING] = a;
            o->xr[q] = o->xr[q + TXP_OS_RING] = c;
            o->w++;
        }
        b = (uint32_t)(newest & (TXP_OS_RING - 1u)) + TXP_OS_RING;   /* x[newest - j] = x[b - j] */
        for (j = 0; j < TXP_OS_PT; j++) { al += h[j] * o->xl[b - j]; ar += h[j] * o->xr[b - j]; }
        o->t++;
        *l = al; *r = ar;
    }
}
static void txp_os_stage(txp_os_t *o, int i, float *l, float *r);
static void txp_os_from(txp_os_t *o, int i, float *l, float *r)   /* stage i's input: the stage before, or the base */
{
    if (i == 0) txp_os_base(o, l, r); else txp_os_stage(o, i - 1, l, r);
}
static void txp_os_stage(txp_os_t *o, int i, float *l, float *r)  /* half-band stage i's next output */
{
    txp_hb_t *s = &o->hb[i];
    uint32_t q, b, j, n = 2u * s->k;
    if (!s->odd) {                       /* y[2m] = x[m - K]: a new input first */
        float a, c;
        txp_os_from(o, i, &a, &c);
        q = s->w & (TXP_OS_RING - 1u);
        s->xl[q] = s->xl[q + TXP_OS_RING] = a;
        s->xr[q] = s->xr[q + TXP_OS_RING] = c;
        s->w++;
        b = ((s->w - 1u - s->k) & (TXP_OS_RING - 1u)) + TXP_OS_RING;   /* (before the first K inputs: the ring's zeros) */
        *l = s->xl[b]; *r = s->xr[b];
        s->odd = 1;
        return;
    }
    {                                    /* y[2m + 1] = sum h[2j + 1] x[m - j], j = 0 .. 2K - 1 */
        float al = 0.0f, ar = 0.0f;
        const float *xl, *xr;
        b = ((s->w - 1u) & (TXP_OS_RING - 1u)) + TXP_OS_RING - (n - 1u);   /* x[m - 2K + 1] .. x[m] */
        xl = s->xl + b; xr = s->xr + b;
        for (j = 0; j < n; j++) { al += s->h[j] * xl[j]; ar += s->h[j] * xr[j]; }
        *l = al; *r = ar;
        s->odd = 0;
    }
}
/* n output frames */
static void txp_os_render(txp_os_t *o, float *l, float *r, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        if (o->nhb) txp_os_stage(o, o->nhb - 1, &l[i], &r[i]);
        else txp_os_base(o, &l[i], &r[i]);
    }
}
#endif
