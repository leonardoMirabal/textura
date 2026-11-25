/* SPDX-License-Identifier: GPL-3.0-only */
/* txfxsweep: every effect control turned 0 -> top -> 0 at a human rate (a step every 20 ms) and moved once (its ends
 * both ways and one detent at the middle; a selector: every neighbouring pair both ways) under a ringing chord, against
 * renders of the same control held (Frodhes' control sweep, on tx_fx). The chord (C3 E4 G4 A4 C5, struck at 0.5 s, ~4 s
 * decay, harmonics 1..4) goes through the mix's DRIVE + FILTER and the bus' GRAIN DELAY + REVERB. Host only (the chord
 * and the checks are computed in double: a bench tool, not an engine).
 *
 *   txfxsweep check [NAME ...]   the cases (all, or those whose name holds a NAME): exit 1 on a flag not in KNOWN
 *                                (make tx-fx-sweep; make tx-pristine runs it too, so CI keeps it clean)
 *   txfxsweep list               every control: "GROUP ID MAX DEF NAME" a line
 *   txfxsweep GROUP ID SETUP MODE A B OUT   one render: GROUP d f g r (DRIVE, FILTER, GRAIN DELAY, REVERB); SETUP
 *                                "gid=v,..." (the other controls first, g a group letter); MODE s (held at A) | w (the
 *                                sweep) | f (A, then B at 2 s); OUT raw int32 L R interleaved, Q23
 *
 * The checks, each against the held renders:
 *   click   the largest |second difference| from 3 ms before to 30 ms after the move, against the larger of the two
 *           held renders' (and, for a control that glides through values, the held values between): over 10x flags
 *   trans   20 ms windows over 300 ms after the move outside the held renders' range (3 dB over the louder, 6 dB under
 *           the quieter; the wet alone on the bus)
 *   zipper  the sweep's largest |second difference| per 1024 samples against the held renders' there: over 2x flags
 *   jump    a level step between two 1024-sample windows of the sweep beyond the held renders' own: over 1.5 dB flags
 *   stuck   after the sweep, back at 0: 300 ms against the render held at 0, over 1.5 dB flags
 * KNOWN: the flags understood, each with its reason; any other flag fails. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "tx_fx.h"
#define FS 44100
#define T_NOTE 500
#define T_SW0 1000
#define STEP 20
#define T_FLIP 2000
#define MAXV 256u
#define MAXMS 8000u
static tx_fx_t fx;
static const tx_param_t *table(char g, unsigned *n)
{
    if (g == 'd') { *n = TX_DR_NP; return tx_fx_drive_params; }
    if (g == 'f') { *n = TX_MF_NP; return tx_fx_mfilter_params; }
    if (g == 'g') { *n = TX_GD_NP; return tx_fx_grain_params; }
    *n = TX_RV_NP; return tx_fx_reverb_params;
}
static unsigned maxof(char g, unsigned id) { unsigned n; return table(g, &n)[id].max; }
static unsigned pid(char g, const char *name)
{
    unsigned n, i;
    const tx_param_t *t = table(g, &n);
    for (i = 0; i < n; i++) if (!strcmp(t[i].name, name)) return i;
    fprintf(stderr, "txfxsweep: no control %c %s\n", g, name);
    exit(2);
}
static void setp(char g, unsigned id, unsigned v)
{
    if (g == 'd') tx_fx_drive_set(&fx, id, v);
    else if (g == 'f') tx_fx_drive_set(&fx, TX_FX_MF_ID + id, v);
    else if (g == 'g') tx_fx_grain_param(&fx.gd, id, v);
    else tx_fx_reverb_param(&fx.rev, id, v);
}

/* the chord, computed once: every render gets the same samples */
static int32_t chord[MAXMS * FS / 1000u + TX_BLOCK];
static void chord_make(void)
{
    static const int notes[5] = {48, 64, 67, 69, 72};
    double ph[5][4] = {{0}};
    unsigned i, k, h;
    for (i = 0; i < sizeof chord / sizeof chord[0]; i++) {
        double tt = (double)i / FS, s = 0;
        if (tt >= T_NOTE / 1000.0)
            for (k = 0; k < 5; k++)
                for (h = 0; h < 4; h++) {
                    double f = 440.0 * pow(2.0, (notes[k] - 69) / 12.0) * (h + 1);
                    ph[k][h] += f / FS;
                    s += 0.06 / (h + 1) * exp(-(tt - 0.5) * (1.0 + h) / 4.0) * sin(6.283185307179586 * ph[k][h]);
                }
        chord[i] = (int32_t)lrint(s * 8388608.0);
    }
}

typedef struct { int32_t *x; unsigned n; } snd_t;   /* L R interleaved, Q23; n frames */
static snd_t render(char g, unsigned id, const char *setup, char mode, unsigned A, unsigned B)
{
    char buf[400], *t, *sv;
    unsigned mx = maxof(g, id), nsw = 2u * mx, tend = mode == 'w' ? T_SW0 + nsw * STEP + 800u : 4000u, nb, b, j;
    int32_t L[TX_BLOCK], R[TX_BLOCK];
    snd_t o;
    if (tend > MAXMS) { fprintf(stderr, "txfxsweep: %c %u: the sweep is over %u ms\n", g, id, MAXMS); exit(2); }
    nb = (tend * FS / 1000u + TX_BLOCK - 1u) / TX_BLOCK;
    o.n = nb * TX_BLOCK;
    o.x = malloc(sizeof(int32_t) * 2u * o.n);
    if (!o.x) exit(2);
    tx_fx_init(&fx);
    snprintf(buf, sizeof buf, "%s", setup);
    for (t = strtok_r(buf, ",", &sv); t; t = strtok_r(0, ",", &sv)) {
        char *e = strchr(t, '=');
        if (e) setp(t[0], (unsigned)atoi(t + 1), (unsigned)atoi(e + 1));
    }
    setp(g, id, A);
    for (b = 0; b < nb; b++) {
        unsigned ms = (unsigned)((uint64_t)b * TX_BLOCK * 1000u / FS), v = A;
        if (mode == 'w' && ms >= T_SW0) { unsigned s = (ms - T_SW0) / STEP; v = s >= nsw ? 0 : s <= mx ? s : nsw - s; }
        if (mode == 'f') v = ms >= T_FLIP ? B : A;
        setp(g, id, v);
        for (j = 0; j < TX_BLOCK; j++) L[j] = R[j] = chord[b * TX_BLOCK + j];
        tx_fx_drive_mix(&fx, L, R, TX_BLOCK);
        tx_fx_bus(&fx, L, R, TX_BLOCK);
        for (j = 0; j < TX_BLOCK; j++) o.x[2u * (b * TX_BLOCK + j)] = L[j], o.x[2u * (b * TX_BLOCK + j) + 1u] = R[j];
    }
    return o;
}

/* ---- the cases, from the effects' own tables (they follow the controls as they change) */
typedef struct { char name[40]; char g; unsigned id, mx; char setup[80]; int sel, through; } case_t;
static case_t C[64];
static unsigned nc;
static void add(const char *name, char g, const char *pn, const char *setup, int sel, int through)
{
    case_t *c = &C[nc++];
    snprintf(c->name, sizeof c->name, "%s", name);
    c->g = g; c->id = pid(g, pn); c->mx = maxof(g, c->id);
    snprintf(c->setup, sizeof c->setup, "%s", setup);
    c->sel = sel; c->through = through;
}
static void cases(void)
{
    static const struct { int t; const char *n; } DT[] = {{1, "KHLON"}, {2, "TSH8"}, {3, "OHCD"}, {4, "BHLUE"}};
    static const struct { int t; const char *n; } RT[] = {{1, "ROOM"}, {2, "HALL"}, {3, "INF"}, {7, "PLATE"}};
    char nm[40], s[80], d0[16], rm[16];
    unsigned i, n, lp = pid('f', "LOW PASS"), rs = pid('f', "RESONANCE"), rt = pid('r', "TYPE");
    const tx_param_t *t;
    snprintf(d0, sizeof d0, "d%u=64", pid('d', "AMOUNT"));
    add("drive AMOUNT", 'd', "AMOUNT", "", 0, 0);
    add("drive TONE", 'd', "TONE", d0, 0, 0);
    add("drive MIX", 'd', "MIX", d0, 0, 1);
    add("drive TYPE", 'd', "TYPE", d0, 1, 0);
    for (i = 0; i < 4u; i++) {
        snprintf(nm, sizeof nm, "drive %s AMOUNT", DT[i].n);
        snprintf(s, sizeof s, "d%u=%d", pid('d', "TYPE"), DT[i].t);
        add(nm, 'd', "AMOUNT", s, 0, 0);
    }
    add("filter HIGH PASS", 'f', "HIGH PASS", "", 0, 1);
    add("filter LOW PASS", 'f', "LOW PASS", "", 0, 1);
    snprintf(s, sizeof s, "f%u=90", rs);
    add("filter HIGH PASS RESO", 'f', "HIGH PASS", s, 0, 1);
    add("filter LOW PASS RESO", 'f', "LOW PASS", s, 0, 1);
    snprintf(s, sizeof s, "f%u=64", lp);
    add("filter RESONANCE", 'f', "RESONANCE", s, 0, 0);
    snprintf(s, sizeof s, "f%u=64,f%u=60", lp, rs);
    add("filter SLOPE", 'f', "SLOPE", s, 1, 0);
    t = table('g', &n);
    for (i = 0; i < n; i++) {
        snprintf(nm, sizeof nm, "grain %s", t[i].name);
        if (!strcmp(t[i].name, "MIX")) s[0] = 0;
        else if (!strcmp(t[i].name, "INTERVALS"))
            snprintf(s, sizeof s, "g%u=64,g%u=64", pid('g', "MIX"), pid('g', "MOD"));
        else snprintf(s, sizeof s, "g%u=64", pid('g', "MIX"));
        add(nm, 'g', t[i].name, s, !strcmp(t[i].name, "FREEZE") || !strcmp(t[i].name, "INTERVALS"), 0);
    }
    snprintf(rm, sizeof rm, "r%u=64", pid('r', "MIX"));
    add("reverb TYPE", 'r', "TYPE", rm, 1, 0);
    for (i = 0; i < 4u; i++) {
        snprintf(nm, sizeof nm, "reverb MIX %s", RT[i].n);
        snprintf(s, sizeof s, "r%u=%d", rt, RT[i].t);
        add(nm, 'r', "MIX", s, 0, 0);
        snprintf(nm, sizeof nm, "reverb MOD %s", RT[i].n);
        snprintf(s, sizeof s, "r%u=%d,%s", rt, RT[i].t, rm);
        add(nm, 'r', "MOD", s, 0, 0);
    }
    t = table('r', &n);
    for (i = 0; i < n; i++) {
        if (!strcmp(t[i].name, "TYPE") || !strcmp(t[i].name, "MIX") || !strcmp(t[i].name, "MOD")) continue;
        snprintf(nm, sizeof nm, "reverb %s", t[i].name);
        if (!strcmp(t[i].name, "SHIFT")) snprintf(s, sizeof s, "r%u=2,%s,r%u=64", rt, rm, pid('r', "SHIMMER"));
        else snprintf(s, sizeof s, "r%u=2,%s", rt, rm);
        add(nm, 'r', t[i].name, s, !strcmp(t[i].name, "SHIFT"), 0);
    }
}
static const struct { const char *name, *check, *why; } KNOWN[] = {
    {"reverb TYPE", "trans", "a new TYPE starts empty after the old one's fade (SWELL ducks while played)"},
    {"grain FREEZE", "trans", "FREEZE loops the line's last 1.45 s: the echo then replays the chord as it was then, "
                              "louder than now (+5 dB at the default TIME, 220 ms after the freeze); no click"},
    {"reverb PREDELAY", "trans", "a time, as delay TIME: the tail rings on with the send shifted, so held tones "
                                 "interfere (one step, 63 -> 64, does it too); no click, the move crossfades in 46 ms"},
    {"reverb PREDELAY", "jump", "as its trans: the shifted send's tail beats with the old one's on the held tones; the "
                                "lows, coherent in L and R (tx_fx.c rv_cohere), beat alike on both sides where they "
                                "were apart (a step of 1.6 dB, was 1.2); no click, no zipper"},
    {"grain TIME", "jump", "a time: each step crossfades two taps of the chord 23 ms apart, whose partials interfere "
                           "(+-3 dB a step on the plain echo too: GRAIN 0, FEEDBACK 0, the wet alone); no click"},
    {"grain MIX", "jump", "the grain cloud's own spread (SPRAY's random reads): gone with SPRAY 0 or GRAIN 0"},
    {"grain SIZE", "jump", "the grain cloud's own spread: gone with SPRAY 0"},
    {"grain GRAINS", "trans", "the grain cloud's own spread (the held renders' 20 ms windows wander +-3 dB too); the "
                              "same move on copies (SPRAY 0) stays inside: tx-pristine GRAIN DELAY GRAINS changed"},
    {"grain MOD", "trans", "the grain cloud's own spread (the held renders' 20 ms windows wander +-3 dB too)"},
    {"grain MOD", "zipper", "MOD widens the places toward SPREAD's full reach (+-45 degrees since SPREAD reaches hard "
                            "left .. right): each grain's own random place moves the sides' second differences "
                            "(1.82x at +-11 degrees, 2.06x now); every place is a Hann-windowed grain, no step"},
    {"filter HIGH PASS", "jump", "the high-pass near its top leaves only a quiet residue of the chord (under -73 "
                                 "dBFS), held 2.5 .. 7.4 dB apart a step from 116 to 127: the law's own steps there"},
    {"filter HIGH PASS RESO", "jump", "as filter HIGH PASS, with RESONANCE"},
};
static int known(const char *name, const char *check)
{
    unsigned i;
    for (i = 0; i < sizeof KNOWN / sizeof KNOWN[0]; i++)
        if (!strcmp(KNOWN[i].name, name) && !strcmp(KNOWN[i].check, check)) return 1;
    return 0;
}

/* ---- the checks */
static snd_t DRY;                                       /* GRAIN DELAY and REVERB off: the dry chain */
static double d2max(snd_t a, unsigned s, unsigned e)
{
    double m = 0;
    unsigned i, c;
    if (e > a.n) e = a.n;
    for (i = s; i + 2u < e; i++)
        for (c = 0; c < 2u; c++) {
            double d = fabs((double)a.x[2u * (i + 2u) + c] - 2.0 * a.x[2u * (i + 1u) + c] + (double)a.x[2u * i + c]);
            if (d > m) m = d;
        }
    return m;
}
/* the level s .. e, dB re Q23 squared (less the dry for wet: the bus' wet alone) */
static double lvl(snd_t a, unsigned s, unsigned e, int wet)
{
    double q = 0;
    unsigned i, c;
    if (e > a.n) e = a.n;
    if (wet && e > DRY.n) e = DRY.n;
    for (i = s; i < e; i++)
        for (c = 0; c < 2u; c++) {
            double v = (double)a.x[2u * i + c] - (wet ? (double)DRY.x[2u * i + c] : 0.0);
            q += v * v;
        }
    return e > s ? 10.0 * log10(q / (2.0 * (e - s)) + 1e-9) : -300.0;
}
#define DBFS(x) ((x) + 10.0 * log10(70368744177664.0))   /* a dBFS in the levels' scale (Q23 squared) */
static snd_t held[MAXV + 1u];                            /* the case's held renders, by value */
static snd_t hold(const case_t *c, unsigned v)
{
    if (!held[v].x) held[v] = render(c->g, c->id, c->setup, 's', v, 0);
    return held[v];
}
static void addf(char *s, size_t n, const char *f)
{
    size_t l = strlen(s);
    snprintf(s + l, n - l, "%s%s", l ? ", " : "", f);
}
/* one case (in its own process): its line on stdout; exit 0 clean, 1 a flag not known, 2 known flags only */
static int run_case(const case_t *c)
{
    static const unsigned EV[5] = {0, 32, 64, 96, 127};
    unsigned mx = c->mx, p, np = c->sel ? mx : 2u, o, k, v, wa = 0, wb = 0;
    unsigned s0 = (T_FLIP - 3) * FS / 1000u, s1 = (T_FLIP + 30) * FS / 1000u;
    int wet = c->g == 'g' || c->g == 'r';
    double wclick = 0, wtrans = 0;
    char line[300], bad[120] = "", kn[120] = "", f[40];
    for (p = 0; p < np; p++) {
        unsigned a = c->sel ? p : p ? mx / 2u : 0u, b = c->sel ? p + 1u : p ? mx / 2u + 1u : mx;
        for (o = 0; o < 2u; o++) {
            unsigned x0 = o ? b : a, x1 = o ? a : b, lo = x0 < x1 ? x0 : x1, hi = x0 < x1 ? x1 : x0;
            snd_t mv = render(c->g, c->id, c->setup, 'f', x0, x1), h0 = hold(c, x0), h1 = hold(c, x1);
            double ref = fmax(fmax(d2max(h0, s0, s1), d2max(h1, s0, s1)), 1.0), r;
            if (c->through)                              /* (a glide passes the values between: their held too) */
                for (k = 1; k < 4u; k++) if (lo < EV[k] && EV[k] < hi) ref = fmax(ref, d2max(hold(c, EV[k]), s0, s1));
            r = d2max(mv, s0, s1) / ref;
            if (r > wclick) wclick = r, wa = x0, wb = x1;
            for (k = 0; k < 15u; k++) {                  /* 20 ms windows over 300 ms after the move */
                unsigned s = (T_FLIP + 20u * k) * FS / 1000u, e = s + FS / 50u, q;
                double l0 = lvl(h0, s, e, wet), l1 = lvl(h1, s, e, wet), lmax = fmax(l0, l1), lmin = fmin(l0, l1);
                double lm = lvl(mv, s, e, wet), d;
                if (c->through && k < 3u)
                    for (q = 0; q < 5u; q++)
                        if (lo < EV[q] && EV[q] < hi) {
                            double lx = lvl(hold(c, EV[q]), s, e, wet);
                            lmax = fmax(lmax, lx), lmin = fmin(lmin, lx);
                        }
                if (lmax < DBFS(-80.0)) continue;
                d = fmax(fmax(lm - lmax - 3.0, lmin - lm - 6.0), 0.0);
                if (d > wtrans) wtrans = d;
            }
            free(mv.x);
        }
    }
    snprintf(line, sizeof line, "%-24s click %6.2f%c  trans %6.2f%c", c->name, wclick, wclick > 10.0 ? '!' : ' ',
             wtrans, wtrans > 0 ? '!' : ' ');
    snprintf(f, sizeof f, "click %u->%u", wa, wb);
    if (wclick > 10.0) addf(known(c->name, "click") ? kn : bad, sizeof bad, f);
    if (wtrans > 0) addf(known(c->name, "trans") ? kn : bad, sizeof bad, "trans");
    if (!c->sel) {
        snd_t sw = render(c->g, c->id, c->setup, 'w', 0, 0), st[5];
        unsigned ns = 0, n = sw.n, e_sw = T_SW0 * FS / 1000u + 2u * mx * STEP * FS / 1000u, s, e, i;
        double zr = 0, jr = 0, pl = 0, ps[5] = {0}, stuck = 0;
        int have = 0;
        for (i = 0; i < 5u; i++) {                       /* the held at 0, 32, 64, 96 and the top */
            v = i < 4u ? EV[i] : mx;
            if (v > mx || (i == 4u && (mx == 0u || mx == 32u || mx == 64u || mx == 96u))) continue;
            st[ns] = hold(c, v);
            if (st[ns].n < n) n = st[ns].n;
            ns++;
        }
        for (s = T_SW0 * FS / 1000u; s < e_sw && s + 1024u < n; s += 1024u) {
            double m = 1.0, l, ls[5] = {0}, jst = 0;
            for (i = 0; i < ns; i++) m = fmax(m, d2max(st[i], s, s + 1024u));
            zr = fmax(zr, d2max(sw, s, s + 1024u) / m);
            l = lvl(sw, s, s + 1024u, 0);
            for (i = 0; i < ns; i++) ls[i] = lvl(st[i], s, s + 1024u, 0);
            if (have && l > DBFS(-100.0)) {
                for (i = 0; i < ns; i++) jst = fmax(jst, fabs(ls[i] - ps[i]));
                jr = fmax(jr, fabs(l - pl) - jst);
            }
            pl = l, have = 1;
            memcpy(ps, ls, sizeof ps);
        }
        s = e_sw + FS / 2u; e = s + FS * 3u / 10u;
        if (e > n) e = n;
        if (e > s + 1000u) stuck = fabs(lvl(sw, s, e, wet) - lvl(st[0], s, e, wet));
        snprintf(line + strlen(line), sizeof line - strlen(line), "  zipper %6.2f%c  jump %6.2f%c  stuck %6.2f%c", zr,
                 zr > 2.0 ? '!' : ' ', jr, jr > 1.5 ? '!' : ' ', stuck, stuck > 1.5 ? '!' : ' ');
        if (zr > 2.0) addf(known(c->name, "zipper") ? kn : bad, sizeof bad, "zipper");
        if (jr > 1.5) addf(known(c->name, "jump") ? kn : bad, sizeof bad, "jump");
        if (stuck > 1.5) addf(known(c->name, "stuck") ? kn : bad, sizeof bad, "stuck");
    }
    printf("%s   %s%s%s%s%s\n", line, *bad ? "FLAG " : "OK", bad, *kn ? "   (known: " : "", kn, *kn ? ")" : "");
    return *bad ? 1 : *kn ? 2 : 0;
}
/* every case its own process, 8 at a time; the lines in the cases' order */
static int check(int argc, char **argv)
{
    static FILE *out[64];
    static pid_t pr[64];
    int st[64];                                          /* -1 not run, 0 running, else 1 + its exit */
    unsigned i, nr = 0, nf = 0, nk = 0, run = 0;
    char buf[400];
    cases();
    DRY = render('g', pid('g', "MIX"), "", 's', 0, 0);
    for (i = 0; i < nc; i++) {
        int a, s = argc <= 2;
        for (a = 2; a < argc; a++) s |= strstr(C[i].name, argv[a]) != 0;
        st[i] = s ? 0 : -1;
    }
    fflush(stdout);
    for (i = 0; i < nc || run; ) {
        if (i < nc && st[i] < 0) { i++; continue; }
        if (i < nc && run < 8u) {
            if (!(out[i] = tmpfile()) || (pr[i] = fork()) < 0) { perror("txfxsweep"); return 2; }
            if (!pr[i]) { int r; dup2(fileno(out[i]), 1); r = run_case(&C[i]); fflush(stdout); _exit(r); }
            run++, i++;
        } else {
            int ws;
            pid_t w = wait(&ws);
            unsigned j;
            for (j = 0; j < nc; j++)
                if (!st[j] && pr[j] == w) st[j] = 1 + (WIFEXITED(ws) ? WEXITSTATUS(ws) : 1), run--;
        }
    }
    for (i = 0; i < nc; i++) {
        if (st[i] < 0) continue;
        rewind(out[i]);
        while (fgets(buf, sizeof buf, out[i])) fputs(buf, stdout);
        fclose(out[i]);
        nr++, nf += st[i] != 1 && st[i] != 3, nk += st[i] == 3;
    }
    printf("%u cases: %u flagged, %u with known flags only\n", nr, nf, nk);
    return nf ? 1 : 0;
}

int main(int argc, char **argv)
{
    unsigned k;
    snd_t x;
    FILE *o;
    if (argc == 2 && !strcmp(argv[1], "list")) {
        static const char G[] = "dfgr";
        for (k = 0; k < 4; k++) {
            unsigned n, i;
            const tx_param_t *t = table(G[k], &n);
            for (i = 0; i < n; i++) printf("%c %u %u %u %s\n", G[k], i, t[i].max, t[i].def, t[i].name);
        }
        return 0;
    }
    chord_make();
    if (argc >= 2 && !strcmp(argv[1], "check")) return check(argc, argv);
    if (argc < 8) { fprintf(stderr, "usage: see the head of bench/txfxsweep.c\n"); return 2; }
    x = render(argv[1][0], (unsigned)atoi(argv[2]), argv[3], argv[4][0], (unsigned)atoi(argv[5]),
               (unsigned)atoi(argv[6]));
    o = fopen(argv[7], "wb");
    if (!o) return 2;
    fwrite(x.x, sizeof(int32_t), 2u * x.n, o);
    fclose(o);
    free(x.x);
    return 0;
}
