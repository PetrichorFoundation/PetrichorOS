/* ATA PIO driver for up to four drives (primary/secondary, master/slave).
 * LBA28, polled, no interrupts. */
#include "kernel.h"

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF  0x20
#define ST_BSY 0x80

struct drive {
    u16 io, ctrl;
    u8 slave, present;
    u32 sectors;
    char model[41];
};

static struct drive drv[NDRIVES] = {
    {.io = 0x1F0, .ctrl = 0x3F6}, {.io = 0x1F0, .ctrl = 0x3F6, .slave = 1},
    {.io = 0x170, .ctrl = 0x376}, {.io = 0x170, .ctrl = 0x376, .slave = 1},
};

static void delay400ns(struct drive *d)
{
    for (int i = 0; i < 4; i++)
        inb(d->ctrl);
}

static int wait_ready(struct drive *d, int need_drq)
{
    for (int i = 0; i < 2000000; i++) {
        u8 s = inb(d->io + 7);
        if (s == 0xFF || (s & ST_BSY))
            continue;
        if (s & (ST_ERR | ST_DF))
            return -1;
        if (!need_drq || (s & ST_DRQ))
            return 0;
    }
    return -1;
}

static void insw(u16 port, void *buf, u32 words)
{
    __asm__ volatile("cld; rep insw" : "+D"(buf), "+c"(words) : "d"(port) : "memory", "cc");
}

static void outsw(u16 port, const void *buf, u32 words)
{
    __asm__ volatile("cld; rep outsw" : "+S"(buf), "+c"(words) : "d"(port) : "memory", "cc");
}

static int identify(struct drive *d)
{
    outb(d->ctrl, 0x02);                        /* nIEN: we poll */
    outb(d->io + 6, 0xA0 | (d->slave << 4));
    delay400ns(d);
    for (int r = 2; r <= 5; r++)
        outb(d->io + r, 0);
    outb(d->io + 7, 0xEC);                      /* IDENTIFY */
    u8 s = inb(d->io + 7);
    if (s == 0 || s == 0xFF)
        return -1;
    for (int i = 0; i < 2000000 && (inb(d->io + 7) & ST_BSY); i++)
        ;
    if (inb(d->io + 4) || inb(d->io + 5))       /* ATAPI/SATA signature: not a plain disk */
        return -1;
    if (wait_ready(d, 1) < 0)
        return -1;
    u16 id[256];
    insw(d->io, id, 256);
    d->sectors = id[60] | ((u32)id[61] << 16);
    for (int i = 0; i < 20; i++) {              /* model string: 20 words, bytes swapped */
        d->model[i * 2] = id[27 + i] >> 8;
        d->model[i * 2 + 1] = id[27 + i] & 0xFF;
    }
    d->model[40] = 0;
    for (int i = 39; i >= 0 && d->model[i] == ' '; i--)
        d->model[i] = 0;
    return d->sectors ? 0 : -1;
}

/* Probes all four slots; returns how many disks answered. */
int ata_init(void)
{
    int n = 0;
    for (int i = 0; i < NDRIVES; i++) {
        drv[i].present = identify(&drv[i]) == 0;
        n += drv[i].present;
    }
    return n;
}

int ata_present(int dev) { return dev >= 0 && dev < NDRIVES && drv[dev].present; }
u32 ata_sectors(int dev) { return ata_present(dev) ? drv[dev].sectors : 0; }
const char *ata_model(int dev) { return ata_present(dev) ? drv[dev].model : ""; }

static int ata_rw(int dev, u32 lba, u32 n, void *buf, int write)
{
    if (!ata_present(dev) || lba + n > drv[dev].sectors)
        return -EIO;
    struct drive *d = &drv[dev];
    u8 *p = buf;
    for (u32 i = 0; i < n; i++, lba++, p += 512) {
        outb(d->io + 6, 0xE0 | (d->slave << 4) | ((lba >> 24) & 0x0F));
        outb(d->io + 2, 1);
        outb(d->io + 3, lba & 0xFF);
        outb(d->io + 4, (lba >> 8) & 0xFF);
        outb(d->io + 5, (lba >> 16) & 0xFF);
        outb(d->io + 7, write ? 0x30 : 0x20);
        delay400ns(d);
        if (wait_ready(d, 1) < 0)
            return -EIO;
        if (write) {
            outsw(d->io, p, 256);
            outb(d->io + 7, 0xE7);              /* flush cache: write-through */
            if (wait_ready(d, 0) < 0)
                return -EIO;
        } else {
            insw(d->io, p, 256);
        }
    }
    return 0;
}

int ata_read(int dev, u32 lba, u32 n, void *buf)
{
    return ata_rw(dev, lba, n, buf, 0);
}

int ata_write(int dev, u32 lba, u32 n, const void *buf)
{
    return ata_rw(dev, lba, n, (void *)buf, 1);
}
