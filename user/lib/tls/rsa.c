/* RSA signature verification (PKCS#1 v1.5). */
#include <string.h>
#include "crypto.h"

static const uint8_t DI_SHA256[] = {0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
static const uint8_t DI_SHA384[] = {0x30, 0x41, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02, 0x05, 0x00, 0x04, 0x30};
static const uint8_t DI_SHA512[] = {0x30, 0x51, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03, 0x05, 0x00, 0x04, 0x40};

int rsa_verify_pkcs1(const uint8_t *n, size_t nl, const uint8_t *e, size_t el, int hid,
                     const uint8_t *hash, const uint8_t *sig, size_t siglen)
{
    while (nl && !*n) {
        n++;
        nl--;
    }
    if (nl < 128 || nl > 512 || siglen != nl || el == 0 || el > 4)
        return -1;
    int limbs = (nl + 3) / 4;

    static struct mont ctx;                                    /* big; keep it off the stack */
    limb m[BN_MAXL], s[BN_MAXL], x[BN_MAXL], ex[1];
    if (bn_from_bytes(m, limbs, n, nl) || mont_init(&ctx, m, limbs))
        return -1;
    if (bn_from_bytes(s, limbs, sig, siglen) || bn_cmp(s, m, limbs) >= 0)
        return -1;
    if (bn_from_bytes(ex, 1, e, el) || !(ex[0] & 1) || ex[0] < 3)
        return -1;
    mont_exp(&ctx, x, s, ex, 1);

    uint8_t em[512];
    bn_to_bytes(em, nl, x, limbs);

    const uint8_t *di = hid == H_SHA256 ? DI_SHA256 : hid == H_SHA384 ? DI_SHA384 : DI_SHA512;
    size_t dil = hid == H_SHA256 ? sizeof DI_SHA256 : hid == H_SHA384 ? sizeof DI_SHA384 : sizeof DI_SHA512;
    size_t hl = hash_size(hid);
    size_t pad = nl - 3 - dil - hl;                            /* number of 0xFF bytes */
    if (nl < 3 + dil + hl + 8 || em[0] != 0 || em[1] != 1)
        return -1;
    for (size_t i = 0; i < pad; i++)
        if (em[2 + i] != 0xff)
            return -1;
    if (em[2 + pad] != 0 || memcmp(em + 3 + pad, di, dil) || memcmp(em + 3 + pad + dil, hash, hl))
        return -1;
    return 0;
}
