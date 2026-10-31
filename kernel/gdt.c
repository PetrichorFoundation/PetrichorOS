/* Long-mode segmentation: flat kernel/user code and data, and one 64-bit TSS (only used for rsp0). */
#include "kernel.h"

struct gdt_entry {
    u16 limit_lo, base_lo;
    u8 base_mid, access, gran, base_hi;
} __attribute__((packed));

struct gdt_ptr {
    u16 limit;
    u64 base;
} __attribute__((packed));

struct tss {
    u32 res0;
    u64 rsp0, rsp1, rsp2;
    u64 res1;
    u64 ist[7];
    u64 res2;
    u16 res3, iomap;
} __attribute__((packed));

static struct gdt_entry gdt[7];                  /* the TSS descriptor takes two slots */
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
    gate(1, 0, 0xFFFFF, 0x9A, 0xA0);   /* 0x08 kernel code (long mode) */
    gate(2, 0, 0xFFFFF, 0x92, 0xC0);   /* 0x10 kernel data */
    gate(3, 0, 0xFFFFF, 0xF2, 0xC0);   /* 0x18 user data   */
    gate(4, 0, 0xFFFFF, 0xFA, 0xA0);   /* 0x20 user code (long mode) */
    memset(&tss, 0, sizeof tss);
    tss.iomap = sizeof tss;
    u64 base = (u64)(uptr)&tss;
    gate(5, (u32)base, sizeof tss - 1, 0x89, 0x00);       /* 0x28 TSS: a 16-byte descriptor */
    memset(&gdt[6], 0, sizeof gdt[6]);
    *(u32 *)&gdt[6] = (u32)(base >> 32);

    gp.limit = sizeof gdt - 1;
    gp.base = (u64)(uptr)gdt;
    gdt_flush(&gp);
    __asm__ volatile("ltr %%ax" : : "a"(0x28));
}

void tss_set_kstack(uptr sp)
{
    tss.rsp0 = sp;
}
