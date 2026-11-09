/* Shared memory: page-backed buffers two processes map at once.  The desktop uses them for the windows of
 * separate programs (a program draws into its buffer, the desktop reads it), so pixels never travel through a pipe.
 *
 *   shm(SHM_CREATE, bytes)  -> id   creates a buffer and maps it in the caller
 *   shm(SHM_MAP, id)        -> 0    maps an existing buffer (at SHM_ADDR(id) in abi.h, the same in every process)
 *   shm(SHM_DROP, id)       -> 0    unmaps it; the buffer goes away when nobody has it mapped
 *
 * Buffer i always appears at SHM_BASE + i * SHM_SLOT in every process.  Mappings carry PTE_SHARED, so fork
 * shares them and exit/exec never free the pages themselves: the reference counts here do. */
#include "kernel.h"

#define NSHM      31
#define SHM_BASE  0x90000000u
#define SHM_SLOT  0x01800000u                 /* 24 MiB: room for a 2560x1600 32-bit window */

static struct {
    int used, refs;
    u32 npages;
    u32 *pages;
} shm[NSHM + 1];

static u32 slot_va(int id) { return SHM_BASE + (u32)(id - 1) * SHM_SLOT; }

static void free_seg(int id)
{
    for (u32 i = 0; i < shm[id].npages; i++)
        if (shm[id].pages[i]) pmm_free(shm[id].pages[i]);
    kfree(shm[id].pages);
    memset(&shm[id], 0, sizeof shm[id]);
}

static int map_into(struct proc *p, int id)
{
    if (p->shm_mask & (1u << id))
        return 0;
    u32 va = slot_va(id);
    for (u32 i = 0; i < shm[id].npages; i++)
        if (vmm_map(p->pd, va + i * PAGE_SIZE, shm[id].pages[i], PTE_RW | PTE_US | PTE_SHARED) < 0) {
            for (u32 k = 0; k < i; k++) vmm_unmap(p->pd, va + k * PAGE_SIZE);
            return -ENOMEM;
        }
    p->shm_mask |= 1u << id;
    shm[id].refs++;
    return 0;
}

static void unmap_from(struct proc *p, int id, int touch_pages)
{
    if (!(p->shm_mask & (1u << id)))
        return;
    p->shm_mask &= ~(1u << id);
    if (touch_pages && p->pd) {
        u32 va = slot_va(id);
        for (u32 i = 0; i < shm[id].npages; i++)
            vmm_unmap(p->pd, va + i * PAGE_SIZE);        /* PTE_SHARED: the page itself is not freed */
    }
    if (--shm[id].refs <= 0)
        free_seg(id);
}

long sys_shm(int op, u32 a)
{
    switch (op) {
    case SHM_CREATE: {
        if (a == 0 || a > SHM_SLOT)
            return -EINVAL;
        int id = 0;
        for (int i = 1; i <= NSHM; i++)
            if (!shm[i].used) { id = i; break; }
        if (!id)
            return -ENOMEM;
        u32 n = (a + PAGE_SIZE - 1) / PAGE_SIZE;
        shm[id].pages = kzalloc(n * sizeof(u32));
        if (!shm[id].pages)
            return -ENOMEM;
        shm[id].used = 1;
        shm[id].npages = n;
        for (u32 i = 0; i < n; i++) {
            u32 f = pmm_alloc();
            if (!f) { free_seg(id); return -ENOMEM; }
            memset((void *)(uptr)f, 0, PAGE_SIZE);
            shm[id].pages[i] = f;
        }
        int e = map_into(cur, id);
        if (e < 0) { free_seg(id); return e; }
        return id;
    }
    case SHM_MAP: {
        int id = (int)a;
        if (id < 1 || id > NSHM || !shm[id].used)
            return -EINVAL;
        return map_into(cur, id);
    }
    case SHM_DROP: {
        int id = (int)a;
        if (id < 1 || id > NSHM || !shm[id].used)
            return -EINVAL;
        unmap_from(cur, id, 1);
        return 0;
    }
    }
    return -EINVAL;
}

/* fork: the child shares every buffer the parent has mapped. */
void shm_fork(struct proc *child)
{
    child->shm_mask = cur->shm_mask;
    for (int id = 1; id <= NSHM; id++)
        if (child->shm_mask & (1u << id)) shm[id].refs++;
}

/* exit / exec: let go of everything (the address space is being thrown away, so no unmapping is needed). */
void shm_release_all(struct proc *p)
{
    for (int id = 1; id <= NSHM; id++)
        if (p->shm_mask & (1u << id)) unmap_from(p, id, 0);
    p->shm_mask = 0;
}
