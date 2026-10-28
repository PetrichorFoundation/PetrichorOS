/* Run Program (Alt+F2): type a command, it opens in a terminal. */
#include "desk.h"

struct run {
    struct win *win;
    char text[128];
    int len;
};

static void run_draw(struct win *w, struct rect c)
{
    struct run *r = w->data;
    gfx_text(c.x + 16, c.y + 14, "Run a program (it opens in a terminal):", C_TEXT);
    ui_panel_inset(c.x + 16, c.y + 38, c.w - 32, 26);
    int vis = (c.w - 52) / 8;
    const char *show = r->len > vis ? r->text + r->len - vis : r->text;
    gfx_text(c.x + 24, c.y + 43, show, C_TEXT);
    gfx_fill(c.x + 24 + gfx_text_width(show), c.y + 42, 2, 18, g_accent);
    gfx_text(c.x + 16, c.y + 76, "Enter to run, Esc to cancel", C_TEXT_DIM);
}

static void run_key(struct win *w, struct input_event *e)
{
    struct run *r = w->data;
    if (e->code == 27) {
        wm_close(w);
        return;
    }
    if (e->code == '\n') {
        if (r->len) {
            r->text[r->len] = 0;
            term_open(r->text);
        }
        wm_close(w);
        return;
    }
    if (e->code == 8 && r->len)
        r->len--;
    else if (e->code >= 32 && e->code < 127 && r->len < (int)sizeof r->text - 1)
        r->text[r->len++] = e->code;
    else
        return;
    wm_invalidate(w);
}

static void run_close(struct win *w)
{
    free(w->data);
}

static const struct app run_app = {.draw = run_draw, .key = run_key, .close = run_close};

void run_open(void)
{
    struct run *r = calloc(1, sizeof *r);
    if (!r)
        return;
    r->win = wm_open(&run_app, 460, 140, "Run Program", r);
    if (!r->win) {
        free(r);
        return;
    }
    r->win->x = (SCREEN_W - 460) / 2;
    r->win->y = 200;
    r->win->minw = r->win->w;
    r->win->minh = r->win->h;
    wm_invalidate(r->win);
}
