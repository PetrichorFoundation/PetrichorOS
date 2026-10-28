/* ATA PIO driver for the primary master (LBA28, polled, no interrupts). */
#include "kernel.h"

#define ATA_DATA   0x1F0
#define ATA_COUNT  0x1F2
#define ATA_LBA0   0x1F3
#define ATA_LBA1   0x1F4
#define ATA_LBA2   0x1F5
#define ATA_DRIVE  0x1F6
#define ATA_CMD    0x1F7
#define ATA_STATUS 0x1F7
#define ATA_CTRL   0x3F6

#define ST_ERR 0x01
#define ST_DRQ 0x08
#define ST_DF  0x20
#define ST_BSY 0x80

static int present;
static u32 total_sectors;

static void delay400ns(void)
{
    for (int i = 0; i < 4; i++)
        inb(ATA_CTRL);
}

static int wait_ready(int need_drq)
{
    for (int i = 0; i < 2000000; i++) {
        u8 s = inb(ATA_STATUS);
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

int ata_init(void)
{
    outb(ATA_CTRL, 0x02);                       /* nIEN: we poll */
    outb(ATA_DRIVE, 0xA0);
    delay400ns();
    outb(ATA_COUNT, 0);
    outb(ATA_LBA0, 0);
    outb(ATA_LBA1, 0);
    outb(ATA_LBA2, 0);
    outb(ATA_CMD, 0xEC);                        /* IDENTIFY */
    u8 s = inb(ATA_STATUS);
    if (s == 0 || s == 0xFF)
        return -1;
    for (int i = 0; i < 2000000 && (inb(ATA_STATUS) & ST_BSY); i++)
        ;
    if (inb(ATA_LBA1) || inb(ATA_LBA2))         /* ATAPI or SATA signature: not ours */
        return -1;
    if (wait_ready(1) < 0)
        return -1;
    u16 id[256];
    insw(ATA_DATA, id, 256);
    total_sectors = id[60] | ((u32)id[61] << 16);
    present = total_sectors != 0;
    return present ? 0 : -1;
}

u32 ata_sectors(void)
{
    return present ? total_sectors : 0;
}

static int ata_rw(u32 lba, u32 n, void *buf, int write)
{
    if (!present || lba + n > total_sectors)
        return -EIO;
    u8 *p = buf;
    for (u32 i = 0; i < n; i++, lba++, p += 512) {
        outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
        outb(ATA_COUNT, 1);
        outb(ATA_LBA0, lba & 0xFF);
        outb(ATA_LBA1, (lba >> 8) & 0xFF);
        outb(ATA_LBA2, (lba >> 16) & 0xFF);
        outb(ATA_CMD, write ? 0x30 : 0x20);
        delay400ns();
        if (wait_ready(1) < 0)
            return -EIO;
        if (write) {
            outsw(ATA_DATA, p, 256);
            outb(ATA_CMD, 0xE7);                /* flush cache: write-through */
            if (wait_ready(0) < 0)
                return -EIO;
        } else {
            insw(ATA_DATA, p, 256);
        }
    }
    return 0;
}

int ata_read(u32 lba, u32 n, void *buf)
{
    return ata_rw(lba, n, buf, 0);
}

int ata_write(u32 lba, u32 n, const void *buf)
{
    return ata_rw(lba, n, (void *)buf, 1);
}
