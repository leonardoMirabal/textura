/* SPDX-License-Identifier: GPL-3.0-only */
/* One slot of the desktop plugin (plugin/src/bridge.h): the app, firmware/src/textura.c unchanged, through the
 * browser HAL (web/web_hal.c: DINGHY_HOST + DINGHY_WEB, as web/build.sh builds it), then this slot's glue
 * (slot_bridge.h). From Frodhes' plugin/src/engine_unit.c. Compiled TXP_SLOTS times with -DTXP_SLOT=0..15
 * (plugin/cmake/slots.cmake): every symbol of the unit is static except the slot's table txp_slot_<TXP_SLOT>, which
 * the build checks with nm (no other global), so the copies never meet. The engines (the files in engines/) are linked once
 * beside the slots: they keep no state of their own outside the instances the app hands them (no mutable globals),
 * so every slot shares them. web_hal.c's exports become static here (plugin/compat/emscripten.h makes
 * EMSCRIPTEN_KEEPALIVE "static") and its main() is renamed; the host build's diagnostics (printf, getenv) are
 * compiled out. DINGHY_PLUGIN turns on the plugin's hook in web_hal.c (the clock: the slot's milliseconds).
 * LN_HQ (lanes.c): no CPU limit on a desktop, so nothing is thinned (the governor's pool has no end, every grain of
 * the grain delay plays). */
#ifndef TXP_SLOT
#error "plugin/src/engine_unit.c: build with -DTXP_SLOT=<0..15> (plugin/cmake/slots.cmake)"
#endif
#define DINGHY_HOST 1
#define DINGHY_WEB 1
#define DINGHY_PLUGIN 1
#ifndef LN_HQ
#error "the plugin's slots are the HQ build (plugin/cmake/slots.cmake TXP_ENGINE_DEFS: LN_HQ)"
#endif
#define main(...) static __attribute__((unused)) txp_web_main(__VA_ARGS__)   /* web_hal.c main(): never called */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define printf(...) ((void)0)
#define fprintf(...) ((void)0)
#define getenv(name) ((char *)0)

#include "../../firmware/src/textura.c"

#undef main
#undef printf
#undef fprintf
#undef getenv
#include "bridge.h"
#include "slot_bridge.h"

#define TXP_CAT2(a, b) a##b
#define TXP_CAT(a, b) TXP_CAT2(a, b)
const txp_slot_api_t TXP_CAT(txp_slot_, TXP_SLOT) = {
    .boot = sb_boot, .booted = sb_booted, .reuse = sb_reuse, .put = sb_put, .putx = sb_putx, .step = sb_step,
    .drain = sb_drain, .avail = sb_avail, .read = sb_read, .now = sb_now, .room = sb_room, .set_exact = sb_set_exact,
    .efr = sb_efr, .efw = sb_efw, .snapshot = sb_snapshot, .state_get = sb_state_get, .state_set = sb_state_set,
    .set_tempo = sb_set_tempo, .stats = sb_stats, .params = sb_params, .text = sb_text, .settle = sb_settle,
};
