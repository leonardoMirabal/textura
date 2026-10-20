/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura PULSE core: table pulses (tx_core_pulse.h). */
#include "tx_core_pulse.h"

#define DONE ((uint32_t)TX_PULSE_N << 16)

TX_AUDIO_DATA static const int16_t *const SHAPES[4] = { TX_PULSE_HERTZ, TX_PULSE_RCOS, TX_PULSE_NWAVE, TX_PULSE_CLICK };

void tx_pulses_init(tx_pulses_t *b)
{
    unsigned i;
    for (i = 0; i < TX_PULSES; i++) { b->p[i].pos = DONE; b->p[i].step = 0; b->p[i].amp = 0; b->p[i].shape = 0;
                                      b->p[i].wait = 0; b->p[i].age = 0; }
    b->clock = 0;
}

void tx_pulse_start(tx_pulses_t *b, unsigned shape, uint32_t dur, int32_t amp_q15, unsigned offset)
{
    unsigned i, pick = 0;
    uint32_t oldest = 0xFFFFFFFFu;
    tx_pulse_t *p;
    for (i = 0; i < TX_PULSES; i++) {
        if (b->p[i].pos >= DONE) { pick = i; break; }
        if (b->p[i].age < oldest) { oldest = b->p[i].age; pick = i; }
    }
    p = &b->p[pick];
    if (dur < 2u) dur = 2u;
    if (amp_q15 > 32767) amp_q15 = 32767;
    if (amp_q15 < -32767) amp_q15 = -32767;
    p->pos = 0;
    p->step = DONE / dur;                                    /* 32-bit division, per pulse */
    if (!p->step) p->step = 1;
    p->amp = (int16_t)amp_q15;
    p->shape = (uint8_t)(shape & 3u);
    p->wait = (uint8_t)(offset < 255u ? offset : 255u);
    p->age = ++b->clock;
}

TX_AUDIO_CODE void tx_pulses_add(tx_pulses_t *b, int32_t *out, unsigned n)
{
    unsigned i, j;
    for (i = 0; i < TX_PULSES; i++) {
        tx_pulse_t *p = &b->p[i];
        const int16_t *t = SHAPES[p->shape];
        uint32_t pos = p->pos, step = p->step;
        int32_t a = p->amp;
        if (pos >= DONE) continue;
        j = p->wait < n ? p->wait : n;
        p->wait = (uint8_t)(p->wait - j);
        for (; j < n && pos < DONE; j++, pos += step) {
            uint32_t k = pos >> 16, fr = (pos >> 8) & 0xFFu;
            int32_t s = t[k] + (((int32_t)t[k + 1] - t[k]) * (int32_t)fr >> 8);   /* Q15 */
            out[j] += (s * a) >> 7;                                              /* Q30 -> Q23 */
        }
        p->pos = pos;
    }
}

unsigned tx_pulses_active(const tx_pulses_t *b)
{
    unsigned i, n = 0;
    for (i = 0; i < TX_PULSES; i++) n += b->p[i].pos < DONE;
    return n;
}
