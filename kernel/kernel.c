/* ===========================================================================
 * PumpkinOS - kernel entry point
 * ---------------------------------------------------------------------------
 * By the time we get here the bootloader has put us in 32-bit protected mode
 * with a flat memory model, a stack, and a zeroed .bss (see entry.asm). We
 * bring up the console, install interrupt handling, start the keyboard, and
 * hand control to PumpkinShell.
 * ========================================================================= */
#include "console.h"
#include "idt.h"
#include "pic.h"
#include "keyboard.h"
#include "shell.h"

void kernel_main(void) {
    console_init();

    shell_banner();
    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    console_write("\n  PumpkinOS kernel is alive!\n\n");

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
    console_write("  Starting PS/2 keyboard driver ...... ... ");
    keyboard_init();
    console_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    console_write("ok\n");

    /* Everything is in place - turn on interrupts. */
    __asm__ volatile("sti");

    console_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    console_write("\n  Type 'help' to get started.\n\n");
    console_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    shell_run();   /* never returns */

    /* Just in case the shell ever does return, halt cleanly. */
    for (;;)
        __asm__ volatile("cli; hlt");
}
