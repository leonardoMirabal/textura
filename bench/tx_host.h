/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura host helpers for the bench tools: scenes, WAV files, sha256, timing, instance memory. */
#ifndef TX_HOST_H
#define TX_HOST_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "tx_engine.h"
#include "tx_script.h"

#define TX_SCENES_DIR "bench/scenes"
extern const char *const tx_std_scenes[];   /* the scenes every engine renders: idle tap hold chord cap max */

char *tx_read_file(const char *path);       /* malloc'ed text, or 0 */
/* the text of a scene: "max" = the engine's worst case, a name in bench/scenes/, or a file path; malloc'ed */
char *tx_scene_text(const tx_engine_t *e, const char *scene);
/* load and parse; prints the error and returns -1 on failure */
int tx_scene_load(tx_script_t *s, const tx_engine_t *e, const char *scene);
void *tx_state_new(const tx_engine_t *e);   /* zeroed, aligned, TX_STATE_MAX bytes */

/* WAV: mono, 44.1 kHz, 24-bit PCM from Q23 (saturated) */
typedef struct { FILE *f; uint32_t frames; } tx_wav_t;
int tx_wav_open(tx_wav_t *w, const char *path);
void tx_wav_write(tx_wav_t *w, const int32_t *q23, unsigned n);
void tx_wav_close(tx_wav_t *w);

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; unsigned fill; } tx_sha256_t;
void tx_sha256_init(tx_sha256_t *c);
void tx_sha256_add(tx_sha256_t *c, const void *p, size_t n);
void tx_sha256_hex(tx_sha256_t *c, char out[65]);
void tx_sha256_q23(tx_sha256_t *c, const int32_t *x, unsigned n);   /* the samples as little-endian int32 */

uint64_t tx_now_ns(void);
void tx_bench_prepare(void);                /* performance core + clock warm-up before timing */
#endif
