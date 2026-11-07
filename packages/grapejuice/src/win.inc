/* GrapeJuice: the Win32 window system (USER).  Top-level windows become windows of the Petrichor desktop,
 * reached through the terminal's pipes (include/winproto.h); child controls are drawn here. */
#include "winproto.h"

typedef LRESULT (WINAPI *WNDPROC)(HANDLE, UINT, WPARAM, LPARAM);

enum {
    WM_CREATE = 1, WM_DESTROY = 2, WM_SIZE = 5, WM_SETFOCUS = 7, WM_KILLFOCUS = 8, WM_SETTEXT = 0xC, WM_GETTEXT = 0xD,
    WM_GETTEXTLENGTH = 0xE, WM_PAINT = 0xF, WM_CLOSE = 0x10, WM_QUIT = 0x12, WM_ERASEBKGND = 0x14, WM_SHOWWINDOW = 0x18,
    WM_NCCREATE = 0x81, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102, WM_COMMAND = 0x111, WM_TIMER = 0x113,
    WM_MOUSEMOVE = 0x200, WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202, WM_LBUTTONDBLCLK = 0x203, WM_RBUTTONDOWN = 0x204,
    WM_RBUTTONUP = 0x205, WM_MOUSEWHEEL = 0x20A, BM_GETCHECK = 0xF0, BM_SETCHECK = 0xF1,
};

#define WS_CHILD   0x40000000u
#define WS_VISIBLE 0x10000000u
#define WS_CAPTION 0x00C00000u
#define WS_DISABLED 0x08000000u
#define MAXWINDOWS 64
#define MAXMSGS 256
#define MAXTIMERS 16

struct wclass {
    char name[48];
    WNDPROC proc;
    struct gdiobj *bg;
    int used, builtin;                                   /* builtin: 1 button, 2 static, 3 edit */
};

struct window {
    int used, visible, enabled, shown;
    struct window *parent;
    struct wclass *cls;
    WNDPROC proc;
    u32 style, exstyle;
    char title[256];
    int x, y, w, h, id, pw_id;
    u32 *fb;                                             /* top-level windows: the client area's pixels */
    RECT dirty, inval;
    int has_dirty, has_inval;
    i64 userdata, extra[4];
    int pressed, checked, caret;
};

struct msg {
    HANDLE h;
    UINT message;
    WPARAM wparam;
    LPARAM lparam;
    DWORD time;
    POINT pt;
};

struct wtimer {
    int used;
    HANDLE h;
    u64 id;
    u32 interval, next;
    void *cb;
};

static struct wclass classes[32];
static struct window windows[MAXWINDOWS];
static struct msg mq[MAXMSGS];
static int mq_head, mq_count, quit_posted, quit_code;
static struct wtimer timers[MAXTIMERS];
static struct window *focus_win, *capture_win;
static int mouse_x, mouse_y, mouse_down, pw_ok = -1, next_pw_id = 1;

static HANDLE wh(struct window *w) { return (HANDLE)(u64)(0x10000 + (w - windows)); }

static struct window *W(HANDLE h)
{
    u64 v = (u64)h;
    if (v < 0x10000 || v >= 0x10000 + MAXWINDOWS) return NULL;
    struct window *w = &windows[v - 0x10000];
    return w->used ? w : NULL;
}

static struct window *top_of(struct window *w) { while (w && w->parent) w = w->parent; return w; }

static void abs_origin(struct window *w, int *x, int *y)
{
    *x = *y = 0;
    for (; w && w->parent; w = w->parent) { *x += w->x; *y += w->y; }
}

static void rect_union(RECT *a, int *has, int l, int t, int r, int b)
{
    if (!*has) { *a = (RECT){l, t, r, b}; *has = 1; return; }
    if (l < a->left) a->left = l;
    if (t < a->top) a->top = t;
    if (r > a->right) a->right = r;
    if (b > a->bottom) a->bottom = b;
}

static void mark_dirty(struct dc *d, int l, int t, int r, int b)
{
    if (!d->win) return;
    struct window *top = top_of(d->win);
    l += d->ox; r += d->ox; t += d->oy; b += d->oy;
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > top->w) r = top->w;
    if (b > top->h) b = top->h;
    if (r > l && b > t) rect_union(&top->dirty, &top->has_dirty, l, t, r, b);
}

/* ---- talking to the desktop ---- */

static int pw_available(void)
{
    if (pw_ok < 0) {
        struct stat st;
        pw_ok = fstat(PW_FD_UP, &st) == 0 && fstat(PW_FD_DOWN, &st) == 0;
    }
    return pw_ok;
}

static void pw_send(u32 type, u32 id, u32 a, u32 b, u32 c, const void *payload, u32 len)
{
    u8 buf[sizeof(struct pw_msg) + PW_MAX_ROW * 4 + 64];
    struct pw_msg m = {type, id, a, b, c, 0, len};
    if (len > PW_MAX_ROW * 4 + 64) return;
    memcpy(buf, &m, sizeof m);
    if (len) memcpy(buf + sizeof m, payload, len);
    out_text(PW_FD_UP, (const char *)buf, sizeof m + len);
}

static void present(struct window *top)
{
    if (!top->has_dirty || !top->shown || !top->fb) return;
    RECT r = top->dirty;
    top->has_dirty = 0;
    for (int y = r.top; y < r.bottom && y < top->h; y++)
        for (int x = r.left; x < r.right && x < top->w; x += PW_MAX_ROW) {
            int n = r.right - x < PW_MAX_ROW ? r.right - x : PW_MAX_ROW;
            if (x + n > top->w) n = top->w - x;
            pw_send(PW_PRESENT, top->pw_id, x, y, n, top->fb + y * top->w + x, n * 4);
        }
}

static void present_all(void)
{
    for (int i = 0; i < MAXWINDOWS; i++)
        if (windows[i].used && !windows[i].parent)
            present(&windows[i]);
}

static void ensure_shown(struct window *top)
{
    if (top->shown || top->parent) return;
    if (!pw_available()) {
        fprintf(2, "grape: this program has a window; run it from a terminal in the Petrichor desktop\n");
        exit(1);
    }
    top->shown = 1;
    pw_send(PW_CREATE, top->pw_id, top->w, top->h, 0, top->title, strlen(top->title));
    rect_union(&top->dirty, &top->has_dirty, 0, 0, top->w, top->h);
    rect_union(&top->inval, &top->has_inval, 0, 0, top->w, top->h);
}

/* ---- the message queue ---- */

static void post(struct window *w, UINT m, WPARAM wp, LPARAM lp)
{
    if (mq_count == MAXMSGS) return;
    struct msg *q = &mq[(mq_head + mq_count++) % MAXMSGS];
    q->h = w ? wh(w) : NULL;
    q->message = m; q->wparam = wp; q->lparam = lp;
    q->time = uptime() * 10;
    q->pt = (POINT){mouse_x, mouse_y};
}

static LRESULT call_proc(struct window *w, UINT m, WPARAM wp, LPARAM lp) { return w->proc(wh(w), m, wp, lp); }

/* ---- DCs for windows ---- */

static struct dc *window_dc(struct window *w, int clip_to_inval)
{
    struct window *top = top_of(w);
    struct dc *d = new_dc();
    if (!d || !top->fb) { free(d); return NULL; }
    dc_set_target(d, top->fb, top->w, top->h, 0);
    int ox, oy;
    abs_origin(w, &ox, &oy);
    d->ox = ox; d->oy = oy;
    d->win = w;
    d->clip = (RECT){ox > 0 ? ox : 0, oy > 0 ? oy : 0, ox + w->w < top->w ? ox + w->w : top->w, oy + w->h < top->h ? oy + w->h : top->h};
    if (clip_to_inval && top->has_inval) {
        if (top->inval.left > d->clip.left) d->clip.left = top->inval.left;
        if (top->inval.top > d->clip.top) d->clip.top = top->inval.top;
        if (top->inval.right < d->clip.right) d->clip.right = top->inval.right;
        if (top->inval.bottom < d->clip.bottom) d->clip.bottom = top->inval.bottom;
    }
    return d;
}

/* ---- built-in controls ---- */

#define BTN_FACE 0xE1E1E1
#define BTN_EDGE 0x707070

static void draw_control(struct window *c)
{
    struct dc *d = window_dc(c, 0);
    if (!d) return;
    int w = c->w, h = c->h, kind = c->cls->builtin;
    int type = c->style & 0xF;
    if (kind == 1) {                                                   /* BUTTON */
        if (type == 7) {                                               /* group box */
            dc_frame_rect(d, 0, 8, w, h, 0x909090, 1);
            int tw = strlen(c->title) * 8 + 8;
            dc_fill(d, 8, 0, 8 + tw, 16, 0xF0F0F0);
            d->textcolor = 0; d->bkmode = 1;
            dc_text(d, 12, 0, c->title, strlen(c->title));
        } else if (type == 2 || type == 3 || type == 4 || type == 9) { /* check box / radio */
            int bx = 0, by = (h - 13) / 2;
            if (type == 4 || type == 9) {
                dc_ellipse(d, bx, by, bx + 13, by + 13, 1, 0xFFFFFF, 1, 0x333333);
                if (c->checked) dc_ellipse(d, bx + 3, by + 3, bx + 10, by + 10, 1, 0x000000, 0, 0);
            } else {
                dc_fill(d, bx, by, bx + 13, by + 13, 0xFFFFFF);
                dc_frame_rect(d, bx, by, bx + 13, by + 13, 0x333333, 1);
                if (c->checked) { dc_line(d, bx + 3, by + 6, bx + 5, by + 9, 0, 1); dc_line(d, bx + 5, by + 9, bx + 10, by + 3, 0, 1); }
            }
            d->textcolor = c->enabled ? 0 : 0x808080; d->bkmode = 1;
            dc_text(d, 20, (h - 16) / 2, c->title, strlen(c->title));
        } else {                                                       /* push button */
            u32 face = c->pressed ? 0xCCE4F7 : BTN_FACE;
            dc_fill(d, 0, 0, w, h, face);
            dc_frame_rect(d, 0, 0, w, h, c->pressed ? 0x005499 : (focus_win == c || (c->style & 1) ? 0x0078D7 : BTN_EDGE), 1);
            int tw = strlen(c->title) * 8;
            d->textcolor = c->enabled ? 0 : 0x808080; d->bkmode = 1;
            dc_text(d, (w - tw) / 2, (h - 16) / 2, c->title, strlen(c->title));
        }
    } else if (kind == 2) {                                            /* STATIC */
        int al = c->style & 3, tw = strlen(c->title) * 8;
        int x = al == 1 ? (w - tw) / 2 : al == 2 ? w - tw : 0;
        d->textcolor = 0; d->bkmode = 1;
        RECT rc = {0, 0, w, h};
        if (!strchr(c->title, '\n') && tw <= w) dc_text(d, x, 0, c->title, strlen(c->title));
        else gdi_DrawTextA(d, c->title, -1, &rc, (al == 1 ? 1 : al == 2 ? 2 : 0) | 0x10);
    } else if (kind == 3) {                                            /* EDIT */
        dc_fill(d, 0, 0, w, h, 0xFFFFFF);
        dc_frame_rect(d, 0, 0, w, h, focus_win == c ? 0x0078D7 : 0x7A7A7A, 1);
        int len = strlen(c->title), vis = (w - 8) / 8, first = c->caret > vis - 1 ? c->caret - (vis - 1) : 0;
        char shown[256];
        int n = 0;
        for (int i = first; i < len && n < vis && n < 255; i++) shown[n++] = (c->style & 0x20) ? '*' : c->title[i];
        d->textcolor = 0; d->bkmode = 1;
        dc_text(d, 4, (h - 16) / 2, shown, n);
        if (focus_win == c) {
            int cx = 4 + (c->caret - first) * 8;
            dc_fill(d, cx, (h - 16) / 2, cx + 1, (h - 16) / 2 + 16, 0);
        }
    }
    mark_dirty(d, 0, 0, w, h);
    free(d);
}

static void invalidate_window(struct window *w, const RECT *rc)
{
    struct window *top = top_of(w);
    int ox, oy;
    abs_origin(w, &ox, &oy);
    RECT r = rc ? *rc : (RECT){0, 0, w->w, w->h};
    rect_union(&top->inval, &top->has_inval, r.left + ox, r.top + oy, r.right + ox, r.bottom + oy);
}

static void notify(struct window *c, int code)
{
    if (c->parent)
        call_proc(c->parent, WM_COMMAND, (u64)(u32)(code << 16 | (c->id & 0xFFFF)), (LPARAM)wh(c));
}

static LRESULT WINAPI ctl_proc(HANDLE h, UINT m, WPARAM wp, LPARAM lp)
{
    struct window *c = W(h);
    if (!c) return 0;
    int type = c->style & 0xF, kind = c->cls->builtin;
    switch (m) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        if (!c->enabled) return 0;
        if (kind == 1 || kind == 3) focus_win = c;
        if (kind == 1) { c->pressed = 1; capture_win = c; }
        if (kind == 3) {
            c->caret = ((short)(lp & 0xFFFF) - 4) / 8;
            if (c->caret < 0) c->caret = 0;
            if (c->caret > (int)strlen(c->title)) c->caret = strlen(c->title);
        }
        invalidate_window(c, NULL);
        return 0;
    case WM_LBUTTONUP:
        if (kind == 1 && c->pressed) {
            c->pressed = 0;
            capture_win = NULL;
            int x = (short)(lp & 0xFFFF), y = (short)(lp >> 16 & 0xFFFF);
            if (x >= 0 && y >= 0 && x < c->w && y < c->h) {
                if (type == 3 || type == 2) c->checked = !c->checked;
                if (type == 9 || type == 4) {                              /* radio: clear the others in the group */
                    if (c->parent)
                        for (int i = 0; i < MAXWINDOWS; i++)
                            if (windows[i].used && windows[i].parent == c->parent && windows[i].cls == c->cls && (windows[i].style & 0xF) == type)
                                windows[i].checked = 0;
                    c->checked = 1;
                }
                notify(c, 0);                                              /* BN_CLICKED */
            }
            invalidate_window(c, NULL);
        }
        return 0;
    case BM_GETCHECK: return c->checked;
    case BM_SETCHECK: c->checked = wp != 0; invalidate_window(c, NULL); return 0;
    case WM_SETTEXT:
        safestrcpy(c->title, (const char *)lp, sizeof c->title);
        c->caret = strlen(c->title);
        invalidate_window(c, NULL);
        return 1;
    case WM_CHAR:
        if (kind == 3 && c->enabled) {
            int len = strlen(c->title);
            if (wp == 8) { if (c->caret > 0) { memmove(c->title + c->caret - 1, c->title + c->caret, len - c->caret + 1); c->caret--; notify(c, 0x300); } }
            else if (wp >= 32 && wp < 127 && len < 254) { memmove(c->title + c->caret + 1, c->title + c->caret, len - c->caret + 1); c->title[c->caret++] = wp; notify(c, 0x300); }
            else if (wp == 13 && c->parent) { /* Enter: the dialog's default button is up to the application */ }
            invalidate_window(c, NULL);
        } else if (kind == 1 && wp == ' ' && focus_win == c) {
            if (type == 3 || type == 2) c->checked = !c->checked;
            notify(c, 0);
            invalidate_window(c, NULL);
        }
        return 0;
    case WM_KEYDOWN:
        if (kind == 3) {
            int len = strlen(c->title);
            if (wp == 0x25 && c->caret > 0) c->caret--;
            else if (wp == 0x27 && c->caret < len) c->caret++;
            else if (wp == 0x24) c->caret = 0;
            else if (wp == 0x23) c->caret = len;
            else if (wp == 0x2E && c->caret < len) { memmove(c->title + c->caret, c->title + c->caret + 1, len - c->caret); notify(c, 0x300); }
            invalidate_window(c, NULL);
        }
        return 0;
    case WM_GETTEXTLENGTH: return strlen(c->title);
    case WM_GETTEXT: { int n = strlen(c->title); if ((u64)n >= wp) n = wp ? wp - 1 : 0; memcpy((char *)lp, c->title, n); ((char *)lp)[n] = 0; return n; }
    }
    return 0;
}

/* ---- painting ---- */

static LRESULT WINAPI u_DefWindowProcA(HANDLE h, UINT m, WPARAM wp, LPARAM lp);

static void paint_top(struct window *top)
{
    if (!top->has_inval) return;
    struct window *w = top;
    ensure_shown(top);
    call_proc(w, WM_ERASEBKGND, (WPARAM)0, 0);
    call_proc(w, WM_PAINT, 0, 0);
    for (int i = 0; i < MAXWINDOWS; i++) {                             /* children draw over the parent */
        struct window *c = &windows[i];
        if (!c->used || c->parent != top || !c->visible) continue;
        if (c->cls->builtin) draw_control(c);
        else {
            struct dc *d = window_dc(c, 0);
            if (d) { dc_fill(d, 0, 0, c->w, c->h, c->cls->bg ? c->cls->bg->color : 0xFFFFFF); free(d); }
            call_proc(c, WM_PAINT, 0, 0);
        }
    }
    top->has_inval = 0;
}

/* ---- window procedures and messages ---- */

static void destroy_window(struct window *w)
{
    if (!w || !w->used) return;
    for (int i = 0; i < MAXWINDOWS; i++)                               /* children first */
        if (windows[i].used && windows[i].parent == w) destroy_window(&windows[i]);
    call_proc(w, WM_DESTROY, 0, 0);
    if (focus_win == w) focus_win = NULL;
    if (capture_win == w) capture_win = NULL;
    for (int i = 0; i < MAXTIMERS; i++) if (timers[i].used && timers[i].h == wh(w)) timers[i].used = 0;
    if (!w->parent && w->shown) pw_send(PW_DESTROY, w->pw_id, 0, 0, 0, NULL, 0);
    free(w->fb);
    w->used = 0;
}

static LRESULT WINAPI u_DefWindowProcA(HANDLE h, UINT m, WPARAM wp, LPARAM lp)
{
    struct window *w = W(h);
    if (!w) return 0;
    switch (m) {
    case WM_ERASEBKGND: {
        struct dc *d = window_dc(w, 1);
        if (d) {
            struct gdiobj *bg = w->cls->bg;
            u32 c = bg && (u64)bg > 64 ? bg->color : 0xF0F0F0;
            if (bg && (u64)bg > 64 && bg->style == 1) c = 0xF0F0F0;
            dc_fill(d, 0, 0, w->w, w->h, c);
            mark_dirty(d, 0, 0, w->w, w->h);
            free(d);
        }
        return 1;
    }
    case WM_CLOSE: destroy_window(w); return 0;
    case WM_SETTEXT:
        safestrcpy(w->title, (const char *)lp, sizeof w->title);
        if (!w->parent && w->shown) pw_send(PW_TITLE, w->pw_id, 0, 0, 0, w->title, strlen(w->title));
        return 1;
    case WM_GETTEXTLENGTH: return strlen(w->title);
    case WM_GETTEXT: { int n = strlen(w->title); if ((u64)n >= wp) n = wp ? wp - 1 : 0; memcpy((char *)lp, w->title, n); ((char *)lp)[n] = 0; return n; }
    }
    return 0;
}

static LRESULT WINAPI u_DefWindowProcW(HANDLE h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_SETTEXT) {
        const u16 *s = (const u16 *)lp;
        char tmp[256];
        int n = 0;
        while (s[n] && n < 255) { tmp[n] = s[n] < 128 ? (char)s[n] : '?'; n++; }
        tmp[n] = 0;
        return u_DefWindowProcA(h, m, wp, (LPARAM)tmp);
    }
    return u_DefWindowProcA(h, m, wp, lp);
}

static struct wclass *find_class(const char *name)
{
    for (int i = 0; i < 32; i++)
        if (classes[i].used && !strcmp(classes[i].name, name)) return &classes[i];
    return NULL;
}

static struct wclass *builtin_class(const char *name)
{
    static const char *const names[] = {"BUTTON", "STATIC", "EDIT"};
    for (int i = 0; i < 3; i++) {
        int ok = 1;
        for (int k = 0; names[i][k] || name[k]; k++)
            if ((name[k] | 32) != (names[i][k] | 32)) { ok = 0; break; }
        if (ok) {
            struct wclass *c = find_class(names[i]);
            if (!c) {
                for (int s = 0; s < 32; s++) if (!classes[s].used) { c = &classes[s]; break; }
                if (!c) return NULL;
                memset(c, 0, sizeof *c);
                safestrcpy(c->name, names[i], sizeof c->name);
                c->used = 1;
                c->builtin = i + 1;
                c->proc = ctl_proc;
            }
            return c;
        }
    }
    return NULL;
}

static u16 WINAPI u_RegisterClassA(const u8 *wc)
{
    struct wclass *c = NULL;
    for (int i = 0; i < 32; i++) if (!classes[i].used) { c = &classes[i]; break; }
    if (!c) return 0;
    memset(c, 0, sizeof *c);
    const char *name = (const char *)rd64(wc + 64);
    if ((u64)name < 0x10000) return 0;
    safestrcpy(c->name, name, sizeof c->name);
    c->proc = (WNDPROC)rd64(wc + 8);
    c->bg = (struct gdiobj *)rd64(wc + 48);
    c->used = 1;
    return 0xC000 + (c - classes);
}

static u16 WINAPI u_RegisterClassExA(const u8 *wc) { return u_RegisterClassA(wc); }

static u16 WINAPI u_RegisterClassW(const u8 *wc)
{
    u8 copy[80];
    memcpy(copy, wc, 80);
    const u16 *w = (const u16 *)rd64(wc + 64);
    static char narrow[48];
    int n = 0;
    while (w && w[n] && n < 47) { narrow[n] = w[n] < 128 ? (char)w[n] : '?'; n++; }
    narrow[n] = 0;
    u64 p = (u64)narrow;
    for (int i = 0; i < 8; i++) copy[64 + i] = p >> (8 * i);
    u16 r = u_RegisterClassA(copy);
    return r;
}

static HANDLE create_window(DWORD exstyle, const char *cls, const char *title, DWORD style, int x, int y, int w, int h, HANDLE parent, HANDLE menu, void *param)
{
    struct wclass *c = (u64)cls < 0x10000 ? &classes[(u64)cls - 0xC000 < 32 ? (u64)cls - 0xC000 : 0] : find_class(cls);
    if (!c || !c->used) c = (u64)cls >= 0x10000 ? builtin_class(cls) : NULL;
    if (!c) { last_error = 1407; return NULL; }
    struct window *win = NULL;
    for (int i = 0; i < MAXWINDOWS; i++) if (!windows[i].used) { win = &windows[i]; break; }
    if (!win) return NULL;
    memset(win, 0, sizeof *win);
    win->used = 1;
    win->cls = c;
    win->proc = c->proc ? c->proc : u_DefWindowProcA;
    win->style = style;
    win->exstyle = exstyle;
    win->enabled = !(style & WS_DISABLED);
    win->visible = (style & WS_VISIBLE) != 0;
    safestrcpy(win->title, title ? title : "", sizeof win->title);
    win->id = (int)(u64)menu;
    struct window *par = W(parent);
    if ((style & WS_CHILD) && par) {
        win->parent = par;
        win->x = x; win->y = y; win->w = w < 0 ? 0 : w; win->h = h < 0 ? 0 : h;
        if (c->builtin == 3) win->caret = strlen(win->title);
    } else {
        if ((u32)w == 0x80000000u) w = 640;
        if ((u32)h == 0x80000000u) h = 480;
        int cw = w, ch = h;
        if (style & WS_CAPTION) { cw -= 16; ch -= 39; if (menu) ch -= 20; }
        if (cw < 64) cw = 64;
        if (ch < 32) ch = 32;
        if (cw > PW_MAX_W) cw = PW_MAX_W;
        if (ch > PW_MAX_H) ch = PW_MAX_H;
        win->w = cw; win->h = ch;
        win->pw_id = next_pw_id++;
        win->fb = calloc((size_t)cw * ch, 4);
        if (!win->fb) { win->used = 0; return NULL; }
    }
    u8 cs[96];
    memset(cs, 0, sizeof cs);
    u64 vals[] = {(u64)param, 0, (u64)menu, (u64)parent};
    for (int k = 0; k < 4; k++) for (int i = 0; i < 8; i++) cs[k * 8 + i] = vals[k] >> (8 * i);
    *(i32 *)(cs + 32) = h; *(i32 *)(cs + 36) = w; *(i32 *)(cs + 40) = y; *(i32 *)(cs + 44) = x; *(u32 *)(cs + 48) = style;
    *(u64 *)(cs + 56) = (u64)title; *(u64 *)(cs + 64) = (u64)cls; *(u32 *)(cs + 72) = exstyle;
    LRESULT r = call_proc(win, WM_NCCREATE, 0, (LPARAM)cs);
    (void)r;
    if (call_proc(win, WM_CREATE, 0, (LPARAM)cs) == -1) { win->used = 0; return NULL; }
    if (!win->parent) {
        call_proc(win, WM_SIZE, 0, (win->h << 16) | (win->w & 0xFFFF));
        if (win->visible) { ensure_shown(win); invalidate_window(win, NULL); }
    } else {
        invalidate_window(win, NULL);
    }
    return wh(win);
}

static HANDLE WINAPI u_CreateWindowExA(DWORD ex, const char *cls, const char *title, DWORD style, int x, int y, int w, int h, HANDLE parent, HANDLE menu, HANDLE inst, void *param)
{
    return create_window(ex, cls, title, style, x, y, w, h, parent, menu, param);
}

static void narrow(char *out, int cap, const u16 *s)
{
    int n = 0;
    if ((u64)s < 0x10000) { out[0] = 0; return; }
    while (s[n] && n < cap - 1) { out[n] = s[n] < 128 ? (char)s[n] : '?'; n++; }
    out[n] = 0;
}

static HANDLE WINAPI u_CreateWindowExW(DWORD ex, const u16 *cls, const u16 *title, DWORD style, int x, int y, int w, int h, HANDLE parent, HANDLE menu, HANDLE inst, void *param)
{
    char c[64], t[256];
    if ((u64)cls < 0x10000) return create_window(ex, (const char *)cls, "", style, x, y, w, h, parent, menu, param);
    narrow(c, sizeof c, cls);
    narrow(t, sizeof t, title);
    return create_window(ex, c, t, style, x, y, w, h, parent, menu, param);
}

static BOOL WINAPI u_DestroyWindow(HANDLE h) { struct window *w = W(h); if (!w) return 0; destroy_window(w); return 1; }
static BOOL WINAPI u_ShowWindow(HANDLE h, int cmd)
{
    struct window *w = W(h);
    if (!w) return 0;
    int was = w->visible;
    w->visible = cmd != 0;
    if (w->visible) { ensure_shown(top_of(w)); invalidate_window(w, NULL); }
    return was;
}
static BOOL WINAPI u_UpdateWindow(HANDLE h) { struct window *w = W(h); if (w) paint_top(top_of(w)); return 1; }
static BOOL WINAPI u_InvalidateRect(HANDLE h, const RECT *rc, BOOL erase) { struct window *w = W(h); if (!w) return 0; invalidate_window(w, rc); return 1; }
static BOOL WINAPI u_ValidateRect(HANDLE h, const RECT *rc) { struct window *w = W(h); if (w) top_of(w)->has_inval = 0; return 1; }
static BOOL WINAPI u_IsWindow(HANDLE h) { return W(h) != NULL; }
static BOOL WINAPI u_IsWindowVisible(HANDLE h) { struct window *w = W(h); return w && w->visible; }
static BOOL WINAPI u_EnableWindow(HANDLE h, BOOL en) { struct window *w = W(h); if (!w) return 0; int old = !w->enabled; w->enabled = en != 0; invalidate_window(w, NULL); return old; }
static HANDLE WINAPI u_GetParent(HANDLE h) { struct window *w = W(h); return w && w->parent ? wh(w->parent) : NULL; }
static HANDLE WINAPI u_GetDlgItem(HANDLE h, int id)
{
    struct window *p = W(h);
    for (int i = 0; p && i < MAXWINDOWS; i++) if (windows[i].used && windows[i].parent == p && windows[i].id == id) return wh(&windows[i]);
    return NULL;
}
static BOOL WINAPI u_GetClientRect(HANDLE h, RECT *rc) { struct window *w = W(h); if (!w) return 0; *rc = (RECT){0, 0, w->w, w->h}; return 1; }
static BOOL WINAPI u_GetWindowRect(HANDLE h, RECT *rc) { struct window *w = W(h); if (!w) return 0; *rc = (RECT){w->x, w->y, w->x + w->w + (!w->parent ? 16 : 0), w->y + w->h + (!w->parent ? 39 : 0)}; return 1; }
static BOOL WINAPI u_AdjustWindowRect(RECT *rc, DWORD style, BOOL menu) { if (style & WS_CAPTION) { rc->right += 16; rc->bottom += 39 + (menu ? 20 : 0); } return 1; }
static BOOL WINAPI u_ClientToScreen(HANDLE h, POINT *p) { return 1; }
static BOOL WINAPI u_ScreenToClient(HANDLE h, POINT *p) { return 1; }
static BOOL WINAPI u_GetCursorPos(POINT *p) { p->x = mouse_x; p->y = mouse_y; return 1; }
static BOOL WINAPI u_MoveWindow(HANDLE h, int x, int y, int w, int cy, BOOL repaint)
{
    struct window *win = W(h);
    if (!win) return 0;
    if (win->parent) { win->x = x; win->y = y; win->w = w; win->h = cy; invalidate_window(win, NULL); }
    return 1;
}
static BOOL WINAPI u_SetWindowPos(HANDLE h, HANDLE after, int x, int y, int w, int cy, UINT flags) { if (!(flags & 2) && !(flags & 1)) return u_MoveWindow(h, x, y, w, cy, 1); return 1; }
static BOOL WINAPI u_SetWindowTextA(HANDLE h, const char *s) { struct window *w = W(h); if (!w) return 0; call_proc(w, WM_SETTEXT, 0, (LPARAM)s); return 1; }
static BOOL WINAPI u_SetWindowTextW(HANDLE h, const u16 *s) { char t[256]; narrow(t, sizeof t, s); return u_SetWindowTextA(h, t); }
static int WINAPI u_GetWindowTextA(HANDLE h, char *buf, int n) { struct window *w = W(h); if (!w || n <= 0) return 0; return (int)call_proc(w, WM_GETTEXT, n, (LPARAM)buf); }
static int WINAPI u_GetWindowTextLengthA(HANDLE h) { struct window *w = W(h); return w ? (int)strlen(w->title) : 0; }
static HANDLE WINAPI u_SetFocus(HANDLE h)
{
    struct window *w = W(h), *old = focus_win;
    if (old && old != w) { call_proc(old, WM_KILLFOCUS, 0, 0); invalidate_window(old, NULL); }
    focus_win = w;
    if (w) { call_proc(w, WM_SETFOCUS, 0, 0); invalidate_window(w, NULL); }
    return old ? wh(old) : NULL;
}
static HANDLE WINAPI u_GetFocus(void) { return focus_win ? wh(focus_win) : NULL; }
static HANDLE WINAPI u_SetCapture(HANDLE h) { struct window *old = capture_win; capture_win = W(h); return old ? wh(old) : NULL; }
static BOOL WINAPI u_ReleaseCapture(void) { capture_win = NULL; return 1; }
static HANDLE WINAPI u_GetCapture(void) { return capture_win ? wh(capture_win) : NULL; }

static i64 WINAPI u_GetWindowLongPtrA(HANDLE h, int idx)
{
    struct window *w = W(h);
    if (!w) return 0;
    switch (idx) {
    case -4: return (i64)w->proc;
    case -6: return (i64)image_base;
    case -8: return w->parent ? (i64)wh(w->parent) : 0;
    case -12: return w->id;
    case -16: return w->style;
    case -20: return w->exstyle;
    case -21: return w->userdata;
    }
    return idx >= 0 && idx < 32 ? w->extra[idx / 8] : 0;
}

static i64 WINAPI u_SetWindowLongPtrA(HANDLE h, int idx, i64 v)
{
    struct window *w = W(h);
    if (!w) return 0;
    i64 old = u_GetWindowLongPtrA(h, idx);
    switch (idx) {
    case -4: w->proc = (WNDPROC)v; break;
    case -12: w->id = (int)v; break;
    case -16: w->style = (u32)v; break;
    case -20: w->exstyle = (u32)v; break;
    case -21: w->userdata = v; break;
    default: if (idx >= 0 && idx < 32) w->extra[idx / 8] = v;
    }
    return old;
}

static HANDLE WINAPI u_GetDC(HANDLE h) { struct window *w = W(h); return w ? window_dc(w, 0) : NULL; }
static int WINAPI u_ReleaseDC(HANDLE h, HANDLE dc) { free(dc); return 1; }

static HANDLE WINAPI u_BeginPaint(HANDLE h, u8 *ps)
{
    struct window *w = W(h);
    if (!w) return NULL;
    struct dc *d = window_dc(w, 1);
    memset(ps, 0, 72);
    *(u64 *)ps = (u64)d;
    struct window *top = top_of(w);
    int ox, oy;
    abs_origin(w, &ox, &oy);
    RECT r = top->has_inval ? (RECT){top->inval.left - ox, top->inval.top - oy, top->inval.right - ox, top->inval.bottom - oy} : (RECT){0, 0, w->w, w->h};
    memcpy(ps + 12, &r, sizeof r);
    return d;
}

static BOOL WINAPI u_EndPaint(HANDLE h, const u8 *ps) { free((void *)rd64(ps)); return 1; }

static LRESULT WINAPI u_SendMessageA(HANDLE h, UINT m, WPARAM wp, LPARAM lp)
{
    struct window *w = W(h);
    if (!w) return 0;
    if (m == WM_PAINT) { paint_top(top_of(w)); return 0; }
    return call_proc(w, m, wp, lp);
}

static BOOL WINAPI u_PostMessageA(HANDLE h, UINT m, WPARAM wp, LPARAM lp) { post(W(h), m, wp, lp); return 1; }
static void WINAPI u_PostQuitMessage(int code) { quit_posted = 1; quit_code = code; }

static void make_timer_messages(void)
{
    u32 now = uptime() * 10;
    for (int i = 0; i < MAXTIMERS; i++)
        if (timers[i].used && (i32)(now - timers[i].next) >= 0) {
            timers[i].next = now + timers[i].interval;
            if (timers[i].cb) ((void (WINAPI *)(HANDLE, UINT, WPARAM, DWORD))timers[i].cb)(timers[i].h, WM_TIMER, timers[i].id, now);
            else post(W(timers[i].h), WM_TIMER, timers[i].id, 0);
        }
}

static u64 WINAPI u_SetTimer(HANDLE h, u64 id, UINT ms, void *cb)
{
    for (int i = 0; i < MAXTIMERS; i++)
        if (!timers[i].used) {
            timers[i] = (struct wtimer){1, h, h ? id : (u64)(1000 + i), ms ? ms : 1, uptime() * 10 + ms, cb};
            return timers[i].id;
        }
    return 0;
}

static BOOL WINAPI u_KillTimer(HANDLE h, u64 id)
{
    for (int i = 0; i < MAXTIMERS; i++) if (timers[i].used && timers[i].h == h && timers[i].id == id) { timers[i].used = 0; return 1; }
    return 0;
}

/* ---- input from the desktop ---- */

static struct window *window_by_pw(int id)
{
    for (int i = 0; i < MAXWINDOWS; i++) if (windows[i].used && !windows[i].parent && windows[i].pw_id == id) return &windows[i];
    return NULL;
}

static struct window *hit_child(struct window *top, int x, int y)
{
    struct window *best = NULL;
    for (int i = 0; i < MAXWINDOWS; i++) {
        struct window *c = &windows[i];
        if (c->used && c->parent == top && c->visible && c->enabled && c->cls->builtin != 2 &&
            x >= c->x && y >= c->y && x < c->x + c->w && y < c->y + c->h)
            best = c;
    }
    return best;
}

static int vk_of(u32 code, int *ch)
{
    *ch = 0;
    if (code >= 'a' && code <= 'z') { *ch = code; return code - 32; }
    if (code >= 'A' && code <= 'Z') { *ch = code; return code; }
    if (code >= '0' && code <= '9') { *ch = code; return code; }
    switch (code) {
    case ' ': *ch = ' '; return 0x20;
    case '\n': *ch = 13; return 0x0D;
    case 8: *ch = 8; return 0x08;
    case 9: *ch = 9; return 0x09;
    case 27: return 0x1B;
    case KEY_UP: return 0x26; case KEY_DOWN: return 0x28; case KEY_LEFT: return 0x25; case KEY_RIGHT: return 0x27;
    case KEY_HOME: return 0x24; case KEY_END: return 0x23; case KEY_PGUP: return 0x21; case KEY_PGDN: return 0x22;
    case KEY_DELETE: return 0x2E; case KEY_INSERT: return 0x2D;
    }
    if (code >= KEY_F1 && code < KEY_F1 + 10) return 0x70 + (code - KEY_F1);
    if (code >= 32 && code < 127) { *ch = code; return code; }
    return 0;
}

static void handle_event(const struct pw_event *e)
{
    struct window *top = window_by_pw(e->id);
    if (!top) return;
    switch (e->type) {
    case PE_CLOSE: post(top, WM_CLOSE, 0, 0); break;
    case PE_WHEEL: post(focus_win && top_of(focus_win) == top ? focus_win : top, WM_MOUSEWHEEL, (u64)(u32)((e->a == 1 ? -120 : 120) << 16), (u64)(u32)(mouse_y << 16 | (mouse_x & 0xFFFF))); break;
    case PE_KEY: {
        int ch;
        int vk = vk_of(e->a, &ch);
        struct window *t = focus_win && top_of(focus_win) == top ? focus_win : top;
        if (!vk && !ch) break;
        post(t, WM_KEYDOWN, vk, 0);
        if (ch) post(t, WM_CHAR, ch, 0);
        post(t, WM_KEYUP, vk, 0);
        break;
    }
    case PE_MOUSE: {
        int x = e->a, y = e->b, k = e->c;
        mouse_x = x; mouse_y = y;
        struct window *t = capture_win && top_of(capture_win) == top ? capture_win : hit_child(top, x, y);
        if (!t) t = top;
        int ox, oy;
        abs_origin(t, &ox, &oy);
        LPARAM lp = ((u32)(y - oy) << 16) | ((u32)(x - ox) & 0xFFFF);
        u32 msg = k == 1 ? WM_LBUTTONDOWN : k == 2 ? WM_LBUTTONUP : k == 3 ? WM_RBUTTONDOWN : k == 4 ? WM_LBUTTONDBLCLK : WM_MOUSEMOVE;
        if (k == 1 || k == 4) mouse_down = 1;
        if (k == 1 || k == 4) {                                     /* clicking elsewhere takes the focus away from an edit box */
            if (focus_win && focus_win != t && focus_win->cls->builtin == 3) { struct window *o = focus_win; focus_win = NULL; invalidate_window(o, NULL); }
        }
        post(t, msg, mouse_down ? 1 : 0, lp);
        if (k == 2) mouse_down = 0;
        if (k == 3) post(t, WM_RBUTTONUP, 0, lp);
        break;
    }
    }
}

static void pump_events(int timeout_ms)
{
    if (!pw_available()) { if (timeout_ms > 0) msleep(timeout_ms); return; }
    struct pollfd p = {PW_FD_DOWN, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0 || !(p.revents & POLLIN)) return;
    struct pw_event ev[16];
    int n = read(PW_FD_DOWN, ev, sizeof ev);
    for (int i = 0; i < n / (int)sizeof ev[0]; i++) handle_event(&ev[i]);
}

static int next_message(struct msg *out, int block)
{
    for (;;) {
        if (mq_count) { *out = mq[mq_head]; mq_head = (mq_head + 1) % MAXMSGS; mq_count--; return 1; }
        if (quit_posted) { memset(out, 0, sizeof *out); out->message = WM_QUIT; out->wparam = quit_code; quit_posted = 0; return 1; }
        make_timer_messages();
        if (mq_count) continue;
        for (int i = 0; i < MAXWINDOWS; i++)
            if (windows[i].used && !windows[i].parent && windows[i].has_inval && windows[i].visible) { post(&windows[i], WM_PAINT, 0, 0); break; }
        if (mq_count) continue;
        present_all();
        if (!block) { pump_events(0); return mq_count || quit_posted; }
        u32 now = uptime() * 10;
        int wait = -1;
        for (int i = 0; i < MAXTIMERS; i++)
            if (timers[i].used) { int d = (i32)(timers[i].next - now); if (d < 0) d = 0; if (wait < 0 || d < wait) wait = d; }
        pump_events(wait);
    }
}

static BOOL WINAPI u_GetMessageA(struct msg *m, HANDLE h, UINT lo, UINT hi) { next_message(m, 1); return m->message != WM_QUIT; }
static BOOL WINAPI u_PeekMessageA(struct msg *m, HANDLE h, UINT lo, UINT hi, UINT remove)
{
    if (!next_message(m, 0)) return 0;
    if (!(remove & 1)) { mq_head = (mq_head + MAXMSGS - 1) % MAXMSGS; mq[mq_head] = *m; mq_count++; }
    return 1;
}
static BOOL WINAPI u_TranslateMessage(const struct msg *m) { return 0; }
static LRESULT WINAPI u_DispatchMessageA(const struct msg *m)
{
    struct window *w = W(m->h);
    if (!w) return 0;
    if (m->message == WM_PAINT) { paint_top(top_of(w)); return 0; }
    if (m->message == WM_TIMER && 0) return 0;
    return call_proc(w, m->message, m->wparam, m->lparam);
}

/* ---- MessageBox: a real dialog window with buttons ---- */

static int mb_result;

static LRESULT WINAPI msgbox_proc(HANDLE h, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_COMMAND) { mb_result = (int)(wp & 0xFFFF); return 0; }
    if (m == WM_CLOSE) { mb_result = 2; return 0; }
    return u_DefWindowProcA(h, m, wp, lp);
}

static int WINAPI u_MessageBoxA(HANDLE wnd, const char *text, const char *caption, DWORD type)
{
    if (!pw_available()) {
        printf("[%s] %s\n", caption ? caption : "Message", text ? text : "");
        return 1;
    }
    struct wclass *c = find_class("GrapeMessageBox");
    if (!c) {
        for (int i = 0; i < 32; i++) if (!classes[i].used) { c = &classes[i]; break; }
        memset(c, 0, sizeof *c);
        safestrcpy(c->name, "GrapeMessageBox", sizeof c->name);
        c->used = 1;
        c->proc = msgbox_proc;
    }
    int tl = text ? strlen(text) : 0, tw = tl * 8 > 320 ? 320 : tl * 8;
    int cw = tw + 60 < 220 ? 220 : tw + 60, lines = tl * 8 / (tw ? tw : 1) + 1;
    int ch = 40 + lines * 16 + 50;
    HANDLE d = create_window(0, "GrapeMessageBox", caption ? caption : "Message", WS_CAPTION | 0x10000000, 0, 0, cw + 16, ch + 39, NULL, NULL, NULL);
    if (!d) return 0;
    create_window(0, "STATIC", text, WS_CHILD | WS_VISIBLE, 20, 20, cw - 40, lines * 16, d, (HANDLE)0, NULL);
    int kind = type & 0xF;
    static const int ids[6][3] = {{1, 0, 0}, {1, 2, 0}, {3, 4, 5}, {6, 7, 2}, {6, 7, 0}, {4, 2, 0}};
    static const char *const labels[8] = {"", "OK", "Cancel", "Abort", "Retry", "Ignore", "Yes", "No"};
    int nb = 0;
    while (nb < 3 && kind < 6 && ids[kind][nb]) nb++;
    for (int i = 0; i < nb; i++) {
        int id = ids[kind][i];
        create_window(0, "BUTTON", labels[id], WS_CHILD | WS_VISIBLE | (i == 0 ? 1 : 0), cw / 2 - nb * 45 + i * 90 + 5, ch - 44, 80, 26, d, (HANDLE)(u64)id, NULL);
    }
    mb_result = 0;
    struct msg m;
    while (!mb_result && next_message(&m, 1) && m.message != WM_QUIT)
        u_DispatchMessageA(&m);
    destroy_window(W(d));
    present_all();
    return mb_result ? mb_result : 2;
}

static int WINAPI u_MessageBoxW(HANDLE wnd, const u16 *text, const u16 *caption, DWORD type)
{
    char t[512], c[128];
    narrow(t, sizeof t, text);
    narrow(c, sizeof c, caption);
    return u_MessageBoxA(wnd, t, c, type);
}

/* ---- the rest of USER ---- */

static HANDLE WINAPI u_LoadCursorA(HANDLE inst, const char *name) { return (HANDLE)1; }
static HANDLE WINAPI u_LoadIconA(HANDLE inst, const char *name) { return (HANDLE)1; }
static HANDLE WINAPI u_LoadImageA(HANDLE inst, const char *name, UINT type, int cx, int cy, UINT flags) { return (HANDLE)1; }
static HANDLE WINAPI u_SetCursor(HANDLE c) { return (HANDLE)1; }
static int WINAPI u_ShowCursor(BOOL show) { return show ? 1 : 0; }
static int WINAPI u_GetSystemMetrics(int i)
{
    switch (i) { case 0: return 1024; case 1: return 768; case 2: case 3: return 17; case 4: return 23; case 5: case 6: return 1; case 15: return 19; case 32: case 33: return 4; default: return 0; }
}
static COLORREF WINAPI u_GetSysColor(int i) { switch (i) { case 5: case 4: case 15: return 0xF0F0F0; case 8: return 0; case 1: return 0x800000; default: return 0xD8D8D8; } }
static HANDLE WINAPI u_GetSysColorBrush(int i) { struct gdiobj *o = new_obj(GO_BRUSH); if (o) o->color = cref(u_GetSysColor(i)); return o; }
static i16 WINAPI u_GetKeyState(int vk) { return vk == 1 && mouse_down ? (i16)0x8000 : 0; }
static int WINAPI u_wsprintfA(char *buf, const char *fmt, ...)
{
    ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int n = ms_vsnprintf(buf, 1 << 20, fmt, ap);
    __builtin_ms_va_end(ap);
    if (n >= 0) buf[n] = 0;
    return n;
}
static HANDLE WINAPI u_CreateMenu(void) { return (HANDLE)1; }
static BOOL WINAPI u_SetMenu(HANDLE h, HANDLE menu) { return 1; }
static BOOL WINAPI u_AppendMenuA(HANDLE m, UINT f, u64 id, const char *s) { return 1; }
static BOOL WINAPI u_DrawMenuBar(HANDLE h) { return 1; }
static BOOL WINAPI u_SetForegroundWindow(HANDLE h) { return 1; }
static BOOL WINAPI u_IsDialogMessage(HANDLE h, struct msg *m) { return 0; }
static BOOL WINAPI u_CheckDlgButton(HANDLE h, int id, UINT st) { struct window *c = W(u_GetDlgItem(h, id)); if (c) { c->checked = st == 1; invalidate_window(c, NULL); } return 1; }
static UINT WINAPI u_IsDlgButtonChecked(HANDLE h, int id) { struct window *c = W(u_GetDlgItem(h, id)); return c && c->checked; }
static BOOL WINAPI u_SetDlgItemTextA(HANDLE h, int id, const char *s) { return u_SetWindowTextA(u_GetDlgItem(h, id), s); }
static UINT WINAPI u_GetDlgItemTextA(HANDLE h, int id, char *buf, int n) { return u_GetWindowTextA(u_GetDlgItem(h, id), buf, n); }

static void destroy_all_windows(void)
{
    for (int i = 0; i < MAXWINDOWS; i++)
        if (windows[i].used && !windows[i].parent && windows[i].shown)
            pw_send(PW_DESTROY, windows[i].pw_id, 0, 0, 0, NULL, 0);
}

#define U(n) {#n, (void *)u_##n}
#define G(n) {#n, (void *)gdi_##n}
static const struct imp gui_imports[] = {
    U(RegisterClassA), U(RegisterClassW), U(RegisterClassExA), {"RegisterClassExW", (void *)u_RegisterClassW},
    U(CreateWindowExA), U(CreateWindowExW), U(DestroyWindow), U(ShowWindow), U(UpdateWindow), U(InvalidateRect), U(ValidateRect),
    U(IsWindow), U(IsWindowVisible), U(EnableWindow), U(GetParent), U(GetDlgItem), U(GetClientRect), U(GetWindowRect),
    U(AdjustWindowRect), U(ClientToScreen), U(ScreenToClient), U(GetCursorPos), U(MoveWindow), U(SetWindowPos), U(SetWindowTextA),
    U(SetWindowTextW), U(GetWindowTextA), U(GetWindowTextLengthA), U(SetFocus), U(GetFocus), U(SetCapture), U(ReleaseCapture),
    U(GetCapture), U(GetWindowLongPtrA), U(SetWindowLongPtrA), {"GetWindowLongPtrW", (void *)u_GetWindowLongPtrA},
    {"SetWindowLongPtrW", (void *)u_SetWindowLongPtrA}, {"GetWindowLongA", (void *)u_GetWindowLongPtrA},
    {"SetWindowLongA", (void *)u_SetWindowLongPtrA}, {"GetWindowLongW", (void *)u_GetWindowLongPtrA},
    {"SetWindowLongW", (void *)u_SetWindowLongPtrA}, U(GetDC), U(ReleaseDC), U(BeginPaint), U(EndPaint), U(SendMessageA),
    {"SendMessageW", (void *)u_SendMessageA}, U(PostMessageA), {"PostMessageW", (void *)u_PostMessageA}, U(PostQuitMessage),
    U(SetTimer), U(KillTimer), U(GetMessageA), {"GetMessageW", (void *)u_GetMessageA}, U(PeekMessageA),
    {"PeekMessageW", (void *)u_PeekMessageA}, U(TranslateMessage), U(DispatchMessageA), {"DispatchMessageW", (void *)u_DispatchMessageA},
    U(DefWindowProcA), U(DefWindowProcW), U(MessageBoxA), U(MessageBoxW), U(LoadCursorA), {"LoadCursorW", (void *)u_LoadCursorA},
    U(LoadIconA), {"LoadIconW", (void *)u_LoadIconA}, U(LoadImageA), U(SetCursor), U(ShowCursor), U(GetSystemMetrics), U(GetSysColor),
    U(GetSysColorBrush), U(GetKeyState), {"GetAsyncKeyState", (void *)u_GetKeyState}, U(wsprintfA), U(CreateMenu), U(SetMenu),
    U(AppendMenuA), U(DrawMenuBar), U(SetForegroundWindow), U(IsDialogMessage), U(CheckDlgButton), U(IsDlgButtonChecked),
    U(SetDlgItemTextA), U(GetDlgItemTextA),
    G(GetStockObject), G(CreateSolidBrush), G(CreatePen), G(CreateFontA), G(CreateFontIndirectA), G(SelectObject), G(DeleteObject),
    G(CreateCompatibleDC), G(DeleteDC), G(CreateCompatibleBitmap), G(CreateDIBSection), G(SetTextColor), G(SetBkColor), G(SetBkMode),
    G(GetDeviceCaps), G(SetPixel), G(GetPixel), G(MoveToEx), G(LineTo), G(Rectangle), G(RoundRect), G(Ellipse), G(FillRect), G(FrameRect),
    G(Polygon), G(TextOutA), G(TextOutW), G(ExtTextOutA), G(GetTextExtentPoint32A), G(GetTextExtentPoint32W), G(GetTextMetricsA),
    G(DrawTextA), G(DrawTextW), G(BitBlt), G(StretchBlt), G(StretchDIBits), G(SetDIBitsToDevice), G(SaveDC), G(RestoreDC), G(SetROP2),
    G(SetMapMode), G(SetStretchBltMode), G(IntersectClipRect),
};
