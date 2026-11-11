/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura's modulation (UI-PLAN 6, the MVP's part): 8 LFOs (engines/tx_mod.c), a 16-slot matrix, the song position
 * the synced LFOs read, mod learn; 2 envelopes and 2 followers (engines/tx_mod.c: tx_env, tx_fol).
 *
 * In the audio interrupt, at the start of each block (mod_block, from lanes.c after the queued events): the song
 * position steps on, the LFOs restart on their triggers and run, the matrix sums every route into its target
 * (final = base + sum of depth x source x the target's range, Q8), and the targets whose value changed are set, at most
 * MOD_SENDS a block in rotation (and one more each block refreshed, so a value the panel sent meanwhile is put back).
 *
 * In the main loop (mod_poll): the routes. Each slot's TARGET is an index into one list (mod_tgt_*): lane 1..4 or ALL
 * LANES (LEVEL, PAN, HOW MANY, HOW ACTIVE, the sound's 8 cards; FILTER FREQ / RESO / LFO / RATE once: retired, skipped,
 * a scene's routes onto them dropped), the grain delay's
 * and the reverb's amounts, each LFO's RATE and LEVEL, the mix's DRIVE AMOUNT / TONE / MIX, the mix's FILTER LOW CUT /
 * HIGH CUT (appended; a lane's DRIVE
 * targets of before, still in the list where they were, are the mix's DRIVE now: mod_tgt_drv). Never: a TYPE, FREEZE, SYNC, SHIFT, a
 * sound's PRESET / TYPE (the cards hold none). The routes are rebuilt into the other of two buffers whenever a slot, a
 * base value or a lane's sound changes and the interrupt takes the new one at its next block; a target that loses
 * its last route gets its base value back. HOW MANY is changed through the lane model (scape.c), at most every 250 ms
 * and with most of a step of hysteresis; HOW ACTIVE is the generator's ACTIVITY (or the source's own, Notes off).
 *
 * Sources: LFO 1..8 (bipolar), VELOCITY (the last note), MOD WHEEL (CC 1), AFTERTOUCH, CC (MOD CC, learnable on the
 * MATRIX page), HOW ACTIVE (the target's lane; a global target: the selected lane's), RANDOM (a fresh value at each
 * note), CONSTANT, ENV 1..2, FOLLOW 1..2 (appended: the stored numbers never change). SPREAD (ALL LANES): lane k
 * reads the source k x SPREAD later (up to 90 degrees of its cycle; an LFO's only).
 *
 * The envelopes' triggers, each block: KEY (a note on any lane: a key or the generator's), LANE n (its notes, or its
 * start), EVENTS n (the lane's particles in the last block), RANDOM (tx_env's own), LFO n rising through 0, START
 * (the transport). Their sustain holds while the notes are held (KEY: any lane's, LANE n: that lane's; counted from
 * the note on / off events, reset when the lane's engine changes). The followers' inputs: a lane's level or the
 * mix's (the last block's peak, in dB), a lane's event rate, the grain delay's grains sounding. */
#include "tx_mod.h"

#define MOD_NS 16u                              /* slots */
#define MOD_NT 64u                              /* targets at most (16 slots x 4 lanes) */
#define MOD_NR 64u
#define MOD_SENDS 8u                            /* set() calls a block at most (the cost's cap) */
enum { MS_NONE, MS_LFO1, MS_VEL = 9, MS_MODW, MS_AT, MS_CC, MS_ACTIVE, MS_RANDOM, MS_CONST, MS_ENV1, MS_FOL1 = MS_ENV1 + TX_ENV_N,
       MS_N = MS_FOL1 + TX_FOL_N };   /* (append only: a scene stores the number) */
enum { ME_VEL, ME_MODW, ME_AT, ME_CC, ME_ACT1, ME_RANDOM = ME_ACT1 + 4, ME_CONST, ME_ENV1, ME_FOL1 = ME_ENV1 + TX_ENV_N,
       ME_N = ME_FOL1 + TX_FOL_N };   /* mod_ext[] */
enum { MK_ENG, MK_HOST, MK_PAN, MK_FX, MK_ACT, MK_COUNT, MK_LRATE, MK_LLEVEL, MK_GRV };   /* (MK_GRV: GROOVE AMOUNT) */
#define MT_LANE_N 19u                           /* a lane's targets */
#define MT_GRAIN0 (1u + 5u * MT_LANE_N)         /* 96 */
#define MT_REV0 (MT_GRAIN0 + 11u)               /* 107 */
#define MT_LFO0 (MT_REV0 + 8u)                  /* 115 */
#define MT_DRV0 (MT_LFO0 + 2u * TX_LFO_N)       /* 131: the mix's DRIVE AMOUNT TONE MIX */
#define MT_MF0 (MT_DRV0 + 3u)                   /* 134: the mix's FILTER LOW CUT HIGH CUT */
#define MT_GMOD (MT_MF0 + 2u)                   /* 136: the GRAIN delay's MOD */
#define MT_GRV (MT_GMOD + 1u)                   /* 137: GROOVE AMOUNT (groove.c) */
#define MT_N (MT_GRV + 1u)                      /* 138: TARGET 0 (none) .. 137 */
#define MT_LDRV 12u                             /* a lane's targets 12 .. 14: its DRIVE once, the mix's now */
static const uint8_t MT_GRAIN[11] = {TX_GD_MIX, TX_GD_TIME, TX_GD_FEEDBACK, TX_GD_GRAINS, TX_GD_SIZE, TX_GD_SPRAY,
                                     TX_GD_PITCH, TX_GD_REVERSE, TX_GD_SPREAD, TX_GD_TONE, TX_GD_GRAIN};
static const uint8_t MT_REV[8] = {TX_RV_MIX, TX_RV_DECAY, TX_RV_TONE, TX_RV_PREDELAY, TX_RV_SIZE, TX_RV_MOD,
                                  TX_RV_LOWCUT, TX_RV_SHIMMER};

static tx_lfo_t mod_lfo[TX_LFO_N];
static tx_env_t mod_env[TX_ENV_N];
static tx_fol_t mod_fol[TX_FOL_N];
static int32_t m_lprev[TX_LFO_N];               /* (the LFOs' last values: an envelope's LFO n trigger) */
static uint8_t m_held[4];                       /* notes held per lane (the envelopes' sustain) */
static struct { uint8_t src, tgt, depth, spread; } mslot[MOD_NS];   /* depth 0 .. 200: -100 .. +100 % */
static uint8_t mod_cc = 2;                      /* MOD CC (the CC source's number; 2: breath) */
static volatile int16_t mod_ext[ME_N];

typedef struct {
    uint16_t id;                                /* the param id (params.c) it is */
    uint8_t kind, lane, pid, grp, hi, inv;
    int16_t base;
    const tx_engine_t *eng;                     /* MK_ENG / MK_HOST: the engine it was resolved on */
} mt_t;
typedef struct { uint8_t nt, nr; mt_t t[MOD_NT]; tx_mod_rt_t r[MOD_NR]; } mbuf_t;
static mbuf_t mbuf[2];
static volatile uint8_t mcur;
static volatile uint32_t mgen;
/* the interrupt's */
static int16_t m_last[MOD_NT];
static volatile int16_t m_final[MOD_NT];
static volatile int32_t m_fq8[MOD_NT];          /* (HOW MANY: the final in Q8, for its hysteresis) */
static int32_t m_sum[MOD_NT];
static uint32_t m_seen, m_rot, m_ref;
static uint8_t m_lrate[TX_LFO_N], m_llev[TX_LFO_N], m_lmod;   /* the LFOs' RATE / LEVEL as modulated (m_lmod: by LFO) */
static uint32_t m_trig, m_rng = 0x2545F491u;
static volatile uint32_t mpos, mblk_n, mblk_ms;   /* the song position (1/96 quarter, Q8); the last block's count, ms */
static uint32_t mpos_frac;
static volatile uint32_t mpos_inc = 285u << 8;  /* its step a block, Q16 (120 BPM) */
static volatile int32_t mpos_nudge;
static volatile uint8_t mpos_snap, m_start;
static volatile uint32_t mpos_snap_v;
#define MPOS_W ((uint32_t)TX_POS_WRAP << 8)

/* ---- the interrupt */
static void mod_key_isr(uint32_t k, uint32_t vel)   /* a note on (a key, the generator, a lane's start: lanes.c) */
{
    m_trig |= 1u << (k & 3u);
#ifdef DINGHY_HOST
    gr_note_host(k);                            /* (groove.c: how near its pulse each note falls) */
#endif
    if (m_held[k & 3u] < 255u) m_held[k & 3u]++;
    mod_ext[ME_VEL] = (int16_t)(vel > 127u ? 32767 : vel * 258u);
    m_rng ^= m_rng << 13; m_rng ^= m_rng >> 17; m_rng ^= m_rng << 5;
    mod_ext[ME_RANDOM] = (int16_t)(m_rng >> 17);
}
static void mod_key_off_isr(uint32_t k)        /* a note off (lanes.c) */
{
    if (m_held[k & 3u]) m_held[k & 3u]--;
}
static void mod_lane_reset_isr(uint32_t k) { m_held[k & 3u] = 0; }   /* the lane's engine changed: its notes gone */
/* the envelopes and the followers, a block: their triggers and inputs (lanes.c's last block: ln.lpk, ln.lev) */
static void mod_env_block(uint32_t trig, uint32_t start)
{
    uint32_t i, k, held = 0;
    for (k = 0; k < 4u; k++) held |= (m_held[k] != 0) << k;
    for (i = 0; i < TX_ENV_N; i++) {
        tx_env_t *e = &mod_env[i];
        uint32_t t = e->p[TX_EP_TRIGGER], tg = 0, gate = 0;
        if (t == TX_ET_KEY) { tg = trig != 0; gate = held != 0; }
        else if (t < TX_ET_LANE1 + 4u) { tg = trig >> (t - TX_ET_LANE1) & 1u; gate = held >> (t - TX_ET_LANE1) & 1u; }
        else if (t < TX_ET_EV1 + 4u) tg = ln.lev[t - TX_ET_EV1] != 0;
        else if (t >= TX_ET_LFO1 && t < TX_ET_LFO1 + TX_LFO_N) tg = m_lprev[t - TX_ET_LFO1] < 0 && mod_lfo[t - TX_ET_LFO1].out >= 0;
        else if (t == TX_ET_START) tg = start;
        tx_env_block(e, tg, gate);
        mod_ext[ME_ENV1 + i] = (int16_t)e->out;
    }
    for (i = 0; i < TX_LFO_N; i++) m_lprev[i] = mod_lfo[i].out;
    for (i = 0; i < TX_FOL_N; i++) {
        tx_fol_t *f = &mod_fol[i];
        uint32_t in = f->p[TX_FP_INPUT];
        int32_t x;
        if (in < TX_FI_RATE1) x = tx_fol_level_q15((uint32_t)ln.lpk[in - TX_FI_LEVEL1]);
        else if (in < TX_FI_MIX) x = tx_fol_rate_q15((uint32_t)ln.lev[in - TX_FI_RATE1] << 8);
        else if (in == TX_FI_MIX) x = tx_fol_level_q15((uint32_t)(ln.mix.raw_peak > 0 ? ln.mix.raw_peak : 0));
        else x = ln.fx.gd.act >= 16u ? 32767 : (int32_t)(ln.fx.gd.act * 32767u / 16u);   /* (16 grains: full scale, as
                                                         * before 30: the same follower up to 16) */
        tx_fol_block(f, x);
        mod_ext[ME_FOL1 + i] = (int16_t)f->out;
    }
}
static void mod_apply(const mt_t *t, uint32_t v)
{
    ln_ev_t ev = {0, t->lane, 0, 0, 0};
    uint32_t k = t->lane;
    if (t->kind == MK_ENG || t->kind == MK_HOST) {
        if (k >= LN_N || !ln.mix.e[k] || (t->eng && ln.mix.e[k] != t->eng)) return;   /* (the lane switched meanwhile) */
        ev.kind = t->kind == MK_ENG ? LN_EV_PARAM : LN_EV_HOST;
        ev.a = t->pid; ev.b = (uint8_t)(t->inv ? t->hi - v : v);
    } else if (t->kind == MK_PAN) { ev.kind = LN_EV_PAN; ev.a = (uint8_t)v; }
    else if (t->kind == MK_FX) { ev.kind = LN_EV_FX; ev.a = t->grp; ev.b = t->pid; ev.c = (uint8_t)v; }
    else if (t->kind == MK_ACT) {
        if (k >= LN_N || !ln.mix.e[k]) return;
        if (ln.gen_on[k]) { ev.kind = LN_EV_GEN_PARAM; ev.a = TX_GEN_ACTIVITY; ev.b = (uint8_t)v; }
        else if (ln.int_id[k] >= 0) { ev.kind = LN_EV_PARAM; ev.a = (uint8_t)ln.int_id[k]; ev.b = (uint8_t)v; }
        else return;
    } else return;
    ln_event(&ev);
}
static void mod_block(void)
{
    const mbuf_t *b = &mbuf[mcur];
    uint32_t i, sent = 0, t, trig = m_trig, start = m_start;
    mblk_n++; mblk_ms = fm1_ms;
    {                                           /* the song position */
        uint32_t f = mpos_frac + mpos_inc, p = mpos + (f >> 8);
        mpos_frac = f & 255u;
        if (mpos_snap) { p = mpos_snap_v; mpos_snap = 0; }
        else if (mpos_nudge) { p += (uint32_t)mpos_nudge; mpos_nudge = 0; }
        if ((int32_t)p < 0) p += MPOS_W;
        while (p >= MPOS_W) p -= MPOS_W;
        mpos = p;
    }
    m_trig = 0; m_start = 0;
    for (i = 0; i < TX_LFO_N; i++) {            /* the LFOs: their triggers, then a block */
        tx_lfo_t *l = &mod_lfo[i];
        uint32_t m = l->p[TX_LP_MODE];
        if ((trig && (m == TX_LM_KEY || m == TX_LM_ONESHOT)) || (m >= TX_LM_LANE1 && m <= TX_LM_LANE4 && trig >> (m - TX_LM_LANE1) & 1u) ||
            (start && l->p[TX_LP_RESTART])) tx_lfo_restart(l);
        tx_lfo_block(l, m_lmod >> i & 1u ? m_lrate[i] : l->p[TX_LP_RATE], m_lmod >> (i + 8u) & 1u ? m_llev[i] : l->p[TX_LP_LEVEL], mpos);
    }
    mod_env_block(trig, start);
    if (mgen != m_seen) { m_seen = mgen; for (t = 0; t < MOD_NT; t++) m_last[t] = -1; m_lmod = 0; gr_amt_on = 0; }
    if (!b->nt) return;
    for (t = 0; t < b->nt; t++) m_sum[t] = 0;
    tx_mod_sum(b->r, b->nr, mod_lfo, (const int16_t *)mod_ext, m_sum);
    for (t = 0; t < b->nt; t++) {
        const mt_t *d = &b->t[t];
        int32_t q = ((int32_t)d->base << 8) + m_sum[t], v;
        if (q < 0) q = 0;
        if (q > (int32_t)d->hi << 8) q = (int32_t)d->hi << 8;
        v = (q + 128) >> 8;
        if (v > d->hi) v = d->hi;
        m_fq8[t] = q; m_final[t] = (int16_t)v;
        if (d->kind == MK_LRATE) { m_lrate[d->lane] = (uint8_t)v; m_lmod |= 1u << d->lane; }
        else if (d->kind == MK_LLEVEL) { m_llev[d->lane] = (uint8_t)v; m_lmod |= 1u << (d->lane + 8u); }
        else if (d->kind == MK_GRV) { gr_amt_mod = (uint8_t)v; gr_amt_on = 1; }   /* (groove.c reads it this block) */
    }
    for (i = 0; i < b->nt && sent < MOD_SENDS; i++) {   /* the changed ones, in rotation */
        t = (m_rot + i) % b->nt;
        if (b->t[t].kind >= MK_COUNT || m_final[t] == m_last[t]) continue;
        mod_apply(&b->t[t], (uint32_t)m_final[t]);
        m_last[t] = m_final[t];
        sent++;
    }
    m_rot = (m_rot + 1u) % b->nt;
    t = m_ref++ % b->nt;                        /* one refreshed (a value the panel sent meanwhile: put back) */
    if (b->t[t].kind < MK_COUNT && m_last[t] >= 0) mod_apply(&b->t[t], (uint32_t)m_last[t]);
}

/* ---- the targets' list (TARGET 1 .. MT_N - 1) */
static uint32_t mod_tgt_drv(uint32_t T)         /* the mix's DRIVE parameter T is (TX_DR_NP: not one) */
{
    if (T >= MT_DRV0 && T < MT_MF0) return T - MT_DRV0;
    if (T && T < MT_GRAIN0 && (T - 1u) % MT_LANE_N >= MT_LDRV && (T - 1u) % MT_LANE_N < MT_LDRV + 3u)
        return (T - 1u) % MT_LANE_N - MT_LDRV;   /* (TX_DR_AMOUNT TONE MIX: 0 1 2) */
    return TX_DR_NP;
}
static int mod_tgt_retired(uint32_t T)          /* a lane's FILTER FREQ / RESO / LFO / RATE (15 .. 18): retired, dropped */
{
    return T && T < MT_GRAIN0 && (T - 1u) % MT_LANE_N >= 15u;
}
static uint32_t mod_tgt_lanes(uint32_t T)       /* 0..3, 4 ALL, 5 none (a global) */
{
    return T && T < MT_GRAIN0 && mod_tgt_drv(T) == TX_DR_NP ? (T - 1u) / MT_LANE_N : 5u;
}
static uint32_t mod_tgt_pid(uint32_t T, uint32_t k)   /* the param id it is on lane k */
{
    if (!T || T >= MT_N || mod_tgt_retired(T)) return NOID_P;
    if (mod_tgt_drv(T) < TX_DR_NP) return PID(P_GLOBAL, PG_DRIVE0 + mod_tgt_drv(T));
    if (T == MT_GMOD) return PID(P_GLOBAL, PG_GD(TX_GD_MOD));
    if (T == MT_GRV) return PID(P_GLOBAL, PG_GRV0);
    if (T >= MT_MF0) return PID(P_GLOBAL, PG_MF0 + (T - MT_MF0));   /* (TX_MF_LOW, TX_MF_HIGH) */
    if (T < MT_GRAIN0) {
        static const uint8_t J[MT_LANE_N] = {PL_LEVEL, PL_PAN, PL_COUNT, PL_ACTIVE, PL_CARD0, PL_CARD0 + 1, PL_CARD0 + 2,
            PL_CARD0 + 3, PL_CARD0 + 4, PL_CARD0 + 5, PL_CARD0 + 6, PL_CARD0 + 7, 0, 0, 0,   /* (12 .. 14: mod_tgt_drv) */
            0, 0, 0, 0};                        /* (15 .. 18: retired, mod_tgt_retired) */
        return PID(k, J[(T - 1u) % MT_LANE_N]);
    }
    if (T < MT_REV0) return PID(P_GLOBAL, PG_GRAIN0 + MT_GRAIN[T - MT_GRAIN0]);
    if (T < MT_LFO0) return PID(P_GLOBAL, PG_REVERB0 + MT_REV[T - MT_REV0]);
    return PID(P_LFO, ((T - MT_LFO0) / 2u) * 16u + ((T - MT_LFO0) & 1u ? TX_LP_LEVEL : TX_LP_RATE));
}
/* "L2 SIZE", "ALL LEVEL", "GRAIN MIX", "REV DECAY", "LFO3 RATE", "DRV AMOUNT" */
static void mod_tgt_text(uint32_t T, char *s)
{
    uint32_t sc_ = mod_tgt_lanes(T), k = sc_ < 4u ? sc_ : sc.sel;
    s[0] = 0;
    if (!T || T >= MT_N || mod_tgt_retired(T)) { p_strcpy(s, "NONE", 5); return; }
    if (mod_tgt_drv(T) < TX_DR_NP) { p_strcpy(s, "DRV ", 4); p_cat(s, p_lab(mod_tgt_pid(T, 0)), 23); return; }
    if (T == MT_GMOD) { p_strcpy(s, "GRAIN ", 6); p_cat(s, p_lab(mod_tgt_pid(T, 0)), 23); return; }
    if (T == MT_GRV) { p_strcpy(s, "GROOVE AMOUNT", 23); return; }
    if (T >= MT_MF0) { p_strcpy(s, "FLT ", 4); p_cat(s, p_lab(mod_tgt_pid(T, 0)), 23); return; }
    if (sc_ < 4u) { s[0] = 'L'; s[1] = (char)('1' + sc_); s[2] = ' '; s[3] = 0; }
    else if (sc_ == 4u) p_strcpy(s, "ALL ", 4);
    else if (T < MT_REV0) p_strcpy(s, "GRAIN ", 6);
    else if (T < MT_LFO0) p_strcpy(s, "REV ", 4);
    else { p_strcpy(s, "LFO", 3); { char n[2] = {(char)('1' + (T - MT_LFO0) / 2u), 0}; p_cat(s, n, 23); } p_cat(s, " ", 23); }
    if (T < MT_GRAIN0) {
        uint32_t j = (T - 1u) % MT_LANE_N;
        if (j >= 4u && j < 12u && !p_valid(mod_tgt_pid(T, k))) { char c[4] = {'C', (char)('1' + j - 4u), 0, 0}; p_cat(s, c, 23); return; }
    }
    p_cat(s, p_lab(mod_tgt_pid(T, k)), 23);
}
/* the TARGET index of a param id (0: not a target) */
static uint32_t mod_tgt_of(uint32_t id)
{
    uint32_t T, k = P_SCOPE(id) < P_LANES ? P_SCOPE(id) : 0u;
    for (T = 1; T < MT_N; T++) {
        uint32_t l = mod_tgt_lanes(T);
        if (l == 4u || (l < 4u && l != k)) continue;   /* (a lane's target: that lane's, not lane 1's) */
        if (T < MT_GRAIN0 && mod_tgt_drv(T) < TX_DR_NP) continue;   /* (the old lane DRIVE's: the appended ones) */
        if (mod_tgt_pid(T, k) == id) return T;
    }
    return 0;
}

/* ---- the routes (main loop) */
static uint8_t m_cbase[SC_N], m_cb_ok[SC_N], m_capplied[SC_N];   /* HOW MANY: the user's value, the one set by mod */
static uint32_t m_ct[SC_N];
static int mod_concrete(uint32_t T, uint32_t k, mt_t *o)   /* target T on lane k: 1 if it can be set now */
{
    uint32_t id = mod_tgt_pid(T, k), i = P_IDX(id);
    o->id = (uint16_t)id; o->lane = (uint8_t)k; o->inv = 0; o->eng = 0; o->grp = 0; o->pid = 0;
    if (mod_tgt_drv(T) < TX_DR_NP) {            /* the mix's DRIVE */
        o->kind = MK_FX; o->lane = 0; o->grp = LN_FX_DRIVE; o->pid = (uint8_t)mod_tgt_drv(T);
        o->hi = (uint8_t)p_hi(id); o->base = (int16_t)pv_glob[i];
        return 1;
    }
    if (T == MT_GMOD) {                         /* the GRAIN delay's MOD */
        o->kind = MK_FX; o->lane = 0; o->grp = LN_FX_GRAIN; o->pid = TX_GD_MOD;
        o->hi = (uint8_t)p_hi(id); o->base = (int16_t)pv_glob[i];
        return 1;
    }
    if (T == MT_GRV) {                          /* GROOVE AMOUNT (the interrupt sets groove.c's gr_amt_mod) */
        o->kind = MK_GRV; o->lane = 0; o->hi = 127; o->base = gr_amount;
        return 1;
    }
    if (T >= MT_MF0) {                          /* the mix's FILTER (the DRIVE's group: the mix's first stage) */
        o->kind = MK_FX; o->lane = 0; o->grp = LN_FX_DRIVE; o->pid = (uint8_t)(TX_FX_MF_ID + T - MT_MF0);
        o->hi = (uint8_t)p_hi(id); o->base = (int16_t)pv_glob[i];
        return 1;
    }
    if (T >= MT_LFO0) {
        uint32_t n = (T - MT_LFO0) / 2u;
        o->kind = (T - MT_LFO0) & 1u ? MK_LLEVEL : MK_LRATE; o->lane = (uint8_t)n; o->hi = 127;
        o->base = mod_lfo[n].p[o->kind == MK_LRATE ? TX_LP_RATE : TX_LP_LEVEL];
        return 1;
    }
    if (T >= MT_GRAIN0) {
        o->kind = MK_FX; o->lane = 0;
        o->grp = T < MT_REV0 ? LN_FX_GRAIN : LN_FX_REVERB;
        o->pid = (uint8_t)(T < MT_REV0 ? i - PG_GRAIN0 : i - PG_REVERB0);
        o->hi = (uint8_t)p_hi(id); o->base = (int16_t)pv_glob[i];
        return 1;
    }
    if (k >= SC_N || sc.snd[k] < 0 || !sc.eng[k]) return 0;
    if (i == PL_LEVEL) { o->kind = MK_ENG; o->pid = TX_PID_LEVEL; o->hi = 127; o->base = (int16_t)sc_vol_of(k); return 1; }
    if (i == PL_PAN) { o->kind = MK_PAN; o->hi = 127; o->base = sc.pan[k]; return 1; }
    if (i == PL_COUNT) {
        if (!sc.run[k]) return 0;
        if (!m_cb_ok[k]) { m_cbase[k] = (uint8_t)sc_cnt_of(k); m_capplied[k] = m_cbase[k]; m_cb_ok[k] = 1; }
        o->kind = MK_COUNT; o->hi = LS_NCNT - 1u; o->base = m_cbase[k];
        return 1;
    }
    if (i == PL_ACTIVE) {
        if (!sc.run[k]) return 0;
        o->kind = MK_ACT; o->hi = 127; o->base = (int16_t)sc_act_val(sc.step[k]);
        return 1;
    }
    if (i >= PL_CARD0 && i < PL_CARD0 + SCD_MAX) {
        p_cards_t *c = p_cards(k);
        uint32_t j = i - PL_CARD0;
        if (j >= c->n || sc_eq(c->lab[j], "TYPE") || sc_eq(c->lab[j], "PRESET")) return 0;
        if (c->cv[j] == 0xFF) c->cv[j] = (uint8_t)p_card_get(k, j);   /* (the value now is the base from here on) */
        o->kind = c->kind[j] & CK_HOST ? MK_HOST : MK_ENG; o->pid = c->id[j]; o->hi = c->hi[j];
        o->inv = (uint8_t)((c->kind[j] & CK_INV) != 0); o->base = c->cv[j]; o->eng = sc.eng[k];
        return 1;
    }
    return 0;
}
static void mod_restore(const mt_t *t)          /* a target without routes: its base value back */
{
    uint32_t k = t->lane, i = P_IDX(t->id);
    switch (t->kind) {
    case MK_ENG:
        if (t->pid == TX_PID_LEVEL) { sc.a_level[k] = -1; sc_sync(k); break; }
        if (sc.eng[k] == t->eng && i >= PL_CARD0) p_card_set(k, i - PL_CARD0, (uint32_t)t->base);
        break;
    case MK_HOST: if (sc.eng[k] == t->eng) p_card_set(k, i - PL_CARD0, (uint32_t)t->base); break;
    case MK_PAN: sc.a_pan[k] = 0xFF; sc_sync(k); break;
    case MK_FX: ln_fx(t->grp, k, t->pid, (uint32_t)t->base); break;
    case MK_ACT: sc.a_act[k] = -1; sc_sync(k); break;
    case MK_COUNT: if (sc_cnt_of(k) == m_capplied[k]) sc_count(k, m_cbase[k]); m_cb_ok[k] = 0; break;
    }
}
static int mt_same(const mt_t *a, const mt_t *b) { return a->kind == b->kind && a->lane == b->lane && a->pid == b->pid && a->grp == b->grp && a->id == b->id; }
static void mod_routes(void)
{
    mbuf_t *nb = &mbuf[mcur ^ 1u];
    const mbuf_t *ob = &mbuf[mcur];
    uint32_t s, k, t;
    nb->nt = nb->nr = 0;
    for (s = 0; s < MOD_NS; s++) {
        uint32_t src = mslot[s].src, T = mslot[s].tgt, ln_ = mod_tgt_lanes(T), k0, k1;
        if (!src || src >= MS_N || !T || T >= MT_N || mslot[s].depth == 100u) continue;
        k0 = ln_ < 4u ? ln_ : 0u; k1 = ln_ < 4u ? ln_ : ln_ == 4u ? SC_N - 1u : 0u;
        for (k = k0; k <= k1; k++) {
            mt_t c;
            tx_mod_rt_t *r;
            if (!mod_concrete(T, k, &c) || nb->nr >= MOD_NR) continue;
            for (t = 0; t < nb->nt && !mt_same(&nb->t[t], &c); t++) {}
            if (t == nb->nt) { if (nb->nt >= MOD_NT) continue; nb->t[nb->nt++] = c; }
            r = &nb->r[nb->nr++];
            r->depth = (int16_t)(((int32_t)mslot[s].depth - 100) * 32767 / 100);
            r->t = (uint8_t)t; r->hi = c.hi;
            r->lag = (uint8_t)(ln_ == 4u ? k * mslot[s].spread * 64u / 127u : 0u);   /* (8 ring steps = 90 degrees) */
            if (src <= TX_LFO_N) r->src = (uint8_t)(src - MS_LFO1);
            else if (src == MS_ACTIVE) r->src = (uint8_t)(TX_LFO_N + ME_ACT1 + (ln_ < 5u ? k : sc.sel));
            else if (src >= MS_ENV1) r->src = (uint8_t)(TX_LFO_N + ME_ENV1 + (src - MS_ENV1));   /* (ENV, FOLLOW) */
            else r->src = (uint8_t)(TX_LFO_N + (src == MS_VEL ? ME_VEL : src == MS_MODW ? ME_MODW : src == MS_AT ? ME_AT :
                                                 src == MS_CC ? ME_CC : src == MS_RANDOM ? ME_RANDOM : ME_CONST));
        }
    }
    {                                           /* the same as the interrupt's: nothing to do */
        int same = nb->nt == ob->nt && nb->nr == ob->nr;
        for (t = 0; same && t < nb->nt; t++) same = mt_same(&nb->t[t], &ob->t[t]) && nb->t[t].base == ob->t[t].base && nb->t[t].hi == ob->t[t].hi && nb->t[t].eng == ob->t[t].eng && nb->t[t].inv == ob->t[t].inv;
        for (t = 0; same && t < nb->nr; t++) same = nb->r[t].depth == ob->r[t].depth && nb->r[t].src == ob->r[t].src && nb->r[t].t == ob->r[t].t && nb->r[t].lag == ob->r[t].lag && nb->r[t].hi == ob->r[t].hi;
        if (same) return;
    }
    mcur ^= 1u;                                 /* (the interrupt takes it at its next block) */
    mgen++;
    for (t = 0; t < ob->nt; t++) {              /* the targets that lost every route: their base back */
        uint32_t u;
        for (u = 0; u < nb->nt && !mt_same(&nb->t[u], &ob->t[t]); u++) {}
        if (u == nb->nt) mod_restore(&ob->t[t]);
    }
}
/* the target holding param id now (-1: none) */
static int mod_t_of(uint32_t id)
{
    const mbuf_t *b = &mbuf[mcur];
    uint32_t t;
    for (t = 0; t < b->nt; t++) if (b->t[t].id == id && b->t[t].kind < MK_LRATE) return (int)t;
    for (t = 0; t < b->nt; t++) if (b->t[t].id == id) return (int)t;
    return -1;
}
static int mod_holds(uint32_t id) { return mod_t_of(id) >= 0; }   /* (params.c: the base only; the interrupt sets it) */
static int mod_live(uint32_t id, int32_t *v)    /* a modulated param's value now */
{
    int t = mod_t_of(id);
    if (t < 0) return 0;
    *v = m_final[t];
    return 1;
}

/* the song position against the tempo's beats (tempo.c: beat_t0 at pulse beat_k0, beat_ms a beat): a snap over half
 * a beat off, else 1/8 of the error a pass */
static uint32_t m_blk_seen, m_run_was;
static void mod_pos_poll(void)
{
    uint32_t n = mblk_n, ms, el, bm = beat_ms ? beat_ms : 500u, want, p;
    int32_t err;
    uint32_t inc = 1610612736u / (beat_smp ? beat_smp : 22050u);   /* 96 x 256 x 256 x 256 / beat_smp: Q16 a block */
    if (inc != mpos_inc) mpos_inc = inc;
    if (tm_running() && !m_run_was) m_start = 1;   /* START: the LFOs with RESTART ON START */
    m_run_was = (uint32_t)tm_running();
    if (n == m_blk_seen) return;
    m_blk_seen = n;
    ms = mblk_ms; p = mpos;
    el = ms - beat_t0;
    if ((int32_t)el < 0) el = 0;
    want = ((beat_k0 * 4u) % TX_POS_WRAP + (el / bm % 768u) * 96u) % TX_POS_WRAP;
    want = (want << 8) + (el % bm) * 24576u / bm;
    if (want >= MPOS_W) want -= MPOS_W;
    err = (int32_t)(want - p);
    if (err > (int32_t)(MPOS_W / 2u)) err -= (int32_t)MPOS_W;
    if (err < -(int32_t)(MPOS_W / 2u)) err += (int32_t)MPOS_W;
    if (gr_rephase || err > (96 << 7) || err < -(96 << 7)) { mpos_snap_v = want; mpos_snap = 1; gr_rephase = 0; }   /* (bar 1
                                                 * anew, tempo.c: the position snaps there, not glides) */
    else if (!mpos_nudge) mpos_nudge = err / 8;
}
static void mod_poll(void)
{
    uint32_t k, t;
    const mbuf_t *b;
    for (k = 0; k < SC_N; k++) mod_ext[ME_ACT1 + k] = (int16_t)(sc.run[k] ? sc_act_val(sc.step[k]) * 258u : 0u);
    mod_pos_poll();
    for (k = 0; k < SC_N; k++)                  /* HOW MANY changed by hand under a route: the new base */
        if (m_cb_ok[k] && sc_cnt_of(k) != m_capplied[k]) { m_cbase[k] = (uint8_t)sc_cnt_of(k); m_capplied[k] = m_cbase[k]; }
    mod_routes();
    b = &mbuf[mcur];
    for (t = 0; t < b->nt; t++) {               /* HOW MANY: through the lane model, every 250 ms at most */
        const mt_t *d = &b->t[t];
        int32_t q, cur;
        if (d->kind != MK_COUNT) continue;
        k = d->lane;
        q = m_fq8[t]; cur = (int32_t)sc_cnt_of(k);
        if (q > (cur << 8) - 192 && q < (cur << 8) + 192) continue;   /* (most of a step of hysteresis) */
        if (fm1_ms - m_ct[k] < 250u || !sc.run[k]) continue;
        cur = (q + 128) >> 8;
        if (cur < 1) cur = 1;                   /* (a route never stops the lane) */
        m_ct[k] = fm1_ms;
        if ((uint32_t)cur != sc_cnt_of(k)) { sc_count(k, (uint32_t)cur); m_capplied[k] = (uint8_t)sc_cnt_of(k); }
    }
}

/* ---- MIDI: the mod wheel, aftertouch, MOD CC (and its learn) */
static uint8_t mod_cc_learn;                    /* (ui.c: the MATRIX page with a CC slot) */
static uint32_t mod_cc_learned;                 /* (ui.c: the CC just learned + 1) */
static void mod_cc_in(uint32_t cc, uint32_t v)
{
    if (cc == 1u) mod_ext[ME_MODW] = (int16_t)(v * 258u);
    if (mod_cc_learn && cc != 1u && cc < 120u) { mod_cc = (uint8_t)cc; mod_cc_learn = 0; mod_cc_learned = cc + 1u; }
    if (cc == mod_cc) mod_ext[ME_CC] = (int16_t)(v * 258u);
}
static void mod_at(uint32_t v) { mod_ext[ME_AT] = (int16_t)((v > 127u ? 127u : v) * 258u); }

/* ---- the params (params.c: scope 5 the LFOs, n x 16 + param, 10 NOW and 11 ROUTES read only; scope 7 the matrix,
 * slot x 4 + SOURCE TARGET DEPTH SPREAD, 64 MOD CC) */
static const char *const N_LSHAPE[TX_LS_N] = {"SINE", "TRI", "SAW UP", "SAW DN", "SQUARE", "PULSE", "S&H", "SMOOTH",
                                              "DRIFT", "GUSTS", "CHAOS", "STEPS"};
static const char *const N_LMODE[TX_LM_N] = {"FREE", "KEY", "LANE 1", "LANE 2", "LANE 3", "LANE 4", "ONE-SHOT"};
static const char *const N_LSYNC[TX_LFO_NSYNC] = {"FREE", "1/32T", "1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8",
    "1/8D", "1/4T", "1/4", "1/4D", "1/2T", "1/2", "1/2D", "1/1T", "1 BAR", "1D", "2 BAR", "4 BAR", "8 BAR", "16 BAR",
    "32 BAR", "64 BAR"};
static const char *const N_MSRC[MS_N] = {"NONE", "LFO 1", "LFO 2", "LFO 3", "LFO 4", "LFO 5", "LFO 6", "LFO 7", "LFO 8",
                                         "VELOCITY", "MOD WHEEL", "AFTERTCH", "CC", "ACTIVE", "RANDOM", "CONSTANT",
                                         "ENV 1", "ENV 2", "FOLLOW 1", "FOLLOW 2"};
static const char *const N_ETRIG[TX_ET_N] = {"KEY", "LANE 1", "LANE 2", "LANE 3", "LANE 4", "EVENTS 1", "EVENTS 2",
    "EVENTS 3", "EVENTS 4", "RANDOM", "LFO 1", "LFO 2", "LFO 3", "LFO 4", "LFO 5", "LFO 6", "LFO 7", "LFO 8", "START"};
static const char *const N_FIN[TX_FI_N] = {"LEVEL 1", "LEVEL 2", "LEVEL 3", "LEVEL 4", "RATE 1", "RATE 2", "RATE 3",
                                           "RATE 4", "MIX", "GRAIN"};
/* scope 6 (P_ENV): ENV n (0..1) at n x 16 + its param (tx_env_params order); FOLLOW n at 32 + n x 16 + its param */
#define MV_FOL0 32u
static int mod_is_env(uint32_t i) { return i < MV_FOL0 && i / 16u < TX_ENV_N && i % 16u < TX_EP_N; }
static int mod_is_fol(uint32_t i) { return i >= MV_FOL0 && (i - MV_FOL0) / 16u < TX_FOL_N && (i - MV_FOL0) % 16u < TX_FP_N; }
enum { ML_NOW = TX_LP_N, ML_ROUTES, ML_N };
enum { MQ_SRC, MQ_TGT, MQ_DEPTH, MQ_SPREAD, MQ_N };
#define MQ_CC 64u
static int mod_valid(uint32_t id)
{
    uint32_t s = P_SCOPE(id), i = P_IDX(id);
    if (s == P_LFO) return i / 16u < TX_LFO_N && i % 16u < ML_N;
    if (s == P_MAT) return i < MOD_NS * MQ_N || i == MQ_CC;
    if (s == P_ENV) return mod_is_env(i) || mod_is_fol(i);
    return 0;
}
static const char *mod_lab(uint32_t id)
{
    static const char *const L[ML_N] = {"SHAPE", "RATE", "SYNC", "MODE", "PHASE", "WIDTH", "JITTER", "FADE", "RESTART",
                                        "LEVEL", "NOW", "ROUTES"};
    static const char *const Q[MQ_N] = {"SOURCE", "TARGET", "DEPTH", "SPREAD"};
    uint32_t i = P_IDX(id);
    if (P_SCOPE(id) == P_ENV) return mod_is_env(i) ? tx_env_params[i % 16u].name : tx_fol_params[(i - MV_FOL0) % 16u].name;
    if (P_SCOPE(id) == P_LFO) return L[i % 16u];
    return i == MQ_CC ? "MOD CC" : Q[i % MQ_N];
}
static int32_t mod_hi(uint32_t id)
{
    static const uint8_t Q[MQ_N] = {MS_N - 1u, MT_N - 1u, 200, 127};
    uint32_t i = P_IDX(id);
    if (P_SCOPE(id) == P_ENV) return mod_is_env(i) ? tx_env_params[i % 16u].max : mod_is_fol(i) ? tx_fol_params[(i - MV_FOL0) % 16u].max : 0;
    if (P_SCOPE(id) == P_LFO) return i % 16u < TX_LP_N ? tx_lfo_params[i % 16u].max : 0;
    return i == MQ_CC ? 119 : Q[i % MQ_N];
}
static int32_t mod_get(uint32_t id)
{
    uint32_t i = P_IDX(id);
    if (P_SCOPE(id) == P_ENV) return mod_is_env(i) ? mod_env[i / 16u].p[i % 16u] : mod_is_fol(i) ? mod_fol[(i - MV_FOL0) / 16u].p[(i - MV_FOL0) % 16u] : 0;
    if (P_SCOPE(id) == P_LFO) {
        const tx_lfo_t *l = &mod_lfo[(i / 16u) % TX_LFO_N];
        if (i % 16u == ML_NOW) return (l->out + 32768) >> 11;   /* (in 32 steps: the screen's redraws) */
        if (i % 16u == ML_ROUTES) {
            uint32_t s, n = 0;
            for (s = 0; s < MOD_NS; s++) n += mslot[s].src == MS_LFO1 + i / 16u && mslot[s].tgt;
            return (int32_t)n;
        }
        return l->p[i % 16u];
    }
    if (i == MQ_CC) return mod_cc;
    switch (i % MQ_N) {
    case MQ_SRC: return mslot[i / MQ_N].src;
    case MQ_TGT: return mslot[i / MQ_N].tgt;
    case MQ_DEPTH: return mslot[i / MQ_N].depth;
    }
    return mslot[i / MQ_N].spread;
}
static void mod_set(uint32_t id, int32_t v)
{
    uint32_t i = P_IDX(id);
    if (v < 0) v = 0;
    if (v > mod_hi(id)) v = mod_hi(id);
    if (P_SCOPE(id) == P_ENV) {
        if (mod_is_env(i)) tx_env_param(&mod_env[i / 16u], i % 16u, (uint32_t)v);
        else if (mod_is_fol(i)) tx_fol_param(&mod_fol[(i - MV_FOL0) / 16u], (i - MV_FOL0) % 16u, (uint32_t)v);
        return;
    }
    if (P_SCOPE(id) == P_LFO) { if (i % 16u < TX_LP_N) tx_lfo_param(&mod_lfo[(i / 16u) % TX_LFO_N], i % 16u, (uint32_t)v); return; }
    if (i == MQ_CC) { mod_cc = (uint8_t)v; return; }
    switch (i % MQ_N) {
    case MQ_SRC: mslot[i / MQ_N].src = (uint8_t)v; break;
    case MQ_TGT: {                              /* (past a retired target, the way it was turned; none there: as it was) */
        int32_t o = mslot[i / MQ_N].tgt, st = v > o ? 1 : -1;
        while (v > 0 && v < (int32_t)MT_N && mod_tgt_retired((uint32_t)v)) v += st;
        if (v >= 0 && v < (int32_t)MT_N) mslot[i / MQ_N].tgt = (uint8_t)v;
        break;
    }
    case MQ_DEPTH: mslot[i / MQ_N].depth = (uint8_t)v; break;
    default: mslot[i / MQ_N].spread = (uint8_t)v; break;
    }
}
static int mod_choice(uint32_t id)              /* a choice (segments; turned a step a detent) */
{
    uint32_t i = P_IDX(id) % 16u;
    if (P_SCOPE(id) == P_ENV) return mod_is_env(P_IDX(id)) ? i == TX_EP_TRIGGER || i == TX_EP_LOOP : i == TX_FP_INPUT;
    if (P_SCOPE(id) == P_LFO) return i == TX_LP_SHAPE || i == TX_LP_SYNC || i == TX_LP_MODE || i == TX_LP_RESTART;
    return P_IDX(id) != MQ_CC && (P_IDX(id) % MQ_N == MQ_SRC || P_IDX(id) % MQ_N == MQ_TGT);
}
static const char *mod_text(uint32_t id, char *t)
{
    uint32_t i = P_IDX(id), v = (uint32_t)mod_get(id);
    t[0] = 0;
    if (P_SCOPE(id) == P_ENV) {
        uint32_t ms;
        if (mod_is_env(i)) {
            switch (i % 16u) {
            case TX_EP_TRIGGER: p_strcpy(t, N_ETRIG[v % TX_ET_N], 10); return 0;
            case TX_EP_LOOP: p_strcpy(t, v ? "ON" : "OFF", 4); return 0;
            case TX_EP_ATTACK: case TX_EP_DECAY: case TX_EP_RELEASE: ms = tx_env_ms(v); break;
            default: p_fmt_int(t, (int32_t)((v * 100u + 63u) / 127u)); return "%";
            }
        } else {
            switch ((i - MV_FOL0) % 16u) {
            case TX_FP_INPUT: p_strcpy(t, N_FIN[v % TX_FI_N], 10); return 0;
            case TX_FP_GAIN: p_strcpy(t, "x", 1); { char n[8]; p_fmt_int(n, (int32_t)(v / 64u)); p_cat(t, n, 15); } p_cat(t, ".", 15);
                { char n[4]; uint32_t c = v % 64u * 100u / 64u; n[0] = (char)('0' + c / 10u); n[1] = (char)('0' + c % 10u); n[2] = 0; p_cat(t, n, 15); } return 0;
            default: ms = tx_fol_ms(v); break;
            }
        }
        if (ms >= 10000u) { p_fmt_int(t, (int32_t)((ms + 500u) / 1000u)); return "s"; }
        if (ms >= 1000u) { p_fmt_dec(t, (int32_t)(ms / 100u)); return "s"; }
        p_fmt_int(t, (int32_t)ms); return "ms";
    }
    if (P_SCOPE(id) == P_LFO) {
        const tx_lfo_t *l = &mod_lfo[(i / 16u) % TX_LFO_N];
        switch (i % 16u) {
        case TX_LP_SHAPE: p_strcpy(t, N_LSHAPE[v % TX_LS_N], 10); return 0;
        case TX_LP_SYNC: p_strcpy(t, N_LSYNC[v % TX_LFO_NSYNC], 10); return 0;
        case TX_LP_MODE: p_strcpy(t, N_LMODE[v % TX_LM_N], 10); return 0;
        case TX_LP_RESTART: p_strcpy(t, v ? "ON START" : "OFF", 9); return 0;
        case TX_LP_RATE: {                      /* Hz, or the period in s / min */
            uint32_t ms = tx_lfo_period_ms(v);
            if (l->p[TX_LP_SYNC]) { p_strcpy(t, N_LSYNC[l->p[TX_LP_SYNC]], 10); return 0; }
            if (ms >= 120000u) { p_fmt_dec(t, (int32_t)(ms / 6000u)); return "min"; }
            if (ms >= 10000u) { p_fmt_int(t, (int32_t)((ms + 500u) / 1000u)); return "s"; }
            if (ms >= 1000u) { p_fmt_dec(t, (int32_t)(ms / 100u)); return "s"; }
            {
                uint32_t c = 100000u / ms;      /* centi-Hz */
                p_fmt_int(t, (int32_t)(c / 100u));
                if (c < 1000u) { char d[3] = {(char)('0' + c / 10u % 10u), (char)('0' + c % 10u), 0}; p_cat(t, ".", 15); p_cat(t, d, 15); }
                return "Hz";
            }
        }
        case TX_LP_PHASE: p_fmt_int(t, (int32_t)(v * 360u / 128u)); return "deg";
        case TX_LP_JITTER: p_fmt_int(t, (int32_t)((v * 50u + 63u) / 127u)); return "%";
        case TX_LP_FADE: p_fmt_dec(t, (int32_t)(v * 100u / 127u)); return "s";
        case TX_LP_LEVEL: case TX_LP_WIDTH: p_fmt_int(t, (int32_t)((v * 100u + 63u) / 127u)); return "%";
        case ML_NOW: { int32_t pc = l->out * 100 / 32767; if (pc > 0) { t[0] = '+'; p_fmt_int(t + 1, pc); } else p_fmt_int(t, pc); return "%"; }
        }
        p_fmt_int(t, (int32_t)v);
        return 0;
    }
    if (i == MQ_CC) { p_strcpy(t, "CC ", 3); { char n[4]; p_fmt_int(n, (int32_t)v); p_cat(t, n, 15); } return 0; }
    switch (i % MQ_N) {
    case MQ_SRC:
        if (v == MS_CC) { p_strcpy(t, "CC ", 3); { char n[4]; p_fmt_int(n, mod_cc); p_cat(t, n, 15); } return 0; }
        p_strcpy(t, N_MSRC[v % MS_N], 10); return 0;
    case MQ_TGT: mod_tgt_text(v, t); return 0;
    case MQ_DEPTH: { int32_t d = (int32_t)v - 100; if (d > 0) { t[0] = '+'; p_fmt_int(t + 1, d); } else p_fmt_int(t, d); return "%"; }
    }
    p_fmt_int(t, (int32_t)(v * 90u / 127u)); return "deg";
}

/* ---- mod learn (ui.c: LFO or ENV held + a card turned): the source onto param id, its depth moved by d (%); 0 removes the slot.
 * Returns the slot (or -1: not a target, -2: the matrix is full) */
static int mod_learn(uint32_t src, uint32_t id, int32_t d)   /* src: an MS_ source (LFO n, ENV n, FOLLOW n) */
{
    uint32_t T = mod_tgt_of(id), s;
    int32_t dp;
    if (!T) return -1;
    for (s = 0; s < MOD_NS && !(mslot[s].src == src && mslot[s].tgt == T); s++) {}
    if (s == MOD_NS) {
        for (s = 0; s < MOD_NS && mslot[s].src && mslot[s].tgt; s++) {}
        if (s == MOD_NS) return -2;
        mslot[s].src = (uint8_t)src; mslot[s].tgt = (uint8_t)T; mslot[s].depth = 100; mslot[s].spread = 0;
    }
    dp = (int32_t)mslot[s].depth + d;
    mslot[s].depth = (uint8_t)(dp < 0 ? 0 : dp > 200 ? 200 : dp);
    if (mslot[s].depth == 100u) { mslot[s].src = MS_NONE; mslot[s].tgt = 0; }   /* (back to 0: the slot is free) */
    return (int)s;
}
static void mod_init(void)
{
    uint32_t i;
    for (i = 0; i < TX_LFO_N; i++) tx_lfo_init(&mod_lfo[i], i, tx_seed());
    for (i = 0; i < TX_ENV_N; i++) tx_env_init(&mod_env[i], tx_seed());
    for (i = 0; i < TX_FOL_N; i++) tx_fol_init(&mod_fol[i]);
    for (i = 0; i < MOD_NS; i++) { mslot[i].depth = 100; mslot[i].spread = 32; }
    mod_ext[ME_CONST] = 32767;
}
