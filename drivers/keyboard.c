/* ===========================================================================
 * PumpkinOS - PS/2 keyboard driver
 * ---------------------------------------------------------------------------
 * The 8042 PS/2 controller raises IRQ1 whenever a key changes state. We read
 * the raw scancode from the data port (0x60), translate it (scancode set 1,
 * US layout), and push the resulting ASCII byte into a small ring buffer.
 * The shell drains that buffer with keyboard_getchar().
 * ========================================================================= */
#include "keyboard.h"
#include "pic.h"
#include "io.h"
#include <stdint.h>

#define PS2_DATA   0x60
#define PS2_STATUS 0x64

/* --- scancode set 1 -> ASCII, US layout ------------------------------------ */
/* Index by make code (0x00..0x7F). Zero means "no printable character". */
static const char keymap[128] = {
    [0x01] = 27,
    [0x02] = '1',  [0x03] = '2',  [0x04] = '3',  [0x05] = '4',  [0x06] = '5',
    [0x07] = '6',  [0x08] = '7',  [0x09] = '8',  [0x0A] = '9',  [0x0B] = '0',
    [0x0C] = '-',  [0x0D] = '=',  [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q',  [0x11] = 'w',  [0x12] = 'e',  [0x13] = 'r',  [0x14] = 't',
    [0x15] = 'y',  [0x16] = 'u',  [0x17] = 'i',  [0x18] = 'o',  [0x19] = 'p',
    [0x1A] = '[',  [0x1B] = ']',  [0x1C] = '\n',
    [0x1E] = 'a',  [0x1F] = 's',  [0x20] = 'd',  [0x21] = 'f',  [0x22] = 'g',
    [0x23] = 'h',  [0x24] = 'j',  [0x25] = 'k',  [0x26] = 'l',  [0x27] = ';',
    [0x28] = '\'', [0x29] = '`',  [0x2B] = '\\',
    [0x2C] = 'z',  [0x2D] = 'x',  [0x2E] = 'c',  [0x2F] = 'v',  [0x30] = 'b',
    [0x31] = 'n',  [0x32] = 'm',  [0x33] = ',',  [0x34] = '.',  [0x35] = '/',
    [0x37] = '*',  [0x39] = ' ',
    /* numeric keypad */
    [0x47] = '7',  [0x48] = '8',  [0x49] = '9',  [0x4A] = '-',
    [0x4B] = '4',  [0x4C] = '5',  [0x4D] = '6',  [0x4E] = '+',
    [0x4F] = '1',  [0x50] = '2',  [0x51] = '3',  [0x52] = '0',  [0x53] = '.',
};

static const char keymap_shift[128] = {
    [0x01] = 27,
    [0x02] = '!',  [0x03] = '@',  [0x04] = '#',  [0x05] = '$',  [0x06] = '%',
    [0x07] = '^',  [0x08] = '&',  [0x09] = '*',  [0x0A] = '(',  [0x0B] = ')',
    [0x0C] = '_',  [0x0D] = '+',  [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'Q',  [0x11] = 'W',  [0x12] = 'E',  [0x13] = 'R',  [0x14] = 'T',
    [0x15] = 'Y',  [0x16] = 'U',  [0x17] = 'I',  [0x18] = 'O',  [0x19] = 'P',
    [0x1A] = '{',  [0x1B] = '}',  [0x1C] = '\n',
    [0x1E] = 'A',  [0x1F] = 'S',  [0x20] = 'D',  [0x21] = 'F',  [0x22] = 'G',
    [0x23] = 'H',  [0x24] = 'J',  [0x25] = 'K',  [0x26] = 'L',  [0x27] = ':',
    [0x28] = '"',  [0x29] = '~',  [0x2B] = '|',
    [0x2C] = 'Z',  [0x2D] = 'X',  [0x2E] = 'C',  [0x2F] = 'V',  [0x30] = 'B',
    [0x31] = 'N',  [0x32] = 'M',  [0x33] = '<',  [0x34] = '>',  [0x35] = '?',
    [0x37] = '*',  [0x39] = ' ',
};

/* --- modifier state (only touched from the IRQ handler) -------------------- */
static int shift_down = 0;
static int caps_lock  = 0;
static int extended   = 0;   /* set after an 0xE0 prefix byte */

/* --- ring buffer ----------------------------------------------------------- */
#define KBD_BUF_SIZE 128
static volatile char     kbd_buf[KBD_BUF_SIZE];
static volatile uint32_t kbd_head = 0;   /* written by the IRQ handler */
static volatile uint32_t kbd_tail = 0;   /* read by the shell          */

static void kbd_enqueue(char c) {
    uint32_t next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next != kbd_tail) {          /* drop the key if the buffer is full */
        kbd_buf[kbd_head] = c;
        kbd_head = next;
    }
}

void keyboard_init(void) {
    /* Drain anything the BIOS or the user left in the output buffer, so the
     * controller is free to raise fresh IRQs. */
    while (inb(PS2_STATUS) & 0x01)
        (void)inb(PS2_DATA);

    pic_unmask(1);   /* let IRQ1 through */
}

void keyboard_irq(void) {
    uint8_t sc = inb(PS2_DATA);

    /* 0xE0 introduces an "extended" key (arrows, right ctrl, ...). We don't
     * map those yet, so remember the prefix and skip the following byte. */
    if (sc == 0xE0) {
        extended = 1;
        return;
    }
    if (extended) {
        extended = 0;
        return;
    }

    if (sc & 0x80) {                 /* key release */
        uint8_t make = sc & 0x7F;
        if (make == 0x2A || make == 0x36)
            shift_down = 0;
        return;
    }

    /* key press */
    if (sc == 0x2A || sc == 0x36) {  /* left / right shift */
        shift_down = 1;
        return;
    }
    if (sc == 0x3A) {                /* caps lock toggles on each press */
        caps_lock = !caps_lock;
        return;
    }

    char c = shift_down ? keymap_shift[sc] : keymap[sc];
    if (c == 0)
        return;

    /* Caps lock flips the case of letters only. */
    if (caps_lock) {
        if (c >= 'a' && c <= 'z')
            c -= 32;
        else if (c >= 'A' && c <= 'Z')
            c += 32;
    }

    kbd_enqueue(c);
}

int keyboard_haschar(void) {
    return kbd_head != kbd_tail;
}

char keyboard_getchar(void) {
    char c;

    /* Wait with interrupts enabled, sleeping until IRQ1 delivers a key. The
     * cli/(sti;hlt)/cli dance closes the race where a key could arrive between
     * the emptiness check and the hlt. */
    __asm__ volatile("cli");
    while (kbd_head == kbd_tail)
        __asm__ volatile("sti; hlt; cli");

    c = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    __asm__ volatile("sti");
    return c;
}
