#include <ulib.h>

int main(int argc, char **argv)
{
    int max = 10, i = 1, fd = 0;
    if (argc > 2 && !strcmp(argv[1], "-n")) {
        max = atoi(argv[2]);
        i = 3;
    }
    if (i < argc) {
        fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(2, "head: %s: %s\n", argv[i], strerror(fd));
            return 1;
        }
    }
    char line[512];
    for (int n = 0; n < max && read_line(fd, line, sizeof line) >= 0; n++)
        puts(line);
    return 0;
}
