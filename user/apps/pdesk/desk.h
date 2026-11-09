/* Petrichor Desktop: shared definitions between the window manager and its apps. */
#pragma once
#include "gfx.h"

/* The desktop's drawing size.  It is the monitor's native mode unless Settings picks a smaller one, in which
 * case the desktop renders at that size and is scaled up to the screen. */
extern int screen_w, screen_h;
#define SCREEN_W screen_w
#define SCREEN_H screen_h
#define MAX_SCREEN_W 2560
#define MAX_SCREEN_H 1600
#define PANEL_H  28
#define TITLE_H  26
#define DOCK_H   58
#define NOFILE_USER 32

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

enum { M_MOVE, M_DOWN, M_UP, M_DRAG, M_DBL, M_RDOWN, M_MDOWN };   /* M_MDOWN: middle button (paste) */

struct win;

struct app {
    void (*draw)(struct win *w, struct rect c);            /* c = client area, already clipped */
    void (*key)(struct win *w, struct input_event *e);
    void (*mouse)(struct win *w, int x, int y, int kind);  /* x,y relative to the client area */
    void (*wheel)(struct win *w, int dir);                 /* dir: +1 scroll down, -1 up */
    void (*tick)(struct win *w, u32 now);                  /* roughly once per second */
    int (*pollfd)(struct win *w, struct pollfd *p);        /* 0 or 1 fd to wait on */
    int (*pollfds)(struct win *w, struct pollfd *p, int max);   /* several fds; returns how many */
    void (*fdready)(struct win *w, struct pollfd *p);
    void (*close)(struct win *w);
    int (*request_close)(struct win *w);                   /* the user clicked X: return 1 to handle it (and close later) */
    int (*frame)(struct win *w, u32 now_ms);               /* every pass of the event loop: ms until it wants the next (-1 none) */
};

struct win {
    int used;
    int x, y, w, h;                    /* outer frame */
    int minimized, maximized;
    int hide;                          /* not drawn: its open/restore animation is playing */
    int sx, sy, sw, sh;                /* geometry to restore after maximize */
    int minw, minh;
    char title[64];
    const struct app *app;
    void *data;
};

/* window manager (main.c) */
struct win *wm_open(const struct app *app, int w, int h, const char *title, void *data);
void wm_close(struct win *w);
void wm_try_close(struct win *w);                   /* like wm_close, but lets the app decline (Windows programs) */
void wm_set_title(struct win *w, const char *title);
void wm_invalidate(struct win *w);                          /* repaint the whole window */
void wm_invalidate_rect(int x, int y, int w, int h);
struct rect wm_client(struct win *w);

void wm_notify(const char *msg);                            /* a toast in the corner */
int wm_installer_available(void);                           /* a second disk we could install to */

/* themes (main.c) */
#define NWALL   7
#define NACCENT 6
extern int g_wall, g_accent_idx;
extern int g_animations;                                    /* window and menu animations on/off */
struct display_info {
    int screen_w, screen_h;      /* what the framebuffer really is */
    int mon_w, mon_h, mon_mhz;   /* the monitor's preferred mode (EDID), 0 if unknown */
    int render_w, render_h;      /* the desktop's drawing size */
    int fixed;                   /* the video mode was set by the bootloader */
    int fps;                     /* frame-rate cap while animating */
};
void display_get(struct display_info *d);
int display_set(int w, int h);                              /* applies and saves; -1 if it doesn't fit */
void fps_set(int fps);                                      /* applies and saves */
void anim_set(int on);                                      /* applies and saves */
const char *theme_wall_name(int i);
u32 theme_wall_color(int i);
u32 theme_accent_color(int i);
void theme_set(int wall, int accent_idx);                   /* applies and saves (and drops a picture wallpaper) */
extern char g_wallpic[128];                                  /* a picture used as the wallpaper ("" = none) */
int wallpaper_set_picture(const char *path);                 /* 0 if the image loaded; applies and saves */

/* clipboard (main.c): shared by every app in the desktop */
void clip_set(const char *text, int len);
const char *clip_get(int *len);                             /* NULL when empty */

/* power (main.c) */
extern int g_blank_min;                                     /* turn the screen off after this many idle minutes; 0 = never */
void blank_set(int minutes);                                /* applies and saves */

/* apps */
void viewer_open(const char *path);                         /* image viewer (PNG, JPEG, GIF) */
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
       ICON_SETTINGS, ICON_INSTALL, ICON_RESTART, ICON_RUN, ICON_PALETTE, ICON_WEB,
       ICON_FILE_TEXT, ICON_FILE_CODE, ICON_FILE_WEB, ICON_FILE_ARCHIVE, ICON_COUNT };
void icon_draw(int kind, int x, int y, int size);

/* small widgets shared by apps */
int ui_button(int x, int y, int w, int h, const char *label, int pressed);
void ui_panel_inset(int x, int y, int w, int h);
