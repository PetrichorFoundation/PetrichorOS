/* IDT, 8259 PIC, PIT, and the common trap dispatcher. */
#include "kernel.h"

struct idt_entry {
    u16 off_lo, sel;
    u8 zero, flags;
    u16 off_hi;
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

extern u32 isr_table[];
extern void isr128(void);

static struct idt_entry idt[256];

static void set_gate(int n, u32 handler, u8 flags)
{
    idt[n].off_lo = handler & 0xFFFF;
    idt[n].sel = 0x08;
    idt[n].zero = 0;
    idt[n].flags = flags;
    idt[n].off_hi = handler >> 16;
}

static void pic_init(void)
{
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 32);   outb(0xA1, 40);
    outb(0x21, 4);    outb(0xA1, 2);
    outb(0x21, 1);    outb(0xA1, 1);
    outb(0x21, 0xFB);              /* only the cascade line open */
    outb(0xA1, 0xFF);
}

void irq_unmask(int irq)
{
    u16 port = irq < 8 ? 0x21 : 0xA1;
    outb(port, inb(port) & ~(1 << (irq & 7)));
}

void idt_init(void)
{
    for (int i = 0; i < 48; i++)
        set_gate(i, isr_table[i], 0x8E);
    set_gate(128, (u32)isr128, 0xEE);   /* int 0x80: callable from ring 3 */

    struct idt_ptr ip = {sizeof idt - 1, (u32)idt};
    __asm__ volatile("lidt %0" : : "m"(ip));
    pic_init();
}

void timer_init(u32 hz)
{
    u32 div = 1193182 / hz;
    outb(0x43, 0x36);
    outb(0x40, div & 0xFF);
    outb(0x40, div >> 8);
    irq_unmask(0);
}

static const char *exc_names[20] = {
    "divide error", "debug", "NMI", "breakpoint", "overflow", "bound range",
    "invalid opcode", "device not available", "double fault", "coprocessor overrun",
    "invalid TSS", "segment not present", "stack fault", "general protection fault",
    "page fault", "reserved", "x87 FPU error", "alignment check", "machine check",
    "SIMD FP exception",
};

static void exception(struct regs *r)
{
    const char *name = r->int_no < 20 ? exc_names[r->int_no] : "unknown exception";
    if ((r->cs & 3) == 3 && r->int_no == 14) {                  /* the stack grows downwards on demand */
        u32 lo = USER_TOP - USER_STACK_MAX * PAGE_SIZE, hi = USER_TOP - USER_STACK_PAGES * PAGE_SIZE;
        u32 a = read_cr2() & ~0xFFFu;
        if (a >= lo && a < hi) {
            u32 va;
            for (va = a; va < hi; va += PAGE_SIZE)
                if (vmm_alloc_page(cur->pd, va) < 0)
                    break;
            if (va >= hi)
                return;
        }
    }
    if ((r->cs & 3) == 3) {
        kprintf("[pid %d %s] killed: %s at eip=%p", cur->pid, cur->name, name, r->eip);
        if (r->int_no == 14)
            kprintf(" (address %p)", read_cr2());
        kprintf("\n");
        proc_exit(r->int_no == 14 ? 139 : 134);
    }
    panic("%s in kernel at eip=%p err=%x cr2=%p", name, r->eip, r->err_code, read_cr2());
}

static void irq(struct regs *r)
{
    int n = r->int_no - 32;
    if (n >= 8)
        outb(0xA0, 0x20);
    outb(0x20, 0x20);              /* EOI first: handlers may context switch */
    switch (n) {
    case 0: timer_tick(r); break;
    case 1: kbd_irq(); break;
    case 4: serial_irq(); break;
    case 12: mouse_irq(); break;
    default:
        if (n == nic_irq_line)
            nic_irq();
        break;
    }
}

void trap_handler(struct regs *r)
{
    if (r->int_no == 128)
        r->eax = syscall_dispatch(r);
    else if (r->int_no >= 32 && r->int_no < 48)
        irq(r);
    else
        exception(r);

    if ((r->cs & 3) == 3 && cur->killed)
        proc_exit(137);
}
