; =============================================================================
; PumpkinOS - context switch
; -----------------------------------------------------------------------------
; void context_switch(uint32_t *old_esp, uint32_t new_esp)
;
; Saves the callee-saved registers + EFLAGS of the current task onto its stack,
; stores the resulting ESP into *old_esp, loads new_esp, restores the next
; task's saved registers, and returns - so execution continues wherever that
; task was last switched out. A freshly created task has a hand-crafted stack
; (see task_spawn) that makes this 'ret' jump into its entry trampoline.
; =============================================================================

[bits 32]
section .text
global context_switch

context_switch:
    push ebp
    push ebx
    push esi
    push edi
    pushfd                     ; save EFLAGS (carries the interrupt-enable bit)

    ; After 5 pushes: [esp+20]=ret, [esp+24]=old_esp, [esp+28]=new_esp
    mov eax, [esp + 24]        ; eax = old_esp (uint32_t *)
    mov [eax], esp             ; *old_esp = current esp
    mov eax, [esp + 28]        ; eax = new_esp
    mov esp, eax               ; switch to the new task's stack

    popfd                      ; restore the new task's saved registers
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret                        ; resume where the new task left off
