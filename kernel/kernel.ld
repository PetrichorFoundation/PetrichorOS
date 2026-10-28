ENTRY(_start)

PHDRS {
    image PT_LOAD FLAGS(7);
}

SECTIONS {
    . = 1M;
    _kernel_start = .;

    .text : ALIGN(4096) {
        KEEP(*(.multiboot))
        *(.text .text.*)
    } :image

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
        *(.isrtab)
    } :image

    .initrd : ALIGN(16) {
        KEEP(*(.initrd))
    } :image

    .data : ALIGN(16) {
        *(.data .data.*)
    } :image

    .bss : ALIGN(16) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
    } :image

    . = ALIGN(4096);
    _end = .;

    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment)
        *(.note*)
    }
}
