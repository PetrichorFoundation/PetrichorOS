#include <ulib.h>

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "-a"))
        printf("%s %s i386 (Multiboot, ring-3 userland)\n", PETRICHOR_NAME, PETRICHOR_VERSION);
    else
        printf("%s\n", PETRICHOR_NAME);
    return 0;
}
