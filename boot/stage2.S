# Petrichor stage 2 (loaded at 0x7E00, up to 15 sectors).
#  1. enable A20 and ask the BIOS how much RAM there is
#  2. read the kernel image from disk in 32 KiB chunks and copy each above 1 MiB
#  3. switch to 32-bit protected mode and jump to the kernel with the Multiboot
#     register convention (eax = magic, ebx = info struct at 0x500)
# tools/mkdisk patches kernel_lba / kernel_sectors.
.intel_syntax noprefix
.code16
.global _start
_start:
    jmp stage2_main
.org 4
kernel_lba:     .long 16
kernel_sectors: .long 0

.set MBI,       0x500
.set BOUNCE_SEG, 0x1000             # 0x10000: real-mode bounce buffer
.set KERNEL_DEST, 0x100000

stage2_main:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov byte ptr [boot_drive], dl

    in al, 0x92                     # fast A20
    or al, 2
    and al, 0xFE
    out 0x92, al

    call get_memory
    call boot_menu                  # sets the command line for the chosen entry

    mov si, offset msg_load
    call print
    mov eax, dword ptr [kernel_lba]
    mov dword ptr [cur_lba], eax
    mov eax, dword ptr [kernel_sectors]
    mov dword ptr [left], eax
    mov dword ptr [dest], KERNEL_DEST

load_loop:
    mov eax, dword ptr [left]
    test eax, eax
    jz load_done
    cmp eax, 64
    jbe 1f
    mov eax, 64
1:  mov dword ptr [chunk], eax

    mov word ptr [dap_count], ax
    mov word ptr [dap_off], 0
    mov word ptr [dap_seg], BOUNCE_SEG
    mov eax, dword ptr [cur_lba]
    mov dword ptr [dap_lba], eax
    mov ah, 0x42
    mov dl, byte ptr [boot_drive]
    mov si, offset dap
    int 0x13
    jc disk_error

    # INT 15h/87h block move: bounce buffer -> extended memory
    mov eax, dword ptr [dest]
    mov word ptr [bios_dst + 2], ax
    shr eax, 16
    mov byte ptr [bios_dst + 4], al
    mov ecx, dword ptr [chunk]
    shl ecx, 8                      # sectors * 512 / 2 words
    mov si, offset bios_gdt
    mov ah, 0x87
    int 0x15
    jc disk_error

    mov eax, dword ptr [chunk]
    add dword ptr [cur_lba], eax
    sub dword ptr [left], eax
    shl eax, 9
    add dword ptr [dest], eax
    mov al, '.'
    call putc
    jmp load_loop

load_done:
    mov si, offset msg_go
    call print

    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    .byte 0x66, 0xEA                # jmp 0x08:pm_entry (32-bit offset)
    .long pm_entry
    .word 0x08

disk_error:
    mov si, offset msg_err
    call print
1:  hlt
    jmp 1b

putc:
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    ret

print:
    lodsb
    test al, al
    jz 2f
    call putc
    jmp print
2:  ret


# ---------------- boot menu ----------------
.set TIMEOUT_TICKS, 73              # ~4 seconds at 18.2 Hz
.set N_ENTRIES, 4

# Writes the string at SI to video memory at DI (a byte offset) with attribute AH.
vputs:
    push es
    push ax
    mov ax, 0xB800
    mov es, ax
    pop ax
1:  lodsb
    test al, al
    jz 2f
    stosw
    jmp 1b
2:  pop es
    ret

# Fills CX cells at DI with a space of attribute AH.
vfill:
    push es
    push ax
    mov ax, 0xB800
    mov es, ax
    pop ax
    mov al, ' '
    rep stosw
    pop es
    ret

draw_menu:
    mov ax, 0x0600                  # clear the screen
    mov bh, 0x07
    xor cx, cx
    mov dx, 0x184F
    int 0x10

    mov di, 2*80*2 + 6*2
    mov si, offset txt_title
    mov ah, 0x0B
    call vputs
    mov di, 3*80*2 + 6*2
    mov si, offset txt_sub
    mov ah, 0x03
    call vputs

    xor bx, bx                      # entry index
.entry:
    mov ax, bx
    mov cx, 160
    mul cx                          # ax = index * 160 (one text row)
    mov di, ax
    add di, 7*160 + 8*2
    mov ah, 0x07
    mov si, offset txt_blank
    cmp bl, byte ptr [sel]
    jne 1f
    push di
    sub di, 4
    mov cx, 50
    mov ah, 0x70
    call vfill                      # highlight bar
    pop di
    mov ah, 0x70
1:  mov si, bx
    shl si, 1
    mov si, word ptr [entry_text + si]
    call vputs
    inc bx
    cmp bx, N_ENTRIES
    jb .entry

    mov di, 14*160 + 8*2
    mov si, offset txt_help
    mov ah, 0x08
    call vputs
    cmp byte ptr [auto], 0
    je 1f
    mov di, 16*160 + 8*2
    mov si, offset txt_count
    mov ah, 0x0E
    call vputs
    mov al, byte ptr [secs]
    add al, '0'
    mov ah, 0x0E
    push es
    push ax
    mov ax, 0xB800
    mov es, ax
    pop ax
    stosw
    pop es
    mov si, offset txt_sec
    mov ah, 0x0E
    call vputs
1:  ret

boot_menu:
    mov byte ptr [sel], 0
    mov byte ptr [auto], 1
    xor ah, ah
    int 0x1A                        # CX:DX = BIOS tick count
    add dx, TIMEOUT_TICKS
    mov word ptr [deadline], dx
    mov byte ptr [secs], 0xFF
.loop:
    cmp byte ptr [auto], 0
    je .poll
    xor ah, ah
    int 0x1A
    mov ax, word ptr [deadline]
    sub ax, dx                      # ticks remaining (16-bit, signed)
    jle .chosen                     # deadline passed: boot the highlighted entry
    xor dx, dx
    mov cx, 19
    div cx
    inc al                          # whole seconds left, rounded up
    cmp al, byte ptr [secs]
    je .poll
    mov byte ptr [secs], al
    call draw_menu
.poll:
    cmp byte ptr [auto], 0
    jne 1f
    cmp byte ptr [secs], 0xFE       # first redraw after the countdown is cancelled
    je 1f
    mov byte ptr [secs], 0xFE
    call draw_menu
1:  mov ah, 1
    int 0x16
    jz .loop
    xor ah, ah
    int 0x16                        # AH = scan code, AL = ASCII
    mov byte ptr [auto], 0
    cmp ah, 0x48
    je .up
    cmp ah, 0x50
    je .down
    cmp al, 13
    je .chosen
    cmp al, '1'
    jb .loop
    cmp al, '0' + N_ENTRIES
    ja .loop
    sub al, '1'
    mov byte ptr [sel], al
    jmp .chosen
.up:
    cmp byte ptr [sel], 0
    je .redraw
    dec byte ptr [sel]
    jmp .redraw
.down:
    cmp byte ptr [sel], N_ENTRIES - 1
    jae .redraw
    inc byte ptr [sel]
.redraw:
    mov byte ptr [secs], 0xFD
    call draw_menu
    jmp .loop

.chosen:
    mov ax, 0x0003                  # leave a clean screen for the loader messages
    int 0x10
    mov si, offset msg_banner
    call print

    # copy the entry's command line to 0x520 and advertise it in the info struct
    movzx bx, byte ptr [sel]
    shl bx, 1
    mov si, word ptr [entry_cmd + bx]
    mov di, 0x520
1:  lodsb
    stosb
    test al, al
    jnz 1b
    or dword ptr [MBI], 4           # flags bit 2: command line present
    mov dword ptr [MBI + 16], 0x520
    ret

.align 2
entry_text: .word ent0, ent1, ent2, ent3
entry_cmd:  .word cmd0, cmd1, cmd2, cmd3
ent0: .asciz "1  Petrichor"
ent1: .asciz "2  Petrichor (text console)"
ent2: .asciz "3  Petrichor (graphical desktop)"
ent3: .asciz "4  Petrichor safe mode (no disk)"
cmd0: .asciz ""
cmd1: .asciz "mode=text"
cmd2: .asciz "mode=desktop"
cmd3: .asciz "mode=text nodisk"
txt_title: .asciz "Petrichor Boot Menu"
txt_sub:   .asciz "the smell of rain on dry earth"
txt_help:  .asciz "Use the Up/Down keys and Enter, or press 1-4."
txt_count: .asciz "Booting the first entry in "
txt_sec:   .asciz " s  (press any key to stop)"
txt_blank: .asciz ""
sel:      .byte 0
auto:     .byte 1
secs:     .byte 0
deadline: .word 0

# Fills the Multiboot-style info struct: flags=1 (memory fields valid).
get_memory:
    mov dword ptr [MBI], 1
    int 0x12                        # KiB below 640K
    movzx eax, ax
    mov dword ptr [MBI + 4], eax
    mov ax, 0xE801
    int 0x15
    jc 1f
    test ax, ax
    jnz 2f
    mov ax, cx                      # some BIOSes answer in cx/dx
    mov bx, dx
2:  movzx eax, ax                   # KiB between 1M and 16M
    movzx ebx, bx                   # 64 KiB blocks above 16M
    shl ebx, 6
    add eax, ebx
    mov dword ptr [MBI + 8], eax
    ret
1:  mov ah, 0x88
    int 0x15
    movzx eax, ax
    mov dword ptr [MBI + 8], eax
    ret

.code32
pm_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    mov eax, 0x2BADB002
    mov ebx, MBI
    mov ecx, dword ptr [0x10000C]   # kernel entry point, stored after the Multiboot header
    jmp ecx

.code16
.align 4
gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF        # 0x08: 32-bit code, flat 4 GiB
    .quad 0x00CF92000000FFFF        # 0x10: 32-bit data, flat 4 GiB
gdt_desc:
    .word gdt_desc - gdt - 1
    .long gdt

# Descriptor table for INT 15h/87h: [2] = source, [3] = destination.
.align 4
bios_gdt:
    .quad 0
    .quad 0
    .word 0xFFFF, 0x0000            # source: 0x10000
    .byte 0x01, 0x93, 0x00, 0x00
bios_dst:
    .word 0xFFFF, 0x0000            # destination: patched per chunk
    .byte 0x10, 0x93, 0x00, 0x00
    .quad 0
    .quad 0

.align 4
dap:
    .byte 16, 0
dap_count: .word 0
dap_off:   .word 0
dap_seg:   .word 0
dap_lba:   .long 0, 0

boot_drive: .byte 0
.align 4
cur_lba: .long 0
left:    .long 0
dest:    .long 0
chunk:   .long 0

msg_banner: .asciz "Petrichor bootloader\r\n"
msg_load:   .asciz "loading kernel "
msg_go:     .asciz " ok\r\n"
msg_err:    .asciz "\r\nboot failed: disk error"
