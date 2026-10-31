# Petrichor entry point.  Loaded by the Petrichor bootloader or any Multiboot loader (QEMU -kernel) in
# 32-bit protected mode; this stub switches the CPU to 64-bit long mode and calls kmain().
#
# The multiboot header uses the "a.out kludge" (flag bit 16): the loader copies this file, as is,
# to _kernel_start and jumps to entry_addr.  The Petrichor bootloader reads entry_addr at offset 28.

.set MB_MAGIC, 0x1BADB002
.set MB_FLAGS, 0x00010003          # page-align modules, provide memory info, addresses below are valid

.section .multiboot, "a"
.align 4
mb_header:
.long MB_MAGIC
.long MB_FLAGS
.long -(MB_MAGIC + MB_FLAGS)
.long mb_header                    # header_addr
.long _kernel_start                # load_addr
.long 0                            # load_end_addr: the whole file
.long _end                         # bss_end_addr: keeps the loader's info block out of our .bss
.long _start                       # entry_addr (read by the Petrichor bootloader at offset 28)

.section .bss
.align 4096
boot_pml4: .skip 4096              # identity map of the first 1 GiB in 2 MiB pages, until mem_init builds the real tables
boot_pdpt: .skip 4096
boot_pd:   .skip 4096
.align 16
.global boot_stack_bottom
boot_stack_bottom:
.skip 16384
boot_stack_top:

.section .rodata
.align 8
boot_gdt:
.quad 0
.quad 0x00209A0000000000           # 0x08: 64-bit code, ring 0
.quad 0x0000920000000000           # 0x10: data
boot_gdt_ptr:
.word 23
.long boot_gdt

msg_no64: .asciz "Petrichor needs a 64-bit (x86_64) processor."

.section .text
.code32
.global _start
_start:
    cli
    mov %eax, %esi                  # keep the loader's magic and info pointer
    mov %ebx, %ebp
    mov $__bss_start, %edi          # a raw loader doesn't clear .bss for us
    mov $_end, %ecx
    sub %edi, %ecx
    xor %eax, %eax
    cld
    rep stosb
    mov $boot_stack_top, %esp

    mov $0x80000000, %eax           # does this CPU have long mode?
    cpuid
    cmp $0x80000001, %eax
    jb .Lno64
    mov $0x80000001, %eax
    cpuid
    test $0x20000000, %edx
    jz .Lno64

    mov $boot_pdpt, %eax
    or $3, %eax
    mov %eax, boot_pml4
    mov $boot_pd, %eax
    or $3, %eax
    mov %eax, boot_pdpt
    xor %ecx, %ecx
1:  mov %ecx, %eax
    shl $21, %eax
    or $0x83, %eax                  # present, writable, 2 MiB page
    mov %eax, boot_pd(,%ecx,8)
    inc %ecx
    cmp $512, %ecx
    jb 1b

    mov $boot_pml4, %eax
    mov %eax, %cr3
    mov %cr4, %eax
    or $0x20, %eax                  # PAE
    mov %eax, %cr4
    mov $0xC0000080, %ecx           # EFER.LME
    rdmsr
    or $0x100, %eax
    wrmsr
    mov %cr0, %eax
    or $0x80000000, %eax            # paging on: we are now in compatibility mode
    mov %eax, %cr0
    lgdt boot_gdt_ptr
    ljmp $0x08, $long_entry

.Lno64:
    mov $msg_no64, %esi
    mov $0xB8000, %edi
    mov $0x4F, %ah
2:  lodsb
    test %al, %al
    jz 3f
    stosw
    jmp 2b
3:  cli
    hlt
    jmp 3b

.code64
long_entry:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %eax, %eax
    mov %ax, %fs
    mov %ax, %gs
    mov $boot_stack_top, %esp
    mov %esi, %edi                  # kmain(magic, mbi)
    mov %ebp, %esi
    xor %ebp, %ebp
    call kmain
1:  cli
    hlt
    jmp 1b

# void gdt_flush(struct gdt_ptr *p)
.global gdt_flush
gdt_flush:
    lgdt (%rdi)
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %ss
    xor %eax, %eax
    mov %ax, %fs
    mov %ax, %gs
    pushq $0x08
    lea .Lflushed(%rip), %rax
    pushq %rax
    lretq
.Lflushed:
    ret

# void switch_context(u64 *old_sp, u64 new_sp)
# Saves callee-saved registers on the current kernel stack, swaps stacks.
.global switch_context
switch_context:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, (%rdi)
    mov %rsi, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
