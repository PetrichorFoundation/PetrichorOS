/* fetch URL: download a page over plain HTTP for the browser.
 *
 * Output (stdout):   STATUS 200 / URL <final url> / TYPE <content-type> / blank line / body
 *               or:  ERROR <message>
 * Follows up to 8 redirects, decodes chunked transfer encoding, times out stalled servers.
 * HTTPS isn't supported (there is no TLS); a redirect to https:// is reported as an error. */
#include <ulib.h>

#define MAX_BODY (4 * 1024 * 1024)

static int fail(const char *msg, const char *detail)
{
    printf("ERROR %s%s%s\n", msg, detail ? ": " : "", detail ? detail : "");
    return 1;
}

static int ci_prefix(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++)
        if ((*s | 0x20) != (*prefix | 0x20))
            return 0;
    return 1;
}

/* Value of header `name` (e.g. "location:") in the header block, trimmed, or NULL. */
static char *header(char *hdrs, const char *name)
{
    for (char *l = strchr(hdrs, '\n'); l; l = strchr(l + 1, '\n')) {
        l++;
        if (ci_prefix(l, name)) {
            l += strlen(name);
            while (*l == ' ' || *l == '\t')
                l++;
            return l;
        }
    }
    return NULL;
}

static void cut_line(char *v)
{
    for (; *v; v++)
        if (*v == '\r' || *v == '\n') {
            *v = 0;
            return;
        }
}

/* Decodes a chunked body in place; returns the new length. */
static int dechunk(char *b, int len)
{
    int in = 0, out = 0;
    while (in < len) {
        int size = 0;
        while (in < len && b[in] != '\r' && b[in] != '\n' && b[in] != ';') {
            char c = b[in++] | 0x20;
            size = size * 16 + (c >= 'a' ? c - 'a' + 10 : c - '0');
        }
        while (in < len && b[in] != '\n')
            in++;
        in++;
        if (size == 0 || in + size > len)
            break;
        memmove(b + out, b + in, size);
        out += size;
        in += size + 2;
    }
    return out;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return fail("usage: fetch URL", NULL);
    char url[512];
    safestrcpy(url, argv[1], sizeof url);

    for (int hop = 0; hop < 8; hop++) {
        char *hash = strchr(url, '#');
        if (hash)
            *hash = 0;
        if (!strncmp(url, "https://", 8))
            return fail("this browser can't open HTTPS pages (there is no TLS support yet)", url);
        if (strncmp(url, "http://", 7))
            return fail("unsupported address", url);

        char host[128];
        const char *path;
        int port;
        if (url_split(url, host, sizeof host, &port, &path) < 0)
            return fail("bad address", url);
        u32 ip;
        int e = resolve(host, &ip);
        if (e < 0)
            return fail("can't find the server", host);
        int fd = socket(SOCK_STREAM);
        if (fd < 0 || (e = connect(fd, ip, port)) < 0) {
            close(fd);
            return fail(e == -ECONNREFUSED ? "connection refused" : e == -ETIMEDOUT ? "connection timed out" : "can't connect",
                        host);
        }
        settimeout(fd, 15000);
        char req[768];
        int n = snprintf(req, sizeof req,
                         "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Petrichor/0.1 (pbrowse)\r\n"
                         "Accept: text/html,text/plain;q=0.9,*/*;q=0.5\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
                         path, host);
        if (write(fd, req, n) < 0)
            return fail("can't send the request", NULL);

        int cap = 16384, used = 0, hdr_end = -1, content_len = -1;
        char *buf = malloc(cap);
        while (buf) {
            if (used + 4096 + 1 > cap) {
                if (cap >= MAX_BODY)
                    break;
                char *nb = realloc(buf, cap *= 2);
                if (!nb)
                    break;
                buf = nb;
            }
            int r = read(fd, buf + used, cap - used - 1);
            if (r < 0) {
                if (used == 0)
                    return fail(r == -ETIMEDOUT ? "the server didn't answer" : "connection lost", host);
                break;
            }
            if (r == 0)
                break;
            used += r;
            buf[used] = 0;
            if (hdr_end < 0) {
                char *sep = strstr(buf, "\r\n\r\n");
                if (sep) {
                    hdr_end = sep + 4 - buf;
                    char *cl = header(buf, "content-length:");
                    content_len = cl ? atoi(cl) : -1;
                }
            }
            if (hdr_end >= 0 && content_len >= 0 && used - hdr_end >= content_len)
                break;                                   /* we have it all; don't wait for the server to close */
        }
        close(fd);
        if (!buf || hdr_end < 0)
            return fail("the server sent an invalid response", host);
        buf[used] = 0;
        int status = !strncmp(buf, "HTTP/", 5) ? atoi(buf + 9) : 0;
        char hdrs[2048];
        int hl = hdr_end < (int)sizeof hdrs ? hdr_end : (int)sizeof hdrs - 1;
        memcpy(hdrs, buf, hl);
        hdrs[hl] = 0;

        if (status >= 300 && status < 400) {
            char *loc = header(hdrs, "location:");
            if (loc) {
                cut_line(loc);
                char next[512];
                url_resolve(url, loc, next, sizeof next);
                safestrcpy(url, next, sizeof url);
                free(buf);
                continue;
            }
        }
        char *body = buf + hdr_end;
        int blen = used - hdr_end;
        char *te = header(hdrs, "transfer-encoding:");
        if (te && ci_prefix(te, "chunked"))
            blen = dechunk(body, blen);
        char *ct = header(hdrs, "content-type:");
        if (ct)
            cut_line(ct);
        printf("STATUS %d\nURL %s\nTYPE %s\n\n", status, url, ct ? ct : "text/html");
        int off = 0;
        while (off < blen) {
            int w = write(1, body + off, blen - off > 4096 ? 4096 : blen - off);
            if (w <= 0)
                break;
            off += w;
        }
        return 0;
    }
    return fail("too many redirects", url);
}
