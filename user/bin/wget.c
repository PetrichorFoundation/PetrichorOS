/* wget [-i] [-k] [-O file] URL  -- HTTP/1.0 GET over http:// or https://; the body goes to stdout or -O file.
 * -i keeps the response headers, -k skips HTTPS certificate validation (testing only). */
#include <ulib.h>
#include "tls/tls.h"

int main(int argc, char **argv)
{
    const char *url = NULL, *outfile = NULL;
    int show_headers = 0, flags = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O") && i + 1 < argc)
            outfile = argv[++i];
        else if (!strcmp(argv[i], "-i"))
            show_headers = 1;
        else if (!strcmp(argv[i], "-k"))
            flags = TLS_INSECURE;
        else
            url = argv[i];
    }
    if (!url) {
        fprintf(2, "usage: wget [-i] [-k] [-O file] http[s]://host[:port]/path\n");
        return 2;
    }
    char host[128];
    const char *path, *err;
    int port;
    if (url_split(url, host, sizeof host, &port, &path) < 0) {
        fprintf(2, "wget: bad url\n");
        return 2;
    }
    struct conn c;
    int e = conn_open(&c, host, port, !strncmp(url, "https://", 8), flags, &err);
    if (e < 0) {
        fprintf(2, "wget: %s: %s\n", host, err);
        return 1;
    }
    char req[512];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: petrichor-wget\r\n\r\n",
                     path, host);
    if ((e = conn_write(&c, req, n)) < 0) {
        fprintf(2, "wget: send: %s\n", strerror(e));
        return 1;
    }

    int out = 1;
    if (outfile && (out = open(outfile, O_WRONLY | O_CREAT | O_TRUNC)) < 0) {
        fprintf(2, "wget: %s: %s\n", outfile, strerror(out));
        return 1;
    }
    char buf[1024], tail[4] = {0};
    int in_headers = !show_headers, total = 0;
    while ((n = conn_read(&c, buf, sizeof buf)) > 0) {
        int start = 0;
        if (in_headers) {                       /* skip to the blank line ending the headers */
            for (int i = 0; i < n; i++) {
                memmove(tail, tail + 1, 3);
                tail[3] = buf[i];
                if (!memcmp(tail, "\r\n\r\n", 4)) {
                    in_headers = 0;
                    start = i + 1;
                    break;
                }
            }
            if (in_headers)
                continue;
        }
        write(out, buf + start, n - start);
        total += n - start;
    }
    conn_close(&c);
    if (n < 0) {
        fprintf(2, "wget: read: %s\n", strerror(n));
        return 1;
    }
    if (outfile)
        printf("saved %d bytes to %s\n", total, outfile);
    return 0;
}
