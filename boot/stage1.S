# Petrichor stage 1: the 512-byte MBR.  BIOS loads it at 0x7C00; it pulls
# stage 2 (LBA 1..15) to 0x7E00 with INT 13h extended reads and jumps there.
.intel_syntax noprefix
.code16
.global _start
_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov byte ptr [boot_drive], dl
    mov si, offset msg
    call print

    mov ah, 0x42
    mov dl, byte ptr [boot_drive]
    mov si, offset dap
    int 0x13
    jc disk_error

    mov dl, byte ptr [boot_drive]
    .byte 0xEA                      # jmp 0x0000:0x7E00
    .word 0x7E00, 0x0000

disk_error:
    mov si, offset err
    call print
1:  hlt
    jmp 1b

print:
    lodsb
    test al, al
    jz 2f
    mov ah, 0x0E
    mov bx, 7
    int 0x10
    jmp print
2:  ret

msg:        .asciz "Petrichor: "
err:        .asciz "disk error"
boot_drive: .byte 0

.align 4
dap:
    .byte 16, 0
    .word 15                        # sectors
    .word 0x7E00, 0x0000            # destination offset:segment
    .long 1, 0                      # starting LBA

.org 510
.word 0xAA55
