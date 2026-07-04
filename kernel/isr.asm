; =============================================================================
; PumpkinOS - interrupt stubs
; -----------------------------------------------------------------------------
; The IDT points every vector at one of the isrN / irqN stubs below. Each stub
; pushes a (dummy) error code + the vector number so every entry looks the same
; on the stack, then jumps to isr_common, which saves the CPU state, calls the
; C dispatcher isr_handler(), restores state, and returns with iret.
; =============================================================================

[bits 32]
extern isr_handler

; --- exception with NO CPU error code: push a dummy 0 so the frame is uniform
%macro ISR_NOERR 1
    global isr%1
isr%1:
    push dword 0            ; fake error code
    push dword %1          ; interrupt number
    jmp isr_common
%endmacro

; --- exception that DOES push its own error code
%macro ISR_ERR 1
    global isr%1
isr%1:
    push dword %1          ; interrupt number (error code already on stack)
    jmp isr_common
%endmacro

; --- hardware IRQ: arg1 = irq stub name suffix, arg2 = remapped vector number
%macro IRQ 2
    global irq%1
irq%1:
    push dword 0
    push dword %2
    jmp isr_common
%endmacro

; CPU exceptions 0..31 (8, 10-14 and 17 push a real error code).
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; Hardware IRQs 0..15, remapped by the PIC to vectors 32..47.
IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

; -----------------------------------------------------------------------------
; Common landing pad: save state, call C, restore state, iret.
; -----------------------------------------------------------------------------
isr_common:
    pusha                  ; edi,esi,ebp,esp,ebx,edx,ecx,eax

    xor eax, eax
    mov ax, ds
    push eax               ; save the data segment selector

    mov ax, 0x10           ; load the kernel data segment
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp               ; pass a pointer to 'struct registers'
    call isr_handler
    add esp, 4

    pop eax                ; restore the data segment selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    popa
    add esp, 8             ; discard int_no and err_code
    iret                   ; pops eip, cs, eflags (re-enables interrupts)
