/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura FILTER (a lane's insert): Frodhes' FILTER and its LFO (fx.c fx_filter: our own) as an instance. A TPT
 * state-variable stage, stable at any setting. TYPE: OFF, LP12, LP18, HP12, HP18, BP, NOTCH (12 dB / octave = the
 * resonant stage; 18 = it and a 6 dB one-pole). FREQ 30 Hz .. 16 kHz (log), RESO Q 0.707 .. 20, the LFO moves FREQ by
 * up to +-4 octaves. Coefficients every 32 samples (smoothed: no zipper), 32-bit divisions only.
 * Precision: the signal enters at Q18 (the mix's Q23 / 32: Frodhes' "8x finer inside" on Q15) and leaves the same,
 * with headroom for 64x full scale; LP fully open (FREQ 127, RESO 0, no LFO) passes the lane exactly (a 46 ms glide
 * in and out of it), and TYPE OFF never touches it. */
#include "tx_fx.h"

const tx_param_t tx_fx_filter_params[TX_FL_NP] = {
    {"TYPE", 6, 0, "the filter: 0 off | 1 low-pass 12 dB | 2 low-pass 18 dB | 3 high-pass 12 dB | 4 high-pass 18 dB | 5 band-pass | 6 notch"},
    {"FREQ", 127, 127, "the cutoff: 30 Hz .. 16 kHz (log)"},
    {"RESO", 127, 0, "the resonance: Q 0.707 .. 20"},
    {"LFO", 127, 0, "the LFO's depth on the cutoff: none .. +-4 octaves"},
    {"RATE", 127, 40, "the LFO's rate: 0.5 .. 12 Hz (exponential)"},
    {"SHAPE", 5, 0, "the LFO's shape: 0 sine | 1 triangle | 2 saw up | 3 saw down | 4 square | 5 random steps"},
};

void tx_fx_filter_init(tx_fx_filter_t *f)
{
    unsigned i;
    tx_fx_memset(f, 0, sizeof *f);
    for (i = 0; i < TX_FL_NP; i++) f->p[i] = tx_fx_filter_params[i].def;
    f->lfo_rnd = 0x9E3779B9u;
    f->pos = 127 * 516;
    f->ow = 32768;
}
void tx_fx_filter_param(tx_fx_filter_t *f, unsigned id, unsigned v)
{
    if (id < TX_FL_NP) f->p[id] = (uint8_t)(v > tx_fx_filter_params[id].max ? tx_fx_filter_params[id].max : v);
}

TX_RAM static int32_t lfo_value(const tx_fx_filter_t *f)          /* +-32767 */
{
    uint32_t ph = f->lfo_ph;
    switch (f->p[TX_FL_SHAPE]) {
    case 1: return ph < 0x80000000u ? (int32_t)(ph >> 15) - 32768 : 98303 - (int32_t)(ph >> 15);
    case 2: return (int32_t)(ph >> 16) - 32768;
    case 3: return 32767 - (int32_t)(ph >> 16);
    case 4: return ph < 0x80000000u ? 32767 : -32767;
    case 5: return f->lfo_sh;
    default: return tx_fx_sine(ph);
    }
}
TX_RAM static int32_t recip27(uint32_t den)                        /* 2^43 / den (den Q16) = 1 / den in Q27, 32-bit only */
{
    uint32_t q = 0x80000000u / den, r = 0x80000000u - q * den;
    return (int32_t)((q << 12) + (r << 12) / den);
}
typedef struct { int32_t a1, a2, a3, k; } svf_t;
TX_RAM static void svf_coef(svf_t *c, int32_t g, int32_t k)        /* g, k in Q24 */
{
    c->a1 = recip27(65536u + (uint32_t)(((int64_t)g * (g + k)) >> 32));
    c->a2 = (int32_t)(((int64_t)g * c->a1) >> 24);
    c->a3 = (int32_t)(((int64_t)g * c->a2) >> 24);
    c->k = k;
}
TX_INLINE int32_t fl_cl(int32_t v) { v = v > -(1 << 24) ? v : -(1 << 24); return v < (1 << 24) ? v : 1 << 24; }
TX_INLINE int32_t svf_tick(const svf_t *c, int32_t *ic, int32_t x, uint32_t mode)   /* 0 LP 1 HP 2 BP 3 NOTCH */
{
    int32_t v3 = x - ic[1], v1, v2, kv1;
    v1 = (int32_t)(((int64_t)c->a1 * ic[0] + (int64_t)c->a2 * v3 + (1 << 26)) >> 27);   /* rounded: no DC creep */
    v2 = ic[1] + (int32_t)(((int64_t)c->a2 * ic[0] + (int64_t)c->a3 * v3 + (1 << 26)) >> 27);
    ic[0] = fl_cl(2 * v1 - ic[0]);
    ic[1] = fl_cl(2 * v2 - ic[1]);
    kv1 = (int32_t)(((int64_t)c->k * v1 + (1 << 23)) >> 24);
    return mode == 0u ? v2 : mode == 1u ? x - kv1 - v2 : mode == 2u ? kv1 : x - kv1;
}
TX_INLINE void fl_run(tx_fx_filter_t *f, int32_t *m, uint32_t n, const svf_t *c, const uint32_t mode, const int p18,
                          int32_t gp)
{
    int32_t ic[2] = {f->ic[0], f->ic[1]}, op = f->op, *e = m + n;
    for (; m < e; m++) {
        int32_t x = (*m + 16) >> 5;                                     /* Q23 -> Q18 */
        x = fl_cl(x);
        x = svf_tick(c, ic, x, mode);
        if (p18) {
            int32_t v = (int32_t)(((int64_t)(x - op) * gp + (1 << 26)) >> 27), lp = v + op;
            op = fl_cl(lp + v);
            x = mode == 0u ? lp : x - lp;
        }
        *m = x * 32;                                                    /* Q18 -> Q23 (|x| <= 2^24: no overflow) */
    }
    f->ic[0] = ic[0]; f->ic[1] = ic[1]; f->op = op;
}

TX_RAM TX_AUDIO_CODE void tx_fx_filter_process(tx_fx_filter_t *f, int32_t *m, unsigned n)
{
    static const uint8_t POLES[7] = {0, 2, 3, 2, 3, 2, 2};
    uint32_t type = f->p[TX_FL_TYPE], inc = TX_FX_LFO_INC[f->p[TX_FL_RATE]], sb, j, poles;
    int32_t k = (int32_t)TX_FX_FL_K[f->p[TX_FL_RESO]], base = f->p[TX_FL_FREQ] * 516, depth = f->p[TX_FL_LFO];
    int open;
    if (!type || type > 6u) { f->was_on = 0; f->lfo_ph += inc * n; return; }   /* off: the LFO keeps time */
    poles = POLES[type];
    open = (type == 1u || type == 2u) && f->p[TX_FL_FREQ] >= 127 && !f->p[TX_FL_RESO] && !depth;
    if (!f->was_on) { f->ic[0] = f->ic[1] = 0; f->op = 0; f->pos = base; f->was_on = 1; f->ow = open ? 0 : 32768; }
    for (sb = 0; sb < n; sb += 32u) {
        uint32_t prev = f->lfo_ph, i;
        int32_t tgt, g, fr, gp = 0;
        svf_t c;
        f->lfo_ph += inc * 32u;
        if (f->lfo_ph < prev) { f->lfo_rnd = f->lfo_rnd * 1664525u + 1013904223u; f->lfo_sh = (int32_t)(f->lfo_rnd >> 16) - 32768; }
        tgt = clampi(base + (int32_t)(((int64_t)lfo_value(f) * depth * 28932) >> 22), 0, 65535);   /* 4 octaves */
        f->pos += (tgt - f->pos) >> 2;
        i = (uint32_t)f->pos >> 8; fr = f->pos & 255;
        g = (int32_t)TX_FX_FL_G[i] + ((((int32_t)TX_FX_FL_G[i + 1] - (int32_t)TX_FX_FL_G[i]) * fr) >> 8);   /* Q24 */
        svf_coef(&c, g, k);
        if (poles == 3u) gp = (1 << 27) - recip27(65536u + ((uint32_t)g >> 8));
        j = sb + 32u < n ? sb + 32u : n;
        if (f->ow != 32768 || open) tx_fx_copy32(f->dry, m + sb, j - sb);
        switch (type) {                                  /* one loop per (mode, poles) */
        case 1: fl_run(f, m + sb, j - sb, &c, 0, 0, gp); break;
        case 2: fl_run(f, m + sb, j - sb, &c, 0, 1, gp); break;
        case 3: fl_run(f, m + sb, j - sb, &c, 1, 0, gp); break;
        case 4: fl_run(f, m + sb, j - sb, &c, 1, 1, gp); break;
        case 5: fl_run(f, m + sb, j - sb, &c, 2, 0, gp); break;
        default: fl_run(f, m + sb, j - sb, &c, 3, 0, gp); break;
        }
        if (f->ow != 32768 || open) {                    /* fully open: the lane passes untouched; a 46 ms glide */
            int32_t w1 = open ? (f->ow > 512 ? f->ow - 512 : 0) : (f->ow < 32256 ? f->ow + 512 : 32768);
            uint32_t q, len = j - sb;
            if (!f->ow && !w1) tx_fx_copy32(m + sb, f->dry, len);
            else {
                int32_t w = f->ow, dw = (w1 - f->ow) / 32;
                for (q = 0; q < len; q++) {
                    int32_t d = f->dry[q];
                    w += dw;
                    m[sb + q] = d + (int32_t)(((int64_t)(m[sb + q] - d) * w) >> 15);
                }
            }
            f->ow = w1;
        }
    }
}
