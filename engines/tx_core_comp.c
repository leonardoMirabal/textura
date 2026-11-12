/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura COMP core: the master bus compressor (tx_core_comp.h). */
#include "tx_core_comp.h"
#include "tx_core_math.h"

#define CB 32u                     /* the control block, frames */
#define KNEE (120 * 13606 / 100)   /* 12 dB (power log2 Q12) */
#define K_SA 119                   /* the slow envelope a control block (0.726 ms), Q16: attack 400 ms */
#define K_SR 32                    /*                                                    release 1.5 s */
#define HOLD 32u                   /* the fast envelope held after its last rise, control blocks (23 ms) */
#define GSH 7                      /* the reduction's release: 2^-7 a control block (~93 ms) */
#define PDR 4                      /* the slowest adapted release: REL's speed / 2^4 */
#define SAS 4                      /* a rise under +3.5 dB (power x 2.25) caught at ATK's speed / 16 ... */
#define SAK 59                     /* ... when REL is fast (59 and under: 150 ms, quick enough to fall between beats) */
#define HKS 1                      /* held: the adapted release at REL's speed / 2 or slower (a chord's beats caught again) */
#define HSH 11                     /* then the net reduction averaged over all of it, at most 2^11 control blocks (1.5 s) */
#define HUP (340 * 16)             /* ... never more than 0.25 dB from the reduction of the moment (power log2 Q12 x 16) */
#define NRB 32u                    /* the programme gone: under the envelope by 3 dB for 32 control blocks (23 ms) */
#define PDT 6                      /* then the release back to REL's: 2^-6 a control block (~46 ms) */
#define T0 64                      /* the auto make-up and the body lift: none up to THR 64 (-20 dBFS), the factory one */
#define T4 (-(T0 * 54424) / 127)  /* THR 64's threshold (power log2 Q12) */
#define BETA 90                    /* the body lift: 0.35 of the steady reduction given back (Q8), from THR 88 */
#define BSH 8                      /* the steady reduction's rise: 2^-8 a control block (~186 ms) */

const tx_param_t tx_comp_params[TX_CP_NP] = {
    {"COMP", 1, 1, TX_TEXT("the bus compressor, the first effect on the mix: off / on")},
    {"THR", 127, 64, TX_TEXT("threshold 0 .. -40 dBFS (64: -20 dBFS)")},
    {"ATTACK", 127, 108, TX_TEXT("attack 0.1 .. 50 ms (108: 20 ms)")},
    {"REL", 127, 59, TX_TEXT("release 20 ms .. 1.5 s (59: 150 ms; a dense sound holds its gain longer: no pumping)")},
    {"RATIO", 7, 0, TX_TEXT("ratio 1.5:1 2:1 3:1 4:1 6:1 8:1 10:1 20:1")},
    {"MAKEUP", 127, 14, TX_TEXT("make-up 0 .. +24 dB (14: +2.6 dB)")},
};

/* the auto make-up (power log2 Q12; with MKUP at most +24 dB) and the body lift (Q8), from THR and RAT: the reduction
 * per unit of slope over THR (power log2 Q12, linear between these THR values; none up to T0), times the slope */
static void cauto(tx_comp_t *c)
{
    static const int16_t AT[5] = {T0, 80, 96, 112, 127}, AQ[5] = {0, 3536, 6998, 11320, 15487};
    int32_t t = c->p[TX_CP_THR], i = 1, q = 0;
    if (t > T0) {
        while (t > AT[i]) i++;
        q = AQ[i - 1] + (AQ[i] - AQ[i - 1]) * (t - AT[i - 1]) / (AT[i] - AT[i - 1]);
    }
    q = (q * c->s) >> 12;
    c->amk = q > 32654 - c->mk ? 32654 - c->mk : q;
    c->beta = (uint8_t)(t <= T0 ? 0 : t >= T0 + 24 ? BETA : BETA * (t - T0) / 24);   /* (the body lift fades in) */
}

/* the gain computer: the reduction (power log2 Q12, >= 0) at level lp, over a soft knee */
static int32_t gc(const tx_comp_t *c, int32_t lp, int32_t t)
{
    int32_t over = lp - t, x;
    if (over <= -KNEE / 2) return 0;
    if (over >= KNEE / 2) return (over >> 4) * c->s >> 8;
    x = over + KNEE / 2;                                      /* 0 .. KNEE: x^2 < 2^28 */
    return ((x * x) / (2 * KNEE) >> 4) * c->s >> 8;
}

/* a one-pole's coefficient a control block (Q16, 1 .. 65535) from log2(block / tau) (Q16): 1 - 2^-(block / tau log2 e) */
static int32_t k_of(int32_t l2)
{
    int32_t lz = l2 + 34655;                                  /* + log2(log2 e) */
    uint32_t z;
    if (lz >= (4 << 16)) return 65535;
    z = tx_exp2(lz + (16 << 16));                             /* Q16 */
    if (z >= (16u << 16)) return 65535;
    z = 65536u - tx_exp2((16 << 16) - (int32_t)z);
    return z < 1u ? 1 : z > 65535u ? 65535 : (int32_t)z;
}

void tx_comp_init(tx_comp_t *c)
{
    unsigned i;
    tx_zero(c, sizeof *c);
    c->g = 16384;
    for (i = 0; i < TX_CP_NP; i++) tx_comp_param(c, i, i == TX_CP_ON ? 0u : tx_comp_params[i].def);
}

void tx_comp_param(tx_comp_t *c, unsigned id, unsigned v)
{
    static const int16_t SLOPE[8] = {1365, 2048, 2731, 3072, 3413, 3584, 3686, 3891};   /* 1 - 1/R, Q12 (Frodhes') */
    int32_t x;
    if (id >= TX_CP_NP) return;
    if (v > tx_comp_params[id].max) v = tx_comp_params[id].max;
    c->p[id] = (uint8_t)v;
    x = (int32_t)v;
    switch (id) {
    case TX_CP_THR: c->t = -(x * 54424) / 127; break;                         /* -40 dB x v / 127 */
    case TX_CP_ATK: c->ka = k_of(187381 - x * 587582 / 127); break;          /* tau 0.1 ms x 500^(v / 127) */
    case TX_CP_REL:
        c->kr = k_of(-313570 - x * 408211 / 127);                            /* tau 20 ms x 75^(v / 127) */
        for (x = GSH; x > 0 && (c->kr << (x + 1)) > 65536; x--) ;             /* the smoothing at most half REL's tau */
        c->gsh = (uint8_t)x;
        break;
    case TX_CP_RATIO: c->s = SLOPE[v & 7u]; break;
    case TX_CP_MKUP: c->mk = x * 32654 / 127; break;                         /* +24 dB x v / 127 */
    }
    if (id == TX_CP_THR || id == TX_CP_RATIO || id == TX_CP_MKUP) cauto(c);
}

static int32_t env(int32_t e, int32_t p, int32_t ka, int32_t kr)
{
    return e + ((((p - e) >> 11) * (p > e ? ka : kr)) >> 5);   /* |p - e| < 2^26: 2^15 x 2^16 < 2^31 */
}

TX_AUDIO_CODE void tx_comp_process(tx_comp_t *c, int32_t *L, int32_t *R, unsigned n)
{
    unsigned j0, j, m;
    if (!c->p[TX_CP_ON] && c->g == 16384) { c->on = 0; c->gr = 0; return; }   /* off: the samples untouched */
    for (j0 = 0; j0 < n; j0 += m) {
        int32_t g0 = c->g, gt, dg, g, p = 0, det;
        m = n - j0 < CB ? n - j0 : CB;
        for (j = j0; j < j0 + m; j++) {                       /* the block's power: the louder side, squared */
            int32_t a = L[j] < 0 ? -L[j] : L[j], b = R[j] < 0 ? -R[j] : R[j], v = (a > b ? a : b) >> 10;
            v = v > 16383 ? 16383 : v;                        /* (2 full scale: v^2 < 2^28) */
            p += (v * v) >> 5;
        }
        if (m != CB) p = p / (int32_t)m * (int32_t)CB;
        if (!c->on) { c->ef = c->es = p; c->on = 1; c->hd = 0; c->gs = -1; c->kq = c->kr; c->mg = c->mk;
            c->bs = 0; c->amg = c->amk << 4; c->bq = c->beta << 4; c->nr = 0; c->g4 = -1;
            c->hx = 0; }                                    /* (from the bypass: no stale level) */
        {
            int32_t e0 = c->ef, h0 = c->hd, k = c->kq;
            if (p >= c->ef >> 1) c->nr = 0;                   /* (the programme still at its top, within 3 dB) */
            else if (c->nr <= NRB) c->nr++;
            if (p >= c->ef) {                                 /* attack (a small rise slower, REL fast), then held */
                c->ef = env(c->ef, p, p - c->ef > c->ef + (c->ef >> 2) || c->p[TX_CP_REL] > SAK ? c->ka : (c->ka >> SAS) + 1, k);
                c->hd = HOLD;
            }
            else if (c->hd) c->hd--;
            else c->ef = env(c->ef, p, c->ka, k);
            if (c->ef - e0 > e0 + (e0 >> 2)) k = c->kr;       /* a new strike (power x 2.25, +3.5 dB): REL's own */
            else if (!h0 && c->ef > e0) { k -= k >> 1; if (k < (c->kr >> PDR)) k = c->kr >> PDR; }   /* caught again */
            else if (c->nr > NRB) k += (c->kr - k) >> PDT;   /* gone: REL back (held there while it stays) */
            c->kq = k < 1 ? 1 : k;
        }
        c->es = env(c->es, p, K_SA, K_SR);
        det = c->ef > c->es ? c->ef : c->es;
        det = det < 1 ? 1 : det > (1 << 26) - 1 ? (1 << 26) - 1 : det;
        if (c->p[TX_CP_ON]) {
            int32_t lp = -(int32_t)TX_FAR(tx_neglog2_q12)((uint32_t)det << 6);   /* power log2 re full scale, Q12
                                                         * (TX_FAR: the helper may stay in flash at -Os) */
            int32_t g16 = gc(c, lp, c->t) << 4, a16 = c->amk << 4, b16 = c->beta << 4, gr, mx;
            if (g16 >= c->gs) c->gs = g16;       /* more reduction at once, less through the one-pole */
            else c->gs += (g16 - c->gs) >> c->gsh;
            gr = c->gs >> 4;
            if (c->gs <= c->bs) c->bs = c->gs;   /* the body lift: the steady reduction follows it down at once, up */
            else c->bs += (c->gs - c->bs) >> BSH;   /* over ~186 ms; BETA of it given back */
            if (b16 >= c->bq) c->bq = b16;       /* (the lift itself: up at once, down at the reduction's smoothing) */
            else c->bq += (b16 - c->bq) >> c->gsh;
            gr -= ((c->bs >> 4) * c->bq) >> 12;
            c->gr = gr;
            if (a16 >= c->amg) c->amg = a16;     /* the auto make-up: up at once, down at the reduction's smoothing */
            else c->amg += (a16 - c->amg) >> c->gsh;
            mx = c->amg >> 4;
            if (mx > gr) mx = gr;                /* never past the reduction: no peak over the bypass */
            {   /* past THR 64 never louder than THR 64 (its reduction, smoothed alike, followed at any THR): the
                 * level falls or stays as THR goes down, however loud */
                int32_t f16 = gc(c, lp, T4) << 4;
                if (f16 >= c->g4) c->g4 = f16;
                else c->g4 += (f16 - c->g4) >> c->gsh;
                if (c->p[TX_CP_THR] > T0 && gr - mx < c->g4 >> 4) mx = gr - (c->g4 >> 4);
            }
            {   /* held (the release adapted to a held chord's beats): the net reduction (less the make-up and the
                 * lift) averaged since the hold began, over 1.5 s at most, within 0.25 dB of the reduction of the
                 * moment: the gain no longer rides the chord's slowly turning peaks; a strike gives REL back and the
                 * reduction of the moment at once, a decay is followed within 0.25 dB of before; after the hold the
                 * average falls back to it at the reduction's own smoothing, still within 0.25 dB (more reduction:
                 * at once) */
                int32_t n16 = (gr - mx) << 4;
                if (c->kq <= c->kr >> HKS) {
                    if (c->hx != 1) { c->gh = n16; c->hn = 0; c->hs = 0; c->hx = 1; }   /* (the average from here) */
                    else {
                        c->gh += (n16 - c->gh) >> c->hs;
                        if (++c->hn >= (2u << c->hs) && c->hs < HSH) c->hs++;   /* (a running mean: 1/2, 1/4 ...) */
                    }
                    if (c->gh > n16 + HUP) c->gh = n16 + HUP;
                    else if (c->gh < n16 - HUP) c->gh = n16 - HUP;
                    if (c->gh < 0) c->gh = 0;
                } else if (c->hx && n16 < c->gh - 16) {
                    c->gh += (n16 - c->gh) >> c->gsh;
                    if (c->gh > n16 + HUP) c->gh = n16 + HUP;
                    c->hx = 2;
                } else { c->gh = n16; c->hx = 0; }
                mx = gr - (c->gh >> 4);
            }
            {   /* MKUP glides: 1/8 of the way a control block (~6 ms), then exactly there (no click on a fast move) */
                int32_t dm = (c->mk - c->mg) >> 3;
                c->mg = dm ? c->mg + dm : c->mk;
            }
            gt = (int32_t)TX_FAR(tx_exp2)(((c->mg - gr + mx) / 2) * 16 + (14 << 16));   /* the amplitude: half the power log2 */
        } else { c->gr = 0; c->gs = c->g4 = 0; c->hx = 0; gt = 16384; }
        g = g0 + ((gt - g0) >> 3);                            /* the glide: ~6 ms */
        if (g - gt < 8 && gt - g < 8) g = gt;
        c->g = g;
        dg = (g - g0) / (int32_t)m;
        for (j = j0, g = g0; j < j0 + m; j++) {                /* the gain ramped across the block */
            int32_t l = L[j], r = R[j];
            g += dg;
            if (j == j0 + m - 1u) g = c->g;
            l = l > (1 << 28) ? (1 << 28) : l < -(1 << 28) ? -(1 << 28) : l;
            r = r > (1 << 28) ? (1 << 28) : r < -(1 << 28) ? -(1 << 28) : r;
            L[j] = (l >> 14) * g + (int32_t)((((uint32_t)l & 16383u) * (uint32_t)g) >> 14);
            R[j] = (r >> 14) * g + (int32_t)((((uint32_t)r & 16383u) * (uint32_t)g) >> 14);
        }
    }
}
