/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura grain delay: the line's length, from the RAM it is given (the one number to re-tune). Shared by tx_fx.h and
 * bench/gen_fx_tables.c (TIME's table reaches the line's top: make tx-fx-tables after a change). */
#ifndef TX_FX_GD_LEN_H
#define TX_FX_GD_LEN_H

/* The line's bytes: what it had (98 000 B, 1.451 s) + the FM-1 pool's free bytes with the lanes' other cuts in
 * (make fm1-app, 2026-10-29: 34 236 B) - a 16 KiB margin; + what app.ld's one RAM region (the debug gap, .data /
 * .bss and the pool together) and core 1's stack and a sector buffer in NOINIT's spare room left free (make fm1-app,
 * 2026-10-30: 80 628 B) - a 4 KiB margin; - 20 100 B for the reverb at the full rate (its TIME top at 2.5 s, the
 * user's choice: the reverb first). Never under 1.0 s (67 528 B). */
#define TX_GD_BYTES (98000u + 34236u - 16384u + 80628u - 4096u - 20100u)
/* the line in samples: 49 bytes per 32 (12-bit mantissas, a 4-bit shift per 16), a multiple of 32 (112 512: 2.551 s) */
#define TX_GD_N (TX_GD_BYTES * 32u / 49u / 32u * 32u)
/* TIME's top: the line less 51 ms (the guards, SPRAY's least reach): 110 252 samples, 2.500 s (the line 112 512, 2.551 s) */
#define TX_GD_TOP (TX_GD_N - 2260u)
/* TIME 0 .. 127 as they were (20 ms .. 1.664 s, the old line's top, exponential); past 127 the same steps go on up
 * to TX_GD_TOP, the last one cut to it: TX_GD_TIME_MAX the least step that reaches it (gen_fx_tables checks) */
#define TX_GD_TOP0 73388u
#define TX_GD_TIME_MAX 139u

#endif
