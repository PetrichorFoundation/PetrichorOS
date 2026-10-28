/* SHA-384/512, a hash dispatcher, HMAC and the TLS 1.2 PRF. */
#include <string.h>
#include "crypto.h"
#include "tables.inc"

#define ROR64(x, n) (((x) >> (n)) | ((x) << (64 - (n))))

static void sha512_block(struct sha512 *s, const uint8_t *p)
{
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        uint64_t v = 0;
        for (int j = 0; j < 8; j++)
            v = v << 8 | p[i * 8 + j];
        w[i] = v;
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = ROR64(w[i - 15], 1) ^ ROR64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = ROR64(w[i - 2], 19) ^ ROR64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint64_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 80; i++) {
        uint64_t t1 = h + (ROR64(e, 14) ^ ROR64(e, 18) ^ ROR64(e, 41)) + ((e & f) ^ (~e & g)) + SHA512_K[i] + w[i];
        uint64_t t2 = (ROR64(a, 28) ^ ROR64(a, 34) ^ ROR64(a, 39)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha512_setup(struct sha512 *s, const uint64_t *iv, int outlen)
{
    memcpy(s->h, iv, sizeof s->h);
    s->fill = 0;
    s->len = 0;
    s->outlen = outlen;
}

void sha512_init(struct sha512 *s) { sha512_setup(s, SHA512_H, 64); }
void sha384_init(struct sha512 *s) { sha512_setup(s, SHA384_H, 48); }

void sha512_update(struct sha512 *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->len += len;
    while (len) {
        size_t take = 128 - s->fill;
        if (take > len)
            take = len;
        memcpy(s->buf + s->fill, p, take);
        s->fill += take;
        p += take;
        len -= take;
        if (s->fill == 128) {
            sha512_block(s, s->buf);
            s->fill = 0;
        }
    }
}

void sha512_final(struct sha512 *s, uint8_t *out)
{
    uint64_t bits = s->len << 3;
    uint8_t pad[144] = {0x80};
    size_t padlen = (s->fill < 112 ? 112 : 240) - s->fill;
    sha512_update(s, pad, padlen);
    uint8_t lenb[16] = {0};
    for (int i = 0; i < 8; i++)
        lenb[15 - i] = bits >> (8 * i);
    sha512_update(s, lenb, 16);
    for (int i = 0; i < s->outlen; i++)
        out[i] = s->h[i / 8] >> (56 - 8 * (i % 8));
}

/* ---- dispatcher ---- */

int hash_size(int id) { return id == H_SHA256 ? 32 : id == H_SHA384 ? 48 : 64; }
int hash_block(int id) { return id == H_SHA256 ? 64 : 128; }

void hash_init(struct hash *h, int id)
{
    h->id = id;
    if (id == H_SHA256)
        sha256_init(&h->u.a);
    else if (id == H_SHA384)
        sha384_init(&h->u.b);
    else
        sha512_init(&h->u.b);
}

void hash_update(struct hash *h, const void *data, size_t len)
{
    if (h->id == H_SHA256)
        sha256_update(&h->u.a, data, len);
    else
        sha512_update(&h->u.b, data, len);
}

void hash_final(struct hash *h, uint8_t *out)
{
    if (h->id == H_SHA256)
        sha256_final(&h->u.a, out);
    else
        sha512_final(&h->u.b, out);
}

/* ---- HMAC ---- */

void hmac_init(struct hmac *h, int id, const uint8_t *key, size_t klen)
{
    uint8_t k[128] = {0}, pad[128];
    int bs = hash_block(id);
    if (klen > (size_t)bs) {
        struct hash t;
        hash_init(&t, id);
        hash_update(&t, key, klen);
        hash_final(&t, k);
    } else {
        memcpy(k, key, klen);
    }
    for (int i = 0; i < bs; i++)
        pad[i] = k[i] ^ 0x36;
    hash_init(&h->in, id);
    hash_update(&h->in, pad, bs);
    for (int i = 0; i < bs; i++)
        pad[i] = k[i] ^ 0x5c;
    hash_init(&h->out, id);
    hash_update(&h->out, pad, bs);
}

void hmac_update(struct hmac *h, const void *data, size_t len) { hash_update(&h->in, data, len); }

void hmac_final(struct hmac *h, uint8_t *out)
{
    uint8_t inner[64];
    hash_final(&h->in, inner);
    hash_update(&h->out, inner, hash_size(h->in.id));
    hash_final(&h->out, out);
}

/* ---- TLS 1.2 PRF ---- */

void tls_prf(int hid, const uint8_t *secret, size_t slen, const char *label,
             const uint8_t *seed1, size_t l1, const uint8_t *seed2, size_t l2, uint8_t *out, size_t outlen)
{
    size_t ll = strlen(label), hl = hash_size(hid);
    uint8_t a[64], blk[64];
    struct hmac h;

    hmac_init(&h, hid, secret, slen);                        /* A(1) = HMAC(secret, label + seed) */
    hmac_update(&h, label, ll);
    hmac_update(&h, seed1, l1);
    hmac_update(&h, seed2, l2);
    hmac_final(&h, a);
    while (outlen) {
        hmac_init(&h, hid, secret, slen);
        hmac_update(&h, a, hl);
        hmac_update(&h, label, ll);
        hmac_update(&h, seed1, l1);
        hmac_update(&h, seed2, l2);
        hmac_final(&h, blk);
        size_t take = outlen < hl ? outlen : hl;
        memcpy(out, blk, take);
        out += take;
        outlen -= take;
        if (outlen) {
            hmac_init(&h, hid, secret, slen);
            hmac_update(&h, a, hl);
            hmac_final(&h, a);
        }
    }
}
