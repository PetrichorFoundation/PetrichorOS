#include <ulib.h>

static int grep(int fd, const char *pat, const char *name, int show_name)
{
    char line[512];
    int found = 0;
    while (read_line(fd, line, sizeof line) >= 0) {
        if (strstr(line, pat)) {
            if (show_name)
                printf("%s:", name);
            puts(line);
            found = 1;
        }
    }
    return found;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(2, "usage: grep pattern [file...]\n");
        return 2;
    }
    if (argc == 2)
        return !grep(0, argv[1], "", 0);
    int found = 0;
    for (int i = 2; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            fprintf(2, "grep: %s: %s\n", argv[i], strerror(fd));
            continue;
        }
        found |= grep(fd, argv[1], argv[i], argc > 3);
        close(fd);
    }
    return !found;
}
