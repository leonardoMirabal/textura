/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura effects: the REVERB's framework and the chain (tx_fx.h). The framework is Frodhes' (rev_types.c and fx.c
 * fx_reverb / fx_predelay: our own) as an instance: the send (the mix's mid x MIX), LOW CUT, the pre-delay, the 2:1
 * down into the kernel, the kernel at 22.05 kHz, the 1:2 up, the return (which eases off as MIX rises), a TYPE
 * change's fade (5.8 ms) and the tail's end in exact silence. ROOM (Textura) is the CHAMBER (tx_core_chamber) run on
 * the send at the full rate, so the CHAMBER is one of the reverb's TYPEs now: one room at a time, one memory union. */
#include "tx_fx.h"

const tx_param_t tx_fx_reverb_params[TX_RV_NP] = {
    {"TYPE", TX_RVT_N - 1, 0, "the space: 0 off (the tail rings out) | 1 room (the CHAMBER: body modes + a short tail) | 2 hall | 3 infinite (DECAY 120..126 sustain, 127 freeze) | 4 dark (the infinite tank, darker and still: a deep frozen bed)"},
    {"MIX", 127, 40, "the send and the return: dry .. wet (the return eases off as the send rises)"},
    {"DECAY", 127, 64, "hall: x0.5 .. x2 the time; infinite / dark: T60 4 .. 60 s, 120..126 sustain, 127 freeze; room: the walls (127 = hard, long)"},
    {"TONE", 127, 64, "the tail's top: 0 dark .. 64 as designed .. 127 bright; room: the body's resonance"},
    {"PREDELAY", 127, 0, "before the reverb: 0 .. 80 ms"},
    {"SIZE", 127, 0, "0 = from MIX (auto); 1..127 the size (room: 0.2 .. 3 m)"},
    {"MOD", 127, 64, "the tail's slow wander (hall, infinite): none .. 2x"},
    {"LOWCUT", 127, 0, "a high-pass on the send: off (0), 20 .. 800 Hz"},
};

#define RV_FADE 8u                  /* control blocks of a TYPE change's fade: 256 frames, 5.8 ms */
#define FX_PDLY_SAMPLES(v) ((uint32_t)(v) * 3528u / 127u)
#define RV_UPI(u) ((RV_HB((u) + 0, 1) + 512) >> 10)

static uint32_t kern(uint32_t t) { return t == TX_RVT_DARK ? TX_RVT_INF : t; }   /* DARK runs the infinite kernel */

static void rev_begin(tx_fx_reverb_t *r, uint32_t t)
{
    tx_fx_memset(r->m, 0, sizeof r->m); tx_fx_memset(r->s, 0, sizeof r->s); r->hp = r->he = 0;
    r->cur = (uint8_t)kern(t);
    if (r->cur == TX_RVT_HALL) tx_hall_start(&r->u.h);
    else if (r->cur == TX_RVT_INF) tx_inf_start(&r->u.i);
    else if (r->cur == TX_RVT_ROOM) tx_chamber_init(&r->u.c);
}
static int rev_active(const tx_fx_reverb_t *r)
{
    return r->cur == TX_RVT_INF ? tx_inf_active(&r->u.i) : r->cur == TX_RVT_ROOM ? !tx_chamber_quiet(&r->u.c) : 0;
}

void tx_fx_reverb_init(tx_fx_reverb_t *r)
{
    unsigned i;
    tx_fx_memset(r, 0, sizeof *r - sizeof r->u);
    for (i = 0; i < TX_RV_NP; i++) r->p[i] = tx_fx_reverb_params[i].def;
    r->damp = 60;
    r->dk = 64;
    r->cur = TX_RVT_OFF;
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
    tx_chamber_process(&r->u.c, x, n);
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
    int quiet = !r->running, dark = tgt == TX_RVT_DARK;
    if (!send && !r->running && !r->fade) return;
    r->running = 1;
    if (tgt == TX_RVT_DARK) damp += 60;                         /* DARK: TONE's damping 60 steps darker */
    for (sb = 0; sb + CTL <= n; sb += CTL) {
        int32_t act = 0, *om = r->m + TX_RV_QZ, *os = r->s + TX_RV_QZ, *in = r->inb + RV_DZ;
        uint32_t cur;
        if (tgt && kern((uint32_t)tgt) != r->cur && !r->fade) {
            if (quiet || !r->cur) rev_begin(r, (uint32_t)tgt);
            else r->fade = RV_FADE;
        }
        for (j = 0; j < RV_DZ; j++) r->inb[j] = r->inb[CTL + j];
        for (j = 0; j < CTL; j++) {                             /* the send: the mid, Q23 -> Q15 (64-bit: the bus may */
            int32_t mq = (int32_t)(((L[sb + j] >> 1) + (R[sb + j] >> 1) + 128) >> 8);   /* be far over full scale) */
            in[j] = clampi((int32_t)(((int64_t)mq * send) >> 15), -131071, 131071);
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
            cp[TX_CH_BODY] = (unsigned)((tv * 40) >> 6 > 127 ? 127 : (tv * 40) >> 6); cp[TX_CH_TAIL] = 50u;
            for (q = 0; q < TX_CH_NP; q++) if (r->u.c.p[q] != cp[q]) tx_chamber_param(&r->u.c, q, cp[q]);   /* changes only */
            if (r->rn == 0) r->r0 = sb;
            for (j = 0; j < CTL; j++) r->rm[sb + j] = in[j] * 256;   /* Q15 -> Q23: the chamber's scale */
            r->rf[sb / CTL] = r->fade;
            r->rn += CTL;
            act = 1;                                            /* (its end: the chamber's own quiet, after the run) */
        } else {
            if (cur) for (i = 0; i < TX_RV_QZ; i++) r->m[i] = om[HL_NC - TX_RV_QZ + i], r->s[i] = os[HL_NC - TX_RV_QZ + i];
            if (cur == TX_RVT_HALL) tx_hall_run(&r->u.h, in, size + r->off, r->damp, md, om, os);
            else if (cur == TX_RVT_INF) {
                tx_rev_ctl_t c;
                c.decay = r->dk; c.damp = r->damp; c.size = zv ? zv : v; c.mod = r->p[TX_RV_MOD]; c.dark = dark;
                c.freeze = kern((uint32_t)tgt) != TX_RVT_INF || r->dk < 120 ? 0 : r->dk < 127 ? 1 : 2;
                tx_rev_down(in, &r->hp, &r->he, r->x);
                tx_inf_run(&r->u.i, r->x, om, os, &c);
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
    if (r->rn) {
        room_flush(r, L, R);
        if (!send && r->cur == TX_RVT_ROOM && tx_chamber_quiet(&r->u.c)) {   /* the room's tail has ended */
            r->running = 0; r->pd_on = 0;
            tx_fx_memset(r->inb, 0, sizeof r->inb);
        }
    }
}

/* ---- the chain */
void tx_fx_init(tx_fx_t *fx)
{
    unsigned k;
    for (k = 0; k < TX_FX_LANES; k++) { tx_fx_drive_init(&fx->drive[k]); tx_fx_filter_init(&fx->filter[k]); }
    tx_fx_reverb_init(&fx->rev);
}
TX_AUDIO_CODE void tx_fx_lane(void *u, unsigned k, int32_t *x, unsigned n)
{
    tx_fx_t *fx = (tx_fx_t *)u;
    if (k >= TX_FX_LANES) return;
    tx_fx_drive_process(&fx->drive[k], x, n);
    tx_fx_filter_process(&fx->filter[k], x, n);
}
TX_AUDIO_CODE void tx_fx_bus(void *u, int32_t *L, int32_t *R, unsigned n)
{
    tx_fx_reverb_process(&((tx_fx_t *)u)->rev, L, R, n);
}
int tx_fx_bus_quiet(void *u) { return tx_fx_reverb_quiet(&((tx_fx_t *)u)->rev); }
