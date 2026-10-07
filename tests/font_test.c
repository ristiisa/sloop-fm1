/* SPDX-License-Identifier: GPL-3.0-only */
/* Text rendering (firmware/src/gfx.c, build/gen/felucca_font.h): every byte the firmware draws
 * (ASCII and the U-umlaut of HUGELTON) in both fonts, clipped at the canvas edges and with the graph
 * y offset, hashed against the pixels of the 4-bit Latin-1 fonts it replaced (FONT_L was a table
 * of its own, now the same bitmaps at 2x): the screen stays the same. The rest of Latin-1 draws
 * as '?'. Also the cost of a line of text. */
#include <stdint.h>
#include <stdio.h>
#include <time.h>
static void lcd_sync(void) {}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *p)
{ (void)x; (void)y; (void)w; (void)h; (void)p; }
#include "../firmware/src/gfx.c"

static uint32_t hash, cnt;
static void mix(uint32_t v) { hash = (hash ^ v) * 16777619u; }
static void mix_canvas(void)
{
    uint32_t i;
    for (i = 0; i < cv_w * cv_h; i++)
        mix(cv_px[i]);
    cnt++;
}

static uint32_t render_all(const felucca_font_t *f)
{
    static const int32_t AT[][3] = {{3, 2, 0}, {-5, -7, 0}, {30, 30, 0}, {2, 3, 5}, {1, 1, -9}};
    char s[2] = {0, 0}, line[97];
    uint32_t ch, k;
    hash = 2166136261u;
    for (ch = 1; ch < 256u; ch++) {
        if (ch >= 160u && ch != 0xDCu)
            continue;
        for (k = 0; k < 5u; k++) {
            s[0] = (char)ch;
            cv_begin(40, 40, 0x0841u);
            cv_oy = AT[k][2];
            mix((uint32_t)cv_text(AT[k][0], AT[k][1], f, s, k & 1u ? 0xF81Fu : 0x7BEFu));
            mix((uint32_t)text_w(f, s));
            mix_canvas();
        }
    }
    for (k = 0; k < 96u; k++)                           /* a line: the glyphs side by side */
        line[k] = (char)(32u + (k * 37u) % 95u);
    line[96] = 0;
    cv_oy = 0;
    cv_begin(240, 40, 0);
    mix((uint32_t)cv_text(-3, 2, f, line, 0xFFFFu));
    mix((uint32_t)text_w(f, line));
    mix_canvas();
    return hash;
}

/* the canvas of one character */
static uint32_t one(const felucca_font_t *f, uint32_t ch)
{
    char s[2] = {(char)ch, 0};
    hash = 2166136261u;
    cv_oy = 0;
    cv_begin(40, 40, 0);
    cv_text(3, 2, f, s, 0xFFFFu);
    mix_canvas();
    return hash;
}

int main(void)
{
    static const struct { const felucca_font_t *f; const char *name; uint32_t want; } T[] = {
        {&FONT_S, "small font (1x): every byte, clipped, offset", 0x6f3dce7du},
        {&FONT_L, "large font (2x): every byte, clipped, offset", 0x1d75ee84u},
    };
    uint32_t i, n, fails = 0;
    for (i = 0; i < 2u; i++) {
        uint32_t h = render_all(T[i].f);
        int ok = h == T[i].want;
        printf("font: %-58s %s\n", T[i].name, ok ? "ok" : "FAIL");
        if (!ok)
            printf("      hash 0x%08x, want 0x%08x\n", h, T[i].want);
        fails += !ok;
    }
    for (i = 0; i < 2u; i++) {
        const felucca_font_t *f = i ? &FONT_L : &FONT_S;
        uint32_t ch, q = one(f, '?'), bad = 0;
        for (ch = 160u; ch < 256u; ch++)
            bad += ch != 0xDCu && one(f, ch) != q;
        printf("font: %-58s %s\n", i ? "large font: Latin-1 draws '?'" : "small font: Latin-1 but the U-umlaut draws '?'",
               bad ? "FAIL" : "ok");
        fails += bad != 0;
    }
    for (i = 0; i < 2u; i++) {                         /* the cost: a 28-character line, many times */
        const felucca_font_t *f = i ? &FONT_L : &FONT_S;
        struct timespec t0, t1;
        double ns;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (n = 0; n < 20000u; n++) {
            cv_begin(240, f->h, 0);
            cv_text(0, 0, f, "SLOOP 120.0 bpm ANALOG ab-12", 0xFFFFu);
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ns = ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 20000.0;
        printf("font: %s: a 28-character line with its canvas %.0f ns\n", i ? "L" : "S", ns);
    }
    return fails ? 1 : 0;
}
