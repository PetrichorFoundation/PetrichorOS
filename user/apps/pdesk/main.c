/* pdesk: the Petrichor desktop environment.
 *
 * One process owns the framebuffer and the input devices.  It composes
 * windows (back to front) into an off-screen buffer, copies only the damaged
 * rectangle to the screen, and routes keyboard and mouse events to the focused
 * window's app.  The panel, dock, menu and desktop icons are drawn here. */
#include "desk.h"

#define MAXWIN 12

static struct fbinfo fb;
static u32 *back, *wall;
static struct rect screen = {0, 0, SCREEN_W, SCREEN_H};

static struct win wins[MAXWIN];
static struct win *zorder[MAXWIN];
static int nz;
static struct win *focus;
static int opened;                       /* total windows ever opened: drives cascading */

static int running = 1, want_poweroff;
static int mx = SCREEN_W / 2, my = SCREEN_H / 2, mbtn;

enum { DR_NONE, DR_MOVE, DR_RESIZE, DR_APP };
static int drag_mode;
static struct win *drag_win;
static int drag_ox, drag_oy;
static u32 last_click_tick;
static int last_click_x, last_click_y;

static int menu_open, menu_hover = -1, dock_hover = -1, icon_sel = -1;

static struct rect damage;
static int dirty;

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

static const char *const cursor_art[19] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ", "X    X..X   ",
    "     X..X   ", "      X..X  ", "      X..X  ", "       XX   ",
};

static void cursor_show(void)
{
    u32 *px = (u32 *)fb.addr;
    for (int y = 0; y < 19; y++) {
        for (int x = 0; x < 12; x++) {
            char c = cursor_art[y][x];
            int sx = mx + x, sy = my + y;
            if (c == ' ' || sx >= SCREEN_W || sy >= SCREEN_H)
                continue;
            px[sy * SCREEN_W + sx] = c == 'X' ? 0x000000 : 0xFFFFFF;
        }
    }
}

static void present(struct rect r)
{
    u32 *px = (u32 *)fb.addr;
    for (int y = r.y; y < r.y + r.h; y++)
        memcpy(px + y * SCREEN_W + r.x, back + y * SCREEN_W + r.x, r.w * 4);
}

static void cursor_hide(void)
{
    struct rect r = rect_isect((struct rect){mx, my, 12, 19}, screen);
    if (r.w > 0)
        present(r);
}

/* ---------------- wallpaper ---------------- */

static void render_wallpaper(void)
{
    gfx_set_target(wall, SCREEN_W, SCREEN_H);
    gfx_vgrad(0, 0, SCREEN_W, SCREEN_H, RGB(0x0c, 0x22, 0x3a), RGB(0x1c, 0x5a, 0x6a));
    u32 seed = 0x2545F491;
    for (int i = 0; i < 260; i++) {                      /* rain streaks */
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
    gfx_text_scaled(SCREEN_W - 400, SCREEN_H - 170, "Petrichor", RGB(0x3c, 0x7c, 0x8c), 5);
    gfx_text_scaled(SCREEN_W - 398, SCREEN_H - 84, "the smell of rain on dry earth",
                    RGB(0x3c, 0x7c, 0x8c), 1);
    gfx_set_target(back, SCREEN_W, SCREEN_H);
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
    wm_invalidate(win);
    return win;
}

void wm_set_title(struct win *w, const char *title)
{
    safestrcpy(w->title, title, sizeof w->title);
    wm_invalidate_rect(w->x, w->y, w->w, TITLE_H);
    invalidate(0, 0, SCREEN_W, PANEL_H);
}

void wm_close(struct win *w)
{
    if (!w->used)
        return;
    wm_invalidate(w);
    if (w->app->close)
        w->app->close(w);
    w->used = 0;
    z_remove(w);
    if (drag_win == w)
        drag_mode = DR_NONE;
    if (focus == w) {
        focus = NULL;
        for (int i = nz - 1; i >= 0; i--)
            if (!zorder[i]->minimized) {
                set_focus(zorder[i]);
                break;
            }
    }
    invalidate(0, 0, SCREEN_W, PANEL_H);
}

static void toggle_maximize(struct win *w)
{
    wm_invalidate(w);
    if (w->maximized) {
        w->x = w->sx; w->y = w->sy; w->w = w->sw; w->h = w->sh;
        w->maximized = 0;
    } else {
        w->sx = w->x; w->sy = w->y; w->sw = w->w; w->sh = w->h;
        w->x = 0; w->y = PANEL_H;
        w->w = SCREEN_W; w->h = SCREEN_H - PANEL_H - DOCK_H + 10;
        w->maximized = 1;
    }
    wm_invalidate(w);
}

static void minimize(struct win *w)
{
    w->minimized = 1;
    wm_invalidate(w);
    if (focus == w) {
        focus = NULL;
        for (int i = nz - 1; i >= 0; i--)
            if (!zorder[i]->minimized) {
                set_focus(zorder[i]);
                break;
            }
    }
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
    gfx_vgrad(x, y, 18, 16, active ? RGB(0x7e, 0x9a, 0xb8) : RGB(0xb4, 0xbc, 0xc4),
              active ? RGB(0x5a, 0x76, 0x94) : RGB(0x9a, 0xa4, 0xae));
    gfx_frame(x, y, 18, 16, active ? RGB(0x2c, 0x40, 0x58) : RGB(0x70, 0x7a, 0x84));
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
    gfx_fill(w->x, w->y, w->w, w->h, C_WIN_EDGE);
    gfx_vgrad(w->x + 1, w->y + 1, w->w - 2, TITLE_H - 1, act ? C_TITLE_A : C_TITLE_IA,
              act ? C_TITLE_B : C_TITLE_IB);

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

/* ---------------- drawing: desktop furniture ---------------- */

struct place {
    const char *label;
    const char *path;
    int icon;
};

static const struct place places[] = {
    {"Computer", "/", ICON_DISK},
    {"Home", "/disk", ICON_HOME},
    {"Programs", "/bin", ICON_FOLDER},
};
#define NPLACES 3

static void draw_desktop_icons(void)
{
    for (int i = 0; i < NPLACES; i++) {
        int x = 28, y = PANEL_H + 24 + i * 92;
        if (icon_sel == i)
            gfx_blend(x - 14, y - 6, 84, 84, C_ACCENT, 90);
        icon_draw(places[i].icon, x, y, 48);
        int tw = gfx_text_width(places[i].label);
        gfx_text(x + 25 - tw / 2, y + 56, places[i].label, 0x000000);
        gfx_text(x + 24 - tw / 2, y + 55, places[i].label, C_WHITE);
    }
}

static const char *const month_name[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const day_name[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static void clock_text(char *buf, int n)
{
    struct rtc_time t;
    gettime(&t);
    int y = t.year, m = t.month < 1 || t.month > 12 ? 1 : t.month;
    static const int off[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3)
        y--;
    int dow = (y + y / 4 - y / 100 + y / 400 + off[m - 1] + t.day) % 7;
    snprintf(buf, n, "%s %d %s  %02d:%02d", day_name[dow], t.day, month_name[m - 1], t.hour, t.min);
}

#define APPBTN_W 128

static struct rect taskbar_rect(int idx, int count)
{
    int x0 = APPBTN_W + 8, x1 = SCREEN_W - 200;
    int w = (x1 - x0) / (count ? count : 1);
    if (w > 170)
        w = 170;
    return (struct rect){x0 + idx * (w + 2), 2, w, PANEL_H - 4};
}

static void draw_panel(void)
{
    gfx_vgrad(0, 0, SCREEN_W, PANEL_H, RGB(0x30, 0x39, 0x43), C_PANEL);
    gfx_hline(0, PANEL_H - 1, SCREEN_W, RGB(0x10, 0x14, 0x18));

    gfx_fill(0, 0, APPBTN_W, PANEL_H - 1, menu_open ? C_ACCENT : C_PANEL_HI);
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
        gfx_fill(r.x, r.y, r.w, r.h, act ? RGB(0x56, 0x6c, 0x84) : C_PANEL_HI);
        if (act)
            gfx_fill(r.x, r.y + r.h - 2, r.w, 2, C_ACCENT);
        char t[32];
        int maxc = (r.w - 12) / 8;
        safestrcpy(t, wins[i].title, sizeof t);
        if ((int)strlen(t) > maxc && maxc > 3)
            strcpy(t + maxc - 3, "...");
        gfx_text(r.x + 6, r.y + 4, t, wins[i].minimized ? RGB(0x90, 0x9a, 0xa4) : C_WHITE);
    }

    char clk[40];
    clock_text(clk, sizeof clk);
    gfx_text(SCREEN_W - gfx_text_width(clk) - 10, 6, clk, C_WHITE);
    gfx_text(SCREEN_W - 200 + 8, 6, "root", RGB(0xa8, 0xb4, 0xc0));
}

struct dock_item {
    const char *label;
    int icon;
};

static const struct dock_item dock_items[] = {
    {"Terminal", ICON_TERM},
    {"File Manager", ICON_FILES},
    {"Text Editor", ICON_EDITOR},
    {"System Monitor", ICON_MONITOR},
    {"Software", ICON_SOFTWARE},
};
#define NDOCK 5

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

static void draw_dock(void)
{
    struct rect d = dock_rect();
    gfx_blend(d.x, d.y, d.w, d.h, RGB(0x10, 0x1c, 0x28), 185);
    gfx_frame(d.x, d.y, d.w, d.h, RGB(0x50, 0x62, 0x74));
    for (int i = 0; i < NDOCK; i++) {
        struct rect r = dock_icon_rect(i);
        if (dock_hover == i)
            gfx_blend(r.x - 2, r.y - 2, r.w + 4, r.h + 4, C_WHITE, 55);
        icon_draw(dock_items[i].icon, r.x + 6, r.y + 4, 32);
    }
    if (dock_hover >= 0) {
        struct rect r = dock_icon_rect(dock_hover);
        int tw = gfx_text_width(dock_items[dock_hover].label) + 16;
        int tx = r.x + r.w / 2 - tw / 2;
        gfx_fill(tx, d.y - 26, tw, 22, RGB(0x14, 0x1a, 0x20));
        gfx_frame(tx, d.y - 26, tw, 22, RGB(0x50, 0x62, 0x74));
        gfx_text(tx + 8, d.y - 23, dock_items[dock_hover].label, C_WHITE);
    }
}

enum { A_TERM, A_FILES, A_EDIT, A_MON, A_SOFT, A_ABOUT, A_LOGOUT, A_POWER, A_SEP };

struct menu_item {
    const char *label;
    int icon, action;
};

static const struct menu_item menu_items[] = {
    {"Terminal", ICON_TERM, A_TERM},
    {"File Manager", ICON_FILES, A_FILES},
    {"Text Editor", ICON_EDITOR, A_EDIT},
    {"System Monitor", ICON_MONITOR, A_MON},
    {"Software (ppm)", ICON_SOFTWARE, A_SOFT},
    {"", 0, A_SEP},
    {"About Petrichor", ICON_ABOUT, A_ABOUT},
    {"", 0, A_SEP},
    {"Log Out", ICON_LOGOUT, A_LOGOUT},
    {"Shut Down", ICON_POWER, A_POWER},
};
#define NMENU 10

static int menu_item_h(int i) { return menu_items[i].action == A_SEP ? 9 : 30; }

static struct rect menu_rect(void)
{
    int h = 8;
    for (int i = 0; i < NMENU; i++)
        h += menu_item_h(i);
    return (struct rect){2, PANEL_H, 230, h};
}

static int menu_index_at(int x, int y)
{
    struct rect m = menu_rect();
    if (!rect_hit(m, x, y))
        return -1;
    int top = m.y + 4;
    for (int i = 0; i < NMENU; i++) {
        int h = menu_item_h(i);
        if (y >= top && y < top + h)
            return menu_items[i].action == A_SEP ? -1 : i;
        top += h;
    }
    return -1;
}

static void draw_menu(void)
{
    struct rect m = menu_rect();
    gfx_blend(m.x + 3, m.y + 3, m.w, m.h, 0, 60);
    gfx_fill(m.x, m.y, m.w, m.h, RGB(0xf4, 0xf5, 0xf6));
    gfx_frame(m.x, m.y, m.w, m.h, RGB(0x50, 0x5a, 0x64));
    int y = m.y + 4;
    for (int i = 0; i < NMENU; i++) {
        int h = menu_item_h(i);
        if (menu_items[i].action == A_SEP) {
            gfx_hline(m.x + 8, y + 4, m.w - 16, RGB(0xc0, 0xc6, 0xcc));
        } else {
            if (menu_hover == i)
                gfx_fill(m.x + 2, y, m.w - 4, h, C_ACCENT);
            icon_draw(menu_items[i].icon, m.x + 10, y + 5, 20);
            gfx_text(m.x + 40, y + 7, menu_items[i].label, menu_hover == i ? C_WHITE : C_TEXT);
        }
        y += h;
    }
}

/* ---------------- repaint ---------------- */

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
        if (!zorder[i]->minimized)
            draw_window(zorder[i], d);
    draw_panel();
    draw_dock();
    if (menu_open)
        draw_menu();
    present(d);
    cursor_show();
}

/* ---------------- launching ---------------- */

static void launch(int action)
{
    switch (action) {
    case A_TERM:   term_open(NULL); break;
    case A_FILES:  files_open("/"); break;
    case A_EDIT:   editor_open(NULL); break;
    case A_MON:    sysmon_open(); break;
    case A_SOFT:   term_open("ppm list"); break;
    case A_ABOUT:  about_open(); break;
    case A_LOGOUT: running = 0; break;
    case A_POWER:  running = 0; want_poweroff = 1; break;
    }
}

static void close_menu(void)
{
    if (!menu_open)
        return;
    menu_open = 0;
    menu_hover = -1;
    struct rect m = menu_rect();
    invalidate(m.x, m.y, m.w + 6, m.h + 6);
    invalidate(0, 0, APPBTN_W, PANEL_H);
}

/* ---------------- mouse ---------------- */

static struct win *window_at(int x, int y)
{
    for (int i = nz - 1; i >= 0; i--) {
        struct win *w = zorder[i];
        if (!w->minimized && rect_hit((struct rect){w->x, w->y, w->w, w->h}, x, y))
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
    if (rect_hit(d, mx, my) && !window_at(mx, my))
        for (int i = 0; i < NDOCK; i++)
            if (rect_hit(dock_icon_rect(i), mx, my))
                dh = i;
    if (dh != dock_hover) {
        dock_hover = dh;
        invalidate(d.x, d.y - 30, d.w, d.h + 30);
    }
    if (menu_open) {
        int mh = menu_index_at(mx, my);
        if (mh != menu_hover) {
            menu_hover = mh;
            struct rect m = menu_rect();
            invalidate(m.x, m.y, m.w, m.h);
        }
    }
}

static void on_down(int dbl)
{
    if (menu_open) {
        int i = menu_index_at(mx, my);
        int was_button = my < PANEL_H && mx < APPBTN_W;
        close_menu();
        if (i >= 0)
            launch(menu_items[i].action);
        if (i >= 0 || was_button)
            return;
    }

    if (my < PANEL_H) {                                           /* panel */
        if (mx < APPBTN_W) {
            menu_open = 1;
            struct rect m = menu_rect();
            invalidate(m.x, m.y, m.w + 6, m.h + 6);
            invalidate(0, 0, APPBTN_W, PANEL_H);
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
                launch(i);                                         /* dock order matches A_TERM.. A_SOFT */
        return;
    }
    struct win *w = window_at(mx, my);
    if (w) {
        if (w != focus || w != zorder[nz - 1])
            activate(w);
        if (my < w->y + TITLE_H) {
            int rx = w->x + w->w;
            if (mx >= rx - 24 && mx < rx - 6) { wm_close(w); return; }
            if (mx >= rx - 46 && mx < rx - 28) { toggle_maximize(w); return; }
            if (mx >= rx - 68 && mx < rx - 50) { minimize(w); return; }
            if (dbl) { toggle_maximize(w); return; }
            if (!w->maximized) {
                drag_mode = DR_MOVE;
                drag_win = w;
                drag_ox = mx - w->x;
                drag_oy = my - w->y;
            }
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

    for (int i = 0; i < NPLACES; i++) {                            /* desktop icons */
        struct rect r = {14, PANEL_H + 18 + i * 92, 84, 84};
        if (rect_hit(r, mx, my)) {
            if (dbl)
                files_open(places[i].path);
            if (icon_sel != i) {
                icon_sel = i;
                invalidate(0, PANEL_H, 110, 300);
            }
            return;
        }
    }
    if (icon_sel >= 0) {
        icon_sel = -1;
        invalidate(0, PANEL_H, 110, 300);
    }
    set_focus(NULL);
}

static void on_up(void)
{
    if (drag_mode == DR_APP && drag_win && drag_win->used) {
        struct win *w = drag_win;
        struct rect c = wm_client(w);
        if (w->app->mouse)
            w->app->mouse(w, mx - c.x, my - c.y, M_UP);
    }
    drag_mode = DR_NONE;
}

static void handle_mouse(struct input_event *e)
{
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
    if (e->code == 27 && menu_open) {
        close_menu();
        return;
    }
    if ((e->flags & IEF_ALT) && e->code == '\t') { cycle_focus(); return; }
    if ((e->flags & IEF_ALT) && e->code == KEY_F1 + 3) {           /* Alt+F4 */
        if (focus)
            wm_close(focus);
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
    gfx_vgrad(c.x, c.y, c.w, 92, RGB(0x0c, 0x22, 0x3a), RGB(0x1c, 0x5a, 0x6a));
    gfx_text_scaled(c.x + 20, c.y + 18, "Petrichor", C_WHITE, 3);
    gfx_text(c.x + 22, c.y + 66, "a from-scratch Unix for i386", RGB(0xa8, 0xd8, 0xe8));

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

int main(void)
{
    int e = fbmap(&fb, SCREEN_W, SCREEN_H);
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
    back = malloc(SCREEN_W * SCREEN_H * 4);
    wall = malloc(SCREEN_W * SCREEN_H * 4);
    if (!back || !wall) {
        fbmap(NULL, 0, 0);
        fprintf(2, "pdesk: out of memory\n");
        return 1;
    }
    gfx_set_target(back, SCREEN_W, SCREEN_H);
    render_wallpaper();
    invalidate(0, 0, SCREEN_W, SCREEN_H);
    repaint();

    u32 last_sec = uptime();
    char last_clock[40] = "";
    while (running) {
        struct pollfd pf[MAXWIN + 1];
        struct win *owner[MAXWIN + 1];
        int n = 0;
        pf[n] = (struct pollfd){in, POLLIN, 0};
        owner[n++] = NULL;
        for (int i = 0; i < nz; i++) {
            struct win *w = zorder[i];
            if (w->app->pollfd && n < MAXWIN + 1 && w->app->pollfd(w, &pf[n]))
                owner[n++] = w;
        }
        int r = poll(pf, n, 100);

        cursor_hide();
        if (r > 0) {
            if (pf[0].revents & POLLIN) {
                struct input_event ev[32];
                int got = read(in, ev, sizeof ev) / (int)sizeof ev[0];
                for (int i = 0; i < got; i++) {
                    if (ev[i].type == INPUT_MOUSE)
                        handle_mouse(&ev[i]);
                    else if (ev[i].type == INPUT_KEY)
                        handle_key(&ev[i]);
                }
            }
            for (int i = 1; i < n; i++)
                if (pf[i].revents && owner[i]->used && owner[i]->app->fdready)
                    owner[i]->app->fdready(owner[i], &pf[i]);
        }

        u32 now = uptime();
        if (now - last_sec >= 100) {
            last_sec = now;
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

        if (dirty)
            repaint();
        else
            cursor_show();
    }

    for (int i = 0; i < MAXWIN; i++)
        if (wins[i].used)
            wm_close(&wins[i]);
    close(in);
    fbmap(NULL, 0, 0);
    if (want_poweroff)
        shutdown(SHUT_POWEROFF);
    return 0;
}
