#include <ulib.h>

int main(int argc, char **argv)
{
    int nl = 1, i = 1;
    if (argc > 1 && !strcmp(argv[1], "-n")) {
        nl = 0;
        i = 2;
    }
    for (; i < argc; i++) {
        fputs(1, argv[i]);
        if (i + 1 < argc)
            fputs(1, " ");
    }
    if (nl)
        fputs(1, "\n");
    return 0;
}
