/* mkppk: build a Petrichor package (.ppk) and register it in a repository index.
 *
 *   mkppk repo_dir stage_dir name version deps description
 *
 * Writes repo_dir/name-version.ppk (same archive layout as the initrd, paths
 * relative to /disk) and appends a tab-separated line to repo_dir/index:
 *   name  version  size  sha256  deps  filename  description
 */
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sha256.h"

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
        fprintf(stderr, "mkppk: path too long: %s\n", rel);
        exit(1);
    }
    if (nents == capents) {
        capents = capents ? capents * 2 : 32;
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

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void walk(const char *root, const char *rel)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/%s", root, rel);
    DIR *d = opendir(dir);
    if (!d) {
        perror(dir);
        exit(1);
    }
    char *names[256];
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < 256)
        if (de->d_name[0] != '.')
            names[n++] = strdup(de->d_name);
    closedir(d);
    qsort(names, n, sizeof names[0], cmp);
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
    if (argc != 7) {
        fprintf(stderr, "usage: mkppk repo_dir stage_dir name version deps description\n");
        return 1;
    }
    const char *repo = argv[1], *stage = argv[2], *name = argv[3], *version = argv[4];
    const char *deps = argv[5], *desc = argv[6];
    walk(stage, "");

    uint32_t off = sizeof(struct hdr) + nents * sizeof(struct ent);
    for (size_t i = 0; i < nents; i++) {
        off = (off + 15) & ~15u;
        ents[i].offset = off;
        off += ents[i].size;
    }
    uint8_t *img = calloc(1, off + 16);
    struct hdr h = {"PETRIPKG", (uint32_t)nents, 0};
    memcpy(img, &h, sizeof h);
    memcpy(img + sizeof h, ents, nents * sizeof *ents);
    for (size_t i = 0; i < nents; i++) {
        if (ents[i].type != T_FILE)
            continue;
        FILE *in = fopen(srcs[i], "rb");
        if (!in || fread(img + ents[i].offset, 1, ents[i].size, in) != ents[i].size) {
            perror(srcs[i]);
            return 1;
        }
        fclose(in);
    }

    char file[256], path[1024], hex[65];
    snprintf(file, sizeof file, "%s-%s.ppk", name, version);
    snprintf(path, sizeof path, "%s/%s", repo, file);
    FILE *out = fopen(path, "wb");
    if (!out || fwrite(img, 1, off, out) != off) {
        perror(path);
        return 1;
    }
    fclose(out);
    sha256_hex(img, off, hex);

    snprintf(path, sizeof path, "%s/index", repo);
    FILE *idx = fopen(path, "ab");
    if (!idx) {
        perror(path);
        return 1;
    }
    fprintf(idx, "%s\t%s\t%u\t%s\t%s\t%s\t%s\n", name, version, off, hex, *deps ? deps : "-", file, desc);
    fclose(idx);
    printf("  %-10s %-6s %6u bytes  sha256 %.12s...\n", name, version, off, hex);
    return 0;
}
