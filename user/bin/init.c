/* PID 1: owns the console, runs a shell, reaps orphans. */
#include <ulib.h>

static void show_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    char buf[256];
    int n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        write(1, buf, n);
    close(fd);
}

int main(void)
{
    open("/dev/console", O_RDWR);   /* fd 0 */
    dup(0);                          /* fd 1 */
    dup(0);                          /* fd 2 */

    show_file("/etc/motd");

    for (;;) {
        int pid = fork();
        if (pid < 0) {
            printf("init: fork failed: %s\n", strerror(pid));
            msleep(1000);
            continue;
        }
        if (pid == 0) {
            setpgid(0, 0);
            char *argv[] = {"sh", NULL};
            exec("/bin/sh", argv);
            printf("init: cannot exec /bin/sh\n");
            exit(1);
        }
        for (;;) {                   /* adopt and reap until the shell is gone */
            int status;
            int w = wait(&status);
            if (w == pid || w < 0)
                break;
        }
        printf("\ninit: shell exited, restarting\n");
    }
}
