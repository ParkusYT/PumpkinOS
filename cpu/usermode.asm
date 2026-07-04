; =============================================================================
; PumpkinOS - drop into ring 3
; -----------------------------------------------------------------------------
; void enter_user_mode(uint32_t entry, uint32_t user_esp)
;
; You cannot just jump to ring 3 - the privilege level only drops through an
; inter-privilege return. So we load the user data segments, build the exact
; stack frame an `iret` expects for a ring-0 -> ring-3 return, and iret. The
; CPU pops EIP/CS/EFLAGS and, because CS has RPL 3, also ESP/SS - landing us at
; `entry` on the user stack at CPL 3.
; =============================================================================

[bits 32]
section .text
global enter_user_mode

USER_CODE equ 0x18 | 3          ; user code selector, RPL 3 => 0x1B
USER_DATA equ 0x20 | 3          ; user data selector, RPL 3 => 0x23

enter_user_mode:
    mov eax, [esp + 4]          ; entry point
    mov ecx, [esp + 8]          ; user stack pointer

    mov bx, USER_DATA           ; point the data segments at user data
    mov ds, bx
    mov es, bx
    mov fs, bx
    mov gs, bx

    push dword USER_DATA        ; SS   (popped last by iret)
    push ecx                    ; ESP  (user stack)
    pushfd                      ; EFLAGS...
    pop edx
    or  edx, 0x200              ; ...with IF set so interrupts stay on
    push edx
    push dword USER_CODE        ; CS
    push eax                    ; EIP  (popped first by iret)
    iret
