/* Anonymous memory mappings (mmap/munmap/mprotect) and the kernel side of threads (futexes, shared address
 * spaces, thread-local storage).
 *
 * mmap hands out zeroed pages from [MMAP_BASE, MMAP_TOP), first fit over what the page table already holds.
 * A thread (made by clone, in proc.c) is a proc that shares its creator's page table; the page table is
 * freed by whichever thread of the group leaves it last.  A futex sleeps on the physical address of a user
 * word, so it is unique even when two processes share memory. */
#include "kernel.h"

#define PADDR(e) ((e) & 0x000FFFFFFFFFF000ull)
#define PG(x) (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

static int range_ok(u32 addr, u32 len)
{
    return !(addr & (PAGE_SIZE - 1)) && len && addr >= MMAP_BASE && len <= MMAP_TOP - addr;
}

long sys_mmap(u32 len, u32 prot)
{
    if (!len || len > MMAP_TOP - MMAP_BASE)
        return -EINVAL;
    u32 n = PG(len) / PAGE_SIZE, run = 0, start = 0;
    for (u32 va = MMAP_BASE; va < MMAP_TOP; va += PAGE_SIZE) {
        if (vmm_get_pte(cur->pd, va) & PTE_P) {
            run = 0;
            continue;
        }
        if (!run++)
            start = va;
        if (run == n)
            break;
    }
    if (run < n)
        return -ENOMEM;
    u64 fl = PTE_US | (prot & PROT_WRITE ? PTE_RW : 0);
    for (u32 i = 0; i < n; i++) {
        u32 f = pmm_alloc();
        if (!f || vmm_map(cur->pd, start + i * PAGE_SIZE, f, fl) < 0) {
            pmm_free(f);
            for (u32 k = 0; k < i; k++)
                vmm_unmap(cur->pd, start + k * PAGE_SIZE);
            return -ENOMEM;
        }
    }
    return (long)(i32)start;            /* above 2 GiB: user space reads it back as unsigned */
}

long sys_munmap(u32 addr, u32 len)
{
    if (!range_ok(addr, len))
        return -EINVAL;
    for (u32 va = addr; va < addr + PG(len); va += PAGE_SIZE)
        vmm_unmap(cur->pd, va);
    return 0;
}

long sys_mprotect(u32 addr, u32 len, u32 prot)
{
    if (!range_ok(addr, len))
        return -EINVAL;
    for (u32 va = addr; va < addr + PG(len); va += PAGE_SIZE)
        if (vmm_set_rw(cur->pd, va, (prot & PROT_WRITE) != 0) < 0)
            return -ENOMEM;
    vmm_switch(cur->pd);                /* flush the TLB */
    return 0;
}

/* ---------------- futexes ---------------- */

static void *futex_chan(u32 addr)
{
    u64 pte = vmm_get_pte(cur->pd, addr);
    if (!(pte & PTE_P))
        return NULL;
    return (void *)(uptr)(PADDR(pte) + (addr & 0xFFF) + 0x100000000ull);   /* never a kernel object's address */
}

long sys_futex(u32 addr, int op, u32 val, u32 ms)
{
    if ((addr & 3) || !user_ok(addr, 4))
        return -EFAULT;
    void *chan = futex_chan(addr);
    if (!chan)
        return -EFAULT;
    if (op == FUTEX_WAIT) {
        if (*(volatile u32 *)(uptr)addr != val)
            return -EAGAIN;
        if (cur->killed)
            return -EINTR;
        u32 deadline = ms ? ticks + (ms + 9) / 10 + 1 : 0;      /* 100 Hz timer */
        if (sleep_until(chan, deadline))
            return -ETIMEDOUT;
        return 0;
    }
    if (op == FUTEX_WAKE) {
        int n = 0;
        for (int i = 1; i < NPROC; i++)
            if (ptable[i].state == PS_SLEEPING && ptable[i].chan == chan)
                n++;
        wakeup(chan);
        return n;
    }
    return -EINVAL;
}

/* ---------------- shared address spaces ---------------- */

int pd_users(u64 *pd, struct proc *except)
{
    int n = 0;
    if (!pd)
        return 0;
    for (int i = 1; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p != except && p->pd == pd && p->state != PS_UNUSED && p->state != PS_ZOMBIE)
            n++;
    }
    return n;
}

void kill_sharers(u64 *pd, struct proc *except)
{
    for (int i = 1; i < NPROC; i++) {
        struct proc *p = &ptable[i];
        if (p != except && p->pd == pd && p->thread && p->state != PS_UNUSED && p->state != PS_ZOMBIE) {
            p->killed = 1;
            if (p->state == PS_SLEEPING)
                p->state = PS_RUNNABLE;
        }
    }
}

/* pthread_join waits on clear_tid: zero it and wake the waiters. */
void thread_exit_notify(struct proc *p)
{
    u32 a = p->clear_tid;
    if (!a || (a & 3) || !user_ok(a, 4))
        return;
    *(volatile u32 *)(uptr)a = 0;
    void *chan = futex_chan(a);
    if (chan)
        wakeup(chan);
}

void set_fsbase(u64 v)
{
    __asm__ volatile("wrmsr" : : "c"(0xC0000100u), "a"((u32)v), "d"((u32)(v >> 32)));
}
