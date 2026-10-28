#include <ulib.h>

int main(int argc, char **argv)
{
    int fd = 0;
    if (argc > 1 && (fd = open(argv[1], O_RDONLY)) < 0) {
        fprintf(2, "hd: %s: %s\n", argv[1], strerror(fd));
        return 1;
    }
    unsigned char buf[16];
    int n, off = 0;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        printf("%08x  ", off);
        for (int i = 0; i < 16; i++) {
            if (i < n) printf("%02x ", buf[i]); else printf("   ");
            if (i == 7) printf(" ");
        }
        printf(" |");
        for (int i = 0; i < n; i++)
            printf("%c", buf[i] >= 32 && buf[i] < 127 ? buf[i] : '.');
        printf("|\n");
        off += n;
    }
    return 0;
}
