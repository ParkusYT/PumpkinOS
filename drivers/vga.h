/* ===========================================================================
 * PumpkinOS - VGA / VBE display driver
 * ---------------------------------------------------------------------------
 * The kernel normally runs in 80x25 text mode. For the desktop we switch to a
 * graphics framebuffer. Two paths:
 *
 *   - Bochs/QEMU VBE ("dispi") over I/O ports 0x1CE/0x1CF: a high-resolution
 *     32-bpp linear framebuffer, set entirely from protected mode. We query the
 *     maximum supported size and pick a sensible desktop resolution.
 *   - Fallback: plain VGA mode 13h (320x200x256) programmed register by
 *     register, for hardware without the dispi interface.
 *
 * Either way we snapshot the text font (which mode-13h drawing corrupts) and
 * restore it - plus the standard 16-colour palette - so the text console comes
 * back cleanly on exit.
 * ========================================================================= */
#ifndef PUMPKIN_VGA_H
#define PUMPKIN_VGA_H

#include <stdint.h>

/* Enter graphics mode (VBE high-res if available, else mode 13h). */
void vga_enter_graphics(void);

/* Restore 80x25 text mode, the font and palette, and clear the screen. */
void vga_leave_graphics(void);

/* Framebuffer geometry chosen by vga_enter_graphics(). */
int      vga_width(void);
int      vga_height(void);
int      vga_bpp(void);          /* 8 (mode 13h) or 32 (VBE) */
uint32_t vga_pitch(void);        /* bytes per scanline       */
uint8_t *vga_framebuffer(void);  /* the mapped framebuffer    */

/* Set a DAC palette entry (r/g/b are 6-bit). Only meaningful at 8 bpp. */
void vga_set_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b);

/* The snapshotted text font: 256 glyphs, 32-byte stride, 16 rows used, bit 7 =
 * leftmost pixel. Valid after vga_enter_graphics(). */
const uint8_t *vga_font(void);

#endif /* PUMPKIN_VGA_H */
