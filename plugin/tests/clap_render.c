/* SPDX-License-Identifier: GPL-3.0-only */
/* A headless CLAP host for the plugin's tests: loads a .clap, plays a host-build script's MIDI lines through it
 * (tests/host.c's format: "T midi ST D1 D2", "T end"; every other line is ignored) and writes the output as a 32-bit
 * float WAV, the latency's frames dropped (the output starts at the app's own frame 0).
 *
 *   txp_clap_render PLUGIN.clap SCRIPT OUT.wav [--rate HZ] [--block N] [--keep-latency] [--state-check] [--cpu-from MS]
 *                   [--fuzz SEED] [--inactive] [--state-in FILE] [--state-out FILE] [--shot OUT.png] [--shot-w PX]
 *                   [--gui-test]
 *
 * --rate     the host's rate (44100 by default); --block  frames a process call (512)
 * --fuzz SEED  before the activation, every automatable parameter at a random value (one flush)
 * --inactive  no activation and no render (with --state-check: the state of an instance never activated)
 * --cpu-from MS  time only the process calls from that millisecond on (after a scene's load)
 * --state-in FILE  a state loaded before the activation (and the fuzz), as a host restoring a project
 * --state-out FILE  the state saved after the render (or without one, --inactive)
 * --shot OUT.png  after the render: the window (a headless one, no native view) painted at --shot-w pixels wide (1200
 *            by default) as a PNG; exit 5 when it looks empty
 * --gui-test  with --shot, after it: clicks and drags in the window (M, S, VOL, a map dot, LANES +, a row), checked
 *            against the parameters, the host's events, the masks and the saved state (exit 5 when one fails)
 * --state-check  after the render: save the state, load it into a second instance, check every parameter reads
 *            back the same (exit 3 when not) and that it saves the same text again (exit 4 when not: both kept as
 *            OUT.wav.state1, .state2)
 * It prints the latency, and the time each process call took: average and worst, in microseconds and as a share of the
 * buffer's own duration (the CPU the plugin needs on this machine; tools/plugin_gate.py). */
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <clap/clap.h>

#include "gui_shot.h"

typedef struct { uint32_t t; uint8_t st, d1, d2; } sev_t;
static sev_t ev[16384];
static uint32_t nev, end_ms = 6500;

static void read_script(const char *path)
{
    char line[256], what[16];
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    while (fgets(line, sizeof line, f) && nev < 16384u) {
        unsigned t = 0, a = 0, b = 0, c = 0;
        if (line[0] == '#' || sscanf(line, "%u %15s", &t, what) != 2) continue;
        if (!strcmp(what, "end")) { end_ms = t; continue; }
        if (strcmp(what, "midi")) continue;
        sscanf(line, "%*u %*s %i %u %u", (int *)&a, &b, &c);
        ev[nev].t = t; ev[nev].st = (uint8_t)a; ev[nev].d1 = (uint8_t)b; ev[nev].d2 = (uint8_t)c;
        nev++;
    }
    fclose(f);
}

/* ---- the host */
static void h_log(const clap_host_t *h, clap_log_severity s, const char *m) { (void)h; fprintf(stderr, "plugin log %d: %s\n", (int)s, m); }
static const clap_host_log_t H_LOG = { h_log };
static const void *h_ext(const clap_host_t *h, const char *id) { (void)h; return !strcmp(id, CLAP_EXT_LOG) ? &H_LOG : 0; }
static void h_nop(const clap_host_t *h) { (void)h; }
static const clap_host_t HOST = { CLAP_VERSION_INIT, 0, "txp_clap_render", "Textura", "", "1", h_ext, h_nop, h_nop, h_nop };

/* the block's events */
typedef struct { clap_event_midi_t e[512]; uint32_t n; } evl_t;
static uint32_t el_size(const clap_input_events_t *l) { return ((const evl_t *)l->ctx)->n; }
static const clap_event_header_t *el_get(const clap_input_events_t *l, uint32_t i) { return &((const evl_t *)l->ctx)->e[i].header; }
static uint32_t fz_n;                    /* --fuzz: the parameter events of one flush */
static uint32_t fz_size(const clap_input_events_t *l) { (void)l; return fz_n; }
static const clap_event_header_t *fz_get(const clap_input_events_t *l, uint32_t i) { return &((const clap_event_param_value_t *)l->ctx)[i].header; }
static uint32_t n_out[3];                /* the output events pushed: gesture begins, values, gesture ends */
static bool ol_push(const clap_output_events_t *l, const clap_event_header_t *e)
{
    (void)l;
    if (e->space_id == CLAP_CORE_EVENT_SPACE_ID) {
        if (e->type == CLAP_EVENT_PARAM_GESTURE_BEGIN) n_out[0]++;
        else if (e->type == CLAP_EVENT_PARAM_VALUE) n_out[1]++;
        else if (e->type == CLAP_EVENT_PARAM_GESTURE_END) n_out[2]++;
    }
    return true;
}

/* the state through memory */
typedef struct { char *b; size_t n, at; } mem_t;
static int64_t os_write(const clap_ostream_t *s, const void *p, uint64_t n)
{
    mem_t *m = s->ctx;
    m->b = realloc(m->b, m->n + n);
    memcpy(m->b + m->n, p, n);
    m->n += n;
    return (int64_t)n;
}
static int64_t is_read(const clap_istream_t *s, void *p, uint64_t n)
{
    mem_t *m = s->ctx;
    size_t k = m->n - m->at < n ? m->n - m->at : (size_t)n;
    memcpy(p, m->b + m->at, k);
    m->at += k;
    return (int64_t)k;
}

static double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static void wav_write(const char *path, const float *lr, uint32_t n, uint32_t rate)
{
    FILE *f = fopen(path, "wb");
    uint32_t bytes = n * 8u, v[] = {16, 0x00020003u, rate, rate * 8u, 0x00200008u}, riff = bytes + 36u;
    if (!f) { perror(path); exit(1); }
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); fwrite(v, 4, 5, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    fwrite(lr, 8, n, f);
    fclose(f);
}

/* ---- the window (--shot, --gui-test): a headless window core through the plugin's test extension (gui_shot.h) */
static uint32_t crc_tab[256];
static uint32_t crc32_of(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t x = i; for (int k = 0; k < 8; k++) x = x & 1 ? 0xEDB88320u ^ (x >> 1) : x >> 1; crc_tab[i] = x; }
    c = ~c;
    while (n--) c = crc_tab[(c ^ *p++) & 255u] ^ (c >> 8);
    return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void png_chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t h[8];
    uint32_t c;
    be32(h, n); memcpy(h + 4, type, 4);
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    c = crc32_of(0, (const uint8_t *)type, 4);
    c = crc32_of(c, d, n);
    be32(h, c);
    fwrite(h, 1, 4, f);
}
static int png_write(const char *path, const uint32_t *px, int w, int h)   /* RGB, zlib stored blocks (no deflate) */
{
    size_t raw_n = (size_t)h * (size_t)(1 + 3 * w), nblk = (raw_n + 65534u) / 65535u, zn = 2 + raw_n + nblk * 5 + 4, at = 0, o = 0;
    uint8_t *raw = malloc(raw_n), *z = malloc(zn), ihdr[13];
    uint32_t a = 1, b = 0;
    FILE *f;
    if (!raw || !z) return -1;
    for (int y = 0; y < h; y++) {
        raw[at++] = 0;
        for (int x = 0; x < w; x++) {
            uint32_t p = px[(size_t)y * (size_t)w + (size_t)x];
            raw[at++] = (uint8_t)(p >> 16); raw[at++] = (uint8_t)(p >> 8); raw[at++] = (uint8_t)p;
        }
    }
    z[o++] = 0x78; z[o++] = 0x01;
    for (at = 0; at < raw_n;) {
        size_t n = raw_n - at < 65535u ? raw_n - at : 65535u;
        z[o++] = at + n == raw_n ? 1 : 0;
        z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8); z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + at, n); o += n; at += n;
    }
    for (at = 0; at < raw_n; at++) { a = (a + raw[at]) % 65521u; b = (b + a) % 65521u; }
    be32(z + o, b << 16 | a); o += 4;
    if (!(f = fopen(path, "wb"))) { perror(path); return -1; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    be32(ihdr, (uint32_t)w); be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(f, "IHDR", ihdr, 13);
    png_chunk(f, "IDAT", z, (uint32_t)o);
    png_chunk(f, "IEND", 0, 0);
    fclose(f);
    free(raw); free(z);
    return 0;
}
static void gui_flush(const clap_plugin_t *p)   /* as a host answers request_flush */
{
    const clap_plugin_params_t *xp = p->get_extension(p, CLAP_EXT_PARAMS);
    static evl_t none;
    clap_input_events_t in = { &none, el_size, el_get };
    clap_output_events_t on = { 0, ol_push };
    none.n = 0;
    xp->flush(p, &in, &on);
}
static void gui_key(const clap_plugin_t *p, const txg_gui_shot_t *xs, int key)
{
    frg_input e;
    memset(&e, 0, sizeof e);
    e.kind = FRG_KEY; e.key = key;
    xs->input(p, &e);
    gui_flush(p);
    xs->tick(p);
}
static void gui_click(const clap_plugin_t *p, const txg_gui_shot_t *xs, float x, float y, int mods)
{
    frg_input e;
    memset(&e, 0, sizeof e);
    e.kind = FRG_DOWN; e.button = FRG_LEFT; e.x = x; e.y = y; e.mods = mods;
    xs->input(p, &e);
    e.kind = FRG_UP;
    xs->input(p, &e);
    gui_flush(p);
    xs->tick(p);
}
static double gui_value(const clap_plugin_t *p, uint32_t id)
{
    const clap_plugin_params_t *xp = p->get_extension(p, CLAP_EXT_PARAMS);
    double v = -1.0;
    xp->get_value(p, id, &v);
    return v;
}
static void gui_wait(void) { struct timespec ts = { 0, 350000000 }; nanosleep(&ts, 0); }   /* (past the window's own
                                          * value's hold: what it shows then is the app's) */
#define LANE_ID(k, i) (((uint32_t)(k) < 4u ? (uint32_t)(k) : 12u + (uint32_t)(k)) << 8 | (uint32_t)(i))
#define ID_PRESET 0x10000u               /* (bridge.h TXP_ID_PRESET) */
/* gui_core.h's controls the tests click (its enum's values) */
enum { W_ROW = 1, W_VOL, W_MUTE, W_SOLO, W_DOT, W_LUP = 9, W_SET = 10, W_VMAP = 13, W_VSPACE, W_PRESET, W_SND, W_COLDOT,
       W_SWATCH, W_SEG = 26, W_FIELD = 27, W_PARAM = 30, W_MANY, W_ACT, W_REC, W_UNDO, W_SAVE, W_FXOPEN, W_FXTAB, W_SLOT,
       W_SAVEGO, W_NAME, W_TEMPO, W_ROWS, W_PLACE, W_MAPR, W_SELDOT, W_STEP, W_CMETER, W_TIP, W_PREVIEW, W_PVSTEP, W_FXD };
enum { W_CLOSE = 11, W_LIST = 23, W_KEEP = 24 };
#define ID_KEYS 0x10001u                 /* (bridge.h TXP_ID_KEYS) */
static int gui_hit(const clap_plugin_t *p, const txg_gui_shot_t *xs, int kind, int lane)   /* a click on a control */
{
    float r[4];
    if (!xs->find(p, kind, lane, r)) { fprintf(stderr, "gui: no control %d (%d)\n", kind, lane); return 0; }
    if (kind == W_FXOPEN) gui_click(p, xs, r[0] + r[2] - 24.f, r[1] + r[3] * 0.5f, 0);   /* (a section's head: at its ALL,
                                          * clear of COMP's own ON) */
    else gui_click(p, xs, r[0] + r[2] * 0.5f, r[1] + r[3] * 0.5f, 0);
    return 1;
}
static int gui_png(const clap_plugin_t *p, const txg_gui_shot_t *xs, const char *png, const char *tag)   /* the window
                                          * now, as png with "-tag" before its ".png" */
{
    const uint32_t *px;
    int w, h;
    char path[1024];
    const char *dot = strrchr(png, '.');
    size_t n = dot ? (size_t)(dot - png) : strlen(png);
    xs->tick(p);
    if (!xs->grab(p, 0, &px, &w, &h)) return 5;
    snprintf(path, sizeof path, "%.*s-%s.png", (int)n, png, tag);
    return png_write(path, px, w, h) ? 5 : 0;
}
static int gui_views(const clap_plugin_t *p, const txg_gui_shot_t *xs, const char *png)   /* the other views' pictures:
                                          * SPACE, the sound browser, the presets browser, SETTINGS, the colour picker */
{
    int rc = 0, sel;
    float r[4];
    if (!gui_hit(p, xs, W_VSPACE, -1)) return 5;
    rc |= gui_png(p, xs, png, "space");
    if (!gui_hit(p, xs, W_VMAP, -1)) return 5;
    for (sel = 0; sel < 16 && !xs->find(p, W_COLDOT, sel, r); sel++) {}
    if (!gui_hit(p, xs, W_SND, sel < 16 ? sel : 0)) return 5;
    rc |= gui_png(p, xs, png, "sounds");
    gui_key(p, xs, FRK_ESCAPE);
    if (!gui_hit(p, xs, W_PRESET, -1)) return 5;
    rc |= gui_png(p, xs, png, "presets");
    gui_key(p, xs, FRK_ESCAPE);
    if (!gui_hit(p, xs, W_SET, -1)) return 5;
    rc |= gui_png(p, xs, png, "settings");
    gui_key(p, xs, FRK_ESCAPE);
    if (sel < 16) {
        if (!gui_hit(p, xs, W_COLDOT, sel)) return 5;
        rc |= gui_png(p, xs, png, "colour");
        gui_key(p, xs, FRK_ESCAPE);
    }
    if (!gui_hit(p, xs, W_FXOPEN, 3)) return 5;   /* the strip's DELAY: its drawer, every parameter */
    rc |= gui_png(p, xs, png, "fx");
    if (!gui_hit(p, xs, W_FXOPEN, 0)) return 5;   /* COMP: its meters, its curve */
    rc |= gui_png(p, xs, png, "comp");
    if (!gui_hit(p, xs, W_FXOPEN, 2)) return 5;   /* FILTER: its response in Hz */
    rc |= gui_png(p, xs, png, "filter");
    if (!gui_hit(p, xs, W_FXOPEN, 4)) return 5;   /* REVERB: its tail */
    rc |= gui_png(p, xs, png, "reverb");
    if (!gui_hit(p, xs, W_FXOPEN, 5)) return 5;   /* MOVEMENT: LFO 1 */
    rc |= gui_png(p, xs, png, "lfo");
    if (!gui_hit(p, xs, W_FXTAB, 13)) return 5;   /* ENV, FOLLOW */
    rc |= gui_png(p, xs, png, "env");
    if (!gui_hit(p, xs, W_FXTAB, 14)) return 5;   /* the matrix */
    rc |= gui_png(p, xs, png, "matrix");
    gui_key(p, xs, FRK_ESCAPE);
    if (!gui_hit(p, xs, W_SAVE, -1)) return 5;    /* SAVE's panel */
    rc |= gui_png(p, xs, png, "save");
    gui_key(p, xs, FRK_ESCAPE);
    if (!gui_hit(p, xs, W_REC, -1)) return 5;     /* STOP ALL (what PLAY ALL brings back shown), PLAY ALL */
    gui_wait(); gui_flush(p); xs->tick(p);
    rc |= gui_png(p, xs, png, "stopped");
    if (!gui_hit(p, xs, W_SND, sel < 16 ? sel : 0) || !gui_hit(p, xs, W_LIST, 2)) return 5;   /* the preview on a lane
                                          * stopped: its third sound */
    gui_wait(); gui_flush(p); xs->tick(p);
    rc |= gui_png(p, xs, png, "preview");
    gui_key(p, xs, FRK_ESCAPE);
    gui_wait(); gui_flush(p); xs->tick(p);
    if (!gui_hit(p, xs, W_REC, -1)) return 5;
    gui_wait(); gui_flush(p); xs->tick(p);
    rc |= gui_png(p, xs, png, "replay");
    if (!gui_hit(p, xs, W_SELDOT, 2)) return 5;   /* the rows, lane 3 selected (then the lane before again) */
    gui_wait(); gui_flush(p); xs->tick(p);
    rc |= gui_png(p, xs, png, "rows");
    if (!gui_hit(p, xs, W_SELDOT, sel < 16 ? sel : 0)) return 5;
    gui_wait(); gui_flush(p); xs->tick(p);
    return rc;
}
static void gui_wheel(const clap_plugin_t *p, const txg_gui_shot_t *xs, float x, float y, float d)
{
    frg_input e;
    memset(&e, 0, sizeof e);
    e.kind = FRG_WHEEL; e.x = x; e.y = y; e.wheel = d;
    xs->input(p, &e);
    gui_flush(p);
    xs->tick(p);
}
static void gui_drag(const clap_plugin_t *p, const txg_gui_shot_t *xs, float x, float y, float dx, float dy, int dbl)
{
    frg_input e;
    memset(&e, 0, sizeof e);
    e.kind = dbl ? FRG_DOUBLE : FRG_DOWN; e.button = FRG_LEFT; e.x = x; e.y = y;
    xs->input(p, &e);
    if (!dbl) { e.kind = FRG_MOVE; e.x += dx * 0.5f; e.y += dy * 0.5f; xs->input(p, &e); e.x = x + dx; e.y = y + dy; xs->input(p, &e); }
    e.kind = FRG_UP;
    xs->input(p, &e);
    gui_flush(p);
    xs->tick(p);
}
static int gui_param_wheel(const clap_plugin_t *p, const txg_gui_shot_t *xs, uint32_t id, float d, const char *what)
{   /* a value's cell under the wheel: the value moved */
    float r[4];
    double v0, v1;
    if (!xs->find(p, W_PARAM, (int)id, r)) { fprintf(stderr, "gui: no cell for %s (%#x)\n", what, id); return 1; }
    v0 = gui_value(p, id);
    gui_wheel(p, xs, r[0] + r[2] * 0.5f, r[1] + r[3] * 0.5f, d);
    v1 = gui_value(p, id);
    printf("gui %s: %g -> %g\n", what, v0, v1);
    if (v1 == v0) { fprintf(stderr, "gui: %s did not move\n", what); return 1; }
    return 0;
}
static int gui_test5(const clap_plugin_t *p, const txg_gui_shot_t *xs)   /* step 5: HOW MANY / ACTIVE, the drawer, the
                                          * strip and its expanded view, KEYS, SAVE */
{
    int bad = 0;
    float r[4];
    double v0, v1;
    /* HOW MANY: lane 1's third block (4); HOW ACTIVE: its fifth (5) */
    {   /* (a block that is not the top lit one: that one again is one less) */
        int tm = gui_value(p, LANE_ID(0, 1)) == 3.0 ? 4 : 3, ta = gui_value(p, LANE_ID(0, 2)) == 5.0 ? 6 : 5;
        if (!xs->find(p, W_MANY, 0, r)) { fprintf(stderr, "gui: no HOW MANY on lane 1\n"); return 5; }
        gui_click(p, xs, r[0] + 2.f + (r[2] - 4.f) * (((float)tm - 0.5f) / 6.f), r[1] + r[3] * 0.5f, 0);
        if (!xs->find(p, W_ACT, 0, r)) { fprintf(stderr, "gui: no HOW ACTIVE on lane 1\n"); return 5; }
        gui_click(p, xs, r[0] + 2.f + (r[2] - 4.f) * (((float)ta - 0.5f) / 9.f), r[1] + r[3] * 0.5f, 0);
        v0 = gui_value(p, LANE_ID(0, 1)); v1 = gui_value(p, LANE_ID(0, 2));
        printf("gui HOW MANY %g, HOW ACTIVE %g (blocks %d, %d)\n", v0, v1, tm, ta);
        if (v0 != (double)tm || v1 != (double)ta) { fprintf(stderr, "gui: the HOW MANY / HOW ACTIVE blocks\n"); bad = 1; }
    }
    /* the drawer: lane 1's first card dragged up, then a double-click puts its sound's own value back */
    if (!xs->find(p, W_PARAM, (int)LANE_ID(0, 8), r)) { fprintf(stderr, "gui: no card 1 in the drawer\n"); return 5; }
    v0 = gui_value(p, LANE_ID(0, 8));
    gui_drag(p, xs, r[0] + r[2] * 0.5f, r[1] + r[3] * 0.5f, 0.f, v0 > 64.0 ? 50.f : -50.f, 0);
    v1 = gui_value(p, LANE_ID(0, 8));
    gui_drag(p, xs, r[0] + r[2] * 0.5f, r[1] + r[3] * 0.5f, 0.f, 0.f, 1);
    printf("gui card 1: %g -> %g -> %g (double-click)\n", v0, v1, gui_value(p, LANE_ID(0, 8)));
    if (v1 == v0 || gui_value(p, LANE_ID(0, 8)) != v0) { fprintf(stderr, "gui: the drawer's card\n"); bad = 1; }
    bad |= gui_param_wheel(p, xs, LANE_ID(0, 48), 1.f, "GROOVE PART");
    /* the strip: FILTER's HIGH PASS; expanded: DELAY's SPRAY, LFO 1's RATE, MOD 1's SOURCE, ENV 1's ATTACK */
    bad |= gui_param_wheel(p, xs, 4u << 8 | 52u, 1.f, "HIGH PASS");
    if (!gui_hit(p, xs, W_FXOPEN, 3)) return 5;
    bad |= gui_param_wheel(p, xs, 4u << 8 | 5u, 1.f, "GRAIN SPRAY");
    if (!gui_hit(p, xs, W_FXOPEN, 5)) return 5;
    bad |= gui_param_wheel(p, xs, 5u << 8 | 1u, 1.f, "LFO 1 RATE");
    if (!gui_hit(p, xs, W_FXTAB, 13)) return 5;
    bad |= gui_param_wheel(p, xs, 6u << 8 | 1u, 1.f, "ENV 1 ATTACK");
    if (!gui_hit(p, xs, W_FXTAB, 14)) return 5;
    if (!xs->find(p, W_PARAM, 7 << 8 | 0, r)) { fprintf(stderr, "gui: no MOD 1 SOURCE\n"); return 5; }
    v0 = gui_value(p, 7u << 8);
    gui_click(p, xs, r[0] + r[2] * 0.5f, r[1] + r[3] * 0.5f, 0);
    printf("gui MOD 1 SOURCE: %g -> %g (a click)\n", v0, gui_value(p, 7u << 8));
    if (gui_value(p, 7u << 8) == v0) { fprintf(stderr, "gui: MOD 1 SOURCE\n"); bad = 1; }
    bad |= gui_param_wheel(p, xs, 7u << 8 | 64u, 1.f, "MOD CC");
    gui_key(p, xs, FRK_ESCAPE);
    /* SETTINGS: KEYS to FM-1 KEYS */
    if (!gui_hit(p, xs, W_SET, -1)) return 5;
    if (!gui_hit(p, xs, W_SEG, (int)((ID_KEYS & 0xFFFFu) << 8 | 1u))) return 5;
    gui_key(p, xs, FRK_ESCAPE);
    printf("gui KEYS: %g\n", gui_value(p, ID_KEYS));
    if (gui_value(p, ID_KEYS) != 1.0) { fprintf(stderr, "gui: KEYS\n"); bad = 1; }
    /* SAVE: user preset U01 named TEST */
    if (!gui_hit(p, xs, W_SAVE, -1)) return 5;
    if (!gui_hit(p, xs, W_SLOT, 0)) return 5;
    for (const char *t = "TEST"; *t; t++) gui_key(p, xs, *t);
    gui_key(p, xs, FRK_RETURN);
    for (int i = 0; i < 8; i++) { gui_wait(); gui_flush(p); xs->tick(p); }
    gui_key(p, xs, FRK_ESCAPE);
    return bad;
}
static int gui_steps(const clap_plugin_t *p, const txg_gui_shot_t *xs, int act, int k, int v)   /* HOW MANY / HOW ACTIVE:
                                          * lane k's block v clicked */
{
    float r[4];
    if (!xs->find(p, act ? W_ACT : W_MANY, k, r)) { fprintf(stderr, "gui: no HOW %s on lane %d\n", act ? "ACTIVE" : "MANY", k + 1); return 0; }
    gui_click(p, xs, r[0] + 2.f + (r[2] - 4.f) * (((float)v - 0.5f) / (act ? 9.f : 6.f)), r[1] + r[3] * 0.5f, 0);
    return 1;
}
static int gui_test5b(const clap_plugin_t *p, const txg_gui_shot_t *xs)   /* step 5b: STOP ALL / PLAY ALL keep the lanes'
                                          * own HOW MANY / HOW ACTIVE and the selected lane; the selection by the row's
                                          * dot and number */
{
    int bad = 0, run_m, run_a, sel;
    /* lane 2 MANY 4 / ACTIVE 6 (the blocks, set twice: a block that is the top lit one again is one less) */
    if (gui_value(p, LANE_ID(1, 1)) != 4.0 && !gui_steps(p, xs, 0, 1, 4)) return 5;
    if (gui_value(p, LANE_ID(1, 2)) != 6.0 && !gui_steps(p, xs, 1, 1, 6)) return 5;
    gui_wait(); gui_flush(p); xs->tick(p);
    if (gui_value(p, LANE_ID(1, 1)) != 4.0 || gui_value(p, LANE_ID(1, 2)) != 6.0) {
        fprintf(stderr, "gui: lane 2 not MANY 4 / ACTIVE 6 (%g / %g)\n", gui_value(p, LANE_ID(1, 1)), gui_value(p, LANE_ID(1, 2)));
        return 5;
    }
    /* lane 3 selected by its dot, then lane 4 by its number, then lane 3 by its dot again */
    if (!gui_hit(p, xs, W_SELDOT, 2)) return 5;
    gui_wait(); gui_flush(p); xs->tick(p);
    sel = xs->sel(p);
    if (!gui_hit(p, xs, W_SELDOT, 3)) return 5;
    gui_wait(); gui_flush(p); xs->tick(p);
    printf("gui select: the dot of lane 3 -> lane %d, of lane 4 -> lane %d\n", sel + 1, xs->sel(p) + 1);
    if (sel != 2 || xs->sel(p) != 3) { fprintf(stderr, "gui: the row's dot did not select its lane\n"); bad = 1; }
    if (!gui_hit(p, xs, W_SELDOT, 2)) return 5;
    gui_wait(); gui_flush(p); xs->tick(p);
    /* STOP ALL, PLAY ALL: lane 2 back at MANY 4 / ACTIVE 6, lane 3 still selected */
    if (!gui_hit(p, xs, W_REC, -1)) return 5;
    gui_wait(); gui_flush(p); xs->tick(p);
    run_m = (int)gui_value(p, LANE_ID(1, 1)); run_a = (int)gui_value(p, LANE_ID(1, 2));
    printf("gui STOP ALL: lane 2 MANY %d / ACTIVE %d, lane %d selected\n", run_m, run_a, xs->sel(p) + 1);
    if (run_m != 0 || xs->sel(p) != 2) { fprintf(stderr, "gui: STOP ALL\n"); bad = 1; }
    if (!gui_hit(p, xs, W_REC, -1)) return 5;
    gui_wait(); gui_flush(p); xs->tick(p);
    run_m = (int)gui_value(p, LANE_ID(1, 1)); run_a = (int)gui_value(p, LANE_ID(1, 2));
    printf("gui PLAY ALL: lane 2 MANY %d / ACTIVE %d, lane %d selected\n", run_m, run_a, xs->sel(p) + 1);
    if (run_m != 4 || run_a != 6 || xs->sel(p) != 2) { fprintf(stderr, "gui: PLAY ALL did not bring lane 2 back as it was\n"); bad = 1; }
    {   /* the same through the project's state: STOP ALL, saved, loaded again (the app forgets what REC remembered),
         * PLAY ALL: lane 2 back as it was */
        const clap_plugin_state_t *st = p->get_extension(p, CLAP_EXT_STATE);
        mem_t m = {0, 0, 0};
        clap_ostream_t os = { &m, os_write };
        clap_istream_t is = { &m, is_read };
        if (!gui_hit(p, xs, W_REC, -1)) return 5;
        gui_wait(); gui_flush(p); xs->tick(p);
        if (!st || !st->save(p, &os)) { fprintf(stderr, "gui: state save failed\n"); return 5; }
        m.at = 0;
        if (!st->load(p, &is)) { fprintf(stderr, "gui: state load failed\n"); free(m.b); return 5; }
        free(m.b);
        for (int i = 0; i < 4; i++) { gui_wait(); gui_flush(p); xs->tick(p); }
        run_m = (int)gui_value(p, LANE_ID(1, 1));
        if (!gui_hit(p, xs, W_REC, -1)) return 5;
        gui_wait(); gui_flush(p); xs->tick(p);
        printf("gui STOP ALL, the state saved and loaded (lane 2 MANY %d), PLAY ALL: lane 2 MANY %g / ACTIVE %g, lane %d selected\n",
               run_m, gui_value(p, LANE_ID(1, 1)), gui_value(p, LANE_ID(1, 2)), xs->sel(p) + 1);
        if (run_m != 0 || gui_value(p, LANE_ID(1, 1)) != 4.0 || gui_value(p, LANE_ID(1, 2)) != 6.0 || xs->sel(p) != 2) {
            fprintf(stderr, "gui: PLAY ALL after the state's load did not bring lane 2 back\n");
            bad = 1;
        }
    }
    return bad;
}
/* ---- step 5c */
static int gui_open_fx(const clap_plugin_t *p, const txg_gui_shot_t *xs, int want)   /* the drawer open: its tab (-1 none) */
{
    float r[4];
    xs->tick(p);
    for (int t = 0; t < 15; t++) if (xs->find(p, W_FXD, t, r)) return want < 0 ? 0 : want == 5 ? t >= 5 : t == want;
    return want < 0;
}
static int gui_test5c_drawer(const clap_plugin_t *p, const txg_gui_shot_t *xs)   /* each effect's drawer: opens from the
                                          * strip with all its parameters, switches, closes (CLOSE, Esc); the lanes stay
                                          * usable above it */
{
    static const char *const MOD[5] = { "Compressor", "Drive", "Filter", "Grain delay", "Reverb" };
    const clap_plugin_params_t *xp = p->get_extension(p, CLAP_EXT_PARAMS);
    uint32_t np = xp->count(p);
    int bad = 0, s0 = xs->sel(p);
    float r[4], d[4];
    for (int s = 0; s < 6; s++) {
        int miss = 0, n = 0, sel1;
        if (!gui_hit(p, xs, W_FXOPEN, s)) return 5;
        if (!gui_open_fx(p, xs, s)) { fprintf(stderr, "gui: section %d's drawer did not open\n", s); bad = 1; continue; }
        if (s < 5)
            for (uint32_t i = 0; i < np; i++) {   /* (every parameter of the effect in its drawer) */
                clap_param_info_t inf;
                xp->get_info(p, i, &inf);
                if (strcmp(inf.module, MOD[s])) continue;
                n++;
                if (!xs->find(p, W_PARAM, (int)inf.id, r)) { fprintf(stderr, "gui: %s not in %s's drawer\n", inf.name, MOD[s]); miss++; }
            }
        /* the lanes above it: lane 2 selected by its row, its HOW MANY there */
        xs->find(p, W_FXD, s < 5 ? s : 5, d);
        for (int t = 5; s == 5 && t < 15; t++) if (xs->find(p, W_FXD, t, d)) break;
        if (!xs->find(p, W_ROW, 1, r) || r[1] + r[3] > d[1] + 0.5f || !xs->find(p, W_MANY, 1, r)) {
            fprintf(stderr, "gui: lane 2's row not above section %d's drawer\n", s);
            bad = 1;
        }
        if (!gui_hit(p, xs, W_SELDOT, s % 2 ? 1 : 0)) return 5;
        gui_wait(); gui_flush(p); xs->tick(p);
        sel1 = xs->sel(p);
        printf("gui drawer %d: open, %d of %d parameters in it, lane %d selected under it\n", s, n - miss, n, sel1 + 1);
        if (miss || sel1 != (s % 2 ? 1 : 0) || !gui_open_fx(p, xs, s)) { fprintf(stderr, "gui: section %d's drawer\n", s); bad = 1; }
    }
    if (!gui_hit(p, xs, W_FXOPEN, 2) || !gui_open_fx(p, xs, 2)) bad = 1;   /* switched to FILTER, then closed by it */
    if (!gui_hit(p, xs, W_FXOPEN, 2) || !gui_open_fx(p, xs, -1)) { fprintf(stderr, "gui: the drawer's CLOSE\n"); bad = 1; }
    if (!gui_hit(p, xs, W_FXOPEN, 4)) return 5;
    gui_key(p, xs, FRK_ESCAPE);
    if (!gui_open_fx(p, xs, -1)) { fprintf(stderr, "gui: Esc did not close the drawer\n"); bad = 1; }
    printf("gui drawer: switched, closed by its section, closed by Esc\n");
    if (!gui_hit(p, xs, W_SELDOT, s0)) return 5;
    gui_wait(); gui_flush(p); xs->tick(p);
    return bad;
}
static int gui_test5c_preview(const clap_plugin_t *p, const txg_gui_shot_t *xs)   /* the sound browser's preview plays on
                                          * a stopped lane; CLOSE puts it back exactly (sound, stopped, HOW MANY, the
                                          * memory of PLAY ALL, a card); KEEP keeps the sound, the lane stopped */
{
    int bad = 0, f0, s0, f1, s1, f2, s2;
    double card0, card1, m0, a0, mp, cardb;
    float r[4];
    const int k = 1;
    /* lane 2 playing at MANY 4 / ACTIVE 6 (gui_test5b's), a card set, then STOP ALL */
    if (!gui_hit(p, xs, W_SELDOT, k)) return 5;
    gui_wait(); gui_flush(p); xs->tick(p);
    if (!xs->find(p, W_PARAM, (int)LANE_ID(k, 8), r)) { fprintf(stderr, "gui: no card 1 for lane 2\n"); return 5; }
    gui_wheel(p, xs, r[0] + r[2] * 0.5f, r[1] + r[3] * 0.5f, 1.f);
    gui_wheel(p, xs, r[0] + r[2] * 0.5f, r[1] + r[3] * 0.5f, 1.f);
    gui_wait(); gui_flush(p); xs->tick(p);
    card0 = gui_value(p, LANE_ID(k, 8));
    m0 = gui_value(p, LANE_ID(k, 1)); a0 = gui_value(p, LANE_ID(k, 2));
    if (!gui_hit(p, xs, W_REC, -1)) return 5;
    gui_wait(); gui_flush(p); xs->tick(p);
    if (!xs->lane(p, k, &f0, &s0) || gui_value(p, LANE_ID(k, 1)) != 0.0) { fprintf(stderr, "gui: lane 2 not stopped\n"); return 5; }
    /* the browser: a click on another sound plays it */
    if (!gui_hit(p, xs, W_SND, k)) return 5;
    if (!xs->find(p, W_KEEP, -1, r)) { fprintf(stderr, "gui: CHANGE did not open the sound browser\n"); return 5; }
    if (!gui_hit(p, xs, W_LIST, s0 == 3 ? 4 : 3)) return 5;
    gui_wait(); gui_flush(p); xs->tick(p);
    (void)xs->lane(p, k, &f1, &s1);
    mp = gui_value(p, LANE_ID(k, 1));
    printf("gui preview: lane 2 stopped (%d.%d), a click: %d.%d plays at HOW MANY %g\n", f0, s0, f1, s1, mp);
    if ((f1 == f0 && s1 == s0) || mp < 1.0) { fprintf(stderr, "gui: the preview did not play on the stopped lane\n"); bad = 1; }
    /* PREVIEW's HOW MANY one down (its < ): heard at once */
    if (!gui_hit(p, xs, W_PVSTEP, 0)) return 5;
    gui_wait(); gui_flush(p); xs->tick(p);
    printf("gui preview: its HOW MANY one down: lane 2 at %g\n", gui_value(p, LANE_ID(k, 1)));
    if (gui_value(p, LANE_ID(k, 1)) != mp - 1.0) { fprintf(stderr, "gui: the preview's HOW MANY\n"); bad = 1; }
    /* CLOSE: lane 2 exactly as it was */
    if (!gui_hit(p, xs, W_CLOSE, -1)) return 5;
    gui_wait(); gui_flush(p); xs->tick(p);
    (void)xs->lane(p, k, &f2, &s2);
    card1 = gui_value(p, LANE_ID(k, 8));
    printf("gui preview CLOSE: lane 2 %d.%d, HOW MANY %g, card 1 %g (was %d.%d, 0, %g)\n", f2, s2, gui_value(p, LANE_ID(k, 1)), card1,
           f0, s0, card0);
    if (f2 != f0 || s2 != s0 || gui_value(p, LANE_ID(k, 1)) != 0.0 || card1 != card0) { fprintf(stderr, "gui: CLOSE did not put lane 2 back\n"); bad = 1; }
    /* KEEP: the sound kept, the lane still stopped */
    if (!gui_hit(p, xs, W_SND, k)) return 5;
    if (!gui_hit(p, xs, W_LIST, s0 == 3 ? 4 : 3)) return 5;
    gui_wait(); gui_flush(p); xs->tick(p);
    if (!gui_hit(p, xs, W_KEEP, -1)) return 5;
    gui_wait(); gui_flush(p); xs->tick(p);
    (void)xs->lane(p, k, &f2, &s2);
    cardb = gui_value(p, LANE_ID(k, 1));
    printf("gui preview KEEP: lane 2 %d.%d, HOW MANY %g\n", f2, s2, cardb);
    if (f2 != f1 || s2 != s1 || cardb != 0.0) { fprintf(stderr, "gui: KEEP did not keep the sound on the stopped lane\n"); bad = 1; }
    /* PLAY ALL: lane 2 back at its own HOW MANY / HOW ACTIVE (what STOP ALL remembered survived the preview) */
    if (!gui_hit(p, xs, W_REC, -1)) return 5;
    gui_wait(); gui_flush(p); xs->tick(p);
    printf("gui preview: PLAY ALL, lane 2 MANY %g / ACTIVE %g (was %g / %g)\n", gui_value(p, LANE_ID(k, 1)), gui_value(p, LANE_ID(k, 2)), m0, a0);
    if (gui_value(p, LANE_ID(k, 1)) != m0 || gui_value(p, LANE_ID(k, 2)) != a0) { fprintf(stderr, "gui: PLAY ALL after the preview\n"); bad = 1; }
    return bad;
}
static int gui_test5c_rows(const clap_plugin_t *p, const txg_gui_shot_t *xs)   /* a click anywhere on a row selects its
                                          * lane, its name does not open the browser, CHANGE does */
{
    int bad = 0, sel;
    float r[4], n[4];
    if (!xs->find(p, W_ROW, 3, r) || !xs->find(p, W_SND, 3, n)) { fprintf(stderr, "gui: no row / CHANGE for lane 4\n"); return 5; }
    gui_click(p, xs, n[0] - 60.f, r[1] + r[3] * 0.3f, 0);   /* (on its name) */
    gui_wait(); gui_flush(p); xs->tick(p);
    sel = xs->sel(p);
    printf("gui row: a click on lane 4's name selects lane %d, the browser %s\n", sel + 1, xs->find(p, W_KEEP, -1, n) ? "OPEN" : "closed");
    if (sel != 3 || xs->find(p, W_KEEP, -1, n)) { fprintf(stderr, "gui: the name opened the browser or did not select\n"); bad = 1; }
    if (!xs->find(p, W_ROW, 2, r) || !xs->find(p, W_SND, 2, n)) return 5;
    gui_click(p, xs, n[0] + n[2] + 24.f, r[1] + r[3] * 0.5f, 0);   /* (on its event line, past CHANGE: no control) */
    gui_wait(); gui_flush(p); xs->tick(p);
    printf("gui row: a click on lane 3's event line selects lane %d\n", xs->sel(p) + 1);
    if (xs->sel(p) != 2) { fprintf(stderr, "gui: the row's space did not select\n"); bad = 1; }
    if (!gui_hit(p, xs, 3 /* TXG_W_MUTE */, 1)) return 5;   /* (M on lane 2: it selects lane 2 too; M again) */
    gui_wait(); gui_flush(p); xs->tick(p);
    sel = xs->sel(p);
    if (!gui_hit(p, xs, 3, 1)) return 5;
    printf("gui row: M on lane 2 selects lane %d\n", sel + 1);
    if (sel != 1) { fprintf(stderr, "gui: M did not select its lane\n"); bad = 1; }
    if (!gui_hit(p, xs, W_SND, 3)) return 5;   /* CHANGE: the browser */
    printf("gui row: CHANGE on lane 4 opens the browser: %s\n", xs->find(p, W_KEEP, -1, n) ? "yes" : "NO");
    if (!xs->find(p, W_KEEP, -1, n)) { fprintf(stderr, "gui: CHANGE did not open the browser\n"); bad = 1; }
    gui_key(p, xs, FRK_ESCAPE);
    return bad;
}
static int gui_test3b(const clap_plugin_t *p, const txg_gui_shot_t *xs)   /* the browsers, SETTINGS, a colour, SPACE */
{
    int bad = 0, f0 = -1, s0 = -1, f1 = -1, s1 = -1;
    double v0, v1;
    float r[4];
    /* the presets browser: the next bank's first preset loaded (PRESET follows a factory one) */
    v0 = gui_value(p, ID_PRESET);
    if (!gui_hit(p, xs, W_PRESET, -1)) return 5;
    gui_key(p, xs, FRK_RIGHT);
    gui_key(p, xs, FRK_RETURN);
    gui_key(p, xs, FRK_ESCAPE);
    gui_wait(); gui_flush(p); xs->tick(p);
    v1 = gui_value(p, ID_PRESET);
    printf("gui presets: PRESET %g -> %g\n", v0, v1);
    if (v1 == v0) { fprintf(stderr, "gui: the presets browser did not load\n"); bad = 1; }
    /* the sound browser on lane 1: the next family's first sound kept */
    if (!xs->lane(p, 0, &f0, &s0)) { fprintf(stderr, "gui: lane 1 empty\n"); return 5; }
    if (!gui_hit(p, xs, W_SND, 0)) return 5;
    gui_key(p, xs, FRK_RIGHT);
    gui_key(p, xs, FRK_RETURN);
    gui_wait(); gui_flush(p); xs->tick(p);
    (void)xs->lane(p, 0, &f1, &s1);
    printf("gui sounds: lane 1 %d.%d -> %d.%d\n", f0, s0, f1, s1);
    if (f1 == f0 || s1 != 0) { fprintf(stderr, "gui: the sound browser did not set the sound\n"); bad = 1; }
    /* SETTINGS: lane 2's channel one up, EFFECTS ON LOAD set to NEVER */
    v0 = gui_value(p, 4u << 8 | 63u);
    if (!gui_hit(p, xs, W_SET, -1)) return 5;
    if (!gui_hit(p, xs, W_FIELD, 0x400 | 63)) return 5;
    if (!gui_hit(p, xs, W_SEG, (0x400 | 38) << 8 | 2)) return 5;
    gui_key(p, xs, FRK_ESCAPE);
    v1 = gui_value(p, 4u << 8 | 63u);
    printf("gui settings: lane 2 channel %g -> %g, EFFECTS ON LOAD %g\n", v0, v1, gui_value(p, 4u << 8 | 38u));
    if (v1 == v0 || gui_value(p, 4u << 8 | 38u) != 2.0) { fprintf(stderr, "gui: SETTINGS\n"); bad = 1; }
    /* the colour picker: lane 1's engine TEAL (6; in the saved state) */
    if (xs->find(p, W_ROW, 0, r)) gui_click(p, xs, r[0] + 30.f, r[1] + r[3] * 0.5f, 0);
    if (!gui_hit(p, xs, W_COLDOT, 0)) return 5;
    if (!gui_hit(p, xs, W_SWATCH, 6)) return 5;
    gui_key(p, xs, FRK_ESCAPE);
    /* SPACE: lane 1 dragged left on the ground */
    if (!gui_hit(p, xs, W_VSPACE, -1) || !xs->find(p, W_DOT, 0, r)) return 5;
    v0 = gui_value(p, LANE_ID(0, 4));
    {
        frg_input e;
        memset(&e, 0, sizeof e);
        e.kind = FRG_DOWN; e.button = FRG_LEFT; e.x = r[0] + r[2] * 0.5f; e.y = r[1] + r[3] * 0.5f;
        xs->input(p, &e);
        e.kind = FRG_MOVE; e.x -= 60.f; e.y += 10.f;
        xs->input(p, &e);
        e.kind = FRG_UP;
        xs->input(p, &e);
        gui_flush(p); xs->tick(p);
    }
    v1 = gui_value(p, LANE_ID(0, 4));
    printf("gui SPACE: lane 1 PAN %g -> %g\n", v0, v1);
    if (!(v1 < v0)) { fprintf(stderr, "gui: the SPACE drag did not move PAN\n"); bad = 1; }
    return bad;
}
static int gui_shot(const clap_plugin_t *p, const char *png, int pw, int ph, int test)
{
    const txg_gui_shot_t *xs = p->get_extension(p, TXG_EXT_GUI_SHOT);
    const uint32_t *px;
    int w, h, bad = 0;
    uint32_t i, ncol = 0, ink = 0;
    static uint8_t seen[1u << 21];       /* (24-bit colours, a bit each) */
    if (!xs || !xs->headless(p, pw, ph)) { fprintf(stderr, "gui: no headless window\n"); return 5; }
    xs->tick(p);
    if (!xs->grab(p, 0, &px, &w, &h)) { fprintf(stderr, "gui: nothing painted\n"); return 5; }
    for (i = 0; i < (uint32_t)(w * h); i++) {
        uint32_t c = px[i] & 0xFFFFFFu;
        if (!(seen[c >> 3] >> (c & 7) & 1)) { seen[c >> 3] |= (uint8_t)(1u << (c & 7)); ncol++; }
        if ((px[i] >> 16 & 255u) > 200u && (px[i] >> 8 & 255u) > 190u && (px[i] & 255u) > 180u) ink++;
    }
    printf("gui %d x %d, %u colours, %u bright pixels\n", w, h, ncol, ink);
    if (png && png_write(png, px, w, h)) return 5;
    if (ncol < 40u || ink < (uint32_t)(w * h) / 400u) { fprintf(stderr, "gui: the window looks empty\n"); return 5; }
    {   /* the MAP at this size: a half circle (one scale both ways: round), its L R FAR YOU inside the place */
        float pl[4], mr[4];
        double sx, sy;
        if (!xs->find(p, W_PLACE, -1, pl) || !xs->find(p, W_MAPR, -1, mr)) { fprintf(stderr, "gui: no place / map\n"); return 5; }
        sx = (double)w / 1200.0; sy = (double)h / 720.0;
        sx = sx < sy ? sx : sy;          /* (the window's scale: canvas pixels a logical pixel, both ways) */
        printf("gui map: radius %.0f px, round (%.0f x %.0f px), in the place %.0f x %.0f\n", mr[3] * sx, mr[2] * sx,
               mr[3] * sx * 2.0, pl[2] * sx, pl[3] * sx);
        if (mr[2] < 2.f * mr[3] - 0.5f || mr[2] > 2.f * mr[3] + 0.5f || mr[3] < 100.f || mr[0] - 20.f < pl[0] ||
            mr[0] + mr[2] + 20.f > pl[0] + pl[2] || mr[1] - 20.f < pl[1] + 46.f || mr[1] + mr[3] + 30.f > pl[1] + pl[3]) {
            fprintf(stderr, "gui: the map is not round inside the place with its labels\n");
            return 5;
        }
    }
    if (test) {                          /* the compressor's meters: live off the render's end (COMP on in the gate's run) */
        int in10 = -900, out10 = -900, gr10 = 0;
        float r[4];
        if (!xs->comp(p, &in10, &out10, &gr10)) return 5;
        printf("gui COMP meters: in %.1f dB, out %.1f dB, gain reduction %.1f dB\n", in10 / 10.0, out10 / 10.0, gr10 / 10.0);
        if (gui_value(p, 4u << 8 | 26u) > 0.5 && (in10 <= -900 || out10 <= -900 || gr10 <= 0)) {
            fprintf(stderr, "gui: the compressor's meters are not fed\n");
            bad = 1;
        }
        if (!xs->find(p, W_CMETER, -1, r)) { fprintf(stderr, "gui: no compressor meter in the strip\n"); bad = 1; }
    }
    if (png && gui_views(p, xs, png)) return 5;
    if (test) {                          /* the controls do what they say (inactive: each write applied at its flush) */
        float r[4];
        double v0, v1;
        uint32_t m = 0, s = 0, lanes0 = (uint32_t)gui_value(p, 4u << 8 | 81u);
        n_out[0] = n_out[1] = n_out[2] = 0;
        if (!xs->find(p, 3 /* TXG_W_MUTE */, 1, r)) { fprintf(stderr, "gui: no M on lane 2\n"); return 5; }
        gui_click(p, xs, r[0] + r[2] * 0.5f, r[1] + r[3] * 0.5f, 0);
        if (!xs->find(p, 4 /* TXG_W_SOLO */, 2, r)) { fprintf(stderr, "gui: no S on lane 3\n"); return 5; }
        gui_click(p, xs, r[0] + r[2] * 0.5f, r[1] + r[3] * 0.5f, 0);
        gui_wait(); xs->tick(p);
        xs->masks(p, &m, &s);
        printf("gui mute %#x solo %#x\n", m, s);
        if (!(m & 2u) || s != 4u) { fprintf(stderr, "gui: M / S did not reach the app\n"); bad = 1; }
        if (!xs->find(p, 2 /* TXG_W_VOL */, 0, r)) { fprintf(stderr, "gui: no VOL on lane 1\n"); return 5; }
        gui_click(p, xs, r[0] + 4.f + (r[2] - 8.f) * 0.25f, r[1] + r[3] * 0.5f, 0);
        v1 = gui_value(p, LANE_ID(0, 3));
        printf("gui VOL: lane 1 LEVEL %g (events to the host: %u begin, %u values, %u end)\n", v1, n_out[0], n_out[1], n_out[2]);
        if (v1 < 31.0 || v1 > 33.0 || !n_out[0] || n_out[0] != n_out[2] || !n_out[1]) { fprintf(stderr, "gui: VOL\n"); bad = 1; }
        if (!xs->find(p, 5 /* TXG_W_DOT */, 0, r)) { fprintf(stderr, "gui: no dot for lane 1\n"); return 5; }
        v0 = gui_value(p, LANE_ID(0, 4));
        {
            frg_input e;
            memset(&e, 0, sizeof e);
            e.kind = FRG_DOWN; e.button = FRG_LEFT; e.x = r[0] + r[2] * 0.5f; e.y = r[1] + r[3] * 0.5f;
            xs->input(p, &e);
            e.kind = FRG_MOVE; e.x -= 70.f; e.y -= 30.f;
            xs->input(p, &e);
            e.kind = FRG_UP;
            xs->input(p, &e);
            gui_flush(p); xs->tick(p);
        }
        v1 = gui_value(p, LANE_ID(0, 4));
        printf("gui MAP: lane 1 PAN %g -> %g\n", v0, v1);
        if (!(v1 < v0)) { fprintf(stderr, "gui: the map's drag did not move PAN\n"); bad = 1; }
        if (!xs->find(p, 9 /* TXG_W_LUP */, -1, r)) { fprintf(stderr, "gui: no LANES +\n"); return 5; }
        gui_click(p, xs, r[0] + r[2] * 0.5f, r[1] + r[3] * 0.5f, 0);
        v1 = gui_value(p, 4u << 8 | 81u);
        printf("gui LANES %u -> %g\n", lanes0 + 1u, v1 + 1.0);
        if (lanes0 < 15u && v1 != lanes0 + 1.0) { fprintf(stderr, "gui: LANES +\n"); bad = 1; }
        if (gui_test3b(p, xs)) bad = 1;
        if (gui_test5(p, xs)) bad = 1;
        if (gui_test5b(p, xs)) bad = 1;
        if (gui_test5c_drawer(p, xs)) bad = 1;
        if (gui_test5c_preview(p, xs)) bad = 1;
        if (gui_test5c_rows(p, xs)) bad = 1;
        if (xs->find(p, 1 /* TXG_W_ROW */, 2, r)) gui_click(p, xs, r[0] + 30.f, r[1] + r[3] * 0.5f, 0);   /* (lane 3 selected) */
    }
    return bad ? 5 : 0;
}

static const clap_plugin_t *make(const clap_plugin_factory_t *fac)
{
    const clap_plugin_descriptor_t *d = fac->get_plugin_descriptor(fac, 0);
    const clap_plugin_t *p = d ? fac->create_plugin(fac, &HOST, d->id) : 0;
    if (!p || !p->init(p)) { fprintf(stderr, "the plugin did not start\n"); exit(2); }
    return p;
}

int main(int argc, char **argv)
{
    const char *plug, *script, *out;
    double rate = 44100.0;
    uint32_t block = 512, keep = 0, state_check = 0, i, lat, total, nproc = 0, ei = 0, cpu_from = 0, fuzz = 0, inactive = 0;
    const char *state_in = 0, *state_out = 0, *shot = 0;
    int shot_w = 1200, shot_h = 0, gui_test = 0, list = 0;
    uint32_t set_id[16], np_set = 0, get_id[16], n_get = 0;
    double set_v[16];
    char bin[1024];
    void *so;
    const clap_plugin_entry_t *entry;
    const clap_plugin_factory_t *fac;
    const clap_plugin_t *p;
    const clap_plugin_latency_t *xl;
    float *buf, *l, *r;
    double t_sum = 0, t_max = 0;
    static evl_t evl;
    clap_input_events_t in = { &evl, el_size, el_get };
    clap_output_events_t on = { 0, ol_push };
    if (argc < 4) { fprintf(stderr, "usage: txp_clap_render PLUGIN.clap SCRIPT OUT.wav [--rate HZ] [--block N] [--keep-latency] [--state-check] [--cpu-from MS] [--fuzz SEED] [--inactive] [--state-in FILE] [--state-out FILE]\n"); return 1; }
    plug = argv[1]; script = argv[2]; out = argv[3];
    for (i = 4; i < (uint32_t)argc; i++) {
        if (!strcmp(argv[i], "--rate") && i + 1 < (uint32_t)argc) rate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--block") && i + 1 < (uint32_t)argc) block = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cpu-from") && i + 1 < (uint32_t)argc) cpu_from = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fuzz") && i + 1 < (uint32_t)argc) fuzz = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--inactive")) inactive = 1;
        else if (!strcmp(argv[i], "--state-in") && i + 1 < (uint32_t)argc) state_in = argv[++i];
        else if (!strcmp(argv[i], "--state-out") && i + 1 < (uint32_t)argc) state_out = argv[++i];
        else if (!strcmp(argv[i], "--shot") && i + 1 < (uint32_t)argc) shot = argv[++i];
        else if (!strcmp(argv[i], "--shot-w") && i + 1 < (uint32_t)argc) shot_w = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gui-test")) gui_test = 1;
        else if (!strcmp(argv[i], "--keep-latency")) keep = 1;
        else if (!strcmp(argv[i], "--state-check")) state_check = 1;
        else if (!strcmp(argv[i], "--shot-h") && i + 1 < (uint32_t)argc) shot_h = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--list")) list = 1;
        else if (!strcmp(argv[i], "--get") && i + 1 < (uint32_t)argc && n_get < 16u) get_id[n_get++] = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--param") && i + 1 < (uint32_t)argc && np_set < 16u) {
            char *eq = strchr(argv[++i], '=');
            if (eq) { set_id[np_set] = (uint32_t)strtoul(argv[i], 0, 0); set_v[np_set++] = atof(eq + 1); }
        }
    }
    if (block < 1 || block > 8192) block = 512;
    read_script(script);
#ifdef __APPLE__
    {   /* a bundle: Contents/MacOS/<name> */
        const char *s = strrchr(plug, '/'), *nm = s ? s + 1 : plug;
        size_t k = strlen(nm);
        if (k > 5 && !strcmp(nm + k - 5, ".clap")) snprintf(bin, sizeof bin, "%s/Contents/MacOS/%.*s", plug, (int)(k - 5), nm);
        else snprintf(bin, sizeof bin, "%s", plug);
    }
#else
    snprintf(bin, sizeof bin, "%s", plug);
#endif
    if (shot) {                          /* (the plugin offers its test extension only so; set before
                                          * the load: a Windows DLL's static CRT copies the environment then) */
#ifdef _WIN32
        _putenv_s("TEXTURA_PLUGIN_GUI_SHOT", "1");
#else
        setenv("TEXTURA_PLUGIN_GUI_SHOT", "1", 1);
#endif
        if (shot_w < 400 || shot_w > 4800) shot_w = 1200;
    }
    if (!(so = dlopen(bin, RTLD_NOW | RTLD_LOCAL))) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    entry = dlsym(so, "clap_entry");
    if (!entry || !entry->init(plug)) { fprintf(stderr, "no clap_entry\n"); return 2; }
    fac = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    p = make(fac);
    if (list) {                          /* the host parameters: id, range, module, name */
        const clap_plugin_params_t *xp = p->get_extension(p, CLAP_EXT_PARAMS);
        for (i = 0; i < xp->count(p); i++) {
            clap_param_info_t inf;
            xp->get_info(p, i, &inf);
            printf("%#x\t%g\t%s\t%s\n", inf.id, inf.max_value, inf.module, inf.name);
        }
        p->destroy(p);
        return 0;
    }
    if (state_in) {                      /* a project's state, before anything else */
        const clap_plugin_state_t *xs = p->get_extension(p, CLAP_EXT_STATE);
        mem_t m = {0, 0, 0};
        clap_istream_t is = { &m, is_read };
        FILE *f = fopen(state_in, "rb");
        char tmp[4096];
        size_t k;
        if (!f) { perror(state_in); return 1; }
        while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) { m.b = realloc(m.b, m.n + k); memcpy(m.b + m.n, tmp, k); m.n += k; }
        fclose(f);
        if (!xs || !xs->load(p, &is)) { fprintf(stderr, "state: %s did not load\n", state_in); return 3; }
        free(m.b);
    }
    if (fuzz) {                          /* every automatable parameter at a random value, before the activation (a
                                          * flush on the main thread: as clap-validator's state tests do) */
        const clap_plugin_params_t *xp = p->get_extension(p, CLAP_EXT_PARAMS);
        static clap_event_param_value_t pe[1024];
        uint32_t np = xp->count(p), k = 0, x = fuzz * 2654435761u;
        clap_input_events_t pin = { pe, 0, 0 };
        for (i = 0; i < np && k < 1024u; i++) {
            clap_param_info_t inf;
            xp->get_info(p, i, &inf);
            if (!(inf.flags & CLAP_PARAM_IS_AUTOMATABLE)) continue;
            x = x * 1664525u + 1013904223u;
            memset(&pe[k], 0, sizeof pe[k]);
            pe[k].header.size = sizeof pe[k]; pe[k].header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            pe[k].header.type = CLAP_EVENT_PARAM_VALUE;
            pe[k].param_id = inf.id; pe[k].note_id = -1; pe[k].port_index = -1; pe[k].channel = -1; pe[k].key = -1;
            pe[k].value = floor((double)(x >> 8) / 16777216.0 * (inf.max_value + 1.0));
            k++;
        }
        fz_n = k;
        pin.size = fz_size; pin.get = fz_get;
        xp->flush(p, &pin, &on);
    }
    if (np_set) {                        /* --param ID=V: set before the activation (a flush on the main thread) */
        const clap_plugin_params_t *xp = p->get_extension(p, CLAP_EXT_PARAMS);
        static clap_event_param_value_t pe[16];
        clap_input_events_t pin = { pe, 0, 0 };
        for (i = 0; i < np_set; i++) {
            memset(&pe[i], 0, sizeof pe[i]);
            pe[i].header.size = sizeof pe[i]; pe[i].header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            pe[i].header.type = CLAP_EVENT_PARAM_VALUE;
            pe[i].param_id = set_id[i]; pe[i].note_id = -1; pe[i].port_index = -1; pe[i].channel = -1; pe[i].key = -1;
            pe[i].value = set_v[i];
        }
        fz_n = np_set;
        pin.size = fz_size; pin.get = fz_get;
        xp->flush(p, &pin, &on);
    }
    if (inactive) goto states;           /* (--inactive: never activated, no render: the state check alone) */
    if (!p->activate(p, rate, 1, block)) { fprintf(stderr, "activate refused at %.0f Hz\n", rate); return 2; }
    p->start_processing(p);
    xl = p->get_extension(p, CLAP_EXT_LATENCY);
    lat = xl ? xl->get(p) : 0;
    total = (uint32_t)ceil(end_ms * rate / 1000.0) + (keep ? 0 : lat);
    buf = calloc((size_t)total + block, 8);
    l = calloc(block, 4); r = calloc(block, 4);
    for (uint32_t at = 0; at < total; at += block) {
        uint32_t n = total - at < block ? total - at : block;
        clap_process_t pr;
        clap_audio_buffer_t ob;
        float *ch[2] = { l, r };
        double t0;
        evl.n = 0;
        /* an event at T ms: the host frame ceil(T x rate / 1000), the first whose time is T ms or later */
        while (ei < nev && evl.n < 512u) {
            uint32_t h = (uint32_t)ceil((double)ev[ei].t * rate / 1000.0 - 1e-9);
            clap_event_midi_t *e;
            if (h >= at + n) break;
            e = &evl.e[evl.n++];
            memset(e, 0, sizeof *e);
            e->header.size = sizeof *e; e->header.space_id = CLAP_CORE_EVENT_SPACE_ID; e->header.type = CLAP_EVENT_MIDI;
            e->header.time = h < at ? 0 : h - at;
            e->data[0] = ev[ei].st; e->data[1] = ev[ei].d1; e->data[2] = ev[ei].d2;
            ei++;
        }
        memset(&ob, 0, sizeof ob);
        ob.data32 = ch; ob.channel_count = 2;
        memset(&pr, 0, sizeof pr);
        pr.steady_time = at; pr.frames_count = n;
        pr.audio_outputs = &ob; pr.audio_outputs_count = 1;
        pr.in_events = &in; pr.out_events = &on;
        t0 = now_us();
        if (p->process(p, &pr) == CLAP_PROCESS_ERROR) { fprintf(stderr, "process error\n"); return 2; }
        t0 = now_us() - t0;
        if ((double)at >= cpu_from * rate / 1000.0) { t_sum += t0; if (t0 > t_max) t_max = t0; nproc++; }
        for (i = 0; i < n; i++) { buf[2u * (at + i)] = l[i]; buf[2u * (at + i) + 1u] = r[i]; }
    }
    p->stop_processing(p);
    p->deactivate(p);
    wav_write(out, buf + (keep ? 0 : 2u * lat), total - (keep ? 0 : lat), (uint32_t)rate);
    {
        double dur = block * 1e6 / rate;
        printf("latency %u frames\n", lat);
        printf("cpu %u calls of %u frames at %.0f Hz: average %.1f us (%.2f%%), worst %.1f us (%.2f%%)\n", nproc, block,
               rate, t_sum / (nproc ? nproc : 1), 100.0 * t_sum / (nproc ? nproc : 1) / dur, t_max, 100.0 * t_max / dur);
    }
states:
    if (shot) {
        int rc = gui_shot(p, shot, shot_w, shot_h > 0 ? shot_h : shot_w * 3 / 5, gui_test);
        if (!rc && gui_test) {           /* the window's writes in the saved state: the solo, the selected lane */
            const clap_plugin_state_t *xs = p->get_extension(p, CLAP_EXT_STATE);
            mem_t m = {0, 0, 0};
            clap_ostream_t os = { &m, os_write };
            if (!xs || !xs->save(p, &os)) { fprintf(stderr, "gui: state save failed\n"); return 5; }
            m.b = realloc(m.b, m.n + 1); m.b[m.n] = 0;
            printf("gui state: solo %s, sel %s, view %s, colour %s, keys %s, user U01 %s\n", strstr(m.b, "\"solo\":\"4\"") ? "saved" : "MISSING",
                   strstr(m.b, "\"sel\":\"2\"") ? "saved" : "MISSING", strstr(m.b, "\"view\":\"1\"") ? "saved" : "MISSING",
                   strstr(m.b, ":6;") ? "saved" : "MISSING", strstr(m.b, "\"keys\":\"1\"") ? "saved" : "MISSING",
                   strstr(m.b, "\"umask\":\"1\"") ? "saved" : "MISSING");
            if (!strstr(m.b, "\"solo\":\"4\"") || !strstr(m.b, "\"sel\":\"2\"") || !strstr(m.b, "\"view\":\"1\"") ||
                !strstr(m.b, ":6;") || !strstr(m.b, "\"keys\":\"1\"") || !strstr(m.b, "\"umask\":\"1\""))
                rc = 5;
            free(m.b);
        }
        if (rc) { p->destroy(p); return rc; }
        printf("gui ok\n");
    }
    for (i = 0; i < n_get; i++) printf("param %#x = %g\n", get_id[i], gui_value(p, get_id[i]));   /* --get ID: a value at the end */
    if (state_out) {
        const clap_plugin_state_t *xs = p->get_extension(p, CLAP_EXT_STATE);
        mem_t m = {0, 0, 0};
        clap_ostream_t os = { &m, os_write };
        FILE *f;
        if (!xs || !xs->save(p, &os) || !(f = fopen(state_out, "wb"))) { fprintf(stderr, "state: save failed\n"); return 3; }
        fwrite(m.b, 1, m.n, f);
        fclose(f);
        free(m.b);
    }
    if (state_check) {
        const clap_plugin_state_t *xs = p->get_extension(p, CLAP_EXT_STATE);
        const clap_plugin_params_t *xp = p->get_extension(p, CLAP_EXT_PARAMS);
        mem_t m = {0, 0, 0}, m2 = {0, 0, 0};
        clap_ostream_t os = { &m, os_write }, os2 = { &m2, os_write };
        clap_istream_t is = { &m, is_read };
        const clap_plugin_t *q;
        uint32_t np, bad = 0, same;
        if (!xs || !xp || !xs->save(p, &os)) { fprintf(stderr, "state: save failed\n"); return 3; }
        q = make(fac);
        if (!xs->load(q, &is)) { fprintf(stderr, "state: load failed\n"); return 3; }
        if (!xs->save(q, &os2)) { fprintf(stderr, "state: second save failed\n"); return 3; }
        same = m.n == m2.n && !memcmp(m.b, m2.b, m.n);
        if (!same) {                     /* (both kept beside the output, to compare) */
            char sp[1100];
            FILE *f;
            snprintf(sp, sizeof sp, "%s.state1", out);
            if ((f = fopen(sp, "wb"))) { fwrite(m.b, 1, m.n, f); fclose(f); }
            snprintf(sp, sizeof sp, "%s.state2", out);
            if ((f = fopen(sp, "wb"))) { fwrite(m2.b, 1, m2.n, f); fclose(f); }
        }
        np = xp->count(p);
        for (i = 0; i < np; i++) {
            clap_param_info_t inf;
            double a = 0, b = 0;
            xp->get_info(p, i, &inf);
            xp->get_value(p, inf.id, &a);
            xp->get_value(q, inf.id, &b);
            if (a == b) continue;
            if (bad < 10) fprintf(stderr, "state: %s %g -> %g\n", inf.name, a, b);
            bad++;
        }
        printf("state %zu bytes, saved again %s, %u parameters, %u differ\n", m.n, same ? "the same" : "DIFFERENT", np, bad);
        q->destroy(q);
        free(m.b); free(m2.b);
        if (bad || !same) { p->destroy(p); return bad ? 3 : 4; }   /* (4: the values the same, the saved text not) */
    }
    p->destroy(p);
    entry->deinit();
    return 0;
}
