/* mkefi: builds the UEFI boot media pieces.
 *
 *   mkefi fat  esp.img  BOOTX64.EFI  KERNEL.BIN      a FAT16 EFI System Partition image:
 *                                                    /EFI/BOOT/BOOTX64.EFI and /KERNEL.BIN
 *   mkefi gpt  disk.img esp.img                      a GPT disk image with that image as its ESP
 *
 * The ESP image is what the ISO's El Torito UEFI entry boots; the GPT disk image can be written to a USB
 * stick or attached to a virtual machine.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEC 512

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

static void put16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

/* ---- FAT16 ---- */

static uint8_t *img;
static uint32_t total_secs, fat_secs, root_secs = 32, data_start, next_cluster = 2;

static uint16_t *fat(void) { return (uint16_t *)(img + SEC); }

/* Allocates a chain for `len` bytes (at least one cluster), copies data in, returns the first cluster. */
static uint32_t put_file(const uint8_t *data, size_t len)
{
    uint32_t n = len ? (uint32_t)((len + SEC - 1) / SEC) : 1, first = next_cluster;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = next_cluster++;
        fat()[c] = i + 1 < n ? c + 1 : 0xFFFF;
        size_t off = (size_t)i * SEC, take = len > off ? (len - off < SEC ? len - off : SEC) : 0;
        if (take)
            memcpy(img + (size_t)(data_start + c - 2) * SEC, data + off, take);
    }
    return first;
}

static void dirent(uint8_t *e, const char *name83, int is_dir, uint32_t cluster, uint32_t size)
{
    memset(e, 0, 32);
    memcpy(e, name83, 11);
    e[11] = is_dir ? 0x10 : 0x20;
    put16(e + 14, 0x6000); put16(e + 16, (2026 - 1980) << 9 | 11 << 5 | 7);        /* created 2026-11-07 */
    put16(e + 18, (2026 - 1980) << 9 | 11 << 5 | 7);
    put16(e + 22, 0x6000); put16(e + 24, (2026 - 1980) << 9 | 11 << 5 | 7);
    put16(e + 26, cluster);
    put32(e + 28, size);
}

/* A directory with "." and ".." and `n` further entries, in a fresh cluster. */
static uint32_t make_dir(uint32_t parent, const char *names[], int is_dir[], uint32_t clusters[], uint32_t sizes[], int n)
{
    uint8_t buf[SEC] = {0};
    uint32_t me = next_cluster;
    dirent(buf, ".          ", 1, me, 0);
    dirent(buf + 32, "..         ", 1, parent, 0);
    for (int i = 0; i < n; i++)
        dirent(buf + 64 + i * 32, names[i], is_dir[i], clusters[i], sizes[i]);
    put_file(buf, SEC);
    return me;
}

static void make_fat(const char *out, const char *efi_path, const char *kernel_path)
{
    size_t ne, nk;
    uint8_t *efi = slurp(efi_path, &ne), *kern = slurp(kernel_path, &nk);
    size_t need = ne + nk + 8 * SEC;
    size_t bytes = need * 13 / 10 + (1 << 20);
    if (bytes < (4u << 20)) bytes = 4u << 20;
    if (bytes > (32u << 20)) {
        fprintf(stderr, "mkefi: files too big for the FAT16 image (%zu bytes)\n", need);
        exit(1);
    }
    total_secs = (uint32_t)(bytes / SEC);
    fat_secs = (total_secs * 2 + SEC - 1) / SEC + 1;
    data_start = 1 + 2 * fat_secs + root_secs;
    uint32_t clusters = total_secs - data_start;
    if (clusters < 4085 || clusters > 65524) {
        fprintf(stderr, "mkefi: bad cluster count %u\n", clusters);
        exit(1);
    }
    img = calloc(total_secs, SEC);

    uint8_t *bs = img;                                                  /* BIOS parameter block */
    bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90;
    memcpy(bs + 3, "PETRICHO", 8);
    put16(bs + 11, SEC);
    bs[13] = 1;                                                         /* sectors per cluster */
    put16(bs + 14, 1);                                                  /* reserved sectors */
    bs[16] = 2;                                                         /* FATs */
    put16(bs + 17, 512);                                                /* root directory entries */
    put16(bs + 19, total_secs < 65536 ? total_secs : 0);
    bs[21] = 0xF8;
    put16(bs + 22, fat_secs);
    put16(bs + 24, 32); put16(bs + 26, 64);
    put32(bs + 28, 0);                                                  /* hidden sectors (patched for GPT) */
    put32(bs + 32, total_secs < 65536 ? 0 : total_secs);
    bs[36] = 0x80; bs[38] = 0x29;
    put32(bs + 39, 0x50455452);
    memcpy(bs + 43, "PETRICHOR  ", 11);
    memcpy(bs + 54, "FAT16   ", 8);
    bs[510] = 0x55; bs[511] = 0xAA;

    fat()[0] = 0xFFF8;
    fat()[1] = 0xFFFF;

    uint32_t efi_c = put_file(efi, ne), kern_c = put_file(kern, nk);
    const char *boot_names[] = {"BOOTX64 EFI"};
    int boot_dir[] = {0};
    uint32_t boot_cl[] = {efi_c}, boot_sz[] = {(uint32_t)ne};
    uint32_t boot = make_dir(0, boot_names, boot_dir, boot_cl, boot_sz, 1);
    const char *efi_names[] = {"BOOT       "};
    int efi_dir[] = {1};
    uint32_t efi_cl[] = {boot}, efi_sz[] = {0};
    uint32_t efid = make_dir(0, efi_names, efi_dir, efi_cl, efi_sz, 1);
    /* ".." of BOOT should be EFI, not the root */
    put16(img + (size_t)(data_start + boot - 2) * SEC + 32 + 26, efid);

    uint8_t *root = img + (size_t)(1 + 2 * fat_secs) * SEC;
    dirent(root, "PETRICHOR  ", 0, 0, 0);
    root[11] = 0x08;                                                    /* volume label */
    dirent(root + 32, "EFI        ", 1, efid, 0);
    dirent(root + 64, "KERNEL  BIN", 0, kern_c, (uint32_t)nk);
    memcpy(img + (size_t)(1 + fat_secs) * SEC, img + SEC, (size_t)fat_secs * SEC);   /* the second FAT */

    FILE *f = fopen(out, "wb");
    if (!f || fwrite(img, SEC, total_secs, f) != total_secs) {
        perror(out);
        exit(1);
    }
    fclose(f);
    printf("esp: %u KiB FAT16 (BOOTX64.EFI %zu B, KERNEL.BIN %zu B)\n", total_secs / 2, ne, nk);
}

/* ---- GPT ---- */

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFF;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? (c >> 1) ^ 0xEDB88320 : c >> 1;
    }
    return ~c;
}

static const uint8_t ESP_TYPE[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};
static const uint8_t DISK_GUID[16] = {0x50, 0x65, 0x74, 0x72, 0x69, 0x63, 0x68, 0x6F, 0x72, 0x00, 0x44, 0x69, 0x73, 0x6B, 0x00, 0x01};
static const uint8_t PART_GUID[16] = {0x50, 0x65, 0x74, 0x72, 0x69, 0x63, 0x68, 0x6F, 0x72, 0x00, 0x45, 0x53, 0x50, 0x00, 0x00, 0x02};

static void make_gpt(const char *out, const char *esp_path)
{
    size_t ne;
    uint8_t *esp = slurp(esp_path, &ne);
    uint64_t esp_secs = ne / SEC, first = 2048, last = first + esp_secs - 1, total = last + 1 + 2048;
    uint8_t *d = calloc(total, SEC);
    memcpy(d + first * SEC, esp, ne);
    put32(d + first * SEC + 28, (uint32_t)first);                       /* the FAT's hidden-sectors field */

    uint8_t *mbr = d;                                                   /* protective MBR */
    mbr[446 + 1] = 0; mbr[446 + 2] = 2; mbr[446 + 3] = 0;
    mbr[446 + 4] = 0xEE;
    mbr[446 + 5] = 0xFF; mbr[446 + 6] = 0xFF; mbr[446 + 7] = 0xFF;
    put32(mbr + 446 + 8, 1);
    put32(mbr + 446 + 12, total - 1 > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)(total - 1));
    mbr[510] = 0x55; mbr[511] = 0xAA;

    uint8_t *ents = d + 2 * SEC;                                        /* one partition entry */
    memcpy(ents, ESP_TYPE, 16);
    memcpy(ents + 16, PART_GUID, 16);
    put64(ents + 32, first);
    put64(ents + 40, last);
    const char *name = "EFI System";
    for (int i = 0; name[i]; i++) ents[56 + i * 2] = name[i];
    uint32_t ents_crc = crc32(ents, 128 * 128);

    for (int copy = 0; copy < 2; copy++) {                              /* primary and backup header */
        uint8_t *h = copy ? d + (total - 1) * SEC : d + SEC;
        memcpy(h, "EFI PART", 8);
        put32(h + 8, 0x00010000);
        put32(h + 12, 92);
        put64(h + 24, copy ? total - 1 : 1);                            /* my LBA */
        put64(h + 32, copy ? 1 : total - 1);                            /* the other header */
        put64(h + 40, 34);
        put64(h + 48, total - 34);
        memcpy(h + 56, DISK_GUID, 16);
        put64(h + 72, copy ? total - 33 : 2);
        put32(h + 80, 128);
        put32(h + 84, 128);
        put32(h + 88, ents_crc);
        put32(h + 16, 0);
        put32(h + 16, crc32(h, 92));
    }
    memcpy(d + (total - 33) * SEC, ents, 128 * 128);                    /* backup entries */

    FILE *f = fopen(out, "wb");
    if (!f || fwrite(d, SEC, total, f) != total) {
        perror(out);
        exit(1);
    }
    fclose(f);
    printf("gpt: %llu MiB disk with a %llu KiB EFI System Partition\n", (unsigned long long)(total / 2048), (unsigned long long)(esp_secs / 2));
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "fat")) {
        make_fat(argv[2], argv[3], argv[4]);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "gpt")) {
        make_gpt(argv[2], argv[3]);
        return 0;
    }
    fprintf(stderr, "usage: mkefi fat esp.img BOOTX64.EFI KERNEL.BIN | mkefi gpt disk.img esp.img\n");
    return 1;
}
