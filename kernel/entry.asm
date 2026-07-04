; =============================================================================
; PumpkinOS - 32-bit kernel entry stub
; -----------------------------------------------------------------------------
; The boot sector loads KERNEL.BIN at 0x10000, gathers the E820 map, enables
; A20, and switches to 32-bit protected mode before jumping here. This stub is
; linked first (so _start sits at the start of the image, i.e. 0x10000). It
; zeroes .bss, sets up a clean stack, and calls the C kernel_main().
; =============================================================================

[bits 32]

global _start
extern kernel_main
extern __bss_start
extern __bss_end

section .text
_start:
    ; segments are already the flat data selector from the boot sector, but set
    ; them again to be safe.
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; zero .bss (uninitialised globals must start as zero on real hardware)
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    cld
    rep stosb

    mov esp, 0x90000           ; kernel stack (grows down, clear of everything)
    xor ebp, ebp               ; terminate the stack-frame chain for debuggers

    call kernel_main

.hang:
    cli
    hlt
    jmp .hang
