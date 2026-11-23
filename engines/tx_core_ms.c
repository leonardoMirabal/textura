/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura M/S core: the mix's middle and sides, after the reverb (tx_core_ms.h). */
#include "tx_core_ms.h"
#include "tx_core_math.h"

const tx_param_t tx_ms_params[TX_MS_NP] = {
    {"MID", 127, TX_MS_0DB, TX_TEXT("the middle (what both sides share): -inf, -57 .. +6 dB (115: 0 dB, no change)")},
    {"SIDE", 127, TX_MS_0DB, TX_TEXT("the sides (what differs left to right): -inf, -57 .. +6 dB (115: 0 dB)")},
    {"WIDTH", 100, TX_MS_W100, TX_TEXT("0 .. 200 %: 0 mono, 50 (100 %) as it is, 100 twice as wide: the sides after SIDE")},
    {"MONO", 1, 0, TX_TEXT("the sides off, to hear the mix in mono: off / on")},
};

#define MS_CLAMP (1 << 27)              /* (|L|, |R| <= 16 full scale: L + R and the products below stay in 32 bits) */
#define MS_SNAP 8                       /* a glide this near its gain: there */

int32_t tx_ms_gain(unsigned v)
{
    if (!v) return 0;
    if (v > 127u) v = 127u;
    return (int32_t)tx_exp2((14 << 16) + ((int32_t)v - (int32_t)TX_MS_0DB) * 5443);   /* 16384 x 2^(dB / 6.02): 5443 =
                                                 * 0.5 dB as log2, Q16 (127: 32690, +6 dB) */
}

void tx_ms_init(tx_ms_t *m)
{
    unsigned i;
    tx_zero(m, sizeof *m);
    for (i = 0; i < TX_MS_NP; i++) m->p[i] = tx_ms_params[i].def;
    m->gm = m->gs = m->tm = m->ts = TX_MS_UNITY;
    m->corr = 32767;
}

static void ms_targets(tx_ms_t *m)
{
    m->tm = tx_ms_gain(m->p[TX_MS_MID]);
    m->ts = m->p[TX_MS_MONO] ? 0 : (tx_ms_gain(m->p[TX_MS_SIDE]) * (int32_t)m->p[TX_MS_WIDTH] + (int32_t)TX_MS_W100 / 2) /
                                   (int32_t)TX_MS_W100;   /* x WIDTH / 100 % (50: exactly SIDE's; 32-bit: < 2^22) */
}

void tx_ms_param(tx_ms_t *m, unsigned id, unsigned v)
{
    if (id >= TX_MS_NP) return;
    m->p[id] = (uint8_t)(v > tx_ms_params[id].max ? tx_ms_params[id].max : v);
    ms_targets(m);
}

static int32_t ms_glide(int32_t g, int32_t t)   /* the gain at the block's end */
{
    int32_t d = t - g;
    if (d <= MS_SNAP && d >= -MS_SNAP) return t;
    return g + (d >= 0 ? (d + 3) >> 2 : -((-d + 3) >> 2));
}
/* x times a Q15 gain (|x| <= 2^28, g <= 65380: SIDE +6 dB at WIDTH 200 %): 32-bit products (2^13 x 2^16; the low
 * part 32767 x 65380 < 2^32 unsigned) */
#define MS_MUL(x, g) (((x) >> 15) * (g) + (int32_t)((((uint32_t)(x) & 32767u) * (uint32_t)(g)) >> 15))
#define MS_ABS(x) ((x) < 0 ? -(x) : (x))
#define MS_SQ(x) (((x) >> 13) > 2047 ? 2047 * 2047 : ((x) >> 13) < -2047 ? 2047 * 2047 : ((x) >> 13) * ((x) >> 13))

/* the block's meters taken (peaks, energies of M and S: each sample's M and S at 2^-13, clamped, so the sums of a
 * 256-frame block stay under 2^31) */
static void ms_meter_end(tx_ms_t *m, const int32_t *pk, uint32_t em, uint32_t es)
{
    unsigned i;
    uint32_t sum = em + es;
    for (i = 0; i < 4u; i++) { int32_t h = m->pk[i] - (m->pk[i] >> 3) - 1; m->pk[i] = pk[i] > h ? pk[i] : h > 0 ? h : 0; }
    if (sum >= 16u) {
        int32_t d = (int32_t)em - (int32_t)es, c;
        while (sum >= 65536u) { sum >>= 1; d >>= 1; }            /* (32-bit: d x 32767 < 2^31) */
        c = d * 32767 / (int32_t)(sum ? sum : 1u);
        m->corr = c - m->corr < 4 && m->corr - c < 4 ? c : m->corr + (c - m->corr) / 4;   /* (near: there) */
    }
}

TX_AUDIO_CODE void tx_ms_process(tx_ms_t *m, int32_t *L, int32_t *R, unsigned n)
{
    int32_t gm = m->gm, gs = m->gs, nm, ns, pk[4] = {0, 0, 0, 0};
    uint32_t em = 0, es = 0;
    unsigned j;
    if (!n) return;
    nm = ms_glide(gm, m->tm); ns = ms_glide(gs, m->ts);
    if (gm == TX_MS_UNITY && gs == TX_MS_UNITY && nm == TX_MS_UNITY && ns == TX_MS_UNITY) {   /* 0 dB, MONO off:
                                                 * untouched; the meters only when asked */
        if (!m->meter) return;
        for (j = 0; j < n; j++) {
            int32_t l = L[j], r = R[j], a, b;
            l = l > MS_CLAMP ? MS_CLAMP : l < -MS_CLAMP ? -MS_CLAMP : l;
            r = r > MS_CLAMP ? MS_CLAMP : r < -MS_CLAMP ? -MS_CLAMP : r;
            a = (l + r) >> 1; b = (l - r) >> 1;
            if (MS_ABS(l) > pk[0]) pk[0] = MS_ABS(l);
            if (MS_ABS(r) > pk[1]) pk[1] = MS_ABS(r);
            if (MS_ABS(a) > pk[2]) pk[2] = MS_ABS(a);
            if (MS_ABS(b) > pk[3]) pk[3] = MS_ABS(b);
            em += (uint32_t)MS_SQ(a); es += (uint32_t)MS_SQ(b);
        }
        ms_meter_end(m, pk, em, es);
        return;
    }
    {
        int32_t dm = (nm - gm) * 256 / (int32_t)n, ds = (ns - gs) * 256 / (int32_t)n;   /* the ramps, Q8 a sample */
        int32_t am = gm * 256, as = gs * 256;
        for (j = 0; j < n; j++) {
            int32_t l = L[j], r = R[j], cm, cs, a, b;
            am += dm; as += ds;
            cm = j + 1u == n ? nm : am >> 8;                     /* (the block's last sample: the gain exactly) */
            cs = j + 1u == n ? ns : as >> 8;
            l = l > MS_CLAMP ? MS_CLAMP : l < -MS_CLAMP ? -MS_CLAMP : l;
            r = r > MS_CLAMP ? MS_CLAMP : r < -MS_CLAMP ? -MS_CLAMP : r;
            a = MS_MUL(l + r, cm); b = MS_MUL(l - r, cs);       /* M x gain, S x gain (the halves in the gains) */
            L[j] = a + b; R[j] = a - b;
            if (m->meter) {
                if (MS_ABS(L[j]) > pk[0]) pk[0] = MS_ABS(L[j]);
                if (MS_ABS(R[j]) > pk[1]) pk[1] = MS_ABS(R[j]);
                if (MS_ABS(a) > pk[2]) pk[2] = MS_ABS(a);
                if (MS_ABS(b) > pk[3]) pk[3] = MS_ABS(b);
                em += (uint32_t)MS_SQ(a); es += (uint32_t)MS_SQ(b);
            }
        }
    }
    m->gm = nm; m->gs = ns;
    if (m->meter) ms_meter_end(m, pk, em, es);
}
