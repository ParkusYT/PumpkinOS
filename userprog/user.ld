/* Link a PumpkinOS ring-3 program at 0x40000000 - the user region, which sits
 * above the kernel's identity map so mapping user pages there can't shadow
 * kernel RAM. One LOAD segment holds code, read-only data, data and bss. */
ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
