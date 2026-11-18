/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura as a CLAP instrument: the entry, the factory and the plugin. clap-wrapper builds the VST3 and the Audio
 * Unit (v2) from the same code (plugin/CMakeLists.txt, make_clapfirst_plugins). From Frodhes' plugin/src/frodhes_clap.c,
 * without its window, its update checker and its UI core (step 1 has no window of its own: the host's generic
 * parameter view).
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
 * Sample rates: 44.1 kHz (the app's own), the 44.1 and 48 kHz families through an oversampler, any other from 1 to
 * 1536 kHz through a windowed sinc (bridge.c); outside that the instance refuses to activate and logs why. More than
 * TXP_SLOTS (16) instances in one process: the extra ones play silence and log why. */
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>
#include "bridge.h"

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

typedef struct {
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
                  (d->id == TXP_ID_PRESET ? 0 : CLAP_PARAM_IS_AUTOMATABLE);   /* (PRESET is a choice, not a curve) */
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
static void p_flush(const clap_plugin_t *plugin, const clap_input_events_t *in, const clap_output_events_t *out)
{
    txp_plugin_t *p = plugin->plugin_data;
    int proc = atomic_load(&p->processing);
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

static bool st_save(const clap_plugin_t *plugin, const clap_ostream_t *s)
{
    txp_plugin_t *p = plugin->plugin_data;
    static _Thread_local char buf[TXP_STATE_MAX];
    int n = txp_inst_state_get(p->in, buf, TXP_STATE_MAX), done = 0;
    if (n < 0) return false;
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
    p->tab = txp_params(&p->np);
    p->host_view = calloc(p->np ? p->np : 1u, sizeof *p->host_view);
    p->def = calloc(p->np ? p->np : 1u, sizeof *p->def);
    if (!p->host_view || !p->def) return false;
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
    txp_inst_free(p->in);
    free(p->host_view);
    free(p->def);
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
    (void)plugin;
    if (!strcmp(id, CLAP_EXT_PARAMS)) return &EXT_PARAMS;
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

/* the entry's three functions (plugin/src/clap_entry.cpp exports clap_entry from them in every format) */
bool txp_entry_init(const char *path) { (void)path; return true; }
void txp_entry_deinit(void) {}
const void *txp_entry_get_factory(const char *id) { return id && !strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &FACTORY : 0; }
