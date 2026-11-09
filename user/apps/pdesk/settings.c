/* Settings: appearance, startup mode, system information, keyboard shortcuts. */
#include "desk.h"

#define SIDE_W 130
#define CONF   "/disk/etc/petrichor.conf"

enum { TAB_APPEARANCE, TAB_DISPLAY, TAB_POWER, TAB_STARTUP, TAB_SYSTEM, TAB_SHORTCUTS, NTABS };

static const char *const tab_names[NTABS] = {"Appearance", "Display", "Power", "Startup", "System", "Shortcuts"};

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

/* The resolutions on offer: the screen's own mode first, then the standard ones that fit inside it. */
static int res_list(int out[][2], int max)
{
    static const int std[][2] = {{800, 600}, {1024, 768}, {1280, 720}, {1280, 800}, {1366, 768}, {1440, 900},
                                 {1600, 900}, {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1440}};
    struct display_info d;
    display_get(&d);
    int n = 0;
    out[n][0] = d.screen_w; out[n][1] = d.screen_h; n++;
    for (int i = (int)(sizeof std / sizeof std[0]) - 1; i >= 0 && n < max; i--)
        if (std[i][0] <= d.screen_w && std[i][1] <= d.screen_h && !(std[i][0] == d.screen_w && std[i][1] == d.screen_h)) {
            out[n][0] = std[i][0]; out[n][1] = std[i][1]; n++;
        }
    return n;
}

static const int fps_opts[5] = {0, 30, 60, 75, 100};

static void draw_display(int x, int y)
{
    struct display_info d;
    display_get(&d);
    char t[96];
    snprintf(t, sizeof t, "%dx%d%s", d.screen_w, d.screen_h, d.fixed ? " (set by the bootloader)" : "");
    line(x, &y, "Screen", t);
    if (d.mon_w) {
        if (d.mon_mhz)
            snprintf(t, sizeof t, "%dx%d at %d.%02d Hz", d.mon_w, d.mon_h, d.mon_mhz / 1000, d.mon_mhz % 1000 / 10);
        else
            snprintf(t, sizeof t, "%dx%d", d.mon_w, d.mon_h);
    } else {
        snprintf(t, sizeof t, "not reported");
    }
    line(x, &y, "Monitor prefers", t);
    snprintf(t, sizeof t, "%dx%d%s", d.render_w, d.render_h,
             d.render_w == d.screen_w && d.render_h == d.screen_h ? "" : " (scaled to the screen)");
    line(x, &y, "Desktop size", t);
    y += 10;
    gfx_text(x, y, "Resolution", C_TEXT);
    int list[16][2];
    int n = res_list(list, 12);
    for (int i = 0; i < n; i++) {
        snprintf(t, sizeof t, "%dx%d%s", list[i][0], list[i][1], i == 0 ? "  native" : "");
        radio(x + (i / 6) * 220, y + 26 + (i % 6) * 22, list[i][0] == d.render_w && list[i][1] == d.render_h, t);
    }
    y += 26 + 6 * 22 + 8;
    gfx_text(x, y, "Animation frame rate", C_TEXT);
    for (int i = 0; i < 5; i++) {
        if (!fps_opts[i])
            snprintf(t, sizeof t, "Monitor");
        else
            snprintf(t, sizeof t, "%d fps", fps_opts[i]);
        radio(x + i * 96, y + 26, fps_opts[i] == d.fps, t);
    }
    gfx_text(x, y + 52, "Smaller sizes are drawn smaller and stretched to fit.", C_TEXT_DIM);
    gfx_text(x, y + 68, "The screen mode itself is picked at boot from the", C_TEXT_DIM);
    gfx_text(x, y + 84, "monitor's own report (EDID).", C_TEXT_DIM);
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
            int sx = x + i * 72, sy = y + 24;
            gfx_vgrad(sx, sy, 64, 44, gfx_mix(theme_wall_color(i), 0x000000, 90), gfx_mix(theme_wall_color(i), C_WHITE, 40));
            int sel = i == g_wall && !g_wallpic[0];
            gfx_frame(sx, sy, 64, 44, sel ? g_accent : RGB(0x80, 0x88, 0x90));
            if (sel) gfx_frame(sx + 1, sy + 1, 62, 42, g_accent);
            gfx_text(sx + 32 - gfx_text_width(theme_wall_name(i)) / 2, sy + 50, theme_wall_name(i), C_TEXT);
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
        gfx_text(x, y, g_wallpic[0] ? "Using a picture. Pick a tile to go back, or open another image and press W." : "For a picture: open it in the image viewer and press W.", C_TEXT_DIM);
        radio(x, y + 30, g_animations, "Animate windows, menus and the dock");
    } else if (s->tab == TAB_DISPLAY) {
        draw_display(x, y);
    } else if (s->tab == TAB_POWER) {
        static const int mins[5] = {0, 5, 10, 30, 60};
        gfx_text(x, y, "Turn the screen off when idle for", C_TEXT);
        for (int i = 0; i < 5; i++) {
            char t[24];
            if (mins[i]) snprintf(t, sizeof t, "%d min", mins[i]); else snprintf(t, sizeof t, "Never");
            radio(x + i * 96, y + 26, g_blank_min == mins[i], t);
        }
        gfx_text(x, y + 56, "The screen goes black; any key or mouse movement wakes it.", C_TEXT_DIM);
        gfx_text(x, y + 100, "Shut down and Restart are in the Applications menu.", C_TEXT);
        gfx_text(x, y + 124, "Restart uses the ACPI reset register, then the PCI and", C_TEXT_DIM);
        gfx_text(x, y + 140, "keyboard-controller resets, so it works on modern PCs too.", C_TEXT_DIM);
        gfx_text(x, y + 180, "Not supported yet: battery level, sleep/suspend, brightness", C_TEXT_DIM);
        gfx_text(x, y + 196, "keys. They need an ACPI bytecode (AML) interpreter.", C_TEXT_DIM);
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
        struct display_info di2;
        display_get(&di2);
        snprintf(t, sizeof t, "%dx%d", di2.screen_w, di2.screen_h);
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
            {"Print Screen", "Screenshot to Pictures"}, {"Ctrl+C/X/V, Ctrl+Z/Y", "Copy/cut/paste, undo/redo"},
            {"Ctrl+Shift+C/V (terminal)", "Copy/paste; drag selects"}, {"Middle click", "Paste"},
            {"Ctrl+C/V (Files)", "Copy and paste files"},
        };
        for (int i = 0; i < 16; i++) {
            gfx_text(x, y, keys[i][0], g_accent);
            gfx_text(x + 218, y, keys[i][1], C_TEXT);
            y += 19;
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
        if (cy >= 24 && cy < 68) {
            int i = cx / 72;
            if (i >= 0 && i < NWALL && cx % 72 < 64 && (i != g_wall || g_wallpic[0])) {
                g_wallpic[0] = 0;
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
    } else if (s->tab == TAB_DISPLAY) {
        int list[16][2];
        int n = res_list(list, 12);
        int top = 60 + 10 + 26;                         /* first radio row of the resolution list */
        if (cy >= top && cy < top + 6 * 22 && cx >= 0) {
            int col = cx / 220, row = (cy - top) / 22, i = col * 6 + row;
            if (col < 2 && cx % 220 < 200 && i < n) {
                if (display_set(i == 0 ? 0 : list[i][0], list[i][1]) < 0)
                    wm_notify("Could not switch to that size");
                wm_invalidate(w);
            }
        }
        int ftop = 60 + 10 + 26 + 6 * 22 + 8 + 26;
        if (cy >= ftop && cy < ftop + 20 && cx >= 0 && cx < 5 * 96 && cx % 96 < 90) {
            fps_set(fps_opts[cx / 96]);
            wm_invalidate(w);
        }
    } else if (s->tab == TAB_POWER) {
        static const int mins[5] = {0, 5, 10, 30, 60};
        if (cy >= 26 && cy < 46 && cx >= 0 && cx < 5 * 96 && cx % 96 < 90) {
            blank_set(mins[cx / 96]);
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
    s->win = wm_open(&settings_app, 680, 440, "Settings", s);
    if (!s->win) {
        free(s);
        return;
    }
    s->win->minw = 560;
    s->win->minh = 400;
}
