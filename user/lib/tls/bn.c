/* Fixed-size big integers (little-endian 32-bit limbs) and Montgomery arithmetic.
 * Not constant-time: it only ever handles public values and ephemeral keys. */
#include <string.h>
#include "crypto.h"

int bn_from_bytes(limb *r, int n, const uint8_t *b, size_t len)
{
    while (len && !*b) {
        b++;
        len--;
    }
    if (len > (size_t)n * 4)
        return -1;
    memset(r, 0, n * sizeof *r);
    for (size_t i = 0; i < len; i++)
        r[i / 4] |= (limb)b[len - 1 - i] << (8 * (i % 4));
    return 0;
}

void bn_to_bytes(uint8_t *out, size_t len, const limb *a, int n)
{
    for (size_t i = 0; i < len; i++)
        out[len - 1 - i] = (size_t)(i / 4) < (size_t)n ? a[i / 4] >> (8 * (i % 4)) : 0;
}

int bn_cmp(const limb *a, const limb *b, int n)
{
    for (int i = n - 1; i >= 0; i--)
        if (a[i] != b[i])
            return a[i] > b[i] ? 1 : -1;
    return 0;
}

int bn_is_zero(const limb *a, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i])
            return 0;
    return 1;
}

limb bn_add(limb *r, const limb *a, const limb *b, int n)
{
    uint64_t c = 0;
    for (int i = 0; i < n; i++) {
        c += (uint64_t)a[i] + b[i];
        r[i] = (limb)c;
        c >>= 32;
    }
    return (limb)c;
}

limb bn_sub(limb *r, const limb *a, const limb *b, int n)
{
    int64_t c = 0;
    for (int i = 0; i < n; i++) {
        c += (int64_t)a[i] - b[i];
        r[i] = (limb)c;
        c >>= 32;
    }
    return (limb)(c & 1);
}

/* x = 2x mod m, for x < m */
static void dbl_mod(limb *x, const limb *m, int n)
{
    limb carry = x[n - 1] >> 31;
    for (int i = n - 1; i > 0; i--)
        x[i] = x[i] << 1 | x[i - 1] >> 31;
    x[0] <<= 1;
    if (carry || bn_cmp(x, m, n) >= 0)
        bn_sub(x, x, m, n);
}

int mont_init(struct mont *c, const limb *m, int n)
{
    if (n < 1 || n > BN_MAXL || !(m[0] & 1) || !m[n - 1])
        return -1;
    c->n = n;
    memcpy(c->m, m, n * sizeof *m);
    limb inv = m[0];                                          /* Newton iteration for m^-1 mod 2^32 */
    for (int i = 0; i < 5; i++)
        inv *= 2 - m[0] * inv;
    c->n0 = -inv;
    memset(c->one, 0, sizeof c->one);
    c->one[0] = 1;
    if (n == 1 && m[0] == 1)
        return -1;
    for (int i = 0; i < 32 * n; i++)                          /* R mod m */
        dbl_mod(c->one, c->m, n);
    memcpy(c->r2, c->one, sizeof c->r2);
    for (int i = 0; i < 32 * n; i++)                          /* R^2 mod m */
        dbl_mod(c->r2, c->m, n);
    return 0;
}

/* r = a * b / R mod m (CIOS); inputs < m, r may alias a or b */
void mont_mul(const struct mont *c, limb *r, const limb *a, const limb *b)
{
    int n = c->n;
    limb t[BN_MAXL + 2];
    memset(t, 0, (n + 2) * sizeof *t);
    for (int i = 0; i < n; i++) {
        uint64_t s, carry = 0;
        for (int j = 0; j < n; j++) {
            s = (uint64_t)t[j] + (uint64_t)a[j] * b[i] + carry;
            t[j] = (limb)s;
            carry = s >> 32;
        }
        s = (uint64_t)t[n] + carry;
        t[n] = (limb)s;
        t[n + 1] = (limb)(s >> 32);
        limb q = t[0] * c->n0;
        s = (uint64_t)t[0] + (uint64_t)q * c->m[0];
        carry = s >> 32;
        for (int j = 1; j < n; j++) {
            s = (uint64_t)t[j] + (uint64_t)q * c->m[j] + carry;
            t[j - 1] = (limb)s;
            carry = s >> 32;
        }
        s = (uint64_t)t[n] + carry;
        t[n - 1] = (limb)s;
        t[n] = t[n + 1] + (limb)(s >> 32);
    }
    if (t[n] || bn_cmp(t, c->m, n) >= 0)
        bn_sub(t, t, c->m, n);
    memcpy(r, t, n * sizeof *r);
}

/* r = a^e in the Montgomery domain (a is already multiplied by R) */
void mont_pow(const struct mont *c, limb *r, const limb *a, const limb *e, int en)
{
    limb acc[BN_MAXL], base[BN_MAXL];
    int n = c->n;
    memcpy(base, a, n * sizeof *a);
    memcpy(acc, c->one, n * sizeof *acc);
    for (int i = en * 32 - 1; i >= 0; i--) {
        mont_mul(c, acc, acc, acc);
        if (e[i / 32] >> (i % 32) & 1)
            mont_mul(c, acc, acc, base);
    }
    memcpy(r, acc, n * sizeof *r);
}

void mont_exp(const struct mont *c, limb *r, const limb *a, const limb *e, int en)
{
    limb am[BN_MAXL], one[BN_MAXL];
    int n = c->n;
    mont_mul(c, am, a, c->r2);
    mont_pow(c, am, am, e, en);
    memset(one, 0, n * sizeof *one);
    one[0] = 1;
    mont_mul(c, r, am, one);
}
