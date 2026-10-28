/* PetrichorFS: an in-memory Unix filesystem (files, directories, devices).
 * Directories carry explicit "." and ".." entries, so path walking is uniform. */
#include "kernel.h"

struct inode *fs_root;
static u32 next_ino = 1;

/* ---------------- inodes ---------------- */

static struct inode *inode_new(u32 type)
{
    struct inode *ip = kzalloc(sizeof *ip);
    if (!ip)
        return NULL;
    ip->type = type;
    ip->ino = next_ino++;
    return ip;
}

static void inode_free(struct inode *ip)
{
    if (ip->pfs) {
        pfs_destroy(ip);
        return;
    }
    kfree(ip->data);
    kfree(ip->ents);
    kfree(ip);
}

void iref(struct inode *ip) { ip->refs++; }

void iput(struct inode *ip)
{
    if (--ip->refs <= 0 && ip->nlink <= 0)
        inode_free(ip);
}

int inode_read(struct inode *ip, u32 off, void *dst, u32 n)
{
    if (ip->pfs)
        return pfs_read(ip, off, dst, n);
    if (off >= ip->size)
        return 0;
    if (n > ip->size - off)
        n = ip->size - off;
    memcpy(dst, ip->data + off, n);
    return n;
}

int inode_write(struct inode *ip, u32 off, const void *src, u32 n)
{
    if (ip->pfs)
        return pfs_write(ip, off, src, n);
    u32 end = off + n;
    if (end < off)
        return -EFBIG;
    if (end > ip->cap) {
        u32 cap = ip->cap * 2;
        if (cap < end)
            cap = end;
        cap = (cap + 63) & ~63u;
        u8 *nd = kmalloc(cap);
        if (!nd)
            return -ENOSPC;
        memcpy(nd, ip->data, ip->size);
        memset(nd + ip->size, 0, cap - ip->size);
        kfree(ip->data);
        ip->data = nd;
        ip->cap = cap;
    }
    memcpy(ip->data + off, src, n);
    if (end > ip->size)
        ip->size = end;
    return n;
}

void inode_truncate(struct inode *ip)
{
    if (ip->pfs) {
        pfs_truncate(ip);
        return;
    }
    kfree(ip->data);
    ip->data = NULL;
    ip->size = ip->cap = 0;
}

/* ---------------- directories ---------------- */

static struct dent *dir_find(struct inode *dp, const char *name)
{
    for (u32 i = 0; i < dp->nents; i++)
        if (!strcmp(dp->ents[i].name, name))
            return &dp->ents[i];
    return NULL;
}

static int is_dots(const char *name)
{
    return !strcmp(name, ".") || !strcmp(name, "..");
}

/* Finds `name` in any directory, RAM or disk. */
static struct inode *lookup(struct inode *dp, const char *name)
{
    if (dp->pfs)
        return pfs_lookup(dp, name);
    struct dent *d = dir_find(dp, name);
    return d ? d->ip : NULL;
}

u32 inode_nents(struct inode *ip)
{
    return ip->pfs ? pfs_nents(ip) : ip->nents;
}

/* Reads directory entry `idx` into `de`; 1 on success, 0 past the end. */
int inode_dirent(struct inode *ip, u32 idx, struct dirent *de)
{
    if (ip->pfs)
        return pfs_dirent(ip, idx, de);
    if (idx >= ip->nents)
        return 0;
    struct dent *d = &ip->ents[idx];
    de->ino = d->ip->ino;
    de->type = d->ip->type;
    safestrcpy(de->name, d->name, sizeof de->name);
    return 1;
}

static int dir_add(struct inode *dp, const char *name, struct inode *ip)
{
    if (dp->nents == dp->cap_ents) {
        u32 cap = dp->cap_ents ? dp->cap_ents * 2 : 8;
        struct dent *ne = kmalloc(cap * sizeof *ne);
        if (!ne)
            return -ENOSPC;
        memcpy(ne, dp->ents, dp->nents * sizeof *ne);
        kfree(dp->ents);
        dp->ents = ne;
        dp->cap_ents = cap;
    }
    struct dent *d = &dp->ents[dp->nents++];
    safestrcpy(d->name, name, sizeof d->name);
    d->ip = ip;
    if (!is_dots(name))
        ip->nlink++;
    return 0;
}

static void dir_remove(struct inode *dp, const char *name)
{
    struct dent *d = dir_find(dp, name);
    if (!d)
        return;
    struct inode *ip = d->ip;
    u32 idx = d - dp->ents;
    memmove(&dp->ents[idx], &dp->ents[idx + 1], (dp->nents - idx - 1) * sizeof *d);
    dp->nents--;
    if (!is_dots(name) && --ip->nlink <= 0 && ip->refs <= 0)
        inode_free(ip);
}

static struct inode *dir_new(struct inode *parent)
{
    struct inode *ip = inode_new(T_DIR);
    if (!ip)
        return NULL;
    ip->parent = parent ? parent : ip;
    if (dir_add(ip, ".", ip) < 0 || dir_add(ip, "..", ip->parent) < 0) {
        inode_free(ip);
        return NULL;
    }
    return ip;
}

/* ---------------- path resolution ---------------- */

/* Walks `path`.  Without want_parent, *out is the named inode.  With it, *out
 * is the directory containing the last component, copied into `name`. */
static int walk(const char *path, int want_parent, struct inode **out, char *name)
{
    struct inode *ip = (*path == '/' || !cur || !cur->cwd) ? fs_root : cur->cwd;
    char comp[NAME_MAX + 1];

    while (*path) {
        while (*path == '/')
            path++;
        if (!*path)
            break;
        u32 len = 0;
        while (path[len] && path[len] != '/')
            len++;
        if (len > NAME_MAX)
            return -ENAMETOOLONG;
        memcpy(comp, path, len);
        comp[len] = 0;
        path += len;

        const char *rest = path;
        while (*rest == '/')
            rest++;

        if (ip->type != T_DIR)
            return -ENOTDIR;
        if (!*rest && want_parent) {
            *out = ip;
            strcpy(name, comp);
            return 0;
        }
        if (ip->mount_of && !strcmp(comp, ".."))
            ip = ip->mount_of;                  /* leaving a mounted filesystem */
        struct inode *next = lookup(ip, comp);
        if (!next)
            return -ENOENT;
        ip = next->mounted ? next->mounted : next;
    }
    if (want_parent)
        return -EINVAL;
    *out = ip;
    return 0;
}

int fs_lookup(const char *path, struct inode **out)
{
    return walk(path, 0, out, NULL);
}

int fs_create(const char *path, u32 type, struct inode **out)
{
    struct inode *dp;
    char name[NAME_MAX + 1];
    int e = walk(path, 1, &dp, name);
    if (e < 0)
        return e;
    if (dp->pfs)
        return pfs_create(dp, name, type, out);
    if (dir_find(dp, name))
        return -EEXIST;
    struct inode *ip = type == T_DIR ? dir_new(dp) : inode_new(type);
    if (!ip)
        return -ENOSPC;
    if ((e = dir_add(dp, name, ip)) < 0) {
        inode_free(ip);
        return e;
    }
    if (out)
        *out = ip;
    return 0;
}

int fs_unlink(const char *path)
{
    struct inode *dp;
    char name[NAME_MAX + 1];
    int e = walk(path, 1, &dp, name);
    if (e < 0)
        return e;
    if (is_dots(name))
        return -EINVAL;
    if (dp->pfs)
        return pfs_unlink_entry(dp, name);
    struct dent *d = dir_find(dp, name);
    if (!d)
        return -ENOENT;
    if (d->ip == fs_root)
        return -EPERM;
    if (d->ip->mounted)
        return -EBUSY;
    if (d->ip->type == T_DIR && d->ip->nents > 2)
        return -ENOTEMPTY;
    dir_remove(dp, name);
    return 0;
}

int fs_rename(const char *from, const char *to)
{
    struct inode *op, *np;
    char oname[NAME_MAX + 1], nname[NAME_MAX + 1];
    int e;
    if ((e = walk(from, 1, &op, oname)) < 0 || (e = walk(to, 1, &np, nname)) < 0)
        return e;
    if (is_dots(oname) || is_dots(nname))
        return -EINVAL;
    if (op->pfs != np->pfs)
        return -EXDEV;
    if (op->pfs)
        return pfs_rename(op, oname, np, nname);
    struct dent *od = dir_find(op, oname);
    if (!od)
        return -ENOENT;
    struct inode *ip = od->ip;
    if (ip == fs_root || ip->mounted)
        return -EPERM;

    struct dent *nd = dir_find(np, nname);
    if (nd) {
        if (nd->ip == ip)
            return 0;
        if (nd->ip->type == T_DIR) {
            if (ip->type != T_DIR)
                return -EISDIR;
            if (nd->ip->nents > 2)
                return -ENOTEMPTY;
        } else if (ip->type == T_DIR) {
            return -ENOTDIR;
        }
    }
    if (ip->type == T_DIR) {                    /* can't move a dir into itself */
        for (struct inode *a = np;; a = a->parent) {
            if (a == ip)
                return -EINVAL;
            if (a == fs_root)
                break;
        }
    }

    ip->nlink++;                                /* keep alive across the shuffle */
    if (nd)
        dir_remove(np, nname);
    if ((e = dir_add(np, nname, ip)) < 0) {
        ip->nlink--;
        return e;
    }
    dir_remove(op, oname);
    ip->nlink--;
    if (ip->type == T_DIR && np != op) {
        dir_find(ip, "..")->ip = np;
        ip->parent = np;
    }
    return 0;
}

/* Builds the absolute path of directory `ip` by climbing ".." links. */
int fs_getcwd(struct inode *ip, char *buf, u32 size)
{
    char tmp[PATH_MAX];
    u32 pos = sizeof tmp;
    tmp[--pos] = 0;
    for (;;) {
        if (ip->mount_of)
            ip = ip->mount_of;                  /* hop from a mounted root to its mountpoint */
        if (ip == fs_root)
            break;
        struct inode *parent = ip->pfs ? pfs_lookup(ip, "..") : ip->parent;
        char name[NAME_MAX + 1];
        int found = 0;
        if (parent && parent->pfs) {
            found = pfs_name_of(parent, ip->dino, name) == 0;
        } else if (parent) {
            for (u32 i = 0; i < parent->nents; i++) {
                if (parent->ents[i].ip == ip && !is_dots(parent->ents[i].name)) {
                    strcpy(name, parent->ents[i].name);
                    found = 1;
                    break;
                }
            }
        }
        if (!found)
            return -ENOENT;                     /* directory was removed */
        u32 len = strlen(name);
        if (pos < len + 2)
            return -ENAMETOOLONG;
        pos -= len;
        memcpy(tmp + pos, name, len);
        tmp[--pos] = '/';
        ip = parent;
    }
    if (tmp[pos] == 0)
        tmp[--pos] = '/';
    if (strlen(tmp + pos) + 1 > size)
        return -ENAMETOOLONG;
    strcpy(buf, tmp + pos);
    return 0;
}

/* ---------------- boot-time setup ---------------- */

static void make_dev(const char *path, int major)
{
    struct inode *ip;
    if (fs_create(path, T_DEV, &ip) == 0)
        ip->major = major;
}

void fs_init(void)
{
    fs_root = dir_new(NULL);
    if (!fs_root)
        panic("cannot allocate root directory");
    fs_root->nlink = 1;

    fs_create("/dev", T_DIR, NULL);
    fs_create("/disk", T_DIR, NULL);
    make_dev("/dev/console", DEV_CONSOLE);
    make_dev("/dev/tty", DEV_CONSOLE);
    make_dev("/dev/null", DEV_NULL);
    make_dev("/dev/zero", DEV_ZERO);
    make_dev("/dev/random", DEV_RANDOM);
    make_dev("/dev/input", DEV_INPUT);
}

/* The initrd is a flat table of {path, type, size, offset} followed by data. */
struct initrd_hdr {
    char magic[8];
    u32 count, pad;
};

struct initrd_ent {
    char path[96];
    u32 type, size, offset, pad;
};

static void mkdirs(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            fs_create(path, T_DIR, NULL);
            *p = '/';
        }
    }
}

void initrd_load(const u8 *img, u32 size)
{
    const struct initrd_hdr *h = (const void *)img;
    if (size < sizeof *h || memcmp(h->magic, "PTRCHRFS", 8))
        panic("bad initrd image");
    const struct initrd_ent *ents = (const void *)(img + sizeof *h);

    u32 files = 0, bytes = 0;
    for (u32 i = 0; i < h->count; i++) {
        char path[sizeof ents[i].path + 2];
        path[0] = '/';
        safestrcpy(path + 1, ents[i].path, sizeof ents[i].path);
        mkdirs(path);

        if (ents[i].type == T_DIR) {
            fs_create(path, T_DIR, NULL);
            continue;
        }
        struct inode *ip;
        if (fs_create(path, T_FILE, &ip) < 0)
            continue;
        if (ents[i].size && inode_write(ip, 0, img + ents[i].offset, ents[i].size) < 0)
            panic("out of memory unpacking %s", path);
        files++;
        bytes += ents[i].size;
    }
    kprintf("[fs  ] initrd: %u files, %u KiB\n", files, bytes >> 10);
}
