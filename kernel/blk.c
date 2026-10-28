/* Block devices: a registry that the IDE, AHCI and NVMe drivers fill in, plus
 * detection of the disk we booted from. */
#include "kernel.h"

struct blkdev {
    char name[8];
    char model[41];
    u32 sectors;
    int bus;
    blk_rw_fn rw;
    void *priv;
};

static struct blkdev devs[NBLK];
static int ndev;
int boot_disk = -1;

int blk_register(const char *model, u32 sectors, int bus, blk_rw_fn rw, void *priv)
{
    if (ndev == NBLK)
        return -1;
    struct blkdev *d = &devs[ndev];
    safestrcpy(d->model, model, sizeof d->model);
    d->sectors = sectors;
    d->bus = bus;
    d->rw = rw;
    d->priv = priv;
    d->name[0] = 'h';
    d->name[1] = 'd';
    d->name[2] = 'a' + ndev;
    d->name[3] = 0;
    return ndev++;
}

int blk_present(int dev) { return dev >= 0 && dev < ndev; }
u32 blk_sectors(int dev) { return blk_present(dev) ? devs[dev].sectors : 0; }
const char *blk_model(int dev) { return blk_present(dev) ? devs[dev].model : ""; }
const char *blk_name(int dev) { return blk_present(dev) ? devs[dev].name : "?"; }
int blk_bus(int dev) { return blk_present(dev) ? devs[dev].bus : 0; }

int blk_read(int dev, u32 lba, u32 n, void *buf)
{
    if (!blk_present(dev) || lba + n < lba || lba + n > devs[dev].sectors)
        return -EIO;
    return devs[dev].rw(devs[dev].priv, lba, n, buf, 0);
}

int blk_write(int dev, u32 lba, u32 n, const void *buf)
{
    if (!blk_present(dev) || lba + n < lba || lba + n > devs[dev].sectors)
        return -EIO;
    return devs[dev].rw(devs[dev].priv, lba, n, (void *)buf, 1);
}

/* The bootloader leaves stage 2 in low memory (0x7E00) with a "PSG2" tag and a
 * per-install id.  The disk we booted from holds the same bytes at LBA 1. */
static void find_boot_disk(void)
{
    const u8 *ram = (const u8 *)0x7E00;
    if (memcmp(ram + 28, "PSG2", 4) != 0) {         /* not started by our bootloader (e.g. QEMU -kernel) */
        boot_disk = ndev > 0 ? 0 : -1;
        return;
    }
    static u8 sec[512];
    for (int i = 0; i < ndev; i++) {
        if (blk_read(i, 1, 1, sec) == 0 && !memcmp(sec, ram, 64)) {
            boot_disk = i;
            return;
        }
    }
    boot_disk = -1;                                  /* CD, USB stick, or a bus we can't drive */
}

int storage_init(void)
{
    ide_probe();
    ahci_probe();
    nvme_probe();
    find_boot_disk();
    static const char *const bus_name[] = {"IDE", "SATA", "NVMe"};
    kprintf("[disk] %d disk%s:", ndev, ndev == 1 ? "" : "s");
    for (int i = 0; i < ndev; i++)
        kprintf(" %s(%s %u MiB)", devs[i].name, bus_name[devs[i].bus], devs[i].sectors >> 11);
    if (ndev)
        kprintf("; booted from %s\n", boot_disk >= 0 ? devs[boot_disk].name : "a removable or unsupported device");
    else
        kprintf("\n");
    return ndev;
}
