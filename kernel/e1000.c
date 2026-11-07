/* Intel PRO/1000 gigabit Ethernet (the "e1000" family): QEMU's e1000 and e1000e, VirtualBox's default
 * card, VMware's e1000, and many physical Intel NICs.  Legacy descriptors, polled and interrupt driven. */
#include "kernel.h"

#define R_CTRL  0x0000
#define R_STATUS 0x0008
#define R_EERD  0x0014
#define R_ICR   0x00C0
#define R_IMS   0x00D0
#define R_IMC   0x00D8
#define R_RCTL  0x0100
#define R_TCTL  0x0400
#define R_TIPG  0x0410
#define R_RDBAL 0x2800
#define R_RDBAH 0x2804
#define R_RDLEN 0x2808
#define R_RDH   0x2810
#define R_RDT   0x2818
#define R_RXDCTL 0x2828
#define R_TDBAL 0x3800
#define R_TDBAH 0x3804
#define R_TDLEN 0x3808
#define R_TDH   0x3810
#define R_TDT   0x3818
#define R_TXDCTL 0x3828
#define R_MTA   0x5200
#define R_RAL0  0x5400
#define R_RAH0  0x5404

#define CTRL_SLU  (1u << 6)
#define CTRL_ASDE (1u << 5)
#define CTRL_RST  (1u << 26)

#define NRX 32
#define NTX 8
#define BUFSZ 2048

struct rxd {
    u64 addr;
    u16 len, csum;
    u8 status, errors;
    u16 special;
} __attribute__((packed));

struct txd {
    u64 addr;
    u16 len;
    u8 cso, cmd, status, css;
    u16 special;
} __attribute__((packed));

static volatile u32 *regs;
static struct rxd *rx;
static struct txd *tx;
static u8 *rx_bufs, *tx_bufs;
static u32 rx_cur, tx_cur;
static int tx_used[NTX];

static u32 rd(u32 r) { return regs[r / 4]; }
static void wr(u32 r, u32 v) { regs[r / 4] = v; }

static int supported(u32 dev)
{
    static const u16 ids[] = {
        0x1000, 0x1001, 0x1004, 0x1008, 0x1009, 0x100C, 0x100D, 0x100E, 0x100F, 0x1010, 0x1011, 0x1012, 0x1013,
        0x1014, 0x1015, 0x1016, 0x1017, 0x1018, 0x1019, 0x101A, 0x101D, 0x101E, 0x1026, 0x1027, 0x1028, 0x1075,
        0x1076, 0x1077, 0x1078, 0x1079, 0x107A, 0x107B, 0x107C, 0x10B5, 0x10D3,
        0x105E, 0x105F, 0x1060, 0x107D, 0x107E, 0x107F, 0x10A4, 0x10A5, 0x10BC, 0x10D5, 0x150C,            /* 82571/2/3/83 */
        0x1533, 0x1536, 0x1537, 0x1538, 0x1539, 0x157B, 0x157C,                                          /* I210, I211 */
    };
    for (u32 i = 0; i < sizeof ids / sizeof ids[0]; i++)
        if (ids[i] == dev)
            return 1;
    return 0;
}

static u16 eeprom_word(int w)
{
    for (int fmt = 0; fmt < 2; fmt++) {                     /* 82540-style (done bit 4) and 82574-style (done bit 1) */
        wr(R_EERD, 1 | (u32)w << (fmt ? 2 : 8));
        for (int i = 0; i < 100000; i++) {
            u32 v = rd(R_EERD);
            if (v & (fmt ? 2u : 16u))
                return v >> 16;
        }
    }
    return 0;
}

int e1000_init(void)
{
    struct pcidev d;
    int found = 0;
    for (int i = 0; !found && pci_find_class(0x020000, i, &d) == 0; i++) {
        u32 id = pci_cfg_read(&d, 0);
        if ((id & 0xFFFF) == 0x8086 && supported(id >> 16))
            found = id >> 16;
    }
    if (!found)
        return -ENODEV;

    u32 bar = pci_cfg_read(&d, 0x10);
    if (bar & 1)
        return -ENODEV;
    pci_enable(&d);
    regs = mmio_map(bar & ~0xFu, 0x20000);
    if (!regs)
        return -ENOMEM;

    wr(R_IMC, 0xFFFFFFFFu);
    wr(R_RCTL, 0);
    wr(R_TCTL, 0);
    wr(R_CTRL, rd(R_CTRL) | CTRL_RST);
    for (int i = 0; i < 200000; i++)
        (void)rd(R_STATUS);
    wr(R_IMC, 0xFFFFFFFFu);
    (void)rd(R_ICR);
    wr(R_CTRL, (rd(R_CTRL) | CTRL_SLU | CTRL_ASDE) & ~((1u << 3) | (1u << 31) | (1u << 30) | (1u << 7)));

    u32 ral = rd(R_RAL0), rah = rd(R_RAH0);
    if (rah & 0x80000000u) {
        for (int i = 0; i < 4; i++) nic_mac[i] = ral >> (8 * i);
        for (int i = 0; i < 2; i++) nic_mac[4 + i] = rah >> (8 * i);
    } else {
        for (int w = 0; w < 3; w++) {
            u16 v = eeprom_word(w);
            nic_mac[2 * w] = v;
            nic_mac[2 * w + 1] = v >> 8;
        }
        wr(R_RAL0, nic_mac[0] | nic_mac[1] << 8 | nic_mac[2] << 16 | (u32)nic_mac[3] << 24);
        wr(R_RAH0, nic_mac[4] | nic_mac[5] << 8 | 0x80000000u);
    }
    for (int i = 0; i < 128; i++)
        wr(R_MTA + 4 * i, 0);

    rx = kalloc_aligned(NRX * sizeof *rx, 128);
    tx = kalloc_aligned(NTX * sizeof *tx, 128);
    rx_bufs = kalloc_aligned(NRX * BUFSZ, 16);
    tx_bufs = kalloc_aligned(NTX * BUFSZ, 16);
    if (!rx || !tx || !rx_bufs || !tx_bufs)
        return -ENOMEM;
    for (int i = 0; i < NRX; i++)
        rx[i].addr = (u64)(uptr)(rx_bufs + i * BUFSZ);
    for (int i = 0; i < NTX; i++)
        tx[i].addr = (u64)(uptr)(tx_bufs + i * BUFSZ);

    wr(R_RDBAL, (u32)(uptr)rx);
    wr(R_RDBAH, 0);
    wr(R_RDLEN, NRX * sizeof *rx);
    wr(R_RDH, 0);
    wr(R_RDT, NRX - 1);
    rx_cur = 0;
    wr(R_TDBAL, (u32)(uptr)tx);
    wr(R_TDBAH, 0);
    wr(R_TDLEN, NTX * sizeof *tx);
    wr(R_TDH, 0);
    wr(R_TDT, 0);
    tx_cur = 0;
    if (found == 0x10D3) {                                  /* 82574: queues must be enabled explicitly */
        wr(R_RXDCTL, rd(R_RXDCTL) | (1u << 25));
        wr(R_TXDCTL, rd(R_TXDCTL) | (1u << 25));
    }
    wr(R_RCTL, 0x2 | 0x8000 | 0x4000000);                   /* enable, accept broadcast, strip CRC, 2 KiB buffers */
    wr(R_TIPG, 0x0060200A);
    wr(R_TCTL, 0x2 | 0x8 | (0xF << 4) | (0x3F << 12));      /* enable, pad short packets, collision settings */

    nic_irq_line = pci_cfg_read(&d, 0x3C) & 0xFF;
    if (nic_irq_line < 16) {
        wr(R_IMS, 0x80 | 0x10 | 0x40 | 0x4);                /* receive timer/threshold/overrun, link change */
        irq_unmask(nic_irq_line);
    } else {
        nic_irq_line = -1;                                  /* no usable interrupt: the tick polls instead */
    }
    return 0;
}

int e1000_send(const u8 *frame, u32 len)
{
    if (!regs || len > 1514)
        return -EINVAL;
    u32 i = tx_cur;
    if (tx_used[i]) {
        int spin = 0;
        while (!(tx[i].status & 1) && ++spin < 1000000)
            ;
    }
    memcpy(tx_bufs + i * BUFSZ, frame, len);
    u32 n = len < 60 ? 60 : len;
    if (n > len)
        memset(tx_bufs + i * BUFSZ + len, 0, n - len);
    tx[i].len = n;
    tx[i].cmd = 0x0B;                                       /* end of packet, insert FCS, report status */
    tx[i].status = 0;
    tx_used[i] = 1;
    tx_cur = (i + 1) % NTX;
    wr(R_TDT, tx_cur);
    return 0;
}

void e1000_poll(void)
{
    if (!regs)
        return;
    while (rx[rx_cur].status & 1) {
        struct rxd *d = &rx[rx_cur];
        if (d->len > 0 && !d->errors)
            net_rx(rx_bufs + rx_cur * BUFSZ, d->len);
        d->status = 0;
        wr(R_RDT, rx_cur);
        rx_cur = (rx_cur + 1) % NRX;
    }
}

void e1000_irq(void)
{
    u32 icr = rd(R_ICR);                                    /* reading clears it */
    random_event(icr);
    e1000_poll();
}
