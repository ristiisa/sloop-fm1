/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Small-canvas renderer (no full framebuffer). Draw text/lines into
 * an off-screen strip, then blit it in one DMA transfer. Pixels are stored
 * byte-swapped (the panel takes RGB565 big-endian). */
typedef struct {               /* one 8 x 16 bitmap font at 1x or 2x, see tools/gen_font.py */
    uint8_t h;
    uint8_t scale;
    uint8_t last;              /* beyond: '?'; lower case folds to upper case below 'a' */
} felucca_font_t;
#include "felucca_font.h"

#define CV_MAX (240u * 124u)      /* the graph strip is 240 x 124 */
static uint16_t cv_px[CV_MAX] __attribute__((section(".pool")));
static uint32_t cv_w, cv_h;
static int32_t cv_oy;            /* y offset for graph drawing */

#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define C_BLACK 0x0000u
#define C_WHITE 0xFFFFu              /* accent only: what is being touched / where we are */
/* The screen is five steps of one colour, darkest to brightest, plus white.
 * Palettes are picked in the HOME-hold menu (COLOR). */
typedef struct {
    const char *name;
    uint16_t c[5];
    uint8_t inv;                 /* the panel shows the negative (lcd_invert): black is white, c[] as their complements */
} palette_t;
static const palette_t PALETTES[] = {
    {"GREEN", {RGB(0, 40, 12), RGB(0, 84, 30), RGB(16, 140, 54), RGB(56, 200, 92), RGB(120, 255, 146)}},
    {"AMBER", {RGB(60, 26, 0), RGB(110, 50, 0), RGB(170, 82, 0), RGB(225, 120, 8), RGB(255, 166, 40)}},
    {"CYAN", {RGB(0, 30, 50), RGB(0, 62, 96), RGB(16, 112, 160), RGB(56, 172, 222), RGB(140, 222, 255)}},
    {"RED", {RGB(52, 8, 8), RGB(100, 18, 14), RGB(170, 36, 26), RGB(226, 64, 48), RGB(255, 112, 92)}},
    {"MONO", {RGB(40, 40, 40), RGB(80, 80, 80), RGB(130, 130, 130), RGB(186, 186, 186), RGB(226, 226, 226)}},
    /* inverted, for a dim panel: a white screen, dark text (on the panel: light grey / green .. black / dark green) */
    {"INV BW", {RGB(40, 40, 40), RGB(80, 80, 80), RGB(130, 130, 130), RGB(186, 186, 186), RGB(226, 226, 226)}, 1},
    {"INV GRN", {RGB(65, 35, 57), RGB(125, 70, 110), RGB(195, 115, 175), RGB(235, 155, 210), RGB(255, 195, 235)}, 1},
};
#define NPALETTES (sizeof(PALETTES) / sizeof(PALETTES[0]))
static uint16_t pal[5];
static uint8_t pal_inv;              /* the palette wants the panel inverted (main loop: lcd_invert) */
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
    pal_inv = PALETTES[i % NPALETTES].inv;
}

static inline uint16_t swap16(uint32_t c) { return (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8)); }

static void cv_begin(uint32_t w, uint32_t h, uint16_t bg)
{
    uint32_t i, n;
    if (w * h > CV_MAX)
        h = CV_MAX / w;
    lcd_sync();                     /* the last blit may still read cv_px */
    cv_w = w;
    cv_h = h;
    n = w * h;
    for (i = 0; i < n; i++)
        cv_px[i] = swap16(bg);
}

static void cv_blit(uint32_t x, uint32_t y) { lcd_blit(x, y, cv_w, cv_h, cv_px); }

/* canvas rows r0 .. cv_h-1 only, to screen row y + r0 */
static void cv_blit_from(uint32_t x, uint32_t y, uint32_t r0)
{
    if (r0 < cv_h)
        lcd_blit(x, y + r0, cv_w, cv_h - r0, cv_px + r0 * cv_w);
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
 * has none, anything missing (controls, Latin-1 but the U-umlaut) draws as '?' */
static uint32_t glyph(const felucca_font_t *f, uint32_t ch)
{
    if (ch >= 'a' && ch <= 'z' && f->last < 'a')
        ch -= 32u;
    if (ch == 0xDCu && f->last >= 'a')
        return FONT_UUML_GLYPH;
    if (ch < 32u || ch > f->last)
        ch = '?';
    return ch - 32u;
}

/* text in colour c (pixel doubled at 2x); returns the end x */
static int32_t cv_text(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    int32_t sc = f->scale, gx, gy, dx, dy;
    for (; *s; s++, x += 8 * sc) {
        const uint8_t *gd = FONT_BITS + glyph(f, (uint8_t)*s) * 16u;
        for (gy = 0; gy < 16; gy++) {
            uint32_t bits = gd[gy];
            for (gx = x; bits; gx += sc, bits = bits << 1 & 0xFFu)
                if (bits & 0x80u)
                    for (dy = 0; dy < sc; dy++)
                        for (dx = 0; dx < sc; dx++)
                            cv_pset(gx + dx, y + gy * sc + dy, c);
        }
    }
    return x;
}

static int32_t text_w(const felucca_font_t *f, const char *s)
{
    int32_t n = 0;
    while (s[n])
        n++;
    return n * 8 * f->scale;
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
