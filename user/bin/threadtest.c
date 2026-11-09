/* threadtest: exercises mmap, threads, mutexes, condition variables and thread-local keys. */
#include "pthread.h"

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static volatile int counter, ready;
static pthread_key_t key;
static int fails;

#define CHECK(c, what) do { if (c) printf("  ok   %s\n", what); else { printf("  FAIL %s\n", what); fails++; } } while (0)

static void *adder(void *arg)
{
    pthread_setspecific(key, arg);
    for (int i = 0; i < 20000; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
        if (i % 1000 == 0)
            free(malloc(64 + i % 300));      /* the heap is shared and locked */
    }
    return pthread_getspecific(key);
}

static void *waiter(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    while (!ready)
        pthread_cond_wait(&cond, &lock);
    pthread_mutex_unlock(&lock);
    return (void *)42;
}

int main(void)
{
    printf("threadtest\n");
    u8 *m = mmap(3 * 4096 + 5, PROT_READ | PROT_WRITE);
    CHECK(m != MAP_FAILED && m[0] == 0 && m[3 * 4096 + 4] == 0, "mmap gives zeroed memory");
    m[100] = 7;
    CHECK(mprotect(m, 4096, PROT_READ) == 0 && m[100] == 7, "mprotect read-only");
    CHECK(munmap(m, 4 * 4096) == 0, "munmap");

    pthread_key_create(&key, NULL);
    pthread_t t[4];
    for (long i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, adder, (void *)(i + 1));
    long sum = 0;
    for (int i = 0; i < 4; i++) {
        void *r;
        pthread_join(t[i], &r);
        sum += (long)r;
    }
    CHECK(counter == 80000, "4 threads x 20000 locked increments");
    CHECK(sum == 10, "thread-specific data and return values");

    pthread_t w;
    pthread_create(&w, NULL, waiter, NULL);
    msleep(50);
    pthread_mutex_lock(&lock);
    ready = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&lock);
    void *r;
    pthread_join(w, &r);
    CHECK((long)r == 42, "condition variable wakes a waiter");

    printf(fails ? "threadtest: %d FAILED\n" : "threadtest: all passed\n", fails);
    return fails != 0;
}
