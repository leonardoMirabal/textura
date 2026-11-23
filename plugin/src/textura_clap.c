/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura as a CLAP instrument: the entry, the factory and the plugin. clap-wrapper builds the VST3 and the Audio
 * Unit (v2) from the same code (plugin/CMakeLists.txt, make_clapfirst_plugins). From Frodhes' plugin/src/frodhes_clap.c,
 * without its update checker, with its own window (plugin/gui_clap.c, plugin/gui_core.c) over its own calls for the
 * window (plugin/src/txp_ui.h, at the end of this file).
 *
 *   audio-ports   one stereo output, no input
 *   note-ports    one input, CLAP and MIDI dialects: notes, CC, pitch bend, program change, as the FM-1 takes them
 *                 over USB MIDI (lane n answers on channel n, the effects and settings on channel 5, the selected lane
 *                 on channel 1's notes: docs/MIDI.md); a CLAP note's channel is the MIDI channel
 *   params        the app's own parameters by their stable ids (plugin/src/slot_bridge.h sb_params) and the plugin's
 *                 PRESET (F001 .. F100); a change the app makes itself (a MIDI CC, a program change, a scene load)
 *                 goes back to the host as a parameter output event
 *   state         the scene the FM-1's user bank keeps, and the settings that are parameters (slot_bridge.h)
 *   latency       the bridge's (plugin/src/bridge.c): one app block + the oversampler, constant per rate
 *   tail          a fixed bound (TAIL_MS): the reverb's longest decay and the grain delay's feedback
 *   gui           the window (macOS: an NSView; plugin/gui_win.c and gui_x11.c are the other systems', not built yet);
 *                 its writes reach the app and the host through the UI queue (uq) at the next process call or flush
 * Sample rates: 44.1 kHz (the app's own), the 44.1 and 48 kHz families through an oversampler, any other from 1 to
 * 1536 kHz through a windowed sinc (bridge.c); outside that the instance refuses to activate and logs why. More than
 * TXP_SLOTS (16) instances in one process: the extra ones play silence and log why. */
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <clap/clap.h>
#include "../gui_size.h"
#include "bridge.h"
#include "txp_ui.h"
#include "../gui_clap.h"
#include "../gui_shot.h"

#ifndef TXP_VERSION
#define TXP_VERSION "0.0.0-dev"
#endif
#define TXP_PLUGIN_ID "io.github.leonardomirabal.textura"
#define TAIL_MS 30000u                   /* the reverb at its longest and a long grain-delay feedback (an upper bound) */

static const char *const FEATURES[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
                                        CLAP_PLUGIN_FEATURE_STEREO, 0 };
static const clap_plugin_descriptor_t DESC = {
    .clap_version = CLAP_VERSION_INIT,
    .id = TXP_PLUGIN_ID,
    .name = "Textura",
    .vendor = "Textura",
    .url = "https://github.com/leonardoMirabal/textura",
    .manual_url = "https://github.com/leonardoMirabal/textura/blob/main/docs/PLUGIN.md",
    .support_url = "https://github.com/leonardoMirabal/textura/issues",
    .version = TXP_VERSION,
    .description = "Physically modelled nature sounds: water, rain, fire, wind, insects",
    .features = FEATURES,
};

#define UQ 1024u                         /* the window's writes waiting for the audio side (a power of two) */
enum { UQ_BEGIN, UQ_VALUE, UQ_END, UQ_EV };   /* UQ_EV + k: the slot's event TXP_EV_* k (bridge.h), id its a */
typedef struct { uint8_t type; uint32_t id; int32_t v; } uq_ev_t;
#define PEND_S 0.3                       /* the window's own value wins this long after its write (then the app's) */

typedef struct txp_plugin {
    clap_plugin_t plugin;
    const clap_host_t *host;
    const clap_host_log_t *log;
    const clap_host_params_t *hparams;
    const clap_host_latency_t *hlat;
    txp_inst_t *in;
    const txp_param_t *tab;
    uint32_t np;
    int active;
    double rate;                         /* the activation's (0: not yet) */
    double *host_view;                   /* the audio thread's view of what the host believes */
    double *def;                         /* a fresh instance's values (the boot's scene) */
    txp_snap_t last;
    int have_last;
    _Atomic uint32_t rescan_rev;         /* a state loaded while active: re-read the values once the app took it */
    _Atomic int rescan_tries;
    _Atomic int processing;
    /* the window: its own pointer (plugin/gui_clap.c), its writes on their way (main thread in, audio thread or flush
     * out), what it wrote last until the app shows it, its width and selected lane (saved with the project) */
    const clap_host_state_t *hstate;
    void *gui;
    uq_ev_t uq[UQ];
    _Atomic uint32_t uqw, uqr;
    double *pend_t;
    int32_t *pend_v;
    double pend_mt[TXP_LANES], pend_st, pend_selt;
    uint8_t pend_m[TXP_LANES];
    uint16_t pend_s;
    int win_w, win_h, sel, view;                /* (sel: the window's last choice, until the app shows it: pend_selt) */
    uint8_t col[TXP_ENG_MAX];            /* each engine's colour (txp_ui.h TXP_COL_N: 0 bone), saved with the project */
    int shot;                            /* TEXTURA_PLUGIN_GUI_SHOT set: the tests' extension (gui_shot.h) */
    int dice, dice_fixed;                /* RANDOM's groups ticked (dice.c DICE_*: saved with the project); the tests'
                                          * fixed seeds (TEXTURA_PLUGIN_TEST or TEXTURA_PLUGIN_GUI_SHOT set) */
    uint32_t dice_n;                     /* the rolls so far (the tests' seed) */
} txp_plugin_t;

static void say(txp_plugin_t *p, clap_log_severity sev, const char *msg)
{
    if (p->log && p->log->log) p->log->log(p->host, sev, msg);
}

/* ---- parameters */
static uint32_t p_count(const clap_plugin_t *plugin) { return ((txp_plugin_t *)plugin->plugin_data)->np; }
static bool p_info(const clap_plugin_t *plugin, uint32_t index, clap_param_info_t *info)
{
    const txp_plugin_t *p = plugin->plugin_data;
    const txp_param_t *d;
    if (index >= p->np) return false;
    d = &p->tab[index];
    memset(info, 0, sizeof *info);
    info->id = d->id;
    info->flags = CLAP_PARAM_IS_STEPPED | (d->is_enum ? CLAP_PARAM_IS_ENUM : 0) |
                  (d->id >= TXP_ID_OWN0 ? 0 : CLAP_PARAM_IS_AUTOMATABLE);   /* (PRESET and KEYS are choices, not curves) */
    snprintf(info->name, sizeof info->name, "%s", d->name);
    snprintf(info->module, sizeof info->module, "%s", d->module);
    info->min_value = 0.0;
    info->max_value = d->hi;
    info->default_value = p->def[index];
    return true;
}
static bool p_value(const clap_plugin_t *plugin, clap_id id, double *out)
{
    txp_plugin_t *p = plugin->plugin_data;
    return txp_inst_get(p->in, id, out) == 0;
}
static bool p_to_text(const clap_plugin_t *plugin, clap_id id, double value, char *out, uint32_t size)
{
    int i = txp_param_index(id);
    (void)plugin;
    return i >= 0 && !txp_param_text((uint32_t)i, value, out, size);
}
static bool p_from_text(const clap_plugin_t *plugin, clap_id id, const char *text, double *out)
{
    int i = txp_param_index(id);
    (void)plugin;
    return i >= 0 && !txp_param_parse((uint32_t)i, text, out);
}
static void p_events_in(txp_plugin_t *p, const clap_input_events_t *ev, int timed)
{
    uint32_t n = ev ? ev->size(ev) : 0, k;
    for (k = 0; k < n; k++) {
        const clap_event_header_t *h = ev->get(ev, k);
        const clap_event_param_value_t *pv;
        int i;
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE) continue;
        pv = (const clap_event_param_value_t *)h;
        if ((i = txp_param_index(pv->param_id)) < 0) continue;
        p->host_view[i] = floor(pv->value + 0.5);
        if (timed) txp_inst_param(p->in, h->time, pv->param_id, pv->value);
        else txp_inst_param_now(p->in, pv->param_id, pv->value);
    }
}
static void p_out(txp_plugin_t *p, const clap_output_events_t *out);
static void uq_drain(txp_plugin_t *p, const clap_output_events_t *out, int timed);
static void p_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out)
{
    txp_plugin_t *p = plugin->plugin_data;
    int proc = atomic_load(&p->processing);
    uq_drain(p, out, p->active && proc);     /* (the window's writes first: the host's own come after them) */
    p_events_in(p, in, p->active && proc);   /* processing: the audio thread, between two blocks; else at once */
    if (!(p->active && proc)) txp_inst_drain(p->in);
    p_out(p, out);
}
static const clap_plugin_params_t EXT_PARAMS = { p_count, p_info, p_value, p_to_text, p_from_text, p_flush };

/* ---- audio and note ports */
static uint32_t ap_count(const clap_plugin_t *plugin, bool is_input) { (void)plugin; return is_input ? 0 : 1; }
static bool ap_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_audio_port_info_t *info)
{
    (void)plugin;
    if (is_input || index) return false;
    memset(info, 0, sizeof *info);
    info->id = 0;
    snprintf(info->name, sizeof info->name, "Main");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
static const clap_plugin_audio_ports_t EXT_AUDIO = { ap_count, ap_get };
static uint32_t np_count(const clap_plugin_t *plugin, bool is_input) { (void)plugin; return is_input ? 1 : 0; }
static bool np_get(const clap_plugin_t *plugin, uint32_t index, bool is_input, clap_note_port_info_t *info)
{
    (void)plugin;
    if (!is_input || index) return false;
    memset(info, 0, sizeof *info);
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    snprintf(info->name, sizeof info->name, "Notes");
    return true;
}
static const clap_plugin_note_ports_t EXT_NOTES = { np_count, np_get };

static uint32_t tail_get(const clap_plugin_t *plugin)
{
    const txp_plugin_t *p = plugin->plugin_data;   /* (in frames at the activation's rate) */
    return (uint32_t)(TAIL_MS * ((p->rate > 0.0 ? p->rate : 48000.0) / 1000.0));
}
static const clap_plugin_tail_t EXT_TAIL = { tail_get };
static uint32_t lat_get(const clap_plugin_t *plugin)
{
    txp_plugin_t *p = plugin->plugin_data;
    return txp_inst_latency(p->in);
}
static const clap_plugin_latency_t EXT_LATENCY = { lat_get };

/* the state: the slot's (slot_bridge.h: the selected lane is the app's, "sel", in it) and the window's own, "win" (its
 * width, 0: the default), "view", "dice" (RANDOM's ticks) and "col", added before the closing brace (a build of before
 * reads past them) */
static int st_key(const char *j, const char *key, int *v)   /* "key":"N" in a flat object: 0 found */
{
    char pat[16];
    const char *q;
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    if (!(q = strstr(j, pat))) return -1;
    *v = atoi(q + strlen(pat));
    return 0;
}
/* "col": the engines' colours, "name:colour;..." (only those not bone; an engine this build has not is skipped) */
static int st_col_put(const txp_plugin_t *p, char *o, int room)
{
    const txp_catalog_t *c = txp_catalog();
    int n = 0;
    uint32_t e;
    if (room < 16) { o[0] = '}'; o[1] = 0; return 1; }
    n += snprintf(o + n, (size_t)(room - n), ",\"col\":\"");
    for (e = 0; e < c->neng && n < room - 40; e++)
        if (p->col[e]) n += snprintf(o + n, (size_t)(room - n), "%s:%u;", c->eng[e], (unsigned)p->col[e]);
    n += snprintf(o + n, (size_t)(room - n), "\"}");
    return n;
}
static void st_col_get(txp_plugin_t *p, const char *j)
{
    const txp_catalog_t *c = txp_catalog();
    const char *q = strstr(j, "\"col\":\""), *end;
    memset(p->col, 0, sizeof p->col);
    if (!q) return;
    q += 7;
    end = strchr(q, '"');
    while (end && q < end) {
        const char *colon = strchr(q, ':'), *semi = strchr(q, ';');
        uint32_t e;
        if (!colon || !semi || colon > semi || semi > end) break;
        for (e = 0; e < c->neng; e++)
            if (strlen(c->eng[e]) == (size_t)(colon - q) && !strncmp(c->eng[e], q, (size_t)(colon - q))) {
                int v = atoi(colon + 1);
                p->col[e] = (uint8_t)(v > 0 && v < TXP_COL_N ? v : 0);
            }
        q = semi + 1;
    }
}
static bool st_save(const clap_plugin_t *plugin, const clap_ostream_t *s)
{
    txp_plugin_t *p = plugin->plugin_data;
    static _Thread_local char buf[TXP_STATE_MAX];
    int n = txp_inst_state_get(p->in, buf, TXP_STATE_MAX - 2048), done = 0;
    if (n < 2 || buf[n - 1] != '}') return false;
    n += snprintf(buf + n - 1, 80, ",\"win\":\"%d\",\"winh\":\"%d\",\"view\":\"%d\",\"dice\":\"%d\"}", p->win_w, p->win_h,
                  p->view, p->dice) - 1;
    n += st_col_put(p, buf + n - 1, TXP_STATE_MAX - n) - 1;
    while (done < n) {
        int64_t w = s->write(s, buf + done, (uint64_t)(n - done));
        if (w <= 0) return false;
        done += (int)w;
    }
    return true;
}
static bool st_load(const clap_plugin_t *plugin, const clap_istream_t *s)
{
    txp_plugin_t *p = plugin->plugin_data;
    static _Thread_local char buf[TXP_STATE_MAX * 2];
    int64_t got = 0, r;
    while (got < (int64_t)sizeof buf - 1 && (r = s->read(s, buf + got, (uint64_t)(sizeof buf - 1 - (size_t)got))) > 0) got += r;
    buf[got] = 0;
    if (txp_inst_state_set(p->in, buf)) { say(p, CLAP_LOG_WARNING, "Textura: the saved state could not be read"); return false; }
    {   /* the window's (a state of before has none: the default size, the MAP) */
        int w = 0, h = 0, vw = 0, dc = 15;
        (void)st_key(buf, "win", &w);
        (void)st_key(buf, "dice", &dc);
        p->dice = dc & 15;
        (void)st_key(buf, "winh", &h);
        (void)st_key(buf, "view", &vw);
        p->pend_selt = 0.0;
        p->view = vw == 1;
        st_col_get(p, buf);
        w = w >= TXG_MIN_W && w <= TXG_MAX_W ? w : 0;
        h = h >= TXG_MIN_H && h <= TXG_MAX_H ? h : 0;
        if (w != p->win_w || h != p->win_h) { p->win_w = w; p->win_h = h; txg_clap_size_changed(p->gui); }
    }
    if (!p->active) { if (p->hparams) p->hparams->rescan(p->host, CLAP_PARAM_RESCAN_VALUES); }
    else {                               /* applied by the audio thread's next steps: re-read the values after it */
        atomic_store(&p->rescan_rev, txp_inst_rev(p->in));
        atomic_store(&p->rescan_tries, 200);
        p->host->request_callback(p->host);
    }
    return true;
}
static const clap_plugin_state_t EXT_STATE = { st_save, st_load };

/* ---- the plugin */
static bool pl_init(const clap_plugin_t *plugin)
{
    txp_plugin_t *p = plugin->plugin_data;
    p->log = p->host->get_extension(p->host, CLAP_EXT_LOG);
    p->hparams = p->host->get_extension(p->host, CLAP_EXT_PARAMS);
    p->hlat = p->host->get_extension(p->host, CLAP_EXT_LATENCY);
    p->hstate = p->host->get_extension(p->host, CLAP_EXT_STATE);
    p->tab = txp_params(&p->np);
    p->host_view = calloc(p->np ? p->np : 1u, sizeof *p->host_view);
    p->def = calloc(p->np ? p->np : 1u, sizeof *p->def);
    p->pend_t = calloc(p->np ? p->np : 1u, sizeof *p->pend_t);
    p->pend_v = calloc(p->np ? p->np : 1u, sizeof *p->pend_v);
    if (!p->host_view || !p->def || !p->pend_t || !p->pend_v) return false;
    {
        const char *s = getenv("TEXTURA_PLUGIN_GUI_SHOT");
        p->shot = s && *s && strcmp(s, "0");
        s = getenv("TEXTURA_PLUGIN_TEST");
        p->dice_fixed = p->shot || (s && *s && strcmp(s, "0"));
        p->dice = 15;
    }
    p->gui = txg_clap_new(p->host, p);   /* (the window's state; the window opens on the host's create) */
    p->in = txp_inst_new();
    if (!p->in) return false;
    if (txp_inst_slot(p->in) < 0)
        say(p, CLAP_LOG_WARNING, "Textura: 16 instances already play in this process: this one stays silent");
    for (uint32_t i = 0; i < p->np; i++) {
        double v = 0.0;
        (void)txp_inst_get(p->in, p->tab[i].id, &v);
        p->host_view[i] = v;
        p->def[i] = v;
    }
    return true;
}
static void pl_destroy(const clap_plugin_t *plugin)
{
    txp_plugin_t *p = plugin->plugin_data;
    txg_clap_free(p->gui);
    txp_inst_free(p->in);
    free(p->host_view);
    free(p->def);
    free(p->pend_t);
    free(p->pend_v);
    free(p);
}
static bool pl_activate(const clap_plugin_t *plugin, double rate, uint32_t min_frames, uint32_t max_frames)
{
    txp_plugin_t *p = plugin->plugin_data;
    uint32_t lat0 = txp_inst_latency(p->in);
    (void)min_frames; (void)max_frames;
    if (txp_inst_activate(p->in, rate)) {
        char m[160];
        snprintf(m, sizeof m, "Textura: plays at 1 to 1536 kHz, not at %.0f Hz", rate);
        say(p, CLAP_LOG_ERROR, m);
        return false;
    }
    p->have_last = 0;
    p->active = 1;
    p->rate = rate;
    /* the latency is the rate's: a host that asked before is told here, the one place CLAP lets it change */
    if (txp_inst_latency(p->in) != lat0 && p->hlat && p->hlat->changed) p->hlat->changed(p->host);
    return true;
}
static void pl_deactivate(const clap_plugin_t *plugin)
{
    txp_plugin_t *p = plugin->plugin_data;
    txp_stats_t st;
    uint32_t late;
    txp_inst_deactivate(p->in);
    p->active = 0;
    txp_inst_stats(p->in, &st, &late);
    if (late || st.q_full) {
        char m[160];
        snprintf(m, sizeof m, "Textura: %u events were late, %u did not fit the queue", late, st.q_full);
        say(p, CLAP_LOG_WARNING, m);
    }
}
static bool pl_start(const clap_plugin_t *plugin) { txp_plugin_t *p = plugin->plugin_data; atomic_store(&p->processing, 1); return true; }
static void pl_stop(const clap_plugin_t *plugin) { txp_plugin_t *p = plugin->plugin_data; atomic_store(&p->processing, 0); }
static void pl_reset(const clap_plugin_t *plugin)
{
    txp_plugin_t *p = plugin->plugin_data;
    txp_inst_reset(p->in);
}

static uint8_t vel7(double v)
{
    long x = lround(v * 127.0);
    return (uint8_t)(x < 0 ? 0 : x > 127 ? 127 : x);
}
static clap_process_status pl_process(const clap_plugin_t *plugin, const clap_process_t *pr)
{
    txp_plugin_t *p = plugin->plugin_data;
    const clap_input_events_t *ev = pr->in_events;
    uint32_t n = ev ? ev->size(ev) : 0, k, tempo10 = 0;
    float *l, *r;
    if (pr->transport && (pr->transport->flags & CLAP_TRANSPORT_HAS_TEMPO) && pr->transport->tempo > 0.0)
        tempo10 = (uint32_t)lround(pr->transport->tempo * 10.0);
    uq_drain(p, pr->out_events, 1);
    for (k = 0; k < n; k++) {
        const clap_event_header_t *h = ev->get(ev, k);
        if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        switch (h->type) {
        case CLAP_EVENT_NOTE_ON: case CLAP_EVENT_NOTE_OFF: case CLAP_EVENT_NOTE_CHOKE: {
            const clap_event_note_t *e = (const clap_event_note_t *)h;
            uint8_t ch = (uint8_t)(e->channel >= 0 && e->channel < 16 ? e->channel : 0), v;
            if (e->key < 0 || e->key > 127) break;
            if (h->type == CLAP_EVENT_NOTE_ON) {
                v = vel7(e->velocity);
                txp_inst_midi(p->in, h->time, (uint8_t)(0x90u | ch), (uint8_t)e->key, v ? v : 1u);
            } else
                txp_inst_midi(p->in, h->time, (uint8_t)(0x80u | ch), (uint8_t)e->key, h->type == CLAP_EVENT_NOTE_OFF ? vel7(e->velocity) : 0u);
            break;
        }
        case CLAP_EVENT_MIDI: {
            const clap_event_midi_t *e = (const clap_event_midi_t *)h;
            txp_inst_midi(p->in, h->time, e->data[0], e->data[1], e->data[2]);
            break;
        }
        case CLAP_EVENT_PARAM_VALUE: {
            const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;
            int i = txp_param_index(e->param_id);
            if (i < 0) break;
            p->host_view[i] = floor(e->value + 0.5);
            txp_inst_param(p->in, h->time, e->param_id, e->value);
            break;
        }
        default: break;
        }
    }
    if (pr->audio_outputs_count < 1 || pr->audio_outputs[0].channel_count < 1 || !pr->audio_outputs[0].data32)
        return CLAP_PROCESS_ERROR;
    l = pr->audio_outputs[0].data32[0];
    r = pr->audio_outputs[0].channel_count > 1 ? pr->audio_outputs[0].data32[1] : 0;
    if (r) txp_inst_render(p->in, l, r, pr->frames_count, tempo10);
    else {                               /* (a mono bus: L only) */
        float scratch[1024];
        uint32_t done = 0;
        while (done < pr->frames_count) {
            uint32_t c = pr->frames_count - done < 1024u ? pr->frames_count - done : 1024u;
            txp_inst_render(p->in, l + done, scratch, c, tempo10);
            done += c;
        }
    }
    pr->audio_outputs[0].constant_mask = 0;
    if (pr->out_events) p_out(p, pr->out_events);
    return CLAP_PROCESS_CONTINUE;
}
/* the host's view: the app's own changes (a MIDI CC, a program change, a scene): a value that moved since the last
 * time and is not what the host last sent */
static void p_out(txp_plugin_t *p, const clap_output_events_t *out)
{
    txp_snap_t sn;
    uint32_t i;
    if (!out || txp_inst_snap(p->in, &sn)) return;
    for (i = 0; i < p->np; i++) {
        double v = sn.pv[i];
        clap_event_param_value_t o;
        if (!p->have_last || sn.pv[i] == p->last.pv[i] || v == p->host_view[i]) continue;
        memset(&o, 0, sizeof o);
        o.header.size = sizeof o;
        o.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        o.header.type = CLAP_EVENT_PARAM_VALUE;
        o.param_id = p->tab[i].id;
        o.note_id = -1; o.port_index = -1; o.channel = -1; o.key = -1;
        o.value = v;
        if (out->try_push(out, &o.header)) p->host_view[i] = v;
    }
    p->last = sn;
    p->have_last = 1;
}

static const void *pl_ext(const clap_plugin_t *plugin, const char *id)
{
    const txp_plugin_t *p = plugin->plugin_data;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &EXT_PARAMS;
    if (!strcmp(id, CLAP_EXT_GUI)) return txg_clap_ext();
    if (!strcmp(id, TXG_EXT_GUI_SHOT)) return p->shot ? txg_clap_shot_ext() : 0;   /* (the tests only) */
    if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return txg_clap_fd_ext();         /* (Linux: the X11 view) */
    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return txg_clap_timer_ext();
    if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &EXT_AUDIO;
    if (!strcmp(id, CLAP_EXT_NOTE_PORTS)) return &EXT_NOTES;
    if (!strcmp(id, CLAP_EXT_LATENCY)) return &EXT_LATENCY;
    if (!strcmp(id, CLAP_EXT_STATE)) return &EXT_STATE;
    if (!strcmp(id, CLAP_EXT_TAIL)) return &EXT_TAIL;
    return 0;
}
static void pl_main(const clap_plugin_t *plugin)
{
    txp_plugin_t *p = plugin->plugin_data;
    int t = atomic_load(&p->rescan_tries);
    if (t <= 0) return;
    if (txp_inst_rev(p->in) != atomic_load(&p->rescan_rev) || t == 1) {
        atomic_store(&p->rescan_tries, 0);
        if (p->hparams) p->hparams->rescan(p->host, CLAP_PARAM_RESCAN_VALUES);
    } else {
        atomic_store(&p->rescan_tries, t - 1);
        p->host->request_callback(p->host);
    }
}

static const clap_plugin_t *f_create(const clap_plugin_factory_t *f, const clap_host_t *host, const char *id)
{
    txp_plugin_t *p;
    (void)f;
    if (!host || !id || strcmp(id, DESC.id) || !clap_version_is_compatible(host->clap_version)) return 0;
    p = calloc(1, sizeof *p);
    if (!p) return 0;
    p->host = host;
    p->plugin.desc = &DESC;
    p->plugin.plugin_data = p;
    p->plugin.init = pl_init;
    p->plugin.destroy = pl_destroy;
    p->plugin.activate = pl_activate;
    p->plugin.deactivate = pl_deactivate;
    p->plugin.start_processing = pl_start;
    p->plugin.stop_processing = pl_stop;
    p->plugin.reset = pl_reset;
    p->plugin.process = pl_process;
    p->plugin.get_extension = pl_ext;
    p->plugin.on_main_thread = pl_main;
    return &p->plugin;
}
static uint32_t f_count(const clap_plugin_factory_t *f) { (void)f; return 1; }
static const clap_plugin_descriptor_t *f_desc(const clap_plugin_factory_t *f, uint32_t i) { (void)f; return i ? 0 : &DESC; }
static const clap_plugin_factory_t FACTORY = { f_count, f_desc, f_create };

/* ---- the window's calls (txp_ui.h) */
static double mono_s(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + t.tv_nsec * 1e-9;
#endif
}
static int out_push(const clap_output_events_t *out, uint8_t type, uint32_t id, double v)
{
    if (type == UQ_VALUE) {
        clap_event_param_value_t o;
        memset(&o, 0, sizeof o);
        o.header.size = sizeof o;
        o.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        o.header.type = CLAP_EVENT_PARAM_VALUE;
        o.param_id = id;
        o.note_id = -1; o.port_index = -1; o.channel = -1; o.key = -1;
        o.value = v;
        return out->try_push(out, &o.header);
    } else {
        clap_event_param_gesture_t g;
        memset(&g, 0, sizeof g);
        g.header.size = sizeof g;
        g.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        g.header.type = type == UQ_BEGIN ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END;
        g.param_id = id;
        return out->try_push(out, &g.header);
    }
}
/* the window's writes, in their order: told to the host (gestures, values), then into the app (timed: the audio
 * thread, for this process call's block; else queued, the flush applies them). The host's list full: the rest wait */
static void uq_drain(txp_plugin_t *p, const clap_output_events_t *out, int timed)
{
    uint32_t r = atomic_load_explicit(&p->uqr, memory_order_relaxed);
    while (r != atomic_load_explicit(&p->uqw, memory_order_acquire)) {
        const uq_ev_t *e = &p->uq[r & (UQ - 1u)];
        int i;
        if (e->type <= UQ_END && out && !out_push(out, e->type, e->id, e->v)) return;
        if (e->type == UQ_VALUE) {
            if ((i = txp_param_index(e->id)) >= 0) p->host_view[i] = e->v;
            if (timed) txp_inst_param(p->in, 0, e->id, e->v); else txp_inst_param_now(p->in, e->id, e->v);
        } else if (e->type >= UQ_EV) {
            int k = e->type - UQ_EV;
            if (timed) txp_inst_event(p->in, 0, k, e->id, e->v); else txp_inst_event_now(p->in, k, e->id, e->v);
        }
        atomic_store_explicit(&p->uqr, ++r, memory_order_release);
    }
}
static void uq_put(txp_plugin_t *c, uint8_t type, uint32_t id, int32_t v)
{
    uint32_t w = atomic_load_explicit(&c->uqw, memory_order_relaxed);
    if (w - atomic_load_explicit(&c->uqr, memory_order_acquire) >= UQ) return;   /* (full: dropped; a host that never
                                          * processes nor flushes) */
    c->uq[w & (UQ - 1u)] = (uq_ev_t){ type, id, v };
    atomic_store_explicit(&c->uqw, w + 1u, memory_order_release);
    if (c->hparams && c->hparams->request_flush) c->hparams->request_flush(c->host);   /* (the host calls flush, or
                                          * process, and the app and the host get it) */
}
txp_core *txp_core_of(const clap_plugin_t *plugin) { return plugin ? plugin->plugin_data : 0; }
void **txp_ui_gui_data(txp_core *c) { return &c->gui; }
int txp_ui_has_slot(const txp_core *c) { return txp_inst_slot(c->in) >= 0; }
int txp_ui_snap(const txp_core *c, txp_snap_t *out)
{
    if (txp_inst_snap(c->in, out) == 0) return 0;
    memset(out, 0, sizeof *out);
    out->ui.nlane = 4;
    return -1;
}
int txp_ui_get(const txp_core *c, uint32_t id)
{
    int i = txp_param_index(id);
    double v;
    if (i < 0 || txp_inst_get(c->in, id, &v)) return -1;
    if (c->pend_t[i] > 0.0) {            /* the window's write until the app shows it (or PEND_S passed) */
        if ((int)v == c->pend_v[i] || mono_s() - c->pend_t[i] >= PEND_S) ((txp_core *)c)->pend_t[i] = 0.0;
        else return c->pend_v[i];
    }
    return (int)v;
}
int txp_ui_hi(uint32_t id)
{
    uint32_t n;
    const txp_param_t *t = txp_params(&n);
    int i = txp_param_index(id);
    return i < 0 ? -1 : t[i].hi;
}
int txp_ui_default(const txp_core *c, uint32_t id) { int i = txp_param_index(id); return i < 0 ? -1 : (int)c->def[i]; }
void txp_ui_begin(txp_core *c, uint32_t id) { if (txp_param_index(id) >= 0) uq_put(c, UQ_BEGIN, id, 0); }
void txp_ui_end(txp_core *c, uint32_t id) { if (txp_param_index(id) >= 0) uq_put(c, UQ_END, id, 0); }
void txp_ui_set(txp_core *c, uint32_t id, int v)
{
    int i = txp_param_index(id), hi = txp_ui_hi(id);
    if (i < 0) return;
    v = v < 0 ? 0 : v > hi ? hi : v;
    if (txp_ui_get(c, id) == v) return;
    c->pend_v[i] = v;
    c->pend_t[i] = mono_s();
    uq_put(c, UQ_VALUE, id, v);
}
static void ui_dirty(txp_core *c) { if (c->hstate && c->hstate->mark_dirty) c->hstate->mark_dirty(c->host); }
void txp_ui_mute(txp_core *c, uint32_t lane, int on)
{
    if (lane >= TXP_LANES) return;
    c->pend_m[lane] = (uint8_t)(on != 0);
    c->pend_mt[lane] = mono_s();
    uq_put(c, UQ_EV + TXP_EV_MUTE, lane, on != 0);
    ui_dirty(c);
}
void txp_ui_solo(txp_core *c, uint32_t mask)
{
    c->pend_s = (uint16_t)mask;
    c->pend_st = mono_s();
    uq_put(c, UQ_EV + TXP_EV_SOLO, 0, (int32_t)(mask & 0xFFFFu));
    ui_dirty(c);
}
void txp_ui_mutes(const txp_core *c, uint32_t *mute, uint32_t *solo)
{
    txp_core *w = (txp_core *)c;         /* (the pending writes resolved) */
    static txp_snap_t sn;                /* (the main thread's alone) */
    double now = mono_s();
    uint32_t k, m = 0, s = 0;
    if (txp_ui_snap(c, &sn) == 0) {
        for (k = 0; k < TXP_LANES; k++) if (sn.ui.lane[k].mute) m |= 1u << k;
        s = sn.ui.solo;
    }
    for (k = 0; k < TXP_LANES; k++) {
        if (w->pend_mt[k] <= 0.0) continue;
        if ((uint32_t)w->pend_m[k] == (m >> k & 1u) || now - w->pend_mt[k] >= PEND_S) w->pend_mt[k] = 0.0;
        else m = (m & ~(1u << k)) | (uint32_t)w->pend_m[k] << k;
    }
    if (w->pend_st > 0.0) {
        if (w->pend_s == s || now - w->pend_st >= PEND_S) w->pend_st = 0.0;
        else s = w->pend_s;
    }
    *mute = m; *solo = s;
}
uint32_t txp_ui_act(const txp_core *c, uint8_t (*ev)[TXP_LANES], uint8_t (*pk)[TXP_LANES], uint32_t n)
{
    return txp_inst_act(c->in, ev, pk, n);
}
void txp_ui_sound(txp_core *c, int lane, int fam, int snd, int keep)
{
    if (lane < 0 || lane >= (int)TXP_LANES || fam < 0 || fam > 15 || snd < 0 || snd > 15) return;
    uq_put(c, UQ_EV + TXP_EV_SOUND, (uint32_t)lane, fam << 4 | snd | (keep ? TXP_SND_KEEP : 0));
    if (keep) ui_dirty(c);
}
void txp_ui_preview(txp_core *c, int lane, int fam, int snd, int cnt, int act, int force)
{
    if (lane < 0 || lane >= (int)TXP_LANES || fam < 0 || fam > 15 || snd < 0 || snd > 15) return;
    cnt = cnt < 0 ? 0 : cnt > 15 ? 15 : cnt;
    act = act < 0 ? 0 : act > 15 ? 15 : act;
    uq_put(c, UQ_EV + TXP_EV_PREVIEW, (uint32_t)lane, TXP_PV_PLAY | fam << 4 | snd | cnt << 8 | act << 12 | (force ? TXP_PV_FORCE : 0));
}
void txp_ui_preview_end(txp_core *c, int lane, int fam, int snd, int keep, int vals)
{
    if (lane < 0 || lane >= (int)TXP_LANES) return;
    fam = fam < 0 || fam > 15 ? 15 : fam;
    snd = snd < 0 || snd > 15 ? 15 : snd;
    uq_put(c, UQ_EV + TXP_EV_PREVIEW, (uint32_t)lane, TXP_PV_END | fam << 4 | snd | (keep ? TXP_PV_KEEP : 0) | (vals ? TXP_PV_VALS : 0));
    if (keep) ui_dirty(c);
}
void txp_ui_load(txp_core *c, int what, int with_fx)
{
    if (what < 0) return;
    uq_put(c, UQ_EV + TXP_EV_LOAD, with_fx ? 1u : 0u, what);
    ui_dirty(c);
}
int txp_ui_col(const txp_core *c, int eng) { return eng >= 0 && eng < (int)TXP_ENG_MAX ? c->col[eng] : 0; }
void txp_ui_col_set(txp_core *c, int eng, int col)
{
    if (eng < 0 || eng >= (int)TXP_ENG_MAX || col < 0 || col >= TXP_COL_N || c->col[eng] == col) return;
    c->col[eng] = (uint8_t)col;
    ui_dirty(c);
}
void txp_ui_col_reset(txp_core *c) { memset(c->col, 0, sizeof c->col); ui_dirty(c); }
int txp_ui_view(const txp_core *c) { return c->view; }
void txp_ui_view_set(txp_core *c, int v) { if (c->view != (v == 1)) { c->view = v == 1; ui_dirty(c); } }
void txp_ui_win_resize(txp_core *c, int w)
{
    txp_ui_win_w_set(c, w == TXG_W ? 0 : w);
    c->win_h = 0;                        /* (SETTINGS' sizes: the 5:3 of that width) */
    txg_clap_size_changed(c->gui);
    ui_dirty(c);
}
int txp_ui_win_w(const txp_core *c) { return c->win_w; }
void txp_ui_win_w_set(txp_core *c, int w) { c->win_w = w >= TXG_MIN_W && w <= TXG_MAX_W ? w : 0; }
int txp_ui_win_h(const txp_core *c) { return c->win_h; }
void txp_ui_win_h_set(txp_core *c, int h) { c->win_h = h >= TXG_MIN_H && h <= TXG_MAX_H ? h : 0; }
int txp_ui_sel(const txp_core *c)        /* the app's (the window's own choice until the app shows it) */
{
    txp_core *w = (txp_core *)c;
    static txp_snap_t sn;                /* (the main thread's alone) */
    int s = txp_ui_snap(c, &sn) == 0 ? sn.ui.sel : 0;
    if (w->pend_selt > 0.0) {
        if (s == w->sel || mono_s() - w->pend_selt >= PEND_S) w->pend_selt = 0.0;
        else s = w->sel;
    }
    return s >= 0 && s < (int)TXP_LANES ? s : 0;
}
void txp_ui_sel_set(txp_core *c, int k)
{
    if (k < 0 || k >= (int)TXP_LANES) return;
    c->sel = k;
    c->pend_selt = mono_s();
    uq_put(c, UQ_EV + TXP_EV_SEL, (uint32_t)k, 0);
    ui_dirty(c);
}
void txp_ui_lane_run(txp_core *c, uint32_t lane, int on)
{
    if (lane >= TXP_LANES) return;
    uq_put(c, UQ_EV + TXP_EV_LANERUN, lane, on != 0);
    ui_dirty(c);
}
void txp_ui_rec(txp_core *c) { uq_put(c, UQ_EV + TXP_EV_REC, 0, 0); ui_dirty(c); }
void txp_ui_undo(txp_core *c) { uq_put(c, UQ_EV + TXP_EV_UNDO, 0, 0); ui_dirty(c); }
int txp_ui_dice(const txp_core *c) { return c->dice; }
void txp_ui_dice_set(txp_core *c, int what) { if (c->dice != (what & 15)) { c->dice = what & 15; ui_dirty(c); } }
int txp_ui_roll(txp_core *c, int lane)
{
    uint32_t seed, a;
    if (lane >= (int)TXP_LANES) return -1;
    if (lane < 0 && !(c->dice & 15)) return -1;
    a = lane < 0 ? TXP_DICE_ALL | (uint32_t)c->dice : (uint32_t)lane;
    c->dice_n++;
    if (c->dice_fixed) seed = c->dice_n * 7919u + 1u;   /* (the tests: the same rolls every run) */
    else {
        double t = mono_s();
        seed = (uint32_t)(uint64_t)(t * 1e6) ^ (uint32_t)(uint64_t)(t * 1e3) << 16 ^ c->dice_n * 2654435761u ^ (uint32_t)(uintptr_t)c;
    }
    uq_put(c, UQ_BEGIN, TXP_ID_PRESET, 0);   /* (one gesture, the host's undo one step: the roll reaches the host as a
                                              * preset load does, the app's own values) */
    uq_put(c, UQ_EV + TXP_EV_DICE, a, (int32_t)(seed ? seed : 1u));
    uq_put(c, UQ_END, TXP_ID_PRESET, 0);
    ui_dirty(c);
    return 0;
}
void txp_ui_save(txp_core *c, int slot, const char *name)
{
    uint32_t i;
    char nm[12] = {0};
    if (slot < 0 || slot >= (int)TXP_USER_N) return;
    snprintf(nm, sizeof nm, "%s", name ? name : "");
    for (i = 0; i < 11u; i += 4u)        /* (its letters, four an event; then the save) */
        uq_put(c, UQ_EV + TXP_EV_NAME, i, (int32_t)((uint32_t)(uint8_t)nm[i] | (uint32_t)(uint8_t)nm[i + 1u] << 8 |
                                                    (uint32_t)(uint8_t)nm[i + 2u] << 16 | (i + 3u < 11u ? (uint32_t)(uint8_t)nm[i + 3u] << 24 : 0u)));
    uq_put(c, UQ_EV + TXP_EV_SAVE, (uint32_t)slot, 0);
    ui_dirty(c);
}
void txp_ui_tempo(txp_core *c, int bpm10)
{
    if (bpm10 < 400 || bpm10 > 2400) return;
    uq_put(c, UQ_EV + TXP_EV_TEMPO, 0, bpm10);
    ui_dirty(c);
}

/* the entry's three functions (plugin/src/clap_entry.cpp exports clap_entry from them in every format) */
bool txp_entry_init(const char *path) { (void)path; return true; }
void txp_entry_deinit(void) {}
const void *txp_entry_get_factory(const char *id) { return id && !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &FACTORY : 0; }
