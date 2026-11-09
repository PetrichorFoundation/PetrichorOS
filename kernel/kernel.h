#pragma once
#include "types.h"
#include "abi.h"
#include "string.h"
#include "format.h"

#define KSTACK_SIZE      8192
#define NPROC            64
#define NOFILE           32
#define NFILE            256
#define PAGE_SIZE        4096u
#define USER_BASE        0x40000000u
#define USER_TOP         0xC0000000u
#define USER_HEAP_MAX    0x7F000000u
#define USER_STACK_PAGES 32              /* mapped at exec; the stack grows on demand up to USER_STACK_MAX */
#define USER_STACK_MAX   512
#define TICK_HZ          100
#define MAXARG           32
#define PIPE_SIZE        4096

#define PTE_P  0x1ull
#define PTE_RW 0x2ull
#define PTE_US 0x4ull
#define PTE_PS 0x80ull          /* 2 MiB page (in a page directory entry) */
#define PTE_SHARED 0x200ull       /* user page backed by device memory: never copied or freed */

#define DEV_CONSOLE 1
#define DEV_NULL    2
#define DEV_ZERO    3
#define DEV_RANDOM  4
#define DEV_INPUT   5
#define DEV_DISK    6   /* raw disk; minor = drive index */
#define DEV_KIMG    7   /* read-only: the kernel image exactly as it was loaded from the boot medium */
#define NBLK        8   /* block devices (disks) */

/* Register snapshot built by isr_common (see isr.S). */
struct regs {
    u64 r15, r14, r13, r12, r11, r10, r9, r8, rdi, rsi, rbp, rbx, rdx, rcx, rax;
    u64 int_no, err_code;
    u64 rip, cs, rflags, rsp, ss;
};

/* ---- port I/O and CPU helpers ---- */
static inline void outb(u16 port, u8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outw(u16 port, u16 v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline void outl(u16 port, u32 v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port)); }
static inline u32 inl(u16 port) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline u16 inw(u16 port) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }
static inline u64 read_cr2(void) { u64 v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }

/* ---- filesystem objects ---- */
struct dent {
    char name[NAME_MAX + 1];
    struct inode *ip;
};

struct inode {
    u32 type;
    u32 ino;
    int nlink;                 /* directory entries naming this inode */
    int refs;                  /* open files + working directories */
    u32 size, cap;             /* files */
    u8 *data;
    struct dent *ents;         /* directories (includes "." and "..") */
    u32 nents, cap_ents;
    int major, minor;          /* devices */
    struct inode *parent;      /* directories (RAM filesystem) */

    /* mounts */
    struct inode *mounted;     /* on a directory: root of the filesystem mounted here */
    struct inode *mount_of;    /* on a mounted root: the directory it covers */

    /* disk-backed (PFS) inodes */
    int pfs;
    u32 dino;
    u32 dblk[12], iblk, i2blk;
    struct inode *pnext;
};

struct pipe {
    u8 buf[PIPE_SIZE];
    u32 nread, nwrite;         /* free-running counters */
    int readopen, writeopen;
};

enum { FD_NONE, FD_INODE, FD_PIPE, FD_SOCK };

struct sock;

struct file {
    int type;
    int refs;
    int readable, writable, append;
    struct inode *ip;
    struct pipe *pipe;
    struct sock *sock;
    u32 off;
};

/* ---- processes ---- */
struct proc {
    u8 fpu[512] __attribute__((aligned(16)));    /* x87/SSE state (fxsave format), switched with the process */
    int pid, ppid, pgid;
    int state;
    u64 *pd;
    u32 brk, brk_start;
    u8 *kstack;
    struct regs *tf;
    uptr ctx_sp;
    void *chan;
    u32 wake_tick;
    int killed, exit_status;
    struct file *ofile[NOFILE];
    struct inode *cwd;
    char name[16];
    u32 start_tick, cpu_ticks;
    u32 shm_mask;                                /* shared-memory buffers mapped (shm.c), bit = id */
    int thread;                                  /* a thread made by clone: shares pd with its process (vm.c) */
    int tgid;                                    /* the process (thread group) this belongs to */
    u32 clear_tid;                               /* user address zeroed and futex-woken when the thread exits */
    u64 fsbase;                                  /* thread-local storage pointer */
};

extern struct proc ptable[NPROC];
extern struct proc *cur;
extern volatile u32 ticks;
extern int tty_fg_pgrp;

/* What the bootloader told us (main.c). */
struct bootinfo {
    u32 kimg, kimg_size;                         /* pristine copy of the boot image kernel, for the installer */
    u32 fb_addr, fb_pitch, fb_w, fb_h, fb_bpp;   /* VESA linear framebuffer, if the bootloader set one up */
    u32 rsdp;                                    /* ACPI RSDP address when the firmware loader supplied it (UEFI) */
    u32 edid_w, edid_h, edid_mhz;                /* the monitor's preferred mode, read by the bootloader (0 = unknown) */
    u32 uefi;                                    /* started by the UEFI loader, not the BIOS bootloader */
    u32 nranges, range[16][2];                   /* usable RAM above 1 MiB, from the UEFI memory map (else empty) */
};
extern struct bootinfo bootinfo;

/* console.c */
void console_init(void);
void console_putc(char c);
void console_write(const char *s, u32 n);
void console_use_fb(void);                       /* switch to the framebuffer text console (after mem_init) */
void console_suspend(void);                      /* a graphical session owns the screen */
void console_resume(void);
void serial_irq_enable(void);
void serial_irq(void);
int kprintf(const char *fmt, ...);
void panic(const char *fmt, ...) __attribute__((noreturn));

/* gdt.c / idt.c */
void gdt_init(void);
void tss_set_kstack(uptr sp);
void fpu_init(void);
void fpu_reset(u8 *state);                       /* the state of a fresh program */
void idt_init(void);
void irq_unmask(int irq);
void timer_init(u32 hz);
void trap_handler(struct regs *r);
extern void trapret(void);
extern void gdt_flush(void *ptr);
extern void switch_context(uptr *old_sp, uptr new_sp);

/* mem.c */
void mem_init(u32 ram_bytes);
u32 pmm_alloc(void);
void pmm_free(u32 pa);
void pmm_stats(u32 *total_kb, u32 *free_kb);
void *kmalloc(u32 n);
void *kzalloc(u32 n);
void kfree(void *p);
void heap_stats(u32 *total, u32 *free);
u64 *vmm_new_pd(void);                                  /* an address space is a PML4 */
int vmm_map(u64 *pd, u32 va, u32 pa, u64 flags);
u64 vmm_get_pte(u64 *pd, u32 va);
void vmm_unmap(u64 *pd, u32 va);
u64 *vmm_clone_pd(u64 *src);
void vmm_free_pd(u64 *pd);
void vmm_switch(u64 *pd);
u32 vmm_count_pages(u64 *pd);
int vmm_alloc_page(u64 *pd, u32 va);
int copy_to_space(u64 *pd, u32 va, const void *src, u32 len);
int user_ok(u64 addr, u32 len);
extern u64 *kernel_pd;

/* fs.c */
void fs_init(void);
void initrd_load(const u8 *img, u32 size);
extern struct inode *fs_root;
int fs_lookup(const char *path, struct inode **out);
int fs_create(const char *path, u32 type, struct inode **out);
int fs_unlink(const char *path);
int fs_rename(const char *from, const char *to);
int inode_read(struct inode *ip, u32 off, void *dst, u32 n);
int inode_write(struct inode *ip, u32 off, const void *src, u32 n);
void inode_truncate(struct inode *ip);
void iref(struct inode *ip);
void iput(struct inode *ip);
int fs_getcwd(struct inode *ip, char *buf, u32 size);
u32 inode_nents(struct inode *ip);
u32 inode_size(struct inode *ip);                    /* bytes (entries for a directory) */
int inode_dirent(struct inode *ip, u32 idx, struct dirent *de);

/* blk.c: block devices.  Drivers register disks; everything else uses indexes. */
#define BUS_IDE  0
#define BUS_SATA 1
#define BUS_NVME 2
#define BUS_USB  3
#define BUS_VIRTIO 4
typedef int (*blk_rw_fn)(void *priv, u32 lba, u32 n, void *buf, int write);
int storage_init(void);                          /* probe IDE, AHCI, NVMe; find the boot disk */
int blk_register(const char *model, u32 sectors, int bus, blk_rw_fn rw, void *priv);
int blk_present(int dev);
u32 blk_sectors(int dev);                        /* 512-byte sectors */
const char *blk_model(int dev);
const char *blk_name(int dev);                   /* "hda".. */
int blk_bus(int dev);
int blk_read(int dev, u32 lba, u32 n, void *buf);
int blk_write(int dev, u32 lba, u32 n, const void *buf);
extern int boot_disk;
extern int live_disk;                                            /* the install USB stick / disk we are running from, or -1 */
extern u32 pfs_base_lba, pfs_sectors;                            /* index of the disk we booted from, or -1 */
int ide_probe(void);
int ahci_probe(void);
int nvme_probe(void);
int virtio_blk_probe(void);
int usb_init(void);                                              /* xHCI: keyboards, mice, USB disks */
void usb_tick(void);                                             /* timer tick: poll USB input */
void kbd_scancode(u8 sc);                                        /* feed one PC (set 1) scancode */

/* PCI (pci.c) and device memory (mem.c) */
struct pcidev {
    u32 bus, slot, fn;
};
int pci_find_class(u32 class24, int index, struct pcidev *out);   /* class<<16 | subclass<<8 | progif */
u32 pci_cfg_read(const struct pcidev *d, u32 off);
void pci_cfg_write(const struct pcidev *d, u32 off, u32 v);
void pci_enable(const struct pcidev *d);                         /* memory space + bus mastering */
void *mmio_map(u64 phys, u32 size);                              /* uncached kernel mapping of device memory */
void *kalloc_aligned(u32 size, u32 align);                       /* physically contiguous (identity-mapped heap) */

/* pfs.c: on-disk filesystem */
int pfs_mount(struct inode *mountpoint);
struct inode *pfs_lookup(struct inode *dir, const char *name);
int pfs_create(struct inode *dir, const char *name, u32 type, struct inode **out);
int pfs_unlink_entry(struct inode *dir, const char *name);
int pfs_rename(struct inode *op, const char *oname, struct inode *np, const char *nname);
int pfs_read(struct inode *ip, u32 off, void *dst, u32 n);
int pfs_write(struct inode *ip, u32 off, const void *src, u32 n);
void pfs_truncate(struct inode *ip);
void pfs_destroy(struct inode *ip);
u32 pfs_nents(struct inode *dir);
int pfs_dirent(struct inode *dir, u32 idx, struct dirent *de);
int pfs_name_of(struct inode *dir, u32 dino, char *out);
void pfs_stats(u32 *total_kb, u32 *free_kb);

/* nic.c: network card drivers (rtl8139.c, e1000.c, virtio_net.c) behind one interface */
struct nic_driver {
    const char *name;
    int (*init)(void);                           /* 0 if a card of this kind was found and started */
    int (*send)(const u8 *frame, u32 len);
    void (*irq)(void);
    void (*poll)(void);                          /* also called every tick: receive without relying on the interrupt */
};
int nic_init(void);
int nic_send(const u8 *frame, u32 len);
void nic_irq(void);
void nic_poll(void);
extern u8 nic_mac[6];
extern int nic_irq_line;
extern const char *nic_name;
int rtl8168_init(void);
int rtl8168_send(const u8 *frame, u32 len);
void rtl8168_irq(void);
void rtl8168_poll(void);
int rtl8139_init(void);
int rtl8139_send(const u8 *frame, u32 len);
void rtl8139_irq(void);
int e1000_init(void);
int e1000_send(const u8 *frame, u32 len);
void e1000_irq(void);
void e1000_poll(void);
int virtio_net_init(void);
int virtio_net_send(const u8 *frame, u32 len);
void virtio_net_irq(void);
void virtio_net_poll(void);

/* net.c: Ethernet / ARP / IPv4 / ICMP / UDP / TCP and sockets */
void net_init(void);
void net_rx(const u8 *frame, u32 len);
void net_tick(void);
void net_info(struct netinfo *ni);
int net_ping(u32 ip, u32 timeout_ms);
int sock_create(int type, struct sock **out);
int sock_connect(struct sock *s, u32 ip, u16 port);
int sock_bind(struct sock *s, u16 port);
int sock_read(struct sock *s, void *dst, u32 n);
int sock_write(struct sock *s, const void *src, u32 n);
int sock_sendto(struct sock *s, const void *buf, u32 n, u32 ip, u16 port);
int sock_recvfrom(struct sock *s, void *buf, u32 n, struct sockaddr_in *from);
void sock_close(struct sock *s);
void sock_settimeout(struct sock *s, u32 ms);
int sock_ready(struct sock *s);

/* file.c */
struct file *file_alloc(void);
struct file *file_dup(struct file *f);
void file_close(struct file *f);
int file_read(struct file *f, void *dst, u32 n);
int file_write(struct file *f, const void *src, u32 n);
int file_stat(struct file *f, struct stat *st);
int file_readdir(struct file *f, struct dirent *de);
int file_seek(struct file *f, int off, int whence);
int file_ready(struct file *f);
int pipe_create(struct file **rf, struct file **wf);

/* input.c: raw keyboard + mouse events for /dev/input */
void input_push(u8 type, u8 flags, u16 code, i16 dx, i16 dy);
int input_active(void);
int input_ready(void);
int input_read(void *dst, u32 n);
void input_open(void);
void input_close(void);
void mouse_init(void);
void mouse_irq(void);

/* gfx.c: linear framebuffer (Bochs/QEMU VBE) */
int gfx_map(struct fbinfo *out, u32 w, u32 h);
void gfx_off(void);
extern int gfx_owner;
u32 pci_read(u32 bus, u32 slot, u32 fn, u32 off);
void pci_write(u32 bus, u32 slot, u32 fn, u32 off, u32 v);

/* tty.c / kbd.c */
int tty_ready(void);
void tty_input(char c);
int tty_read(char *buf, u32 n);
void random_init(void);
void random_event(u32 v);                       /* stir interrupt timing into the pool */
void random_add(const void *data, u32 n);
void random_bytes(void *dst, u32 n);
void kbd_init(void);
void kbd_irq(void);

/* proc.c */
void proc_init(void);
void proc_spawn_init(const char *path);
void schedule(void);
void yield(void);
int sleep_until(void *chan, u32 deadline);   /* 1 if the deadline passed */
void sleep_on(void *chan);
void wakeup(void *chan);
int proc_fork(struct regs *r);
void proc_exit(int status) __attribute__((noreturn));
int proc_wait(int *status, int flags);
int proc_kill(int pid);
int proc_kill_pgrp(int pgid);
int proc_setpgid(int pid, int pgid);
void timer_tick(struct regs *r);
void idle_loop(void) __attribute__((noreturn));

/* exec.c */
int exec_into(struct proc *p, struct inode *ip, int argc, char **argv);

/* acpi.c */
void acpi_poweroff(void);
void acpi_reset(void);

/* shm.c */
long sys_shm(int op, u32 a);
void shm_fork(struct proc *child);
void shm_release_all(struct proc *p);

/* vm.c: mmap and threads */
#define MMAP_BASE 0xAF000000u                    /* between the shared-memory slots and the stack */
#define MMAP_TOP  0xBF000000u
long sys_mmap(u32 len, u32 prot);
long sys_munmap(u32 addr, u32 len);
long sys_mprotect(u32 addr, u32 len, u32 prot);
long sys_clone(struct regs *r, u32 entry, u32 stack, u32 arg, u32 ctid);
long sys_futex(u32 addr, int op, u32 val, u32 ms);
int pd_users(u64 *pd, struct proc *except);      /* live threads besides `except` using this address space */
void kill_sharers(u64 *pd, struct proc *except);
void thread_exit_notify(struct proc *p);
void set_fsbase(u64 v);
int vmm_set_rw(u64 *pd, u32 va, int rw);

/* syscall.c */
int syscall_dispatch(struct regs *r);
