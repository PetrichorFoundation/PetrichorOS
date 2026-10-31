# Petrichor stage 2 (loaded at 0x7E00, up to 15 sectors).
#  1. ask the BIOS how much RAM there is and what kind of drive we booted from
#  2. show the boot menu (it also picks the kernel command line)
#  3. read the kernel into RAM above 1 MiB (works from disks with 512-byte sectors and from CDs
#     with 2048-byte sectors) and keep a pristine copy at 12 MiB for the installer
#  4. for graphical boots, switch the display to a 1024x768 32-bit VESA linear framebuffer
#  5. enter 32-bit protected mode and jump to the kernel with the Multiboot register convention
#     (eax = magic, ebx = info structure at 0x500)
# tools/mkdisk and tools/mkiso patch kernel_lba / kernel_sectors; pinstall writes boot_id.
.intel_syntax noprefix
.code16
.global _start
_start:
    jmp stage2_main
.org 4
kernel_lba:     .long 16                # in 512-byte units; must be a multiple of 4 on CDs
kernel_sectors: .long 0
.org 12
boot_id:        .fill 16, 1, 0          # random per install, so the kernel can tell which disk it booted from
.org 28
                .ascii "PSG2"           # the kernel recognises this sector in RAM and on disk

.set MBI,         0x500                 # Multiboot info (112 bytes)
.set CMDLINE,     0x600
.set MODULE,      0x620                 # one module: the pristine kernel copy
.set BOUNCE_SEG,  0x1000                # 0x10000: real-mode bounce buffer
.set KERNEL_DEST, 0x100000
.set KERNEL_COPY, 0xC00000              # 12 MiB
.set VBE_INFO,    0x9000
.set VBE_MODE,    0x9200

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

    mov byte ptr [sec_shift], 0     # 2048-byte sectors (a CD) => shift by 2
    mov ah, 0x48
    mov dl, byte ptr [boot_drive]
    mov si, offset drv_params
    mov word ptr [drv_params], 0x1E
    int 0x13
    jc 1f
    cmp word ptr [drv_params + 0x18], 2048
    jne 1f
    mov byte ptr [sec_shift], 2
1:
    call get_memory
    call boot_menu                  # sets the command line and want_vbe

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

    mov cl, byte ptr [sec_shift]    # convert 512-byte units to the drive's sector size
    mov edx, 1
    shl edx, cl
    dec edx
    add eax, edx
    shr eax, cl
    mov word ptr [dap_count], ax
    mov word ptr [dap_off], 0
    mov word ptr [dap_seg], BOUNCE_SEG
    mov eax, dword ptr [cur_lba]
    shr eax, cl
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
    call copy_kernel_image
    mov si, offset msg_go
    call print

    cmp byte ptr [want_vbe], 0
    je 1f
    call vbe_setup
1:
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

# Copies the freshly loaded kernel to KERNEL_COPY so the installer can write an unmodified
# image later (the running kernel changes its own .data).
copy_kernel_image:
    mov dword ptr [csrc], KERNEL_DEST
    mov dword ptr [cdst], KERNEL_COPY
    mov eax, dword ptr [kernel_sectors]
    shl eax, 9
    mov dword ptr [cleft], eax
1:  mov eax, dword ptr [cleft]
    test eax, eax
    jz 2f
    cmp eax, 0x10000
    jbe 3f
    mov eax, 0x10000
3:  mov dword ptr [cnow], eax
    mov eax, dword ptr [csrc]
    mov word ptr [bios_src + 2], ax
    shr eax, 16
    mov byte ptr [bios_src + 4], al
    mov eax, dword ptr [cdst]
    mov word ptr [bios_dst + 2], ax
    shr eax, 16
    mov byte ptr [bios_dst + 4], al
    mov ecx, dword ptr [cnow]
    shr ecx, 1
    mov si, offset bios_gdt
    mov ah, 0x87
    int 0x15
    jc disk_error
    mov eax, dword ptr [cnow]
    add dword ptr [csrc], eax
    add dword ptr [cdst], eax
    sub dword ptr [cleft], eax
    jmp 1b
2:  mov dword ptr [MODULE], KERNEL_COPY
    mov eax, dword ptr [kernel_sectors]
    shl eax, 9
    add eax, KERNEL_COPY
    mov dword ptr [MODULE + 4], eax
    mov dword ptr [MODULE + 8], offset mod_name
    mov dword ptr [MODULE + 12], 0
    mov dword ptr [MBI + 20], 1     # mods_count
    mov dword ptr [MBI + 24], MODULE
    or dword ptr [MBI], 8           # flags bit 3: modules present
    ret

# Looks for a 1024x768, 32-bit, B-G-R linear-framebuffer mode and switches to it.
# On success it fills the Multiboot framebuffer fields; on failure the screen stays in text mode.
vbe_setup:
    push es
    xor ax, ax
    mov es, ax
    mov di, VBE_INFO
    mov dword ptr [VBE_INFO], 0x32454256      # "VBE2": ask for VBE 2.0+ information
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .fail
    mov si, word ptr [VBE_INFO + 14]          # far pointer to the mode list
    mov ax, word ptr [VBE_INFO + 16]
    mov fs, ax
.next:
    mov cx, word ptr fs:[si]
    add si, 2
    cmp cx, 0xFFFF
    je .fail
    push si
    push cx
    push fs
    mov ax, 0x4F01
    mov di, VBE_MODE
    int 0x10
    pop fs
    pop cx
    pop si
    cmp ax, 0x004F
    jne .next
    mov ax, word ptr [VBE_MODE]
    and ax, 0x0081                            # supported + linear framebuffer
    cmp ax, 0x0081
    jne .next
    cmp word ptr [VBE_MODE + 0x12], 1024
    jne .next
    cmp word ptr [VBE_MODE + 0x14], 768
    jne .next
    cmp byte ptr [VBE_MODE + 0x19], 32
    jne .next
    cmp byte ptr [VBE_MODE + 0x1B], 6         # direct colour
    jne .next
    cmp byte ptr [VBE_MODE + 0x20], 16        # red at bit 16, green at 8, blue at 0
    jne .next
    cmp byte ptr [VBE_MODE + 0x22], 8
    jne .next
    mov bx, cx
    or bx, 0x4000                             # use the linear framebuffer
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .fail
    mov eax, dword ptr [VBE_MODE + 0x28]
    mov dword ptr [MBI + 88], eax
    mov dword ptr [MBI + 92], 0
    movzx eax, word ptr [VBE_MODE + 0x10]
    mov dword ptr [MBI + 96], eax
    mov dword ptr [MBI + 100], 1024
    mov dword ptr [MBI + 104], 768
    mov byte ptr [MBI + 108], 32
    mov byte ptr [MBI + 109], 1               # direct RGB
    or dword ptr [MBI], 0x1000                # flags bit 12: framebuffer info present
.fail:
    pop es
    ret

# Fills the Multiboot-style info struct: flags=1 (memory fields valid).
get_memory:
    xor eax, eax
    mov di, MBI                     # clear the whole structure first
    mov cx, 28
    cld
    rep stosd
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

    movzx bx, byte ptr [sel]
    mov byte ptr [want_vbe], 0      # the default and the "graphical desktop" entries want a framebuffer
    test bl, bl
    jz 1f
    cmp bl, 2
    jne 2f
1:  mov byte ptr [want_vbe], 1
2:  shl bx, 1
    mov si, word ptr [entry_cmd + bx]
    mov di, CMDLINE
3:  lodsb
    stosb
    test al, al
    jnz 3b
    or dword ptr [MBI], 4           # flags bit 2: command line present
    mov dword ptr [MBI + 16], CMDLINE
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
    mov ecx, dword ptr [0x10001C]   # kernel entry point: entry_addr in the Multiboot (a.out kludge) header
    jmp ecx

.code16
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
mod_name:  .asciz "kernel"
sel:      .byte 0
auto:     .byte 1
secs:     .byte 0
want_vbe: .byte 0
sec_shift: .byte 0
deadline: .word 0

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
bios_src:
    .word 0xFFFF, 0x0000            # source: 0x10000 (the bounce buffer) unless patched
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

.align 4
drv_params: .fill 32, 1, 0
boot_drive: .byte 0
.align 4
cur_lba: .long 0
left:    .long 0
dest:    .long 0
chunk:   .long 0
csrc:    .long 0
cdst:    .long 0
cleft:   .long 0
cnow:    .long 0

msg_banner: .asciz "Petrichor bootloader\r\n"
msg_load:   .asciz "loading kernel "
msg_go:     .asciz " ok\r\n"
msg_err:    .asciz "\r\nboot failed: disk error"
