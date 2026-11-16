/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Small-canvas renderer (no full framebuffer). Draw text/lines into
 * an off-screen strip, then blit it in one DMA transfer. Pixels are stored
 * byte-swapped (the panel takes RGB565 big-endian). */
typedef struct {               /* proportional, see tools/gen_font.py */
    uint8_t h;
    uint8_t pad;               /* bitmap starts this many pixels left of the pen */
    uint8_t first, last;
    const uint8_t *adv;        /* advance per glyph */
    const uint8_t *bw;         /* bitmap width per glyph (starts FONT_PAD left of the pen) */
    const uint16_t *off;       /* each glyph's first bit */
    const uint8_t *data;       /* one bit a pixel, h rows of bw bits, MSB first (a set bit: alpha 15) */
} felucca_font_t;
#include "felucca_font1.h"     /* (tools/gen_font1.py: felucca_font.h's Terminus, a bitmap font, at one bit a pixel) */

#ifndef CV_MAX
#define CV_MAX (240u * 124u)      /* the graph strip is 240 x 124 */
#endif
#ifdef DINGHY_HOST
static uint16_t cv_px[CV_MAX];                  /* host mode: no .pool (Mach-O has no such section) */
#else
static uint16_t cv_px[CV_MAX] __attribute__((section(".pool")));
#endif
static uint32_t cv_w, cv_h;
static int32_t cv_oy;            /* y offset for graph drawing */
/* Textura: strips. A canvas taller than CV_MAX allows is drawn in strips of CV_MAX / w rows: the part's drawing runs
 * once a strip (cv_strips), cv_begin gives each pass the next strip (cv_s) and sets cv_oy so the drawing lands in it
 * (callers that offset their drawing add to cv_oy after cv_begin), cv_blit puts it at its rows. The pixels are those of
 * one canvas as tall as the part. */
static uint32_t cv_s;            /* the strip this pass draws */
static uint32_t cv_y0;           /* its first row in the part */
static uint8_t cv_more;          /* a part begun this pass has rows past this strip */

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define C_BLACK 0x0000u
#define C_WHITE 0xFFFFu              /* accent only: what is being touched / where we are */
/* The screen is five steps of one colour, darkest to brightest, plus white.
 * Palettes are picked on the DISPLAY page (COLOR: params.c color_apply). */
typedef struct {
    const char *name;
    uint16_t c[5];
} palette_t;
static const palette_t PALETTES[] = {
    {"GREEN", {RGB(0, 40, 12), RGB(0, 84, 30), RGB(16, 140, 54), RGB(56, 200, 92), RGB(120, 255, 146)}},
    {"AMBER", {RGB(60, 26, 0), RGB(110, 50, 0), RGB(170, 82, 0), RGB(225, 120, 8), RGB(255, 166, 40)}},
    {"CYAN", {RGB(0, 30, 50), RGB(0, 62, 96), RGB(16, 112, 160), RGB(56, 172, 222), RGB(140, 222, 255)}},
    {"RED", {RGB(52, 8, 8), RGB(100, 18, 14), RGB(170, 36, 26), RGB(226, 64, 48), RGB(255, 112, 92)}},
    {"MONO", {RGB(40, 40, 40), RGB(80, 80, 80), RGB(130, 130, 130), RGB(186, 186, 186), RGB(226, 226, 226)}},
    {"DX", {RGB(16, 46, 46), RGB(26, 90, 92), RGB(40, 150, 152), RGB(56, 210, 212), RGB(66, 245, 245)}},   /* sloopDX: cyan */
};
#define NPALETTES (sizeof(PALETTES) / sizeof(PALETTES[0]))
static uint16_t pal[5];
#define C_LINE pal[0]                /* 1 rules, separators */
#define C_DIM pal[1]                 /* 2 inactive, empty steps, units */
#define C_GRAY pal[2]                /* 3 labels */
#define C_AMB pal[3]                 /* 4 secondary text */
#define C_HI pal[4]                  /* 5 values, curves */

static void palette_set(uint32_t i)
{
    uint32_t k;
    for (k = 0; k < 5u; k++)
        pal[k] = PALETTES[i % NPALETTES].c[k];
}

static inline uint16_t swap16(uint32_t c) { return (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8)); }

static void cv_begin(uint32_t w, uint32_t h, uint16_t bg)
{
    uint32_t i, n, r = CV_MAX / w;  /* (Textura: the strip cv_s of a part h rows tall) */
    lcd_sync();                     /* the last blit may still read cv_px */
    cv_w = w;
    cv_y0 = cv_s * r;
    cv_h = cv_y0 < h ? (h - cv_y0 < r ? h - cv_y0 : r) : 0u;
    if (h > cv_y0 + r)
        cv_more = 1;
    cv_oy = -(int32_t)cv_y0;
    n = w * cv_h;
    for (i = 0; i < n; i++)
        cv_px[i] = swap16(bg);
}

static void cv_blit(uint32_t x, uint32_t y)
{
    if (cv_h)
        lcd_blit(x, y + cv_y0, cv_w, cv_h, cv_px);
}

/* canvas rows r0 .. the part's last only, to screen row y + r0 */
static void cv_blit_from(uint32_t x, uint32_t y, uint32_t r0)
{
    if (r0 < cv_y0)
        r0 = cv_y0;
    if (r0 < cv_y0 + cv_h)
        lcd_blit(x, y + r0, cv_w, cv_y0 + cv_h - r0, cv_px + (r0 - cv_y0) * cv_w);
}

/* rows y .. y + h - 1 of the part (in its drawing's coordinates, cv_oy's offset included) fall in this strip */
static int cv_has(int32_t y, int32_t h)
{
    y += cv_oy;
    return y < (int32_t)cv_h && y + h > 0;
}

/* f(a) draws one part (cv_begin .. cv_blit, one or more canvases): once a strip, until no part has rows left */
static void cv_strips(void (*f)(uint32_t), uint32_t a)
{
    for (cv_s = 0;; cv_s++) {
        cv_more = 0;
        f(a);
        if (!cv_more)
            break;
    }
    cv_s = 0;
}

static inline void cv_pset(int32_t x, int32_t y, uint16_t c)
{
    y += cv_oy;
    if ((uint32_t)x < cv_w && (uint32_t)y < cv_h)
        cv_px[(uint32_t)y * cv_w + (uint32_t)x] = swap16(c);
}

static void cv_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)
{
    int32_t x1 = x + w, y1 = y + h + cv_oy, i;
    uint16_t sc = swap16(c);
    y += cv_oy;                             /* clipped once, then filled row by row */
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > (int32_t)cv_w)
        x1 = (int32_t)cv_w;
    if (y1 > (int32_t)cv_h)
        y1 = (int32_t)cv_h;
    for (; y < y1; y++)
        for (i = x; i < x1; i++)
            cv_px[(uint32_t)y * cv_w + (uint32_t)i] = sc;
}

static void cv_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c)
{
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy, guard = 2000;
    while (guard--) {                       /* bounded: a line is never longer than 480 px */
        int32_t e2 = 2 * err;               /* both tests use the same error value */
        cv_pset(x0, y0, c);
        if (x0 == x1 && y0 == y1)
            break;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* glyph index of a character: lower case folds to upper case when the font
 * has none, anything missing (and C1 controls) draws as '?' */
static uint32_t glyph(const felucca_font_t *f, uint32_t ch)
{
    if (ch >= 'a' && ch <= 'z' && f->last < 'a')
        ch -= 32u;
    if (ch < f->first || ch > f->last || (ch >= 127u && ch < 160u))
        ch = '?';
    return ch - f->first;
}

/* text, alpha-blended onto black with colour c; returns the end x */
static int32_t cv_text(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    uint16_t ramp[16];
    uint32_t r = c >> 11, g = (c >> 5) & 63u, b = c & 31u, a;
    for (a = 0; a < 16u; a++)
        ramp[a] = (uint16_t)(((r * a / 15u) << 11) | ((g * a / 15u) << 5) | (b * a / 15u));
    for (; *s; s++) {
        uint32_t gi = glyph(f, (uint8_t)*s), gx, gy, w, bit;
        w = f->bw[gi];
        bit = f->off[gi];
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++, bit++)
                if ((f->data[bit >> 3] >> (7u - (bit & 7u))) & 1u)
                    cv_pset(x - f->pad + (int32_t)gx, y + (int32_t)gy, ramp[15]);
        x += f->adv[gi];
    }
    return x;
}

static int32_t text_w(const felucca_font_t *f, const char *s)
{
    int32_t w = 0;
    for (; *s; s++)
        w += f->adv[glyph(f, (uint8_t)*s)];
    return w;
}

/* one-shot: text in a box, cleared to black, blitted */
static void draw_text_box(uint32_t x, uint32_t y, uint32_t w, const felucca_font_t *f, const char *s,
                          uint16_t c, int align)
{
    int32_t tw = text_w(f, s), tx = 0;
    cv_begin(w, f->h, C_BLACK);
    if (align == 1)
        tx = ((int32_t)w - tw) / 2;
    else if (align == 2)
        tx = (int32_t)w - tw;
    cv_text(tx, 0, f, s, c);
    cv_blit(x, y);
    lcd_sync();                     /* one-shots (boot, crash, UBOOT, update) finish here */
}
