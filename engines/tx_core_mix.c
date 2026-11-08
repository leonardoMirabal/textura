/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura MIX core: four layers (-> their inserts) -> pan -> stereo -> the DRIVE -> the compressor -> CHAMBER (mid
 * send) -> the bus effects (grain delay, reverb) -> linked limiter (tx_core_mix.h). */
#include "tx_core_mix.h"

void tx_mix_init(tx_mix_t *m)
{
    unsigned k;
    tx_zero(m, sizeof *m);
    for (k = 0; k < TX_MIX_SLOTS; k++) { tx_mix_pan(m, k, 64); m->tg[k] = 4096; }
    tx_comp_init(&m->comp);
    tx_limit_init(&m->lim);
    m->lim.ch = 2;                                           /* (interleaved: tx_mix_end) */
}

void tx_mix_set(tx_mix_t *m, unsigned k, const tx_engine_t *e, void *st, uint32_t seed)
{
    if (k >= TX_MIX_SLOTS) return;
    m->e[k] = e; m->st[k] = st; m->slot_peak[k] = 0;
    if (e && st) e->init(st, seed);
}

void tx_mix_pan(tx_mix_t *m, unsigned k, unsigned pan)
{
    uint32_t ph;
    if (k >= TX_MIX_SLOTS) return;
    if (pan > 127u) pan = 127u;
    ph = pan == 127u ? 0x40000000u : pan * 0x810204u;       /* 0 .. pi/2 as a phase (2^30 / 127) */
    m->gr[k] = (tx_sin_q15(ph) * 23170) >> 15;               /* sqrt(2) sin, Q14: 16384 at the centre */
    m->gl[k] = (tx_sin_q15(ph + 0x40000000u) * 23170) >> 15; /* sqrt(2) cos */
}

void tx_mix_trim(tx_mix_t *m, unsigned k, unsigned q12)
{
    if (k < TX_MIX_SLOTS) m->tg[k] = (int32_t)(q12 > 65535u ? 65535u : q12);
}

TX_AUDIO_CODE void tx_mix_begin(tx_mix_t *m, unsigned n)
{
    unsigned j;
    for (j = 0; j < n; j++) m->L[j] = m->R[j] = 0;
}

TX_AUDIO_CODE void tx_mix_add(tx_mix_t *m, unsigned k, const int32_t *x, unsigned n)
{
    unsigned j;
    int32_t gl, gr, pk, tg;
    if (k >= TX_MIX_SLOTS) return;
    gl = m->gl[k]; gr = m->gr[k]; pk = m->slot_peak[k]; tg = m->tg[k];
    for (j = 0; j < n; j++) {
        int32_t v = x[j], a, hi;
        uint32_t lo;
        if (tg != 4096) {                                    /* the trim: |v| <= 8 full scale first, 32-bit products */
            v = v > (1 << 26) ? (1 << 26) : v < -(1 << 26) ? -(1 << 26) : v;
            v = (v >> 12) * tg + (int32_t)((((uint32_t)v & 4095u) * (uint32_t)tg) >> 12);
        }
        a = v < 0 ? -v : v; hi = v >> 14;
        lo = (uint32_t)v & 0x3FFFu;
        if (a > pk) pk = a;
        hi = hi > 131071 ? 131071 : hi < -131072 ? -131072 : hi;   /* |v| <= 32 full scale: products < 2^31 */
        m->L[j] += hi * gl + (int32_t)((lo * (uint32_t)gl) >> 14);
        m->R[j] += hi * gr + (int32_t)((lo * (uint32_t)gr) >> 14);
    }
    m->slot_peak[k] = pk;
}

TX_AUDIO_CODE void tx_mix_slot_render(tx_mix_t *m, unsigned k, unsigned n)
{
    unsigned j;
    if (k >= TX_MIX_SLOTS || !m->e[k]) return;
    for (j = 0; j < n; j++) m->blk[j] = 0;
    m->e[k]->render(m->st[k], m->blk, n);
    if (m->ins) m->ins(m->fx, k, m->blk, n);                 /* the slot's insert (the filter) */
    tx_mix_add(m, k, m->blk, n);
}

TX_AUDIO_CODE void tx_mix_end(tx_mix_t *m, int32_t *L, int32_t *R, unsigned n)
{
    tx_mix_run(m, n, 0u);
    tx_mix_out(m, L, R, n);
}
TX_AUDIO_CODE void tx_mix_run(tx_mix_t *m, unsigned n, unsigned c)
{
    unsigned j;
    int32_t pk = 0;
    void (*pre)(void *, int32_t *, int32_t *, unsigned) = c ? m->pre1 : m->pre;
    void (*bus)(void *, int32_t *, int32_t *, unsigned) = c ? m->bus1 : m->bus;
    if (pre) pre(m->fx, m->L, m->R, n);                      /* the DRIVE, the first effect (off: returns at once) */
    TX_FAR(tx_comp_process)(&m->comp, m->L, m->R, n);        /* the compressor, after it (off: returns at once; in XIP,
                                                              * as the limiter: the bus runs on core 1, LN_PIPE) */
    if (m->ch && m->ch_on) {                                 /* the chamber on the mid: its added part to both sides */
        for (j = 0; j < n; j++) m->mid[j] = m->blk[j] = (m->L[j] >> 1) + (m->R[j] >> 1);
        if (c) TX_C1(tx_chamber_process)(m->ch, m->mid, n);
        else TX_FAR(tx_chamber_process)(m->ch, m->mid, n);   /* (in RAM on the FM-1: TX_RAM) */
        for (j = 0; j < n; j++) { int32_t w = m->mid[j] - m->blk[j]; m->L[j] += w; m->R[j] += w; }
    }
    if (bus) bus(m->fx, m->L, m->R, n);                      /* the bus effects (the grain delay, the reverb) */
    for (j = 0; j < n; j++) {                                /* linked: one gain for both, from the louder */
        int32_t a = m->L[j] < 0 ? -m->L[j] : m->L[j], b = m->R[j] < 0 ? -m->R[j] : m->R[j];
        if (a > pk) pk = a;
        if (b > pk) pk = b;
        m->ilv[2 * j] = m->L[j]; m->ilv[2 * j + 1] = m->R[j];
    }
    m->raw_peak = pk;
    TX_FAR(tx_limit_process)(&m->lim, m->ilv, 2u * n);       /* interleaved: the pair shares the gain envelope (in XIP) */
}
TX_AUDIO_CODE void tx_mix_out(const tx_mix_t *m, int32_t *L, int32_t *R, unsigned n)
{
    unsigned j;
    for (j = 0; j < n; j++) { L[j] = m->ilv[2 * j]; R[j] = m->ilv[2 * j + 1]; }
}

int tx_mix_quiet(const tx_mix_t *m)
{
    unsigned k;
    for (k = 0; k < TX_MIX_SLOTS; k++)
        if (m->e[k]) { tx_stats_t s; m->e[k]->stats(m->st[k], &s); if (!s.quiet) return 0; }
    if (m->busq && !m->busq(m->fx)) return 0;
    return !(m->ch && m->ch_on) || tx_chamber_quiet(m->ch);
}
