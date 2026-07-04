; =============================================================================
; PumpkinOS - ring-3 user programs
; -----------------------------------------------------------------------------
; These little blobs run at CPL 3. They are position-independent (only relative
; branches, register-relative addressing, immediates and int 0x80), so the
; kernel can copy them to any user page and jump in. They reach the kernel only
; through int 0x80 - they cannot touch hardware or kernel memory directly.
; =============================================================================

[bits 32]
section .text

global user_program_start
global user_program_end
global user_fault_start
global user_fault_end

SYS_EXIT   equ 0
SYS_PUTC   equ 1
SYS_GETCPL equ 3

; ---- clean demo: print a message, then the caller's CPL, then exit ----------
user_program_start:
    call .get_ip
.get_ip:
    pop ebx                         ; ebx = address of .get_ip (position-independent)
    lea esi, [ebx + (.msg - .get_ip)]
.loop:
    movzx ebx, byte [esi]           ; next character of the message
    test bl, bl
    jz .show_cpl
    mov eax, SYS_PUTC
    int 0x80
    inc esi
    jmp .loop
.show_cpl:
    mov eax, SYS_GETCPL             ; ask the kernel what ring we are in
    int 0x80
    add eax, '0'                    ; turn 0..3 into a digit
    mov ebx, eax
    mov eax, SYS_PUTC
    int 0x80
    mov ebx, 10                     ; newline
    mov eax, SYS_PUTC
    int 0x80
    mov eax, SYS_EXIT               ; done - hand control back to the scheduler
    int 0x80
.hang:
    jmp .hang
.msg:
    db "[ring3] hello from PumpkinOS user mode! I print via int 0x80 and my CPL is ", 0
user_program_end:

; ---- protection demo: read kernel memory from ring 3 (expect a page fault) --
user_fault_start:
    mov eax, [0x1000]               ; 0x1000 is a supervisor-only kernel page
    jmp $                           ; (never reached - the read faults first)
user_fault_end:
