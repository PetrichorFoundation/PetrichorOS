#include <ulib.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(2, "usage: mv src dst\n");
        return 1;
    }
    char dst[PATH_MAX];
    struct stat st;
    if (stat(argv[2], &st) == 0 && st.type == T_DIR) {
        const char *base = strrchr(argv[1], '/');
        snprintf(dst, sizeof dst, "%s/%s", argv[2], base ? base + 1 : argv[1]);
    } else {
        safestrcpy(dst, argv[2], sizeof dst);
    }
    int e = rename(argv[1], dst);
    if (e < 0) {
        fprintf(2, "mv: %s: %s\n", argv[1], strerror(e));
        return 1;
    }
    return 0;
}
