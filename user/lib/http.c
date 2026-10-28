/* A minimal blocking HTTP/1.0 GET client. */
#include "ulib.h"

#define MAX_BODY (16 * 1024 * 1024)

/* Splits "http://host[:port]/path" (scheme optional). */
int url_split(const char *url, char *host, int hostsz, int *port, const char **path)
{
    if (!strncmp(url, "http://", 7))
        url += 7;
    const char *slash = strchr(url, '/');
    int hl = slash ? slash - url : (int)strlen(url);
    if (hl <= 0 || hl >= hostsz)
        return -EINVAL;
    memcpy(host, url, hl);
    host[hl] = 0;
    *port = 80;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        *port = atoi(colon + 1);
    }
    *path = slash ? slash : "/";
    return 0;
}

/* Fetches `url`; on success *body is malloc'd (NUL-terminated) and *len is its size. */
int http_get(const char *url, char **body, int *len)
{
    char host[128];
    const char *path;
    int port;
    int e = url_split(url, host, sizeof host, &port, &path);
    if (e < 0)
        return e;
    u32 ip;
    if ((e = resolve(host, &ip)) < 0)
        return e;
    int fd = socket(SOCK_STREAM);
    if (fd < 0)
        return fd;
    if ((e = connect(fd, ip, port)) < 0) {
        close(fd);
        return e;
    }
    char req[512];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: petrichor-ppm\r\n\r\n",
                     path, host);
    if ((e = write(fd, req, n)) < 0) {
        close(fd);
        return e;
    }

    int cap = 8192, used = 0;
    char *buf = malloc(cap);
    while (buf) {
        if (used + 4096 + 1 > cap) {
            if (cap >= MAX_BODY) {
                free(buf);
                close(fd);
                return -EFBIG;
            }
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
        }
        n = read(fd, buf + used, cap - used - 1);
        if (n == 0)
            break;
        if (n < 0) {
            free(buf);
            close(fd);
            return n;
        }
        used += n;
    }
    close(fd);
    if (!buf)
        return -ENOMEM;
    buf[used] = 0;

    int status = 0;                                 /* "HTTP/1.x NNN ..." */
    if (used > 12 && !strncmp(buf, "HTTP/", 5))
        status = atoi(buf + 9);
    char *sep = strstr(buf, "\r\n\r\n");
    if (!sep || status < 100) {
        free(buf);
        return -EIO;
    }
    if (status != 200) {
        free(buf);
        return status == 404 ? -ENOENT : -EIO;
    }
    int hdr = sep + 4 - buf;
    memmove(buf, buf + hdr, used - hdr + 1);
    *body = buf;
    *len = used - hdr;
    return 0;
}
