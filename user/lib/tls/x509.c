/* DER / X.509 parsing and certificate-chain validation (RFC 5280, the parts the web needs). */
#include <string.h>
#include "crypto.h"
#include "x509.h"

/* Reads one TLV at pp (advancing it); val and vlen describe the contents. Only single-byte tags and lengths up to 4 bytes. */
int asn1_get(const uint8_t **pp, const uint8_t *end, int *tag, const uint8_t **val, size_t *vlen)
{
    const uint8_t *p = *pp;
    if (end - p < 2 || (p[0] & 0x1f) == 0x1f)
        return -1;
    *tag = p[0];
    size_t l = p[1];
    p += 2;
    if (l & 0x80) {
        int nb = l & 0x7f;
        if (nb == 0 || nb > 4 || end - p < nb)
            return -1;
        l = 0;
        while (nb--)
            l = l << 8 | *p++;
    }
    if ((size_t)(end - p) < l)
        return -1;
    *val = p;
    *vlen = l;
    *pp = p + l;
    return 0;
}

/* Expects the next element to have the given tag. */
static int expect(const uint8_t **pp, const uint8_t *end, int want, const uint8_t **val, size_t *vlen)
{
    int tag;
    if (asn1_get(pp, end, &tag, val, vlen) || tag != want)
        return -1;
    return 0;
}

static int oid_is(const uint8_t *v, size_t l, const char *hex)
{
    size_t n = strlen(hex) / 2;
    if (l != n)
        return 0;
    for (size_t i = 0; i < n; i++) {
        int a = hex[2 * i] <= '9' ? hex[2 * i] - '0' : hex[2 * i] - 'a' + 10;
        int b = hex[2 * i + 1] <= '9' ? hex[2 * i + 1] - '0' : hex[2 * i + 1] - 'a' + 10;
        if (v[i] != (a << 4 | b))
            return 0;
    }
    return 1;
}

static int sigalg_from_oid(const uint8_t *v, size_t l)
{
    if (oid_is(v, l, "2a864886f70d01010b")) return SIG_RSA_SHA256;
    if (oid_is(v, l, "2a864886f70d01010c")) return SIG_RSA_SHA384;
    if (oid_is(v, l, "2a864886f70d01010d")) return SIG_RSA_SHA512;
    if (oid_is(v, l, "2a8648ce3d040302")) return SIG_ECDSA_SHA256;
    if (oid_is(v, l, "2a8648ce3d040303")) return SIG_ECDSA_SHA384;
    if (oid_is(v, l, "2a8648ce3d040304")) return SIG_ECDSA_SHA512;
    return SIG_NONE;
}

int x509_parse_spki(struct pubkey *k, const uint8_t *spki, size_t len)
{
    const uint8_t *p = spki, *end = spki + len, *v, *alg, *aend, *oid, *bits;
    size_t vl, al, ol, bl;
    memset(k, 0, sizeof *k);
    if (expect(&p, end, 0x30, &v, &vl))
        return -1;
    p = v;
    end = v + vl;
    if (expect(&p, end, 0x30, &alg, &al) || expect(&p, end, 0x03, &bits, &bl) || bl < 2 || bits[0] != 0)
        return -1;
    bits++;
    bl--;
    aend = alg + al;
    if (expect(&alg, aend, 0x06, &oid, &ol))
        return -1;
    if (oid_is(oid, ol, "2a864886f70d010101")) {                       /* rsaEncryption */
        const uint8_t *q = bits, *qe = bits + bl, *s, *n, *e;
        size_t sl, nl, el;
        if (expect(&q, qe, 0x30, &s, &sl))
            return -1;
        const uint8_t *sp = s, *se = s + sl;
        if (expect(&sp, se, 0x02, &n, &nl) || expect(&sp, se, 0x02, &e, &el))
            return -1;
        while (nl && !*n) {
            n++;
            nl--;
        }
        while (el && !*e) {
            e++;
            el--;
        }
        k->type = PK_RSA;
        k->n = n; k->nl = nl; k->e = e; k->el = el;
        return 0;
    }
    if (oid_is(oid, ol, "2a8648ce3d0201")) {                          /* id-ecPublicKey */
        const uint8_t *curve;
        size_t cl;
        if (expect(&alg, aend, 0x06, &curve, &cl))
            return -1;
        if (oid_is(curve, cl, "2a8648ce3d030107"))
            k->curve = EC_P256;
        else if (oid_is(curve, cl, "2b81040022"))
            k->curve = EC_P384;
        else
            return -1;
        if (bl != 1 + 2 * (size_t)ec_coord_size(k->curve) || bits[0] != 4)
            return -1;
        k->type = PK_EC;
        k->pt = bits;
        k->ptl = bl;
        return 0;
    }
    return -1;
}

/* UTCTime / GeneralizedTime to YYYYMMDDHHMMSS. */
static int parse_time(int tag, const uint8_t *v, size_t l, char out[15])
{
    const char *s = (const char *)v;
    size_t digits = tag == 0x17 ? 12 : 14;
    if ((tag != 0x17 && tag != 0x18) || l != digits + 1 || s[digits] != 'Z')
        return -1;
    for (size_t i = 0; i < digits; i++)
        if (s[i] < '0' || s[i] > '9')
            return -1;
    if (tag == 0x17) {
        memcpy(out, s[0] < '5' ? "20" : "19", 2);
        memcpy(out + 2, s, 12);
    } else {
        memcpy(out, s, 14);
    }
    out[14] = 0;
    return 0;
}

int x509_parse(struct cert *c, const uint8_t *der, size_t len, const char **why)
{
    const uint8_t *p = der, *end = der + len, *cert, *tbs, *v;
    size_t cl, tl, vl;
    int tag;
    memset(c, 0, sizeof *c);
    c->pathlen = -1;
    *why = "malformed certificate";
    if (expect(&p, end, 0x30, &cert, &cl))
        return -1;
    p = cert;
    end = cert + cl;
    const uint8_t *tbs_start = p;
    if (expect(&p, end, 0x30, &tbs, &tl))
        return -1;
    c->tbs = tbs_start;
    c->tbslen = p - tbs_start;

    const uint8_t *alg, *sig;
    size_t al, sl;
    if (expect(&p, end, 0x30, &alg, &al) || expect(&p, end, 0x03, &sig, &sl) || sl < 2 || sig[0] != 0)
        return -1;
    c->sig = sig + 1;
    c->siglen = sl - 1;
    const uint8_t *oid;
    size_t ol;
    const uint8_t *ap = alg;
    if (expect(&ap, alg + al, 0x06, &oid, &ol))
        return -1;
    c->sigalg = sigalg_from_oid(oid, ol);

    /* TBSCertificate */
    p = tbs;
    end = tbs + tl;
    if (end - p > 0 && *p == 0xa0) {                                   /* version */
        if (expect(&p, end, 0xa0, &v, &vl))
            return -1;
    }
    if (expect(&p, end, 0x02, &v, &vl))                                /* serial */
        return -1;
    const uint8_t *ialg;
    size_t ial;
    if (expect(&p, end, 0x30, &ialg, &ial))                            /* signature algorithm: must match the outer one */
        return -1;
    const uint8_t *iap = ialg, *ioid;
    size_t iol;
    if (expect(&iap, ialg + ial, 0x06, &ioid, &iol) || iol != ol || memcmp(ioid, oid, ol)) {
        *why = "certificate signature algorithms disagree";
        return -1;
    }
    const uint8_t *start = p;
    if (expect(&p, end, 0x30, &v, &vl))                                /* issuer */
        return -1;
    c->issuer = start;
    c->issuerlen = p - start;
    const uint8_t *val, *ve;
    size_t valL, t1l;
    if (expect(&p, end, 0x30, &val, &valL))                            /* validity */
        return -1;
    ve = val + valL;
    const uint8_t *t1;
    if (asn1_get(&val, ve, &tag, &t1, &t1l) || parse_time(tag, t1, t1l, c->not_before))
        return -1;
    if (asn1_get(&val, ve, &tag, &t1, &t1l) || parse_time(tag, t1, t1l, c->not_after))
        return -1;
    start = p;
    if (expect(&p, end, 0x30, &v, &vl))                                /* subject */
        return -1;
    c->subject = start;
    c->subjectlen = p - start;
    start = p;
    if (expect(&p, end, 0x30, &v, &vl))                                /* subjectPublicKeyInfo */
        return -1;
    c->spki = start;
    c->spkilen = p - start;
    if (x509_parse_spki(&c->key, c->spki, c->spkilen)) {
        *why = "unsupported public key type";
        return -1;
    }

    while (p < end) {                                                  /* optional fields, then extensions [3] */
        const uint8_t *ev;
        size_t evl;
        if (asn1_get(&p, end, &tag, &ev, &evl))
            return -1;
        if (tag != 0xa3)
            continue;
        const uint8_t *xs = ev, *xe = ev + evl, *list;
        size_t ll;
        if (expect(&xs, xe, 0x30, &list, &ll))
            return -1;
        const uint8_t *lp = list, *le = list + ll;
        while (lp < le) {
            const uint8_t *ext, *eo, *ev2;
            size_t el, eol, evl2;
            if (expect(&lp, le, 0x30, &ext, &el))
                return -1;
            const uint8_t *ep = ext, *ee = ext + el;
            if (expect(&ep, ee, 0x06, &eo, &eol))
                return -1;
            int critical = 0;
            if (ee - ep > 0 && *ep == 0x01) {
                const uint8_t *b;
                size_t bl2;
                if (expect(&ep, ee, 0x01, &b, &bl2) || bl2 != 1)
                    return -1;
                critical = b[0] != 0;
            }
            if (expect(&ep, ee, 0x04, &ev2, &evl2))
                return -1;
            int known = 1;
            if (oid_is(eo, eol, "551d13")) {                           /* basicConstraints */
                const uint8_t *bp = ev2, *bs, *b2;
                size_t bsl, b2l;
                if (expect(&bp, ev2 + evl2, 0x30, &bs, &bsl))
                    return -1;
                const uint8_t *q = bs, *qe = bs + bsl;
                c->is_ca = 0;
                if (q < qe && *q == 0x01) {
                    if (expect(&q, qe, 0x01, &b2, &b2l) || b2l != 1)
                        return -1;
                    c->is_ca = b2[0] != 0;
                }
                if (q < qe) {
                    if (expect(&q, qe, 0x02, &b2, &b2l) || b2l < 1 || b2l > 2)
                        return -1;
                    c->pathlen = b2l == 1 ? b2[0] : (b2[0] << 8 | b2[1]);
                }
            } else if (oid_is(eo, eol, "551d0f")) {                    /* keyUsage */
                const uint8_t *bp = ev2, *b;
                size_t bl2;
                if (expect(&bp, ev2 + evl2, 0x03, &b, &bl2) || bl2 < 2)
                    return -1;
                c->has_ku = 1;
                int rev = 0;                                           /* named bit i (MSB first) becomes (1 << i) */
                for (int i = 0; i < 8; i++)
                    if (b[1] & (0x80 >> i))
                        rev |= 1 << i;
                c->key_usage = rev;
            } else if (oid_is(eo, eol, "551d11")) {                    /* subjectAltName */
                const uint8_t *bp = ev2, *b;
                size_t bl2;
                if (expect(&bp, ev2 + evl2, 0x30, &b, &bl2))
                    return -1;
                c->san = b;
                c->sanlen = bl2;
            } else if (oid_is(eo, eol, "551d25")) {                    /* extendedKeyUsage */
                const uint8_t *bp = ev2, *b;
                size_t bl2;
                if (expect(&bp, ev2 + evl2, 0x30, &b, &bl2))
                    return -1;
                c->has_eku = 1;
                const uint8_t *q = b, *qe = b + bl2, *o;
                size_t ol2;
                while (q < qe) {
                    if (expect(&q, qe, 0x06, &o, &ol2))
                        return -1;
                    if (oid_is(o, ol2, "2b06010505070301") || oid_is(o, ol2, "551d2500"))
                        c->eku_server = 1;
                }
            } else if (oid_is(eo, eol, "551d0e") || oid_is(eo, eol, "551d23") || oid_is(eo, eol, "551d1f") ||
                       oid_is(eo, eol, "551d20") || oid_is(eo, eol, "551d1e") || oid_is(eo, eol, "551d24") ||
                       oid_is(eo, eol, "551d36") || oid_is(eo, eol, "2b06010505070101") || oid_is(eo, eol, "551d2e") ||
                       oid_is(eo, eol, "2b06010401d679020402")) {
                /* subject/authority key ids, CRL points, policies, name/policy constraints, AIA, SCT list: not needed */
            } else {
                known = 0;
            }
            if (!known && critical) {
                *why = "certificate has an unrecognised critical extension";
                return -1;
            }
        }
    }
    *why = NULL;
    return 0;
}

/* ---- host names ---- */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int name_match(const char *pat, size_t pl, const char *host)
{
    size_t hl = strlen(host);
    if (pl > 2 && pat[0] == '*' && pat[1] == '.') {
        const char *dot = strchr(host, '.');
        if (!dot || !memchr(pat + 2, '.', pl - 2))
            return 0;
        host = dot + 1;                                                 /* the wildcard stands for exactly one label */
        hl = strlen(host);
        pat += 2;
        pl -= 2;
        if (strchr(host, '*'))
            return 0;
    }
    if (pl != hl)
        return 0;
    for (size_t i = 0; i < pl; i++)
        if (lower(pat[i]) != lower(host[i]))
            return 0;
    return 1;
}

static int parse_ipv4(const char *s, uint8_t out[4])
{
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return -1;
        unsigned v = 0;
        while (*s >= '0' && *s <= '9')
            v = v * 10 + (*s++ - '0');
        if (v > 255 || (i < 3 && *s++ != '.'))
            return -1;
        out[i] = v;
    }
    return *s ? -1 : 0;
}

int x509_check_host(const struct cert *c, const char *host)
{
    uint8_t ip[4];
    int is_ip = parse_ipv4(host, ip) == 0;
    const uint8_t *p = c->san, *end = c->san + c->sanlen;
    while (p && p < end) {
        int tag;
        const uint8_t *v;
        size_t vl;
        if (asn1_get(&p, end, &tag, &v, &vl))
            return 0;
        if (tag == 0x82 && !is_ip && name_match((const char *)v, vl, host))
            return 1;
        if (tag == 0x87 && is_ip && vl == 4 && !memcmp(v, ip, 4))
            return 1;
    }
    return 0;
}

/* ---- dates ---- */

static int days_in(int y, int m)
{
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && (y % 4 == 0 && (y % 100 || y % 400 == 0)) ? 29 : d[m - 1];
}

/* now shifted by one day either way, as "YYYYMMDDHHMMSS" (tolerates a clock kept in local time) */
static void shifted(const struct now *n, int delta, char out[15])
{
    int y = n->year, m = n->month, d = n->day + delta;
    if (d < 1) {
        if (--m < 1) { m = 12; y--; }
        d = days_in(y, m);
    } else if (d > days_in(y, m)) {
        d = 1;
        if (++m > 12) { m = 1; y++; }
    }
    int f[6] = {y, m, d, n->hour, n->min, n->sec};
    static const int w[6] = {4, 2, 2, 2, 2, 2};
    char *o = out;
    for (int i = 0; i < 6; i++) {
        int v = f[i];
        for (int k = w[i] - 1; k >= 0; k--) {
            o[k] = '0' + v % 10;
            v /= 10;
        }
        o += w[i];
    }
    *o = 0;
}

/* ---- signatures ---- */

static int verify_sig(const struct cert *c, const struct pubkey *k)
{
    int hid = (c->sigalg == SIG_RSA_SHA256 || c->sigalg == SIG_ECDSA_SHA256) ? H_SHA256
              : (c->sigalg == SIG_RSA_SHA384 || c->sigalg == SIG_ECDSA_SHA384) ? H_SHA384 : H_SHA512;
    int is_rsa = c->sigalg >= SIG_RSA_SHA256 && c->sigalg <= SIG_RSA_SHA512;
    uint8_t digest[64];
    struct hash h;
    hash_init(&h, hid);
    hash_update(&h, c->tbs, c->tbslen);
    hash_final(&h, digest);
    if (is_rsa)
        return k->type == PK_RSA ? rsa_verify_pkcs1(k->n, k->nl, k->e, k->el, hid, digest, c->sig, c->siglen) : -1;
    if (k->type != PK_EC)
        return -1;
    const uint8_t *p = c->sig, *end = c->sig + c->siglen, *seq, *r, *s;
    size_t sl, rl, ssl;
    if (expect(&p, end, 0x30, &seq, &sl))
        return -1;
    const uint8_t *q = seq, *qe = seq + sl;
    if (expect(&q, qe, 0x02, &r, &rl) || expect(&q, qe, 0x02, &s, &ssl))
        return -1;
    return ecdsa_verify(k->curve, k->pt, k->ptl, digest, hash_size(hid), r, rl, s, ssl);
}

#define MAX_CHAIN 10

const char *x509_verify_chain(const uint8_t *const *der, const size_t *len, int count, const struct anchor *anchors,
                              int nanchors, const char *host, const struct now *now)
{
    static struct cert certs[MAX_CHAIN];
    const char *why;
    if (count < 1)
        return "the server sent no certificate";
    if (count > MAX_CHAIN)
        count = MAX_CHAIN;
    for (int i = 0; i < count; i++)
        if (x509_parse(&certs[i], der[i], len[i], &why))
            return why ? why : "malformed certificate";

    const struct cert *leaf = &certs[0];
    if (host && !x509_check_host(leaf, host))
        return "the certificate is for a different site";
    if (leaf->has_eku && !leaf->eku_server)
        return "the certificate isn't meant for web servers";
    if (leaf->has_ku && !(leaf->key_usage & 1) && leaf->key.type == PK_EC)
        return "the certificate can't be used to sign";

    char early[15], late[15];
    shifted(now, 1, early);                                            /* notBefore must be <= now + 1 day */
    shifted(now, -1, late);                                            /* notAfter must be >= now - 1 day */
    for (int i = 0; i < count; i++) {
        if (memcmp(certs[i].not_before, early, 14) > 0)
            return "the certificate isn't valid yet (is the clock right?)";
        if (memcmp(certs[i].not_after, late, 14) < 0)
            return "the certificate has expired (is the clock right?)";
    }

    for (int i = 0; i < count; i++) {
        const struct cert *c = &certs[i];
        for (int a = 0; a < nanchors; a++) {                           /* the server sent a trusted root itself */
            if (anchors[a].subject_len == c->subjectlen && !memcmp(anchors[a].subject, c->subject, c->subjectlen) &&
                anchors[a].spki_len == c->spkilen && !memcmp(anchors[a].spki, c->spki, c->spkilen))
                return NULL;
        }
        if (c->sigalg == SIG_NONE)
            return "the certificate uses an unsupported signature algorithm";
        int matched = 0;
        for (int a = 0; a < nanchors; a++) {                           /* signed directly by a trusted root */
            struct pubkey k;
            if (anchors[a].subject_len != c->issuerlen || memcmp(anchors[a].subject, c->issuer, c->issuerlen))
                continue;
            matched = 1;
            if (x509_parse_spki(&k, anchors[a].spki, anchors[a].spki_len) == 0 && verify_sig(c, &k) == 0)
                return NULL;
        }
        if (matched && i == count - 1)
            return "a certificate signature in the chain is wrong";
        if (i + 1 >= count)
            return "the certificate was issued by an unknown authority";
        const struct cert *up = &certs[i + 1];                         /* otherwise it must be signed by the next one */
        if (c->issuerlen != up->subjectlen || memcmp(c->issuer, up->subject, c->issuerlen))
            return matched ? "a certificate signature in the chain is wrong" : "the certificate chain is out of order";
        if (!up->is_ca || (up->has_ku && !(up->key_usage & 32)))
            return "an intermediate certificate isn't a certificate authority";
        if (up->pathlen >= 0 && i > up->pathlen)
            return "the certificate chain is longer than its authority allows";
        if (verify_sig(c, &up->key))
            return "a certificate signature in the chain is wrong";
    }
    return "the certificate was issued by an unknown authority";
}
