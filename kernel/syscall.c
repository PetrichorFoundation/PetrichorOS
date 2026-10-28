/* System call layer: argument validation plus thin wrappers over the kernel. */
#include "kernel.h"

/* Copies a NUL-terminated user string; returns its length or -errno. */
static int copy_in_str(u32 uaddr, char *dst, u32 max)
{
    for (u32 i = 0; i < max; i++) {
        if (i == 0 || ((uaddr + i) & 0xFFF) == 0)
            if (!user_ok(uaddr + i, 1))
                return -EFAULT;
        dst[i] = *(char *)(uaddr + i);
        if (!dst[i])
            return i;
    }
    return -ENAMETOOLONG;
}

static int fd_alloc(struct file *f)
{
    for (int i = 0; i < NOFILE; i++) {
        if (!cur->ofile[i]) {
            cur->ofile[i] = f;
            return i;
        }
    }
    return -EMFILE;
}

static struct file *fd_get(u32 fd)
{
    return fd < NOFILE ? cur->ofile[fd] : NULL;
}

static int sys_open(const char *path, int flags)
{
    struct inode *ip;
    int e = fs_lookup(path, &ip);
    if (e == -ENOENT && (flags & O_CREAT))
        e = fs_create(path, T_FILE, &ip);
    if (e < 0)
        return e;

    int acc = flags & 3;
    if (ip->type == T_DIR && acc != O_RDONLY)
        return -EISDIR;
    if (ip->type == T_FILE && (flags & O_TRUNC) && acc != O_RDONLY)
        inode_truncate(ip);

    struct file *f = file_alloc();
    if (!f)
        return -ENOMEM;
    f->type = FD_INODE;
    f->ip = ip;
    iref(ip);
    f->readable = acc != O_WRONLY;
    f->writable = acc != O_RDONLY;
    f->append = !!(flags & O_APPEND);
    int fd = fd_alloc(f);
    if (fd < 0)
        file_close(f);
    return fd;
}

static int sys_pipe(u32 uaddr)
{
    if (!user_ok(uaddr, 8))
        return -EFAULT;
    struct file *rf, *wf;
    int e = pipe_create(&rf, &wf);
    if (e < 0)
        return e;
    int rfd = fd_alloc(rf), wfd = rfd < 0 ? -1 : fd_alloc(wf);
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0)
            cur->ofile[rfd] = NULL;
        file_close(rf);
        file_close(wf);
        return -EMFILE;
    }
    int *fds = (int *)uaddr;
    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

static int sys_sbrk(int n)
{
    u32 old = cur->brk, new = old + n;
    if (n > 0) {
        if (new < old || new > USER_HEAP_MAX)
            return -ENOMEM;
        for (u32 va = (old + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); va < new; va += PAGE_SIZE)
            if (vmm_alloc_page(cur->pd, va) < 0)
                return -ENOMEM;
    } else if (n < 0) {
        if (new > old || new < cur->brk_start)
            return -EINVAL;
        u32 lo = (new + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        u32 hi = (old + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        for (u32 va = lo; va < hi; va += PAGE_SIZE)
            vmm_unmap(cur->pd, va);
    }
    cur->brk = new;
    return old;
}

static int sys_sleep(u32 ms)
{
    u32 target = ticks + (ms + 9) / 10;
    while (ticks < target) {
        if (cur->killed)
            return -EINTR;
        cur->wake_tick = target;
        sleep_on((void *)&ticks);
    }
    return 0;
}

static int sys_exec(u32 upath, u32 uargv)
{
    static char argbuf[4096];
    static char *kargv[MAXARG + 1];
    char path[PATH_MAX];
    int e = copy_in_str(upath, path, sizeof path);
    if (e < 0)
        return e;

    int argc = 0;
    u32 used = 0;
    for (;; argc++) {
        if (!user_ok(uargv + argc * 4, 4))
            return -EFAULT;
        u32 sp = ((u32 *)uargv)[argc];
        if (!sp)
            break;
        if (argc == MAXARG)
            return -E2BIG;
        e = copy_in_str(sp, argbuf + used, sizeof argbuf - used);
        if (e < 0)
            return e == -ENAMETOOLONG ? -E2BIG : e;
        kargv[argc] = argbuf + used;
        used += e + 1;
    }
    kargv[argc] = NULL;
    if (argc == 0)
        return -EINVAL;

    struct inode *ip;
    e = fs_lookup(path, &ip);
    if (e < 0)
        return e;
    return exec_into(cur, ip, argc, kargv);
}

static int sys_ps(u32 ubuf, int max)
{
    if (max < 0 || !user_ok(ubuf, max * sizeof(struct procinfo)))
        return -EFAULT;
    struct procinfo *out = (struct procinfo *)ubuf;
    int n = 0;
    for (int i = 0; i < NPROC && n < max; i++) {
        struct proc *p = &ptable[i];
        if (p->state == PS_UNUSED)
            continue;
        out[n].pid = p->pid;
        out[n].ppid = p->ppid;
        out[n].state = p->state;
        out[n].mem_kb = p->pd ? vmm_count_pages(p->pd) * (PAGE_SIZE / 1024) : 0;
        out[n].ticks = p->cpu_ticks;
        safestrcpy(out[n].name, p->name, sizeof out[n].name);
        n++;
    }
    return n;
}

static int sys_shutdown(int mode)
{
    kprintf("\n[sys ] %s...\n", mode == SHUT_REBOOT ? "rebooting" : "powering off");
    cli();
    if (mode == SHUT_REBOOT) {
        outb(0x64, 0xFE);                       /* pulse the CPU reset line */
    } else {
        outw(0x604, 0x2000);                    /* QEMU */
        outw(0xB004, 0x2000);                   /* Bochs / old QEMU */
    }
    kprintf("[sys ] it is now safe to turn off your computer.\n");
    for (;;)
        __asm__ volatile("hlt");
}

int syscall_dispatch(struct regs *r)
{
    u32 a = r->ebx, b = r->ecx, c = r->edx;
    char path[PATH_MAX], path2[PATH_MAX];
    struct file *f;
    int e;

    switch (r->eax) {
    case SYS_exit:
        proc_exit(a);
    case SYS_fork:
        return proc_fork(r);
    case SYS_read:
        if (!(f = fd_get(a)))
            return -EBADF;
        if (!user_ok(b, c))
            return -EFAULT;
        return file_read(f, (void *)b, c);
    case SYS_write:
        if (!(f = fd_get(a)))
            return -EBADF;
        if (!user_ok(b, c))
            return -EFAULT;
        return file_write(f, (void *)b, c);
    case SYS_open:
        if ((e = copy_in_str(a, path, sizeof path)) < 0)
            return e;
        return sys_open(path, b);
    case SYS_close:
        if (!(f = fd_get(a)))
            return -EBADF;
        cur->ofile[a] = NULL;
        file_close(f);
        return 0;
    case SYS_wait: {
        int status = 0;
        int pid = proc_wait(&status, b);
        if (pid > 0 && a) {
            if (!user_ok(a, 4))
                return -EFAULT;
            *(int *)a = status;
        }
        return pid;
    }
    case SYS_exec:
        return sys_exec(a, b);
    case SYS_getpid:
        return cur->pid;
    case SYS_sbrk:
        return sys_sbrk((int)a);
    case SYS_sleep:
        return sys_sleep(a);
    case SYS_pipe:
        return sys_pipe(a);
    case SYS_dup:
        if (!(f = fd_get(a)))
            return -EBADF;
        if ((e = fd_alloc(f)) < 0)
            return e;
        file_dup(f);
        return e;
    case SYS_dup2:
        if (!(f = fd_get(a)))
            return -EBADF;
        if (b >= NOFILE)
            return -EBADF;
        if (a == b)
            return b;
        if (cur->ofile[b])
            file_close(cur->ofile[b]);
        cur->ofile[b] = file_dup(f);
        return b;
    case SYS_chdir: {
        struct inode *ip;
        if ((e = copy_in_str(a, path, sizeof path)) < 0 || (e = fs_lookup(path, &ip)) < 0)
            return e;
        if (ip->type != T_DIR)
            return -ENOTDIR;
        iref(ip);
        iput(cur->cwd);
        cur->cwd = ip;
        return 0;
    }
    case SYS_getcwd:
        if (!user_ok(a, b))
            return -EFAULT;
        if ((e = fs_getcwd(cur->cwd, path, sizeof path)) < 0)
            return e;
        if (strlen(path) + 1 > b)
            return -ENAMETOOLONG;
        strcpy((char *)a, path);
        return 0;
    case SYS_mkdir:
        if ((e = copy_in_str(a, path, sizeof path)) < 0)
            return e;
        return fs_create(path, T_DIR, NULL);
    case SYS_unlink:
        if ((e = copy_in_str(a, path, sizeof path)) < 0)
            return e;
        return fs_unlink(path);
    case SYS_fstat:
        if (!(f = fd_get(a)))
            return -EBADF;
        if (!user_ok(b, sizeof(struct stat)))
            return -EFAULT;
        return file_stat(f, (struct stat *)b);
    case SYS_stat: {
        struct inode *ip;
        if ((e = copy_in_str(a, path, sizeof path)) < 0 || (e = fs_lookup(path, &ip)) < 0)
            return e;
        if (!user_ok(b, sizeof(struct stat)))
            return -EFAULT;
        struct stat *st = (struct stat *)b;
        st->type = ip->type;
        st->size = ip->type == T_DIR ? ip->nents : ip->size;
        st->nlink = ip->nlink;
        st->ino = ip->ino;
        return 0;
    }
    case SYS_readdir:
        if (!(f = fd_get(a)))
            return -EBADF;
        if (!user_ok(b, sizeof(struct dirent)))
            return -EFAULT;
        return file_readdir(f, (struct dirent *)b);
    case SYS_kill:
        return proc_kill(a);
    case SYS_uptime:
        return ticks;
    case SYS_lseek:
        if (!(f = fd_get(a)))
            return -EBADF;
        return file_seek(f, (int)b, c);
    case SYS_getppid:
        return cur->ppid;
    case SYS_ps:
        return sys_ps(a, b);
    case SYS_rename:
        if ((e = copy_in_str(a, path, sizeof path)) < 0 ||
            (e = copy_in_str(b, path2, sizeof path2)) < 0)
            return e;
        return fs_rename(path, path2);
    case SYS_setpgid:
        return proc_setpgid(a, b);
    case SYS_getpgid:
        if (a == 0)
            return cur->pgid;
        for (int i = 1; i < NPROC; i++)
            if (ptable[i].pid == (int)a && ptable[i].state != PS_UNUSED)
                return ptable[i].pgid;
        return -ESRCH;
    case SYS_tcsetpgrp:
        tty_fg_pgrp = a;
        return 0;
    case SYS_meminfo: {
        if (!user_ok(a, sizeof(struct meminfo)))
            return -EFAULT;
        struct meminfo *mi = (struct meminfo *)a;
        u32 ht, hf;
        pmm_stats(&mi->total_kb, &mi->free_kb);
        heap_stats(&ht, &hf);
        mi->heap_total_kb = ht >> 10;
        mi->heap_free_kb = hf >> 10;
        return 0;
    }
    case SYS_shutdown:
        return sys_shutdown(a);
    }
    return -ENOSYS;
}
