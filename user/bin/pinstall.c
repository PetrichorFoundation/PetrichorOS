/* pinstall: install Petrichor onto another disk.
 *
 *   pinstall                 interactive: pick a disk, confirm, install
 *   pinstall -y [--progress] hdb
 *                            install without asking; --progress prints lines that
 *                            the graphical installer parses (PROGRESS n text / DONE / ERROR text)
 *
 * What is copied: the first 2 MiB of the boot disk (MBR, stage 2 and the kernel with
 * its built-in root image).  The target's data partition is cleared so the kernel formats
 * it on first boot.  The copy is read back and compared before we say it worked. */
#include <ulib.h>

#define PART_SECTORS 4096                  /* boot region: LBA 0 .. 4095 */
#define CHUNK        (64 * 1024)
#define MIN_SECTORS  (PART_SECTORS + 2048) /* room for a small data partition */

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

static int install(const char *name)
{
    struct driveinfo d[4];
    int n = drives(d, 4), target = -1;
    for (int i = 0; i < n; i++)
        if (!strcmp(d[i].name, name))
            target = i;
    if (target < 0)
        return fail("no such disk", 0);
    if (d[target].boot)
        return fail("that is the disk Petrichor is running from; pick another one", 0);
    if (d[target].sectors < MIN_SECTORS)
        return fail("the disk is too small (need at least 3 MiB)", 0);

    report(2, "Opening disks");
    char path[16];
    snprintf(path, sizeof path, "/dev/%s", name);
    int src = open("/dev/hda", O_RDONLY), dst = open(path, O_RDWR);
    if (src < 0 || dst < 0)
        return fail("cannot open the disks", src < 0 ? src : dst);

    char *a = malloc(CHUNK), *b = malloc(CHUNK);
    if (!a || !b)
        return fail("out of memory", 0);
    int chunks = PART_SECTORS * 512 / CHUNK;

    for (int i = 0; i < chunks; i++) {
        report(5 + i * 60 / chunks, "Copying the bootloader and kernel");
        int e = xfer(src, a, CHUNK, 0);
        if (e < 0)
            return fail("read error on the boot disk", e);
        if ((e = xfer(dst, a, CHUNK, 1)) < 0)
            return fail("write error on the target disk", e);
    }

    lseek(src, 0, SEEK_SET);
    lseek(dst, 0, SEEK_SET);
    for (int i = 0; i < chunks; i++) {
        report(65 + i * 30 / chunks, "Verifying the copy");
        if (xfer(src, a, CHUNK, 0) < 0 || xfer(dst, b, CHUNK, 0) < 0)
            return fail("read error while verifying", 0);
        if (memcmp(a, b, CHUNK))
            return fail("verification failed: the target disk does not match", 0);
    }

    report(96, "Preparing the data partition");
    memset(a, 0, CHUNK);
    lseek(dst, (u32)PART_SECTORS * 512, SEEK_SET);
    int e = xfer(dst, a, CHUNK, 1);                 /* clears the superblock and bitmaps */
    if (e < 0)
        return fail("write error on the target disk", e);
    close(src);
    close(dst);
    free(a);
    free(b);
    report(100, "Finished");
    if (progress_mode)
        printf("DONE\n");
    else
        printf("\nPetrichor is installed on %s.\nShut down, remove the installer disk, and boot from %s.\n", name, name);
    return 0;
}

int main(int argc, char **argv)
{
    int yes = 0;
    const char *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-y"))
            yes = 1;
        else if (!strcmp(argv[i], "--progress"))
            progress_mode = 1;
        else
            name = argv[i];
    }
    if (name && yes)
        return install(name);

    struct driveinfo d[4];
    int n = drives(d, 4);
    printf("Petrichor installer\n\nAvailable target disks:\n");
    int offered = 0;
    for (int i = 0; i < n; i++) {
        if (d[i].boot || d[i].sectors < MIN_SECTORS)
            continue;
        printf("  %s  %u MiB  %s\n", d[i].name, d[i].sectors / 2048, d[i].model);
        offered++;
    }
    if (!offered) {
        printf("  (none: attach a second disk of at least 3 MiB)\n");
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
