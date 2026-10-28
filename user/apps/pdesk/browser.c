/* Web browser: loads pages with /bin/fetch in the background (so the desktop never freezes),
 * renders them with the HTML engine in html.c, and follows links.  Plain HTTP only. */
#include "html.h"

#define TOOL_H   38
#define STATUS_H 22
#define SB_W     14
#define NHIST    32
#define HOME_URL "about:home"
#define MAX_PAGE (6 * 1024 * 1024)

struct hist {
    char url[512];
    int scroll;
};

struct browser {
    struct win *win;
    struct page page;
    struct hist hist[NHIST];
    int hpos, hcount;
    char addr[512];
    int alen, addr_focus, addr_sel;
    int scroll, restore_scroll;
    int hover;
    int loading, rfd, pid;
    char *rbuf;
    int rlen, rcap;
    char status[160];
    int sb_drag, sb_off;
};

static const char home_html[] =
    "<html><head><title>Petrichor Browser</title></head><body>"
    "<h1>Petrichor Web Browser</h1>"
    "<p>A small web browser written from scratch for Petrichor. It reads plain "
    "<b>http://</b> pages: headings, text, lists, tables and links. There is no HTTPS (the system has no "
    "encryption support yet), no images, and no JavaScript, so many modern sites won't work.</p>"
    "<h2>Try these</h2>"
    "<ul>"
    "<li><a href=\"http://example.com/\">example.com</a>: the classic example page</li>"
    "<li><a href=\"http://neverssl.com/\">neverssl.com</a>: a plain-HTTP test site</li>"
    "<li><a href=\"http://info.cern.ch/\">info.cern.ch</a>: the first website, still online</li>"
    "<li><a href=\"file:///etc/demo.html\">The demo page</a>: shows what the renderer can do</li>"
    "<li><a href=\"http://10.0.2.2:8080/\">Your computer, port 8080</a> (for example after "
    "<code>make serve-repo</code>)</li>"
    "<li><a href=\"file:///\">Browse the filesystem</a></li>"
    "</ul>"
    "<h2>Keys</h2>"
    "<table><tr><td><b>Ctrl+L</b></td><td>address bar</td></tr>"
    "<tr><td><b>Alt+Left / Alt+Right</b></td><td>back / forward</td></tr>"
    "<tr><td><b>F5</b></td><td>reload</td></tr>"
    "<tr><td><b>Space, PgUp, PgDn, wheel</b></td><td>scroll</td></tr></table>"
    "</body></html>";

static void status_num(struct browser *b, const char *prefix, int n, const char *suffix)
{
    snprintf(b->status, sizeof b->status, "%s%d%s", prefix, n, suffix);
}

static void esc(char *dst, int max, const char *src)
{
    int n = 0;
    for (; *src && n < max - 7; src++) {
        const char *r = *src == '<' ? "&lt;" : *src == '>' ? "&gt;" : *src == '&' ? "&amp;" : *src == '"' ? "&quot;" : NULL;
        if (r)
            while (*r)
                dst[n++] = *r++;
        else
            dst[n++] = *src;
    }
    dst[n] = 0;
}

static void update_title(struct browser *b)
{
    char t[96];
    snprintf(t, sizeof t, "%s - Web Browser", b->page.title[0] ? b->page.title : b->page.url);
    wm_set_title(b->win, t);
}

static void show_html(struct browser *b, const char *url, const char *html, int len)
{
    page_clear(&b->page);
    safestrcpy(b->page.url, url, sizeof b->page.url);
    html_parse(&b->page, html, len);
    b->hover = -1;
    safestrcpy(b->addr, url, sizeof b->addr);
    b->alen = strlen(b->addr);
    b->scroll = b->restore_scroll;
    b->restore_scroll = 0;
    update_title(b);
    wm_invalidate(b->win);
}

static void show_message(struct browser *b, const char *url, const char *headline, const char *detail)
{
    char d[400], u[600], html[1400];
    esc(d, sizeof d, detail);
    esc(u, sizeof u, url);
    int n = snprintf(html, sizeof html,
                     "<html><head><title>%s</title></head><body><h2>%s</h2><p>%s</p>"
                     "<p><a href=\"%s\">Try again</a> &nbsp; <a href=\"about:home\">Home</a></p></body></html>",
                     headline, headline, d, u);
    show_html(b, url, html, n);
}

/* ---------------- loading ---------------- */

static void stop_fetch(struct browser *b)
{
    if (!b->loading)
        return;
    kill(b->pid, 9);
    close(b->rfd);
    b->loading = 0;
    free(b->rbuf);
    b->rbuf = NULL;
}

static void start_fetch(struct browser *b, const char *url)
{
    int fds[2];
    if (pipe(fds) < 0) {
        show_message(b, url, "Can't load the page", "Out of resources.");
        return;
    }
    int pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        show_message(b, url, "Can't load the page", "Out of processes.");
        return;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        for (int fd = 3; fd < NOFILE_USER; fd++)
            close(fd);
        char *argv[] = {"fetch", (char *)url, NULL};
        exec("/bin/fetch", argv);
        exit(127);
    }
    close(fds[1]);
    b->rfd = fds[0];
    b->pid = pid;
    b->loading = 1;
    b->rcap = 16384;
    b->rbuf = malloc(b->rcap);
    b->rlen = 0;
    snprintf(b->status, sizeof b->status, "Loading %s ...", url);
    safestrcpy(b->addr, url, sizeof b->addr);
    b->alen = strlen(b->addr);
    wm_invalidate(b->win);
}

static int ends_with(const char *s, const char *suffix)
{
    int n = strlen(s), m = strlen(suffix);
    return n >= m && !strcmp(s + n - m, suffix);
}

static void load_file(struct browser *b, const char *url)
{
    const char *path = url + 7;                         /* after "file://" */
    struct stat st;
    if (stat(path, &st) < 0) {
        show_message(b, url, "File not found", path);
        return;
    }
    if (st.type == T_DIR) {
        int fd = open(path, O_RDONLY);
        char *html = malloc(64 * 1024);
        if (fd < 0 || !html) {
            show_message(b, url, "Can't open the directory", path);
            free(html);
            return;
        }
        int n = snprintf(html, 64 * 1024, "<html><head><title>Index of %s</title></head><body><h2>Index of %s</h2><ul>", path, path);
        struct dirent de;
        const char *sl = ends_with(path, "/") ? "" : "/";
        if (strcmp(path, "/"))
            n += snprintf(html + n, 64 * 1024 - n, "<li><a href=\"..\">..</a></li>");
        while (readdir(fd, &de) > 0 && n < 64 * 1024 - 400) {
            if (!strcmp(de.name, ".") || !strcmp(de.name, ".."))
                continue;
            n += snprintf(html + n, 64 * 1024 - n, "<li><a href=\"%s%s%s\">%s%s</a></li>", path, sl, de.name, de.name,
                          de.type == T_DIR ? "/" : "");
        }
        close(fd);
        n += snprintf(html + n, 64 * 1024 - n, "</ul></body></html>");
        show_html(b, url, html, n);
        free(html);
        return;
    }
    int fd = open(path, O_RDONLY);
    char *buf = malloc(st.size + 1);
    int n = fd >= 0 && buf ? read(fd, buf, st.size) : -1;
    if (fd >= 0)
        close(fd);
    if (n < 0) {
        show_message(b, url, "Can't read the file", path);
        free(buf);
        return;
    }
    buf[n] = 0;
    if (ends_with(path, ".html") || ends_with(path, ".htm")) {
        show_html(b, url, buf, n);
    } else {
        page_clear(&b->page);
        safestrcpy(b->page.url, url, sizeof b->page.url);
        text_parse(&b->page, buf, n);
        b->scroll = b->restore_scroll;
        b->restore_scroll = 0;
        safestrcpy(b->addr, url, sizeof b->addr);
        b->alen = strlen(b->addr);
        update_title(b);
        wm_invalidate(b->win);
    }
    free(buf);
}

static void history_push(struct browser *b, const char *url)
{
    if (b->hcount > 0)
        b->hist[b->hpos].scroll = b->scroll;
    b->hcount = b->hcount ? b->hpos + 1 : 0;            /* drop the forward entries */
    if (b->hcount == NHIST) {
        memmove(&b->hist[0], &b->hist[1], (NHIST - 1) * sizeof b->hist[0]);
        b->hcount--;
    }
    safestrcpy(b->hist[b->hcount].url, url, sizeof b->hist[0].url);
    b->hist[b->hcount].scroll = 0;
    b->hpos = b->hcount++;
}

static void navigate(struct browser *b, const char *input, int push)
{
    char url[512];
    while (*input == ' ')
        input++;
    safestrcpy(url, input, sizeof url);
    for (int n = strlen(url); n > 0 && url[n - 1] == ' '; n--)
        url[n - 1] = 0;
    if (!url[0])
        return;
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8) && strncmp(url, "file://", 7) && strncmp(url, "about:", 6)) {
        char tmp[512];
        if (url[0] == '/')
            snprintf(tmp, sizeof tmp, "file://%s", url);
        else if (strchr(url, ' '))
            snprintf(tmp, sizeof tmp, "about:blank");
        else
            snprintf(tmp, sizeof tmp, "http://%s", url);
        safestrcpy(url, tmp, sizeof url);
    }
    stop_fetch(b);
    if (push)
        history_push(b, url);
    b->status[0] = 0;
    if (!strcmp(url, "about:blank")) {
        show_html(b, url, "", 0);
        safestrcpy(b->status, "Type an address in the bar above and press Enter.", sizeof b->status);
    } else if (!strcmp(url, "about:home")) {
        show_html(b, url, home_html, sizeof home_html - 1);
    } else if (!strncmp(url, "file://", 7)) {
        load_file(b, url);
    } else if (!strncmp(url, "https://", 8)) {
        show_message(b, url, "HTTPS isn't supported yet",
                     "This browser has no encryption (TLS), so it can only open http:// pages. "
                     "Many sites only work over HTTPS. Try a plain-HTTP site from the home page.");
    } else {
        start_fetch(b, url);
    }
}

static void finish_load(struct browser *b)
{
    char *buf = b->rbuf;
    int len = b->rlen;
    if (!buf || len == 0) {
        show_message(b, b->hist[b->hpos].url, "No response", "The server returned nothing.");
        goto done;
    }
    buf[len] = 0;
    const char *req_url = b->hist[b->hpos].url;
    if (!strncmp(buf, "ERROR ", 6)) {
        char *nl = strchr(buf, '\n');
        if (nl)
            *nl = 0;
        show_message(b, req_url, "Can't open the page", buf + 6);
        goto done;
    }
    int status = 0;
    char url[512], type[96] = "text/html";
    safestrcpy(url, req_url, sizeof url);
    char *body = NULL;
    for (char *l = buf; *l;) {
        char *nl = strchr(l, '\n');
        if (!nl)
            break;
        *nl = 0;
        if (!strncmp(l, "STATUS ", 7)) status = atoi(l + 7);
        else if (!strncmp(l, "URL ", 4)) safestrcpy(url, l + 4, sizeof url);
        else if (!strncmp(l, "TYPE ", 5)) safestrcpy(type, l + 5, sizeof type);
        else if (!*l) {
            body = nl + 1;
            break;
        }
        l = nl + 1;
    }
    if (!body) {
        show_message(b, req_url, "Can't open the page", "The response was malformed.");
        goto done;
    }
    int blen = len - (body - buf);
    safestrcpy(b->hist[b->hpos].url, url, sizeof b->hist[0].url);     /* the final URL after redirects */
    if (strstr(type, "html") || (!strstr(type, "text/plain") && blen > 0 && strchr(body, '<') && !strstr(type, "image"))) {
        show_html(b, url, body, blen);
    } else if (!strncmp(type, "text/", 5)) {
        page_clear(&b->page);
        safestrcpy(b->page.url, url, sizeof b->page.url);
        text_parse(&b->page, body, blen);
        safestrcpy(b->addr, url, sizeof b->addr);
        b->alen = strlen(b->addr);
        b->scroll = b->restore_scroll;
        b->restore_scroll = 0;
        update_title(b);
        wm_invalidate(b->win);
    } else {
        char d[160];
        snprintf(d, sizeof d, "This browser can't display content of type %s (%d bytes).", type, blen);
        show_message(b, url, "Can't display this", d);
    }
    if (status >= 400)
        status_num(b, "HTTP error ", status, "");
    else
        status_num(b, "Done (", blen, " bytes)");
done:
    free(b->rbuf);
    b->rbuf = NULL;
}

/* ---------------- app callbacks ---------------- */

static int br_pollfd(struct win *w, struct pollfd *p)
{
    struct browser *b = w->data;
    if (!b->loading)
        return 0;
    *p = (struct pollfd){b->rfd, POLLIN, 0};
    return 1;
}

static void br_fdready(struct win *w, struct pollfd *p)
{
    struct browser *b = w->data;
    if (b->rlen + 8192 + 1 > b->rcap) {
        if (b->rcap >= MAX_PAGE) {
            stop_fetch(b);
            show_message(b, b->hist[b->hpos].url, "Page too large", "The page is bigger than this browser can handle.");
            return;
        }
        char *nb = realloc(b->rbuf, b->rcap * 2);
        if (!nb) {
            stop_fetch(b);
            return;
        }
        b->rbuf = nb;
        b->rcap *= 2;
    }
    int n = read(b->rfd, b->rbuf + b->rlen, 8192);
    if (n > 0) {
        b->rlen += n;
        snprintf(b->status, sizeof b->status, "Loading ... %d KB", b->rlen / 1024);
    } else {
        close(b->rfd);
        b->loading = 0;
        finish_load(b);
    }
    wm_invalidate(w);
}

static struct rect view_rect(struct rect c)
{
    return (struct rect){c.x, c.y + TOOL_H, c.w - SB_W, c.h - TOOL_H - STATUS_H};
}

static int max_scroll(struct browser *b, struct rect view)
{
    int m = b->page.height - view.h;
    return m > 0 ? m : 0;
}

static void tool_button(int x, int y, const char *label, int disabled)
{
    ui_button(x, y, 28, 26, label, disabled);
}

static void br_draw(struct win *w, struct rect c)
{
    struct browser *b = w->data;
    gfx_vgrad(c.x, c.y, c.w, TOOL_H, RGB(0xf2, 0xf3, 0xf4), RGB(0xd6, 0xd9, 0xdc));
    tool_button(c.x + 8, c.y + 6, "<", b->hpos <= 0);
    tool_button(c.x + 40, c.y + 6, ">", b->hpos >= b->hcount - 1);
    tool_button(c.x + 72, c.y + 6, b->loading ? "x" : "R", 0);
    tool_button(c.x + 104, c.y + 6, "H", 0);

    int ax = c.x + 140, aw = c.w - 148;
    ui_panel_inset(ax, c.y + 6, aw, 26);
    int vis = (aw - 16) / 8;
    const char *show = b->alen > vis ? b->addr + b->alen - vis : b->addr;
    int tx = ax + 8;
    if (b->addr_focus && b->addr_sel)
        gfx_fill(tx - 1, c.y + 9, (int)strlen(show) * 8 + 2, 20, g_accent);
    gfx_text(tx, c.y + 11, show, b->addr_focus && b->addr_sel ? C_WHITE : C_TEXT);
    if (b->addr_focus && !b->addr_sel)
        gfx_fill(tx + (int)strlen(show) * 8, c.y + 10, 2, 18, g_accent);

    struct rect view = view_rect(c);
    gfx_fill(view.x, view.y, view.w, view.h, C_WHITE);
    if (b->page.layout_w != view.w)
        page_layout(&b->page, view.w);
    int ms = max_scroll(b, view);
    if (b->scroll > ms) b->scroll = ms;
    if (b->scroll < 0) b->scroll = 0;
    struct rect saved = gfx_get_clip();
    struct rect clip = rect_isect(saved, view);
    if (clip.w > 0) {
        gfx_set_clip(clip.x, clip.y, clip.w, clip.h);
        if (b->loading && !b->page.nitems)
            gfx_text(view.x + 16, view.y + 16, "Loading...", C_TEXT_DIM);
        page_draw(&b->page, view, b->scroll, b->hover);
        gfx_set_clip(saved.x, saved.y, saved.w, saved.h);
    }

    int sx = c.x + c.w - SB_W;                                      /* scrollbar */
    gfx_fill(sx, view.y, SB_W, view.h, RGB(0xe4, 0xe6, 0xe8));
    if (ms > 0) {
        int th = view.h * view.h / b->page.height;
        if (th < 24) th = 24;
        int ty = view.y + (view.h - th) * b->scroll / ms;
        gfx_fill(sx + 2, ty, SB_W - 4, th, RGB(0x8a, 0x94, 0xa0));
    }

    int sy = c.y + c.h - STATUS_H;
    gfx_fill(c.x, sy, c.w, STATUS_H, RGB(0xe0, 0xe3, 0xe6));
    gfx_hline(c.x, sy, c.w, RGB(0xb0, 0xb6, 0xbc));
    const char *st = b->hover >= 0 && b->hover < b->page.nlinks ? b->page.links[b->hover] : b->status;
    char line[100];
    safestrcpy(line, st, sizeof line);
    gfx_text(c.x + 8, sy + 3, line, C_TEXT_DIM);
}

static void go_back(struct browser *b)
{
    if (b->hpos > 0) {
        b->hist[b->hpos].scroll = b->scroll;
        b->hpos--;
        b->restore_scroll = b->hist[b->hpos].scroll;
        navigate(b, b->hist[b->hpos].url, 0);
    }
}

static void go_forward(struct browser *b)
{
    if (b->hpos < b->hcount - 1) {
        b->hist[b->hpos].scroll = b->scroll;
        b->hpos++;
        b->restore_scroll = b->hist[b->hpos].scroll;
        navigate(b, b->hist[b->hpos].url, 0);
    }
}

static void br_mouse(struct win *w, int x, int y, int kind)
{
    struct browser *b = w->data;
    struct rect c = wm_client(w);
    struct rect view = view_rect((struct rect){0, 0, c.w, c.h});
    int ms = max_scroll(b, view);

    if (kind == M_MOVE) {
        int link = x < view.w && y >= TOOL_H && y < TOOL_H + view.h ? page_link_at(&b->page, x, y - TOOL_H + b->scroll) : -1;
        if (link != b->hover) {
            b->hover = link;
            wm_invalidate(w);
        }
        return;
    }
    if (kind == M_UP) {
        b->sb_drag = 0;
        return;
    }
    if (kind == M_DRAG) {
        if (b->sb_drag && ms > 0) {
            int th = view.h * view.h / b->page.height;
            if (th < 24) th = 24;
            int track = view.h - th;
            b->scroll = track > 0 ? (y - TOOL_H - b->sb_off) * ms / track : 0;
            if (b->scroll < 0) b->scroll = 0;
            if (b->scroll > ms) b->scroll = ms;
            wm_invalidate(w);
        }
        return;
    }
    if (kind != M_DOWN && kind != M_DBL)
        return;

    if (y < TOOL_H) {
        b->addr_focus = 0;
        if (y >= 6 && y < 32) {
            if (x >= 8 && x < 36) go_back(b);
            else if (x >= 40 && x < 68) go_forward(b);
            else if (x >= 72 && x < 100) {
                if (b->loading) {
                    stop_fetch(b);
                    safestrcpy(b->status, "Stopped.", sizeof b->status);
                } else if (b->hcount) {
                    b->restore_scroll = b->scroll;
                    navigate(b, b->hist[b->hpos].url, 0);
                }
            } else if (x >= 104 && x < 132) navigate(b, HOME_URL, 1);
            else if (x >= 140) {
                b->addr_focus = 1;
                b->addr_sel = 1;
            }
        }
        wm_invalidate(w);
        return;
    }
    b->addr_focus = 0;
    if (x >= view.w) {                                              /* scrollbar */
        if (ms > 0) {
            int th = view.h * view.h / b->page.height;
            if (th < 24) th = 24;
            int ty = TOOL_H + (view.h - th) * b->scroll / ms;
            if (y >= ty && y < ty + th) {
                b->sb_drag = 1;
                b->sb_off = y - TOOL_H - (ty - TOOL_H);
            } else {
                b->scroll += y < ty ? -view.h + 32 : view.h - 32;
                if (b->scroll < 0) b->scroll = 0;
                if (b->scroll > ms) b->scroll = ms;
            }
        }
        wm_invalidate(w);
        return;
    }
    int link = page_link_at(&b->page, x, y - TOOL_H + b->scroll);
    if (link >= 0) {
        char target[512];
        safestrcpy(target, b->page.links[link], sizeof target);
        navigate(b, target, 1);
    }
    wm_invalidate(w);
}

static void br_wheel(struct win *w, int dir)
{
    struct browser *b = w->data;
    b->scroll += dir * 48;
    wm_invalidate(w);
}

static void br_key(struct win *w, struct input_event *e)
{
    struct browser *b = w->data;
    struct rect c = wm_client(w);
    struct rect view = view_rect((struct rect){0, 0, c.w, c.h});
    int ctrl = e->flags & IEF_CTRL, alt = e->flags & IEF_ALT;

    if (ctrl && (e->code == 'l' || e->code == 'L')) {
        b->addr_focus = 1;
        b->addr_sel = 1;
        wm_invalidate(w);
        return;
    }
    if (alt && e->code == KEY_LEFT) { go_back(b); return; }
    if (alt && e->code == KEY_RIGHT) { go_forward(b); return; }
    if (e->code == KEY_F1 + 4) {                                    /* F5 */
        b->restore_scroll = b->scroll;
        navigate(b, b->hist[b->hpos].url, 0);
        return;
    }
    if (b->addr_focus) {
        if (e->code == '\n') {
            b->addr_focus = 0;
            navigate(b, b->addr, 1);
            return;
        }
        if (e->code == 27) {
            b->addr_focus = 0;
            if (b->hcount) {
                safestrcpy(b->addr, b->hist[b->hpos].url, sizeof b->addr);
                b->alen = strlen(b->addr);
            }
        } else if (e->code == 8) {
            if (b->addr_sel) { b->alen = 0; b->addr_sel = 0; }
            else if (b->alen) b->alen--;
            b->addr[b->alen] = 0;
        } else if (e->code >= 32 && e->code < 127 && !ctrl) {
            if (b->addr_sel) { b->alen = 0; b->addr_sel = 0; }
            if (b->alen < (int)sizeof b->addr - 1)
                b->addr[b->alen++] = e->code;
            b->addr[b->alen] = 0;
        } else {
            return;
        }
        wm_invalidate(w);
        return;
    }
    int ms = max_scroll(b, view);
    switch (e->code) {
    case KEY_DOWN: b->scroll += 40; break;
    case KEY_UP: b->scroll -= 40; break;
    case KEY_PGDN: case ' ': b->scroll += view.h - 40; break;
    case KEY_PGUP: b->scroll -= view.h - 40; break;
    case KEY_HOME: b->scroll = 0; break;
    case KEY_END: b->scroll = ms; break;
    case 8: go_back(b); return;
    default: return;
    }
    wm_invalidate(w);
}

static void br_tick(struct win *w, u32 now)
{
    struct browser *b = w->data;
    if (b->loading)
        wm_invalidate_rect(w->x, w->y + TITLE_H, w->w, 44);
}

static void br_close(struct win *w)
{
    struct browser *b = w->data;
    stop_fetch(b);
    page_free(&b->page);
    free(b);
}

static const struct app browser_app = {.draw = br_draw, .key = br_key, .mouse = br_mouse, .wheel = br_wheel,
                                       .tick = br_tick, .pollfd = br_pollfd, .fdready = br_fdready,
                                       .close = br_close};

void browser_open(const char *url)
{
    struct browser *b = calloc(1, sizeof *b);
    if (!b)
        return;
    b->hover = -1;
    b->win = wm_open(&browser_app, 860, 620, "Web Browser", b);
    if (!b->win) {
        free(b);
        return;
    }
    b->win->minw = 420;
    b->win->minh = 260;
    navigate(b, url && *url ? url : HOME_URL, 1);
}
