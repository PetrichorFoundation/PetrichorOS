/* PFS: Petrichor's on-disk filesystem.
 *
 * Partition layout (4 KiB blocks, block numbers relative to the partition):
 *   0           superblock
 *   1           inode bitmap
 *   2           block bitmap
 *   3 ..        inode table (64-byte inodes)
 *   data_start  file and directory data
 *
 * Inodes hold 12 direct blocks and one indirect block (max file ~4 MiB).
 * Directories are files of 32-byte {ino, name} entries; ino 0 marks a free
 * slot.  Every write goes straight through the buffer cache to the disk, so
 * the on-disk image is always current (no sync needed). */
#include "kernel.h"

#define PFS_MAGIC  0x31534650u          /* "PFS1" */
#define BLK        4096
#define SPB        (BLK / 512)
#define NDIRECT    12
#define NINDIRECT  (BLK / 4)
#define NINODES    1024
#define ISIZE      64
#define IPB        (BLK / ISIZE)
#define MAX_BLOCKS (BLK * 8)            /* one bitmap block */
#define MAXFILE    ((NDIRECT + NINDIRECT) * BLK)
#define DESIZE     32

struct psuper {
    u32 magic, nblocks, ninodes, ibitmap, bbitmap, itable, data_start, root;
};

struct pdinode {
    u16 type, nlink;
    u32 size;
    u32 dblk[NDIRECT];
    u32 iblk;
    u32 pad;
};

struct pdent {
    u32 ino;
    char name[NAME_MAX + 1];
};

static int pdev = -1;                   /* the block device holding the partition */
static struct psuper sb;
static int mounted;
static struct inode *icache;

/* ---------------- buffer cache ---------------- */

#define NBUF 64

struct buf {
    u32 blk;
    int valid;
    u32 stamp;
    u8 data[BLK];
};

static struct buf bufs[NBUF];
static u32 stamp;

static struct buf *getbuf(u32 blk, int read)
{
    struct buf *victim = NULL;
    for (int i = 0; i < NBUF; i++) {
        struct buf *b = &bufs[i];
        if (b->valid && b->blk == blk) {
            b->stamp = ++stamp;
            return b;
        }
        /* evict an unused buffer if there is one, else the least recently used */
        if (!victim || (victim->valid && (!b->valid || b->stamp < victim->stamp)))
            victim = b;
    }
    victim->blk = blk;
    victim->valid = 1;
    victim->stamp = ++stamp;
    if (read) {
        if (blk_read(pdev, pfs_base_lba + blk * SPB, SPB, victim->data) < 0) {
            kprintf("[pfs ] read error on block %u\n", blk);
            memset(victim->data, 0, BLK);
        }
    } else {
        memset(victim->data, 0, BLK);
    }
    return victim;
}

static struct buf *bread(u32 blk) { return getbuf(blk, 1); }
static struct buf *bnew(u32 blk) { return getbuf(blk, 0); }

static void bwrite(struct buf *b)
{
    if (blk_write(pdev, pfs_base_lba + b->blk * SPB, SPB, b->data) < 0)
        kprintf("[pfs ] write error on block %u\n", b->blk);
}

/* ---------------- allocation ---------------- */

static int bit_test(const u8 *map, u32 i) { return map[i >> 3] & (1 << (i & 7)); }
static void bit_set(u8 *map, u32 i) { map[i >> 3] |= 1 << (i & 7); }
static void bit_clear(u8 *map, u32 i) { map[i >> 3] &= ~(1 << (i & 7)); }

static u32 balloc(void)
{
    struct buf *b = bread(sb.bbitmap);
    for (u32 i = sb.data_start; i < sb.nblocks; i++) {
        if (bit_test(b->data, i))
            continue;
        bit_set(b->data, i);
        bwrite(b);
        struct buf *z = bnew(i);                /* fresh blocks read back as zeros */
        bwrite(z);
        return i;
    }
    return 0;
}

static void bfree(u32 blk)
{
    if (!blk)
        return;
    struct buf *b = bread(sb.bbitmap);
    bit_clear(b->data, blk);
    bwrite(b);
}

static u32 ialloc(void)
{
    struct buf *b = bread(sb.ibitmap);
    for (u32 i = 1; i < sb.ninodes; i++) {
        if (!bit_test(b->data, i)) {
            bit_set(b->data, i);
            bwrite(b);
            return i;
        }
    }
    return 0;
}

static void ifree(u32 dino)
{
    struct buf *b = bread(sb.ibitmap);
    bit_clear(b->data, dino);
    bwrite(b);
}

/* ---------------- inodes ---------------- */

static void iupdate(struct inode *ip)
{
    struct buf *b = bread(sb.itable + ip->dino / IPB);
    struct pdinode *d = (struct pdinode *)b->data + ip->dino % IPB;
    d->type = ip->type;
    d->nlink = ip->nlink;
    d->size = ip->size;
    memcpy(d->dblk, ip->dblk, sizeof d->dblk);
    d->iblk = ip->iblk;
    bwrite(b);
}

static struct inode *icache_new(u32 dino, u32 type)
{
    struct inode *ip = kzalloc(sizeof *ip);
    if (!ip)
        return NULL;
    ip->pfs = 1;
    ip->dino = dino;
    ip->ino = dino;
    ip->type = type;
    ip->pnext = icache;
    icache = ip;
    return ip;
}

static struct inode *iget(u32 dino)
{
    if (!dino || dino >= sb.ninodes)
        return NULL;
    for (struct inode *ip = icache; ip; ip = ip->pnext)
        if (ip->dino == dino)
            return ip;
    struct buf *b = bread(sb.itable + dino / IPB);
    struct pdinode *d = (struct pdinode *)b->data + dino % IPB;
    if (!d->type)
        return NULL;
    struct inode *ip = icache_new(dino, d->type);
    if (!ip)
        return NULL;
    ip->nlink = d->nlink;
    ip->size = d->size;
    memcpy(ip->dblk, d->dblk, sizeof ip->dblk);
    ip->iblk = d->iblk;
    return ip;
}

/* Maps file block `idx` to a disk block, allocating it when asked to. */
static u32 bmap(struct inode *ip, u32 idx, int alloc)
{
    if (idx < NDIRECT) {
        if (!ip->dblk[idx] && alloc)
            ip->dblk[idx] = balloc();
        return ip->dblk[idx];
    }
    idx -= NDIRECT;
    if (idx >= NINDIRECT)
        return 0;
    if (!ip->iblk) {
        if (!alloc || !(ip->iblk = balloc()))
            return 0;
    }
    u32 blk = ((u32 *)bread(ip->iblk)->data)[idx];
    if (!blk && alloc) {
        blk = balloc();
        if (!blk)
            return 0;
        struct buf *b = bread(ip->iblk);        /* balloc may have recycled the buffer */
        ((u32 *)b->data)[idx] = blk;
        bwrite(b);
    }
    return blk;
}

int pfs_read(struct inode *ip, u32 off, void *dst, u32 n)
{
    if (off >= ip->size)
        return 0;
    if (n > ip->size - off)
        n = ip->size - off;
    u8 *out = dst;
    u32 done = 0;
    while (done < n) {
        u32 pos = off + done, bo = pos % BLK, m = BLK - bo;
        if (m > n - done)
            m = n - done;
        u32 blk = bmap(ip, pos / BLK, 0);
        if (blk)
            memcpy(out + done, bread(blk)->data + bo, m);
        else
            memset(out + done, 0, m);           /* hole */
        done += m;
    }
    return done;
}

int pfs_write(struct inode *ip, u32 off, const void *src, u32 n)
{
    if (off + n < off || off + n > MAXFILE)
        return -EFBIG;
    const u8 *in = src;
    u32 done = 0;
    while (done < n) {
        u32 pos = off + done, bo = pos % BLK, m = BLK - bo;
        if (m > n - done)
            m = n - done;
        u32 blk = bmap(ip, pos / BLK, 1);
        if (!blk)
            break;
        struct buf *b = bread(blk);
        memcpy(b->data + bo, in + done, m);
        bwrite(b);
        done += m;
    }
    if (off + done > ip->size)
        ip->size = off + done;
    iupdate(ip);
    return done ? (int)done : -ENOSPC;
}

void pfs_truncate(struct inode *ip)
{
    for (int i = 0; i < NDIRECT; i++) {
        bfree(ip->dblk[i]);
        ip->dblk[i] = 0;
    }
    if (ip->iblk) {
        for (u32 i = 0; i < NINDIRECT; i++)
            bfree(((u32 *)bread(ip->iblk)->data)[i]);
        bfree(ip->iblk);
        ip->iblk = 0;
    }
    ip->size = 0;
    iupdate(ip);
}

/* Called once nothing links to or has opened the inode. */
void pfs_destroy(struct inode *ip)
{
    pfs_truncate(ip);
    ip->type = 0;
    iupdate(ip);
    ifree(ip->dino);
    for (struct inode **pp = &icache; *pp; pp = &(*pp)->pnext) {
        if (*pp == ip) {
            *pp = ip->pnext;
            break;
        }
    }
    kfree(ip);
}

/* ---------------- directories ---------------- */

static int dget(struct inode *dp, u32 idx, struct pdent *e)
{
    return pfs_read(dp, idx * DESIZE, e, DESIZE) == DESIZE;
}

/* Returns the entry index of `name`, or -1. */
static int dfind(struct inode *dp, const char *name, struct pdent *out)
{
    struct pdent e;
    for (u32 i = 0; dget(dp, i, &e); i++) {
        if (e.ino && !strcmp(e.name, name)) {
            if (out)
                *out = e;
            return i;
        }
    }
    return -1;
}

/* Adds an entry without touching link counts. */
static int dadd(struct inode *dp, const char *name, u32 ino)
{
    struct pdent e;
    u32 slot = dp->size / DESIZE;
    for (u32 i = 0; dget(dp, i, &e); i++) {
        if (!e.ino) {
            slot = i;
            break;
        }
    }
    memset(&e, 0, sizeof e);
    e.ino = ino;
    safestrcpy(e.name, name, sizeof e.name);
    int r = pfs_write(dp, slot * DESIZE, &e, DESIZE);
    return r < 0 ? r : 0;
}

static void dclear(struct inode *dp, u32 idx)
{
    struct pdent e;
    memset(&e, 0, sizeof e);
    pfs_write(dp, idx * DESIZE, &e, DESIZE);
}

struct inode *pfs_lookup(struct inode *dir, const char *name)
{
    struct pdent e;
    if (dfind(dir, name, &e) < 0)
        return NULL;
    return iget(e.ino);
}

u32 pfs_nents(struct inode *dir)
{
    struct pdent e;
    u32 n = 0;
    for (u32 i = 0; dget(dir, i, &e); i++)
        if (e.ino)
            n++;
    return n;
}

int pfs_dirent(struct inode *dir, u32 idx, struct dirent *de)
{
    struct pdent e;
    u32 n = 0;
    for (u32 i = 0; dget(dir, i, &e); i++) {
        if (!e.ino)
            continue;
        if (n++ != idx)
            continue;
        struct inode *ip = iget(e.ino);
        de->ino = e.ino;
        de->type = ip ? ip->type : 0;
        safestrcpy(de->name, e.name, sizeof de->name);
        return 1;
    }
    return 0;
}

int pfs_name_of(struct inode *dir, u32 dino, char *out)
{
    struct pdent e;
    for (u32 i = 0; dget(dir, i, &e); i++) {
        if (e.ino == dino && strcmp(e.name, ".") && strcmp(e.name, "..")) {
            strcpy(out, e.name);
            return 0;
        }
    }
    return -ENOENT;
}

int pfs_create(struct inode *dir, const char *name, u32 type, struct inode **out)
{
    if (type != T_FILE && type != T_DIR)
        return -EPERM;
    if (dfind(dir, name, NULL) >= 0)
        return -EEXIST;
    u32 dino = ialloc();
    if (!dino)
        return -ENOSPC;
    struct inode *ip = icache_new(dino, type);
    if (!ip) {
        ifree(dino);
        return -ENOMEM;
    }
    iupdate(ip);
    int e = 0;
    if (type == T_DIR)
        e = dadd(ip, ".", dino) ?: dadd(ip, "..", dir->dino);
    if (!e)
        e = dadd(dir, name, dino);
    if (e < 0) {
        ip->nlink = 0;
        pfs_destroy(ip);
        return e;
    }
    ip->nlink = 1;
    iupdate(ip);
    if (out)
        *out = ip;
    return 0;
}

/* Removes `name` from dir; frees the target if that was its last link. */
int pfs_unlink_entry(struct inode *dir, const char *name)
{
    struct pdent e;
    int idx = dfind(dir, name, &e);
    if (idx < 0)
        return -ENOENT;
    struct inode *ip = iget(e.ino);
    if (!ip)
        return -EIO;
    if (ip->type == T_DIR && pfs_nents(ip) > 2)
        return -ENOTEMPTY;
    dclear(dir, idx);
    if (--ip->nlink <= 0 && ip->refs <= 0)
        pfs_destroy(ip);
    else
        iupdate(ip);
    return 0;
}

int pfs_rename(struct inode *op, const char *oname, struct inode *np, const char *nname)
{
    struct pdent oe, ne;
    if (dfind(op, oname, &oe) < 0)
        return -ENOENT;
    struct inode *ip = iget(oe.ino);
    if (!ip)
        return -EIO;
    if (dfind(np, nname, &ne) >= 0) {
        if (ne.ino == oe.ino)
            return 0;
        struct inode *tp = iget(ne.ino);
        if (tp && tp->type == T_DIR) {
            if (ip->type != T_DIR)
                return -EISDIR;
            if (pfs_nents(tp) > 2)
                return -ENOTEMPTY;
        } else if (ip->type == T_DIR) {
            return -ENOTDIR;
        }
    }
    if (ip->type == T_DIR) {                    /* not into its own subtree */
        for (struct inode *a = np; a; a = pfs_lookup(a, "..")) {
            if (a == ip)
                return -EINVAL;
            if (a->dino == sb.root)
                break;
        }
    }

    /* A rename just moves the name: the link count is unchanged. */
    if (dfind(np, nname, NULL) >= 0)
        pfs_unlink_entry(np, nname);
    int e = dadd(np, nname, ip->dino);
    if (e < 0)
        return e;
    int idx = dfind(op, oname, NULL);
    if (idx >= 0 && !(op == np && !strcmp(oname, nname)))
        dclear(op, idx);
    if (ip->type == T_DIR && np != op) {
        int di = dfind(ip, "..", NULL);
        struct pdent d = {np->dino, ".."};
        pfs_write(ip, di * DESIZE, &d, DESIZE);
    }
    return 0;
}

/* ---------------- mounting ---------------- */

static int format(u32 nblocks)
{
    kprintf("[pfs ] formatting %u blocks\n", nblocks);
    sb = (struct psuper){PFS_MAGIC, nblocks, NINODES, 1, 2, 3, 3 + NINODES * ISIZE / BLK, 1};
    for (u32 i = 0; i < sb.data_start; i++)
        bwrite(bnew(i));

    struct buf *b = bread(sb.bbitmap);
    for (u32 i = 0; i < sb.data_start; i++)
        bit_set(b->data, i);
    bwrite(b);
    b = bread(sb.ibitmap);
    bit_set(b->data, 0);
    bit_set(b->data, 1);
    bwrite(b);

    struct inode *root = icache_new(1, T_DIR);
    if (!root)
        return -ENOMEM;
    root->nlink = 1;
    iupdate(root);
    if (dadd(root, ".", 1) < 0 || dadd(root, "..", 1) < 0)
        return -EIO;

    b = bnew(0);
    memcpy(b->data, &sb, sizeof sb);
    bwrite(b);
    return 0;
}

int pfs_mount(struct inode *mp)
{
    if (boot_disk < 0 || !blk_present(boot_disk))
        return -ENODEV;
    pdev = boot_disk;
    u32 sectors = blk_sectors(pdev);
    u32 part = pfs_sectors ? pfs_sectors : sectors - pfs_base_lba;    /* GPT installs know their size; BIOS ones run to the end */
    if (sectors <= pfs_base_lba + 64 * SPB || part < 64 * SPB)
        return -ENODEV;
    u32 nblocks = part / SPB;
    if (nblocks > MAX_BLOCKS)
        nblocks = MAX_BLOCKS;

    struct psuper *disk = (struct psuper *)bread(0)->data;
    if (disk->magic == PFS_MAGIC && disk->nblocks == nblocks) {
        sb = *disk;
    } else if (format(nblocks) < 0) {
        return -EIO;
    }

    struct inode *root = iget(sb.root);
    if (!root || root->type != T_DIR)
        return -EIO;
    root->mount_of = mp;
    mp->mounted = root;
    mounted = 1;

    u32 total, free;
    pfs_stats(&total, &free);
    kprintf("[disk] %s %u MiB, PFS %u KiB (%u KiB free) mounted on /disk\n",
            blk_name(pdev), sectors >> 11, total, free);
    return 0;
}

void pfs_stats(u32 *total_kb, u32 *free_kb)
{
    *total_kb = *free_kb = 0;
    if (!mounted)
        return;
    struct buf *b = bread(sb.bbitmap);
    u32 used = 0;
    for (u32 i = 0; i < sb.nblocks; i++)
        if (bit_test(b->data, i))
            used++;
    *total_kb = (sb.nblocks - sb.data_start) * (BLK / 1024);
    *free_kb = (sb.nblocks - used) * (BLK / 1024);
}
