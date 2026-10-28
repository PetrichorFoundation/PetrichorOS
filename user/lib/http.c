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

static char *strpbrk_q(char *s)
{
    for (; *s; s++)
        if (*s == '?' || *s == '#')
            return s;
    return NULL;
}

/* Resolves `ref` (an href, possibly relative) against the absolute URL `base`. */
void url_resolve(const char *base, const char *ref, char *out, int outsz)
{
    /* absolute: "scheme:" before any '/', '?' or '#' */
    const char *c = ref;
    while ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '+' || *c == '-' || *c == '.')
        c++;
    if (c > ref && *c == ':') {
        safestrcpy(out, ref, outsz);
        return;
    }

    char scheme[16] = "http", origin[160] = "", path[256] = "/";
    const char *p = base;
    const char *sep = strstr(base, "://");
    if (sep && sep - base < (int)sizeof scheme) {
        memcpy(scheme, base, sep - base);
        scheme[sep - base] = 0;
        p = sep + 3;
    }
    const char *slash = strchr(p, '/');
    int ol = slash ? slash - p : (int)strlen(p);
    if (ol >= (int)sizeof origin)
        ol = sizeof origin - 1;
    memcpy(origin, p, ol);
    origin[ol] = 0;
    if (slash)
        safestrcpy(path, slash, sizeof path);
    char *frag = strchr(path, '#');
    if (frag)
        *frag = 0;

    if (ref[0] == '/' && ref[1] == '/') {                  /* scheme-relative */
        snprintf(out, outsz, "%s:%s", scheme, ref);
        return;
    }
    char merged[320];
    if (ref[0] == 0) {
        safestrcpy(merged, path, sizeof merged);
    } else if (ref[0] == '#') {
        snprintf(merged, sizeof merged, "%s%s", path, ref);
    } else if (ref[0] == '?') {
        char *q = strchr(path, '?');
        if (q)
            *q = 0;
        snprintf(merged, sizeof merged, "%s%s", path, ref);
    } else if (ref[0] == '/') {
        safestrcpy(merged, ref, sizeof merged);
    } else {
        char *q = strchr(path, '?');
        if (q)
            *q = 0;
        char *last = strrchr(path, '/');
        if (last)
            last[1] = 0;
        snprintf(merged, sizeof merged, "%s%s", path, ref);
    }

    /* collapse "./" and "../" in the path part */
    char norm[320];
    int n = 0;
    char *tail = strpbrk_q(merged);
    char save = 0;
    if (tail) {
        save = *tail;
        *tail = 0;
    }
    for (char *s = merged; *s;) {
        if (!strncmp(s, "/./", 3)) { s += 2; continue; }
        if (!strcmp(s, "/.")) { s += 2; norm[n++] = '/'; continue; }
        if (!strncmp(s, "/../", 4) || !strcmp(s, "/..")) {
            while (n > 0 && norm[n - 1] != '/')
                n--;
            if (n > 0)
                n--;                                        /* drop the parent's slash too */
            s += 3;
            if (!*s)
                norm[n++] = '/';
            continue;
        }
        if (n < (int)sizeof norm - 2)
            norm[n++] = *s;
        s++;
    }
    norm[n] = 0;
    if (tail)
        *tail = save;
    snprintf(out, outsz, "%s://%s%s%s", scheme, origin, norm[0] ? norm : "/", tail ? tail : "");
}
