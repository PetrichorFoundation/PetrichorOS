#include <ulib.h>

int main(void)
{
    int fd = open("/disk/share/fortune/fortunes", O_RDONLY);
    if (fd < 0) {
        fprintf(2, "fortune: no fortune file: %s\n", strerror(fd));
        return 1;
    }
    static char buf[8192];
    int n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return 1;
    buf[n] = 0;

    char *starts[64];
    int count = 0;
    char *p = buf;
    while (*p && count < 64) {
        starts[count++] = p;
        char *sep = strstr(p, "\n%\n");
        if (!sep)
            break;
        *sep = 0;
        p = sep + 3;
    }
    u32 r = 0;
    int rf = open("/dev/random", O_RDONLY);
    read(rf, &r, sizeof r);
    close(rf);
    puts(starts[r % count]);
    return 0;
}
