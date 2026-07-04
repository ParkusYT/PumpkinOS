/* ===========================================================================
 * PumpkinOS - VGA mode 13h (320x200x256) and text-mode restore
 * ---------------------------------------------------------------------------
 * Register-level mode setting after the fashion of the classic FreeVGA / Chris
 * Giese reference: write the Misc Output, Sequencer, CRTC, Graphics Controller
 * and Attribute Controller register banks from a canned table. mode 13h gives a
 * flat 8-bpp framebuffer at 0xA0000; returning to mode 03h needs the text
 * register table plus the font reloaded into plane 2 (which mode-13h drawing
 * corrupts), so we save the font before switching and restore it after.
 * ========================================================================= */
#include "vga.h"
#include "io.h"
#include <stdint.h>

/* ---- register bank offsets ------------------------------------------------ */
#define MISC_WRITE   0x3C2
#define SEQ_INDEX    0x3C4
#define SEQ_DATA     0x3C5
#define GC_INDEX     0x3CE
#define GC_DATA      0x3CF
#define CRTC_INDEX   0x3D4
#define CRTC_DATA    0x3D5
#define AC_INDEX     0x3C0
#define AC_WRITE     0x3C0
#define INPUT_STATUS 0x3DA
#define DAC_WRITE    0x3C8
#define DAC_DATA     0x3C9

/* Canned register dumps (misc, 5 seq, 25 crtc, 9 gc, 21 attribute). */
static const uint8_t mode_13h[] = {
    /* MISC */ 0x63,
    /* SEQ  */ 0x03, 0x01, 0x0F, 0x00, 0x0E,
    /* CRTC */ 0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
               0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
               0xFF,
    /* GC   */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF,
    /* AC   */ 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
               0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
               0x41, 0x00, 0x0F, 0x00, 0x00,
};

static const uint8_t mode_03h[] = {
    /* MISC */ 0x67,
    /* SEQ  */ 0x03, 0x00, 0x03, 0x00, 0x02,
    /* CRTC */ 0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
               0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50,
               0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
               0xFF,
    /* GC   */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF,
    /* AC   */ 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x14, 0x07,
               0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
               0x0C, 0x00, 0x0F, 0x08, 0x00,
};

/* 8 KiB is enough for 256 glyphs at the 32-byte VGA font stride. */
static uint8_t font_backup[256 * 32];

/* Launder a fixed physical address into a pointer so -O2 doesn't decide the
 * dereference is out of bounds (the framebuffer/font live at 0xA0000). */
static volatile uint8_t *phys_ptr(uint32_t addr) {
    volatile uint8_t *p = (volatile uint8_t *)addr;
    __asm__("" : "+r"(p));
    return p;
}

static void write_regs(const uint8_t *regs) {
    outb(MISC_WRITE, *regs++);

    for (uint8_t i = 0; i < 5; i++) {
        outb(SEQ_INDEX, i);
        outb(SEQ_DATA, *regs++);
    }

    /* Unlock the CRTC registers (clear the protect bit at index 0x11). */
    outb(CRTC_INDEX, 0x03);
    outb(CRTC_DATA, (uint8_t)(inb(CRTC_DATA) | 0x80));
    outb(CRTC_INDEX, 0x11);
    outb(CRTC_DATA, (uint8_t)(inb(CRTC_DATA) & ~0x80));

    uint8_t crtc[25];
    for (int i = 0; i < 25; i++)
        crtc[i] = regs[i];
    crtc[0x03] |= 0x80;          /* keep them unlocked while we write */
    crtc[0x11] &= ~0x80;
    for (uint8_t i = 0; i < 25; i++) {
        outb(CRTC_INDEX, i);
        outb(CRTC_DATA, crtc[i]);
    }
    regs += 25;

    for (uint8_t i = 0; i < 9; i++) {
        outb(GC_INDEX, i);
        outb(GC_DATA, *regs++);
    }

    for (uint8_t i = 0; i < 21; i++) {
        (void)inb(INPUT_STATUS);         /* reset the AC index/data flip-flop */
        outb(AC_INDEX, i);
        outb(AC_WRITE, *regs++);
    }

    /* Lock the palette and re-enable video output. */
    (void)inb(INPUT_STATUS);
    outb(AC_INDEX, 0x20);
}

/* Point the sequencer/graphics controller at plane 2 as a flat 64 KiB window at
 * 0xA0000, which is where the character-generator font lives. */
static void font_access_begin(void) {
    outb(SEQ_INDEX, 0x00); outb(SEQ_DATA, 0x01);   /* sync reset      */
    outb(SEQ_INDEX, 0x02); outb(SEQ_DATA, 0x04);   /* write plane 2   */
    outb(SEQ_INDEX, 0x04); outb(SEQ_DATA, 0x07);   /* flat addressing */
    outb(SEQ_INDEX, 0x00); outb(SEQ_DATA, 0x03);   /* end reset       */
    outb(GC_INDEX, 0x04);  outb(GC_DATA, 0x02);    /* read plane 2    */
    outb(GC_INDEX, 0x05);  outb(GC_DATA, 0x00);    /* no odd/even     */
    outb(GC_INDEX, 0x06);  outb(GC_DATA, 0x00);    /* map at 0xA0000  */
}

/* Put the sequencer/graphics controller back to the text-mode defaults. */
static void font_access_end(void) {
    outb(SEQ_INDEX, 0x00); outb(SEQ_DATA, 0x01);
    outb(SEQ_INDEX, 0x02); outb(SEQ_DATA, 0x03);   /* planes 0 and 1  */
    outb(SEQ_INDEX, 0x04); outb(SEQ_DATA, 0x03);   /* odd/even        */
    outb(SEQ_INDEX, 0x00); outb(SEQ_DATA, 0x03);
    outb(GC_INDEX, 0x04);  outb(GC_DATA, 0x00);
    outb(GC_INDEX, 0x05);  outb(GC_DATA, 0x10);    /* odd/even        */
    outb(GC_INDEX, 0x06);  outb(GC_DATA, 0x0E);    /* text at 0xB8000 */
}

static void save_font(void) {
    volatile uint8_t *vram = phys_ptr(0xA0000);
    font_access_begin();
    for (int i = 0; i < 256 * 32; i++)
        font_backup[i] = vram[i];
    font_access_end();
}

static void restore_font(void) {
    volatile uint8_t *vram = phys_ptr(0xA0000);
    font_access_begin();
    for (int i = 0; i < 256 * 32; i++)
        vram[i] = font_backup[i];
    font_access_end();
}

void vga_enter_graphics(void) {
    save_font();                 /* must happen while still in text mode */
    write_regs(mode_13h);
}

void vga_leave_graphics(void) {
    write_regs(mode_03h);
    restore_font();

    /* Blank the text framebuffer so the shell starts from a clean screen. */
    volatile uint16_t *text = (volatile uint16_t *)phys_ptr(0xB8000);
    for (int i = 0; i < 80 * 25; i++)
        text[i] = 0x0700;        /* light-grey on black space */
}

uint8_t *vga_framebuffer(void) {
    return (uint8_t *)phys_ptr(0xA0000);
}

void vga_set_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    outb(DAC_WRITE, index);
    outb(DAC_DATA, r);
    outb(DAC_DATA, g);
    outb(DAC_DATA, b);
}

const uint8_t *vga_font(void) {
    return font_backup;
}
