#include "ulib.h"
#include "format.h"

/* Program entry: the kernel leaves {fake return, argc, argv} on the stack. */
int main(int argc, char **argv);

/* Static constructors (C++ globals, __attribute__((constructor))) run before main, from user.ld's tables. */
typedef void (*initfn)(void);
extern initfn __init_array_start[], __init_array_end[];
void __cxa_finalize(void *dso);

void _start(int argc, char **argv)
{
    for (initfn *f = __init_array_start; f < __init_array_end; f++)
        (*f)();
    int r = main(argc, argv);
    __cxa_finalize(0);                   /* C++ static destructors and atexit handlers (cxxrt.c) */
    exit(r);
}

int wait(int *status)
{
    return wait2(status, 0);
}

/* ---------------- stdio ---------------- */

struct outbuf {
    int fd, n;
    char buf[256];
};

static void flush(struct outbuf *o)
{
    int off = 0;
    while (off < o->n) {
        int w = write(o->fd, o->buf + off, o->n - off);
        if (w <= 0)
            break;
        off += w;
    }
    o->n = 0;
}

static void out_char(char c, void *ctx)
{
    struct outbuf *o = ctx;
    if (o->n == sizeof o->buf)
        flush(o);
    o->buf[o->n++] = c;
}

int vfprintf(int fd, const char *fmt, va_list ap)
{
    struct outbuf o = {.fd = fd};
    int n = vformat(out_char, &o, fmt, ap);
    flush(&o);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(1, fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

struct strbuf {
    char *buf;
    u32 size, n;
};

static void str_char(char c, void *ctx)
{
    struct strbuf *s = ctx;
    if (s->n + 1 < s->size)
        s->buf[s->n] = c;
    s->n++;
}

int snprintf(char *buf, u32 size, const char *fmt, ...)
{
    struct strbuf s = {buf, size, 0};
    va_list ap;
    va_start(ap, fmt);
    vformat(str_char, &s, fmt, ap);
    va_end(ap);
    if (size)
        buf[s.n < size ? s.n : size - 1] = 0;
    return s.n;
}

void fputs(int fd, const char *s)
{
    write(fd, s, strlen(s));
}

void puts(const char *s)
{
    fputs(1, s);
    write(1, "\n", 1);
}

/* Reads one line (without the newline).  Returns -1 at EOF with nothing read. */
int read_line(int fd, char *buf, int max)
{
    int n = 0;
    char c;
    while (n < max - 1) {
        int r = read(fd, &c, 1);
        if (r <= 0)
            return n ? (buf[n] = 0, n) : -1;
        if (c == '\n')
            break;
        buf[n++] = c;
    }
    buf[n] = 0;
    return n;
}

int atoi(const char *s)
{
    int sign = 1, v = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-' || *s == '+')
        sign = *s++ == '-' ? -1 : 1;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v * sign;
}

const char *strerror(int err)
{
    if (err < 0)
        err = -err;
    switch (err) {
    case 0: return "success";
    case EPERM: return "operation not permitted";
    case EPROTO: return "secure connection failed";
    case ENOENT: return "no such file or directory";
    case ESRCH: return "no such process";
    case EINTR: return "interrupted";
    case EIO: return "I/O error";
    case E2BIG: return "argument list too long";
    case ENOEXEC: return "exec format error";
    case EBADF: return "bad file descriptor";
    case ECHILD: return "no child processes";
    case EAGAIN: return "resource temporarily unavailable";
    case ENOMEM: return "out of memory";
    case EACCES: return "permission denied";
    case EFAULT: return "bad address";
    case EBUSY: return "device or resource busy";
    case EXDEV: return "cross-device link";
    case ENOTSOCK: return "not a socket";
    case EADDRINUSE: return "address in use";
    case ENETDOWN: return "network is down";
    case ECONNRESET: return "connection reset";
    case EISCONN: return "already connected";
    case ENOTCONN: return "not connected";
    case ETIMEDOUT: return "timed out";
    case ECONNREFUSED: return "connection refused";
    case EHOSTUNREACH: return "host unreachable";
    case EMSGSIZE: return "message too long";
    case EEXIST: return "file exists";
    case ENODEV: return "no such device";
    case ENOTDIR: return "not a directory";
    case EISDIR: return "is a directory";
    case EINVAL: return "invalid argument";
    case EMFILE: return "too many open files";
    case EFBIG: return "file too large";
    case ENOSPC: return "no space left";
    case ESPIPE: return "illegal seek";
    case EPIPE: return "broken pipe";
    case ENAMETOOLONG: return "name too long";
    case ENOSYS: return "function not implemented";
    case ENOTEMPTY: return "directory not empty";
    }
    return "unknown error";
}

/* ---------------- malloc: first fit over sbrk ---------------- */

struct mblock {                      /* 16 bytes, so every block's data is 16-byte aligned (SSE code relies on it) */
    u32 size;
    u32 free;
    struct mblock *next;
};
_Static_assert(sizeof(struct mblock) == 16, "malloc headers must keep 16-byte alignment");

static struct mblock *mhead, *mtail;

/* Once a program starts a thread (pthread.c sets this), malloc and free take a futex lock. */
volatile int __ulib_threaded;
static volatile u32 mlock;

static void heap_lock(void)
{
    if (!__ulib_threaded)
        return;
    u32 c = 0;
    if (__atomic_compare_exchange_n(&mlock, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return;
    if (c != 2)
        c = __atomic_exchange_n(&mlock, 2, __ATOMIC_ACQUIRE);
    while (c) {
        futex((u32 *)&mlock, FUTEX_WAIT, 2, 0);
        c = __atomic_exchange_n(&mlock, 2, __ATOMIC_ACQUIRE);
    }
}

static void heap_unlock(void)
{
    if (!mlock)                          /* not threaded: never taken */
        return;
    if (__atomic_fetch_sub(&mlock, 1, __ATOMIC_RELEASE) != 1) {
        __atomic_store_n(&mlock, 0, __ATOMIC_RELEASE);
        futex((u32 *)&mlock, FUTEX_WAKE, 1, 0);
    }
}

static void *malloc_locked(u32 n);
static void free_locked(void *p);

void *malloc(u32 n)
{
    heap_lock();
    void *p = malloc_locked(n);
    heap_unlock();
    return p;
}

void free(void *p)
{
    heap_lock();
    free_locked(p);
    heap_unlock();
}

static void *malloc_locked(u32 n)
{
    n = (n + 15) & ~15u;
    if (!n)
        n = 16;
    for (struct mblock *b = mhead; b; b = b->next) {
        if (!b->free || b->size < n)
            continue;
        if (b->size >= n + sizeof *b + 16) {
            struct mblock *nb = (struct mblock *)((char *)(b + 1) + n);
            nb->size = b->size - n - sizeof *b;
            nb->free = 1;
            nb->next = b->next;
            b->next = nb;
            b->size = n;
            if (mtail == b)
                mtail = nb;
        }
        b->free = 0;
        return b + 1;
    }
    u32 grow = (n + sizeof(struct mblock) + 4095) & ~4095u;
    struct mblock *b = sbrk(grow);
    if (b == (void *)-1 || (long)b < 0)
        return NULL;
    b->size = grow - sizeof *b;
    b->free = 0;
    b->next = NULL;
    if (mtail)
        mtail->next = b;
    else
        mhead = b;
    mtail = b;
    if (b->size >= n + sizeof *b + 16) {
        struct mblock *nb = (struct mblock *)((char *)(b + 1) + n);
        nb->size = b->size - n - sizeof *b;
        nb->free = 1;
        nb->next = NULL;
        b->next = nb;
        b->size = n;
        mtail = nb;
    }
    return b + 1;
}

static void free_locked(void *p)
{
    if (!p)
        return;
    struct mblock *b = (struct mblock *)p - 1;
    b->free = 1;
    for (struct mblock *m = mhead; m; m = m->next) {   /* coalesce neighbours */
        while (m->free && m->next && m->next->free &&
               (char *)(m + 1) + m->size == (char *)m->next) {
            if (mtail == m->next)
                mtail = m;
            m->size += sizeof *m + m->next->size;
            m->next = m->next->next;
        }
    }
}

void *calloc(u32 n, u32 size)
{
    void *p = malloc(n * size);
    if (p)
        memset(p, 0, n * size);
    return p;
}

void *realloc(void *p, u32 n)
{
    if (!p)
        return malloc(n);
    heap_lock();
    struct mblock *b = (struct mblock *)p - 1;
    if (b->size >= n) {
        heap_unlock();
        return p;
    }
    void *q = malloc_locked(n);
    if (q) {
        memcpy(q, p, b->size);
        free_locked(p);
    }
    heap_unlock();
    return q;
}
