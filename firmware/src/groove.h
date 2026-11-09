/* SPDX-License-Identifier: GPL-3.0-only */
/* GROOVE's state, for lanes.c's hook (LN_GROOVE) and the params; the rest is groove.c's (included after mod.c).
 * engines/tx_groove.h tells what it is: a pulse the landscape's discrete events lean toward, never a beat. */
#include "tx_groove.h"

#define LN_GROOVE                               /* lanes.c: each lane's generator rate (and ACTIVITY) leans to it */
#ifdef DINGHY_HOST
#define GR_BSS
#else
#define GR_BSS __attribute__((section(".bss.groove")))   /* (GROOVE's state on its own: the compiler's merged pool of the
                                                 * others' variables keeps its short offsets; zeros at boot, gr_init) */
#endif
#define gr_amount pv_glob[PG_GRV0]              /* AMOUNT TYPE SHIFT (params.c's values; gr_init: the defaults) */
#define gr_type pv_glob[PG_GTYPE]
#define gr_shift pv_glob[PG_GSHIFT]
static GR_BSS volatile uint8_t gr_amt_mod, gr_amt_on;   /* the matrix's GROOVE AMOUNT (mod.c, in the interrupt) */
static GR_BSS uint8_t gr_set[2][4];             /* each lane's PART (0 AUTO, then PULSE OFFBEAT FILL FREE) and CYCLE (0 AUTO,
                                                 * 1 .. 8: 2 .. 9 pulses over the span): params.c PL_GPART, PL_GCYC */
static GR_BSS uint8_t gr_rephase;               /* bar 1 set anew (tempo.c: START, a tap's re-phase): mod.c snaps the song
                                                 * position */
static int32_t gr_isr(uint32_t k);              /* one block of lane k: its multiplier (Q12; groove.c) */
static uint8_t gr_act(int32_t m, uint32_t in);  /* the lane's ACTIVITY leaning with it */
#ifdef DINGHY_HOST
static void gr_note_host(uint32_t k);           /* the host's checks: a note on lane k (mod.c's mod_key_isr) */
#endif
