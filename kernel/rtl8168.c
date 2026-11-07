/* Realtek RTL8111/8168/8411 and RTL8101/8102 Gigabit/Fast Ethernet (PCIe), the most common desktop and laptop
 * network chip.  Descriptor-ring driver over the chip's I/O-port window; it relies on the firmware having brought
 * the PHY up and does none of the per-revision tuning that Linux does.  Not testable in QEMU (it has no model of
 * this chip), so it is unverified on real hardware. */
#include "kernel.h"

#define R_IDR0   0x00
#define R_MAR    0x08
#define R_TNPDS  0x20
#define R_CR     0x37
#define R_TPPOLL 0x38
#define R_IMR    0x3C
#define R_ISR    0x3E
#define R_TCR    0x40
#define R_RCR    0x44
#define R_9346   0x50
#define R_RMS    0xDA
#define R_CPCR   0xE0
#define R_RDSAR  0xE4
#define R_MTPS   0xEC

#define NDESC 64
#define BUFSZ 2048

#define D_OWN (1u << 31)
#define D_EOR (1u << 30)
#define D_FS  (1u << 29)
#define D_LS  (1u << 28)

struct desc {
    volatile u32 opts1, opts2, lo, hi;
};

static u16 io;
static struct desc *rx, *tx;
static u8 *rxbuf, *txbuf;
static u32 rx_i, tx_i;

static void rx_arm(u32 i)
{
    rx[i].lo = (u32)(uptr)(rxbuf + i * BUFSZ);
    rx[i].hi = 0;
    rx[i].opts2 = 0;
    __asm__ volatile("" ::: "memory");
    rx[i].opts1 = D_OWN | (i == NDESC - 1 ? D_EOR : 0) | BUFSZ;
}

int rtl8168_init(void)
{
    struct pcidev d;
    int found = 0;
    for (int i = 0; !found && pci_find_class(0x020000, i, &d) == 0; i++) {
        u32 id = pci_cfg_read(&d, 0);
        u32 dev = id >> 16;
        if ((id & 0xFFFF) == 0x10EC && (dev == 0x8168 || dev == 0x8161 || dev == 0x8136))
            found = 1;
    }
    if (!found)
        return -ENODEV;
    u32 bar = pci_cfg_read(&d, 0x10);
    if (!(bar & 1))
        return -ENODEV;
    io = bar & ~3u;
    pci_cfg_write(&d, 0x04, pci_cfg_read(&d, 0x04) | 0x5);          /* I/O space + bus master */

    outb(io + R_CR, 0x10);                                          /* reset */
    int ok = 0;
    for (int i = 0; i < 100000; i++)
        if (!(inb(io + R_CR) & 0x10)) {
            ok = 1;
            break;
        }
    if (!ok)
        return -ENODEV;
    for (int i = 0; i < 6; i++)
        nic_mac[i] = inb(io + R_IDR0 + i);

    rx = kalloc_aligned(NDESC * sizeof(struct desc), 256);
    tx = kalloc_aligned(NDESC * sizeof(struct desc), 256);
    rxbuf = kalloc_aligned(NDESC * BUFSZ, 16);
    txbuf = kalloc_aligned(NDESC * BUFSZ, 16);
    if (!rx || !tx || !rxbuf || !txbuf)
        return -ENOMEM;
    for (u32 i = 0; i < NDESC; i++)
        rx_arm(i);
    tx[NDESC - 1].opts1 = D_EOR;
    rx_i = tx_i = 0;

    outb(io + R_9346, 0xC0);                                        /* unlock the configuration registers */
    outw(io + R_CPCR, inw(io + R_CPCR) | 0x0008);                   /* PCI multiple read/write */
    outw(io + R_RMS, 0x1FFF);
    outb(io + R_MTPS, 0x3B);
    outl(io + R_TNPDS, (u32)(uptr)tx);
    outl(io + R_TNPDS + 4, 0);
    outl(io + R_RDSAR, (u32)(uptr)rx);
    outl(io + R_RDSAR + 4, 0);
    outb(io + R_CR, 0x0C);                                          /* receiver + transmitter on */
    outl(io + R_TCR, 0x03000700);
    outl(io + R_RCR, 0xE70F);                                       /* accept unicast, multicast, broadcast; no limits */
    outl(io + R_MAR, 0xFFFFFFFF);
    outl(io + R_MAR + 4, 0xFFFFFFFF);
    outb(io + R_9346, 0);
    outw(io + R_ISR, 0xFFFF);
    outw(io + R_IMR, 0x0005 | 0x0002 | 0x0008 | 0x0010);            /* rx ok/error, tx ok/error, rx overflow */

    nic_irq_line = pci_cfg_read(&d, 0x3C) & 0xFF;
    if (nic_irq_line < 16)
        irq_unmask(nic_irq_line);
    else
        nic_irq_line = -1;
    return 0;
}

int rtl8168_send(const u8 *frame, u32 len)
{
    if (!io || len > 1514)
        return -EINVAL;
    u32 i = tx_i;
    int spin = 0;
    while ((tx[i].opts1 & D_OWN) && ++spin < 1000000)
        ;
    if (tx[i].opts1 & D_OWN)
        return -EIO;
    u32 n = len < 60 ? 60 : len;
    u8 *b = txbuf + i * BUFSZ;
    memcpy(b, frame, len);
    if (n > len)
        memset(b + len, 0, n - len);
    tx[i].lo = (u32)(uptr)b;
    tx[i].hi = 0;
    tx[i].opts2 = 0;
    __asm__ volatile("" ::: "memory");
    tx[i].opts1 = D_OWN | D_FS | D_LS | (i == NDESC - 1 ? D_EOR : 0) | n;
    tx_i = (i + 1) % NDESC;
    outb(io + R_TPPOLL, 0x40);                                      /* normal-priority queue: go */
    return 0;
}

void rtl8168_poll(void)
{
    if (!io)
        return;
    for (int guard = 0; guard < NDESC; guard++) {
        u32 o = rx[rx_i].opts1;
        if (o & D_OWN)
            break;
        u32 len = o & 0x3FFF;
        if ((o & D_FS) && (o & D_LS) && !(o & (1u << 21)) && len > 4 && len <= BUFSZ)
            net_rx(rxbuf + rx_i * BUFSZ, len - 4);                  /* drop the CRC */
        rx_arm(rx_i);
        rx_i = (rx_i + 1) % NDESC;
    }
}

void rtl8168_irq(void)
{
    u16 isr = inw(io + R_ISR);
    outw(io + R_ISR, isr);
    random_event(isr);
    rtl8168_poll();
}
