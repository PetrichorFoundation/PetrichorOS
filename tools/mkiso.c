/* mkiso: build a bootable Petrichor ISO (El Torito) that is also a bootable USB image.
 *
 *   mkiso stage1.bin stage2.bin kernel.bin out.iso
 *
 * Layout (2048-byte sectors):
 *   0-15   system area: stage 1 at byte 0 (with a partition entry) and stage 2 at byte 512, laid out
 *          exactly like the first sectors of a disk, so `dd if=out.iso of=/dev/sdX` makes a USB stick
 *          that BIOS firmware boots as a hard disk
 *   16     primary volume descriptor     17  El Torito boot record     18  terminator
 *   19     boot catalog                  20/21 path tables (L, M)      22  root directory
 *   23     BOOT.IMG: stage 1 patched to read stage 2 from the CD (what firmware loads for "CD boot")
 *   24..   STAGE2.BIN, then KERNEL.BIN
 * Stage 2 reads the kernel from the CD with 2048-byte sectors; it is told where it is via kernel_lba
 * (in 512-byte units) which we set here.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SEC 2048

static uint8_t *iso;
static uint32_t total_secs;

static uint8_t *sector(uint32_t n) { return iso + (size_t)n * SEC; }

static void le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void both32(uint8_t *p, uint32_t v) { le32(p, v); be32(p + 4, v); }
static void both16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 8; p[3] = v; }

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
    uint8_t *b = calloc(1, *len + SEC);
    if (fread(b, 1, *len, f) != *len) {
        perror(path);
        exit(1);
    }
    fclose(f);
    return b;
}

static void pad(uint8_t *dst, const char *s, size_t n)
{
    memset(dst, ' ', n);
    memcpy(dst, s, strlen(s) < n ? strlen(s) : n);
}

/* One directory record; returns its length. */
static int dirrec(uint8_t *p, uint32_t lba, uint32_t size, int is_dir, const char *name, int special)
{
    int nl = special >= 0 ? 1 : (int)strlen(name);
    int len = 33 + nl;
    if (len & 1)
        len++;
    memset(p, 0, len);
    p[0] = len;
    both32(p + 2, lba);
    both32(p + 10, size);
    p[18] = 126; p[19] = 10; p[20] = 28;                    /* 2026-10-28 00:00:00 */
    p[25] = is_dir ? 2 : 0;
    both16(p + 28, 1);
    p[32] = nl;
    if (special >= 0)
        p[33] = special;                                    /* 0 = ".", 1 = ".." */
    else
        memcpy(p + 33, name, nl);
    return len;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: mkiso stage1.bin stage2.bin kernel.bin out.iso\n");
        return 1;
    }
    size_t n1, n2, nk;
    uint8_t *s1 = slurp(argv[1], &n1), *s2 = slurp(argv[2], &n2), *kern = slurp(argv[3], &nk);
    if (n1 != 512 || s1[510] != 0x55 || s1[511] != 0xAA || n2 > 15 * 512) {
        fprintf(stderr, "mkiso: bad bootloader files\n");
        return 1;
    }

    uint32_t s2_secs = (n2 + SEC - 1) / SEC;
    uint32_t s2_sec = 24, k_sec = s2_sec + s2_secs, k_secs = (nk + SEC - 1) / SEC;
    total_secs = k_sec + k_secs;
    iso = calloc(total_secs, SEC);

    /* kernel + stage 2, with the kernel's position patched into stage 2 */
    uint32_t ksectors512 = (nk + 511) / 512, klba512 = k_sec * 4;
    memcpy(s2 + 4, &klba512, 4);
    memcpy(s2 + 8, &ksectors512, 4);
    memcpy(sector(k_sec), kern, nk);
    memcpy(sector(s2_sec), s2, n2);

    /* system area: the MBR (with one bootable partition covering the image) and stage 2 */
    uint8_t *sys = sector(0);
    memcpy(sys, s1, 512);
    memcpy(sys + 512, s2, n2);
    uint8_t *pe = sys + 446;
    pe[0] = 0x80;
    pe[1] = 0; pe[2] = 1; pe[3] = 0;
    pe[4] = 0x17;
    pe[5] = 0xFE; pe[6] = 0xFF; pe[7] = 0xFF;
    le32(pe + 8, 0);
    le32(pe + 12, total_secs * 4);

    /* BOOT.IMG: stage 1 that reads stage 2 from the CD */
    uint8_t *boot = sector(23);
    memcpy(boot, s1, 512);
    boot[0x1A2] = s2_secs; boot[0x1A3] = 0;                 /* DAP count, in CD sectors */
    le32(boot + 0x1A8, s2_sec);                             /* DAP starting LBA, in CD sectors */

    /* 16: primary volume descriptor */
    uint8_t *pvd = sector(16);
    pvd[0] = 1;
    memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    pad(pvd + 8, "PETRICHOR", 32);
    pad(pvd + 40, "PETRICHOR", 32);
    both32(pvd + 80, total_secs);
    both16(pvd + 120, 1);
    both16(pvd + 124, 1);
    both16(pvd + 128, SEC);
    both32(pvd + 132, 10);
    le32(pvd + 140, 20);
    be32(pvd + 148, 21);
    dirrec(pvd + 156, 22, SEC, 1, NULL, 0);
    pad(pvd + 190, "", 128);
    pad(pvd + 318, "", 128);
    pad(pvd + 446, "", 128);
    pad(pvd + 574, "", 128);
    pad(pvd + 702, "", 37);
    pad(pvd + 739, "", 37);
    pad(pvd + 776, "", 37);
    for (int off = 813; off <= 864; off += 17) {            /* creation/modification/expiration/effective */
        memcpy(pvd + off, off == 864 || off == 847 ? "0000000000000000" : "2026102800000000", 16);
        pvd[off + 16] = 0;
    }
    pvd[881] = 1;

    /* 17: El Torito boot record */
    uint8_t *br = sector(17);
    br[0] = 0;
    memcpy(br + 1, "CD001", 5);
    br[6] = 1;
    memcpy(br + 7, "EL TORITO SPECIFICATION", 23);
    le32(br + 71, 19);

    /* 18: volume descriptor set terminator */
    uint8_t *vt = sector(18);
    vt[0] = 255;
    memcpy(vt + 1, "CD001", 5);
    vt[6] = 1;

    /* 19: boot catalog: validation entry + default (no-emulation) entry */
    uint8_t *cat = sector(19);
    cat[0] = 1;
    cat[1] = 0;                                             /* platform: x86 BIOS */
    memcpy(cat + 4, "PETRICHOR", 9);
    cat[30] = 0x55;
    cat[31] = 0xAA;
    uint32_t sum = 0;
    for (int i = 0; i < 32; i += 2)
        sum += cat[i] | (cat[i + 1] << 8);
    uint16_t chk = (uint16_t)(0x10000 - (sum & 0xFFFF));
    cat[28] = chk;
    cat[29] = chk >> 8;
    uint8_t *ent = cat + 32;
    ent[0] = 0x88;                                          /* bootable */
    ent[1] = 0;                                             /* no emulation */
    ent[2] = 0; ent[3] = 0;                                 /* load segment 0 => 0x7C0 */
    ent[4] = 0;
    ent[6] = 4; ent[7] = 0;                                 /* load 4 virtual (512-byte) sectors */
    le32(ent + 8, 23);

    /* 20/21: path tables (root only) */
    uint8_t *pl = sector(20), *pm = sector(21);
    pl[0] = 1; le32(pl + 2, 22); pl[6] = 1; pl[7] = 0; pl[8] = 0;
    pm[0] = 1; be32(pm + 2, 22); pm[6] = 0; pm[7] = 1; pm[8] = 0;

    /* 22: root directory */
    uint8_t *d = sector(22);
    d += dirrec(d, 22, SEC, 1, NULL, 0);
    d += dirrec(d, 22, SEC, 1, NULL, 1);
    d += dirrec(d, 23, 2048, 0, "BOOT.IMG;1", -1);
    d += dirrec(d, s2_sec, (uint32_t)n2, 0, "STAGE2.BIN;1", -1);
    d += dirrec(d, k_sec, (uint32_t)nk, 0, "KERNEL.BIN;1", -1);

    FILE *out = fopen(argv[4], "wb");
    if (!out || fwrite(iso, SEC, total_secs, out) != total_secs) {
        perror(argv[4]);
        return 1;
    }
    fclose(out);
    printf("iso: %u sectors (%u KiB), stage2 at %u, kernel at %u\n", total_secs, total_secs * 2, s2_sec, k_sec);
    return 0;
}
