/* TLS 1.2 client (RFC 5246, RFC 5289 AES-GCM suites, RFC 7627 extended master secret). */
#include <ulib.h>
#include "crypto.h"
#include "x509.h"
#include "tls.h"

#define REC_MAX 16384
#define REC_BUF (5 + 8 + REC_MAX + 16 + 256)
#define HS_MAX (128 * 1024)

enum { CT_CCS = 20, CT_ALERT = 21, CT_HS = 22, CT_DATA = 23 };
enum { HS_CLIENT_HELLO = 1, HS_SERVER_HELLO = 2, HS_CERT = 11, HS_SKE = 12, HS_CERT_REQ = 13, HS_HELLO_DONE = 14,
       HS_CKE = 16, HS_FINISHED = 20 };

struct suite {
    int id, hid, keylen, ecdsa;
};

static const struct suite SUITES[] = {
    {0xC02F, H_SHA256, 16, 0},                 /* ECDHE_RSA_WITH_AES_128_GCM_SHA256 */
    {0xC030, H_SHA384, 32, 0},                 /* ECDHE_RSA_WITH_AES_256_GCM_SHA384 */
    {0xC02B, H_SHA256, 16, 1},                 /* ECDHE_ECDSA_WITH_AES_128_GCM_SHA256 */
    {0xC02C, H_SHA384, 32, 1},                 /* ECDHE_ECDSA_WITH_AES_256_GCM_SHA384 */
};

struct tls {
    int fd, flags, eof, closed, timed_out;
    const char *err;
    const struct suite *suite;
    struct gcm wr, rd;
    uint8_t wiv[4], riv[4];
    uint32_t wseq[2], rseq[2];                 /* {high, low} */
    int wenc, renc;
    uint8_t *rec;                              /* incoming record (header + body) */
    uint8_t *wbuf;                             /* outgoing record */
    uint8_t *app;                              /* unread application data */
    int app_len;
    struct sha256 t256;                        /* handshake transcript, under both hashes */
    struct sha512 t384;
    uint8_t *hs;
    size_t hs_len, hs_pos;
};

/* ---- errors ---- */

static int fail(struct tls *t, const char *msg)
{
    if (!t->err)
        t->err = msg;
    return -1;
}

static const char *alert_text(int desc)
{
    switch (desc) {
    case 40: return "the server refused our security settings (handshake failure)";
    case 42: case 43: case 44: case 45: case 46: case 48: return "the server rejected our connection (bad certificate)";
    case 47: return "the server rejected our TLS parameters";
    case 70: return "the server requires a newer version of TLS than this browser has";
    case 71: return "the server requires stronger encryption than this browser has";
    case 80: return "the server had an internal error";
    case 112: return "the server doesn't know this host name";
    default: return "the server ended the secure connection";
    }
}

const char *tls_error(struct tls *t) { return t->err ? t->err : "TLS error"; }

/* ---- transport ---- */

static int read_full(struct tls *t, uint8_t *buf, int n)
{
    int got = 0;
    while (got < n) {
        int r = read(t->fd, buf + got, n - got);
        if (r == 0) {
            t->closed = 1;
            return got ? fail(t, "the connection closed unexpectedly") : 0;
        }
        if (r < 0) {
            t->timed_out = r == -ETIMEDOUT;
            return fail(t, r == -ETIMEDOUT ? "the server didn't answer" : "the connection was lost");
        }
        got += r;
    }
    return 1;
}

static int write_full(struct tls *t, const uint8_t *buf, int n)
{
    int off = 0;
    while (off < n) {
        int w = write(t->fd, buf + off, n - off);
        if (w <= 0)
            return fail(t, "the connection was lost");
        off += w;
    }
    return 0;
}

static void put_seq(uint8_t *p, const uint32_t seq[2])
{
    for (int i = 0; i < 4; i++) {
        p[i] = seq[0] >> (24 - 8 * i);
        p[4 + i] = seq[1] >> (24 - 8 * i);
    }
}

static void bump(uint32_t seq[2])
{
    if (++seq[1] == 0)
        seq[0]++;
}

static int send_rec(struct tls *t, int type, const uint8_t *data, int n)
{
    uint8_t *o = t->wbuf;
    o[0] = type; o[1] = 3; o[2] = 3;
    if (!t->wenc) {
        o[3] = n >> 8; o[4] = n;
        memcpy(o + 5, data, n);
        return write_full(t, o, 5 + n);
    }
    uint8_t iv[12], aad[13];
    memcpy(iv, t->wiv, 4);
    put_seq(iv + 4, t->wseq);
    memcpy(aad, iv + 4, 8);
    aad[8] = type; aad[9] = 3; aad[10] = 3; aad[11] = n >> 8; aad[12] = n;
    o[3] = (n + 24) >> 8; o[4] = n + 24;
    memcpy(o + 5, iv + 4, 8);
    gcm_seal(&t->wr, iv, aad, 13, data, n, o + 13, o + 13 + n);
    bump(t->wseq);
    return write_full(t, o, 5 + 24 + n);
}

/* Reads the next record. Returns 1 (type/data/len set), 0 at the end of the stream, -1 on error. */
static int rec_read(struct tls *t, int *type, uint8_t **data, int *len)
{
    for (;;) {
        uint8_t *r = t->rec;
        int rc = read_full(t, r, 5);
        if (rc <= 0)
            return rc;
        int n = r[3] << 8 | r[4];
        if (r[1] != 3 || n > REC_MAX + 2048)
            return fail(t, "the server sent something that isn't TLS");
        if ((rc = read_full(t, r + 5, n)) <= 0)
            return rc < 0 ? rc : fail(t, "the connection closed unexpectedly");
        int ty = r[0];
        uint8_t *body = r + 5;
        if (t->renc) {
            if (n < 24)
                return fail(t, "the server sent a damaged record");
            int pl = n - 24;
            uint8_t iv[12], aad[13];
            memcpy(iv, t->riv, 4);
            memcpy(iv + 4, body, 8);
            put_seq(aad, t->rseq);
            aad[8] = ty; aad[9] = 3; aad[10] = 3; aad[11] = pl >> 8; aad[12] = pl;
            if (gcm_open(&t->rd, iv, aad, 13, body + 8, pl, body + 8, body + 8 + pl))
                return fail(t, "a record from the server failed its integrity check");
            bump(t->rseq);
            body += 8;
            n = pl;
        }
        if (ty == CT_ALERT) {
            if (n < 2)
                return fail(t, "the server sent a damaged alert");
            if (body[1] == 0) {                                /* close_notify */
                t->eof = 1;
                return 0;
            }
            if (body[0] == 1)                                  /* other warnings */
                continue;
            return fail(t, alert_text(body[1]));
        }
        *type = ty;
        *data = body;
        *len = n;
        return 1;
    }
}

/* ---- handshake plumbing ---- */

static void th_update(struct tls *t, const uint8_t *d, size_t n)
{
    sha256_update(&t->t256, d, n);
    sha512_update(&t->t384, d, n);
}

static void th_final(struct tls *t, uint8_t *out)                 /* hash of the transcript so far, in the PRF hash */
{
    if (t->suite->hid == H_SHA256) {
        struct sha256 c = t->t256;
        sha256_final(&c, out);
    } else {
        struct sha512 c = t->t384;
        sha512_final(&c, out);
    }
}

static int hs_send(struct tls *t, int type, const uint8_t *body, int n)
{
    uint8_t *m = malloc(n + 4);
    if (!m)
        return fail(t, "out of memory");
    m[0] = type; m[1] = n >> 16; m[2] = n >> 8; m[3] = n;
    memcpy(m + 4, body, n);
    th_update(t, m, n + 4);
    int rc = send_rec(t, CT_HS, m, n + 4);
    free(m);
    return rc;
}

/* Next handshake message: *msg points at its 4-byte header (valid until the next call). */
static int hs_next(struct tls *t, int *type, const uint8_t **body, int *blen, const uint8_t **msg, int *mlen)
{
    for (;;) {
        size_t have = t->hs_len - t->hs_pos;
        if (have >= 4) {
            const uint8_t *m = t->hs + t->hs_pos;
            size_t need = 4 + (m[1] << 16 | m[2] << 8 | m[3]);
            if (need > HS_MAX)
                return fail(t, "the server sent an oversized handshake message");
            if (have >= need) {
                *type = m[0];
                *body = m + 4;
                *blen = need - 4;
                *msg = m;
                *mlen = need;
                t->hs_pos += need;
                return 0;
            }
        }
        int ty, n;
        uint8_t *d;
        int rc = rec_read(t, &ty, &d, &n);
        if (rc <= 0)
            return rc < 0 ? -1 : fail(t, "the server closed the connection during the handshake");
        if (ty != CT_HS)
            return fail(t, "the server sent an unexpected message during the handshake");
        if (t->hs_pos == t->hs_len)
            t->hs_pos = t->hs_len = 0;
        else if (t->hs_pos > 0) {
            memmove(t->hs, t->hs + t->hs_pos, t->hs_len - t->hs_pos);
            t->hs_len -= t->hs_pos;
            t->hs_pos = 0;
        }
        if (t->hs_len + n > HS_MAX)
            return fail(t, "the server sent an oversized handshake message");
        memcpy(t->hs + t->hs_len, d, n);
        t->hs_len += n;
    }
}

static int random_bytes(struct tls *t, uint8_t *out, int n)
{
    int fd = open("/dev/random", O_RDONLY);
    if (fd < 0)
        return fail(t, "no random number source");
    int got = 0;
    while (got < n) {
        int r = read(fd, out + got, n - got);
        if (r <= 0) {
            close(fd);
            return fail(t, "no random number source");
        }
        got += r;
    }
    close(fd);
    return 0;
}

/* ---- trust anchors ---- */

static struct anchor *g_anchors;
static int g_nanchors, g_loaded;

static void load_bundle(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    struct stat st;
    if (fstat(fd, &st) < 0 || st.size < 6 || st.size > 1024 * 1024) {
        close(fd);
        return;
    }
    uint8_t *buf = malloc(st.size);
    int got = 0;
    while (buf && got < (int)st.size) {
        int r = read(fd, buf + got, st.size - got);
        if (r <= 0)
            break;
        got += r;
    }
    close(fd);
    if (!buf || got != (int)st.size || memcmp(buf, "PCA1", 4))
        return;
    int count = buf[4] | buf[5] << 8;
    struct anchor *a = realloc(g_anchors, (g_nanchors + count) * sizeof *a);
    if (!a)
        return;
    g_anchors = a;
    size_t off = 6;
    for (int i = 0; i < count; i++) {
        if (off + 4 > (size_t)got)
            break;
        size_t sl = buf[off] | buf[off + 1] << 8, kl = buf[off + 2] | buf[off + 3] << 8;
        off += 4;
        if (off + sl + kl > (size_t)got)
            break;
        struct anchor *e = &g_anchors[g_nanchors++];
        e->subject = buf + off; e->subject_len = sl;
        e->spki = buf + off + sl; e->spki_len = kl;
        off += sl + kl;
    }
}

static void load_anchors(void)
{
    if (g_loaded)
        return;
    g_loaded = 1;
    load_bundle("/etc/ssl/roots.bin");
    load_bundle("/etc/ssl/extra.bin");                       /* optional: extra roots for private servers */
}

/* ---- the handshake ---- */

static void put_u16(uint8_t *p, int v) { p[0] = v >> 8; p[1] = v; }

static int is_ip_literal(const char *h)
{
    for (; *h; h++)
        if ((*h < '0' || *h > '9') && *h != '.')
            return 0;
    return 1;
}

/* Decodes a DER ECDSA signature into its two integers. */
static int split_ecdsa(const uint8_t *sig, size_t n, const uint8_t **r, size_t *rl, const uint8_t **s, size_t *sl)
{
    const uint8_t *p = sig, *end = sig + n, *seq;
    size_t seql;
    int tag;
    if (asn1_get(&p, end, &tag, &seq, &seql) || tag != 0x30)
        return -1;
    const uint8_t *q = seq, *qe = seq + seql;
    if (asn1_get(&q, qe, &tag, r, rl) || tag != 2 || asn1_get(&q, qe, &tag, s, sl) || tag != 2)
        return -1;
    return 0;
}

static int handshake(struct tls *t, const char *host)
{
    uint8_t crand[32], srand_[32];
    if (random_bytes(t, crand, 32))
        return -1;

    /* ClientHello */
    uint8_t ch[600];
    int n = 0, hl = strlen(host);
    if (hl > 253)
        return fail(t, "host name too long");
    ch[n++] = 3; ch[n++] = 3;
    memcpy(ch + n, crand, 32); n += 32;
    ch[n++] = 0;                                              /* no session id */
    put_u16(ch + n, 2 * (int)(sizeof SUITES / sizeof *SUITES)); n += 2;
    for (unsigned i = 0; i < sizeof SUITES / sizeof *SUITES; i++) {
        put_u16(ch + n, SUITES[i].id);
        n += 2;
    }
    ch[n++] = 1; ch[n++] = 0;                                 /* compression: none */
    int ext_at = n;
    n += 2;
    if (!is_ip_literal(host)) {                               /* server_name */
        put_u16(ch + n, 0); put_u16(ch + n + 2, hl + 5); put_u16(ch + n + 4, hl + 3);
        ch[n + 6] = 0; put_u16(ch + n + 7, hl);
        memcpy(ch + n + 9, host, hl);
        n += 9 + hl;
    }
    static const uint8_t fixed[] = {
        0x00, 0x0a, 0x00, 0x06, 0x00, 0x04, 0x00, 0x17, 0x00, 0x18,                              /* supported_groups: P-256, P-384 */
        0x00, 0x0b, 0x00, 0x02, 0x01, 0x00,                                                      /* ec_point_formats: uncompressed */
        0x00, 0x0d, 0x00, 0x0c, 0x00, 0x0a, 0x04, 0x01, 0x05, 0x01, 0x06, 0x01, 0x04, 0x03, 0x05, 0x03,   /* signature_algorithms */
        0xff, 0x01, 0x00, 0x01, 0x00,                                                            /* renegotiation_info */
        0x00, 0x17, 0x00, 0x00,                                                                  /* extended_master_secret */
    };
    memcpy(ch + n, fixed, sizeof fixed);
    n += sizeof fixed;
    put_u16(ch + ext_at, n - ext_at - 2);
    if (hs_send(t, HS_CLIENT_HELLO, ch, n))
        return -1;

    /* ServerHello */
    int type, blen, mlen;
    const uint8_t *b, *msg;
    if (hs_next(t, &type, &b, &blen, &msg, &mlen))
        return -1;
    if (type != HS_SERVER_HELLO || blen < 38)
        return fail(t, "the server didn't answer with a ServerHello");
    th_update(t, msg, mlen);
    if (b[0] != 3 || b[1] != 3)
        return fail(t, "the server doesn't speak TLS 1.2");
    memcpy(srand_, b + 2, 32);
    int sid = b[34];
    if (blen < 35 + sid + 3)
        return fail(t, "damaged ServerHello");
    int suite_id = b[35 + sid] << 8 | b[36 + sid];
    if (b[37 + sid] != 0)
        return fail(t, "the server chose compression");
    for (unsigned i = 0; i < sizeof SUITES / sizeof *SUITES; i++)
        if (SUITES[i].id == suite_id)
            t->suite = &SUITES[i];
    if (!t->suite)
        return fail(t, "the server picked an encryption method we didn't offer");
    int ems = 0;
    if (blen > 38 + sid) {
        const uint8_t *e = b + 38 + sid, *ee = b + blen;
        if (ee - e < 2 || (e[0] << 8 | e[1]) != ee - e - 2)
            return fail(t, "damaged ServerHello");
        e += 2;
        while (e + 4 <= ee) {
            int et = e[0] << 8 | e[1], el = e[2] << 8 | e[3];
            if (e + 4 + el > ee)
                return fail(t, "damaged ServerHello");
            if (et == 0x0017)
                ems = 1;
            e += 4 + el;
        }
    }
    const struct suite *su = t->suite;

    /* Certificate */
    if (hs_next(t, &type, &b, &blen, &msg, &mlen))
        return -1;
    if (type != HS_CERT || blen < 3)
        return fail(t, "the server didn't send a certificate");
    th_update(t, msg, mlen);
    const uint8_t *der[10];
    size_t dlen[10];
    int ncert = 0;
    {
        int total = b[0] << 16 | b[1] << 8 | b[2];
        const uint8_t *p = b + 3, *end = b + blen;
        if (total != blen - 3)
            return fail(t, "damaged certificate list");
        while (p < end) {
            if (end - p < 3)
                return fail(t, "damaged certificate list");
            size_t l = p[0] << 16 | p[1] << 8 | p[2];
            p += 3;
            if (l > (size_t)(end - p))
                return fail(t, "damaged certificate list");
            if (ncert < 10) {
                der[ncert] = p;
                dlen[ncert++] = l;
            }
            p += l;
        }
    }
    if (ncert == 0)
        return fail(t, "the server sent no certificate");
    if (!(t->flags & TLS_INSECURE)) {
        struct rtc_time rt;
        gettime(&rt);
        struct now now = {rt.year, rt.month, rt.day, rt.hour, rt.min, rt.sec};
        load_anchors();
        const char *why = x509_verify_chain(der, dlen, ncert, g_anchors, g_nanchors, host, &now);
        if (why)
            return fail(t, why);
    }
    uint8_t *leaf_der = malloc(dlen[0]);                      /* keep the leaf: the buffer it lives in gets reused */
    if (!leaf_der)
        return fail(t, "out of memory");
    memcpy(leaf_der, der[0], dlen[0]);
    struct cert leaf;
    const char *why = NULL;
    if (x509_parse(&leaf, leaf_der, dlen[0], &why))
        return fail(t, why ? why : "unreadable certificate");

    /* ServerKeyExchange */
    if (hs_next(t, &type, &b, &blen, &msg, &mlen))
        return -1;
    if (type != HS_SKE || blen < 4 + 1 + 4)
        return fail(t, "the server didn't send key exchange parameters");
    th_update(t, msg, mlen);
    if (b[0] != 3)
        return fail(t, "unsupported key exchange");
    int curve = b[1] << 8 | b[2], publen = b[3];
    if ((curve != EC_P256 && curve != EC_P384) || publen != 1 + 2 * ec_coord_size(curve) || blen < 4 + publen + 4)
        return fail(t, "the server chose an elliptic curve we don't support");
    const uint8_t *pub = b + 4;
    int sigalg = b[4 + publen] << 8 | b[5 + publen], siglen = b[6 + publen] << 8 | b[7 + publen];
    const uint8_t *sig = b + 8 + publen;
    if (4 + publen + 4 + siglen != blen)
        return fail(t, "damaged key exchange message");
    int hid, is_rsa;
    switch (sigalg) {
    case 0x0401: hid = H_SHA256; is_rsa = 1; break;
    case 0x0501: hid = H_SHA384; is_rsa = 1; break;
    case 0x0601: hid = H_SHA512; is_rsa = 1; break;
    case 0x0403: hid = H_SHA256; is_rsa = 0; break;
    case 0x0503: hid = H_SHA384; is_rsa = 0; break;
    case 0x0603: hid = H_SHA512; is_rsa = 0; break;
    default: return fail(t, "the server signed its key exchange in a way we don't support");
    }
    if (is_rsa == su->ecdsa || (is_rsa ? leaf.key.type != PK_RSA : leaf.key.type != PK_EC))
        return fail(t, "the server's key doesn't match its signature");
    uint8_t digest[64];
    struct hash h;
    hash_init(&h, hid);
    hash_update(&h, crand, 32);
    hash_update(&h, srand_, 32);
    hash_update(&h, b, 4 + publen);
    hash_final(&h, digest);
    int bad;
    if (is_rsa) {
        bad = rsa_verify_pkcs1(leaf.key.n, leaf.key.nl, leaf.key.e, leaf.key.el, hid, digest, sig, siglen);
    } else {
        const uint8_t *r, *s;
        size_t rl, sl;
        bad = split_ecdsa(sig, siglen, &r, &rl, &s, &sl) ||
              ecdsa_verify(leaf.key.curve, leaf.key.pt, leaf.key.ptl, digest, hash_size(hid), r, rl, s, sl);
    }
    free(leaf_der);
    if (bad)
        return fail(t, "the server's key exchange signature is wrong");

    /* ... up to ServerHelloDone, noting a CertificateRequest */
    int want_cert = 0;
    uint8_t server_pub[97];
    memcpy(server_pub, pub, publen);
    for (;;) {
        if (hs_next(t, &type, &b, &blen, &msg, &mlen))
            return -1;
        th_update(t, msg, mlen);
        if (type == HS_CERT_REQ)
            want_cert = 1;
        else if (type == HS_HELLO_DONE)
            break;
        else
            return fail(t, "unexpected message from the server");
    }
    if (want_cert) {                                          /* we have no client certificate: send an empty list */
        static const uint8_t empty[3] = {0, 0, 0};
        if (hs_send(t, HS_CERT, empty, 3))
            return -1;
    }

    /* ClientKeyExchange */
    int cs = ec_coord_size(curve);
    uint8_t priv[48], cpub[97], pms[48];
    for (int tries = 0;; tries++) {
        if (tries > 8 || random_bytes(t, priv, cs))
            return fail(t, "no random number source");
        if (ec_keygen(curve, priv, cpub) == 0)
            break;
    }
    if (ec_ecdh(curve, priv, server_pub, publen, pms))
        return fail(t, "the server sent an invalid key exchange point");
    uint8_t cke[98];
    cke[0] = 1 + 2 * cs;
    memcpy(cke + 1, cpub, 1 + 2 * cs);
    if (hs_send(t, HS_CKE, cke, 2 + 2 * cs))
        return -1;

    /* keys */
    uint8_t master[48], seedh[64];
    if (ems) {
        th_final(t, seedh);
        tls_prf(su->hid, pms, cs, "extended master secret", seedh, hash_size(su->hid), NULL, 0, master, 48);
    } else {
        tls_prf(su->hid, pms, cs, "master secret", crand, 32, srand_, 32, master, 48);
    }
    memset(pms, 0, sizeof pms);
    memset(priv, 0, sizeof priv);
    uint8_t kb[2 * 32 + 8];
    tls_prf(su->hid, master, 48, "key expansion", srand_, 32, crand, 32, kb, 2 * su->keylen + 8);
    gcm_init(&t->wr, kb, su->keylen);
    gcm_init(&t->rd, kb + su->keylen, su->keylen);
    memcpy(t->wiv, kb + 2 * su->keylen, 4);
    memcpy(t->riv, kb + 2 * su->keylen + 4, 4);

    /* ChangeCipherSpec + Finished */
    static const uint8_t one = 1;
    if (send_rec(t, CT_CCS, &one, 1))
        return -1;
    t->wenc = 1;
    uint8_t th[64], vd[12];
    th_final(t, th);
    tls_prf(su->hid, master, 48, "client finished", th, hash_size(su->hid), NULL, 0, vd, 12);
    if (hs_send(t, HS_FINISHED, vd, 12))
        return -1;

    /* the server's ChangeCipherSpec + Finished */
    int ty, ln;
    uint8_t *d;
    int rc = rec_read(t, &ty, &d, &ln);
    if (rc <= 0)
        return rc < 0 ? -1 : fail(t, "the server closed the connection during the handshake");
    if (ty != CT_CCS || ln != 1 || d[0] != 1 || t->hs_pos != t->hs_len)
        return fail(t, "the server didn't change to encrypted mode");
    t->renc = 1;
    th_final(t, th);
    tls_prf(su->hid, master, 48, "server finished", th, hash_size(su->hid), NULL, 0, vd, 12);
    memset(master, 0, sizeof master);
    if (hs_next(t, &type, &b, &blen, &msg, &mlen))
        return -1;
    if (type != HS_FINISHED || blen != 12)
        return fail(t, "the server didn't finish the handshake");
    uint8_t diff = 0;
    for (int i = 0; i < 12; i++)
        diff |= vd[i] ^ b[i];
    if (diff)
        return fail(t, "the server's handshake confirmation is wrong");
    return 0;
}

/* ---- public API ---- */

struct tls *tls_connect(int fd, const char *host, int flags, const char **err)
{
    struct tls *t = calloc(1, sizeof *t);
    static const char *oom = "out of memory";
    if (!t) {
        *err = oom;
        return NULL;
    }
    t->fd = fd;
    t->flags = flags;
    t->rec = malloc(REC_BUF);
    t->wbuf = malloc(REC_BUF);
    t->hs = malloc(HS_MAX);
    if (!t->rec || !t->wbuf || !t->hs) {
        *err = oom;
        tls_close(t);
        return NULL;
    }
    sha256_init(&t->t256);
    sha384_init(&t->t384);
    if (handshake(t, host)) {
        static char msg[160];
        safestrcpy(msg, tls_error(t), sizeof msg);
        *err = msg;
        free(t->rec); free(t->wbuf); free(t->hs); free(t);
        return NULL;
    }
    free(t->hs);
    t->hs = NULL;
    return t;
}

int tls_write(struct tls *t, const void *buf, int n)
{
    const uint8_t *p = buf;
    int done = 0;
    while (done < n) {
        int chunk = n - done > REC_MAX ? REC_MAX : n - done;
        if (send_rec(t, CT_DATA, p + done, chunk))
            return -1;
        done += chunk;
    }
    return n;
}

int tls_read(struct tls *t, void *buf, int n)
{
    while (t->app_len == 0) {
        if (t->eof)
            return 0;
        int ty, ln;
        uint8_t *d;
        int rc = rec_read(t, &ty, &d, &ln);
        if (rc == 0) {
            t->eof = 1;
            return 0;
        }
        if (rc < 0)
            return t->closed ? 0 : t->timed_out ? -ETIMEDOUT : -EIO;       /* a close without close_notify is just the end */
        if (ty == CT_DATA && ln > 0) {
            t->app = d;
            t->app_len = ln;
        }
    }
    int take = n < t->app_len ? n : t->app_len;
    memcpy(buf, t->app, take);
    t->app += take;
    t->app_len -= take;
    return take;
}

void tls_close(struct tls *t)
{
    if (t->wenc && t->wbuf) {
        static const uint8_t bye[2] = {1, 0};                  /* warning: close_notify */
        send_rec(t, CT_ALERT, bye, 2);
    }
    free(t->rec);
    free(t->wbuf);
    free(t->hs);
    free(t);
}
