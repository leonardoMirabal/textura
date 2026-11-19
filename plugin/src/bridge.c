/* SPDX-License-Identifier: GPL-3.0-only */
/* The plugin instance (plugin/src/bridge.h): the slot table, the host's frames and events onto the app's frames,
 * the oversampler, float out. From Frodhes' plugin/src/bridge.c (its time mapping, flood parking, oversampler and
 * windowed sinc), without its user preset files, its UI queue and its FM-1 USB test output.
 *
 * Time. The app always runs at 44.1 kHz in 256-frame blocks (slot_bridge.h). Output frame T (from the activation) is
 * the app's frame e0 + (T - lat) x 44100 / rate, lat the latency the host is told, e0 the app's frame at the
 * activation; an event at host frame h is for the app's frame e0 + h x 44100 / rate (in 1/256 frame) and acts from
 * the start of the 256-frame block that holds it (as every event on the FM-1). The slot renders a block only when the
 * output needs its first frame, and the events of a block must all be known by then: the host gives a block's events
 * before its audio, so lat covers one app block (256 frames, 5.8 ms) + the oversampler's delay (0 at 44.1 kHz, ~1 ms
 * otherwise) + 2 frames. An event that comes late anyway starts at the next block's start and is counted
 * (txp_inst_stats: late).
 *
 * Out: the app's int32 samples x 1/32768 (no clip: the host's mixer has the headroom; the app's own limiter holds
 * -2 dBFS), at 44.1 kHz as they are; at 88.2 / 176.4 / 352.8 and 48 / 96 / 192 / 384 kHz through plugin/src/txp_os.h
 * (integer-ratio polyphase stages: 160/147, then 2x half-bands); any other rate (1 to 1536 kHz) through a
 * windowed-sinc resampler in float (RS_HALF zero crossings, a Kaiser window, the cutoff 0.96 x the lower Nyquist),
 * read at the 44.1 kHz time of each output frame.
 *
 * The test mode (TEXTURA_PLUGIN_TEST=ms in the environment when the instance is made and activated;
 * tools/plugin_gate.py): the host build's time (tests/host.c): an event at host frame h is due at the app's millisecond
 * ms0 + h / (frames a ms) and acts from the next 256-frame block; the output starts with a pre-roll of TXP_LAT_MS
 * (10 ms, which keeps every event ahead of the app) + the oversampler's delay. After the latency the output is the
 * host build's render of the same events through the oversampler (44.1 kHz: sample for sample). */
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "bridge.h"
#include "txp_os.h"

static const txp_slot_api_t *const SLOT[TXP_SLOTS] = {
    &txp_slot_0, &txp_slot_1, &txp_slot_2, &txp_slot_3, &txp_slot_4, &txp_slot_5, &txp_slot_6, &txp_slot_7,
    &txp_slot_8, &txp_slot_9, &txp_slot_10, &txp_slot_11, &txp_slot_12, &txp_slot_13, &txp_slot_14, &txp_slot_15,
};
static atomic_int slot_used[TXP_SLOTS];

#define CHUNK 256u                       /* frames read from the FIFO at a time */
#define RS_HALF 96                       /* the other rates: zero crossings each side of the sinc */
#define RS_OS 512                        /* table points a zero crossing (linear between them) */
#define RS_BETA 9.0                      /* the Kaiser window (about -90 dB side lobes) */
#define RS_N 16384u                      /* the input ring (power of 2) */
#define RS_MIN 1000.0
#define RS_MAX 1536000.0
enum { T_EXACT, T_MS };                  /* the events' time: the plugin's own, the host build's (the test mode) */
enum { O_OS, O_SINC };                   /* out: txp_os.h, the sinc */
#define PK_MIDI (16u * 128u + 3u * 16u)  /* the parked MIDI: CC (channel x controller), program, pressure, bend */
#define PK_N (TXP_NPARAM_MAX + PK_MIDI)

struct txp_inst {
    int slot;                            /* the slot playing (-1 none) */
    const txp_slot_api_t *s;
    /* changes held back while the slot's queue is nearly full (a host's flood of automation): the latest value and
     * time per parameter and per MIDI controller, put into the queue as it empties (park_put, park_flush) */
    int32_t pk_v[PK_N];
    uint32_t pk_at[PK_N];
    uint8_t pk_on[PK_N], pk_x[PK_N];
    uint32_t pk_n, pk_merged;
    int active, tmode, omode, m441, m48;
    uint32_t lat;                        /* the latency the host is told */
    uint32_t e0;                         /* the app's frame at the activation (exact) */
    txp_os_t os;
    uint32_t bi, bn;                     /* the oversampler's source: buf[bi .. bn) not yet given */
    uint64_t pos;                        /* host frames since the activation */
    uint32_t ms0;                        /* the slot's ms at the activation */
    uint32_t late;
    int32_t buf[2u * CHUNK];
    double rate, fc, reach;              /* the sinc: the rate, the cutoff, the reach in input frames */
    int64_t kin;
    float rl[RS_N], rr[RS_N];
};

/* ---- the host parameters: the same for every instance (slot 0's table, made from constant tables) */
static txp_param_t ptab[TXP_NPARAM_MAX];
static uint32_t pn;
static pthread_once_t ptab_once = PTHREAD_ONCE_INIT;
static void ptab_make(void) { pn = SLOT[0]->params(ptab, TXP_NPARAM_MAX); }
const txp_param_t *txp_params(uint32_t *n)
{
    pthread_once(&ptab_once, ptab_make);
    if (n) *n = pn;
    return ptab;
}
int txp_param_index(uint32_t id)
{
    uint32_t i;
    txp_params(0);
    for (i = 0; i < pn; i++) if (ptab[i].id == id) return (int)i;
    return -1;
}
int txp_param_text(uint32_t idx, double v, char *out, uint32_t n)
{
    int32_t x;
    if (idx >= pn || !n) return -1;
    x = (int32_t)floor(v + 0.5);
    x = x < 0 ? 0 : x > ptab[idx].hi ? ptab[idx].hi : x;
    if (SLOT[0]->text(ptab[idx].id, x, out, n)) snprintf(out, n, "%d", (int)x);
    return 0;
}
int txp_param_parse(uint32_t idx, const char *text, double *out)
{
    char t[64], *end;
    double x;
    if (idx >= pn || !text) return -1;
    for (int32_t v = 0; v <= ptab[idx].hi; v++)   /* a value's own text ("4.2 kHz", "D100 W50") */
        if (!SLOT[0]->text(ptab[idx].id, v, t, sizeof t) && !strcasecmp(text, t)) { *out = v; return 0; }
    x = strtod(text, &end);              /* else the raw value 0 .. hi */
    if (end == text) return -1;
    *out = x < 0.0 ? 0.0 : x > ptab[idx].hi ? ptab[idx].hi : floor(x + 0.5);
    return 0;
}

/* ---- the windowed sinc */
static float rs_tab[RS_HALF * RS_OS + 2];
static pthread_once_t rs_once = PTHREAD_ONCE_INIT, os_once = PTHREAD_ONCE_INIT;
static double bessel_i0(double x)
{
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 40; k++) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; }
    return s;
}
static void rs_init(void)                /* sinc(u) x Kaiser(u / RS_HALF), u = 0 .. RS_HALF */
{
    double i0b = bessel_i0(RS_BETA);
    for (int i = 0; i <= RS_HALF * RS_OS + 1; i++) {
        double u = (double)i / RS_OS, w = u / RS_HALF, si = u == 0.0 ? 1.0 : sin(M_PI * u) / (M_PI * u);
        rs_tab[i] = w >= 1.0 ? 0.0f : (float)(si * bessel_i0(RS_BETA * sqrt(1.0 - w * w)) / i0b);
    }
}

static uint32_t os_plan(txp_inst_t *in, double rate);
static int slot_claim(void)
{
    int i, pass;
    for (pass = 0; pass < 2; pass++)     /* a slot never used first (a clean app), then a used one */
        for (i = 0; i < (int)TXP_SLOTS; i++) {
            int z = 0;
            if (pass == 0 && SLOT[i]->booted()) continue;
            if (atomic_compare_exchange_strong(&slot_used[i], &z, 1)) return i;
        }
    return -1;
}
txp_inst_t *txp_inst_new(void)
{
    txp_inst_t *in = calloc(1, sizeof *in);
    const char *test = getenv("TEXTURA_PLUGIN_TEST");
    int sl;
    if (!in) return 0;
    txp_params(0);                       /* (the table first: slot 0 makes it before any slot runs) */
    in->slot = -1;
    in->tmode = test && !strcmp(test, "ms") ? T_MS : T_EXACT;
    in->lat = os_plan(in, 48000.0);      /* (the latency before the activation: at a usual rate) */
    if ((sl = slot_claim()) < 0) return in;   /* (all in use: silence) */
    if (SLOT[sl]->booted()) SLOT[sl]->reuse();
    else if (SLOT[sl]->boot()) { atomic_store(&slot_used[sl], 0); return in; }   /* (booted now: the main thread) */
    in->slot = sl;
    in->s = SLOT[sl];
    return in;
}
void txp_inst_free(txp_inst_t *in)
{
    if (!in) return;
    if (in->slot >= 0) atomic_store(&slot_used[in->slot], 0);
    free(in);
}
int txp_inst_slot(const txp_inst_t *in) { return in ? in->slot : -1; }

static void park_flush(txp_inst_t *in);
static void park_drain(txp_inst_t *in);
static void os_poly_once(void) { txp_os_make_poly(); }
static void os_src(void *ctx, float *l, float *r)   /* the app's next frame: what is rendered, else one step at a time */
{
    txp_inst_t *in = ctx;
    if (in->bi == in->bn) {
        uint32_t a = in->s->avail();
        while (!a) { in->s->step(); a = in->s->avail(); }
        if (a > CHUNK) a = CHUNK;
        in->s->read(in->buf, a);
        in->bi = 0; in->bn = a;
    }
    *l = (float)in->buf[2u * in->bi] * (1.0f / 32768.0f);
    *r = (float)in->buf[2u * in->bi + 1u] * (1.0f / 32768.0f);
    in->bi++;
}
static uint32_t os_plan(txp_inst_t *in, double rate)   /* the oversampler set up: the chain's own delay, + one app
                                                        * block (exact) or the 10 ms pre-roll (ms); the latency */
{
    uint32_t d, ahead = in->tmode == T_EXACT ? (uint32_t)ceil(256.0 * rate / TXP_FS) + 2u
                                             : (uint32_t)lround(TXP_LAT_MS * rate / 1000.0);
    pthread_once(&os_once, os_poly_once);
    d = txp_os_init(&in->os, rate, 0, os_src, in);
    return txp_os_init(&in->os, rate, d + ahead, os_src, in);
}
int txp_inst_activate(txp_inst_t *in, double rate)
{
    const char *test = getenv("TEXTURA_PLUGIN_TEST");
    int nhb;
    if (!(rate >= RS_MIN && rate <= RS_MAX)) return -1;
    in->tmode = test && !strcmp(test, "ms") ? T_MS : T_EXACT;
    in->m441 = rate == 44100.0;
    in->m48 = rate == 48000.0;
    in->omode = txp_os_family(rate, &nhb) >= 0 ? O_OS : O_SINC;
    in->rate = rate;
    in->pos = 0;
    in->bi = in->bn = 0;
    if (in->omode == O_OS) in->lat = os_plan(in, rate);
    else {
        pthread_once(&rs_once, rs_init);
        in->fc = 0.96 * (rate < TXP_FS ? rate / TXP_FS : 1.0);
        in->reach = RS_HALF / in->fc;
        in->lat = in->tmode == T_MS ? (uint32_t)ceil((TXP_LAT_MS + ceil(in->reach * 1000.0 / TXP_FS)) * rate / 1000.0)
                                    : (uint32_t)ceil((256.0 + in->reach) * rate / TXP_FS) + 2u;
        in->kin = 0;
        memset(in->rl, 0, sizeof in->rl); memset(in->rr, 0, sizeof in->rr);
    }
    in->late = 0;
    if (in->s) {
        uint32_t a;
        in->s->set_exact(in->tmode == T_EXACT);
        park_flush(in);                  /* (what stayed parked at the deactivation) */
        in->s->drain();                  /* (what the main thread queued while inactive) */
        while ((a = in->s->avail()) != 0) in->s->read(in->buf, a < CHUNK ? a : CHUNK);   /* (frames from before) */
        in->ms0 = in->s->now();
        in->e0 = in->s->efr();
    }
    in->active = 1;
    return 0;
}
void txp_inst_deactivate(txp_inst_t *in)
{
    in->active = 0;
    txp_inst_drain(in);                  /* (what is queued or parked: applied now, and settled as a flush is) */
}
uint32_t txp_inst_latency(const txp_inst_t *in) { return in->lat; }
void txp_inst_reset(txp_inst_t *in)
{
    if (in->s) (void)in->s->put(TXP_EV_RESET, 0, 0, 0);
}

static uint32_t due_x(txp_inst_t *in, uint32_t time)   /* exact: the app frame x 256 an event at this frame is for;
                                                        * one whose block is rendered already is late (counted) */
{
    double q = (double)(in->pos + time) * (TXP_FS * 256.0) / in->rate;
    uint32_t at = in->e0 * 256u + (uint32_t)(uint64_t)llround(q), fw = in->s->efw() * 256u;
    if ((int32_t)(at - fw) < 0) { in->late++; at = fw; }
    return at;
}
static uint32_t due_ms(txp_inst_t *in, uint32_t time)
{
    uint64_t h = in->pos + time;
    uint32_t t = in->ms0 + (uint32_t)(in->m48 ? h / 48u : in->m441 ? h * 10u / 441u : (uint64_t)floor((double)h * 1000.0 / in->rate)),
             now = in->s->now();
    if ((int32_t)(t - now) <= 0) {
        if (!(t == now && now == in->ms0)) in->late++;   /* (an event at the activation's own ms: the first step's) */
        t = now + 1u;
    }
    return t;
}
/* The flood. The slot's queue holds SB_Q events; a host may send far more in one block (a validator's parameter
 * thread: thousands of changes a block). Rather than drop them, a change that finds PK_ROOM places or fewer left is
 * parked: one entry a parameter, and one a MIDI controller of a channel, its program, its pressure and its bend; the
 * entry keeps the latest value at the latest time, and every later change of the same thing joins it while it is
 * parked (so the order per parameter or controller holds). MIDI that is not a level (notes, the switch and mode
 * controllers, RPN / NRPN, bank select) is never merged; the queue keeps PK_ROOM / 2 places for it. */
#define PK_ROOM 1024u
static int park_midi_key(uint8_t st, uint8_t d1)   /* the MIDI's entry (TXP_NPARAM_MAX + ...), -1 not a level */
{
    uint32_t t = st & 0xF0u, ch = st & 15u, cc = d1 & 127u;
    if (t == 0xC0u) return (int)(TXP_NPARAM_MAX + 16u * 128u + ch);
    if (t == 0xD0u) return (int)(TXP_NPARAM_MAX + 16u * 128u + 16u + ch);
    if (t == 0xE0u) return (int)(TXP_NPARAM_MAX + 16u * 128u + 32u + ch);
    if (t != 0xB0u || cc == 0u || cc == 32u || cc == 6u || cc == 38u || (cc >= 64u && cc <= 69u) || (cc >= 96u && cc <= 101u) ||
        cc >= 120u) return -1;
    return (int)(TXP_NPARAM_MAX + ch * 128u + cc);
}
static int sput(txp_inst_t *in, int x, int kind, uint32_t at, uint32_t a, int32_t v)
{
    return x ? in->s->putx(kind, at, a, v) : in->s->put(kind, at, a, v);
}
static int park_put(txp_inst_t *in, uint32_t i, uint32_t at, int32_t v, int x)   /* 1: parked (else the caller puts it) */
{
    if (!in->pk_on[i] && !(at && in->s->room() <= PK_ROOM)) return 0;   /* (parked already: so must this one be) */
    if (!at && in->tmode == T_MS) at = in->s->now() + 1u;
    if (!at) x = 0;
    if (!in->pk_on[i]) { in->pk_on[i] = 1; in->pk_n++; in->pk_at[i] = at; in->pk_x[i] = (uint8_t)x; }
    else {
        in->pk_merged++;
        if (!at || !in->pk_at[i]) { in->pk_at[i] = 0; in->pk_x[i] = 0; }
        else if ((int32_t)(at - in->pk_at[i]) > 0) in->pk_at[i] = at;
    }
    in->pk_v[i] = v;
    return 1;
}
static void park_flush(txp_inst_t *in)   /* the parked changes into the queue, as far as it has room */
{
    if (!in->pk_n || !in->s) return;
    for (uint32_t i = 0; i < PK_N && in->pk_n; i++) {
        if (!in->pk_on[i]) continue;
        if (in->s->room() <= PK_ROOM / 2u) return;
        if (i < TXP_NPARAM_MAX ? sput(in, in->pk_x[i], TXP_EV_PARAM, in->pk_at[i], ptab[i].id, in->pk_v[i])
                               : sput(in, in->pk_x[i], TXP_EV_MIDI, in->pk_at[i], (uint32_t)in->pk_v[i], 0)) return;
        in->pk_on[i] = 0;
        in->pk_n--;
    }
}
static void park_drain(txp_inst_t *in)   /* the thread that owns the slot, no block running */
{
    for (int k = 0; k < 4 && in->s; k++) {
        park_flush(in);
        in->s->drain();
        if (!in->pk_n) break;
    }
}
void txp_inst_midi(txp_inst_t *in, uint32_t time, uint8_t st, uint8_t d1, uint8_t d2)
{
    uint32_t a = (uint32_t)st | (uint32_t)(d1 & 127u) << 8 | (uint32_t)(d2 & 127u) << 16, at;
    int k, x;
    if (!in->s || st < 0x80u || st == 0xF0u || st == 0xF7u) return;
    x = in->tmode == T_EXACT;
    at = x ? due_x(in, time) : due_ms(in, time);
    if ((k = park_midi_key(st, d1)) >= 0 && park_put(in, (uint32_t)k, at, (int32_t)a, x)) return;
    (void)sput(in, x, TXP_EV_MIDI, at, a, 0);
}
static int32_t param_round(uint32_t idx, double v)
{
    double hi = ptab[idx].hi;
    if (!(v >= 0.0)) v = 0.0;            /* (NaN too) */
    if (v > hi) v = hi;
    return (int32_t)floor(v + 0.5);
}
static void param_put(txp_inst_t *in, uint32_t at, uint32_t id, double value, int x)
{
    int i = txp_param_index(id);
    if (i < 0 || !in->s) return;
    if (park_put(in, (uint32_t)i, at, param_round((uint32_t)i, value), x)) return;
    (void)sput(in, x && at, TXP_EV_PARAM, at, id, param_round((uint32_t)i, value));
}
void txp_inst_param(txp_inst_t *in, uint32_t time, uint32_t id, double value)
{
    int x = in->tmode == T_EXACT;
    param_put(in, !in->s ? 0u : x ? due_x(in, time) : due_ms(in, time), id, value, x);
}
void txp_inst_param_now(txp_inst_t *in, uint32_t id, double value) { param_put(in, 0u, id, value, 0); }
/* the window's own events (a mute, the solo): never parked (one at a click) */
void txp_inst_event(txp_inst_t *in, uint32_t time, int kind, uint32_t a, int32_t v)
{
    int x = in->tmode == T_EXACT;
    if (!in->s) return;
    (void)sput(in, x, kind, x ? due_x(in, time) : due_ms(in, time), a, v);
}
void txp_inst_event_now(txp_inst_t *in, int kind, uint32_t a, int32_t v) { if (in->s) (void)in->s->put(kind, 0, a, v); }
uint32_t txp_inst_act(const txp_inst_t *in, uint8_t (*ev)[TXP_LANES], uint8_t (*pk)[TXP_LANES], uint32_t n)
{
    if (!in->s) { memset(ev, 0, n * sizeof *ev); memset(pk, 0, n * sizeof *pk); return 0; }
    return in->s->act(ev, pk, n);
}
/* the queued changes applied (the main thread, no block running). Not active: then run until what they started is
 * done (sb_settle: a SOUND's engine switch, the cards of its new sound, a PRESET's load), as the FM-1's main loop
 * would have before anyone looked; so the values read back, and a state saved, are the settled ones */
void txp_inst_drain(txp_inst_t *in)
{
    park_drain(in);
    if (in->s && !in->active) in->s->settle();
}
int txp_inst_get(const txp_inst_t *in, uint32_t id, double *value)
{
    txp_snap_t sn;
    int i = txp_param_index(id);
    if (i < 0) return -1;
    if (!in->s) { *value = 0; return 0; }
    in->s->snapshot(&sn);
    *value = sn.pv[i];
    return 0;
}
int txp_inst_snap(const txp_inst_t *in, txp_snap_t *out)
{
    if (!in->s) return -1;
    in->s->snapshot(out);
    return 0;
}
/* the state: read on the main thread. Not active: the main thread owns the slot (the queue applied, captured now);
 * active: the audio side captures it at its next step (a host that stops processing meanwhile: the last capture) */
int txp_inst_state_get(const txp_inst_t *in, char *buf, int n)
{
    if (!in->s) return -1;
    if (!in->active) park_drain((txp_inst_t *)in);
    return in->s->state_get(buf, n, in->active);
}
/* a state: active, into the slot's mailbox (the next step applies it, a scene load in real time); not active, applied
 * now and run until its load is done (sb_settle), so the values read back at once are the state's */
int txp_inst_state_set(txp_inst_t *in, const char *json)
{
    if (!in->s || !json) return -1;
    if (in->s->state_set(json)) return -1;   /* (parsed first: a state it cannot read changes nothing) */
    if (!in->active) { park_drain(in); in->s->settle(); }
    return 0;
}
uint32_t txp_inst_rev(const txp_inst_t *in)
{
    txp_snap_t sn;
    if (!in->s) return 0;
    in->s->snapshot(&sn);
    return sn.rev;
}
void txp_inst_stats(const txp_inst_t *in, txp_stats_t *st, uint32_t *late)
{
    if (in->s) in->s->stats(st); else memset(st, 0, sizeof *st);
    *late = in->late;
}

/* n frames out of the slot (stepping it as needed), n <= CHUNK */
static void pull(txp_inst_t *in, uint32_t n)
{
    while (in->s->avail() < n) in->s->step();
    in->s->read(in->buf, n);
}
/* the other rates: output frame j (from the activation) is the 44.1 kHz signal at input frame
 * t = (j - latency) x 44100 / rate; the input frames before the first one read are silence */
static void rs_render(txp_inst_t *in, float *l, float *r, uint32_t n)
{
    const double step = TXP_FS / in->rate, sc = 1.0 / 32768.0, os = in->fc * RS_OS;
    const double pre = (double)txp_inst_latency(in) * step;
    for (uint32_t i = 0; i < n; i++) {
        double t = (double)(in->pos + i) * step - pre, al = 0.0, ar = 0.0, ws = 0.0;
        int64_t k0 = (int64_t)ceil(t - in->reach), k1 = (int64_t)floor(t + in->reach), k;
        while (in->kin <= k1) {          /* the input frames up to k1 */
            int64_t want = k1 - in->kin + 1;
            uint32_t c = want < (int64_t)CHUNK ? (uint32_t)want : CHUNK, q;
            pull(in, c);
            for (q = 0; q < c; q++) {
                in->rl[(uint64_t)(in->kin + q) & (RS_N - 1u)] = (float)in->buf[2u * q];
                in->rr[(uint64_t)(in->kin + q) & (RS_N - 1u)] = (float)in->buf[2u * q + 1u];
            }
            in->kin += c;
        }
        for (k = k0; k <= k1; k++) {
            double x = fabs((double)k - t) * os, w;
            int32_t xi = (int32_t)x;
            if (xi >= RS_HALF * RS_OS) continue;
            w = rs_tab[xi] + (x - xi) * (rs_tab[xi + 1] - rs_tab[xi]);
            ws += w;
            if (k < 0) continue;         /* (before the first frame: silence) */
            al += w * in->rl[(uint64_t)k & (RS_N - 1u)];
            ar += w * in->rr[(uint64_t)k & (RS_N - 1u)];
        }
        if (ws > 0.0) { al /= ws; ar /= ws; }   /* (unity gain at DC whatever the phase) */
        l[i] = (float)(al * sc); r[i] = (float)(ar * sc);
    }
}
void txp_inst_render(txp_inst_t *in, float *l, float *r, uint32_t n, uint32_t tempo_bpm10)
{
    if (!in->s || !in->active) { memset(l, 0, n * sizeof *l); memset(r, 0, n * sizeof *r); return; }
    park_flush(in);
    in->s->set_tempo(tempo_bpm10);
    if (in->omode == O_OS) txp_os_render(&in->os, l, r, n);
    else rs_render(in, l, r, n);
    in->pos += n;
}
