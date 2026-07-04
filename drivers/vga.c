/* ===========================================================================
 * PumpkinOS - VGA mode 13h + Bochs/QEMU VBE high-resolution framebuffer
 * ========================================================================= */
#include "vga.h"
#include "io.h"
#include "paging.h"
#include <stdint.h>

/* ---- register bank offsets (mode 13h / text) ------------------------------ */
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

/* ---- Bochs VBE ("dispi") interface ---------------------------------------- */
#define VBE_INDEX    0x01CE
#define VBE_DATA     0x01CF
#define VBE_I_ID       0
#define VBE_I_XRES     1
#define VBE_I_YRES     2
#define VBE_I_BPP      3
#define VBE_I_ENABLE   4
#define VBE_I_VWIDTH   6
#define VBE_ENABLED    0x01
#define VBE_GETCAPS    0x02
#define VBE_LFB        0x40

/* ---- state ---------------------------------------------------------------- */
static int      g_mode;          /* 0 = mode 13h, 1 = VBE */
static int      g_width, g_height, g_bpp;
static uint32_t g_pitch;
static uint8_t *g_fb;

/* Canned register dumps (misc, 5 seq, 25 crtc, 9 gc, 21 attribute). */
static const uint8_t mode_13h[] = {
    0x63,
    0x03, 0x01, 0x0F, 0x00, 0x0E,
    0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0xBF, 0x1F,
    0x00, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x9C, 0x0E, 0x8F, 0x28, 0x40, 0x96, 0xB9, 0xA3,
    0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x0F, 0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x41, 0x00, 0x0F, 0x00, 0x00,
};

/* Text mode 03h. The attribute palette is a plain 0..15 mapping so restoring
 * DAC entries 0..15 to the standard colours brings text back correctly. */
static const uint8_t mode_03h[] = {
    0x67,
    0x03, 0x00, 0x03, 0x00, 0x02,
    0x5F, 0x4F, 0x50, 0x82, 0x55, 0x81, 0xBF, 0x1F,
    0x00, 0x4F, 0x0D, 0x0E, 0x00, 0x00, 0x00, 0x50,
    0x9C, 0x0E, 0x8F, 0x28, 0x1F, 0x96, 0xB9, 0xA3,
    0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x0E, 0x00, 0xFF,
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
    0x0C, 0x00, 0x0F, 0x08, 0x00,
};

/* Standard VGA 16-colour DAC palette (6-bit r,g,b). */
static const uint8_t dac16[16][3] = {
    { 0,  0,  0}, { 0,  0, 42}, { 0, 42,  0}, { 0, 42, 42},
    {42,  0,  0}, {42,  0, 42}, {42, 21,  0}, {42, 42, 42},
    {21, 21, 21}, {21, 21, 63}, {21, 63, 21}, {21, 63, 63},
    {63, 21, 21}, {63, 21, 63}, {63, 63, 21}, {63, 63, 63},
};

static uint8_t font_backup[256 * 32];

static volatile uint8_t *phys_ptr(uint32_t addr) {
    volatile uint8_t *p = (volatile uint8_t *)addr;
    __asm__("" : "+r"(p));
    return p;
}

/* ---- classic register-level mode setting (mode 13h / text) ---------------- */
static void write_regs(const uint8_t *regs) {
    outb(MISC_WRITE, *regs++);
    for (uint8_t i = 0; i < 5; i++) { outb(SEQ_INDEX, i); outb(SEQ_DATA, *regs++); }

    outb(CRTC_INDEX, 0x03); outb(CRTC_DATA, (uint8_t)(inb(CRTC_DATA) | 0x80));
    outb(CRTC_INDEX, 0x11); outb(CRTC_DATA, (uint8_t)(inb(CRTC_DATA) & ~0x80));

    uint8_t crtc[25];
    for (int i = 0; i < 25; i++) crtc[i] = regs[i];
    crtc[0x03] |= 0x80;
    crtc[0x11] &= ~0x80;
    for (uint8_t i = 0; i < 25; i++) { outb(CRTC_INDEX, i); outb(CRTC_DATA, crtc[i]); }
    regs += 25;

    for (uint8_t i = 0; i < 9; i++) { outb(GC_INDEX, i); outb(GC_DATA, *regs++); }

    for (uint8_t i = 0; i < 21; i++) {
        (void)inb(INPUT_STATUS);
        outb(AC_INDEX, i);
        outb(AC_WRITE, *regs++);
    }
    (void)inb(INPUT_STATUS);
    outb(AC_INDEX, 0x20);
}

static void font_access_begin(void) {
    outb(SEQ_INDEX, 0x00); outb(SEQ_DATA, 0x01);
    outb(SEQ_INDEX, 0x02); outb(SEQ_DATA, 0x04);
    outb(SEQ_INDEX, 0x04); outb(SEQ_DATA, 0x07);
    outb(SEQ_INDEX, 0x00); outb(SEQ_DATA, 0x03);
    outb(GC_INDEX, 0x04);  outb(GC_DATA, 0x02);
    outb(GC_INDEX, 0x05);  outb(GC_DATA, 0x00);
    outb(GC_INDEX, 0x06);  outb(GC_DATA, 0x00);
}
static void font_access_end(void) {
    outb(SEQ_INDEX, 0x00); outb(SEQ_DATA, 0x01);
    outb(SEQ_INDEX, 0x02); outb(SEQ_DATA, 0x03);
    outb(SEQ_INDEX, 0x04); outb(SEQ_DATA, 0x03);
    outb(SEQ_INDEX, 0x00); outb(SEQ_DATA, 0x03);
    outb(GC_INDEX, 0x04);  outb(GC_DATA, 0x00);
    outb(GC_INDEX, 0x05);  outb(GC_DATA, 0x10);
    outb(GC_INDEX, 0x06);  outb(GC_DATA, 0x0E);
}
static void save_font(void) {
    volatile uint8_t *vram = phys_ptr(0xA0000);
    font_access_begin();
    for (int i = 0; i < 256 * 32; i++) font_backup[i] = vram[i];
    font_access_end();
}
static void restore_font(void) {
    volatile uint8_t *vram = phys_ptr(0xA0000);
    font_access_begin();
    for (int i = 0; i < 256 * 32; i++) vram[i] = font_backup[i];
    font_access_end();
}

/* ---- Bochs VBE ------------------------------------------------------------ */
static void vbe_write(uint16_t index, uint16_t value) {
    outw(VBE_INDEX, index);
    outw(VBE_DATA, value);
}
static uint16_t vbe_read(uint16_t index) {
    outw(VBE_INDEX, index);
    return inw(VBE_DATA);
}

/* Scan PCI bus 0 for a display-class device and return its BAR0 (the linear
 * framebuffer physical address), or 0 if none is found. */
static uint32_t pci_lfb(void) {
    for (uint32_t dev = 0; dev < 32; dev++) {
        uint32_t addr = 0x80000000u | (dev << 11);
        outl(0xCF8, addr | 0x00);
        if ((inl(0xCFC) & 0xFFFF) == 0xFFFF)
            continue;                          /* no device */
        outl(0xCF8, addr | 0x08);
        uint32_t class = inl(0xCFC) >> 24;
        if (class != 0x03)                     /* display controller */
            continue;
        outl(0xCF8, addr | 0x10);              /* BAR0 */
        uint32_t bar = inl(0xCFC);
        return bar & 0xFFFFFFF0u;
    }
    return 0;
}

/* Map a physical range into the address space (identity). */
static void map_range(uint32_t base, uint32_t size) {
    uint32_t start = base & ~0xFFFu;
    uint32_t stop  = (base + size + 0xFFFu) & ~0xFFFu;
    for (uint32_t a = start; a != stop; a += 0x1000)
        if (paging_get_phys(a) == 0xFFFFFFFFu)
            paging_map(a, a, PAGE_PRESENT | PAGE_WRITE);
}

/* Try to bring up a high-resolution 32-bpp VBE mode. Returns 0 on success. */
static int vbe_setup(void) {
    uint16_t id = vbe_read(VBE_I_ID);
    if (id < 0xB0C0 || id > 0xB0CF)
        return -1;                             /* no dispi interface */

    /* Ask the hardware for its maximum resolution, then pick a good desktop
     * size that fits within it (auto-detect). */
    vbe_write(VBE_I_ENABLE, VBE_GETCAPS);
    uint16_t maxx = vbe_read(VBE_I_XRES);
    uint16_t maxy = vbe_read(VBE_I_YRES);
    vbe_write(VBE_I_ENABLE, 0);

    static const int want[][2] = { {1280,1024}, {1024,768}, {800,600}, {640,480} };
    int w = 640, h = 480;
    for (unsigned i = 0; i < sizeof(want)/sizeof(want[0]); i++) {
        if (want[i][0] <= maxx && want[i][1] <= maxy) {
            w = want[i][0]; h = want[i][1];
            break;
        }
    }

    uint32_t lfb = pci_lfb();
    if (lfb == 0)
        return -1;

    vbe_write(VBE_I_ENABLE, 0);
    vbe_write(VBE_I_XRES, (uint16_t)w);
    vbe_write(VBE_I_YRES, (uint16_t)h);
    vbe_write(VBE_I_BPP, 32);
    vbe_write(VBE_I_ENABLE, VBE_ENABLED | VBE_LFB);

    g_mode   = 1;
    g_width  = w;
    g_height = h;
    g_bpp    = 32;
    g_pitch  = (uint32_t)vbe_read(VBE_I_VWIDTH) * 4;
    if (g_pitch < (uint32_t)w * 4)
        g_pitch = (uint32_t)w * 4;

    map_range(lfb, g_pitch * (uint32_t)h);
    g_fb = (uint8_t *)phys_ptr(lfb);
    return 0;
}

/* ---- public API ----------------------------------------------------------- */
void vga_enter_graphics(void) {
    save_font();                     /* must happen while still in text mode */

    if (vbe_setup() != 0) {
        write_regs(mode_13h);
        g_mode   = 0;
        g_width  = 320;
        g_height = 200;
        g_bpp    = 8;
        g_pitch  = 320;
        g_fb     = (uint8_t *)phys_ptr(0xA0000);
    }
}

void vga_leave_graphics(void) {
    if (g_mode == 1)
        vbe_write(VBE_I_ENABLE, 0);  /* disable VBE, back to VGA compatibility */

    write_regs(mode_03h);
    restore_font();

    /* Put the standard 16 colours back in the DAC (the desktop may have
     * reprogrammed them at 8 bpp). */
    for (int i = 0; i < 16; i++)
        vga_set_palette((uint8_t)i, dac16[i][0], dac16[i][1], dac16[i][2]);

    volatile uint16_t *text = (volatile uint16_t *)phys_ptr(0xB8000);
    for (int i = 0; i < 80 * 25; i++)
        text[i] = 0x0700;
}

int      vga_width(void)       { return g_width; }
int      vga_height(void)      { return g_height; }
int      vga_bpp(void)         { return g_bpp; }
uint32_t vga_pitch(void)       { return g_pitch; }
uint8_t *vga_framebuffer(void) { return g_fb; }

void vga_set_palette(uint8_t index, uint8_t r, uint8_t g, uint8_t b) {
    outb(DAC_WRITE, index);
    outb(DAC_DATA, r);
    outb(DAC_DATA, g);
    outb(DAC_DATA, b);
}

const uint8_t *vga_font(void) {
    return font_backup;
}
