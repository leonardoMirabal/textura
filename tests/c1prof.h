/* SPDX-License-Identifier: GPL-3.0-only */
/* make fw-c1map: lanes.c's second core on the host, inline and counted (tests/c1prof.c) */
#include <stdint.h>
int c1prof_run(void (*fn)(uint32_t), uint32_t a);
int c1prof_wait(void);
#define LN_PAR
#define LN_PAR_ON() 1
#define LN_PAR_RUN(fn, a) c1prof_run(fn, a)
#define LN_PAR_WAIT() c1prof_wait()
