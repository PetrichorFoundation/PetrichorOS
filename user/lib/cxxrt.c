/* The small C++ runtime a freestanding C++ program needs (built with -fno-exceptions -fno-rtti):
 * operator new/delete, pure-virtual traps, static-destructor registration (__cxa_atexit), and thread-safe
 * initialization of function-local statics (__cxa_guard_*).  Written in C with the Itanium ABI's mangled names. */
#include "ulib.h"

typedef unsigned long size_t_;

static void cxx_abort(const char *why)
{
    fprintf(2, "C++ runtime: %s\n", why);
    exit(134);
}

void *_Znwm(size_t_ n)                                   /* operator new(size_t) */
{
    void *p = malloc((u32)(n ? n : 1));
    if (!p)
        cxx_abort("out of memory in operator new");
    return p;
}
void *_Znam(size_t_ n) { return _Znwm(n); }              /* operator new[] */
void *_ZnwmRKSt9nothrow_t(size_t_ n, const void *t) { (void)t; return malloc((u32)(n ? n : 1)); }
void *_ZnamRKSt9nothrow_t(size_t_ n, const void *t) { (void)t; return malloc((u32)(n ? n : 1)); }
void _ZdlPv(void *p) { free(p); }                        /* operator delete */
void _ZdaPv(void *p) { free(p); }
void _ZdlPvm(void *p, size_t_ n) { (void)n; free(p); }   /* sized delete */
void _ZdaPvm(void *p, size_t_ n) { (void)n; free(p); }
/* aligned new/delete (C++17), alignment up to a page */
void *_ZnwmSt11align_val_t(size_t_ n, size_t_ al)
{
    if (al <= 16)
        return _Znwm(n);
    u8 *raw = malloc((u32)(n + al + 16));
    if (!raw)
        cxx_abort("out of memory in aligned operator new");
    u8 *p = (u8 *)(((uptr)raw + 16 + al - 1) & ~(uptr)(al - 1));
    ((void **)p)[-1] = raw;
    return p;
}
void *_ZnamSt11align_val_t(size_t_ n, size_t_ al) { return _ZnwmSt11align_val_t(n, al); }
void _ZdlPvSt11align_val_t(void *p, size_t_ al) { if (p) free(al <= 16 ? p : ((void **)p)[-1]); }
void _ZdaPvSt11align_val_t(void *p, size_t_ al) { _ZdlPvSt11align_val_t(p, al); }
void _ZdlPvmSt11align_val_t(void *p, size_t_ n, size_t_ al) { (void)n; _ZdlPvSt11align_val_t(p, al); }
void _ZdaPvmSt11align_val_t(void *p, size_t_ n, size_t_ al) { (void)n; _ZdlPvSt11align_val_t(p, al); }

void __cxa_pure_virtual(void) { cxx_abort("pure virtual function called"); }
void __cxa_deleted_virtual(void) { cxx_abort("deleted virtual function called"); }

/* ---- static destructors and atexit, run in reverse order by __cxa_finalize (from _start) ---- */

void *__dso_handle = &__dso_handle;

struct exitfn { void (*fn)(void *); void *arg; };
static struct exitfn *exitfns;
static int nexit, capexit;

int __cxa_atexit(void (*fn)(void *), void *arg, void *dso)
{
    (void)dso;
    if (nexit == capexit) {
        int cap = capexit ? capexit * 2 : 32;
        struct exitfn *n = realloc(exitfns, cap * sizeof *n);
        if (!n)
            return -1;
        exitfns = n;
        capexit = cap;
    }
    exitfns[nexit].fn = fn;
    exitfns[nexit].arg = arg;
    nexit++;
    return 0;
}

static void call_plain(void *f) { ((void (*)(void))f)(); }
int atexit(void (*fn)(void)) { return __cxa_atexit(call_plain, (void *)fn, 0); }

void __cxa_finalize(void *dso)
{
    (void)dso;
    while (nexit > 0) {
        nexit--;
        exitfns[nexit].fn(exitfns[nexit].arg);
    }
}

/* ---- function-local statics: byte 0 of the guard = done, byte 1 = someone is initializing ---- */

int __cxa_guard_acquire(u64 *g)
{
    volatile u8 *b = (volatile u8 *)g;
    for (;;) {
        if (__atomic_load_n(&b[0], __ATOMIC_ACQUIRE))
            return 0;
        u8 z = 0;
        if (__atomic_compare_exchange_n(&b[1], &z, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return 1;
        msleep(0);                       /* another thread is constructing it */
    }
}

void __cxa_guard_release(u64 *g)
{
    volatile u8 *b = (volatile u8 *)g;
    __atomic_store_n(&b[0], 1, __ATOMIC_RELEASE);
    __atomic_store_n(&b[1], 0, __ATOMIC_RELEASE);
}

void __cxa_guard_abort(u64 *g)
{
    __atomic_store_n(&((volatile u8 *)g)[1], 0, __ATOMIC_RELEASE);
}
