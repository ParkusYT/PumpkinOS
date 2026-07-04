; =============================================================================
; PumpkinOS - Stage 1 Bootloader (512-byte BIOS boot sector)
; -----------------------------------------------------------------------------
; The BIOS loads this sector to physical address 0x7C00 in 16-bit real mode
; and jumps to it. Its job is to:
;   1. Set up a stack and save the BIOS boot drive number.
;   2. Load the kernel from the floppy into memory at 0x1000.
;   3. Enable the A20 line (access to memory above 1 MB).
;   4. Load a flat GDT and enter 32-bit protected mode.
;   5. Jump to the kernel entry point.
; =============================================================================

[bits 16]
[org 0x7C00]

KERNEL_OFFSET   equ 0x1000      ; where we load the kernel in memory

; The build passes the exact kernel size with `nasm -DKSECTORS=<n>`, so we only
; read as many sectors as the kernel actually occupies. This matters: the load
; region grows upward from 0x1000, and reading too far would run straight over
; the boot sector (0x7C00) and its stack, corrupting the code as it executes.
%ifndef KSECTORS
%define KSECTORS 8              ; sane fallback for a standalone assemble
%endif
KERNEL_SECTORS  equ KSECTORS

; -----------------------------------------------------------------------------
; Entry point
; -----------------------------------------------------------------------------
start:
    cli                         ; no interrupts while we set up segments
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00              ; stack grows down from just below us
    mov [boot_drive], dl        ; BIOS leaves the boot drive number in DL
    sti

    mov si, msg_load
    call print_string

    call load_kernel            ; copy the kernel off the floppy into RAM

    mov si, msg_ok
    call print_string

    call enable_a20

    ; ---- switch to 32-bit protected mode ----
    cli
    lgdt [gdt_descriptor]       ; tell the CPU about our segment table
    mov eax, cr0
    or  eax, 0x1                ; set the Protection Enable bit
    mov cr0, eax
    jmp CODE_SEG:init_pm        ; far jump flushes the pipeline & loads CS

; -----------------------------------------------------------------------------
; print_string - print the NUL-terminated string at DS:SI via BIOS teletype
; -----------------------------------------------------------------------------
print_string:
    pusha
.next:
    lodsb                       ; AL = [SI++]
    test al, al
    jz .done
    mov ah, 0x0E                ; BIOS teletype output
    xor bh, bh
    int 0x10
    jmp .next
.done:
    popa
    ret

; -----------------------------------------------------------------------------
; load_kernel - read KERNEL_SECTORS sectors starting at LBA 1 into ES:KERNEL_OFFSET
; Reads one sector at a time, converting LBA -> CHS, with retry-on-error.
; Floppy geometry: 18 sectors/track, 2 heads.
; -----------------------------------------------------------------------------
load_kernel:
    mov word [lba], 1                   ; sector 1 = boot sector, kernel starts at LBA 1
    mov word [sectors_left], KERNEL_SECTORS
    mov bx, KERNEL_OFFSET               ; ES:BX = destination pointer (ES = 0)
.next_sector:
    cmp word [sectors_left], 0
    je .done

    ; --- convert LBA (in [lba]) to CHS ---
    mov ax, [lba]
    xor dx, dx
    mov cx, 18
    div cx                              ; AX = LBA/18, DX = LBA%18
    inc dx
    mov [sector], dl                    ; sector = (LBA % 18) + 1

    xor dx, dx
    mov cx, 2
    div cx                              ; AX = cylinder, DX = head
    mov [cylinder], al
    mov [head], dl

    mov di, 5                           ; up to 5 attempts per sector
.attempt:
    mov ah, 0x02                        ; INT 13h: read sectors
    mov al, 1                           ; one sector at a time
    mov ch, [cylinder]
    mov cl, [sector]
    mov dh, [head]
    mov dl, [boot_drive]
    int 0x13
    jnc .read_ok

    ; on error: reset disk controller and retry
    xor ah, ah
    mov dl, [boot_drive]
    int 0x13
    dec di
    jnz .attempt
    jmp disk_error                      ; out of retries

.read_ok:
    add bx, 512                         ; advance destination by one sector
    inc word [lba]
    dec word [sectors_left]
    jmp .next_sector
.done:
    ret

; -----------------------------------------------------------------------------
; enable_a20 - try the BIOS service first, then fall back to the fast A20 gate
; -----------------------------------------------------------------------------
enable_a20:
    mov ax, 0x2401                      ; BIOS: enable A20
    int 0x15
    in  al, 0x92                        ; fast A20 via System Control Port A
    or  al, 0x02
    and al, 0xFE                        ; make sure we don't accidentally reset
    out 0x92, al
    ret

; -----------------------------------------------------------------------------
; disk_error - print a message and halt (still in real mode)
; -----------------------------------------------------------------------------
disk_error:
    mov si, msg_disk_err
    call print_string
.hang:
    hlt
    jmp .hang

; =============================================================================
; 32-bit protected-mode continuation
; =============================================================================
[bits 32]
init_pm:
    mov ax, DATA_SEG                    ; reload the segment registers with our
    mov ds, ax                          ; flat 4 GB data descriptor
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000                    ; a comfortable protected-mode stack
    jmp KERNEL_OFFSET                   ; off to the kernel!

; =============================================================================
; Global Descriptor Table - flat memory model (base 0, limit 4 GB)
; =============================================================================
gdt_start:
    dq 0x0000000000000000              ; null descriptor (required)

gdt_code:                               ; 0x08: code, base 0, limit 4 GB, 32-bit
    dw 0xFFFF                           ; limit (bits 0-15)
    dw 0x0000                           ; base  (bits 0-15)
    db 0x00                             ; base  (bits 16-23)
    db 10011010b                        ; present, ring 0, code, exec/read
    db 11001111b                        ; 4 KB granularity, 32-bit, limit 16-19
    db 0x00                             ; base  (bits 24-31)

gdt_data:                               ; 0x10: data, base 0, limit 4 GB
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b                        ; present, ring 0, data, read/write
    db 11001111b
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1          ; size of GDT minus one
    dd gdt_start                        ; linear address of GDT

CODE_SEG equ gdt_code - gdt_start       ; selector 0x08
DATA_SEG equ gdt_data - gdt_start       ; selector 0x10

; =============================================================================
; Data
; =============================================================================
boot_drive   db 0
lba          dw 0
sectors_left dw 0
cylinder     db 0
head         db 0
sector       db 0

msg_load     db "PumpkinOS: loading kernel...", 13, 10, 0
msg_ok       db "PumpkinOS: entering protected mode.", 13, 10, 0
msg_disk_err db "PumpkinOS: DISK READ ERROR!", 13, 10, 0

; -----------------------------------------------------------------------------
; Boot sector padding and signature
; -----------------------------------------------------------------------------
times 510 - ($ - $$) db 0              ; pad to 510 bytes
dw 0xAA55                               ; boot signature the BIOS looks for
