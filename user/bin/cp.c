#include <ulib.h>

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(2, "usage: cp src dst\n");
        return 1;
    }
    int in = open(argv[1], O_RDONLY);
    if (in < 0) {
        fprintf(2, "cp: %s: %s\n", argv[1], strerror(in));
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
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        fprintf(2, "cp: %s: %s\n", dst, strerror(out));
        return 1;
    }
    char buf[512];
    int n;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, n) != n) {
            fprintf(2, "cp: write failed\n");
            return 1;
        }
    return n < 0;
}
