/* Windows of separate programs that draw into shared memory (see winproto.h), and launching such programs.
 * The desktop maps each window's buffer and copies the changed parts to the screen; input goes back as events. */
#include "desk.h"
#include "winproto.h"
#include "host.h"

#define MAXHOSTS 8

struct hwin {
    struct host *h;
    int id, w, hgt, shm;
    u32 *px;
    struct win *win;
    u32 close_asked;
    int req_w, req_h;                           /* the size we last asked the program for */
};

static int send_ev(struct host *h, u32 type, u32 id, u32 a, u32 b, u32 c)
{
    if (h->down < 0)
        return -1;
    struct pollfd p = {h->down, POLLOUT, 0};
    if (poll(&p, 1, 0) <= 0)
        return -1;                              /* the program isn't reading: drop the event rather than hang */
    struct pw_event e = {type, id, a, b, c, 0};
    return write(h->down, &e, sizeof e) == (int)sizeof e ? 0 : -1;
}

static void hw_draw(struct win *w, struct rect c)
{
    struct hwin *hw = w->data;
    int cw = w->w - 2, ch = w->h - TITLE_H - 1;
    if (hw->px) {
        int bw = hw->w < cw ? hw->w : cw, bh = hw->hgt < ch ? hw->hgt : ch;
        struct rect saved = gfx_get_clip();
        struct rect area = rect_isect(saved, (struct rect){c.x, c.y, bw, bh});
        if (area.w > 0 && area.h > 0) {
            gfx_set_clip(area.x, area.y, area.w, area.h);
            gfx_blit_rgb(hw->px, c.x, c.y, hw->w, hw->hgt);
            gfx_set_clip(saved.x, saved.y, saved.w, saved.h);
        }
        if (bw < cw) gfx_fill(c.x + bw, c.y, cw - bw, ch, RGB(0xe8, 0xe8, 0xe8));
        if (bh < ch) gfx_fill(c.x, c.y + bh, bw, ch - bh, RGB(0xe8, 0xe8, 0xe8));
    } else {
        gfx_fill(c.x, c.y, c.w, c.h, RGB(0xe8, 0xe8, 0xe8));
    }
    if ((cw != hw->w || ch != hw->hgt) && (cw != hw->req_w || ch != hw->req_h) && cw > 0 && ch > 0) {
        hw->req_w = cw;                         /* the window changed size: ask for a matching buffer */
        hw->req_h = ch;
        send_ev(hw->h, PE_RESIZE, hw->id, cw, ch, 0);
    }
}

static void hw_mouse(struct win *w, int x, int y, int kind)
{
    struct hwin *hw = w->data;
    send_ev(hw->h, PE_POINTER, hw->id, x < 0 ? 0 : x, y < 0 ? 0 : y, kind);
}

static void hw_key(struct win *w, struct input_event *e)
{
    struct hwin *hw = w->data;
    send_ev(hw->h, PE_KEY, hw->id, e->code, e->flags, 0);
}

static void hw_wheel(struct win *w, int dir)
{
    struct hwin *hw = w->data;
    send_ev(hw->h, PE_WHEEL, hw->id, dir, 0, 0);
}

static void hw_close(struct win *w)
{
    struct hwin *hw = w->data;
    for (int i = 0; i < HOST_WINS; i++)
        if (hw->h->w[i] == hw)
            hw->h->w[i] = NULL;
    if (hw->shm > 0)
        shm(SHM_DROP, hw->shm);
    free(hw);
}

static int hw_request_close(struct win *w)
{
    struct hwin *hw = w->data;
    u32 now = uptime();
    if (hw->close_asked && now - hw->close_asked < 300) {
        if (hw->h->pid > 0)
            kill(hw->h->pid, 9);                /* asked twice within 3 seconds: the program is stuck */
        return 0;
    }
    if (send_ev(hw->h, PE_CLOSE, hw->id, 0, 0, 0) < 0)
        return 0;
    hw->close_asked = now ? now : 1;
    return 1;
}

static const struct app hwin_app = {
    .draw = hw_draw, .mouse = hw_mouse, .key = hw_key, .wheel = hw_wheel, .close = hw_close,
    .request_close = hw_request_close,
};

static struct hwin *find(struct host *h, u32 id)
{
    for (int i = 0; i < HOST_WINS; i++)
        if (h->w[i] && h->w[i]->id == (int)id)
            return h->w[i];
    return NULL;
}

static void copy_text(char *dst, int cap, const u8 *payload, u32 len)
{
    int n = (int)len < cap - 1 ? (int)len : cap - 1;
    memcpy(dst, payload, n);
    dst[n] = 0;
}

static u32 *map_buffer(int id, int w, int h)
{
    if (id <= 0 || w < 1 || h < 1 || w > MAX_SCREEN_W || h > MAX_SCREEN_H)
        return NULL;
    return shm(SHM_MAP, id) == 0 ? SHM_ADDR(id) : NULL;
}

int host_msg(struct host *h, const struct pw_msg *m, const u8 *payload)
{
    struct hwin *hw = find(h, m->id);
    switch (m->type) {
    case PW_SHM_WINDOW: {
        if (hw)
            return 1;
        int slot = -1;
        for (int i = 0; i < HOST_WINS; i++)
            if (!h->w[i]) { slot = i; break; }
        u32 *px = slot >= 0 ? map_buffer(m->c, m->a, m->b) : NULL;
        if (!px)
            return 1;
        struct hwin *n = calloc(1, sizeof *n);
        if (!n) { shm(SHM_DROP, m->c); return 1; }
        n->h = h;
        n->id = m->id;
        n->w = n->req_w = m->a;
        n->hgt = n->req_h = m->b;
        n->shm = m->c;
        n->px = px;
        char title[64];
        copy_text(title, sizeof title, payload, m->len);
        h->w[slot] = n;
        int ww = n->w + 2, wh = n->hgt + TITLE_H + 1;
        if (ww > SCREEN_W) ww = SCREEN_W;
        if (wh > SCREEN_H - PANEL_H) wh = SCREEN_H - PANEL_H;
        n->win = wm_open(&hwin_app, ww, wh, title[0] ? title : "Program", n);
        if (!n->win) {
            h->w[slot] = NULL;
            shm(SHM_DROP, n->shm);
            free(n);
            return 1;
        }
        n->win->minw = 200;
        n->win->minh = 120;
        return 1;
    }
    case PW_BUFFER: {
        if (!hw)
            return 1;
        u32 *px = map_buffer(m->c, m->a, m->b);
        if (!px)
            return 1;
        if (hw->shm > 0 && hw->shm != (int)m->c)
            shm(SHM_DROP, hw->shm);
        hw->shm = m->c;
        hw->px = px;
        hw->w = m->a;
        hw->hgt = m->b;
        wm_invalidate(hw->win);
        return 1;
    }
    case PW_DAMAGE:
        if (hw && hw->win) {
            int x = m->a, y = m->b, w = m->c, ht = m->d;
            if (w > hw->w) w = hw->w;
            if (ht > hw->hgt) ht = hw->hgt;
            wm_invalidate_rect(hw->win->x + 1 + x, hw->win->y + TITLE_H + y, w, ht);
        }
        return 1;
    case PW_NOTIFY: {
        char text[120];
        copy_text(text, sizeof text, payload, m->len);
        wm_notify(text);
        return 1;
    }
    case PW_TITLE:
        if (hw && hw->win) {
            char title[64];
            copy_text(title, sizeof title, payload, m->len);
            wm_set_title(hw->win, title);
            return 1;
        }
        return 0;
    case PW_DESTROY:
        if (hw && hw->win) {
            wm_close(hw->win);
            return 1;
        }
        return 0;
    }
    return 0;                                   /* not ours (the terminal handles the pipe-pixel kind) */
}

void host_close_all(struct host *h)
{
    for (int i = 0; i < HOST_WINS; i++)
        if (h->w[i] && h->w[i]->win)
            wm_close(h->w[i]->win);
}

/* ---------------- programs the desktop started itself ---------------- */

static struct launched {
    int used;
    struct host h;
    u8 ibuf[8192];
    int ilen;
} hosts[MAXHOSTS];

int app_launch(const char *path, const char *arg)
{
    struct stat st;
    if (stat(path, &st) < 0)
        return -ENOENT;
    int slot = -1;
    for (int i = 0; i < MAXHOSTS; i++)
        if (!hosts[i].used) { slot = i; break; }
    if (slot < 0)
        return -EAGAIN;
    int up[2], down[2];
    if (pipe(up) < 0)
        return -EMFILE;
    if (pipe(down) < 0) {
        close(up[0]); close(up[1]);
        return -EMFILE;
    }
    int pid = fork();
    if (pid < 0) {
        close(up[0]); close(up[1]); close(down[0]); close(down[1]);
        return pid;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(up[1], NOFILE_USER - 1);
        dup2(down[0], NOFILE_USER - 2);
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) { dup2(null, 0); close(null); }
        for (int fd = 3; fd < NOFILE_USER - 2; fd++)
            close(fd);
        dup2(NOFILE_USER - 1, PW_FD_UP);
        dup2(NOFILE_USER - 2, PW_FD_DOWN);
        close(NOFILE_USER - 1);
        close(NOFILE_USER - 2);
        const char *base = strrchr(path, '/');
        char *argv[] = {(char *)(base ? base + 1 : path), (char *)arg, NULL};
        exec(path, argv);
        exit(127);
    }
    close(up[1]);
    close(down[0]);
    struct launched *l = &hosts[slot];
    memset(l, 0, sizeof *l);
    l->used = 1;
    l->h.up = up[0];
    l->h.down = down[1];
    l->h.pid = pid;
    return 0;
}

int hosts_pollfds(struct pollfd *p, int max)
{
    int n = 0;
    for (int i = 0; i < MAXHOSTS && n < max; i++)
        if (hosts[i].used)
            p[n++] = (struct pollfd){hosts[i].h.up, POLLIN, 0};
    return n;
}

void hosts_fdready(struct pollfd *p)
{
    for (int i = 0; i < MAXHOSTS; i++) {
        struct launched *l = &hosts[i];
        if (!l->used || l->h.up != p->fd)
            continue;
        int n = read(l->h.up, l->ibuf + l->ilen, sizeof l->ibuf - l->ilen);
        if (n <= 0) {                                   /* the program exited */
            host_close_all(&l->h);
            close(l->h.up);
            close(l->h.down);
            l->used = 0;
            return;
        }
        l->ilen += n;
        int pos = 0;
        while (l->ilen - pos >= (int)sizeof(struct pw_msg)) {
            struct pw_msg m;
            memcpy(&m, l->ibuf + pos, sizeof m);
            if (m.len > 4096) { pos = l->ilen; break; }
            if (l->ilen - pos < (int)(sizeof m + m.len))
                break;
            host_msg(&l->h, &m, l->ibuf + pos + sizeof m);
            pos += sizeof m + m.len;
        }
        memmove(l->ibuf, l->ibuf + pos, l->ilen - pos);
        l->ilen -= pos;
        return;
    }
}

/* ---------------- the web browser lives in a package ---------------- */

#define BROWSER_PATH "/disk/bin/browser"

int browser_installed(void)
{
    struct stat st;
    return stat(BROWSER_PATH, &st) == 0;
}

void browser_open(const char *url)
{
    if (!browser_installed()) {
        wm_notify("The web browser is a package: install \"browser\" in Software");
        software_open();
        return;
    }
    if (app_launch(BROWSER_PATH, url && *url ? url : NULL) < 0)
        wm_notify("Could not start the web browser");
}
