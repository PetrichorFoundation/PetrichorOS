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

static void test_disk(void)
{
    printf("disk (PFS on /disk)\n");
    struct diskinfo d0, d1;
    diskinfo(&d0);
    if (!d0.mounted) {
        printf("  skip  no disk attached\n");
        return;
    }
    struct stat st;
    CHECK(mkdir("/disk/t") == 0 && mkdir("/disk/t/sub") == 0, "mkdir on disk");

    /* 100 KiB crosses from direct into indirect blocks */
    enum { BIG = 100 * 1024 };
    char *buf = malloc(BIG);
    for (int i = 0; i < BIG; i++)
        buf[i] = (char)(i * 7 + (i >> 8));
    int fd = open("/disk/t/big", O_WRONLY | O_CREAT | O_TRUNC);
    int w = 0;
    for (int off = 0; off < BIG && fd >= 0; off += 3000) {
        int n = BIG - off < 3000 ? BIG - off : 3000;
        w += write(fd, buf + off, n);
    }
    close(fd);
    CHECK(w == BIG, "write 100 KiB in odd-sized chunks");
    CHECK(stat("/disk/t/big", &st) == 0 && st.size == BIG, "size is right");

    char *back = malloc(BIG);
    fd = open("/disk/t/big", O_RDONLY);
    int total = 0, n;
    while ((n = read(fd, back + total, 4096)) > 0)
        total += n;
    close(fd);
    CHECK(total == BIG && !memcmp(buf, back, BIG), "read back identical");

    fd = open("/disk/t/big", O_RDONLY);
    lseek(fd, 70000, SEEK_SET);
    char c;
    read(fd, &c, 1);
    close(fd);
    CHECK(c == buf[70000], "seek into the indirect region");

    CHECK(rename("/disk/t/big", "/disk/t/sub/moved") == 0 && stat("/disk/t/big", &st) < 0 &&
              stat("/disk/t/sub/moved", &st) == 0 && st.size == BIG, "rename across directories");
    CHECK(rename("/disk/t/sub/moved", "/tmp/nope") == -EXDEV, "rename across filesystems -> EXDEV");
    CHECK(unlink("/disk/t") == -ENOTEMPTY, "non-empty dir -> ENOTEMPTY");

    char path[64];
    int made = 0;
    for (int i = 0; i < 40; i++) {
        snprintf(path, sizeof path, "/disk/t/f%d", i);
        fd = open(path, O_WRONLY | O_CREAT);
        if (fd >= 0) { made++; close(fd); }
    }
    CHECK(made == 40, "create 40 files in one directory");
    int count = 0;
    struct dirent de;
    fd = open("/disk/t", O_RDONLY);
    while (readdir(fd, &de) > 0)
        count++;
    close(fd);
    CHECK(count == 40 + 2 + 1, "readdir sees every entry");
    for (int i = 0; i < 40; i++) {
        snprintf(path, sizeof path, "/disk/t/f%d", i);
        unlink(path);
    }

    CHECK(unlink("/disk/t/sub/moved") == 0 && unlink("/disk/t/sub") == 0 && unlink("/disk/t") == 0,
          "cleanup");
    diskinfo(&d1);
    CHECK(d1.free_kb == d0.free_kb, "all blocks returned to the free pool");
    CHECK(unlink("/disk") == -EBUSY, "can't remove a mountpoint");
    free(buf);
    free(back);
}

static void test_sockets(void)
{
    printf("sockets\n");
    struct netinfo ni;
    netinfo(&ni);
    if (!ni.up) {
        printf("  skip  no network card\n");
        return;
    }
    u32 ip;
    CHECK(inet_aton("192.168.1.77", &ip) == 0 && ip == 0xC0A8014D, "inet_aton");
    CHECK(inet_aton("1.2.3", &ip) < 0 && inet_aton("300.1.1.1", &ip) < 0, "inet_aton rejects junk");
    CHECK(!strcmp(inet_ntoa(0x0A00020F), "10.0.2.15"), "inet_ntoa");
    CHECK(socket(99) == -EINVAL, "bad socket type -> EINVAL");
    int u = socket(SOCK_DGRAM);
    CHECK(u >= 0 && write(u, "x", 1) == -ENOTCONN, "unconnected UDP write -> ENOTCONN");
    int u2 = socket(SOCK_DGRAM);
    CHECK(bind(u, 5555) == 0 && bind(u2, 5555) == -EADDRINUSE, "bind conflict -> EADDRINUSE");
    CHECK(read(0, NULL, 0) == 0 && connect(0, 1, 1) == -ENOTSOCK, "connect on a non-socket -> ENOTSOCK");
    struct stat st;
    CHECK(fstat(u, &st) == 0 && st.type == T_SOCK, "sockets are fds (fstat says T_SOCK)");
    settimeout(u2, 100);
    u32 t0 = uptime();
    char b[8];
    CHECK(recvfrom(u2, b, sizeof b, NULL) == -ETIMEDOUT && uptime() - t0 >= 9, "recvfrom times out");
    close(u);
    close(u2);
    int t = socket(SOCK_STREAM);
    CHECK(t >= 0 && connect(t, 0x0A000202, 1) == -ECONNREFUSED, "TCP connect to a closed port -> ECONNREFUSED");
    close(t);
}

static void test_scripts_poll_clock(void)
{
    printf("scripts, poll, clock\n");
    int fd = open("/tmp/s.sh", O_WRONLY | O_CREAT | O_TRUNC);
    write(fd, "#!/bin/echo hi\n", 15);
    close(fd);
    int pid = fork();
    if (pid == 0) {
        int o = open("/tmp/s.out", O_WRONLY | O_CREAT | O_TRUNC);
        dup2(o, 1);
        char *args[] = {"s.sh", "arg", NULL};
        exec("/tmp/s.sh", args);
        exit(1);
    }
    int status;
    wait(&status);
    char buf[64];
    read_all("/tmp/s.out", buf, sizeof buf);
    CHECK(status == 0 && !strcmp(buf, "hi /tmp/s.sh arg\n"), "#! script runs its interpreter");
    unlink("/tmp/s.sh");
    unlink("/tmp/s.out");

    int p[2];
    pipe(p);
    struct pollfd pf = {p[0], POLLIN, 0};
    u32 t0 = uptime();
    CHECK(poll(&pf, 1, 100) == 0 && uptime() - t0 >= 9, "poll times out on an empty pipe");
    write(p[1], "x", 1);
    CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & POLLIN), "poll sees data");
    read(p[0], buf, 1);
    close(p[1]);
    CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & POLLHUP), "poll reports a closed writer (POLLHUP)");
    struct pollfd bad = {99, POLLIN, 0};
    CHECK(poll(&bad, 1, 0) == 1 && (bad.revents & POLLNVAL), "poll flags a bad descriptor");
    close(p[0]);

    struct rtc_time t;
    CHECK(gettime(&t) == 0 && t.year >= 2024 && t.month >= 1 && t.month <= 12 && t.hour < 24 && t.min < 60,
          "RTC returns a plausible date");
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
    test_disk();
    test_sockets();
    test_scripts_poll_clock();
    test_processes();
    test_pipes();
    test_memory();
    printf("\n%d checks, %d failed\n", checks, failures);
    printf(failures ? "SELFTEST FAILED\n" : "SELFTEST PASSED\n");
    return failures != 0;
}
