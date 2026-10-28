/* mkinitrd: pack a directory tree into the image the kernel unpacks at boot.
 *
 *   header  : "PTRCHRFS", u32 count, u32 pad
 *   entries : {char path[96], u32 type, u32 size, u32 offset, u32 pad} * count
 *   data    : file contents, 16-byte aligned, offsets relative to image start
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define T_FILE 1
#define T_DIR  2

struct hdr {
    char magic[8];
    uint32_t count, pad;
};

struct ent {
    char path[96];
    uint32_t type, size, offset, pad;
};

static struct ent *ents;
static char **srcs;
static size_t nents, capents;

static void add(const char *rel, const char *full, uint32_t type, uint32_t size)
{
    if (strlen(rel) >= sizeof ents[0].path) {
        fprintf(stderr, "mkinitrd: path too long: %s\n", rel);
        exit(1);
    }
    if (nents == capents) {
        capents = capents ? capents * 2 : 64;
        ents = realloc(ents, capents * sizeof *ents);
        srcs = realloc(srcs, capents * sizeof *srcs);
    }
    memset(&ents[nents], 0, sizeof ents[0]);
    strcpy(ents[nents].path, rel);
    ents[nents].type = type;
    ents[nents].size = size;
    srcs[nents] = strdup(full);
    nents++;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void walk(const char *root, const char *rel)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/%s", root, rel);
    DIR *d = opendir(dir);
    if (!d) {
        perror(dir);
        exit(1);
    }
    char *names[512];
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < 512)
        if (de->d_name[0] != '.')
            names[n++] = strdup(de->d_name);
    closedir(d);
    qsort(names, n, sizeof names[0], cmp_names);

    for (int i = 0; i < n; i++) {
        char r[1024], f[2048];
        snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", names[i]);
        snprintf(f, sizeof f, "%s/%s", root, r);
        struct stat st;
        if (stat(f, &st) < 0) {
            perror(f);
            exit(1);
        }
        if (S_ISDIR(st.st_mode)) {
            add(r, f, T_DIR, 0);
            walk(root, r);
        } else if (S_ISREG(st.st_mode)) {
            add(r, f, T_FILE, st.st_size);
        }
        free(names[i]);
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: mkinitrd out.img rootdir\n");
        return 1;
    }
    walk(argv[2], "");

    uint32_t off = sizeof(struct hdr) + nents * sizeof(struct ent);
    for (size_t i = 0; i < nents; i++) {
        off = (off + 15) & ~15u;
        ents[i].offset = off;
        off += ents[i].size;
    }

    FILE *out = fopen(argv[1], "wb");
    if (!out) {
        perror(argv[1]);
        return 1;
    }
    struct hdr h = {"PTRCHRFS", (uint32_t)nents, 0};
    fwrite(&h, sizeof h, 1, out);
    fwrite(ents, sizeof *ents, nents, out);

    for (size_t i = 0; i < nents; i++) {
        if (ents[i].type != T_FILE)
            continue;
        while ((uint32_t)ftell(out) < ents[i].offset)
            fputc(0, out);
        FILE *in = fopen(srcs[i], "rb");
        if (!in) {
            perror(srcs[i]);
            return 1;
        }
        char buf[65536];
        size_t r;
        while ((r = fread(buf, 1, sizeof buf, in)) > 0)
            fwrite(buf, 1, r, out);
        fclose(in);
    }
    fclose(out);
    printf("initrd: %zu entries, %u bytes\n", nents, off);
    return 0;
}
