/* Cryptographic primitives for the TLS client (freestanding; also built on the host for tests). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "sha256.h"

/* ---- hashes ---- */
enum { H_SHA256 = 1, H_SHA384, H_SHA512 };

struct sha512 {
    uint64_t h[8];
    uint8_t buf[128];
    uint32_t fill;
    uint64_t len;
    int outlen;
};

void sha512_init(struct sha512 *s);
void sha384_init(struct sha512 *s);
void sha512_update(struct sha512 *s, const void *data, size_t len);
void sha512_final(struct sha512 *s, uint8_t *out);          /* 48 or 64 bytes */

struct hash {
    int id;
    union {
        struct sha256 a;
        struct sha512 b;
    } u;
};

int hash_size(int id);
int hash_block(int id);
void hash_init(struct hash *h, int id);
void hash_update(struct hash *h, const void *data, size_t len);
void hash_final(struct hash *h, uint8_t *out);

struct hmac {
    struct hash in, out;
};

void hmac_init(struct hmac *h, int id, const uint8_t *key, size_t klen);
void hmac_update(struct hmac *h, const void *data, size_t len);
void hmac_final(struct hmac *h, uint8_t *out);

/* TLS 1.2 PRF: P_hash(secret, label + seed1 + seed2). */
void tls_prf(int hid, const uint8_t *secret, size_t slen, const char *label,
             const uint8_t *seed1, size_t l1, const uint8_t *seed2, size_t l2, uint8_t *out, size_t outlen);

/* ---- AES-GCM (128 or 256 bit keys, 96-bit IVs) ---- */
struct gcm {
    uint32_t rk[60];
    int rounds;
    uint64_t hh[16], hl[16];
};

void gcm_init(struct gcm *g, const uint8_t *key, int keylen);
void gcm_seal(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen,
              const uint8_t *in, size_t n, uint8_t *out, uint8_t tag[16]);
int gcm_open(const struct gcm *g, const uint8_t iv[12], const uint8_t *aad, size_t alen,
             const uint8_t *in, size_t n, uint8_t *out, const uint8_t tag[16]);   /* 0, or -1 if the tag is wrong */

/* ---- big integers (see bn.c) ---- */
typedef uint32_t limb;
#define BN_MAXL 130                       /* 4160 bits */

struct mont {
    int n;
    limb n0;
    limb m[BN_MAXL], one[BN_MAXL], r2[BN_MAXL];
};

int bn_from_bytes(limb *r, int n, const uint8_t *b, size_t len);
void bn_to_bytes(uint8_t *out, size_t len, const limb *a, int n);
int bn_cmp(const limb *a, const limb *b, int n);
limb bn_add(limb *r, const limb *a, const limb *b, int n);
limb bn_sub(limb *r, const limb *a, const limb *b, int n);
int bn_is_zero(const limb *a, int n);
int mont_init(struct mont *c, const limb *m, int n);
void mont_mul(const struct mont *c, limb *r, const limb *a, const limb *b);
void mont_pow(const struct mont *c, limb *r, const limb *a, const limb *e, int en);   /* Montgomery domain in and out */
void mont_exp(const struct mont *c, limb *r, const limb *a, const limb *e, int en);   /* plain a^e mod m */

/* ---- public-key algorithms ---- */
enum { EC_P256 = 23, EC_P384 = 24 };           /* TLS named-group numbers */

int ec_coord_size(int curve);                  /* bytes per coordinate, 0 if unsupported */
int ec_keygen(int curve, const uint8_t *rnd, uint8_t *pub);   /* rnd: coord_size random bytes; pub: 04||X||Y; -1 if rnd unusable */
int ec_ecdh(int curve, const uint8_t *rnd, const uint8_t *peer, size_t peerlen, uint8_t *secret);
int ecdsa_verify(int curve, const uint8_t *pub, size_t publen, const uint8_t *hash, size_t hlen,
                 const uint8_t *r, size_t rl, const uint8_t *s, size_t sl);   /* 0 if valid */
int rsa_verify_pkcs1(const uint8_t *n, size_t nl, const uint8_t *e, size_t el, int hid,
                     const uint8_t *hash, const uint8_t *sig, size_t siglen);   /* 0 if valid */
