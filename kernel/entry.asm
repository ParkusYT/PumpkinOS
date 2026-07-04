; =============================================================================
; PumpkinOS - 32-bit kernel entry stub
; -----------------------------------------------------------------------------
; The bootloader jumps here (physical address 0x1000) already in 32-bit
; protected mode with a flat memory model. This stub is linked first so that
; _start sits at the very start of the kernel image. It:
;   1. zeroes the .bss section (uninitialised globals -- the IDT, keyboard
;      ring buffer, etc. must start as zero, and RAM is not zero on real HW),
;   2. sets up a clean stack, and
;   3. calls the C kernel_main().
; If kernel_main ever returns, we halt the CPU forever.
; =============================================================================

[bits 32]

global _start
extern kernel_main
extern __bss_start
extern __bss_end

section .text
_start:
    ; --- zero the .bss section ---
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi               ; ecx = number of bytes in .bss
    xor eax, eax
    cld
    rep stosb                  ; fill [__bss_start, __bss_end) with zero

    ; --- set up the stack and enter C ---
    mov esp, 0x90000           ; kernel stack (grows down, well clear of us)
    xor ebp, ebp               ; terminate the stack-frame chain for debuggers

    call kernel_main

.hang:
    cli
    hlt                        ; sleep; if an interrupt ever fires, halt again
    jmp .hang
