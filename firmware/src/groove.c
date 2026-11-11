/* SPDX-License-Identifier: GPL-3.0-only */
/* GROOVE: the landscape's discrete events lean toward one shared, slowly drifting pulse (engines/tx_groove.h), felt
 * rather than heard: never a beat, never a loop. Water and wind stay free.
 *
 *   the ensemble  AMOUNT, TYPE, SHIFT for the landscape (the RHYTHM page; the lanes' roles: GROOVE LANES; a scene
 *                 keeps them; not an effect: EFFECTS ON LOAD leaves nothing of it); each lane plays a PART: AUTO gives the lanes whose sounds respond PULSE,
 *                 OFFBEAT, FILL, PULSE by their weight (the most first, then lane order), each lane's PART or CYCLE (2
 *                 .. 9 pulses over the span: polyrhythms) set by hand over it
 *   the weights   each sound's response, 2 bits (GR_SW: 0 none: flows, water, wind, rain beds, the fire's roar; 1
 *                 light: drips, sparks, hail; 2 more: stones, knocks, chimes, glass; 3 most: insects, birds, cracks
 *                 and crackles). Kept here, by the sounds' order in landscapes.h (that file is generated from
 *                 audition/app.js: a sound added there needs its weight here; one missing reads 0, free)
 *   the lanes     the generator's notes come at a rate times the groove's multiplier (lanes.c, the interrupt: tx_gen
 *                 rlean), its ACTIVITY leans the same way (a particle cloud swells on the pulse: gr_act); a
 *                 classic engine's presses (scape.c) count their gaps in the groove's time (gr_due)
 *   the clock     the song position (mod.c mpos): the internal tempo, or the clock that leads (FOLLOW / AUTO); a
 *                 START (the transport, a third tap) snaps it to bar 1, so the grooved lanes start in phase
 * AMOUNT 0 (and the matrix's route at 0): every lane exactly as without it. Main loop, but gr_isr (the interrupt). */

static GR_BSS tx_groove_lane_t gr_lane[4];      /* (the interrupt's; part, cycle, wl set by the main loop) */
static GR_BSS volatile int32_t gr_m[4];         /* each lane's rate multiplier now (Q12; gr_init: 4096) */
static GR_BSS uint32_t grd_q[SC_N], grd_t[SC_N];   /* gr_due: the press's time it counts to, when last counted */
static GR_BSS int32_t grd_rem[SC_N];             /* (the gap left, ms Q8) */

/* the sounds' weights: family f, sound i at bits 2i .. 2i+1 */
static const uint32_t GR_SW[LS_N] = {
    0x000150u,      /* Water: drops, cave drips, eaves 1; rain, lapping, brook, springs, falls, waves 0 */
    0x084266u,      /* Stones & sand: stones, ringing rocks, marbles, clinks 2; rockfall, gravel, minerals 1; sand,
                     * dune, friction 0 */
    0x06BF5Au,      /* Ice & glass: lake ice, booming, thin ice 3; icicles, ice in a glass, pane, bottles 2; frost,
                     * snow, shards 1 */
    0x046FBFu,      /* Fire: campfire, bonfire, fireplace, kindling, pine 3; embers, leaves 2; torch, sparks 1; flame 0 */
    0x023FFFu,      /* Wood: the seven woods 3; paper 2; rain stick 0 */
    0x200000u,      /* Wind: wind chimes 2; the winds 0 */
    0x415000u,      /* Storm: the hail 1, spring echo 1; rain, storm, thunder, gales, sleet, thunder sheet 0 */
    0x35FFFFu,      /* Insects: 3, the bees 1 */
    0x0FFFFFu,      /* Songbirds: 3 */
    0x0FFFFFu,      /* Wild birds: 3 */
};
static uint32_t gr_wl(uint32_t k)               /* lane k's sound's weight */
{
    if (sc.snd[k] < 0) return 0;
    return GR_SW[sc.lf[k]] >> (2u * (uint32_t)sc.snd[k]) & 3u;
}

static int32_t gr_isr(uint32_t k)               /* (1 .. under 24000: tx_groove_mul's range) */
{
    return gr_m[k & 3u] = tx_groove_mul(&gr_lane[k & 3u], gr_type, gr_amt_on ? gr_amt_mod : gr_amount, gr_shift, mpos);
}

/* ACTIVITY leaning as the notes do: the particles' rate goes as ACTIVITY^2.5 (the perceptually even curve,
 * tx_core_lin.h), so a rate x m is the ACTIVITY x m^0.4: 1 + 0.4 x / (1 + 0.15 x), x = m - 1 (within 4 % over the
 * multipliers' 0.6 .. 5) */
static uint8_t gr_act(int32_t m, uint32_t in)
{
    int32_t x = m - 4096, f = 4096 + x * 1638 / (4096 + (x * 614 >> 12));   /* (Q12) */
    uint32_t a = (in * (uint32_t)f + 2048u) >> 12;
    return (uint8_t)(a > 127u ? 127u : a);
}
static void gr_init(void)
{
    uint32_t k;
    gr_amount = TX_GR_AMOUNT_DEF; gr_type = TX_GR_TYPE_DEF;
    for (k = 0; k < SC_N; k++) {                /* (its own seeds: tx_seed's sequence stays the lanes'; on the FM-1 the
                                                 * free-running timer: never the same twice) */
        uint32_t s = 0x6A09E667u ^ (k + 1u) * 0x9E3779B9u;
#ifndef DINGHY_HOST
        s ^= fm1_ticks();
#endif
        tx_groove_lane_init(&gr_lane[k], s);
        gr_m[k] = 4096;
    }
}
/* each lane's weight, cycle and part: its own, or AUTO by the weight's rank (the most first, then lane order) PULSE,
 * OFFBEAT, FILL, PULSE; a weight 0: FREE */
static void gr_poll(void)
{
    uint32_t k, j, r, w[SC_N];
    for (k = 0; k < SC_N; k++) w[k] = gr_wl(k) << 2 | (3u - k);   /* (the weight, then the lane: a rank each) */
    for (k = 0; k < SC_N; k++) {
        for (j = 0, r = 0; j < SC_N; j++) r += w[j] > w[k];
        gr_lane[k].wl = (uint8_t)(w[k] >> 2);
        gr_lane[k].cycle = (uint8_t)(gr_set[1][k] ? gr_set[1][k] + 1u : 0u);
        gr_lane[k].part = (uint8_t)(gr_set[0][k] ? gr_set[0][k] : w[k] < 4u ? TX_GRP_FREE : (0x1321u >> 4u * r & 15u));
    }                                           /* (0x1321: PULSE OFFBEAT FILL PULSE by rank) */
}

/* scape.c: lane k's next press is due. Its gap (q_next) runs in the groove's time: the multiplier speeds it up near
 * the pulse, slows it between. Untouched by the groove (4096 all along): the plain time, exactly */
static int gr_due(uint32_t k)
{
    uint32_t q = sc.q_next[k], now = fm1_ms;
    if (q != grd_q[k]) { grd_q[k] = q; grd_t[k] = now; grd_rem[k] = (int32_t)(q - now) * 256; }
    grd_rem[k] -= (int32_t)((now - grd_t[k] < 60000u ? now - grd_t[k] : 60000u) * (uint32_t)gr_m[k] >> 4);
    grd_t[k] = now;
    return grd_rem[k] <= 0;
}

#ifdef DINGHY_HOST
/* the host's checks (tests/scripts/groove.txt): each lane's notes since the counts were read, and those within 1/32
 * of a beat (3 ticks) of its pulse's strong slots; TX_GROOVE_LOG names a file that gets each note (lane, ms, song
 * position) for a study */
static uint32_t grh_n[SC_N], grh_near[SC_N];
static void gr_note_host(uint32_t k)
{
    static FILE *f;
    static int opened;
    int32_t d;
    if (k >= SC_N) return;
    d = tx_groove_dist(&gr_lane[k], gr_type, gr_shift, mpos >> 8);
    grh_n[k]++;
    grh_near[k] += d <= 3;
    if (!opened) { const char *p = getenv("TX_GROOVE_LOG"); opened = 1; f = p ? fopen(p, "w") : 0; }
    if (f) fprintf(f, "%u %u %u %u %d\n", k, fm1_ms, mpos >> 8, gr_lane[k].part, (int)d);
}
#endif
