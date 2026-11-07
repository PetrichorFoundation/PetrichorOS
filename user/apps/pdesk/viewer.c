/* Image viewer: PNG, JPEG and GIF, fitted to the window.  Left/Right (or clicking the right/left half) step
 * through the other images in the folder; Esc closes. */
#include "desk.h"
#include "web.h"

#define BAR_H 26

struct viewer {
    struct win *win;
    char path[PATH_MAX];
    struct image *img;
    char err[96];
};

static int is_image(const char *name)
{
    int n = strlen(name);
    static const char *const ext[] = {".png", ".jpg", ".jpeg", ".gif", ".PNG", ".JPG", ".JPEG", ".GIF"};
    for (int i = 0; i < 8; i++) {
        int m = strlen(ext[i]);
        if (n > m && !strcmp(name + n - m, ext[i]))
            return 1;
    }
    return 0;
}

static void load(struct viewer *v, const char *path)
{
    img_free(v->img);
    v->img = NULL;
    v->err[0] = 0;
    safestrcpy(v->path, path, sizeof v->path);
    struct stat st;
    if (stat(path, &st) < 0 || st.size <= 0) {
        snprintf(v->err, sizeof v->err, "Can't open this file");
        return;
    }
    u8 *buf = malloc(st.size);
    int fd = open(path, O_RDONLY), n = 0, r;
    if (fd >= 0 && buf)
        while (n < (int)st.size && (r = read(fd, buf + n, st.size - n)) > 0)
            n += r;
    if (fd >= 0)
        close(fd);
    if (buf && n == (int)st.size)
        v->img = img_decode(buf, n);
    free(buf);
    if (!v->img)
        snprintf(v->err, sizeof v->err, "Not an image this viewer can read (PNG, JPEG, GIF)");
    const char *base = strrchr(path, '/');
    char title[80];
    snprintf(title, sizeof title, "%s - Image Viewer", base ? base + 1 : path);
    wm_set_title(v->win, title);
}

/* The next (dir = 1) or previous (dir = -1) image in the same folder, in name order. */
static void step(struct viewer *v, int dir)
{
    char folder[PATH_MAX];
    safestrcpy(folder, v->path, sizeof folder);
    char *sl = strrchr(folder, '/');
    if (!sl)
        return;
    const char *cur = sl + 1;
    char curname[NAME_MAX + 1];
    safestrcpy(curname, cur, sizeof curname);
    if (sl == folder)
        sl[1] = 0;
    else
        *sl = 0;
    int fd = open(folder, O_RDONLY);
    if (fd < 0)
        return;
    struct dirent de;
    char best[NAME_MAX + 1] = "", wrap[NAME_MAX + 1] = "";
    while (readdir(fd, &de) > 0) {
        if (!is_image(de.name) || !strcmp(de.name, curname))
            continue;
        int c = strcmp(de.name, curname);
        if (dir > 0) {
            if (c > 0 && (!best[0] || strcmp(de.name, best) < 0)) safestrcpy(best, de.name, sizeof best);
            if (!wrap[0] || strcmp(de.name, wrap) < 0) safestrcpy(wrap, de.name, sizeof wrap);
        } else {
            if (c < 0 && (!best[0] || strcmp(de.name, best) > 0)) safestrcpy(best, de.name, sizeof best);
            if (!wrap[0] || strcmp(de.name, wrap) > 0) safestrcpy(wrap, de.name, sizeof wrap);
        }
    }
    close(fd);
    const char *pick = best[0] ? best : wrap;
    if (!pick[0])
        return;
    char p[PATH_MAX];
    snprintf(p, sizeof p, "%s%s%s", folder, folder[1] ? "/" : "", pick);
    load(v, p);
    wm_invalidate(v->win);
}

static void vw_draw(struct win *w, struct rect c)
{
    struct viewer *v = w->data;
    gfx_fill(c.x, c.y, c.w, c.h, RGB(0x20, 0x22, 0x26));
    int ah = c.h - BAR_H;
    if (v->img && c.w > 4 && ah > 4) {
        int iw = v->img->w, ih = v->img->h, dw = iw, dh = ih;
        if (dw > c.w - 8 || dh > ah - 8) {                       /* shrink to fit, keeping the shape */
            if ((long)iw * (ah - 8) > (long)ih * (c.w - 8)) {
                dw = c.w - 8;
                dh = (int)((long)ih * dw / iw);
            } else {
                dh = ah - 8;
                dw = (int)((long)iw * dh / ih);
            }
        }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        const u32 *px = img_scaled(v->img, dw, dh);
        if (px)
            gfx_blit_argb(px, c.x + (c.w - dw) / 2, c.y + (ah - dh) / 2, dw, dh);
    } else if (v->err[0]) {
        gfx_text(c.x + (c.w - gfx_text_width(v->err)) / 2, c.y + ah / 2, v->err, RGB(0xd0, 0xd0, 0xd0));
    }
    gfx_fill(c.x, c.y + ah, c.w, BAR_H, RGB(0x30, 0x34, 0x3a));
    char t[160];
    if (v->img)
        snprintf(t, sizeof t, "%d x %d    Left/Right: other images in this folder", v->img->w, v->img->h);
    else
        snprintf(t, sizeof t, "Left/Right: other images in this folder");
    gfx_text(c.x + 8, c.y + ah + 5, t, RGB(0xc8, 0xcc, 0xd0));
}

static void vw_key(struct win *w, struct input_event *e)
{
    struct viewer *v = w->data;
    if (e->code == KEY_RIGHT || e->code == ' ')
        step(v, 1);
    else if (e->code == KEY_LEFT || e->code == 8)
        step(v, -1);
    else if (e->code == 27)
        wm_close(w);
}

static void vw_mouse(struct win *w, int x, int y, int kind)
{
    struct viewer *v = w->data;
    if (kind == M_DOWN) {
        struct rect c = wm_client(w);
        step(v, x > c.w / 2 ? 1 : -1);
    }
}

static void vw_close(struct win *w)
{
    struct viewer *v = w->data;
    img_free(v->img);
    free(v);
}

static const struct app viewer_app = {.draw = vw_draw, .key = vw_key, .mouse = vw_mouse, .close = vw_close};

void viewer_open(const char *path)
{
    struct viewer *v = calloc(1, sizeof *v);
    if (!v)
        return;
    v->win = wm_open(&viewer_app, 720, 520, "Image Viewer", v);
    if (!v->win) {
        free(v);
        return;
    }
    v->win->minw = 240;
    v->win->minh = 160;
    load(v, path);
}
