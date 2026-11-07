/* mkefi: builds the UEFI boot media pieces.
 *
 *   mkefi fat  esp.img  BOOTX64.EFI  KERNEL.BIN         a FAT16 EFI System Partition image
 *   mkefi gpt  disk.img esp.img [pfs-MiB]               a GPT disk: that ESP, plus (optionally) a data partition
 *
 * The ESP image is what the ISO's El Torito UEFI entry boots; the GPT disk image can be written to a USB
 * stick or attached to a virtual machine.  The real work is in lib/efiimg.c, which the installer shares.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "efiimg.h"

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
    uint8_t *b = calloc(1, *len + 1);
    if (fread(b, 1, *len, f) != *len) {
        perror(path);
        exit(1);
    }
    fclose(f);
    return b;
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "fat")) {
        size_t ne, nk;
        uint8_t *efi = slurp(argv[3], &ne), *kern = slurp(argv[4], &nk);
        uint32_t secs = efi_esp_sectors(ne, nk);
        uint8_t *img = calloc(secs, EFI_SECTOR);
        if (efi_build_esp(img, secs, efi, ne, kern, nk, 0)) {
            fprintf(stderr, "mkefi: the files don't fit in a FAT16 partition\n");
            return 1;
        }
        FILE *f = fopen(argv[2], "wb");
        if (!f || fwrite(img, EFI_SECTOR, secs, f) != secs) {
            perror(argv[2]);
            return 1;
        }
        fclose(f);
        printf("esp: %u KiB FAT16 (BOOTX64.EFI %zu B, KERNEL.BIN %zu B)\n", secs / 2, ne, nk);
        return 0;
    }
    if ((argc == 4 || argc == 5) && !strcmp(argv[1], "gpt")) {
        size_t ne;
        uint8_t *esp = slurp(argv[3], &ne);
        uint64_t esp_secs = ne / EFI_SECTOR, first = 2048, last = first + esp_secs - 1;
        uint64_t pfs_secs = argc == 5 ? (uint64_t)atoi(argv[4]) * 2048 : 0;
        uint64_t pfs_first = (last + 1 + 2047) & ~2047ull, pfs_last = pfs_secs ? pfs_first + pfs_secs - 1 : 0;
        uint64_t total = (pfs_secs ? pfs_last + 1 : last + 1) + 2048;
        uint8_t *d = calloc(total, EFI_SECTOR);
        memcpy(d + first * EFI_SECTOR, esp, ne);
        d[first * EFI_SECTOR + 28] = first; d[first * EFI_SECTOR + 29] = first >> 8;       /* hidden sectors */
        static const uint8_t guid[16] = {'P', 'e', 't', 'r', 'i', 'c', 'h', 'o', 'r', 0, 'D', 'i', 's', 'k', 0, 1};
        efi_build_gpt(d, d + (total - GPT_TAIL_SECTORS) * EFI_SECTOR, total, first, last, pfs_first, pfs_last, guid);
        FILE *f = fopen(argv[2], "wb");
        if (!f || fwrite(d, EFI_SECTOR, total, f) != total) {
            perror(argv[2]);
            return 1;
        }
        fclose(f);
        printf("gpt: %llu MiB disk, EFI System Partition %llu KiB%s\n", (unsigned long long)(total / 2048), (unsigned long long)(esp_secs / 2),
               pfs_secs ? " + data partition" : "");
        return 0;
    }
    fprintf(stderr, "usage: mkefi fat esp.img BOOTX64.EFI KERNEL.BIN | mkefi gpt disk.img esp.img [pfs-MiB]\n");
    return 1;
}
