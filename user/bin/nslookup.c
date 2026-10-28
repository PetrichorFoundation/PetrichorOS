#include <ulib.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(2, "usage: nslookup name\n");
        return 2;
    }
    u32 ip;
    int e = resolve(argv[1], &ip);
    if (e < 0) {
        fprintf(2, "nslookup: %s: %s\n", argv[1], strerror(e));
        return 1;
    }
    printf("%s has address %s\n", argv[1], inet_ntoa(ip));
    return 0;
}
