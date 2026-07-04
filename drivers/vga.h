/* ===========================================================================
 * PumpkinOS - VGA driver (320x200x256 graphics, mode 13h)
 * ---------------------------------------------------------------------------
 * The kernel normally runs in 80x25 text mode. For the desktop we switch the
 * VGA hardware into mode 13h (a flat 320x200 8-bpp framebuffer at 0xA0000) by
 * programming the registers directly - no BIOS call, since we are in protected
 * mode. Drawing in mode 13h scribbles over the text-mode font stored in VGA
 * plane 2, so we snapshot the font on the way in and write it back on the way
 * out, which lets the text console come back to life afterwards.
 * ========================================================================= */
#ifndef PUMPKIN_VGA_H
#define PUMPKIN_VGA_H

#include <stdint.h>

#define VGA_WIDTH  320
#define VGA_HEIGHT 200

/* Snapshot the text font, then switch to the 320x200x256 framebuffer. */
void vga_enter_graphics(void);

/* Restore 80x25 text mode, put the font back, and clear the screen. */
void vga_leave_graphics(void);

/* The linear 8-bpp framebuffer (0xA0000), one byte (colour index) per pixel. */
uint8_t *vga_framebuffer(void);

/* Set one of the 256 palette entries (r/g/b are 6-bit, 0..63). */
void vga_set_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

/* The snapshotted text font: 256 glyphs, 32-byte stride, 16 rows used, with
 * bit 7 of each row byte being the leftmost pixel. Valid after
 * vga_enter_graphics(). */
const uint8_t *vga_font(void);

#endif /* PUMPKIN_VGA_H */
