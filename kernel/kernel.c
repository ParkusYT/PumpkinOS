/* ===========================================================================
 * PumpkinOS - kernel entry point
 * ---------------------------------------------------------------------------
 * By the time we get here the bootloader has put us in 32-bit protected mode
 * with a flat memory model, a stack, and a zeroed .bss (see entry.asm). We
 * bring up the console, install interrupt handling, start the keyboard, and
 * hand control to PumpkinShell.
 * ========================================================================= */
#include "console.h"
#include "gdt.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "idt.h"
#include "pic.h"
#include "timer.h"
#include "keyboard.h"
#include "mouse.h"
#include "floppy.h"
#include "ata.h"
#include "acpi.h"
#include "ac97.h"
#include "net.h"
#include "sched.h"
#include "fat12.h"
#include "shell.h"

void kernel_main(void) {
    console_init();

    shell_banner();
    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    console_write("\n  PumpkinOS kernel is alive!\n\n");

    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  Loading GDT + TSS (ring 0/3) ....... ... ");
    gdt_init();
    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    console_write("ok\n");

    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  Scanning physical memory (E820) .... ... ");
    pmm_init();
    pmm_reserve(AC97_DMA_PHYS, AC97_DMA_BYTES);   /* fixed audio DMA buffer */
    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    console_write("ok");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  (");
    console_write_dec(pmm_total_frames() * 4 / 1024);
    console_write(" MB usable)\n");

    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  Installing interrupt descriptor table ... ");
    idt_init();
    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    console_write("ok\n");

    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  Remapping the 8259 PIC .............. ... ");
    pic_remap();
    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    console_write("ok\n");

    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  Starting PIT timer @ 100 Hz ........ ... ");
    timer_init(100);
    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    console_write("ok\n");

    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  Starting PS/2 keyboard driver ...... ... ");
    keyboard_init();
    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    console_write("ok\n");

    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  Starting PS/2 mouse driver ......... ... ");
    mouse_init();
    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    console_write("ok\n");

    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  Enabling paging (identity map) ..... ... ");
    paging_init();
    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    console_write("ok\n");

    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  Initialising kernel heap ........... ... ");
    kheap_init();
    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    console_write("ok\n");

    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  Reading ACPI tables (power-off) .... ... ");
    acpi_init();
    if (acpi_available()) {
        console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        console_write("ok\n");
    } else {
        console_set_color(VGA_YELLOW, VGA_BLACK);
        console_write("none\n");
    }

    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  Starting the scheduler ............. ... ");
    sched_init();                 /* this boot context becomes task 0 */
    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    console_write("ok\n");

    /* Everything is in place - turn on interrupts (preemption starts now). */
    __asm__ volatile("sti");

    /* The floppy driver needs interrupts (it waits on IRQ6 and uses the timer
     * for motor spin-up), so bring it and the filesystem up after 'sti'. */
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  Resetting floppy controller ........ ... ");
    floppy_init();
    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    console_write("ok\n");

    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  Detecting RTL8139 network card ..... ... ");
    if (net_init()) {
        const uint8_t *m = net_mac;
        console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        console_write("ok");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        console_write("  (");
        for (int i = 0; i < 6; i++) {
            const char *hex = "0123456789abcdef";
            console_putc(hex[m[i] >> 4]);
            console_putc(hex[m[i] & 0xF]);
            if (i < 5) console_putc(':');
        }
        console_write(")\n");
    } else {
        console_set_color(VGA_YELLOW, VGA_BLACK);
        console_write("none\n");
    }

    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  Detecting IDE/ATA hard disks ....... ... ");
    ata_init();
    if (ata_drive_count() > 0) {
        console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        console_write("ok");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        console_write("  (");
        console_write_dec((uint32_t)ata_drive_count());
        console_write(ata_drive_count() == 1 ? " disk)\n" : " disks)\n");
    } else {
        console_set_color(VGA_YELLOW, VGA_BLACK);
        console_write("none\n");
    }

    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  Mounting FAT12 filesystem .......... ... ");
    int files = fs_init();
    if (files >= 0) {
        console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        console_write("ok");
        console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        console_write("  (");
        console_write_dec((uint32_t)files);
        console_write(" files)\n");
    } else {
        console_set_color(VGA_YELLOW, VGA_BLACK);
        console_write("none\n");
    }

    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    console_write("  Detecting AC'97 audio codec ........ ... ");
    ac97_init();
    if (ac97_present()) {
        console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        console_write("ok\n");
        /* Decode both jingles into their DMA slots now (the slow floppy
         * reads), so the desktop can start them instantly later. */
        ac97_preload(AC97_STARTUP,  "/system/STARTUP.PCM");
        ac97_preload(AC97_SHUTDOWN, "/system/SHUTDOWN.PCM");
    } else {
        console_set_color(VGA_YELLOW, VGA_BLACK);
        console_write("none\n");
    }
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    console_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    console_write("\n  Type 'help' to get started.\n\n");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    shell_run();   /* never returns */

    /* Just in case the shell ever does return, halt cleanly. */
    for (;;)
        __asm__ volatile("cli; hlt");
}
