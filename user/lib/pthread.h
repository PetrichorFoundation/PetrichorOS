#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* POSIX-style threads (user/lib/pthread.c), on the kernel's clone and futex system calls. */
#include "ulib.h"

typedef struct pthread *pthread_t;
typedef struct { int unused; } pthread_attr_t;
typedef struct { volatile u32 v; } pthread_mutex_t;        /* 0 free, 1 locked, 2 locked with waiters */
typedef struct { int unused; } pthread_mutexattr_t;
typedef struct { volatile u32 seq; } pthread_cond_t;
typedef struct { int unused; } pthread_condattr_t;
typedef volatile int pthread_once_t;
typedef unsigned pthread_key_t;

#define PTHREAD_MUTEX_INITIALIZER {0}
#define PTHREAD_COND_INITIALIZER  {0}
#define PTHREAD_ONCE_INIT 0
#define PTHREAD_KEYS_MAX 32

int pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **ret);
int pthread_detach(pthread_t t);
void pthread_exit(void *ret) __attribute__((noreturn));
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);

int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a);
int pthread_cond_destroy(pthread_cond_t *c);
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
int pthread_cond_timedwait_ms(pthread_cond_t *c, pthread_mutex_t *m, u32 ms);   /* ETIMEDOUT on timeout */
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

int pthread_once(pthread_once_t *o, void (*fn)(void));
int pthread_key_create(pthread_key_t *k, void (*dtor)(void *));
int pthread_key_delete(pthread_key_t k);
void *pthread_getspecific(pthread_key_t k);
int pthread_setspecific(pthread_key_t k, const void *v);

int sched_yield(void);

#ifdef __cplusplus
}
#endif
