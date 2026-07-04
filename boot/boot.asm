; =============================================================================
; PumpkinOS - FAT12 boot sector
; -----------------------------------------------------------------------------
; The whole floppy is a single FAT12 volume. This boot sector carries a BPB,
; finds KERNEL.BIN in the root directory, and follows its cluster chain to load
; it HIGH, at 0x10000 (so a large kernel does not collide with the boot sector
; at 0x7C00). It then gathers the E820 memory map, enables A20, loads a flat
; GDT, switches to 32-bit protected mode, and jumps to the kernel at 0x10000.
; =============================================================================

[bits 16]
[org 0x7C00]

KERNEL_SEG    equ 0x1000     ; load kernel to 0x1000:0000 = physical 0x10000
KERNEL_PHYS   equ 0x10000
MMAP_COUNT    equ 0x0500     ; E820 count / entries left here for the kernel
MMAP_ENTRIES  equ 0x0504
ROOT_BUF      equ 0x9000     ; scratch: root directory
FAT_BUF       equ 0x7E00     ; scratch: FAT

; -----------------------------------------------------------------------------
; BIOS Parameter Block - standard 1.44 MB FAT12 (must match mkfs.fat)
; -----------------------------------------------------------------------------
    jmp short start
    nop
    db "PUMPKIN "
bpb_bytes_per_sec: dw 512
bpb_sec_per_clus:  db 1
bpb_reserved:      dw 1
bpb_num_fats:      db 2
bpb_root_entries:  dw 224
bpb_total_secs:    dw 2880
bpb_media:         db 0xF0
bpb_sec_per_fat:   dw 9
bpb_sec_per_track: dw 18
bpb_num_heads:     dw 2
bpb_hidden:        dd 0
bpb_total_big:     dd 0
bpb_drive:         db 0
                   db 0
                   db 0x29
                   dd 0x50554D50
                   db "PUMPKINOS  "
                   db "FAT12   "

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [bpb_drive], dl
    sti

    ; root_lba = reserved + num_fats * sec_per_fat
    mov al, [bpb_num_fats]
    xor ah, ah
    mul word [bpb_sec_per_fat]
    add ax, [bpb_reserved]
    mov [root_lba], ax
    ; root_secs = root_entries / 16
    mov ax, [bpb_root_entries]
    shr ax, 4
    mov [root_secs], ax
    ; data_lba = root_lba + root_secs
    add ax, [root_lba]
    mov [data_lba], ax

    ; load the root directory (ES:BX = 0:ROOT_BUF)
    mov ax, [root_lba]
    mov cx, [root_secs]
    xor bx, bx
    mov es, bx
    mov bx, ROOT_BUF
    call read_sectors

    ; scan for "KERNEL  BIN"
    mov di, ROOT_BUF
    mov cx, [bpb_root_entries]
.scan:
    push cx
    push di
    mov si, kernel_name
    mov cx, 11
    repe cmpsb
    pop di
    pop cx
    je .found
    add di, 32
    loop .scan
    jmp disk_error
.found:
    mov ax, [di + 26]
    mov [cluster], ax

    ; load the FAT (ES:BX = 0:FAT_BUF)
    mov ax, [bpb_reserved]
    mov cx, [bpb_sec_per_fat]
    xor bx, bx
    mov es, bx
    mov bx, FAT_BUF
    call read_sectors

    ; follow the cluster chain, loading the kernel to 0x10000
    mov ax, KERNEL_SEG
    mov es, ax
    xor bx, bx
.load:
    mov ax, [cluster]
    cmp ax, 0x0FF8
    jae .loaded
    sub ax, 2
    add ax, [data_lba]
    mov cx, 1
    call read_sectors          ; reads to ES:BX, advances ES:BX
    mov ax, [cluster]
    call next_cluster
    mov [cluster], ax
    jmp .load
.loaded:

    ; ---- real-mode bring-up, then protected mode ----
    call enable_a20
    call do_e820
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp CODE_SEG:pm_start

; -----------------------------------------------------------------------------
; read_sectors - AX = start LBA, CX = count, ES:BX = dest.
; Advances BX by 512 per sector, carrying into ES at 64 KB boundaries.
; -----------------------------------------------------------------------------
read_sectors:
.loop:
    test cx, cx
    jz .ret
    push cx
    push ax
    xor dx, dx
    div word [bpb_sec_per_track]
    mov cl, dl
    inc cl
    xor dx, dx
    div word [bpb_num_heads]
    mov ch, al
    mov dh, dl
    mov dl, [bpb_drive]
    mov ax, 0x0201
    mov di, 5
.retry:
    int 0x13
    jnc .ok
    xor ah, ah
    int 0x13
    dec di
    jnz .retry
    jmp disk_error
.ok:
    pop ax
    pop cx
    add bx, 512
    jnc .noseg
    mov si, es
    add si, 0x1000              ; next 64 KB segment
    mov es, si
.noseg:
    inc ax
    dec cx
    jmp .loop
.ret:
    ret

; -----------------------------------------------------------------------------
; next_cluster - AX = cluster -> AX = next (from the FAT at FAT_BUF, DS = 0).
; -----------------------------------------------------------------------------
next_cluster:
    push bx
    push cx
    mov cx, ax
    and cx, 1
    mov bx, ax
    shr bx, 1
    add bx, ax
    add bx, FAT_BUF
    mov ax, [bx]
    test cx, cx
    jz .even
    shr ax, 4
    jmp .end
.even:
    and ax, 0x0FFF
.end:
    pop cx
    pop bx
    ret

enable_a20:
    in  al, 0x92               ; fast A20 gate
    or  al, 0x02
    out 0x92, al
    ret

; -----------------------------------------------------------------------------
; do_e820 - INT 15h/E820 memory map into MMAP_ENTRIES, count at MMAP_COUNT.
; Compact loop: repairs edx each iteration and forces a valid ACPI entry.
; -----------------------------------------------------------------------------
do_e820:
    xor ax, ax
    mov es, ax
    mov di, MMAP_ENTRIES
    xor ebx, ebx
    xor bp, bp
.loop:
    mov eax, 0xE820
    mov edx, 0x534D4150
    mov ecx, 24
    mov dword [es:di + 20], 1
    int 0x15
    jc .done                   ; carry: unsupported or end of list
    jcxz .skip
    inc bp
    add di, 24
.skip:
    test ebx, ebx
    jnz .loop
.done:
    mov [MMAP_COUNT], bp
    ret

disk_error:
    mov ax, 0x0E45             ; 'E' via BIOS teletype
    int 0x10
.hang:
    hlt
    jmp .hang

; =============================================================================
; 32-bit protected-mode entry: hand off to the kernel at 0x10000
; =============================================================================
[bits 32]
pm_start:
    mov ax, DATA_SEG
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp KERNEL_PHYS

; =============================================================================
; Flat GDT
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

; =============================================================================
; Data
; =============================================================================
kernel_name db "KERNEL  BIN"
root_lba    dw 0
root_secs   dw 0
data_lba    dw 0
cluster     dw 0

times 510 - ($ - $$) db 0
dw 0xAA55
