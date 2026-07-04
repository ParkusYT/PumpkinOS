; =============================================================================
; PumpkinOS - FAT12 boot sector
; -----------------------------------------------------------------------------
; The whole floppy is a single FAT12 volume. This boot sector carries a BPB,
; so DOS/Linux/mtools all see a normal FAT12 disk. Its job is to find the file
; KERNEL.BIN in the root directory, follow its cluster chain to load it at
; 0x1000, and jump to it in real mode. The kernel itself then does the E820
; probe, A20, GDT and the switch to protected mode (see kernel/entry.asm).
; =============================================================================

[bits 16]
[org 0x7C00]

KERNEL_LOAD equ 0x1000      ; where the kernel file is loaded
FAT_BUF     equ 0x7E00      ; scratch: the FAT (9 sectors)
ROOT_BUF    equ 0x9000      ; scratch: the root directory (14 sectors)

; -----------------------------------------------------------------------------
; BIOS Parameter Block - standard 1.44 MB FAT12 geometry (must match mkfs.fat)
; -----------------------------------------------------------------------------
    jmp short start
    nop
    db "PUMPKIN "              ; 0x03 OEM name
bpb_bytes_per_sec: dw 512      ; 0x0B
bpb_sec_per_clus:  db 1        ; 0x0D
bpb_reserved:      dw 1        ; 0x0E
bpb_num_fats:      db 2        ; 0x10
bpb_root_entries:  dw 224      ; 0x11
bpb_total_secs:    dw 2880     ; 0x13
bpb_media:         db 0xF0     ; 0x15
bpb_sec_per_fat:   dw 9        ; 0x16
bpb_sec_per_track: dw 18       ; 0x18
bpb_num_heads:     dw 2        ; 0x1A
bpb_hidden:        dd 0        ; 0x1C
bpb_total_big:     dd 0        ; 0x20
bpb_drive:         db 0        ; 0x24
                   db 0        ; 0x25 reserved
                   db 0x29     ; 0x26 extended boot signature
                   dd 0x50554D50 ; 0x27 volume id
                   db "PUMPKINOS  " ; 0x2B volume label (11)
                   db "FAT12   " ; 0x36 fs type (8)   -> ends at 0x3D

; -----------------------------------------------------------------------------
; Boot code (starts at offset 0x3E)
; -----------------------------------------------------------------------------
start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [bpb_drive], dl        ; BIOS boot drive
    sti

    mov si, msg_load
    call print_string

    ; ---- root_lba = reserved + num_fats * sec_per_fat ----
    mov al, [bpb_num_fats]
    xor ah, ah
    mul word [bpb_sec_per_fat]
    add ax, [bpb_reserved]
    mov [root_lba], ax

    ; ---- root_sectors = root_entries / 16  (32 bytes each, 512/sector) ----
    mov ax, [bpb_root_entries]
    shr ax, 4
    mov [root_secs], ax

    ; ---- data_lba = root_lba + root_sectors ----
    add ax, [root_lba]
    mov [data_lba], ax

    ; ---- load the root directory ----
    mov ax, [root_lba]
    mov cx, [root_secs]
    mov bx, ROOT_BUF
    call read_sectors

    ; ---- scan for "KERNEL  BIN" ----
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
    jmp disk_error            ; KERNEL.BIN not found

.found:
    mov ax, [di + 26]         ; first cluster
    mov [cluster], ax

    ; ---- load the FAT ----
    mov ax, [bpb_reserved]
    mov cx, [bpb_sec_per_fat]
    mov bx, FAT_BUF
    call read_sectors

    ; ---- follow the cluster chain, loading the kernel to 0x1000 ----
    mov bx, KERNEL_LOAD
.load:
    mov ax, [cluster]
    cmp ax, 0x0FF8
    jae .done
    sub ax, 2                 ; LBA = data_lba + (cluster - 2) * 1
    add ax, [data_lba]
    mov cx, 1
    call read_sectors        ; reads one sector to ES:BX, advances BX
    mov ax, [cluster]
    call next_cluster
    mov [cluster], ax
    jmp .load
.done:
    mov dl, [bpb_drive]
    jmp 0x0000:KERNEL_LOAD    ; enter the kernel (still real mode)

; -----------------------------------------------------------------------------
; read_sectors - AX = start LBA, CX = count, ES:BX = dest (BX advances).
; Converts LBA -> CHS using the BPB geometry, one sector per INT 13h, retries.
; -----------------------------------------------------------------------------
read_sectors:
.loop:
    test cx, cx
    jz .ret
    push cx
    push ax
    xor dx, dx
    div word [bpb_sec_per_track]   ; ax = LBA / spt, dx = sector index
    mov cl, dl
    inc cl                         ; CL = sector number (1-based)
    xor dx, dx
    div word [bpb_num_heads]       ; ax = cylinder, dx = head
    mov ch, al                     ; CH = cylinder low 8 bits
    mov dh, dl                     ; DH = head
    mov dl, [bpb_drive]
    mov ax, 0x0201                 ; AH = read, AL = 1 sector
    mov di, 5
.retry:
    int 0x13
    jnc .ok
    xor ah, ah
    int 0x13                       ; reset controller, retry
    dec di
    jnz .retry
    jmp disk_error
.ok:
    pop ax
    pop cx
    add bx, 512
    inc ax
    dec cx
    jmp .loop
.ret:
    ret

; -----------------------------------------------------------------------------
; next_cluster - AX = cluster -> AX = next cluster (from the FAT in FAT_BUF).
; FAT12 entries are 12 bits, packed 1.5 bytes each.
; -----------------------------------------------------------------------------
next_cluster:
    push bx
    push cx
    mov cx, ax
    and cx, 1                      ; odd/even
    mov bx, ax
    shr bx, 1
    add bx, ax                     ; bx = cluster * 3 / 2
    add bx, FAT_BUF
    mov ax, [bx]
    test cx, cx
    jz .even
    shr ax, 4                      ; odd cluster: high 12 bits
    jmp .end
.even:
    and ax, 0x0FFF                 ; even cluster: low 12 bits
.end:
    pop cx
    pop bx
    ret

; -----------------------------------------------------------------------------
; print_string - NUL-terminated DS:SI via BIOS teletype
; -----------------------------------------------------------------------------
print_string:
    pusha
.next:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    xor bh, bh
    int 0x10
    jmp .next
.done:
    popa
    ret

disk_error:
    mov si, msg_err
    call print_string
.hang:
    hlt
    jmp .hang

; -----------------------------------------------------------------------------
; Data
; -----------------------------------------------------------------------------
kernel_name db "KERNEL  BIN"
root_lba    dw 0
root_secs   dw 0
data_lba    dw 0
cluster     dw 0
msg_load    db "Loading KERNEL.BIN...", 13, 10, 0
msg_err     db "KERNEL.BIN not found!", 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xAA55
