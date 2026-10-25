/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura effects: the REVERB's framework and the chain (tx_fx.h). The framework is Frodhes' (rev_types.c and fx.c
 * fx_reverb / fx_predelay: our own) as an instance: the send (the mix's mid x MIX), LOW CUT, the pre-delay, the 2:1
 * down into the kernel, the kernel at 22.05 kHz, the 1:2 up, the return (which eases off as MIX rises), a TYPE
 * change's fade (5.8 ms) and the tail's end in exact silence. ROOM (Textura) is the CHAMBER (tx_core_chamber) run on
 * the send at the full rate, so the CHAMBER is one of the reverb's TYPEs now: one room at a time, one memory union.
 * DRIFT and SWELL (Textura) run the infinite kernel too: DRIFT its wander a slow random walk (tx_fx_inf.c); SWELL the
 * tail held down while the send is loud and let in when it falls (the return's gain, below): the tail swells in after
 * the notes, in ~1.2 s. */
#include "tx_fx.h"

const tx_param_t tx_fx_reverb_params[TX_RV_NP] = {
    {"TYPE", TX_RVT_N - 1, 0, "the space: 0 off (the tail rings out) | 1 room (the CHAMBER: body modes + a short tail) | 2 hall | 3 infinite (DECAY 120..126 sustain, 127 freeze) | 4 dark (the infinite tank, darker and still: a deep frozen bed) | 5 drift (the infinite tank, slowly bending in pitch at random) | 6 swell (the infinite tank, held down while you play, swelling in after)"},
    {"MIX", 127, 40, "the send and the return: dry .. wet (the return eases off as the send rises)"},
    {"DECAY", 127, 64, "hall: x0.5 .. x2 the time; infinite / dark / drift / swell: T60 4 .. 60 s, 120..126 sustain, 127 freeze; room: the walls (127 = hard, long)"},
    {"TONE", 127, 64, "the tail's top: 0 dark .. 64 as designed .. 127 bright; room: the body's resonance"},
    {"PREDELAY", 127, 0, "before the reverb: 0 .. 80 ms"},
    {"SIZE", 127, 0, "0 = from MIX (auto); 1..127 the size (room: 0.2 .. 3 m)"},
    {"MOD", 127, 64, "the tail's slow wander (hall, infinite; drift: its random bend): none .. 2x"},
    {"LOWCUT", 127, 0, "a high-pass on the send: off (0), 20 .. 800 Hz"},
    {"SHIMMER", 127, 0, "the tail shifted up and fed back into the reverb: none .. a rising, ringing halo"},
    {"SHIFT", 3, 0, "the shimmer's interval: 0 an octave up | 1 a fifth up | 2 an octave and a fifth up | 3 an octave down"},
};

#define RV_FADE 8u                  /* control blocks of a TYPE change's fade: 256 frames, 5.8 ms */
#define FX_PDLY_SAMPLES(v) ((uint32_t)(v) * 3528u / 127u)
#define RV_UPI(u) ((RV_HB((u) + 0, 1) + 512) >> 10)

static uint32_t kern(uint32_t t) { return t >= TX_RVT_INF ? TX_RVT_INF : t; }   /* DARK, DRIFT, SWELL: the infinite kernel */
static inline int32_t mq15s(int32_t a, int32_t q) { return (a >> 15) * q + (((a & 32767) * q) >> 15); }   /* a q, two 32-bit products */

/* ---- SWELL: the return's gain g = r^2, r gliding toward T / (T + the send's level): held down while the send is
 * loud (r falls in ~25 ms), let in when it falls (r rises over ~1.2 s: the tail swells in); g ramps across the control
 * block (no zipper). The level: the block's peak of the mid, held, falling in ~45 ms. */
#define SW_T 1024                   /* the level where the tail is held 12 dB down: -30 dBFS */
#define SW_DN 1200                  /* r's fall a control block (Q15): ~25 ms from 1 to 0 */
#define SW_UP 20                    /* r's rise a control block: 1.2 s from 0 to 1 */
static void swell(tx_fx_reverb_t *r, int32_t pk, int32_t *om, int32_t *os)
{
    int32_t t, r0 = r->swr, g0, g1, i;
    r->swe = pk > r->swe ? pk : r->swe - (r->swe >> 6) - (r->swe > 0);
    t = (int32_t)((SW_T * 32768) / (SW_T + r->swe));
    r->swr = t < r0 ? (r0 - t > SW_DN ? r0 - SW_DN : t) : (t - r0 > SW_UP ? r0 + SW_UP : t);
    g0 = (r0 * r0) >> 15; g1 = (r->swr * r->swr) >> 15;
    if (g0 == 32768 && g1 == 32768) return;
    for (i = 0; i < (int32_t)HL_NC; i++) {                       /* (toward zero: the tail still ends at 0) */
        int32_t g = g0 + (((g1 - g0) * (i + 1)) >> 4), a = om[i], b = os[i];
        om[i] = a < 0 ? -mq15s(-a, g) : mq15s(a, g);
        os[i] = b < 0 ? -mq15s(-b, g) : mq15s(b, g);
    }
}

/* ---- SHIMMER (Textura): the tank's mid, pitch-shifted, fed back into its input (the next control block's). At the
 * half rate: an input low-pass (the octave above folds otherwise), a 2048-sample line, two read heads on one phase,
 * half a turn apart, each through a Hann window over a 1024-sample (46 ms) span: their sum is the mid shifted by
 * SHIFT, with no gaps and no clicks; linear interpolation. A guard lowers the feed when a block's peak is high, so a
 * SUSTAIN or a long hall cannot run away; it recovers slowly. ~20 us a block on the FM-1. */
static const uint32_t SH_INC[4] = {4194304u, 2090051u, 8374407u, 4292870144u};   /* (ratio - 1) x 2^32 / 1024 */
static void shimmer(tx_fx_reverb_t *r, const int32_t *mid)
{
    int32_t amt = r->p[TX_RV_SHIMMER], g = (int32_t)(((amt * 258) >> 3) * (int64_t)r->shg >> 15), pk = 0;
    uint32_t i, inc = SH_INC[r->p[TX_RV_SHIFT] & 3u], ph = r->shph, w = r->shw;
    for (i = 0; i < HL_NC; i++) {
        int32_t y = 0, k;
        r->shlp += (mid[i] - r->shlp) >> 1;                          /* (the input low-pass: the tank's top folds less) */
        r->shb[w] = r->shlp;
        for (k = 0; k < 2; k++) {
            uint32_t p = ph + (uint32_t)k * 0x80000000u, d = (65536u - (p >> 16)) * 1024u, back = (d >> 16) + 2u, j0, j1;
            int32_t a, b, v;
            j0 = (w - back) & 2047u; j1 = (j0 + 1u) & 2047u;           /* the head, and the sample after it */
            a = r->shb[j0]; b = r->shb[j1];
            v = b + mq15s(a - b, (int32_t)((d & 0xFFFFu) >> 1));      /* (between them: d's fraction back from b) */
            y += mq15s(v, TX_FX_HANN[p >> 22]);
        }
        ph += inc;
        w = (w + 1u) & 2047u;
        y = y < 0 ? -mq15s(-y, g) : mq15s(y, g);                    /* (toward zero: a fading tail ends at 0) */
        y = clampi(y, -24000, 24000);
        r->sh[i] = y;
        pk |= y ^ (y >> 31);
    }
    r->shph = ph; r->shw = w;
    if (pk > 6000) r->shg -= r->shg >> 3;                           /* the guard: down fast, back slowly */
    else if (r->shg < 32768) r->shg += ((32768 - r->shg) >> 9) + 1;
}

static void rev_begin(tx_fx_reverb_t *r, uint32_t t)
{
    tx_fx_memset(r->m, 0, sizeof r->m); tx_fx_memset(r->s, 0, sizeof r->s); r->hp = r->he = 0;
    tx_fx_memset(r->sh, 0, sizeof r->sh); tx_fx_memset(r->shb, 0, sizeof r->shb); r->shlp = 0;
    r->cur = (uint8_t)kern(t);
    if (r->cur == TX_RVT_HALL) tx_hall_start(&r->u.h);
    else if (r->cur == TX_RVT_INF) tx_inf_start(&r->u.i);
    else if (r->cur == TX_RVT_ROOM) { tx_chamber_init(&r->u.rm.c); tx_chamber_param(&r->u.rm.c, TX_CH_TAIL, 0); tx_hall_start(&r->u.rm.h); }
}
static int rev_active(const tx_fx_reverb_t *r)
{
    return r->cur == TX_RVT_INF ? tx_inf_active(&r->u.i) : r->cur == TX_RVT_ROOM ? !tx_chamber_quiet(&r->u.rm.c) : 0;
}

void tx_fx_reverb_init(tx_fx_reverb_t *r)
{
    unsigned i;
    tx_fx_memset(r, 0, sizeof *r - sizeof r->u);
    for (i = 0; i < TX_RV_NP; i++) r->p[i] = tx_fx_reverb_params[i].def;
    r->damp = 60;
    r->dk = 64;
    r->shg = 32768;
    r->cur = TX_RVT_OFF;
    r->rng = 0x6C078965u;
    r->swr = 32768;
}
void tx_fx_reverb_param(tx_fx_reverb_t *r, unsigned id, unsigned v)
{
    if (id < TX_RV_NP) r->p[id] = (uint8_t)(v > tx_fx_reverb_params[id].max ? tx_fx_reverb_params[id].max : v);
}
int tx_fx_reverb_quiet(const tx_fx_reverb_t *r) { return !r->running; }
int tx_fx_reverb_frozen(const tx_fx_reverb_t *r)
{
    uint32_t t = r->p[TX_RV_TYPE];
    return r->running && r->cur == TX_RVT_INF && kern(t) == TX_RVT_INF && r->dk >= 120 ? (r->dk < 127 ? 1 : 2) : 0;
}

static int32_t locut_a(int32_t v) { return (3734 * (int32_t)exp2_neg((uint32_t)(127 - v) * 2767u)) >> 15; }
static void rev_locut(tx_fx_reverb_t *r, int32_t *x, int32_t v)   /* x - lowpass(x), 20 .. 800 Hz */
{
    uint32_t j;
    int32_t lc = r->lc, a;
    if (v != r->lcv) { r->lcv = v; r->lca = v ? locut_a(v) : 0; }
    if (!v) { r->lc = 0; return; }
    a = r->lca;
    for (j = 0; j < CTL; j++) {
        int32_t u = clampi(x[j], -(1 << 21), 1 << 21);
        lc += (int32_t)(((int64_t)(u * 256 - lc) * a) >> 15);
        x[j] = u - (lc >> 8);
    }
    r->lc = lc;
}
static void predelay(tx_fx_reverb_t *r, int32_t *x, uint32_t tgt)   /* one control block, x[] delayed by the tap */
{
    uint32_t j;
    if (!r->pd_on) { r->pd_fill = 0; r->pd_d = r->pd_d0 = r->pd_x = 0; r->pd_on = 1; }
    if (!r->pd_x && tgt != r->pd_d) { r->pd_d0 = r->pd_d; r->pd_d = tgt; r->pd_x = 1; }
    for (j = 0; j < CTL; j++) {
        int32_t a;
        r->pd[r->pd_w & (TX_PD_LEN - 1u)] = x[j];
        if (r->pd_fill < TX_PD_LEN) r->pd_fill++;               /* (a tap behind what was written reads 0: no memset) */
        a = r->pd_d < r->pd_fill ? r->pd[(r->pd_w - r->pd_d) & (TX_PD_LEN - 1u)] : 0;
        if (r->pd_x) {                                          /* 256 samples from the old tap to the new one */
            int32_t b = r->pd_d0 < r->pd_fill ? r->pd[(r->pd_w - r->pd_d0) & (TX_PD_LEN - 1u)] : 0;
            a = b + (int32_t)(((int64_t)(a - b) * (int32_t)(r->pd_x << 7)) >> 15);
            if (++r->pd_x > 256u) r->pd_x = 0;
        }
        x[j] = a;
        r->pd_w++;
    }
    if (!r->pd_x && !r->pd_d && !tgt) r->pd_on = 0;
}

/* ROOM: the CHAMBER over the gathered run rm[r0 .. r0 + rn) at once (its per-call costs once a block), the wet part
 * (out - in) added to both sides, each control block faded by its TYPE-change fade (fade / 8 .. (fade - 1) / 8) */
static void room_flush(tx_fx_reverb_t *r, int32_t *L, int32_t *R)
{
    int32_t keep[TX_BLOCK], *x = r->rm + r->r0;
    uint32_t j, n = r->rn;
    for (j = 0; j < n; j++) keep[j] = x[j];
    tx_chamber_process(&r->u.rm.c, x, n);
    for (j = 0; j < n; j++) {
        int32_t w = x[j] - keep[j], f = r->rf[(r->r0 + j) / CTL];
        if (f) w = (int32_t)(((int64_t)w * ((f * 32 - (int32_t)(j & (CTL - 1u))) * 128)) >> 15);
        L[r->r0 + j] += w; R[r->r0 + j] += w;
    }
    r->rn = 0;
}

#define RV_RET(w) ((int32_t)(((int64_t)(w) * rk) >> 30) + (int32_t)((uint32_t)(w) >> 31))
TX_AUDIO_CODE void tx_fx_reverb_process(tx_fx_reverb_t *r, int32_t *L, int32_t *R, unsigned n)
{
    int32_t v = r->p[TX_RV_MIX], tgt = r->p[TX_RV_TYPE], zv = r->p[TX_RV_SIZE];
    int32_t send = tgt ? v * 258 : 0, size = 40 + (zv ? zv : v) * 70 / 127;
    int32_t dv = r->p[TX_RV_DECAY], tv = r->p[TX_RV_TONE], md = (r->p[TX_RV_MOD] + 4) >> 3, lcv = r->p[TX_RV_LOWCUT];
    int32_t off = ((dv - 64) * 15) / 32;
    int32_t damp = tv >= 64 ? 60 - ((tv - 64) * 60) / 63 : 60 + ((64 - tv) * 90) / 64;
    uint32_t sb, i, j, pd = FX_PDLY_SAMPLES(r->p[TX_RV_PREDELAY]);
    int32_t rk = (254 - v) * 4227330 + (4 * (254 - v)) / 254 + 1;   /* floor((254 - v) 2^30 / 254) + 1 */
    int quiet = !r->running, dark = tgt == TX_RVT_DARK, shon = r->p[TX_RV_SHIMMER] != 0;
    if (!send && !r->running && !r->fade) return;
    r->running = 1;
    if (tgt == TX_RVT_DARK) damp += 60;                         /* DARK: TONE's damping 60 steps darker */
    for (sb = 0; sb + CTL <= n; sb += CTL) {
        int32_t act = 0, *om = r->m + TX_RV_QZ, *os = r->s + TX_RV_QZ, *in = r->inb + RV_DZ, spk = 0;
        uint32_t cur;
        if (tgt && kern((uint32_t)tgt) != r->cur && !r->fade) {
            if (quiet || !r->cur) rev_begin(r, (uint32_t)tgt);
            else r->fade = RV_FADE;
        }
        for (j = 0; j < RV_DZ; j++) r->inb[j] = r->inb[CTL + j];
        for (j = 0; j < CTL; j++) {                             /* the send: the mid, Q23 -> Q15 (64-bit: the bus may */
            int32_t mq = (int32_t)(((L[sb + j] >> 1) + (R[sb + j] >> 1) + 128) >> 8);   /* be far over full scale) */
            in[j] = clampi((int32_t)(((int64_t)mq * send) >> 15), -131071, 131071);
            spk |= mq ^ (mq >> 31);                             /* (SWELL: the level, a peak to a power of 2) */
        }
        if (lcv || r->lc) rev_locut(r, in, lcv);
        if (pd || r->pd_on) predelay(r, in, pd);
        if (r->off != off) r->off += r->off < off ? 1 : -1;
        if (r->damp != damp) r->damp += r->damp < damp ? (damp - r->damp > 2 ? 2 : damp - r->damp)
                                                       : -(r->damp - damp > 2 ? 2 : r->damp - damp);
        if (r->dk != dv) r->dk += r->dk < dv ? 1 : -1;
        cur = r->cur;
        if (cur == TX_RVT_ROOM) {                               /* the CHAMBER on the send, full rate: gathered here, */
            unsigned cp[TX_CH_NP], q;                           /* run over the whole run of control blocks (room_flush) */
            cp[TX_CH_SIZE] = (unsigned)(zv ? zv : 76); cp[TX_CH_ABSORB] = (unsigned)(127 - r->dk);
            cp[TX_CH_BODY] = (unsigned)((tv * 40) >> 6 > 127 ? 127 : (tv * 40) >> 6); cp[TX_CH_TAIL] = 0u;
            for (q = 0; q < TX_CH_NP; q++) if (r->u.rm.c.p[q] != cp[q]) tx_chamber_param(&r->u.rm.c, q, cp[q]);   /* changes only */
            if (r->rn == 0) r->r0 = sb;
            for (j = 0; j < CTL; j++) r->rm[sb + j] = in[j] * 256;   /* Q15 -> Q23: the chamber's scale */
            r->rf[sb / CTL] = r->fade;
            r->rn += CTL;
        }
        {   /* the half-rate TYPEs (and ROOM's tail: a short hall) */
            if (cur) for (i = 0; i < TX_RV_QZ; i++) r->m[i] = om[HL_NC - TX_RV_QZ + i], r->s[i] = os[HL_NC - TX_RV_QZ + i];
            if (cur == TX_RVT_HALL) tx_hall_run(&r->u.h, in, shon ? r->sh : 0, size + r->off, r->damp, md, om, os);
            else if (cur == TX_RVT_ROOM)                        /* the room's tail: a hall of size 2 .. 32 (T60 ~0.3 .. 1 s) */
                tx_hall_run(&r->u.rm.h, in, shon ? r->sh : 0, 2 + (zv ? zv : 64) * 30 / 127 + r->off / 2, r->damp, md, om, os);
            else if (cur == TX_RVT_INF) {
                tx_rev_ctl_t c;
                c.decay = r->dk; c.damp = r->damp; c.size = zv ? zv : v; c.mod = r->p[TX_RV_MOD]; c.dark = dark;
                c.drift = tgt == TX_RVT_DRIFT ? &r->rng : 0;
                c.freeze = kern((uint32_t)tgt) != TX_RVT_INF || r->dk < 120 ? 0 : r->dk < 127 ? 1 : 2;
                tx_rev_down(in, &r->hp, &r->he, r->x);
                if (shon) for (i = 0; i < HL_NC; i++) r->x[i] += r->sh[i];
                tx_inf_run(&r->u.i, r->x, om, os, &c);
                if (tgt == TX_RVT_SWELL || r->swr != 32768) swell(r, tgt == TX_RVT_SWELL ? spk : 0, om, os);
            }
            if (cur && (r->p[TX_RV_SHIMMER] || shon)) {         /* the shimmer for the next control block */
                shimmer(r, om);
                shon = r->p[TX_RV_SHIMMER] != 0;
                if (!shon) for (i = 0; i < HL_NC; i++) shon |= r->sh[i] != 0;   /* (its last feed still to go in) */
                if (!r->p[TX_RV_SHIMMER]) tx_fx_memset(r->sh, 0, sizeof r->sh), shon = 0;
                for (i = 0; i < HL_NC; i++) act |= r->sh[i];
            }
            if (cur && (r->fade || (v && !send))) {             /* a fade: the TYPE's out x (fade / 8) ..; OFF: watched */
                int32_t g = (int32_t)r->fade << 12;
                for (i = 0; i < HL_NC; i++, g -= 256) {
                    act |= om[i] | os[i];
                    if (r->fade) { om[i] = mul15(om[i], g); os[i] = mul15(os[i], g); }
                }
                if (r->fade) act = 1;
            }
            if (!cur) {}
            else if (v)                                         /* 1:2 up, the return eased off as MIX rises; Q15 -> Q23 */
                for (i = 0, j = sb; i < HL_NC; i++, j += 2u) {
                    const int32_t *u = r->m + TX_RV_QZ - 2u + i, *q = r->s + TX_RV_QZ - 2u + i;
                    int32_t wm0 = RV_RET(RV_UPI(u)), ws0 = RV_RET(RV_UPI(q)), wm1 = RV_RET(u[0]), ws1 = RV_RET(q[0]);
                    L[j] += (wm0 + ws0) * 256; R[j] += (wm0 - ws0) * 256;
                    L[j + 1u] += (wm1 + ws1) * 256; R[j + 1u] += (wm1 - ws1) * 256;
                }
            else
                for (i = 0; i < HL_NC; i++) {
                    const int32_t *u = r->m + TX_RV_QZ - 2u + i, *q = r->s + TX_RV_QZ - 2u + i;
                    act |= u[2] | q[2];
                }
        }
        if (r->fade && !--r->fade) {
            if (r->rn) room_flush(r, L, R);                     /* (the room's run before its state is reused) */
            rev_begin(r, tgt ? (uint32_t)tgt : cur ? cur : TX_RVT_HALL);
        }
        else if (!send && !act && !rev_active(r)) {             /* the tail has ended in exact silence */
            r->running = 0; r->pd_on = 0; quiet = 1;
            tx_fx_memset(r->inb, 0, sizeof r->inb);
            break;
        }
    }
    if (r->rn) room_flush(r, L, R);                    /* (the room's body: its end is seen by the next block) */
}

/* ---- the chain */
void tx_fx_init(tx_fx_t *fx)
{
    unsigned k;
    for (k = 0; k < TX_FX_LANES; k++) { tx_fx_drive_init(&fx->drive[k]); tx_fx_filter_init(&fx->filter[k]); }
    tx_fx_grain_init(&fx->gd);
    tx_fx_reverb_init(&fx->rev);
    fx->reserve_us = TX_FX_RESERVE_US;
}
TX_AUDIO_CODE void tx_fx_lane(void *u, unsigned k, int32_t *x, unsigned n)
{
    tx_fx_t *fx = (tx_fx_t *)u;
    if (k >= TX_FX_LANES) return;
    tx_fx_drive_process(&fx->drive[k], x, n);
    tx_fx_filter_process(&fx->filter[k], x, n);
}
/* the grain delay's dynamic cap: the grains the effects reserve leaves after the reverb's TYPE (FM-1 us a block,
 * CPU-BUDGET.md section 8: the reverb's worst per TYPE, the delay's line + echo, a sounding grain) */
#define FX_US_GD_BASE 220u         /* the line, the echo, the feedback (make tx-budget section 4, 2026-10-25) */
#define FX_US_GD_GRAIN1 14u        /* a grain at rate 1 (whole samples: a run decoded, one read a sample) */
#define FX_US_GD_GRAINP 42u        /* a pitched or reversed grain (its run decoded, the interpolation) */
#define FX_US_SHIMMER 75u          /* the shimmer on top of its reverb */
static const uint16_t FX_US_REV[TX_RVT_N] = {0, 390, 210, 280, 280, 280, 290};
TX_AUDIO_CODE void tx_fx_bus(void *u, int32_t *L, int32_t *R, unsigned n)
{
    tx_fx_t *fx = (tx_fx_t *)u;
    uint32_t t = fx->rev.p[TX_RV_TYPE], c = fx->rev.cur > t ? fx->rev.cur : t, rv = c < TX_RVT_N ? FX_US_REV[c] : 0;
    if (rv && fx->rev.p[TX_RV_SHIMMER]) rv += FX_US_SHIMMER;
    uint32_t room = fx->reserve_us > rv + FX_US_GD_BASE ? fx->reserve_us - rv - FX_US_GD_BASE : 0;
    room /= fx->gd.p[TX_GD_PITCH] == 64 && !fx->gd.p[TX_GD_REVERSE] ? FX_US_GD_GRAIN1 : FX_US_GD_GRAINP;
    fx->gd.cap = (uint8_t)(room > TX_GD_MAXG ? TX_GD_MAXG : room);
    tx_fx_grain_process(&fx->gd, L, R, n);
    tx_fx_reverb_process(&fx->rev, L, R, n);
}
int tx_fx_bus_quiet(void *u) { return tx_fx_reverb_quiet(&((tx_fx_t *)u)->rev) && tx_fx_grain_quiet(&((tx_fx_t *)u)->gd); }
