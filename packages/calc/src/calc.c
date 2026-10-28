#include <ulib.h>

static const char *p;
static int err;

static int expr(void);

static void skip(void) { while (*p == ' ') p++; }

static int atom(void)
{
    skip();
    if (*p == '(') {
        p++;
        int v = expr();
        skip();
        if (*p++ != ')') err = 1;
        return v;
    }
    if (*p == '-') { p++; return -atom(); }
    if (*p < '0' || *p > '9') { err = 1; return 0; }
    int v = 0;
    while (*p >= '0' && *p <= '9')
        v = v * 10 + (*p++ - '0');
    return v;
}

static int term(void)
{
    int v = atom();
    for (;;) {
        skip();
        if (*p == '*') { p++; v *= atom(); }
        else if (*p == '/' || *p == '%') {
            char op = *p++;
            int d = atom();
            if (d == 0) { err = 2; return 0; }
            v = op == '/' ? v / d : v % d;
        } else return v;
    }
}

static int expr(void)
{
    int v = term();
    for (;;) {
        skip();
        if (*p == '+') { p++; v += term(); }
        else if (*p == '-') { p++; v -= term(); }
        else return v;
    }
}

int main(int argc, char **argv)
{
    char line[256] = "";
    for (int i = 1; i < argc; i++) {
        strcat(line, argv[i]);
        strcat(line, " ");
    }
    if (argc < 2) {
        fprintf(2, "usage: calc expression   e.g. calc '(2+3)*4'\n");
        return 2;
    }
    p = line;
    int v = expr();
    skip();
    if (err == 2) { fprintf(2, "calc: division by zero\n"); return 1; }
    if (err || *p) { fprintf(2, "calc: syntax error\n"); return 1; }
    printf("%d\n", v);
    return 0;
}
