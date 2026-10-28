ENTRY(_start)

PHDRS {
    image PT_LOAD FLAGS(7);
}

SECTIONS {
    . = 0x40000000;

    .text : ALIGN(4096) {
        *(.text .text.*)
    } :image

    .rodata : ALIGN(16) {
        *(.rodata .rodata.*)
    } :image

    .data : ALIGN(16) {
        *(.data .data.*)
    } :image

    .bss : ALIGN(16) {
        *(COMMON)
        *(.bss .bss.*)
    } :image

    /DISCARD/ : {
        *(.eh_frame*)
        *(.comment)
        *(.note*)
    }
}
