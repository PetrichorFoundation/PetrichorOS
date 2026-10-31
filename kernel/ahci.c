/* AHCI (SATA) host controller driver: one command slot per port, polled DMA
 * through a bounce buffer (user pages aren't physically contiguous). */
#include "kernel.h"

#define GHC      0x04
#define PI       0x0C
#define P_CLB    0x00
#define P_CLBU   0x04
#define P_FB     0x08
#define P_FBU    0x0C
#define P_IS     0x10
#define P_IE     0x14
#define P_CMD    0x18
#define P_TFD    0x20
#define P_SIG    0x24
#define P_SSTS   0x28
#define P_SERR   0x30
#define P_CI     0x38

#define CMD_ST   (1u << 0)
#define CMD_FRE  (1u << 4)
#define CMD_FR   (1u << 14)
#define CMD_CR   (1u << 15)
#define TFD_BSY  0x80
#define TFD_DRQ  0x08
#define TFD_ERR  0x01
#define BOUNCE   (64 * 1024)

struct port {
    volatile u32 *regs;
    u8 *clb, *ctba;
};

static u8 *bounce;

static u32 rd(struct port *p, u32 off) { return p->regs[off / 4]; }
static void wr(struct port *p, u32 off, u32 v) { p->regs[off / 4] = v; }

static int wait_clear(struct port *p, u32 off, u32 mask)
{
    for (int i = 0; i < 1000000; i++)
        if (!(rd(p, off) & mask))
            return 0;
    return -1;
}

/* Issues one command through slot 0; the data buffer must be physically contiguous. */
static int command(struct port *p, u8 cmd, u32 lba, u32 count, void *buf, u32 bytes, int write)
{
    u32 *hdr = (u32 *)p->clb;
    hdr[0] = 5 | (write ? 1u << 6 : 0) | (1u << 16);           /* CFIS = 5 dwords, one PRD entry */
    hdr[1] = 0;
    hdr[2] = (u32)(uptr)p->ctba;
    hdr[3] = 0;

    u8 *ct = p->ctba;
    memset(ct, 0, 0x90);
    ct[0] = 0x27;                                              /* register host-to-device FIS */
    ct[1] = 0x80;                                              /* command, not control */
    ct[2] = cmd;
    ct[4] = lba;
    ct[5] = lba >> 8;
    ct[6] = lba >> 16;
    ct[7] = 0x40;                                              /* LBA mode */
    ct[8] = lba >> 24;
    ct[12] = count;
    ct[13] = count >> 8;
    u32 *prd = (u32 *)(ct + 0x80);
    prd[0] = (u32)(uptr)buf;
    prd[1] = 0;
    prd[2] = 0;
    prd[3] = bytes - 1;

    if (wait_clear(p, P_TFD, TFD_BSY | TFD_DRQ) < 0)
        return -EIO;
    wr(p, P_IS, 0xFFFFFFFF);
    wr(p, P_CI, 1);
    for (int i = 0; i < 5000000; i++) {
        if (!(rd(p, P_CI) & 1))
            return (rd(p, P_TFD) & TFD_ERR) ? -EIO : 0;
        if (rd(p, P_IS) & (1u << 30))                          /* task file error */
            return -EIO;
    }
    return -EIO;
}

static int ahci_rw(void *priv, u32 lba, u32 n, void *buf, int write)
{
    struct port *p = priv;
    u8 *b = buf;
    while (n) {
        u32 chunk = n > BOUNCE / 512 ? BOUNCE / 512 : n;
        if (write)
            memcpy(bounce, b, chunk * 512);
        if (command(p, write ? 0x35 : 0x25, lba, chunk, bounce, chunk * 512, write) < 0)
            return -EIO;                                       /* READ/WRITE DMA EXT */
        if (!write)
            memcpy(b, bounce, chunk * 512);
        lba += chunk;
        b += chunk * 512;
        n -= chunk;
    }
    return 0;
}

static int setup_port(volatile u32 *hba, int port)
{
    struct port *p = kzalloc(sizeof *p);
    if (!p)
        return 0;
    p->regs = hba + (0x100 + port * 0x80) / 4;
    u32 ssts = rd(p, P_SSTS);
    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1 || rd(p, P_SIG) != 0x00000101)
        return 0;                                              /* nothing there, or not a SATA disk */

    wr(p, P_CMD, rd(p, P_CMD) & ~(CMD_ST | CMD_FRE));         /* stop the engine before reprogramming */
    if (wait_clear(p, P_CMD, CMD_FR | CMD_CR) < 0)
        return 0;
    p->clb = kalloc_aligned(1024, 1024);
    u8 *fis = kalloc_aligned(256, 256);
    p->ctba = kalloc_aligned(256, 128);
    if (!p->clb || !fis || !p->ctba)
        return 0;
    wr(p, P_CLB, (u32)(uptr)p->clb);
    wr(p, P_CLBU, 0);
    wr(p, P_FB, (u32)(uptr)fis);
    wr(p, P_FBU, 0);
    wr(p, P_SERR, 0xFFFFFFFF);
    wr(p, P_IS, 0xFFFFFFFF);
    wr(p, P_IE, 0);                                            /* we poll */
    wr(p, P_CMD, rd(p, P_CMD) | CMD_FRE);
    wr(p, P_CMD, rd(p, P_CMD) | CMD_ST);

    u16 id[256];
    if (command(p, 0xEC, 0, 0, bounce, 512, 0) < 0)            /* IDENTIFY DEVICE */
        return 0;
    memcpy(id, bounce, 512);
    char model[41];
    for (int i = 0; i < 20; i++) {
        model[i * 2] = id[27 + i] >> 8;
        model[i * 2 + 1] = id[27 + i] & 0xFF;
    }
    model[40] = 0;
    for (int i = 39; i >= 0 && model[i] == ' '; i--)
        model[i] = 0;
    u32 sectors = id[60] | ((u32)id[61] << 16);
    if (id[83] & (1 << 10)) {                                  /* 48-bit LBA */
        u32 hi = id[102] | ((u32)id[103] << 16);
        sectors = hi ? 0xFFFFFFFFu : (id[100] | ((u32)id[101] << 16));
    }
    if (!sectors)
        return 0;
    return blk_register(model, sectors, BUS_SATA, ahci_rw, p) >= 0;
}

int ahci_probe(void)
{
    int found = 0;
    struct pcidev dev;
    for (int i = 0; pci_find_class(0x010601, i, &dev) == 0; i++) {
        pci_enable(&dev);
        u32 abar = pci_cfg_read(&dev, 0x24) & ~0xFu;
        if (!abar)
            continue;
        volatile u32 *hba = mmio_map(abar, 0x2000);
        if (!hba)
            continue;
        if (!bounce && !(bounce = kalloc_aligned(BOUNCE, 4)))
            return found;
        hba[GHC / 4] |= 1u << 31;                              /* AHCI enable */
        u32 pi = hba[PI / 4];
        for (int port = 0; port < 32; port++)
            if (pi & (1u << port))
                found += setup_port(hba, port);
    }
    return found;
}
