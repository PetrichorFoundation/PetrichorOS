/* ELF32 loader: builds a fresh address space and points the trapframe at it. */
#include "kernel.h"

struct elf_hdr {
    u8 ident[16];
    u16 type, machine;
    u32 version, entry, phoff, shoff, flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};

struct elf_phdr {
    u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

#define PT_LOAD 1

static int exec_image(struct proc *p, struct inode *ip, const u8 *img, int argc, char **argv);

/* Disk-backed programs are read into a temporary kernel buffer first. */
int exec_into(struct proc *p, struct inode *ip, int argc, char **argv)
{
    if (ip->type != T_FILE)
        return -EACCES;
    if (!ip->pfs)
        return exec_image(p, ip, ip->data, argc, argv);
    u8 *buf = kmalloc(ip->size ? ip->size : 1);
    if (!buf)
        return -ENOMEM;
    int r = inode_read(ip, 0, buf, ip->size) == (int)ip->size
                ? exec_image(p, ip, buf, argc, argv) : -EIO;
    kfree(buf);
    return r;
}

static int exec_image(struct proc *p, struct inode *ip, const u8 *img, int argc, char **argv)
{
    if (ip->size < sizeof(struct elf_hdr))
        return -ENOEXEC;
    const struct elf_hdr *eh = (const void *)img;
    if (memcmp(eh->ident, "\177ELF", 4) || eh->ident[4] != 1 || eh->ident[5] != 1 ||
        eh->type != 2 || eh->machine != 3 || eh->phentsize != sizeof(struct elf_phdr))
        return -ENOEXEC;
    if (eh->phoff > ip->size || eh->phnum * sizeof(struct elf_phdr) > ip->size - eh->phoff)
        return -ENOEXEC;
    const struct elf_phdr *ph = (const void *)(img + eh->phoff);

    u32 *pd = vmm_new_pd();
    if (!pd)
        return -ENOMEM;

    int err = -ENOEXEC;
    u32 maxend = USER_BASE;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0)
            continue;
        u32 start = ph[i].vaddr, end = start + ph[i].memsz;
        if (start < USER_BASE || end < start || end > USER_HEAP_MAX || ph[i].filesz > ph[i].memsz ||
            ph[i].offset > ip->size || ph[i].filesz > ip->size - ph[i].offset)
            goto bad;
        for (u32 va = start & ~0xFFFu; va < end; va += PAGE_SIZE) {
            if (vmm_alloc_page(pd, va) < 0) {
                err = -ENOMEM;
                goto bad;
            }
        }
        copy_to_space(pd, start, img + ph[i].offset, ph[i].filesz);
        if (end > maxend)
            maxend = end;
    }
    if (eh->entry < USER_BASE || eh->entry >= maxend)
        goto bad;

    for (int i = 1; i <= USER_STACK_PAGES; i++) {
        if (vmm_alloc_page(pd, USER_TOP - i * PAGE_SIZE) < 0) {
            err = -ENOMEM;
            goto bad;
        }
    }

    /* Stack, top down: strings, argv[], then {fake return, argc, argv}. */
    u32 sp = USER_TOP;
    u32 uargv[MAXARG + 1];
    for (int i = argc - 1; i >= 0; i--) {
        u32 len = strlen(argv[i]) + 1;
        sp -= len;
        copy_to_space(pd, sp, argv[i], len);
        uargv[i] = sp;
    }
    uargv[argc] = 0;
    sp &= ~3u;
    sp -= (argc + 1) * 4;
    copy_to_space(pd, sp, uargv, (argc + 1) * 4);
    u32 frame[3] = {0, (u32)argc, sp};
    sp -= sizeof frame;
    copy_to_space(pd, sp, frame, sizeof frame);

    /* Commit: nothing below can fail. */
    u32 *old = p->pd;
    p->pd = pd;
    if (p == cur)
        vmm_switch(pd);
    if (old)
        vmm_free_pd(old);

    p->brk = p->brk_start = (maxend + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    const char *base = strrchr(argv[0], '/');
    safestrcpy(p->name, base ? base + 1 : argv[0], sizeof p->name);

    struct regs *tf = p->tf;
    memset(tf, 0, sizeof *tf);
    tf->cs = 0x1B;
    tf->ds = tf->es = tf->fs = tf->gs = tf->ss = 0x23;
    tf->eflags = 0x202;
    tf->eip = eh->entry;
    tf->useresp = sp;
    return 0;

bad:
    vmm_free_pd(pd);
    return err;
}
