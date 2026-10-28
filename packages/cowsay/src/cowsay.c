#include <ulib.h>

#define WIDTH 38

int main(int argc, char **argv)
{
    char text[1024];
    int n = 0;
    if (argc > 1) {
        for (int i = 1; i < argc && n < (int)sizeof text - 2; i++) {
            if (i > 1)
                text[n++] = ' ';
            for (const char *p = argv[i]; *p && n < (int)sizeof text - 2; p++)
                text[n++] = *p;
        }
    } else {
        int r;
        while (n < (int)sizeof text - 2 && (r = read(0, text + n, sizeof text - 1 - n)) > 0)
            n += r;
    }
    for (int i = 0; i < n; i++)
        if (text[i] == '\n' || text[i] == '\t')
            text[i] = ' ';
    while (n && text[n - 1] == ' ')
        n--;
    text[n] = 0;

    /* word-wrap into lines of at most WIDTH characters */
    char lines[16][WIDTH + 1];
    int nl = 0, len = 0, maxw = 0;
    char *p = text;
    while (*p && nl < 16) {
        while (*p == ' ')
            p++;
        char *w = p;
        while (*p && *p != ' ')
            p++;
        int wl = p - w;
        if (!wl)
            break;
        if (wl > WIDTH)
            wl = WIDTH;
        if (len && len + 1 + wl > WIDTH) {
            lines[nl][len] = 0;
            if (len > maxw) maxw = len;
            nl++;
            len = 0;
            if (nl == 16)
                break;
        }
        if (len)
            lines[nl][len++] = ' ';
        memcpy(lines[nl] + len, w, wl);
        len += wl;
    }
    if (nl < 16 && len) {
        lines[nl][len] = 0;
        if (len > maxw) maxw = len;
        nl++;
    }
    if (!nl) {
        strcpy(lines[0], "moo");
        maxw = 3;
        nl = 1;
    }

    printf(" ");
    for (int i = 0; i < maxw + 2; i++) printf("_");
    printf("\n");
    for (int i = 0; i < nl; i++) {
        char open = nl == 1 ? '<' : i == 0 ? '/' : i == nl - 1 ? '\\' : '|';
        char close = nl == 1 ? '>' : i == 0 ? '\\' : i == nl - 1 ? '/' : '|';
        printf("%c %-*s %c\n", open, maxw, lines[i], close);
    }
    printf(" ");
    for (int i = 0; i < maxw + 2; i++) printf("-");
    printf("\n");
    puts("        \\   ^__^");
    puts("         \\  (oo)\\_______");
    puts("            (__)\\       )\\/\\");
    puts("                ||----w |");
    puts("                ||     ||");
    return 0;
}
