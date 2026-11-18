/* SPDX-License-Identifier: GPL-3.0-only */
/* A headless CLAP host for the plugin's tests: loads a .clap, plays a host-build script's MIDI lines through it
 * (tests/host.c's format: "T midi ST D1 D2", "T end"; every other line is ignored) and writes the output as a 32-bit
 * float WAV, the latency's frames dropped (the output starts at the app's own frame 0).
 *
 *   txp_clap_render PLUGIN.clap SCRIPT OUT.wav [--rate HZ] [--block N] [--keep-latency] [--state-check] [--cpu-from MS]
 *
 * --rate     the host's rate (44100 by default); --block  frames a process call (512)
 * --fuzz SEED  before the activation, every automatable parameter at a random value (one flush)
 * --inactive  no activation and no render (with --state-check: the state of an instance never activated)
 * --cpu-from MS  time only the process calls from that millisecond on (after a scene's load)
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
static bool ol_push(const clap_output_events_t *l, const clap_event_header_t *e) { (void)l; (void)e; return true; }

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
    if (argc < 4) { fprintf(stderr, "usage: txp_clap_render PLUGIN.clap SCRIPT OUT.wav [--rate HZ] [--block N] [--keep-latency] [--state-check] [--cpu-from MS] [--fuzz SEED] [--inactive]\n"); return 1; }
    plug = argv[1]; script = argv[2]; out = argv[3];
    for (i = 4; i < (uint32_t)argc; i++) {
        if (!strcmp(argv[i], "--rate") && i + 1 < (uint32_t)argc) rate = atof(argv[++i]);
        else if (!strcmp(argv[i], "--block") && i + 1 < (uint32_t)argc) block = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--cpu-from") && i + 1 < (uint32_t)argc) cpu_from = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fuzz") && i + 1 < (uint32_t)argc) fuzz = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--inactive")) inactive = 1;
        else if (!strcmp(argv[i], "--keep-latency")) keep = 1;
        else if (!strcmp(argv[i], "--state-check")) state_check = 1;
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
    if (!(so = dlopen(bin, RTLD_NOW | RTLD_LOCAL))) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    entry = dlsym(so, "clap_entry");
    if (!entry || !entry->init(plug)) { fprintf(stderr, "no clap_entry\n"); return 2; }
    fac = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    p = make(fac);
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
