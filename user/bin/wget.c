/* wget http://host[:port]/path  -- HTTP/1.0 GET; body goes to stdout or -O file. */
#include <ulib.h>

int main(int argc, char **argv)
{
    const char *url = NULL, *outfile = NULL;
    int show_headers = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-O") && i + 1 < argc)
            outfile = argv[++i];
        else if (!strcmp(argv[i], "-i"))
            show_headers = 1;
        else
            url = argv[i];
    }
    if (!url) {
        fprintf(2, "usage: wget [-i] [-O file] http://host[:port]/path\n");
        return 2;
    }
    if (!strncmp(url, "http://", 7))
        url += 7;

    char host[128];
    const char *slash = strchr(url, '/');
    int hl = slash ? slash - url : (int)strlen(url);
    if (hl == 0 || hl >= (int)sizeof host) {
        fprintf(2, "wget: bad url\n");
        return 2;
    }
    memcpy(host, url, hl);
    host[hl] = 0;
    const char *path = slash ? slash : "/";
    int port = 80;
    char *colon = strchr(host, ':');
    if (colon) {
        *colon = 0;
        port = atoi(colon + 1);
    }

    u32 ip;
    int e = resolve(host, &ip);
    if (e < 0) {
        fprintf(2, "wget: %s: %s\n", host, strerror(e));
        return 1;
    }
    int fd = socket(SOCK_STREAM);
    if ((e = connect(fd, ip, port)) < 0) {
        fprintf(2, "wget: connect %s:%d: %s\n", inet_ntoa(ip), port, strerror(e));
        return 1;
    }
    char req[512];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: petrichor-wget\r\n\r\n",
                     path, host);
    if ((e = write(fd, req, n)) < 0) {
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
    while ((n = read(fd, buf, sizeof buf)) > 0) {
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
    close(fd);
    if (n < 0) {
        fprintf(2, "wget: read: %s\n", strerror(n));
        return 1;
    }
    if (outfile)
        printf("saved %d bytes to %s\n", total, outfile);
    return 0;
}
