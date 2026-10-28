#include <ulib.h>

int main(int argc, char **argv)
{
    int count = 4, i = 1;
    if (argc > 2 && !strcmp(argv[1], "-c")) {
        count = atoi(argv[2]);
        i = 3;
    }
    if (i >= argc) {
        fprintf(2, "usage: ping [-c count] host\n");
        return 2;
    }
    u32 ip;
    int e = resolve(argv[i], &ip);
    if (e < 0) {
        fprintf(2, "ping: %s: %s\n", argv[i], strerror(e));
        return 2;
    }
    printf("PING %s (%s)\n", argv[i], inet_ntoa(ip));
    int ok = 0;
    for (int n = 0; n < count; n++) {
        int rtt = ping(ip, 2000);
        if (rtt >= 0) {
            ok++;
            if (rtt == 0)
                printf("reply from %s: seq=%d time<10 ms\n", inet_ntoa(ip), n + 1);
            else
                printf("reply from %s: seq=%d time=%d ms\n", inet_ntoa(ip), n + 1, rtt * 10);
        } else {
            printf("seq=%d: %s\n", n + 1, strerror(rtt));
        }
        if (n + 1 < count)
            msleep(500);
    }
    printf("%d sent, %d received\n", count, ok);
    return ok == 0;
}
