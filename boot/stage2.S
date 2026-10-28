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

    mov si, offset msg_banner
    call print

    in al, 0x92                     # fast A20
    or al, 2
    and al, 0xFE
    out 0x92, al

    call get_memory

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
