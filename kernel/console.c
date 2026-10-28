/* Text console: VGA 80x25 with a small ANSI parser, mirrored to COM1. */
#include "kernel.h"

#define COM1  0x3F8

#include "font8x16.h"

static volatile u16 *const vga = (volatile u16 *)0xB8000;
static int con_w = 80, con_h = 25;               /* the text grid: 80x25 VGA, or fbw/8 x fbh/16 */
static int cx, cy;
static int fg = 7, bg = 0, bold;
static int esc_state;              /* 0 = text, 1 = saw ESC, 2 = inside CSI */
static int params[4], np;
static int serial_ok;

static const u8 ansi2vga[8] = {0, 4, 2, 6, 1, 5, 3, 7};

/* Framebuffer text console (used when the bootloader set up a VESA mode). */
static int fb_mode, fb_suspended;
static u8 *lfb;                    /* the screen */
static u32 *shadow;                /* RAM copy of the screen, always up to date */
static u16 *model;                 /* the text cells: char | attr << 8 */
static u32 fbw, fbh, fbpitch;
static int cursor_idx = -1;
static char logbuf[8192];          /* recent output, replayed when we switch to the framebuffer */
static u32 loglen;

static const u32 vga_rgb[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

/* ---- serial ---- */
static void serial_init(void)
{
    outb(COM1 + 1, 0x00);          /* no interrupts yet */
    outb(COM1 + 3, 0x80);          /* DLAB */
    outb(COM1 + 0, 0x01);          /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);          /* 8N1 */
    outb(COM1 + 2, 0xC7);          /* FIFO */
    outb(COM1 + 4, 0x0B);          /* DTR, RTS, OUT2 */
    serial_ok = inb(COM1 + 5) != 0xFF;
}

static void serial_putc(char c)
{
    if (!serial_ok)
        return;
    if (c == '\n')
        serial_putc('\r');
    for (int spin = 0; spin < 100000 && !(inb(COM1 + 5) & 0x20); spin++)
        ;
    outb(COM1, (u8)c);
}

void serial_irq_enable(void)
{
    if (!serial_ok)
        return;
    outb(COM1 + 1, 0x01);          /* interrupt on received data */
    irq_unmask(4);
}

void serial_irq(void)
{
    while (inb(COM1 + 5) & 1) {
        char c = (char)inb(COM1);
        if (c == '\r')
            c = '\n';
        tty_input(c);
    }
}

/* ---- screen cells ---- */
static u16 cell(char c)
{
    int f = fg | (bold ? 8 : 0);
    return (u16)(((bg << 4) | f) << 8) | (u8)c;
}

static void fb_draw_cell(int x, int y, u16 v, int under)
{
    u32 ch = v & 0xFF, attr = v >> 8;
    u32 fgc = vga_rgb[attr & 15], bgc = vga_rgb[(attr >> 4) & 7];
    for (int row = 0; row < 16; row++) {
        u32 bits = (ch >= 32 && ch <= 126) ? font8x16[ch - 32][row] : 0;
        if (under && row >= 14)
            bits = 0xFF;
        u32 *srow = shadow + (y * 16 + row) * fbw + x * 8;
        for (int col = 0; col < 8; col++)
            srow[col] = (bits & (0x80 >> col)) ? fgc : bgc;
        if (!fb_suspended)
            memcpy(lfb + (y * 16 + row) * fbpitch + x * 32, srow, 32);
    }
}

static void cell_set(int idx, u16 v)
{
    if (fb_mode) {
        model[idx] = v;
        fb_draw_cell(idx % con_w, idx / con_w, v, 0);
    } else {
        vga[idx] = v;
    }
}

static void fb_blit(void)
{
    for (u32 y = 0; y < fbh; y++)
        memcpy(lfb + y * fbpitch, shadow + y * fbw, fbw * 4);
}

static void vga_cursor(void)
{
    if (fb_mode) {
        int idx = cy * con_w + cx;
        if (cursor_idx >= 0 && cursor_idx != idx && cursor_idx < con_w * con_h)
            fb_draw_cell(cursor_idx % con_w, cursor_idx / con_w, model[cursor_idx], 0);
        cursor_idx = idx;
        if (idx < con_w * con_h)
            fb_draw_cell(cx, cy, model[idx], 1);
        return;
    }
    u16 pos = cy * con_w + cx;
    outb(0x3D4, 14);
    outb(0x3D5, pos >> 8);
    outb(0x3D4, 15);
    outb(0x3D5, pos & 0xFF);
}

static void vga_clear_range(int from, int to)
{
    for (int i = from; i < to; i++)
        cell_set(i, cell(' '));
}

static void vga_scroll(void)
{
    if (fb_mode) {
        memmove(model, model + con_w, (con_w * (con_h - 1)) * sizeof *model);
        memmove(shadow, shadow + fbw * 16, fbw * 16 * (con_h - 1) * 4);
        u32 blank = vga_rgb[bg & 7];
        for (u32 i = fbw * 16 * (con_h - 1); i < fbw * fbh; i++)
            shadow[i] = blank;
        for (int i = con_w * (con_h - 1); i < con_w * con_h; i++)
            model[i] = cell(' ');
        if (!fb_suspended)
            fb_blit();
        return;
    }
    for (int i = 0; i < con_w * (con_h - 1); i++)
        vga[i] = vga[i + con_w];
    vga_clear_range(con_w * (con_h - 1), con_w * con_h);
}

static void vga_put(char c)
{
    switch (c) {
    case '\n': cx = 0; cy++; break;
    case '\r': cx = 0; break;
    case '\b': if (cx > 0) cx--; break;
    case '\t': cx = (cx + 8) & ~7; break;
    default:
        if ((u8)c >= ' ')
            cell_set(cy * con_w + cx++, cell(c));
        break;
    }
    if (cx >= con_w) {
        cx = 0;
        cy++;
    }
    while (cy >= con_h) {
        vga_scroll();
        cy--;
    }
}

static void sgr(int code)
{
    if (code == 0) { fg = 7; bg = 0; bold = 0; }
    else if (code == 1) bold = 1;
    else if (code == 22) bold = 0;
    else if (code >= 30 && code <= 37) fg = ansi2vga[code - 30];
    else if (code == 39) fg = 7;
    else if (code >= 40 && code <= 47) bg = ansi2vga[code - 40];
    else if (code == 49) bg = 0;
    else if (code >= 90 && code <= 97) { fg = ansi2vga[code - 90]; bold = 1; }
}

static void csi(char final)
{
    int n = np + 1;
    int p0 = params[0] ? params[0] : 1;
    switch (final) {
    case 'm':
        for (int i = 0; i < n; i++)
            sgr(params[i]);
        break;
    case 'H':
    case 'f':
        cy = (params[0] ? params[0] : 1) - 1;
        cx = (params[1] ? params[1] : 1) - 1;
        if (cy >= con_h) cy = con_h - 1;
        if (cx >= con_w) cx = con_w - 1;
        break;
    case 'J':
        if (params[0] == 2 || params[0] == 3) {
            vga_clear_range(0, con_w * con_h);
            cx = cy = 0;
        } else if (params[0] == 0) {
            vga_clear_range(cy * con_w + cx, con_w * con_h);
        }
        break;
    case 'K':
        if (params[0] == 2)
            vga_clear_range(cy * con_w, (cy + 1) * con_w);
        else
            vga_clear_range(cy * con_w + cx, (cy + 1) * con_w);
        break;
    case 'A': cy -= p0; if (cy < 0) cy = 0; break;
    case 'B': cy += p0; if (cy >= con_h) cy = con_h - 1; break;
    case 'C': cx += p0; if (cx >= con_w) cx = con_w - 1; break;
    case 'D': cx -= p0; if (cx < 0) cx = 0; break;
    }
}

static void screen_char(char c)
{
    if (esc_state == 0) {
        if (c == 033)
            esc_state = 1;
        else
            vga_put(c);
    } else if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            np = 0;
            memset(params, 0, sizeof params);
        } else {
            esc_state = 0;
        }
    } else {
        if (c >= '0' && c <= '9') {
            params[np] = params[np] * 10 + (c - '0');
        } else if (c == ';') {
            if (np < 3)
                np++;
        } else {
            csi(c);
            esc_state = 0;
        }
    }
}

static void put_nocursor(char c)
{
    serial_putc(c);
    if (loglen < sizeof logbuf)
        logbuf[loglen++] = c;
    else {                                          /* keep the most recent half */
        memmove(logbuf, logbuf + sizeof logbuf / 2, sizeof logbuf / 2);
        loglen = sizeof logbuf / 2;
        logbuf[loglen++] = c;
    }
    screen_char(c);
}

void console_putc(char c)
{
    put_nocursor(c);
    vga_cursor();
}

void console_write(const char *s, u32 n)
{
    for (u32 i = 0; i < n; i++)
        put_nocursor(s[i]);
    vga_cursor();
}

void console_init(void)
{
    serial_init();
    vga_clear_range(0, con_w * con_h);
    cx = cy = 0;
    vga_cursor();
}

/* The bootloader put the display in a VESA graphics mode, so VGA text is gone: render the
 * text grid on the framebuffer instead and replay what has been printed so far. */
void console_use_fb(void)
{
    fbw = bootinfo.fb_w;
    fbh = bootinfo.fb_h;
    fbpitch = bootinfo.fb_pitch;
    lfb = mmio_map(bootinfo.fb_addr, fbpitch * fbh);
    shadow = kmalloc(fbw * fbh * 4);
    model = kmalloc((fbw / 8) * (fbh / 16) * sizeof *model);
    if (!lfb || !shadow || !model)
        return;
    fb_mode = 1;
    con_w = fbw / 8;
    con_h = fbh / 16;
    for (u32 i = 0; i < fbw * fbh; i++)
        shadow[i] = 0;
    for (int i = 0; i < con_w * con_h; i++)
        model[i] = cell(' ');
    fb_blit();
    cx = cy = 0;
    cursor_idx = -1;
    esc_state = 0;
    fg = 7; bg = 0; bold = 0;
    for (u32 i = 0; i < loglen; i++)
        screen_char(logbuf[i]);
    vga_cursor();
}

void console_suspend(void)
{
    fb_suspended = 1;
}

void console_resume(void)
{
    if (!fb_mode || !fb_suspended)
        return;
    fb_suspended = 0;
    fb_blit();
    cursor_idx = -1;
    vga_cursor();
}

static void kputc(char c, void *ctx) { (void)ctx; put_nocursor(c); }

int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vformat(kputc, NULL, fmt, ap);
    va_end(ap);
    vga_cursor();
    return n;
}

void panic(const char *fmt, ...)
{
    cli();
    console_resume();
    fg = 7; bg = 4; bold = 1;
    esc_state = 0;
    kprintf("\n*** KERNEL PANIC: ");
    va_list ap;
    va_start(ap, fmt);
    vformat(kputc, NULL, fmt, ap);
    va_end(ap);
    kprintf(" ***\n");
    for (;;)
        __asm__ volatile("hlt");
}
