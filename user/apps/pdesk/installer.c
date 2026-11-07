/* Installer wizard: copies Petrichor to another disk by running `pinstall`. */
#include "desk.h"

enum { PG_WELCOME, PG_DISK, PG_CONFIRM, PG_WORKING, PG_DONE, PG_ERROR };

struct inst {
    struct win *win;
    int page;
    struct driveinfo d[MAXDRIVES];
    int nd;
    int cand[MAXDRIVES], ncand;                 /* indexes into d[] of usable targets */
    int sel;                            /* index into cand[] */
    char note[MAXDRIVES][96];           /* what is already on each candidate disk */
    int confirm;                        /* "I understand" checkbox */
    int rfd, pct;
    char status[96], error[120];
    char linebuf[160];
    int ll;
};

#define BTN_Y(c) ((c).y + (c).h - 44)

/* Describes existing partitions, so nobody erases another operating system by accident. */
static void probe_note(const char *name, char *out, int outsz)
{
    char path[16];
    snprintf(path, sizeof path, "/dev/%s", name);
    out[0] = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    unsigned char b[1024];
    int got = 0, r;
    while (got < (int)sizeof b && (r = read(fd, b + got, sizeof b - got)) > 0)
        got += r;
    close(fd);
    if (got < (int)sizeof b)
        return;
    if (!memcmp(b + 512, "EFI PART", 8)) {
        safestrcpy(out, "Has a GPT partition table: another OS may be installed", outsz);
    } else if (b[510] == 0x55 && b[511] == 0xAA) {
        int parts = 0;
        for (int i = 0; i < 4; i++)
            parts += b[446 + i * 16 + 4] != 0;
        if (parts)
            snprintf(out, outsz, "Has %d partition%s: another OS may be installed", parts, parts == 1 ? "" : "s");
    }
}

static void scan(struct inst *s)
{
    s->nd = drives(s->d, 4);
    s->ncand = 0;
    for (int i = 0; i < s->nd; i++)
        if (!s->d[i].boot && s->d[i].sectors >= 6144)
            s->cand[s->ncand++] = i;
    for (int i = 0; i < s->ncand; i++)
        probe_note(s->d[s->cand[i]].name, s->note[i], sizeof s->note[i]);
    if (s->sel >= s->ncand)
        s->sel = 0;
}

static void begin(struct inst *s)
{
    const struct driveinfo *d = &s->d[s->cand[s->sel]];
    int fds[2];
    if (pipe(fds) < 0)
        return;
    int pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        for (int fd = 3; fd < NOFILE_USER; fd++)
            close(fd);
        char *argv[] = {"pinstall", "--progress", "-y", (char *)d->name, NULL};
        exec("/bin/pinstall", argv);
        exit(127);
    }
    close(fds[1]);
    s->rfd = fds[0];
    s->page = PG_WORKING;
    s->pct = 0;
    s->ll = 0;
    safestrcpy(s->status, "Starting...", sizeof s->status);
}

static void handle_line(struct inst *s, const char *l)
{
    if (!strncmp(l, "PROGRESS ", 9)) {
        s->pct = atoi(l + 9);
        const char *t = strchr(l + 9, ' ');
        if (t)
            safestrcpy(s->status, t + 1, sizeof s->status);
    } else if (!strcmp(l, "DONE")) {
        s->page = PG_DONE;
        wm_notify("Petrichor was installed");
    } else if (!strncmp(l, "ERROR ", 6)) {
        safestrcpy(s->error, l + 6, sizeof s->error);
        s->page = PG_ERROR;
    }
}

static int in_pollfd(struct win *w, struct pollfd *p)
{
    struct inst *s = w->data;
    if (s->page != PG_WORKING)
        return 0;
    *p = (struct pollfd){s->rfd, POLLIN, 0};
    return 1;
}

static void in_fdready(struct win *w, struct pollfd *p)
{
    struct inst *s = w->data;
    char buf[256];
    int n = read(s->rfd, buf, sizeof buf);
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n' || s->ll == (int)sizeof s->linebuf - 1) {
                s->linebuf[s->ll] = 0;
                handle_line(s, s->linebuf);
                s->ll = 0;
            } else if (buf[i] != '\r') {
                s->linebuf[s->ll++] = buf[i];
            }
        }
    } else {
        close(s->rfd);
        if (s->page == PG_WORKING) {
            safestrcpy(s->error, "the installer stopped unexpectedly", sizeof s->error);
            s->page = PG_ERROR;
        }
    }
    wm_invalidate(w);
}

static void title(struct rect c, const char *t)
{
    gfx_vgrad(c.x, c.y, c.w, 56, RGB(0x0c, 0x22, 0x3a), RGB(0x1c, 0x5a, 0x6a));
    gfx_text_scaled(c.x + 20, c.y + 12, t, C_WHITE, 2);
}

static void in_draw(struct win *w, struct rect c)
{
    struct inst *s = w->data;
    int x = c.x + 24, y = c.y + 76, by = BTN_Y(c);
    switch (s->page) {
    case PG_WELCOME:
        title(c, "Install Petrichor");
        gfx_text(x, y, "This copies Petrichor (the bootloader, kernel and system)", C_TEXT); y += 20;
        gfx_text(x, y, "to another disk so that it can start by itself.", C_TEXT); y += 36;
        gfx_text(x, y, "You will choose the disk next. Nothing is changed until", C_TEXT); y += 20;
        gfx_text(x, y, "you confirm.", C_TEXT);
        ui_button(c.x + c.w - 110, by, 90, 28, "Next >", 0);
        break;
    case PG_DISK:
        title(c, "Choose a disk");
        if (!s->ncand) {
            gfx_text(x, y, "No suitable disk was found.", RGB(0xc0, 0x40, 0x30)); y += 24;
            gfx_text(x, y, "Attach a second disk of at least 3 MiB and try again.", C_TEXT);
        } else {
            gfx_text(x, y, "Install Petrichor on:", C_TEXT); y += 28;
            for (int i = 0; i < s->ncand; i++) {
                const struct driveinfo *d = &s->d[s->cand[i]];
                if (i == s->sel)
                    gfx_fill(x - 6, y - 4, c.w - 36, 44, g_accent);
                gfx_frame(x, y + 2, 14, 14, i == s->sel ? C_WHITE : RGB(0x60, 0x68, 0x70));
                if (i == s->sel) gfx_fill(x + 4, y + 6, 6, 6, C_WHITE);
                char t[96];
                static const char *const bus[] = {"IDE", "SATA", "NVMe", "USB", "virtio"};
                snprintf(t, sizeof t, "%s  %s  %u MiB  %s", d->name, bus[d->bus < 3 ? d->bus : 0], d->sectors / 2048, d->model);
                gfx_text(x + 26, y + 2, t, i == s->sel ? C_WHITE : C_TEXT);
                if (s->note[i][0])
                    gfx_text(x + 26, y + 20, s->note[i], i == s->sel ? RGB(0xff, 0xe0, 0xa0) : RGB(0xc0, 0x40, 0x30));
                y += 50;
            }
        }
        ui_button(c.x + 20, by, 90, 28, "< Back", 0);
        ui_button(c.x + c.w - 110, by, 90, 28, "Next >", !s->ncand);
        break;
    case PG_CONFIRM: {
        const struct driveinfo *d = &s->d[s->cand[s->sel]];
        title(c, "Ready to install");
        char t[96];
        snprintf(t, sizeof t, "Everything on %s (%u MiB, %s)", d->name, d->sectors / 2048, d->model);
        gfx_text(x, y, t, RGB(0xc0, 0x40, 0x30)); y += 20;
        gfx_text(x, y, "will be erased.", RGB(0xc0, 0x40, 0x30)); y += 24;
        if (s->note[s->sel][0]) {
            gfx_text(x, y, s->note[s->sel], RGB(0xc0, 0x40, 0x30));
            y += 20;
        }
        y += 8;
        gfx_text(x, y, "The copy is verified when it is finished.", C_TEXT);
        int cy2 = by - 34;
        gfx_frame(x, cy2, 16, 16, RGB(0x50, 0x58, 0x60));
        gfx_fill(x + 1, cy2 + 1, 14, 14, C_WHITE);
        if (s->confirm)
            gfx_fill(x + 3, cy2 + 3, 10, 10, g_accent);
        gfx_text(x + 26, cy2, "I understand that everything on this disk will be erased", C_TEXT);
        ui_button(c.x + 20, by, 90, 28, "< Back", 0);
        ui_button(c.x + c.w - 130, by, 110, 28, "Install", !s->confirm);
        break;
    }
    case PG_WORKING:
        title(c, "Installing...");
        gfx_text(x, y, s->status, C_TEXT); y += 32;
        gfx_fill(x, y, c.w - 48, 22, RGB(0xd0, 0xd4, 0xd8));
        gfx_fill(x, y, (c.w - 48) * s->pct / 100, 22, g_accent);
        gfx_frame(x, y, c.w - 48, 22, RGB(0x70, 0x78, 0x80));
        char t[8];
        snprintf(t, sizeof t, "%d%%", s->pct);
        gfx_text(c.x + c.w / 2 - 12, y + 3, t, s->pct > 48 ? C_WHITE : C_TEXT);
        gfx_text(x, y + 44, "Please don't power off the machine.", C_TEXT_DIM);
        break;
    case PG_DONE:
        title(c, "Installation complete");
        gfx_text(x, y, "Petrichor is installed and was verified.", C_TEXT); y += 28;
        gfx_text(x, y, "To start it: shut down, remove the installer", C_TEXT); y += 20;
        gfx_text(x, y, "disk, and boot from the new disk.", C_TEXT);
        ui_button(c.x + 20, by, 100, 28, "Close", 0);
        ui_button(c.x + c.w - 130, by, 110, 28, "Shut Down", 0);
        break;
    case PG_ERROR:
        title(c, "Installation failed");
        gfx_text(x, y, "Something went wrong:", C_TEXT); y += 28;
        gfx_text(x, y, s->error, RGB(0xc0, 0x40, 0x30));
        ui_button(c.x + c.w - 110, by, 90, 28, "Close", 0);
        break;
    }
}

static void in_mouse(struct win *w, int x, int y, int kind)
{
    struct inst *s = w->data;
    if (kind != M_DOWN && kind != M_DBL)
        return;
    struct rect c = wm_client(w);
    int by = BTN_Y(c) - c.y;
    int next = y >= by && y < by + 28 && x >= c.w - 130 && x < c.w - 10;
    int prev = y >= by && y < by + 28 && x >= 20 && x < 120;
    switch (s->page) {
    case PG_WELCOME:
        if (next) { scan(s); s->page = PG_DISK; }
        break;
    case PG_DISK:
        if (prev) s->page = PG_WELCOME;
        else if (next && s->ncand) { s->page = PG_CONFIRM; s->confirm = 0; }
        else if (s->ncand) {
            int i = (y - 100) / 50;
            if (y >= 100 && i >= 0 && i < s->ncand) {
                s->sel = i;
                s->confirm = 0;
            }
        }
        break;
    case PG_CONFIRM: {
        int box = y >= by - 38 && y < by - 14 && x >= 20 && x < c.w - 20;
        if (box)
            s->confirm = !s->confirm;
        else if (prev)
            s->page = PG_DISK;
        else if (next && s->confirm)
            begin(s);
        break;
    }
    case PG_DONE:
        if (prev) { wm_close(w); return; }
        if (next) shutdown(SHUT_POWEROFF);
        break;
    case PG_ERROR:
        if (next) { wm_close(w); return; }
        break;
    }
    wm_invalidate(w);
}

static void in_close(struct win *w)
{
    struct inst *s = w->data;
    if (s->page == PG_WORKING)
        close(s->rfd);                    /* the copy keeps going; the target is only usable once it finishes */
    free(s);
}

static const struct app inst_app = {.draw = in_draw, .mouse = in_mouse, .pollfd = in_pollfd,
                                    .fdready = in_fdready, .close = in_close};

void installer_open(void)
{
    struct inst *s = calloc(1, sizeof *s);
    if (!s)
        return;
    scan(s);
    s->win = wm_open(&inst_app, 560, 360, "Install Petrichor", s);
    if (!s->win) {
        free(s);
        return;
    }
    s->win->minw = s->win->w;
    s->win->minh = s->win->h;
}
