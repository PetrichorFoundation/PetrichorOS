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

    static char cmdline[128];
    if ((mbi[0] & 4) && mbi[4]) {                   /* flags bit 2: a command line from the loader */
        const char *src = (const char *)mbi[4];
        u32 n = 0;
        while (src[n] && n < sizeof cmdline - 1) {
            cmdline[n] = src[n];
            n++;
        }
        cmdline[n] = 0;
    }

    gdt_init();
    idt_init();
    mem_init(kb * 1024);
    int ndisks = ata_init();
    kprintf("[ata ] %d disk%s\n", ndisks, ndisks == 1 ? "" : "s");
    fs_init();
    initrd_load(_initrd_start, _initrd_end - _initrd_start);
    {
        struct inode *ci;
        if (fs_create("/etc/cmdline", T_FILE, &ci) == 0 && cmdline[0])
            inode_write(ci, 0, cmdline, strlen(cmdline));
        if (cmdline[0])
            kprintf("[boot] command line: %s\n", cmdline);
    }
    {
        struct inode *mp;
        if (strstr(cmdline, "nodisk")) {
            kprintf("[disk] safe mode: not mounting /disk\n");
        } else if (fs_lookup("/disk", &mp) == 0) {
            int e = pfs_mount(mp);
            if (e < 0)
                kprintf("[disk] no usable disk (%d): /disk stays empty\n", e);
        }
    }
    proc_init();
    net_init();
    kbd_init();
    mouse_init();
    serial_irq_enable();
    timer_init(TICK_HZ);

    kprintf("[init] starting /bin/init\n\n");
    proc_spawn_init("/bin/init");
    idle_loop();
}
