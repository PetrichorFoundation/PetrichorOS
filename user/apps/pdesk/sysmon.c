/* System monitor: CPU graph, memory/disk/network summary, process table. */
#include "desk.h"

#define NHIST 90
#define MAXP  64

struct sysmon {
    struct win *win;
    struct procinfo pi[MAXP], prev[MAXP];
    int np, nprev;
    int cpu_pct[MAXP];                  /* per process, parallel to pi[] */
    u8 hist[NHIST];
    int cpu_total;
    u32 last_ticks;
    int sel;                            /* pid, or -1 */
    struct meminfo mem;
    struct diskinfo disk;
    struct netinfo net;
};

static const char *const state_name[] = {"unused", "embryo", "sleeping", "ready", "running", "zombie"};

static void sample(struct sysmon *s, u32 now)
{
    memcpy(s->prev, s->pi, sizeof s->pi);
    s->nprev = s->np;
    s->np = ps(s->pi, MAXP);
    if (s->np < 0)
        s->np = 0;
    meminfo(&s->mem);
    diskinfo(&s->disk);
    netinfo(&s->net);

    u32 dt = s->last_ticks ? now - s->last_ticks : 100;
    if (dt == 0)
        dt = 1;
    int total = 0;
    for (int i = 0; i < s->np; i++) {
        u32 before = 0;
        for (int j = 0; j < s->nprev; j++)
            if (s->prev[j].pid == s->pi[i].pid)
                before = s->prev[j].ticks;
        int d = s->pi[i].ticks - before;
        s->cpu_pct[i] = d < 0 ? 0 : d * 100 / dt;
        if (s->pi[i].pid != 0)
            total += s->cpu_pct[i];
    }
    s->cpu_total = total > 100 ? 100 : total;
    memmove(s->hist, s->hist + 1, NHIST - 1);
    s->hist[NHIST - 1] = s->cpu_total;
    s->last_ticks = now;
}

static void bar(int x, int y, int w, int pct, u32 color)
{
    gfx_fill(x, y, w, 12, RGB(0xd0, 0xd4, 0xd8));
    gfx_fill(x, y, w * pct / 100, 12, color);
    gfx_frame(x, y, w, 12, RGB(0x8a, 0x92, 0x9a));
}

static void mon_draw(struct win *w, struct rect c)
{
    struct sysmon *s = w->data;
    int x = c.x + 12, y = c.y + 10;

    gfx_text(x, y, "CPU history", C_TEXT);
    char t[48];
    snprintf(t, sizeof t, "%d%%", s->cpu_total);
    gfx_text(x + 130, y, t, C_ACCENT);
    int gw = NHIST * 3, gh = 70;
    gfx_fill(x, y + 20, gw, gh, RGB(0x1b, 0x1f, 0x24));
    for (int g = 1; g < 4; g++)
        gfx_hline(x, y + 20 + g * gh / 4, gw, RGB(0x2e, 0x36, 0x3e));
    for (int i = 0; i < NHIST; i++) {
        int h = s->hist[i] * gh / 100;
        gfx_fill(x + i * 3, y + 20 + gh - h, 3, h, RGB(0x4e, 0xc9, 0x4e));
    }
    gfx_frame(x, y + 20, gw, gh, RGB(0x50, 0x58, 0x60));

    int bx = x + gw + 28, bw = c.w - gw - 70;
    if (bw < 80) bw = 80;
    u32 used = s->mem.total_kb - s->mem.free_kb;
    snprintf(t, sizeof t, "Memory  %u / %u MiB", used / 1024, s->mem.total_kb / 1024);
    gfx_text(bx, y, t, C_TEXT);
    bar(bx, y + 20, bw, s->mem.total_kb ? used * 100 / s->mem.total_kb : 0, C_ACCENT);

    u32 hu = s->mem.heap_total_kb - s->mem.heap_free_kb;
    snprintf(t, sizeof t, "Kernel heap  %u / %u KiB", hu, s->mem.heap_total_kb);
    gfx_text(bx, y + 38, t, C_TEXT);
    bar(bx, y + 58, bw, s->mem.heap_total_kb ? hu * 100 / s->mem.heap_total_kb : 0, RGB(0xd8, 0xb0, 0x30));

    if (s->disk.mounted) {
        u32 du = s->disk.total_kb - s->disk.free_kb;
        snprintf(t, sizeof t, "Disk /disk  %u / %u KiB", du, s->disk.total_kb);
        gfx_text(bx, y + 76, t, C_TEXT);
        bar(bx, y + 96, bw, s->disk.total_kb ? du * 100 / s->disk.total_kb : 0, RGB(0xb0, 0x60, 0xc8));
    }
    snprintf(t, sizeof t, "Net  rx %u  tx %u packets", s->net.rx_packets, s->net.tx_packets);
    gfx_text(x, y + 20 + gh + 10, s->net.up ? t : "Net  not connected", C_TEXT_DIM);

    int ty = y + 20 + gh + 34;
    gfx_fill(c.x, ty, c.w, 20, RGB(0xd0, 0xd4, 0xd8));
    gfx_text(x, ty + 2, "PID", C_TEXT);
    gfx_text(x + 56, ty + 2, "Name", C_TEXT);
    gfx_text(x + 190, ty + 2, "State", C_TEXT);
    gfx_text(x + 290, ty + 2, "CPU", C_TEXT);
    gfx_text(x + 350, ty + 2, "Memory", C_TEXT);
    int rows = (c.y + c.h - ty - 20 - 34) / 20;
    for (int i = 0, r = 0; i < s->np && r < rows; i++, r++) {
        int ry = ty + 20 + r * 20;
        int sel = s->pi[i].pid == s->sel;
        if (sel)
            gfx_fill(c.x, ry, c.w, 20, C_ACCENT);
        else if (r & 1)
            gfx_fill(c.x, ry, c.w, 20, RGB(0xf5, 0xf7, 0xf9));
        else
            gfx_fill(c.x, ry, c.w, 20, C_WHITE);
        u32 col = sel ? C_WHITE : C_TEXT;
        snprintf(t, sizeof t, "%d", s->pi[i].pid);
        gfx_text(x, ry + 2, t, col);
        gfx_text(x + 56, ry + 2, s->pi[i].name, col);
        gfx_text(x + 190, ry + 2, state_name[s->pi[i].state <= PS_ZOMBIE ? s->pi[i].state : 0], col);
        snprintf(t, sizeof t, "%d%%", s->cpu_pct[i]);
        gfx_text(x + 290, ry + 2, t, col);
        snprintf(t, sizeof t, "%u KiB", s->pi[i].mem_kb);
        gfx_text(x + 350, ry + 2, t, col);
    }
    int by = c.y + c.h - 30;
    gfx_fill(c.x, by, c.w, 30, RGB(0xe0, 0xe3, 0xe6));
    ui_button(c.x + 10, by + 4, 110, 22, "End Process", 0);
    gfx_text(c.x + 134, by + 7, "Select a process, then End Process or Delete", C_TEXT_DIM);
}

static void mon_mouse(struct win *w, int x, int y, int kind)
{
    struct sysmon *s = w->data;
    if (kind != M_DOWN && kind != M_DBL)
        return;
    struct rect c = wm_client(w);
    int ty = 10 + 20 + 70 + 34;
    if (y >= c.h - 30 && x >= 10 && x < 120 && s->sel > 1) {
        kill(s->sel, 9);
        s->sel = -1;
        sample(s, uptime());
    } else if (y >= ty + 20 && y < c.h - 34) {
        int r = (y - ty - 20) / 20;
        if (r >= 0 && r < s->np)
            s->sel = s->pi[r].pid;
    }
    wm_invalidate(w);
}

static void mon_key(struct win *w, struct input_event *e)
{
    struct sysmon *s = w->data;
    if (e->code == KEY_DELETE && s->sel > 1) {
        kill(s->sel, 9);
        s->sel = -1;
        sample(s, uptime());
        wm_invalidate(w);
    }
}

static void mon_tick(struct win *w, u32 now)
{
    sample(w->data, now);
    if (!w->minimized)
        wm_invalidate(w);
}

static void mon_close(struct win *w)
{
    free(w->data);
}

static const struct app mon_app = {.draw = mon_draw, .key = mon_key, .mouse = mon_mouse,
                                   .tick = mon_tick, .close = mon_close};

void sysmon_open(void)
{
    struct sysmon *s = calloc(1, sizeof *s);
    if (!s)
        return;
    s->sel = -1;
    s->win = wm_open(&mon_app, 620, 480, "System Monitor", s);
    if (!s->win) {
        free(s);
        return;
    }
    s->win->minw = 560;
    s->win->minh = 340;
    sample(s, uptime());
    sample(s, uptime());
}
