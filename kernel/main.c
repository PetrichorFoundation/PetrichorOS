#include "kernel.h"

extern const u8 _initrd_start[], _initrd_end[];

static void banner(void)
{
    kprintf("\033[1;36m");
    kprintf("  ___    _       _      _                 ___  ___\n");
    kprintf(" | _ \\__| |_ _ _(_)__| |_  ___ _ _      / _ \\/ __|\n");
    kprintf(" |  _/ -_)  _| '_| / _| ' \\/ _ \\ '_|    | (_) \\__ \\\n");
    kprintf(" |_| \\___|\\__|_| |_\\__|_||_\\___/_|       \\___/|___/\n");
    kprintf("\033[0m");
    kprintf(" %s %s - a from-scratch Unix for i386\n\n", PETRICHOR_NAME, PETRICHOR_VERSION);
}

void kmain(u32 magic, u32 *mbi)
{
    console_init();
    banner();

    if (magic != 0x2BADB002)
        panic("not started by a Multiboot loader (magic %x)", magic);

    u32 kb = 64 * 1024;
    if (mbi[0] & 1) {                           /* flags bit 0: mem_lower/mem_upper valid */
        kb = 1024 + mbi[2];
        if (kb > 512 * 1024)
            kb = 512 * 1024;
    }

    gdt_init();
    idt_init();
    mem_init(kb * 1024);
    fs_init();
    initrd_load(_initrd_start, _initrd_end - _initrd_start);
    proc_init();
    kbd_init();
    serial_irq_enable();
    timer_init(TICK_HZ);

    kprintf("[init] starting /bin/init\n\n");
    proc_spawn_init("/bin/init");
    idle_loop();
}
