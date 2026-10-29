/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura CHAMBER: body modes + a short FDN tail (tx_core_chamber.h). Our own design: no code taken from any reverb
 * (Felucca's hall, reused in Frodhes, is GPL code of another author: ideas only, CLAUDE.md). */
#include "tx_core_chamber.h"

const tx_param_t tx_chamber_params[TX_CH_NP] = {
    {"SIZE", 127, 76, "room size L: 0.2 .. 3 m (log): body modes c/2L = 860 .. 57 Hz, tail density and length"},
    {"ABSORB", 127, 50, "wall absorption a: 0.02 (stone cave) .. 0.6 (soft) (log): T60 = 0.0268 L / a, duller"},
    {"BODY", 127, 40, "the enclosure's resonance (its lowest modes): none .. strong"},
    {"TAIL", 127, 50, "the diffuse tail (reverberation): none .. wet"},
};

/* line lengths for L = 1 m (primes, ~2.9 .. 4.5 ms: the mean free path 4V/S = 0.67 L sets the scale) */
static const uint16_t BASE_LEN[TX_CH_LINES] = { 127, 137, 149, 157, 163, 173, 191, 199 };
static const uint16_t MODE_R[TX_CH_MODES] = { 1024, 1290, 1628, 2048 };   /* 1 : 1.26 : 1.59 : 2, Q10 */
static const uint16_t MODE_G[TX_CH_MODES] = { 1024, 819, 717, 614 };     /* Q11 (ours: -6 dB at the default, by render) */

static uint32_t isqrt_u(uint32_t x)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) { if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return r;
}

#define LIM (1 << 22)              /* lines saturate at +-4.0 (Q20) */
#define MUL15(a, b) ((((a) >> 15) * (b)) + ((((a) & 0x7FFF) * (b)) >> 15))   /* a x b / 2^15, |a| < 2^24, 0 <= b < 2^15 */

static void setup(tx_chamber_t *c)
{
    uint32_t L_mm = tx_knob_exp(c->p[TX_CH_SIZE], 200u, 255852);           /* 200 .. 3000 mm (3.9 oct) */
    uint32_t a_q16 = tx_knob_exp(c->p[TX_CH_ABSORB], 1311u, 321612);       /* 0.02 .. 0.6, Q16 (4.9 oct) */
    uint32_t t60, f1, i, maxlen = 0;
    int changed = 0;
    /* Sabine for a cube: T60 = 0.161 V / (S a) = 0.0268 L / a (ms: 26.8 L_mm / a) */
    t60 = L_mm * 110u / ((a_q16 >> 4) ? (a_q16 >> 4) : 1u);      /* 0.0268 L_mm / a ms = L_mm x 1756 / a_q16 */
    t60 = t60 < 30u ? 30u : t60 > 4000u ? 4000u : t60;
    c->t60_ms = t60;
    for (i = 0; i < TX_CH_LINES; i++) {                      /* lengths ~ L: 4.5 ms per metre */
        uint32_t len = (BASE_LEN[i] * L_mm + 500u) / 1000u;
        len = len < 23u ? 23u : len > TX_CH_LMAX ? TX_CH_LMAX : len;
        len |= 1u;
        if (len > TX_CH_LMAX) len -= 2u;
        if (len != c->len[i]) changed = 1;
        c->len[i] = (uint16_t)len;
        if (len > maxlen) maxlen = len;
        /* per-line gain for T60: 2^(-9.966 len / (fs T60)) = 2^(-len x 9.966 x 1000 / 44100 / T60_ms), Q16 exp */
        {
            uint32_t x = len * 14810u / t60;                     /* 9.966 / (44.1 T60_ms) x 2^16 per sample */
            uint32_t g = x >= (15u << 16) ? 0u : tx_exp2((15 << 16) - (int32_t)x);   /* Q15 */
            c->g[i] = (int32_t)((g * 11585u) >> 15);         /* x 1 / sqrt(8): the Hadamard's gain */
        }
    }
    if (changed) {                                           /* new lengths: the old content is meaningless */
        uint32_t k, j;
        for (k = 0; k < TX_CH_LINES; k++) { for (j = 0; j < TX_CH_LMAX; j++) c->line[k][j] = 0; c->pos[k] = 0; c->lp[k] = 0; }
    }
    /* high-frequency loss in the loop: cutoff 12 kHz x sqrt(0.02 / a), 1.5 .. 16 kHz (ours) */
    {
        uint32_t fc = (12000u * isqrt_u(85917696u / a_q16)) >> 8;   /* sqrt(0.02 / a) = sqrt(1311 / a_q16), Q8 */
        fc = fc < 1500u ? 1500u : fc > 16000u ? 16000u : fc;
        c->damp = 32768 - (int32_t)tx_exp2((15 << 16) - (int32_t)(fc * 862u >> 6));
    }
    /* body: axial modes c / 2L, Q = pi f T60 / 6.91 */
    f1 = 171500u / L_mm;                                     /* 343 / 2L Hz */
    {
        uint32_t qx16 = 0;
        for (i = 0; i < TX_CH_MODES; i++) {
            uint32_t hz = (f1 * MODE_R[i]) >> 10, q;
            q = (uint32_t)(((uint64_t)hz * t60 * 29798u) >> 16) / 1000u;   /* pi / 6.91 = 0.4547 = 29798 / 2^16 */
            if (!i) qx16 = q * 16u;
            c->bf[i] = tx_svf_coef(hz);
            c->bgin[i] = (int32_t)tx_svf_gin_noise(hz, qx16 < 32u ? 32u : qx16);
            c->bg[i] = MODE_G[i];
        }
        qx16 = qx16 < 32u ? 32u : qx16 > 16000u ? 16000u : qx16;      /* Q 2 .. 1000, shared (constant-T60 law: */
        c->bq = (int32_t)((16u << 14) / qx16);                          /* the first mode's) */
    }
    (void)maxlen;
}

void tx_chamber_init(tx_chamber_t *c)
{
    uint8_t *b = (uint8_t *)c;
    unsigned i;
    tx_zero(b, sizeof *c);
    for (i = 0; i < TX_CH_NP; i++) c->p[i] = tx_chamber_params[i].def;
    c->body_mix = c->p[TX_CH_BODY] * 258;
    c->tail_mix = c->p[TX_CH_TAIL] * 258;
    setup(c);
}

void tx_chamber_param(tx_chamber_t *c, unsigned id, unsigned v)
{
    if (id >= TX_CH_NP) return;
    if (v > tx_chamber_params[id].max) v = tx_chamber_params[id].max;
    c->p[id] = (uint8_t)v;
    if (id == TX_CH_BODY) c->body_mix = (int32_t)v * 258;
    else if (id == TX_CH_TAIL) c->tail_mix = (int32_t)v * 258;
    else setup(c);
}

TX_RAM TX_AUDIO_CODE void tx_chamber_process(tx_chamber_t *c, int32_t *buf, unsigned n)
{
    int32_t *t = c->tmp, any = 0, damp = c->damp, tm = c->tail_mix;
    unsigned j, k;
    if (n > TX_BLOCK) n = TX_BLOCK;
    for (j = 0; j < n; j++) any |= buf[j];
    if (!any && !c->active) return;                          /* silent in, silent state: nothing to do */
    c->active = 1;
    /* body (on the dry input) */
    for (j = 0; j < n; j++) t[j] = 0;
    if (c->body_mix) {
        int32_t g[TX_CH_MODES];
        for (k = 0; k < TX_CH_MODES; k++) g[k] = (c->bg[k] * (c->body_mix >> 4)) >> 11;   /* Q11 */
        tx_svf_bank_add_g_ram(c->body, c->bf, c->bq, c->bgin, g, TX_CH_MODES, buf, t, n);
    }
    /* tail: input -> allpass -> into every line (alternate signs); lines -> low-pass -> gain -> Hadamard (Q20) */
    if (tm) {
        int32_t v[TX_CH_LINES], tq = tm >> 3;                /* Q12 */
        for (j = 0; j < n; j++) {
            int32_t x = buf[j] >> 3, y, w, s0, s1, s2, s3, s4, s5, s6, s7, o;
            x = x > LIM ? LIM : x < -LIM ? -LIM : x;
            {                                                /* input allpass, g 0.5 (shifts) */
                int32_t d = c->ap[c->appos];
                w = x + (d >> 1);
                w = w > LIM ? LIM : w < -LIM ? -LIM : w;
                y = d - (w >> 1);
                c->ap[c->appos] = w;
                c->appos = (uint16_t)(c->appos + 1u >= TX_CH_AP ? 0u : c->appos + 1u);
            }
            for (k = 0; k < TX_CH_LINES; k++) {
                int32_t r = c->line[k][c->pos[k]];
                c->lp[k] += MUL15(r - c->lp[k], damp);
                v[k] = MUL15(c->lp[k], c->g[k]);
            }
            /* Hadamard 8: three butterfly stages (the 1 / sqrt(8) is in g) */
            s0 = v[0] + v[1]; s1 = v[0] - v[1]; s2 = v[2] + v[3]; s3 = v[2] - v[3];
            s4 = v[4] + v[5]; s5 = v[4] - v[5]; s6 = v[6] + v[7]; s7 = v[6] - v[7];
            v[0] = s0 + s2; v[2] = s0 - s2; v[1] = s1 + s3; v[3] = s1 - s3;
            v[4] = s4 + s6; v[6] = s4 - s6; v[5] = s5 + s7; v[7] = s5 - s7;
            s0 = v[0] + v[4]; s4 = v[0] - v[4]; s1 = v[1] + v[5]; s5 = v[1] - v[5];
            s2 = v[2] + v[6]; s6 = v[2] - v[6]; s3 = v[3] + v[7]; s7 = v[3] - v[7];
            o = 0;
            {
                int32_t in = y >> 1, nv[TX_CH_LINES];
                nv[0] = s0 + in; nv[1] = s1 - in; nv[2] = s2 + in; nv[3] = s3 - in;
                nv[4] = s4 + in; nv[5] = s5 - in; nv[6] = s6 + in; nv[7] = s7 - in;
                for (k = 0; k < TX_CH_LINES; k++) {
                    int32_t q = nv[k] > LIM ? LIM : nv[k] < -LIM ? -LIM : nv[k];
                    c->line[k][c->pos[k]] = q;
                    c->pos[k] = (uint16_t)(c->pos[k] + 1u >= c->len[k] ? 0u : c->pos[k] + 1u);
                    o += (k & 1u) ? -c->lp[k] : c->lp[k];
                }
            }
            t[j] += ((o >> 7) * tq) >> 2;                    /* Q20 sum x tail mix -> Q23 (|o| < 2^25: < 2^31) */
        }
    }
    for (j = 0; j < n; j++) buf[j] += t[j] > (1 << 25) ? (1 << 25) : t[j] < -(1 << 25) ? -(1 << 25) : t[j];   /* wet <= 4 FS */
    /* silence: the input silent for 2 T60 + the longest line (the tail is then under -120 dB: truncation toward zero
     * has emptied the lines) and the body rung out -> clear, idle. (The output alone cannot tell: it is one
     * projection of the 8 lines and can read zero while they still ring.) */
    if (!any) {
        c->quiet_blocks += n;
        if (c->quiet_blocks > c->t60_ms * 88u + TX_CH_LMAX && tx_svf_bank_quiet(c->body, TX_CH_MODES)) {
            unsigned i, m;
            for (i = 0; i < TX_CH_LINES; i++) { for (m = 0; m < TX_CH_LMAX; m++) c->line[i][m] = 0; c->lp[i] = 0; }
            for (m = 0; m < TX_CH_AP; m++) c->ap[m] = 0;
            c->active = 0;
            c->quiet_blocks = 0;
        }
    } else
        c->quiet_blocks = 0;
}
