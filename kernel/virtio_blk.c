/* virtio block device (legacy interface, I/O ports): the disk of QEMU/KVM, VirtualBox and most cloud virtual
 * machines.  One request at a time, polled; transfers go through a bounce buffer. */
#include "kernel.h"

#define V_FEATURES     0x00
#define V_GUEST_FEAT   0x04
#define V_QUEUE_PFN    0x08
#define V_QUEUE_SIZE   0x0C
#define V_QUEUE_SEL    0x0E
#define V_QUEUE_NOTIFY 0x10
#define V_STATUS       0x12
#define V_CONFIG       0x14                                 /* capacity (u64, in 512-byte sectors) */

#define CHUNK 128                                           /* sectors per request (64 KiB) */

struct vblk {
    u16 io;
    u16 qsize, last_used;
    u8 *desc;
    volatile u16 *avail, *used;
    u8 *hdr;                                                /* 16-byte request header + status byte */
    u8 *data;
};

static u32 align(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

static void desc_set(struct vblk *d, u32 i, void *addr, u32 len, u16 flags, u16 next)
{
    u8 *e = d->desc + 16 * i;
    *(u64 *)e = (u64)(uptr)addr;
    *(u32 *)(e + 8) = len;
    *(u16 *)(e + 12) = flags;
    *(u16 *)(e + 14) = next;
}

static int vblk_rw(void *priv, u32 lba, u32 n, void *buf, int write)
{
    struct vblk *d = priv;
    while (n) {
        u32 c = n > CHUNK ? CHUNK : n;
        *(u32 *)d->hdr = write ? 1 : 0;                     /* VIRTIO_BLK_T_OUT / T_IN */
        *(u32 *)(d->hdr + 4) = 0;
        *(u64 *)(d->hdr + 8) = lba;
        d->hdr[16] = 0xFF;
        if (write)
            memcpy(d->data, buf, c * 512);
        desc_set(d, 0, d->hdr, 16, 1, 1);                   /* header: device reads, chained */
        desc_set(d, 1, d->data, c * 512, write ? 1 : 1 | 2, 2);           /* data: chained; the device writes it on reads */
        desc_set(d, 2, d->hdr + 16, 1, 2, 0);               /* status: device writes */
        u16 idx = d->avail[1];
        d->avail[2 + idx % d->qsize] = 0;
        __asm__ volatile("" ::: "memory");
        d->avail[1] = idx + 1;
        outw(d->io + V_QUEUE_NOTIFY, 0);
        u32 spin = 0;
        while (d->used[1] == d->last_used && ++spin < 50000000)
            ;
        if (d->used[1] == d->last_used) {
                        return -EIO;
        }
        d->last_used++;
        inb(d->io + 0x13);                                  /* reading the ISR register drops the interrupt line */
        if (d->hdr[16] != 0)
            return -EIO;
        if (!write)
            memcpy(buf, d->data, c * 512);
        lba += c;
        n -= c;
        buf = (u8 *)buf + c * 512;
    }
    return 0;
}

int virtio_blk_probe(void)
{
    int found = 0;
    struct pcidev p;
    for (int i = 0; pci_find_class(0x010000, i, &p) == 0; i++) {
        u32 id = pci_cfg_read(&p, 0);
        if ((id & 0xFFFF) != 0x1AF4 || ((id >> 16) != 0x1001 && (id >> 16) != 0x1042))
            continue;
        u32 bar = pci_cfg_read(&p, 0x10);
        if (!(bar & 1))
            continue;                                       /* modern-only device: no legacy ports */
        struct vblk *d = kzalloc(sizeof *d);
        if (!d)
            continue;
        d->io = bar & ~3u;
        pci_cfg_write(&p, 0x04, pci_cfg_read(&p, 0x04) | 0x7);
        u16 io = d->io;
        outb(io + V_STATUS, 0);
        outb(io + V_STATUS, 1);                             /* acknowledge */
        outb(io + V_STATUS, 1 | 2);                         /* driver */
        outl(io + V_GUEST_FEAT, 0);                         /* no optional features */
        outw(io + V_QUEUE_SEL, 0);
        u32 size = inw(io + V_QUEUE_SIZE);
        if (!size || size > 1024)
            continue;
        u32 desc_bytes = 16 * size, avail_bytes = 6 + 2 * size, used_off = align(desc_bytes + avail_bytes, 4096);
        u8 *mem = kalloc_aligned(used_off + align(6 + 8 * size, 4096), 4096);
        d->hdr = kalloc_aligned(32, 16);
        d->data = kalloc_aligned(CHUNK * 512, 4096);
        if (!mem || !d->hdr || !d->data)
            continue;
        d->qsize = size;
        d->desc = mem;
        d->avail = (volatile u16 *)(mem + desc_bytes);
        d->used = (volatile u16 *)(mem + used_off);
        d->avail[0] = 1;                                    /* VRING_AVAIL_F_NO_INTERRUPT: we poll */
        outl(io + V_QUEUE_PFN, (u32)((uptr)mem >> 12));
        outb(io + V_STATUS, 1 | 2 | 4);                     /* driver ok */
        u64 cap = (u64)inl(io + V_CONFIG) | ((u64)inl(io + V_CONFIG + 4) << 32);
        if (cap > 0xFFFFFFFFull)
            cap = 0xFFFFFFFFull;
        if (blk_register("virtio disk", (u32)cap, BUS_VIRTIO, vblk_rw, d) >= 0)
            found++;
    }
    return found;
}
