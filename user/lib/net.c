/* Address parsing and a small DNS resolver. */
#include "ulib.h"

int inet_aton(const char *s, u32 *ip)
{
    u32 v = 0;
    for (int i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return -EINVAL;
        u32 octet = 0;
        while (*s >= '0' && *s <= '9')
            octet = octet * 10 + (*s++ - '0');
        if (octet > 255 || (i < 3 && *s++ != '.'))
            return -EINVAL;
        v = v << 8 | octet;
    }
    if (*s)
        return -EINVAL;
    *ip = v;
    return 0;
}

const char *inet_ntoa(u32 ip)
{
    static char buf[16];
    snprintf(buf, sizeof buf, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return buf;
}

/* Skips a (possibly compressed) DNS name; returns the offset after it. */
static int skip_name(const u8 *m, int len, int off)
{
    while (off < len) {
        u8 l = m[off];
        if (l == 0)
            return off + 1;
        if ((l & 0xC0) == 0xC0)
            return off + 2;
        off += l + 1;
    }
    return -1;
}

int resolve(const char *name, u32 *ip)
{
    if (inet_aton(name, ip) == 0)
        return 0;
    struct netinfo ni;
    netinfo(&ni);
    if (!ni.up)
        return -ENETDOWN;

    u8 q[300];
    int n = 0;
    static u16 id = 0x1234;
    id++;
    q[n++] = id >> 8; q[n++] = id;
    q[n++] = 0x01; q[n++] = 0x00;                       /* recursion desired */
    q[n++] = 0; q[n++] = 1;                             /* one question */
    for (int i = 0; i < 6; i++)
        q[n++] = 0;
    for (const char *p = name; *p;) {                   /* QNAME: length-prefixed labels */
        const char *dot = strchr(p, '.');
        int l = dot ? dot - p : (int)strlen(p);
        if (l == 0 || l > 63 || n + l + 6 > (int)sizeof q)
            return -EINVAL;
        q[n++] = l;
        memcpy(q + n, p, l);
        n += l;
        p += l + (dot ? 1 : 0);
    }
    q[n++] = 0;
    q[n++] = 0; q[n++] = 1;                             /* type A */
    q[n++] = 0; q[n++] = 1;                             /* class IN */

    int fd = socket(SOCK_DGRAM);
    if (fd < 0)
        return fd;
    settimeout(fd, 1500);
    int rc = -ETIMEDOUT;
    for (int attempt = 0; attempt < 3 && rc == -ETIMEDOUT; attempt++) {
        int e = sendto(fd, q, n, ni.dns, 53);
        if (e < 0) {
            rc = e;
            break;
        }
        u8 r[512];
        int len = recvfrom(fd, r, sizeof r, NULL);
        if (len < 0) {
            rc = len;
            continue;
        }
        if (len < 12 || r[0] != (id >> 8) || r[1] != (u8)id)
            continue;
        if ((r[3] & 0x0F) != 0) {
            rc = -ENOENT;                               /* NXDOMAIN etc. */
            break;
        }
        int qd = r[4] << 8 | r[5], an = r[6] << 8 | r[7];
        int off = 12;
        for (int i = 0; i < qd && off >= 0; i++)
            off = skip_name(r, len, off) + 4;
        rc = -ENOENT;
        for (int i = 0; i < an && off > 0 && off + 10 <= len; i++) {
            off = skip_name(r, len, off);
            if (off < 0 || off + 10 > len)
                break;
            int type = r[off] << 8 | r[off + 1];
            int rdlen = r[off + 8] << 8 | r[off + 9];
            off += 10;
            if (type == 1 && rdlen == 4 && off + 4 <= len) {
                *ip = (u32)r[off] << 24 | r[off + 1] << 16 | r[off + 2] << 8 | r[off + 3];
                rc = 0;
                break;
            }
            off += rdlen;
        }
    }
    close(fd);
    return rc;
}
