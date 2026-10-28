# Petrichor entry point. Loaded by any Multiboot-compliant loader (QEMU -kernel, GRUB).

.set MB_MAGIC, 0x1BADB002
.set MB_FLAGS, 0x00000003          # page-align modules, provide memory info

.section .multiboot, "a"
.align 4
.long MB_MAGIC
.long MB_FLAGS
.long -(MB_MAGIC + MB_FLAGS)
.long _start                      # read by the Petrichor bootloader (offset 12)

.section .bss
.align 16
.global boot_stack_bottom
boot_stack_bottom:
.skip 16384
boot_stack_top:

.section .text
.global _start
_start:
    cli
    mov %eax, %esi                  # keep the loader's magic and info pointer
    mov %ebx, %edx
    mov $__bss_start, %edi          # a raw loader doesn't clear .bss for us
    mov $_end, %ecx
    sub %edi, %ecx
    xor %eax, %eax
    cld
    rep stosb
    mov $boot_stack_top, %esp
    xor %ebp, %ebp
    push %edx                       # multiboot info pointer
    push %esi                       # multiboot magic
    call kmain
1:  cli
    hlt
    jmp 1b

# void gdt_flush(struct gdt_ptr *p)
.global gdt_flush
gdt_flush:
    mov 4(%esp), %eax
    lgdt (%eax)
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    pushl $0x08
    pushl $.Lflushed
    lret
.Lflushed:
    ret

# void switch_context(u32 *old_sp, u32 new_sp)
# Saves callee-saved registers on the current kernel stack, swaps stacks.
.global switch_context
switch_context:
    mov 4(%esp), %eax
    mov 8(%esp), %edx
    push %ebp
    push %ebx
    push %esi
    push %edi
    mov %esp, (%eax)
    mov %edx, %esp
    pop %edi
    pop %esi
    pop %ebx
    pop %ebp
    ret
