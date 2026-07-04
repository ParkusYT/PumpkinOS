/* ===========================================================================
 * PumpkinOS - 2D graphics on top of the mode-13h framebuffer
 * ---------------------------------------------------------------------------
 * All drawing goes to an in-RAM back buffer; gfx_present() blits the whole
 * thing to the VGA framebuffer in one go, so the screen never shows a partly
 * drawn frame (no flicker). Text uses the font snapshotted by the VGA driver
 * (8x16 glyphs).
 * ========================================================================= */
#ifndef PUMPKIN_GFX_H
#define PUMPKIN_GFX_H

#include <stdint.h>

#define GFX_W 320
#define GFX_H 200

#define FONT_W 8
#define FONT_H 16

void gfx_clear(uint8_t color);
void gfx_pixel(int x, int y, uint8_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint8_t color);
void gfx_rect(int x, int y, int w, int h, uint8_t color);       /* 1px border */
void gfx_hline(int x, int y, int w, uint8_t color);
void gfx_vline(int x, int y, int h, uint8_t color);

/* Draw one glyph / a NUL-terminated string; 'bg' < 0 means transparent. */
void gfx_char(int x, int y, char c, uint8_t fg, int bg);
void gfx_text(int x, int y, const char *s, uint8_t fg, int bg);
int  gfx_text_width(const char *s);

/* Push the back buffer to the screen. */
void gfx_present(void);

#endif /* PUMPKIN_GFX_H */
