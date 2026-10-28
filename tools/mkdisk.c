/* mkdisk: lay the bootloader and kernel onto a raw disk image.
 *
 *   LBA 0        stage 1 (MBR)
 *   LBA 1..15    stage 2
 *   LBA 16..     flat kernel image
 *   LBA 4096..   PFS data partition (created by the kernel on first boot)
 *
 * The image is created if missing and otherwise updated in place, so the
 * data partition survives rebuilds.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECTOR      512
#define STAGE2_LBA  1
#define STAGE2_MAX  15
#define KERNEL_LBA  16
#define PART_LBA    4096
#define DISK_MB     32

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = calloc(1, *len + SECTOR);
    if (fread(buf, 1, *len, f) != *len) {
        perror(path);
        exit(1);
    }
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: mkdisk stage1.bin stage2.bin kernel.bin disk.img\n");
        return 1;
    }
    size_t n1, n2, nk;
    uint8_t *s1 = slurp(argv[1], &n1), *s2 = slurp(argv[2], &n2), *k = slurp(argv[3], &nk);
    if (n1 != SECTOR || s1[510] != 0x55 || s1[511] != 0xAA) {
        fprintf(stderr, "mkdisk: stage1 must be exactly 512 bytes ending in 55 AA\n");
        return 1;
    }
    if (n2 > STAGE2_MAX * SECTOR) {
        fprintf(stderr, "mkdisk: stage2 is %zu bytes, limit %d\n", n2, STAGE2_MAX * SECTOR);
        return 1;
    }
    size_t ksectors = (nk + SECTOR - 1) / SECTOR;
    if (KERNEL_LBA + ksectors > PART_LBA) {
        fprintf(stderr, "mkdisk: kernel too big (%zu sectors)\n", ksectors);
        return 1;
    }

    uint32_t lba = KERNEL_LBA, count = (uint32_t)ksectors;
    memcpy(s2 + 4, &lba, 4);                    /* kernel_lba     */
    memcpy(s2 + 8, &count, 4);                  /* kernel_sectors */

    FILE *d = fopen(argv[4], "r+b");
    if (!d) {
        d = fopen(argv[4], "w+b");
        if (!d) {
            perror(argv[4]);
            return 1;
        }
        static uint8_t zero[1 << 20];
        for (int i = 0; i < DISK_MB; i++)
            fwrite(zero, 1, sizeof zero, d);
    }
    fseek(d, 0, SEEK_SET);
    fwrite(s1, 1, SECTOR, d);
    fseek(d, STAGE2_LBA * SECTOR, SEEK_SET);
    fwrite(s2, 1, STAGE2_MAX * SECTOR > n2 ? n2 : STAGE2_MAX * SECTOR, d);
    fseek(d, KERNEL_LBA * SECTOR, SEEK_SET);
    fwrite(k, 1, ksectors * SECTOR, d);
    fclose(d);
    printf("disk: stage2 %zu B, kernel %zu sectors at LBA %u\n", n2, ksectors, KERNEL_LBA);
    return 0;
}
