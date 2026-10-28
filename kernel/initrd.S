# The root filesystem image (built by tools/mkinitrd) is linked into the kernel.
.section .initrd, "a"
.align 16
.global _initrd_start
_initrd_start:
.incbin "initrd.img"
.global _initrd_end
_initrd_end:
