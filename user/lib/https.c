/* Client connections that speak plain HTTP or HTTPS, and a blocking GET built on them. */
#include "ulib.h"
#include "tls/tls.h"

#define MAX_BODY (16 * 1024 * 1024)

int conn_open(struct conn *c, const char *host, int port, int https, int flags, const char **err)
{
    static char msg[160];
    c->fd = -1;
    c->tls = NULL;
    u32 ip;
    int e = resolve(host, &ip);
    if (e < 0) {
        *err = "can't find the server";
        return e;
    }
    int fd = socket(SOCK_STREAM);
    if (fd < 0 || (e = connect(fd, ip, port)) < 0) {
        if (fd >= 0)
            close(fd);
        e = fd < 0 ? fd : e;
        *err = e == -ECONNREFUSED ? "connection refused" : e == -ETIMEDOUT ? "connection timed out" : "can't connect";
        return e;
    }
    settimeout(fd, 15000);
    c->fd = fd;
    if (https) {
        c->tls = tls_connect(fd, host, flags, err);
        if (!c->tls) {
            snprintf(msg, sizeof msg, "%s", *err);
            *err = msg;
            close(fd);
            c->fd = -1;
            return -EPROTO;
        }
    }
    return 0;
}

int conn_read(struct conn *c, void *buf, int n) { return c->tls ? tls_read(c->tls, buf, n) : read(c->fd, buf, n); }

int conn_write(struct conn *c, const void *buf, int n)
{
    if (c->tls)
        return tls_write(c->tls, buf, n);
    int off = 0;
    while (off < n) {
        int w = write(c->fd, (const char *)buf + off, n - off);
        if (w <= 0)
            return w < 0 ? w : -EIO;
        off += w;
    }
    return n;
}

void conn_close(struct conn *c)
{
    if (c->tls)
        tls_close(c->tls);
    if (c->fd >= 0)
        close(c->fd);
    c->tls = NULL;
    c->fd = -1;
}

/* Fetches `url`; on success *body is malloc'd (NUL-terminated) and *len is its size. */
int http_get(const char *url, char **body, int *len)
{
    char host[128];
    const char *path, *err;
    int port;
    int https = !strncmp(url, "https://", 8);
    int e = url_split(url, host, sizeof host, &port, &path);
    if (e < 0)
        return e;
    struct conn c;
    if ((e = conn_open(&c, host, port, https, 0, &err)) < 0) {
        if (https)
            fprintf(2, "%s: %s\n", host, err);
        return e;
    }
    char req[512];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: petrichor-ppm\r\n\r\n", path, host);
    if ((e = conn_write(&c, req, n)) < 0) {
        conn_close(&c);
        return e;
    }

    int cap = 8192, used = 0;
    char *buf = malloc(cap);
    while (buf) {
        if (used + 4096 + 1 > cap) {
            if (cap >= MAX_BODY) {
                free(buf);
                conn_close(&c);
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
        n = conn_read(&c, buf + used, cap - used - 1);
        if (n == 0)
            break;
        if (n < 0) {
            free(buf);
            conn_close(&c);
            return n;
        }
        used += n;
    }
    conn_close(&c);
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
