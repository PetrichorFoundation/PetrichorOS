/* Threads for user programs: pthread_* on top of clone (a thread shares the address space), futex (sleep on a
 * word until someone changes it) and settls (each thread's %fs points at its struct pthread).
 *
 * A thread's stack and its struct pthread share one mmap'd block.  The kernel zeroes `alive` and wakes it
 * when the thread ends, which is what pthread_join waits for. */
#include "pthread.h"

#define STACK_SIZE (256 * 1024)

struct pthread {
    struct pthread *self;                    /* %fs:0 */
    volatile u32 alive;                      /* the kernel's clear_tid: 1 while running */
    int tid, detached;
    void *(*fn)(void *);
    void *arg, *ret;
    void *map;
    u32 maplen;
    void *keys[PTHREAD_KEYS_MAX];
};

extern volatile int __ulib_threaded;         /* ulib.c: malloc takes its lock once this is set */
static struct pthread main_thread = {.self = &main_thread, .alive = 1};
static void (*key_dtor[PTHREAD_KEYS_MAX])(void *);
static volatile u32 key_used;

/* ---- memory mappings ---- */

long __mmap(u32 len, int prot);
long __munmap(u32 addr, u32 len);
long __mprotect(u32 addr, u32 len, int prot);
long __clone(void (*entry)(struct pthread *), u32 stack, struct pthread *arg, volatile u32 *ctid);

void *mmap(u32 len, int prot)
{
    long r = __mmap(len, prot);
    if (r < 0 && r > -4096)
        return MAP_FAILED;
    return (void *)(uptr)(u32)r;
}

int munmap(void *a, u32 len) { return (int)__munmap((u32)(uptr)a, len); }
int mprotect(void *a, u32 len, int prot) { return (int)__mprotect((u32)(uptr)a, len, prot); }

/* ---- threads ---- */

pthread_t pthread_self(void)
{
    if (!__ulib_threaded)
        return &main_thread;
    struct pthread *t;
    __asm__ volatile("mov %%fs:0, %0" : "=r"(t));
    return t;
}

int pthread_equal(pthread_t a, pthread_t b) { return a == b; }

static void run_key_dtors(struct pthread *t)
{
    for (int round = 0; round < 4; round++)
        for (int k = 0; k < PTHREAD_KEYS_MAX; k++) {
            void *v = t->keys[k];
            if (v && key_dtor[k] && (key_used & (1u << k))) {
                t->keys[k] = NULL;
                key_dtor[k](v);
            }
        }
}

void pthread_exit(void *ret)
{
    struct pthread *t = pthread_self();
    if (t == &main_thread)
        exit(0);
    t->ret = ret;
    run_key_dtors(t);
    exit(0);                                 /* only this thread: the kernel knows it is one */
}

static void thread_start(struct pthread *t)
{
    settls(t);
    pthread_exit(t->fn(t->arg));
}

int pthread_create(pthread_t *out, const pthread_attr_t *attr, void *(*fn)(void *), void *arg)
{
    (void)attr;
    if (!__ulib_threaded) {
        settls(&main_thread);
        __ulib_threaded = 1;
    }
    u32 len = STACK_SIZE + ((sizeof(struct pthread) + 4095) & ~4095u);
    u8 *m = mmap(len, PROT_READ | PROT_WRITE);
    if (m == MAP_FAILED)
        return EAGAIN;
    struct pthread *t = (struct pthread *)(m + STACK_SIZE);
    t->self = t;
    t->alive = 1;
    t->fn = fn;
    t->arg = arg;
    t->map = m;
    t->maplen = len;
    long tid = __clone(thread_start, (u32)(uptr)t & ~15u, t, &t->alive);
    if (tid < 0) {
        munmap(m, len);
        return EAGAIN;
    }
    t->tid = (int)tid;
    *out = t;
    return 0;
}

int pthread_join(pthread_t t, void **ret)
{
    if (t == pthread_self())
        return EINVAL;
    u32 v;
    while ((v = t->alive))
        futex((u32 *)&t->alive, FUTEX_WAIT, v, 0);
    if (ret)
        *ret = t->ret;
    munmap(t->map, t->maplen);
    return 0;
}

int pthread_detach(pthread_t t)
{
    t->detached = 1;                         /* its block stays mapped: nobody can free a running stack */
    return 0;
}

int sched_yield(void) { return msleep(0); }

/* ---- mutexes: 0 free, 1 locked, 2 locked and someone may be waiting (Drepper, "Futexes are tricky") ---- */

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) { (void)a; m->v = 0; return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }

int pthread_mutex_lock(pthread_mutex_t *m)
{
    u32 c = 0;
    if (__atomic_compare_exchange_n(&m->v, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;
    if (c != 2)
        c = __atomic_exchange_n(&m->v, 2, __ATOMIC_ACQUIRE);
    while (c != 0) {
        futex((u32 *)&m->v, FUTEX_WAIT, 2, 0);
        c = __atomic_exchange_n(&m->v, 2, __ATOMIC_ACQUIRE);
    }
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *m)
{
    u32 c = 0;
    return __atomic_compare_exchange_n(&m->v, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 0 : EBUSY;
}

int pthread_mutex_unlock(pthread_mutex_t *m)
{
    if (__atomic_fetch_sub(&m->v, 1, __ATOMIC_RELEASE) != 1) {
        __atomic_store_n(&m->v, 0, __ATOMIC_RELEASE);
        futex((u32 *)&m->v, FUTEX_WAKE, 1, 0);
    }
    return 0;
}

/* ---- condition variables: a sequence number waiters sleep on ---- */

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) { (void)a; c->seq = 0; return 0; }
int pthread_cond_destroy(pthread_cond_t *c) { (void)c; return 0; }

int pthread_cond_timedwait_ms(pthread_cond_t *c, pthread_mutex_t *m, u32 ms)
{
    u32 seq = c->seq;
    pthread_mutex_unlock(m);
    long r = futex((u32 *)&c->seq, FUTEX_WAIT, seq, ms);
    while (__atomic_exchange_n(&m->v, 2, __ATOMIC_ACQUIRE) != 0)      /* relock, assuming others wait */
        futex((u32 *)&m->v, FUTEX_WAIT, 2, 0);
    return r == -ETIMEDOUT ? ETIMEDOUT : 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { return pthread_cond_timedwait_ms(c, m, 0); }

int pthread_cond_signal(pthread_cond_t *c)
{
    __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
    futex((u32 *)&c->seq, FUTEX_WAKE, 1, 0);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c)
{
    __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
    futex((u32 *)&c->seq, FUTEX_WAKE, 0x7FFFFFFF, 0);
    return 0;
}

/* ---- once and thread-specific data ---- */

int pthread_once(pthread_once_t *o, void (*fn)(void))
{
    int z = 0;
    if (__atomic_compare_exchange_n(o, &z, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
        fn();
        __atomic_store_n(o, 2, __ATOMIC_RELEASE);
        futex((u32 *)o, FUTEX_WAKE, 0x7FFFFFFF, 0);
        return 0;
    }
    while (__atomic_load_n(o, __ATOMIC_ACQUIRE) != 2)
        futex((u32 *)o, FUTEX_WAIT, 1, 0);
    return 0;
}

int pthread_key_create(pthread_key_t *k, void (*dtor)(void *))
{
    for (unsigned i = 0; i < PTHREAD_KEYS_MAX; i++) {
        u32 old = key_used;
        if (old & (1u << i))
            continue;
        if (__atomic_compare_exchange_n(&key_used, &old, old | (1u << i), 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            key_dtor[i] = dtor;
            *k = i;
            return 0;
        }
        i--;                                 /* lost a race: look at this slot again */
    }
    return EAGAIN;
}

int pthread_key_delete(pthread_key_t k)
{
    if (k >= PTHREAD_KEYS_MAX)
        return EINVAL;
    __atomic_fetch_and(&key_used, ~(1u << k), __ATOMIC_ACQ_REL);
    return 0;
}

void *pthread_getspecific(pthread_key_t k)
{
    return k < PTHREAD_KEYS_MAX ? pthread_self()->keys[k] : NULL;
}

int pthread_setspecific(pthread_key_t k, const void *v)
{
    if (k >= PTHREAD_KEYS_MAX)
        return EINVAL;
    pthread_self()->keys[k] = (void *)v;
    return 0;
}
