/* SPDX-License-Identifier: GPL-3.0-only */
/* The landscape model: which landscape plays, which of its sounds sits on each of the four lanes, and each lane's
 * HOW MANY (the source's COUNT) and HOW ACTIVE (the lane generator's ACTIVITY), its volume, its pan and its mute. It is the
 * audition player's lane mode (audition/app.js: soundLoad, soundKey, laneCount, laneStep, laneStart, quickTick,
 * setFamily, startLandscape) in C, on the tables tools/gen_landscapes.py writes from app.js (landscapes.h).
 *
 * The state here is what the player wants; sc_sync sends a lane's engine only what differs from what it was last sent
 * (lanes.c's queue), so a change made while a lane's last engine switch still rings (ln_can_select: no) is simply
 * sent later; meanwhile the old engine plays on, then fades out under the new one (lanes.c). A scene's load (and a
 * switch) glides a lane's level, pan, trim, HOW MANY and HOW ACTIVE from what was sent to what is wanted (sc_glide,
 * SC_GLIDE_MS). A sound is a SOURCE preset + TYPE, or a classic engine: those have no population, so HOW MANY and
 * HOW ACTIVE together move a few of their own parameters (the QUICK maps), and the one-shot ones (thunder, glass ...)
 * are pressed again and again at random (exponential) gaps.
 *
 * Main loop only (sc_poll every pass): integers, no division in 64 bits. */
#include "landscapes.h"
#include "tx_core_lin.h"

#define SC_N LN_N                               /* lanes */
#define SC_STEPS 14                             /* HOW ACTIVE as a step 1 .. 14 (0: off), as the player's */
#define SC_QUICK_MS 50u                         /* the classic engines' glide: 20 a second, 0.4 of the way each */
#define SC_HOLD_NOTE 60u                        /* the key a lane holds down (the source, a held classic engine) */
#define SC_HOLD_VEL 100u
#define SC_PRESS_MS 150u                        /* a one-shot engine's press, and its shortest gap */
#define SC_FAM_MS 300u                          /* ALGORITHM: the landscape changes this long after the last detent */
#define SC_GLIDE_MS 500u                        /* a scene's (a switch's) lane settings: from the old to the new */
#define SC_GLIDE_TICK 10u                       /* (its steps) */
enum { SC_GL_LEVEL, SC_GL_PAN, SC_GL_TRIM, SC_GL_COUNT, SC_GL_ACT };

static const uint16_t SC_BLINK_MS[LS_NACT] = {1600, 1498, 1155, 776, 503, 333, 228, 163, 120};   /* the blink's rate on the
                                                 * keys' curve: 1 / (1 / 1.6 s + (1 / 0.12 s - 1 / 1.6 s) (a / 8)^2.5) */

static struct {
    uint8_t fam, sel;                           /* the landscape; the selected lane */
    uint8_t fam_want, fam_pend;                 /* ALGORITHM's landscape, waiting for SC_FAM_MS */
    uint32_t fam_t;
    const tx_engine_t *src;                     /* the SOURCE engine */
    int8_t id_preset, id_type, id_count;        /* its parameters */
    /* each lane, as wanted */
    int8_t snd[SC_N];                           /* the sound, in its family (-1: none) */
    uint8_t lf[SC_N];                           /* the lane's family: the landscape's, or another (the sound browser) */
    const tx_engine_t *eng[SC_N];
    uint8_t preset[SC_N], type[SC_N];           /* (the source) */
    uint8_t cnt[SC_N], step[SC_N], run[SC_N], notes[SC_N], pan[SC_N];
    uint8_t mute[SC_N];                         /* muted: the lane plays on, its output gated (lanes.c ln_gate) */
    uint8_t reswap[SC_N];                       /* the source's sound changed (PRESET / TYPE): a fresh instance, the
                                                 * old one rings out (lanes.c), never re-made in place (a cut) */
    uint32_t nsel[SC_N], sel_t[SC_N];           /* the lane's engine switches so far (the tests: a lane left alone);
                                                 * the last one's time */
    const ls_quick_t *q[SC_N];                  /* a classic engine's map (0: none) */
    /* and as last sent */
    const tx_engine_t *a_eng[SC_N];
    int16_t a_preset[SC_N], a_type[SC_N], a_count[SC_N], a_act[SC_N], a_level[SC_N], a_pan[SC_N];
    int32_t a_trim[SC_N];                       /* (the sound's own level, Q12: landscapes.h) */
    uint8_t a_gen[SC_N], a_mute[SC_N], a_held[SC_N], a_gate[SC_N];   /* (a_gate: muted as last sent) */
    /* the glide (sc_glide): LEVEL, PAN, the sound's trim, HOW MANY (COUNT), HOW ACTIVE (ACTIVITY) on their way from
     * what was sent (-1: nothing yet, so no glide) to what is wanted, SC_GLIDE_MS end to end */
    int32_t gl_from[SC_N][5];
    uint32_t gl_t0[SC_N], gl_tick;
    uint8_t gl_on[SC_N];
    /* the classic engines' glide and presses */
    int8_t q_id[SC_N][3];
    int32_t q_cur[SC_N][3];                     /* Q8 (-1 << 30: not yet sent) */
    uint32_t q_next[SC_N], q_rel[SC_N];         /* the next press, the press's release (0: none) */
    uint8_t q_on[SC_N];
    uint32_t q_t;
    uint8_t vol[LS_N][LS_SND_MAX];              /* each sound's volume (LEVEL, 100 = as tuned) */
    uint8_t mem_cnt[SC_N], mem_step[SC_N];      /* REC: what played when all stopped */
    tx_rng_t rng;
} sc;

/* ---- the player's scales */
static uint32_t sc_act_step(uint32_t w) { return 1u + (w * 26u + LS_NACT - 1u) / (2u * (LS_NACT - 1u)); }   /* HOW ACTIVE key
                                                 * 0 .. 8 -> step 1 .. 14, even (1 3 4 6 8 9 11 12 14) */
static uint32_t sc_act_of(uint32_t s) { return s ? ((s - 1u) * 2u * (LS_NACT - 1u) + 13u) / 26u : 0u; }   /* step -> HOW
                                                 * ACTIVE 0 .. 8 */
static uint32_t sc_act_val(uint32_t s)         /* step -> ACTIVITY 0 .. 127: the key's, equal steps (0, 16, 32 .. 127; the
                                                 * source and the generator are perceptually even in it: each key sounds
                                                 * the same more, tx_core_lin.h) */
{
    return s ? (sc_act_of(s) * 254u + LS_NACT - 1u) / (2u * (LS_NACT - 1u)) : 0u;
}

static int sc_eq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
/* the source's preset by name, read from its PRESET help line ("the source: 0 rain on water | 1 stone cloud | ...") */
static int sc_preset_index(const char *name)
{
    const char *h = sc.src && sc.id_preset >= 0 ? sc.src->params[sc.id_preset].help : "";
    while (*h && *h != ':') h++;
    while (*h) {
        int n = 0;
        const char *a;
        h++;                                    /* (':' or '|') */
        while (*h == ' ') h++;
        if (*h < '0' || *h > '9') break;
        while (*h >= '0' && *h <= '9') n = n * 10 + (*h++ - '0');
        while (*h == ' ') h++;
        for (a = name; *a && *a == *h; a++, h++) {}
        if (!*a && (!*h || *h == '|' || *h == ' ')) {
            while (*h == ' ') h++;
            if (!*h || *h == '|') return n;
        }
        while (*h && *h != '|') h++;
    }
    return -1;
}
static const ls_quick_t *sc_quick(const tx_engine_t *e)
{
    uint32_t i;
    for (i = 0; e && i < LS_NQUICK; i++) if (sc_eq(LS_QUICK[i].eng, e->name)) return &LS_QUICK[i];
    return 0;
}
static int sc_is_src(uint32_t k) { return sc.eng[k] && sc.eng[k] == sc.src; }
static int sc_oneshot(uint32_t k) { return !sc_is_src(k) && sc.q[k] && sc.q[k]->every_ms; }
static uint32_t sc_cnt_of(uint32_t k) { return sc.run[k] ? sc.cnt[k] : 0u; }
static uint32_t sc_step_of(uint32_t k) { return sc.run[k] ? sc.step[k] : 0u; }
static uint32_t sc_vol_of(uint32_t k)
{
    return sc.snd[k] >= 0 ? sc.vol[sc.lf[k]][sc.snd[k]] : 100u;
}
static const char *sc_sound_name(uint32_t k) { return sc.snd[k] >= 0 ? LS[sc.lf[k]].s[sc.snd[k]].name : "-"; }
static uint32_t sc_blink_ms(uint32_t k) { return SC_BLINK_MS[sc_act_of(sc_step_of(k))]; }

/* ---- the glide: a lane's settings from what they were to what they are wanted, SC_GLIDE_MS end to end (a scene's
 * load, a switch: a level, a pan, a population never steps) */
static void sc_glide(uint32_t k)
{
    sc.gl_from[k][SC_GL_LEVEL] = sc.a_level[k]; sc.gl_from[k][SC_GL_PAN] = sc.a_pan[k];
    sc.gl_from[k][SC_GL_TRIM] = sc.a_trim[k]; sc.gl_from[k][SC_GL_COUNT] = sc.a_count[k];
    sc.gl_from[k][SC_GL_ACT] = sc.a_act[k];
    sc.gl_t0[k] = fm1_ms; sc.gl_on[k] = 1;
}
static int32_t sc_gl(uint32_t k, uint32_t q, int32_t to)   /* what to send now: to, or the glide's way to it */
{
    int32_t f = sc.gl_from[k][q];
    uint32_t t = fm1_ms - sc.gl_t0[k];
    if (!sc.gl_on[k] || f < 0 || t >= SC_GLIDE_MS) return to;
    return f + (to - f) * (int32_t)t / (int32_t)SC_GLIDE_MS;   /* (|to - f| < 2^16: 32 bits) */
}

/* the instance lane k plays has another sound than family f's sound s on engine e (loaded: a switch, a fresh one) */
static int sc_swaps(uint32_t k, const ls_sound_t *s, const tx_engine_t *e)
{
    int p;
    if (e != sc.a_eng[k]) return 1;
    if (s->eng || sc.a_preset[k] < 0) return 0;
    p = sc_preset_index(s->src);
    return (p >= 0 && p != sc.a_preset[k]) || s->type != sc.a_type[k];
}

/* ---- what a lane's engine is sent: only what differs from the last time */
static void sc_sync(uint32_t k)
{
    int src, gen, held;
    uint32_t i;
    int32_t w;
    if (sc.mute[k] != sc.a_gate[k] && ln_gate(k, !sc.mute[k]) >= 0) sc.a_gate[k] = sc.mute[k];   /* (the mix's, any engine) */
    if (sc.eng[k] != sc.a_eng[k] || sc.reswap[k]) {   /* another engine (or sound): the old one's notes go first */
        if (!ln_can_select(k, sc.eng[k])) {     /* the last switch still rings: the old engine plays on till its turn, */
            ln_hurry();                         /* that ring fades now (never cut); next pass */
            return;
        }
        if (sc.a_held[k]) ln_hurry();           /* the old engine still held: it keeps its key and fades out playing
                                                 * (TX_RING_FADE, 151 ms: a crossfade); a released one rings out */
        sc.a_held[k] = 0;                       /* (the new one gets its own key) */
        if (sc.a_gen[k]) { ln_gen_on(k, 0, 0); sc.a_gen[k] = 0; }
        if (ln_select(k, sc.eng[k], tx_seed()) < 0) return;   /* (a full queue: next pass) */
        sc.a_eng[k] = sc.eng[k]; sc.reswap[k] = 0; sc.nsel[k]++; sc.sel_t[k] = fm1_ms;
        sc.a_preset[k] = sc.a_type[k] = sc.a_count[k] = sc.a_level[k] = sc.a_act[k] = -1;
        sc_glide(k);                            /* (the fresh engine's own settings at once; the lane's trim, pan:
                                                 * the ring under it hears them too, so they glide) */
        for (i = 0; i < 3u; i++) {              /* the classic engine's QUICK parameters */
            sc.q_id[k][i] = (int8_t)(sc.q[k] && i < sc.q[k]->np ? tx_param_find(sc.eng[k], sc.q[k]->p[i].name) : -1);
            sc.q_cur[k][i] = -(1 << 30);
        }
    }
    if (!sc.eng[k]) return;
    src = sc_is_src(k);
    if (src && sc.preset[k] != sc.a_preset[k]) { ln_param(k, (uint32_t)sc.id_preset, sc.preset[k]); sc.a_preset[k] = sc.preset[k]; }
    if (src && sc.id_type >= 0 && sc.type[k] != sc.a_type[k]) { ln_param(k, (uint32_t)sc.id_type, sc.type[k]); sc.a_type[k] = sc.type[k]; }
    if (src && sc.id_count >= 0 && (w = sc_gl(k, SC_GL_COUNT, LS_COUNT_V[sc.cnt[k]])) != sc.a_count[k]) {
        ln_param(k, (uint32_t)sc.id_count, (uint32_t)w); sc.a_count[k] = (int16_t)w;
    }
    if ((w = sc_gl(k, SC_GL_LEVEL, (int32_t)sc_vol_of(k))) != sc.a_level[k]) { ln_level(k, (uint32_t)w); sc.a_level[k] = (int16_t)w; }
    if ((w = sc_gl(k, SC_GL_PAN, sc.pan[k])) != sc.a_pan[k]) { ln_pan(k, (uint32_t)w); sc.a_pan[k] = (int16_t)w; }
    {
        int32_t t = sc_gl(k, SC_GL_TRIM, sc.snd[k] >= 0 ? (int32_t)LS[sc.lf[k]].s[sc.snd[k]].trim : 4096);
        if (t != sc.a_trim[k] && ln_trim(k, (uint32_t)t) >= 0) sc.a_trim[k] = t;
    }
    gen = sc.run[k] && sc.notes[k];             /* the generator: on while the lane runs (Notes on) */
    if (gen && (w = sc_gl(k, SC_GL_ACT, (int32_t)sc_act_val(sc.step[k]))) != sc.a_act[k]) {
        ln_gen_param(k, TX_GEN_ACTIVITY, (uint32_t)w); sc.a_act[k] = (int16_t)w;
    }
    if ((uint8_t)!src != sc.a_mute[k]) { ln_gen_mute(k, !src); sc.a_mute[k] = (uint8_t)!src; }   /* a classic engine: no notes */
    if (gen != sc.a_gen[k]) { ln_gen_on(k, (uint32_t)gen, tx_seed() | 1u); sc.a_gen[k] = (uint8_t)gen; }
    if (src && !sc.notes[k] && sc.run[k]) {     /* Notes off: the fader is the source's activity itself */
        int id = ln_act_id(sc.src);
        if (id >= 0) ln_param(k, (uint32_t)id, sc_act_val(sc.step[k]));
    }
    held = sc.run[k] && !sc_oneshot(k);         /* the source, a held classic engine: key 60 down while it runs */
    if (held != sc.a_held[k]) { ln_key(k, SC_HOLD_NOTE, held ? SC_HOLD_VEL : 0u, (uint32_t)held); sc.a_held[k] = (uint8_t)held; }
}

/* ---- a classic engine's activity: its QUICK parameters glide toward the place HOW MANY and HOW ACTIVE give */
static uint32_t sc_quick_pos(uint32_t k)        /* 0 .. 65536 */
{
    uint32_t c = sc.cnt[k] > 1u ? (sc.cnt[k] - 1u) * 65536u / (LS_NCNT - 2u) : 0u, a, x;
    if (sc.notes[k]) a = (uint32_t)ln_gen_activity(k) * 65536u / 254u;   /* the generator's wander (/ 127, x 0.5) */
    else a = sc.step[k] * 65536u / (2u * SC_STEPS);
    x = c / 2u + a;
    return x > 65536u ? 65536u : x;
}
static void sc_quick_tick(uint32_t k, int jump)
{
    const ls_quick_t *q = sc.q[k];
    uint32_t x = sc_quick_pos(k), i;
    if (!q || sc.a_eng[k] != sc.eng[k]) return;
    for (i = 0; i < q->np; i++) {
        int32_t t = q->p[i].lo * 256 + (int32_t)(((q->p[i].hi - q->p[i].lo) * (int32_t)x) >> 8);   /* Q8 */
        int32_t now = sc.q_cur[k][i], nx, d;
        if (now == -(1 << 30) || jump) now = t;
        d = t - now;
        nx = d > -128 && d < 128 ? t : now + ((d * 13107) >> 15);   /* 0.4 of the way */
        if (sc.q_id[k][i] >= 0 && ((nx + 128) >> 8 != (now + 128) >> 8 || sc.q_cur[k][i] == -(1 << 30) || jump))
            ln_param(k, (uint32_t)sc.q_id[k][i], (uint32_t)((nx + 128) >> 8));
        sc.q_cur[k][i] = nx;
    }
}
/* a one-shot engine's next press: an exponential gap around the mean at the place (C4's gap .. C6's; the press rate
 * ra + (rb - ra) x^2.5 between them: each step sounds the same more, tx_core_lin.h) */
static uint32_t sc_quick_gap(uint32_t k)
{
    const ls_quick_t *q = sc.q[k];
    uint32_t x = sc_quick_pos(k), mean, nl, b, ra, rb, r;
    b = (q->every_ms * (tx_exp2(q->every_oct_q16 + (16 << 16)) >> 4)) >> 12;   /* C6's gap, ms */
    ra = 10000000u / (q->every_ms ? q->every_ms : 1u); rb = 10000000u / (b ? b : 1u);   /* presses per 10^4 s */
    r = ra + (uint32_t)(((int32_t)(rb - ra) * (int32_t)(tx_lin_pow(x) >> 4)) >> 12);
    mean = 10000000u / (r ? r : 1u);                         /* ms */
    nl = (tx_neglog2_q12(tx_rand(&sc.rng)) * 2839u) >> 12;           /* -ln u, Q12 */
    mean = ((mean >> 4) * nl) >> 8;
    return mean < SC_PRESS_MS ? SC_PRESS_MS : mean > 600000u ? 600000u : mean;
}

/* ---- the player's lane actions */
static void sc_start(uint32_t k, int on)        /* laneStart: the lane plays on its own, or stops (tails ring) */
{
    sc.run[k] = (uint8_t)(on != 0);
    sc.q_next[k] = 0;
    if (on && !sc_is_src(k)) sc_quick_tick(k, 1);   /* (a classic engine: its settings first, no glide) */
    if (on && sc_oneshot(k)) {                  /* (its first strike after a gap: within ~2.5 s, so the rarest step */
        uint32_t g = sc_quick_gap(k);           /* is never silent) */
        if (g > SC_PRESS_MS + 2500u) g = SC_PRESS_MS + tx_rand(&sc.rng) % 2500u;
        sc.q_next[k] = fm1_ms + g + 1u;
    }
    sc_sync(k);
}
static void sc_step(uint32_t k, uint32_t w);
static void sc_count(uint32_t k, uint32_t c)    /* HOW MANY: 0 off (new events stop, the tails ring), 1 .. 6 */
{
    if (k >= SC_N) return;
    sc.cnt[k] = (uint8_t)(c < LS_NCNT ? c : LS_NCNT - 1u);
    if (!c) { if (sc.run[k]) sc_start(k, 0); else sc_sync(k); return; }
    if (!sc.run[k]) sc_step(k, sc_act_step(0)); /* (started from off: rare, so the first key is 1 on both) */
    else sc_sync(k);
}
static void sc_step(uint32_t k, uint32_t w)     /* HOW ACTIVE as a step 1 .. 14 (0: off) */
{
    if (k >= SC_N) return;
    if (!w) { sc_count(k, 0); return; }
    if (!sc.cnt[k]) sc.cnt[k] = 1;              /* a sound off: the activity keys start it with one */
    sc.step[k] = (uint8_t)(w > SC_STEPS ? SC_STEPS : w);
    if (!sc.run[k]) sc_start(k, 1);
    else sc_sync(k);
}
static void sc_load(uint32_t k, uint32_t f, uint32_t i)   /* soundLoad: family f's sound i into lane k, stopped */
{
    const ls_sound_t *s;
    const tx_engine_t *e;
    int sw;
    if (f >= LS_N || i >= LS[f].n) return;
    s = &LS[f].s[i];
    e = s->eng ? tx_engine_find(s->src) : sc.src;
    if (!e) return;
    sw = sc_swaps(k, s, e);
    if (sc.run[k]) {
        if (sw) { sc.run[k] = 0; sc.q_next[k] = 0; }   /* (a switch: the old engine plays on till it rings out) */
        else sc_start(k, 0);
    }
    sc.step[k] = 0; sc.cnt[k] = 0;
    sc.eng[k] = e;
    if (!s->eng) {
        int p = sc_preset_index(s->src);
        if (p >= 0) sc.preset[k] = (uint8_t)p;
        sc.type[k] = s->type;
    }
    sc.reswap[k] = (uint8_t)(sw && e == sc.a_eng[k]);   /* (the source playing another sound: a fresh one, swapped) */
    sc.q[k] = sc_is_src(k) ? 0 : sc_quick(sc.eng[k]);
    sc.snd[k] = (int8_t)i; sc.lf[k] = (uint8_t)f;
    sc_sync(k);
}
/* PLAY mode: the keys play the lane's engine, so a lane at HOW MANY off gets one (its COUNT) without starting */
static void sc_play_ready(uint32_t k)
{
    if (k < SC_N && !sc.cnt[k]) { sc.cnt[k] = 1; sc_sync(k); }
}
static void sc_select(uint32_t k) { if (k < SC_N) sc.sel = (uint8_t)k; }
/* the lane's sound set to its family's sound i (a CC, the SOUND card): playing, it goes on playing as it was; a sound
 * already on another lane stays there (no change) */
static void sc_sound_to(uint32_t k, uint32_t i)
{
    uint32_t j, f;
    uint8_t run, c, st;
    if (k >= SC_N) return;
    f = sc.lf[k];
    if (i >= LS[f].n || sc.snd[k] == (int)i) return;
    for (j = 0; j < SC_N; j++) if (sc.snd[j] == (int)i && sc.lf[j] == f) return;
    run = sc.run[k]; c = sc.cnt[k]; st = sc.step[k];
    sc_load(k, f, i);
    if (run) { sc_count(k, c); sc_step(k, st); }
}
/* the sound browser's pick (and its preview): family f's sound i on lane k, a sound on another lane too (each lane its
 * own instance); playing, it goes on playing as it was */
static void sc_sound_set(uint32_t k, uint32_t f, uint32_t i)
{
    uint8_t run, c, st;
    if (k >= SC_N || f >= LS_N || i >= LS[f].n || (sc.snd[k] == (int)i && sc.lf[k] == f)) return;
    run = sc.run[k]; c = sc.cnt[k]; st = sc.step[k];
    sc_load(k, f, i);
    if (run) { sc_count(k, c); sc_step(k, st); }
}
static void sc_volume(uint32_t k, uint32_t v)
{
    if (k >= SC_N || sc.snd[k] < 0) return;
    sc.vol[sc.lf[k]][sc.snd[k]] = (uint8_t)(v > 127u ? 127u : v);
    sc_sync(k);
}
/* a black key held: the lane muted / unmuted. Only its output: it goes on playing, so it comes back at once (a short
 * ramp, tx_mix_gate); a scene keeps it, a new landscape clears it */
static void sc_mute(uint32_t k, int on) { if (k < SC_N) { sc.mute[k] = (uint8_t)(on != 0); sc_sync(k); } }
static void sc_pan(uint32_t k, uint32_t v) { if (k < SC_N) { sc.pan[k] = (uint8_t)(v > 127u ? 127u : v); sc_sync(k); } }
static void sc_family(uint32_t f)               /* setFamily: every lane stopped, the landscape's first sounds loaded */
{
    uint32_t k;
    sc.fam = (uint8_t)(f % LS_N);
    sc.fam_want = sc.fam; sc.fam_pend = 0;
    for (k = 0; k < SC_N; k++) { if (sc.run[k]) sc_start(k, 0); sc.snd[k] = -1; sc.mem_cnt[k] = 0; sc_mute(k, 0); }
    for (k = 0; k < SC_N; k++) sc.lf[k] = sc.fam;
    for (k = 0; k < SC_N && k < LS[sc.fam].n; k++) sc_load(k, sc.fam, k);
    sc.sel = 0;
}
static void sc_start_landscape(void)            /* the first three sounds at once, low: 2 / 1 / 1 of them, rare */
{
    static const uint8_t C[3] = {2, 1, 1};
    uint32_t k;
    for (k = 0; k < 3u; k++) if (sc.snd[k] >= 0) { sc_count(k, C[k]); sc_step(k, sc_act_step(0)); }
    sc.sel = 0;
}
static int sc_any_run(void) { uint32_t k; for (k = 0; k < SC_N; k++) if (sc.run[k]) return 1; return 0; }
static void sc_all_toggle(void)                 /* REC: all lanes stop (remembered) / start again (or the landscape's start) */
{
    uint32_t k;
    int mem = 0;
    if (sc_any_run()) {
        for (k = 0; k < SC_N; k++) {
            sc.mem_cnt[k] = (uint8_t)sc_cnt_of(k); sc.mem_step[k] = (uint8_t)sc_step_of(k);
            if (sc.run[k]) sc_start(k, 0);
        }
        return;
    }
    for (k = 0; k < SC_N; k++) if (sc.mem_cnt[k]) { mem = 1; sc_count(k, sc.mem_cnt[k]); sc_step(k, sc.mem_step[k]); }
    if (!mem) sc_start_landscape();
}
/* ALGORITHM: another landscape; what played goes on playing (the new landscape's start), quiet stays quiet.
 * The detents gather for SC_FAM_MS so a fast turn loads one landscape, not each it passes */
static void sc_family_turn(int32_t d)
{
    int32_t f = (int32_t)sc.fam_want + d % (int32_t)LS_N + (int32_t)LS_N;
    sc.fam_want = (uint8_t)(f % (int32_t)LS_N);
    sc.fam_pend = 1; sc.fam_t = fm1_ms;
}
static void sc_family_go(void)
{
    int play = sc_any_run();
    sc_family(sc.fam_want);
    if (play) sc_start_landscape();
}

static void sc_init(void)
{
    uint32_t k, f, i;
    sc.src = tx_engine_find("source");
    sc.id_preset = (int8_t)(sc.src ? tx_param_find(sc.src, "PRESET") : -1);
    sc.id_type = (int8_t)(sc.src ? tx_param_find(sc.src, "TYPE") : -1);
    sc.id_count = (int8_t)(sc.src ? tx_param_find(sc.src, "COUNT") : -1);
    tx_rng_seed(&sc.rng, tx_seed());
    for (f = 0; f < LS_N; f++) for (i = 0; i < LS_SND_MAX; i++) sc.vol[f][i] = 100;
    for (k = 0; k < SC_N; k++) {
        sc.snd[k] = -1; sc.notes[k] = 1; sc.pan[k] = 64;
        sc.a_preset[k] = sc.a_type[k] = sc.a_count[k] = sc.a_act[k] = sc.a_level[k] = sc.a_pan[k] = -1;
        sc.a_trim[k] = -1;
    }
    sc_family(0);
}
static void sc_poll(void)
{
    uint32_t k;
    if (sc.fam_pend && fm1_ms - sc.fam_t >= SC_FAM_MS) sc_family_go();
    for (k = 0; k < SC_N; k++) {
        if (sc.a_eng[k] != sc.eng[k] || sc.reswap[k] || sc.a_gate[k] != sc.mute[k]) sc_sync(k);   /* a switch (a mute) that had to wait */
        if (sc.q_rel[k] && (int32_t)(fm1_ms - sc.q_rel[k]) >= 0) { ln_key(k, SC_HOLD_NOTE, 0, 0); sc.q_rel[k] = 0; }
        if (sc.run[k] && sc.q_next[k] && sc_oneshot(k) && sc.a_eng[k] == sc.eng[k] && (int32_t)(fm1_ms - sc.q_next[k]) >= 0) {
            ln_key(k, SC_HOLD_NOTE, SC_HOLD_VEL, 1);   /* quickPress */
            sc.q_rel[k] = fm1_ms + SC_PRESS_MS; if (!sc.q_rel[k]) sc.q_rel[k] = 1;
            sc.q_next[k] = fm1_ms + sc_quick_gap(k); if (!sc.q_next[k]) sc.q_next[k] = 1;
        }
    }
    if (fm1_ms - sc.gl_tick >= SC_GLIDE_TICK) {   /* the glides' next step (and the last: the value wanted) */
        sc.gl_tick = fm1_ms;
        for (k = 0; k < SC_N; k++) {
            int end;
            if (!sc.gl_on[k]) continue;
            end = fm1_ms - sc.gl_t0[k] >= SC_GLIDE_MS;
            sc_sync(k);
            if (end && sc.a_eng[k] == sc.eng[k] && !sc.reswap[k]) sc.gl_on[k] = 0;
        }
    }
    if (fm1_ms - sc.q_t >= SC_QUICK_MS) {
        sc.q_t = fm1_ms;
        for (k = 0; k < SC_N; k++) if (sc.run[k] && !sc_is_src(k)) sc_quick_tick(k, 0);
    }
}
