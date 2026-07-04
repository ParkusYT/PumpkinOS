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
#include "floppy.h"
#include "ata.h"
#include "acpi.h"
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
    console_write("  Starting scheduler + demo tasks .... ... ");
    sched_init();                 /* this boot context becomes task 0 */
    shell_spawn_demo_tasks();     /* two background worker threads */
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

    console_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    console_write("\n  Type 'help' to get started.\n\n");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    shell_run();   /* never returns */

    /* Just in case the shell ever does return, halt cleanly. */
    for (;;)
        __asm__ volatile("cli; hlt");
}
