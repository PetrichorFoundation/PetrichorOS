/* pinstall: install Petrichor onto a disk.
 *
 *   pinstall                       interactive: pick a disk, confirm, install
 *   pinstall --probe hdb           describe what is on a disk
 *   pinstall -y [--progress] hdb   install without asking; --progress prints lines the graphical
 *                                  installer parses (PROGRESS n text / DONE / ERROR text)
 *
 * It works from any boot medium (CD, USB stick, hard disk).  The image written is built from
 *   - /boot/stage1.bin and /boot/stage2.bin, the bootloader stages shipped inside the system, and
 *   - /dev/kernelimg, the kernel exactly as the bootloader loaded it.
 * Stage 2 gets this install's kernel size and a random id, so the kernel can later tell which disk
 * it booted from.  The data partition is cleared so the kernel formats it on first boot, and the
 * copy is read back and compared before we say it worked. */
#include <ulib.h>

#define BOOT_SECTORS 4096                  /* boot region: LBA 0 .. 4095 */
#define KERNEL_LBA   16
#define CHUNK        (64 * 1024)
#define MIN_SECTORS  (BOOT_SECTORS + 2048) /* room for a small data partition */
#define STAGE2_MAX   (15 * 512)

static int progress_mode;

static void report(int pct, const char *msg)
{
    if (progress_mode)
        printf("PROGRESS %d %s\n", pct, msg);
    else
        printf("[%3d%%] %s\n", pct, msg);
}

static int fail(const char *msg, int err)
{
    if (progress_mode)
        printf("ERROR %s%s%s\n", msg, err ? ": " : "", err ? strerror(err) : "");
    else
        fprintf(2, "pinstall: %s%s%s\n", msg, err ? ": " : "", err ? strerror(err) : "");
    return 1;
}

static int xfer(int fd, void *buf, int n, int write_it)
{
    int done = 0;
    while (done < n) {
        int r = write_it ? write(fd, (char *)buf + done, n - done) : read(fd, (char *)buf + done, n - done);
        if (r <= 0)
            return r < 0 ? r : -EIO;
        done += r;
    }
    return 0;
}

static int slurp(const char *path, void *buf, int max)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return fd;
    int n = 0, r;
    while (n < max && (r = read(fd, (char *)buf + n, max - n)) > 0)
        n += r;
    close(fd);
    return n;
}

/* A short description of what is already on the disk (empty if it looks blank). */
static const char *probe(const char *name, char *out, int outsz)
{
    char path[16];
    snprintf(path, sizeof path, "/dev/%s", name);
    out[0] = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return out;
    unsigned char s[1024];
    if (xfer(fd, s, sizeof s, 0) == 0) {
        if (!memcmp(s + 512, "EFI PART", 8)) {
            safestrcpy(out, "has a GPT partition table (another OS may be installed)", outsz);
        } else if (s[510] == 0x55 && s[511] == 0xAA) {
            int parts = 0;
            for (int i = 0; i < 4; i++)
                parts += s[446 + i * 16 + 4] != 0;
            if (parts)
                snprintf(out, outsz, "has %d partition%s (another OS may be installed)", parts, parts == 1 ? "" : "s");
            else if (!memcmp(s + 512 + 28, "PSG2", 4) || s[0] == 0xFA)
                safestrcpy(out, "has a boot sector (possibly a previous Petrichor install)", outsz);
        }
    }
    close(fd);
    return out;
}

static int install(const char *name)
{
    struct driveinfo d[MAXDRIVES];
    int n = drives(d, MAXDRIVES), target = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(d[i].name, name))
            target = i;
    if (target < 0)
        return fail("no such disk", 0);
    if (d[target].boot)
        return fail("that is the disk Petrichor is running from; pick another one", 0);
    if (d[target].sectors < MIN_SECTORS)
        return fail("the disk is too small (need at least 3 MiB)", 0);

    report(2, "Preparing the install image");
    int kfd = open("/dev/kernelimg", O_RDONLY);
    if (kfd < 0)
        return fail("no kernel image is available: start Petrichor with its own bootloader to install", 0);
    struct stat kst;
    fstat(kfd, &kst);
    u32 ksize = kst.size;
    if (ksize == 0 || KERNEL_LBA * 512 + ksize > (u32)BOOT_SECTORS * 512)
        return fail("the kernel image has an unexpected size", 0);

    u32 imgsize = BOOT_SECTORS * 512;
    u8 *img = calloc(1, imgsize);
    if (!img)
        return fail("out of memory", 0);
    int n1 = slurp("/boot/stage1.bin", img, 512);
    int n2 = slurp("/boot/stage2.bin", img + 512, STAGE2_MAX);
    if (n1 != 512 || n2 < 64 || img[510] != 0x55 || img[511] != 0xAA)
        return fail("the bootloader files are missing from the system", 0);
    if (xfer(kfd, img + KERNEL_LBA * 512, ksize, 0) < 0)
        return fail("cannot read the kernel image", 0);
    close(kfd);

    u32 ksectors = (ksize + 511) / 512, klba = KERNEL_LBA;
    memcpy(img + 512 + 4, &klba, 4);                 /* stage 2: where the kernel is, how big, and this install's id */
    memcpy(img + 512 + 8, &ksectors, 4);
    unsigned char id[16];
    int rf = open("/dev/random", O_RDONLY);
    read(rf, id, sizeof id);
    close(rf);
    id[0] |= 1;                                      /* never all zero */
    memcpy(img + 512 + 12, id, 16);

    report(5, "Opening the target disk");
    char path[16];
    snprintf(path, sizeof path, "/dev/%s", name);
    int dst = open(path, O_RDWR);
    if (dst < 0)
        return fail("cannot open the target disk", dst);

    int chunks = imgsize / CHUNK;
    for (int i = 0; i < chunks; i++) {
        report(8 + i * 57 / chunks, "Writing the bootloader and kernel");
        int e = xfer(dst, img + i * CHUNK, CHUNK, 1);
        if (e < 0)
            return fail("write error on the target disk", e);
    }

    u8 *chk = malloc(CHUNK);
    if (!chk)
        return fail("out of memory", 0);
    lseek(dst, 0, SEEK_SET);
    for (int i = 0; i < chunks; i++) {
        report(65 + i * 30 / chunks, "Verifying the copy");
        if (xfer(dst, chk, CHUNK, 0) < 0)
            return fail("read error while verifying", 0);
        if (memcmp(chk, img + i * CHUNK, CHUNK))
            return fail("verification failed: the target disk does not match", 0);
    }

    report(96, "Preparing the data partition");
    memset(chk, 0, CHUNK);
    lseek(dst, (u32)BOOT_SECTORS * 512, SEEK_SET);
    int e = xfer(dst, chk, CHUNK, 1);                /* clears the superblock and bitmaps */
    if (e < 0)
        return fail("write error on the target disk", e);
    close(dst);
    free(img);
    free(chk);
    report(100, "Finished");
    if (progress_mode)
        printf("DONE\n");
    else
        printf("\nPetrichor is installed on %s.\nShut down, remove the installer medium, and boot from %s.\n", name, name);
    return 0;
}

int main(int argc, char **argv)
{
    int yes = 0, do_probe = 0;
    const char *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-y"))
            yes = 1;
        else if (!strcmp(argv[i], "--progress"))
            progress_mode = 1;
        else if (!strcmp(argv[i], "--probe"))
            do_probe = 1;
        else
            name = argv[i];
    }
    char note[96];
    if (do_probe && name) {
        probe(name, note, sizeof note);
        printf("%s\n", note[0] ? note : "looks blank");
        return 0;
    }
    if (name && yes)
        return install(name);

    struct driveinfo d[MAXDRIVES];
    int n = drives(d, MAXDRIVES);
    printf("Petrichor installer\n\nAvailable target disks:\n");
    int offered = 0;
    for (int i = 0; i < n; i++) {
        if (d[i].boot || d[i].sectors < MIN_SECTORS)
            continue;
        probe(d[i].name, note, sizeof note);
        printf("  %s  %u MiB  %s%s%s\n", d[i].name, d[i].sectors / 2048, d[i].model, note[0] ? "  -- " : "", note);
        offered++;
    }
    if (!offered) {
        printf("  (none: attach a disk of at least 3 MiB)\n");
        return 1;
    }
    char line[64];
    if (!name) {
        printf("\nInstall to which disk? ");
        if (read_line(0, line, sizeof line) <= 0)
            return 1;
        name = line;
    }
    printf("\nEverything on %s will be erased. Type 'yes' to continue: ", name);
    char answer[16];
    if (read_line(0, answer, sizeof answer) <= 0 || strcmp(answer, "yes")) {
        printf("Cancelled.\n");
        return 1;
    }
    return install(name);
}
