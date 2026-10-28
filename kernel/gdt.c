/* Flat segmentation: kernel code/data, user code/data, one TSS. */
#include "kernel.h"

struct gdt_entry {
    u16 limit_lo, base_lo;
    u8 base_mid, access, gran, base_hi;
} __attribute__((packed));

struct gdt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

struct tss {
    u32 prev, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
    u32 eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u32 es, cs, ss, ds, fs, gs, ldt;
    u16 trap, iomap;
} __attribute__((packed));

static struct gdt_entry gdt[6];
static struct gdt_ptr gp;
static struct tss tss;

static void gate(int i, u32 base, u32 limit, u8 access, u8 gran)
{
    gdt[i].limit_lo = limit & 0xFFFF;
    gdt[i].base_lo = base & 0xFFFF;
    gdt[i].base_mid = (base >> 16) & 0xFF;
    gdt[i].access = access;
    gdt[i].gran = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[i].base_hi = (base >> 24) & 0xFF;
}

void gdt_init(void)
{
    gate(0, 0, 0, 0, 0);
    gate(1, 0, 0xFFFFF, 0x9A, 0xC0);   /* 0x08 kernel code */
    gate(2, 0, 0xFFFFF, 0x92, 0xC0);   /* 0x10 kernel data */
    gate(3, 0, 0xFFFFF, 0xFA, 0xC0);   /* 0x18 user code   */
    gate(4, 0, 0xFFFFF, 0xF2, 0xC0);   /* 0x20 user data   */
    gate(5, (u32)&tss, sizeof tss - 1, 0x89, 0x00);

    memset(&tss, 0, sizeof tss);
    tss.ss0 = 0x10;
    tss.iomap = sizeof tss;

    gp.limit = sizeof gdt - 1;
    gp.base = (u32)gdt;
    gdt_flush(&gp);
    __asm__ volatile("ltr %%ax" : : "a"(0x28));
}

void tss_set_kstack(u32 sp)
{
    tss.esp0 = sp;
}
