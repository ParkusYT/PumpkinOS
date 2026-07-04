/* ===========================================================================
 * PumpkinOS - kernel linker script
 * ---------------------------------------------------------------------------
 * The bootloader loads the kernel image as a flat binary at physical address
 * 0x1000 and jumps to its first byte, so we must:
 *   - place the code at 0x1000, and
 *   - make sure entry.o (which defines _start) comes first in .text.
 * The build passes entry.o before kernel.o on the link line, so *(.text)
 * naturally puts _start at the very beginning.
 * ========================================================================= */

ENTRY(_start)

SECTIONS
{
    . = 0x1000;

    .text ALIGN(4) :
    {
        *(.text*)
    }

    .rodata ALIGN(4) :
    {
        *(.rodata*)
    }

    .data ALIGN(4) :
    {
        *(.data*)
    }

    .bss ALIGN(4) :
    {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        . = ALIGN(4);
        __bss_end = .;
    }

    /* Everything the kernel actually needs is in the sections above.
     * Drop notes / comments / unwind tables so the flat binary stays tight. */
    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
