/* ===========================================================================
 * PumpkinOS - 2D graphics on top of the framebuffer
 * ---------------------------------------------------------------------------
 * Drawing goes to an in-RAM 8-bpp (palette-indexed) back buffer sized to the
 * current display; gfx_present() blits it to the real framebuffer in one go,
 * converting to the framebuffer's format (direct copy at 8 bpp, palette lookup
 * to 32-bpp for the VBE modes). Text uses the VGA driver's snapshotted font.
 * ========================================================================= */
#ifndef PUMPKIN_GFX_H
#define PUMPKIN_GFX_H

#include <stdint.h>

#define FONT_W 8
#define FONT_H 16

/* Allocate the back buffer for the current display size. 0 on success. */
int  gfx_init(void);
void gfx_shutdown(void);

int  gfx_width(void);
int  gfx_height(void);

/* Define palette entry 'index' (r/g/b are 6-bit, 0..63). */
void gfx_set_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

void gfx_clear(uint8_t color);
void gfx_pixel(int x, int y, uint8_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint8_t color);
void gfx_rect(int x, int y, int w, int h, uint8_t color);
void gfx_hline(int x, int y, int w, uint8_t color);
void gfx_vline(int x, int y, int h, uint8_t color);

void gfx_char(int x, int y, char c, uint8_t fg, int bg);   /* bg < 0 = transparent */
void gfx_text(int x, int y, const char *s, uint8_t fg, int bg);
int  gfx_text_width(const char *s);

/* Blit the whole back buffer, or just a rectangle of it, to the framebuffer. */
void gfx_present(void);
void gfx_present_rect(int x, int y, int w, int h);

/* Write one pixel straight to the framebuffer (not the back buffer) - used to
 * paint the mouse cursor on top without disturbing the composed scene. */
void gfx_fb_pixel(int x, int y, uint8_t color);

#endif /* PUMPKIN_GFX_H */
