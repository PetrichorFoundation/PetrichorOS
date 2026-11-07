/* Text editor: one flat buffer, line numbers, selection (Shift+arrows, mouse drag), clipboard (Ctrl+C/X/V),
 * undo/redo (Ctrl+Z/Y), Ctrl+A, Ctrl+S to save; asks before closing with unsaved changes. */
#include "desk.h"

#define EMAX    (1024 * 1024)
#define NUNDO   100
#define UNDO_BYTES (8 * 1024 * 1024)
#define TOOL_H  30
#define STAT_H  22
#define GUTTER  44

struct editor {
    struct win *win;
    char path[PATH_MAX];
    char *buf;
    int len, cur, top, left, modified;
    int want_col;                       /* column to return to on Up/Down */
    char status[80];
    int anchor;                         /* selection is [anchor, cur) in either order; -1 = none */
    struct snap { char *buf; int len, cur; } undo[NUNDO], redo[NUNDO];
    int nundo, nredo, last_kind, last_end, close_warned;
};

enum { K_NONE, K_TYPE, K_DEL, K_OTHER };

static int sel_range(struct editor *e, int *a, int *b)
{
    if (e->anchor < 0 || e->anchor == e->cur)
        return 0;
    *a = e->anchor < e->cur ? e->anchor : e->cur;
    *b = e->anchor < e->cur ? e->cur : e->anchor;
    return 1;
}

static void snap_free(struct snap *s) { free(s->buf); s->buf = NULL; }

static int snap_take(struct editor *e, struct snap *s)
{
    s->buf = malloc(e->len + 1);
    if (!s->buf)
        return -1;
    memcpy(s->buf, e->buf, e->len);
    s->len = e->len;
    s->cur = e->cur;
    return 0;
}

static void push(struct snap *stack, int *n, struct snap s)
{
    if (*n == NUNDO) {
        snap_free(&stack[0]);
        memmove(stack, stack + 1, (NUNDO - 1) * sizeof *stack);
        (*n)--;
    }
    stack[(*n)++] = s;
}

/* Saves the state before an edit.  Runs of typing (or of deleting) at one spot make one undo step. */
static void before_edit(struct editor *e, int kind)
{
    if (kind != K_OTHER && kind == e->last_kind && e->cur == e->last_end)
        return;
    long total = 0;
    for (int i = 0; i < e->nundo; i++)
        total += e->undo[i].len;
    while (e->nundo && total + e->len > UNDO_BYTES) {          /* keep undo memory bounded */
        total -= e->undo[0].len;
        snap_free(&e->undo[0]);
        memmove(e->undo, e->undo + 1, (e->nundo - 1) * sizeof e->undo[0]);
        e->nundo--;
    }
    struct snap s;
    if (snap_take(e, &s) == 0)
        push(e->undo, &e->nundo, s);
    while (e->nredo)
        snap_free(&e->redo[--e->nredo]);
}

static void after_edit(struct editor *e, int kind)
{
    e->last_kind = kind;
    e->last_end = e->cur;
    e->close_warned = 0;
}

static void restore(struct editor *e, struct snap *from, int *nfrom, struct snap *to, int *nto)
{
    if (!*nfrom)
        return;
    struct snap cur;
    if (snap_take(e, &cur) < 0)
        return;
    push(to, nto, cur);
    struct snap s = from[--*nfrom];
    memcpy(e->buf, s.buf, s.len);
    e->len = s.len;
    e->cur = s.cur > s.len ? s.len : s.cur;
    free(s.buf);
    e->anchor = -1;
    e->last_kind = K_NONE;
    e->modified = 1;
}

static int line_start(struct editor *e, int pos)
{
    while (pos > 0 && e->buf[pos - 1] != '\n')
        pos--;
    return pos;
}

static int line_end(struct editor *e, int pos)
{
    while (pos < e->len && e->buf[pos] != '\n')
        pos++;
    return pos;
}

static int vcol(struct editor *e, int ls, int pos)      /* visual column; tabs are 4 wide */
{
    int col = 0;
    for (int i = ls; i < pos; i++)
        col += e->buf[i] == '\t' ? 4 - col % 4 : 1;
    return col;
}

static int pos_at_vcol(struct editor *e, int ls, int want)
{
    int col = 0, i = ls;
    int le = line_end(e, ls);
    while (i < le) {
        int next = col + (e->buf[i] == '\t' ? 4 - col % 4 : 1);
        if (next > want)
            break;
        col = next;
        i++;
    }
    return i;
}

static int line_of(struct editor *e, int pos)
{
    int n = 0;
    for (int i = 0; i < pos; i++)
        n += e->buf[i] == '\n';
    return n;
}

static int rows_vis(struct rect c)
{
    return (c.h - TOOL_H - STAT_H) / 16;
}

static void update_title(struct editor *e)
{
    const char *name = e->path[0] ? strrchr(e->path, '/') + 1 : "Untitled";
    char t[64];
    snprintf(t, sizeof t, "%s%s - Text Editor", e->modified ? "*" : "", name);
    wm_set_title(e->win, t);
}

static void save(struct editor *e)
{
    if (!e->path[0]) {
        struct stat st;
        safestrcpy(e->path, stat("/disk", &st) == 0 ? "/disk/untitled.txt" : "/tmp/untitled.txt", sizeof e->path);
    }
    int fd = open(e->path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        snprintf(e->status, sizeof e->status, "Save failed: %s", strerror(fd));
        return;
    }
    int w = write(fd, e->buf, e->len);
    close(fd);
    if (w != e->len) {
        snprintf(e->status, sizeof e->status, "Save failed: %s", strerror(w < 0 ? w : -ENOSPC));
        return;
    }
    e->modified = 0;
    snprintf(e->status, sizeof e->status, "Saved %d bytes to %s", e->len, e->path);
    wm_notify("File saved");
    update_title(e);
}

static void insert(struct editor *e, const char *s, int n)
{
    if (e->len + n > EMAX) {
        snprintf(e->status, sizeof e->status, "Buffer full (%d bytes)", EMAX);
        return;
    }
    memmove(e->buf + e->cur + n, e->buf + e->cur, e->len - e->cur);
    memcpy(e->buf + e->cur, s, n);
    e->len += n;
    e->cur += n;
    if (!e->modified) {
        e->modified = 1;
        update_title(e);
    }
}

static void erase(struct editor *e, int from, int n)
{
    memmove(e->buf + from, e->buf + from + n, e->len - from - n);
    e->len -= n;
    e->cur = from;
    if (!e->modified) {
        e->modified = 1;
        update_title(e);
    }
}

static int erase_sel(struct editor *e)
{
    int a, b;
    if (!sel_range(e, &a, &b))
        return 0;
    erase(e, a, b - a);
    e->anchor = -1;
    return 1;
}

static void copy_sel(struct editor *e)
{
    int a, b;
    if (sel_range(e, &a, &b))
        clip_set(e->buf + a, b - a);
}

static void paste(struct editor *e)
{
    int n;
    const char *t = clip_get(&n);
    if (!t)
        return;
    before_edit(e, K_OTHER);
    erase_sel(e);
    insert(e, t, n);
    after_edit(e, K_OTHER);
}

static void scroll_to_cursor(struct editor *e, struct rect c)
{
    int rows = rows_vis(c), cols = (c.w - GUTTER - 8) / 8;
    int ln = line_of(e, e->cur);
    if (rows < 1) rows = 1;
    if (ln < e->top) e->top = ln;
    if (ln >= e->top + rows) e->top = ln - rows + 1;
    int vc = vcol(e, line_start(e, e->cur), e->cur);
    if (vc < e->left) e->left = vc;
    if (vc >= e->left + cols) e->left = vc - cols + 1;
}

static void ed_draw(struct win *w, struct rect c)
{
    struct editor *e = w->data;
    scroll_to_cursor(e, c);

    gfx_vgrad(c.x, c.y, c.w, TOOL_H, RGB(0xf2, 0xf3, 0xf4), RGB(0xdc, 0xdf, 0xe2));
    ui_button(c.x + 6, c.y + 4, 56, 22, "Save", 0);
    gfx_text(c.x + 74, c.y + 7, e->path[0] ? e->path : "(unsaved)", C_TEXT_DIM);

    int ty = c.y + TOOL_H, rows = rows_vis(c);
    int tx = c.x + GUTTER;
    gfx_fill(c.x, ty, c.w, c.h - TOOL_H - STAT_H, RGB(0xfc, 0xfc, 0xfd));
    gfx_fill(c.x, ty, GUTTER - 4, c.h - TOOL_H - STAT_H, RGB(0xe8, 0xea, 0xed));
    gfx_vline(c.x + GUTTER - 5, ty, c.h - TOOL_H - STAT_H, RGB(0xc4, 0xc8, 0xcc));

    int pos = 0, ln = 0;
    while (ln < e->top && pos <= e->len) {                  /* skip to the first visible line */
        pos = line_end(e, pos) + 1;
        ln++;
    }
    int cur_ln = line_of(e, e->cur);
    for (int r = 0; r < rows && pos <= e->len; r++, ln++) {
        int y = ty + r * 16, le = line_end(e, pos);
        if (ln == cur_ln)
            gfx_fill(tx - 4, y, c.w - GUTTER + 4, 16, RGB(0xee, 0xf3, 0xfa));
        char num[12];
        snprintf(num, sizeof num, "%d", ln + 1);
        gfx_text(c.x + GUTTER - 12 - gfx_text_width(num), y, num, C_TEXT_DIM);
        int col = 0, sa, sb, has = sel_range(e, &sa, &sb);
        for (int i = pos; i <= le && i <= e->len; i++) {
            int wdt = i < le && e->buf[i] == '\t' ? 4 - col % 4 : 1;
            if (has && i >= sa && i < sb && col >= e->left)
                gfx_fill(tx + (col - e->left) * 8, y, wdt * 8, 16, RGB(0xb8, 0xd4, 0xf4));
            if (i == le)
                break;
            if (e->buf[i] != '\t' && col >= e->left)
                gfx_char(tx + (col - e->left) * 8, y, e->buf[i], C_TEXT);
            col += wdt;
        }
        if (ln == cur_ln) {                                 /* caret */
            int cc = vcol(e, pos, e->cur) - e->left;
            gfx_fill(tx + cc * 8, y, 2, 16, C_ACCENT);
        }
        pos = le + 1;
    }
    int sy = c.y + c.h - STAT_H;
    gfx_fill(c.x, sy, c.w, STAT_H, RGB(0xe0, 0xe3, 0xe6));
    char st[120];
    int ls = line_start(e, e->cur);
    snprintf(st, sizeof st, "Ln %d, Col %d   %d bytes%s", cur_ln + 1, vcol(e, ls, e->cur) + 1, e->len,
             e->modified ? "   [modified]" : "");
    gfx_text(c.x + 8, sy + 3, st, C_TEXT_DIM);
    if (e->status[0])
        gfx_text(c.x + c.w - gfx_text_width(e->status) - 8, sy + 3, e->status, C_TEXT_DIM);
}

static void ed_key(struct win *w, struct input_event *ev)
{
    struct editor *e = w->data;
    struct rect c = wm_client(w);
    int rows = rows_vis(c);
    e->status[0] = 0;
    int vertical = 0;

    if (ev->flags & IEF_CTRL) {
        int k = ev->code >= 'A' && ev->code <= 'Z' ? ev->code + 32 : ev->code;
        if (k == 's') save(e);
        else if (k == 'a') { e->anchor = 0; e->cur = e->len; }
        else if (k == 'c') copy_sel(e);
        else if (k == 'x') { copy_sel(e); before_edit(e, K_OTHER); erase_sel(e); after_edit(e, K_OTHER); }
        else if (k == 'v') paste(e);
        else if (k == 'z') restore(e, e->undo, &e->nundo, e->redo, &e->nredo);
        else if (k == 'y') restore(e, e->redo, &e->nredo, e->undo, &e->nundo);
        else return;
        wm_invalidate(w);
        return;
    }
    int moving = ev->code == KEY_LEFT || ev->code == KEY_RIGHT || ev->code == KEY_UP || ev->code == KEY_DOWN ||
                 ev->code == KEY_PGUP || ev->code == KEY_PGDN || ev->code == KEY_HOME || ev->code == KEY_END;
    if (moving) {
        if (ev->flags & IEF_SHIFT) {
            if (e->anchor < 0)
                e->anchor = e->cur;
        } else {
            e->anchor = -1;
        }
        e->last_kind = K_NONE;
    } else if (ev->code == KEY_DELETE || ev->code == 8 || ev->code == '\n' || ev->code == '\t' ||
               (ev->code >= 32 && ev->code < 127)) {
        int kind = ev->code == KEY_DELETE || ev->code == 8 ? K_DEL : K_TYPE;
        int a, b, had = sel_range(e, &a, &b);
        before_edit(e, had ? K_OTHER : kind);
        if (had) {
            erase_sel(e);
            if (kind == K_DEL) {                       /* deleting a selection is the whole edit */
                after_edit(e, K_OTHER);
                wm_invalidate(w);
                return;
            }
        }
        e->anchor = -1;
    }
    switch (ev->code) {
    case KEY_LEFT:  if (e->cur > 0) e->cur--; break;
    case KEY_RIGHT: if (e->cur < e->len) e->cur++; break;
    case KEY_UP:
    case KEY_DOWN:
    case KEY_PGUP:
    case KEY_PGDN: {
        int ls = line_start(e, e->cur);
        if (e->want_col < 0)
            e->want_col = vcol(e, ls, e->cur);
        int steps = ev->code == KEY_UP || ev->code == KEY_DOWN ? 1 : rows;
        int up = ev->code == KEY_UP || ev->code == KEY_PGUP;
        for (int i = 0; i < steps; i++) {
            if (up) {
                if (ls == 0) break;
                ls = line_start(e, ls - 1);
            } else {
                int le = line_end(e, ls);
                if (le >= e->len) break;
                ls = le + 1;
            }
        }
        e->cur = pos_at_vcol(e, ls, e->want_col);
        vertical = 1;
        break;
    }
    case KEY_HOME: e->cur = line_start(e, e->cur); break;
    case KEY_END:  e->cur = line_end(e, e->cur); break;
    case KEY_DELETE: if (e->cur < e->len) erase(e, e->cur, 1); break;
    case 8: if (e->cur > 0) erase(e, e->cur - 1, 1); break;
    case '\n': {
        int ls = line_start(e, e->cur), ind = 0;
        while (ls + ind < e->len && e->buf[ls + ind] == ' ' && ls + ind < e->cur)
            ind++;                                          /* keep the indentation */
        insert(e, "\n", 1);
        for (int i = 0; i < ind; i++)
            insert(e, " ", 1);
        break;
    }
    case '\t': insert(e, "    ", 4); break;
    default:
        if (ev->code >= 32 && ev->code < 127) {
            char ch = ev->code;
            insert(e, &ch, 1);
        } else {
            return;
        }
    }
    if (!moving) {
        after_edit(e, ev->code == KEY_DELETE || ev->code == 8 ? K_DEL : K_TYPE);
        if (ev->code == '\n')
            e->last_kind = K_NONE;                     /* each line is its own undo step */
    }
    if (!vertical)
        e->want_col = -1;
    wm_invalidate(w);
}

static void ed_mouse(struct win *w, int x, int y, int kind)
{
    struct editor *e = w->data;
    if (kind == M_MDOWN) {
        paste(e);
        wm_invalidate(w);
        return;
    }
    if (kind != M_DOWN && kind != M_DBL && kind != M_DRAG)
        return;
    if (y < TOOL_H) {
        if (kind == M_DRAG)
            return;
        if (x >= 6 && x < 62 && y >= 4 && y < 26) {
            save(e);
            wm_invalidate(w);
        }
        return;
    }
    int row = (y - TOOL_H) / 16 + e->top, pos = 0;
    for (int ln = 0; ln < row && pos <= e->len; ln++) {
        int le = line_end(e, pos);
        if (le >= e->len) { pos = line_start(e, e->len); break; }
        pos = le + 1;
    }
    int want = (x - GUTTER) / 8 + e->left;
    e->cur = pos_at_vcol(e, pos, want < 0 ? 0 : want);
    if (kind == M_DOWN)
        e->anchor = e->cur;                            /* dragging from here selects */
    e->last_kind = K_NONE;
    e->want_col = -1;
    wm_invalidate(w);
}

static void ed_wheel(struct win *w, int dir)
{
    struct editor *e = w->data;
    int lines = line_of(e, e->len) + 1, rows = rows_vis(wm_client(w));
    e->top += dir * 3;
    if (e->top > lines - rows) e->top = lines - rows;
    if (e->top < 0) e->top = 0;
    /* keep the caret on screen by moving it with the view */
    int ln = line_of(e, e->cur);
    if (ln < e->top || ln >= e->top + rows) {
        int target = ln < e->top ? e->top : e->top + rows - 1, pos = 0;
        for (int i = 0; i < target && pos <= e->len; i++)
            pos = line_end(e, pos) + 1 > e->len ? e->len : line_end(e, pos) + 1;
        e->cur = pos;
    }
    wm_invalidate(w);
}

static int ed_request_close(struct win *w)
{
    struct editor *e = w->data;
    if (!e->modified || e->close_warned)
        return 0;
    e->close_warned = 1;
    wm_notify("Unsaved changes: Ctrl+S saves, closing again discards them");
    return 1;
}

static void ed_close(struct win *w)
{
    struct editor *e = w->data;
    for (int i = 0; i < e->nundo; i++) snap_free(&e->undo[i]);
    for (int i = 0; i < e->nredo; i++) snap_free(&e->redo[i]);
    free(e->buf);
    free(e);
}

static const struct app editor_app = {.draw = ed_draw, .key = ed_key, .mouse = ed_mouse, .wheel = ed_wheel,
                                      .close = ed_close, .request_close = ed_request_close};

void editor_open(const char *path)
{
    struct editor *e = calloc(1, sizeof *e);
    if (!e)
        return;
    e->buf = malloc(EMAX + 1);
    if (!e->buf) {
        free(e);
        return;
    }
    e->want_col = -1;
    e->anchor = -1;
    if (path) {
        safestrcpy(e->path, path, sizeof e->path);
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            int n;
            while (e->len < EMAX && (n = read(fd, e->buf + e->len, EMAX - e->len)) > 0)
                e->len += n;
            close(fd);
        }
    }
    e->win = wm_open(&editor_app, 600, 420, "Text Editor", e);
    if (!e->win) {
        free(e->buf);
        free(e);
        return;
    }
    e->win->minw = 300;
    e->win->minh = 160;
    update_title(e);
}
