/* ===========================================================================
 * PumpkinOS - text console (VGA text mode + COM1 serial mirror)
 * ========================================================================= */
#ifndef PUMPKIN_CONSOLE_H
#define PUMPKIN_CONSOLE_H

#include <stdint.h>

/* Standard VGA text-mode 16-colour palette. */
enum vga_color {
    VGA_BLACK = 0,  VGA_BLUE,          VGA_GREEN,       VGA_CYAN,
    VGA_RED,        VGA_MAGENTA,       VGA_BROWN,       VGA_LIGHT_GREY,
    VGA_DARK_GREY,  VGA_LIGHT_BLUE,    VGA_LIGHT_GREEN, VGA_LIGHT_CYAN,
    VGA_LIGHT_RED,  VGA_LIGHT_MAGENTA, VGA_YELLOW,      VGA_WHITE,
};

void console_init(void);                                  /* serial + clear   */
void console_clear(void);
void console_set_color(enum vga_color fg, enum vga_color bg);
void console_putc(char c);                                /* \n \r \t \b aware */
void console_write(const char *s);
void console_write_dec(uint32_t value);
void console_write_hex(uint32_t value);

#endif /* PUMPKIN_CONSOLE_H */
