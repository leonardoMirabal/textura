/* SPDX-License-Identifier: GPL-3.0-only */
/* txactivity: the lane fader's range, sound by sound, driven the way the player drives a lane (audition/app.js lane
 * mode, audition/tx_web.c): every sound of every landscape (FAMILIES: a source preset + its TYPE, or a classic engine
 * through the QUICK mapping), its lane started at an ACTIVITY step with the generator on (tx_core_gen: INTENSITY each
 * block for the source, its notes pressed as keys into any engine), the source held (key 60, velocity 100) or the
 * classic engine held / re-pressed as the player's quick lanes do (quickTick every 50 ms, quickPress's exponential
 * gaps). Through the mixer (centre pan, the output limiter), as the player renders a lane.
 *
 * Per sound, COUNT and ACTIVITY (the player's two faders; by default COUNT 1, 8, 64 individuals against ACTIVITY 0 = the
 * rarest step, 9, 64, 127), SEEDS runs of SECS s after a 5 s start, each a new engine and generator seed. COUNT is given in
 * individuals and set as the source's COUNT knob (1 + 21 log2 n: 1, 8, 64 = knob 1, 64, 127; 0 = none); a classic
 * engine has no COUNT (its column reads "-" and its runs ignore it):
 *   ev/s     the engine's own event count (tx_stats_t.total) per second
 *   bout/s   sound starting after 200 ms or more of gap (as gaps: 20 dB under the mean), per second: a bout of many grains (a
 *            gurgle of bubbles) is one; the lone verdict takes the fewer of ev/s and bout/s
 *   LUFS-S   short-term loudness (BS.1770 K-weighting, 3 s windows every 100 ms; stereo out), its 5th .. 95th percentile
 *   heroes   mean heroes sounding / the PARTICLES limit; drop/s: events with no hero free (or over the block's cap)
 *   gaps     the share of 10 ms frames more than 20 dB under the run's mean power (a cloud has none)
 *   env      the spread of the 10 ms frames' level (dB, standard deviation): separate events read high, a cloud low
 *   us       the lane's cost per block on the FM-1 (host time x the calibration, the mean; w8: the worst 8-block mean)
 *   cut      the player's governor (tx_core_gov, budget 3193 us) on this lane alone: heroes it took away (the most of
 *            the seeds)
 * and the verdict, on the grid's two corners:
 *   the fewest and least active (the smallest COUNT at the lowest ACTIVITY) should be a lone event now and then:
 *     LONE (at ACTIVITY 0: <= 0.5 events/s and >= 80 % gaps; at a higher lowest ACTIVITY: <= 2 /s and >= 50 %), else BUSY;
 *   the most and most active (the largest COUNT at ACTIVITY 127) should be a cloud:
 *     CLOUD (dense: >= 15 events/s, no gaps, env <= 6 dB), DENSE (one of them short), EVENTS.
 * A last line per sound gives both corners (lone / cloud) and "ok" when both hold.
 *
 * With -o, the lane's start instead (no 5 s warm-up): a sound switched on must not open louder than it goes on. Per run,
 * momentary loudness (BS.1770, 400 ms every 100 ms): the loudest in the first second against, over the rest of the
 * run, the 95th percentile of all moments and of the sounding ones (over -60 LUFS) and the loudest; LOUD when the seeds'
 * median first second is over 1 dB above the 95th percentile of its sounding moments (a strike at the press: every
 * start; one natural event early in one seed is not), else ok; t1, when it first sounds (a sound switched on should answer
 * within a few seconds). A classic one-shot lane's first strike comes after a random gap, as the player's quickNext.
 *
 *   txactivity [-o] [-s SECS] [-n SEEDS] [-c N,N,..] [-a A,B,..] [-f SUBSTRING] [-p PARTICLES]
 *            (defaults 30 s, 3 seeds, COUNT 1,8,64, ACTIVITY 0,9,64,127, every sound; -c: COUNT in individuals, 0 = none;
 *            -p: the lane's PARTICLES set (the engine's maximum at most) instead of its default: does a bigger hero
 *            pool make the top a cloud?)
 * Timing (us, gov) needs the calibration (build/calib/run, make tx-calib); without it those columns read "-". */
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tx_host.h"
#include "tx_core_gen.h"
#include "tx_core_gov.h"
#include "tx_core_mix.h"

#define FM1_WORST_US 3620.0         /* Frodhes' worst scene on the FM-1 (txbench / txbudget's calibration) */
#define FR_SKIP_BLOCKS 20u
#define WARM_S 5u
#define GOV_BUDGET 3193u            /* the player's governor budget (tx_web.c gov_init) */
#define MAXA 8u

/* ---- the landscapes (audition/app.js FAMILIES): name, source preset ("#engine": a classic engine), TYPE */
typedef struct { const char *name, *pre; unsigned type; } snd_t;
static const snd_t SND[] = {
    {"rain on water", "rain on water", 0}, {"rain on a roof", "rain on water", 64}, {"hail", "rain on water", 110}, {"water drops", "water", 0},
    {"cave drips", "water", 16}, {"eaves", "water", 32}, {"brook", "water", 48}, {"spring", "water", 64},
    {"waterfall", "water", 80}, {"waves on sand", "water", 96}, {"waves on shingle", "water", 112},
    {"stones", "stone cloud", 0}, {"rockfall", "stone cloud", 32}, {"ringing rocks", "stone cloud", 96},
    {"gravel", "gravel", 0}, {"marbles", "gravel", 96}, {"sand", "sand", 0}, {"singing dune", "sand", 96},
    {"minerals", "minerals", 0}, {"stone friction", "minerals", 96},
    {"ice", "ice", 0}, {"lake ice", "ice", 96}, {"glass", "glass", 0},
    {"fire", "fire", 0}, {"flame", "fire", 96}, {"wood", "wood", 0},
    {"wind", "wind", 0}, {"thunder", "#thunder", 0},
    {"crickets", "crickets", 0}, {"cicadas", "cicadas", 0}, {"katydids", "katydids", 0}, {"bees", "bees", 0},
    {"songbird", "songbird", 0}, {"blackbird", "blackbird", 0}, {"song thrush", "song thrush", 0},
    {"nightingale", "nightingale", 0}, {"robin", "robin", 0}, {"wren", "wren", 0}, {"hermit thrush", "hermit thrush", 0},
    {"skylark", "skylark", 0}, {"wood thrush", "wood thrush", 0}, {"reed warbler", "reed warbler", 0},
    {"chirper", "chirper", 0}, {"crow", "crow", 0}, {"gull", "gull", 0}, {"curlew", "curlew", 0},
    {"oystercatcher", "oystercatcher", 0}, {"cuckoo", "cuckoo", 0}, {"woodpecker", "woodpecker", 0},
    {"wood pigeon", "wood pigeon", 0}, {"nightjar", "nightjar", 0}, {"bittern", "bittern", 0},
};
#define NSND (sizeof SND / sizeof SND[0])

/* ---- the QUICK mapping (audition/app.js QUICK) for the classic engines in the landscapes */
typedef struct { const char *eng; unsigned np; struct { const char *name; double lo, hi; } p[3]; double every[2]; } quick_t;
static const quick_t QUICK[] = {
    {"stream", 2, {{"FLOW", 10, 127}, {"ROAR", 0, 90}}, {0, 0}},
    {"surf", 2, {{"HEIGHT", 10, 127}, {"CHOP", 30, 90}}, {0, 0}},
    {"thunder", 2, {{"DIST", 110, 25}, {"ENERGY", 40, 100}}, {600, 10}},
};

static double factor;                                     /* FM-1 us per host us (0: no calibration) */
static unsigned count_knob = 64;                         /* the run's COUNT knob (count_of: individuals -> knob) */
static unsigned particles;                                /* -p: the lane's PARTICLES (0: the engine's default) */
static int onset;                                         /* -o: the lane's start measured (no warm-up) */
static int calib(const char *dir)                         /* (txbudget's) */
{
    DIR *d = opendir(dir);
    struct dirent *de;
    double worst = 0;
    unsigned n = 0;
    static uint64_t v[64][40000];
    uint32_t nb = 0xFFFFFFFFu, b;
    if (!d) return -1;
    while ((de = readdir(d)) && n < 64) {
        char path[600];
        FILE *f;
        unsigned long x;
        uint32_t k = 0;
        if (de->d_name[0] != 'w' || !strstr(de->d_name, ".txt")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (!(f = fopen(path, "r"))) continue;
        while (k < 40000 && fscanf(f, "%lu", &x) == 1) v[n][k++] = x;
        fclose(f);
        if (k < nb) nb = k;
        n++;
    }
    closedir(d);
    if (n < 3 || nb <= FR_SKIP_BLOCKS) return -1;
    for (b = FR_SKIP_BLOCKS; b < nb; b++) {
        uint64_t m = v[0][b];
        unsigned r;
        for (r = 1; r < n; r++) if (v[r][b] < m) m = v[r][b];
        if (m / 1000.0 > worst) worst = m / 1000.0;
    }
    factor = FM1_WORST_US / worst;
    return 0;
}

/* ---- BS.1770 K-weighting at 44.1 kHz (the shelf and the RLB high-pass, the standard's analog designs) */
typedef struct { double b0, b1, b2, a1, a2, x1, x2, y1, y2; } bq_t;
static double bq(bq_t *f, double x)
{
    double y = f->b0 * x + f->b1 * f->x1 + f->b2 * f->x2 - f->a1 * f->y1 - f->a2 * f->y2;
    f->x2 = f->x1; f->x1 = x; f->y2 = f->y1; f->y1 = y;
    return y;
}
static void kw_init(bq_t *s, bq_t *h)
{
    double fs = 44100.0, K = tan(M_PI * 1681.974450955533 / fs), Q = 0.7071752369554196, Vh = pow(10.0, 3.999843853973347 / 20);
    double Vb = pow(Vh, 0.4996667741545416), a0 = 1 + K / Q + K * K;
    memset(s, 0, sizeof *s); memset(h, 0, sizeof *h);
    s->b0 = (Vh + Vb * K / Q + K * K) / a0; s->b1 = 2 * (K * K - Vh) / a0; s->b2 = (Vh - Vb * K / Q + K * K) / a0;
    s->a1 = 2 * (K * K - 1) / a0; s->a2 = (1 - K / Q + K * K) / a0;
    K = tan(M_PI * 38.13547087602444 / fs); Q = 0.5003270373238773; a0 = 1 + K / Q + K * K;
    h->b0 = 1; h->b1 = -2; h->b2 = 1; h->a1 = 2 * (K * K - 1) / a0; h->a2 = (1 - K / Q + K * K) / a0;
}

static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

typedef struct {
    double evs, bouts, lu5, lu50, lu95, heroes, lim, drops, gaps, env, us, w8;
    double m1, mp95, mpa, mmax, t1;          /* -o: momentary loudness (400 ms): the loudest in the first second; after it, the
                                     * 95th percentile, that of the moments something sounds (over -60 LUFS), the loudest */
    int cut;                        /* heroes the governor took away (-1: no timing) */
} res_t;

static int preset_index(const tx_engine_t *e, const char *name)    /* "0 rain on water | 1 stone cloud | ..." */
{
    int id = tx_param_find(e, "PRESET");
    const char *h = id >= 0 ? e->params[id].help : 0, *p;
    size_t n = strlen(name);
    if (!h) return -1;
    for (p = strchr(h, ':'); p && *p; p++) {
        int k;
        char *q;
        if (*p < '0' || *p > '9') continue;
        k = (int)strtol(p, &q, 10);
        while (*q == ' ') q++;
        if (!strncmp(q, name, n) && (q[n] == ' ' || q[n] == 0 || q[n] == '|')) return k;
        p = q;
    }
    return -1;
}

/* one run: sound s at ACTIVITY act, seed; secs measured after WARM_S */
static int run(const snd_t *s, unsigned act, uint32_t seed, unsigned secs, res_t *r)
{
    static uint8_t arena[TX_STATE_MAX] __attribute__((aligned(64)));
    static tx_mix_t mx;
    static tx_gen_t gen;
    const tx_engine_t *e;
    const quick_t *q = 0;
    tx_gov_t gov;
    tx_stats_t stt;
    bq_t ks[2], kh[2];
    double cur[3] = {0, 0, 0}, tick = 0, nextp = -1, relp = -1, t = 0;
    int curset[3] = {0, 0, 0}, pid[3] = {-1, -1, -1}, iid = -1;
    uint32_t warm = onset ? 0u : WARM_S, nb = (warm + secs) * TX_FS / TX_BLOCK, w0 = warm * TX_FS / TX_BLOCK, b, total0 = 0, drop = 0;
    uint32_t nfr = 0, nst = 0, rng = seed * 2654435761u + 12345u;
    int32_t L[TX_BLOCK], R[TX_BLOCK];
    double *fr = malloc(sizeof(double) * (secs * 100u + 8u)), *st = malloc(sizeof(double) * (secs * 10u + 8u));
    double *cost = calloc(nb + 1u, sizeof(double));
    double facc = 0, sumh = 0, sumc = 0, w8 = 0;
    unsigned fn = 0;
    unsigned k;
    if (!fr || !st || !cost) return -1;
    if (s->pre[0] == '#') {
        e = tx_engine_find(s->pre + 1);
        for (k = 0; k < sizeof QUICK / sizeof QUICK[0]; k++) if (!strcmp(QUICK[k].eng, s->pre + 1)) q = &QUICK[k];
    } else e = tx_engine_find("source");
    if (!e || e->state_bytes > TX_STATE_MAX) return -1;
    tx_mix_init(&mx);
    memset(arena, 0, sizeof arena);
    tx_mix_set(&mx, 0, e, arena, seed);
    if (!q) {
        int p = preset_index(e, s->pre), ti = tx_param_find(e, "TYPE");
        if (p < 0) { fprintf(stderr, "no preset '%s'\n", s->pre); return -1; }
        e->param(arena, (uint8_t)tx_param_find(e, "PRESET"), (uint8_t)p);
        if (ti >= 0) e->param(arena, (uint8_t)ti, (uint8_t)s->type);
        ti = tx_param_find(e, "COUNT");
        if (ti >= 0) e->param(arena, (uint8_t)ti, (uint8_t)count_knob);
        iid = tx_param_find(e, "INTENSITY");
    } else
        for (k = 0; k < q->np; k++) pid[k] = tx_param_find(e, q->p[k].name);
    tx_gen_init(&gen, seed);
    tx_gen_param(&gen, TX_GEN_ACTIVITY, act);
    tx_gov_init(&gov, TX_MIX_SLOTS, GOV_BUDGET);
    {
        unsigned pp = particles ? (particles < e->particles_max ? particles : e->particles_max) : e->particles_def;
        if (particles) e->param(arena, TX_PID_PARTICLES, (uint8_t)pp);
        tx_gov_user(&gov, 0, pp);
    }
    if (!q || !q->every[0]) e->key(arena, 60, 100, 1);    /* the lane held (srcHold) */
    kw_init(&ks[0], &kh[0]); kw_init(&ks[1], &kh[1]);
    for (b = 0; b < nb; b++) {
        tx_gen_ev_t ev[TX_GEN_EV_MAX];
        uint8_t in;
        unsigned ne, j;
        uint64_t t0;
        double dt;
        t += (double)TX_BLOCK / TX_FS;
        if (q) {                                          /* the player's quick lane: its 50 ms tick, its presses */
            double step = (gen.a >> 8) * 14.0 / 127.0;    /* (quickTick: the generator's activity, read each tick) */
            if (t >= tick) {
                tick += 0.05;
                for (k = 0; k < q->np; k++) {
                    double tg = q->p[k].lo + (q->p[k].hi - q->p[k].lo) * step / 14.0;
                    double now = curset[k] ? cur[k] : tg, nx = fabs(tg - now) < 0.5 ? tg : now + (tg - now) * 0.15;
                    if ((!curset[k] || lround(nx) != lround(now)) && pid[k] >= 0) e->param(arena, (uint8_t)pid[k], (uint8_t)lround(nx));
                    cur[k] = nx; curset[k] = 1;
                }
            }
            if (q->every[0]) {
                if (nextp < 0 || t >= nextp) {            /* (the lane's first strike after a gap of its own, as
                                                     * the player's quickNext: never at the start) */
                    double mean = q->every[0] * pow(q->every[1] / q->every[0], step / 14.0), u;
                    rng = rng * 1664525u + 1013904223u;
                    u = ((rng >> 8) + 0.5) / 16777216.0;
                    if (nextp >= 0) { e->key(arena, 60, 100, 1); relp = t + 0.15; }
                    nextp = t + fmax(0.15, -log(1 - u) * mean);
                }
                if (relp >= 0 && t >= relp) { e->key(arena, 60, 0, 0); relp = -1; }
            }
        }
        tx_mix_begin(&mx, TX_BLOCK);
        t0 = tx_now_ns();
        ne = tx_gen_block(&gen, TX_BLOCK, ev, TX_GEN_EV_MAX, &in);
        if (iid >= 0) e->param(arena, (uint8_t)iid, in);
        for (j = 0; j < ne && !q; j++) e->key(arena, ev[j].note, ev[j].vel, ev[j].vel != 0);   /* (a classic lane: no notes) */
        tx_mix_slot_render(&mx, 0, TX_BLOCK);
        dt = (tx_now_ns() - t0) / 1000.0;
        tx_mix_end(&mx, L, R, TX_BLOCK);
        e->stats(arena, &stt);
        cost[b] = dt * factor;
        if (factor > 0) {                                 /* the governor on this lane alone */
            uint32_t c[TX_MIX_SLOTS] = {0}, lv[TX_MIX_SLOTS] = {0};
            uint8_t on[TX_MIX_SLOTS] = {1, 0, 0, 0};
            int pk;
            c[0] = (uint32_t)cost[b]; lv[0] = (uint32_t)mx.slot_peak[0];
            mx.slot_peak[0] = 0;
            pk = tx_gov_block(&gov, c, lv, on);
            if (pk == 0) e->param(arena, TX_PID_PARTICLES, gov.cur[0]);
        }
        if (b == w0) total0 = stt.total;
        if (b < w0) continue;
        drop += stt.dropped;
        sumh += stt.particles;
        sumc += cost[b];
        if (b >= w0 + 7u) {
            double a = 0;
            for (k = 0; k < 8; k++) a += cost[b - k];
            if (a / 8 > w8) w8 = a / 8;
        }
        for (j = 0; j < TX_BLOCK; j++) {                  /* K-weighted power, 10 ms frames */
            double xl = L[j] / 8388608.0, xr = R[j] / 8388608.0, yl = bq(&kh[0], bq(&ks[0], xl)), yr = bq(&kh[1], bq(&ks[1], xr));
            facc += yl * yl + yr * yr;
            if (++fn == 441u) { fr[nfr++] = facc / 441.0; facc = 0; fn = 0; }
        }
    }
    {   /* short-term loudness: 3 s (300 frames) every 100 ms (10 frames) */
        uint32_t i, j;
        for (i = 300; i <= nfr; i += 10) {
            double a = 0;
            for (j = i - 300; j < i; j++) a += fr[j];
            a /= 300;
            st[nst++] = a > 1e-12 ? -0.691 + 10 * log10(a) : -120;
        }
        if (onset) {                                  /* momentary: 400 ms (40 frames) every 100 ms */
            double *mo = malloc(sizeof(double) * (nfr / 10u + 8u));
            uint32_t nm = 0;
            r->m1 = r->mmax = -120; r->t1 = -1;
            for (i = 40; i <= nfr && mo; i += 10) {
                double a = 0, l;
                for (j = i - 40; j < i; j++) a += fr[j];
                l = a / 40 > 1e-12 ? -0.691 + 10 * log10(a / 40) : -120;
                if (r->t1 < 0 && l > -60) r->t1 = i / 100.0;   /* (the first moment sounding: its window's end) */
                if (i <= 100) { if (l > r->m1) r->m1 = l; }
                else if (i >= 140) { mo[nm++] = l; if (l > r->mmax) r->mmax = l; }
            }
            if (mo) {
                uint32_t na = 0;
                qsort(mo, nm, sizeof *mo, cmpd);
                r->mp95 = nm ? mo[nm - 1 - nm / 20] : -120;
                for (i = 0; i < nm; i++) na += mo[i] > -60;
                r->mpa = na ? mo[nm - 1 - na / 20] : -120;
                free(mo);
            }
        }
        qsort(st, nst, sizeof *st, cmpd);
        r->lu5 = nst ? st[nst / 20] : -120; r->lu50 = nst ? st[nst / 2] : -120; r->lu95 = nst ? st[nst - 1 - nst / 20] : -120;
        {   /* gaps and the envelope's spread, against the run's mean power */
            double m = 0, s1 = 0, s2 = 0;
            uint32_t g = 0, nn = 0;
            for (i = 0; i < nfr; i++) m += fr[i];
            m /= nfr ? nfr : 1;
            for (i = 0; i < nfr; i++) {
                double d = 10 * log10((fr[i] + 1e-14) / (m + 1e-14));
                if (d < -20) g++;
                if (d < -40) d = -40;
                s1 += d; s2 += d * d; nn++;
            }
            r->gaps = m > 1e-12 ? 100.0 * g / nfr : 100;
            {   /* bouts: sound after at least 200 ms of gap (the same -20 dB), each one event however many grains */
                uint32_t run_g = 20, nbt = 0;
                for (i = 0; i < nfr && m > 1e-12; i++) {
                    if (fr[i] < m * 0.01) run_g++;
                    else { if (run_g >= 20) nbt++; run_g = 0; }
                }
                r->bouts = nbt / (double)secs;
            }
            r->env = nn && m > 1e-12 ? sqrt(fmax(0, s2 / nn - (s1 / nn) * (s1 / nn))) : 0;
        }
    }
    r->evs = (stt.total - total0) / (double)secs;
    r->heroes = sumh / (nb - w0); r->lim = stt.limit; r->drops = drop / (double)secs;
    r->us = factor > 0 ? sumc / (nb - w0) : -1; r->w8 = factor > 0 ? w8 : -1;
    r->cut = factor > 0 ? (int)gov.user[0] - (int)gov.cur[0] : -1;
    free(fr); free(st); free(cost);
    return 0;
}

/* COUNT in individuals -> the knob: 0 none, n -> 1 + 21 log2 n (the source's count_q8 = 2^((v-1)/21)), at most 127 */
static unsigned count_of(unsigned n)
{
    double v = n ? 1 + 21 * log2((double)n) : 0;
    return v > 127 ? 127u : (unsigned)lround(v);
}

int main(int argc, char **argv)
{
    unsigned secs = 30, seeds = 3, acts[MAXA] = {0, 9, 64, 127}, na = 4, cnts[MAXA] = {1, 8, 64}, nc = 3, i, si, ai, ci, sd;
    unsigned cmin = 0, cmax = 0, amin = 0, amax = 0;
    const char *filt = 0;
    for (i = 1; i < (unsigned)argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < (unsigned)argc) secs = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n") && i + 1 < (unsigned)argc) seeds = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < (unsigned)argc) filt = argv[++i];
        else if (!strcmp(argv[i], "-p") && i + 1 < (unsigned)argc) particles = (unsigned)atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o")) onset = 1;
        else if (!strcmp(argv[i], "-c") && i + 1 < (unsigned)argc) {
            char *p = argv[++i];
            for (nc = 0; nc < MAXA && *p; ) { cnts[nc++] = (unsigned)strtol(p, &p, 10); if (*p == ',') p++; else break; }
        }
        else if (!strcmp(argv[i], "-a") && i + 1 < (unsigned)argc) {
            char *p = argv[++i];
            for (na = 0; na < MAXA && *p; ) { acts[na++] = (unsigned)strtol(p, &p, 10); if (*p == ',') p++; else break; }
        }
    }
    if (!secs || !seeds || !na || !nc) return 2;
    if (calib("build/calib/run")) factor = 0;
    if (onset) {                                          /* the lane's start against what follows */
        printf("txactivity -o: each sound's lane started (key down, the generator on), its first second against the %u s after it;\n"
               "momentary loudness (400 ms, every 100 ms), %u seeds: the first second's loudest, after it the 95th percentile\n"
               "(all moments / the sounding ones, over -60 LUFS) and the loudest (means over the seeds); rise: the first second over\n"
               "that 95th percentile of sounding moments, the median of the seeds (worst: their most); LOUD when the median is over 1 dB;\n"
               "t1: the first moment over -60 LUFS (s, the seeds' median / latest; 999: none)\n", secs - 1u, seeds);
        printf("%-15s %4s %4s | %7s %7s %7s %7s | %6s %6s | %5s %5s | %s\n", "sound", "CNT", "ACT", "first", "p95", "p95snd", "max", "rise", "worst",
               "t1", "t1max", "verdict");
        for (si = 0; si < NSND; si++) {
            int classic = SND[si].pre[0] == '#';
            if (filt && !strstr(SND[si].name, filt)) continue;
            for (ci = 0; ci < nc; ci++) {
                if (classic && ci) continue;
                count_knob = count_of(cnts[ci]);
                for (ai = 0; ai < na; ai++) {
                    double m1 = 0, p95 = 0, pa = 0, mx = 0, rise = -1e9, rs[64], ts[64];
                    unsigned nr = 0;
                    char c[8];
                    for (sd = 0; sd < seeds; sd++) {
                        res_t x;
                        if (run(&SND[si], acts[ai], 0x9E3779B9u * (sd + 1u) + si, secs, &x)) { printf("%s: failed\n", SND[si].name); return 1; }
                        m1 += x.m1 / seeds; p95 += x.mp95 / seeds; pa += x.mpa / seeds; mx += x.mmax / seeds;
                        if (nr < 64) ts[nr] = x.t1 < 0 ? 999 : x.t1;
                        {
                            double d = x.m1 > -100 ? x.m1 - x.mpa : -99;
                            if (d > rise) rise = d;
                            if (nr < 64) rs[nr++] = d;
                        }
                    }
                    if (classic) snprintf(c, sizeof c, "-"); else snprintf(c, sizeof c, "%u", cnts[ci]);
                    qsort(rs, nr, sizeof *rs, cmpd);
                    qsort(ts, nr, sizeof *ts, cmpd);
                    printf("%-15s %4s %4u | %7.1f %7.1f %7.1f %7.1f | %6.1f %6.1f | %5.1f %5.1f | %s\n", SND[si].name, c, acts[ai], m1, p95, pa, mx,
                           rs[nr / 2] < -98 ? -99.0 : rs[nr / 2], rise < -98 ? -99.0 : rise, ts[nr / 2], ts[nr - 1], rs[nr / 2] > 1.0 ? "LOUD" : "ok");
                }
            }
        }
        return 0;
    }
    printf("txactivity: each sound as a lane in the player (the generator on), %u s x %u seeds per COUNT x ACTIVITY%s%s\n", secs, seeds,
           particles ? ", PARTICLES set (-p)" : "", factor > 0 ? "" : "; no calibration (make tx-calib): us / cut not measured");
    printf("%-15s %4s %4s | %8s %6s %15s %13s %6s %6s %5s | %9s %3s | %s\n", "sound", "CNT", "ACT", "ev/s", "bout/s", "LUFS-S 5..95", "heroes/lim",
           "drop/s", "gaps%", "env", "us/w8", "cut", "verdict");
    for (ci = 0; ci < nc; ci++) {                         /* the grid's corners: the fewest / the most, least / most active */
        if (cnts[ci] < cnts[cmin]) cmin = ci;
        if (cnts[ci] > cnts[cmax]) cmax = ci;
    }
    for (ai = 0; ai < na; ai++) {
        if (acts[ai] < acts[amin]) amin = ai;
        if (acts[ai] > acts[amax]) amax = ai;
    }
    for (si = 0; si < NSND; si++) {
        const char *lone = "-", *cloud = "-";
        int classic = SND[si].pre[0] == '#';
        if (filt && !strstr(SND[si].name, filt)) continue;
        for (ci = 0; ci < nc; ci++) {
            if (classic && ci != cmax) continue;              /* (no COUNT: one row per ACTIVITY) */
            count_knob = count_of(cnts[ci]);
            for (ai = 0; ai < na; ai++) {
                res_t a = {0}, x;
                double l5 = 1e9, l95 = -1e9;
                int cut = -1;
                for (sd = 0; sd < seeds; sd++) {
                    if (run(&SND[si], acts[ai], 0x9E3779B9u * (sd + 1u) + si, secs, &x)) { printf("%s: failed\n", SND[si].name); return 1; }
                    a.evs += x.evs / seeds; a.bouts += x.bouts / seeds; a.heroes += x.heroes / seeds; a.lim += x.lim / seeds; a.drops += x.drops / seeds;
                    a.gaps += x.gaps / seeds; a.env += x.env / seeds; a.us += x.us / seeds; a.w8 = fmax(a.w8, x.w8);
                    if (x.lu5 < l5) l5 = x.lu5;
                    if (x.lu95 > l95) l95 = x.lu95;
                    if (x.cut > cut) cut = x.cut;
                }
                {
                    const char *v = "";
                    char g[8], u[16], c[8];
                    if ((ci == cmin || classic) && ai == amin)   /* (at ACTIVITY 0: the player's rarest step, stricter) */
                    {   /* (the events: the engine's, or its bouts where fewer: a gurgle of many bubbles is one) */
                        double ev = a.bouts < a.evs ? a.bouts : a.evs;
                        v = lone = (acts[ai] ? ev <= 2.0 && a.gaps >= 50.0 : ev <= 0.5 && a.gaps >= 80.0) ? "LONE" : "BUSY";
                    }
                    if (ci == cmax && ai == amax) {
                        int dense = a.evs >= 15, full = a.gaps < 1.0, even = a.env <= 6.0;
                        v = cloud = dense && full && even ? "CLOUD" : dense + full + even >= 2 ? "DENSE" : "EVENTS";
                    }
                    if (cut >= 0) snprintf(g, sizeof g, "%d", cut); else snprintf(g, sizeof g, "-");
                    if (a.us >= 0) snprintf(u, sizeof u, "%4.0f/%4.0f", a.us, a.w8); else snprintf(u, sizeof u, "-");
                    if (classic) snprintf(c, sizeof c, "-"); else snprintf(c, sizeof c, "%u", cnts[ci]);
                    printf("%-15s %4s %4u | %8.2f %6.2f %6.1f..%6.1f %6.1f/%-6.0f %6.2f %6.1f %5.1f | %9s %3s | %s\n", SND[si].name, c,
                           acts[ai], a.evs, a.bouts, l5 < -99 ? -99.9 : l5, l95 < -99 ? -99.9 : l95, a.heroes, a.lim, a.drops, a.gaps,
                           a.env, u, g, v);
                }
            }
        }
        printf("%-15s grid: %s / %s%s\n", SND[si].name, lone, cloud,
               !strcmp(lone, "LONE") && !strcmp(cloud, "CLOUD") ? "  ok" : "");
    }
    return 0;
}
