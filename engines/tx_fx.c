/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura effects: the REVERB's framework and the chain (tx_fx.h). The framework is Frodhes' (rev_types.c and fx.c
 * fx_reverb / fx_predelay: our own) as an instance: the send (the mix's mid, whole), LOW CUT, the pre-delay, the
 * kernel (Textura: at the full rate, 44.1 kHz; Frodhes runs them at 22.05 kHz between a 2:1 down and a 1:2 up), the
 * return, then MIX as a true dry / wet (the dry and the return each x its gain: tx_fx.h), a TYPE change's fade (5.8 ms) and the tail's end in exact silence. ROOM (Textura) is the CHAMBER (tx_core_chamber) run on
 * the send at the full rate, so the CHAMBER is one of the reverb's TYPEs now: one room at a time, one memory union.
 * PLATE is Frodhes' plate kernel (tx_fx_plate.c). DRIFT and SWELL (Textura) run the infinite kernel too: DRIFT its wander a slow random walk (tx_fx_inf.c); SWELL the
 * tail held down while the send is loud and let in when it falls (the return's gain, below): the tail swells in after
 * the notes, in ~1.2 s. */
#include "tx_fx.h"

const tx_param_t tx_fx_reverb_params[TX_RV_NP] = {
    {"TYPE", TX_RVT_N - 1, 0, TX_TEXT("the space: 0 off (the tail rings out) | 1 room (the CHAMBER: body modes + a short tail) | 2 hall | 3 infinite (DECAY 120..126 sustain, 127 freeze) | 4 dark (the infinite tank, darker and still: a deep frozen bed) | 5 drift (the infinite tank, slowly bending in pitch at random) | 6 swell (the infinite tank, held down while you play, swelling in after) | 7 plate (a bright, dense plate)")},
    {"MIX", 127, 35, TX_TEXT("0 the sound alone; up to 64 the reverb comes in, the sound stays whole; 64 both whole; above it the sound fades out, 127 the reverb alone")},
    {"DECAY", 127, 64, TX_TEXT("hall: x0.5 .. x2 the time; infinite / dark / drift / swell: T60 4 .. 60 s, 120..126 sustain, 127 freeze; plate: T60 0.8 .. 6 s; room: the walls (127 = hard, long)")},
    {"TONE", 127, 64, TX_TEXT("the tail's top: 0 dark .. 64 as designed .. 127 bright; room: the body's resonance")},
    {"PREDELAY", 127, 0, TX_TEXT("before the reverb: 0 .. 80 ms")},
    {"SIZE", 127, 0, TX_TEXT("0 = auto (HALL, PLATE: from MIX; INFINITE: fixed); 1..127 the size (room: 0.2 .. 3 m)")},
    {"MOD", 127, 64, TX_TEXT("the tail's slow wander (hall, infinite, plate; drift: its random bend): none .. 2x")},
    {"LOWCUT", 127, 0, TX_TEXT("a high-pass on the send: off (0), 20 .. 800 Hz")},
    {"SHIMMER", 127, 0, TX_TEXT("the tail shifted up and fed back into the reverb: none .. a rising, ringing halo")},
    {"SHIFT", 3, 0, TX_TEXT("the shimmer's interval: 0 an octave up | 1 a fifth up | 2 an octave and a fifth up | 3 an octave down")},
};

#define RV_FADE 8u                  /* control blocks of a TYPE change's fade: 256 frames, 5.8 ms */
#define FX_PDLY_SAMPLES(v) ((uint32_t)(v) * 3528u / 127u)

TX_INLINE uint32_t kern(uint32_t t)                    /* DARK, DRIFT, SWELL: the infinite kernel */
{
    return t >= TX_RVT_INF && t <= TX_RVT_SWELL ? TX_RVT_INF : t;
}
TX_INLINE int32_t mq15s(int32_t a, int32_t q) { return (a >> 15) * q + (((a & 32767) * q) >> 15); }   /* a q, two 32-bit products */

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
        int32_t g = g0 + (((g1 - g0) * (i + 1)) >> 5), a = om[i], b = os[i];   /* (/ HL_NC) */
        om[i] = a < 0 ? -mq15s(-a, g) : mq15s(a, g);
        os[i] = b < 0 ? -mq15s(-b, g) : mq15s(b, g);
    }
}

/* ---- SHIMMER (Textura): the tank's mid, pitch-shifted, fed back into its input (the next control block's). An
 * input low-pass (two poles at ~2 kHz: the shifted copy rings, it does not hiss, and what the shift lifts past fs / 2
 * cannot fold back), a SHN-sample line, two read heads on one phase, half a turn apart, each through a Hann window over
 * a 2048-sample (46 ms) span: their sum is the mid shifted by SHIFT, with no gaps and no clicks; linear interpolation.
 * The feed goes into the send ahead of the tanks' DC blocker (tx_rev_down; ROOM's tail runs its own), so neither DC
 * nor the windows' 21.5 Hz can circulate. Its gain: SHIMMER's concave law x the kernel's row at the DECAY (SH_KT, under
 * the loop's limit everywhere), x SHIFT's and TONE's room. A guard lowers the feed when its or the tank's peak is
 * high, so a SUSTAIN or a FREEZE holds its level and a loud held chord keeps the bus under full scale; it recovers
 * slowly. A second one (the decay guard, below) makes any tail with no send die away; the feed's lowest steps are not
 * fed back. ~20 us a block on the FM-1. */
#define SH_GUARD 16000                  /* (the guard's block peak: the feed's) */
#define SH_MIDG 20000                   /* (and the tank's mid's: a loud held chord under a long PLATE built the bus
                                         * past full scale, x1.24; at 20000 every TYPE's worst held case is < 0.98) */
#define SH_CLAMP 48000                  /* (the feed's bound, a sample) */
#define SH_LIVE 64                      /* (a send over this, -54 dBFS: the input plays) */
#define SH_WIN 4096                     /* (the decay guard's window: 4096 control blocks, 3 s) */
#define SH_FLOOR 8                      /* (the feed's floor: taken off every sample, toward zero) */
/* the loop's gain at SHIMMER 127 (Q15 per SHIMMER step), per kernel over DECAY 0, 32, 64, 96, 127 (between: straight
 * lines): 0.7 of the gain where the loop stops decaying (measured per kernel and DECAY: a 440 Hz pluck, 30 s, every
 * SHIFT, TONE 64 and 127), or 350 where that is over 500, so the longest tail still dies away at any SHIMMER. The
 * INFINITE kernel's row is the lowest of INFINITE, DRIFT and SWELL (DARK has its own); its last point is DECAY 119's
 * (120 .. 127, SUSTAIN and FREEZE, hold by design: the guard caps the level) */
static const uint16_t SH_KT[5][5] = {
    {350, 350, 350, 350, 350},          /* ROOM */
    {350, 350, 350, 304, 174},          /* HALL */
    {350, 350, 154, 116, 132},          /* INFINITE, DRIFT, SWELL */
    {302, 302, 258, 241, 197},          /* PLATE */
    {350, 350, 370, 101, 184},          /* DARK (its own: darker, it rings less at DECAY 64) */
};
static const uint16_t SH_KS[4] = {282, 256, 345, 282};   /* SHIFT's room over a fifth up's (Q8, measured) */
static const uint32_t SH_INC[4] = {2097152u, 1045025u, 4187203u, 4293918720u};   /* (ratio - 1) x 2^32 / 2048 */
static void shimmer(tx_fx_reverb_t *r, const int32_t *mid, int live)
{
    int32_t amt = r->p[TX_RV_SHIMMER], dk = r->dk, pk = 0, row = r->cur == TX_RVT_ROOM ? 0 : r->cur == TX_RVT_HALL ? 1
        : r->cur != TX_RVT_INF ? 3 : r->p[TX_RV_TYPE] == TX_RVT_DARK ? 4 : 2;
    int32_t kg = SH_KT[row][dk >> 5], g, held, zv = r->p[TX_RV_SIZE], mpk = 0;
    uint32_t i, inc = SH_INC[r->p[TX_RV_SHIFT] & 3u], ph = r->shph, w = r->shw;
    kg += ((SH_KT[row][(dk >> 5) + 1] - kg) * (dk & 31)) >> 5;          /* (the row at DECAY) */
    kg = (kg * SH_KS[r->p[TX_RV_SHIFT] & 3u]) >> 8;                    /* (the rows hold for a fifth up, TONE 127: */
    kg = (kg * (320 - (r->damp < 60 ? (60 - r->damp) * 16 / 15 : 0))) >> 8;   /* the other SHIFTs, a darker TONE
                                                                 * ring longer: up to x1.35, x1.25) */
    if (zv && zv < 64 && row >= 2) kg = (kg * (128 + 2 * zv)) >> 8;   /* (INFINITE, PLATE: a small SIZE, a short
                                                                 * loop comes round more often; x0.5 .. x1 to SIZE 64) */
    g = ((r->shg >> 1) * (r->shd >> 1)) >> 13;                       /* (the two guards) */
    g = (int32_t)(((amt * (254 - amt) / 127) * kg * (int64_t)g) >> 15);   /* (SHIMMER's law: concave, a third of the
                                                                 * knob already 0.56 of the gain; g < 76000) */
    for (i = 0; i < HL_NC; i++) {
        int32_t y = 0, k;
        mpk = mid[i] > mpk ? mid[i] : -mid[i] > mpk ? -mid[i] : mpk;
        r->shlp += mq15s(mid[i] - r->shlp, 9598);                    /* (the input low-pass, two poles at ~2 kHz: */
        r->shlp2 += mq15s(r->shlp - r->shlp2, 9598);                 /* what the shift lifts past fs / 2 folds back) */
        r->shb[w] = r->shlp2;
        for (k = 0; k < 2; k++) {
            uint32_t p = ph + (uint32_t)k * 0x80000000u, d = (65536u - (p >> 16)) * 2048u, back = (d >> 16) + 2u, j0, j1;
            int32_t a, b, v;
            j0 = w >= back ? w - back : w + SHN - back;             /* the head, and the sample after it */
            j1 = j0 + 1u < SHN ? j0 + 1u : 0u;
            a = r->shb[j0]; b = r->shb[j1];
            v = b + mq15s(a - b, (int32_t)((d & 0xFFFFu) >> 1));      /* (between them: d's fraction back from b) */
            y += mq15s(v, TX_FX_HANN[p >> 22]);
        }
        ph += inc;
        w = w + 1u < SHN ? w + 1u : 0u;
        y = (y < 0 ? -mq15s(-y, g >> 1) : mq15s(y, g >> 1)) * 2;    /* (toward zero: a fading tail ends at 0; g / 2:
                                                                 * mq15s's low product stays in 32 bits to g 65536) */
        y = y > SH_FLOOR ? y - SH_FLOOR : y < -SH_FLOOR ? y + SH_FLOOR : 0;   /* (the tanks' own rounding, -72
                                                                 * dBFS and under, not fed back: no limit cycle held) */
        y = clampi(y, -SH_CLAMP, SH_CLAMP);
        r->sh[i] = y;
        k = y ^ (y >> 31);
        pk = k > pk ? k : pk;
    }
    r->shph = ph; r->shw = w;
    /* the decay guard: with no send, the feed's peak must fall from one 3 s window to the next (by 1/4 at least, T60
     * under ~70 s), or the gain drops 2.5 dB (shd, back only while the send plays): a loop that holds or grows (a
     * small SIZE, a dark TONE, a corner the rows did not see) dies away all the same. Not under FREEZE / SUSTAIN (held
     * by design) */
    held = r->cur == TX_RVT_INF && kern(r->p[TX_RV_TYPE]) == TX_RVT_INF && r->dk >= 120;
    r->shm = pk > r->shm ? pk : r->shm;
    r->shq = live ? 0 : r->shq < SH_WIN ? r->shq + 1 : SH_WIN;
    if (++r->shn >= SH_WIN) {
        if (r->shq >= SH_WIN && r->shm > SH_FLOOR && r->shm > r->shp - (r->shp >> 2) && !held) r->shd -= r->shd >> 2;
        r->shp = r->shm; r->shm = 0; r->shn = 0;
    }
    if (pk > SH_GUARD || mpk > SH_MIDG) r->shg -= r->shg >> 3;          /* the peak guard: down fast, back slowly */
    else if (r->shg < 32768) r->shg += ((32768 - r->shg) >> 9) + 1;
    if (live && r->shd < 32768) r->shd += ((32768 - r->shd) >> 9) + 1;   /* (the decay guard's: back while the send
                                                                 * plays) */
}

/* SIZE 0 = AUTO. HALL: from MIX, as always. The INFINITE kernel (INFINITE, DARK, DRIFT, SWELL): a fixed TX_RV_INF_AUTO,
 * since a SIZE change glides its lines' lengths, a Doppler bend of the whole tail (-50 .. -250 cents while MIX moved
 * 64 -> 120 under a held tail), and its tails ring for seconds .. for ever: MIX is the level only. PLATE: MIX, latched
 * while the plate rings (pz; a size step moves its delays: MIX turned under a tail scrambled its pitch), taken anew
 * from silence or when a whole sound loads (tx_fx_reverb_relatch). As Frodhes' fixes (its inf-size branch; its
 * INFINITE size is 64, ours 90: our FREEZE check's size, the steadier hold) */
#define TX_RV_INF_AUTO 90                       /* (MIX 90's old size: a FREEZE holds within 3 dB) */
static void rev_begin(tx_fx_reverb_t *r, uint32_t t)
{
    tx_fx_memset(r->m, 0, sizeof r->m); tx_fx_memset(r->s, 0, sizeof r->s); r->hp = r->he = 0;
    tx_fx_memset(r->sh, 0, sizeof r->sh); tx_fx_memset(r->shb, 0, sizeof r->shb); r->shlp = r->shlp2 = 0;
    r->shm = r->shp = 0; r->shn = r->shq = 0; r->shd = 32768;
    r->cur = (uint8_t)kern(t); r->xin = 0;
    if (r->cur == TX_RVT_HALL) tx_hq_start(&r->u.hq.q, r->u.hq.mem, HQ_HALL_WORDS);
    else if (r->cur == TX_RVT_INF) tx_inf_start(&r->u.i);
    else if (r->cur == TX_RVT_PLATE) tx_plate_start(&r->u.p), r->pz = -1;
    else if (r->cur == TX_RVT_ROOM) { tx_chamber_init(&r->u.rm.c); tx_chamber_param(&r->u.rm.c, TX_CH_TAIL, 0); tx_hall_start(&r->u.rm.h); }
}
static int rev_active(const tx_fx_reverb_t *r)
{
    return r->cur == TX_RVT_INF ? tx_inf_active(&r->u.i) : r->cur == TX_RVT_PLATE ? tx_plate_active(&r->u.p)
         : r->cur == TX_RVT_ROOM ? !tx_chamber_quiet(&r->u.rm.c) : 0;
}

void tx_fx_reverb_init(tx_fx_reverb_t *r)
{
    unsigned i;
    tx_fx_memset(r, 0, sizeof *r - sizeof r->u);
    for (i = 0; i < TX_RV_NP; i++) r->p[i] = tx_fx_reverb_params[i].def;
    r->damp = 60;
    r->dk = 64;
    r->shg = r->shd = 32768;
    r->cur = TX_RVT_OFF;
    r->rng = 0x6C078965u;
    r->swr = 32768;
    r->pz = -1;
    r->xin = 32768;
}
void tx_fx_reverb_relatch(tx_fx_reverb_t *r) { *(volatile int32_t *)&r->pz = -1; }
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
/* (LOW CUT, PREDELAY and ROOM's body out: in flash on the FM-1, TX_FAR; a control block's run each: the RAM is the
 * dry / wet blend's) */
static void rev_locut(tx_fx_reverb_t *r, int32_t *x, int32_t v)   /* x - lowpass(x), 20 .. 800 Hz */
{
    uint32_t j;
    int32_t lc = r->lc, a;
    if (v != r->lcv) { r->lcv = v; r->lca = v ? TX_FAR(locut_a)(v) : 0; }
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
        uint32_t w = r->pd_w;
        r->pd[w] = x[j];
        if (r->pd_fill < TX_PD_LEN) r->pd_fill++;               /* (a tap behind what was written reads 0: no memset) */
        a = r->pd_d < r->pd_fill ? r->pd[w >= r->pd_d ? w - r->pd_d : w + TX_PD_LEN - r->pd_d] : 0;
        if (r->pd_x) {                                          /* 2048 samples (46 ms) from the old tap to the new one */
            int32_t b = r->pd_d0 < r->pd_fill ? r->pd[w >= r->pd_d0 ? w - r->pd_d0 : w + TX_PD_LEN - r->pd_d0] : 0;
            a = b + (int32_t)(((int64_t)(a - b) * (int32_t)(r->pd_x << 4)) >> 15);
            if (++r->pd_x > 2048u) r->pd_x = 0;
        }
        x[j] = a;
        r->pd_w = w + 1u < TX_PD_LEN ? w + 1u : 0u;
    }
    if (!r->pd_x && !r->pd_d && !tgt) r->pd_on = 0;
}

/* ROOM: the CHAMBER over the gathered run rm[r0 .. r0 + rn) at once (its per-call costs once a block), the wet part
 * (out - in) added to both sides, each control block faded by its TYPE-change fade (fade / 8 .. (fade - 1) / 8) */
TX_INLINE int32_t rv_gw(const tx_fx_reverb_t *r, uint32_t j)   /* the wet's gain at sample j of the block (Q15) */
{
    int32_t a = r->gw[j / CTL], b = r->gw[j / CTL + 1u];
    return a + (((b - a) * (int32_t)((j & (CTL - 1u)) + 1u)) >> 5);
}
/* the dry / wet blend (tx_fx.h): L, R = dry x d + wet (wl, wr; 0: none) x w x k, the gains (Q15) straight from d0, w0
 * to d1, w1 across the n samples (exact at the end; 32768 and no wet: untouched) */
void tx_fx_drywet(int32_t *L, int32_t *R, const int32_t *wl, const int32_t *wr, unsigned n, int32_t d0,
                         int32_t d1, int32_t w0, int32_t w1, int32_t k)
{
    int32_t da = d0 * 256, wa = w0 * 256, ds, ws;
    unsigned j;
    if ((d0 == 32768 && d1 == 32768 && !wl) || !n) return;
    ds = (d1 - d0) * 256 / (int32_t)n; ws = (w1 - w0) * 256 / (int32_t)n;
    for (j = 0; j < n; j++) {
        int32_t l, rr;
        da += ds; wa += ws;
        if (j + 1u == n) da = d1 * 256, wa = w1 * 256;
        l = mq15s(L[j], da >> 8); rr = mq15s(R[j], da >> 8);
        if (wl) { l += mq15s(wl[j], wa >> 8) * k; rr += mq15s(wr[j], wa >> 8) * k; }
        L[j] = l; R[j] = rr;
    }
}
/* ROOM's body, its wet part (w, the run's r->rn samples) x the wet's gain, into both sides */
static void rv_body(const tx_fx_reverb_t *r, int32_t *L, int32_t *R, const int32_t *w)
{
    uint32_t j;
    for (j = 0; j < r->rn; j++) {
        int32_t v = mq15s(w[j], rv_gw(r, r->r0 + j));
        L[r->r0 + j] += v; R[r->r0 + j] += v;
    }
}
static void room_flush(tx_fx_reverb_t *r, int32_t *L, int32_t *R)
{
    int32_t keep[TX_BLOCK], *x = r->u.rm.rm + r->r0;
    uint32_t j, n = r->rn;
    for (j = 0; j < n; j++) keep[j] = x[j];
    tx_chamber_process(&r->u.rm.c, x, n);
    for (j = 0; j < n; j++) {
        int32_t w = x[j] - keep[j], f = r->rf[(r->r0 + j) / CTL];
        if (f) w = (int32_t)(((int64_t)w * ((f * 32 - (int32_t)(j & (CTL - 1u))) * 128)) >> 15);
        keep[j] = w;
    }
    TX_FAR(rv_body)(r, L, R, keep);
    r->rn = 0;
}

/* ROOM on two cores: the body over the gathered run (as room_flush, no fade), its wet part into rw */
TX_AUDIO_CODE void tx_fx_reverb_body(tx_fx_reverb_t *r)
{
    int32_t *x = r->u.rm.rm + r->r0;
    uint32_t j, n = r->rn;
    for (j = 0; j < n; j++) r->u.rm.rw[j] = x[j];
    tx_chamber_process(&r->u.rm.c, x, n);
    for (j = 0; j < n; j++) r->u.rm.rw[j] = x[j] - r->u.rm.rw[j];
}

TX_AUDIO_CODE void tx_fx_reverb_process(tx_fx_reverb_t *r, int32_t *L, int32_t *R, unsigned n)
{
    int32_t v = r->p[TX_RV_MIX], tgt = r->p[TX_RV_TYPE], zv = r->p[TX_RV_SIZE];
    int32_t send = tgt && v ? 32766 : 0, size = 40 + (zv ? zv : v) * 70 / 127;   /* (the send: the mid whole; MIX is
                                                         * the dry / wet at the end) */
    int32_t mt = TX_FX_MIXQ(v), dt = tgt ? mt : 0, gd[TX_BLOCK / CTL + 1u];   /* MIX's place: the wet's, the dry's
                                                         * (TYPE OFF: the dry whole); the dry's gain at each control
                                                         * block's edge (Q15) */
    uint32_t nb = (n > TX_BLOCK ? TX_BLOCK : n) / CTL, b, done = 0;
    int32_t dv = r->p[TX_RV_DECAY], tv = r->p[TX_RV_TONE], md = (r->p[TX_RV_MOD] + 4) >> 3, lcv = r->p[TX_RV_LOWCUT];
    int32_t off = ((dv - 64) * 15) / 32;
    int32_t damp = tv >= 64 ? 60 - ((tv - 64) * 60) / 63 : 60 + ((64 - tv) * 90) / 64;
    uint32_t sb, i, j, pd = FX_PDLY_SAMPLES(r->p[TX_RV_PREDELAY]);
    int quiet = !r->running, dark = tgt == TX_RVT_DARK, shon = r->p[TX_RV_SHIMMER] != 0, two = 0;
    if (!send && !r->running && !r->fade && !r->xd) { r->xw = mt; return; }   /* (off: the mix untouched) */
    r->running = 1;
    TX_FAR(tx_fx_mix_glide)(&r->xd, dt, 512, nb, 0, gd);       /* MIX glides: 512 a control block, 46 ms end to */
    TX_FAR(tx_fx_mix_glide)(&r->xw, mt, 512, nb, r->gw, 0);    /* end (in flash) */
    if (tgt == TX_RVT_DARK) damp += 60;                         /* DARK: TONE's damping 60 steps darker */
    if (r->body_run && tgt == TX_RVT_ROOM && r->cur == TX_RVT_ROOM && send && !r->fade && r->xin == 32768 && n == TX_BLOCK
        && !r->rn) {
        /* ROOM on two cores: every control block's send first (as below: the loop then takes them from sv), the body
         * handed over, the tail here; no TYPE change and a send (no fade, no early end): the same samples */
        for (sb = 0; sb < n; sb += CTL) {
            int32_t *in = r->u.rm.sv + sb;
            unsigned cp[TX_CH_NP], q;
            for (j = 0; j < CTL; j++) {
                int32_t mq = (int32_t)(((L[sb + j] >> 1) + (R[sb + j] >> 1) + 128) >> 8);
                in[j] = clampi((int32_t)(((int64_t)mq * send) >> 15), -131071, 131071);
            }
            if (lcv || r->lc) TX_FAR(rev_locut)(r, in, lcv);
            if (pd || r->pd_on) TX_FAR(predelay)(r, in, pd);
            if (r->off != off) r->off += r->off < off ? 1 : -1;
            if (r->damp != damp) r->damp += r->damp < damp ? (damp - r->damp > 2 ? 2 : damp - r->damp)
                                                           : -(r->damp - damp > 2 ? 2 : r->damp - damp);
            if (r->dk != dv) r->dk += r->dk < dv ? 1 : -1;
            r->u.rm.svo[sb / CTL] = r->off; r->u.rm.svd[sb / CTL] = r->damp;
            cp[TX_CH_SIZE] = (unsigned)(zv ? zv : 76); cp[TX_CH_ABSORB] = (unsigned)(127 - r->dk);
            cp[TX_CH_BODY] = (unsigned)((tv * 40) >> 6 > 127 ? 127 : (tv * 40) >> 6); cp[TX_CH_TAIL] = 0u;
            for (q = 0; q < TX_CH_NP; q++) if (r->u.rm.c.p[q] != cp[q]) TX_FAR(tx_chamber_param)(&r->u.rm.c, q, cp[q]);
            if (r->rn == 0) r->r0 = sb;
            for (j = 0; j < CTL; j++) r->u.rm.rm[sb + j] = in[j] * 256;
            r->rf[sb / CTL] = 0;
            r->rn += CTL;
        }
        two = r->body_run() ? 1 : 2;                            /* 1: on the other core; 2: refused, here at the end */
    }
    for (sb = 0; sb + CTL <= n; sb += CTL) {
        int32_t act = 0, *om = r->m, *os = r->s, *in = r->inb, spk = 0;
        uint32_t cur;
        if (two) {                                              /* (the sends are in: the tail's control block) */
            in = r->u.rm.sv + sb;
            r->off = r->u.rm.svo[sb / CTL]; r->damp = r->u.rm.svd[sb / CTL];
            cur = r->cur;
            goto tail;
        }
        if (tgt && kern((uint32_t)tgt) != r->cur && !r->fade) {
            if (quiet || !r->cur) TX_FAR(rev_begin)(r, (uint32_t)tgt);   /* (TYPE starts: in XIP) */
            else r->fade = RV_FADE;
        }
        for (j = 0; j < CTL; j++) {                             /* the send: the mid, Q23 -> Q15 (64-bit: the bus may */
            int32_t mq = (int32_t)(((L[sb + j] >> 1) + (R[sb + j] >> 1) + 128) >> 8);   /* be far over full scale) */
            in[j] = clampi((int32_t)(((int64_t)mq * send) >> 15), -131071, 131071);
            spk |= mq ^ (mq >> 31);                             /* (SWELL: the level, a peak to a power of 2) */
        }
        if (lcv || r->lc) TX_FAR(rev_locut)(r, in, lcv);
        if (pd || r->pd_on) TX_FAR(predelay)(r, in, pd);
        if (r->xin < 32768)                                     /* a TYPE's start: its send fades in (no step into */
            for (j = 0; j < CTL; j++) {                         /* its empty lines) */
                r->xin = r->xin + 128 < 32768 ? r->xin + 128 : 32768;
                in[j] = (int32_t)(((int64_t)in[j] * r->xin) >> 15);
            }
        if (r->off != off) r->off += r->off < off ? 1 : -1;
        if (r->damp != damp) r->damp += r->damp < damp ? (damp - r->damp > 2 ? 2 : damp - r->damp)
                                                       : -(r->damp - damp > 2 ? 2 : r->damp - damp);
        if (r->dk != dv) r->dk += r->dk < dv ? 1 : -1;
        cur = r->cur;
        if (cur == TX_RVT_ROOM) {                               /* the CHAMBER on the send, full rate: gathered here, */
            unsigned cp[TX_CH_NP], q;                           /* run over the whole run of control blocks (room_flush) */
            cp[TX_CH_SIZE] = (unsigned)(zv ? zv : 76); cp[TX_CH_ABSORB] = (unsigned)(127 - r->dk);
            cp[TX_CH_BODY] = (unsigned)((tv * 40) >> 6 > 127 ? 127 : (tv * 40) >> 6); cp[TX_CH_TAIL] = 0u;
            for (q = 0; q < TX_CH_NP; q++) if (r->u.rm.c.p[q] != cp[q]) TX_FAR(tx_chamber_param)(&r->u.rm.c, q, cp[q]);   /* changes only */
            if (r->rn == 0) r->r0 = sb;
            for (j = 0; j < CTL; j++) r->u.rm.rm[sb + j] = in[j] * 256;   /* Q15 -> Q23: the chamber's scale */
            r->rf[sb / CTL] = r->fade;
            r->rn += CTL;
        }
    tail:
        {   /* the kernels (and ROOM's tail: a short hall) */
            int32_t ipk = 0;                                    /* (the send's peak: the shimmer's guard reads it) */
            for (i = 0; i < HL_NC; i++) ipk |= in[i] ^ (in[i] >> 31);
            if (shon) for (i = 0; i < HL_NC; i++) in[i] += r->sh[i];   /* the shimmer's feed, into the send: through
                                                                 * the tanks' DC blocker (no DC, no windows' 21.5 Hz loop) */
            if (cur == TX_RVT_HALL || cur == TX_RVT_INF || cur == TX_RVT_PLATE)   /* the tanks' input: the DC blocker */
                tx_rev_down(in, &r->hp, &r->he, r->x);   /* (here, in the twin on core 1) */
            if (cur == TX_RVT_HALL || cur == TX_RVT_ROOM) {     /* HALL: the HQ tank (in flash; its T60 law there); */
                int hq = cur == TX_RVT_HALL;                    /* ROOM's tail: a hall of size 2 .. 32 (T60 ~0.3 .. 1 s); */
                int32_t rz = 2 + (zv ? zv : 64) * 30 / 127 + r->off / 2;   /* (2 at least: a small SIZE with a low DECAY
                                                                 * went under 0, which hall_gains read as its longest) */
                (hq ? TX_FAR(tx_hq_run) : tx_hall_run)(hq ? (tx_hall_t *)(void *)&r->u.hq.q : &r->u.rm.h, hq ? r->x : in,
                    0,
                    hq ? size + r->off : rz < 2 ? 2 : rz, r->damp, md, om, os,
                    two == 1 ? 0 : &r->par);                    /* (one call: RAM text. ROOM on one core here while the
                                                                 * body holds the other) */
            }
            else if (cur == TX_RVT_INF) {
                tx_rev_ctl_t c;
                c.decay = r->dk; c.damp = r->damp; c.size = zv ? zv : TX_RV_INF_AUTO; c.mod = r->p[TX_RV_MOD]; c.dark = dark;
                c.drift = tgt == TX_RVT_DRIFT ? &r->rng : 0; c.par = &r->par;
                c.freeze = kern((uint32_t)tgt) != TX_RVT_INF || r->dk < 120 ? 0 : r->dk < 127 ? 1 : 2;
                TX_FAR(tx_inf_run)(&r->u.i, r->x, om, os, &c);
                if (tgt == TX_RVT_SWELL || r->swr != 32768) swell(r, tgt == TX_RVT_SWELL ? spk : 0, om, os);
            } else if (cur == TX_RVT_PLATE) {
                tx_rev_ctl_t c;
                if (r->pz < 0 || !TX_FAR(tx_plate_active)(&r->u.p)) r->pz = v;   /* (AUTO: MIX's size, taken from silence) */
                c.decay = r->dk; c.damp = r->damp; c.size = zv ? zv : r->pz; c.mod = r->p[TX_RV_MOD];
                c.freeze = 0; c.dark = 0; c.drift = 0; c.par = &r->par;
                TX_FAR(tx_plate_run)(&r->u.p, r->x, om, os, &c);
            }
            if (cur && (r->p[TX_RV_SHIMMER] || shon)) {         /* the shimmer for the next control block */
                shimmer(r, om, ipk > SH_LIVE);
                shon = r->p[TX_RV_SHIMMER] != 0;
                if (!shon) for (i = 0; i < HL_NC; i++) shon |= r->sh[i] != 0;   /* (its last feed still to go in) */
                if (!r->p[TX_RV_SHIMMER]) { for (i = 0; i < HL_NC; i++) r->sh[i] = 0; shon = 0; }
                for (i = 0; i < HL_NC; i++) act |= r->sh[i];
            }
            if (cur && (r->fade || (v && !send))) {             /* a fade: the TYPE's out x (fade / 8) ..; OFF: watched */
                int32_t g = (int32_t)r->fade << 12;             /* (the step first: the last sample at 0) */
                for (i = 0; i < HL_NC; i++) {
                    act |= om[i] | os[i];
                    if (r->fade) { g -= 128; om[i] = mul15(om[i], g); os[i] = mul15(os[i], g); }
                }
                if (r->fade) act = 1;
            }
            if (cur && !v)
                for (i = 0; i < HL_NC; i++) act |= om[i] | os[i];
            b = sb / CTL;                               /* the dry x its gain at MIX's place, the return x the wet's (the tail at
                                                         * half: MIX 127 as the old full send's); mid / side -> L, R */
            if (cur && (r->gw[b] || r->gw[b + 1u])) {
                for (i = 0; i < HL_NC; i++) { int32_t a = om[i]; om[i] = a + os[i]; os[i] = a - os[i]; }
                tx_fx_drywet(L + sb, R + sb, om, os, CTL, gd[b], gd[b + 1u], r->gw[b] >> 1, r->gw[b + 1u] >> 1, 256);
            } else tx_fx_drywet(L + sb, R + sb, 0, 0, CTL, gd[b], gd[b + 1u], 0, 0, 0);
            done = b + 1u;
        }
        if (r->fade && !--r->fade) {
            if (r->rn) room_flush(r, L, R);                     /* (the room's run before its state is reused) */
            TX_FAR(rev_begin)(r, tgt ? (uint32_t)tgt : cur ? cur : TX_RVT_HALL);
        }
        else if (!send && !act && !TX_FAR(rev_active)(r)) {             /* the tail has ended in exact silence */
            r->running = 0; r->pd_on = 0; quiet = 1;
            for (j = 0; j < sizeof r->inb / sizeof r->inb[0]; j++) r->inb[j] = 0;
            break;
        }
    }
    if (two) {                                         /* ROOM on two cores: its body's wet part, added */
        if (two == 2 || r->body_wait()) tx_fx_reverb_body(r);   /* (refused or lost: here; lost mid-way is a fault's
                                                 * recovery, not the same samples) */
        TX_FAR(rv_body)(r, L, R, r->u.rm.rw);
        r->rn = 0;
    }
    else if (r->rn) room_flush(r, L, R);               /* (the room's body: its end is seen by the next block) */
    if (done < nb) tx_fx_drywet(L + done * CTL, R + done * CTL, 0, 0, (nb - done) * CTL, gd[done], gd[nb], 0, 0, 0);
                                                        /* (the tail ended early: the dry on, straight to the end) */
}

/* ---- the chain */
void tx_fx_init(tx_fx_t *fx)
{
    unsigned k;
    tx_fx_drive_shared_init(&fx->drv);
    for (k = 0; k < 2u; k++) { tx_fx_drive_init(&fx->drive[k]); tx_fx_drive_share(&fx->drive[k], &fx->drv); }
    tx_fx_drive_pair(&fx->drive[1], &fx->drive[0]);
    fx->scr1 = 0; fx->dr_x = 0; fx->dr_n = 0;
    tx_fx_mfilter_init(&fx->mf);
    tx_fx_grain_init(&fx->gd);
    tx_fx_reverb_init(&fx->rev);
    fx->reserve_us = TX_FX_RESERVE_US;
    fx->room_us = -1;
    fx->up = 0;
    fx->two = 0;
}
TX_AUDIO_CODE void tx_fx_lane(void *u, unsigned k, int32_t *x, unsigned n) { (void)u; (void)k; (void)x; (void)n; }
TX_AUDIO_CODE void tx_fx_lane_run(void *u, unsigned k, int32_t *x, unsigned n, unsigned core)
{
    (void)u; (void)k; (void)x; (void)n; (void)core;     /* (no lane insert: each lane's FILTER is retired) */
}
uint32_t tx_fx_lane_us(const tx_fx_t *fx, unsigned k) { (void)fx; (void)k; return 0u; }
/* the mix's DRIVE: one set of settings on the pair */
void tx_fx_drive_set(tx_fx_t *fx, unsigned id, unsigned v)
{
    if (id >= TX_FX_MF_ID) { tx_fx_mfilter_param(&fx->mf, id - TX_FX_MF_ID, v); return; }   /* (the FILTER after it) */
    tx_fx_drive_param(&fx->drive[0], id, v);
    tx_fx_drive_param(&fx->drive[1], id, v);
}
/* the pair's R on the other core (the reverb's hooks, rev.par), or here (a lost job): in XIP either way */
static void drv_job1(void *a) { tx_fx_t *fx = (tx_fx_t *)a; TX_FAR(tx_fx_drive_run)(&fx->drive[1], fx->dr_x, fx->dr_n, fx->scr1); }
static void drv_job0(void *a) { tx_fx_t *fx = (tx_fx_t *)a; TX_FAR(tx_fx_drive_run)(&fx->drive[1], fx->dr_x, fx->dr_n, fx->scr1); }
static void drive_pair(tx_fx_t *fx, int32_t *L, int32_t *R, unsigned n, unsigned c)
{
    unsigned t;
    int taken;
    if (!fx->drive[0].on && !fx->drive[0].p[TX_DR_AMOUNT] && !fx->drive[1].on) return;   /* (off: the mix untouched) */
    tx_fx_drive_prep(&fx->drive[0]);                    /* (a TYPE change: the pair together, the first first) */
    tx_fx_drive_prep(&fx->drive[1]);
    if (c) {                                            /* core 1 (the bus one block behind): both, its twins */
        TX_FAR(tx_fx_drive_run)(&fx->drive[0], L, n, 0);
        TX_FAR(tx_fx_drive_run)(&fx->drive[1], R, n, 0);
        return;
    }
    t = fx->drive[0].cur;                               /* two cores: each side writes only its own state, samples and
                                                         * scratch; TSH8 stays on one (R runs on the tables L builds) */
    if (t == fx->drive[1].cur && (t == TX_DRT_FDRV || (t != TX_DRT_TSH8 && fx->scr1))) {
        fx->dr_x = R; fx->dr_n = n;
        taken = rv_fork(&fx->rev.par, drv_job1, fx);
        TX_FAR(tx_fx_drive_run)(&fx->drive[0], L, n, 0);
        if (taken) rv_join(&fx->rev.par, drv_job0, fx, taken);
        else TX_FAR(tx_fx_drive_run)(&fx->drive[1], R, n, 0);   /* (one core: as below, the shared scratch) */
        return;
    }
    TX_FAR(tx_fx_drive_run)(&fx->drive[0], L, n, 0);     /* (in XIP: the bus runs on core 1, lanes.c LN_PIPE) */
    TX_FAR(tx_fx_drive_run)(&fx->drive[1], R, n, 0);
}
static void drive_mix(tx_fx_t *fx, int32_t *L, int32_t *R, unsigned n, unsigned c)
{
    drive_pair(fx, L, R, n, c);
    TX_FAR(tx_fx_mfilter_process)(&fx->mf, L, R, n);   /* the FILTER after the DRIVE (off: returns at once; in XIP) */
}
TX_AUDIO_CODE void tx_fx_drive_mix(void *u, int32_t *L, int32_t *R, unsigned n) { drive_mix((tx_fx_t *)u, L, R, n, 0u); }
TX_AUDIO_CODE void tx_fx_drive_mix_c1(void *u, int32_t *L, int32_t *R, unsigned n) { drive_mix((tx_fx_t *)u, L, R, n, 1u); }
uint32_t tx_fx_drive_mix_us(const tx_fx_t *fx)
{
    return tx_fx_drive_us(&fx->drive[0]) + tx_fx_drive_us(&fx->drive[1]) + tx_fx_mfilter_us(&fx->mf);
}
/* the grain delay's dynamic cap: the grains the effects reserve leaves after the reverb's TYPE (FM-1 us a block,
 * CPU-BUDGET.md section 8: the reverb's worst per TYPE, the delay's line + echo, a sounding grain) */
#define FX_US_GD_BASE 220u         /* the line, the echo, the feedback (make tx-budget section 4, 2026-10-25) */
#define FX_US_GD_WOW 20u           /* MOD: the echo's moving read (make tx-budget section 4: ~15) */
#define FX_US_GD_GRAIN1 14u        /* a grain at rate 1 (whole samples: a run decoded, one read a sample) */
#define FX_US_GD_GRAINP 42u        /* a pitched or reversed grain (its run decoded, the interpolation) */
#define FX_US_SHIMMER 100u         /* the shimmer on top of its reverb (75 before its second low-pass pole, floor and
                                    * decay guard: make tx-budget's HALL / INFINITE + SHIMMER rose ~+20 .. +70) */
static const uint16_t FX_US_REV[TX_RVT_N] = {0, 390, 210, 280, 280, 280, 290, 180};
/* the measured room (room_us >= 0, the device): the delay's own costs on the FM-1 (w8, 4 lanes' code around it: the
 * line, the echo and the feedback; a sounding grain), the cap at once down to what fits, but never under one grain (at
 * cap 0 the grains fall silent: the target is 1600 us under the block, a grain ~60), up by one every RV_FADE blocks
 * (a sound whose cost swings block to block does not start and stop grains with it) */
#define FX_DEV_GD_BASE 283u
#define FX_DEV_GD_GRAIN1 44u
#define FX_DEV_GD_GRAINP 64u
TX_AUDIO_CODE uint32_t tx_fx_gd_dev_us(const tx_fx_t *fx, uint32_t cap)   /* the delay at cap grains (FM-1 us) */
{
    int pitched = fx->gd.p[TX_GD_PITCH] != 64 || fx->gd.p[TX_GD_REVERSE] || fx->gd.mg;   /* (MOD: grains pitched, bent) */
    return FX_DEV_GD_BASE + (fx->gd.mg ? FX_US_GD_WOW : 0u) + cap * (pitched ? FX_DEV_GD_GRAINP : FX_DEV_GD_GRAIN1);
}
TX_AUDIO_CODE void tx_fx_gd_cap(tx_fx_t *fx)
{
    int pitched = fx->gd.p[TX_GD_PITCH] != 64 || fx->gd.p[TX_GD_REVERSE] || fx->gd.mg;   /* (MOD: grains pitched, bent) */
    uint32_t room, cap;
    if (fx->room_us < 0 && fx->two) { fx->gd.cap = TX_GD_MAXG; return; }   /* (the model, two cores: every grain) */
    if (fx->room_us < 0) {                                         /* the model: the reserve less the reverb's TYPE */
        uint32_t t = fx->rev.p[TX_RV_TYPE], c = fx->rev.cur, rv = t < TX_RVT_N ? FX_US_REV[t] : 0;
        if (c < TX_RVT_N && FX_US_REV[c] > rv) rv = FX_US_REV[c];      /* (a TYPE change: the dearer of the two) */
        if (rv && fx->rev.p[TX_RV_SHIMMER]) rv += FX_US_SHIMMER;
        if (fx->gd.mg) rv += FX_US_GD_WOW;
        room = fx->reserve_us > rv + FX_US_GD_BASE ? fx->reserve_us - rv - FX_US_GD_BASE : 0;
        room /= pitched ? FX_US_GD_GRAINP : FX_US_GD_GRAIN1;
        fx->gd.cap = (uint8_t)(room > TX_GD_MAXG ? TX_GD_MAXG : room);
        return;
    }
    room = (uint32_t)fx->room_us > FX_DEV_GD_BASE + (fx->gd.mg ? FX_US_GD_WOW : 0u)
         ? (uint32_t)fx->room_us - FX_DEV_GD_BASE - (fx->gd.mg ? FX_US_GD_WOW : 0u) : 0;
    cap = room / (pitched ? FX_DEV_GD_GRAINP : FX_DEV_GD_GRAIN1);
    if (cap > TX_GD_MAXG) cap = TX_GD_MAXG;
    if (!cap) cap = 1;
    if (cap < fx->gd.cap) fx->gd.cap = (uint8_t)cap, fx->up = 0;
    else if (cap == fx->gd.cap) fx->up = 0;
    else if (++fx->up >= RV_FADE) fx->gd.cap++, fx->up = 0;
}
TX_AUDIO_CODE int tx_fx_bus_begin(void *u, unsigned n)
{
    tx_fx_t *fx = (tx_fx_t *)u;
    tx_fx_gd_cap(fx);
    return tx_fx_grain_begin(&fx->gd, n);
}
TX_AUDIO_CODE void tx_fx_bus_end(void *u, int32_t *L, int32_t *R, unsigned n)
{
    tx_fx_t *fx = (tx_fx_t *)u;
    TX_FAR(tx_fx_reverb_process)(&fx->rev, L, R, n);   /* the REVERB first (in XIP, its whole chain: the bus runs on
                                                         * core 1), */
    TX_FAR(tx_fx_grain_end)(&fx->gd, L, R, n);         /* then the GRAIN DELAY on what it gives (in RAM on the FM-1:
                                                         * TX_RAM; its voices ran ahead from the line the last block
                                                         * wrote, so the order changes no timing) */
}
TX_AUDIO_CODE void tx_fx_bus_end_c1(void *u, int32_t *L, int32_t *R, unsigned n)   /* (core 1: the twin; the reverb's
                                                         * hooks hand nothing on from there: the caller's) */
{
    tx_fx_t *fx = (tx_fx_t *)u;
    TX_FAR(tx_fx_reverb_process)(&fx->rev, L, R, n);   /* (in XIP: its whole chain) */
    TX_C1(tx_fx_grain_end)(&fx->gd, L, R, n);
}
TX_AUDIO_CODE void tx_fx_bus(void *u, int32_t *L, int32_t *R, unsigned n)
{
    tx_fx_t *fx = (tx_fx_t *)u;
    if (tx_fx_bus_begin(fx, n)) TX_FAR(tx_fx_grain_voices)(&fx->gd, n);
    tx_fx_bus_end(fx, L, R, n);
}
void tx_fx_mix_glide(int32_t *x, int32_t t, int32_t step, unsigned np, int32_t *gs, int32_t *gc)
{
    unsigned k;
    for (k = 0; k <= np; k++) {
        if (k) *x += *x < t ? (t - *x > step ? step : t - *x) : -(*x - t > step ? step : *x - t);
        if (gs) gs[k] = tx_fx_mixw(*x);
        if (gc) gc[k] = tx_fx_mixd(*x);
    }
}
int tx_fx_bus_quiet(void *u) { return tx_fx_reverb_quiet(&((tx_fx_t *)u)->rev) && tx_fx_grain_quiet(&((tx_fx_t *)u)->gd); }
