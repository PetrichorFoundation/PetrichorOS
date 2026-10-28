/* Open-file table, devices and pipes. */
#include "kernel.h"

static struct file ftable[NFILE];

struct file *file_alloc(void)
{
    for (int i = 0; i < NFILE; i++) {
        if (ftable[i].refs == 0) {
            memset(&ftable[i], 0, sizeof ftable[i]);
            ftable[i].refs = 1;
            return &ftable[i];
        }
    }
    return NULL;
}

struct file *file_dup(struct file *f)
{
    f->refs++;
    return f;
}

/* ---------------- pipes ---------------- */

static void pipe_close(struct pipe *p, int writer)
{
    if (writer)
        p->writeopen = 0;
    else
        p->readopen = 0;
    wakeup(&p->nread);
    wakeup(&p->nwrite);
    if (!p->readopen && !p->writeopen)
        kfree(p);
}

static int pipe_read(struct pipe *p, u8 *dst, u32 n)
{
    while (p->nread == p->nwrite) {
        if (!p->writeopen)
            return 0;
        if (cur->killed)
            return -EINTR;
        sleep_on(&p->nread);
    }
    u32 i = 0;
    while (i < n && p->nread != p->nwrite)
        dst[i++] = p->buf[p->nread++ % PIPE_SIZE];
    wakeup(&p->nwrite);
    return i;
}

static int pipe_write(struct pipe *p, const u8 *src, u32 n)
{
    u32 i = 0;
    while (i < n) {
        if (!p->readopen)
            return i ? (int)i : -EPIPE;
        if (p->nwrite - p->nread == PIPE_SIZE) {
            wakeup(&p->nread);
            if (cur->killed)
                return -EINTR;
            sleep_on(&p->nwrite);
            continue;
        }
        p->buf[p->nwrite++ % PIPE_SIZE] = src[i++];
    }
    wakeup(&p->nread);
    return i;
}

int pipe_create(struct file **rf, struct file **wf)
{
    struct pipe *p = kzalloc(sizeof *p);
    struct file *r = file_alloc();
    struct file *w = r ? file_alloc() : NULL;
    if (!p || !r || !w) {
        kfree(p);
        if (r) r->refs = 0;
        if (w) w->refs = 0;
        return -ENOMEM;
    }
    p->readopen = p->writeopen = 1;
    r->type = w->type = FD_PIPE;
    r->pipe = w->pipe = p;
    r->readable = 1;
    w->writable = 1;
    *rf = r;
    *wf = w;
    return 0;
}

/* ---------------- devices ---------------- */

static int dev_read(struct inode *ip, void *dst, u32 n)
{
    switch (ip->major) {
    case DEV_CONSOLE:
        return tty_read(dst, n);
    case DEV_INPUT:
        return input_read(dst, n);
    case DEV_NULL:
        return 0;
    case DEV_ZERO:
        memset(dst, 0, n);
        return n;
    case DEV_RANDOM:
        random_bytes(dst, n);
        return n;
    }
    return -ENODEV;
}

static int dev_write(struct inode *ip, const void *src, u32 n)
{
    switch (ip->major) {
    case DEV_CONSOLE:
        console_write(src, n);
        return n;
    case DEV_RANDOM:
        random_add(src, n);                     /* whatever is written is stirred into the pool */
        return n;
    case DEV_NULL:
    case DEV_ZERO:
        return n;
    }
    return -ENODEV;
}

/* Raw disk access: whole sectors at sector-aligned offsets, up to 128 KiB per call. */
static int disk_rw(struct file *f, void *buf, u32 n, int write)
{
    int dev = f->ip->minor;
    if (f->off % 512)
        return -EINVAL;
    n &= ~511u;
    if (!n)
        return -EINVAL;
    u32 lba = f->off / 512, total = blk_sectors(dev);
    if (lba >= total)
        return 0;                               /* end of disk */
    if (n > 128 * 1024)
        n = 128 * 1024;
    if (lba + n / 512 > total)
        n = (total - lba) * 512;
    int r = write ? blk_write(dev, lba, n / 512, buf) : blk_read(dev, lba, n / 512, buf);
    if (r < 0)
        return r;
    f->off += n;
    return n;
}

static int kimg_read(struct file *f, void *dst, u32 n)
{
    if (f->off >= bootinfo.kimg_size)
        return 0;
    if (n > bootinfo.kimg_size - f->off)
        n = bootinfo.kimg_size - f->off;
    memcpy(dst, (const void *)(bootinfo.kimg + f->off), n);
    f->off += n;
    return n;
}

/* ---------------- file operations ---------------- */

void file_close(struct file *f)
{
    if (--f->refs > 0)
        return;
    if (f->type == FD_PIPE)
        pipe_close(f->pipe, f->writable);
    else if (f->type == FD_SOCK)
        sock_close(f->sock);
    else if (f->type == FD_INODE) {
        if (f->ip->type == T_DEV && f->ip->major == DEV_INPUT)
            input_close();
        iput(f->ip);
    }
    f->type = FD_NONE;
}

int file_read(struct file *f, void *dst, u32 n)
{
    if (!f->readable)
        return -EBADF;
    if (f->type == FD_PIPE)
        return pipe_read(f->pipe, dst, n);
    if (f->type == FD_SOCK)
        return sock_read(f->sock, dst, n);
    struct inode *ip = f->ip;
    switch (ip->type) {
    case T_DIR:
        return -EISDIR;
    case T_DEV:
        if (ip->major == DEV_DISK)
            return disk_rw(f, dst, n, 0);
        if (ip->major == DEV_KIMG)
            return kimg_read(f, dst, n);
        return dev_read(ip, dst, n);
    default: {
        int r = inode_read(ip, f->off, dst, n);
        if (r > 0)
            f->off += r;
        return r;
    }
    }
}

int file_write(struct file *f, const void *src, u32 n)
{
    if (!f->writable)
        return -EBADF;
    if (f->type == FD_PIPE)
        return pipe_write(f->pipe, src, n);
    if (f->type == FD_SOCK)
        return sock_write(f->sock, src, n);
    struct inode *ip = f->ip;
    switch (ip->type) {
    case T_DIR:
        return -EISDIR;
    case T_DEV:
        if (ip->major == DEV_DISK)
            return disk_rw(f, (void *)src, n, 1);
        return dev_write(ip, src, n);
    default: {
        if (f->append)
            f->off = ip->size;
        int r = inode_write(ip, f->off, src, n);
        if (r > 0)
            f->off += r;
        return r;
    }
    }
}

int file_stat(struct file *f, struct stat *st)
{
    if (f->type == FD_SOCK) {
        st->type = T_SOCK;
        st->size = 0;
        st->nlink = 1;
        st->ino = 0;
        return 0;
    }
    if (f->type == FD_PIPE) {
        st->type = T_PIPE;
        st->size = f->pipe->nwrite - f->pipe->nread;
        st->nlink = 1;
        st->ino = 0;
        return 0;
    }
    st->type = f->ip->type;
    st->size = inode_size(f->ip);
    st->nlink = f->ip->nlink;
    st->ino = f->ip->ino;
    return 0;
}

/* Reads the entry at the file offset (an index) and advances it. */
int file_readdir(struct file *f, struct dirent *de)
{
    if (f->type != FD_INODE || f->ip->type != T_DIR)
        return -ENOTDIR;
    int r = inode_dirent(f->ip, f->off, de);
    if (r > 0)
        f->off++;
    return r;
}

int file_seek(struct file *f, int off, int whence)
{
    if (f->type != FD_INODE ||
        !(f->ip->type == T_FILE ||
          (f->ip->type == T_DEV && (f->ip->major == DEV_DISK || f->ip->major == DEV_KIMG))))
        return -ESPIPE;
    int base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = f->off; break;
    case SEEK_END: base = inode_size(f->ip); break;
    default: return -EINVAL;
    }
    if (base + off < 0)
        return -EINVAL;
    f->off = base + off;
    return f->off;
}

/* Would read() return without blocking?  POLLIN, plus POLLHUP for a closed pipe. */
int file_ready(struct file *f)
{
    switch (f->type) {
    case FD_PIPE:
        if (!f->readable)
            return 0;
        if (f->pipe->nread != f->pipe->nwrite)
            return POLLIN;
        return f->pipe->writeopen ? 0 : POLLIN | POLLHUP;
    case FD_SOCK:
        return sock_ready(f->sock) ? POLLIN : 0;
    case FD_INODE:
        if (f->ip->type == T_DEV) {
            if (f->ip->major == DEV_CONSOLE)
                return tty_ready() ? POLLIN : 0;
            if (f->ip->major == DEV_INPUT)
                return input_ready() ? POLLIN : 0;
        }
        return POLLIN;                          /* files and the rest never block */
    }
    return 0;
}
