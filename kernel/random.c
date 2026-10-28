/* Entropy pool and generator behind /dev/random.
 *
 * Seeded at boot from the clock, the CPU's hardware generator when it has one (RDRAND) and timing jitter;
 * interrupts (timer, keyboard, mouse, network) keep stirring the pool with the cycle counter.  Output is
 * SHA-256 over a secret key and a counter, and the key is replaced after every request.  This is only as
 * unpredictable as the machine's timing noise: good enough for TLS session keys on real hardware, and
 * much weaker inside an emulator without RDRAND. */
#include "kernel.h"
#include "sha256.h"

static u32 pool[8];
static u32 pool_pos, events;
static u8 key[32];
static u32 counter;
static int have_rdrand, seeded;

static inline u32 rdtsc_lo(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return lo ^ hi;
}

static int cpu_has_rdrand(void)
{
    u32 a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return c >> 30 & 1;
}

static int rdrand32(u32 *v)
{
    u8 ok;
    for (int i = 0; i < 10; i++) {
        __asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok) : : "cc");
        if (ok)
            return 1;
    }
    return 0;
}

/* Cheap enough for interrupt handlers. */
void random_event(u32 v)
{
    u32 t = rdtsc_lo();
    u32 i = pool_pos++ & 7;
    u32 x = pool[i] ^ t ^ (v * 2654435761u);
    pool[i] = (x << 7 | x >> 25) + events++;
}

static void stir(const void *extra, u32 n)
{
    struct sha256 s;
    sha256_init(&s);
    sha256_update(&s, "key", 3);
    sha256_update(&s, key, sizeof key);
    sha256_update(&s, pool, sizeof pool);
    u32 t = rdtsc_lo();
    sha256_update(&s, &t, sizeof t);
    u32 r;
    if (have_rdrand && rdrand32(&r))
        sha256_update(&s, &r, sizeof r);
    if (n)
        sha256_update(&s, extra, n);
    sha256_final(&s, key);
}

void random_add(const void *data, u32 n)
{
    stir(data, n);
}

void random_init(void)
{
    have_rdrand = cpu_has_rdrand();
    u32 clock[6];
    for (int i = 0; i < 6; i++) {
        static const u8 regs[6] = {0, 2, 4, 7, 8, 9};
        outb(0x70, regs[i]);
        clock[i] = inb(0x71);
    }
    for (int i = 0; i < 512; i++) {                         /* the time each slow port read takes is noisy */
        outb(0x70, i & 15);
        random_event(inb(0x71));
    }
    stir(clock, sizeof clock);
    if (have_rdrand)
        for (int i = 0; i < 8; i++)
            stir(NULL, 0);
    seeded = 1;
}

void random_bytes(void *dst, u32 n)
{
    u8 *p = dst;
    if (!seeded)
        random_init();
    while (n) {
        struct sha256 s;
        u8 block[32];
        sha256_init(&s);
        sha256_update(&s, "out", 3);
        sha256_update(&s, key, sizeof key);
        counter++;
        sha256_update(&s, &counter, sizeof counter);
        sha256_final(&s, block);
        u32 take = n < 32 ? n : 32;
        memcpy(p, block, take);
        p += take;
        n -= take;
    }
    stir(NULL, 0);                                          /* forward secrecy: replace the key */
}
