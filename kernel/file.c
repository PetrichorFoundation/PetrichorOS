/* Open-file table, devices and pipes. */
#include "kernel.h"

static struct file ftable[NFILE];
static u32 rng_state;

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
    case DEV_NULL:
        return 0;
    case DEV_ZERO:
        memset(dst, 0, n);
        return n;
    case DEV_RANDOM: {
        u8 *p = dst;
        if (!rng_state)
            rng_state = ticks * 2654435761u | 1;
        for (u32 i = 0; i < n; i++) {
            rng_state ^= rng_state << 13;
            rng_state ^= rng_state >> 17;
            rng_state ^= rng_state << 5;
            p[i] = rng_state >> 8;
        }
        return n;
    }
    }
    return -ENODEV;
}

static int dev_write(struct inode *ip, const void *src, u32 n)
{
    switch (ip->major) {
    case DEV_CONSOLE:
        console_write(src, n);
        return n;
    case DEV_NULL:
    case DEV_ZERO:
    case DEV_RANDOM:
        return n;
    }
    return -ENODEV;
}

/* ---------------- file operations ---------------- */

void file_close(struct file *f)
{
    if (--f->refs > 0)
        return;
    if (f->type == FD_PIPE)
        pipe_close(f->pipe, f->writable);
    else if (f->type == FD_INODE)
        iput(f->ip);
    f->type = FD_NONE;
}

int file_read(struct file *f, void *dst, u32 n)
{
    if (!f->readable)
        return -EBADF;
    if (f->type == FD_PIPE)
        return pipe_read(f->pipe, dst, n);
    struct inode *ip = f->ip;
    switch (ip->type) {
    case T_DIR:
        return -EISDIR;
    case T_DEV:
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
    struct inode *ip = f->ip;
    switch (ip->type) {
    case T_DIR:
        return -EISDIR;
    case T_DEV:
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
    if (f->type == FD_PIPE) {
        st->type = T_PIPE;
        st->size = f->pipe->nwrite - f->pipe->nread;
        st->nlink = 1;
        st->ino = 0;
        return 0;
    }
    st->type = f->ip->type;
    st->size = f->ip->type == T_DIR ? f->ip->nents : f->ip->size;
    st->nlink = f->ip->nlink;
    st->ino = f->ip->ino;
    return 0;
}

/* Reads the entry at the file offset (an index) and advances it. */
int file_readdir(struct file *f, struct dirent *de)
{
    if (f->type != FD_INODE || f->ip->type != T_DIR)
        return -ENOTDIR;
    if (f->off >= f->ip->nents)
        return 0;
    struct dent *d = &f->ip->ents[f->off++];
    de->ino = d->ip->ino;
    de->type = d->ip->type;
    safestrcpy(de->name, d->name, sizeof de->name);
    return 1;
}

int file_seek(struct file *f, int off, int whence)
{
    if (f->type != FD_INODE || f->ip->type != T_FILE)
        return -ESPIPE;
    int base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = f->off; break;
    case SEEK_END: base = f->ip->size; break;
    default: return -EINVAL;
    }
    if (base + off < 0)
        return -EINVAL;
    f->off = base + off;
    return f->off;
}
