/* URL helpers shared by the HTTP clients. */
#include "ulib.h"

/* Splits "http[s]://host[:port]/path" (scheme optional; the default port follows the scheme). */
int url_split(const char *url, char *host, int hostsz, int *port, const char **path)
{
    *port = 80;
    if (!strncmp(url, "http://", 7))
        url += 7;
    else if (!strncmp(url, "https://", 8)) {
        url += 8;
        *port = 443;
    }
    const char *slash = strchr(url, '/');
    int hl = slash ? slash - url : (int)strlen(url);
    if (hl <= 0 || hl >= hostsz)
        return -EINVAL;
    memcpy(host, url, hl);
    host[hl] = 0;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        *port = atoi(colon + 1);
    }
    *path = slash ? slash : "/";
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
