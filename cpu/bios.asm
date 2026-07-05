; ===========================================================================
; PumpkinOS - real-mode BIOS call thunk
; ---------------------------------------------------------------------------
; void bios_int(uint8_t intno, regs16_t *regs)
;
; Steps down from 32-bit paged protected mode to real mode, runs one BIOS
; interrupt, and steps back:
;   PM32 -> (disable paging) -> 16-bit PM -> (clear PE) -> real mode
;   -> int N -> real mode -> (set PE) -> PM32 -> (restore paging)
;
; The real-mode portion plus its scratch data are copied down to a fixed low
; address (< 1 MiB, reachable from the 16-bit code segment whose limit is 64
; KiB) each call, so all the mode-transition code runs from there.
; ===========================================================================
[bits 32]
section .text
global bios_int

CODE32  equ 0x08
DATA32  equ 0x10
CODE16  equ 0x30
DATA16  equ 0x38

RELOC   equ 0x7C00                 ; where the relocatable block runs
STACK16 equ 0x7BFC                 ; real-mode stack top (just below RELOC)

%define REBASE(x) ((x) - reloc + RELOC)

bios_int:
    cli

    ; Preserve the caller's GP registers FIRST - the copies below clobber
    ; esi/edi/ecx, so saving after them would lose the caller's esi/edi.
    pushad                         ; args now at [esp+0x24] (intno), [esp+0x28] (regs)

    ; Copy the relocatable block down to RELOC.
    mov  esi, reloc
    mov  edi, RELOC
    mov  ecx, reloc_end - reloc
    rep  movsb

    ; Patch the interrupt number and remember the caller's regs pointer.
    mov  eax, [esp + 0x24]
    mov  [REBASE(ib)], al
    mov  eax, [esp + 0x28]
    mov  [REBASE(saved_regs)], eax

    ; Copy the caller's registers into the relocated block (26 bytes).
    mov  esi, eax
    mov  edi, REBASE(r_block)
    mov  ecx, 26
    rep  movsb

    ; Save PM state (GP registers were already pushed above).
    mov  [REBASE(saved_esp)], esp
    sidt [REBASE(saved_idt)]
    mov  eax, cr0
    mov  [REBASE(saved_cr0)], eax
    mov  eax, cr3
    mov  [REBASE(saved_cr3)], eax

    ; Real-mode IVT, then turn paging off (identity-mapped, so EIP stays valid).
    lidt [REBASE(rm_idt)]
    mov  eax, cr0
    and  eax, 0x7FFFFFFF
    mov  cr0, eax

    jmp  CODE16:REBASE(pm16)

; --------------------------------------------------------------------------
reloc:
[bits 16]
pm16:
    mov  ax, DATA16
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax
    mov  eax, cr0                  ; clear PE -> real mode
    and  al, 0xFE
    mov  cr0, eax
    jmp  0x0000:REBASE(rm16)

rm16:
    xor  ax, ax
    mov  ds, ax
    mov  ss, ax
    mov  sp, STACK16

    mov  ax, [REBASE(r_es)]
    mov  es, ax
    mov  ax, [REBASE(r_ax)]
    mov  bx, [REBASE(r_bx)]
    mov  cx, [REBASE(r_cx)]
    mov  dx, [REBASE(r_dx)]
    mov  si, [REBASE(r_si)]
    mov  di, [REBASE(r_di)]
    mov  bp, [REBASE(r_bp)]

    db 0xCD                        ; int N
ib: db 0x00

    mov  [REBASE(r_ax)], ax
    mov  [REBASE(r_bx)], bx
    mov  [REBASE(r_cx)], cx
    mov  [REBASE(r_dx)], dx
    mov  [REBASE(r_si)], si
    mov  [REBASE(r_di)], di
    mov  [REBASE(r_bp)], bp
    mov  ax, es
    mov  [REBASE(r_es)], ax
    pushf
    pop  ax
    mov  [REBASE(r_flags)], ax

    mov  eax, cr0                  ; set PE -> protected mode
    or   al, 1
    mov  cr0, eax
    jmp  CODE32:REBASE(pm32)

[bits 32]
pm32:
    mov  ax, DATA32
    mov  ds, ax
    mov  es, ax
    mov  fs, ax
    mov  gs, ax
    mov  ss, ax

    mov  eax, [REBASE(saved_cr3)]
    mov  cr3, eax
    mov  eax, [REBASE(saved_cr0)]  ; re-enables PG (and PE)
    mov  cr0, eax
    lidt [REBASE(saved_idt)]
    mov  esp, [REBASE(saved_esp)]

    mov  edi, [REBASE(saved_regs)] ; copy results back to the caller's struct
    mov  esi, REBASE(r_block)
    mov  ecx, 26
    rep  movsb

    popad
    sti
    ret

; ---- relocated data -------------------------------------------------------
align 4
rm_idt:     dw 0x3FF
            dd 0
saved_idt:  dw 0
            dd 0
saved_cr0:  dd 0
saved_cr3:  dd 0
saved_esp:  dd 0
saved_regs: dd 0

r_block:
r_di:    dw 0
r_si:    dw 0
r_bp:    dw 0
r_sp:    dw 0
r_bx:    dw 0
r_dx:    dw 0
r_cx:    dw 0
r_ax:    dw 0
r_gs:    dw 0
r_fs:    dw 0
r_es:    dw 0
r_ds:    dw 0
r_flags: dw 0
reloc_end:
