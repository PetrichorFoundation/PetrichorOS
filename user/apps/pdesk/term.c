/* Terminal: runs /bin/sh on a pair of pipes.  There is no pty, so this app
 * does the line editing and echo that the kernel tty would normally do. */
#include "desk.h"

#define TCOLS  120
#define TLINES 400

struct term {
    struct win *win;
    u8 ch[TLINES][TCOLS];
    u8 col[TLINES][TCOLS];
    int cols;                    /* wrap width, follows the window */
    u32 line;                    /* absolute index of the cursor's line */
    int cx;
    int scroll;                  /* lines scrolled back from the bottom */
    int pid, wfd, rfd, alive;
    char inbuf[256];
    int inlen;
    int fg, bold;
    int esc, ep[4], np;
};

static const u32 palette[16] = {
    RGB(0x2e, 0x34, 0x36), RGB(0xcc, 0x44, 0x44), RGB(0x4e, 0xc9, 0x4e), RGB(0xd8, 0xb0, 0x30),
    RGB(0x4a, 0x80, 0xd8), RGB(0xb0, 0x60, 0xc8), RGB(0x38, 0xb8, 0xc0), RGB(0xd3, 0xd7, 0xcf),
    RGB(0x6a, 0x72, 0x78), RGB(0xff, 0x6a, 0x6a), RGB(0x7a, 0xee, 0x7a), RGB(0xff, 0xdc, 0x5a),
    RGB(0x78, 0xa8, 0xff), RGB(0xd8, 0x90, 0xee), RGB(0x6a, 0xe0, 0xe8), RGB(0xff, 0xff, 0xff),
};
static const int ansi2idx[8] = {0, 1, 2, 3, 4, 5, 6, 7};

static void clear_line(struct term *t, u32 n)
{
    memset(t->ch[n % TLINES], ' ', TCOLS);
    memset(t->col[n % TLINES], 7, TCOLS);
}

static void clear_all(struct term *t)
{
    for (int i = 0; i < TLINES; i++)
        clear_line(t, i);
    t->line = 0;
    t->cx = 0;
    t->scroll = 0;
}

static void newline(struct term *t)
{
    t->line++;
    t->cx = 0;
    clear_line(t, t->line);
}

static void sgr(struct term *t, int code)
{
    if (code == 0) { t->fg = 7; t->bold = 0; }
    else if (code == 1) t->bold = 1;
    else if (code == 22) t->bold = 0;
    else if (code >= 30 && code <= 37) t->fg = ansi2idx[code - 30];
    else if (code == 39) t->fg = 7;
    else if (code >= 90 && code <= 97) { t->fg = ansi2idx[code - 90]; t->bold = 1; }
}

static void put(struct term *t, unsigned char c)
{
    if (t->esc == 1) {
        if (c == '[') { t->esc = 2; t->np = 0; memset(t->ep, 0, sizeof t->ep); }
        else t->esc = 0;
        return;
    }
    if (t->esc == 2) {
        if (c >= '0' && c <= '9') {
            t->ep[t->np] = t->ep[t->np] * 10 + (c - '0');
        } else if (c == ';') {
            if (t->np < 3) t->np++;
        } else {
            if (c == 'm') {
                for (int i = 0; i <= t->np; i++) sgr(t, t->ep[i]);
            } else if (c == 'J' && (t->ep[0] == 2 || t->ep[0] == 3)) {
                clear_all(t);
            } else if (c == 'K') {
                for (int i = t->cx; i < TCOLS; i++) t->ch[t->line % TLINES][i] = ' ';
            } else if (c == 'H' || c == 'f') {
                t->cx = 0;
            }
            t->esc = 0;
        }
        return;
    }
    switch (c) {
    case 033: t->esc = 1; return;
    case '\n': newline(t); return;
    case '\r': t->cx = 0; return;
    case '\b': if (t->cx > 0) t->cx--; return;
    case '\t': t->cx = (t->cx + 8) & ~7; if (t->cx >= t->cols) newline(t); return;
    }
    if (c < 32 || c > 126)
        return;
    if (t->cx >= t->cols)
        newline(t);
    t->ch[t->line % TLINES][t->cx] = c;
    t->col[t->line % TLINES][t->cx] = t->fg + (t->bold ? 8 : 0);
    t->cx++;
}

static void puts_t(struct term *t, const char *s)
{
    while (*s)
        put(t, *s++);
}

/* ---------------- drawing ---------------- */

static void term_draw(struct win *w, struct rect c)
{
    struct term *t = w->data;
    gfx_fill(c.x, c.y, c.w, c.h, C_TERM_BG);
    int cols = (c.w - 8) / 8, rows = (c.h - 4) / 16;
    if (cols > TCOLS) cols = TCOLS;
    if (cols < 20) cols = 20;
    if (rows < 1) rows = 1;
    t->cols = cols;

    long oldest = t->line >= TLINES ? (long)t->line - TLINES + 1 : 0;
    long top = (long)t->line - rows + 1 - t->scroll;
    if (top < oldest) {
        top = oldest;
        t->scroll = (long)t->line - rows + 1 - top;
        if (t->scroll < 0) t->scroll = 0;
    }
    for (int r = 0; r < rows; r++) {
        long idx = top + r;
        if (idx > (long)t->line)
            break;
        const u8 *chs = t->ch[idx % TLINES], *cs = t->col[idx % TLINES];
        for (int x = 0; x < cols; x++)
            if (chs[x] != ' ')
                gfx_char(c.x + 4 + x * 8, c.y + 2 + r * 16, chs[x], palette[cs[x] & 15]);
    }
    if (t->scroll == 0 && t->alive) {                      /* cursor block */
        long r = (long)t->line - top;
        if (r >= 0 && r < rows && t->cx < cols) {
            int px = c.x + 4 + t->cx * 8, py = c.y + 2 + r * 16;
            gfx_fill(px, py + 13, 8, 2, C_TERM_FG);
        }
    }
    if (t->scroll > 0)
        gfx_text(c.x + c.w - 150, c.y + 2, "[scrolled back]", RGB(0xd8, 0xb0, 0x30));
}

/* ---------------- process plumbing ---------------- */

/* Kills everything the shell started, but not the shell itself. */
static void kill_children(struct term *t)
{
    struct procinfo pi[64];
    int n = ps(pi, 64);
    int mark[64] = {0};
    for (int pass = 0; pass < 4; pass++)
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
                if (pi[i].ppid == t->pid || (mark[j] && pi[i].ppid == pi[j].pid))
                    mark[i] = 1;
    for (int i = 0; i < n; i++)
        if (mark[i] && pi[i].pid > 1)
            kill(pi[i].pid, 9);
}

static void term_close(struct win *w)
{
    struct term *t = w->data;
    if (t->alive) {
        kill_children(t);
        kill(t->pid, 9);
    }
    if (t->wfd >= 0) close(t->wfd);
    if (t->rfd >= 0) close(t->rfd);
    free(t);
}

static int term_pollfd(struct win *w, struct pollfd *p)
{
    struct term *t = w->data;
    if (!t->alive)
        return 0;
    *p = (struct pollfd){t->rfd, POLLIN, 0};
    return 1;
}

static void term_fdready(struct win *w, struct pollfd *p)
{
    struct term *t = w->data;
    char buf[512];
    int n = read(t->rfd, buf, sizeof buf);
    if (n > 0) {
        for (int i = 0; i < n; i++)
            put(t, buf[i]);
        t->scroll = 0;
    } else {
        t->alive = 0;
        t->fg = 3;
        if (t->cx) put(t, '\n');
        puts_t(t, "[shell exited - close this window]\n");
        t->fg = 7;
        wm_set_title(w, "Terminal (exited)");
    }
    wm_invalidate_rect(w->x, w->y + TITLE_H, w->w, w->h - TITLE_H);
}

static void term_key(struct win *w, struct input_event *e)
{
    struct term *t = w->data;
    struct rect c = wm_client(w);
    int rows = (c.h - 4) / 16;
    int ctrl = e->flags & IEF_CTRL;

    if (e->code == KEY_PGUP) { t->scroll += rows / 2; }
    else if (e->code == KEY_PGDN) { t->scroll -= rows / 2; if (t->scroll < 0) t->scroll = 0; }
    else if (!t->alive) { return; }
    else if (ctrl && (e->code == 'c' || e->code == 'C')) {
        puts_t(t, "^C\n");
        t->inlen = 0;
        kill_children(t);
        t->scroll = 0;
    } else if (ctrl && (e->code == 'd' || e->code == 'D')) {
        if (t->inlen == 0) {
            close(t->wfd);                                 /* EOF to the shell */
            t->wfd = -1;
        }
    } else if (ctrl && (e->code == 'l' || e->code == 'L')) {
        clear_all(t);
        write(t->wfd, "\n", 1);
    } else if (ctrl) {
        return;
    } else if (e->code == '\n') {
        t->scroll = 0;
        put(t, '\n');
        if (t->wfd >= 0) {
            write(t->wfd, t->inbuf, t->inlen);
            write(t->wfd, "\n", 1);
        }
        t->inlen = 0;
    } else if (e->code == 8) {
        if (t->inlen) {
            t->inlen--;
            put(t, '\b'); put(t, ' '); put(t, '\b');
        }
    } else if (e->code >= 32 && e->code < 127 && t->inlen < (int)sizeof t->inbuf - 1) {
        t->scroll = 0;
        t->inbuf[t->inlen++] = e->code;
        put(t, e->code);
    } else {
        return;
    }
    wm_invalidate_rect(w->x, w->y + TITLE_H, w->w, w->h - TITLE_H);
}

static const struct app term_app = {
    .draw = term_draw,
    .key = term_key,
    .pollfd = term_pollfd,
    .fdready = term_fdready,
    .close = term_close,
};

void term_open(const char *initial_command)
{
    struct term *t = calloc(1, sizeof *t);
    if (!t)
        return;
    clear_all(t);
    t->fg = 7;
    t->cols = 80;
    t->wfd = t->rfd = -1;

    int in[2], out[2];
    if (pipe(in) < 0 || pipe(out) < 0) {
        free(t);
        return;
    }
    int pid = fork();
    if (pid < 0) {
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        free(t);
        return;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        for (int fd = 3; fd < NOFILE_USER; fd++)       /* don't leak other windows' pipes */
            close(fd);
        char *argv[] = {"sh", NULL};
        exec("/bin/sh", argv);
        exit(127);
    }
    close(in[0]);
    close(out[1]);
    t->pid = pid;
    t->wfd = in[1];
    t->rfd = out[0];
    t->alive = 1;

    t->win = wm_open(&term_app, 656, 424, "Terminal", t);
    if (!t->win) {
        kill(pid, 9);
        close(t->wfd);
        close(t->rfd);
        free(t);
        return;
    }
    if (initial_command) {
        puts_t(t, initial_command);
        put(t, '\n');
        write(t->wfd, initial_command, strlen(initial_command));
        write(t->wfd, "\n", 1);
    }
}
