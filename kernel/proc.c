/* Processes and the scheduler.
 *
 * The kernel is non-preemptive: it runs with interrupts off, and only user
 * code is preempted by the timer.  A blocked process sleeps on a channel
 * address until someone calls wakeup() on it. */
#include "kernel.h"

struct proc ptable[NPROC];
struct proc *cur;
volatile u32 ticks;
static int next_pid = 1;

void proc_init(void)
{
    memset(ptable, 0, sizeof ptable);
    cur = &ptable[0];                   /* slot 0 is the idle task: the boot stack */
    cur->state = PS_RUNNING;
    safestrcpy(cur->name, "idle", sizeof cur->name);
}

/* Reserves a slot and prepares a kernel stack that "returns" into trapret. */
static struct proc *proc_alloc(void)
{
    for (int i = 1; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->state != PS_UNUSED)
            continue;
        memset(p, 0, sizeof *p);
        p->kstack = kmalloc(KSTACK_SIZE);
        if (!p->kstack)
            return NULL;
        fpu_reset(p->fpu);
        p->pid = next_pid++;
        p->state = PS_EMBRYO;
        p->start_tick = ticks;

        u8 *sp = p->kstack + KSTACK_SIZE;
        sp -= sizeof(struct regs);
        p->tf = (struct regs *)sp;
        sp -= 8;
        *(u64 *)sp = (u64)(uptr)trapret;
        sp -= 48;                       /* r15, r14, r13, r12, rbx, rbp popped by switch_context */
        memset(sp, 0, 48);
        p->ctx_sp = (uptr)sp;
        return p;
    }
    return NULL;
}

static void proc_discard(struct proc *p)
{
    if (p->pd)
        vmm_free_pd(p->pd);
    kfree(p->kstack);
    p->kstack = NULL;
    p->pd = NULL;
    p->state = PS_UNUSED;
}

void proc_spawn_init(const char *path)
{
    struct inode *ip;
    if (fs_lookup(path, &ip) < 0)
        panic("cannot find %s", path);
    struct proc *p = proc_alloc();
    if (!p)
        panic("cannot allocate init");
    char *argv[] = {"init", NULL};
    if (exec_into(p, ip, 1, argv) < 0)
        panic("cannot exec %s", path);
    p->cwd = fs_root;
    iref(fs_root);
    p->ppid = 0;
    p->pgid = p->pid;
    p->state = PS_RUNNABLE;
}

/* ---------------- scheduling ---------------- */

void schedule(void)
{
    struct proc *old = cur, *next = NULL;
    int idx = old - ptable;

    for (int i = 1; i <= NPROC; i++) {
        struct proc *p = &ptable[(idx + i) % NPROC];
        if (p != &ptable[0] && p->state == PS_RUNNABLE) {
            next = p;
            break;
        }
    }
    if (!next) {
        if (old->state == PS_RUNNING || old->state == PS_RUNNABLE || old == &ptable[0]) {
            if (old != &ptable[0])
                old->state = PS_RUNNING;
            return;
        }
        next = &ptable[0];
    }
    if (next == old) {
        old->state = PS_RUNNING;
        return;
    }

    next->state = PS_RUNNING;
    cur = next;
    if (next->kstack)
        tss_set_kstack((uptr)next->kstack + KSTACK_SIZE);
    vmm_switch(next->pd ? next->pd : kernel_pd);
    __asm__ volatile("fxsave %0" : "=m"(*(u8 (*)[512])old->fpu));
    __asm__ volatile("fxrstor %0" : : "m"(*(u8 (*)[512])next->fpu));
    switch_context(&old->ctx_sp, next->ctx_sp);
}

void yield(void)
{
    cur->state = PS_RUNNABLE;
    schedule();
}

void sleep_on(void *chan)
{
    cur->chan = chan;
    cur->state = PS_SLEEPING;
    schedule();
    cur->chan = NULL;
    cur->wake_tick = 0;
}

/* Sleeps on chan until woken or `deadline` (an absolute tick; 0 = never). */
int sleep_until(void *chan, u32 deadline)
{
    cur->wake_tick = deadline;
    sleep_on(chan);
    return deadline && (i32)(ticks - deadline) >= 0;
}

void wakeup(void *chan)
{
    for (int i = 1; i < NPROC; i++)
        if (ptable[i].state == PS_SLEEPING && ptable[i].chan == chan)
            ptable[i].state = PS_RUNNABLE;
}

void timer_tick(struct regs *r)
{
    ticks++;
    random_event(ticks);
    if (cur != &ptable[0])
        cur->cpu_ticks++;
    for (int i = 1; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->state == PS_SLEEPING && p->wake_tick && (i32)(ticks - p->wake_tick) >= 0)
            p->state = PS_RUNNABLE;
    }
    net_tick();
    if ((r->cs & 3) == 3)
        yield();                        /* preempt user code only */
}

void idle_loop(void)
{
    for (;;) {
        cli();
        schedule();
        __asm__ volatile("sti; hlt");
    }
}

/* ---------------- lifecycle ---------------- */

int proc_fork(struct regs *r)
{
    struct proc *np = proc_alloc();
    if (!np)
        return -EAGAIN;
    np->pd = vmm_clone_pd(cur->pd);
    if (!np->pd) {
        proc_discard(np);
        return -ENOMEM;
    }
    np->brk = cur->brk;
    np->brk_start = cur->brk_start;
    for (int i = 0; i < NOFILE; i++)
        if (cur->ofile[i])
            np->ofile[i] = file_dup(cur->ofile[i]);
    np->cwd = cur->cwd;
    iref(np->cwd);
    memcpy(np->name, cur->name, sizeof np->name);
    np->ppid = cur->pid;
    np->pgid = cur->pgid;
    __asm__ volatile("fxsave %0" : "=m"(*(u8 (*)[512])cur->fpu));      /* the child starts with the parent's registers */
    memcpy(np->fpu, cur->fpu, sizeof np->fpu);
    *np->tf = *r;
    np->tf->rax = 0;
    np->state = PS_RUNNABLE;
    return np->pid;
}

void proc_exit(int status)
{
    if (cur->pid == 1)
        panic("init exited with status %d", status);
    if (cur->pid == gfx_owner)
        gfx_off();                      /* back to the text console */

    for (int i = 0; i < NOFILE; i++) {
        if (cur->ofile[i]) {
            file_close(cur->ofile[i]);
            cur->ofile[i] = NULL;
        }
    }
    if (cur->cwd) {
        iput(cur->cwd);
        cur->cwd = NULL;
    }

    /* Orphans go to init. */
    struct proc *init = &ptable[0];
    for (int i = 1; i < NPROC; i++)
        if (ptable[i].pid == 1 && ptable[i].state != PS_UNUSED)
            init = &ptable[i];
    for (int i = 1; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->state != PS_UNUSED && p->ppid == cur->pid) {
            p->ppid = 1;
            if (p->state == PS_ZOMBIE)
                wakeup(init);
        }
    }

    cur->exit_status = status;
    vmm_switch(kernel_pd);              /* we're about to free our own address space */
    vmm_free_pd(cur->pd);
    cur->pd = NULL;
    cur->state = PS_ZOMBIE;

    for (int i = 1; i < NPROC; i++)
        if (ptable[i].state != PS_UNUSED && ptable[i].pid == cur->ppid)
            wakeup(&ptable[i]);
    schedule();
    panic("zombie process was rescheduled");
}

int proc_wait(int *status, int flags)
{
    for (;;) {
        int havekids = 0;
        for (int i = 1; i < NPROC; i++) {
            struct proc *p = &ptable[i];
            if (p->state == PS_UNUSED || p->ppid != cur->pid)
                continue;
            havekids = 1;
            if (p->state == PS_ZOMBIE) {
                int pid = p->pid;
                *status = p->exit_status;
                kfree(p->kstack);
                p->kstack = NULL;
                p->state = PS_UNUSED;
                return pid;
            }
        }
        if (!havekids)
            return -ECHILD;
        if (flags & WNOHANG)
            return 0;
        if (cur->killed)
            return -EINTR;
        sleep_on(cur);
    }
}

int proc_kill(int pid)
{
    if (pid == 1)
        return -EPERM;
    for (int i = 1; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->pid == pid && p->state != PS_UNUSED && p->state != PS_ZOMBIE) {
            p->killed = 1;
            if (p->state == PS_SLEEPING)
                p->state = PS_RUNNABLE;
            return 0;
        }
    }
    return -ESRCH;
}

int proc_kill_pgrp(int pgid)
{
    int n = 0;
    if (pgid <= 1)
        return 0;
    for (int i = 1; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p->pgid == pgid && p->pid != 1 && p->state != PS_UNUSED && p->state != PS_ZOMBIE) {
            p->killed = 1;
            if (p->state == PS_SLEEPING)
                p->state = PS_RUNNABLE;
            n++;
        }
    }
    return n;
}

int proc_setpgid(int pid, int pgid)
{
    struct proc *p = NULL;
    if (pid == 0)
        p = cur;
    for (int i = 1; i < NPROC && !p; i++)
        if (ptable[i].pid == pid && ptable[i].state != PS_UNUSED)
            p = &ptable[i];
    if (!p)
        return -ESRCH;
    if (p != cur && p->ppid != cur->pid)
        return -EPERM;
    p->pgid = pgid ? pgid : p->pid;
    return 0;
}
