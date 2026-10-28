#include <ulib.h>

static void count(int fd, const char *name)
{
    char buf[512];
    int n, lines = 0, words = 0, bytes = 0, inword = 0;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            bytes++;
            if (c == '\n')
                lines++;
            if (c == ' ' || c == '\n' || c == '\t') {
                inword = 0;
            } else if (!inword) {
                inword = 1;
                words++;
            }
        }
    }
    printf("%7d %7d %7d %s\n", lines, words, bytes, name);
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        count(0, "");
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(2, "wc: %s: %s\n", argv[i], strerror(fd));
            continue;
        }
        count(fd, argv[i]);
        close(fd);
    }
    return 0;
}
