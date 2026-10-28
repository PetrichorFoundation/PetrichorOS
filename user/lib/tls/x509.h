/* DER / X.509 parsing and certificate-chain validation. */
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { PK_NONE, PK_RSA, PK_EC };
enum { SIG_NONE, SIG_RSA_SHA256, SIG_RSA_SHA384, SIG_RSA_SHA512, SIG_ECDSA_SHA256, SIG_ECDSA_SHA384, SIG_ECDSA_SHA512 };

struct pubkey {
    int type;
    const uint8_t *n, *e;            /* RSA modulus and exponent */
    size_t nl, el;
    int curve;                       /* EC_P256 / EC_P384 */
    const uint8_t *pt;               /* 04||X||Y */
    size_t ptl;
};

struct cert {
    const uint8_t *tbs, *issuer, *subject, *spki, *sig, *san;
    size_t tbslen, issuerlen, subjectlen, spkilen, siglen, sanlen;
    char not_before[15], not_after[15];            /* YYYYMMDDHHMMSS */
    int sigalg;
    struct pubkey key;
    int is_ca, pathlen;                            /* pathlen -1: unlimited */
    int has_ku, key_usage;                         /* key_usage: bit 0 = digitalSignature, bit 5 = keyCertSign */
    int has_eku, eku_server;
};

/* A trust anchor: a subject name and its public key (both DER). */
struct anchor {
    const uint8_t *subject, *spki;
    size_t subject_len, spki_len;
};

struct now {
    int year, month, day, hour, min, sec;
};

int asn1_get(const uint8_t **pp, const uint8_t *end, int *tag, const uint8_t **val, size_t *vlen);
int x509_parse(struct cert *c, const uint8_t *der, size_t len, const char **why);
int x509_parse_spki(struct pubkey *k, const uint8_t *spki, size_t len);
int x509_check_host(const struct cert *c, const char *host);

/* Validates der[0] (the server's certificate) against the anchors; returns NULL or a reason. */
const char *x509_verify_chain(const uint8_t *const *der, const size_t *len, int count, const struct anchor *anchors,
                              int nanchors, const char *host, const struct now *now);
