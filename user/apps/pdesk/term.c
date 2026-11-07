/* Terminal: runs /bin/sh on a pair of pipes.  There is no pty, so this app
 * does the line editing and echo that the kernel tty would normally do. */
#include "desk.h"
#include "winproto.h"

#define TCOLS  120
#define TLINES 400

#define MAXCW 8

struct term;

struct cwin {                                    /* a window opened by a Windows program (GrapeJuice) */
    struct term *t;
    int id, w, h;
    struct win *win;
    u32 *fb;
    u32 close_asked;
};

struct term {
    struct win *win;
    int upfd, downfd;                            /* window protocol: messages from programs, events to them */
    u8 ibuf[8192];
    int ilen;
    struct cwin *cw[MAXCW];
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
    long view_top;               /* absolute line shown in the first row (for mouse selection) */
    long sel_a, sel_b;           /* selection: absolute positions line * TCOLS + col, half-open; equal = none */
    int selecting;
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
    t->view_top = top;
    for (int r = 0; r < rows; r++) {
        long idx = top + r;
        if (idx > (long)t->line)
            break;
        const u8 *chs = t->ch[idx % TLINES], *cs = t->col[idx % TLINES];
        long lo = t->sel_a < t->sel_b ? t->sel_a : t->sel_b, hi = t->sel_a < t->sel_b ? t->sel_b : t->sel_a;
        for (int x = 0; x < cols; x++) {
            long p = idx * TCOLS + x;
            if (p >= lo && p < hi)
                gfx_fill(c.x + 4 + x * 8, c.y + 2 + r * 16, 8, 16, RGB(0x3a, 0x5a, 0x80));
        }
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

/* ---------------- windows of Windows programs ---------------- */

static int send_event(struct term *t, u32 type, u32 id, u32 a, u32 b, u32 c)
{
    if (t->downfd < 0)
        return -1;
    struct pollfd p = {t->downfd, POLLOUT, 0};
    if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLOUT))
        return -1;                               /* the program isn't reading: drop it rather than block the desktop */
    struct pw_event e = {type, id, a, b, c, 0};
    return write(t->downfd, &e, sizeof e) == (int)sizeof e ? 0 : -1;
}

static void cw_draw(struct win *w, struct rect c)
{
    struct cwin *cw = w->data;
    gfx_fill(c.x, c.y, c.w, c.h, RGB(0x30, 0x30, 0x30));
    if (cw->fb)
        gfx_blit_rgb(cw->fb, c.x, c.y, cw->w, cw->h);
}

static void cw_mouse(struct win *w, int x, int y, int kind)
{
    struct cwin *cw = w->data;
    u32 k = kind == M_DOWN ? 1 : kind == M_UP ? 2 : kind == M_RDOWN ? 3 : kind == M_DBL ? 4 : 0;
    if (kind == M_MOVE && 0)
        return;
    send_event(cw->t, PE_MOUSE, cw->id, x < 0 ? 0 : x, y < 0 ? 0 : y, k);
}

static void cw_key(struct win *w, struct input_event *e)
{
    struct cwin *cw = w->data;
    send_event(cw->t, PE_KEY, cw->id, e->code, e->flags, 0);
}

static void cw_wheel(struct win *w, int dir)
{
    struct cwin *cw = w->data;
    send_event(cw->t, PE_WHEEL, cw->id, dir, 0, 0);
}

static void cw_close(struct win *w)
{
    struct cwin *cw = w->data;
    for (int i = 0; i < MAXCW; i++)
        if (cw->t->cw[i] == cw)
            cw->t->cw[i] = NULL;
    free(cw->fb);
    free(cw);
}

static int cw_request_close(struct win *w)
{
    struct cwin *cw = w->data;
    u32 now = uptime();
    if (cw->close_asked && now - cw->close_asked < 300)
        return 0;                                /* asked twice within 3 seconds: the program is stuck, force it */
    if (send_event(cw->t, PE_CLOSE, cw->id, 0, 0, 0) < 0)
        return 0;
    cw->close_asked = now ? now : 1;
    return 1;
}

static const struct app cwin_app = {
    .draw = cw_draw,
    .mouse = cw_mouse,
    .key = cw_key,
    .wheel = cw_wheel,
    .close = cw_close,
    .request_close = cw_request_close,
};

static struct cwin *cw_find(struct term *t, u32 id)
{
    for (int i = 0; i < MAXCW; i++)
        if (t->cw[i] && t->cw[i]->id == (int)id)
            return t->cw[i];
    return NULL;
}

static void cw_handle(struct term *t, const struct pw_msg *m, const u8 *payload)
{
    struct cwin *cw = cw_find(t, m->id);
    switch (m->type) {
    case PW_CREATE: {
        if (cw || m->a < 1 || m->b < 1)
            return;
        int slot = -1;
        for (int i = 0; i < MAXCW; i++)
            if (!t->cw[i]) { slot = i; break; }
        if (slot < 0)
            return;
        struct cwin *n = calloc(1, sizeof *n);
        if (!n)
            return;
        n->t = t;
        n->id = m->id;
        n->w = m->a > PW_MAX_W ? PW_MAX_W : m->a;
        n->h = m->b > PW_MAX_H ? PW_MAX_H : m->b;
        n->fb = malloc(n->w * n->h * 4);
        if (!n->fb) { free(n); return; }
        for (int i = 0; i < n->w * n->h; i++)
            n->fb[i] = 0xFFFFFF;
        char title[64];
        int tl = m->len < 63 ? m->len : 63;
        memcpy(title, payload, tl);
        title[tl] = 0;
        t->cw[slot] = n;
        n->win = wm_open(&cwin_app, n->w + 2, n->h + TITLE_H + 1, title[0] ? title : "Windows program", n);
        if (!n->win) {
            t->cw[slot] = NULL;
            free(n->fb);
            free(n);
        }
        break;
    }
    case PW_PRESENT:
        if (!cw || m->c == 0 || m->len < m->c * 4 || m->a >= (u32)cw->w || m->b >= (u32)cw->h)
            return;
        {
            u32 cnt = m->c;
            if (m->a + cnt > (u32)cw->w) cnt = cw->w - m->a;
            memcpy(cw->fb + m->b * cw->w + m->a, payload, cnt * 4);
            wm_invalidate_rect(cw->win->x + 1 + m->a, cw->win->y + TITLE_H + m->b, cnt, 1);
        }
        break;
    case PW_TITLE:
        if (cw && cw->win) {
            char title[64];
            int tl = m->len < 63 ? m->len : 63;
            memcpy(title, payload, tl);
            title[tl] = 0;
            wm_set_title(cw->win, title);
        }
        break;
    case PW_DESTROY:
        if (cw && cw->win)
            wm_close(cw->win);
        break;
    }
}

/* Reads whatever the programs sent and acts on each complete message. */
static void cw_read(struct term *t)
{
    int n = read(t->upfd, t->ibuf + t->ilen, sizeof t->ibuf - t->ilen);
    if (n <= 0) {
        close(t->upfd);
        t->upfd = -1;
        return;
    }
    t->ilen += n;
    int pos = 0;
    while (t->ilen - pos >= (int)sizeof(struct pw_msg)) {
        struct pw_msg m;
        memcpy(&m, t->ibuf + pos, sizeof m);
        if (m.len > 4096) {                       /* garbage: drop the whole buffer */
            pos = t->ilen;
            break;
        }
        if (t->ilen - pos < (int)(sizeof m + m.len))
            break;
        cw_handle(t, &m, t->ibuf + pos + sizeof m);
        pos += sizeof m + m.len;
    }
    memmove(t->ibuf, t->ibuf + pos, t->ilen - pos);
    t->ilen -= pos;
}

static void cw_close_all(struct term *t)
{
    for (int i = 0; i < MAXCW; i++)
        if (t->cw[i] && t->cw[i]->win)
            wm_close(t->cw[i]->win);
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
    cw_close_all(t);
    if (t->upfd >= 0) close(t->upfd);
    if (t->downfd >= 0) close(t->downfd);
    if (t->alive) {
        kill_children(t);
        kill(t->pid, 9);
    }
    if (t->wfd >= 0) close(t->wfd);
    if (t->rfd >= 0) close(t->rfd);
    free(t);
}

static int term_pollfds(struct win *w, struct pollfd *p, int max)
{
    struct term *t = w->data;
    int n = 0;
    if (t->alive && n < max)
        p[n++] = (struct pollfd){t->rfd, POLLIN, 0};
    if (t->upfd >= 0 && n < max)
        p[n++] = (struct pollfd){t->upfd, POLLIN, 0};
    return n;
}

static void term_fdready(struct win *w, struct pollfd *p)
{
    struct term *t = w->data;
    if (p->fd == t->upfd) {
        cw_read(t);
        return;
    }
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

static void paste_text(struct win *w, struct term *t, const char *s, int n);
static void copy_selection(struct term *t);

static void term_key(struct win *w, struct input_event *e)
{
    struct term *t = w->data;
    struct rect c = wm_client(w);
    int rows = (c.h - 4) / 16;
    int ctrl = e->flags & IEF_CTRL;

    if (ctrl && (e->flags & IEF_SHIFT) && (e->code == 'c' || e->code == 'C')) {   /* Ctrl+Shift+C/V: copy, paste */
        copy_selection(t);
        return;
    }
    if (ctrl && (e->flags & IEF_SHIFT) && (e->code == 'v' || e->code == 'V')) {
        int n;
        const char *c = clip_get(&n);
        if (c && t->alive)
            paste_text(w, t, c, n);
        return;
    }
    if (e->code >= 32 && e->code < 127)
        t->sel_a = t->sel_b = 0;                            /* typing clears the highlight */
    if (e->code == KEY_PGUP) { t->scroll += rows / 2; }
    else if (e->code == KEY_PGDN) { t->scroll -= rows / 2; if (t->scroll < 0) t->scroll = 0; }
    else if (!t->alive) { return; }
    else if (ctrl && (e->code == 'c' || e->code == 'C')) {
        puts_t(t, "^C\n");
        t->inlen = 0;
        kill_children(t);
        cw_close_all(t);
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

static void paste_text(struct win *w, struct term *t, const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        struct input_event e = {.type = INPUT_KEY, .flags = IEF_DOWN, .code = (u8)s[i] == '\r' ? 0 : (u8)s[i]};
        if (s[i] == '\t')
            e.code = ' ';
        if (e.code == '\n' || (e.code >= 32 && e.code < 127))
            term_key(w, &e);
    }
}

static void copy_selection(struct term *t)
{
    long lo = t->sel_a < t->sel_b ? t->sel_a : t->sel_b, hi = t->sel_a < t->sel_b ? t->sel_b : t->sel_a;
    if (lo == hi)
        return;
    static char out[TLINES * (TCOLS + 1)];
    int n = 0;
    for (long l = lo / TCOLS; l <= (hi - 1) / TCOLS && n < (int)sizeof out - TCOLS - 2; l++) {
        if (l > (long)t->line || l < (long)t->line - TLINES + 1)
            continue;
        int c0 = l == lo / TCOLS ? lo % TCOLS : 0, c1 = l == (hi - 1) / TCOLS ? (hi - 1) % TCOLS + 1 : t->cols;
        int start = n;
        for (int x = c0; x < c1; x++)
            out[n++] = t->ch[l % TLINES][x];
        while (n > start && out[n - 1] == ' ')
            n--;                                            /* no trailing blanks */
        if (l != (hi - 1) / TCOLS)
            out[n++] = '\n';
    }
    clip_set(out, n);
}

static void term_mouse(struct win *w, int x, int y, int kind)
{
    struct term *t = w->data;
    int col = (x - 4) / 8, row = (y - 2) / 16;
    if (col < 0) col = 0;
    if (col > t->cols) col = t->cols;
    if (row < 0) row = 0;
    long p = (t->view_top + row) * TCOLS + col;
    if (kind == M_MDOWN) {
        int n;
        const char *c = clip_get(&n);
        if (c && t->alive)
            paste_text(w, t, c, n);
        return;
    }
    if (kind == M_DOWN) {
        t->sel_a = t->sel_b = p;
        t->selecting = 1;
    } else if (kind == M_DRAG && t->selecting) {
        t->sel_b = p;
    } else if (kind == M_UP && t->selecting) {
        t->selecting = 0;
        copy_selection(t);                                  /* like X11: selecting copies */
    } else {
        return;
    }
    wm_invalidate_rect(w->x, w->y + TITLE_H, w->w, w->h - TITLE_H);
}

static void term_wheel(struct win *w, int dir)
{
    struct term *t = w->data;
    t->scroll -= dir * 3;
    if (t->scroll < 0)
        t->scroll = 0;
    wm_invalidate_rect(w->x, w->y + TITLE_H, w->w, w->h - TITLE_H);
}

static const struct app term_app = {
    .wheel = term_wheel,
    .mouse = term_mouse,
    .draw = term_draw,
    .key = term_key,
    .pollfds = term_pollfds,
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
    t->wfd = t->rfd = t->upfd = t->downfd = -1;

    int in[2], out[2], up[2], down[2];
    if (pipe(in) < 0 || pipe(out) < 0 || pipe(up) < 0 || pipe(down) < 0) {
        free(t);
        return;
    }
    int pid = fork();
    if (pid < 0) {
        close(in[0]); close(in[1]); close(out[0]); close(out[1]); close(up[0]); close(up[1]); close(down[0]); close(down[1]);
        free(t);
        return;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(up[1], NOFILE_USER - 1);                    /* park the window-protocol pipes out of the way... */
        dup2(down[0], NOFILE_USER - 2);
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        for (int fd = 3; fd < NOFILE_USER - 2; fd++)     /* don't leak other windows' pipes */
            close(fd);
        dup2(NOFILE_USER - 1, PW_FD_UP);                 /* ...and put them at fds 3 and 4 for the shell's programs */
        dup2(NOFILE_USER - 2, PW_FD_DOWN);
        close(NOFILE_USER - 1);
        close(NOFILE_USER - 2);
        char *argv[] = {"sh", NULL};
        exec("/bin/sh", argv);
        exit(127);
    }
    close(in[0]);
    close(out[1]);
    close(up[1]);
    close(down[0]);
    t->upfd = up[0];
    t->downfd = down[1];
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
