#pragma once
#include "types.h"
#include "abi.h"
#include "string.h"
#include "format.h"

#define KSTACK_SIZE      8192
#define NPROC            64
#define NOFILE           16
#define NFILE            128
#define PAGE_SIZE        4096u
#define USER_BASE        0x40000000u
#define USER_TOP         0xC0000000u
#define USER_HEAP_MAX    0x7F000000u
#define USER_STACK_PAGES 32
#define TICK_HZ          100
#define MAXARG           32
#define PIPE_SIZE        4096

#define PTE_P  0x1u
#define PTE_RW 0x2u
#define PTE_US 0x4u

#define DEV_CONSOLE 1
#define DEV_NULL    2
#define DEV_ZERO    3
#define DEV_RANDOM  4

/* Register snapshot built by isr_common (see isr.S). */
struct regs {
    u32 edi, esi, ebp, esp_dummy, ebx, edx, ecx, eax;
    u32 ds, es, fs, gs;
    u32 int_no, err_code;
    u32 eip, cs, eflags, useresp, ss;
};

/* ---- port I/O and CPU helpers ---- */
static inline void outb(u16 port, u8 v) { __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port)); }
static inline u8 inb(u16 port) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outw(u16 port, u16 v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port)); }
static inline void cli(void) { __asm__ volatile("cli"); }
static inline void sti(void) { __asm__ volatile("sti"); }
static inline u32 read_cr2(void) { u32 v; __asm__ volatile("mov %%cr2, %0" : "=r"(v)); return v; }

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
    int major;                 /* devices */
    struct inode *parent;      /* directories */
};

struct pipe {
    u8 buf[PIPE_SIZE];
    u32 nread, nwrite;         /* free-running counters */
    int readopen, writeopen;
};

enum { FD_NONE, FD_INODE, FD_PIPE };

struct file {
    int type;
    int refs;
    int readable, writable, append;
    struct inode *ip;
    struct pipe *pipe;
    u32 off;
};

/* ---- processes ---- */
struct proc {
    int pid, ppid, pgid;
    int state;
    u32 *pd;
    u32 brk, brk_start;
    u8 *kstack;
    struct regs *tf;
    u32 ctx_sp;
    void *chan;
    u32 wake_tick;
    int killed, exit_status;
    struct file *ofile[NOFILE];
    struct inode *cwd;
    char name[16];
    u32 start_tick, cpu_ticks;
};

extern struct proc ptable[NPROC];
extern struct proc *cur;
extern volatile u32 ticks;
extern int tty_fg_pgrp;

/* console.c */
void console_init(void);
void console_putc(char c);
void console_write(const char *s, u32 n);
void serial_irq_enable(void);
void serial_irq(void);
int kprintf(const char *fmt, ...);
void panic(const char *fmt, ...) __attribute__((noreturn));

/* gdt.c / idt.c */
void gdt_init(void);
void tss_set_kstack(u32 sp);
void idt_init(void);
void irq_unmask(int irq);
void timer_init(u32 hz);
void trap_handler(struct regs *r);
extern void trapret(void);
extern void gdt_flush(void *ptr);
extern void switch_context(u32 *old_sp, u32 new_sp);

/* mem.c */
void mem_init(u32 ram_bytes);
u32 pmm_alloc(void);
void pmm_free(u32 pa);
void pmm_stats(u32 *total_kb, u32 *free_kb);
void *kmalloc(u32 n);
void *kzalloc(u32 n);
void kfree(void *p);
void heap_stats(u32 *total, u32 *free);
u32 *vmm_new_pd(void);
int vmm_map(u32 *pd, u32 va, u32 pa, u32 flags);
u32 vmm_get_pte(u32 *pd, u32 va);
void vmm_unmap(u32 *pd, u32 va);
u32 *vmm_clone_pd(u32 *src);
void vmm_free_pd(u32 *pd);
void vmm_switch(u32 *pd);
u32 vmm_count_pages(u32 *pd);
int vmm_alloc_page(u32 *pd, u32 va);
int copy_to_space(u32 *pd, u32 va, const void *src, u32 len);
int user_ok(u32 addr, u32 len);
extern u32 *kernel_pd;

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

/* file.c */
struct file *file_alloc(void);
struct file *file_dup(struct file *f);
void file_close(struct file *f);
int file_read(struct file *f, void *dst, u32 n);
int file_write(struct file *f, const void *src, u32 n);
int file_stat(struct file *f, struct stat *st);
int file_readdir(struct file *f, struct dirent *de);
int file_seek(struct file *f, int off, int whence);
int pipe_create(struct file **rf, struct file **wf);

/* tty.c / kbd.c */
void tty_input(char c);
int tty_read(char *buf, u32 n);
void kbd_init(void);
void kbd_irq(void);

/* proc.c */
void proc_init(void);
void proc_spawn_init(const char *path);
void schedule(void);
void yield(void);
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

/* syscall.c */
int syscall_dispatch(struct regs *r);
