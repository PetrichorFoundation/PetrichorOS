/* File manager: places sidebar, toolbar, sortable-by-kind list, open/new/delete. */
#include "desk.h"

#define SIDEBAR_W 124
#define TOOLBAR_H 32
#define PATHBAR_H 24
#define STATUS_H  22
#define ROW_H     22
#define HEADER_H  20

struct fent {
    char name[NAME_MAX + 1];
    u32 type, size;
};

struct files {
    struct win *win;
    char path[PATH_MAX];
    struct fent *ents;
    int n, sel, top;
    char status[96];
};

static const struct {
    const char *label, *path;
    int icon;
} sidebar[] = {
    {"Computer", "/", ICON_DISK},
    {"Home", "/disk", ICON_HOME},
    {"Pictures", "/disk/Pictures", ICON_FOLDER},
    {"Programs", "/bin", ICON_FOLDER},
    {"Disk bin", "/disk/bin", ICON_EXEC},
    {"Temp", "/tmp", ICON_FOLDER},
    {"Config", "/etc", ICON_FOLDER},
    {"Devices", "/dev", ICON_FOLDER},
};
#define NSIDE 8

static int cmp_ent(const struct fent *a, const struct fent *b)
{
    int da = a->type == T_DIR, db = b->type == T_DIR;
    if (da != db)
        return db - da;                         /* directories first */
    return strcmp(a->name, b->name);
}

static void reload(struct files *f)
{
    free(f->ents);
    f->ents = NULL;
    f->n = 0;
    int fd = open(f->path, O_RDONLY);
    if (fd < 0) {
        snprintf(f->status, sizeof f->status, "Cannot open %s: %s", f->path, strerror(fd));
        return;
    }
    int cap = 32;
    f->ents = malloc(cap * sizeof *f->ents);
    struct dirent de;
    while (f->ents && readdir(fd, &de) > 0) {
        if (!strcmp(de.name, ".") || !strcmp(de.name, ".."))
            continue;
        if (f->n == cap) {
            cap *= 2;
            f->ents = realloc(f->ents, cap * sizeof *f->ents);
            if (!f->ents)
                break;
        }
        struct fent *e = &f->ents[f->n++];
        safestrcpy(e->name, de.name, sizeof e->name);
        e->type = de.type;
        char full[PATH_MAX];
        snprintf(full, sizeof full, "%s%s%s", f->path, f->path[1] ? "/" : "", de.name);
        struct stat st;
        e->size = stat(full, &st) == 0 ? st.size : 0;
    }
    close(fd);
    for (int i = 1; i < f->n; i++) {            /* insertion sort */
        struct fent t = f->ents[i];
        int j = i - 1;
        while (j >= 0 && cmp_ent(&f->ents[j], &t) > 0) {
            f->ents[j + 1] = f->ents[j];
            j--;
        }
        f->ents[j + 1] = t;
    }
    snprintf(f->status, sizeof f->status, "%d item%s", f->n, f->n == 1 ? "" : "s");
    if (f->sel >= f->n)
        f->sel = f->n - 1;
}

static void go(struct files *f, const char *path)
{
    safestrcpy(f->path, path, sizeof f->path);
    f->sel = -1;
    f->top = 0;
    reload(f);
    char title[64];
    snprintf(title, sizeof title, "%s - File Manager", f->path);
    wm_set_title(f->win, title);
    wm_invalidate(f->win);
}

static void go_up(struct files *f)
{
    char p[PATH_MAX];
    safestrcpy(p, f->path, sizeof p);
    char *slash = strrchr(p, '/');
    if (slash == p)
        p[1] = 0;
    else if (slash)
        *slash = 0;
    go(f, p);
}

static int visible_rows(struct rect c)
{
    return (c.h - TOOLBAR_H - PATHBAR_H - HEADER_H - STATUS_H) / ROW_H;
}

static void new_entry(struct files *f, int dir)
{
    const char *base = f->path[1] ? f->path : "";
    char p[PATH_MAX], suffix[16];
    for (int i = 1; i < 100; i++) {
        if (i == 1)
            suffix[0] = 0;
        else
            snprintf(suffix, sizeof suffix, dir ? " %d" : "-%d", i);
        if (dir)
            snprintf(p, sizeof p, "%s/New Folder%s", base, suffix);
        else
            snprintf(p, sizeof p, "%s/untitled%s.txt", base, suffix);
        struct stat st;
        if (stat(p, &st) == 0)
            continue;
        int e = dir ? mkdir(p) : open(p, O_WRONLY | O_CREAT);
        if (e < 0) {
            snprintf(f->status, sizeof f->status, "Cannot create: %s", strerror(e));
        } else {
            if (!dir)
                close(e);
            reload(f);
        }
        return;
    }
}

static void open_entry(struct files *f, int i)
{
    struct fent *e = &f->ents[i];
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s%s%s", f->path, f->path[1] ? "/" : "", e->name);
    if (e->type == T_DIR) {
        go(f, p);
        return;
    }
    if (e->type != T_FILE) {
        snprintf(f->status, sizeof f->status, "%s is a device", e->name);
        return;
    }
    int pl = strlen(p);
    static const char *const imgs[] = {".png", ".jpg", ".jpeg", ".gif", ".PNG", ".JPG", ".JPEG", ".GIF"};
    for (int k = 0; k < 8; k++) {
        int m = strlen(imgs[k]);
        if (pl > m && !strcmp(p + pl - m, imgs[k])) {
            viewer_open(p);
            return;
        }
    }
    if ((pl > 5 && !strcmp(p + pl - 5, ".html")) || (pl > 4 && !strcmp(p + pl - 4, ".htm"))) {
        char url[PATH_MAX + 8];
        snprintf(url, sizeof url, "file://%s", p);
        browser_open(url);
        return;
    }
    if ((pl > 5 && !strcmp(p + pl - 5, ".rain")) || (pl > 4 && !strcmp(p + pl - 4, ".exe"))) {   /* programs (.exe via GrapeJuice) */
        term_open(p);
        return;
    }
    char head[512];
    int fd = open(p, O_RDONLY), n = fd < 0 ? 0 : read(fd, head, sizeof head);
    if (fd >= 0)
        close(fd);
    if (n >= 4 && !memcmp(head, "\177ELF", 4)) {
        term_open(p);                           /* programs run in a terminal */
        return;
    }
    for (int k = 0; k < n; k++)
        if (head[k] == 0) {
            snprintf(f->status, sizeof f->status, "%s is a binary file", e->name);
            return;
        }
    editor_open(p);
}

static void delete_sel(struct files *f)
{
    if (f->sel < 0 || f->sel >= f->n)
        return;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s%s%s", f->path, f->path[1] ? "/" : "", f->ents[f->sel].name);
    int e = unlink(p);
    if (e < 0)
        snprintf(f->status, sizeof f->status, "Cannot delete %s: %s", f->ents[f->sel].name, strerror(e));
    else
        reload(f);
}

/* Picks an icon from the file's name (and folder). */
static int entry_icon(struct files *f, const struct fent *e)
{
    if (e->type == T_DIR)
        return ICON_FOLDER;
    if (e->type == T_DEV)
        return ICON_DISK;
    const char *n = e->name, *dot = strrchr(n, '.');
    static const char *const web[] = {"html", "htm", "xhtml"};
    static const char *const code[] = {"c", "h", "S", "py", "js", "css", "json", "sh", "ld", "mk", "rs", "go"};
    static const char *const text[] = {"txt", "md", "conf", "cfg", "log", "csv", "ini"};
    static const char *const arch[] = {"ppk", "zip", "tar", "gz", "img", "iso", "bin"};
    if (dot && (!strcmp(dot, ".rain") || !strcmp(dot, ".exe")))
        return ICON_EXEC;
    if (dot && dot[1]) {
        for (u32 i = 0; i < sizeof web / sizeof *web; i++) if (!strcmp(dot + 1, web[i])) return ICON_FILE_WEB;
        for (u32 i = 0; i < sizeof code / sizeof *code; i++) if (!strcmp(dot + 1, code[i])) return ICON_FILE_CODE;
        for (u32 i = 0; i < sizeof text / sizeof *text; i++) if (!strcmp(dot + 1, text[i])) return ICON_FILE_TEXT;
        for (u32 i = 0; i < sizeof arch / sizeof *arch; i++) if (!strcmp(dot + 1, arch[i])) return ICON_FILE_ARCHIVE;
    }
    if (!strcmp(f->path, "/bin") || !strcmp(f->path, "/disk/bin"))
        return ICON_EXEC;
    if (!strncmp(f->path, "/etc", 4) || !strcmp(n, "motd") || !strcmp(n, "README"))
        return ICON_FILE_TEXT;
    return ICON_FILE;
}

/* ---------------- drawing ---------------- */

static void fmt_size(char *buf, int n, u32 sz)
{
    if (sz < 1024)
        snprintf(buf, n, "%u B", sz);
    else if (sz < 1024 * 1024)
        snprintf(buf, n, "%u.%u KB", sz / 1024, sz % 1024 * 10 / 1024);
    else
        snprintf(buf, n, "%u.%u MB", sz / 1048576, sz % 1048576 * 10 / 1048576);
}

static void files_draw(struct win *w, struct rect c)
{
    struct files *f = w->data;
    int lx = c.x + SIDEBAR_W;

    gfx_fill(c.x, c.y, SIDEBAR_W, c.h, RGB(0xdd, 0xe0, 0xe4));      /* sidebar */
    gfx_vline(c.x + SIDEBAR_W - 1, c.y, c.h, RGB(0xa8, 0xb0, 0xb8));
    gfx_text(c.x + 10, c.y + 8, "PLACES", C_TEXT_DIM);
    for (int i = 0; i < NSIDE; i++) {
        int y = c.y + 28 + i * 28;
        if (!strcmp(f->path, sidebar[i].path))
            gfx_fill(c.x + 2, y - 3, SIDEBAR_W - 5, 26, RGB(0xb8, 0xcc, 0xe4));
        icon_draw(sidebar[i].icon, c.x + 8, y, 20);
        gfx_text(c.x + 34, y + 2, sidebar[i].label, C_TEXT);
    }

    gfx_vgrad(lx, c.y, c.w - SIDEBAR_W, TOOLBAR_H, RGB(0xf2, 0xf3, 0xf4), RGB(0xdc, 0xdf, 0xe2));
    ui_button(lx + 6, c.y + 4, 46, 24, "Up", 0);
    ui_button(lx + 56, c.y + 4, 76, 24, "Refresh", 0);
    ui_button(lx + 136, c.y + 4, 96, 24, "New Folder", 0);
    ui_button(lx + 236, c.y + 4, 80, 24, "New File", 0);

    gfx_fill(lx, c.y + TOOLBAR_H, c.w - SIDEBAR_W, PATHBAR_H, RGB(0xe6, 0xe8, 0xea));
    gfx_text(lx + 8, c.y + TOOLBAR_H + 4, "Location:", C_TEXT_DIM);
    gfx_text(lx + 88, c.y + TOOLBAR_H + 4, f->path, C_TEXT);

    int ly = c.y + TOOLBAR_H + PATHBAR_H;
    int lw = c.w - SIDEBAR_W;
    gfx_fill(lx, ly, lw, HEADER_H, RGB(0xd0, 0xd4, 0xd8));
    gfx_text(lx + 30, ly + 2, "Name", C_TEXT);
    gfx_text(lx + lw - 110, ly + 2, "Size", C_TEXT);
    int rows = visible_rows(c);
    int ry = ly + HEADER_H;
    gfx_fill(lx, ry, lw, rows * ROW_H + (c.h - TOOLBAR_H - PATHBAR_H - HEADER_H - STATUS_H) % ROW_H, C_WHITE);

    for (int r = 0; r < rows; r++) {
        int i = f->top + r;
        if (i >= f->n)
            break;
        struct fent *e = &f->ents[i];
        int y = ry + r * ROW_H;
        if (i == f->sel)
            gfx_fill(lx, y, lw, ROW_H, C_ACCENT);
        else if (r & 1)
            gfx_fill(lx, y, lw, ROW_H, RGB(0xf5, 0xf7, 0xf9));
        icon_draw(entry_icon(f, e), lx + 6, y + 1, 20);
        u32 tc = i == f->sel ? C_WHITE : C_TEXT;
        gfx_text(lx + 32, y + 3, e->name, tc);
        if (e->type == T_FILE) {
            char sz[24];
            fmt_size(sz, sizeof sz, e->size);
            gfx_text(lx + lw - 110, y + 3, sz, i == f->sel ? C_WHITE : C_TEXT_DIM);
        } else if (e->type == T_DIR) {
            gfx_text(lx + lw - 110, y + 3, "folder", i == f->sel ? C_WHITE : C_TEXT_DIM);
        }
    }
    if (f->n > rows) {                                              /* scrollbar */
        int th = (c.h - TOOLBAR_H - PATHBAR_H - HEADER_H - STATUS_H);
        gfx_fill(lx + lw - 10, ry, 10, th, RGB(0xe4, 0xe6, 0xe8));
        int bh = th * rows / f->n, by = ry + (th - bh) * f->top / (f->n - rows);
        gfx_fill(lx + lw - 9, by, 8, bh, RGB(0x90, 0x9a, 0xa4));
    }
    gfx_fill(lx, c.y + c.h - STATUS_H, lw, STATUS_H, RGB(0xe0, 0xe3, 0xe6));
    gfx_text(lx + 8, c.y + c.h - STATUS_H + 3, f->status, C_TEXT_DIM);
}

static void files_mouse(struct win *w, int x, int y, int kind)
{
    struct files *f = w->data;
    if (kind != M_DOWN && kind != M_DBL)
        return;
    struct rect c = wm_client(w);
    int rows = visible_rows(c);
    if (x < SIDEBAR_W) {
        for (int i = 0; i < NSIDE; i++)
            if (y >= 25 + i * 28 && y < 25 + i * 28 + 26) {
                struct stat st;
                if (stat(sidebar[i].path, &st) == 0)
                    go(f, sidebar[i].path);
                else {
                    snprintf(f->status, sizeof f->status, "%s does not exist", sidebar[i].path);
                    wm_invalidate(w);
                }
            }
        return;
    }
    int lx = x - SIDEBAR_W;
    if (y < TOOLBAR_H) {
        if (y >= 4 && y < 28) {
            if (lx >= 6 && lx < 52) go_up(f);
            else if (lx >= 56 && lx < 132) { reload(f); wm_invalidate(w); }
            else if (lx >= 136 && lx < 232) { new_entry(f, 1); wm_invalidate(w); }
            else if (lx >= 236 && lx < 316) { new_entry(f, 0); wm_invalidate(w); }
        }
        return;
    }
    int ly = TOOLBAR_H + PATHBAR_H + HEADER_H;
    if (y < ly)
        return;
    int row = (y - ly) / ROW_H;
    if (row >= rows)
        return;
    int i = f->top + row;
    if (i >= f->n)
        return;
    f->sel = i;
    if (kind == M_DBL)
        open_entry(f, i);
    wm_invalidate(w);
}

/* ---------------- copy and paste ---------------- */

static char copied[PATH_MAX];                      /* shared by every Files window */

static int copy_file(const char *from, const char *to)
{
    int in = open(from, O_RDONLY);
    if (in < 0)
        return in;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        close(in);
        return out;
    }
    static char buf[16384];
    int r, e = 0;
    while ((r = read(in, buf, sizeof buf)) > 0)
        if (write(out, buf, r) != r) {
            e = -ENOSPC;
            break;
        }
    close(in);
    close(out);
    return r < 0 ? r : e;
}

static int copy_tree(const char *from, const char *to, int depth)
{
    struct stat st;
    if (stat(from, &st) < 0)
        return -ENOENT;
    if (st.type != T_DIR)
        return copy_file(from, to);
    if (depth > 16)
        return -EINVAL;
    int e = mkdir(to);
    if (e < 0 && e != -EEXIST)
        return e;
    int fd = open(from, O_RDONLY);
    if (fd < 0)
        return fd;
    struct dirent de;
    int err = 0;
    while (!err && readdir(fd, &de) > 0) {
        if (!strcmp(de.name, ".") || !strcmp(de.name, ".."))
            continue;
        char a[PATH_MAX], b[PATH_MAX];
        snprintf(a, sizeof a, "%s/%s", from, de.name);
        snprintf(b, sizeof b, "%s/%s", to, de.name);
        err = copy_tree(a, b, depth + 1);
    }
    close(fd);
    return err;
}

static void paste(struct files *f)
{
    if (!copied[0]) {
        snprintf(f->status, sizeof f->status, "Nothing copied (select a file and press Ctrl+C)");
        return;
    }
    const char *base = strrchr(copied, '/');
    base = base ? base + 1 : copied;
    char dst[PATH_MAX];
    struct stat st;
    snprintf(dst, sizeof dst, "%s%s%s", f->path, f->path[1] ? "/" : "", base);
    for (int i = 2; stat(dst, &st) == 0 && i < 100; i++)        /* never overwrite: "name (2)", "name (3)", ... */
        snprintf(dst, sizeof dst, "%s%s%s (%d)", f->path, f->path[1] ? "/" : "", base, i);
    int plen = strlen(copied);
    if (!strncmp(dst, copied, plen) && dst[plen] == '/') {
        snprintf(f->status, sizeof f->status, "Can't paste a folder inside itself");
        return;
    }
    int e = copy_tree(copied, dst, 0);
    snprintf(f->status, sizeof f->status, e < 0 ? "Paste failed: %s" : "Pasted %s", e < 0 ? strerror(e) : base);
    reload(f);
}

static void files_key(struct win *w, struct input_event *e)
{
    struct files *f = w->data;
    struct rect c = wm_client(w);
    int rows = visible_rows(c);
    if (rows < 1) rows = 1;
    if ((e->flags & IEF_CTRL) && (e->code == 'c' || e->code == 'C')) {
        if (f->sel >= 0 && f->sel < f->n) {
            snprintf(copied, sizeof copied, "%s%s%s", f->path, f->path[1] ? "/" : "", f->ents[f->sel].name);
            snprintf(f->status, sizeof f->status, "Copied %s: open a folder and press Ctrl+V", f->ents[f->sel].name);
            clip_set(copied, strlen(copied));
        }
        wm_invalidate(w);
        return;
    }
    if ((e->flags & IEF_CTRL) && (e->code == 'v' || e->code == 'V')) {
        paste(f);
        wm_invalidate(w);
        return;
    }
    if (e->flags & IEF_CTRL)
        return;
    switch (e->code) {
    case KEY_UP:   if (f->sel > 0) f->sel--; else if (f->sel < 0 && f->n) f->sel = 0; break;
    case KEY_DOWN: if (f->sel < f->n - 1) f->sel++; break;
    case KEY_PGUP: f->sel -= rows; if (f->sel < 0) f->sel = f->n ? 0 : -1; break;
    case KEY_PGDN: f->sel += rows; if (f->sel >= f->n) f->sel = f->n - 1; break;
    case KEY_HOME: f->sel = f->n ? 0 : -1; break;
    case KEY_END:  f->sel = f->n - 1; break;
    case '\n':     if (f->sel >= 0) { open_entry(f, f->sel); } break;
    case 8:        go_up(f); return;
    case KEY_DELETE: delete_sel(f); break;
    case KEY_F1 + 4: reload(f); break;                      /* F5 */
    default: return;
    }
    if (f->sel >= 0) {
        if (f->sel < f->top) f->top = f->sel;
        if (f->sel >= f->top + rows) f->top = f->sel - rows + 1;
    }
    wm_invalidate(w);
}

static void files_wheel(struct win *w, int dir)
{
    struct files *f = w->data;
    int rows = visible_rows(wm_client(w));
    f->top += dir * 3;
    if (f->top > f->n - rows) f->top = f->n - rows;
    if (f->top < 0) f->top = 0;
    wm_invalidate(w);
}

static void files_close(struct win *w)
{
    struct files *f = w->data;
    free(f->ents);
    free(f);
}

static const struct app files_app = {.draw = files_draw, .key = files_key, .mouse = files_mouse,
                                     .wheel = files_wheel, .close = files_close};

void files_open(const char *path)
{
    struct files *f = calloc(1, sizeof *f);
    if (!f)
        return;
    f->win = wm_open(&files_app, 680, 440, "File Manager", f);
    if (!f->win) {
        free(f);
        return;
    }
    f->win->minw = 420;
    f->win->minh = 240;
    f->sel = -1;
    struct stat st;
    go(f, stat(path, &st) == 0 && st.type == T_DIR ? path : "/");
}
