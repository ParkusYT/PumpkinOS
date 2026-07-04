/* ===========================================================================
 * PumpkinOS - 2D graphics primitives (back buffer + blit)
 * ========================================================================= */
#include "gfx.h"
#include "vga.h"
#include "string.h"
#include <stdint.h>

static uint8_t backbuf[GFX_W * GFX_H];

void gfx_clear(uint8_t color) {
    memset(backbuf, color, sizeof(backbuf));
}

void gfx_pixel(int x, int y, uint8_t color) {
    if (x < 0 || x >= GFX_W || y < 0 || y >= GFX_H)
        return;
    backbuf[y * GFX_W + x] = color;
}

void gfx_fill_rect(int x, int y, int w, int h, uint8_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > GFX_W) w = GFX_W - x;
    if (y + h > GFX_H) h = GFX_H - y;
    for (int yy = 0; yy < h; yy++) {
        uint8_t *row = &backbuf[(y + yy) * GFX_W + x];
        for (int xx = 0; xx < w; xx++)
            row[xx] = color;
    }
}

void gfx_hline(int x, int y, int w, uint8_t color) {
    gfx_fill_rect(x, y, w, 1, color);
}
void gfx_vline(int x, int y, int h, uint8_t color) {
    gfx_fill_rect(x, y, 1, h, color);
}

void gfx_rect(int x, int y, int w, int h, uint8_t color) {
    gfx_hline(x, y, w, color);
    gfx_hline(x, y + h - 1, w, color);
    gfx_vline(x, y, h, color);
    gfx_vline(x + w - 1, y, h, color);
}

void gfx_char(int x, int y, char c, uint8_t fg, int bg) {
    const uint8_t *glyph = vga_font() + (uint8_t)c * 32;
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_W; col++) {
            if (bits & (0x80 >> col))
                gfx_pixel(x + col, y + row, fg);
            else if (bg >= 0)
                gfx_pixel(x + col, y + row, (uint8_t)bg);
        }
    }
}

void gfx_text(int x, int y, const char *s, uint8_t fg, int bg) {
    for (; *s; s++) {
        gfx_char(x, y, *s, fg, bg);
        x += FONT_W;
    }
}

int gfx_text_width(const char *s) {
    return (int)strlen(s) * FONT_W;
}

void gfx_present(void) {
    memcpy(vga_framebuffer(), backbuf, sizeof(backbuf));
}
