#!/usr/bin/env python3
"""Builds the trust-anchor bundle (/etc/ssl/roots.bin) that the TLS client validates servers against.

    tools/mkroots.py OUT [--system] [FILE.pem ...]

--system  take Apple's root certificates from the macOS system keychain
Each record keeps only what chain validation needs: the subject name and the public key (both DER).
Expired roots and keys the OS can't use (anything but RSA >= 2048 bits, P-256, P-384) are dropped.

File format: "PCA1", u16 count (little endian), then count x { u16 subject_len, u16 spki_len, subject, spki }.
"""
import base64, re, struct, subprocess, sys, time

def tlv(b, i):
    t = b[i]; l = b[i + 1]; i += 2
    if l & 0x80:
        n = l & 0x7f; l = int.from_bytes(b[i:i + n], 'big'); i += n
    return t, i, l            # tag, content start, content length

def children(b, start, length):
    i, end, out = start, start + length, []
    while i < end:
        t, cs, l = tlv(b, i)
        out.append((t, i, cs, l)); i = cs + l
    return out

P256 = bytes.fromhex('2a8648ce3d030107'); P384 = bytes.fromhex('2b81040022')
RSA = bytes.fromhex('2a864886f70d010101'); EC = bytes.fromhex('2a8648ce3d0201')

def parse(der):
    t, cs, l = tlv(der, 0)
    top = children(der, cs, l)
    tbs = children(der, top[0][2], top[0][3])
    k = 1 if tbs[0][0] == 0xa0 else 0
    # tbs[k]=serial, k+1=sigalg, k+2=issuer, k+3=validity, k+4=subject, k+5=spki
    nb, na = children(der, tbs[k + 3][2], tbs[k + 3][3])
    def when(e):
        s = der[e[2]:e[2] + e[3]].decode()
        if e[0] == 0x17:
            s = ('20' if s[0] < '5' else '19') + s
        return time.mktime(time.strptime(s[:14], '%Y%m%d%H%M%S'))
    subj = der[tbs[k + 4][1]:tbs[k + 4][2] + tbs[k + 4][3]]
    spki = der[tbs[k + 5][1]:tbs[k + 5][2] + tbs[k + 5][3]]
    parts = children(der, tbs[k + 5][2], tbs[k + 5][3])
    alg = children(der, parts[0][2], parts[0][3])
    oid = der[alg[0][2]:alg[0][2] + alg[0][3]]
    ok = False
    if oid == RSA:
        bits = der[parts[1][2] + 1:parts[1][2] + parts[1][3]]
        _, s, l = tlv(bits, 0)
        n = children(bits, s, l)[0]
        mod = bits[n[2]:n[2] + n[3]].lstrip(b'\0')
        ok = len(mod) * 8 >= 2048
    elif oid == EC:
        curve = der[alg[1][2]:alg[1][2] + alg[1][3]]
        ok = curve in (P256, P384)
    return subj, spki, when(nb), when(na), ok

def pems(text):
    for m in re.finditer(r'-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----', text, re.S):
        yield base64.b64decode(m.group(1))

out = sys.argv[1]
args = sys.argv[2:]
certs = []
if '--system' in args:
    args.remove('--system')
    kc = '/System/Library/Keychains/SystemRootCertificates.keychain'
    certs += list(pems(subprocess.run(['security', 'find-certificate', '-a', '-p', kc], capture_output=True, text=True).stdout))
for f in args:
    certs += list(pems(open(f).read()))

now = time.time()
seen, recs, skipped = set(), [], 0
for der in certs:
    try:
        subj, spki, nb, na, ok = parse(der)
    except Exception:
        skipped += 1; continue
    if not ok or na < now or (subj, spki) in seen:
        skipped += 1; continue
    seen.add((subj, spki)); recs.append((subj, spki))
recs.sort()
blob = b'PCA1' + struct.pack('<H', len(recs))
for s, k in recs:
    blob += struct.pack('<HH', len(s), len(k)) + s + k
open(out, 'wb').write(blob)
print('%s: %d roots, %d bytes (%d skipped)' % (out, len(recs), len(blob), skipped))
