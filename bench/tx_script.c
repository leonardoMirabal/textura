/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura scene scripts: parser and runner (tx_script.h). Host code (also built into the browser player). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "tx_script.h"

static int note_of(const char *t)
{
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7};      /* A B C D E F G */
    int n, oct;
    char *end;
    if (isdigit((unsigned char)t[0])) { n = (int)strtol(t, &end, 10); return *end ? -1 : n; }
    if (toupper((unsigned char)t[0]) < 'A' || toupper((unsigned char)t[0]) > 'G') return -1;
    n = base[toupper((unsigned char)t[0]) - 'A'];
    t++;
    if (*t == '#') { n++; t++; } else if (*t == 'b') { n--; t++; }
    oct = (int)strtol(t, &end, 10);
    if (end == t || *end) return -1;
    n += (oct + 1) * 12;                                     /* C4 = 60 */
    return n >= 0 && n <= 127 ? n : -1;
}

void tx_script_engine(const char *text, char *name, unsigned cap)
{
    const char *p = text;
    name[0] = 0;
    while (p && *p) {
        char w[32], v[32];
        if (sscanf(p, "%31s %31s", w, v) == 2 && !strcmp(w, "engine")) { snprintf(name, cap, "%s", v); return; }
        p = strchr(p, '\n');
        if (p) p++;
    }
}

int tx_script_parse(tx_script_t *s, const tx_engine_t *e, const char *text)
{
    const char *p = text;
    unsigned line = 0;
    uint32_t last = 0, end = 0;
    int has_end = 0;
    memset(s, 0, sizeof *s);
    s->seed = 1;
    while (p && *p) {
        char buf[256], w0[32] = "", w1[32] = "", w2[32] = "", w3[32] = "";
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char *h;
        int nw;
        unsigned t;
        line++;
        if (len >= sizeof buf) len = sizeof buf - 1;
        memcpy(buf, p, len); buf[len] = 0;
        p = nl ? nl + 1 : 0;
        if ((h = strchr(buf, '#'))) *h = 0;
        nw = sscanf(buf, "%31s %31s %31s %31s", w0, w1, w2, w3);
        if (nw <= 0) continue;
        if (!strcmp(w0, "seed") && nw >= 2) { s->seed = (uint32_t)strtoul(w1, 0, 0); continue; }
        if (!strcmp(w0, "engine") && nw >= 2) { snprintf(s->engine, sizeof s->engine, "%s", w1); continue; }
        if (!isdigit((unsigned char)w0[0]) || nw < 2) goto bad;
        t = (unsigned)strtoul(w0, 0, 10);
        if (t > last) last = t;
        if (!strcmp(w1, "end")) { end = t; has_end = 1; continue; }
        if (s->n >= sizeof s->ev / sizeof s->ev[0]) { snprintf(s->err, sizeof s->err, "line %u: too many events", line); return -1; }
        {
            tx_sev_t *v = &s->ev[s->n];
            v->block = tx_ms_block(t);
            if (!strcmp(w1, "on") && nw >= 4) {
                int n = note_of(w2), vel = atoi(w3);
                if (n < 0 || vel < 1 || vel > 127) goto bad;
                v->kind = TX_SEV_ON; v->a = (uint8_t)n; v->b = (uint8_t)vel;
            } else if (!strcmp(w1, "off") && nw >= 3) {
                int n = note_of(w2);
                if (n < 0) goto bad;
                v->kind = TX_SEV_OFF; v->a = (uint8_t)n;
            } else if (!strcmp(w1, "param") && nw >= 4) {
                int id = isdigit((unsigned char)w2[0]) ? atoi(w2) : tx_param_find(e, w2), val = atoi(w3);
                if (id < 0 || id >= e->n_params) { snprintf(s->err, sizeof s->err, "line %u: %s has no parameter %s", line, e->name, w2); return -1; }
                v->kind = TX_SEV_PARAM; v->id = (uint8_t)id; v->a = (uint8_t)(val < 0 ? 0 : val > 127 ? 127 : val);
            } else if (!strcmp(w1, "chamber") && nw >= 4) {
                int id = -1, k, val = atoi(w3);
                for (k = 0; k < (int)TX_CH_NP; k++) {
                    const char *a = tx_chamber_params[k].name, *b = w2;
                    while (*a && toupper((unsigned char)*b) == *a) { a++; b++; }
                    if (!*a && !*b) id = k;
                }
                if (id < 0) { snprintf(s->err, sizeof s->err, "line %u: the chamber has no parameter %s", line, w2); return -1; }
                v->kind = TX_SEV_CHAMBER; v->id = (uint8_t)id; v->a = (uint8_t)(val < 0 ? 0 : val > 127 ? 127 : val);
            } else if ((!strcmp(w1, "particles") || !strcmp(w1, "level")) && nw >= 3) {
                int val = atoi(w2);
                v->kind = TX_SEV_PARAM; v->id = w1[0] == 'p' ? TX_PID_PARTICLES : TX_PID_LEVEL;
                v->a = (uint8_t)(val < 0 ? 0 : val > 127 ? 127 : val);
            } else
                goto bad;
            if (s->n && v->block < s->ev[s->n - 1].block) { snprintf(s->err, sizeof s->err, "line %u: out of time order", line); return -1; }
            s->n++;
        }
        continue;
    bad:
        snprintf(s->err, sizeof s->err, "line %u: cannot read \"%s\"", line, buf);
        return -1;
    }
    s->blocks = tx_ms_block(has_end ? end : last + 1000u) + 1u;
    return 0;
}

static void apply(const tx_engine_t *e, void *st, const tx_sev_t *v)
{
    if (v->kind == TX_SEV_CHAMBER) return;
    if (v->kind == TX_SEV_ON) e->key(st, v->a, v->b, 1);
    else if (v->kind == TX_SEV_OFF) e->key(st, v->a, 0, 0);
    else e->param(st, v->id, v->a);
}

void tx_script_step(const tx_engine_t *e, void *st, const tx_script_t *s, uint32_t blk, unsigned *next, int32_t *mix)
{
    unsigned j;
    while (*next < s->n && s->ev[*next].block <= blk) apply(e, st, &s->ev[(*next)++]);
    for (j = 0; j < TX_BLOCK; j++) mix[j] = 0;
    e->render(st, mix, TX_BLOCK);
}

void tx_script_run(const tx_engine_t *e, void *st, const tx_script_t *s, tx_block_cb cb, void *user)
{
    int32_t mix[TX_BLOCK];
    unsigned next = 0;
    uint32_t b;
    e->init(st, s->seed);
    for (b = 0; b < s->blocks; b++) {
        tx_stats_t stt;
        tx_script_step(e, st, s, b, &next, mix);
        e->stats(st, &stt);
        if (cb) cb(user, b, mix, &stt);
    }
}

void tx_script_step_ch(const tx_engine_t *e, void *st, tx_chamber_t *ch, const tx_script_t *s, uint32_t blk,
                       unsigned *next, int32_t *mix)
{
    unsigned j;
    while (*next < s->n && s->ev[*next].block <= blk) {
        const tx_sev_t *v = &s->ev[(*next)++];
        if (v->kind == TX_SEV_CHAMBER) { if (ch) tx_chamber_param(ch, v->id, v->a); }
        else apply(e, st, v);
    }
    for (j = 0; j < TX_BLOCK; j++) mix[j] = 0;
    e->render(st, mix, TX_BLOCK);
    if (ch) tx_chamber_process(ch, mix, TX_BLOCK);
}

void tx_script_run_ch(const tx_engine_t *e, void *st, tx_chamber_t *ch, const tx_script_t *s, tx_block_cb cb, void *user)
{
    int32_t mix[TX_BLOCK];
    unsigned next = 0;
    uint32_t b;
    e->init(st, s->seed);
    if (ch) tx_chamber_init(ch);
    for (b = 0; b < s->blocks; b++) {
        tx_stats_t stt;
        tx_script_step_ch(e, st, ch, s, b, &next, mix);
        e->stats(st, &stt);
        if (cb) cb(user, b, mix, &stt);
    }
}

int tx_script_has_chamber(const tx_script_t *s)
{
    unsigned i;
    for (i = 0; i < s->n; i++) if (s->ev[i].kind == TX_SEV_CHAMBER) return 1;
    return 0;
}
