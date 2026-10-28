/* Physical frames, kernel heap, and paging.
 *
 * Layout: the kernel image sits at 1 MiB, followed by the kernel heap.  All of
 * RAM is identity-mapped (supervisor-only) in every address space, so the
 * kernel can touch any physical frame directly.  User memory lives at
 * [USER_BASE, USER_TOP) and is private to each process. */
#include "kernel.h"

extern char _end[];

#define MAX_RAM (512u * 1024 * 1024)

static u32 bitmap[(MAX_RAM / PAGE_SIZE) / 32];
static u32 total_frames, nfree, hint;
u32 *kernel_pd;

/* ---------------- physical frame allocator ---------------- */

static void frame_set(u32 f)   { bitmap[f >> 5] |= 1u << (f & 31); }
static void frame_clear(u32 f) { bitmap[f >> 5] &= ~(1u << (f & 31)); }

u32 pmm_alloc(void)
{
    u32 words = (total_frames + 31) / 32;
    for (u32 pass = 0; pass < 2; pass++) {
        u32 start = pass ? 0 : hint >> 5;
        for (u32 w = start; w < words; w++) {
            if (bitmap[w] == 0xFFFFFFFFu)
                continue;
            for (u32 b = 0; b < 32; b++) {
                u32 f = w * 32 + b;
                if (f < total_frames && !(bitmap[w] & (1u << b))) {
                    frame_set(f);
                    nfree--;
                    hint = f;
                    u32 pa = f * PAGE_SIZE;
                    memset((void *)pa, 0, PAGE_SIZE);
                    return pa;
                }
            }
        }
    }
    return 0;
}

void pmm_free(u32 pa)
{
    u32 f = pa / PAGE_SIZE;
    if (!pa || f >= total_frames)
        return;
    frame_clear(f);
    nfree++;
    if (f < hint)
        hint = f;
}

void pmm_stats(u32 *total_kb, u32 *free_kb)
{
    *total_kb = total_frames * (PAGE_SIZE / 1024);
    *free_kb = nfree * (PAGE_SIZE / 1024);
}

/* ---------------- kernel heap (first fit, address-ordered) ---------------- */

struct hblock {
    u32 size;
    u32 free;
    struct hblock *prev, *next;
};

static struct hblock *heap_head;

void heap_init(u32 start, u32 size)
{
    heap_head = (struct hblock *)start;
    heap_head->size = size - sizeof(struct hblock);
    heap_head->free = 1;
    heap_head->prev = heap_head->next = NULL;
}

void *kmalloc(u32 n)
{
    n = (n + 15) & ~15u;
    if (!n)
        n = 16;
    for (struct hblock *b = heap_head; b; b = b->next) {
        if (!b->free || b->size < n)
            continue;
        if (b->size >= n + sizeof(struct hblock) + 16) {
            struct hblock *nb = (struct hblock *)((u8 *)b + sizeof(struct hblock) + n);
            nb->size = b->size - n - sizeof(struct hblock);
            nb->free = 1;
            nb->prev = b;
            nb->next = b->next;
            if (b->next)
                b->next->prev = nb;
            b->next = nb;
            b->size = n;
        }
        b->free = 0;
        return (u8 *)b + sizeof(struct hblock);
    }
    return NULL;
}

void *kzalloc(u32 n)
{
    void *p = kmalloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}

void kfree(void *p)
{
    if (!p)
        return;
    struct hblock *b = (struct hblock *)((u8 *)p - sizeof(struct hblock));
    b->free = 1;
    if (b->next && b->next->free) {
        b->size += sizeof(struct hblock) + b->next->size;
        b->next = b->next->next;
        if (b->next)
            b->next->prev = b;
    }
    if (b->prev && b->prev->free) {
        b->prev->size += sizeof(struct hblock) + b->size;
        b->prev->next = b->next;
        if (b->next)
            b->next->prev = b->prev;
    }
}

void heap_stats(u32 *total, u32 *free)
{
    *total = *free = 0;
    for (struct hblock *b = heap_head; b; b = b->next) {
        *total += b->size;
        if (b->free)
            *free += b->size;
    }
}

/* ---------------- paging ---------------- */

u32 *vmm_new_pd(void)
{
    u32 *pd = (u32 *)pmm_alloc();
    if (!pd)
        return NULL;
    /* Share the kernel's identity-mapped page tables. */
    for (int i = 0; i < 256; i++)
        pd[i] = kernel_pd[i];
    return pd;
}

int vmm_map(u32 *pd, u32 va, u32 pa, u32 flags)
{
    u32 di = va >> 22;
    if (!(pd[di] & PTE_P)) {
        u32 pt = pmm_alloc();
        if (!pt)
            return -1;
        pd[di] = pt | PTE_P | PTE_RW | PTE_US;
    }
    u32 *pt = (u32 *)(pd[di] & ~0xFFFu);
    pt[(va >> 12) & 0x3FF] = (pa & ~0xFFFu) | flags | PTE_P;
    return 0;
}

u32 vmm_get_pte(u32 *pd, u32 va)
{
    u32 de = pd[va >> 22];
    if (!(de & PTE_P))
        return 0;
    return ((u32 *)(de & ~0xFFFu))[(va >> 12) & 0x3FF];
}

void vmm_unmap(u32 *pd, u32 va)
{
    u32 pte = vmm_get_pte(pd, va);
    if (!(pte & PTE_P))
        return;
    if (!(pte & PTE_SHARED))
        pmm_free(pte & ~0xFFFu);
    ((u32 *)(pd[va >> 22] & ~0xFFFu))[(va >> 12) & 0x3FF] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

/* Maps a fresh zeroed user page at va unless one is already there. */
int vmm_alloc_page(u32 *pd, u32 va)
{
    if (vmm_get_pte(pd, va) & PTE_P)
        return 0;
    u32 f = pmm_alloc();
    if (!f)
        return -1;
    if (vmm_map(pd, va, f, PTE_RW | PTE_US) < 0) {
        pmm_free(f);
        return -1;
    }
    return 0;
}

u32 *vmm_clone_pd(u32 *src)
{
    u32 *pd = vmm_new_pd();
    if (!pd)
        return NULL;
    for (u32 di = USER_BASE >> 22; di < USER_TOP >> 22; di++) {
        if (!(src[di] & PTE_P))
            continue;
        u32 *spt = (u32 *)(src[di] & ~0xFFFu);
        for (u32 ti = 0; ti < 1024; ti++) {
            if (!(spt[ti] & PTE_P))
                continue;
            u32 va = (di << 22) | (ti << 12);
            if (spt[ti] & PTE_SHARED) {         /* device memory: child shares the mapping */
                if (vmm_map(pd, va, spt[ti] & ~0xFFFu, spt[ti] & 0xFFFu & ~PTE_P) < 0) {
                    vmm_free_pd(pd);
                    return NULL;
                }
                continue;
            }
            u32 f = pmm_alloc();
            if (!f || vmm_map(pd, va, f, spt[ti] & 0xFFFu & ~PTE_P) < 0) {
                pmm_free(f);
                vmm_free_pd(pd);
                return NULL;
            }
            memcpy((void *)f, (void *)(spt[ti] & ~0xFFFu), PAGE_SIZE);
        }
    }
    return pd;
}

void vmm_free_pd(u32 *pd)
{
    for (u32 di = USER_BASE >> 22; di < USER_TOP >> 22; di++) {
        if (!(pd[di] & PTE_P))
            continue;
        u32 *pt = (u32 *)(pd[di] & ~0xFFFu);
        for (u32 ti = 0; ti < 1024; ti++)
            if ((pt[ti] & PTE_P) && !(pt[ti] & PTE_SHARED))
                pmm_free(pt[ti] & ~0xFFFu);
        pmm_free((u32)pt);
    }
    pmm_free((u32)pd);
}

u32 vmm_count_pages(u32 *pd)
{
    u32 n = 0;
    for (u32 di = USER_BASE >> 22; di < USER_TOP >> 22; di++) {
        if (!(pd[di] & PTE_P))
            continue;
        u32 *pt = (u32 *)(pd[di] & ~0xFFFu);
        for (u32 ti = 0; ti < 1024; ti++)
            if ((pt[ti] & PTE_P) && !(pt[ti] & PTE_SHARED))
                n++;
    }
    return n;
}

void vmm_switch(u32 *pd)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(pd) : "memory");
}

/* Copies into another address space through the identity map. */
int copy_to_space(u32 *pd, u32 va, const void *src, u32 len)
{
    const u8 *s = src;
    while (len) {
        u32 pte = vmm_get_pte(pd, va);
        if (!(pte & PTE_P))
            return -1;
        u32 off = va & 0xFFF;
        u32 n = PAGE_SIZE - off;
        if (n > len)
            n = len;
        memcpy((void *)((pte & ~0xFFFu) + off), s, n);
        va += n;
        s += n;
        len -= n;
    }
    return 0;
}

/* Is [addr, addr+len) entirely mapped user memory in the current process? */
int user_ok(u32 addr, u32 len)
{
    if (len == 0)
        return 1;
    if (addr < USER_BASE || addr >= USER_TOP || len > USER_TOP - addr)
        return 0;
    for (u32 va = addr & ~0xFFFu; va < addr + len; va += PAGE_SIZE) {
        u32 pte = vmm_get_pte(cur->pd, va);
        if ((pte & (PTE_P | PTE_US)) != (PTE_P | PTE_US))
            return 0;
    }
    return 1;
}

void mem_init(u32 ram_bytes)
{
    if (ram_bytes > MAX_RAM)
        ram_bytes = MAX_RAM;
    total_frames = ram_bytes / PAGE_SIZE;

    u32 heap_size = ram_bytes / 4;
    if (heap_size < (8u << 20)) heap_size = 8u << 20;
    if (heap_size > (96u << 20)) heap_size = 96u << 20;
    u32 heap_start = ((u32)_end + 4095) & ~4095u;
    u32 heap_end = (heap_start + heap_size) & ~4095u;
    if (heap_end > ram_bytes)
        panic("not enough RAM (%u MiB)", ram_bytes >> 20);
    heap_init(heap_start, heap_end - heap_start);

    /* Frames below the end of the heap belong to the firmware/kernel/heap. */
    memset(bitmap, 0xFF, sizeof bitmap);
    for (u32 f = heap_end / PAGE_SIZE; f < total_frames; f++) {
        frame_clear(f);
        nfree++;
    }
    hint = heap_end / PAGE_SIZE;

    /* Identity-map all RAM, supervisor only, in 4 KiB pages. */
    kernel_pd = (u32 *)pmm_alloc();
    u32 npde = (ram_bytes + (4u << 20) - 1) >> 22;
    for (u32 i = 0; i < npde; i++) {
        u32 *pt = (u32 *)pmm_alloc();
        if (!pt)
            panic("out of memory building page tables");
        for (u32 j = 0; j < 1024; j++)
            pt[j] = ((i << 22) | (j << 12)) | PTE_P | PTE_RW;
        kernel_pd[i] = (u32)pt | PTE_P | PTE_RW;
    }
    __asm__ volatile(
        "mov %0, %%cr3\n"
        "mov %%cr0, %%eax\n"
        "or $0x80000000, %%eax\n"
        "mov %%eax, %%cr0\n"
        : : "r"(kernel_pd) : "eax", "memory");

    kprintf("[mem ] %u MiB RAM, %u MiB kernel heap, paging on\n",
            ram_bytes >> 20, (heap_end - heap_start) >> 20);
}
