#include "string.h"

void *memcpy(void *dst, const void *src, size_t n)
{
    void *ret = dst;
    size_t words = n >> 2, rest = n & 3;
    __asm__ volatile("cld; rep movsl" : "+D"(dst), "+S"(src), "+c"(words) : : "memory");
    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(rest) : : "memory");
    return ret;
}

void *memmove(void *dst, const void *src, size_t n)
{
    if ((u8 *)dst <= (const u8 *)src || (u8 *)dst >= (const u8 *)src + n)
        return memcpy(dst, src, n);
    u8 *d = (u8 *)dst + n - 1;
    const u8 *s = (const u8 *)src + n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    void *ret = dst;
    u32 v = (u8)c * 0x01010101u;
    size_t words = n >> 2, rest = n & 3;
    __asm__ volatile("cld; rep stosl" : "+D"(dst), "+c"(words) : "a"(v) : "memory");
    __asm__ volatile("rep stosb" : "+D"(dst), "+c"(rest) : "a"(v) : "memory");
    return ret;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (u8)*a - (u8)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b)
        n--, a++, b++;
    return n ? (u8)*a - (u8)*b : 0;
}

char *strcpy(char *dst, const char *src)
{
    char *ret = dst;
    while ((*dst++ = *src++))
        ;
    return ret;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = 0;
    return dst;
}

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

char *strstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n)
        return (char *)hay;
    for (; *hay; hay++)
        if (*hay == *needle && !strncmp(hay, needle, n))
            return (char *)hay;
    return NULL;
}

/* Always NUL-terminates (unlike strncpy). */
void safestrcpy(char *dst, const char *src, size_t n)
{
    if (!n)
        return;
    size_t i = 0;
    for (; i + 1 < n && src[i]; i++)
        dst[i] = src[i];
    dst[i] = 0;
}
