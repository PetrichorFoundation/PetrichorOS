/* Realtek RTL8139 Fast Ethernet (QEMU: -nic user,model=rtl8139). */
#include "kernel.h"

#define REG_MAC    0x00
#define REG_TSD    0x10
#define REG_TSAD   0x20
#define REG_RBSTART 0x30
#define REG_CMD    0x37
#define REG_CAPR   0x38
#define REG_IMR    0x3C
#define REG_ISR    0x3E
#define REG_RCR    0x44
#define REG_CONFIG1 0x52

#define CMD_RST    0x10
#define CMD_RE     0x08
#define CMD_TE     0x04
#define CMD_BUFE   0x01

#define ISR_ROK    0x01
#define ISR_TOK    0x04
#define TSD_OWN    (1u << 13)

#define RX_RING    8192
#define RX_BUF     (RX_RING + 16 + 1536)

u8 nic_mac[6];
int nic_irq_line = -1;

static u16 io;
static u8 *rx_buf;
static u32 rx_off;
static u8 tx_buf[4][1536] __attribute__((aligned(4)));
static int tx_cur;
static int tx_used[4];

/* ---------------- PCI configuration space ---------------- */

u32 pci_read(u32 bus, u32 slot, u32 fn, u32 off)
{
    outl(0xCF8, 0x80000000u | bus << 16 | slot << 11 | fn << 8 | (off & 0xFC));
    return inl(0xCFC);
}

void pci_write(u32 bus, u32 slot, u32 fn, u32 off, u32 v)
{
    outl(0xCF8, 0x80000000u | bus << 16 | slot << 11 | fn << 8 | (off & 0xFC));
    outl(0xCFC, v);
}

int nic_init(void)
{
    u32 slot;
    for (slot = 0; slot < 32; slot++)
        if (pci_read(0, slot, 0, 0) == 0x813910EC)      /* device 8139, vendor Realtek */
            break;
    if (slot == 32)
        return -ENODEV;

    io = pci_read(0, slot, 0, 0x10) & ~3u;
    nic_irq_line = pci_read(0, slot, 0, 0x3C) & 0xFF;
    pci_write(0, slot, 0, 0x04, pci_read(0, slot, 0, 0x04) | 0x5);   /* I/O space + bus master */

    outb(io + REG_CONFIG1, 0);                          /* power on */
    outb(io + REG_CMD, CMD_RST);
    for (int i = 0; i < 100000 && (inb(io + REG_CMD) & CMD_RST); i++)
        ;

    for (int i = 0; i < 6; i++)
        nic_mac[i] = inb(io + REG_MAC + i);

    rx_buf = kzalloc(RX_BUF);
    if (!rx_buf)
        return -ENOMEM;
    outl(io + REG_RBSTART, (u32)rx_buf);
    outw(io + REG_IMR, ISR_ROK | ISR_TOK);
    outl(io + REG_RCR, 0x0F | (1 << 7));                /* accept all/multicast/broadcast/mine; WRAP */
    outb(io + REG_CMD, CMD_RE | CMD_TE);
    rx_off = 0;
    irq_unmask(nic_irq_line);
    return 0;
}

int nic_send(const u8 *frame, u32 len)
{
    if (!io || len > 1514)
        return -EINVAL;
    int d = tx_cur;
    if (tx_used[d]) {
        int spin = 0;
        while (!(inl(io + REG_TSD + 4 * d) & TSD_OWN) && ++spin < 1000000)
            ;
    }
    memcpy(tx_buf[d], frame, len);
    u32 n = len < 60 ? 60 : len;                        /* pad runt frames */
    if (n > len)
        memset(tx_buf[d] + len, 0, n - len);
    outl(io + REG_TSAD + 4 * d, (u32)tx_buf[d]);
    outl(io + REG_TSD + 4 * d, n);                      /* writing the size starts DMA */
    tx_used[d] = 1;
    tx_cur = (d + 1) & 3;
    return 0;
}

void nic_irq(void)
{
    u16 isr = inw(io + REG_ISR);
    outw(io + REG_ISR, isr);                            /* acknowledge */
    if (!(isr & ISR_ROK))
        return;
    while (!(inb(io + REG_CMD) & CMD_BUFE)) {
        u8 *p = rx_buf + rx_off;
        u16 status = p[0] | (p[1] << 8);
        u16 len = p[2] | (p[3] << 8);                   /* includes the 4-byte CRC */
        if (!(status & 1) || len < 18 || len > 1600) {  /* corrupt: resynchronise */
            outb(io + REG_CMD, CMD_TE);
            outb(io + REG_CMD, CMD_RE | CMD_TE);
            rx_off = 0;
            outw(io + REG_CAPR, (u16)(0 - 16));
            break;
        }
        net_rx(p + 4, len - 4);
        rx_off = (rx_off + len + 4 + 3) & ~3u;
        if (rx_off >= RX_RING)
            rx_off -= RX_RING;
        outw(io + REG_CAPR, (u16)(rx_off - 16));
    }
}
