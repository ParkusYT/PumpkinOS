/* ===========================================================================
 * PumpkinOS - 2D graphics primitives (indexed back buffer + blit)
 * ========================================================================= */
#include "gfx.h"
#include "vga.h"
#include "kheap.h"
#include "string.h"
#include <stdint.h>

static uint8_t  *buf;              /* w*h palette-indexed back buffer */
static int       gw, gh;
static uint32_t  lut[256];         /* index -> 0x00RRGGBB (for 32-bpp blits) */

int gfx_init(void) {
    gw = vga_width();
    gh = vga_height();
    buf = (uint8_t *)kmalloc((uint32_t)gw * gh);
    return buf ? 0 : -1;
}

void gfx_shutdown(void) {
    if (buf) { kfree(buf); buf = 0; }
}

int gfx_width(void)  { return gw; }
int gfx_height(void) { return gh; }

void gfx_set_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    /* 6-bit -> 8-bit for the true-colour lookup table. */
    uint8_t r8 = (uint8_t)((r << 2) | (r >> 4));
    uint8_t g8 = (uint8_t)((g << 2) | (g >> 4));
    uint8_t b8 = (uint8_t)((b << 2) | (b >> 4));
    lut[index] = ((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8;
    if (vga_bpp() == 8)
        vga_set_palette(index, r, g, b);
}

void gfx_clear(uint8_t color) {
    memset(buf, color, (uint32_t)gw * gh);
}

void gfx_pixel(int x, int y, uint8_t color) {
    if (x < 0 || x >= gw || y < 0 || y >= gh)
        return;
    buf[y * gw + x] = color;
}

void gfx_fill_rect(int x, int y, int w, int h, uint8_t color) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > gw) w = gw - x;
    if (y + h > gh) h = gh - y;
    for (int yy = 0; yy < h; yy++) {
        uint8_t *row = &buf[(y + yy) * gw + x];
        for (int xx = 0; xx < w; xx++)
            row[xx] = color;
    }
}

void gfx_hline(int x, int y, int w, uint8_t color) { gfx_fill_rect(x, y, w, 1, color); }
void gfx_vline(int x, int y, int h, uint8_t color) { gfx_fill_rect(x, y, 1, h, color); }

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
    for (; *s; s++) { gfx_char(x, y, *s, fg, bg); x += FONT_W; }
}

int gfx_text_width(const char *s) { return (int)strlen(s) * FONT_W; }

void gfx_present(void) {
    uint8_t *fb = vga_framebuffer();
    uint32_t pitch = vga_pitch();
    int bpp = vga_bpp();

    for (int y = 0; y < gh; y++) {
        uint8_t *src = buf + y * gw;
        uint8_t *row = fb + (uint32_t)y * pitch;

        if (bpp == 8) {
            memcpy(row, src, gw);
        } else if (bpp == 32) {
            uint32_t *dst = (uint32_t *)row;
            for (int x = 0; x < gw; x++)
                dst[x] = lut[src[x]];
        } else if (bpp == 24) {
            for (int x = 0; x < gw; x++) {
                uint32_t c = lut[src[x]];
                uint8_t *p = row + x * 3;
                p[0] = (uint8_t)(c);         /* B */
                p[1] = (uint8_t)(c >> 8);    /* G */
                p[2] = (uint8_t)(c >> 16);   /* R */
            }
        } else {                              /* 16 bpp (5:6:5) */
            uint16_t *dst = (uint16_t *)row;
            for (int x = 0; x < gw; x++) {
                uint32_t c = lut[src[x]];
                uint8_t r = (uint8_t)(c >> 16), g = (uint8_t)(c >> 8), b = (uint8_t)c;
                dst[x] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
        }
    }
}
