/* Software: a graphical front-end for ppm.  It reads the package index and the
 * installed-package database from /disk/var/ppm and runs `ppm` for the actual work. */
#include "desk.h"

#define TOOL_H   36
#define HEADER_H 20
#define ROW_H    22
#define LOG_H    104

struct pkg {
    char name[32], version[16], desc[96];
    int installed, outdated;
};

struct sw {
    struct win *win;
    struct pkg *pk;
    int n, sel, top;
    int busy, rfd;
    char action[32];
    char log[6][96];
    int nlog;
    char linebuf[96];
    int ll;
    int failed;
};

static void log_line(struct sw *s, const char *text)
{
    if (s->nlog == 6) {
        memmove(s->log[0], s->log[1], 5 * sizeof s->log[0]);
        s->nlog = 5;
    }
    safestrcpy(s->log[s->nlog++], text, sizeof s->log[0]);
    if (!strncmp(text, "ppm:", 4))
        s->failed = 1;
}

static int field(char **p, char *dst, int max)
{
    char *tab = strchr(*p, '\t');
    if (!tab)
        return 0;
    *tab = 0;
    safestrcpy(dst, *p, max);
    *p = tab + 1;
    return 1;
}

static void reload(struct sw *s)
{
    free(s->pk);
    s->pk = NULL;
    s->n = 0;
    int fd = open("/disk/var/ppm/index", O_RDONLY);
    if (fd < 0)
        return;
    struct stat st;
    fstat(fd, &st);
    char *text = malloc(st.size + 1);
    int len = text ? read(fd, text, st.size) : 0;
    close(fd);
    if (len <= 0) {
        free(text);
        return;
    }
    text[len] = 0;
    s->pk = malloc(64 * sizeof *s->pk);
    for (char *l = text; *l && s->pk && s->n < 64;) {
        char *nl = strchr(l, '\n');
        if (nl)
            *nl = 0;
        char size[16], sha[65], deps[128], file[96], *p = l;
        struct pkg *k = &s->pk[s->n];
        if (field(&p, k->name, sizeof k->name) && field(&p, k->version, sizeof k->version) &&
            field(&p, size, sizeof size) && field(&p, sha, sizeof sha) && field(&p, deps, sizeof deps) &&
            field(&p, file, sizeof file)) {
            safestrcpy(k->desc, p, sizeof k->desc);
            char path[96], ver[32] = "";
            snprintf(path, sizeof path, "/disk/var/ppm/db/%s", k->name);
            int d = open(path, O_RDONLY);
            k->installed = d >= 0;
            if (d >= 0) {
                int n = read(d, ver, sizeof ver - 1);
                ver[n > 0 ? n : 0] = 0;
                char *nlp = strchr(ver, '\n');
                if (nlp) *nlp = 0;
                close(d);
            }
            k->outdated = k->installed && strcmp(ver, k->version);
            s->n++;
        }
        if (!nl)
            break;
        l = nl + 1;
    }
    free(text);
    if (s->sel >= s->n)
        s->sel = s->n - 1;
}

static void start(struct sw *s, const char *label, const char *a1, const char *a2)
{
    if (s->busy)
        return;
    struct diskinfo di;
    diskinfo(&di);
    if (!di.mounted) {
        wm_notify("Packages need a disk mounted at /disk");
        return;
    }
    int fds[2];
    if (pipe(fds) < 0)
        return;
    int pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        for (int fd = 3; fd < NOFILE_USER; fd++)
            close(fd);
        char *argv[] = {"ppm", (char *)a1, (char *)a2, NULL};
        exec("/bin/ppm", argv);
        exit(127);
    }
    close(fds[1]);
    s->rfd = fds[0];
    s->busy = 1;
    s->failed = 0;
    s->ll = 0;
    safestrcpy(s->action, label, sizeof s->action);
    s->nlog = 0;
    char t[96];
    snprintf(t, sizeof t, "$ ppm %s%s%s", a1, a2 ? " " : "", a2 ? a2 : "");
    log_line(s, t);
    wm_invalidate(s->win);
}

static int sw_pollfd(struct win *w, struct pollfd *p)
{
    struct sw *s = w->data;
    if (!s->busy)
        return 0;
    *p = (struct pollfd){s->rfd, POLLIN, 0};
    return 1;
}

static void sw_fdready(struct win *w, struct pollfd *p)
{
    struct sw *s = w->data;
    char buf[256];
    int n = read(s->rfd, buf, sizeof buf);
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n' || s->ll == (int)sizeof s->linebuf - 1) {
                s->linebuf[s->ll] = 0;
                log_line(s, s->linebuf);
                s->ll = 0;
            } else if (buf[i] != '\r') {
                s->linebuf[s->ll++] = buf[i];
            }
        }
    } else {
        close(s->rfd);
        s->busy = 0;
        reload(s);
        char t[64];
        snprintf(t, sizeof t, "%s %s", s->action, s->failed ? "failed" : "finished");
        wm_notify(t);
    }
    wm_invalidate(w);
}

static int visible_rows(struct rect c)
{
    return (c.h - TOOL_H - HEADER_H - LOG_H - 22) / ROW_H;
}

static void sw_draw(struct win *w, struct rect c)
{
    struct sw *s = w->data;
    gfx_vgrad(c.x, c.y, c.w, TOOL_H, RGB(0xf2, 0xf3, 0xf4), RGB(0xdc, 0xdf, 0xe2));
    ui_button(c.x + 8, c.y + 6, 80, 24, "Refresh", s->busy);
    int has = s->n && s->sel >= 0 && s->sel < s->n;
    const char *act = !has ? "Install" : s->pk[s->sel].outdated ? "Upgrade" : s->pk[s->sel].installed ? "Remove" : "Install";
    ui_button(c.x + 96, c.y + 6, 80, 24, act, s->busy || !has);
    ui_button(c.x + 184, c.y + 6, 110, 24, "Upgrade all", s->busy);
    gfx_text(c.x + 308, c.y + 10, s->busy ? "Working..." : "", C_ACCENT);

    int ly = c.y + TOOL_H;
    gfx_fill(c.x, ly, c.w, HEADER_H, RGB(0xd0, 0xd4, 0xd8));
    gfx_text(c.x + 10, ly + 2, "Package", C_TEXT);
    gfx_text(c.x + 150, ly + 2, "Version", C_TEXT);
    gfx_text(c.x + 230, ly + 2, "Status", C_TEXT);
    gfx_text(c.x + 330, ly + 2, "Description", C_TEXT);
    int rows = visible_rows(c), ry = ly + HEADER_H;
    gfx_fill(c.x, ry, c.w, rows * ROW_H, C_WHITE);
    if (!s->n) {
        gfx_text(c.x + 16, ry + 16, "No package list yet. Click Refresh to download it", C_TEXT_DIM);
        gfx_text(c.x + 16, ry + 36, "from the repository (see 'ppm repo').", C_TEXT_DIM);
    }
    for (int r = 0; r < rows; r++) {
        int i = s->top + r;
        if (i >= s->n)
            break;
        struct pkg *k = &s->pk[i];
        int y = ry + r * ROW_H;
        if (i == s->sel)
            gfx_fill(c.x, y, c.w, ROW_H, g_accent);
        else if (r & 1)
            gfx_fill(c.x, y, c.w, ROW_H, RGB(0xf5, 0xf7, 0xf9));
        u32 col = i == s->sel ? C_WHITE : C_TEXT;
        icon_draw(ICON_SOFTWARE, c.x + 8, y + 1, 20);
        gfx_text(c.x + 34, y + 3, k->name, col);
        gfx_text(c.x + 150, y + 3, k->version, col);
        gfx_text(c.x + 230, y + 3, k->outdated ? "update available" : k->installed ? "installed" : "-",
                 i == s->sel ? C_WHITE : (k->installed ? RGB(0x2a, 0x9a, 0x4a) : C_TEXT_DIM));
        gfx_text(c.x + 330, y + 3, k->desc, col);
    }
    int gy = c.y + c.h - LOG_H - 22;
    gfx_fill(c.x, gy, c.w, LOG_H + 22, C_TERM_BG);
    for (int i = 0; i < s->nlog; i++)
        gfx_text(c.x + 8, gy + 4 + i * 16, s->log[i], !strncmp(s->log[i], "ppm:", 4) ? RGB(0xff, 0x6a, 0x6a) : C_TERM_FG);
    if (!s->nlog)
        gfx_text(c.x + 8, gy + 4, "Output from ppm appears here.", RGB(0x6a, 0x72, 0x78));
}

static void sw_mouse(struct win *w, int x, int y, int kind)
{
    struct sw *s = w->data;
    if (kind != M_DOWN && kind != M_DBL)
        return;
    struct rect c = wm_client(w);
    if (y < TOOL_H) {
        if (y < 6 || y >= 30 || s->busy)
            return;
        if (x >= 8 && x < 88) {
            start(s, "Refresh", "update", NULL);
        } else if (x >= 96 && x < 176 && s->sel >= 0 && s->sel < s->n) {
            struct pkg *k = &s->pk[s->sel];
            if (k->outdated)
                start(s, "Upgrade", "upgrade", NULL);
            else if (k->installed)
                start(s, "Remove", "remove", k->name);
            else
                start(s, "Install", "install", k->name);
        } else if (x >= 184 && x < 294) {
            start(s, "Upgrade", "upgrade", NULL);
        }
        return;
    }
    int ry = TOOL_H + HEADER_H;
    if (y >= ry && (y - ry) / ROW_H < visible_rows(c)) {
        int i = s->top + (y - ry) / ROW_H;
        if (i < s->n) {
            s->sel = i;
            wm_invalidate(w);
        }
    }
}

static void sw_wheel(struct win *w, int dir)
{
    struct sw *s = w->data;
    int rows = visible_rows(wm_client(w));
    s->top += dir * 3;
    if (s->top > s->n - rows) s->top = s->n - rows;
    if (s->top < 0) s->top = 0;
    wm_invalidate(w);
}

static void sw_key(struct win *w, struct input_event *e)
{
    struct sw *s = w->data;
    if (e->code == KEY_UP && s->sel > 0) s->sel--;
    else if (e->code == KEY_DOWN && s->sel < s->n - 1) s->sel++;
    else return;
    int rows = visible_rows(wm_client(w));
    if (s->sel < s->top) s->top = s->sel;
    if (s->sel >= s->top + rows) s->top = s->sel - rows + 1;
    wm_invalidate(w);
}

static void sw_close(struct win *w)
{
    struct sw *s = w->data;
    if (s->busy)
        close(s->rfd);
    free(s->pk);
    free(s);
}

static const struct app sw_app = {.draw = sw_draw, .key = sw_key, .mouse = sw_mouse, .wheel = sw_wheel,
                                  .pollfd = sw_pollfd, .fdready = sw_fdready, .close = sw_close};

void software_open(void)
{
    struct sw *s = calloc(1, sizeof *s);
    if (!s)
        return;
    s->sel = -1;
    s->win = wm_open(&sw_app, 700, 480, "Software", s);
    if (!s->win) {
        free(s);
        return;
    }
    s->win->minw = 520;
    s->win->minh = 340;
    reload(s);
}
