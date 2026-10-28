/* AES (encryption only) and GCM. */
#include <string.h>
#include "crypto.h"
#include "tables.inc"

static inline uint32_t ror32(uint32_t x, int n) { return x >> n | x << (32 - n); }

static uint32_t sub_word(uint32_t w)
{
    return (uint32_t)AES_SBOX[w >> 24] << 24 | (uint32_t)AES_SBOX[(w >> 16) & 255] << 16 |
           (uint32_t)AES_SBOX[(w >> 8) & 255] << 8 | AES_SBOX[w & 255];
}

static void aes_expand(struct gcm *g, const uint8_t *key, int keylen)
{
    int nk = keylen / 4;
    g->rounds = nk + 6;
    int total = 4 * (g->rounds + 1);
    for (int i = 0; i < nk; i++)
        g->rk[i] = (uint32_t)key[4 * i] << 24 | key[4 * i + 1] << 16 | key[4 * i + 2] << 8 | key[4 * i + 3];
    uint32_t rcon = 1;
    for (int i = nk; i < total; i++) {
        uint32_t t = g->rk[i - 1];
        if (i % nk == 0) {
            t = sub_word(t << 8 | t >> 24) ^ rcon << 24;
            rcon = (rcon << 1) ^ (rcon & 0x80 ? 0x11b : 0);
        } else if (nk > 6 && i % nk == 4) {
            t = sub_word(t);
        }
        g->rk[i] = g->rk[i - nk] ^ t;
    }
}

static void aes_encrypt(const struct gcm *g, const uint8_t in[16], uint8_t out[16])
{
    uint32_t s[4], t[4];
    for (int i = 0; i < 4; i++)
        s[i] = ((uint32_t)in[4 * i] << 24 | in[4 * i + 1] << 16 | in[4 * i + 2] << 8 | in[4 * i + 3]) ^ g->rk[i];
    for (int r = 1; r < g->rounds; r++) {
        const uint32_t *rk = g->rk + 4 * r;
        for (int i = 0; i < 4; i++)
            t[i] = AES_TE0[s[i] >> 24] ^ ror32(AES_TE0[(s[(i + 1) & 3] >> 16) & 255], 8) ^
                   ror32(AES_TE0[(s[(i + 2) & 3] >> 8) & 255], 16) ^ ror32(AES_TE0[s[(i + 3) & 3] & 255], 24) ^ rk[i];
        memcpy(s, t, sizeof s);
    }
    const uint32_t *rk = g->rk + 4 * g->rounds;
    for (int i = 0; i < 4; i++) {
        uint32_t v = (uint32_t)AES_SBOX[s[i] >> 24] << 24 | (uint32_t)AES_SBOX[(s[(i + 1) & 3] >> 16) & 255] << 16 |
                     (uint32_t)AES_SBOX[(s[(i + 2) & 3] >> 8) & 255] << 8 | AES_SBOX[s[(i + 3) & 3] & 255];
        v ^= rk[i];
        out[4 * i] = v >> 24; out[4 * i + 1] = v >> 16; out[4 * i + 2] = v >> 8; out[4 * i + 3] = v;
    }
}

/* ---- GHASH with 4-bit tables ---- */

static const uint16_t LAST4[16] = {0x0000, 0x1c20, 0x3840, 0x2460, 0x7080, 0x6ca0, 0x48c0, 0x54e0,
                                   0xe100, 0xfd20, 0xd940, 0xc560, 0x9180, 0x8da0, 0xa9c0, 0xb5e0};

static uint64_t be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = v << 8 | p[i];
    return v;
}

static void put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = v >> (56 - 8 * i);
}

void gcm_init(struct gcm *g, const uint8_t *key, int keylen)
{
    aes_expand(g, key, keylen);
    uint8_t zero[16] = {0}, h[16];
    aes_encrypt(g, zero, h);
    uint64_t vh = be64(h), vl = be64(h + 8);
    g->hl[8] = vl;
    g->hh[8] = vh;
    g->hl[0] = g->hh[0] = 0;
    for (int i = 4; i > 0; i >>= 1) {
        uint32_t t = (vl & 1) * 0xe1000000u;
        vl = vh << 63 | vl >> 1;
        vh = (vh >> 1) ^ ((uint64_t)t << 32);
        g->hl[i] = vl;
        g->hh[i] = vh;
    }
    for (int i = 2; i < 16; i <<= 1)
        for (int j = 1; j < i; j++) {
            g->hh[i + j] = g->hh[i] ^ g->hh[j];
            g->hl[i + j] = g->hl[i] ^ g->hl[j];
        }
}

/* x = x * H in GF(2^128) */
static void ghash_mul(const struct gcm *g, uint8_t x[16])
{
    int lo = x[15] & 15;
    uint64_t zh = g->hh[lo], zl = g->hl[lo];
    for (int i = 15; i >= 0; i--) {
        lo = x[i] & 15;
        int hi = x[i] >> 4;
        if (i != 15) {
            int rem = zl & 15;
            zl = zh << 60 | zl >> 4;
            zh >>= 4;
            zh ^= (uint64_t)LAST4[rem] << 48;
            zh ^= g->hh[lo];
            zl ^= g->hl[lo];
        }
        int rem = zl & 15;
        zl = zh << 60 | zl >> 4;
        zh >>= 4;
        zh ^= (uint64_t)LAST4[rem] << 48;
        zh ^= g->hh[hi];
        zl ^= g->hl[hi];
    }
    put64(x, zh);
    put64(x + 8, zl);
}

static void ghash_update(const struct gcm *g, uint8_t y[16], const uint8_t *d, size_t n)
{
    while (n) {
        size_t take = n < 16 ? n : 16;
        for (size_t i = 0; i < take; i++)
            y[i] ^= d[i];
        ghash_mul(g, y);
        d += take;
        n -= take;
    }
}

static void ctr_xor(const struct gcm *g, const uint8_t iv[12], const uint8_t *in, size_t n, uint8_t *out)
{
    uint8_t ctr[16], ks[16];
    memcpy(ctr, iv, 12);
    uint32_t c = 1;
    while (n) {
        c++;
        ctr[12] = c >> 24; ctr[13] = c >> 16; ctr[14] = c >> 8; ctr[15] = c;
        aes_encrypt(g, ctr, ks);
        size_t take = n < 16 ? n : 16;
        for (size_t i = 0; i < take; i++)
            out[i] = in[i] ^ ks[i];
        in += take;
        out += take;
        n -= take;
    }
}

static void make_tag(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen,
                     const uint8_t *ct, size_t n, uint8_t tag[16])
{
    uint8_t y[16] = {0}, lens[16], j0[16], ek[16];
    ghash_update(g, y, aad, alen);
    ghash_update(g, y, ct, n);
    put64(lens, (uint64_t)alen * 8);
    put64(lens + 8, (uint64_t)n * 8);
    ghash_update(g, y, lens, 16);
    memcpy(j0, iv, 12);
    j0[12] = j0[13] = j0[14] = 0;
    j0[15] = 1;
    aes_encrypt(g, j0, ek);
    for (int i = 0; i < 16; i++)
        tag[i] = y[i] ^ ek[i];
}

void gcm_seal(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen,
              const uint8_t *in, size_t n, uint8_t *out, uint8_t tag[16])
{
    ctr_xor(g, iv, in, n, out);
    make_tag(g, iv, aad, alen, out, n, tag);
}

int gcm_open(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen,
             const uint8_t *in, size_t n, uint8_t *out, const uint8_t tag[16])
{
    uint8_t want[16];
    make_tag(g, iv, aad, alen, in, n, want);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++)
        diff |= want[i] ^ tag[i];
    if (diff)
        return -1;
    ctr_xor(g, iv, in, n, out);
    return 0;
}
