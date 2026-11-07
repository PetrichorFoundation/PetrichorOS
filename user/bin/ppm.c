/* ppm: the Petrichor package manager.
 *
 *   ppm update                 fetch the package index from the repository
 *   ppm search [text]          list available packages
 *   ppm info NAME              show details about a package
 *   ppm install NAME...        install packages and their dependencies
 *   ppm remove [-f] NAME...    remove packages
 *   ppm upgrade                upgrade every installed package that has a newer version
 *   ppm list                   list installed packages
 *   ppm files NAME             list the files of an installed package
 *   ppm repo [URL]             show or set the repository URL
 *
 * Packages install under /disk (so they persist).  Downloads are verified
 * against the SHA-256 and size recorded in the index. */
#include <ulib.h>
#include "sha256.h"

#define ROOT      "/disk"
#define STATE     ROOT "/var/ppm"
#define DBDIR     STATE "/db"
#define INDEXFILE STATE "/index"
#define CONFFILE  ROOT "/etc/ppm.conf"
#define DEFAULT_REPO "https://raw.githubusercontent.com/krspybacon/petrichor-packages/main"
#define LOCAL_REPO   "http://10.0.2.2:8080"       /* `make serve-repo` on the host, tried if the default repository is unreachable */

struct pkg {
    char name[32], version[16], sha[65], deps[128], file[96], desc[128];
    u32 size;
};

static char repo[128] = DEFAULT_REPO;
static int repo_from_config;
static struct pkg *index_pkgs;
static int nindex;

static int die(const char *what, int err)
{
    fprintf(2, "ppm: %s: %s\n", what, strerror(err));
    return 1;
}

/* ---------------- small filesystem helpers ---------------- */

static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void mkdir_p(const char *path)
{
    char p[PATH_MAX];
    safestrcpy(p, path, sizeof p);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            mkdir(p);
            *s = '/';
        }
    }
    mkdir(p);
}

static char *slurp(const char *path, int *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    fstat(fd, &st);
    char *buf = malloc(st.size + 1);
    int n = buf ? read(fd, buf, st.size) : -1;
    close(fd);
    if (n < 0) {
        free(buf);
        return NULL;
    }
    buf[n] = 0;
    if (len)
        *len = n;
    return buf;
}

static int spit(const char *path, const void *data, int len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return fd;
    int off = 0;
    while (off < len) {
        int w = write(fd, (const char *)data + off, len - off > 4096 ? 4096 : len - off);
        if (w <= 0) {
            close(fd);
            return w < 0 ? w : -ENOSPC;
        }
        off += w;
    }
    close(fd);
    return 0;
}

static void load_config(void)
{
    char *last = slurp(STATE "/repo", NULL);             /* the repository the last `ppm update` really used */
    if (last) {
        char *nl = strchr(last, '\n');
        if (nl)
            *nl = 0;
        if (last[0] && strlen(last) < sizeof repo)
            safestrcpy(repo, last, sizeof repo);
        free(last);
    }
    char *conf = slurp(CONFFILE, NULL);
    if (!conf)
        return;
    char *p = strstr(conf, "repo=");
    if (p) {
        p += 5;
        int n = 0;
        while (p[n] && p[n] != '\n' && n < (int)sizeof repo - 1)
            n++;
        memcpy(repo, p, n);
        repo[n] = 0;
        repo_from_config = 1;
    }
    free(conf);
}

static int require_disk(void)
{
    struct diskinfo di;
    diskinfo(&di);
    if (!di.mounted) {
        fprintf(2, "ppm: no disk is mounted at /disk; packages need somewhere persistent to live\n");
        return 0;
    }
    mkdir_p(DBDIR);
    return 1;
}

/* ---------------- the index ---------------- */

static int field(char **p, char *dst, int max)
{
    char *s = *p, *tab = strchr(s, '\t');
    if (!tab)
        return 0;
    *tab = 0;
    safestrcpy(dst, s, max);
    *p = tab + 1;
    return 1;
}

static int load_index(void)
{
    if (index_pkgs)
        return nindex;
    char *text = slurp(INDEXFILE, NULL);
    if (!text) {
        fprintf(2, "ppm: no package index; run 'ppm update' first\n");
        return -1;
    }
    int cap = 16;
    index_pkgs = malloc(cap * sizeof *index_pkgs);
    for (char *line = text; *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        struct pkg pk;
        char size[16];
        char *p = line;
        if (field(&p, pk.name, sizeof pk.name) && field(&p, pk.version, sizeof pk.version) &&
            field(&p, size, sizeof size) && field(&p, pk.sha, sizeof pk.sha) &&
            field(&p, pk.deps, sizeof pk.deps) && field(&p, pk.file, sizeof pk.file)) {
            safestrcpy(pk.desc, p, sizeof pk.desc);
            pk.size = atoi(size);
            if (!strcmp(pk.deps, "-"))
                pk.deps[0] = 0;
            if (nindex == cap) {
                cap *= 2;
                index_pkgs = realloc(index_pkgs, cap * sizeof *index_pkgs);
            }
            index_pkgs[nindex++] = pk;
        }
        if (!nl)
            break;
        line = nl + 1;
    }
    free(text);
    return nindex;
}

static struct pkg *find_pkg(const char *name)
{
    for (int i = 0; i < nindex; i++)
        if (!strcmp(index_pkgs[i].name, name))
            return &index_pkgs[i];
    return NULL;
}

/* ---------------- the installed-package database ---------------- */

/* A db entry is a text file:  version\n deps\n description\n then one file path per line. */
struct inst {
    char version[16], deps[128];
    char *text;                          /* owns the strings below */
    char *files;                         /* start of the file list */
};

static int db_read(const char *name, struct inst *out)
{
    char path[PATH_MAX];
    snprintf(path, sizeof path, DBDIR "/%s", name);
    char *text = slurp(path, NULL);
    if (!text)
        return 0;
    char *l1 = strchr(text, '\n'), *l2 = l1 ? strchr(l1 + 1, '\n') : NULL, *l3 = l2 ? strchr(l2 + 1, '\n') : NULL;
    if (!l3) {
        free(text);
        return 0;
    }
    *l1 = *l2 = *l3 = 0;
    memset(out, 0, sizeof *out);
    safestrcpy(out->version, text, sizeof out->version);
    safestrcpy(out->deps, l1 + 1, sizeof out->deps);
    out->text = text;
    out->files = l3 + 1;
    return 1;
}

static int installed(const char *name)
{
    struct inst in;
    if (!db_read(name, &in))
        return 0;
    free(in.text);
    return 1;
}

/* Calls fn(name) for every installed package. */
static int for_each_installed(void (*fn)(const char *name, void *ctx), void *ctx)
{
    int fd = open(DBDIR, O_RDONLY), n = 0;
    if (fd < 0)
        return 0;
    struct dirent de;
    char names[64][NAME_MAX + 1];
    while (readdir(fd, &de) > 0 && n < 64)
        if (strcmp(de.name, ".") && strcmp(de.name, ".."))
            safestrcpy(names[n++], de.name, sizeof names[0]);
    close(fd);
    for (int i = 0; i < n; i++)
        fn(names[i], ctx);
    return n;
}

/* ---------------- commands ---------------- */

static int cmd_update(void)
{
    printf("Fetching %s/index\n", repo);
    char url[160], *body;
    int len;
    snprintf(url, sizeof url, "%s/index", repo);
    int e = http_get(url, &body, &len);
    if (e < 0 && !repo_from_config) {                   /* no repository chosen: also try the development server */
        printf("(%s: %s)\nFetching %s/index\n", url, strerror(e), LOCAL_REPO);
        snprintf(url, sizeof url, "%s/index", LOCAL_REPO);
        e = http_get(url, &body, &len);
        if (e == 0)
            safestrcpy(repo, LOCAL_REPO, sizeof repo);
    }
    if (e < 0)
        return die(url, e);
    e = spit(INDEXFILE, body, len);
    free(body);
    if (e < 0)
        return die(INDEXFILE, e);
    char used[140];
    spit(STATE "/repo", used, snprintf(used, sizeof used, "%s\n", repo));
    free(index_pkgs);
    index_pkgs = NULL;
    nindex = 0;
    int n = load_index();
    printf("%d package%s available.\n", n, n == 1 ? "" : "s");
    return 0;
}

static int cmd_search(const char *term)
{
    if (load_index() < 0)
        return 1;
    int shown = 0;
    for (int i = 0; i < nindex; i++) {
        struct pkg *p = &index_pkgs[i];
        if (term && !strstr(p->name, term) && !strstr(p->desc, term))
            continue;
        printf("%-12s %-8s %s%s\n", p->name, p->version, p->desc, installed(p->name) ? "  [installed]" : "");
        shown++;
    }
    if (!shown)
        printf("No packages match.\n");
    return 0;
}

static int cmd_info(const char *name)
{
    if (load_index() < 0)
        return 1;
    struct pkg *p = find_pkg(name);
    if (!p) {
        fprintf(2, "ppm: no such package: %s\n", name);
        return 1;
    }
    struct inst in;
    printf("Name:         %s\nVersion:      %s\nDescription:  %s\nDepends on:   %s\nDownload:     %u bytes\n"
           "SHA-256:      %s\nInstalled:    ",
           p->name, p->version, p->desc, p->deps[0] ? p->deps : "(nothing)", p->size, p->sha);
    if (db_read(name, &in)) {
        printf("yes (version %s)\n", in.version);
        free(in.text);
    } else {
        printf("no\n");
    }
    return 0;
}

/* Unpacks a verified archive under /disk and records it in the database. */
static int install_archive(struct pkg *p, const u8 *img, int len)
{
    struct hdr { char magic[8]; u32 count, pad; };
    struct ent { char path[96]; u32 type, size, offset, pad; };
    const struct hdr *h = (const void *)img;
    if (len < (int)sizeof *h || memcmp(h->magic, "PETRIPKG", 8) ||
        (u32)len < sizeof *h + h->count * sizeof(struct ent))
        return -ENOEXEC;
    const struct ent *ents = (const void *)(img + sizeof *h);

    char dbpath[PATH_MAX];
    snprintf(dbpath, sizeof dbpath, DBDIR "/%s", p->name);
    int cap = 4096, used = 0;
    char *db = malloc(cap);
    used = snprintf(db, cap, "%s\n%s\n%s\n", p->version, p->deps, p->desc);

    for (u32 i = 0; i < h->count; i++) {
        char path[PATH_MAX + 8];
        if (ents[i].path[95]) {
            free(db);
            return -ENOEXEC;
        }
        snprintf(path, sizeof path, ROOT "/%s", ents[i].path);
        if (ents[i].type == T_DIR) {
            mkdir_p(path);
            continue;
        }
        if (ents[i].offset > (u32)len || ents[i].size > (u32)len - ents[i].offset) {
            free(db);
            return -ENOEXEC;
        }
        char dir[PATH_MAX + 8];
        safestrcpy(dir, path, sizeof dir);
        char *slash = strrchr(dir, '/');
        if (slash && slash != dir) {
            *slash = 0;
            mkdir_p(dir);
        }
        int e = spit(path, img + ents[i].offset, ents[i].size);
        if (e < 0) {
            free(db);
            fprintf(2, "ppm: cannot write %s: %s\n", path, strerror(e));
            return e;
        }
        if (used + 120 > cap) {
            cap *= 2;
            db = realloc(db, cap);
        }
        used += snprintf(db + used, cap - used, "%s\n", ents[i].path);
    }
    int e = spit(dbpath, db, used);
    free(db);
    return e;
}

static int install_one(struct pkg *p, int upgrade)
{
    char url[256], hex[65];
    snprintf(url, sizeof url, "%s/%s", repo, p->file);
    printf("%s %s %s (%u bytes)\n", upgrade ? "Upgrading" : "Installing", p->name, p->version, p->size);
    char *body;
    int len;
    int e = http_get(url, &body, &len);
    if (e < 0)
        return die(url, e);
    if ((u32)len != p->size) {
        free(body);
        fprintf(2, "ppm: %s: size mismatch (got %d, index says %u)\n", p->file, len, p->size);
        return 1;
    }
    sha256_hex(body, len, hex);
    if (strcmp(hex, p->sha)) {
        free(body);
        fprintf(2, "ppm: %s: checksum mismatch\n  expected %s\n  got      %s\n", p->file, p->sha, hex);
        return 1;
    }
    e = install_archive(p, (u8 *)body, len);
    free(body);
    if (e < 0)
        return die(p->name, e);
    return 0;
}

/* Installs `name` after its dependencies. */
static int install_with_deps(const char *name, int depth)
{
    struct pkg *p = find_pkg(name);
    if (!p) {
        fprintf(2, "ppm: no such package: %s\n", name);
        return 1;
    }
    if (depth > 8) {
        fprintf(2, "ppm: dependency loop at %s\n", name);
        return 1;
    }
    if (installed(name))
        return 0;
    char deps[128];
    safestrcpy(deps, p->deps, sizeof deps);
    for (char *d = deps; *d;) {
        char *comma = strchr(d, ',');
        if (comma)
            *comma = 0;
        if (install_with_deps(d, depth + 1))
            return 1;
        if (!comma)
            break;
        d = comma + 1;
    }
    return install_one(p, 0);
}

static int cmd_install(int argc, char **argv)
{
    if (!exists(INDEXFILE) && cmd_update())
        return 1;
    if (load_index() < 0)
        return 1;
    for (int i = 0; i < argc; i++) {
        if (installed(argv[i])) {
            printf("%s is already installed.\n", argv[i]);
            continue;
        }
        if (install_with_deps(argv[i], 0))
            return 1;
    }
    printf("Done.\n");
    return 0;
}

struct rdep_ctx {
    const char *target;
    char who[128];
};

static void find_rdeps(const char *name, void *c)
{
    struct rdep_ctx *ctx = c;
    struct inst in;
    if (!db_read(name, &in))
        return;
    char deps[128];
    safestrcpy(deps, in.deps, sizeof deps);
    for (char *d = deps; *d;) {
        char *comma = strchr(d, ',');
        if (comma)
            *comma = 0;
        if (!strcmp(d, ctx->target)) {
            if (strlen(ctx->who) + strlen(name) + 2 < sizeof ctx->who) {
                if (ctx->who[0]) strcat(ctx->who, " ");
                strcat(ctx->who, name);
            }
        }
        if (!comma)
            break;
        d = comma + 1;
    }
    free(in.text);
}

static int remove_one(const char *name, int force)
{
    struct inst in;
    if (!db_read(name, &in)) {
        fprintf(2, "ppm: %s is not installed\n", name);
        return 1;
    }
    struct rdep_ctx ctx = {name, ""};
    for_each_installed(find_rdeps, &ctx);
    if (ctx.who[0] && !force) {
        fprintf(2, "ppm: cannot remove %s: needed by %s (use -f to force)\n", name, ctx.who);
        free(in.text);
        return 1;
    }
    printf("Removing %s %s\n", name, in.version);
    char dirs[64][PATH_MAX];
    int nd = 0;
    for (char *line = in.files; *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (*line) {
            char path[PATH_MAX + 8];
            snprintf(path, sizeof path, ROOT "/%s", line);
            unlink(path);
            char *slash = strrchr(path, '/');          /* remember parent dirs to tidy up */
            if (slash && nd < 64) {
                *slash = 0;
                int seen = 0;
                for (int i = 0; i < nd; i++)
                    seen |= !strcmp(dirs[i], path);
                if (!seen)
                    safestrcpy(dirs[nd++], path, PATH_MAX);
            }
        }
        if (!nl)
            break;
        line = nl + 1;
    }
    for (int i = 0; i < nd; i++) {                      /* rmdir fails harmlessly if still in use */
        for (char *s = dirs[i] + strlen(dirs[i]); s > dirs[i] + sizeof ROOT; s--) {
            if (*s == '/' || !*s) {
                char keep = *s;
                *s = 0;
                if (strcmp(dirs[i], ROOT "/bin") && strcmp(dirs[i], ROOT "/share") && unlink(dirs[i]) < 0)
                    break;
                *s = keep;
            }
        }
    }
    free(in.text);
    char dbpath[PATH_MAX];
    snprintf(dbpath, sizeof dbpath, DBDIR "/%s", name);
    unlink(dbpath);
    return 0;
}

static int cmd_remove(int argc, char **argv)
{
    int force = 0, rc = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "-f"))
            force = 1;
        else
            rc |= remove_one(argv[i], force);
    }
    return rc;
}

static void upgrade_one(const char *name, void *ctx)
{
    struct inst in;
    struct pkg *p = find_pkg(name);
    if (!p || !db_read(name, &in))
        return;
    if (strcmp(in.version, p->version)) {
        printf("%s: %s -> %s\n", name, in.version, p->version);
        free(in.text);
        if (!install_one(p, 1))
            ++*(int *)ctx;
    } else {
        free(in.text);
    }
}

static int cmd_upgrade(void)
{
    if (cmd_update() || load_index() < 0)
        return 1;
    int upgraded = 0;
    for_each_installed(upgrade_one, &upgraded);
    printf(upgraded ? "%d package%s upgraded.\n" : "Everything is up to date.\n", upgraded, upgraded == 1 ? "" : "s");
    return 0;
}

static void list_one(const char *name, void *ctx)
{
    struct inst in;
    if (!db_read(name, &in))
        return;
    char *desc = in.text + strlen(in.text) + 1;
    desc += strlen(desc) + 1;
    printf("%-12s %-8s %s\n", name, in.version, desc);
    free(in.text);
    ++*(int *)ctx;
}

static int cmd_list(void)
{
    int n = 0;
    for_each_installed(list_one, &n);
    if (!n)
        printf("No packages installed.\n");
    return 0;
}

static int cmd_files(const char *name)
{
    struct inst in;
    if (!db_read(name, &in)) {
        fprintf(2, "ppm: %s is not installed\n", name);
        return 1;
    }
    for (char *line = in.files; *line;) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        if (*line)
            printf(ROOT "/%s\n", line);
        if (!nl)
            break;
        line = nl + 1;
    }
    free(in.text);
    return 0;
}

static int cmd_repo(const char *url)
{
    if (!url) {
        printf("%s\n", repo);
        return 0;
    }
    char line[160];
    mkdir_p(ROOT "/etc");
    int n = snprintf(line, sizeof line, "repo=%s\n", url);
    int e = spit(CONFFILE, line, n);
    if (e < 0)
        return die(CONFFILE, e);
    printf("Repository set to %s\n", url);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(2, "usage: ppm update | search [text] | info NAME | install NAME... |\n"
                   "           remove [-f] NAME... | upgrade | list | files NAME | repo [URL]\n");
        return 2;
    }
    if (!require_disk())
        return 1;
    load_config();
    const char *cmd = argv[1];
    if (!strcmp(cmd, "update")) return cmd_update();
    if (!strcmp(cmd, "search")) return cmd_search(argc > 2 ? argv[2] : NULL);
    if (!strcmp(cmd, "info") && argc > 2) return cmd_info(argv[2]);
    if (!strcmp(cmd, "install") && argc > 2) return cmd_install(argc - 2, argv + 2);
    if ((!strcmp(cmd, "remove") || !strcmp(cmd, "uninstall")) && argc > 2) return cmd_remove(argc - 2, argv + 2);
    if (!strcmp(cmd, "upgrade")) return cmd_upgrade();
    if (!strcmp(cmd, "list")) return cmd_list();
    if (!strcmp(cmd, "files") && argc > 2) return cmd_files(argv[2]);
    if (!strcmp(cmd, "repo")) return cmd_repo(argc > 2 ? argv[2] : NULL);
    fprintf(2, "ppm: unknown or incomplete command '%s'\n", cmd);
    return 2;
}
