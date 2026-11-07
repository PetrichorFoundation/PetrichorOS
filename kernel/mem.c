/* Physical frames, kernel heap, and paging.
 *
 * Layout: the kernel image sits at 1 MiB, followed by the kernel heap.  All of
 * RAM is identity-mapped (supervisor-only) in every address space, so the
 * kernel can touch any physical frame directly.  User memory lives at
 * [USER_BASE, USER_TOP) and is private to each process. */
#include "kernel.h"

extern char _end[];
static void mmio_init(void);

#define MAX_RAM (512u * 1024 * 1024)

static u32 bitmap[(MAX_RAM / PAGE_SIZE) / 32];
static u32 total_frames, nfree, hint;
u64 *kernel_pd;

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
                    memset((void *)(uptr)pa, 0, PAGE_SIZE);
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
} __attribute__((aligned(16)));                  /* payloads stay 16-byte aligned */

static struct hblock *heap_head;

void heap_init(u32 start, u32 size)
{
    heap_head = (struct hblock *)(uptr)start;
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

/* ---------------- paging ----------------
 *
 * Four-level (long mode) tables, but the layout is the same as it always was, and everything the kernel
 * touches lives in the low 4 GiB:
 *   PML4[0] -> PDPT:  [0, 1 GiB)  identity-mapped RAM, supervisor only, 2 MiB pages   (shared by all spaces)
 *                     [1, 3 GiB)  user memory, private page directories and tables
 *                     [3, 4 GiB)  the device-memory window                              (shared by all spaces)
 * A process address space is its own PML4 + PDPT; the shared entries point at the kernel's page directories. */

#define IDX_PML4(va) (((u64)(va) >> 39) & 511)
#define IDX_PDPT(va) (((u64)(va) >> 30) & 511)
#define IDX_PD(va)   (((u64)(va) >> 21) & 511)
#define IDX_PT(va)   (((u64)(va) >> 12) & 511)
#define PADDR(e)     ((e) & 0x000FFFFFFFFFF000ull)

static u64 *kernel_pdpt, *kernel_pd0, *kernel_pd3;      /* kernel_pd (the PML4) is declared above */

u64 *vmm_new_pd(void)
{
    u64 *pml4 = (u64 *)(uptr)pmm_alloc();
    u64 *pdpt = (u64 *)(uptr)pmm_alloc();
    if (!pml4 || !pdpt) {
        pmm_free((u32)(uptr)pml4);
        pmm_free((u32)(uptr)pdpt);
        return NULL;
    }
    pml4[0] = (u64)(uptr)pdpt | PTE_P | PTE_RW | PTE_US;
    pdpt[0] = kernel_pdpt[0];
    pdpt[3] = kernel_pdpt[3];
    return pml4;
}

/* The page table covering va, optionally creating the levels above it (user addresses only). */
static u64 *walk(u64 *pml4, u32 va, int create)
{
    if (!(pml4[0] & PTE_P))
        return NULL;
    if (IDX_PDPT(va) != 1 && IDX_PDPT(va) != 2)         /* only [1 GiB, 3 GiB) holds user pages */
        return NULL;
    u64 *pdpt = (u64 *)(uptr)PADDR(pml4[0]);
    u64 *e = &pdpt[IDX_PDPT(va)];
    if (!(*e & PTE_P)) {
        if (!create)
            return NULL;
        u32 pd = pmm_alloc();
        if (!pd)
            return NULL;
        *e = pd | PTE_P | PTE_RW | PTE_US;
    }
    u64 *pd = (u64 *)(uptr)PADDR(*e);
    e = &pd[IDX_PD(va)];
    if (!(*e & PTE_P)) {
        if (!create)
            return NULL;
        u32 pt = pmm_alloc();
        if (!pt)
            return NULL;
        *e = pt | PTE_P | PTE_RW | PTE_US;
    }
    return (u64 *)(uptr)PADDR(*e);
}

int vmm_map(u64 *pd, u32 va, u32 pa, u64 flags)
{
    u64 *pt = walk(pd, va, 1);
    if (!pt)
        return -1;
    pt[IDX_PT(va)] = (u64)(pa & ~0xFFFu) | flags | PTE_P;
    return 0;
}

u64 vmm_get_pte(u64 *pd, u32 va)
{
    u64 *pt = walk(pd, va, 0);
    return pt ? pt[IDX_PT(va)] : 0;
}

void vmm_unmap(u64 *pd, u32 va)
{
    u64 *pt = walk(pd, va, 0);
    if (!pt || !(pt[IDX_PT(va)] & PTE_P))
        return;
    u64 pte = pt[IDX_PT(va)];
    if (!(pte & PTE_SHARED))
        pmm_free((u32)PADDR(pte));
    pt[IDX_PT(va)] = 0;
    __asm__ volatile("invlpg (%0)" : : "r"((uptr)va) : "memory");
}

/* Maps a fresh zeroed user page at va unless one is already there. */
int vmm_alloc_page(u64 *pd, u32 va)
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

u64 *vmm_clone_pd(u64 *src)
{
    u64 *pd = vmm_new_pd();
    if (!pd)
        return NULL;
    u64 *spdpt = (u64 *)(uptr)PADDR(src[0]);
    for (int a = 1; a <= 2; a++) {
        if (!(spdpt[a] & PTE_P))
            continue;
        u64 *spdir = (u64 *)(uptr)PADDR(spdpt[a]);
        for (int b = 0; b < 512; b++) {
            if (!(spdir[b] & PTE_P))
                continue;
            u64 *spt = (u64 *)(uptr)PADDR(spdir[b]);
            for (int c = 0; c < 512; c++) {
                u64 pte = spt[c];
                if (!(pte & PTE_P))
                    continue;
                u32 va = (u32)(((u64)a << 30) | ((u64)b << 21) | ((u64)c << 12));
                u64 fl = pte & 0xFFFull & ~PTE_P;
                if (pte & PTE_SHARED) {                 /* device memory: child shares the mapping */
                    if (vmm_map(pd, va, (u32)PADDR(pte), fl) < 0) {
                        vmm_free_pd(pd);
                        return NULL;
                    }
                    continue;
                }
                u32 f = pmm_alloc();
                if (!f || vmm_map(pd, va, f, fl) < 0) {
                    pmm_free(f);
                    vmm_free_pd(pd);
                    return NULL;
                }
                memcpy((void *)(uptr)f, (void *)(uptr)PADDR(pte), PAGE_SIZE);
            }
        }
    }
    return pd;
}

void vmm_free_pd(u64 *pd)
{
    u64 *pdpt = (u64 *)(uptr)PADDR(pd[0]);
    for (int a = 1; a <= 2; a++) {
        if (!(pdpt[a] & PTE_P))
            continue;
        u64 *pdir = (u64 *)(uptr)PADDR(pdpt[a]);
        for (int b = 0; b < 512; b++) {
            if (!(pdir[b] & PTE_P))
                continue;
            u64 *pt = (u64 *)(uptr)PADDR(pdir[b]);
            for (int c = 0; c < 512; c++)
                if ((pt[c] & PTE_P) && !(pt[c] & PTE_SHARED))
                    pmm_free((u32)PADDR(pt[c]));
            pmm_free((u32)(uptr)pt);
        }
        pmm_free((u32)(uptr)pdir);
    }
    pmm_free((u32)(uptr)pdpt);
    pmm_free((u32)(uptr)pd);
}

u32 vmm_count_pages(u64 *pd)
{
    u32 n = 0;
    u64 *pdpt = (u64 *)(uptr)PADDR(pd[0]);
    for (int a = 1; a <= 2; a++) {
        if (!(pdpt[a] & PTE_P))
            continue;
        u64 *pdir = (u64 *)(uptr)PADDR(pdpt[a]);
        for (int b = 0; b < 512; b++) {
            if (!(pdir[b] & PTE_P))
                continue;
            u64 *pt = (u64 *)(uptr)PADDR(pdir[b]);
            for (int c = 0; c < 512; c++)
                if ((pt[c] & PTE_P) && !(pt[c] & PTE_SHARED))
                    n++;
        }
    }
    return n;
}

void vmm_switch(u64 *pd)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"((u64)(uptr)pd) : "memory");
}

/* Copies into another address space through the identity map. */
int copy_to_space(u64 *pd, u32 va, const void *src, u32 len)
{
    const u8 *s = src;
    while (len) {
        u64 pte = vmm_get_pte(pd, va);
        if (!(pte & PTE_P))
            return -1;
        u32 off = va & 0xFFF;
        u32 n = PAGE_SIZE - off;
        if (n > len)
            n = len;
        memcpy((void *)(uptr)(PADDR(pte) + off), s, n);
        va += n;
        s += n;
        len -= n;
    }
    return 0;
}

/* Is [addr, addr+len) entirely mapped user memory in the current process? */
int user_ok(u64 addr, u32 len)
{
    if (len == 0)
        return 1;
    if (addr < USER_BASE || addr >= USER_TOP || len > USER_TOP - addr)
        return 0;
    for (u64 va = addr & ~0xFFFull; va < addr + len; va += PAGE_SIZE) {
        u64 pte = vmm_get_pte(cur->pd, (u32)va);
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
    u32 heap_start = ((u32)(uptr)_end + 4095) & ~4095u, heap_end;

    memset(bitmap, 0xFF, sizeof bitmap);
    if (bootinfo.nranges) {
        /* UEFI told us exactly which RAM is usable (it has holes).  The heap goes into the largest range
         * above the kernel; every other usable frame above the kernel image joins the free pool. */
        u32 best_s = 0, best_size = 0;
        for (u32 i = 0; i < bootinfo.nranges; i++) {
            u32 s = bootinfo.range[i][0] > heap_start ? bootinfo.range[i][0] : heap_start, e = bootinfo.range[i][1];
            if (e > ram_bytes) e = ram_bytes;
            s = (s + 4095) & ~4095u;
            if (e > s && e - s > best_size) {
                best_s = s;
                best_size = e - s;
            }
        }
        if (best_size < (4u << 20))
            panic("not enough usable RAM");
        if (heap_size > best_size / 2)
            heap_size = best_size / 2;
        heap_start = best_s;
        heap_end = (heap_start + heap_size) & ~4095u;
        heap_init(heap_start, heap_end - heap_start);
        u32 first_free = 0;
        for (u32 i = 0; i < bootinfo.nranges; i++) {
            u32 s = bootinfo.range[i][0] > ((u32)(uptr)_end + 4095 & ~4095u) ? bootinfo.range[i][0] : ((u32)(uptr)_end + 4095 & ~4095u);
            u32 e = bootinfo.range[i][1] > ram_bytes ? ram_bytes : bootinfo.range[i][1];
            for (u32 f = (s + 4095) / PAGE_SIZE; f < e / PAGE_SIZE; f++) {
                u32 pa = f * PAGE_SIZE;
                if (pa >= heap_start && pa < heap_end)
                    continue;
                frame_clear(f);
                nfree++;
                if (!first_free)
                    first_free = f;
            }
        }
        hint = first_free;
    } else {
        u32 low_free_start = 0, low_free_end = 0;
        if (bootinfo.kimg && bootinfo.kimg + bootinfo.kimg_size > heap_start) {
            low_free_start = heap_start;                      /* the gap before the kernel copy is ordinary RAM */
            low_free_end = bootinfo.kimg & ~4095u;
            heap_start = (bootinfo.kimg + bootinfo.kimg_size + 4095) & ~4095u;
        }
        heap_end = (heap_start + heap_size) & ~4095u;
        if (heap_end > ram_bytes)
            panic("not enough RAM (%u MiB)", ram_bytes >> 20);
        heap_init(heap_start, heap_end - heap_start);

        /* Frames below the end of the heap belong to the firmware/kernel/heap. */
        for (u32 f = heap_end / PAGE_SIZE; f < total_frames; f++) {
            frame_clear(f);
            nfree++;
        }
        for (u32 f = low_free_start / PAGE_SIZE; f < low_free_end / PAGE_SIZE; f++) {
            frame_clear(f);
            nfree++;
        }
        hint = low_free_start ? low_free_start / PAGE_SIZE : heap_end / PAGE_SIZE;
    }

    /* Identity-map all RAM (supervisor only) in 2 MiB pages. */
    kernel_pd = (u64 *)(uptr)pmm_alloc();
    kernel_pdpt = (u64 *)(uptr)pmm_alloc();
    kernel_pd0 = (u64 *)(uptr)pmm_alloc();
    kernel_pd3 = (u64 *)(uptr)pmm_alloc();
    if (!kernel_pd || !kernel_pdpt || !kernel_pd0 || !kernel_pd3)
        panic("out of memory building page tables");
    u32 npde = (ram_bytes + (2u << 20) - 1) >> 21;
    for (u32 i = 0; i < npde && i < 512; i++)
        kernel_pd0[i] = ((u64)i << 21) | PTE_P | PTE_RW | PTE_PS;
    kernel_pdpt[0] = (u64)(uptr)kernel_pd0 | PTE_P | PTE_RW | PTE_US;
    kernel_pdpt[3] = (u64)(uptr)kernel_pd3 | PTE_P | PTE_RW | PTE_US;
    kernel_pd[0] = (u64)(uptr)kernel_pdpt | PTE_P | PTE_RW | PTE_US;
    mmio_init();
    vmm_switch(kernel_pd);                                    /* paging is already on: the bootloader stub enabled it */

    kprintf("[mem ] %u MiB RAM, %u MiB kernel heap, paging on (64-bit)\n",
            ram_bytes >> 20, (heap_end - heap_start) >> 20);
}

/* Device memory (PCI BARs, framebuffers) lives above 3 GiB in a window shared by all processes. */
#define MMIO_BASE 0xD0000000u
#define MMIO_SIZE (64u << 20)
static u32 mmio_next = MMIO_BASE;

/* Every address space shares the kernel's page directory for [3 GiB, 4 GiB), so tables added to it later are
 * visible everywhere.  The window's page tables are created up front. */
static void mmio_init(void)
{
    for (u32 va = MMIO_BASE; va < MMIO_BASE + MMIO_SIZE; va += 2u << 20) {
        u32 pt = pmm_alloc();
        if (!pt)
            panic("out of memory for the device-memory window");
        kernel_pd3[IDX_PD(va)] = pt | PTE_P | PTE_RW;
    }
}

void *mmio_map(u32 phys, u32 size)
{
    u32 off = phys & 0xFFF, base = phys & ~0xFFFu;
    u32 pages = (off + size + PAGE_SIZE - 1) / PAGE_SIZE;
    if (mmio_next + pages * PAGE_SIZE > MMIO_BASE + MMIO_SIZE)
        return NULL;
    u32 va = mmio_next;
    mmio_next += pages * PAGE_SIZE;
    for (u32 i = 0; i < pages; i++) {
        u32 v = va + i * PAGE_SIZE;
        u64 *pt = (u64 *)(uptr)PADDR(kernel_pd3[IDX_PD(v)]);
        pt[IDX_PT(v)] = (u64)(base + i * PAGE_SIZE) | PTE_P | PTE_RW | 0x18;  /* PCD|PWT */
        __asm__ volatile("invlpg (%0)" : : "r"((uptr)v) : "memory");
    }
    return (void *)(uptr)(va + off);
}

void *kalloc_aligned(u32 size, u32 align)
{
    u8 *p = kzalloc(size + align);
    return p ? (void *)(((uptr)p + align - 1) & ~(uptr)(align - 1)) : NULL;
}
