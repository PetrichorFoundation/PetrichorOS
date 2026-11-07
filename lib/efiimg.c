#include <string.h>
#include "efiimg.h"
#include "ptypes.h"

#define SEC EFI_SECTOR

static void put16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

uint32_t efi_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFF;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? (c >> 1) ^ 0xEDB88320 : c >> 1;
    }
    return ~c;
}

/* ---- FAT16 ---- */

struct fat {
    uint8_t *img;
    uint32_t fat_secs, data_start, next_cluster;
};

static uint16_t *fat_table(struct fat *f) { return (uint16_t *)(f->img + SEC); }

static uint32_t put_file(struct fat *f, const uint8_t *data, size_t len)
{
    uint32_t n = len ? (uint32_t)((len + SEC - 1) / SEC) : 1, first = f->next_cluster;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t c = f->next_cluster++;
        fat_table(f)[c] = i + 1 < n ? c + 1 : 0xFFFF;
        size_t off = (size_t)i * SEC, take = len > off ? (len - off < SEC ? len - off : SEC) : 0;
        if (take)
            memcpy(f->img + (size_t)(f->data_start + c - 2) * SEC, data + off, take);
    }
    return first;
}

static void dirent(uint8_t *e, const char *name83, int is_dir, uint32_t cluster, uint32_t size)
{
    uint16_t date = (2026 - 1980) << 9 | 11 << 5 | 7;
    memset(e, 0, 32);
    memcpy(e, name83, 11);
    e[11] = is_dir ? 0x10 : 0x20;
    put16(e + 14, 0x6000); put16(e + 16, date);
    put16(e + 18, date);
    put16(e + 22, 0x6000); put16(e + 24, date);
    put16(e + 26, cluster);
    put32(e + 28, size);
}

uint32_t efi_esp_sectors(size_t efi_len, size_t kernel_len)
{
    size_t bytes = (efi_len + kernel_len + 8 * SEC) * 13 / 10 + (1 << 20);
    if (bytes < (4u << 20))
        bytes = 4u << 20;
    return (uint32_t)(bytes / SEC);
}

int efi_build_esp(uint8_t *img, uint32_t total_secs, const uint8_t *efi, size_t efi_len, const uint8_t *kernel,
                  size_t kernel_len, uint32_t hidden)
{
    struct fat f = {.img = img, .next_cluster = 2};
    f.fat_secs = (total_secs * 2 + SEC - 1) / SEC + 1;
    f.data_start = 1 + 2 * f.fat_secs + 32;
    if (total_secs <= f.data_start)
        return -1;
    uint32_t clusters = total_secs - f.data_start;
    if (clusters < 4085 || clusters > 65524)
        return -1;
    if ((efi_len + kernel_len) / SEC + 16 > clusters)
        return -1;

    uint8_t *bs = img;
    bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90;
    memcpy(bs + 3, "PETRICHO", 8);
    put16(bs + 11, SEC);
    bs[13] = 1;
    put16(bs + 14, 1);
    bs[16] = 2;
    put16(bs + 17, 512);
    put16(bs + 19, total_secs < 65536 ? total_secs : 0);
    bs[21] = 0xF8;
    put16(bs + 22, f.fat_secs);
    put16(bs + 24, 32); put16(bs + 26, 64);
    put32(bs + 28, hidden);
    put32(bs + 32, total_secs < 65536 ? 0 : total_secs);
    bs[36] = 0x80; bs[38] = 0x29;
    put32(bs + 39, 0x50455452);
    memcpy(bs + 43, "PETRICHOR  ", 11);
    memcpy(bs + 54, "FAT16   ", 8);
    bs[510] = 0x55; bs[511] = 0xAA;
    fat_table(&f)[0] = 0xFFF8;
    fat_table(&f)[1] = 0xFFFF;

    uint32_t efi_c = put_file(&f, efi, efi_len), kern_c = put_file(&f, kernel, kernel_len);

    uint8_t buf[SEC];                                                   /* /EFI/BOOT */
    memset(buf, 0, sizeof buf);
    uint32_t boot = f.next_cluster;
    dirent(buf, ".          ", 1, boot, 0);
    dirent(buf + 32, "..         ", 1, 0, 0);
    dirent(buf + 64, "BOOTX64 EFI", 0, efi_c, (uint32_t)efi_len);
    put_file(&f, buf, SEC);
    memset(buf, 0, sizeof buf);                                         /* /EFI */
    uint32_t efid = f.next_cluster;
    dirent(buf, ".          ", 1, efid, 0);
    dirent(buf + 32, "..         ", 1, 0, 0);
    dirent(buf + 64, "BOOT       ", 1, boot, 0);
    put_file(&f, buf, SEC);
    put16(img + (size_t)(f.data_start + boot - 2) * SEC + 32 + 26, efid);   /* BOOT's ".." is EFI */

    uint8_t *root = img + (size_t)(1 + 2 * f.fat_secs) * SEC;
    dirent(root, "PETRICHOR  ", 0, 0, 0);
    root[11] = 0x08;
    dirent(root + 32, "EFI        ", 1, efid, 0);
    dirent(root + 64, "KERNEL  BIN", 0, kern_c, (uint32_t)kernel_len);
    memcpy(img + (size_t)(1 + f.fat_secs) * SEC, img + SEC, (size_t)f.fat_secs * SEC);
    return 0;
}

/* ---- GPT ---- */

static const uint8_t ESP_TYPE[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};

static void entry(uint8_t *e, const uint8_t type[16], const uint8_t guid[16], uint8_t tweak, uint64_t first, uint64_t last, const char *name)
{
    memcpy(e, type, 16);
    memcpy(e + 16, guid, 16);
    e[31] ^= tweak;
    put64(e + 32, first);
    put64(e + 40, last);
    for (int i = 0; name[i]; i++)
        e[56 + i * 2] = name[i];
}

void efi_build_gpt(uint8_t *head, uint8_t *tail, uint64_t total, uint64_t esp_first, uint64_t esp_last,
                   uint64_t pfs_first, uint64_t pfs_last, const uint8_t disk_guid[16])
{
    static const uint8_t pfs_type[16] = PFS_TYPE_GUID_BYTES;
    uint8_t *mbr = head;
    mbr[446 + 2] = 2;
    mbr[446 + 4] = 0xEE;
    mbr[446 + 5] = 0xFF; mbr[446 + 6] = 0xFF; mbr[446 + 7] = 0xFF;
    put32(mbr + 446 + 8, 1);
    put32(mbr + 446 + 12, total - 1 > 0xFFFFFFFFull ? 0xFFFFFFFF : (uint32_t)(total - 1));
    mbr[510] = 0x55; mbr[511] = 0xAA;

    uint8_t *ents = head + 2 * SEC;
    entry(ents, ESP_TYPE, disk_guid, 1, esp_first, esp_last, "EFI System");
    if (pfs_last)
        entry(ents + 128, pfs_type, disk_guid, 2, pfs_first, pfs_last, "Petrichor data");
    uint32_t ents_crc = efi_crc32(ents, 128 * 128);

    for (int copy = 0; copy < 2; copy++) {
        uint8_t *h = copy ? tail + (GPT_TAIL_SECTORS - 1) * SEC : head + SEC;
        memcpy(h, "EFI PART", 8);
        put32(h + 8, 0x00010000);
        put32(h + 12, 92);
        put64(h + 24, copy ? total - 1 : 1);
        put64(h + 32, copy ? 1 : total - 1);
        put64(h + 40, 34);
        put64(h + 48, total - 34);
        memcpy(h + 56, disk_guid, 16);
        put64(h + 72, copy ? total - 33 : 2);
        put32(h + 80, 128);
        put32(h + 84, 128);
        put32(h + 88, ents_crc);
        put32(h + 16, 0);
        put32(h + 16, efi_crc32(h, 92));
    }
    memcpy(tail, ents, 128 * 128);
}
