/* Web browser: loads pages (and their stylesheets and images) with /bin/fetch in the background so the
 * desktop never freezes, renders them with the engine in web.h (HTML, CSS, images), and follows links. */
#include "web.h"

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

#define NSLOT 3
struct rslot {                                  /* one sub-resource download */
    int fd, pid, res, len, cap;
    char *buf;
};

struct browser {
    struct win *win;
    struct rslot slot[NSLOT];
    struct page page;
    struct hist hist[NHIST];
    int hpos, hcount;
    char addr[512];
    int alen, addr_focus, addr_sel;
    int secure;                                 /* the page on screen came over a verified HTTPS connection */
    int scroll, restore_scroll;
    int hover;
    int loading, rfd, pid;
    char *rbuf;
    int rlen, rcap;
    char status[160];
    int sb_drag, sb_off;
    struct node *focus, *hover_node;            /* the focused form field; the element under the pointer */
    int scripts_next, scripts_done;             /* <script> elements run so far (in document order) */
};

static void navigate(struct browser *b, const char *input, int push);
static void pump(struct browser *b);
static void run_scripts(struct browser *b);

static const char home_html[] =
    "<html><head><title>Petrichor Browser</title></head><body>"
    "<h1>Petrichor Web Browser</h1>"
    "<p>A small web browser written from scratch for Petrichor. It opens <b>http://</b> and "
    "<b>https://</b> pages (certificates are checked against the built-in list of trusted authorities) and "
    "renders them with CSS: colors, fonts, boxes, flexbox, floats, tables, and PNG, JPEG and GIF images. "
    "It runs JavaScript (the everyday language and the DOM: events, forms, timers, localStorage), so "
    "simple interactive pages work; big web apps that need regular expressions, promises or the network "
    "from scripts will not.</p>"
    "<h2>Try these</h2>"
    "<ul>"
    "<li><a href=\"https://example.com/\">example.com</a>: the classic example page, over HTTPS</li>"
    "<li><a href=\"https://en.wikipedia.org/wiki/Petrichor\">Wikipedia: Petrichor</a>: a big real page</li>"
    "<li><a href=\"http://neverssl.com/\">neverssl.com</a>: a plain-HTTP test site</li>"
    "<li><a href=\"http://info.cern.ch/\">info.cern.ch</a>: the first website, still online</li>"
    "<li><a href=\"file:///etc/demo.html\">The demo page</a>: shows what the renderer can do</li>"
    "<li><a href=\"file:///etc/jsdemo.html\">The JavaScript demo</a>: a counter, a to-do list, a clock and a game</li>"
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
    b->secure = 0;
    page_clear(&b->page);
    b->focus = b->hover_node = NULL;
    safestrcpy(b->page.url, url, sizeof b->page.url);
    html_parse(&b->page, html, len);
    b->hover = -1;
    js_page_start(&b->page);
    b->scripts_next = 0;
    b->scripts_done = 0;
    safestrcpy(b->addr, url, sizeof b->addr);
    b->alen = strlen(b->addr);
    b->scroll = b->restore_scroll;
    b->restore_scroll = 0;
    update_title(b);
    wm_invalidate(b->win);
    run_scripts(b);
}

static void show_message(struct browser *b, const char *url, const char *headline, const char *detail)
{
    b->secure = 0;
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

static void stop_slots(struct browser *b)
{
    for (int i = 0; i < NSLOT; i++) {
        struct rslot *sl = &b->slot[i];
        if (!sl->buf)
            continue;
        kill(sl->pid, 9);
        close(sl->fd);
        free(sl->buf);
        sl->buf = NULL;
    }
}

static void stop_fetch(struct browser *b)
{
    stop_slots(b);
    if (!b->loading)
        return;
    kill(b->pid, 9);
    close(b->rfd);
    b->loading = 0;
    free(b->rbuf);
    b->rbuf = NULL;
}

/* ---------------- sub-resources: stylesheets first, then images, three at a time ---------------- */

static int next_wanted(struct page *p)
{
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < p->nres; i++)
            if (p->res[i].state == RS_WANT && (pass ? p->res[i].kind == R_IMAGE : p->res[i].kind != R_IMAGE))
                return i;
    return -1;
}

static int css_pending(struct page *p)
{
    for (int i = 0; i < p->nres; i++)
        if (p->res[i].kind == R_CSS && (p->res[i].state == RS_WANT || p->res[i].state == RS_LOADING))
            return 1;
    return 0;
}

static int res_left(struct page *p)
{
    int n = 0;
    for (int i = 0; i < p->nres; i++)
        if (p->res[i].state == RS_WANT || p->res[i].state == RS_LOADING)
            n++;
    return n;
}

static void pump(struct browser *b)
{
    struct page *p = &b->page;
    for (int i = 0; i < NSLOT; i++) {
        struct rslot *sl = &b->slot[i];
        if (sl->buf)
            continue;
        int r = next_wanted(p);
        if (r < 0)
            break;
        if (!strncmp(p->res[r].url, "file://", 7)) {                 /* local files: read them right here */
            const char *path = p->res[r].url + 7;
            struct stat st;
            int fd = stat(path, &st) == 0 ? open(path, O_RDONLY) : -1;
            sl->cap = (fd >= 0 ? st.size : 0) + 32;
            sl->buf = malloc(sl->cap);
            if (!sl->buf) { if (fd >= 0) close(fd); break; }
            sl->len = snprintf(sl->buf, sl->cap, "STATUS %d\n\n", fd >= 0 ? 200 : 404);
            int got;
            while (fd >= 0 && sl->len < sl->cap - 1 && (got = read(fd, sl->buf + sl->len, sl->cap - 1 - sl->len)) > 0)
                sl->len += got;
            if (fd >= 0) close(fd);
            sl->res = r;
            sl->fd = -1;
            sl->pid = 0;
            p->res[r].state = RS_LOADING;
            extern void finish_slot_local(struct browser *b, struct rslot *sl);
            finish_slot_local(b, sl);
            i--;                                                     /* the slot is free again */
            continue;
        }
        int fds[2];
        if (pipe(fds) < 0)
            break;
        int pid = fork();
        if (pid < 0) {
            close(fds[0]);
            close(fds[1]);
            break;
        }
        if (pid == 0) {
            setpgid(0, 0);
            dup2(fds[1], 1);
            dup2(fds[1], 2);
            for (int fd = 3; fd < NOFILE_USER; fd++)
                close(fd);
            char *argv[] = {"fetch", p->res[r].url, NULL};
            exec("/bin/fetch", argv);
            exit(127);
        }
        close(fds[1]);
        sl->fd = fds[0];
        sl->pid = pid;
        sl->res = r;
        sl->len = 0;
        sl->cap = 16384;
        sl->buf = malloc(sl->cap);
        if (!sl->buf) {
            kill(pid, 9);
            close(fds[0]);
            break;
        }
        p->res[r].state = RS_LOADING;
    }
    int left = res_left(p);
    if (left)
        snprintf(b->status, sizeof b->status, "Loading page resources (%d left)", left);
    else if (!b->loading)
        snprintf(b->status, sizeof b->status, "Done");
}

/* A sub-resource finished downloading. */
static void finish_slot(struct browser *b, struct rslot *sl)
{
    struct page *p = &b->page;
    struct res *r = &p->res[sl->res];
    sl->buf[sl->len] = 0;
    char *body = NULL;
    int blen = 0, status = 0;
    if (!strncmp(sl->buf, "STATUS ", 7)) {
        status = atoi(sl->buf + 7);
        char *sep = strstr(sl->buf, "\n\n");
        if (sep) {
            body = sep + 2;
            blen = sl->len - (body - sl->buf);
        }
    }
    r->state = RS_FAILED;
    if (status == 200 && body && blen > 0) {
        if (r->kind == R_CSS || r->kind == R_SCRIPT) {
            r->css = malloc(blen + 1);
            if (r->css) {
                memcpy(r->css, body, blen);
                r->css[blen] = 0;
                r->css_len = blen;
                r->state = RS_DONE;
            }
        } else {
            r->img = img_decode((const u8 *)body, blen);
            if (r->img)
                r->state = RS_DONE;
        }
    }
    free(sl->buf);
    sl->buf = NULL;
    if (sl->fd >= 0)
        close(sl->fd);
    if (r->kind == R_CSS)
        p->dirty_style = 1;
    p->layout_w = 0;                                    /* sizes may have changed: lay out again */
    if (r->kind == R_SCRIPT)
        run_scripts(b);
    if (b->page.root)
        pump(b);
    wm_invalidate(b->win);
}

void finish_slot_local(struct browser *b, struct rslot *sl) { finish_slot(b, sl); }

/* ---------------- JavaScript ---------------- */

/* After scripts ran: show alerts, follow location changes, restyle and relayout when the DOM changed.
 * Returns 1 if the page was replaced (navigation). */
static int js_after(struct browser *b)
{
    struct page *p = &b->page;
    const char *a = js_take_alert(p);
    if (a)
        wm_notify(a);
    const char *nav = js_take_nav(p);
    if (nav) {
        char u[512], target[512];
        safestrcpy(target, nav, sizeof target);
        url_resolve(p->url, target, u, sizeof u);
        navigate(b, u, 1);
        return 1;
    }
    struct node *jf = js_focus(p);
    if (jf && jf != b->focus) { b->focus = jf; p->dirty_style = 1; p->layout_w = 0; }
    if (js_dirty(p)) {
        p->dirty_style = 1;
        p->layout_w = 0;
        pump(b);
    }
    update_title(b);
    wm_invalidate(b->win);
    return 0;
}

static int runnable_script(struct node *n)
{
    const char *t = attr_val(n, "type");
    if (!t || !*t) return 1;
    return strstr(t, "javascript") || strstr(t, "ecmascript");     /* not modules, JSON, templates */
}

/* Runs the page's scripts in document order, waiting for external ones to download. */
static void run_scripts(struct browser *b)
{
    struct page *p = &b->page;
    if (b->scripts_done || !p->root || !p->js)
        return;
    for (int guard = 0; guard < 500; guard++) {
        int idx = 0;
        struct node *found = NULL;
        for (struct node *n = p->root; n;) {
            if (n->type == 0 && n->tag == TG_SCRIPT && idx++ == b->scripts_next) { found = n; break; }
            if (n->first) { n = n->first; continue; }
            while (n && !n->next) n = n->parent;
            if (n) n = n->next;
        }
        if (!found)
            break;
        if (!runnable_script(found)) { b->scripts_next++; continue; }
        const char *src = attr_val(found, "src");
        if (src && *src) {
            char full[512];
            url_resolve(p->url, src, full, sizeof full);
            int ri = -1;
            for (int i = 0; i < p->nres; i++)
                if (p->res[i].kind == R_SCRIPT && !strcmp(p->res[i].url, full)) ri = i;
            if (ri < 0)
                ri = add_resource(p, full, R_SCRIPT);
            if (ri < 0) { b->scripts_next++; continue; }
            struct res *r = &p->res[ri];
            if (r->state == RS_WANT || r->state == RS_LOADING) {
                snprintf(b->status, sizeof b->status, "Loading script %s", full);
                pump(b);
                return;                                              /* finish_slot calls us again */
            }
            b->scripts_next++;
            if (r->state == RS_DONE && r->css)
                js_run(p, r->css, r->css_len, full);
        } else {
            b->scripts_next++;
            if (found->first && found->first->type == 1)
                js_run(p, found->first->text, found->first->tlen, "inline script");
        }
        if (js_after(b))
            return;                                                  /* the script navigated away */
    }
    b->scripts_done = 1;
    js_loaded(p);
    js_after(b);
}

static struct node *element_of(struct node *n)
{
    while (n && n->type != 0) n = n->parent;
    return n;
}

static int text_field(struct node *n)
{
    if (!n || n->type != 0) return 0;
    if (n->tag == TG_TEXTAREA) return 1;
    if (n->tag != TG_INPUT) return 0;
    const char *t = attr_val(n, "type");
    return !t || !strcmp(t, "text") || !strcmp(t, "password") || !strcmp(t, "email") || !strcmp(t, "search") ||
           !strcmp(t, "number") || !strcmp(t, "url") || !strcmp(t, "tel");
}

static int focusable(struct node *n)
{
    return n && n->type == 0 && (text_field(n) || n->tag == TG_BUTTON || n->tag == TG_SELECT ||
                                 (n->tag == TG_INPUT) || (n->tag == TG_A && attr_val(n, "href")) || attr_val(n, "tabindex"));
}

static void set_focus(struct browser *b, struct node *n)
{
    if (b->focus == n) return;
    struct page *p = &b->page;
    struct node *old = b->focus;
    b->focus = n;
    if (old && p->js) js_event(p, old, "blur", 0, 0);
    if (n && p->js) js_event(p, n, "focus", 0, 0);
    p->dirty_style = 1;
    p->layout_w = 0;
}

/* Appends the URL-encoded form of s. */
static int url_enc(char *out, int o, int cap, const char *s)
{
    for (; *s && o < cap - 4; s++) {
        u8 c = *s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            out[o++] = c;
        else if (c == ' ')
            out[o++] = '+';
        else
            o += snprintf(out + o, cap - o, "%%%02X", c);
    }
    return o;
}

/* A form is submitted: the page may handle it; otherwise its fields go to the action URL (GET). */
static void submit_form(struct browser *b, struct node *form)
{
    struct page *p = &b->page;
    if (p->js && js_event(p, form, "submit", 0, 0)) { js_after(b); return; }
    if (js_after(b)) return;
    const char *action = attr_val(form, "action");
    char base[512], url[1024];
    url_resolve(p->url, action && *action ? action : p->url, base, sizeof base);
    char *q = strchr(base, '?');
    if (q) *q = 0;
    int o = snprintf(url, sizeof url, "%s?", base), first = 1;
    for (struct node *n = form; n;) {
        if (n->type == 0 && (n->tag == TG_INPUT || n->tag == TG_TEXTAREA || n->tag == TG_SELECT)) {
            const char *name = attr_val(n, "name"), *t = attr_val(n, "type");
            int skip = !name || (t && (!strcmp(t, "submit") || !strcmp(t, "button") || !strcmp(t, "reset") || !strcmp(t, "file"))) ||
                       (t && (!strcmp(t, "checkbox") || !strcmp(t, "radio")) && !attr_val(n, "checked"));
            if (!skip) {
                char val[512] = "";
                if (n->tag == TG_TEXTAREA) dom_text(n, val, sizeof val);
                else if (n->tag == TG_SELECT) {
                    struct node *pick = NULL;
                    for (struct node *c = n->first; c; c = c->next)
                        if (c->type == 0 && c->tag == TG_OPTION && (!pick || attr_val(c, "selected"))) pick = c;
                    if (pick) { const char *v = attr_val(pick, "value"); if (v) safestrcpy(val, v, sizeof val); else dom_text(pick, val, sizeof val); }
                } else {
                    const char *v = attr_val(n, "value");
                    safestrcpy(val, v ? v : (t && !strcmp(t, "checkbox") ? "on" : ""), sizeof val);
                }
                if (!first && o < (int)sizeof url - 2) url[o++] = '&';
                first = 0;
                o = url_enc(url, o, sizeof url, name);
                if (o < (int)sizeof url - 2) url[o++] = '=';
                o = url_enc(url, o, sizeof url, val);
                url[o] = 0;
            }
        }
        if (n->first) { n = n->first; continue; }
        while (n && n != form && !n->next) n = n->parent;
        if (!n || n == form) break;
        n = n->next;
    }
    url[o < (int)sizeof url ? o : (int)sizeof url - 1] = 0;
    if (url[o - 1] == '?') url[o - 1] = 0;
    navigate(b, url, 1);
}

static struct node *form_of(struct node *n)
{
    while (n && !(n->type == 0 && n->tag == TG_FORM)) n = n->parent;
    return n;
}

/* What a click does when the page didn't cancel it. Returns 1 if it navigated. */
static int default_click(struct browser *b, struct node *el, int link)
{
    struct page *p = &b->page;
    for (struct node *n = el; n; n = n->parent) {
        if (n->type != 0) continue;
        if (n->tag == TG_INPUT) {
            const char *t = attr_val(n, "type");
            if (t && (!strcmp(t, "checkbox") || !strcmp(t, "radio"))) {
                int on = !attr_val(n, "checked");
                if (!strcmp(t, "radio")) {
                    on = 1;
                    const char *nm = attr_val(n, "name");
                    struct node *f = form_of(n);
                    for (struct node *x = f ? f : p->root; x && nm;) {
                        if (x != n && x->type == 0 && x->tag == TG_INPUT) {
                            const char *xt = attr_val(x, "type"), *xn = attr_val(x, "name");
                            if (xt && !strcmp(xt, "radio") && xn && !strcmp(xn, nm)) dom_set_attr(p, x, "checked", NULL);
                        }
                        if (x->first) { x = x->first; continue; }
                        while (x && !x->next) x = x->parent;
                        if (x) x = x->next;
                    }
                }
                dom_set_attr(p, n, "checked", on ? "" : NULL);
                p->dirty_style = 1;
                p->layout_w = 0;
                if (p->js) { js_event(p, n, "input", 0, 0); js_event(p, n, "change", 0, 0); }
                return js_after(b);
            }
            if (t && (!strcmp(t, "submit") || !strcmp(t, "image"))) {
                struct node *f = form_of(n);
                if (f) { submit_form(b, f); return 1; }
            }
            return 0;
        }
        if (n->tag == TG_BUTTON) {
            const char *t = attr_val(n, "type");
            struct node *f = form_of(n);
            if (f && (!t || !strcmp(t, "submit"))) { submit_form(b, f); return 1; }
            return 0;
        }
        if (n->tag == TG_LABEL) {
            const char *id = attr_val(n, "for");
            if (id) {
                for (struct node *x = p->root; x;) {
                    if (x->type == 0) { const char *xi = attr_val(x, "id"); if (xi && !strcmp(xi, id)) return default_click(b, x, -1); }
                    if (x->first) { x = x->first; continue; }
                    while (x && !x->next) x = x->parent;
                    if (x) x = x->next;
                }
            }
        }
    }
    if (link >= 0 && link < p->nlinks) {
        char target[512];
        safestrcpy(target, p->links[link], sizeof target);
        navigate(b, target, 1);
        return 1;
    }
    return 0;
}

/* Typing into the focused text field. Returns 1 if the key was used. */
static int field_key(struct browser *b, struct input_event *e)
{
    struct page *p = &b->page;
    struct node *n = b->focus;
    if (!text_field(n)) return 0;
    if (e->flags & (IEF_CTRL | IEF_ALT)) {
        if ((e->flags & IEF_CTRL) && (e->code == 'v' || e->code == 'V')) {
            int len;
            const char *c = clip_get(&len);
            if (!c) return 1;
            for (int i = 0; i < len && i < 2000; i++) {
                struct input_event k = {INPUT_KEY, IEF_DOWN, (u8)c[i] == '\r' ? 0 : (u8)c[i], 0, 0};
                if (k.code == '\n' && n->tag != TG_TEXTAREA) continue;
                if (k.code) field_key(b, &k);
            }
            return 1;
        }
        return 0;
    }
    if (e->code == 27) { set_focus(b, NULL); wm_invalidate(b->win); return 1; }
    if (e->code == '\t') { set_focus(b, NULL); return 1; }
    int is_area = n->tag == TG_TEXTAREA;
    char cur[4096];
    if (is_area) dom_text(n, cur, sizeof cur);
    else { const char *v = attr_val(n, "value"); safestrcpy(cur, v ? v : "", sizeof cur); }
    int l = strlen(cur);
    if (e->code == '\n' && !is_area) {
        if (p->js) js_event(p, n, "change", 0, 0);
        if (js_after(b)) return 1;
        struct node *f = form_of(n);
        if (f) submit_form(b, f);
        return 1;
    }
    if (e->code == 8) {
        if (!l) return 1;
        l--;
        while (l > 0 && ((u8)cur[l] & 0xC0) == 0x80) l--;            /* a whole UTF-8 character */
        cur[l] = 0;
    } else if ((e->code >= 32 && e->code < 127) || (e->code == '\n' && is_area)) {
        if (l >= (int)sizeof cur - 2) return 1;
        const char *ml = attr_val(n, "maxlength");
        if (ml && l >= atoi(ml) && atoi(ml) > 0) return 1;
        cur[l++] = e->code;
        cur[l] = 0;
    } else {
        return 0;
    }
    if (is_area) {
        while (n->first) dom_remove(n->first);
        if (l) { struct node *t = dom_new_text(p, cur, l); if (t) dom_insert(p, n, t, NULL); }
    } else {
        dom_set_attr(p, n, "value", cur);
    }
    p->layout_w = 0;
    if (p->js) js_event(p, n, "input", e->code, e->flags);
    js_after(b);
    return 1;
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
        safestrcpy(b->status, "Failed", sizeof b->status);
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
        b->secure = !strncmp(url, "https://", 8);
    } else if (!strncmp(type, "text/", 5)) {
        page_clear(&b->page);
        safestrcpy(b->page.url, url, sizeof b->page.url);
        text_parse(&b->page, body, blen);
        b->secure = !strncmp(url, "https://", 8);
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
    if (b->page.root)
        pump(b);
}

/* ---------------- app callbacks ---------------- */

static int br_pollfds(struct win *w, struct pollfd *p, int max)
{
    struct browser *b = w->data;
    int n = 0;
    if (b->loading && n < max)
        p[n++] = (struct pollfd){b->rfd, POLLIN, 0};
    for (int i = 0; i < NSLOT; i++)
        if (b->slot[i].buf && n < max)
            p[n++] = (struct pollfd){b->slot[i].fd, POLLIN, 0};
    return n;
}

static void br_fdready(struct win *w, struct pollfd *p)
{
    struct browser *b = w->data;
    for (int i = 0; i < NSLOT; i++) {
        struct rslot *sl = &b->slot[i];
        if (!sl->buf || sl->fd != p->fd)
            continue;
        if (sl->len + 8192 + 1 > sl->cap) {
            if (sl->cap >= 8 * 1024 * 1024) {
                sl->len = 0;
                finish_slot(b, sl);
                return;
            }
            char *nb = realloc(sl->buf, sl->cap * 2);
            if (!nb) { sl->len = 0; finish_slot(b, sl); return; }
            sl->buf = nb;
            sl->cap *= 2;
        }
        int n = read(sl->fd, sl->buf + sl->len, 8192);
        if (n > 0)
            sl->len += n;
        else
            finish_slot(b, sl);
        return;
    }
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
    int lock = b->secure ? 18 : 0;
    int vis = (aw - 16 - lock) / 8;
    const char *show = b->alen > vis ? b->addr + b->alen - vis : b->addr;
    int tx = ax + 8 + lock;
    if (b->secure) {                                           /* padlock: a verified HTTPS connection */
        int lx = ax + 8, ly = c.y + 13;
        gfx_frame(lx + 2, ly, 7, 7, RGB(0x2d, 0x7a, 0x3a));
        gfx_vline(lx + 3, ly + 1, 6, RGB(0x2d, 0x7a, 0x3a));
        gfx_vline(lx + 7, ly + 1, 6, RGB(0x2d, 0x7a, 0x3a));
        gfx_fill(lx, ly + 6, 11, 9, RGB(0x2d, 0x7a, 0x3a));
        gfx_fill(lx + 5, ly + 9, 1, 3, RGB(0xe8, 0xf4, 0xea));
    }
    if (b->addr_focus && b->addr_sel)
        gfx_fill(tx - 1, c.y + 9, (int)strlen(show) * 8 + 2, 20, g_accent);
    gfx_text(tx, c.y + 11, show, b->addr_focus && b->addr_sel ? C_WHITE : C_TEXT);
    if (b->addr_focus && !b->addr_sel)
        gfx_fill(tx + (int)strlen(show) * 8, c.y + 10, 2, 18, g_accent);

    struct rect view = view_rect(c);
    gfx_fill(view.x, view.y, view.w, view.h, C_WHITE);
    struct page *pg = &b->page;
    if (css_focus != b->focus || css_hover != b->hover_node) {           /* the state is per window */
        css_focus = b->focus;
        css_hover = b->hover_node;
        if (pg->root) { pg->dirty_style = pg->dirty_style || pg->has_dyn_css || b->focus; pg->layout_w = 0; }
    }
    if (pg->root && (!pg->styled || (pg->dirty_style && !css_pending(pg)))) {
        css_restyle(pg, view.w);
        pg->layout_w = 0;
    }
    if (pg->layout_w != view.w)
        page_layout(pg, view.w);
    int ms = max_scroll(b, view);
    if (b->scroll > ms) b->scroll = ms;
    if (b->scroll < 0) b->scroll = 0;
    struct rect saved = gfx_get_clip();
    struct rect clip = rect_isect(saved, view);
    if (clip.w > 0) {
        gfx_set_clip(clip.x, clip.y, clip.w, clip.h);
        if (b->loading && !b->page.root)
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
    char logline[160];
    const char *lg = js_log(&b->page);
    if (b->hover < 0 && lg && *lg && !b->loading) {                     /* the newest console line */
        const char *end = lg + strlen(lg);
        if (end > lg && end[-1] == '\n') end--;
        const char *start = end;
        while (start > lg && start[-1] != '\n') start--;
        snprintf(logline, sizeof logline, "%s%.*s", js_stopped(&b->page) ? "Scripts stopped: " : "Console: ", (int)(end - start), start);
        st = logline;
    }
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
        int inside = x < view.w && y >= TOOL_H && y < TOOL_H + view.h;
        int link = inside ? page_link_at(&b->page, x, y - TOOL_H + b->scroll) : -1;
        if (link != b->hover) {
            b->hover = link;
            wm_invalidate(w);
        }
        if (b->page.has_dyn_css && b->page.root) {                      /* :hover rules: restyle when it changes */
            struct node *h = inside ? element_of(page_node_at(&b->page, x, y - TOOL_H + b->scroll)) : NULL;
            if (h != b->hover_node) {
                b->hover_node = h;
                wm_invalidate(w);
            }
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
    int px = x, py = y - TOOL_H + b->scroll;
    int link = page_link_at(&b->page, px, py);
    struct page *p = &b->page;
    struct node *el = p->root ? element_of(page_node_at(p, px, py)) : NULL;
    struct node *f = el;
    while (f && !focusable(f)) f = f->parent;
    set_focus(b, f && text_field(f) ? f : f && f->tag != TG_A ? f : NULL);
    if (el && p->js) {
        js_event(p, el, "mousedown", 0, 0);
        if (js_after(b)) return;
        js_event(p, el, "mouseup", 0, 0);
        if (js_after(b)) return;
        int prevented = js_event(p, el, "click", 0, 0);
        if (js_after(b)) return;
        if (kind == M_DBL) { js_event(p, el, "dblclick", 0, 0); if (js_after(b)) return; }
        if (prevented) { wm_invalidate(w); return; }
    }
    if (default_click(b, el, link))
        return;
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
    if (ctrl && (e->code == 'c' || e->code == 'C')) {             /* copy the address */
        clip_set(b->addr, strlen(b->addr));
        wm_notify("Address copied");
        return;
    }
    if (ctrl && (e->code == 'v' || e->code == 'V')) {             /* paste into the address bar */
        int n;
        const char *c = clip_get(&n);
        if (!c)
            return;
        if (!b->addr_focus || b->addr_sel) {
            b->alen = 0;
            b->addr_sel = 0;
        }
        b->addr_focus = 1;
        for (int i = 0; i < n && b->alen < (int)sizeof b->addr - 1; i++)
            if (c[i] > 32 && c[i] < 127)
                b->addr[b->alen++] = c[i];
        b->addr[b->alen] = 0;
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
    struct page *pg = &b->page;
    if (pg->js && !(e->flags & IEF_ALT)) {                          /* the page sees the key first */
        struct node *target = b->focus ? b->focus : pg->body;
        int prevented = js_event(pg, target, "keydown", e->code, e->flags);
        if (js_after(b)) return;
        if (prevented) { wm_invalidate(w); return; }
    }
    if (field_key(b, e)) { wm_invalidate(w); return; }
    if (b->focus && e->code == '\n' && b->focus->tag != TG_A) {    /* Enter on a button */
        default_click(b, b->focus, -1);
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

static int br_frame(struct win *w, u32 now)
{
    struct browser *b = w->data;
    if (!b->page.js || js_stopped(&b->page))
        return -1;
    int next = js_timers(&b->page);
    if (js_dirty(&b->page)) {
        b->page.dirty_style = 1;
        b->page.layout_w = 0;
        wm_invalidate(w);
    }
    const char *a = js_take_alert(&b->page);
    if (a)
        wm_notify(a);
    const char *nav = js_take_nav(&b->page);
    if (nav) {
        char u[512], t[512];
        safestrcpy(t, nav, sizeof t);
        url_resolve(b->page.url, t, u, sizeof u);
        navigate(b, u, 1);
        return -1;
    }
    return next;
}

static void br_close(struct win *w)
{
    struct browser *b = w->data;
    stop_fetch(b);
    page_free(&b->page);
    free(b);
}

static const struct app browser_app = {.draw = br_draw, .key = br_key, .mouse = br_mouse, .wheel = br_wheel,
                                       .tick = br_tick, .pollfds = br_pollfds, .fdready = br_fdready,
                                       .close = br_close, .frame = br_frame};

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
