/* Settings: appearance, startup mode, system information, keyboard shortcuts. */
#include "desk.h"

#define SIDE_W 130
#define CONF   "/disk/etc/petrichor.conf"

enum { TAB_APPEARANCE, TAB_STARTUP, TAB_SYSTEM, TAB_SHORTCUTS, NTABS };

static const char *const tab_names[NTABS] = {"Appearance", "Startup", "System", "Shortcuts"};

struct settings {
    struct win *win;
    int tab;
    int autostart_text;           /* current saved choice */
    int have_disk;
};

static void load(struct settings *s)
{
    struct diskinfo di;
    diskinfo(&di);
    s->have_disk = di.mounted;
    char buf[64] = "";
    int fd = open(CONF, O_RDONLY);
    if (fd >= 0) {
        int n = read(fd, buf, sizeof buf - 1);
        buf[n > 0 ? n : 0] = 0;
        close(fd);
    }
    s->autostart_text = strstr(buf, "autostart=text") != NULL;
}

static void save_startup(struct settings *s, int text)
{
    if (!s->have_disk) {
        wm_notify("No disk: the setting can't be saved");
        return;
    }
    mkdir("/disk/etc");
    const char *line = text ? "autostart=text\n" : "autostart=desktop\n";
    int fd = open(CONF, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0 || write(fd, line, strlen(line)) < 0) {
        wm_notify("Could not save the startup setting");
        if (fd >= 0) close(fd);
        return;
    }
    close(fd);
    s->autostart_text = text;
    wm_notify(text ? "Boot will start the text console" : "Boot will start the desktop");
}

static void line(int x, int *y, const char *label, const char *value)
{
    gfx_text(x, *y, label, C_TEXT_DIM);
    gfx_text(x + 130, *y, value, C_TEXT);
    *y += 20;
}

static void radio(int x, int y, int on, const char *text)
{
    gfx_frame(x, y, 14, 14, RGB(0x60, 0x68, 0x70));
    gfx_fill(x + 1, y + 1, 12, 12, C_WHITE);
    if (on)
        gfx_fill(x + 3, y + 3, 8, 8, g_accent);
    gfx_text(x + 24, y - 1, text, C_TEXT);
}

static void st_draw(struct win *w, struct rect c)
{
    struct settings *s = w->data;
    gfx_fill(c.x, c.y, SIDE_W, c.h, RGB(0xdd, 0xe0, 0xe4));
    gfx_vline(c.x + SIDE_W - 1, c.y, c.h, RGB(0xa8, 0xb0, 0xb8));
    for (int i = 0; i < NTABS; i++) {
        int y = c.y + 10 + i * 32;
        if (i == s->tab)
            gfx_fill(c.x + 4, y - 4, SIDE_W - 9, 28, g_accent);
        gfx_text(c.x + 16, y + 2, tab_names[i], i == s->tab ? C_WHITE : C_TEXT);
    }
    int x = c.x + SIDE_W + 20, y = c.y + 18;

    if (s->tab == TAB_APPEARANCE) {
        gfx_text(x, y, "Wallpaper", C_TEXT);
        for (int i = 0; i < NWALL; i++) {
            int sx = x + i * 92, sy = y + 24;
            gfx_vgrad(sx, sy, 84, 54, gfx_mix(theme_wall_color(i), 0x000000, 90), gfx_mix(theme_wall_color(i), C_WHITE, 40));
            gfx_frame(sx, sy, 84, 54, i == g_wall ? g_accent : RGB(0x80, 0x88, 0x90));
            if (i == g_wall) gfx_frame(sx + 1, sy + 1, 82, 52, g_accent);
            gfx_text(sx + 42 - gfx_text_width(theme_wall_name(i)) / 2, sy + 60, theme_wall_name(i), C_TEXT);
        }
        y += 120;
        gfx_text(x, y, "Accent color", C_TEXT);
        for (int i = 0; i < NACCENT; i++) {
            int sx = x + i * 52, sy = y + 24;
            gfx_fill(sx, sy, 40, 40, theme_accent_color(i));
            if (i == g_accent_idx) {
                gfx_frame(sx - 3, sy - 3, 46, 46, C_TEXT);
                gfx_frame(sx - 2, sy - 2, 44, 44, C_TEXT);
            }
        }
        y += 90;
        gfx_text(x, y, "Right-click the desktop and choose Change Wallpaper to cycle.", C_TEXT_DIM);
        radio(x, y + 30, g_animations, "Animate windows, menus and the dock");
    } else if (s->tab == TAB_STARTUP) {
        gfx_text(x, y, "When Petrichor starts", C_TEXT);
        radio(x, y + 34, !s->autostart_text, "Start the graphical desktop");
        radio(x, y + 64, s->autostart_text, "Start the text console");
        gfx_text(x, y + 110, "A choice made in the boot menu overrides this setting.", C_TEXT_DIM);
        gfx_text(x, y + 130, "If the desktop can't start, the text console is used.", C_TEXT_DIM);
        if (!s->have_disk)
            gfx_text(x, y + 170, "No disk is mounted, so this can't be saved.", RGB(0xc0, 0x40, 0x30));
        else
            gfx_text(x, y + 170, "Saved in /disk/etc/petrichor.conf", C_TEXT_DIM);
    } else if (s->tab == TAB_SYSTEM) {
        struct meminfo mi;
        struct netinfo ni;
        struct driveinfo di[MAXDRIVES];
        meminfo(&mi);
        netinfo(&ni);
        int nd = drives(di, MAXDRIVES);
        char t[96];
        u32 up = uptime() / 100;
        line(x, &y, "Petrichor", PETRICHOR_VERSION);
        snprintf(t, sizeof t, "%dx%d", SCREEN_W, SCREEN_H);
        line(x, &y, "Display", t);
        snprintf(t, sizeof t, "%u / %u MiB used", (mi.total_kb - mi.free_kb) / 1024, mi.total_kb / 1024);
        line(x, &y, "Memory", t);
        snprintf(t, sizeof t, "%u:%02u:%02u", up / 3600, up / 60 % 60, up % 60);
        line(x, &y, "Uptime", t);
        y += 8;
        for (int i = 0; i < nd; i++) {
            snprintf(t, sizeof t, "%u MiB %s%s", di[i].sectors / 2048, di[i].model, di[i].boot ? " (boot)" : "");
            line(x, &y, di[i].name, t);
        }
        y += 8;
        if (ni.up) {
            line(x, &y, "IP address", inet_ntoa(ni.ip));
            line(x, &y, "Gateway", inet_ntoa(ni.gw));
            line(x, &y, "DNS", inet_ntoa(ni.dns));
            snprintf(t, sizeof t, "%02x:%02x:%02x:%02x:%02x:%02x", ni.mac[0], ni.mac[1], ni.mac[2], ni.mac[3], ni.mac[4], ni.mac[5]);
            line(x, &y, "MAC", t);
        } else {
            line(x, &y, "Network", "not connected");
        }
    } else {
        static const char *const keys[][2] = {
            {"Alt+Tab", "Switch window"}, {"Alt+F4", "Close window"}, {"Alt+F2", "Run a program"},
            {"Ctrl+Alt+T", "New terminal"}, {"Esc", "Close menus"}, {"Double-click title", "Maximize / restore"},
            {"Drag to screen edge", "Snap / maximize"}, {"Right-click desktop", "Context menu"},
            {"Click the clock", "Calendar"}, {"Ctrl+S (editor)", "Save"}, {"PgUp/PgDn (terminal)", "Scrollback"},
        };
        for (int i = 0; i < 11; i++) {
            gfx_text(x, y, keys[i][0], g_accent);
            gfx_text(x + 190, y, keys[i][1], C_TEXT);
            y += 22;
        }
    }
}

static void st_mouse(struct win *w, int x, int y, int kind)
{
    struct settings *s = w->data;
    if (kind != M_DOWN && kind != M_DBL)
        return;
    if (x < SIDE_W) {
        int t = (y - 6) / 32;
        if (t >= 0 && t < NTABS && s->tab != t) {
            s->tab = t;
            if (t == TAB_STARTUP)
                load(s);
            wm_invalidate(w);
        }
        return;
    }
    int cx = x - SIDE_W - 20, cy = y - 18;
    if (s->tab == TAB_APPEARANCE) {
        if (cy >= 24 && cy < 78) {
            int i = cx / 92;
            if (i >= 0 && i < NWALL && cx % 92 < 84 && i != g_wall) {
                theme_set(i, g_accent_idx);
                wm_invalidate(w);
            }
        } else if (cy >= 144 && cy < 184) {
            int i = cx / 52;
            if (i >= 0 && i < NACCENT && cx % 52 < 40 && i != g_accent_idx) {
                theme_set(g_wall, i);
                wm_invalidate(w);
            }
        } else if (cy >= 236 && cy < 258 && cx >= 0 && cx < 320) {
            anim_set(!g_animations);
            wm_invalidate(w);
        }
    } else if (s->tab == TAB_STARTUP) {
        if (cx >= 0 && cx < 300 && cy >= 32 && cy < 52)
            save_startup(s, 0);
        else if (cx >= 0 && cx < 300 && cy >= 62 && cy < 82)
            save_startup(s, 1);
        wm_invalidate(w);
    }
}

static void st_tick(struct win *w, u32 now)
{
    struct settings *s = w->data;
    if (s->tab == TAB_SYSTEM && !w->minimized)
        wm_invalidate(w);
}

static void st_close(struct win *w)
{
    free(w->data);
}

static const struct app settings_app = {.draw = st_draw, .mouse = st_mouse, .tick = st_tick, .close = st_close};

void settings_open(void)
{
    struct settings *s = calloc(1, sizeof *s);
    if (!s)
        return;
    load(s);
    s->win = wm_open(&settings_app, 640, 400, "Settings", s);
    if (!s->win) {
        free(s);
        return;
    }
    s->win->minw = 560;
    s->win->minh = 360;
}
