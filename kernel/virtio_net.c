/* virtio network card (legacy interface, I/O ports): QEMU/KVM, VirtualBox, and most cloud virtual machines. */
#include "kernel.h"

#define V_FEATURES   0x00
#define V_GUEST_FEAT 0x04
#define V_QUEUE_PFN  0x08
#define V_QUEUE_SIZE 0x0C
#define V_QUEUE_SEL  0x0E
#define V_QUEUE_NOTIFY 0x10
#define V_STATUS     0x12
#define V_ISR        0x13
#define V_MAC        0x14

#define ST_ACK 1
#define ST_DRIVER 2
#define ST_DRIVER_OK 4
#define F_MAC (1u << 5)

#define HDR 10                                              /* struct virtio_net_hdr without mergeable buffers */
#define BUFSZ 2048
#define NBUF 16

struct vq {
    u16 size;
    u8 *desc;                                               /* 16 bytes each */
    volatile u16 *avail;                                    /* flags, idx, ring[] */
    volatile u16 *used;                                     /* flags, idx, then {u32 id, u32 len} */
    u16 last_used;
};

static u16 io;
static struct vq rxq, txq;
static u8 *rx_bufs, *tx_bufs;
static u32 tx_next, tx_pending;

static u32 align(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }

static int vq_setup(struct vq *q, int index)
{
    outw(io + V_QUEUE_SEL, index);
    u32 size = inw(io + V_QUEUE_SIZE);
    if (!size || size > 1024)
        return -ENODEV;
    u32 desc_bytes = 16 * size, avail_bytes = 6 + 2 * size, used_off = align(desc_bytes + avail_bytes, 4096);
    u32 total = used_off + align(6 + 8 * size, 4096);
    u8 *mem = kalloc_aligned(total, 4096);
    if (!mem)
        return -ENOMEM;
    q->size = size;
    q->desc = mem;
    q->avail = (volatile u16 *)(mem + desc_bytes);
    q->used = (volatile u16 *)(mem + used_off);
    q->last_used = 0;
    outl(io + V_QUEUE_PFN, (u32)((uptr)mem >> 12));
    return 0;
}

static void desc_set(struct vq *q, u32 i, void *addr, u32 len, u16 flags)
{
    u8 *d = q->desc + 16 * i;
    *(u64 *)d = (u64)(uptr)addr;
    *(u32 *)(d + 8) = len;
    *(u16 *)(d + 12) = flags;
    *(u16 *)(d + 14) = 0;
}

static void post_rx(u32 id)
{
    desc_set(&rxq, id, rx_bufs + id * BUFSZ, BUFSZ, 2 /* device writes */);
    u16 idx = rxq.avail[1];
    rxq.avail[2 + idx % rxq.size] = id;
    __asm__ volatile("" ::: "memory");
    rxq.avail[1] = idx + 1;
}

int virtio_net_init(void)
{
    struct pcidev d;
    int found = 0;
    for (int i = 0; !found && pci_find_class(0x020000, i, &d) == 0; i++) {
        u32 id = pci_cfg_read(&d, 0);
        if ((id & 0xFFFF) == 0x1AF4 && ((id >> 16) == 0x1000 || (id >> 16) == 0x1041))
            found = 1;
    }
    if (!found)
        return -ENODEV;
    u32 bar = pci_cfg_read(&d, 0x10);
    if (!(bar & 1))
        return -ENODEV;                                     /* modern-only device: no legacy ports */
    io = bar & ~3u;
    pci_cfg_write(&d, 0x04, pci_cfg_read(&d, 0x04) | 0x7);  /* I/O space, memory, bus master */

    outb(io + V_STATUS, 0);
    outb(io + V_STATUS, ST_ACK);
    outb(io + V_STATUS, ST_ACK | ST_DRIVER);
    u32 feat = inl(io + V_FEATURES);
    outl(io + V_GUEST_FEAT, feat & F_MAC);
    if (vq_setup(&rxq, 0) < 0 || vq_setup(&txq, 1) < 0)
        return -ENODEV;
    rx_bufs = kalloc_aligned(NBUF * BUFSZ, 16);
    tx_bufs = kalloc_aligned(NBUF * BUFSZ, 16);
    if (!rx_bufs || !tx_bufs)
        return -ENOMEM;
    for (int i = 0; i < 6; i++)
        nic_mac[i] = inb(io + V_MAC + i);
    for (u32 i = 0; i < NBUF; i++)
        post_rx(i);
    outb(io + V_STATUS, ST_ACK | ST_DRIVER | ST_DRIVER_OK);
    outw(io + V_QUEUE_NOTIFY, 0);

    nic_irq_line = pci_cfg_read(&d, 0x3C) & 0xFF;
    if (nic_irq_line < 16)
        irq_unmask(nic_irq_line);
    else
        nic_irq_line = -1;
    return 0;
}

int virtio_net_send(const u8 *frame, u32 len)
{
    if (!io || len > 1514)
        return -EINVAL;
    u32 i = tx_next % NBUF;
    if (tx_pending >= NBUF) {                               /* all slots in flight: wait for one to complete */
        int spin = 0;
        while (txq.used[1] == txq.last_used && ++spin < 1000000)
            ;
        while (txq.last_used != txq.used[1]) {
            txq.last_used++;
            tx_pending--;
        }
    }
    u8 *b = tx_bufs + i * BUFSZ;
    memset(b, 0, HDR);
    memcpy(b + HDR, frame, len);
    desc_set(&txq, i, b, HDR + (len < 60 ? 60 : len), 0);
    if (len < 60)
        memset(b + HDR + len, 0, 60 - len);
    u16 idx = txq.avail[1];
    txq.avail[2 + idx % txq.size] = i;
    __asm__ volatile("" ::: "memory");
    txq.avail[1] = idx + 1;
    tx_next++;
    tx_pending++;
    outw(io + V_QUEUE_NOTIFY, 1);
    return 0;
}

void virtio_net_poll(void)
{
    if (!io)
        return;
    while (txq.last_used != txq.used[1]) {                  /* reclaim finished transmits */
        txq.last_used++;
        if (tx_pending)
            tx_pending--;
    }
    while (rxq.last_used != rxq.used[1]) {
        u32 e = rxq.last_used % rxq.size;
        volatile u32 *elem = (volatile u32 *)(rxq.used + 2) + 2 * e;
        u32 id = elem[0], len = elem[1];
        if (id < NBUF && len > HDR)
            net_rx(rx_bufs + id * BUFSZ + HDR, len - HDR);
        if (id < NBUF)
            post_rx(id);
        rxq.last_used++;
    }
    outw(io + V_QUEUE_NOTIFY, 0);
}

void virtio_net_irq(void)
{
    (void)inb(io + V_ISR);                                  /* reading acknowledges the interrupt */
    virtio_net_poll();
}
