#include <ulib.h>

int main(int argc, char **argv)
{
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int e = mkdir(argv[i]);
        if (e < 0) {
            fprintf(2, "mkdir: %s: %s\n", argv[i], strerror(e));
            rc = 1;
        }
    }
    if (argc < 2) {
        fprintf(2, "usage: mkdir dir...\n");
        rc = 1;
    }
    return rc;
}
