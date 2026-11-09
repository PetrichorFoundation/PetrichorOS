/* pdesk: the Petrichor desktop environment.
 *
 * One process owns the framebuffer and the input devices.  It composes
 * windows (back to front) into an off-screen buffer, copies only the damaged
 * rectangle to the screen, and routes keyboard and mouse events to the focused
 * window's app.  The panel, dock, menus, notifications, calendar and desktop
 * icons are drawn here.
 *
 * Exit status tells init what to do next: 0 = log out (show the desktop
 * again), 10 = drop to the text console. */
#include "desk.h"
#include "host.h"
#include "img.h"

#define MAXWIN 12

static void ghost_open(struct win *w);
static void ghost_close(struct win *w);
static void ghost_minimize(struct win *w);
static void ghost_restore(struct win *w);
static u32 popup_started;
static u32 dock_bounce[16];

/* ---------------- state ---------------- */

static struct fbinfo fb;
static u32 *back, *wall;
int screen_w = 1024, screen_h = 768;
static struct rect screen = {0, 0, 1024, 768};
static int *xsrc;                        /* framebuffer column -> desktop column, when the desktop is scaled */

static struct win wins[MAXWIN];
static struct win *zorder[MAXWIN];
static int nz;
static struct win *focus;
static int opened;                       /* total windows ever opened: drives cascading */

static int running = 1, exit_code, want_poweroff, want_reboot;
static int mx = 512, my = 384, mbtn;

enum { DR_NONE, DR_MOVE, DR_RESIZE, DR_APP };
static int drag_mode;
static struct win *drag_win;
static int drag_ox, drag_oy;
static u32 last_click_tick;
static int last_click_x, last_click_y;

static int dock_hover = -1, icon_sel = -1;

static struct rect damage;
static int dirty;

u32 g_accent;
int g_wall, g_accent_idx;

/* ---------------- themes ---------------- */

static const struct {
    const char *name;
    u32 top, bottom;
    int rain, stars, swoosh;
} walls[NWALL] = {
    {"Rain", RGB(0x0c, 0x22, 0x3a), RGB(0x1c, 0x5a, 0x6a), 1, 0, 0},
    {"Forest", RGB(0x08, 0x1c, 0x14), RGB(0x2a, 0x5e, 0x3a), 1, 0, 0},
    {"Dusk", RGB(0x24, 0x18, 0x48), RGB(0xd0, 0x78, 0x4c), 0, 0, 0},
    {"Slate", RGB(0x26, 0x2c, 0x34), RGB(0x58, 0x62, 0x6e), 0, 0, 0},
    {"Midnight", RGB(0x02, 0x04, 0x10), RGB(0x16, 0x20, 0x44), 0, 1, 0},
    {"Aqua", RGB(0x0a, 0x3c, 0x8c), RGB(0x6c, 0xc8, 0xf0), 0, 0, 1},
    {"Aurora", RGB(0x04, 0x1a, 0x2c), RGB(0x10, 0x6a, 0x5a), 0, 1, 2},
};

static const u32 accents[NACCENT] = {
    RGB(0x4a, 0x90, 0xd9), RGB(0x3f, 0xae, 0x72), RGB(0xe0, 0x8a, 0x3c),
    RGB(0x9a, 0x6a, 0xd8), RGB(0xd8, 0x55, 0x4a), RGB(0x2f, 0xb3, 0xb3),
};

const char *theme_wall_name(int i) { return walls[i].name; }
u32 theme_wall_color(int i) { return gfx_mix(walls[i].top, walls[i].bottom, 150); }
u32 theme_accent_color(int i) { return accents[i]; }

#define THEME_CONF "/disk/etc/pdesk.conf"

int g_animations = 1;
int g_fps = 0;                           /* 0: match the monitor */
static int g_res_w, g_res_h;             /* saved resolution, 0 = native */
int g_blank_min = 10;
static int blanked;
static u32 last_input;

char g_wallpic[128];

static void theme_load(void)
{
    g_wall = 0;
    g_accent_idx = 0;
    int fd = open(THEME_CONF, O_RDONLY);
    if (fd >= 0) {
        char buf[320];
        int n = read(fd, buf, sizeof buf - 1);
        close(fd);
        buf[n > 0 ? n : 0] = 0;
        char *p = strstr(buf, "wall=");
        if (p) g_wall = atoi(p + 5);
        p = strstr(buf, "accent=");
        if (p) g_accent_idx = atoi(p + 7);
        p = strstr(buf, "anim=");
        if (p) g_animations = atoi(p + 5);
        p = strstr(buf, "res=");
        if (p) {
            g_res_w = atoi(p + 4);
            while (*p && *p != 'x') p++;
            g_res_h = *p == 'x' ? atoi(p + 1) : 0;
        }
        p = strstr(buf, "fps=");
        if (p) g_fps = atoi(p + 4);
        p = strstr(buf, "blank=");
        if (p) g_blank_min = atoi(p + 6);
        p = strstr(buf, "pic=");
        if (p) {
            int i = 0;
            for (p += 4; *p && *p != '\n' && i < (int)sizeof g_wallpic - 1; p++) g_wallpic[i++] = *p;
            g_wallpic[i] = 0;
        }
    }
    if (g_wall < 0 || g_wall >= NWALL) g_wall = 0;
    if (g_accent_idx < 0 || g_accent_idx >= NACCENT) g_accent_idx = 0;
    g_accent = accents[g_accent_idx];
}

/* A picture wallpaper, scaled to cover the screen (cropping the overflow).  Returns 0 if it couldn't be loaded. */
static int draw_picture(const char *path)
{
    struct stat st;
    if (!path[0] || stat(path, &st) < 0 || st.size <= 0 || st.size > 24 * 1024 * 1024)
        return 0;
    u8 *data = malloc(st.size);
    int fd = data ? open(path, O_RDONLY) : -1, got = 0, r;
    if (fd < 0) { free(data); return 0; }
    while (got < (int)st.size && (r = read(fd, data + got, st.size - got)) > 0) got += r;
    close(fd);
    struct image *im = got == (int)st.size ? img_decode(data, got) : NULL;
    free(data);
    if (!im)
        return 0;
    int dw = SCREEN_W, dh = (int)((long)im->h * SCREEN_W / im->w);
    if (dh < SCREEN_H) {
        dh = SCREEN_H;
        dw = (int)((long)im->w * SCREEN_H / im->h);
    }
    const u32 *px = img_scaled(im, dw, dh);
    if (px) {
        gfx_fill(0, 0, SCREEN_W, SCREEN_H, 0);
        gfx_blit_argb(px, (SCREEN_W - dw) / 2, (SCREEN_H - dh) / 2, dw, dh);
    }
    img_free(im);
    return px != NULL;
}

/* Y2K-style light ribbons: layered translucent sine bands with a bright edge. */
static void draw_swooshes(int kind)
{
    static const int sin64[65] = {0, 6, 13, 19, 25, 31, 37, 43, 49, 55, 60, 66, 71, 76, 81, 85, 90, 94, 98, 102, 106,
                                  109, 112, 115, 117, 120, 122, 123, 125, 126, 127, 127, 128, 127, 127, 126, 125, 123,
                                  122, 120, 117, 115, 112, 109, 106, 102, 98, 94, 90, 85, 81, 76, 71, 66, 60, 55, 49, 43,
                                  37, 31, 25, 19, 13, 6, 0};
    for (int band = 0; band < 5; band++) {
        int base = SCREEN_H * (40 + band * 9) / 100, amp = SCREEN_H / (5 + band), thick = 26 + band * 22;
        int phase = band * 37, period = SCREEN_W + band * 260;
        u32 col = kind == 1 ? (band & 1 ? RGB(0xd8, 0xf4, 0xff) : RGB(0x9c, 0xe0, 0xff))
                            : (band & 1 ? RGB(0x60, 0xf0, 0xb0) : RGB(0x70, 0xa8, 0xff));
        for (int x = 0; x < SCREEN_W; x++) {
            int t8 = ((x * 1024 / period) + phase * 8) % 1024;           /* one full sine over 128 steps, */
            int t = t8 >> 3, f = t8 & 7, t2 = (t + 1) & 127;             /* interpolated in eighths */
            int s0 = t < 64 ? sin64[t] : -sin64[t - 64], s1 = t2 < 64 ? sin64[t2] : -sin64[t2 - 64];
            int s = s0 + (s1 - s0) * f / 8;
            int y = base - s * amp / 128 - (x * SCREEN_H / 6) / SCREEN_W;
            for (int k = 0; k < thick; k++) {
                int py = y + k;
                if (py < 0 || py >= SCREEN_H) continue;
                int a = k < 3 ? 70 - k * 12 : (thick - k) * 28 / thick + 4;     /* bright lip, then a soft fade */
                u32 *p = wall + py * SCREEN_W + x;
                *p = gfx_mix(*p, col, a);
            }
        }
    }
}

static void render_wallpaper(void)
{
    gfx_set_target(wall, SCREEN_W, SCREEN_H);
    gfx_set_clip(0, 0, SCREEN_W, SCREEN_H);
    if (g_wallpic[0] && draw_picture(g_wallpic)) {
        gfx_set_target(back, SCREEN_W, SCREEN_H);
        return;
    }
    gfx_vgrad(0, 0, SCREEN_W, SCREEN_H, walls[g_wall].top, walls[g_wall].bottom);
    if (walls[g_wall].swoosh)
        draw_swooshes(walls[g_wall].swoosh);
    u32 seed = 0x2545F491;
    if (walls[g_wall].rain) {
        for (int i = 0; i < 260; i++) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            int x = seed % SCREEN_W;
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            int y = seed % SCREEN_H, len = 18 + seed % 40;
            for (int k = 0; k < len; k++) {
                int px = x + k / 4, py = y + k;
                if (px < SCREEN_W && py < SCREEN_H) {
                    u32 *p = wall + py * SCREEN_W + px;
                    *p = gfx_mix(*p, RGB(0xa8, 0xd8, 0xe8), 18 + k * 36 / len);
                }
            }
        }
    }
    if (walls[g_wall].stars) {
        for (int i = 0; i < 220; i++) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            int x = seed % SCREEN_W;
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            int y = seed % SCREEN_H, b = 90 + seed % 150;
            gfx_fill(x, y, 1 + (i % 7 == 0), 1 + (i % 7 == 0), RGB(b, b, 255));
        }
    }
    u32 mark = gfx_mix(walls[g_wall].bottom, RGB(0xc8, 0xe8, 0xf4), 70);
    gfx_text_scaled(SCREEN_W - 400, SCREEN_H - 170, "Petrichor", mark, 5);
    gfx_text_scaled(SCREEN_W - 398, SCREEN_H - 84, "the smell of rain on dry earth", mark, 1);
    gfx_set_target(back, SCREEN_W, SCREEN_H);
}

/* ---------------- damage and presentation ---------------- */

static void invalidate(int x, int y, int w, int h)
{
    struct rect r = rect_isect((struct rect){x, y, w, h}, screen);
    if (r.w <= 0)
        return;
    damage = dirty ? rect_union(damage, r) : r;
    dirty = 1;
}

void wm_invalidate_rect(int x, int y, int w, int h) { invalidate(x, y, w, h); }

void wm_invalidate(struct win *w)
{
    invalidate(w->x - 4, w->y - 4, w->w + 10, w->h + 12);      /* includes the shadow */
}

struct rect wm_client(struct win *w)
{
    return (struct rect){w->x + 1, w->y + TITLE_H, w->w - 2, w->h - TITLE_H - 1};
}

static void theme_save(void)
{
    struct diskinfo di;
    diskinfo(&di);
    if (di.mounted) {
        mkdir("/disk/etc");
        char buf[320];
        int n = snprintf(buf, sizeof buf, "wall=%d\naccent=%d\nanim=%d\nres=%dx%d\nfps=%d\nblank=%d\npic=%s\n", g_wall,
                         g_accent_idx, g_animations, g_res_w, g_res_h, g_fps, g_blank_min, g_wallpic);
        int fd = open(THEME_CONF, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd >= 0) {
            write(fd, buf, n);
            close(fd);
        }
    }
}

int wallpaper_set_picture(const char *path)
{
    char old[sizeof g_wallpic];
    safestrcpy(old, g_wallpic, sizeof old);
    safestrcpy(g_wallpic, path, sizeof g_wallpic);
    gfx_set_target(wall, SCREEN_W, SCREEN_H);
    gfx_set_clip(0, 0, SCREEN_W, SCREEN_H);
    int ok = draw_picture(g_wallpic);
    gfx_set_target(back, SCREEN_W, SCREEN_H);
    if (!ok) {
        safestrcpy(g_wallpic, old, sizeof g_wallpic);
        return -1;
    }
    invalidate(0, 0, SCREEN_W, SCREEN_H);
    theme_save();
    return 0;
}

void theme_set(int wall_idx, int accent_idx)
{
    if (wall_idx != g_wall)
        g_wallpic[0] = 0;                       /* picking a built-in wallpaper replaces the picture */
    g_wall = wall_idx;
    g_accent_idx = accent_idx;
    g_accent = accents[accent_idx];
    render_wallpaper();
    invalidate(0, 0, SCREEN_W, SCREEN_H);
    theme_save();
}

void anim_set(int on)
{
    g_animations = on;
    theme_save();
}

static const char *const cursor_art[19] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ", "X    X..X   ",
    "     X..X   ", "      X..X  ", "      X..X  ", "       XX   ",
};

static int fb_scaled(void) { return screen_w != (int)fb.width || screen_h != (int)fb.height; }

/* Where the pointer is on the real screen. */
static void cursor_pos(int *cx, int *cy)
{
    *cx = fb_scaled() ? (int)((long)mx * fb.width / screen_w) : mx;
    *cy = fb_scaled() ? (int)((long)my * fb.height / screen_h) : my;
}

static void cursor_show(void)
{
    u32 *px = (u32 *)(uptr)fb.addr;
    int pitch = fb.pitch / 4, cx, cy;
    cursor_pos(&cx, &cy);
    for (int y = 0; y < 19; y++) {
        for (int x = 0; x < 12; x++) {
            char c = cursor_art[y][x];
            int sx = cx + x, sy = cy + y;
            if (c == ' ' || sx >= (int)fb.width || sy >= (int)fb.height)
                continue;
            px[sy * pitch + sx] = c == 'X' ? 0x000000 : 0xFFFFFF;
        }
    }
}

/* Copies a rectangle of the desktop to the screen (scaling it when the desktop is drawn smaller). */
static void present(struct rect r)
{
    u32 *px = (u32 *)(uptr)fb.addr;
    int pitch = fb.pitch / 4;
    if (!fb_scaled()) {
        for (int y = r.y; y < r.y + r.h; y++)
            memcpy(px + y * pitch + r.x, back + y * screen_w + r.x, r.w * 4);
        return;
    }
    int fw = fb.width, fh = fb.height;
    int x0 = (int)((long)r.x * fw / screen_w), x1 = (int)(((long)(r.x + r.w) * fw + screen_w - 1) / screen_w);
    int y0 = (int)((long)r.y * fh / screen_h), y1 = (int)(((long)(r.y + r.h) * fh + screen_h - 1) / screen_h);
    if (x1 > fw) x1 = fw;
    if (y1 > fh) y1 = fh;
    for (int y = y0; y < y1; y++) {
        const u32 *row = back + (int)((long)y * screen_h / fh) * screen_w;
        u32 *out = px + y * pitch;
        for (int x = x0; x < x1; x++)
            out[x] = row[xsrc[x]];
    }
}

static void cursor_hide(void)
{
    int cx, cy;
    cursor_pos(&cx, &cy);
    int x0 = (int)((long)cx * screen_w / fb.width), x1 = (int)(((long)(cx + 12) * screen_w + fb.width - 1) / fb.width);
    int y0 = (int)((long)cy * screen_h / fb.height), y1 = (int)(((long)(cy + 19) * screen_h + fb.height - 1) / fb.height);
    struct rect r = rect_isect((struct rect){x0, y0, x1 - x0, y1 - y0}, screen);
    if (r.w > 0)
        present(r);
}

/* ---------------- notifications ---------------- */

#define NTOAST 4
static struct {
    char text[72];
    u32 expire, born;
} toasts[NTOAST];

static struct rect toast_rect(int i)
{
    return (struct rect){SCREEN_W - 322, PANEL_H + 8 + i * 42, 312, 36};
}

void wm_notify(const char *msg)
{
    for (int i = NTOAST - 1; i > 0; i--)
        toasts[i] = toasts[i - 1];
    safestrcpy(toasts[0].text, msg, sizeof toasts[0].text);
    toasts[0].expire = uptime() + 450;
    toasts[0].born = uptime();
    invalidate(SCREEN_W - 330, PANEL_H, 330, NTOAST * 42 + 12);
}

static int ease_out(int t);

static int toast_offset(const void *t_);

static void draw_toasts(void)
{
    for (int i = 0; i < NTOAST; i++) {
        if (!toasts[i].expire)
            continue;
        struct rect r = toast_rect(i);
        r.x += toast_offset(&toasts[i]);
        gfx_blend(r.x + 3, r.y + 3, r.w, r.h, 0, 70);
        gfx_fill(r.x, r.y, r.w, r.h, RGB(0x20, 0x28, 0x32));
        gfx_frame(r.x, r.y, r.w, r.h, RGB(0x50, 0x62, 0x74));
        gfx_fill(r.x, r.y, 4, r.h, g_accent);
        gfx_text(r.x + 14, r.y + 10, toasts[i].text, C_WHITE);
    }
}

static void expire_toasts(u32 now)
{
    for (int i = 0; i < NTOAST; i++) {
        if (toasts[i].expire && (int)(now - toasts[i].expire) >= 0) {
            toasts[i].expire = 0;
            invalidate(SCREEN_W - 330, PANEL_H, 330, NTOAST * 42 + 12);
        }
    }
}

/* ---------------- windows ---------------- */

static void z_remove(struct win *w)
{
    int j = 0;
    for (int i = 0; i < nz; i++)
        if (zorder[i] != w)
            zorder[j++] = zorder[i];
    nz = j;
}

static void raise_win(struct win *w)
{
    z_remove(w);
    zorder[nz++] = w;
    if (w->minimized)
        ghost_restore(w);
    w->minimized = 0;
}

static void set_focus(struct win *w)
{
    if (focus == w)
        return;
    if (focus && focus->used)
        wm_invalidate(focus);
    focus = w;
    if (w)
        wm_invalidate(w);
    invalidate(0, 0, SCREEN_W, PANEL_H);                 /* taskbar highlight */
}

struct win *wm_open(const struct app *app, int w, int h, const char *title, void *data)
{
    struct win *win = NULL;
    for (int i = 0; i < MAXWIN; i++)
        if (!wins[i].used) {
            win = &wins[i];
            break;
        }
    if (!win)
        return NULL;
    memset(win, 0, sizeof *win);
    win->used = 1;
    win->app = app;
    win->data = data;
    win->w = w;
    win->h = h;
    win->minw = 220;
    win->minh = 120;
    safestrcpy(win->title, title, sizeof win->title);
    int step = opened++ % 8;
    win->x = 110 + step * 30;
    win->y = PANEL_H + 24 + step * 26;
    if (win->x + w > SCREEN_W)
        win->x = SCREEN_W - w;
    if (win->y + h > SCREEN_H - DOCK_H)
        win->y = SCREEN_H - DOCK_H - h > PANEL_H ? SCREEN_H - DOCK_H - h : PANEL_H;
    zorder[nz++] = win;
    set_focus(win);
    ghost_open(win);
    wm_invalidate(win);
    return win;
}

void wm_set_title(struct win *w, const char *title)
{
    safestrcpy(w->title, title, sizeof w->title);
    wm_invalidate_rect(w->x, w->y, w->w, TITLE_H);
    invalidate(0, 0, SCREEN_W, PANEL_H);
}

static void focus_top(void)
{
    focus = NULL;
    for (int i = nz - 1; i >= 0; i--)
        if (!zorder[i]->minimized) {
            set_focus(zorder[i]);
            break;
        }
}

void wm_close(struct win *w)
{
    if (!w->used)
        return;
    ghost_close(w);
    wm_invalidate(w);
    if (w->app->close)
        w->app->close(w);
    w->used = 0;
    z_remove(w);
    if (drag_win == w)
        drag_mode = DR_NONE;
    if (focus == w)
        focus_top();
    invalidate(0, 0, SCREEN_W, PANEL_H);
}

void wm_try_close(struct win *w)
{
    if (w->app->request_close && w->app->request_close(w))
        return;
    wm_close(w);
}

#define WORK_H (SCREEN_H - PANEL_H - DOCK_H + 10)

static void set_geometry(struct win *w, int x, int y, int ww, int hh)
{
    wm_invalidate(w);
    if (!w->maximized) {
        w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
    }
    w->x = x; w->y = y; w->w = ww; w->h = hh;
    w->maximized = 1;
    wm_invalidate(w);
}

static void toggle_maximize(struct win *w)
{
    if (w->maximized) {
        wm_invalidate(w);
        w->x = w->sx; w->y = w->sy; w->w = w->sw; w->h = w->sh;
        w->maximized = 0;
        wm_invalidate(w);
    } else {
        set_geometry(w, 0, PANEL_H, SCREEN_W, WORK_H);
    }
}

static void minimize(struct win *w)
{
    ghost_minimize(w);
    w->minimized = 1;
    wm_invalidate(w);
    if (focus == w)
        focus_top();
    invalidate(0, 0, SCREEN_W, PANEL_H);
}

static void activate(struct win *w)
{
    raise_win(w);
    set_focus(w);
    wm_invalidate(w);
}

/* ---------------- drawing: windows ---------------- */

static void title_button(int x, int y, int kind, int active)
{
    /* glossy gel buttons: a dark rim, the colored body, a bright upper reflection and a glow at the bottom */
    u32 body = kind == 2 ? RGB(0xd2, 0x3c, 0x2c) : gfx_mix(RGB(0x30, 0x68, 0xb0), g_accent, 110);
    if (!active)
        body = gfx_mix(body, RGB(0xa8, 0xb4, 0xc0), 170);
    gfx_round_rect(x, y, 18, 16, 5, 0xFF000000 | gfx_mix(body, 0, 110), 0);
    gfx_round_rect(x + 1, y + 1, 16, 14, 4, 0xFF000000 | body, 0);
    gfx_round_rect(x + 3, y + 2, 12, 6, 3, 0x8CFFFFFF, 0);
    gfx_round_rect(x + 4, y + 10, 10, 4, 2, 0x30FFFFFF, 0);
    u32 c = C_WHITE;
    if (kind == 0)
        gfx_fill(x + 5, y + 10, 8, 2, c);                         /* minimize */
    else if (kind == 1)
        gfx_frame(x + 5, y + 4, 8, 8, c);                         /* maximize */
    else
        for (int i = 0; i < 8; i++) {                             /* close: an X */
            gfx_fill(x + 5 + i, y + 4 + i, 2, 1, c);
            gfx_fill(x + 12 - i, y + 4 + i, 2, 1, c);
        }
}

static void draw_window(struct win *w, struct rect damage_clip)
{
    int act = w == focus;
    gfx_blend(w->x + 4, w->y + 5, w->w, w->h, 0, 24);            /* soft shadow */
    gfx_blend(w->x + 2, w->y + 3, w->w, w->h, 0, 24);
    /* Glass: the title bar and edges are tinted, see-through panes with a glossy highlight on the top half. */
    u32 tint = act ? gfx_mix(RGB(0x1c, 0x4c, 0x88), g_accent, 100) : RGB(0x7c, 0x8e, 0xa2);
    gfx_blend(w->x, w->y, w->w, TITLE_H, tint, act ? 170 : 130);
    gfx_blend(w->x + 1, w->y + 1, w->w - 2, TITLE_H / 2 - 1, C_WHITE, act ? 64 : 44);
    gfx_blend(w->x + 1, w->y + 1, w->w - 2, 1, C_WHITE, 150);
    gfx_blend(w->x, w->y + TITLE_H - 1, w->w, 1, 0, 90);
    gfx_frame(w->x, w->y, w->w, w->h, act ? gfx_mix(tint, 0, 140) : RGB(0x50, 0x5c, 0x68));
    gfx_fill(w->x + 1, w->y + TITLE_H, w->w - 2, w->h - TITLE_H - 1, C_WIN_EDGE);

    char t[64];
    int maxc = (w->w - 110) / 8;
    safestrcpy(t, w->title, sizeof t);
    if ((int)strlen(t) > maxc && maxc > 3)
        strcpy(t + maxc - 3, "...");
    int tw = gfx_text_width(t);
    gfx_text(w->x + (w->w - tw) / 2 + 1, w->y + 6, t, RGB(0x10, 0x18, 0x20));
    gfx_text(w->x + (w->w - tw) / 2, w->y + 5, t, act ? C_WHITE : RGB(0xee, 0xf0, 0xf2));

    title_button(w->x + w->w - 24, w->y + 5, 2, act);
    title_button(w->x + w->w - 46, w->y + 5, 1, act);
    title_button(w->x + w->w - 68, w->y + 5, 0, act);

    struct rect c = wm_client(w);
    struct rect vis = rect_isect(c, damage_clip);
    if (vis.w > 0) {
        gfx_set_clip(vis.x, vis.y, vis.w, vis.h);
        gfx_fill(c.x, c.y, c.w, c.h, C_WIN_BG);
        if (w->app->draw)
            w->app->draw(w, c);
        gfx_set_clip(damage_clip.x, damage_clip.y, damage_clip.w, damage_clip.h);
    }
    if (!w->maximized) {                                         /* resize grip */
        for (int i = 0; i < 3; i++)
            gfx_fill(w->x + w->w - 5 - i * 4, w->y + w->h - 3, 3, 1, RGB(0x70, 0x78, 0x80));
    }
}

/* ---------------- actions and menus ---------------- */

enum { A_WEB, A_TERM, A_FILES, A_EDIT, A_MON, A_SOFT, A_SETTINGS, A_RUN, A_INSTALL, A_ABOUT, A_CONSOLE,
       A_LOGOUT, A_RESTART, A_POWER, A_WALLPAPER, A_SEP };

struct menu_item {
    const char *label;
    int icon, action;
};

struct popup {
    struct menu_item items[20];
    int n, x, y, hover;
};

static struct popup app_menu, ctx_menu;
static struct popup *open_popup;
static int installable;

static void add_item(struct popup *p, const char *label, int icon, int action)
{
    p->items[p->n++] = (struct menu_item){label, icon, action};
}

static void build_menus(void)
{
    app_menu.n = 0;
    add_item(&app_menu, "Web Browser", ICON_WEB, A_WEB);
    add_item(&app_menu, "Terminal", ICON_TERM, A_TERM);
    add_item(&app_menu, "File Manager", ICON_FILES, A_FILES);
    add_item(&app_menu, "Text Editor", ICON_EDITOR, A_EDIT);
    add_item(&app_menu, "System Monitor", ICON_MONITOR, A_MON);
    add_item(&app_menu, "Software", ICON_SOFTWARE, A_SOFT);
    add_item(&app_menu, "Run Program...", ICON_RUN, A_RUN);
    add_item(&app_menu, "", 0, A_SEP);
    add_item(&app_menu, "Settings", ICON_SETTINGS, A_SETTINGS);
    if (installable)
        add_item(&app_menu, "Install Petrichor", ICON_INSTALL, A_INSTALL);
    add_item(&app_menu, "About Petrichor", ICON_ABOUT, A_ABOUT);
    add_item(&app_menu, "", 0, A_SEP);
    add_item(&app_menu, "Exit to Console", ICON_TERM, A_CONSOLE);
    add_item(&app_menu, "Log Out", ICON_LOGOUT, A_LOGOUT);
    add_item(&app_menu, "Restart", ICON_RESTART, A_RESTART);
    add_item(&app_menu, "Shut Down", ICON_POWER, A_POWER);

    ctx_menu.n = 0;
    add_item(&ctx_menu, "Web Browser", ICON_WEB, A_WEB);
    add_item(&ctx_menu, "Open Terminal", ICON_TERM, A_TERM);
    add_item(&ctx_menu, "File Manager", ICON_FILES, A_FILES);
    add_item(&ctx_menu, "Text Editor", ICON_EDITOR, A_EDIT);
    add_item(&ctx_menu, "", 0, A_SEP);
    add_item(&ctx_menu, "Change Wallpaper", ICON_PALETTE, A_WALLPAPER);
    add_item(&ctx_menu, "Settings", ICON_SETTINGS, A_SETTINGS);
    add_item(&ctx_menu, "", 0, A_SEP);
    add_item(&ctx_menu, "About Petrichor", ICON_ABOUT, A_ABOUT);
}

int wm_installer_available(void) { return installable; }

static int item_h(const struct menu_item *it) { return it->action == A_SEP ? 9 : 30; }

static struct rect popup_rect(struct popup *p)
{
    int h = 8;
    for (int i = 0; i < p->n; i++)
        h += item_h(&p->items[i]);
    return (struct rect){p->x, p->y, 230, h};
}

static int popup_index_at(struct popup *p, int x, int y)
{
    struct rect m = popup_rect(p);
    if (!rect_hit(m, x, y))
        return -1;
    int top = m.y + 4;
    for (int i = 0; i < p->n; i++) {
        int h = item_h(&p->items[i]);
        if (y >= top && y < top + h)
            return p->items[i].action == A_SEP ? -1 : i;
        top += h;
    }
    return -1;
}

static int popup_reveal_h(int full);

static void popup_draw(struct popup *p)
{
    struct rect m = popup_rect(p);
    struct rect saved = gfx_get_clip();
    int reveal = popup_reveal_h(m.h + 6);
    if (reveal < m.h + 6) {                                      /* unrolling: only the top part shows */
        struct rect c = rect_isect(saved, (struct rect){m.x, m.y, m.w + 6, reveal});
        gfx_set_clip(c.x, c.y, c.w, c.h);
    }
    gfx_blend(m.x + 3, m.y + 3, m.w, m.h, 0, 60);
    gfx_fill(m.x, m.y, m.w, m.h, RGB(0xf4, 0xf5, 0xf6));
    gfx_frame(m.x, m.y, m.w, m.h, RGB(0x50, 0x5a, 0x64));
    int y = m.y + 4;
    for (int i = 0; i < p->n; i++) {
        int h = item_h(&p->items[i]);
        if (p->items[i].action == A_SEP) {
            gfx_hline(m.x + 8, y + 4, m.w - 16, RGB(0xc0, 0xc6, 0xcc));
        } else {
            if (p->hover == i)
                gfx_fill(m.x + 2, y, m.w - 4, h, g_accent);
            icon_draw(p->items[i].icon, m.x + 10, y + 5, 20);
            gfx_text(m.x + 40, y + 7, p->items[i].label, p->hover == i ? C_WHITE : C_TEXT);
        }
        y += h;
    }
    gfx_set_clip(saved.x, saved.y, saved.w, saved.h);
}

static void popup_close(void)
{
    if (!open_popup)
        return;
    struct rect m = popup_rect(open_popup);
    invalidate(m.x, m.y, m.w + 6, m.h + 6);
    invalidate(0, 0, 130, PANEL_H);
    open_popup = NULL;
}

static void popup_show(struct popup *p, int x, int y)
{
    popup_close();
    p->hover = -1;
    p->x = x;
    p->y = y;
    struct rect m = popup_rect(p);
    if (p->x + m.w > SCREEN_W) p->x = SCREEN_W - m.w - 2;
    if (p->y + m.h > SCREEN_H) p->y = SCREEN_H - m.h - 2;
    open_popup = p;
    popup_started = uptime();
    m = popup_rect(p);
    invalidate(m.x, m.y, m.w + 6, m.h + 6);
    invalidate(0, 0, 130, PANEL_H);
}

static void launch(int action)
{
    switch (action) {
    case A_WEB:      browser_open(NULL); break;
    case A_TERM:     term_open(NULL); break;
    case A_FILES:    files_open("/"); break;
    case A_EDIT:     editor_open(NULL); break;
    case A_MON:      sysmon_open(); break;
    case A_SOFT:     software_open(); break;
    case A_SETTINGS: settings_open(); break;
    case A_RUN:      run_open(); break;
    case A_INSTALL:  installer_open(); break;
    case A_ABOUT:    about_open(); break;
    case A_CONSOLE:  running = 0; exit_code = 10; break;
    case A_LOGOUT:   running = 0; break;
    case A_RESTART:  running = 0; want_reboot = 1; break;
    case A_POWER:    running = 0; want_poweroff = 1; break;
    case A_WALLPAPER: theme_set((g_wall + 1) % NWALL, g_accent_idx); break;
    }
}

/* ---------------- desktop icons ---------------- */

struct dicon {
    const char *label, *path;                          /* path == NULL: the installer */
    int icon;
};

static struct dicon dicons[4] = {
    {"Computer", "/", ICON_DISK},
    {"Home", "/disk", ICON_HOME},
    {"Programs", "/bin", ICON_FOLDER},
};
static int ndicons = 3;

static struct rect dicon_rect(int i)
{
    return (struct rect){14, PANEL_H + 18 + i * 92, 84, 84};
}

static void draw_desktop_icons(void)
{
    for (int i = 0; i < ndicons; i++) {
        int x = 28, y = PANEL_H + 24 + i * 92;
        if (icon_sel == i)
            gfx_blend(x - 14, y - 6, 84, 84, g_accent, 90);
        icon_draw(dicons[i].icon, x, y, 48);
        const char *label = dicons[i].path ? dicons[i].label : "Install";
        int tw = gfx_text_width(label);
        gfx_text(x + 25 - tw / 2, y + 56, label, 0x000000);
        gfx_text(x + 24 - tw / 2, y + 55, label, C_WHITE);
    }
}

/* ---------------- panel: taskbar, tray, clock, calendar ---------------- */

static const char *const month_name[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const month_full[] = {"January", "February", "March", "April", "May", "June", "July",
                                         "August", "September", "October", "November", "December"};
static const char *const day_name[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static int weekday(int y, int m, int d)
{
    static const int off[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3)
        y--;
    return (y + y / 4 - y / 100 + y / 400 + off[m - 1] + d) % 7;
}

static int days_in(int y, int m)
{
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0) ? 29 : d[m - 1];
}

static void clock_text(char *buf, int n)
{
    struct rtc_time t;
    gettime(&t);
    int m = t.month < 1 || t.month > 12 ? 1 : t.month;
    snprintf(buf, n, "%s %d %s  %02d:%02d", day_name[weekday(t.year, m, t.day)], t.day, month_name[m - 1],
             t.hour, t.min);
}

static int cal_open, cal_year, cal_month;
static struct rect cal_rect(void) { return (struct rect){SCREEN_W - 252, PANEL_H + 2, 248, 214}; }

static void cal_show(void)
{
    struct rtc_time t;
    gettime(&t);
    cal_year = t.year;
    cal_month = t.month < 1 || t.month > 12 ? 1 : t.month;
    cal_open = 1;
    struct rect r = cal_rect();
    invalidate(r.x, r.y, r.w + 6, r.h + 6);
}

static void cal_close(void)
{
    cal_open = 0;
    struct rect r = cal_rect();
    invalidate(r.x, r.y, r.w + 6, r.h + 6);
}

static void draw_calendar(void)
{
    struct rect r = cal_rect();
    gfx_blend(r.x + 3, r.y + 3, r.w, r.h, 0, 60);
    gfx_fill(r.x, r.y, r.w, r.h, RGB(0xf4, 0xf5, 0xf6));
    gfx_frame(r.x, r.y, r.w, r.h, RGB(0x50, 0x5a, 0x64));
    gfx_fill(r.x + 1, r.y + 1, r.w - 2, 30, g_accent);
    char head[40];
    snprintf(head, sizeof head, "%s %d", month_full[cal_month - 1], cal_year);
    gfx_text(r.x + (r.w - gfx_text_width(head)) / 2, r.y + 8, head, C_WHITE);
    gfx_text(r.x + 10, r.y + 8, "<", C_WHITE);
    gfx_text(r.x + r.w - 18, r.y + 8, ">", C_WHITE);
    for (int i = 0; i < 7; i++)
        gfx_text(r.x + 10 + i * 32, r.y + 38, day_name[i], C_TEXT_DIM);
    struct rtc_time t;
    gettime(&t);
    int first = weekday(cal_year, cal_month, 1), nd = days_in(cal_year, cal_month);
    for (int d = 1; d <= nd; d++) {
        int slot = first + d - 1, col = slot % 7, row = slot / 7;
        int x = r.x + 8 + col * 32, y = r.y + 58 + row * 24;
        int today = d == t.day && cal_month == t.month && cal_year == t.year;
        if (today)
            gfx_fill(x - 2, y - 3, 28, 22, g_accent);
        char num[4];
        snprintf(num, sizeof num, "%d", d);
        gfx_text(x + 12 - gfx_text_width(num) / 2, y, num, today ? C_WHITE : C_TEXT);
    }
}

#define APPBTN_W 128
#define TRAY_W   150

static struct rect taskbar_rect(int idx, int count)
{
    int x0 = APPBTN_W + 8, x1 = SCREEN_W - 200 - TRAY_W;
    int w = (x1 - x0) / (count ? count : 1);
    if (w > 170)
        w = 170;
    return (struct rect){x0 + idx * (w + 2), 2, w, PANEL_H - 4};
}

static int cpu_pct, mem_mb, net_up;

static void sample_tray(void)
{
    static u32 prev_ticks, prev_now;
    struct procinfo pi[64];
    int n = ps(pi, 64);
    u32 total = 0, now = uptime();
    for (int i = 0; i < n; i++)
        if (pi[i].pid != 0)
            total += pi[i].ticks;
    u32 dt = now - prev_now;
    if (prev_now && dt) {
        u32 pct = (total - prev_ticks) * 100 / dt;
        cpu_pct = pct > 100 ? 100 : pct;
    }
    prev_ticks = total;
    prev_now = now;
    struct meminfo mi;
    meminfo(&mi);
    mem_mb = (mi.total_kb - mi.free_kb) / 1024;
    struct netinfo ni;
    netinfo(&ni);
    net_up = ni.up;
    invalidate(SCREEN_W - 200 - TRAY_W, 0, TRAY_W, PANEL_H);
}

static void draw_panel(void)
{
    /* a smoked-glass bar: the wallpaper shows through, with a gloss on the upper half */
    gfx_blend(0, 0, SCREEN_W, PANEL_H, RGB(0x08, 0x16, 0x28), 175);
    gfx_blend(0, 0, SCREEN_W, PANEL_H / 2, C_WHITE, 30);
    gfx_blend(0, 0, SCREEN_W, 1, C_WHITE, 80);
    gfx_hline(0, PANEL_H - 1, SCREEN_W, RGB(0x04, 0x0a, 0x12));

    /* the Applications button: a gel pill in the accent color */
    u32 pill = open_popup == &app_menu ? gfx_mix(g_accent, C_WHITE, 60) : g_accent;
    gfx_round_rect(2, 2, APPBTN_W - 4, PANEL_H - 5, 11, 0xFF000000 | gfx_mix(pill, 0, 120), 0);
    gfx_round_rect(3, 3, APPBTN_W - 6, PANEL_H - 7, 10, 0xFF000000 | pill, 0);
    gfx_round_rect(6, 4, APPBTN_W - 12, (PANEL_H - 7) / 2, 6, 0x70FFFFFF, 0);
    icon_draw(ICON_APPS, 6, 5, 18);
    gfx_text(30, 6, "Applications", C_WHITE);

    int count = 0;
    for (int i = 0; i < MAXWIN; i++)
        count += wins[i].used;
    int idx = 0;
    for (int i = 0; i < MAXWIN; i++) {
        if (!wins[i].used)
            continue;
        struct rect r = taskbar_rect(idx++, count);
        int act = &wins[i] == focus && !wins[i].minimized;
        u32 tb = act ? gfx_mix(g_accent, RGB(0x10, 0x30, 0x60), 90) : RGB(0x40, 0x58, 0x74);
        gfx_round_rect(r.x, r.y, r.w, r.h, 5, (act ? 0xD0000000u : 0x70000000u) | tb, 0);
        gfx_round_rect(r.x + 2, r.y + 1, r.w - 4, r.h / 2, 4, act ? 0x60FFFFFF : 0x28FFFFFF, 0);
        gfx_round_rect(r.x, r.y, r.w, r.h, 5, act ? 0x90FFFFFF : 0x40FFFFFF, 1);
        char t[32];
        int maxc = (r.w - 12) / 8;
        safestrcpy(t, wins[i].title, sizeof t);
        if ((int)strlen(t) > maxc && maxc > 3)
            strcpy(t + maxc - 3, "...");
        gfx_text(r.x + 6, r.y + 4, t, wins[i].minimized ? RGB(0x90, 0x9a, 0xa4) : C_WHITE);
    }

    int tx = SCREEN_W - 200 - TRAY_W + 6;                        /* tray: network, CPU, memory */
    gfx_fill(tx, 10, 8, 8, net_up ? RGB(0x4e, 0xc9, 0x4e) : RGB(0x80, 0x88, 0x90));
    char t[40];
    snprintf(t, sizeof t, "CPU %d%%", cpu_pct);
    gfx_text(tx + 14, 6, t, RGB(0xc8, 0xd0, 0xd8));
    snprintf(t, sizeof t, "%dM", mem_mb);
    gfx_text(tx + 90, 6, t, RGB(0xc8, 0xd0, 0xd8));

    char clk[40];
    clock_text(clk, sizeof clk);
    gfx_text(SCREEN_W - gfx_text_width(clk) - 10, 6, clk, C_WHITE);
    gfx_text(SCREEN_W - 200 + 8, 6, "root", RGB(0xa8, 0xb4, 0xc0));
}

/* ---------------- dock ---------------- */

struct dock_item {
    const char *label;
    int icon, action;
};

static const struct dock_item dock_items[] = {
    {"Web Browser", ICON_WEB, A_WEB},
    {"Terminal", ICON_TERM, A_TERM},
    {"File Manager", ICON_FILES, A_FILES},
    {"Text Editor", ICON_EDITOR, A_EDIT},
    {"System Monitor", ICON_MONITOR, A_MON},
    {"Software", ICON_SOFTWARE, A_SOFT},
    {"Settings", ICON_SETTINGS, A_SETTINGS},
};
#define NDOCK 7

static struct rect dock_rect(void)
{
    int w = NDOCK * 54 + 16;
    return (struct rect){(SCREEN_W - w) / 2, SCREEN_H - 58, w, 52};
}

static struct rect dock_icon_rect(int i)
{
    struct rect d = dock_rect();
    return (struct rect){d.x + 10 + i * 54, d.y + 6, 44, 44};
}

static int dock_lift_px(int i);

static void draw_dock(void)
{
    struct rect d = dock_rect();
    /* a glass shelf: translucent body, a gloss band on top, a bright rim */
    gfx_round_rect(d.x, d.y, d.w, d.h, 12, 0x9C0C2038, 0);
    gfx_round_rect(d.x + 3, d.y + 2, d.w - 6, d.h / 2 - 2, 10, 0x2EFFFFFF, 0);
    gfx_round_rect(d.x, d.y, d.w, d.h, 12, 0x8CC8E8FF, 1);
    for (int i = 0; i < NDOCK; i++) {
        struct rect r = dock_icon_rect(i);
        if (dock_hover == i)
            gfx_round_rect(r.x - 2, r.y - 2, r.w + 4, r.h + 4, 9, 0x50FFFFFF, 0);
        icon_draw(dock_items[i].icon, r.x + 6, r.y + 4 - dock_lift_px(i), 32);
    }
    if (dock_hover >= 0) {
        struct rect r = dock_icon_rect(dock_hover);
        int tw = gfx_text_width(dock_items[dock_hover].label) + 16;
        int tx = r.x + r.w / 2 - tw / 2;
        gfx_round_rect(tx, d.y - 26, tw, 22, 6, 0xE0102438, 0);
        gfx_round_rect(tx, d.y - 26, tw, 22, 6, 0x90C8E8FF, 1);
        gfx_text(tx + 8, d.y - 23, dock_items[dock_hover].label, C_WHITE);
    }
}

/* ---------------- animations ----------------
 *
 * Windows open, close, minimize and restore by animating a "ghost": a snapshot of the window drawn
 * scaled and faded.  Menus unroll, dock icons lift when hovered and bounce when launched, and
 * notifications slide in.  Everything follows the clock (uptime ticks), so slow frames just skip ahead. */

static int ease_out(int t)                                       /* t, result: 0..256 */
{
    int u = 256 - t;
    return 256 - u * u / 256 * u / 256;
}

enum { GH_OPEN, GH_CLOSE, GH_MIN, GH_RESTORE };
#define NGHOST 6

struct ghost {
    int used, kind, alpha;
    u32 *px;
    int pw, ph;
    struct rect from, to, cur, last;
    int a0, a1;
    u32 start;
    int dur;
    struct win *win;                                              /* hidden until the ghost lands */
};

static struct ghost ghosts[NGHOST];

static struct rect lerp_rect(struct rect a, struct rect b, int e)
{
    return (struct rect){a.x + (b.x - a.x) * e / 256, a.y + (b.y - a.y) * e / 256,
                         a.w + (b.w - a.w) * e / 256, a.h + (b.h - a.h) * e / 256};
}

static struct rect scaled_rect(struct rect r, int pct)
{
    int w = r.w * pct / 100, h = r.h * pct / 100;
    return (struct rect){r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h};
}

static struct rect taskbar_slot(struct win *w)
{
    int count = 0, idx = 0;
    for (int i = 0; i < MAXWIN; i++) {
        if (!wins[i].used)
            continue;
        if (&wins[i] == w)
            idx = count;
        count++;
    }
    return taskbar_rect(idx, count);
}

/* Draws the window, as it looks right now, into a fresh buffer of its own size. */
static u32 *capture_window(struct win *w)
{
    if (w->w <= 0 || w->h <= 0 || (long)w->w * w->h > SCREEN_W * SCREEN_H)
        return NULL;
    u32 *buf = malloc(w->w * w->h * 4);
    if (!buf)
        return NULL;
    struct rect old = gfx_get_clip();
    gfx_set_target(buf, w->w, w->h);
    int ox = w->x, oy = w->y, hid = w->hide;
    w->x = w->y = 0;
    w->hide = 0;
    draw_window(w, (struct rect){0, 0, w->w, w->h});
    w->x = ox;
    w->y = oy;
    w->hide = hid;
    gfx_set_target(back, SCREEN_W, SCREEN_H);
    gfx_set_clip(old.x, old.y, old.w, old.h);
    return buf;
}

static void ghost_add(int kind, struct win *w, u32 *px, struct rect from, struct rect to, int a0, int a1, int dur)
{
    for (int i = 0; i < NGHOST; i++) {
        if (ghosts[i].used)
            continue;
        struct ghost *g = &ghosts[i];
        memset(g, 0, sizeof *g);
        g->used = 1;
        g->kind = kind;
        g->px = px;
        g->pw = w->w;
        g->ph = w->h;
        g->from = g->cur = g->last = from;
        g->to = to;
        g->a0 = a0;
        g->a1 = a1;
        g->start = uptime();
        g->dur = dur;
        g->win = (kind == GH_OPEN || kind == GH_RESTORE) ? w : NULL;
        g->alpha = a0;
        return;
    }
    free(px);
    if (kind == GH_OPEN || kind == GH_RESTORE)
        w->hide = 0;
}

static void ghost_open(struct win *w)
{
    if (!g_animations || !running)
        return;
    struct rect full = {w->x, w->y, w->w, w->h};
    w->hide = 1;
    ghost_add(GH_OPEN, w, NULL, scaled_rect(full, 90), full, 0, 255, 16);
}

static void ghost_close(struct win *w)
{
    if (!g_animations || !running || w->minimized || w->hide)
        return;
    u32 *px = capture_window(w);
    if (!px)
        return;
    struct rect full = {w->x, w->y, w->w, w->h};
    ghost_add(GH_CLOSE, w, px, full, scaled_rect(full, 88), 255, 0, 13);
}

static void ghost_minimize(struct win *w)
{
    if (!g_animations || !running || w->hide)
        return;
    u32 *px = capture_window(w);
    if (!px)
        return;
    struct rect full = {w->x, w->y, w->w, w->h}, slot = taskbar_slot(w);
    ghost_add(GH_MIN, w, px, full, slot, 255, 40, 18);
}

static void ghost_restore(struct win *w)
{
    if (!g_animations || !running)
        return;
    u32 *px = capture_window(w);
    if (!px)
        return;
    struct rect full = {w->x, w->y, w->w, w->h}, slot = taskbar_slot(w);
    w->hide = 1;
    ghost_add(GH_RESTORE, w, px, slot, full, 40, 255, 18);
}

static int popup_reveal_h(int full)
{
    if (!g_animations || !open_popup)
        return full;
    int t = (int)(uptime() - popup_started) * 256 / 12;
    if (t >= 256)
        return full;
    return full * ease_out(t < 0 ? 0 : t) / 256;
}

static int toast_offset(const void *t_)
{
    const u32 *born = (const u32 *)((const char *)t_ + 72 + 4);         /* struct layout: text[72], expire, born */
    if (!g_animations)
        return 0;
    int t = (int)(uptime() - *born) * 256 / 14;
    if (t >= 256)
        return 0;
    return (256 - ease_out(t < 0 ? 0 : t)) * 340 / 256;
}

static int dock_lift[16];                                        /* 0..256 */

static int dock_lift_px(int i)
{
    int lift = g_animations ? dock_lift[i] * 7 / 256 : 0;
    if (g_animations && dock_bounce[i]) {
        int ph = (int)(uptime() - dock_bounce[i]);
        if (ph >= 0 && ph < 60) {
            int tri = ph % 20 < 10 ? ph % 20 : 20 - ph % 20, amp = 16 - (ph / 20) * 5;
            lift += tri * amp / 10;
        }
    }
    return lift;
}

static int anim_active(void)
{
    if (!g_animations)
        return 0;
    for (int i = 0; i < NGHOST; i++)
        if (ghosts[i].used)
            return 1;
    u32 now = uptime();
    if (open_popup && (int)(now - popup_started) < 14)
        return 1;
    for (int i = 0; i < NDOCK; i++)
        if (dock_lift[i] != (dock_hover == i ? 256 : 0) || (dock_bounce[i] && (int)(now - dock_bounce[i]) < 62))
            return 1;
    for (int i = 0; i < NTOAST; i++)
        if (toasts[i].expire && (int)(now - toasts[i].born) < 16)
            return 1;
    return 0;
}

static struct rect grow(struct rect r, int n) { return (struct rect){r.x - n, r.y - n, r.w + 2 * n, r.h + 2 * n}; }

static void anim_step(u32 now)
{
    if (!g_animations) {
        for (int i = 0; i < NGHOST; i++)
            if (ghosts[i].used) {                                  /* switched off mid-animation: finish at once */
                ghosts[i].start = now - 1000;
            }
    }
    for (int i = 0; i < NGHOST; i++) {
        struct ghost *g = &ghosts[i];
        if (!g->used)
            continue;
        if (g->kind == GH_OPEN && !g->px) {                        /* the app has drawn its first frame by now */
            if (g->win && g->win->used)
                g->px = capture_window(g->win);
            if (!g->px) {
                if (g->win) g->win->hide = 0;
                invalidate(grow(g->to, 8).x, grow(g->to, 8).y, grow(g->to, 8).w, grow(g->to, 8).h);
                g->used = 0;
                continue;
            }
        }
        int t = (int)(now - g->start) * 256 / g->dur;
        if (t > 256) t = 256;
        if (t < 0) t = 0;
        int e = ease_out(t);
        g->cur = lerp_rect(g->from, g->to, e);
        g->alpha = g->a0 + (g->a1 - g->a0) * e / 256;
        struct rect u = grow(rect_union(g->last, g->cur), 8);
        invalidate(u.x, u.y, u.w, u.h);
        g->last = g->cur;
        if (t >= 256) {
            if (g->win && g->win->used) {
                g->win->hide = 0;
                wm_invalidate(g->win);
            }
            free(g->px);
            g->px = NULL;
            g->used = 0;
        }
    }
    int moved = 0;
    for (int i = 0; i < NDOCK; i++) {
        int target = g_animations && dock_hover == i ? 256 : 0, old = dock_lift[i];
        dock_lift[i] += (target - dock_lift[i]) * 2 / 5;
        if (abs_i(target - dock_lift[i]) < 12)
            dock_lift[i] = target;
        if (dock_lift[i] != old || (dock_bounce[i] && (int)(now - dock_bounce[i]) < 62))
            moved = 1;
        if (dock_bounce[i] && (int)(now - dock_bounce[i]) >= 62)
            dock_bounce[i] = 0;
    }
    if (moved) {
        struct rect d = dock_rect();
        invalidate(d.x, d.y - 30, d.w, d.h + 30);
    }
    if (open_popup && (int)(now - popup_started) < 14) {
        struct rect m = popup_rect(open_popup);
        invalidate(m.x, m.y, m.w + 6, m.h + 6);
    }
    for (int i = 0; i < NTOAST; i++)
        if (toasts[i].expire && (int)(now - toasts[i].born) < 16)
            invalidate(SCREEN_W - 330, PANEL_H, 330, NTOAST * 42 + 12);
}

/* Draws a snapshot scaled into d, blended with the screen at alpha 0..255. */
static void blit_scaled(const u32 *src, int sw, int sh, struct rect d, int alpha)
{
    if (d.w <= 0 || d.h <= 0 || alpha <= 0)
        return;
    struct rect c = rect_isect(d, gfx_get_clip());
    if (c.w <= 0)
        return;
    static int xmap[MAX_SCREEN_W];
    for (int x = 0; x < c.w; x++)
        xmap[x] = (int)((long)(c.x + x - d.x) * sw / d.w);
    for (int y = c.y; y < c.y + c.h; y++) {
        const u32 *row = src + (int)((long)(y - d.y) * sh / d.h) * sw;
        u32 *out = back + y * SCREEN_W + c.x;
        if (alpha >= 255)
            for (int x = 0; x < c.w; x++)
                out[x] = row[xmap[x]];
        else
            for (int x = 0; x < c.w; x++)
                out[x] = gfx_mix(out[x], row[xmap[x]], alpha);
    }
}

static void anim_draw(void)
{
    for (int i = 0; i < NGHOST; i++)
        if (ghosts[i].used && ghosts[i].px)
            blit_scaled(ghosts[i].px, ghosts[i].pw, ghosts[i].ph, ghosts[i].cur, ghosts[i].alpha);
}

/* ---------------- repaint ---------------- */

static void anim_draw(void);
static int display_apply(int w, int h);
static void screenshot(void);
static int frame_wait(void);

static void repaint(void)
{
    if (!dirty)
        return;
    struct rect d = rect_isect(damage, screen);
    dirty = 0;
    if (d.w <= 0 || d.h <= 0)
        return;
    gfx_set_clip(d.x, d.y, d.w, d.h);
    gfx_copy(wall, 0, 0, SCREEN_W, SCREEN_H);
    draw_desktop_icons();
    for (int i = 0; i < nz; i++)
        if (!zorder[i]->minimized && !zorder[i]->hide)
            draw_window(zorder[i], d);
    anim_draw();
    draw_panel();
    draw_dock();
    draw_toasts();
    if (cal_open)
        draw_calendar();
    if (open_popup)
        popup_draw(open_popup);
    present(d);
    cursor_show();
}

/* ---------------- mouse ---------------- */

static struct win *window_at(int x, int y)
{
    for (int i = nz - 1; i >= 0; i--) {
        struct win *w = zorder[i];
        if (!w->minimized && !w->hide && rect_hit((struct rect){w->x, w->y, w->w, w->h}, x, y))
            return w;
    }
    return NULL;
}

static void on_move(void)
{
    if (drag_mode == DR_MOVE) {
        struct win *w = drag_win;
        wm_invalidate(w);
        w->x = mx - drag_ox;
        w->y = my - drag_oy;
        if (w->y < PANEL_H) w->y = PANEL_H;
        if (w->y > SCREEN_H - 40) w->y = SCREEN_H - 40;
        if (w->x < 40 - w->w) w->x = 40 - w->w;
        if (w->x > SCREEN_W - 40) w->x = SCREEN_W - 40;
        wm_invalidate(w);
        return;
    }
    if (drag_mode == DR_RESIZE) {
        struct win *w = drag_win;
        wm_invalidate(w);
        w->w = mx - w->x + drag_ox;
        w->h = my - w->y + drag_oy;
        if (w->w < w->minw) w->w = w->minw;
        if (w->h < w->minh) w->h = w->minh;
        if (w->x + w->w > SCREEN_W) w->w = SCREEN_W - w->x;
        if (w->y + w->h > SCREEN_H) w->h = SCREEN_H - w->y;
        wm_invalidate(w);
        return;
    }
    if (drag_mode == DR_APP) {
        struct win *w = drag_win;
        struct rect c = wm_client(w);
        if (w->app->mouse)
            w->app->mouse(w, mx - c.x, my - c.y, M_DRAG);
        return;
    }

    int dh = -1;                                                  /* hover feedback */
    struct rect d = dock_rect();
    if (rect_hit(d, mx, my))
        for (int i = 0; i < NDOCK; i++)
            if (rect_hit(dock_icon_rect(i), mx, my))
                dh = i;
    if (dh != dock_hover) {
        dock_hover = dh;
        invalidate(d.x - 120, d.y - 30, d.w + 240, d.h + 30);       /* the tooltip can overhang the dock */
    }
    if (!open_popup && !cal_open) {
        struct win *hw = window_at(mx, my);
        if (hw && hw->app->mouse && !rect_hit(dock_rect(), mx, my) && my >= PANEL_H) {
            struct rect hc = wm_client(hw);
            if (rect_hit(hc, mx, my))
                hw->app->mouse(hw, mx - hc.x, my - hc.y, M_MOVE);
        }
    }
    if (open_popup) {
        int mh = popup_index_at(open_popup, mx, my);
        if (mh != open_popup->hover) {
            open_popup->hover = mh;
            struct rect m = popup_rect(open_popup);
            invalidate(m.x, m.y, m.w, m.h);
        }
    }
}

static void on_down(int dbl)
{
    if (cal_open) {                                               /* calendar popup */
        struct rect r = cal_rect();
        if (rect_hit(r, mx, my)) {
            if (my < r.y + 30 && mx < r.x + 34) { if (--cal_month < 1) { cal_month = 12; cal_year--; } }
            else if (my < r.y + 30 && mx > r.x + r.w - 34) { if (++cal_month > 12) { cal_month = 1; cal_year++; } }
            invalidate(r.x, r.y, r.w + 6, r.h + 6);
            return;
        }
        int on_clock = my < PANEL_H && mx >= SCREEN_W - 200;
        cal_close();
        if (on_clock)
            return;
    }
    if (open_popup) {
        struct popup *p = open_popup;
        int i = popup_index_at(p, mx, my);
        int inside = rect_hit(popup_rect(p), mx, my);
        int was_button = p == &app_menu && my < PANEL_H && mx < APPBTN_W;
        popup_close();
        if (i >= 0)
            launch(p->items[i].action);
        if (inside || was_button)
            return;
    }

    if (my < PANEL_H) {                                           /* panel */
        if (mx < APPBTN_W) {
            popup_show(&app_menu, 2, PANEL_H);
            return;
        }
        if (mx >= SCREEN_W - 200) {
            cal_show();
            return;
        }
        int count = 0;
        for (int i = 0; i < MAXWIN; i++)
            count += wins[i].used;
        int idx = 0;
        for (int i = 0; i < MAXWIN; i++) {
            if (!wins[i].used)
                continue;
            if (rect_hit(taskbar_rect(idx++, count), mx, my)) {
                struct win *w = &wins[i];
                if (w == focus && !w->minimized)
                    minimize(w);
                else
                    activate(w);
                return;
            }
        }
        return;
    }

    if (rect_hit(dock_rect(), mx, my)) {                           /* the dock floats above windows */
        for (int i = 0; i < NDOCK; i++)
            if (rect_hit(dock_icon_rect(i), mx, my))
            {
                dock_bounce[i] = uptime();
                launch(dock_items[i].action);
            }
        return;
    }
    struct win *w = window_at(mx, my);
    if (w) {
        if (w != focus || w != zorder[nz - 1])
            activate(w);
        if (my < w->y + TITLE_H) {
            int rx = w->x + w->w;
            if (mx >= rx - 24 && mx < rx - 6) { wm_try_close(w); return; }
            if (mx >= rx - 46 && mx < rx - 28) { toggle_maximize(w); return; }
            if (mx >= rx - 68 && mx < rx - 50) { minimize(w); return; }
            if (dbl) { toggle_maximize(w); return; }
            drag_mode = DR_MOVE;
            drag_win = w;
            if (w->maximized) {                                    /* grabbing a maximized/snapped title restores it */
                int rel = (mx - w->x) * 256 / w->w;
                wm_invalidate(w);
                w->x = w->sx; w->y = w->sy; w->w = w->sw; w->h = w->sh;
                w->maximized = 0;
                w->x = mx - rel * w->w / 256;
                wm_invalidate(w);
            }
            drag_ox = mx - w->x;
            drag_oy = my - w->y;
            return;
        }
        if (!w->maximized && mx > w->x + w->w - 16 && my > w->y + w->h - 16) {
            drag_mode = DR_RESIZE;
            drag_win = w;
            drag_ox = w->x + w->w - mx;
            drag_oy = w->y + w->h - my;
            return;
        }
        struct rect c = wm_client(w);
        if (rect_hit(c, mx, my)) {
            drag_mode = DR_APP;
            drag_win = w;
            if (w->app->mouse)
                w->app->mouse(w, mx - c.x, my - c.y, dbl ? M_DBL : M_DOWN);
        }
        return;
    }

    for (int i = 0; i < ndicons; i++) {                            /* desktop icons */
        if (rect_hit(dicon_rect(i), mx, my)) {
            if (dbl) {
                if (dicons[i].path)
                    files_open(dicons[i].path);
                else
                    installer_open();
            }
            if (icon_sel != i) {
                icon_sel = i;
                invalidate(0, PANEL_H, 110, 400);
            }
            return;
        }
    }
    if (icon_sel >= 0) {
        icon_sel = -1;
        invalidate(0, PANEL_H, 110, 400);
    }
    set_focus(NULL);
}

static void on_rdown(void)
{
    if (open_popup || cal_open) {
        popup_close();
        if (cal_open)
            cal_close();
        return;
    }
    if (my < PANEL_H || rect_hit(dock_rect(), mx, my))
        return;
    struct win *w = window_at(mx, my);
    if (w) {
        if (w != focus)
            activate(w);
        struct rect c = wm_client(w);
        if (rect_hit(c, mx, my) && w->app->mouse)
            w->app->mouse(w, mx - c.x, my - c.y, M_RDOWN);
        return;
    }
    popup_show(&ctx_menu, mx, my);
}

static void on_up(void)
{
    if (drag_mode == DR_APP && drag_win && drag_win->used) {
        struct win *w = drag_win;
        struct rect c = wm_client(w);
        if (w->app->mouse)
            w->app->mouse(w, mx - c.x, my - c.y, M_UP);
    } else if (drag_mode == DR_MOVE && drag_win && drag_win->used) {
        struct win *w = drag_win;                                  /* snap to screen edges */
        if (my <= PANEL_H + 2)
            set_geometry(w, 0, PANEL_H, SCREEN_W, WORK_H);
        else if (mx <= 2)
            set_geometry(w, 0, PANEL_H, SCREEN_W / 2, WORK_H);
        else if (mx >= SCREEN_W - 3)
            set_geometry(w, SCREEN_W / 2, PANEL_H, SCREEN_W / 2, WORK_H);
    }
    drag_mode = DR_NONE;
}

static void handle_mouse(struct input_event *e)
{
    if ((e->code & BTN_MIDDLE) && !(mbtn & BTN_MIDDLE)) {
        struct win *w = window_at(mx, my);
        if (w && w->app->mouse && rect_hit(wm_client(w), mx, my)) {
            struct rect c = wm_client(w);
            activate(w);
            w->app->mouse(w, mx - c.x, my - c.y, M_MDOWN);
        }
    }
    if (e->flags & IEF_ABS) {                                     /* tablet (VMs): an absolute position */
        int nx = (int)((long)e->dx * SCREEN_W / 32768), ny = (int)((long)e->dy * SCREEN_H / 32768);
        e->dx = nx - mx;
        e->dy = ny - my;
    }
    mx += e->dx;
    my += e->dy;
    if (mx < 0) mx = 0;
    if (my < 0) my = 0;
    if (mx >= SCREEN_W) mx = SCREEN_W - 1;
    if (my >= SCREEN_H) my = SCREEN_H - 1;
    if (e->dx || e->dy)
        on_move();

    int nb = e->code, old = mbtn;
    mbtn = nb;
    if ((nb & BTN_LEFT) && !(old & BTN_LEFT)) {
        u32 now = uptime();
        int dbl = now - last_click_tick < 40 && abs_i(mx - last_click_x) < 5 && abs_i(my - last_click_y) < 5;
        last_click_tick = dbl ? 0 : now;
        last_click_x = mx;
        last_click_y = my;
        on_down(dbl);
    } else if (!(nb & BTN_LEFT) && (old & BTN_LEFT)) {
        on_up();
    }
    if ((nb & BTN_RIGHT) && !(old & BTN_RIGHT))
        on_rdown();
}

static void handle_wheel(int dir)
{
    struct win *w = window_at(mx, my);
    if (w && w->app->wheel && rect_hit(wm_client(w), mx, my))
        w->app->wheel(w, dir);
}

/* ---------------- keyboard ---------------- */

static void cycle_focus(void)
{
    for (int i = 0; i < nz; i++) {                                 /* bottom of the stack comes up */
        if (!zorder[i]->minimized && zorder[i] != focus) {
            activate(zorder[i]);
            return;
        }
    }
}

static void handle_key(struct input_event *e)
{
    if (!(e->flags & IEF_DOWN))
        return;
    if (e->code == 27 && (open_popup || cal_open)) {
        popup_close();
        if (cal_open)
            cal_close();
        return;
    }
    if ((e->flags & IEF_ALT) && e->code == '\t') { cycle_focus(); return; }
    if (e->code == KEY_PRINT) {
        screenshot();
        return;
    }
    if ((e->flags & IEF_ALT) && e->code == KEY_F1 + 3) {           /* Alt+F4 */
        if (focus)
            wm_try_close(focus);
        return;
    }
    if ((e->flags & IEF_ALT) && e->code == KEY_F1 + 1) {           /* Alt+F2: run a program */
        run_open();
        return;
    }
    if ((e->flags & IEF_ALT) && (e->flags & IEF_CTRL) && (e->code == 't' || e->code == 'T')) {
        term_open(NULL);
        return;
    }
    if (focus && focus->app->key)
        focus->app->key(focus, e);
}

/* ---------------- about window ---------------- */

static void about_draw(struct win *w, struct rect c)
{
    gfx_vgrad(c.x, c.y, c.w, 92, walls[g_wall].top, walls[g_wall].bottom);
    gfx_text_scaled(c.x + 20, c.y + 18, "Petrichor", C_WHITE, 3);
    gfx_text(c.x + 22, c.y + 66, "a from-scratch 64-bit Unix", RGB(0xa8, 0xd8, 0xe8));

    struct meminfo mi;
    struct netinfo ni;
    meminfo(&mi);
    netinfo(&ni);
    u32 up = uptime() / 100;
    char line[96];
    int y = c.y + 108;
    snprintf(line, sizeof line, "Version      %s", PETRICHOR_VERSION);
    gfx_text(c.x + 20, y, line, C_TEXT); y += 20;
    snprintf(line, sizeof line, "Memory       %u / %u MiB used", (mi.total_kb - mi.free_kb) / 1024, mi.total_kb / 1024);
    gfx_text(c.x + 20, y, line, C_TEXT); y += 20;
    snprintf(line, sizeof line, "Uptime       %u:%02u:%02u", up / 3600, up / 60 % 60, up % 60);
    gfx_text(c.x + 20, y, line, C_TEXT); y += 20;
    snprintf(line, sizeof line, "Network      %s", ni.up ? inet_ntoa(ni.ip) : "not connected");
    gfx_text(c.x + 20, y, line, C_TEXT); y += 20;
    snprintf(line, sizeof line, "Display      %dx%d, 32-bit", SCREEN_W, SCREEN_H);
    gfx_text(c.x + 20, y, line, C_TEXT); y += 28;
    gfx_text(c.x + 20, y, "Kernel, libc, shell and this desktop", C_TEXT_DIM); y += 18;
    gfx_text(c.x + 20, y, "are all written from scratch.", C_TEXT_DIM);
}

static const struct app about_app = {.draw = about_draw};

void about_open(void)
{
    struct win *w = wm_open(&about_app, 360, 330, "About Petrichor", NULL);
    if (w) {
        w->minw = w->w;
        w->minh = w->h;
    }
}

/* ---------------- main loop ---------------- */

static void gallery_draw(struct win *w, struct rect c)
{
    gfx_fill(c.x, c.y, c.w, c.h, RGB(0x5a, 0x66, 0x74));
    static const int sizes[] = {64, 48, 32, 20, 16};
    for (int k = 0; k < ICON_COUNT; k++) {
        int col = k % 8, row = k / 8, x0 = c.x + 10 + col * 112, y0 = c.y + 10 + row * 150;
        gfx_fill(x0 - 4, y0 - 4, 108, 146, k % 2 ? RGB(0x66, 0x72, 0x80) : RGB(0xe8, 0xeb, 0xee));
        icon_draw(k, x0, y0, 64);
        int x = x0;
        for (int i = 1; i < 5; i++) {
            icon_draw(k, x, y0 + 72 + (i == 1 ? 0 : 0), sizes[i]);
            x += sizes[i] + 4;
        }
        char t[8];
        snprintf(t, sizeof t, "%d", k);
        gfx_text(x0, y0 + 124, t, k % 2 ? C_WHITE : C_TEXT);
    }
}

/* ---------------- display ---------------- */

/* (Re)allocates the drawing buffers for a w x h desktop.  Does not touch the saved setting. */
static int display_apply(int w, int h)
{
    if (w < 640 || h < 480 || w > MAX_SCREEN_W || h > MAX_SCREEN_H)
        return -1;
    u32 *nb = malloc(w * h * 4), *nw = malloc(w * h * 4);
    int *nx = malloc(fb.width * sizeof(int));
    if (!nb || !nw || !nx) {
        free(nb); free(nw); free(nx);
        return -1;
    }
    int ow = screen_w, oh = screen_h;
    free(back); free(wall); free(xsrc);
    back = nb; wall = nw; xsrc = nx;
    screen_w = w; screen_h = h;
    screen = (struct rect){0, 0, w, h};
    for (u32 x = 0; x < fb.width; x++)
        xsrc[x] = (int)((long)x * w / fb.width);
    mx = (int)((long)mx * w / ow); my = (int)((long)my * h / oh);
    if (mx >= w) mx = w - 1;
    if (my >= h) my = h - 1;
    gfx_set_target(back, w, h);
    gfx_set_clip(0, 0, w, h);
    for (int i = 0; i < MAXWIN; i++) {
        struct win *wn = &wins[i];
        if (!wn->used)
            continue;
        if (wn->maximized)
            set_geometry(wn, 0, PANEL_H, w, WORK_H);
        else {
            if (wn->w > w) wn->w = w;
            if (wn->h > h - PANEL_H) wn->h = h - PANEL_H;
            if (wn->x + wn->w > w) wn->x = w - wn->w;
            if (wn->y + 40 > h) wn->y = h - 40 > PANEL_H ? h - 40 : PANEL_H;
            if (wn->x < 0) wn->x = 0;
        }
    }
    return 0;
}

void display_get(struct display_info *d)
{
    d->screen_w = fb.width; d->screen_h = fb.height;
    d->mon_w = fb.mon_w; d->mon_h = fb.mon_h; d->mon_mhz = fb.mon_mhz;
    d->render_w = screen_w; d->render_h = screen_h;
    d->fixed = fb.fixed;
    d->fps = g_fps;
}

int display_set(int w, int h)
{
    if (w <= 0) {                                    /* native */
        w = fb.width; h = fb.height;
    }
    if (w > (int)fb.width || h > (int)fb.height)
        return -1;
    if (w != screen_w || h != screen_h) {
        if (display_apply(w, h) < 0)
            return -1;
        render_wallpaper();
        invalidate(0, 0, w, h);
    }
    g_res_w = w == (int)fb.width && h == (int)fb.height ? 0 : w;
    g_res_h = g_res_w ? h : 0;
    theme_save();
    return 0;
}

void blank_set(int minutes)
{
    g_blank_min = minutes;
    theme_save();
}

/* ---------------- clipboard ---------------- */

static char *clip;
static int clip_len;

/* Kept in a file too, so separate programs (the browser package) share it. */
#define CLIP_FILE "/tmp/clipboard"

void clip_set(const char *text, int len)
{
    char *n = malloc(len + 1);
    if (!n)
        return;
    memcpy(n, text, len);
    n[len] = 0;
    free(clip);
    clip = n;
    clip_len = len;
    int fd = open(CLIP_FILE, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd >= 0) {
        write(fd, text, len);
        close(fd);
    }
}

const char *clip_get(int *len)
{
    struct stat st;
    if (stat(CLIP_FILE, &st) == 0 && st.size > 0 && st.size < 4 * 1024 * 1024) {   /* someone else may have copied */
        char *n = malloc(st.size + 1);
        int fd = open(CLIP_FILE, O_RDONLY);
        int got = fd >= 0 && n ? read(fd, n, st.size) : -1;
        if (fd >= 0) close(fd);
        if (got > 0) {
            n[got] = 0;
            free(clip);
            clip = n;
            clip_len = got;
        } else {
            free(n);
        }
    }
    if (len)
        *len = clip_len;
    return clip_len ? clip : NULL;
}

/* ---------------- screenshots ---------------- */

static u32 crc_tab[256];

static u32 crc32_upd(u32 c, const u8 *p, u32 n)
{
    if (!crc_tab[1])
        for (u32 i = 0; i < 256; i++) {
            u32 v = i;
            for (int k = 0; k < 8; k++)
                v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            crc_tab[i] = v;
        }
    for (u32 i = 0; i < n; i++)
        c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

static void put_be32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* Writes the desktop as a PNG (uncompressed deflate blocks: big but simple and exact). */
static int write_png(const char *path, const u32 *px, int w, int h)
{
    u32 row = 1 + w * 3, raw = row * h;
    u32 nblk = (raw + 65534) / 65535;
    u32 zlen = 2 + raw + nblk * 5 + 4;
    u32 total = 8 + 25 + 12 + zlen + 12;
    u8 *out = malloc(total);
    if (!out)
        return -1;
    static const u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    memcpy(out, sig, 8);
    u8 *p = out + 8;
    put_be32(p, 13); memcpy(p + 4, "IHDR", 4); put_be32(p + 8, w); put_be32(p + 12, h);
    p[16] = 8; p[17] = 2; p[18] = p[19] = p[20] = 0;
    put_be32(p + 21, ~crc32_upd(~0u, p + 4, 17));
    p += 25;
    put_be32(p, zlen); memcpy(p + 4, "IDAT", 4);
    u8 *z = p + 8, *q = z;
    *q++ = 0x78; *q++ = 0x01;
    u32 a = 1, b = 0, left = raw, pos = 0;
    u8 *rowbuf = malloc(row);
    if (!rowbuf) {
        free(out);
        return -1;
    }
    int cur_row = -1;
    while (left) {
        u32 n = left > 65535 ? 65535 : left;
        *q++ = left == n;                       /* BFINAL, stored */
        *q++ = n; *q++ = n >> 8; *q++ = ~n; *q++ = (~n) >> 8;
        for (u32 i = 0; i < n; i++, pos++) {
            int y = pos / row, x = pos % row;
            if (y != cur_row) {
                cur_row = y;
                rowbuf[0] = 0;
                for (int k = 0; k < w; k++) {
                    u32 c = px[y * w + k];
                    rowbuf[1 + k * 3] = c >> 16; rowbuf[2 + k * 3] = c >> 8; rowbuf[3 + k * 3] = c;
                }
            }
            u8 v = rowbuf[x];
            *q++ = v;
            a = (a + v) % 65521;
            b = (b + a) % 65521;
        }
        left -= n;
    }
    free(rowbuf);
    put_be32(q, (b << 16) | a);
    q += 4;
    put_be32(q, ~crc32_upd(~0u, p + 4, 4 + zlen));
    p = q + 4;
    put_be32(p, 0); memcpy(p + 4, "IEND", 4); put_be32(p + 8, ~crc32_upd(~0u, p + 4, 4));
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC), ok = -1;
    if (fd >= 0) {
        ok = write(fd, out, total) == (int)total ? 0 : -1;
        close(fd);
    }
    free(out);
    return ok;
}

static void screenshot(void)
{
    struct diskinfo di;
    diskinfo(&di);
    const char *dir = di.mounted ? "/disk/Pictures" : "/tmp";
    mkdir(dir);
    struct rtc_time t;
    gettime(&t);
    char path[96];
    snprintf(path, sizeof path, "%s/shot-%04d%02d%02d-%02d%02d%02d.png", dir, t.year, t.month, t.day, t.hour, t.min,
             t.sec);                                           /* file names are limited to 27 characters */
    if (write_png(path, back, screen_w, screen_h) == 0) {
        char msg[120];
        snprintf(msg, sizeof msg, "Screenshot saved in %s", dir);
        wm_notify(msg);
    } else {
        wm_notify("Could not save the screenshot");
    }
}

void fps_set(int fps)
{
    g_fps = fps;
    theme_save();
}

/* Milliseconds to wait between animation frames: the cap, or the monitor's refresh rate.  The kernel's clock
 * ticks every 10 ms, so the pace is exact on average but individual frames land on 10 ms boundaries. */
static int frame_wait(void)
{
    int fps = g_fps;
    if (fps <= 0)
        fps = fb.mon_mhz ? (fb.mon_mhz + 500) / 1000 : 60;
    if (fps > 100) fps = 100;
    if (fps < 10) fps = 10;
    static u32 next;                                  /* when the next frame is due, in ms */
    u32 now = uptime() * 10;
    if ((int)(next - now) < -200 || next == 0)
        next = now;
    next += 1000 / fps;
    int wait = (int)(next - now);
    return wait < 1 ? 1 : wait;
}

static const struct app gallery_app = {.draw = gallery_draw};

static int anim_active(void);
static void anim_step(u32 now);

int main(int argc, char **argv)
{
    int e = fbmap(&fb, 0, 0);
    if (e < 0) {
        fprintf(2, "pdesk: cannot start graphics (%s). Run under QEMU with the default VGA.\n",
                strerror(e));
        return 1;
    }
    int in = open("/dev/input", O_RDONLY);
    if (in < 0) {
        fbmap(NULL, 0, 0);
        fprintf(2, "pdesk: cannot open /dev/input\n");
        return 1;
    }
    theme_load();
    {
        struct diskinfo dk;
        diskinfo(&dk);
        if (dk.mounted)
            mkdir("/disk/Pictures");                      /* screenshots land here */
    }
    if (display_apply(g_res_w > 0 && g_res_w <= (int)fb.width && g_res_h > 0 && g_res_h <= (int)fb.height ? g_res_w : fb.width,
                      g_res_w > 0 && g_res_w <= (int)fb.width && g_res_h > 0 && g_res_h <= (int)fb.height ? g_res_h : fb.height) < 0) {
        fbmap(NULL, 0, 0);
        fprintf(2, "pdesk: out of memory\n");
        return 1;
    }
    struct driveinfo di[MAXDRIVES];
    int nd = drives(di, MAXDRIVES);
    for (int i = 0; i < nd; i++)
        if (!di[i].boot && di[i].sectors >= 6144)
            installable = 1;
    struct stat kst;                                     /* no kernel image (UEFI boot): nothing to install from */
    if (installable && (stat("/dev/kernelimg", &kst) < 0 || kst.size == 0))
        installable = 0;
    if (installable)
        dicons[ndicons++] = (struct dicon){"Install Petrichor", NULL, ICON_INSTALL};
    build_menus();
    render_wallpaper();
    sample_tray();
    if (argc > 1 && !strcmp(argv[1], "--icons")) {            /* development aid: show every icon */
        struct win *g = wm_open(&gallery_app, 920, 520, "Icon gallery", NULL);
        if (g) { g->x = 50; g->y = 40; }
    }
    invalidate(0, 0, SCREEN_W, SCREEN_H);
    repaint();

    u32 last_sec = uptime();
    last_input = last_sec;
    char last_clock[40] = "";
    while (running) {
        struct pollfd pf[64];
        struct win *owner[64];
        int n = 0;
        pf[n] = (struct pollfd){in, POLLIN, 0};
        owner[n++] = NULL;
        for (int i = 0; i < nz; i++) {
            struct win *w = zorder[i];
            if (w->app->pollfds) {
                int got = w->app->pollfds(w, &pf[n], 64 - n);
                for (int k = 0; k < got; k++)
                    owner[n++] = w;
            } else if (w->app->pollfd && n < 64 && w->app->pollfd(w, &pf[n]))
                owner[n++] = w;
        }
        int first_host = n;                                /* programs the desktop launched (host.c) */
        n += hosts_pollfds(&pf[n], 64 - n);
        int wait = anim_active() ? frame_wait() : 100;
        for (int i = 0; i < nz; i++) {                     /* apps with timers (web pages running scripts) */
            struct win *w = zorder[i];
            if (w->app->frame) {
                int ms = w->app->frame(w, uptime() * 10);
                if (ms >= 0 && ms < wait)
                    wait = ms < 1 ? 1 : ms;
            }
        }
        int r = poll(pf, n, wait);

        if (!blanked)
            cursor_hide();
        if (r > 0) {
            if (pf[0].revents & POLLIN) {
                struct input_event ev[32];
                int got = read(in, ev, sizeof ev) / (int)sizeof ev[0];
                if (got > 0) {
                    last_input = uptime();
                    if (blanked) {                             /* any input wakes the screen; it isn't passed on */
                        blanked = 0;
                        invalidate(0, 0, SCREEN_W, SCREEN_H);
                        got = 0;
                    }
                }
                for (int i = 0; i < got; i++) {
                    if (ev[i].type == INPUT_MOUSE)
                        handle_mouse(&ev[i]);
                    else if (ev[i].type == INPUT_WHEEL)
                        handle_wheel(ev[i].dy);
                    else if (ev[i].type == INPUT_KEY)
                        handle_key(&ev[i]);
                }
            }
            for (int i = 1; i < first_host; i++)
                if (pf[i].revents && owner[i]->used && owner[i]->app->fdready)
                    owner[i]->app->fdready(owner[i], &pf[i]);
            for (int i = first_host; i < n; i++)
                if (pf[i].revents)
                    hosts_fdready(&pf[i]);
        }

        u32 now = uptime();
        expire_toasts(now);
        anim_step(now);
        if (now - last_sec >= 100) {
            last_sec = now;
            sample_tray();
            for (int i = 0; i < nz; i++)
                if (zorder[i]->app->tick)
                    zorder[i]->app->tick(zorder[i], now);
            int st;
            while (wait2(&st, WNOHANG) > 0)
                ;                                                  /* reap finished children */
        }
        char clk[40];
        clock_text(clk, sizeof clk);
        if (strcmp(clk, last_clock)) {
            strcpy(last_clock, clk);
            invalidate(SCREEN_W - 200, 0, 200, PANEL_H);
        }

        if (!blanked && g_blank_min > 0 && now - last_input > (u32)g_blank_min * 60 * 100) {
            blanked = 1;                                       /* screen off: black, nothing drawn until input */
            u32 *px = (u32 *)(uptr)fb.addr;
            for (u32 y = 0; y < fb.height; y++)
                memset(px + y * (fb.pitch / 4), 0, fb.width * 4);
        }
        if (blanked) {
            dirty = 0;
        } else {
            if (dirty)
                repaint();
            cursor_show();                                     /* always: a repaint may have covered the pointer */
        }
    }

    for (int i = 0; i < MAXWIN; i++)
        if (wins[i].used)
            wm_close(&wins[i]);
    close(in);
    fbmap(NULL, 0, 0);
    if (want_poweroff)
        shutdown(SHUT_POWEROFF);
    if (want_reboot)
        shutdown(SHUT_REBOOT);
    return exit_code;
}
