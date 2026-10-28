/* PID 1: owns the console, starts the session, reaps orphans.
 *
 * What to start comes from the boot menu (/etc/cmdline: mode=text|desktop)
 * or, if the menu entry didn't say, from "autostart=" in /disk/etc/petrichor.conf.
 * The default is the graphical desktop.  If the desktop can't start (no
 * compatible graphics, or it keeps crashing) we fall back to a text console. */
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

static char *slurp(const char *path)
{
    static char bufs[2][256];
    static int next;
    char *buf = bufs[next++ & 1];
    int fd = open(path, O_RDONLY);
    buf[0] = 0;
    if (fd >= 0) {
        int n = read(fd, buf, 255);
        buf[n > 0 ? n : 0] = 0;
        close(fd);
    }
    return buf;
}

/* Runs a program in its own process group and waits for it, adopting any orphans meanwhile. */
static int run(const char *path)
{
    int pid = fork();
    if (pid < 0) {
        msleep(500);
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        char *argv[] = {(char *)path, NULL};
        exec(path, argv);
        fprintf(2, "init: cannot exec %s\n", path);
        exit(127);
    }
    for (;;) {
        int status;
        int w = wait(&status);
        if (w == pid)
            return status;
        if (w < 0)
            return -1;
    }
}

static void text_session(void)
{
    run("/bin/sh");
    printf("\ninit: shell exited, restarting\n");
}

int main(void)
{
    open("/dev/console", O_RDWR);   /* fd 0 */
    dup(0);                          /* fd 1 */
    dup(0);                          /* fd 2 */

    int desktop = 1;
    const char *cmd = slurp("/etc/cmdline");
    if (strstr(cmd, "mode=text"))
        desktop = 0;
    else if (strstr(cmd, "mode=desktop"))
        desktop = 1;
    else if (strstr(slurp("/disk/etc/petrichor.conf"), "autostart=text"))
        desktop = 0;

    if (desktop) {
        printf("Starting the Petrichor desktop...\n");
        int fails = 0;
        while (fails < 2) {
            u32 t0 = uptime();
            int st = run("/bin/pdesk");
            if (st == 0) {                       /* logged out: show the desktop again */
                fails = 0;
            } else if (st == 10) {               /* "Exit to Console" */
                show_file("/etc/motd");
                text_session();
                fails = 0;
            } else if (uptime() - t0 < 500) {    /* died within five seconds */
                fails++;
            }
        }
        printf("init: the desktop could not start; using the text console.\n");
    }

    show_file("/etc/motd");
    for (;;)
        text_session();
}
