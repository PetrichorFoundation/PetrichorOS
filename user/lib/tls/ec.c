/* NIST P-256 / P-384: ECDH and ECDSA verification (Jacobian coordinates, a = -3). */
#include <string.h>
#include "crypto.h"

#define MAXL 12

struct curve {
    int id, bytes, n;
    struct mont p, q;                      /* field prime and group order */
    limb b[MAXL];                          /* curve constant, Montgomery form */
    limb gx[MAXL], gy[MAXL];               /* base point, Montgomery form */
    limb pm2[MAXL], qm2[MAXL];             /* p-2 and q-2 (for inversion) */
    int ready;
};

struct pt {
    limb x[MAXL], y[MAXL], z[MAXL];        /* z == 0: the point at infinity */
};

static const char *const P256[] = {
    "ffffffff00000001000000000000000000000000ffffffffffffffffffffffff",
    "ffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551",
    "5ac635d8aa3a93e7b3ebbd55769886bc651d06b0cc53b0f63bce3c3e27d2604b",
    "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",
    "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5",
};
static const char *const P384[] = {
    "fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffeffffffff0000000000000000ffffffff",
    "ffffffffffffffffffffffffffffffffffffffffffffffffc7634d81f4372ddf581a0db248b0a77aecec196accc52973",
    "b3312fa7e23ee7e4988e056be3f82d19181d9c6efe8141120314088f5013875ac656398d8a2ed19d2a85c8edd3ec2aef",
    "aa87ca22be8b05378eb1c71ef320ad746e1d3b628ba79b9859f741e082542a385502f25dbf55296c3a545e3872760ab7",
    "3617de4a96262c6f5d9e98bf9292dc29f8f41dbd289a147ce9da3113b5f0b8c00a60b1ce1d7e819d7a431d7c90ea0e5f",
};

static struct curve c256 = {.id = EC_P256, .bytes = 32, .n = 8};
static struct curve c384 = {.id = EC_P384, .bytes = 48, .n = 12};

static int hexval(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }

static void hex_to_limbs(limb *r, int n, const char *hex)
{
    uint8_t b[48];
    int len = strlen(hex) / 2;
    for (int i = 0; i < len; i++)
        b[i] = hexval(hex[2 * i]) << 4 | hexval(hex[2 * i + 1]);
    bn_from_bytes(r, n, b, len);
}

static void to_mont(const struct curve *c, limb *r, const limb *a) { mont_mul(&c->p, r, a, c->p.r2); }

static void sub_small(limb *r, const limb *a, int n, limb v)
{
    limb t[MAXL] = {0};
    t[0] = v;
    bn_sub(r, a, t, n);
}

static struct curve *curve_get(int id)
{
    struct curve *c = id == EC_P256 ? &c256 : id == EC_P384 ? &c384 : NULL;
    if (!c || c->ready)
        return c;
    const char *const *k = id == EC_P256 ? P256 : P384;
    limb v[MAXL];
    hex_to_limbs(v, c->n, k[0]);
    mont_init(&c->p, v, c->n);
    sub_small(c->pm2, v, c->n, 2);
    hex_to_limbs(v, c->n, k[1]);
    mont_init(&c->q, v, c->n);
    sub_small(c->qm2, v, c->n, 2);
    hex_to_limbs(v, c->n, k[2]);
    to_mont(c, c->b, v);
    hex_to_limbs(v, c->n, k[3]);
    to_mont(c, c->gx, v);
    hex_to_limbs(v, c->n, k[4]);
    to_mont(c, c->gy, v);
    c->ready = 1;
    return c;
}

int ec_coord_size(int curve) { return curve == EC_P256 ? 32 : curve == EC_P384 ? 48 : 0; }

/* ---- field arithmetic (Montgomery form) ---- */

static void fe_add(const struct curve *c, limb *r, const limb *a, const limb *b)
{
    limb carry = bn_add(r, a, b, c->n);
    if (carry || bn_cmp(r, c->p.m, c->n) >= 0)
        bn_sub(r, r, c->p.m, c->n);
}

static void fe_sub(const struct curve *c, limb *r, const limb *a, const limb *b)
{
    if (bn_sub(r, a, b, c->n))
        bn_add(r, r, c->p.m, c->n);
}

static void fe_mul(const struct curve *c, limb *r, const limb *a, const limb *b) { mont_mul(&c->p, r, a, b); }

/* ---- group operations ---- */

static void pt_inf(const struct curve *c, struct pt *r) { memset(r, 0, sizeof *r); }

static void pt_dbl(const struct curve *c, struct pt *r, const struct pt *p)
{
    if (bn_is_zero(p->z, c->n)) {
        pt_inf(c, r);
        return;
    }
    limb delta[MAXL], gamma[MAXL], beta[MAXL], alpha[MAXL], t[MAXL], u[MAXL];
    struct pt o;
    fe_mul(c, delta, p->z, p->z);
    fe_mul(c, gamma, p->y, p->y);
    fe_mul(c, beta, p->x, gamma);
    fe_sub(c, t, p->x, delta);
    fe_add(c, u, p->x, delta);
    fe_mul(c, alpha, t, u);
    fe_add(c, t, alpha, alpha);
    fe_add(c, alpha, t, alpha);                                /* alpha = 3 (x - delta)(x + delta) */
    fe_mul(c, o.x, alpha, alpha);
    fe_add(c, t, beta, beta);
    fe_add(c, t, t, t);                                        /* 4 beta */
    fe_add(c, u, t, t);                                        /* 8 beta */
    fe_sub(c, o.x, o.x, u);
    fe_add(c, u, p->y, p->z);
    fe_mul(c, o.z, u, u);
    fe_sub(c, o.z, o.z, gamma);
    fe_sub(c, o.z, o.z, delta);
    fe_sub(c, t, t, o.x);
    fe_mul(c, o.y, alpha, t);
    fe_mul(c, t, gamma, gamma);
    fe_add(c, t, t, t);
    fe_add(c, t, t, t);
    fe_add(c, t, t, t);                                        /* 8 gamma^2 */
    fe_sub(c, o.y, o.y, t);
    *r = o;
}

static void pt_add(const struct curve *c, struct pt *r, const struct pt *p, const struct pt *q)
{
    if (bn_is_zero(p->z, c->n)) {
        *r = *q;
        return;
    }
    if (bn_is_zero(q->z, c->n)) {
        *r = *p;
        return;
    }
    limb z1z1[MAXL], z2z2[MAXL], u1[MAXL], u2[MAXL], s1[MAXL], s2[MAXL], h[MAXL], i[MAXL], j[MAXL], rr[MAXL], v[MAXL], t[MAXL];
    struct pt o;
    fe_mul(c, z1z1, p->z, p->z);
    fe_mul(c, z2z2, q->z, q->z);
    fe_mul(c, u1, p->x, z2z2);
    fe_mul(c, u2, q->x, z1z1);
    fe_mul(c, t, q->z, z2z2);
    fe_mul(c, s1, p->y, t);
    fe_mul(c, t, p->z, z1z1);
    fe_mul(c, s2, q->y, t);
    fe_sub(c, h, u2, u1);
    fe_sub(c, rr, s2, s1);
    if (bn_is_zero(h, c->n)) {
        if (bn_is_zero(rr, c->n))
            pt_dbl(c, r, p);
        else
            pt_inf(c, r);
        return;
    }
    fe_add(c, rr, rr, rr);
    fe_add(c, t, h, h);
    fe_mul(c, i, t, t);
    fe_mul(c, j, h, i);
    fe_mul(c, v, u1, i);
    fe_mul(c, o.x, rr, rr);
    fe_sub(c, o.x, o.x, j);
    fe_sub(c, o.x, o.x, v);
    fe_sub(c, o.x, o.x, v);
    fe_sub(c, t, v, o.x);
    fe_mul(c, o.y, rr, t);
    fe_mul(c, t, s1, j);
    fe_add(c, t, t, t);
    fe_sub(c, o.y, o.y, t);
    fe_add(c, t, p->z, q->z);
    fe_mul(c, t, t, t);
    fe_sub(c, t, t, z1z1);
    fe_sub(c, t, t, z2z2);
    fe_mul(c, o.z, t, h);
    *r = o;
}

/* r = k * p; k is a big-endian byte string */
static void pt_mul(const struct curve *c, struct pt *r, const uint8_t *k, size_t klen, const struct pt *p)
{
    struct pt acc;
    pt_inf(c, &acc);
    for (size_t i = 0; i < klen * 8; i++) {
        pt_dbl(c, &acc, &acc);
        if (k[i / 8] >> (7 - i % 8) & 1)
            pt_add(c, &acc, &acc, p);
    }
    *r = acc;
}

static void pt_from_affine(const struct curve *c, struct pt *p, const limb *x, const limb *y)
{
    memcpy(p->x, x, sizeof p->x);
    memcpy(p->y, y, sizeof p->y);
    memcpy(p->z, c->p.one, sizeof p->z);
    memset(p->z + c->n, 0, (MAXL - c->n) * sizeof(limb));
}

/* Affine coordinates (plain, not Montgomery) of a finite point. */
static void pt_affine(const struct curve *c, limb *x, limb *y, const struct pt *p)
{
    limb zi[MAXL], z2[MAXL], t[MAXL], one[MAXL] = {1};
    mont_pow(&c->p, zi, p->z, c->pm2, c->n);
    fe_mul(c, z2, zi, zi);
    fe_mul(c, t, p->x, z2);
    mont_mul(&c->p, x, t, one);
    fe_mul(c, z2, z2, zi);
    fe_mul(c, t, p->y, z2);
    mont_mul(&c->p, y, t, one);
}

/* Decodes 04||X||Y and checks that it lies on the curve. */
static int pt_decode(const struct curve *c, struct pt *p, const uint8_t *buf, size_t len)
{
    if (len != 1 + 2 * (size_t)c->bytes || buf[0] != 4)
        return -1;
    limb x[MAXL] = {0}, y[MAXL] = {0}, xm[MAXL] = {0}, ym[MAXL] = {0};
    bn_from_bytes(x, c->n, buf + 1, c->bytes);
    bn_from_bytes(y, c->n, buf + 1 + c->bytes, c->bytes);
    if (bn_cmp(x, c->p.m, c->n) >= 0 || bn_cmp(y, c->p.m, c->n) >= 0)
        return -1;
    to_mont(c, xm, x);
    to_mont(c, ym, y);
    limb l[MAXL] = {0}, r[MAXL] = {0}, t[MAXL];
    fe_mul(c, l, ym, ym);                                      /* y^2 = x^3 - 3x + b ? */
    fe_mul(c, r, xm, xm);
    fe_mul(c, r, r, xm);
    fe_add(c, t, xm, xm);
    fe_add(c, t, t, xm);
    fe_sub(c, r, r, t);
    fe_add(c, r, r, c->b);
    if (bn_cmp(l, r, c->n))
        return -1;
    pt_from_affine(c, p, xm, ym);
    return 0;
}

static void pt_encode(const struct curve *c, uint8_t *out, const struct pt *p)
{
    limb x[MAXL], y[MAXL];
    pt_affine(c, x, y, p);
    out[0] = 4;
    bn_to_bytes(out + 1, c->bytes, x, c->n);
    bn_to_bytes(out + 1 + c->bytes, c->bytes, y, c->n);
}

/* ---- ECDH ---- */

static int scalar_ok(const struct curve *c, const uint8_t *k)
{
    limb d[MAXL];
    bn_from_bytes(d, c->n, k, c->bytes);
    return !bn_is_zero(d, c->n) && bn_cmp(d, c->q.m, c->n) < 0;
}

int ec_keygen(int curve, const uint8_t *rnd, uint8_t *pub)
{
    struct curve *c = curve_get(curve);
    if (!c || !scalar_ok(c, rnd))
        return -1;
    struct pt g, r;
    pt_from_affine(c, &g, c->gx, c->gy);
    pt_mul(c, &r, rnd, c->bytes, &g);
    if (bn_is_zero(r.z, c->n))
        return -1;
    pt_encode(c, pub, &r);
    return 0;
}

int ec_ecdh(int curve, const uint8_t *rnd, const uint8_t *peer, size_t peerlen, uint8_t *secret)
{
    struct curve *c = curve_get(curve);
    struct pt q, r;
    if (!c || !scalar_ok(c, rnd) || pt_decode(c, &q, peer, peerlen))
        return -1;
    pt_mul(c, &r, rnd, c->bytes, &q);
    if (bn_is_zero(r.z, c->n))
        return -1;
    limb x[MAXL], y[MAXL];
    pt_affine(c, x, y, &r);
    bn_to_bytes(secret, c->bytes, x, c->n);
    return 0;
}

/* ---- ECDSA ---- */

int ecdsa_verify(int curve, const uint8_t *pub, size_t publen, const uint8_t *hash, size_t hlen,
                 const uint8_t *rb, size_t rl, const uint8_t *sb, size_t sl)
{
    struct curve *c = curve_get(curve);
    struct pt q, g, a, b;
    if (!c || pt_decode(c, &q, pub, publen))
        return -1;
    int n = c->n;
    limb r[MAXL], s[MAXL], z[MAXL] = {0}, w[MAXL], u1[MAXL], u2[MAXL], sm[MAXL];
    if (bn_from_bytes(r, n, rb, rl) || bn_from_bytes(s, n, sb, sl))
        return -1;
    if (bn_is_zero(r, n) || bn_is_zero(s, n) || bn_cmp(r, c->q.m, n) >= 0 || bn_cmp(s, c->q.m, n) >= 0)
        return -1;

    size_t zb = hlen < (size_t)c->bytes ? hlen : (size_t)c->bytes;      /* leftmost bits of the hash */
    bn_from_bytes(z, n, hash, zb);
    if (bn_cmp(z, c->q.m, n) >= 0)
        bn_sub(z, z, c->q.m, n);

    mont_mul(&c->q, sm, s, c->q.r2);                           /* w = s^-1 mod q */
    mont_pow(&c->q, w, sm, c->qm2, n);                         /* (Montgomery form) */
    mont_mul(&c->q, u1, z, w);                                 /* z * (wR) / R = z w, plain */
    mont_mul(&c->q, u2, r, w);

    uint8_t k1[MAXL * 4], k2[MAXL * 4];
    bn_to_bytes(k1, c->bytes, u1, n);
    bn_to_bytes(k2, c->bytes, u2, n);
    pt_from_affine(c, &g, c->gx, c->gy);
    pt_mul(c, &a, k1, c->bytes, &g);
    pt_mul(c, &b, k2, c->bytes, &q);
    pt_add(c, &a, &a, &b);
    if (bn_is_zero(a.z, n))
        return -1;
    limb x[MAXL], y[MAXL];
    pt_affine(c, x, y, &a);
    if (bn_cmp(x, c->q.m, n) >= 0)
        bn_sub(x, x, c->q.m, n);
    return bn_cmp(x, r, n) ? -1 : 0;
}
