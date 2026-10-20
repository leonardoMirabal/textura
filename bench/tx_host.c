/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura host helpers (tx_host.h). sha256 after FIPS 180-4. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __APPLE__
#include <pthread.h>
#include <pthread/qos.h>
#endif
#include "tx_host.h"

const char *const tx_std_scenes[] = {"idle", "tap", "hold", "chord", "cap", "max", 0};

char *tx_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *b;
    if (!f) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = 0; }
    if (b) b[n] = 0;
    fclose(f);
    return b;
}

char *tx_scene_text(const tx_engine_t *e, const char *scene)
{
    char path[512];
    if (!strcmp(scene, "max")) {
        char *b = malloc(strlen(e->scene_max) + 1);
        if (b) strcpy(b, e->scene_max);
        return b;
    }
    if (strchr(scene, '/') || strstr(scene, ".txt")) return tx_read_file(scene);
    snprintf(path, sizeof path, "%s/%s.txt", TX_SCENES_DIR, scene);
    return tx_read_file(path);
}

int tx_scene_load(tx_script_t *s, const tx_engine_t *e, const char *scene)
{
    char *t = tx_scene_text(e, scene);
    if (!t) { fprintf(stderr, "no scene \"%s\" (bench/scenes/%s.txt)\n", scene, scene); return -1; }
    if (tx_script_parse(s, e, t)) { fprintf(stderr, "%s: %s\n", scene, s->err); free(t); return -1; }
    free(t);
    return 0;
}

void *tx_state_new(const tx_engine_t *e)
{
    void *p = 0;
    if (e->state_bytes > TX_STATE_MAX) { fprintf(stderr, "%s: state %u B > TX_STATE_MAX\n", e->name, e->state_bytes); exit(2); }
    if (posix_memalign(&p, 64, TX_STATE_MAX)) { perror("posix_memalign"); exit(2); }
    memset(p, 0, TX_STATE_MAX);
    return p;
}

/* ---- WAV */
static void le32(FILE *f, uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; fwrite(b, 1, 4, f); }
static void le16(FILE *f, uint32_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; fwrite(b, 1, 2, f); }
static void wav_header(tx_wav_t *w)
{
    uint32_t bytes = w->frames * 3u;
    fseek(w->f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, w->f); le32(w->f, 36u + bytes); fwrite("WAVEfmt ", 1, 8, w->f);
    le32(w->f, 16); le16(w->f, 1); le16(w->f, 1); le32(w->f, TX_FS); le32(w->f, TX_FS * 3u); le16(w->f, 3); le16(w->f, 24);
    fwrite("data", 1, 4, w->f); le32(w->f, bytes);
}
int tx_wav_open(tx_wav_t *w, const char *path)
{
    w->frames = 0;
    if (!(w->f = fopen(path, "wb"))) { perror(path); return -1; }
    wav_header(w);
    return 0;
}
void tx_wav_write(tx_wav_t *w, const int32_t *x, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        int32_t v = x[i] > TX_Q23 - 1 ? TX_Q23 - 1 : x[i] < -TX_Q23 ? -TX_Q23 : x[i];
        uint8_t b[3] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16)};
        fwrite(b, 1, 3, w->f);
    }
    w->frames += n;
}
void tx_wav_close(tx_wav_t *w) { wav_header(w); fclose(w->f); w->f = 0; }

/* ---- sha256 */
static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};
#define ROR(x, n) ((x) >> (n) | (x) << (32 - (n)))
static void sha_block(tx_sha256_t *c, const uint8_t *p)
{
    uint32_t w[64], a, b, d, e, f, g, h, cc, i, t1, t2;
    for (i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0]; b = c->h[1]; cc = c->h[2]; d = c->h[3]; e = c->h[4]; f = c->h[5]; g = c->h[6]; h = c->h[7];
    for (i = 0; i < 64; i++) {
        t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}
void tx_sha256_init(tx_sha256_t *c)
{
    static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, H0, sizeof H0); c->len = 0; c->fill = 0;
}
void tx_sha256_add(tx_sha256_t *c, const void *pv, size_t n)
{
    const uint8_t *p = pv;
    c->len += n;
    while (n--) {
        c->buf[c->fill++] = *p++;
        if (c->fill == 64) { sha_block(c, c->buf); c->fill = 0; }
    }
}
void tx_sha256_hex(tx_sha256_t *c, char out[65])
{
    uint64_t bits = c->len * 8u;
    uint8_t pad = 0x80, z = 0, l[8];
    int i;
    tx_sha256_add(c, &pad, 1);
    while (c->fill != 56) tx_sha256_add(c, &z, 1);
    for (i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    tx_sha256_add(c, l, 8);
    for (i = 0; i < 8; i++) sprintf(out + 8 * i, "%08x", c->h[i]);
}
void tx_sha256_q23(tx_sha256_t *c, const int32_t *x, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++) {
        uint32_t v = (uint32_t)x[i];
        uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
        tx_sha256_add(c, b, 4);
    }
}

uint64_t tx_now_ns(void)
{
#ifdef __APPLE__
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
#endif
}

/* timing runs: ask for a performance core (macOS schedules a default-QoS process on the efficiency cores at times,
 * ~3x slower) and spin 300 ms so the clock is up before the first timed block */
void tx_bench_prepare(void)
{
    uint64_t t0;
    volatile uint32_t x = 1;
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    t0 = tx_now_ns();
    while (tx_now_ns() - t0 < 300000000u) x = x * 1664525u + 1013904223u;
}
