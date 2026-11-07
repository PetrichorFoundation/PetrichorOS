/* Block devices: a registry that the IDE, AHCI and NVMe drivers fill in, plus
 * detection of the disk we booted from. */
#include "kernel.h"
#include "ptypes.h"

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
u32 pfs_base_lba = 4096, pfs_sectors;       /* the data partition: BIOS installs put it at LBA 4096 and run to the end of the disk */

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

/* The install medium itself (an ISO written to a USB stick or disk) holds an ISO 9660 volume descriptor at 32 KiB.
 * The installer must never offer to overwrite it, so it counts as the boot disk for that purpose. */
int live_disk = -1;

static void find_live_disk(void)
{
    static u8 sec[512];
    for (int i = 0; i < ndev; i++)
        if (devs[i].bus == BUS_USB && blk_read(i, 64, 1, sec) == 0 && !memcmp(sec + 1, "CD001", 5))
            live_disk = i;
}

/* The bootloader leaves stage 2 in low memory (0x7E00) with a "PSG2" tag and a
 * per-install id.  The disk we booted from holds the same bytes at LBA 1. */
static void find_boot_disk(void)
{
    const u8 *ram = (const u8 *)0x7E00;
    if (bootinfo.uefi) {
        /* Booted through UEFI: the disk's layout is a GPT.  Use a disk that has Petrichor's data partition (an installed
         * system); a live USB stick isn't visible to us, and an ESP is never touched. */
        static const u8 pfs_type[16] = PFS_TYPE_GUID_BYTES;
        static u8 hdr[512], ents[512];
        boot_disk = -1;
        for (int i = 0; i < ndev && boot_disk < 0; i++) {
            if (i == live_disk)
                continue;
            if (blk_read(i, 1, 1, hdr) || memcmp(hdr, "EFI PART", 8))
                continue;
            u32 elba = *(u32 *)(hdr + 72), n = *(u32 *)(hdr + 80), esz = *(u32 *)(hdr + 84);
            if (esz < 128 || esz > 512 || n > 128)
                continue;
            for (u32 k = 0; k < n && boot_disk < 0; k++) {
                if ((k * esz) % 512 == 0 && blk_read(i, elba + k * esz / 512, 1, ents))
                    break;
                const u8 *e = ents + (k * esz) % 512;
                if (!memcmp(e, pfs_type, 16)) {
                    u64 first = *(u64 *)(e + 32), last = *(u64 *)(e + 40);
                    if (last > first && last < 0xFFFFFFFFull) {
                        pfs_base_lba = (u32)first;
                        pfs_sectors = (u32)(last - first + 1);
                        boot_disk = i;
                    }
                }
            }
        }
        return;
    }
    if (memcmp(ram + 28, "PSG2", 4) != 0) {         /* not started by our bootloader (e.g. QEMU -kernel) */
        boot_disk = ndev > 0 ? 0 : -1;
        return;
    }
    static u8 sec[512];
    for (int i = 0; i < ndev; i++) {
        if (i != live_disk && blk_read(i, 1, 1, sec) == 0 && !memcmp(sec, ram, 64)) {
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
    virtio_blk_probe();
    usb_init();
    find_live_disk();
    find_boot_disk();
    static const char *const bus_name[] = {"IDE", "SATA", "NVMe", "USB", "virtio"};
    kprintf("[disk] %d disk%s:", ndev, ndev == 1 ? "" : "s");
    for (int i = 0; i < ndev; i++)
        kprintf(" %s(%s %u MiB)", devs[i].name, bus_name[devs[i].bus], devs[i].sectors >> 11);
    if (ndev)
        kprintf("; booted from %s\n", boot_disk >= 0 ? devs[boot_disk].name
                                       : live_disk >= 0 ? "the install USB stick (protected)" : "a removable or unsupported device");
    else
        kprintf("\n");
    return ndev;
}
