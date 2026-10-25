/* SPDX-License-Identifier: GPL-3.0-only */
/* Textura DRIVE TYPEs: the state of Frodhes' four drive TYPEs (drive.c, drv_khlon.c, drv_tsh8.c, drv_ohcd.c,
 * drv_bhlue.c: our own) as instances. Each lane's DRIVE holds one TYPE's state at a time (a union: one TYPE runs on a
 * lane at a time); what every lane may share sits once in the effects (tx_drv_shared_t): the scratch buffers (the
 * lanes run one after the other) and TSH8's two diode tables (12.2 KB: TSH8 runs on one lane at a time). The code:
 * tx_fx_drive.c (the framework) and tx_fx_drv_<type>.c (included by it); the tables: tx_drive_tables.h. */
#ifndef TX_FX_DRV_H
#define TX_FX_DRV_H
#include <stdint.h>

enum { TX_DRT_FDRV, TX_DRT_KHLON, TX_DRT_TSH8, TX_DRT_OHCD, TX_DRT_BHLUE, TX_DRT_N };   /* DRIVE TYPE (append only) */
#define TX_DRV_BLK 256u            /* the most samples a TYPE runs a call (the lane's block) */
#define TX_DRV_TS_NODES 761u       /* TSH8's diode table (tx_drive_tables.h TSH8_NODES: checked there) */

typedef struct {                   /* KHLON: the rows that run, the transposed form II states, the clipper */
    int32_t o1[5], cl[3], sl[3], po[5];
    int64_t so1[2], spo[2], scl, ssl;
    int32_t sc, e1, F1, v1, kc4, rail;
    const int32_t *gf;
    int32_t ho1[4], hcl[4], hsl[4], hpo[4];   /* each section's last inputs and outputs (a row change rebuilds) */
    uint8_t aa, ra, rt, pre, dip, held;
} tx_drv_kh_t;
typedef struct {                   /* TSH8 (its tables: tx_drv_shared_t) */
    int32_t is[4], bs[4], ts[4], as[2];       /* the input biquad, the branch, the tone; the clipper's {x1, F1} */
    int32_t br[5], tone[5];                   /* the branch and tone rows that run (tone: b x the level) */
    int32_t ra, rt;                           /* the AMOUNT and TONE steps that run (-1: take the knobs) */
} tx_drv_ts_t;
typedef struct { int32_t b0, b1, b2, a1, a2, s0, s1, s2, r1, r2, o0, o1, o2, p1, p2; } tx_drv_ohk_t;
typedef struct { int32_t lp, x1, x2, y1, y2, au1, aF1, u1, u2, v1, v2, rp, t1, t2, w1, w2; } tx_drv_ohs_t;
typedef struct { tx_drv_ohk_t k; tx_drv_ohs_t s; int32_t ra, rt; } tx_drv_oh_t;                /* OHCD */
typedef struct { int32_t pa, ph, b0, b1, b2, a1, a2, c0, c1, c2, d1, d2, lo, hi, s0, s1, sp; } tx_drv_bhk_t;
typedef struct { int32_t xa1, x1, h1, h2, u1, u2, w1, w2, z1, z2, xo, up, ax1, aF1; } tx_drv_bhs_t;
typedef struct { tx_drv_bhk_t k; tx_drv_bhs_t s; int32_t ra, rt; } tx_drv_bh_t;                /* BHLUE */
typedef union { tx_drv_kh_t kh; tx_drv_ts_t ts; tx_drv_oh_t oh; tx_drv_bh_t bh; } tx_drv_state_t;

typedef struct {
    int32_t q[TX_DRV_BLK];                    /* the lane's block at Q15, then the TYPE's wet signal (in place) */
    int32_t t1[TX_DRV_BLK], t2[TX_DRV_BLK];   /* scratch: KHLON's driven path and its sum while it dips; TSH8's Vi */
    int32_t ts_tab[2][2u * TX_DRV_TS_NODES];  /* TSH8: the diode table that runs ({g, F} a node), and the next one */
    int32_t ts_in[5];                         /* TSH8: the input biquad (fixed) */
    int32_t ts_trow, ts_brow, ts_bpos;        /* the table's AMOUNT step (-1: none), the one being built, where */
    int32_t ts_v0, ts_v1, ts_gp;              /* the build's keys and g at its node */
    int64_t ts_s;                             /* the build's running sum */
    uint8_t ts_cur;                           /* the table that runs */
    const void *ts_own;                       /* the lane's DRIVE that has TSH8 (0: free) */
} tx_drv_shared_t;

#endif
