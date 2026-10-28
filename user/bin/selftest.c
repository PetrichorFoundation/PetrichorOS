/* selftest: exercises the kernel's syscalls end to end. */
#include <ulib.h>

static int failures, checks;

#define CHECK(cond, what)                                              \
    do {                                                               \
        checks++;                                                      \
        if (cond) {                                                    \
            printf("  ok    %s\n", what);                              \
        } else {                                                       \
            printf("  FAIL  %s  (%s:%d)\n", what, __FILE__, __LINE__); \
            failures++;                                                \
        }                                                              \
    } while (0)

static int read_all(const char *path, char *buf, int max)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return fd;
    int n = read(fd, buf, max - 1);
    close(fd);
    if (n >= 0)
        buf[n] = 0;
    return n;
}

static void test_files(void)
{
    printf("files\n");
    char buf[128];
    int fd = open("/tmp/a.txt", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 0, "create file");
    CHECK(write(fd, "hello, ", 7) == 7, "write");
    CHECK(write(fd, "world", 5) == 5, "write continues at offset");
    close(fd);
    CHECK(read_all("/tmp/a.txt", buf, sizeof buf) == 12 && !strcmp(buf, "hello, world"), "read back");

    fd = open("/tmp/a.txt", O_WRONLY | O_APPEND);
    write(fd, "!", 1);
    close(fd);
    read_all("/tmp/a.txt", buf, sizeof buf);
    CHECK(!strcmp(buf, "hello, world!"), "O_APPEND");

    fd = open("/tmp/a.txt", O_RDONLY);
    CHECK(lseek(fd, 7, SEEK_SET) == 7, "lseek");
    CHECK(read(fd, buf, 5) == 5 && !memcmp(buf, "world", 5), "read after seek");
    CHECK(lseek(fd, 0, SEEK_END) == 13, "seek to end");
    CHECK(read(fd, buf, 5) == 0, "EOF reads 0");
    close(fd);

    struct stat st;
    CHECK(stat("/tmp/a.txt", &st) == 0 && st.type == T_FILE && st.size == 13, "stat");
    CHECK(open("/tmp/nope", O_RDONLY) == -ENOENT, "open missing file -> ENOENT");

    CHECK(rename("/tmp/a.txt", "/tmp/b.txt") == 0, "rename");
    CHECK(stat("/tmp/a.txt", &st) == -ENOENT && stat("/tmp/b.txt", &st) == 0, "rename moved it");
    CHECK(unlink("/tmp/b.txt") == 0 && stat("/tmp/b.txt", &st) == -ENOENT, "unlink");
}

static void test_dirs(void)
{
    printf("directories\n");
    struct stat st;
    char buf[64];
    CHECK(mkdir("/tmp/d") == 0, "mkdir");
    CHECK(mkdir("/tmp/d") == -EEXIST, "mkdir twice -> EEXIST");
    CHECK(mkdir("/tmp/d/e") == 0, "nested mkdir");
    CHECK(stat("/tmp/d/e/..", &st) == 0 && st.type == T_DIR, "'..' resolves");
    CHECK(chdir("/tmp/d/e") == 0, "chdir");
    CHECK(getcwd(buf, sizeof buf) == 0 && !strcmp(buf, "/tmp/d/e"), "getcwd");
    int fd = open("../x", O_WRONLY | O_CREAT);
    CHECK(fd >= 0, "relative create");
    close(fd);
    CHECK(stat("/tmp/d/x", &st) == 0, "relative path landed in parent");
    CHECK(unlink("/tmp/d") == -ENOTEMPTY, "unlink non-empty dir -> ENOTEMPTY");
    CHECK(rename("/tmp/d/e", "/tmp/e2") == 0, "move directory");
    CHECK(chdir("/") == 0 && chdir("/tmp/e2") == 0 && getcwd(buf, sizeof buf) == 0 && !strcmp(buf, "/tmp/e2"),
          "moved dir keeps working '..'");
    chdir("/");
    CHECK(unlink("/tmp/e2") == 0 && unlink("/tmp/d/x") == 0 && unlink("/tmp/d") == 0, "cleanup");

    int n = 0;
    struct dirent de;
    fd = open("/dev", O_RDONLY);
    while (readdir(fd, &de) > 0)
        n++;
    close(fd);
    CHECK(n >= 7, "readdir /dev");
}

static void test_devices(void)
{
    printf("devices\n");
    char buf[16];
    memset(buf, 0xAA, sizeof buf);
    int fd = open("/dev/zero", O_RDONLY);
    CHECK(read(fd, buf, sizeof buf) == 16 && buf[0] == 0 && buf[15] == 0, "/dev/zero");
    close(fd);
    fd = open("/dev/null", O_RDWR);
    CHECK(write(fd, "x", 1) == 1 && read(fd, buf, 1) == 0, "/dev/null");
    close(fd);
}

static void test_processes(void)
{
    printf("processes\n");
    int status = -1;
    int pid = fork();
    if (pid == 0)
        exit(42);
    CHECK(pid > 0, "fork");
    CHECK(wait(&status) == pid && status == 42, "wait returns child's exit status");
    CHECK(wait(&status) == -ECHILD, "wait with no children -> ECHILD");

    int value = 1;
    pid = fork();
    if (pid == 0) {
        value = 99;
        exit(value == 99 ? 0 : 1);
    }
    wait(&status);
    CHECK(value == 1 && status == 0, "fork gives a private address space");

    pid = fork();
    if (pid == 0) {
        char *args[] = {"echo", "from", "exec", NULL};
        int fd = open("/tmp/exec.out", O_WRONLY | O_CREAT | O_TRUNC);
        dup2(fd, 1);
        exec("/bin/echo", args);
        exit(1);
    }
    wait(&status);
    char buf[64];
    read_all("/tmp/exec.out", buf, sizeof buf);
    CHECK(status == 0 && !strcmp(buf, "from exec\n"), "fork + dup2 + exec");
    unlink("/tmp/exec.out");

    pid = fork();
    if (pid == 0) {
        char *args[] = {"nonexistent", NULL};
        exit(exec("/bin/nonexistent", args) == -ENOENT ? 7 : 8);
    }
    wait(&status);
    CHECK(status == 7, "exec of missing file -> ENOENT, caller survives");

    pid = fork();
    if (pid == 0) {
        msleep(60000);
        exit(0);
    }
    msleep(50);
    CHECK(kill(pid, 9) == 0, "kill");
    CHECK(wait(&status) == pid && status == 137, "killed child reaped with status 137");

    pid = fork();
    if (pid == 0) {
        volatile int *bad = (int *)0x10;
        *bad = 1;
        exit(0);
    }
    wait(&status);
    CHECK(status == 139, "wild pointer kills only the offender");

    u32 t0 = uptime();
    msleep(200);
    u32 dt = uptime() - t0;
    CHECK(dt >= 20 && dt <= 40, "msleep(200) ~ 20 ticks");
}

static void test_pipes(void)
{
    printf("pipes\n");
    int fds[2];
    CHECK(pipe(fds) == 0, "pipe");
    int pid = fork();
    if (pid == 0) {
        close(fds[0]);
        for (int i = 0; i < 2000; i++)
            write(fds[1], "0123456789", 10);   /* 20000 bytes > pipe capacity: forces blocking */
        exit(0);
    }
    close(fds[1]);
    int total = 0, n;
    char buf[300];
    while ((n = read(fds[0], buf, sizeof buf)) > 0)
        total += n;
    int status;
    wait(&status);
    CHECK(total == 20000, "20000 bytes through a 4096-byte pipe");
    close(fds[0]);

    pipe(fds);
    close(fds[0]);
    CHECK(write(fds[1], "x", 1) == -EPIPE, "write to pipe with no reader -> EPIPE");
    close(fds[1]);

    /* echo | wc, wired by hand like the shell does */
    pipe(fds);
    if (fork() == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        char *args[] = {"echo", "a b c", NULL};
        exec("/bin/echo", args);
        exit(1);
    }
    close(fds[1]);
    int out[2];
    pipe(out);
    if (fork() == 0) {
        dup2(fds[0], 0);
        dup2(out[1], 1);
        close(fds[0]); close(out[0]); close(out[1]);
        char *args[] = {"wc", NULL};
        exec("/bin/wc", args);
        exit(1);
    }
    close(fds[0]);
    close(out[1]);
    wait(&status);
    wait(&status);
    n = read(out[0], buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = 0;
    close(out[0]);
    CHECK(n > 0 && atoi(buf) == 1 && strstr(buf, "3") && strstr(buf, "6"), "echo | wc");
}

static void test_memory(void)
{
    printf("memory\n");
    char *p = sbrk(0);
    char *q = sbrk(3 * 4096);
    CHECK(q == p, "sbrk returns old break");
    memset(q, 0x5A, 3 * 4096);
    CHECK(q[3 * 4096 - 1] == 0x5A, "grown heap is writable");
    CHECK(sbrk(-3 * 4096) == q + 3 * 4096 && sbrk(0) == p, "sbrk shrinks");

    char *a = malloc(100), *b = malloc(5000), *c = malloc(10);
    CHECK(a && b && c && a != b && b != c, "malloc");
    memset(b, 1, 5000);
    free(b);
    char *d = malloc(4000);
    CHECK(d && d <= b, "free + malloc reuses freed space (coalesced)");
    free(a); free(c); free(d);

    int ok = 1;
    char *blocks[64];
    for (int i = 0; i < 64; i++) {
        blocks[i] = malloc(1000 + i);
        memset(blocks[i], i, 1000 + i);
    }
    for (int i = 0; i < 64; i++)
        for (int j = 0; j < 1000 + i; j++)
            if (blocks[i][j] != (char)i)
                ok = 0;
    CHECK(ok, "64 live allocations hold their contents");
    for (int i = 0; i < 64; i++)
        free(blocks[i]);

    struct meminfo m1, m2;
    meminfo(&m1);
    for (int i = 0; i < 20; i++) {
        int pid = fork();
        if (pid == 0)
            exit(0);
        int st;
        wait(&st);
    }
    meminfo(&m2);
    CHECK(m1.free_kb == m2.free_kb, "20 fork/exit cycles leak no physical memory");
}

int main(void)
{
    printf("Petrichor selftest (pid %d)\n", getpid());
    test_files();
    test_dirs();
    test_devices();
    test_processes();
    test_pipes();
    test_memory();
    printf("\n%d checks, %d failed\n", checks, failures);
    printf(failures ? "SELFTEST FAILED\n" : "SELFTEST PASSED\n");
    return failures != 0;
}
