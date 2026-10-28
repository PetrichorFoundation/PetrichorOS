/* Petrichor Desktop: shared definitions between the window manager and its apps. */
#pragma once
#include "gfx.h"

#define SCREEN_W 1024
#define SCREEN_H 768
#define PANEL_H  28
#define TITLE_H  26
#define DOCK_H   58
#define NOFILE_USER 16

/* Palette (loosely Greybird/XFCE). */
#define C_PANEL      RGB(0x26, 0x2d, 0x35)
#define C_PANEL_HI   RGB(0x3a, 0x45, 0x50)
extern u32 g_accent;                     /* user-selectable (Settings) */
#define C_ACCENT     g_accent
#define C_TITLE_A    RGB(0x56, 0x74, 0x94)
#define C_TITLE_B    RGB(0x3f, 0x5a, 0x76)
#define C_TITLE_IA   RGB(0x9a, 0xa5, 0xb1)
#define C_TITLE_IB   RGB(0x85, 0x91, 0x9e)
#define C_WIN_BG     RGB(0xed, 0xed, 0xee)
#define C_WIN_EDGE   RGB(0x22, 0x2a, 0x33)
#define C_TEXT       RGB(0x2e, 0x34, 0x36)
#define C_TEXT_DIM   RGB(0x6b, 0x74, 0x7c)
#define C_WHITE      RGB(0xff, 0xff, 0xff)
#define C_TERM_BG    RGB(0x1b, 0x1f, 0x24)
#define C_TERM_FG    RGB(0xd3, 0xd7, 0xcf)

static inline int abs_i(int v) { return v < 0 ? -v : v; }

enum { M_MOVE, M_DOWN, M_UP, M_DRAG, M_DBL, M_RDOWN };

struct win;

struct app {
    void (*draw)(struct win *w, struct rect c);            /* c = client area, already clipped */
    void (*key)(struct win *w, struct input_event *e);
    void (*mouse)(struct win *w, int x, int y, int kind);  /* x,y relative to the client area */
    void (*wheel)(struct win *w, int dir);                 /* dir: +1 scroll down, -1 up */
    void (*tick)(struct win *w, u32 now);                  /* roughly once per second */
    int (*pollfd)(struct win *w, struct pollfd *p);        /* 0 or 1 fd to wait on */
    void (*fdready)(struct win *w, struct pollfd *p);
    void (*close)(struct win *w);
};

struct win {
    int used;
    int x, y, w, h;                    /* outer frame */
    int minimized, maximized;
    int sx, sy, sw, sh;                /* geometry to restore after maximize */
    int minw, minh;
    char title[64];
    const struct app *app;
    void *data;
};

/* window manager (main.c) */
struct win *wm_open(const struct app *app, int w, int h, const char *title, void *data);
void wm_close(struct win *w);
void wm_set_title(struct win *w, const char *title);
void wm_invalidate(struct win *w);                          /* repaint the whole window */
void wm_invalidate_rect(int x, int y, int w, int h);
struct rect wm_client(struct win *w);

void wm_notify(const char *msg);                            /* a toast in the corner */
int wm_installer_available(void);                           /* a second disk we could install to */

/* themes (main.c) */
#define NWALL   5
#define NACCENT 6
extern int g_wall, g_accent_idx;
const char *theme_wall_name(int i);
u32 theme_wall_color(int i);
u32 theme_accent_color(int i);
void theme_set(int wall, int accent_idx);                   /* applies and saves */

/* apps */
void term_open(const char *initial_command);                /* NULL = just a shell */
void files_open(const char *path);
void editor_open(const char *path);                         /* NULL = empty buffer */
void sysmon_open(void);
void about_open(void);
void settings_open(void);
void software_open(void);
void run_open(void);
void installer_open(void);
void browser_open(const char *url);                          /* NULL = the home page */

/* icons.c */
enum { ICON_FOLDER, ICON_FILE, ICON_TERM, ICON_EDITOR, ICON_MONITOR, ICON_FILES, ICON_ABOUT,
       ICON_DISK, ICON_EXEC, ICON_APPS, ICON_POWER, ICON_SOFTWARE, ICON_LOGOUT, ICON_HOME,
       ICON_SETTINGS, ICON_INSTALL, ICON_RESTART, ICON_RUN, ICON_PALETTE, ICON_WEB };
void icon_draw(int kind, int x, int y, int size);

/* small widgets shared by apps */
int ui_button(int x, int y, int w, int h, const char *label, int pressed);
void ui_panel_inset(int x, int y, int w, int h);
