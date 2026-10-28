/* nc [-u] host port: pipe stdin to a TCP/UDP socket and the socket to stdout. */
#include <ulib.h>

int main(int argc, char **argv)
{
    int udp = 0, i = 1;
    if (i < argc && !strcmp(argv[i], "-u")) {
        udp = 1;
        i++;
    }
    if (argc - i != 2) {
        fprintf(2, "usage: nc [-u] host port\n");
        return 2;
    }
    u32 ip;
    int e = resolve(argv[i], &ip);
    if (e < 0) {
        fprintf(2, "nc: %s: %s\n", argv[i], strerror(e));
        return 1;
    }
    int fd = socket(udp ? SOCK_DGRAM : SOCK_STREAM);
    if ((e = connect(fd, ip, atoi(argv[i + 1]))) < 0) {
        fprintf(2, "nc: connect: %s\n", strerror(e));
        return 1;
    }
    if (udp)
        settimeout(fd, 2000);

    int pid = fork();
    if (pid == 0) {                              /* child: stdin -> socket */
        char buf[512];
        int n;
        while ((n = read(0, buf, sizeof buf)) > 0)
            if (write(fd, buf, n) < 0)
                break;
        exit(0);
    }
    char buf[1024];
    int n;
    while ((n = read(fd, buf, sizeof buf)) > 0)  /* parent: socket -> stdout */
        write(1, buf, n);
    kill(pid, 9);
    wait(&e);
    close(fd);
    return n < 0 && n != -ETIMEDOUT;
}
