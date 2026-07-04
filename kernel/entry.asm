; =============================================================================
; PumpkinOS - kernel entry (real-mode setup + jump to 32-bit)
; -----------------------------------------------------------------------------
; The FAT12 boot sector loads this file (KERNEL.BIN) at 0x1000 and jumps here
; in 16-bit real mode. Because the boot sector is busy parsing FAT12, the
; kernel does the low-level bring-up itself: gather the BIOS memory map (E820),
; enable A20, load a flat GDT, switch to 32-bit protected mode, zero .bss, set
; up the stack, and call kernel_main().
; =============================================================================

global _start
extern kernel_main
extern __bss_start
extern __bss_end

; Where the E820 memory map is left for the kernel (see mm/pmm.c).
MMAP_COUNT   equ 0x0500
MMAP_ENTRIES equ 0x0504

section .text
[bits 16]
_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00              ; a scratch real-mode stack
    sti

    call do_e820               ; gather the BIOS memory map (real mode only)
    call enable_a20

    cli
    lgdt [gdt_descriptor]       ; load our flat GDT
    mov eax, cr0
    or  eax, 0x1               ; set Protection Enable
    mov cr0, eax
    jmp CODE_SEG:pm_entry       ; far jump into 32-bit protected mode

; -----------------------------------------------------------------------------
; do_e820 - INT 15h/E820 memory map into MMAP_ENTRIES, count at MMAP_COUNT.
; -----------------------------------------------------------------------------
do_e820:
    mov dword [MMAP_COUNT], 0
    mov di, MMAP_ENTRIES
    xor ebx, ebx
    xor bp, bp
    mov edx, 0x534D4150
    mov eax, 0xE820
    mov dword [es:di + 20], 1
    mov ecx, 24
    int 0x15
    jc .done
    mov edx, 0x534D4150
    cmp eax, edx
    jne .done
    test ebx, ebx
    je .done
    jmp .jmpin
.loop:
    mov eax, 0xE820
    mov dword [es:di + 20], 1
    mov ecx, 24
    int 0x15
    jc .finish
    mov edx, 0x534D4150
.jmpin:
    jcxz .skip
    cmp cl, 20
    jbe .notext
    test byte [es:di + 20], 1
    je .skip
.notext:
    mov eax, [es:di + 8]
    or  eax, [es:di + 12]
    jz .skip
    inc bp
    add di, 24
.skip:
    test ebx, ebx
    jne .loop
.finish:
    mov [MMAP_COUNT], bp
.done:
    ret

; -----------------------------------------------------------------------------
; enable_a20 - BIOS service, then the fast A20 gate as a fallback.
; -----------------------------------------------------------------------------
enable_a20:
    mov ax, 0x2401
    int 0x15
    in  al, 0x92
    or  al, 0x02
    and al, 0xFE
    out 0x92, al
    ret

; =============================================================================
; 32-bit protected-mode entry
; =============================================================================
[bits 32]
pm_entry:
    mov ax, DATA_SEG
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

    mov esp, 0x90000           ; kernel stack
    xor ebp, ebp

    call kernel_main

.hang:
    cli
    hlt
    jmp .hang

; =============================================================================
; Flat GDT (base 0, limit 4 GB)
; =============================================================================
gdt_start:
    dq 0x0000000000000000
gdt_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 11001111b
    db 0x00
gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start
