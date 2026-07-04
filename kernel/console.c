/* ===========================================================================
 * PumpkinOS - text console
 * ---------------------------------------------------------------------------
 * Drives two devices at once:
 *   - the VGA text framebuffer at 0xB8000 (80x25, colour, hardware cursor)
 *   - the first serial port, COM1, which mirrors everything (handy for
 *     debugging on headless QEMU or a real machine's serial cable).
 * ========================================================================= */
#include "console.h"
#include "io.h"
#include <stddef.h>

/* ---- VGA text mode -------------------------------------------------------- */
#define VGA_MEMORY ((volatile uint16_t *)0xB8000)
#define VGA_COLS   80
#define VGA_ROWS   25

static size_t  cur_row;
static size_t  cur_col;
static uint8_t cur_attr = 0x07;   /* light grey on black */

static inline uint16_t vga_cell(char c, uint8_t attr) {
    return (uint16_t)(unsigned char)c | (uint16_t)attr << 8;
}

void console_set_color(enum vga_color fg, enum vga_color bg) {
    cur_attr = (uint8_t)fg | (uint8_t)(bg << 4);
}

/* Move the blinking hardware cursor to the current (row, col). */
static void vga_move_cursor(void) {
    uint16_t pos = (uint16_t)(cur_row * VGA_COLS + cur_col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void console_clear(void) {
    uint16_t blank = vga_cell(' ', cur_attr);
    for (size_t i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA_MEMORY[i] = blank;
    cur_row = 0;
    cur_col = 0;
    vga_move_cursor();
}

/* Scroll the whole screen up by one line. */
static void vga_scroll(void) {
    for (size_t r = 1; r < VGA_ROWS; r++)
        for (size_t c = 0; c < VGA_COLS; c++)
            VGA_MEMORY[(r - 1) * VGA_COLS + c] = VGA_MEMORY[r * VGA_COLS + c];

    uint16_t blank = vga_cell(' ', cur_attr);
    for (size_t c = 0; c < VGA_COLS; c++)
        VGA_MEMORY[(VGA_ROWS - 1) * VGA_COLS + c] = blank;

    cur_row = VGA_ROWS - 1;
}

static void vga_putc(char c) {
    switch (c) {
    case '\n':
        cur_col = 0;
        cur_row++;
        break;
    case '\r':
        cur_col = 0;
        break;
    case '\t':
        cur_col = (cur_col + 4) & ~(size_t)3;
        break;
    case '\b':
        if (cur_col > 0) {
            cur_col--;
            VGA_MEMORY[cur_row * VGA_COLS + cur_col] = vga_cell(' ', cur_attr);
        }
        break;
    default:
        VGA_MEMORY[cur_row * VGA_COLS + cur_col] = vga_cell(c, cur_attr);
        cur_col++;
        break;
    }

    if (cur_col >= VGA_COLS) {
        cur_col = 0;
        cur_row++;
    }
    if (cur_row >= VGA_ROWS)
        vga_scroll();
}

/* ---- COM1 serial ---------------------------------------------------------- */
#define COM1 0x3F8

static void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* disable interrupts                     */
    outb(COM1 + 3, 0x80);   /* enable DLAB (set baud divisor)         */
    outb(COM1 + 0, 0x03);   /* divisor low  = 3 -> 38400 baud         */
    outb(COM1 + 1, 0x00);   /* divisor high = 0                       */
    outb(COM1 + 3, 0x03);   /* 8 bits, no parity, one stop bit        */
    outb(COM1 + 2, 0xC7);   /* enable + clear FIFO, 14-byte threshold */
    outb(COM1 + 4, 0x0B);   /* IRQs enabled, RTS/DSR set              */
}

static void serial_putc(char c) {
    while ((inb(COM1 + 5) & 0x20) == 0)   /* wait for transmit buffer empty */
        ;
    outb(COM1, (uint8_t)c);
}

/* ---- unified console ------------------------------------------------------ */
void console_init(void) {
    serial_init();
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_clear();
}

void console_putc(char c) {
    if (c == '\n')
        serial_putc('\r');             /* terminals want CR before LF */
    if (c == '\b') {                    /* erase-on-backspace over serial */
        serial_putc('\b');
        serial_putc(' ');
        serial_putc('\b');
    } else {
        serial_putc(c);
    }
    vga_putc(c);
    vga_move_cursor();
}

void console_write(const char *s) {
    for (; *s; s++)
        console_putc(*s);
}

void console_write_dec(uint32_t value) {
    char buf[11];
    int i = 0;
    if (value == 0) {
        console_putc('0');
        return;
    }
    while (value > 0) {
        buf[i++] = (char)('0' + value % 10);
        value /= 10;
    }
    while (i-- > 0)
        console_putc(buf[i]);
}

void console_write_hex(uint32_t value) {
    static const char digits[] = "0123456789ABCDEF";
    console_write("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        console_putc(digits[(value >> shift) & 0xF]);
}
