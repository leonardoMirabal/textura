/* SPDX-License-Identifier: GPL-3.0-only */
/* The FM-1 in a web browser: platform.c includes this file instead of the hardware when DINGHY_WEB is set (with
 * DINGHY_HOST, so the rest of the code takes its host paths). The app (frodhes.c) runs unchanged, compiled to
 * WebAssembly with Emscripten (web/build.sh):
 *
 *   app_main()      runs as ever; platform_poll() hands the browser a turn (Asyncify: emscripten_sleep), so the
 *                   page stays alive and the clock (fm1_ms) follows the real time
 *   web_audio(n)    the audio ISR: JavaScript's audio callback asks for n blocks of 256 frames, as the FM-1's
 *                   I2S interrupt does; the result is in web_out (float, L R L R), MASTER applied
 *   web_btn / web_keys / web_enc / web_midi / web_master   the panel and USB-MIDI, from the page
 *   web_fb / web_leds / web_store                          the screen (RGB565), the LEDs, the settings record
 *
 * Settings live in web_store (the page keeps it in the browser's localStorage); the user preset bank stays in
 * memory for the session. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <emscripten.h>

#ifndef APP_NAME
#define APP_NAME "DINGHY"
#endif
#ifndef APP_VER
#define APP_VER "web"
#endif
#define FS 44100u
#define HALF_FRAMES 256u

static volatile uint32_t fm1_ms;
static struct { uint32_t notes, buttons; } fm1_in;
static uint32_t host_edges, host_released;
static int32_t host_enc[8];
static uint8_t host_led[64];
static uint32_t fm1_input_edges(uint32_t *released) { uint32_t b = host_edges; host_edges = 0; *released = host_released; host_released = 0; return b; }
static uint32_t fm1_input_note_edges(void) { return 0; }
static int32_t fm1_enc_take(uint32_t e) { int32_t d = e < 8u ? host_enc[e] : 0; if (e < 8u) host_enc[e] = 0; return d; }
static void fm1_led_key(uint32_t id, int on) { if (id < 64u) host_led[id] = (uint8_t)(on != 0); }
static volatile uint8_t uart_midi_on = 1;        /* midi_uart.c: the TRS input (none here) */
static volatile uint8_t usb_pre_master;          /* platform.c: USB audio before MASTER (no USB here) */
static uint8_t fm1_led_duty[2] = {16u, 16u};   /* hal/fm1_input.h: the lit duty of the keys and the buttons, /16 (DISPLAY BRIGHTNESS) */
static void fm1_led_group_ids(uint32_t id0, uint32_t id1) { (void)id0; (void)id1; }   /* (which LEDs are the keys: the hardware's) */
static void fm1_led_build(void) {}               /* (the scan ISR's tables: the hardware's) */
static uint8_t host_glow[64];                   /* the dim layer (fm1_led_key_glow) */
static void fm1_led_key_glow(uint32_t id, int on) { if (id < 64u) host_glow[id] = (uint8_t)(on != 0); }

/* the screen: 240 x 240 RGB565 (gfx.c sends the bytes swapped, as the ST7789 wants them) */
static uint16_t fb[240 * 240];
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = c;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u) {
                uint16_t c = px[j * w + i];
                fb[(y + j) * 240u + x + i] = (uint16_t)(c >> 8 | c << 8);
            }
}
#ifndef CV_MAX                                   /* (Textura: the app's own, textura.c) */
#define CV_MAX (240u * 64u)
#endif
#include "gfx.c"

/* settings: one record, kept by the page in localStorage */
static uint8_t web_store_buf[256 + 4];
static uint8_t web_store_dirty;
static int settings_load(void *p, uint32_t n)
{
    if (web_store_buf[0] != 1 || n > 256u) return -1;
    memcpy(p, web_store_buf + 4, n);
    return 0;
}
static int settings_save(const void *p, uint32_t n)
{
    web_store_buf[0] = 1;
    memcpy(web_store_buf + 4, p, n);
    web_store_dirty = 1;
    return 0;
}
static int settings_save_start(const void *p, uint32_t n) { return settings_save(p, n); }   /* (at once: 0 done) */
static int settings_save_poll(void) { return 0; }

/* Textura: a window of the flash for the app's own records (the scene banks: scenes.c), WEB_FLW_LEN bytes from
 * store.h's scenes; the page keeps it in localStorage too (web_flw, web_flw_take_dirty) */
#include "store.h"
#define WEB_FLW_AT SCENES_AT
#ifdef SCENES_WIN_SECT
#define WEB_FLW_LEN (SCENES_WIN_SECT * 0x1000u)   /* (more lanes, the plugin: store.h) */
#else
#define WEB_FLW_LEN (SCENES_SECT * 0x1000u)
#endif
static uint8_t web_flw[WEB_FLW_LEN], web_flw_ok, web_flw_dirty;
static int web_flw_in(uint32_t at, uint32_t n)
{
    if (!web_flw_ok) { memset(web_flw, 0xFF, sizeof web_flw); web_flw_ok = 1; }
    return at >= WEB_FLW_AT && n <= WEB_FLW_LEN && at - WEB_FLW_AT <= WEB_FLW_LEN - n;
}
static int fl_window_read(uint32_t at, void *p, uint32_t n)
{
    if (!web_flw_in(at, n)) return -1;
    memcpy(p, web_flw + (at - WEB_FLW_AT), n);
    return 0;
}
static int fl_window_erase(uint32_t at)
{
    if ((at & 0xFFFu) || !web_flw_in(at, 0x1000u)) return -1;
    memset(web_flw + (at - WEB_FLW_AT), 0xFF, 0x1000u);
    web_flw_dirty = 1;
    return 0;
}
static int fl_window_write(uint32_t at, const void *p, uint32_t n)
{
    uint32_t i;
    if (!web_flw_in(at, n)) return -1;
    for (i = 0; i < n; i++) web_flw[at - WEB_FLW_AT + i] &= ((const uint8_t *)p)[i];
    web_flw_dirty = 1;
    return 0;
}

/* USB-MIDI in (Web MIDI or the page); MIDI out is dropped */
static uint8_t mq[64][3];
static uint32_t mq_w, mq_r;
static int midi_get(uint8_t *st, uint8_t *d1, uint8_t *d2)
{
    if (mq_r == mq_w) return 0;
    *st = mq[mq_r % 64u][0]; *d1 = mq[mq_r % 64u][1]; *d2 = mq[mq_r % 64u][2];
    mq_r++;
    return 1;
}
static void midi_send(uint32_t st, uint32_t d1, uint32_t d2) { (void)st; (void)d1; (void)d2; }

static void app_audio(int32_t *out, uint32_t n);
static void app_main(void);

static void platform_poll(void)
{
#ifdef DINGHY_PLUGIN
    /* the desktop plugin (plugin/src/slot_bridge.h): the clock is the slot's own milliseconds, the one the next pass
     * runs in (as tests/host.c: its pass after millisecond k sees fm1_ms = k, the first one too) */
    fm1_ms = (uint32_t)emscripten_get_now();
    emscripten_sleep(1);
#else
    static double t0;
    double now = emscripten_get_now();
    if (t0 == 0.0) t0 = now;
    fm1_ms = (uint32_t)(now - t0);
    emscripten_sleep(1);                       /* the browser's turn: input, the screen, the audio callback */
#endif
}

/* ---- the page's side */
static float web_out[2u * HALF_FRAMES * 16u];
static int32_t web_master_q12 = 3500;

EMSCRIPTEN_KEEPALIVE float *web_audio(uint32_t blocks)
{
    static int32_t blk[2u * HALF_FRAMES];
    uint32_t b, i;
    if (blocks > 16u) blocks = 16u;
    for (b = 0; b < blocks; b++) {
        memset(blk, 0, sizeof blk);
        app_audio(blk, HALF_FRAMES);
        for (i = 0; i < 2u * HALF_FRAMES; i++) {
            int32_t v = (blk[i] * web_master_q12) >> 12;   /* MASTER, then the clip as platform.c does */
            v = v > 32767 ? 32767 : v < -32767 ? -32767 : v;
            web_out[b * 2u * HALF_FRAMES + i] = (float)v / 32768.0f;
        }
    }
    return web_out;
}
EMSCRIPTEN_KEEPALIVE void web_btn(uint32_t id, int down)
{
    if (id >= 32u) return;
    if (down) { host_edges |= 1u << id; fm1_in.buttons |= 1u << id; }
    else { host_released |= 1u << id; fm1_in.buttons &= ~(1u << id); }
}
EMSCRIPTEN_KEEPALIVE void web_keys(uint32_t mask) { fm1_in.notes = mask; }
EMSCRIPTEN_KEEPALIVE void web_enc(uint32_t id, int32_t d) { if (id < 8u) host_enc[id] += d; }
EMSCRIPTEN_KEEPALIVE void web_midi(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (mq_w - mq_r >= 64u) return;
    mq[mq_w % 64u][0] = (uint8_t)st; mq[mq_w % 64u][1] = (uint8_t)d1; mq[mq_w % 64u][2] = (uint8_t)d2;
    if (st >= 0xF8u) {                          /* its arrival and the pulse count (tempo.c, as usb.c stamps them) */
        static uint8_t clk_n;
        uint32_t stamp;
        if (st == 0xF8u) clk_n++;
        stamp = (fm1_ms & 0xFFFu) | (clk_n & 15u) << 12;
        mq[mq_w % 64u][1] = (uint8_t)stamp; mq[mq_w % 64u][2] = (uint8_t)(stamp >> 8);
    }
    mq_w++;
}
EMSCRIPTEN_KEEPALIVE void web_master(int32_t q12) { web_master_q12 = q12 < 0 ? 0 : q12 > 4096 ? 4096 : q12; }
EMSCRIPTEN_KEEPALIVE uint16_t *web_fb(void) { return fb; }
EMSCRIPTEN_KEEPALIVE uint8_t *web_leds(void)              /* per id: 1 lit, 2 the glow, 0 off */
{
    static uint8_t l[64];
    uint32_t i;
    for (i = 0; i < 64u; i++) l[i] = host_led[i] ? 1u : host_glow[i] ? 2u : 0u;
    return l;
}
EMSCRIPTEN_KEEPALIVE uint8_t *web_store(void) { return web_store_buf; }
EMSCRIPTEN_KEEPALIVE int web_store_take_dirty(void) { int d = web_store_dirty; web_store_dirty = 0; return d; }
EMSCRIPTEN_KEEPALIVE uint8_t *web_flw_buf(void) { (void)web_flw_in(WEB_FLW_AT, 0); return web_flw; }
EMSCRIPTEN_KEEPALIVE uint32_t web_flw_len(void) { return WEB_FLW_LEN; }
EMSCRIPTEN_KEEPALIVE int web_flw_take_dirty(void) { int d = web_flw_dirty; web_flw_dirty = 0; return d; }

int main(void)
{
    app_main();
    return 0;
}
