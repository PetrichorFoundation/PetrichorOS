#include <ulib.h>

static int cat(int fd)
{
    char buf[512];
    int n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        if (write(1, buf, n) != n)
            return 1;
    return n < 0;
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return cat(0);
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(2, "cat: %s: %s\n", argv[i], strerror(fd));
            rc = 1;
            continue;
        }
        rc |= cat(fd);
        close(fd);
    }
    return rc;
}
