/* sh: the Petrichor shell.  Pipes, redirection, ; and &, quoting, builtins. */
#include <ulib.h>

#define MAXTOK 128
#define MAXCMD 16
#define MAXARGS 32

enum { TK_WORD, TK_PIPE, TK_IN, TK_OUT, TK_APPEND, TK_AMP, TK_SEMI };

struct tok {
    int type;
    char *s;
};

struct cmd {
    char *argv[MAXARGS + 1];
    int argc;
    char *in, *out;
    int append;
};

static int last_status;

static void prompt(void)
{
    char cwd[128];
    if (getcwd(cwd, sizeof cwd) < 0)
        strcpy(cwd, "?");
    printf("\033[1;35mpetrichor\033[0m:\033[1;34m%s\033[0m# ", cwd);
}

/* Splits `line` into tokens, resolving quotes and escapes into `pool`. */
static int tokenize(const char *line, struct tok *toks, int max, char *pool)
{
    int n = 0;
    while (*line) {
        while (*line == ' ' || *line == '\t' || *line == '\n')
            line++;
        if (!*line)
            break;
        if (n == max)
            return -1;
        char c = *line;
        if (c == '#') {
            break;
        } else if (c == '|') {
            toks[n++] = (struct tok){TK_PIPE, 0};
            line++;
        } else if (c == '<') {
            toks[n++] = (struct tok){TK_IN, 0};
            line++;
        } else if (c == '>') {
            line++;
            if (*line == '>') {
                line++;
                toks[n++] = (struct tok){TK_APPEND, 0};
            } else {
                toks[n++] = (struct tok){TK_OUT, 0};
            }
        } else if (c == '&') {
            toks[n++] = (struct tok){TK_AMP, 0};
            line++;
        } else if (c == ';') {
            toks[n++] = (struct tok){TK_SEMI, 0};
            line++;
        } else {
            char *w = pool;
            while (*line && !strchr(" \t\n|<>&;", *line)) {
                if (*line == '\'' || *line == '"') {
                    char q = *line++;
                    while (*line && *line != q)
                        *pool++ = *line++;
                    if (*line)
                        line++;
                } else if (*line == '\\' && line[1]) {
                    *pool++ = line[1];
                    line += 2;
                } else {
                    *pool++ = *line++;
                }
            }
            *pool++ = 0;
            toks[n++] = (struct tok){TK_WORD, w};
        }
    }
    return n;
}

static int builtin(struct cmd *c)
{
    const char *name = c->argv[0];
    if (!strcmp(name, "cd")) {
        const char *dir = c->argc > 1 ? c->argv[1] : "/";
        int e = chdir(dir);
        if (e < 0)
            fprintf(2, "cd: %s: %s\n", dir, strerror(e));
        last_status = e < 0;
        return 1;
    }
    if (!strcmp(name, "exit")) {
        exit(c->argc > 1 ? atoi(c->argv[1]) : last_status);
    }
    if (!strcmp(name, "help")) {
        puts("builtins: cd [dir]   exit [n]   help");
        puts("syntax:   cmd args | cmd2   cmd < in > out   cmd >> log   cmd &   a ; b");
        puts("programs: ls /bin");
        last_status = 0;
        return 1;
    }
    return 0;
}

/* Tries `path`, then `path.rain`. */
static int exec_rain(const char *path, char **argv)
{
    char p[PATH_MAX];
    int e = exec(path, argv);
    if (e == -ENOENT && !strstr(path, ".rain")) {
        snprintf(p, sizeof p, "%s.rain", path);
        e = exec(p, argv);
    }
    return e;
}

/* .rain files are Petrichor's executables: a native program or a #! script, found with or without the extension. */
static void run_exec(char **argv)
{
    char path[PATH_MAX];
    int e;
    if (strchr(argv[0], '/')) {
        e = exec_rain(argv[0], argv);
    } else {                                    /* PATH is the current directory (for .rain files), /bin, /disk/bin */
        e = -ENOENT;
        int len = strlen(argv[0]);
        if (len > 5 && !strcmp(argv[0] + len - 5, ".rain"))
            e = exec(argv[0], argv);
        const char *dirs[] = {"/bin", "/disk/bin"};
        for (int i = 0; i < 2 && e == -ENOENT; i++) {
            snprintf(path, sizeof path, "%s/%s", dirs[i], argv[0]);
            e = exec_rain(path, argv);
        }
    }
    if (e == -ENOENT)
        fprintf(2, "sh: %s: command not found\n", argv[0]);
    else
        fprintf(2, "sh: %s: %s\n", argv[0], strerror(e));
}

static void run_pipeline(struct cmd *c, int n, int bg)
{
    if (n == 1 && c[0].argc > 0 && builtin(&c[0]))
        return;

    int pids[MAXCMD], started = 0, prev = -1, pgid = 0;
    for (int i = 0; i < n; i++) {
        int fds[2] = {-1, -1};
        if (i < n - 1 && pipe(fds) < 0) {
            fprintf(2, "sh: cannot create pipe\n");
            break;
        }
        int pid = fork();
        if (pid < 0) {
            fprintf(2, "sh: fork failed: %s\n", strerror(pid));
            if (fds[0] >= 0) { close(fds[0]); close(fds[1]); }
            break;
        }
        if (pid == 0) {
            if (i == 0) {
                setpgid(0, 0);
                if (!bg)
                    tcsetpgrp(getpid());
            } else {
                setpgid(0, pgid);
            }
            if (prev >= 0) { dup2(prev, 0); close(prev); }
            if (fds[1] >= 0) { dup2(fds[1], 1); close(fds[1]); }
            if (fds[0] >= 0) close(fds[0]);
            if (c[i].in) {
                int fd = open(c[i].in, O_RDONLY);
                if (fd < 0) {
                    fprintf(2, "sh: %s: %s\n", c[i].in, strerror(fd));
                    exit(1);
                }
                dup2(fd, 0);
                close(fd);
            }
            if (c[i].out) {
                int fd = open(c[i].out, O_WRONLY | O_CREAT | (c[i].append ? O_APPEND : O_TRUNC));
                if (fd < 0) {
                    fprintf(2, "sh: %s: %s\n", c[i].out, strerror(fd));
                    exit(1);
                }
                dup2(fd, 1);
                close(fd);
            }
            if (c[i].argc == 0)
                exit(0);
            run_exec(c[i].argv);
            exit(127);
        }
        if (i == 0) {
            pgid = pid;
            setpgid(pid, pid);
            if (!bg)
                tcsetpgrp(pid);
        } else {
            setpgid(pid, pgid);
        }
        pids[started++] = pid;
        if (prev >= 0) close(prev);
        if (fds[1] >= 0) close(fds[1]);
        prev = fds[0];
    }
    if (prev >= 0)
        close(prev);

    if (bg) {
        if (started)
            printf("[%d]\n", pids[started - 1]);
        return;
    }
    for (int left = started; left > 0;) {
        int status;
        int pid = wait(&status);
        if (pid < 0)
            break;
        int ours = 0;
        for (int i = 0; i < started; i++) {
            if (pids[i] == pid) {
                ours = 1;
                left--;
                if (i == started - 1)
                    last_status = status;
            }
        }
        if (!ours)
            printf("[%d] done\n", pid);     /* a background job finished meanwhile */
    }
    tcsetpgrp(0);
}

static void run_line(const char *line)
{
    struct tok toks[MAXTOK];
    char pool[1024];
    if (strlen(line) >= sizeof pool) {
        fprintf(2, "sh: line too long\n");
        return;
    }
    int nt = tokenize(line, toks, MAXTOK, pool);
    if (nt < 0) {
        fprintf(2, "sh: too many tokens\n");
        return;
    }

    int i = 0;
    while (i < nt) {
        struct cmd cmds[MAXCMD];
        memset(cmds, 0, sizeof cmds);
        int ncmd = 0, bg = 0, bad = 0;

        while (i < nt && toks[i].type != TK_SEMI && toks[i].type != TK_AMP) {
            struct cmd *c = &cmds[ncmd];
            switch (toks[i].type) {
            case TK_WORD:
                if (c->argc == MAXARGS) { bad = 1; break; }
                c->argv[c->argc++] = toks[i].s;
                break;
            case TK_PIPE:
                if (c->argc == 0 || ncmd == MAXCMD - 1) { bad = 1; break; }
                ncmd++;
                break;
            default: {
                int type = toks[i].type;
                if (i + 1 >= nt || toks[i + 1].type != TK_WORD) { bad = 1; break; }
                char *target = toks[++i].s;
                if (type == TK_IN) {
                    c->in = target;
                } else {
                    c->out = target;
                    c->append = type == TK_APPEND;
                }
            }
            }
            if (bad)
                break;
            i++;
        }
        if (bad) {
            fprintf(2, "sh: syntax error\n");
            last_status = 2;
            return;
        }
        if (i < nt) {
            bg = toks[i].type == TK_AMP;
            i++;
        }
        if (ncmd == 0 && cmds[0].argc == 0 && !cmds[0].in && !cmds[0].out)
            continue;
        run_pipeline(cmds, ncmd + 1, bg);
    }
}

static void reap_background(void)
{
    int status, pid;
    while ((pid = wait2(&status, WNOHANG)) > 0)
        printf("[%d] done\n", pid);
}

static int run_script(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(2, "sh: %s: %s\n", path, strerror(fd));
        return 127;
    }
    char line[512];
    while (read_line(fd, line, sizeof line) >= 0)
        run_line(line);
    close(fd);
    return last_status;
}

int main(int argc, char **argv)
{
    if (argc > 1)
        return run_script(argv[1]);

    char line[512];
    for (;;) {
        reap_background();
        prompt();
        int n = read(0, line, sizeof line - 1);
        if (n == 0) {
            printf("exit\n");
            return last_status;
        }
        if (n < 0)
            continue;
        line[n] = 0;
        run_line(line);
    }
}
