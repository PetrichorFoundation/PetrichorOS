#include <ulib.h>

int main(int argc, char **argv)
{
    printf("Hello from Petrichor, pid %d!\n", getpid());
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    return 0;
}
